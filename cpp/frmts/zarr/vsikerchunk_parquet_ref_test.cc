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
// Tests for Kerchunk Parquet references virtual file system
// (vsikerchunk_parquet_ref.cpp).
//
// Caveats:
//   The GDAL Arrow PARQUET driver is not compiled into //third_party/gdal in
//   google3. This test exercises VSIKerchunkParquetRefFileSystem using the
//   ScopedFakeParquetDriver in-memory test double from zarr_test_util.h.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/zarr/vsikerchunk_parquet_ref.cpp

#include <array>
#include <cstdint>
#include <format>  // NOLINT
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/frmts/zarr/zarr_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/zarr/vsikerchunk.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

class VsikerchunkParquetRefTest : public ::testing::Test {
 protected:
  void SetUp() override { VSIInstallKerchunkParquetRefFileSystem(); }
  void TearDown() override { VSIKerchunkParquetRefFileSystemCleanCache(); }
};

TEST_F(VsikerchunkParquetRefTest, SplitFilenameErrors) {
  // Missing '{'
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    VSIStatBufL sStat;
    EXPECT_EQ(-1,
              VSIStatL("/vsikerchunk_parquet_ref/path/without/braces", &sStat));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // Unclosed '{'
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    VSIStatBufL sStat;
    EXPECT_EQ(-1, VSIStatL("/vsikerchunk_parquet_ref/{/unclosed/path", &sStat));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // Open with write mode rejected
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<VSILFILE, decltype(&VSIFCloseL)> fp(
        VSIFOpenL("/vsikerchunk_parquet_ref/{/dummy/root}/file", "wb"),
        VSIFCloseL);
    EXPECT_EQ(nullptr, fp);
  }
}

TEST_F(VsikerchunkParquetRefTest, LoadValidationErrors) {
  ScopedTestDir test_dir("kc_parq_load_err");

  // Helper to test Stat failure on corrupt .zmetadata
  auto CheckLoadFailure = [&](std::string_view name,
                              std::string_view json_content,
                              std::string_view expected_err) {
    const std::string root = test_dir.File(name);
    VSIMkdirRecursive(root.c_str(), 0755);
    ASSERT_TRUE(
        WriteVsiFile(CPLFormFilenameSafe(root.c_str(), ".zmetadata", nullptr),
                     json_content));

    const std::string vsi_path =
        std::format("/vsikerchunk_parquet_ref/{{{}}}/any_key", root);
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    VSIStatBufL sStat;
    EXPECT_EQ(-1, VSIStatL(vsi_path.c_str(), &sStat));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    if (!expected_err.empty()) {
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr(expected_err));
    }
  };

  // Missing .zmetadata
  {
    const std::string empty_root = test_dir.File("empty");
    VSIMkdirRecursive(empty_root.c_str(), 0755);
    const std::string vsi_path =
        std::format("/vsikerchunk_parquet_ref/{{{}}}/key", empty_root);
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    VSIStatBufL sStat;
    EXPECT_EQ(-1, VSIStatL(vsi_path.c_str(), &sStat));
  }

  struct ValidationTestCase {
    std::string_view name;
    std::string_view json;
    std::string_view expected_err;
  };
  static constexpr auto kValidationCases = std::to_array<ValidationTestCase>({
      {.name = "no_rec_size",
       .json = R"json({"metadata": {}})json",
       .expected_err = "key 'record_size' missing or not of type integer"},
      {.name = "str_rec_size",
       .json = R"json({"record_size": "bad", "metadata": {}})json",
       .expected_err = "key 'record_size' missing or not of type integer"},
      {.name = "zero_rec_size",
       .json = R"json({"record_size": 0, "metadata": {}})json",
       .expected_err = "Invalid 'record_size'"},
      {.name = "no_meta",
       .json = R"json({"record_size": 100})json",
       .expected_err = "key 'metadata' missing or not of type dict"},
      {.name = "child_not_obj",
       .json = R"json({
    "record_size": 100,
    "metadata": {"bad_child": "string"}
  })json",
       .expected_err = "invalid value type for key"},
      {.name = "no_shape",
       .json = R"json({
    "record_size": 100,
    "metadata": {"arr/.zarray": {"chunks": [4]}}
  })json",
       .expected_err = "missing 'shape' entry"},
      {.name = "no_chunks",
       .json = R"json({
    "record_size": 100,
    "metadata": {"arr/.zarray": {"shape": [4]}}
  })json",
       .expected_err = "missing 'chunks' entry"},
      {.name = "shape_chunk_mismatch",
       .json = R"json({
    "record_size": 100,
    "metadata": {"arr/.zarray": {"shape": [4], "chunks": [4, 4]}}
  })json",
       .expected_err = "not the same number of values"},
      {.name = "shape_zero",
       .json = R"json({
    "record_size": 100,
    "metadata": {"arr/.zarray": {"shape": [0], "chunks": [4]}}
  })json",
       .expected_err = "shape[0]=0"},
      {.name = "chunk_zero",
       .json = R"json({
    "record_size": 100,
    "metadata": {"arr/.zarray": {"shape": [4], "chunks": [0]}}
  })json",
       .expected_err = "chunks[0]=0"},
  });

  for (const auto& tc : kValidationCases) {
    SCOPED_TRACE(tc.name);
    CheckLoadFailure(tc.name, tc.json, tc.expected_err);
  }

  // .zarray shape > 32 dimensions
  std::string many_dims_json =
      R"json({"record_size": 100, "metadata": {"arr/.zarray": {"shape": [)json";
  std::string many_chunks_str = R"json(, "chunks": [)json";
  for (int i = 0; i < 33; ++i) {
    many_dims_json += (i > 0 ? ",1" : "1");
    many_chunks_str += (i > 0 ? ",1" : "1");
  }
  many_dims_json += "]" + many_chunks_str + "]}}}";
  CheckLoadFailure("too_many_dims", many_dims_json,
                   "'shape' has too many dimensions");
}

