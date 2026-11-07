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
// Tests for Zarr V3 array handling (zarr_v3_array.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/zarr/zarr_v3_array.cpp

#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <format>  // NOLINT
#include <limits>
#include <memory>
#include <span>  // NOLINT
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/frmts/zarr/zarr_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/zarr/zarr.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_multidim.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_compressor.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_json.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

class ZarrV3ArrayTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_Zarr(); }
};

TEST_F(ZarrV3ArrayTest, LoadArrayDtypesAndFillValues) {
  ScopedTestDir test_dir("zarr_v3_dtypes");
  const std::string root_dir = test_dir.File("root");
  VSIMkdirRecursive(root_dir.c_str(), 0755);
  WriteVsiFile(CPLFormFilenameSafe(root_dir.c_str(), "zarr.json", nullptr),
               R"json({
                 "zarr_format": 3,
                 "node_type": "group"
               })json");

  auto shared_res = ZarrSharedResource::Create(root_dir, /*bUpdatable=*/false);
  auto grp = ZarrV3Group::Create(shared_res, "", "/", root_dir);
  ASSERT_NE(nullptr, grp);

  // 1. All ParseDtypeV3 supported data types
  struct DtypeCase {
    std::string_view type_str;
    GDALDataType expected_gdal_type;
  };
  constexpr DtypeCase kDtypes[] = {
      {.type_str = "bool", .expected_gdal_type = GDT_Byte},
      {.type_str = "int8", .expected_gdal_type = GDT_Int8},
      {.type_str = "uint8", .expected_gdal_type = GDT_Byte},
      {.type_str = "int16", .expected_gdal_type = GDT_Int16},
      {.type_str = "uint16", .expected_gdal_type = GDT_UInt16},
      {.type_str = "int32", .expected_gdal_type = GDT_Int32},
      {.type_str = "uint32", .expected_gdal_type = GDT_UInt32},
      {.type_str = "int64", .expected_gdal_type = GDT_Int64},
      {.type_str = "uint64", .expected_gdal_type = GDT_UInt64},
      {.type_str = "float16", .expected_gdal_type = GDT_Float16},
      {.type_str = "float32", .expected_gdal_type = GDT_Float32},
      {.type_str = "float64", .expected_gdal_type = GDT_Float64},
      {.type_str = "complex64", .expected_gdal_type = GDT_CFloat32},
      {.type_str = "complex128", .expected_gdal_type = GDT_CFloat64},
  };

  for (const auto& c : kDtypes) {
    const std::string arr_dir = CPLFormFilenameSafe(
        root_dir.c_str(), std::string(c.type_str).c_str(), nullptr);
    VSIMkdirRecursive(arr_dir.c_str(), 0755);
    const std::string zarr_json = std::format(
        R"json({{
          "zarr_format": 3,
          "node_type": "array",
          "shape": [4],
          "data_type": "{}",
          "chunk_grid": {{
            "name": "regular",
            "configuration": {{"chunk_shape": [4]}}
          }},
          "chunk_key_encoding": {{"name": "default"}},
          "fill_value": null,
          "codecs": [{{"name": "bytes", "configuration": {{"endian": "little"}}}}]
        }})json",
        c.type_str);
    ASSERT_TRUE(WriteVsiFile(
        CPLFormFilenameSafe(arr_dir.c_str(), "zarr.json", nullptr), zarr_json));

    std::shared_ptr<GDALMDArray> arr;
    {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      arr = grp->OpenMDArray(std::string(c.type_str), nullptr);
    }
    ASSERT_NE(nullptr, arr) << "Failed to open dtype: " << c.type_str;
    EXPECT_EQ(c.expected_gdal_type, arr->GetDataType().GetNumericDataType());
  }

  // 2. Data type with fallback object
  {
    const std::string arr_dir =
        CPLFormFilenameSafe(root_dir.c_str(), "fallback_type", nullptr);
    VSIMkdirRecursive(arr_dir.c_str(), 0755);
    ASSERT_TRUE(
        WriteVsiFile(CPLFormFilenameSafe(arr_dir.c_str(), "zarr.json", nullptr),
                     R"json({
                   "zarr_format": 3,
                   "node_type": "array",
                   "shape": [2],
                   "data_type": {"name": "custom_extension", "fallback": "float32"},
                   "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [2]}},
                   "chunk_key_encoding": {"name": "default"},
                   "fill_value": 0,
                   "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
                 })json"));
    auto arr = grp->OpenMDArray("fallback_type", nullptr);
    ASSERT_NE(nullptr, arr);
    EXPECT_EQ(GDT_Float32, arr->GetDataType().GetNumericDataType());
  }

  // 3. Invalid data_type error branches
  {
    auto ExpectBadDtype = [&](std::string_view name, std::string_view dt_val) {
      const std::string arr_dir = CPLFormFilenameSafe(
          root_dir.c_str(), std::string(name).c_str(), nullptr);
      VSIMkdirRecursive(arr_dir.c_str(), 0755);
      ASSERT_TRUE(WriteVsiFile(
          CPLFormFilenameSafe(arr_dir.c_str(), "zarr.json", nullptr),
          std::format(
              R"json({{
                "zarr_format": 3,
                "node_type": "array",
                "shape": [2],
                "data_type": {},
                "chunk_grid": {{"name": "regular", "configuration": {{"chunk_shape": [2]}}}},
                "chunk_key_encoding": {{"name": "default"}},
                "codecs": [{{"name": "bytes", "configuration": {{"endian": "little"}}}}]
              }})json",
              dt_val)));
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, grp->OpenMDArray(std::string(name), nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("Invalid or unsupported format for data_type"));
    };

    ExpectBadDtype("bad_dt_str", "\"unknown_type\"");
    ExpectBadDtype("bad_dt_int", "123");
    ExpectBadDtype("bad_dt_obj_no_fallback", "{\"name\": \"no_fallback\"}");
    ExpectBadDtype("bad_dt_obj_bad_fallback", "{\"fallback\": \"invalid\"}");
  }

  // 4. Fill values: special strings, hex, binary, and complex
  struct FillValueCase {
    std::string_view name;
    std::string_view dtype;
    std::string_view fill_val_json;
    bool expect_success;
    double expected_double;
  };
  constexpr FillValueCase kFillCases[] = {
      {.name = "nan_f32",
       .dtype = "float32",
       .fill_val_json = "\"NaN\"",
       .expect_success = true,
       .expected_double = 0.0},
      {.name = "inf_f64",
       .dtype = "float64",
       .fill_val_json = "\"Infinity\"",
       .expect_success = true,
       .expected_double = std::numeric_limits<double>::infinity()},
      {.name = "plus_inf",
       .dtype = "float64",
       .fill_val_json = "\"+Infinity\"",
       .expect_success = true,
       .expected_double = std::numeric_limits<double>::infinity()},
      {.name = "minus_inf",
       .dtype = "float32",
       .fill_val_json = "\"-Infinity\"",
       .expect_success = true,
       .expected_double = -std::numeric_limits<double>::infinity()},
      {.name = "hex_f32",
       .dtype = "float32",
       .fill_val_json = "\"0x3f800000\"",
       .expect_success = true,
       .expected_double = 1.0},
      {.name = "hex_f64",
       .dtype = "float64",
       .fill_val_json = "\"0x3ff0000000000000\"",
       .expect_success = true,
       .expected_double = 1.0},
      {.name = "bin_f32",
       .dtype = "float32",
       .fill_val_json = "\"0b00111111100000000000000000000000\"",
       .expect_success = true,
       .expected_double = 1.0},
      {.name = "bin_f64",
       .dtype = "float64",
       .fill_val_json = "\"0b00111111111100000000000000000000000000000000000000"
                        "00000000000000\"",
       .expect_success = true,
       .expected_double = 1.0},
      {.name = "cpx64_nums",
       .dtype = "complex64",
       .fill_val_json = "[1.5, -2.5]",
       .expect_success = true,
       .expected_double = 1.5},
      {.name = "cpx128_strings",
       .dtype = "complex128",
       .fill_val_json = "[\"NaN\", \"-Infinity\"]",
       .expect_success = true,
       .expected_double = 0.0},
      {.name = "cpx64_hex",
       .dtype = "complex64",
       .fill_val_json = "[\"0x3f800000\", \"0x40000000\"]",
       .expect_success = true,
       .expected_double = 1.0},
      // Errors
      {.name = "bad_hex_len",
       .dtype = "float32",
       .fill_val_json = "\"0x1234567890ab\"",
       .expect_success = false,
       .expected_double = 0.0},
      {.name = "bad_hex_type",
       .dtype = "uint8",
       .fill_val_json = "\"0x12\"",
       .expect_success = false,
       .expected_double = 0.0},
      {.name = "bad_bin_len",
       .dtype = "float32",
       .fill_val_json = "\"0b111111111111111111111111111111111111\"",
       .expect_success = false,
       .expected_double = 0.0},
      {.name = "bad_bin_type",
       .dtype = "uint8",
       .fill_val_json = "\"0b101\"",
       .expect_success = false,
       .expected_double = 0.0},
      {.name = "bad_str_on_int",
       .dtype = "int32",
       .fill_val_json = "\"not_a_number\"",
       .expect_success = false,
       .expected_double = 0.0},
      {.name = "bad_cpx_len",
       .dtype = "complex64",
       .fill_val_json = "[1.0, 2.0, 3.0]",
       .expect_success = false,
       .expected_double = 0.0},
      {.name = "bad_cpx_elem",
       .dtype = "complex64",
       .fill_val_json = "[true, false]",
       .expect_success = false,
       .expected_double = 0.0},
  };

  for (const auto& tc : kFillCases) {
    const std::string arr_dir = CPLFormFilenameSafe(
        root_dir.c_str(), std::string(tc.name).c_str(), nullptr);
    VSIMkdirRecursive(arr_dir.c_str(), 0755);
    ASSERT_TRUE(
        WriteVsiFile(CPLFormFilenameSafe(arr_dir.c_str(), "zarr.json", nullptr),
                     std::format(
                         R"json({{
              "zarr_format": 3,
              "node_type": "array",
              "shape": [2],
              "data_type": "{}",
              "chunk_grid": {{"name": "regular", "configuration": {{"chunk_shape": [2]}}}},
              "chunk_key_encoding": {{"name": "default"}},
              "fill_value": {},
              "codecs": [{{"name": "bytes", "configuration": {{"endian": "little"}}}}]
            }})json",
                         tc.dtype, tc.fill_val_json)));

    if (tc.expect_success) {
      auto arr = grp->OpenMDArray(std::string(tc.name), nullptr);
      ASSERT_NE(nullptr, arr) << "Failed for fill_value case: " << tc.name;
      bool has_nodata = false;
      const double nd = arr->GetNoDataValueAsDouble(&has_nodata);
      EXPECT_TRUE(has_nodata);
      if (std::isnan(tc.expected_double)) {
        EXPECT_TRUE(std::isnan(nd));
      } else if (std::isinf(tc.expected_double)) {
        EXPECT_TRUE(std::isinf(nd));
      } else if (tc.name == "nan_f32" || tc.name == "cpx128_strings") {
        EXPECT_TRUE(std::isnan(nd));
      } else {
        EXPECT_DOUBLE_EQ(tc.expected_double, nd);
      }
    } else {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, grp->OpenMDArray(std::string(tc.name), nullptr))
          << "Expected failure for case: " << tc.name;
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_STRNE("", CPLGetLastErrorMsg());
    }
  }
}

