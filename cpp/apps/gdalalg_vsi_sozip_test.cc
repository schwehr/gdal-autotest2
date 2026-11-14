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
// Tests for GDALVSISOZIPAlgorithm (apps/gdalalg_vsi_sozip.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vsi_sozip.cpp

#include "third_party/gdal/apps/gdalalg_vsi_sozip.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_vsi_sozip_test";

class GdalalgVsiSozipTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_EQ(0, VSIMkdir(kTestDir, 0755)); }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

void WritePatternFile(const std::string& path, size_t size) {
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<uint8_t>((i * 37 + i / 256) & 0xFF);
  }
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup close_fp = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
  ASSERT_EQ(size, VSIFWriteL(data.data(), 1, size, fp));
}

void ReadFileBytes(const std::string& path, std::vector<uint8_t>& out) {
  VSIStatBufL sbuf;
  ASSERT_EQ(0, VSIStatL(path.c_str(), &sbuf));
  out.resize(static_cast<size_t>(sbuf.st_size));
  VSILFILE* fp = VSIFOpenL(path.c_str(), "rb");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup close_fp = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
  ASSERT_EQ(out.size(), VSIFReadL(out.data(), 1, out.size(), fp));
}

void WriteFileBytes(const std::string& path, const std::vector<uint8_t>& data) {
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup close_fp = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
  ASSERT_EQ(data.size(), VSIFWriteL(data.data(), 1, data.size(), fp));
}

std::string GetActualOutputString(GDALAlgorithm& alg) {
  const auto* arg =
      alg.GetActualAlgorithm().GetArg(GDAL_ARG_NAME_OUTPUT_STRING);
  return arg ? arg->Get<std::string>() : "";
}

TEST_F(GdalalgVsiSozipTest, BasicPropertiesAndDirectRunFails) {
  GDALVSISOZIPAlgorithm alg;
  EXPECT_EQ("sozip", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_EQ("/programs/gdal_vsi_sozip.html", alg.GetHelpURL());

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_FALSE(alg.Run());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("should not be called directly"));
}

