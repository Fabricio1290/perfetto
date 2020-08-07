#!/usr/bin/env python3
# Copyright (C) 2020 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import unittest

from trace_processor.api import TraceProcessor, TraceProcessorException
from trace_processor.protos import ProtoFactory


class TestQueryResultIterator(unittest.TestCase):
  # The numbers input into cells correspond the the CellType enum values
  # defined under trace_processor.proto
  CELL_VARINT = ProtoFactory().CellsBatch().CELL_VARINT
  CELL_STRING = ProtoFactory().CellsBatch().CELL_STRING
  CELL_INVALID = ProtoFactory().CellsBatch().CELL_INVALID

  def test_one_batch(self):
    int_values = [100, 200]
    str_values = ['bar1', 'bar2']

    batch = ProtoFactory().CellsBatch()
    batch.cells.extend([
        TestQueryResultIterator.CELL_STRING,
        TestQueryResultIterator.CELL_VARINT,
        TestQueryResultIterator.CELL_STRING, TestQueryResultIterator.CELL_VARINT
    ])
    batch.varint_cells.extend(int_values)
    batch.string_cells = "\0".join(str_values)
    batch.is_last_batch = True

    qr_iterator = TraceProcessor.QueryResultIterator(['foo_id', 'foo_num'],
                                                     [batch])

    for num, row in enumerate(qr_iterator):
      self.assertEqual(row.foo_id, str_values[num])
      self.assertEqual(row.foo_num, int_values[num])

  def test_many_batches(self):
    int_values = [100, 200, 300, 400]
    str_values = ['bar1', 'bar2', 'bar3', 'bar4']

    batch_1 = ProtoFactory().CellsBatch()
    batch_1.cells.extend([
        TestQueryResultIterator.CELL_STRING,
        TestQueryResultIterator.CELL_VARINT,
        TestQueryResultIterator.CELL_STRING, TestQueryResultIterator.CELL_VARINT
    ])
    batch_1.varint_cells.extend(int_values[:2])
    batch_1.string_cells = "\0".join(str_values[:2])
    batch_1.is_last_batch = False

    batch_2 = ProtoFactory().CellsBatch()
    batch_2.cells.extend([
        TestQueryResultIterator.CELL_STRING,
        TestQueryResultIterator.CELL_VARINT,
        TestQueryResultIterator.CELL_STRING, TestQueryResultIterator.CELL_VARINT
    ])
    batch_2.varint_cells.extend(int_values[2:])
    batch_2.string_cells = "\0".join(str_values[2:])
    batch_2.is_last_batch = True

    qr_iterator = TraceProcessor.QueryResultIterator(['foo_id', 'foo_num'],
                                                     [batch_1, batch_2])

    for num, row in enumerate(qr_iterator):
      self.assertEqual(row.foo_id, str_values[num])
      self.assertEqual(row.foo_num, int_values[num])

  def test_empty_batch(self):
    batch = ProtoFactory().CellsBatch()
    batch.is_last_batch = True

    qr_iterator = TraceProcessor.QueryResultIterator([], [batch])

    for num, row in enumerate(qr_iterator):
      self.assertIsNone(row.foo_id)
      self.assertIsNone(row.foo_num)

  def test_invalid_batch(self):
    batch = ProtoFactory().CellsBatch()

    qr_iterator = TraceProcessor.QueryResultIterator([], [batch])

    # Since the batch isn't defined as the last batch, the QueryResultsIterator
    # expects another batch and thus raises IndexError as no next batch exists.
    with self.assertRaises(IndexError):
      for row in qr_iterator:
        pass

  def test_incorrect_cells_batch(self):
    str_values = ['bar1', 'bar2']

    batch = ProtoFactory().CellsBatch()
    batch.cells.extend([
        TestQueryResultIterator.CELL_STRING,
        TestQueryResultIterator.CELL_VARINT,
        TestQueryResultIterator.CELL_STRING, TestQueryResultIterator.CELL_VARINT
    ])
    batch.string_cells = "\0".join(str_values)
    batch.is_last_batch = True

    qr_iterator = TraceProcessor.QueryResultIterator(['foo_id', 'foo_num'],
                                                     [batch])

    # The batch specifies there ought to be 2 cells of type VARINT and 2 cells
    # of type STRING, but there are no string cells defined in the batch. Thus
    # an IndexError occurs as it tries to access the empty string cells list.
    with self.assertRaises(IndexError):
      for row in qr_iterator:
        pass

  def test_incorrect_columns_batch(self):
    batch = ProtoFactory().CellsBatch()
    batch.cells.extend([
        TestQueryResultIterator.CELL_VARINT, TestQueryResultIterator.CELL_VARINT
    ])
    batch.varint_cells.extend([100, 200])
    batch.is_last_batch = True

    qr_iterator = TraceProcessor.QueryResultIterator(
        ['foo_id', 'foo_num', 'foo_dur', 'foo_ms'], [batch])

    # It's always the case that the number of cells is a multiple of the number
    # of columns. However, here this is clearly not the case, so when the
    # iterator tries to access the cell for the third column, it raises an
    # IndexError due to having exhausted the cells list.
    with self.assertRaises(IndexError):
      for row in qr_iterator:
        pass

  def test_invalid_cell_type(self):
    batch = ProtoFactory().CellsBatch()
    batch.cells.extend([
        TestQueryResultIterator.CELL_INVALID,
        TestQueryResultIterator.CELL_VARINT
    ])
    batch.varint_cells.extend([100, 200])
    batch.is_last_batch = True

    qr_iterator = TraceProcessor.QueryResultIterator(['foo_id', 'foo_num'],
                                                     [batch])

    # In this batch we declare the columns types to be CELL_INVALID,
    # CELL_VARINT but that doesn't match the data which are both ints*
    # so we should raise a TraceProcessorException.
    with self.assertRaises(TraceProcessorException):
      for row in qr_iterator:
        pass

  def test_one_batch_as_pandas(self):
    int_values = [100, 200]
    str_values = ['bar1', 'bar2']

    batch = ProtoFactory().CellsBatch()
    batch.cells.extend([
        TestQueryResultIterator.CELL_STRING,
        TestQueryResultIterator.CELL_VARINT,
        TestQueryResultIterator.CELL_STRING, TestQueryResultIterator.CELL_VARINT
    ])
    batch.varint_cells.extend(int_values)
    batch.string_cells = "\0".join(str_values)
    batch.is_last_batch = True

    qr_iterator = TraceProcessor.QueryResultIterator(['foo_id', 'foo_num'],
                                                     [batch])

    qr_df = qr_iterator.as_pandas()
    for num, row in qr_df.iterrows():
      self.assertEqual(row['foo_id'], str_values[num])
      self.assertEqual(row['foo_num'], int_values[num])

  def test_many_batches_as_pandas(self):
    int_values = [100, 200, 300, 400]
    str_values = ['bar1', 'bar2', 'bar3', 'bar4']

    batch_1 = ProtoFactory().CellsBatch()
    batch_1.cells.extend([
        TestQueryResultIterator.CELL_STRING,
        TestQueryResultIterator.CELL_VARINT,
        TestQueryResultIterator.CELL_STRING, TestQueryResultIterator.CELL_VARINT
    ])
    batch_1.varint_cells.extend(int_values[:2])
    batch_1.string_cells = "\0".join(str_values[:2])
    batch_1.is_last_batch = False

    batch_2 = ProtoFactory().CellsBatch()
    batch_2.cells.extend([
        TestQueryResultIterator.CELL_STRING,
        TestQueryResultIterator.CELL_VARINT,
        TestQueryResultIterator.CELL_STRING, TestQueryResultIterator.CELL_VARINT
    ])
    batch_2.varint_cells.extend(int_values[2:])
    batch_2.string_cells = "\0".join(str_values[2:])
    batch_2.is_last_batch = True

    qr_iterator = TraceProcessor.QueryResultIterator(['foo_id', 'foo_num'],
                                                     [batch_1, batch_2])

    qr_df = qr_iterator.as_pandas()
    for num, row in qr_df.iterrows():
      self.assertEqual(row['foo_id'], str_values[num])
      self.assertEqual(row['foo_num'], int_values[num])

  def test_empty_batch_as_pandas(self):
    batch = ProtoFactory().CellsBatch()
    batch.is_last_batch = True

    qr_iterator = TraceProcessor.QueryResultIterator([], [batch])

    qr_df = qr_iterator.as_pandas()
    for num, row in qr_df.iterrows():
      self.assertEqual(row['foo_id'], str_values[num])
      self.assertEqual(row['foo_num'], int_values[num])

  def test_invalid_batch_as_pandas(self):
    batch = ProtoFactory().CellsBatch()

    qr_iterator = TraceProcessor.QueryResultIterator([], [batch])

    # Since the batch isn't defined as the last batch, the QueryResultsIterator
    # expects another batch and thus raises IndexError as no next batch exists.
    with self.assertRaises(IndexError):
      qr_df = qr_iterator.as_pandas()

  def test_incorrect_cells_batch_as_pandas(self):
    str_values = ['bar1', 'bar2']

    batch = ProtoFactory().CellsBatch()
    batch.cells.extend([
        TestQueryResultIterator.CELL_STRING,
        TestQueryResultIterator.CELL_VARINT,
        TestQueryResultIterator.CELL_STRING, TestQueryResultIterator.CELL_VARINT
    ])
    batch.string_cells = "\0".join(str_values)
    batch.is_last_batch = True

    qr_iterator = TraceProcessor.QueryResultIterator(['foo_id', 'foo_num'],
                                                     [batch])

    # The batch specifies there ought to be 2 cells of type VARINT and 2 cells
    # of type STRING, but there are no string cells defined in the batch. Thus
    # an IndexError occurs as it tries to access the empty string cells list.
    with self.assertRaises(IndexError):
      qr_df = qr_iterator.as_pandas()

  def test_incorrect_columns_batch_as_pandas(self):
    batch = ProtoFactory().CellsBatch()
    batch.cells.extend([
        TestQueryResultIterator.CELL_VARINT, TestQueryResultIterator.CELL_VARINT
    ])
    batch.varint_cells.extend([100, 200])
    batch.is_last_batch = True

    qr_iterator = TraceProcessor.QueryResultIterator(
        ['foo_id', 'foo_num', 'foo_dur', 'foo_ms'], [batch])

    # It's always the case that the number of cells is a multiple of the number
    # of columns. However, here this is clearly not the case, so when the
    # iterator tries to access the cell for the third column, it raises an
    # IndexError due to having exhausted the cells list.
    with self.assertRaises(IndexError):
      qr_df = qr_iterator.as_pandas()

  def test_invalid_cell_type_as_pandas(self):
    batch = ProtoFactory().CellsBatch()
    batch.cells.extend([
        TestQueryResultIterator.CELL_INVALID,
        TestQueryResultIterator.CELL_VARINT
    ])
    batch.varint_cells.extend([100, 200])
    batch.is_last_batch = True

    qr_iterator = TraceProcessor.QueryResultIterator(['foo_id', 'foo_num'],
                                                     [batch])

    # In this batch we declare the columns types to be CELL_INVALID,
    # CELL_VARINT but that doesn't match the data which are both ints*
    # so we should raise a TraceProcessorException.
    with self.assertRaises(TraceProcessorException):
      qr_df = qr_iterator.as_pandas()
