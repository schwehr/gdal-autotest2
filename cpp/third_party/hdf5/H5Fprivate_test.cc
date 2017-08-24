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
// Tests for C pre processor macros in hdf5/src/H5Fprivate.h.
//
// See also:
//   hdf5/test/tmeta.c

#include <cstdio>  // This has to be first.

#include "src/H5Fprivate.h"
#include "src/hdf5.h"

#include <cstdint>  // NOLINT(build/include_order)
#include <limits>  // NOLINT(build/include_order)
#include <vector>  // NOLINT(build/include_order)

#include "gmock.h"
#include "gunit.h"

namespace {

std::vector<uint8_t> Int16Encode(int16_t value) {
  std::vector<uint8_t> result(sizeof(value), 0);
  uint8_t *ptr = result.data();
  INT16ENCODE(ptr, value);
  return result;
}

TEST(H5FTest, Int16Encode) {
  EXPECT_THAT(Int16Encode(0), testing::ElementsAre(0x00, 0x00));
  EXPECT_THAT(Int16Encode(1), testing::ElementsAre(0x01, 0x00));
  EXPECT_THAT(Int16Encode(-1), testing::ElementsAre(0xff, 0xff));
  EXPECT_THAT(Int16Encode(std::numeric_limits<int16_t>::max()),
              testing::ElementsAre(0xff, 0x7f));
  EXPECT_THAT(Int16Encode(std::numeric_limits<int16_t>::min()),
              testing::ElementsAre(0x00, 0x80));
  EXPECT_THAT(Int16Encode(-7641), testing::ElementsAre(0x27, 0xe2));
}

std::vector<uint8_t> UInt16Encode(uint16_t value) {
  std::vector<uint8_t> result(sizeof(value), 0);
  uint8_t *ptr = result.data();
  UINT16ENCODE(ptr, value);
  return result;
}

TEST(H5FTest, UInt16Encode) {
  EXPECT_THAT(UInt16Encode(0u), testing::ElementsAre(0x00, 0x00));
  EXPECT_THAT(UInt16Encode(1u), testing::ElementsAre(0x01, 0x00));
  EXPECT_THAT(UInt16Encode(std::numeric_limits<uint16_t>::max()),
              testing::ElementsAre(0xff, 0xff));
  EXPECT_THAT(UInt16Encode(45002u), testing::ElementsAre(0xca, 0xaf));
}

std::vector<uint8_t> Int32Encode(int32_t value) {
  std::vector<uint8_t> result(sizeof(value), 0);
  uint8_t *ptr = result.data();;
  INT32ENCODE(ptr, value);
  return result;
}

TEST(H5FTest, Int32Encode) {
  EXPECT_THAT(Int32Encode(0), testing::ElementsAre(0x00, 0x00, 0x00, 0x00));
  EXPECT_THAT(Int32Encode(1), testing::ElementsAre(0x01, 0x00, 0x00, 0x00));
  EXPECT_THAT(Int32Encode(-1), testing::ElementsAre(0xff, 0xff, 0xff, 0xff));
  EXPECT_THAT(Int32Encode(std::numeric_limits<int32_t>::max()),
              testing::ElementsAre(0xff, 0xff, 0xff, 0x7f));
  EXPECT_THAT(Int32Encode(std::numeric_limits<int32_t>::min()),
              testing::ElementsAre(0x00, 0x00, 0x00, 0x80));
  EXPECT_THAT(Int32Encode(-981236),
              testing::ElementsAre(0x0c, 0x07, 0xf1, 0xff));
}

std::vector<uint8_t> UInt32Encode(uint32_t value) {
  std::vector<uint8_t> result(sizeof(value), 0);
  uint8_t *ptr = result.data();
  INT32ENCODE(ptr, value);
  return result;
}

TEST(H5FTest, UInt32Encode) {
  EXPECT_THAT(UInt32Encode(0u), testing::ElementsAre(0x00, 0x00, 0x00, 0x00));
  EXPECT_THAT(UInt32Encode(1u), testing::ElementsAre(0x01, 0x00, 0x00, 0x00));
  EXPECT_THAT(UInt32Encode(std::numeric_limits<uint32_t>::max()),
              testing::ElementsAre(0xff, 0xff, 0xff, 0xff));
  EXPECT_THAT(UInt32Encode(3476589u),
              testing::ElementsAre(0x6D, 0x0C, 0x35, 0x00));
}

// TODO(schwehr): Test *DECODE
// TODO(schwehr): Test H5F_addr_*

}  // namespace
