/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include <cstdint>

#include "src/trace_redaction/process_thread_timeline.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_redaction {

namespace {

class SliceTestParams {
 public:
  SliceTestParams(uint64_t ts, int32_t pid, uint64_t uid)
      : ts_(ts), pid_(pid), uid_(uid) {}

  uint64_t ts() const { return ts_; }
  int32_t pid() const { return pid_; }
  uint64_t uid() const { return uid_; }

 private:
  uint64_t ts_;
  int32_t pid_;
  uint64_t uid_;
};

constexpr uint64_t kTimeA = 0;
constexpr uint64_t kTimeB = 10;
constexpr uint64_t kTimeC = 20;
constexpr uint64_t kTimeD = 30;
constexpr uint64_t kTimeE = 40;
constexpr uint64_t kTimeF = 50;
constexpr uint64_t kTimeG = 60;

constexpr int32_t kPidA = 1;
constexpr int32_t kPidB = 2;

constexpr uint64_t kNoPackage = 0;

constexpr int32_t kUidA = 98;
constexpr int32_t kUidB = 99;

}  // namespace

class TimelineEventsTest : public testing::Test,
                           public testing::WithParamInterface<SliceTestParams> {
 protected:
  ProcessThreadTimeline timeline_;
};

class TimelineEventsOpenAndCloseSingleTest : public TimelineEventsTest {};

TEST_P(TimelineEventsOpenAndCloseSingleTest, PidsEndOnClose) {
  auto params = GetParam();

  timeline_.Append(
      ProcessThreadTimeline::Event::Open(kTimeB, kPidB, kPidA, kUidA));
  timeline_.Append(ProcessThreadTimeline::Event::Close(kTimeD, kPidB));

  timeline_.Sort();

  auto slice = timeline_.Search(params.ts(), params.pid());
  ASSERT_EQ(slice.pid, params.pid());
  ASSERT_EQ(slice.uid, params.uid());
}

INSTANTIATE_TEST_SUITE_P(AcrossWholeTimeline,
                         TimelineEventsOpenAndCloseSingleTest,
                         testing::Values(
                             // No UID found before opening event.
                             SliceTestParams(kTimeA, kPidB, kNoPackage),

                             // UID found when opening event starts.
                             SliceTestParams(kTimeB, kPidB, kUidA),

                             // UID found between opening and close events.
                             SliceTestParams(kTimeC, kPidB, kUidA),

                             // UID is no longer found at the close event.
                             SliceTestParams(kTimeD, kPidB, kNoPackage),

                             // UID is no longer found after the close event.
                             SliceTestParams(kTimeE, kPidB, kNoPackage)));

class TimelineEventsOpenAfterOpenTest : public TimelineEventsTest {};

// |--- PID A --- >
//                 |--- PID A --- >
TEST_P(TimelineEventsOpenAfterOpenTest, FindsUid) {
  auto params = GetParam();

  timeline_.Append(
      ProcessThreadTimeline::Event::Open(kTimeB, kPidB, kPidA, kUidA));
  timeline_.Append(
      ProcessThreadTimeline::Event::Open(kTimeD, kPidB, kPidA, kUidB));

  timeline_.Sort();

  auto slice = timeline_.Search(params.ts(), params.pid());
  ASSERT_EQ(slice.pid, params.pid());
  ASSERT_EQ(slice.uid, params.uid());
}

INSTANTIATE_TEST_SUITE_P(
    AcrossWholeTimeline,
    TimelineEventsOpenAfterOpenTest,
    testing::Values(SliceTestParams(kTimeA, kPidB, kNoPackage),
                    SliceTestParams(kTimeB, kPidB, kUidA),
                    SliceTestParams(kTimeC, kPidB, kUidA),
                    SliceTestParams(kTimeD, kPidB, kUidB),
                    SliceTestParams(kTimeE, kPidB, kUidB)));

class TimelineEventsOverlappingRangesTest : public TimelineEventsTest {};

TEST_P(TimelineEventsOverlappingRangesTest, FindsUid) {
  auto params = GetParam();

  // |----- PID_A -----|
  //          |----- PID_B -----|
  timeline_.Append(ProcessThreadTimeline::Event::Open(kTimeA, kPidA, 0, kUidA));
  timeline_.Append(ProcessThreadTimeline::Event::Open(kTimeC, kPidB, 0, kUidB));
  timeline_.Append(ProcessThreadTimeline::Event::Close(kTimeE, kPidA));
  timeline_.Append(ProcessThreadTimeline::Event::Close(kTimeG, kPidB));

  timeline_.Sort();

  auto slice = timeline_.Search(params.ts(), params.pid());
  ASSERT_EQ(slice.pid, params.pid());
  ASSERT_EQ(slice.uid, params.uid());
}

INSTANTIATE_TEST_SUITE_P(AcrossWholeTimeline,
                         TimelineEventsOverlappingRangesTest,
                         testing::Values(
                             // When pid A starts and before pid B starts.
                             SliceTestParams(kTimeA, kPidA, kUidA),
                             SliceTestParams(kTimeA, kPidB, kNoPackage),

                             // After pid A starts and before pid B starts.
                             SliceTestParams(kTimeB, kPidA, kUidA),
                             SliceTestParams(kTimeB, kPidB, kNoPackage),

                             // After pid A starts and when pid B starts.
                             SliceTestParams(kTimeC, kPidA, kUidA),
                             SliceTestParams(kTimeC, kPidB, kUidB),

                             // After pid A and pid starts.
                             SliceTestParams(kTimeD, kPidA, kUidA),
                             SliceTestParams(kTimeD, kPidB, kUidB),

                             // When pid A closes but before pid B closes.
                             SliceTestParams(kTimeE, kPidA, kNoPackage),
                             SliceTestParams(kTimeE, kPidB, kUidB),

                             // After pid A closes but before pid B closes.
                             SliceTestParams(kTimeF, kPidA, kNoPackage),
                             SliceTestParams(kTimeF, kPidB, kUidB),

                             // After pid A closes and when pid B closes.
                             SliceTestParams(kTimeG, kPidA, kNoPackage),
                             SliceTestParams(kTimeG, kPidB, kNoPackage)));

class TimelineEventsParentChildTest : public TimelineEventsTest {};

TEST_P(TimelineEventsParentChildTest, FindsUid) {
  auto params = GetParam();

  // |------------- PID_A ------------->
  //         |----- PID_B -----|
  timeline_.Append(ProcessThreadTimeline::Event::Open(kTimeA, kPidA, 0, kUidA));
  timeline_.Append(ProcessThreadTimeline::Event::Open(kTimeC, kPidB, kPidA));
  timeline_.Append(ProcessThreadTimeline::Event::Close(kTimeE, kPidB));

  timeline_.Sort();

  auto slice = timeline_.Search(params.ts(), params.pid());
  ASSERT_EQ(slice.pid, params.pid());
  ASSERT_EQ(slice.uid, params.uid());
}

INSTANTIATE_TEST_SUITE_P(
    AcrossWholeTimeline,
    TimelineEventsParentChildTest,
    testing::Values(SliceTestParams(kTimeB, kPidB, kNoPackage),
                    SliceTestParams(kTimeC, kPidB, kUidA),
                    SliceTestParams(kTimeD, kPidB, kUidA),
                    SliceTestParams(kTimeE, kPidB, kNoPackage)));

}  // namespace perfetto::trace_redaction
