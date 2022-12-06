/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/traced/probes/ps/process_stats_data_source.h"

#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <utility>

#include "perfetto/base/task_runner.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/hash.h"
#include "perfetto/ext/base/metatrace.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/tracing/core/data_source_config.h"

#include "protos/perfetto/config/process_stats/process_stats_config.pbzero.h"
#include "protos/perfetto/trace/ps/process_stats.pbzero.h"
#include "protos/perfetto/trace/ps/process_tree.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

// TODO(primiano): the code in this file assumes that PIDs are never recycled
// and that processes/threads never change names. Neither is always true.

// The notion of PID in the Linux kernel is a bit confusing.
// - PID: is really the thread id (for the main thread: PID == TID).
// - TGID (thread group ID): is the Unix Process ID (the actual PID).
// - PID == TGID for the main thread: the TID of the main thread is also the PID
//   of the process.
// So, in this file, |pid| might refer to either a process id or a thread id.

namespace perfetto {

namespace {

int32_t ReadNextNumericDir(DIR* dirp) {
  while (struct dirent* dir_ent = readdir(dirp)) {
    if (dir_ent->d_type != DT_DIR)
      continue;
    auto int_value = base::CStringToInt32(dir_ent->d_name);
    if (int_value)
      return *int_value;
  }
  return 0;
}

inline int ToInt(const std::string& str) {
  return atoi(str.c_str());
}

inline uint32_t ToU32(const char* str) {
  return static_cast<uint32_t>(strtol(str, nullptr, 10));
}

}  // namespace

// static
const ProbesDataSource::Descriptor ProcessStatsDataSource::descriptor = {
    /*name*/ "linux.process_stats",
    /*flags*/ Descriptor::kHandlesIncrementalState,
    /*fill_descriptor_func*/ nullptr,
};

ProcessStatsDataSource::ProcessStatsDataSource(
    base::TaskRunner* task_runner,
    TracingSessionID session_id,
    std::unique_ptr<TraceWriter> writer,
    const DataSourceConfig& ds_config,
    std::unique_ptr<CpuFreqInfo> cpu_freq_info)
    : ProbesDataSource(session_id, &descriptor),
      task_runner_(task_runner),
      writer_(std::move(writer)),
      cpu_freq_info_(std::move(cpu_freq_info)),
      weak_factory_(this) {
  using protos::pbzero::ProcessStatsConfig;
  ProcessStatsConfig::Decoder cfg(ds_config.process_stats_config_raw());
  record_thread_names_ = cfg.record_thread_names();
  dump_all_procs_on_start_ = cfg.scan_all_processes_on_start();
  resolve_process_fds_ = cfg.resolve_process_fds();

  enable_on_demand_dumps_ = true;
  for (auto quirk = cfg.quirks(); quirk; ++quirk) {
    if (*quirk == ProcessStatsConfig::DISABLE_ON_DEMAND)
      enable_on_demand_dumps_ = false;
  }

  poll_period_ms_ = cfg.proc_stats_poll_ms();
  if (poll_period_ms_ > 0 && poll_period_ms_ < 100) {
    PERFETTO_ILOG("proc_stats_poll_ms %" PRIu32
                  " is less than minimum of 100ms. Increasing to 100ms.",
                  poll_period_ms_);
    poll_period_ms_ = 100;
  }

  if (poll_period_ms_ > 0) {
    auto proc_stats_ttl_ms = cfg.proc_stats_cache_ttl_ms();
    process_stats_cache_ttl_ticks_ =
        std::max(proc_stats_ttl_ms / poll_period_ms_, 1u);
  }
}

ProcessStatsDataSource::~ProcessStatsDataSource() = default;

void ProcessStatsDataSource::Start() {
  if (dump_all_procs_on_start_) {
    WriteAllProcesses();
  }

  if (poll_period_ms_) {
    auto weak_this = GetWeakPtr();
    task_runner_->PostTask(std::bind(&ProcessStatsDataSource::Tick, weak_this));
  }
}

base::WeakPtr<ProcessStatsDataSource> ProcessStatsDataSource::GetWeakPtr()
    const {
  return weak_factory_.GetWeakPtr();
}

void ProcessStatsDataSource::WriteAllProcesses() {
  PERFETTO_METATRACE_SCOPED(TAG_PROC_POLLERS, PS_WRITE_ALL_PROCESSES);
  PERFETTO_DCHECK(!cur_ps_tree_);

  CacheProcFsScanStartTimestamp();

  base::ScopedDir proc_dir = OpenProcDir();
  if (!proc_dir)
    return;
  while (int32_t pid = ReadNextNumericDir(*proc_dir)) {
    WriteProcessOrThread(pid);
    base::StackString<128> task_path("/proc/%d/task", pid);
    base::ScopedDir task_dir(opendir(task_path.c_str()));
    if (!task_dir)
      continue;

    while (int32_t tid = ReadNextNumericDir(*task_dir)) {
      if (tid == pid)
        continue;
      if (record_thread_names_) {
        WriteProcessOrThread(tid);
      } else {
        // If we are not interested in thread names, there is no need to open
        // a proc file for each thread. We can save time and directly write the
        // thread record. Note that we still read proc_status for recording
        // NSpid entries.
        std::string proc_status = ReadProcPidFile(tid, "status");
        WriteThread(tid, pid, /*optional_name=*/nullptr, proc_status);
      }
    }
  }
  FinalizeCurPacket();

  // Also collect any fds open when starting up
  for (const auto pid : seen_pids_)
    WriteFds(pid);
  FinalizeCurPacket();
}

void ProcessStatsDataSource::OnPids(const base::FlatSet<int32_t>& pids) {
  if (!enable_on_demand_dumps_)
    return;
  WriteProcessTree(pids);
}

void ProcessStatsDataSource::WriteProcessTree(
    const base::FlatSet<int32_t>& pids) {
  PERFETTO_METATRACE_SCOPED(TAG_PROC_POLLERS, PS_ON_PIDS);
  PERFETTO_DCHECK(!cur_ps_tree_);
  int pids_scanned = 0;
  for (int32_t pid : pids) {
    if (seen_pids_.count(pid) || pid == 0)
      continue;
    WriteProcessOrThread(pid);
    pids_scanned++;
  }
  FinalizeCurPacket();
  PERFETTO_METATRACE_COUNTER(TAG_PROC_POLLERS, PS_PIDS_SCANNED, pids_scanned);
}

void ProcessStatsDataSource::OnRenamePids(const base::FlatSet<int32_t>& pids) {
  PERFETTO_METATRACE_SCOPED(TAG_PROC_POLLERS, PS_ON_RENAME_PIDS);
  if (!enable_on_demand_dumps_)
    return;
  PERFETTO_DCHECK(!cur_ps_tree_);
  for (int32_t pid : pids)
    seen_pids_.erase(pid);
}

void ProcessStatsDataSource::OnFds(
    const std::unordered_map<pid_t, base::FlatSet<uint64_t>>& fds) {
  if (!resolve_process_fds_)
    return;

  for (const auto& pid_fds : fds) {
    const auto pid = pid_fds.first;
    cur_ps_stats_process_ = nullptr;
    for (const auto fd : pid_fds.second) {
      WriteSingleFd(pid, fd);
    }
  }
  FinalizeCurPacket();
}

void ProcessStatsDataSource::Flush(FlushRequestID,
                                   std::function<void()> callback) {
  // We shouldn't get this in the middle of WriteAllProcesses() or OnPids().
  PERFETTO_DCHECK(!cur_ps_tree_);
  PERFETTO_DCHECK(!cur_ps_stats_);
  PERFETTO_DCHECK(!cur_ps_stats_process_);
  writer_->Flush(callback);
}

void ProcessStatsDataSource::WriteProcessOrThread(int32_t pid) {
  // In case we're called from outside WriteAllProcesses()
  CacheProcFsScanStartTimestamp();

  std::string proc_status = ReadProcPidFile(pid, "status");
  if (proc_status.empty())
    return;
  int tgid = ToInt(ReadProcStatusEntry(proc_status, "Tgid:"));
  int tid = ToInt(ReadProcStatusEntry(proc_status, "Pid:"));
  if (tgid <= 0 || tid <= 0)
    return;

  if (!seen_pids_.count(tgid)) {
    // We need to read the status file if |pid| is non-main thread.
    const std::string& proc_status_tgid =
        (tgid == tid ? proc_status : ReadProcPidFile(tgid, "status"));
    WriteProcess(tgid, proc_status_tgid);
  }
  if (pid != tgid) {
    PERFETTO_DCHECK(!seen_pids_.count(pid));
    std::string thread_name;
    if (record_thread_names_)
      thread_name = ReadProcStatusEntry(proc_status, "Name:");
    WriteThread(pid, tgid, thread_name.empty() ? nullptr : thread_name.c_str(),
                proc_status);
  }
}

void ProcessStatsDataSource::ReadNamespacedTids(int32_t tid,
                                                const std::string& proc_status,
                                                TidArray& out) {
  // If a process has entered a PID namespace, NSpid shows the mapping in
  // the status file like: NSpid:  28971   2
  // NStgid: 28971   2
  // which denotes that the thread (or process) 28971 in the root PID namespace
  // has PID = 2 in the child PID namespace. This information can be read from
  // the NSpid entry in /proc/<tid>/status.
  if (proc_status.empty())
    return;
  std::string nspid = ReadProcStatusEntry(proc_status, "NSpid:");
  if (nspid.empty())
    return;

  out.fill(0);  // Zero-initialize the array in case the caller doesn't.
  auto it = out.begin();

  base::StringSplitter ss(std::move(nspid), '\t');
  ss.Next();  // Skip the 1st element.
  PERFETTO_DCHECK(base::CStringToInt32(ss.cur_token()) == tid);
  while (ss.Next()) {
    PERFETTO_CHECK(it < out.end());
    auto maybe_int32 = base::CStringToInt32(ss.cur_token());
    PERFETTO_DCHECK(maybe_int32.has_value());
    *it++ = *maybe_int32;
  }
}

void ProcessStatsDataSource::WriteProcess(int32_t pid,
                                          const std::string& proc_status) {
  PERFETTO_DCHECK(ToInt(ReadProcStatusEntry(proc_status, "Tgid:")) == pid);
  // Assert that |proc_status| is not for a non-main thread.
  PERFETTO_DCHECK(ToInt(ReadProcStatusEntry(proc_status, "Pid:")) == pid);
  auto* proc = GetOrCreatePsTree()->add_processes();
  proc->set_pid(pid);
  proc->set_ppid(ToInt(ReadProcStatusEntry(proc_status, "PPid:")));
  // Uid will have multiple entries, only return first (real uid).
  proc->set_uid(ToInt(ReadProcStatusEntry(proc_status, "Uid:")));

  // Optionally write namespace-local PIDs.
  TidArray nspids = {};
  ReadNamespacedTids(pid, proc_status, nspids);
  for (auto nspid : nspids) {
    if (nspid == 0)  // No more elements.
      break;
    proc->add_nspid(nspid);
  }

  std::string cmdline = ReadProcPidFile(pid, "cmdline");
  if (!cmdline.empty()) {
    if (cmdline.back() != '\0') {
      // Some kernels can miss the NUL terminator due to a bug. b/147438623.
      cmdline.push_back('\0');
    }
    using base::StringSplitter;
    for (StringSplitter ss(&cmdline[0], cmdline.size(), '\0'); ss.Next();)
      proc->add_cmdline(ss.cur_token());
  } else {
    // Nothing in cmdline so use the thread name instead (which is == "comm").
    proc->add_cmdline(ReadProcStatusEntry(proc_status, "Name:").c_str());
  }
  seen_pids_.insert(pid);
  tids_to_pids_.emplace(pid, pid);
}

void ProcessStatsDataSource::WriteThread(int32_t tid,
                                         int32_t tgid,
                                         const char* optional_name,
                                         const std::string& proc_status) {
  auto* thread = GetOrCreatePsTree()->add_threads();
  thread->set_tid(tid);
  thread->set_tgid(tgid);
  if (optional_name)
    thread->set_name(optional_name);

  // Optionally write namespace-local TIDs.
  TidArray nstids = {};
  ReadNamespacedTids(tid, proc_status, nstids);
  for (auto nstid : nstids) {
    if (nstid == 0)  // No more elements.
      break;
    thread->add_nstid(nstid);
  }
  seen_pids_.insert(tid);
  tids_to_pids_.emplace(tid, tgid);
}

const char* ProcessStatsDataSource::GetProcMountpoint() {
  static constexpr char kDefaultProcMountpoint[] = "/proc";
  return kDefaultProcMountpoint;
}

base::ScopedDir ProcessStatsDataSource::OpenProcDir() {
  base::ScopedDir proc_dir(opendir(GetProcMountpoint()));
  if (!proc_dir)
    PERFETTO_PLOG("Failed to opendir(%s)", GetProcMountpoint());
  return proc_dir;
}

std::string ProcessStatsDataSource::ReadProcPidFile(int32_t pid,
                                                    const std::string& file) {
  base::StackString<128> path("/proc/%" PRId32 "/%s", pid, file.c_str());
  std::string contents;
  contents.reserve(4096);
  if (!base::ReadFile(path.c_str(), &contents))
    return "";
  return contents;
}

std::string ProcessStatsDataSource::ReadProcStatusEntry(const std::string& buf,
                                                        const char* key) {
  auto begin = buf.find(key);
  if (begin == std::string::npos)
    return "";
  begin = buf.find_first_not_of(" \t", begin + strlen(key));
  if (begin == std::string::npos)
    return "";
  auto end = buf.find('\n', begin);
  if (end == std::string::npos || end <= begin)
    return "";
  return buf.substr(begin, end - begin);
}

void ProcessStatsDataSource::StartNewPacketIfNeeded() {
  if (cur_packet_)
    return;
  cur_packet_ = writer_->NewTracePacket();
  cur_packet_->set_timestamp(CacheProcFsScanStartTimestamp());

  if (did_clear_incremental_state_) {
    cur_packet_->set_incremental_state_cleared(true);
    did_clear_incremental_state_ = false;
  }
}

protos::pbzero::ProcessTree* ProcessStatsDataSource::GetOrCreatePsTree() {
  StartNewPacketIfNeeded();
  if (!cur_ps_tree_)
    cur_ps_tree_ = cur_packet_->set_process_tree();
  cur_ps_stats_ = nullptr;
  cur_ps_stats_process_ = nullptr;
  return cur_ps_tree_;
}

protos::pbzero::ProcessStats* ProcessStatsDataSource::GetOrCreateStats() {
  StartNewPacketIfNeeded();
  if (!cur_ps_stats_)
    cur_ps_stats_ = cur_packet_->set_process_stats();
  cur_ps_tree_ = nullptr;
  cur_ps_stats_process_ = nullptr;
  return cur_ps_stats_;
}

protos::pbzero::ProcessStats_Process*
ProcessStatsDataSource::GetOrCreateStatsProcess(int32_t pid) {
  if (cur_ps_stats_process_)
    return cur_ps_stats_process_;
  cur_ps_stats_process_ = GetOrCreateStats()->add_processes();
  cur_ps_stats_process_->set_pid(pid);
  return cur_ps_stats_process_;
}

void ProcessStatsDataSource::FinalizeCurPacket() {
  PERFETTO_DCHECK(!cur_ps_tree_ || cur_packet_);
  PERFETTO_DCHECK(!cur_ps_stats_ || cur_packet_);
  uint64_t now = static_cast<uint64_t>(base::GetBootTimeNs().count());
  if (cur_ps_tree_) {
    cur_ps_tree_->set_collection_end_timestamp(now);
    cur_ps_tree_ = nullptr;
  }
  if (cur_ps_stats_) {
    cur_ps_stats_->set_collection_end_timestamp(now);
    cur_ps_stats_ = nullptr;
  }
  cur_ps_stats_process_ = nullptr;
  cur_procfs_scan_start_timestamp_ = 0;
  cur_packet_ = TraceWriter::TracePacketHandle{};
}

// static
void ProcessStatsDataSource::Tick(
    base::WeakPtr<ProcessStatsDataSource> weak_this) {
  if (!weak_this)
    return;
  ProcessStatsDataSource& thiz = *weak_this;
  uint32_t period_ms = thiz.poll_period_ms_;
  uint32_t delay_ms =
      period_ms -
      static_cast<uint32_t>(base::GetWallTimeMs().count() % period_ms);
  thiz.task_runner_->PostDelayedTask(
      std::bind(&ProcessStatsDataSource::Tick, weak_this), delay_ms);
  thiz.WriteAllProcessStats();

  // We clear the cache every process_stats_cache_ttl_ticks_ ticks.
  if (++thiz.cache_ticks_ == thiz.process_stats_cache_ttl_ticks_) {
    thiz.cache_ticks_ = 0;
    thiz.process_stats_cache_.clear();
  }
}

void ProcessStatsDataSource::WriteAllProcessStats() {
  // TODO(primiano): implement filtering of processes by names.
  // TODO(primiano): Have a pid cache to avoid wasting cycles reading kthreads
  // proc files over and over. Same for non-filtered processes (see above).

  CacheProcFsScanStartTimestamp();
  PERFETTO_METATRACE_SCOPED(TAG_PROC_POLLERS, PS_WRITE_ALL_PROCESS_STATS);
  base::ScopedDir proc_dir = OpenProcDir();
  if (!proc_dir)
    return;
  base::FlatSet<int32_t> pids;
  while (int32_t pid = ReadNextNumericDir(*proc_dir)) {
    cur_ps_stats_process_ = nullptr;

    uint32_t pid_u = static_cast<uint32_t>(pid);
    if (skip_stats_for_pids_.size() > pid_u && skip_stats_for_pids_[pid_u])
      continue;

    std::string proc_status = ReadProcPidFile(pid, "status");
    if (proc_status.empty())
      continue;

    if (!WriteMemCounters(pid, proc_status)) {
      // If WriteMemCounters() fails the pid is very likely a kernel thread
      // that has a valid /proc/[pid]/status but no memory values. In this
      // case avoid keep polling it over and over.
      if (skip_stats_for_pids_.size() <= pid_u)
        skip_stats_for_pids_.resize(pid_u + 1);
      skip_stats_for_pids_[pid_u] = true;
      continue;
    }

    std::string oom_score_adj = ReadProcPidFile(pid, "oom_score_adj");
    if (!oom_score_adj.empty()) {
      CachedProcessStats& cached = process_stats_cache_[pid];
      auto counter = ToInt(oom_score_adj);
      if (counter != cached.oom_score_adj) {
        GetOrCreateStatsProcess(pid)->set_oom_score_adj(counter);
        cached.oom_score_adj = counter;
      }
    }

    // Ensure we write data on any fds not seen before
    WriteFds(pid);

    pids.insert(pid);
  }
  FinalizeCurPacket();

  // Ensure that we write once long-term process info (e.g., name) for new pids
  // that we haven't seen before.
  WriteProcessTree(pids);
}

// Returns true if the stats for the given |pid| have been written, false it
// it failed (e.g., |pid| was a kernel thread and, as such, didn't report any
// memory counters).
bool ProcessStatsDataSource::WriteMemCounters(int32_t pid,
                                              const std::string& proc_status) {
  bool proc_status_has_mem_counters = false;
  CachedProcessStats& cached = process_stats_cache_[pid];

  // Parse /proc/[pid]/status, which looks like this:
  // Name:   cat
  // Umask:  0027
  // State:  R (running)
  // FDSize: 256
  // Groups: 4 20 24 46 997
  // VmPeak:     5992 kB
  // VmSize:     5992 kB
  // VmLck:         0 kB
  // ...
  std::vector<char> key;
  std::vector<char> value;
  enum { kKey, kSeparator, kValue } state = kKey;
  for (char c : proc_status) {
    if (c == '\n') {
      key.push_back('\0');
      value.push_back('\0');

      // |value| will contain "1234 KB". We rely on strtol() (in ToU32()) to
      // stop parsing at the first non-numeric character.
      if (strcmp(key.data(), "VmSize") == 0) {
        // Assume that if we see VmSize we'll see also the others.
        proc_status_has_mem_counters = true;

        auto counter = ToU32(value.data());
        if (counter != cached.vm_size_kb) {
          GetOrCreateStatsProcess(pid)->set_vm_size_kb(counter);
          cached.vm_size_kb = counter;
        }
      } else if (strcmp(key.data(), "VmLck") == 0) {
        auto counter = ToU32(value.data());
        if (counter != cached.vm_locked_kb) {
          GetOrCreateStatsProcess(pid)->set_vm_locked_kb(counter);
          cached.vm_locked_kb = counter;
        }
      } else if (strcmp(key.data(), "VmHWM") == 0) {
        auto counter = ToU32(value.data());
        if (counter != cached.vm_hvm_kb) {
          GetOrCreateStatsProcess(pid)->set_vm_hwm_kb(counter);
          cached.vm_hvm_kb = counter;
        }
      } else if (strcmp(key.data(), "VmRSS") == 0) {
        auto counter = ToU32(value.data());
        if (counter != cached.vm_rss_kb) {
          GetOrCreateStatsProcess(pid)->set_vm_rss_kb(counter);
          cached.vm_rss_kb = counter;
        }
      } else if (strcmp(key.data(), "RssAnon") == 0) {
        auto counter = ToU32(value.data());
        if (counter != cached.rss_anon_kb) {
          GetOrCreateStatsProcess(pid)->set_rss_anon_kb(counter);
          cached.rss_anon_kb = counter;
        }
      } else if (strcmp(key.data(), "RssFile") == 0) {
        auto counter = ToU32(value.data());
        if (counter != cached.rss_file_kb) {
          GetOrCreateStatsProcess(pid)->set_rss_file_kb(counter);
          cached.rss_file_kb = counter;
        }
      } else if (strcmp(key.data(), "RssShmem") == 0) {
        auto counter = ToU32(value.data());
        if (counter != cached.rss_shmem_kb) {
          GetOrCreateStatsProcess(pid)->set_rss_shmem_kb(counter);
          cached.rss_shmem_kb = counter;
        }
      } else if (strcmp(key.data(), "VmSwap") == 0) {
        auto counter = ToU32(value.data());
        if (counter != cached.vm_swap_kb) {
          GetOrCreateStatsProcess(pid)->set_vm_swap_kb(counter);
          cached.vm_swap_kb = counter;
        }
      }

      key.clear();
      state = kKey;
      continue;
    }

    if (state == kKey) {
      if (c == ':') {
        state = kSeparator;
        continue;
      }
      key.push_back(c);
      continue;
    }

    if (state == kSeparator) {
      if (isspace(c))
        continue;
      value.clear();
      value.push_back(c);
      state = kValue;
      continue;
    }

    if (state == kValue) {
      value.push_back(c);
    }
  }
  return proc_status_has_mem_counters;
}

void ProcessStatsDataSource::WriteFds(int32_t pid) {
  if (!resolve_process_fds_) {
    return;
  }

  base::StackString<64> path("%s/%" PRId32 "/fd", GetProcMountpoint(), pid);
  base::ScopedDir proc_dir(opendir(path.c_str()));
  if (!proc_dir) {
    PERFETTO_DPLOG("Failed to opendir(%s)", path.c_str());
    return;
  }
  while (struct dirent* dir_ent = readdir(*proc_dir)) {
    if (dir_ent->d_type != DT_LNK)
      continue;
    auto fd = base::CStringToUInt64(dir_ent->d_name);
    if (fd)
      WriteSingleFd(pid, *fd);
  }
}

void ProcessStatsDataSource::WriteSingleFd(int32_t tid, uint64_t fd) {
  int32_t pid = tid;
  auto it = tids_to_pids_.find(tid);
  if (it != tids_to_pids_.end()) {
    pid = it->second;
  } else {
    PERFETTO_DLOG("TID %" PRId32 " has no process mapping", tid);
  }
  CachedProcessStats& cached = process_stats_cache_[pid];
  if (cached.seen_fds.count(fd)) {
    return;
  }

  base::StackString<128> proc_fd("%s/%" PRId32 "/fd/%" PRIu64,
                                 GetProcMountpoint(), pid, fd);
  std::array<char, 256> path;
  ssize_t actual = readlink(proc_fd.c_str(), path.data(), path.size());
  if (actual >= 0) {
    auto* fd_info = GetOrCreateStatsProcess(pid)->add_fds();
    fd_info->set_fd(fd);
    fd_info->set_path(path.data(), static_cast<size_t>(actual));
    cached.seen_fds.insert(fd);
  } else if (ENOENT != errno) {
    PERFETTO_DPLOG("Failed to readlink '%s'", proc_fd.c_str());
  }
}

uint64_t ProcessStatsDataSource::CacheProcFsScanStartTimestamp() {
  if (!cur_procfs_scan_start_timestamp_)
    cur_procfs_scan_start_timestamp_ =
        static_cast<uint64_t>(base::GetBootTimeNs().count());
  return cur_procfs_scan_start_timestamp_;
}

void ProcessStatsDataSource::ClearIncrementalState() {
  PERFETTO_DLOG("ProcessStatsDataSource clearing incremental state.");
  seen_pids_.clear();
  tids_to_pids_.clear();
  skip_stats_for_pids_.clear();

  cache_ticks_ = 0;
  process_stats_cache_.clear();

  // Set the relevant flag in the next packet.
  did_clear_incremental_state_ = true;
}

}  // namespace perfetto
