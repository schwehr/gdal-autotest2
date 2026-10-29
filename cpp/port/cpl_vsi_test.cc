// Test simple implementation of POSIX VSI functions:
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_vsisimple.cpp
//
// Copyright 2024 Google Inc. All Rights Reserved.
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

#include "third_party/gdal/port/cpl_vsi.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/substitute.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_error.h"

namespace autotest2 {
namespace {

constexpr size_t kReasonablySmallSize = 10;

class ResetHelper : public ::testing::Test {
 protected:
  void SetUp() override {
    // Prevent prior tests from impacting the current test.
    CPLErrorReset();
  }
};

class VsiMallocTest : public ResetHelper {};

TEST_F(VsiMallocTest, VSIMallocSuccessful) {
  void *ptr = VSIMalloc(kReasonablySmallSize);
  absl::Cleanup ptr_cleaner = [ptr] { VSIFree(ptr); };

  EXPECT_NE(ptr, nullptr);
  ASSERT_EQ(CPLGetLastErrorType(), CE_None);
}

TEST_F(VsiMallocTest, VSIMallocTooBig) {
  void *ptr = VSIMalloc(SIZE_MAX);
  absl::Cleanup ptr_cleaner = [ptr] { VSIFree(ptr); };

  EXPECT_EQ(ptr, nullptr);
  ASSERT_EQ(CPLGetLastErrorType(), CE_None);
}

class VsiReallocTest : public ResetHelper {};

TEST_F(VsiReallocTest, VSIReallocSuccessful) {
  void *ptr = VSIRealloc(nullptr, kReasonablySmallSize);
  absl::Cleanup ptr_cleaner = [ptr] { VSIFree(ptr); };

  EXPECT_NE(ptr, nullptr);
  ASSERT_EQ(CPLGetLastErrorType(), CE_None);
}

TEST_F(VsiReallocTest, VSIReallocTooBig) {
  void *ptr = VSIRealloc(nullptr, SIZE_MAX);
  absl::Cleanup ptr_cleaner = [ptr] { VSIFree(ptr); };

  EXPECT_EQ(ptr, nullptr);
  ASSERT_EQ(CPLGetLastErrorType(), CE_None);
}

class VsiCallocTest : public ResetHelper {};

TEST_F(VsiCallocTest, VSICallocSuccessful) {
  void *ptr = VSICalloc(1, kReasonablySmallSize);
  absl::Cleanup ptr_cleaner = [ptr] { VSIFree(ptr); };

  EXPECT_NE(ptr, nullptr);
  ASSERT_EQ(CPLGetLastErrorType(), CE_None);
}

TEST_F(VsiCallocTest, VSICallocTooBig) {
  void *ptr = VSICalloc(1, SIZE_MAX);
  absl::Cleanup ptr_cleaner = [ptr] { VSIFree(ptr); };

  EXPECT_EQ(ptr, nullptr);
  ASSERT_EQ(CPLGetLastErrorType(), CE_None);
}

class VsiMalloc2Test : public ResetHelper {};

TEST_F(VsiMalloc2Test, VSIMalloc2Overflow) {
  WithQuietHandler error_handler;

  void *ptr = VSIMalloc2(SIZE_MAX, SIZE_MAX);
  absl::Cleanup ptr_cleaner = [ptr] { VSIFree(ptr); };

  EXPECT_EQ(ptr, nullptr);
  ASSERT_EQ(CPLGetLastErrorType(), CE_Failure);
}

TEST_F(VsiMalloc2Test, VSIMalloc2OverflowOneLargeSize) {
  WithQuietHandler error_handler;

  void *ptr = VSIMalloc2(SIZE_MAX, 1);
  absl::Cleanup ptr_cleaner = [ptr] { VSIFree(ptr); };

  EXPECT_EQ(ptr, nullptr);
  ASSERT_EQ(CPLGetLastErrorType(), CE_Failure);
}

struct VsiMalloc3TestCase {
  size_t nSize1;
  size_t nSize2;
  size_t nSize3;
};

class VsiMalloc3OverflowTest
    : public ::testing::TestWithParam<VsiMalloc3TestCase> {
 protected:
  void SetUp() override {
    // Prevent prior tests from impacting the current test.
    CPLErrorReset();
  }
};

TEST_P(VsiMalloc3OverflowTest, ShouldOverflow) {
  WithQuietHandler error_handler;

  const VsiMalloc3TestCase &tc = GetParam();

  void *ptr = VSIMalloc3(tc.nSize1, tc.nSize2, tc.nSize3);
  absl::Cleanup ptr_cleaner = [ptr] { VSIFree(ptr); };

  EXPECT_EQ(ptr, nullptr);
  ASSERT_EQ(CPLGetLastErrorType(), CE_Failure);
}

INSTANTIATE_TEST_SUITE_P(
    VsiMalloc3OverflowTestInstantiation, VsiMalloc3OverflowTest,
    ::testing::ValuesIn<VsiMalloc3TestCase>({
        {1, 1, SIZE_MAX},
        {1, SIZE_MAX, 1},
        {SIZE_MAX, 1, 1},
        {1, SIZE_MAX, SIZE_MAX},
        {SIZE_MAX, 1, SIZE_MAX},
        {SIZE_MAX, SIZE_MAX, 1},
    }),
    [](const testing::TestParamInfo<VsiMalloc3OverflowTest::ParamType> &info) {
      const VsiMalloc3TestCase &param = info.param;
      return absl::Substitute("Size$0x$1x$2", param.nSize1, param.nSize2,
                              param.nSize3);
    });

TEST(VsiMallocAligned, AllocatesAccordingToRequestedAlignment) {
  const int kAlignment = 128;
  const int kRequestedSize = 64;
  char *mem = static_cast<char *>(VSIMallocAligned(kAlignment, kRequestedSize));
  absl::Cleanup memory_cleanup = [mem] { VSIFreeAligned(mem); };

  EXPECT_NE(mem, nullptr);
  EXPECT_EQ(reinterpret_cast<uint64_t>(mem) % kAlignment, 0);

  /* Test that the full requested size can be used. */
  const char kTestChar = 'A';
  std::memset(mem, kTestChar, kRequestedSize);

  for (int i = 0; i < kRequestedSize; ++i) {
    EXPECT_EQ(mem[i], kTestChar);
  }
}

struct VsiMallocAlignmentTestCase {
  std::string test_name;
  int alignment;
};

using VsiMallocAlignedAlignmentTest =
    testing::TestWithParam<VsiMallocAlignmentTestCase>;

TEST_P(VsiMallocAlignedAlignmentTest, DisallowedAlignmentValues) {
  const VsiMallocAlignmentTestCase &test_case = GetParam();

  void *mem = VSIMallocAligned(test_case.alignment, 64);
  EXPECT_EQ(mem, nullptr);
}

INSTANTIATE_TEST_SUITE_P(
    VsiMallocAlignedAlignmentTestSuiteInstantiation,
    VsiMallocAlignedAlignmentTest,
    testing::ValuesIn<VsiMallocAlignmentTestCase>({
        {"NotPowerOf2", 3},
        {"NotMultipleOfPointerSize", 4},
        {"AlsoNotMultipleOfPointerSize", 1},
    }),
    [](const testing::TestParamInfo<VsiMallocAlignedAlignmentTest::ParamType>
           &info) { return info.param.test_name; });

}  // namespace
}  // namespace autotest2
