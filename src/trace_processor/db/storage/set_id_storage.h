/*
 * Copyright (C) 2023 The Android Open Source Project
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
#ifndef SRC_TRACE_PROCESSOR_DB_STORAGE_SET_ID_STORAGE_H_
#define SRC_TRACE_PROCESSOR_DB_STORAGE_SET_ID_STORAGE_H_

#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/db/storage/storage.h"
#include "src/trace_processor/db/storage/types.h"

namespace perfetto {

namespace protos::pbzero {
class SerializedColumn_Storage;
}

namespace trace_processor {
namespace storage {

// Storage for SetId columns.
class SetIdStorage final : public Storage {
 public:
  using SetId = uint32_t;

  explicit SetIdStorage(const std::vector<uint32_t>* data) : values_(data) {}

  RangeOrBitVector Search(FilterOp op,
                          SqlValue value,
                          RowMap::Range range) const override;

  RangeOrBitVector IndexSearch(FilterOp op,
                               SqlValue value,
                               uint32_t* indices,
                               uint32_t indices_count,
                               bool sorted) const override;

  void StableSort(uint32_t* rows, uint32_t rows_size) const override;

  void Sort(uint32_t* rows, uint32_t rows_size) const override;

  void Serialize(StorageProto*) const override;

  uint32_t size() const override {
    return static_cast<uint32_t>(values_->size());
  }

 private:
  BitVector IndexSearch(FilterOp, SqlValue, uint32_t*, uint32_t) const;
  RowMap::Range BinarySearchIntrinsic(FilterOp op,
                                      SqlValue val,
                                      RowMap::Range search_range) const;

  // TODO(b/307482437): After the migration vectors should be owned by storage,
  // so change from pointer to value.
  const std::vector<SetId>* values_ = nullptr;
};

}  // namespace storage
}  // namespace trace_processor
}  // namespace perfetto
#endif  // SRC_TRACE_PROCESSOR_DB_STORAGE_SET_ID_STORAGE_H_
