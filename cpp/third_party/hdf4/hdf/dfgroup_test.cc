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
#include <fstream>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/hlimits.h"

namespace hdf4 {
namespace {

std::string GetTempFilePath(const std::string& name) {
  return (std::filesystem::path(::testing::TempDir()) / name).string();
}

TEST(DfgroupTest, DisetupAndDifree) {
  const int32_t gid = DFdisetup(4);
  ASSERT_NE(gid, FAIL);
  EXPECT_EQ(DFdinobj(gid), 4);
  DFdifree(gid);
  // difree on invalid or already-freed group is a safe no-op.
  DFdifree(gid);
  DFdifree(FAIL);
  DFdifree(0);
}

TEST(DfgroupTest, GroupLimitExceeded) {
  std::vector<int32_t> gids;
  gids.reserve(MAX_GROUPS);
  for (int i = 0; i < MAX_GROUPS; ++i) {
    const int32_t gid = DFdisetup(1);
    ASSERT_NE(gid, FAIL);
    gids.push_back(gid);
  }

  HEclear();
  EXPECT_EQ(DFdisetup(1), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_INTERNAL);

  for (int32_t gid : gids) {
    DFdifree(gid);
  }
}

TEST(DfgroupTest, DiputErrors) {
  HEclear();
  EXPECT_EQ(DFdiput(FAIL, DFTAG_RI, 1), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_ARGS);

  HEclear();
  EXPECT_EQ(DFdiput(0, DFTAG_RI, 1), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_ARGS);

  const int32_t gid = DFdisetup(2);
  ASSERT_NE(gid, FAIL);
  absl::Cleanup group_cleanup = [&]() { DFdifree(gid); };

  EXPECT_EQ(DFdiput(gid, DFTAG_RI, 1), SUCCEED);
  EXPECT_EQ(DFdiput(gid, DFTAG_CI, 2), SUCCEED);

  // Exceeding allocated capacity returns FAIL with DFE_INTERNAL.
  HEclear();
  EXPECT_EQ(DFdiput(gid, DFTAG_LUT, 3), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_INTERNAL);
}

TEST(DfgroupTest, DinobjErrors) {
  HEclear();
  EXPECT_EQ(DFdinobj(FAIL), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_ARGS);

  HEclear();
  EXPECT_EQ(DFdinobj(0), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_ARGS);
}

TEST(DfgroupTest, DiwriteErrors) {
  const int32_t gid = DFdisetup(1);
  ASSERT_NE(gid, FAIL);

  HEclear();
  EXPECT_EQ(DFdiwrite(FAIL, gid, DFTAG_RIG, 1), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_ARGS);
  DFdifree(gid);

  const std::string path = GetTempFilePath("diwrite_err.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    Hclose(fid);
    std::filesystem::remove(path);
  };

  HEclear();
  EXPECT_EQ(DFdiwrite(fid, FAIL, DFTAG_RIG, 1), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_ARGS);
}

TEST(DfgroupTest, DiwriteReadOnlyFailure) {
  const std::string path = GetTempFilePath("diwrite_readonly.hdf");
  const int32_t create_fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(create_fid, FAIL);
  ASSERT_NE(Hclose(create_fid), FAIL);

  const int32_t read_fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(read_fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    Hclose(read_fid);
    std::filesystem::remove(path);
  };

  const int32_t gid = DFdisetup(1);
  ASSERT_NE(gid, FAIL);
  EXPECT_EQ(DFdiput(gid, DFTAG_RI, 1), SUCCEED);

  // DFdiwrite fails because file is read-only.
  // Note: DFdiwrite always frees the group even on Hputelement error.
  HEclear();
  EXPECT_EQ(DFdiwrite(read_fid, gid, DFTAG_RIG, 1), FAIL);
  EXPECT_EQ(DFdinobj(gid), FAIL);
}

TEST(DfgroupTest, DireadErrors) {
  HEclear();
  EXPECT_EQ(DFdiread(FAIL, DFTAG_RIG, 1), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_ARGS);

  const std::string path = GetTempFilePath("diread_err.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    Hclose(fid);
    std::filesystem::remove(path);
  };

  HEclear();
  EXPECT_EQ(DFdiread(fid, DFTAG_RIG, 999), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_INTERNAL);
}

TEST(DfgroupTest, DigetErrors) {
  uint16_t tag = 0;
  uint16_t ref = 0;

  HEclear();
  EXPECT_EQ(DFdiget(FAIL, &tag, &ref), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_ARGS);

  // Group with maxsize 0 has current >= num (0 >= 0).
  const int32_t gid = DFdisetup(0);
  ASSERT_NE(gid, FAIL);
  HEclear();
  EXPECT_EQ(DFdiget(gid, &tag, &ref), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_INTERNAL);
  DFdifree(gid);
}

TEST(DfgroupTest, ReadWriteRoundTrip) {
  const std::string path = GetTempFilePath("dfgroup_roundtrip.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    Hclose(fid);
    std::filesystem::remove(path);
  };

  const int32_t write_gid = DFdisetup(3);
  ASSERT_NE(write_gid, FAIL);
  EXPECT_EQ(DFdiput(write_gid, DFTAG_RI, 101), SUCCEED);
  EXPECT_EQ(DFdiput(write_gid, DFTAG_CI, 102), SUCCEED);
  EXPECT_EQ(DFdiput(write_gid, DFTAG_LUT, 103), SUCCEED);

  EXPECT_GT(DFdiwrite(fid, write_gid, DFTAG_RIG, 50), 0);

  // Read group back.
  const int32_t read_gid = DFdiread(fid, DFTAG_RIG, 50);
  ASSERT_NE(read_gid, FAIL);
  EXPECT_EQ(DFdinobj(read_gid), 3);

  uint16_t tag = 0;
  uint16_t ref = 0;
  EXPECT_EQ(DFdiget(read_gid, &tag, &ref), SUCCEED);
  EXPECT_EQ(tag, DFTAG_RI);
  EXPECT_EQ(ref, 101);

  EXPECT_EQ(DFdiget(read_gid, &tag, &ref), SUCCEED);
  EXPECT_EQ(tag, DFTAG_CI);
  EXPECT_EQ(ref, 102);

  EXPECT_EQ(DFdiget(read_gid, &tag, &ref), SUCCEED);
  EXPECT_EQ(tag, DFTAG_LUT);
  EXPECT_EQ(ref, 103);

  // DFdiget automatically frees the group once all DIs are read.
  HEclear();
  EXPECT_EQ(DFdiget(read_gid, &tag, &ref), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_ARGS);
}

TEST(DfgroupTest, ReadPartialAndFree) {
  const std::string path = GetTempFilePath("dfgroup_partial.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    Hclose(fid);
    std::filesystem::remove(path);
  };

  const int32_t write_gid = DFdisetup(2);
  ASSERT_NE(write_gid, FAIL);
  EXPECT_EQ(DFdiput(write_gid, DFTAG_RI, 201), SUCCEED);
  EXPECT_EQ(DFdiput(write_gid, DFTAG_CI, 202), SUCCEED);
  EXPECT_GT(DFdiwrite(fid, write_gid, DFTAG_RIG, 60), 0);

  const int32_t read_gid = DFdiread(fid, DFTAG_RIG, 60);
  ASSERT_NE(read_gid, FAIL);

  uint16_t tag = 0;
  uint16_t ref = 0;
  EXPECT_EQ(DFdiget(read_gid, &tag, &ref), SUCCEED);
  EXPECT_EQ(tag, DFTAG_RI);
  EXPECT_EQ(ref, 201);

  // Partially read group explicitly freed.
  DFdifree(read_gid);
  EXPECT_EQ(DFdinobj(read_gid), FAIL);
}

TEST(DfgroupTest, CorruptedElementReadError) {
  const std::string path = GetTempFilePath("dfgroup_corrupt.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);

  // Write a group tag with 8 bytes of data.
  const uint8_t dummy_data[8] = {0, 1, 0, 2, 0, 3, 0, 4};
  ASSERT_NE(Hputelement(fid, DFTAG_RIG, 77, dummy_data, sizeof(dummy_data)),
            FAIL);
  ASSERT_NE(Hclose(fid), FAIL);

  // Truncate the file so that the DD block is still valid but the data offset
  // is past the end of the file.
  const auto original_size = std::filesystem::file_size(path);
  ASSERT_GT(original_size, sizeof(dummy_data));
  std::filesystem::resize_file(path, original_size - sizeof(dummy_data));

  const int32_t corrupt_fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(corrupt_fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    Hclose(corrupt_fid);
    std::filesystem::remove(path);
  };

  HEclear();
  EXPECT_EQ(DFdiread(corrupt_fid, DFTAG_RIG, 77), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_READERROR);
}

}  // namespace
}  // namespace hdf4