TEST_F(VsikerchunkParquetRefTest, FullParquetStoreOperations) {
  ScopedFakeParquetDriver fake_parquet_driver;
  ScopedTestDir test_dir("kc_parq_full");
  const std::string root_dir = test_dir.File("root");
  VSIMkdirRecursive(root_dir.c_str(), 0755);

  // 1. Write .zmetadata (record_size = 2, 2x2 chunk grid for "arr", and 0D for
  // "scalar")
  const std::string zmeta_content = R"json({
    "record_size": 2,
    "metadata": {
      ".zattrs": {"global": 1},
      "sub/.zgroup": {"zarr_format": 2},
      "arr/.zarray": {
        "zarr_format": 2,
        "shape": [4, 4],
        "chunks": [2, 2],
        "dtype": "<i4"
      },
      "scalar/.zarray": {
        "zarr_format": 2,
        "shape": [],
        "chunks": [],
        "dtype": "<i4"
      }
    }
  })json";
  WriteVsiFile(CPLFormFilenameSafe(root_dir.c_str(), ".zmetadata", nullptr),
               zmeta_content);

  // 2. Pre-seed target data file for external references
  const std::string data_file = test_dir.File("data.bin");
  WriteVsiFile(data_file, "ABCDEFGHIJKLMN");

  // 3. Pre-seed fake Parquet files for "arr":
  // Total chunks for 4x4 array with 2x2 blocks = 2x2 = 4 chunks:
  // Chunk linear indices:
  // (0,0) -> 0 * 2 + 0 = 0 -> refs.0.parq, row 0
  // (0,1) -> 0 * 2 + 1 = 1 -> refs.0.parq, row 1
  // (1,0) -> 1 * 2 + 0 = 2 -> refs.1.parq, row 0
  // (1,1) -> 1 * 2 + 1 = 3 -> refs.1.parq, row 1
  const std::vector<std::pair<std::string, OGRFieldType>> fields = {
      {"path", OFTString},
      {"offset", OFTInteger64},
      {"size", OFTInteger64},
      {"raw", OFTBinary},
  };

  const std::string arr_dir =
      CPLFormFilenameSafe(root_dir.c_str(), "arr", nullptr);
  const std::string refs0_path =
      CPLFormFilenameSafe(arr_dir.c_str(), "refs.0.parq", nullptr);
  const std::string refs1_path =
      CPLFormFilenameSafe(arr_dir.c_str(), "refs.1.parq", nullptr);

  // refs.0.parq:
  // Row 0 (chunk 0.0): inline raw binary bytes
  // Row 1 (chunk 0.1): referenced data file with offset=3, size=4 ("DEFG")
  std::vector<GByte> raw0 = {0x01, 0x02, 0x03, 0x04};
  ScopedFakeParquetDriver::CreateParquetStore(
      refs0_path, fields,
      /*str_fields=*/{{}, {{0, data_file}}},
      /*int_fields=*/{{}, {{1, 3}, {2, 4}}},
      /*bin_fields=*/{{{3, raw0}}, {}});

  // refs.1.parq:
  // Row 0 (chunk 1.0): referenced whole file (offset=0, size=0 -> entire
  // data.bin) Row 1 (chunk 1.1): referenced missing target file
  ScopedFakeParquetDriver::CreateParquetStore(
      refs1_path, fields,
      /*str_fields=*/{{{0, data_file}}, {{0, "/vsimem/missing_target.bin"}}},
      /*int_fields=*/{{{1, 0}, {2, 0}}, {{1, 0}, {2, 10}}},
      /*bin_fields=*/{{}, {}});

  // refs for scalar array: chunk 0 -> inline raw bytes
  const std::string scalar_dir =
      CPLFormFilenameSafe(root_dir.c_str(), "scalar", nullptr);
  const std::string scalar_refs0 =
      CPLFormFilenameSafe(scalar_dir.c_str(), "refs.0.parq", nullptr);
  std::vector<GByte> scalar_raw = {0x42, 0x00, 0x00, 0x00};
  ScopedFakeParquetDriver::CreateParquetStore(
      scalar_refs0, fields,
      /*str_fields=*/{{}},
      /*int_fields=*/{{}},
      /*bin_fields=*/{{{3, scalar_raw}}});

  // Base VSI prefix for root
  const std::string base_vsi =
      std::format("/vsikerchunk_parquet_ref/{{{}}}", root_dir);

  // 4. Test Reading Chunk 0.0 (inline raw)
  {
    const std::string chunk00_path = base_vsi + "/arr/0.0";
    VSIStatBufL sStat;
    EXPECT_EQ(0, VSIStatL(chunk00_path.c_str(), &sStat));
    EXPECT_EQ(4, sStat.st_size);
    EXPECT_TRUE(VSI_ISREG(sStat.st_mode));

    std::string content = ReadVsiFile(chunk00_path);
    EXPECT_EQ(4, content.size());
    EXPECT_EQ(0x01, static_cast<uint8_t>(content[0]));
  }

  // 5. Test Reading Chunk 0.1 (offset 3, size 4 from data.bin)
  {
    const std::string chunk01_path = base_vsi + "/arr/0.1";
    VSIStatBufL sStat;
    EXPECT_EQ(0, VSIStatL(chunk01_path.c_str(), &sStat));
    EXPECT_EQ(4, sStat.st_size);

    std::string content = ReadVsiFile(chunk01_path);
    EXPECT_EQ("DEFG", content);
  }

  // 6. Test Reading Chunk 1.0 (whole data_file)
  {
    const std::string chunk10_path = base_vsi + "/arr/1.0";
    VSIStatBufL sStat;
    EXPECT_EQ(0, VSIStatL(chunk10_path.c_str(), &sStat));
    EXPECT_EQ(14, sStat.st_size);

    std::string content = ReadVsiFile(chunk10_path);
    EXPECT_EQ("ABCDEFGHIJKLMN", content);
  }

  // 7. Test Reading Chunk 1.1 (missing target file)
  {
    const std::string chunk11_path = base_vsi + "/arr/1.1";
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<VSILFILE, decltype(&VSIFCloseL)> fp(
        VSIFOpenL(chunk11_path.c_str(), "rb"), VSIFCloseL);
    EXPECT_EQ(nullptr, fp);
  }

  // 8. Test 0D scalar chunk (scalar/0)
  {
    const std::string scalar0_path = base_vsi + "/scalar/0";
    VSIStatBufL sStat;
    EXPECT_EQ(0, VSIStatL(scalar0_path.c_str(), &sStat));
    EXPECT_EQ(4, sStat.st_size);
    std::string content = ReadVsiFile(scalar0_path);
    EXPECT_EQ(4, content.size());
    EXPECT_EQ(0x42, static_cast<uint8_t>(content[0]));
  }

  // 9. Test Invalid Chunk indices
  for (const char* bad_key :
       {"/arr/9.9", "/arr/-1.0", "/arr/0.bad", "/arr/0.0.0"}) {
    const std::string bad_path = base_vsi + bad_key;
    VSIStatBufL sStat;
    EXPECT_EQ(-1, VSIStatL(bad_path.c_str(), &sStat));
  }

  // 10. Test Stat on root directory, virtual directories, and metadata
  {
    VSIStatBufL sStat;
    EXPECT_EQ(0, VSIStatL(base_vsi.c_str(), &sStat));
    EXPECT_TRUE(VSI_ISDIR(sStat.st_mode));

    EXPECT_EQ(0, VSIStatL((base_vsi + "/arr").c_str(), &sStat));
    EXPECT_TRUE(VSI_ISDIR(sStat.st_mode));

    EXPECT_EQ(0, VSIStatL((base_vsi + "/sub").c_str(), &sStat));
    EXPECT_TRUE(VSI_ISDIR(sStat.st_mode));

    EXPECT_EQ(0, VSIStatL((base_vsi + "/sub/.zgroup").c_str(), &sStat));
    EXPECT_TRUE(VSI_ISREG(sStat.st_mode));

    EXPECT_EQ(-1, VSIStatL((base_vsi + "/missing_dir/file").c_str(), &sStat));
  }

  // 11. Test ReadDirEx on root, array, and scalar
  {
    const CPLStringList root_list(VSIReadDirEx(base_vsi.c_str(), 0));
    ASSERT_NE(nullptr, root_list.List());
    EXPECT_GE(root_list.FindString("arr"), 0);
    EXPECT_GE(root_list.FindString("sub"), 0);
    EXPECT_GE(root_list.FindString("scalar"), 0);

    // ReadDirEx on arr: generates synthesized chunks "0.0", "0.1", "1.0", "1.1"
    const CPLStringList arr_list(VSIReadDirEx((base_vsi + "/arr").c_str(), 0));
    ASSERT_NE(nullptr, arr_list.List());
    EXPECT_GE(arr_list.FindString("0.0"), 0);
    EXPECT_GE(arr_list.FindString("0.1"), 0);
    EXPECT_GE(arr_list.FindString("1.0"), 0);
    EXPECT_GE(arr_list.FindString("1.1"), 0);

    // ReadDirEx on arr with nMaxFiles = 2
    const CPLStringList arr_limited(
        VSIReadDirEx((base_vsi + "/arr").c_str(), 2));
    ASSERT_NE(nullptr, arr_limited.List());
    EXPECT_LE(arr_limited.size(), 2);

    // ReadDirEx on scalar: generates synthesized chunk "0"
    const CPLStringList scalar_list(
        VSIReadDirEx((base_vsi + "/scalar").c_str(), 0));
    ASSERT_NE(nullptr, scalar_list.List());
    EXPECT_GE(scalar_list.FindString("0"), 0);
  }
}

// TODO: b/335317901 - Cover GDALIsInGlobalDestructor() and remaining corner
// cases.

}  // namespace
}  // namespace autotest2