TEST_F(GdalalgVsiSozipTest, CreateAppendRecursiveListOptimizeAndValidate) {
  const std::string sub_dir = std::string(kTestDir) + "/input_dir";
  ASSERT_EQ(0, VSIMkdir(sub_dir.c_str(), 0755));
  const std::string big_file = sub_dir + "/big.bin";
  const std::string small_file = sub_dir + "/small.txt";
  ASSERT_NO_FATAL_FAILURE(WritePatternFile(big_file, 2048));
  ASSERT_NO_FATAL_FAILURE(WritePatternFile(small_file, 32));

  const std::string sozip_path = std::string(kTestDir) + "/archive.zip";

  // 1. Create SOZIP with --overwrite, --no-paths, --content-type, and progress.
  {
    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"create", "--overwrite", "--no-paths", "--enable-sozip", "yes",
         "--sozip-chunk-size", "512", "--sozip-min-file-size", "100",
         "--content-type", "application/octet-stream", big_file, sozip_path}));
    int progress_calls = 0;
    auto progress = [](double, const char*, void* data) -> int {
      ++(*static_cast<int*>(data));
      return 1;
    };
    ASSERT_TRUE(alg.Run(progress, &progress_calls));
    EXPECT_GT(progress_calls, 0);
    EXPECT_THAT(GetActualOutputString(alg), HasSubstr("Adding"));
  }

  // 2. Append a non-SOZIP file to the existing zip (no --overwrite).
  {
    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"create", "--no-paths", "--enable-sozip",
                                       "no", small_file, sozip_path}));
    ASSERT_TRUE(alg.Run());
  }

  // 3. List contents and verify both SOZIP and non-SOZIP entries appear.
  {
    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"list", sozip_path}));
    ASSERT_TRUE(alg.Run());
    const std::string list_out = GetActualOutputString(alg);
    EXPECT_THAT(list_out, HasSubstr("big.bin"));
    EXPECT_THAT(list_out, HasSubstr("small.txt"));
    EXPECT_THAT(list_out, HasSubstr("yes ("));
    EXPECT_THAT(list_out, HasSubstr("512"));
  }

  // 4. Validate SOZIP with --verbose and with --stdout.
  {
    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"validate", "-v", sozip_path}));
    ASSERT_TRUE(alg.Run());
    const std::string val_out = GetActualOutputString(alg);
    EXPECT_THAT(val_out, HasSubstr("has a valid SOZip index"));
    EXPECT_THAT(val_out, HasSubstr("contains 1 SOZip-enabled file(s)"));
  }
  {
    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"validate", "--stdout", sozip_path}));
    ASSERT_TRUE(alg.Run());
  }

  // 5. Create regular zip recursively from directory, then validate & optimize.
  const std::string regular_zip = std::string(kTestDir) + "/regular.zip";
  {
    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"create", "-r", "--enable-sozip", "no",
                                       "--stdout", sub_dir, regular_zip}));
    ASSERT_TRUE(alg.Run());
  }
  {
    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"validate", regular_zip}));
    ASSERT_TRUE(alg.Run());
    EXPECT_THAT(GetActualOutputString(alg),
                HasSubstr("does not contain any SOZip-enabled files"));
  }

  const std::string optimized_zip = std::string(kTestDir) + "/optimized.zip";
  {
    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"optimize", "--enable-sozip", "yes", "--sozip-chunk-size", "512",
         "--sozip-min-file-size", "100", regular_zip, optimized_zip}));
    ASSERT_TRUE(alg.Run());
  }
  {
    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"validate", optimized_zip}));
    ASSERT_TRUE(alg.Run());
    // Note: upstream GDALVSISOZIPCreateBaseAlgorithm::RunImpl initializes
    // aosFiles = m_inputFilenames without clearing it for optimize, so
    // regular.zip itself is also added alongside input_dir/big.bin.
    const std::string out = GetActualOutputString(alg);
    EXPECT_THAT(out, HasSubstr("input_dir/big.bin has a valid SOZip index"));
    EXPECT_THAT(out, HasSubstr("contains 2 SOZip-enabled file(s)"));
  }

  // 6. Optimize without --overwrite when destination exists fails.
  {
    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"optimize", regular_zip, optimized_zip}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("already exists"));
  }
}

TEST_F(GdalalgVsiSozipTest, CreateAndInputErrorPaths) {
  const std::string valid_zip = std::string(kTestDir) + "/ok.zip";
  const std::string not_a_zip = std::string(kTestDir) + "/bad.bin";
  ASSERT_NO_FATAL_FAILURE(WritePatternFile(not_a_zip, 32));

  // 1. Output extension must be .zip.
  {
    GDALVSISOZIPAlgorithm alg;
    WithQuietHandler quiet;
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"create", not_a_zip, not_a_zip}));
  }

  // 2. Missing source file without and with progress callback.
  {
    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"create", std::string(kTestDir) + "/missing.bin", valid_zip}));
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.Run());
  }
  {
    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"create", std::string(kTestDir) + "/missing.bin", valid_zip}));
    auto progress = [](double, const char*, void*) -> int { return 1; };
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.Run(progress, nullptr));
  }

  // 3. Directory without -r fails.
  const std::string sub_dir = std::string(kTestDir) + "/dir_no_r";
  ASSERT_EQ(0, VSIMkdir(sub_dir.c_str(), 0755));
  {
    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"create", sub_dir, valid_zip}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("is a directory"));
  }

  // 4. Non-zip inputs to list, validate, and optimize fail.
  for (const char* subcmd : {"list", "validate"}) {
    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({subcmd, not_a_zip}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("not a valid .zip"));
  }
  {
    const std::string opt_out = std::string(kTestDir) + "/opt_out.zip";
    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"optimize", not_a_zip, opt_out}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("not a valid .zip"));
  }

  // 5. Creating output archive in a non-existent POSIX directory fails
  //    CPLCreateZip().
  {
    GDALVSISOZIPAlgorithm alg;
    const std::string fail_zip = (std::filesystem::path(::testing::TempDir()) /
                                  "no_such_dir_sozip" / "out.zip")
                                     .string();
    ASSERT_TRUE(alg.ParseCommandLineArguments({"create", not_a_zip, fail_zip}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }

  // 6. Progress callback returning 0 aborts CPLAddFileInZip().
  {
    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"create", "--overwrite", "--no-paths", not_a_zip, valid_zip}));
    auto cancel_progress = [](double, const char*, void*) -> int { return 0; };
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run(cancel_progress, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Failed adding"));
    CPLErrorReset();
  }
}