TEST_F(ZarrV3ArrayTest, LoadArrayValidationAndDimensions) {
  ScopedTestDir test_dir("zarr_v3_validation");
  const std::string root_dir = test_dir.File("root");
  VSIMkdirRecursive(root_dir.c_str(), 0755);
  WriteVsiFile(CPLFormFilenameSafe(root_dir.c_str(), "zarr.json", nullptr),
               R"json({
                 "zarr_format": 3,
                 "node_type": "group"
               })json");

  auto shared_res = ZarrSharedResource::Create(root_dir, /*bUpdatable=*/false);
  auto grp = ZarrV3Group::Create(shared_res, "", "/", root_dir);
  ASSERT_NE(nullptr, grp);

  // Group dimension "x"
  auto dim_x = grp->CreateDimension("x", "HORIZONTAL_X", "EAST", 10, nullptr);
  ASSERT_NE(nullptr, dim_x);

  auto ExpectValidationError = [&](std::string_view name,
                                   std::string_view json_body,
                                   std::string_view expected_err) {
    const std::string arr_dir = CPLFormFilenameSafe(
        root_dir.c_str(), std::string(name).c_str(), nullptr);
    VSIMkdirRecursive(arr_dir.c_str(), 0755);
    ASSERT_TRUE(WriteVsiFile(
        CPLFormFilenameSafe(arr_dir.c_str(), "zarr.json", nullptr), json_body));
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, grp->OpenMDArray(std::string(name), nullptr))
        << "Expected failure for: " << name;
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr(expected_err));
  };

  // 1. Root not object
  ExpectValidationError("root_not_obj", "\"not_an_object\"",
                        "shape missing or not an array");

  // 2. Storage transformers not empty
  ExpectValidationError("storage_trans", R"json({
    "zarr_format": 3,
    "node_type": "array",
    "shape": [2],
    "data_type": "int32",
    "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [2]}},
    "chunk_key_encoding": {"name": "default"},
    "storage_transformers": [{"name": "unknown_transformer"}],
    "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
  })json",
                        "storage_transformers are not supported");

  // 3. Bad shape
  ExpectValidationError("bad_shape", R"json({
    "zarr_format": 3,
    "node_type": "array",
    "shape": "not_an_array",
    "data_type": "int32",
    "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [2]}},
    "chunk_key_encoding": {"name": "default"},
    "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
  })json",
                        "shape missing or not an array");

  // 4. Bad chunk_grid
  ExpectValidationError("bad_chunk_grid_name", R"json({
    "zarr_format": 3,
    "node_type": "array",
    "shape": [4],
    "data_type": "int32",
    "chunk_grid": {"name": "irregular"},
    "chunk_key_encoding": {"name": "default"},
    "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
  })json",
                        "Only chunk_grid.name = regular supported");

  ExpectValidationError("chunk_shape_mismatch", R"json({
    "zarr_format": 3,
    "node_type": "array",
    "shape": [4, 4],
    "data_type": "int32",
    "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [4]}},
    "chunk_key_encoding": {"name": "default"},
    "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
  })json",
                        "shape and chunks arrays are of different size");

  // 5. Bad chunk_key_encoding
  ExpectValidationError("bad_cke_name", R"json({
    "zarr_format": 3,
    "node_type": "array",
    "shape": [4],
    "data_type": "int32",
    "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [4]}},
    "chunk_key_encoding": {"name": "unsupported"},
    "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
  })json",
                        "Unsupported chunk_key_encoding.name");

  ExpectValidationError("bad_cke_sep", R"json({
    "zarr_format": 3,
    "node_type": "array",
    "shape": [4],
    "data_type": "int32",
    "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [4]}},
    "chunk_key_encoding": {"name": "default", "configuration": {"separator": "-"}},
    "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
  })json",
                        "Separator can only be '/' or '.'");

  // 6. Bad codecs
  ExpectValidationError("bad_codecs", R"json({
    "zarr_format": 3,
    "node_type": "array",
    "shape": [4],
    "data_type": "int32",
    "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [4]}},
    "chunk_key_encoding": {"name": "default"},
    "codecs": [{"name": "unsupported_codec"}]
  })json",
                        "Unsupported codec");

  // 7. Bad dimension_names
  ExpectValidationError(
      "dim_names_count", R"json({
    "zarr_format": 3,
    "node_type": "array",
    "shape": [4, 4],
    "data_type": "int32",
    "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [4, 4]}},
    "chunk_key_encoding": {"name": "default"},
    "dimension_names": ["x"],
    "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
  })json",
      "Size of dimension_names[] different from the one of shape");

  // 8. Valid dimension_names matching group dim and 1D indexing var discovery
  const std::string valid_dim_dir =
      CPLFormFilenameSafe(root_dir.c_str(), "valid_dim_arr", nullptr);
  VSIMkdirRecursive(valid_dim_dir.c_str(), 0755);
  WriteVsiFile(CPLFormFilenameSafe(valid_dim_dir.c_str(), "zarr.json", nullptr),
               R"json({
        "zarr_format": 3,
        "node_type": "array",
        "shape": [10],
        "data_type": "int32",
        "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [5]}},
        "chunk_key_encoding": {"name": "v2", "configuration": {"separator": "."}},
        "dimension_names": ["x"],
        "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
      })json");
  auto valid_arr = grp->OpenMDArray("valid_dim_arr", nullptr);
  ASSERT_NE(nullptr, valid_arr);
  EXPECT_EQ(1, valid_arr->GetDimensionCount());
  EXPECT_EQ("x", valid_arr->GetDimensions()[0]->GetName());
  EXPECT_EQ(10, valid_arr->GetDimensions()[0]->GetSize());
}

