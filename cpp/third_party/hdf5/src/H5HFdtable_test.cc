// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests fractal heap doubling-table initialization, destruction, offset lookup,
// row/block-size calculations, and entry-span sizing
// (third_party/hdf5/src/H5HFdtable.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5HFdtable.c

#include <cstdint>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields from
// H5C_cache_entry_t (H5AC_info_t) inside internal fractal heap structs.
#ifndef NDEBUG
#define H5HFDTABLE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
// H5HFpkg.h pulls in H5B2private.h, which uses `try` as a C parameter name,
// colliding with both the C++ keyword and Google3's `-fno-exceptions` macro.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try hdf5_param_try
extern "C" {
#define H5HF_FRIEND
#include "third_party/hdf5/src/H5HFpkg.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5HFDTABLE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5HFDTABLE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

H5HF_dtable_t MakeTable(unsigned width, size_t start_block_size,
                        size_t max_direct_size, unsigned max_index,
                        unsigned start_root_rows = 1) {
  H5HF_dtable_t dtable{};
  dtable.cparam.width = width;
  dtable.cparam.start_block_size = start_block_size;
  dtable.cparam.max_direct_size = max_direct_size;
  dtable.cparam.max_index = max_index;
  dtable.cparam.start_root_rows = start_root_rows;
  return dtable;
}

TEST(H5HFdtableTest, InitAndDestVerifyDerivedGeometryAndCleanup) {
  ASSERT_GE(H5open(), 0);

  // Destroying a zero-initialized doubling table (with null arrays) succeeds.
  {
    H5HF_dtable_t empty{};
    EXPECT_GE(H5HF__dtable_dest(&empty), 0);
  }

  // Width = 4, start = 512, max_direct = 4096, max_index = 20.
  {
    H5HF_dtable_t dtable = MakeTable(/*width=*/4, /*start_block_size=*/512,
                                     /*max_direct_size=*/4096, /*max_index=*/20);
    ASSERT_GE(H5HF__dtable_init(&dtable), 0);
    absl::Cleanup destroy = [&] { EXPECT_GE(H5HF__dtable_dest(&dtable), 0); };

    EXPECT_EQ(dtable.start_bits, 9U);
    EXPECT_EQ(dtable.first_row_bits, 11U);
    EXPECT_EQ(dtable.max_root_rows, 10U);
    EXPECT_EQ(dtable.max_direct_bits, 12U);
    EXPECT_EQ(dtable.max_direct_rows, 5U);
    EXPECT_EQ(dtable.num_id_first_row, 2048U);
    EXPECT_EQ(dtable.max_dir_blk_off_size, 2U);

    EXPECT_EQ(dtable.row_block_size[0], 512U);
    EXPECT_EQ(dtable.row_block_off[0], 0U);
    EXPECT_EQ(dtable.row_block_size[1], 512U);
    EXPECT_EQ(dtable.row_block_off[1], 2048U);
    EXPECT_EQ(dtable.row_block_size[2], 1024U);
    EXPECT_EQ(dtable.row_block_off[2], 4096U);
    EXPECT_EQ(dtable.row_block_size[4], 4096U);
    EXPECT_EQ(dtable.row_block_size[5], 8192U);
    EXPECT_EQ(dtable.row_block_off[5], 32768U);
  }

  // Width = 1 minimal configuration.
  {
    H5HF_dtable_t dtable = MakeTable(/*width=*/1, /*start_block_size=*/64,
                                     /*max_direct_size=*/256, /*max_index=*/12);
    ASSERT_GE(H5HF__dtable_init(&dtable), 0);
    absl::Cleanup destroy = [&] { EXPECT_GE(H5HF__dtable_dest(&dtable), 0); };

    EXPECT_EQ(dtable.start_bits, 6U);
    EXPECT_EQ(dtable.first_row_bits, 6U);
    EXPECT_EQ(dtable.max_root_rows, 7U);
    EXPECT_EQ(dtable.max_direct_rows, 4U);
    EXPECT_EQ(dtable.num_id_first_row, 64U);
    EXPECT_EQ(dtable.row_block_size[0], 64U);
    EXPECT_EQ(dtable.row_block_size[1], 64U);
    EXPECT_EQ(dtable.row_block_off[1], 64U);
    EXPECT_EQ(dtable.row_block_size[3], 256U);
  }
}

TEST(H5HFdtableTest, LookupFirstRowAndHigherDoublingRows) {
  ASSERT_GE(H5open(), 0);

  H5HF_dtable_t dtable = MakeTable(/*width=*/4, /*start_block_size=*/64,
                                   /*max_direct_size=*/512, /*max_index=*/16);
  ASSERT_GE(H5HF__dtable_init(&dtable), 0);
  absl::Cleanup destroy = [&] { EXPECT_GE(H5HF__dtable_dest(&dtable), 0); };

  // Offsets inside row 0: off < 64 * 4 = 256.
  struct Case {
    hsize_t off;
    unsigned want_row;
    unsigned want_col;
  };
  const Case cases[] = {
      {0, 0, 0},
      {63, 0, 0},
      {64, 0, 1},
      {191, 0, 2},
      {255, 0, 3},
      // Row 1: [256, 512), block size 64.
      {256, 1, 0},
      {320, 1, 1},
      {511, 1, 3},
      // Row 2: [512, 1024), block size 128.
      {512, 2, 0},
      {768, 2, 2},
      {1023, 2, 3},
      // Row 3: [1024, 2048), block size 256.
      {1536, 3, 2},
      // Row 5 (indirect row): [4096, 8192), block size 1024.
      {4096, 5, 0},
      {7200, 5, 3},
  };
  for (const Case& c : cases) {
    unsigned row = 999;
    unsigned col = 999;
    EXPECT_GE(H5HF__dtable_lookup(&dtable, c.off, &row, &col), 0);
    EXPECT_EQ(row, c.want_row) << "off=" << c.off;
    EXPECT_EQ(col, c.want_col) << "off=" << c.off;
  }
}

TEST(H5HFdtableTest, SizeToRowAndSizeToRows) {
  ASSERT_GE(H5open(), 0);

  H5HF_dtable_t dtable = MakeTable(/*width=*/4, /*start_block_size=*/64,
                                   /*max_direct_size=*/512, /*max_index=*/16);
  ASSERT_GE(H5HF__dtable_init(&dtable), 0);
  absl::Cleanup destroy = [&] { EXPECT_GE(H5HF__dtable_dest(&dtable), 0); };

  // H5HF__dtable_size_to_row checks.
  EXPECT_EQ(H5HF__dtable_size_to_row(&dtable, 64), 0U);
  EXPECT_EQ(H5HF__dtable_size_to_row(&dtable, 128), 2U);
  EXPECT_EQ(H5HF__dtable_size_to_row(&dtable, 256), 3U);
  EXPECT_EQ(H5HF__dtable_size_to_row(&dtable, 512), 4U);

  // H5HF__dtable_size_to_rows checks (num_id_first_row = 256).
  EXPECT_EQ(H5HF__dtable_size_to_rows(&dtable, 256), 1U);
  EXPECT_EQ(H5HF__dtable_size_to_rows(&dtable, 512), 2U);
  EXPECT_EQ(H5HF__dtable_size_to_rows(&dtable, 1024), 3U);
  EXPECT_EQ(H5HF__dtable_size_to_rows(&dtable, 4096), 5U);
}

TEST(H5HFdtableTest, SpanSizeSameRowAdjacentRowsAndMultiRowWithPartialEdges) {
  ASSERT_GE(H5open(), 0);

  H5HF_dtable_t dtable = MakeTable(/*width=*/4, /*start_block_size=*/64,
                                   /*max_direct_size=*/512, /*max_index=*/16);
  ASSERT_GE(H5HF__dtable_init(&dtable), 0);
  absl::Cleanup destroy = [&] { EXPECT_GE(H5HF__dtable_dest(&dtable), 0); };

  // Same row: single entry and multi-entry spans.
  EXPECT_EQ(H5HF__dtable_span_size(&dtable, 0, 0, 1), 64U);
  EXPECT_EQ(H5HF__dtable_span_size(&dtable, 2, 1, 3), 384U);  // 3 * 128

  // Two adjacent rows starting at col 0 (while(start_row < end_row) skipped).
  // Row 0 full (4 * 64 = 256) + Row 1 cols 0..1 (2 * 64 = 128) = 384.
  EXPECT_EQ(H5HF__dtable_span_size(&dtable, 0, 0, 6), 384U);

  // Multi-row starting at col 0 with full intermediate row:
  // Row 0 full (256) + Row 1 full (256) + Row 2 cols 0..1 (2 * 128 = 256) = 768.
  EXPECT_EQ(H5HF__dtable_span_size(&dtable, 0, 0, 10), 768U);

  // Multi-row starting at col > 0 with multiple full intermediate rows:
  // Start row 0 col 2, 12 entries => ends at row 3 col 1.
  // Row 0 cols 2..3: 2 * 64 = 128
  // Row 1 full:      4 * 64 = 256
  // Row 2 full:      4 * 128 = 512
  // Row 3 cols 0..1: 2 * 256 = 512
  // Total = 1408.
  EXPECT_EQ(H5HF__dtable_span_size(&dtable, 0, 2, 12), 1408U);
}

}  // namespace
}  // namespace hdf5
