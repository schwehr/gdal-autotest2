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
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf4/hdf/src/hdf.h"

namespace hdf4 {
namespace {

constexpr size_t kPaletteSize = 768;

class DfpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    DFPrestart();
    HEclear();
  }

  void TearDown() override {
    DFPrestart();
    HEclear();
  }

  std::string GetTempFilePath(const std::string& name) {
    return (std::filesystem::path(::testing::TempDir()) / name).string();
  }

  std::vector<uint8_t> MakeTestPalette(uint8_t seed) {
    std::vector<uint8_t> pal(kPaletteSize);
    for (size_t i = 0; i < pal.size(); ++i) {
      pal[i] = static_cast<uint8_t>((seed + i) % 256);
    }
    return pal;
  }
};

TEST_F(DfpTest, GetpalErrors) {
  HEclear();
  EXPECT_EQ(DFPgetpal("non_existent.hdf", nullptr), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_ARGS);

  std::vector<uint8_t> pal(kPaletteSize);
  HEclear();
  EXPECT_EQ(DFPgetpal("non_existent_file_to_open.hdf", pal.data()), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_BADOPEN);

  const std::string empty_path = GetTempFilePath("dfp_empty.hdf");
  const int32_t empty_fid = Hopen(empty_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(empty_fid, FAIL);
  ASSERT_NE(Hclose(empty_fid), FAIL);
  absl::Cleanup file_cleanup = [&]() { std::filesystem::remove(empty_path); };

  EXPECT_EQ(DFPgetpal(empty_path.c_str(), pal.data()), FAIL);
}

TEST_F(DfpTest, PutpalErrors) {
  HEclear();
  EXPECT_EQ(DFPputpal("file.hdf", nullptr, 0, "w"), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_ARGS);

  const auto pal = MakeTestPalette(10);
  DFPrestart();
  HEclear();
  EXPECT_EQ(DFPputpal("overwrite_non_matching.hdf", pal.data(), 1, "w"), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_BADCALL);

  // Appending to a non-existent file fails in DFPIopen with DFE_BADOPEN.
  HEclear();
  EXPECT_EQ(DFPputpal("non_existent_append.hdf", pal.data(), 0, "a"), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_BADOPEN);
}

TEST_F(DfpTest, BasicPutAndGetRoundTrip) {
  const std::string path = GetTempFilePath("dfp_roundtrip.hdf");
  absl::Cleanup file_cleanup = [&]() { std::filesystem::remove(path); };

  const auto pal1 = MakeTestPalette(42);
  ASSERT_EQ(DFPputpal(path.c_str(), pal1.data(), 0, "w"), SUCCEED);
  const uint16_t ref1 = DFPlastref();
  EXPECT_GT(ref1, 0);

  std::vector<uint8_t> read_pal(kPaletteSize);
  ASSERT_EQ(DFPgetpal(path.c_str(), read_pal.data()), SUCCEED);
  EXPECT_EQ(read_pal, pal1);
  EXPECT_EQ(DFPlastref(), ref1);
}

TEST_F(DfpTest, PutpalOverwrite) {
  const std::string path = GetTempFilePath("dfp_overwrite.hdf");
  absl::Cleanup file_cleanup = [&]() { std::filesystem::remove(path); };

  const auto pal1 = MakeTestPalette(1);
  const auto pal2 = MakeTestPalette(100);

  ASSERT_EQ(DFPputpal(path.c_str(), pal1.data(), 0, "w"), SUCCEED);
  const uint16_t ref1 = DFPlastref();
  EXPECT_GT(ref1, 0);

  // Overwrite the last palette in the same file.
  ASSERT_EQ(DFPputpal(path.c_str(), pal2.data(), 1, "w"), SUCCEED);
  EXPECT_EQ(DFPlastref(), ref1);
  EXPECT_EQ(DFPnpals(path.c_str()), 1);

  DFPrestart();
  std::vector<uint8_t> read_pal(kPaletteSize);
  ASSERT_EQ(DFPgetpal(path.c_str(), read_pal.data()), SUCCEED);
  EXPECT_EQ(read_pal, pal2);
}

TEST_F(DfpTest, AddpalAndSequentialRead) {
  const std::string path = GetTempFilePath("dfp_addpal.hdf");
  absl::Cleanup file_cleanup = [&]() { std::filesystem::remove(path); };

  const auto pal1 = MakeTestPalette(10);
  const auto pal2 = MakeTestPalette(20);

  ASSERT_EQ(DFPputpal(path.c_str(), pal1.data(), 0, "w"), SUCCEED);
  ASSERT_EQ(DFPaddpal(path.c_str(), pal2.data()), SUCCEED);

  EXPECT_EQ(DFPnpals(path.c_str()), 2);

  DFPrestart();
  std::vector<uint8_t> read_pal(kPaletteSize);

  // Read first palette.
  ASSERT_EQ(DFPgetpal(path.c_str(), read_pal.data()), SUCCEED);
  EXPECT_EQ(read_pal, pal1);

  // Read second palette via Readref sequential access.
  ASSERT_EQ(DFPgetpal(path.c_str(), read_pal.data()), SUCCEED);
  EXPECT_EQ(read_pal, pal2);

  // Reading third palette reaches end of palettes and returns FAIL.
  EXPECT_EQ(DFPgetpal(path.c_str(), read_pal.data()), FAIL);
}

TEST_F(DfpTest, ReadrefAndWriteref) {
  const std::string path = GetTempFilePath("dfp_ref_control.hdf");
  absl::Cleanup file_cleanup = [&]() { std::filesystem::remove(path); };

  const auto pal1 = MakeTestPalette(50);
  const auto pal2 = MakeTestPalette(60);

  // Specify ref numbers for writing.
  ASSERT_EQ(DFPwriteref(path.c_str(), 1234), SUCCEED);
  ASSERT_EQ(DFPputpal(path.c_str(), pal1.data(), 0, "w"), SUCCEED);
  EXPECT_EQ(DFPlastref(), 1234);

  ASSERT_EQ(DFPwriteref(path.c_str(), 5678), SUCCEED);
  ASSERT_EQ(DFPaddpal(path.c_str(), pal2.data()), SUCCEED);
  EXPECT_EQ(DFPlastref(), 5678);

  // Read palette with ref 5678 first.
  DFPrestart();
  ASSERT_EQ(DFPreadref(path.c_str(), 5678), SUCCEED);
  std::vector<uint8_t> read_pal(kPaletteSize);
  ASSERT_EQ(DFPgetpal(path.c_str(), read_pal.data()), SUCCEED);
  EXPECT_EQ(read_pal, pal2);

  // Read palette with ref 1234.
  ASSERT_EQ(DFPreadref(path.c_str(), 1234), SUCCEED);
  ASSERT_EQ(DFPgetpal(path.c_str(), read_pal.data()), SUCCEED);
  EXPECT_EQ(read_pal, pal1);

  // Non-existent ref returns FAIL.
  EXPECT_EQ(DFPreadref(path.c_str(), 9999), FAIL);
}

TEST_F(DfpTest, LutOnlyFallback) {
  const std::string path = GetTempFilePath("dfp_lut_only.hdf");
  absl::Cleanup file_cleanup = [&]() { std::filesystem::remove(path); };

  const auto pal_lut = MakeTestPalette(77);

  // Write palette data with DFTAG_LUT only (no DFTAG_IP8).
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  ASSERT_NE(Hputelement(fid, DFTAG_LUT, 300, pal_lut.data(), kPaletteSize),
            FAIL);
  ASSERT_NE(Hclose(fid), FAIL);

  // DFPreadref falls back to DFTAG_LUT.
  ASSERT_EQ(DFPreadref(path.c_str(), 300), SUCCEED);

  // DFPgetpal with Refset falls back to DFTAG_LUT.
  std::vector<uint8_t> read_pal(kPaletteSize);
  ASSERT_EQ(DFPgetpal(path.c_str(), read_pal.data()), SUCCEED);
  EXPECT_EQ(read_pal, pal_lut);

  // Wildcard DFPgetpal also falls back to DFTAG_LUT.
  DFPrestart();
  read_pal.assign(kPaletteSize, 0);
  ASSERT_EQ(DFPgetpal(path.c_str(), read_pal.data()), SUCCEED);
  EXPECT_EQ(read_pal, pal_lut);
}

TEST_F(DfpTest, NpalsEdgeCases) {
  HEclear();
  EXPECT_EQ(DFPnpals("non_existent_file.hdf"), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_BADOPEN);

  const std::string empty_path = GetTempFilePath("dfp_npals_empty.hdf");
  const int32_t empty_fid = Hopen(empty_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(empty_fid, FAIL);
  ASSERT_NE(Hclose(empty_fid), FAIL);
  absl::Cleanup empty_cleanup = [&]() { std::filesystem::remove(empty_path); };
  EXPECT_EQ(DFPnpals(empty_path.c_str()), 0);

  // File with distinct IP8 and LUT palettes.
  const std::string distinct_path = GetTempFilePath("dfp_distinct.hdf");
  absl::Cleanup distinct_cleanup = [&]() {
    std::filesystem::remove(distinct_path);
  };
  const int32_t fid = Hopen(distinct_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  const auto pal = MakeTestPalette(1);
  ASSERT_NE(Hputelement(fid, DFTAG_IP8, 1, pal.data(), kPaletteSize), FAIL);
  ASSERT_NE(Hputelement(fid, DFTAG_LUT, 2, pal.data(), kPaletteSize), FAIL);
  ASSERT_NE(Hclose(fid), FAIL);

  EXPECT_EQ(DFPnpals(distinct_path.c_str()), 2);
}

TEST_F(DfpTest, ReadrefErrors) {
  HEclear();
  EXPECT_EQ(DFPreadref("non_existent_file.hdf", 1), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_BADOPEN);
}

TEST_F(DfpTest, SequentialReadLutOnly) {
  const std::string path = GetTempFilePath("dfp_lut_sequential.hdf");
  absl::Cleanup file_cleanup = [&]() { std::filesystem::remove(path); };

  const auto pal1 = MakeTestPalette(11);
  const auto pal2 = MakeTestPalette(22);

  // Write two LUT elements without IP8.
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  ASSERT_NE(Hputelement(fid, DFTAG_LUT, 10, pal1.data(), kPaletteSize), FAIL);
  ASSERT_NE(Hputelement(fid, DFTAG_LUT, 20, pal2.data(), kPaletteSize), FAIL);
  ASSERT_NE(Hclose(fid), FAIL);

  DFPrestart();
  std::vector<uint8_t> read_pal(kPaletteSize);

  // First read falls back to LUT wildcard.
  ASSERT_EQ(DFPgetpal(path.c_str(), read_pal.data()), SUCCEED);
  EXPECT_EQ(read_pal, pal1);

  // Second read uses Readref and falls back to LUT next read.
  ASSERT_EQ(DFPgetpal(path.c_str(), read_pal.data()), SUCCEED);
  EXPECT_EQ(read_pal, pal2);

  // Third read reaches EOF.
  EXPECT_EQ(DFPgetpal(path.c_str(), read_pal.data()), FAIL);
}

TEST_F(DfpTest, CorruptedPaletteReadError) {
  const std::string path = GetTempFilePath("dfp_corrupted.hdf");
  absl::Cleanup file_cleanup = [&]() { std::filesystem::remove(path); };

  const auto pal = MakeTestPalette(99);
  ASSERT_EQ(DFPputpal(path.c_str(), pal.data(), 0, "w"), SUCCEED);

  // Truncate file so that palette data cannot be read.
  const auto original_size = std::filesystem::file_size(path);
  ASSERT_GT(original_size, kPaletteSize);
  std::filesystem::resize_file(path, original_size - kPaletteSize);

  DFPrestart();
  std::vector<uint8_t> read_pal(kPaletteSize);
  EXPECT_EQ(DFPgetpal(path.c_str(), read_pal.data()), FAIL);
}

TEST_F(DfpTest, DFPIopenReopenFailure) {
  const std::string path = GetTempFilePath("dfp_reopen_deleted.hdf");

  const auto pal = MakeTestPalette(33);
  ASSERT_EQ(DFPputpal(path.c_str(), pal.data(), 0, "w"), SUCCEED);

  // Delete file while Lastfile still holds its name.
  std::filesystem::remove(path);

  // Next call will match Lastfile but fail Hopen in the second branch.
  std::vector<uint8_t> read_pal(kPaletteSize);
  HEclear();
  EXPECT_EQ(DFPgetpal(path.c_str(), read_pal.data()), FAIL);
  EXPECT_EQ(HEvalue(1), DFE_BADOPEN);
}

}  // namespace
}  // namespace hdf4