TEST_F(ZarrV3ArrayTest, ReadWriteCodecsAndAdviseRead) {
  ScopedTestDir test_dir("zarr_v3_io");
  const std::string root_dir = test_dir.File("root");

  auto shared_res = ZarrSharedResource::Create(root_dir, /*bUpdatable=*/true);
  auto grp = ZarrV3Group::CreateOnDisk(shared_res, "", "/", root_dir);
  ASSERT_NE(nullptr, grp);

  auto dim_y = grp->CreateDimension("Y", "HORIZONTAL_Y", "NORTH", 4, nullptr);
  auto dim_x = grp->CreateDimension("X", "HORIZONTAL_X", "EAST", 4, nullptr);
  ASSERT_NE(nullptr, dim_y);
  ASSERT_NE(nullptr, dim_x);
  const std::vector<std::shared_ptr<GDALDimension>> dims = {dim_y, dim_x};

  // 1. Create 2D array with codecs: CHUNK_MEMORY_LAYOUT=F, COMPRESS=GZIP,
  // DIM_SEPARATOR=/
  const char* const opts[] = {
      "CHUNK_MEMORY_LAYOUT=F", "COMPRESS=GZIP", "DIM_SEPARATOR=/",
      "BLOCKSIZE=2,2",         nullptr,
  };
  auto arr = grp->CreateMDArray("arr_f_gzip", dims,
                                GDALExtendedDataType::Create(GDT_Int32), opts);
  ASSERT_NE(nullptr, arr);

  // 2. Write data to tiles (0,0) and (1,1)
  const GUInt64 start0[] = {0, 0};
  const size_t count0[] = {2, 2};
  const GInt64 step[] = {1, 1};
  const GPtrDiff_t stride[] = {2, 1};
  const int write_buf0[4] = {10, 20, 30, 40};
  EXPECT_TRUE(arr->Write(start0, count0, step, stride,
                         GDALExtendedDataType::Create(GDT_Int32), write_buf0));

  const GUInt64 start1[] = {2, 2};
  const int write_buf1[4] = {100, 200, 300, 400};
  EXPECT_TRUE(arr->Write(start1, count0, step, stride,
                         GDALExtendedDataType::Create(GDT_Int32), write_buf1));

  // Flush to disk
  EXPECT_TRUE(std::dynamic_pointer_cast<ZarrArray>(arr)->Flush());

  // Verify tile file exists under c/0/0 and c/1/1
  const std::string chunk0_path = test_dir.File("root/arr_f_gzip/c/0/0");
  VSIStatBufL sStat;
  EXPECT_EQ(0, VSIStatL(chunk0_path.c_str(), &sStat));

  // 3. AdviseRead with NUM_THREADS=2 over full array
  const GUInt64 full_start[] = {0, 0};
  const size_t full_count[] = {4, 4};
  const char* const advise_opts[] = {"NUM_THREADS=2", "CACHE_SIZE=1048576",
                                     nullptr};
  EXPECT_TRUE(arr->AdviseRead(full_start, full_count, advise_opts));

  // 4. Read back tile (0,0)
  int read_buf0[4] = {0};
  EXPECT_TRUE(arr->Read(start0, count0, step, stride,
                        GDALExtendedDataType::Create(GDT_Int32), read_buf0));
  EXPECT_EQ(10, read_buf0[0]);
  EXPECT_EQ(20, read_buf0[1]);
  EXPECT_EQ(30, read_buf0[2]);
  EXPECT_EQ(40, read_buf0[3]);

  // Read empty tile (0,1) -> fill_value = 0
  int read_empty[4] = {-1, -1, -1, -1};
  const GUInt64 start_empty[] = {0, 2};
  EXPECT_TRUE(arr->Read(start_empty, count0, step, stride,
                        GDALExtendedDataType::Create(GDT_Int32), read_empty));
  EXPECT_EQ(0, read_empty[0]);
  EXPECT_EQ(0, read_empty[1]);

  // 5. Overwrite tile (0,0) with zeros (all fill_value) and Flush() -> unlinks
  // chunk file
  const int zero_buf[4] = {0, 0, 0, 0};
  EXPECT_TRUE(arr->Write(start0, count0, step, stride,
                         GDALExtendedDataType::Create(GDT_Int32), zero_buf));
  EXPECT_TRUE(std::dynamic_pointer_cast<ZarrArray>(arr)->Flush());
  EXPECT_EQ(-1, VSIStatL(chunk0_path.c_str(), &sStat))
      << "Chunk file should have been unlinked upon overwrite with fill_value";

  // 6. Corrupt compressed tile error path in LoadTileData
  const std::string chunk1_path = test_dir.File("root/arr_f_gzip/c/1/1");
  WriteVsiFile(chunk1_path, "corrupt_gzip_bytes");
  int read_corrupt[4] = {0};
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(arr->Read(start1, count0, step, stride,
                           GDALExtendedDataType::Create(GDT_Int32),
                           read_corrupt));
  }

  // 7. ZARR_ALLOW_BIG_TILE_SIZE=NO rejection in AllocateWorkingBuffers
  {
    CPLConfigOptionSetter opt_setter("ZARR_ALLOW_BIG_TILE_SIZE", "NO", false);
    auto big_dim = grp->CreateDimension("big_dim", "", "", 2048, nullptr);
    ASSERT_NE(nullptr, big_dim);
    const char* const big_opts[] = {"BLOCKSIZE=2048,2048,100", nullptr};
    const std::vector<std::shared_ptr<GDALDimension>> big_dims = {
        big_dim, big_dim, grp->CreateDimension("z", "", "", 100, nullptr)};
    auto big_arr =
        grp->CreateMDArray("big_arr", big_dims,
                           GDALExtendedDataType::Create(GDT_UInt32), big_opts);
    ASSERT_NE(nullptr, big_arr);
    const GUInt64 b_start[] = {0, 0, 0};
    const size_t b_count[] = {1, 1, 1};
    uint32_t val = 1;
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(big_arr->Write(b_start, b_count, nullptr, nullptr,
                                GDALExtendedDataType::Create(GDT_UInt32),
                                &val));
  }

  // 8. 0D scalar array creation, write, flush, read
  {
    auto scalar_arr = grp->CreateMDArray(
        "scalar_v3", {}, GDALExtendedDataType::Create(GDT_Float64), nullptr);
    ASSERT_NE(nullptr, scalar_arr);
    double in_val = 3.14159;
    EXPECT_TRUE(scalar_arr->Write(nullptr, nullptr, nullptr, nullptr,
                                  GDALExtendedDataType::Create(GDT_Float64),
                                  &in_val));
    EXPECT_TRUE(std::dynamic_pointer_cast<ZarrArray>(scalar_arr)->Flush());
    double out_val = 0.0;
    EXPECT_TRUE(scalar_arr->Read(nullptr, nullptr, nullptr, nullptr,
                                 GDALExtendedDataType::Create(GDT_Float64),
                                 &out_val));
    EXPECT_DOUBLE_EQ(3.14159, out_val);
  }
}

