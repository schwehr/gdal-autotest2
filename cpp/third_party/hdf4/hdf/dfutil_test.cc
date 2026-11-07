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

#include <cstdint>
#include <filesystem>  // NOLINT
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf4/hdf/src/hdf.h"

namespace hdf4 {
namespace {

std::string GetTempFilePath(const std::string& name) {
  return (std::filesystem::path(::testing::TempDir()) / name).string();
}

TEST(DfutilTest, FindNextRefInvalidFid) {
  HEclear();
  EXPECT_EQ(static_cast<uint16_t>(FAIL),
            DFfindnextref(FAIL, DFTAG_RI, DFREF_WILDCARD));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
}

TEST(DfutilTest, FindNextRefNonExistent) {
  const std::string path = GetTempFilePath("dfutil_nonexistent.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    Hclose(fid);
    std::filesystem::remove(path);
  };

  HEclear();
  EXPECT_EQ(static_cast<uint16_t>(FAIL),
            DFfindnextref(fid, DFTAG_RI, DFREF_WILDCARD));
}

TEST(DfutilTest, FindNextRefTraversal) {
  const std::string path = GetTempFilePath("dfutil_traversal.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    Hclose(fid);
    std::filesystem::remove(path);
  };

  constexpr uint16_t kTag = DFTAG_RI;
  constexpr uint8_t kData1[] = "element1";
  constexpr uint8_t kData2[] = "element2";
  constexpr uint8_t kData3[] = "element3";

  ASSERT_NE(FAIL, Hputelement(fid, kTag, 10, kData1, sizeof(kData1)));
  ASSERT_NE(FAIL, Hputelement(fid, kTag, 20, kData2, sizeof(kData2)));
  ASSERT_NE(FAIL, Hputelement(fid, kTag, 30, kData3, sizeof(kData3)));

  // Wildcard search finds the first ref.
  EXPECT_EQ(10, DFfindnextref(fid, kTag, DFREF_WILDCARD));

  // Traversal following specific refs.
  EXPECT_EQ(20, DFfindnextref(fid, kTag, 10));
  EXPECT_EQ(30, DFfindnextref(fid, kTag, 20));

  // Beyond the last ref returns FAIL.
  EXPECT_EQ(static_cast<uint16_t>(FAIL), DFfindnextref(fid, kTag, 30));

  // Non-existent ref returns FAIL.
  EXPECT_EQ(static_cast<uint16_t>(FAIL), DFfindnextref(fid, kTag, 999));
}

}  // namespace
}  // namespace hdf4
