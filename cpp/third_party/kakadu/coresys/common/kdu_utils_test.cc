// Copyright 2017 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests for coresys/common/kdu_utils.h.

#include <limits>

#include "coresys/common/kdu_utils.h"
#include "gunit.h"

namespace kdu_core {
namespace {

// TODO(schwehr): Test kdu_read.
// TODO(schwehr): Test kdu_read_float.
// TODO(schwehr): Test kdu_read_double.

// Why kdu_round() when there is round()?
TEST(KduUtilsTest, KduRound) {
  EXPECT_DOUBLE_EQ(0.0, kdu_round(0.0));
  EXPECT_DOUBLE_EQ(0.0, kdu_round(0.4));
  EXPECT_DOUBLE_EQ(1.0, kdu_round(0.6));
  EXPECT_DOUBLE_EQ(0.0, kdu_round(-0.4));
  EXPECT_DOUBLE_EQ(-1.0, kdu_round(-0.6));
  // Not going to try to test double max and lowest.
}

TEST(KduUtilsTest, CeilRatio) {
  EXPECT_EQ(0, ceil_ratio(0, 1));
  EXPECT_EQ(1, ceil_ratio(1, 1));
  EXPECT_EQ(1, ceil_ratio(1, 10));
  EXPECT_EQ(10, ceil_ratio(10, 1));
  EXPECT_EQ(2, ceil_ratio(11, 10));
}

TEST(KduUtilsTest, CeilRatioBad) {
  EXPECT_THROW(ceil_ratio(1, 0), int);
  EXPECT_THROW(ceil_ratio(1, -1), int);

  // Test one exception explicitly.
  int exception = 0;
  int result;
  try {
    result = ceil_ratio(1, 0);
  } catch (int e) {
    exception = e;
    result = 0xDEADDEAD;
  }
  EXPECT_EQ(0xDEADDEAD, result);
  EXPECT_EQ(0xDEAD1, exception);
}

TEST(KduUtilsTest, FloorRatio) {
  EXPECT_EQ(0, floor_ratio(0, 1));
  EXPECT_EQ(1, floor_ratio(1, 1));
  EXPECT_EQ(0, floor_ratio(1, 10));
  EXPECT_EQ(10, floor_ratio(10, 1));
  EXPECT_EQ(1, floor_ratio(11, 10));

  EXPECT_THROW(floor_ratio(1, 0), int);
  EXPECT_THROW(floor_ratio(1, -1), int);
}

TEST(KduUtilsTest, LongCeilRatio) {
  EXPECT_EQ(0, long_ceil_ratio(0, 1));
  EXPECT_EQ(1, long_ceil_ratio(1, 1));
  EXPECT_EQ(1, long_ceil_ratio(1, 10));
  EXPECT_EQ(10, long_ceil_ratio(10, 1));
  EXPECT_EQ(2, long_ceil_ratio(11, 10));

  EXPECT_THROW(long_ceil_ratio(1, 0), int);
  EXPECT_THROW(long_ceil_ratio(1, -1), int);
}

TEST(KduUtilsTest, LongFloorRatio) {
  EXPECT_EQ(0, long_floor_ratio(0, 1));
  EXPECT_EQ(1, long_floor_ratio(1, 1));
  EXPECT_EQ(0, long_floor_ratio(1, 10));
  EXPECT_EQ(10, long_floor_ratio(10, 1));
  EXPECT_EQ(1, long_floor_ratio(11, 10));

  EXPECT_THROW(long_floor_ratio(1, 0), int);
  EXPECT_THROW(long_floor_ratio(1, -1), int);
}

// TODO(schwehr): Test kdu_hex_hex_decode.
// TODO(schwehr): Test kdu_hex_hex_encode.
// TODO(schwehr): Test kdu_count_leading_zeros.
// TODO(schwehr): Test kdu_count_trailing_zeros.

TEST(KduMemSafe, Add) {
  // No overflow cases
  EXPECT_EQ(0, kdu_memsafe_add(0, 0));

  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_add(0, std::numeric_limits<size_t>::max()));
  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_add(std::numeric_limits<size_t>::max(), 0));

  EXPECT_EQ(KDU_SIZE_MAX - 1,
            kdu_memsafe_add(std::numeric_limits<size_t>::max() - 2, 1));
  EXPECT_EQ(KDU_SIZE_MAX - 1,
            kdu_memsafe_add(1, std::numeric_limits<size_t>::max() - 2));

  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_add(std::numeric_limits<size_t>::max(), 0));
  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_add(0, std::numeric_limits<size_t>::max()));

  // Overflows
  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_add(std::numeric_limits<size_t>::max(), 1));
  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_add(1, std::numeric_limits<size_t>::max()));

  EXPECT_EQ(KDU_SIZE_MAX, kdu_memsafe_add(std::numeric_limits<size_t>::max(),
                                          std::numeric_limits<size_t>::max()));
}

TEST(KduMemSafe, Multiply) {
  // No overflow cases
  EXPECT_EQ(0, kdu_memsafe_mul(0, 0));

  EXPECT_EQ(0, kdu_memsafe_mul(0, std::numeric_limits<size_t>::max()));
  EXPECT_EQ(0, kdu_memsafe_mul(std::numeric_limits<size_t>::max(), 0));

  EXPECT_EQ(KDU_SAFE_ROOT_SIZE_MAX, kdu_memsafe_mul(1, KDU_SAFE_ROOT_SIZE_MAX));
  EXPECT_EQ(KDU_SAFE_ROOT_SIZE_MAX, kdu_memsafe_mul(KDU_SAFE_ROOT_SIZE_MAX, 1));

  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_mul(1, std::numeric_limits<size_t>::max()));
  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_mul(std::numeric_limits<size_t>::max(), 1));

  // Overflows
  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_mul(2, std::numeric_limits<size_t>::max()));
  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_mul(std::numeric_limits<size_t>::max(), 2));

  EXPECT_EQ(KDU_SIZE_MAX, kdu_memsafe_mul(std::numeric_limits<size_t>::max(),
                                          std::numeric_limits<size_t>::max()));
}

TEST(KduMemSafe, MultiplyAndOffset) {
  // No overflow cases
  EXPECT_EQ(0, kdu_memsafe_mulo(0, 0, 0));

  EXPECT_EQ(0, kdu_memsafe_mulo(1, 0, 0));
  EXPECT_EQ(0, kdu_memsafe_mulo(0, 1, 0));

  EXPECT_EQ(1, kdu_memsafe_mulo(0, 0, 1));

  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_mulo(std::numeric_limits<size_t>::max(), 1, 0));
  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_mulo(1, std::numeric_limits<size_t>::max(), 0));
  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_mulo(0, 0, std::numeric_limits<size_t>::max()));

  EXPECT_EQ(KDU_SIZE_MAX - 1,
            kdu_memsafe_mulo(std::numeric_limits<size_t>::max() - 2, 1, 1));
  EXPECT_EQ(KDU_SIZE_MAX - 1,
            kdu_memsafe_mulo(1, std::numeric_limits<size_t>::max() - 2, 1));

  // Overflows
  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_mulo(1, std::numeric_limits<size_t>::max(), 1));
  EXPECT_EQ(KDU_SIZE_MAX,
            kdu_memsafe_mulo(1, 1, std::numeric_limits<size_t>::max()));

  EXPECT_EQ(KDU_SIZE_MAX, kdu_memsafe_mulo(std::numeric_limits<size_t>::max(),
                                           std::numeric_limits<size_t>::max(),
                                           std::numeric_limits<size_t>::max()));
}

}  // namespace
}  // namespace kdu_core
