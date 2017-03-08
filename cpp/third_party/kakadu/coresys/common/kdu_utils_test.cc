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

#include "gunit.h"
#include "coresys/common/kdu_utils.h"

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

TEST(KduUtilsTest, FloorRatio) {
  EXPECT_EQ(0, floor_ratio(0, 1));
  EXPECT_EQ(1, floor_ratio(1, 1));
  EXPECT_EQ(0, floor_ratio(1, 10));
  EXPECT_EQ(10, floor_ratio(10, 1));
  EXPECT_EQ(1, floor_ratio(11, 10));
}

TEST(KduUtilsTest, LongCeilRatio) {
  EXPECT_EQ(0, long_ceil_ratio(0, 1));
  EXPECT_EQ(1, long_ceil_ratio(1, 1));
  EXPECT_EQ(1, long_ceil_ratio(1, 10));
  EXPECT_EQ(10, long_ceil_ratio(10, 1));
  EXPECT_EQ(2, long_ceil_ratio(11, 10));
}

TEST(KduUtilsTest, LongFloorRatio) {
  EXPECT_EQ(0, long_floor_ratio(0, 1));
  EXPECT_EQ(1, long_floor_ratio(1, 1));
  EXPECT_EQ(0, long_floor_ratio(1, 10));
  EXPECT_EQ(10, long_floor_ratio(10, 1));
  EXPECT_EQ(1, long_floor_ratio(11, 10));
}

// TODO(schwehr): Test kdu_hex_hex_decode.
// TODO(schwehr): Test kdu_hex_hex_encode.
// TODO(schwehr): Test kdu_count_leading_zeros.
// TODO(schwehr): Test kdu_count_trailing_zeros.

}  // namespace
}  // namespace kdu_core