TEST_F(GdalalgVsiSozipTest, ValidateCorruptSozipIndexAndSyncMarkers) {
  const std::string data_file = std::string(kTestDir) + "/payload.bin";
  const std::string src_zip = std::string(kTestDir) + "/src_sozip.zip";
  ASSERT_NO_FATAL_FAILURE(WritePatternFile(data_file, 2048));

  {
    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"create", "--overwrite", "--no-paths", "--enable-sozip", "yes",
         "--sozip-chunk-size", "512", "--sozip-min-file-size", "100", data_file,
         src_zip}));
    ASSERT_TRUE(alg.Run());
  }

  const std::string entry_vsi =
      std::string("/vsizip/{") + src_zip + "}/payload.bin";
  const CPLStringList md(VSIGetFileMetadata(entry_vsi.c_str(), "ZIP", nullptr));
  ASSERT_NE(nullptr, md.FetchNameValue("SOZIP_VALID"));
  const uint64_t idx_offset =
      std::strtoull(md.FetchNameValue("SOZIP_START_DATA_OFFSET"), nullptr, 10);
  const uint64_t data_offset =
      std::strtoull(md.FetchNameValue("START_DATA_OFFSET"), nullptr, 10);
  const uint64_t compressed_size =
      std::strtoull(md.FetchNameValue("COMPRESSED_SIZE"), nullptr, 10);

  std::vector<uint8_t> raw;
  ASSERT_NO_FATAL_FAILURE(ReadFileBytes(src_zip, raw));
  ASSERT_GT(raw.size(), idx_offset + 40);

  uint32_t to_skip = 0;
  std::memcpy(&to_skip, &raw[idx_offset + 4], sizeof(to_skip));
  CPL_LSBPTR32(&to_skip);
  const size_t table_pos = static_cast<size_t>(idx_offset + 32 + to_skip);
  ASSERT_GT(raw.size(), table_pos + 16);

  uint64_t offset0 = 0;
  std::memcpy(&offset0, &raw[table_pos], sizeof(offset0));
  CPL_LSBPTR64(&offset0);
  ASSERT_GE(offset0, 9U);

  // 1. Corrupt 9-byte chunk sync marker -> "is not terminated by".
  {
    std::vector<uint8_t> mutated = raw;
    const size_t marker_pos = static_cast<size_t>(data_offset + offset0 - 9);
    ASSERT_LT(marker_pos, mutated.size());
    mutated[marker_pos] ^= 0xFF;
    const std::string bad_zip = std::string(kTestDir) + "/bad_marker.zip";
    ASSERT_NO_FATAL_FAILURE(WriteFileBytes(bad_zip, mutated));

    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"validate", bad_zip}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_NE(CE_None, CPLGetLastErrorType());
    EXPECT_STRNE("", CPLGetLastErrorMsg());
    CPLErrorReset();
  }

  // 2. Patch offset[0] = 2 (< 9) -> "offset[0] (= 2) is invalid".
  {
    std::vector<uint8_t> mutated = raw;
    uint64_t bad_off = 2;
    CPL_LSBPTR64(&bad_off);
    std::memcpy(&mutated[table_pos], &bad_off, sizeof(bad_off));
    const std::string bad_zip = std::string(kTestDir) + "/bad_off0.zip";
    ASSERT_NO_FATAL_FAILURE(WriteFileBytes(bad_zip, mutated));

    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"validate", bad_zip}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_NE(CE_None, CPLGetLastErrorType());
    EXPECT_STRNE("", CPLGetLastErrorMsg());
    CPLErrorReset();
  }

  // 3. Patch offset[1] = offset[0] -> non-monotonic offset error.
  {
    std::vector<uint8_t> mutated = raw;
    uint64_t bad_off1 = offset0;
    CPL_LSBPTR64(&bad_off1);
    std::memcpy(&mutated[table_pos + sizeof(uint64_t)], &bad_off1,
                sizeof(bad_off1));
    const std::string bad_zip = std::string(kTestDir) + "/bad_mono.zip";
    ASSERT_NO_FATAL_FAILURE(WriteFileBytes(bad_zip, mutated));

    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"validate", bad_zip}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_NE(CE_None, CPLGetLastErrorType());
    EXPECT_STRNE("", CPLGetLastErrorMsg());
    CPLErrorReset();
  }

  // 4. Patch offset[0] >= compressed_size -> ">= compressed_size is invalid".
  {
    std::vector<uint8_t> mutated = raw;
    uint64_t bad_oob = compressed_size + 16;
    CPL_LSBPTR64(&bad_oob);
    std::memcpy(&mutated[table_pos], &bad_oob, sizeof(bad_oob));
    const std::string bad_zip = std::string(kTestDir) + "/bad_oob.zip";
    ASSERT_NO_FATAL_FAILURE(WriteFileBytes(bad_zip, mutated));

    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"validate", bad_zip}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_NE(CE_None, CPLGetLastErrorType());
    EXPECT_STRNE("", CPLGetLastErrorMsg());
    CPLErrorReset();
  }

  // 5. Patch offset[0] past EOF -> VSIFReadL() fails reading chunk sync marker.
  {
    std::vector<uint8_t> mutated = raw;
    uint64_t bad_eof = static_cast<uint64_t>(raw.size()) + 1024;
    CPL_LSBPTR64(&bad_eof);
    std::memcpy(&mutated[table_pos], &bad_eof, sizeof(bad_eof));
    const std::string bad_zip = std::string(kTestDir) + "/bad_oob_eof.zip";
    ASSERT_NO_FATAL_FAILURE(WriteFileBytes(bad_zip, mutated));

    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"validate", bad_zip}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_NE(CE_None, CPLGetLastErrorType());
    EXPECT_STRNE("", CPLGetLastErrorMsg());
    CPLErrorReset();
  }

  // 6. Patch nToSkip past EOF -> VSIFReadL() fails reading chunk offset table.
  {
    std::vector<uint8_t> mutated = raw;
    uint32_t bad_skip = 0x10000000U;
    CPL_LSBPTR32(&bad_skip);
    std::memcpy(&mutated[idx_offset + 4], &bad_skip, sizeof(bad_skip));
    const std::string bad_zip = std::string(kTestDir) + "/bad_ntoskip.zip";
    ASSERT_NO_FATAL_FAILURE(WriteFileBytes(bad_zip, mutated));

    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"validate", bad_zip}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_NE(CE_None, CPLGetLastErrorType());
    EXPECT_STRNE("", CPLGetLastErrorMsg());
    CPLErrorReset();
  }

  // 7. Corrupt the final deflate chunk (chunk[3]) -> final VSIFReadL() fails.
  {
    ASSERT_GT(raw.size(), table_pos + 3 * sizeof(uint64_t));
    uint64_t offset2 = 0;
    std::memcpy(&offset2, &raw[table_pos + 2 * sizeof(uint64_t)],
                sizeof(offset2));
    CPL_LSBPTR64(&offset2);
    ASSERT_LT(offset2, compressed_size);
    const size_t corrupt_pos = static_cast<size_t>(
        data_offset + offset2 + (compressed_size - offset2) / 2);
    ASSERT_LT(corrupt_pos + 2, raw.size());

    std::vector<uint8_t> mutated = raw;
    mutated[corrupt_pos] ^= 0xFF;
    mutated[corrupt_pos + 1] ^= 0xFF;
    const std::string bad_zip = std::string(kTestDir) + "/bad_last_chunk.zip";
    ASSERT_NO_FATAL_FAILURE(WriteFileBytes(bad_zip, mutated));

    GDALVSISOZIPAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"validate", bad_zip}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_NE(CE_None, CPLGetLastErrorType());
    EXPECT_STRNE("", CPLGetLastErrorMsg());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