TEST_F(ZarrV3ArrayTest, AdditionalCoverageAndEdgeCases) {
  ScopedTestDir test_dir("zarr_v3_more");
  const std::string root_dir = test_dir.File("root");

  // 1. Group with dimension and indexing variables on disk
  auto shared_res = ZarrSharedResource::Create(root_dir, /*bUpdatable=*/true);
  auto grp = ZarrV3Group::CreateOnDisk(shared_res, "", "/", root_dir);
  ASSERT_NE(nullptr, grp);

  // 2. Int64 and UInt64 fill_value numeric parsing
  {
    const std::string zarr_i64 = R"json({
      "zarr_format": 3,
      "node_type": "array",
      "shape": [2],
      "data_type": "int64",
      "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [2]}},
      "chunk_key_encoding": {"name": "default"},
      "fill_value": 123456789012345,
      "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
    })json";
    const std::string i64_dir = test_dir.File("root/arr_i64");
    VSIMkdirRecursive(i64_dir.c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_i64/zarr.json"), zarr_i64);
    auto arr_i64 = grp->OpenMDArray("arr_i64", nullptr);
    ASSERT_NE(nullptr, arr_i64);
    bool has_nd = false;
    const double nd_val = arr_i64->GetNoDataValueAsDouble(&has_nd);
    EXPECT_TRUE(has_nd);
    EXPECT_DOUBLE_EQ(123456789012345.0, nd_val);

    const std::string zarr_u64 = R"json({
      "zarr_format": 3,
      "node_type": "array",
      "shape": [2],
      "data_type": "uint64",
      "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [2]}},
      "chunk_key_encoding": {"name": "default"},
      "fill_value": 987654321012345,
      "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
    })json";
    const std::string u64_dir = test_dir.File("root/arr_u64");
    VSIMkdirRecursive(u64_dir.c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_u64/zarr.json"), zarr_u64);
    auto arr_u64 = grp->OpenMDArray("arr_u64", nullptr);
    ASSERT_NE(nullptr, arr_u64);
    has_nd = false;
    EXPECT_DOUBLE_EQ(987654321012345.0,
                     arr_u64->GetNoDataValueAsDouble(&has_nd));
  }

  // 3. Float16 with NaN string fill value and invalid fill value cases
  {
    const std::string zarr_f16 = R"json({
      "zarr_format": 3,
      "node_type": "array",
      "shape": [2],
      "data_type": "float16",
      "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [2]}},
      "chunk_key_encoding": {"name": "default"},
      "fill_value": "NaN",
      "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
    })json";
    const std::string f16_dir = test_dir.File("root/arr_f16");
    VSIMkdirRecursive(f16_dir.c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_f16/zarr.json"), zarr_f16);
    auto arr_f16 = grp->OpenMDArray("arr_f16", nullptr);
    ASSERT_NE(nullptr, arr_f16);

    const std::string zarr_bad_i32 = R"json({
      "zarr_format": 3,
      "node_type": "array",
      "shape": [2],
      "data_type": "int32",
      "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [2]}},
      "chunk_key_encoding": {"name": "default"},
      "fill_value": "NaN",
      "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
    })json";
    const std::string bad_i32_dir = test_dir.File("root/bad_i32");
    VSIMkdirRecursive(bad_i32_dir.c_str(), 0755);
    WriteVsiFile(test_dir.File("root/bad_i32/zarr.json"), zarr_bad_i32);
    {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, grp->OpenMDArray("bad_i32", nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid fill_value"));
    }

    const std::string zarr_bad_cpx = R"json({
      "zarr_format": 3,
      "node_type": "array",
      "shape": [2],
      "data_type": "complex64",
      "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [2]}},
      "chunk_key_encoding": {"name": "default"},
      "fill_value": ["invalid_real", "2.0"],
      "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
    })json";
    const std::string bad_cpx_dir = test_dir.File("root/bad_cpx");
    VSIMkdirRecursive(bad_cpx_dir.c_str(), 0755);
    WriteVsiFile(test_dir.File("root/bad_cpx/zarr.json"), zarr_bad_cpx);
    {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, grp->OpenMDArray("bad_cpx", nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid fill_value"));
    }
  }

  // 4. White-box test of ZarrV3Array::Create with NeedDecodedBuffer() == true
  // (exercises AllocateWorkingBuffers, EncodeElt on flush, DecodeSourceElt on
  // read, and AdviseRead with NeedDecodedBuffer)
  {
    auto shared =
        ZarrSharedResource::Create(test_dir.File("wb_v3"), /*bUpdatable=*/true);
    VSIMkdirRecursive(test_dir.File("wb_v3/arr_wb").c_str(), 0755);
    auto dim0 = std::make_shared<ZarrDimension>(
        shared, std::weak_ptr<ZarrGroupBase>(), "/", "y", "", "", 4);
    auto dim1 = std::make_shared<ZarrDimension>(
        shared, std::weak_ptr<ZarrGroupBase>(), "/", "x", "", "", 4);
    std::vector<std::shared_ptr<GDALDimension>> dims = {dim0, dim1};
    DtypeElt elt;
    elt.nativeType = DtypeElt::NativeType::SIGNED_INT;
    elt.nativeOffset = 0;
    elt.nativeSize = 4;
    elt.needByteSwapping = true;
    elt.gdalType = GDALExtendedDataType::Create(GDT_Int32);
    elt.gdalOffset = 0;
    elt.gdalSize = 4;
    std::vector<DtypeElt> elts = {elt};
    std::vector<GUInt64> block_size = {2, 2};
    auto wb_arr = ZarrV3Array::Create(shared, "/", "arr_wb", dims, elt.gdalType,
                                      elts, block_size);
    ASSERT_NE(nullptr, wb_arr);
    wb_arr->SetUpdatable(true);
    wb_arr->SetFilename(test_dir.File("wb_v3/arr_wb/zarr.json"));
    wb_arr->SetDimSeparator("/");
    CPLJSONDocument dtype_doc;
    dtype_doc.LoadMemory(R"("int32")");
    wb_arr->SetDtype(dtype_doc.GetRoot());

    const GUInt64 start[] = {0, 0};
    const size_t count[] = {2, 2};
    const GInt64 step[] = {1, 1};
    const GPtrDiff_t stride[] = {2, 1};
    const int wdata[4] = {0x11223344, 0x05060708, 0x10203040, 0x7f000001};
    EXPECT_TRUE(wb_arr->Write(start, count, step, stride,
                              GDALExtendedDataType::Create(GDT_Int32), wdata));
    EXPECT_TRUE(wb_arr->Flush());

    // AdviseRead on multi-tile array (exercises NeedDecodedBuffer() inside
    // IAdviseRead)
    const GUInt64 full_start[] = {0, 0};
    const size_t full_count[] = {4, 4};
    const char* const adv_opts[] = {"NUM_THREADS=2", "CACHE_SIZE=1048576",
                                    nullptr};
    EXPECT_TRUE(wb_arr->AdviseRead(full_start, full_count, adv_opts));

    int rdata[4] = {0};
    EXPECT_TRUE(wb_arr->Read(start, count, step, stride,
                             GDALExtendedDataType::Create(GDT_Int32), rdata));
    EXPECT_EQ(0x11223344, rdata[0]);
    EXPECT_EQ(0x05060708, rdata[1]);
    EXPECT_EQ(0x10203040, rdata[2]);
    EXPECT_EQ(0x7f000001, rdata[3]);

    // Exercise Serialize / Flush with modified unit, offset, scale, nodata, and
    // renamed dim
    wb_arr->SetUnit("meters");
    wb_arr->SetOffset(1.5, GDT_Float64);
    wb_arr->SetScale(2.0, GDT_Float64);
    const int32_t nd = -999;
    EXPECT_TRUE(wb_arr->SetRawNoDataValue(&nd));
    dim0->SetXArrayDimension();
    dim1->SetXArrayDimension();
    EXPECT_TRUE(dim0->Rename("y_renamed"));
    EXPECT_TRUE(wb_arr->Flush());
    EXPECT_THAT(ReadVsiFile(test_dir.File("wb_v3/arr_wb/zarr.json")),
                HasSubstr("y_renamed"));

    // Exercise 0-dimensional array tile naming (both default "c" and v2 "0")
    std::vector<std::shared_ptr<GDALDimension>> empty_dims;
    std::vector<GUInt64> empty_block;
    auto wb_0d = ZarrV3Array::Create(shared, "/", "arr_0d", empty_dims,
                                     elt.gdalType, elts, empty_block);
    ASSERT_NE(nullptr, wb_0d);
    wb_0d->SetUpdatable(true);
    wb_0d->SetFilename(test_dir.File("wb_v3/arr_0d/zarr.json"));
    wb_0d->SetIsV2ChunkKeyEncoding(false);
    const int val = 123;
    EXPECT_TRUE(wb_0d->Write(nullptr, nullptr, nullptr, nullptr,
                             GDALExtendedDataType::Create(GDT_Int32), &val));
    EXPECT_TRUE(wb_0d->Flush());

    wb_0d->SetIsV2ChunkKeyEncoding(true);
    EXPECT_TRUE(wb_0d->Write(nullptr, nullptr, nullptr, nullptr,
                             GDALExtendedDataType::Create(GDT_Int32), &val));
    EXPECT_TRUE(wb_0d->Flush());

    // Trigger tile creation failure (cannot open tile when directory exists at
    // tile file path)
    VSIMkdirRecursive(test_dir.File("wb_v3/arr_0d_err/0").c_str(), 0755);
    wb_0d->SetFilename(test_dir.File("wb_v3/arr_0d_err/zarr.json"));
    EXPECT_TRUE(wb_0d->Write(nullptr, nullptr, nullptr, nullptr,
                             GDALExtendedDataType::Create(GDT_Int32), &val));
    {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_FALSE(wb_0d->Flush());
    }
  }

  // 5. Uncompressed array (poCodecs == nullptr path in LoadTileData)
  {
    const std::string zarr_raw = R"json({
      "zarr_format": 3,
      "node_type": "array",
      "shape": [2],
      "data_type": "int32",
      "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [2]}},
      "chunk_key_encoding": {"name": "default"},
      "fill_value": 0,
      "codecs": []
    })json";
    const std::string raw_dir = test_dir.File("root/arr_raw");
    VSIMkdirRecursive(raw_dir.c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_raw/zarr.json"), zarr_raw);
    auto arr_raw = grp->OpenMDArray("arr_raw", nullptr);
    ASSERT_NE(nullptr, arr_raw);
    const GUInt64 start[] = {0};
    const size_t count[] = {2};
    const GInt64 step[] = {1};
    const GPtrDiff_t stride[] = {1};
    const int wdata[2] = {42, 84};
    EXPECT_TRUE(arr_raw->Write(start, count, step, stride,
                               GDALExtendedDataType::Create(GDT_Int32), wdata));
    EXPECT_TRUE(std::dynamic_pointer_cast<ZarrArray>(arr_raw)->Flush());
    EXPECT_EQ(8U, ReadVsiFile(test_dir.File("root/arr_raw/c/0")).size());

    // Reopen store to exercise poCodecs == nullptr path in LoadTileData from
    // disk
    auto shared_raw =
        ZarrSharedResource::Create(root_dir, /*bUpdatable=*/false);
    auto grp_raw = ZarrV3Group::Create(shared_raw, "", "/", root_dir);
    ASSERT_NE(nullptr, grp_raw);
    auto reopened_raw = grp_raw->OpenMDArray("arr_raw", nullptr);
    ASSERT_NE(nullptr, reopened_raw);
    int rdata[2] = {0};
    EXPECT_TRUE(reopened_raw->Read(start, count, step, stride,
                                   GDALExtendedDataType::Create(GDT_Int32),
                                   rdata));
    EXPECT_EQ(42, rdata[0]);
    EXPECT_EQ(84, rdata[1]);
  }

  // 6. Complex NoData serialization with NaN and Infinity in Serialize()
  {
    auto cpx_dim = grp->CreateDimension("cpx_dim", "", "", 2, nullptr);
    ASSERT_NE(nullptr, cpx_dim);
    auto cpx_arr =
        grp->CreateMDArray("arr_cpx_ser", {cpx_dim},
                           GDALExtendedDataType::Create(GDT_CFloat64), nullptr);
    ASSERT_NE(nullptr, cpx_arr);
    const double nd_cpx[2] = {std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::infinity()};
    EXPECT_TRUE(cpx_arr->SetRawNoDataValue(nd_cpx));
    EXPECT_TRUE(std::dynamic_pointer_cast<ZarrArray>(cpx_arr)->Flush());

    // Also test negative infinity
    const double nd_cpx_neg[2] = {-std::numeric_limits<double>::infinity(),
                                  123.456};
    EXPECT_TRUE(cpx_arr->SetRawNoDataValue(nd_cpx_neg));
    EXPECT_TRUE(std::dynamic_pointer_cast<ZarrArray>(cpx_arr)->Flush());
    EXPECT_THAT(ReadVsiFile(test_dir.File("root/arr_cpx_ser/zarr.json")),
                HasSubstr("-Infinity"));
  }

  // 7. FindDimension with size mismatch warning and indexing variable discovery
  // on disk
  {
    // Pre-create 1D indexing array "x_idx" on disk
    const std::string x_idx_dir = test_dir.File("root/x_idx");
    VSIMkdirRecursive(x_idx_dir.c_str(), 0755);
    WriteVsiFile(test_dir.File("root/x_idx/zarr.json"), R"json({
      "zarr_format": 3,
      "node_type": "array",
      "shape": [4],
      "data_type": "int32",
      "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [4]}},
      "chunk_key_encoding": {"name": "default"},
      "dimension_names": ["x_idx"],
      "fill_value": 0,
      "attributes": {"_ARRAY_DIMENSIONS": ["x_idx"], "axis": "X", "standard_name": "projection_x_coordinate"},
      "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
    })json");

    // Pre-create mismatching dimension "d_mismatch" of size 2
    auto d_mismatch = grp->CreateDimension("d_mismatch", "", "", 2, nullptr);
    ASSERT_NE(nullptr, d_mismatch);

    // Array referencing d_mismatch with shape 4 (size mismatch warning)
    const std::string zarr_mismatch = R"json({
      "zarr_format": 3,
      "node_type": "array",
      "shape": [4],
      "data_type": "int32",
      "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [4]}},
      "chunk_key_encoding": {"name": "default"},
      "dimension_names": ["d_mismatch"],
      "fill_value": 0,
      "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
    })json";
    const std::string mm_dir = test_dir.File("root/arr_mismatch");
    VSIMkdirRecursive(mm_dir.c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_mismatch/zarr.json"), zarr_mismatch);
    {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      auto mm_arr = grp->OpenMDArray("arr_mismatch", nullptr);
      ASSERT_NE(nullptr, mm_arr);
    }

    // Array referencing x_idx (finds indexing variable on disk and attaches
    // dimension)
    const std::string zarr_find_idx = R"json({
      "zarr_format": 3,
      "node_type": "array",
      "shape": [4],
      "data_type": "int32",
      "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [4]}},
      "chunk_key_encoding": {"name": "default"},
      "dimension_names": ["x_idx"],
      "fill_value": 0,
      "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
    })json";
    const std::string fidx_dir = test_dir.File("root/arr_fidx");
    VSIMkdirRecursive(fidx_dir.c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_fidx/zarr.json"), zarr_find_idx);
    auto fidx_arr = grp->OpenMDArray("arr_fidx", nullptr);
    ASSERT_NE(nullptr, fidx_arr);
    EXPECT_EQ("x_idx", fidx_arr->GetDimensions()[0]->GetName());
    EXPECT_NE(nullptr, fidx_arr->GetDimensions()[0]->GetIndexingVariable());
  }

  // 8. CacheTilePresence, GetDataDirectory, and GetTileIndicesFromFilename
  {
    const std::string zarr_cache = R"json({
      "zarr_format": 3,
      "node_type": "array",
      "shape": [4, 4],
      "data_type": "int32",
      "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [2, 2]}},
      "chunk_key_encoding": {"name": "default"},
      "fill_value": 0,
      "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
    })json";
    const std::string cache_dir = test_dir.File("root/arr_cache");
    VSIMkdirRecursive(cache_dir.c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_cache/zarr.json"), zarr_cache);

    auto zarr_arr = std::dynamic_pointer_cast<ZarrArray>(
        grp->OpenMDArray("arr_cache", nullptr));
    ASSERT_NE(nullptr, zarr_arr);

    // Pre-create Zarr multidim dataset for .gmac tile presence cache
    const std::string gmac_path =
        test_dir.File("root/arr_cache/zarr.json.gmac");
    auto drv = GetGDALDriverManager()->GetDriverByName("Zarr");
    ASSERT_NE(nullptr, drv);
    GDALDatasetUniquePtr ds_gmac(
        drv->CreateMultiDimensional(gmac_path.c_str(), nullptr, nullptr));
    // Stray files to test GetTileIndicesFromFilename branches:
    // non-c prefix, invalid separator, etc.
    WriteVsiFile(test_dir.File("root/arr_cache/cx"), "stray");
    WriteVsiFile(test_dir.File("root/arr_cache/c_invalid"), "stray");

    EXPECT_TRUE(zarr_arr->CacheTilePresence());

    // Also test v2 chunk key encoding with "." and "/" separators
    const std::string zarr_v2_key = R"json({
      "zarr_format": 3,
      "node_type": "array",
      "shape": [4, 4],
      "data_type": "int32",
      "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [2, 2]}},
      "chunk_key_encoding": {"name": "v2", "configuration": {"separator": "."}},
      "fill_value": 0,
      "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
    })json";
    const std::string v2_key_dir = test_dir.File("root/arr_v2_key");
    VSIMkdirRecursive(v2_key_dir.c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_v2_key/zarr.json"), zarr_v2_key);
    WriteVsiFile(test_dir.File("root/arr_v2_key/0.0"), "data");
    WriteVsiFile(test_dir.File("root/arr_v2_key/invalid_file"), "data");
    const std::string gmac_v2_key =
        test_dir.File("root/arr_v2_key/zarr.json.gmac");
    GDALDatasetUniquePtr ds_gmac_v2(
        drv->CreateMultiDimensional(gmac_v2_key.c_str(), nullptr, nullptr));
    auto arr_v2_key = std::dynamic_pointer_cast<ZarrArray>(
        grp->OpenMDArray("arr_v2_key", nullptr));
    ASSERT_NE(nullptr, arr_v2_key);
    EXPECT_TRUE(arr_v2_key->CacheTilePresence());

    // default chunk key encoding with "." separator and stray files
    const std::string zarr_dot_key = R"json({
      "zarr_format": 3,
      "node_type": "array",
      "shape": [4, 4],
      "data_type": "int32",
      "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [2, 2]}},
      "chunk_key_encoding": {"name": "default", "configuration": {"separator": "."}},
      "fill_value": 0,
      "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
    })json";
    const std::string dot_key_dir = test_dir.File("root/arr_dot_key");
    VSIMkdirRecursive(dot_key_dir.c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_dot_key/zarr.json"), zarr_dot_key);
    WriteVsiFile(test_dir.File("root/arr_dot_key/c.0.0"), "data");
    WriteVsiFile(test_dir.File("root/arr_dot_key/c_bad_sep"), "data");
    const std::string gmac_dot_key =
        test_dir.File("root/arr_dot_key/zarr.json.gmac");
    GDALDatasetUniquePtr ds_gmac_dot(
        drv->CreateMultiDimensional(gmac_dot_key.c_str(), nullptr, nullptr));
    auto arr_dot_key = std::dynamic_pointer_cast<ZarrArray>(
        grp->OpenMDArray("arr_dot_key", nullptr));
    ASSERT_NE(nullptr, arr_dot_key);
    EXPECT_TRUE(arr_dot_key->CacheTilePresence());

    // Indexing variable array "x" with dimension_names: ["x"] and
    // _CoordinateAxisType attribute
    const std::string zarr_idx_var = R"json({
      "zarr_format": 3,
      "node_type": "array",
      "shape": [4],
      "data_type": "int32",
      "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [4]}},
      "chunk_key_encoding": {"name": "default"},
      "dimension_names": ["x"],
      "fill_value": 0,
      "attributes": {"_CoordinateAxisType": "GeoX"},
      "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
    })json";
    const std::string idx_dir = test_dir.File("root/x");
    VSIMkdirRecursive(idx_dir.c_str(), 0755);
    WriteVsiFile(test_dir.File("root/x/zarr.json"), zarr_idx_var);
    auto arr_x = grp->OpenMDArray("x", nullptr);
    ASSERT_NE(nullptr, arr_x);

    // Second array sharing dimension "x"
    const std::string zarr_reuse_x = R"json({
      "zarr_format": 3,
      "node_type": "array",
      "shape": [4],
      "data_type": "int32",
      "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [4]}},
      "chunk_key_encoding": {"name": "default"},
      "dimension_names": ["x"],
      "fill_value": 0,
      "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
    })json";
    const std::string reuse_dir = test_dir.File("root/arr_reuse_x");
    VSIMkdirRecursive(reuse_dir.c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_reuse_x/zarr.json"), zarr_reuse_x);
    auto arr_reuse = grp->OpenMDArray("arr_reuse_x", nullptr);
    ASSERT_NE(nullptr, arr_reuse);

    // Error cases in LoadArray:
    // 1) dimension_names not an array
    const std::string zarr_bad_dimnames = R"json({
      "zarr_format": 3,
      "node_type": "array",
      "shape": [2],
      "data_type": "int32",
      "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [2]}},
      "chunk_key_encoding": {"name": "default"},
      "dimension_names": 123,
      "fill_value": 0,
      "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
    })json";
    const std::string bdn_dir = test_dir.File("root/bad_dimnames");
    VSIMkdirRecursive(bdn_dir.c_str(), 0755);
    WriteVsiFile(test_dir.File("root/bad_dimnames/zarr.json"),
                 zarr_bad_dimnames);
    {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, grp->OpenMDArray("bad_dimnames", nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("dimension_names should be an array"));
    }

    // 2) missing data_type
    const std::string zarr_no_dt = R"json({
      "zarr_format": 3,
      "node_type": "array",
      "shape": [2],
      "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [2]}},
      "chunk_key_encoding": {"name": "default"},
      "fill_value": 0,
      "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
    })json";
    const std::string nodt_dir = test_dir.File("root/no_dt");
    VSIMkdirRecursive(nodt_dir.c_str(), 0755);
    WriteVsiFile(test_dir.File("root/no_dt/zarr.json"), zarr_no_dt);
    {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, grp->OpenMDArray("no_dt", nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("data_type missing"));
    }

    // 3) complex data type with scalar fill_value
    const std::string zarr_cpx_scalar_fv = R"json({
      "zarr_format": 3,
      "node_type": "array",
      "shape": [2],
      "data_type": "complex64",
      "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [2]}},
      "chunk_key_encoding": {"name": "default"},
      "fill_value": 0,
      "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
    })json";
    const std::string cpx_sfv_dir = test_dir.File("root/cpx_scalar_fv");
    VSIMkdirRecursive(cpx_sfv_dir.c_str(), 0755);
    WriteVsiFile(test_dir.File("root/cpx_scalar_fv/zarr.json"),
                 zarr_cpx_scalar_fv);
    {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, grp->OpenMDArray("cpx_scalar_fv", nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid fill_value"));
    }

    // Open store with CACHE_TILE_PRESENCE=YES
    const char* const open_opts[] = {"CACHE_TILE_PRESENCE=YES", nullptr};
    GDALDatasetUniquePtr ds_cached(
        GDALDataset::Open(root_dir.c_str(), GDAL_OF_MULTIDIM_RASTER, nullptr,
                          open_opts, nullptr));
    ASSERT_NE(nullptr, ds_cached);
    auto grp_cached = ds_cached->GetRootGroup();
    ASSERT_NE(nullptr, grp_cached);
    auto arr_cached = grp_cached->OpenMDArray("arr_cache", nullptr);
    ASSERT_NE(nullptr, arr_cached);
    // Read an empty tile {2, 2} which is fast-skipped via tile presence cache
    const GUInt64 empty_start[] = {2, 2};
    const size_t empty_count[] = {2, 2};
    const GInt64 step[] = {1, 1};
    const GPtrDiff_t stride[] = {2, 1};
    int empty_rdata[4] = {-1, -1, -1, -1};
    EXPECT_TRUE(arr_cached->Read(empty_start, empty_count, step, stride,
                                 GDALExtendedDataType::Create(GDT_Int32),
                                 empty_rdata));
    EXPECT_EQ(0, empty_rdata[0]);
  }

  // 9. Flush error path: directory creation error when dim separator is "/"
  {
    const std::string vsi_root = test_dir.File("zarr_v3_mkdir_err");
    auto vsi_shared_res =
        ZarrSharedResource::Create(vsi_root, /*bUpdatable=*/true);
    auto vsi_grp = ZarrV3Group::CreateOnDisk(vsi_shared_res, "", "/", vsi_root);
    ASSERT_THAT(vsi_grp, NotNull());

    auto dim_y = vsi_grp->CreateDimension("dim_y", "", "", 2, nullptr);
    auto dim_x = vsi_grp->CreateDimension("dim_x", "", "", 2, nullptr);
    const char* const err_opts[] = {"DIM_SEPARATOR=/", "BLOCKSIZE=1,1",
                                    nullptr};
    auto err_arr = vsi_grp->CreateMDArray(
        "arr_mkdir_err", {dim_y, dim_x},
        GDALExtendedDataType::Create(GDT_Int32), err_opts);
    auto zarr_err = std::dynamic_pointer_cast<ZarrV3Array>(err_arr);
    ASSERT_THAT(zarr_err, NotNull());

    const GUInt64 start[] = {0, 0};
    const size_t count[] = {1, 1};
    int val = 99;
    EXPECT_TRUE(zarr_err->Write(start, count, nullptr, nullptr,
                                GDALExtendedDataType::Create(GDT_Int32), &val));

    // Point filename at a read-only /vsigzip/ path so VSIMkdirRecursive fails
    zarr_err->SetFilename(
        std::format("/vsigzip/{}/ro.gz/arr/zarr.json", vsi_root));
    {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_FALSE(zarr_err->Flush());
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot create directory"));
    }
    zarr_err->SetFilename(vsi_root + "/arr_mkdir_err/zarr.json");
  }

  // 10. Wrong decompressed tile size error path in LoadTileData
  {
    const std::string zarr_bad_decomp = R"json({
      "zarr_format": 3,
      "node_type": "array",
      "shape": [4],
      "data_type": "int32",
      "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [4]}},
      "chunk_key_encoding": {"name": "default"},
      "fill_value": 0,
      "codecs": [
        {"name": "bytes", "configuration": {"endian": "little"}},
        {"name": "gzip", "configuration": {"level": 1}}
      ]
    })json";
    const std::string bd_dir = test_dir.File("root/arr_bad_decomp");
    VSIMkdirRecursive(bd_dir.c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_bad_decomp/zarr.json"),
                 zarr_bad_decomp);

    // Compress 4 bytes instead of the expected 16 bytes (4 x sizeof(int32))
    const auto* comp = CPLGetCompressor("gzip");
    ASSERT_NE(nullptr, comp);
    void* out_buf = nullptr;
    size_t out_size = 0;
    const char in_bytes[4] = {1, 2, 3, 4};
    const bool ok = comp->pfnFunc(in_bytes, sizeof(in_bytes), &out_buf,
                                  &out_size, nullptr, comp->user_data);
    std::unique_ptr<void, decltype(&VSIFree)> out_guard(out_buf, VSIFree);
    ASSERT_TRUE(ok);
    ASSERT_NE(nullptr, out_buf);
    WriteVsiBytes(
        test_dir.File("root/arr_bad_decomp/c/0"),
        std::span<const GByte>(static_cast<const GByte*>(out_buf), out_size));

    auto arr_bd = grp->OpenMDArray("arr_bad_decomp", nullptr);
    ASSERT_NE(nullptr, arr_bd);
    const GUInt64 start[] = {0};
    const size_t count[] = {4};
    int rdata[4] = {0};
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(arr_bd->Read(start, count, nullptr, nullptr,
                              GDALExtendedDataType::Create(GDT_Int32), rdata));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 11. Dimension search recursing up directory hierarchy (lines 1292-1299)
  {
    auto sub_grp = grp->CreateGroup("sub_grp", nullptr);
    ASSERT_NE(nullptr, sub_grp);
    const std::string zarr_recurse = R"json({
      "zarr_format": 3,
      "node_type": "array",
      "shape": [2],
      "data_type": "int32",
      "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [2]}},
      "chunk_key_encoding": {"name": "default"},
      "dimension_names": ["nonexistent_dim"],
      "fill_value": 0,
      "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
    })json";
    const std::string recurse_dir = test_dir.File("root/sub_grp/arr_recurse");
    VSIMkdirRecursive(recurse_dir.c_str(), 0755);
    WriteVsiFile(test_dir.File("root/sub_grp/arr_recurse/zarr.json"),
                 zarr_recurse);
    auto arr_recurse = sub_grp->OpenMDArray("arr_recurse", nullptr);
    EXPECT_NE(nullptr, arr_recurse);
  }
}

// TODO: b/335317901 - Test additional V3 custom codecs and sharding storage
// transformers.

}  // namespace
}  // namespace autotest2
