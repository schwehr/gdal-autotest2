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
// Tests for Zarr V2 array handling (zarr_v2_array.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/zarr/zarr_v2_array.cpp

#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <format>  // NOLINT
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/frmts/zarr/zarr_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
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
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

class ZarrV2ArrayTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_Zarr(); }
};

struct DtypeTestCase {
  std::string_view dtype_str;
  GDALDataType expected_gdt;
};

// 1. Table-driven parsing of all primitive V2 data types
TEST_F(ZarrV2ArrayTest, PrimitiveDtypeParsing) {
  const ScopedTestDir test_dir("zarr_v2_dtype");
  const std::string root_dir = test_dir.File("root");
  VSIMkdirRecursive(root_dir.c_str(), 0755);
  WriteVsiFile(test_dir.File("root/.zgroup"), R"json({"zarr_format": 2})json");

  GDALDatasetUniquePtr ds(GDALDataset::Open(
      root_dir.c_str(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_UPDATE));
  ASSERT_NE(nullptr, ds);
  auto grp = ds->GetRootGroup();
  ASSERT_NE(nullptr, grp);

  constexpr DtypeTestCase kCases[] = {
      {.dtype_str = "|b1", .expected_gdt = GDT_Byte},
      {.dtype_str = "|u1", .expected_gdt = GDT_Byte},
      {.dtype_str = "|i1", .expected_gdt = GDT_Int8},
      {.dtype_str = "<i2", .expected_gdt = GDT_Int16},
      {.dtype_str = ">i2", .expected_gdt = GDT_Int16},
      {.dtype_str = "<i4", .expected_gdt = GDT_Int32},
      {.dtype_str = ">i4", .expected_gdt = GDT_Int32},
      {.dtype_str = "<i8", .expected_gdt = GDT_Int64},
      {.dtype_str = ">i8", .expected_gdt = GDT_Int64},
      {.dtype_str = "<u2", .expected_gdt = GDT_UInt16},
      {.dtype_str = ">u2", .expected_gdt = GDT_UInt16},
      {.dtype_str = "<u4", .expected_gdt = GDT_UInt32},
      {.dtype_str = ">u4", .expected_gdt = GDT_UInt32},
      {.dtype_str = "<u8", .expected_gdt = GDT_UInt64},
      {.dtype_str = ">u8", .expected_gdt = GDT_UInt64},
      {.dtype_str = "<f2", .expected_gdt = GDT_Float16},
      {.dtype_str = ">f2", .expected_gdt = GDT_Float16},
      {.dtype_str = "<f4", .expected_gdt = GDT_Float32},
      {.dtype_str = ">f4", .expected_gdt = GDT_Float32},
      {.dtype_str = "<f8", .expected_gdt = GDT_Float64},
      {.dtype_str = ">f8", .expected_gdt = GDT_Float64},
      {.dtype_str = "<c8", .expected_gdt = GDT_CFloat32},
      {.dtype_str = ">c8", .expected_gdt = GDT_CFloat32},
      {.dtype_str = "<c16", .expected_gdt = GDT_CFloat64},
      {.dtype_str = ">c16", .expected_gdt = GDT_CFloat64},
  };

  int idx = 0;
  for (const auto& tc : kCases) {
    const std::string name = std::format("arr_{}", idx++);
    const std::string zarray = std::format(
        R"json({{
          "zarr_format": 2,
          "shape": [4],
          "chunks": [2],
          "dtype": "{}",
          "order": "C",
          "fill_value": 0,
          "compressor": null,
          "filters": null
        }})json",
        tc.dtype_str);
    const std::string arr_dir = test_dir.File(std::format("root/{}", name));
    VSIMkdirRecursive(arr_dir.c_str(), 0755);
    WriteVsiFile(test_dir.File(std::format("root/{}/.zarray", name)), zarray);

    auto arr = grp->OpenMDArray(name, nullptr);
    ASSERT_NE(nullptr, arr) << "Failed on dtype " << tc.dtype_str;
    EXPECT_EQ(tc.expected_gdt, arr->GetDataType().GetNumericDataType());
  }

  // String types (|S5, <U3, >U3)
  {
    const std::string zarr_s = R"json({
      "zarr_format": 2,
      "shape": [2],
      "chunks": [2],
      "dtype": "|S5",
      "order": "C",
      "fill_value": null,
      "compressor": null,
      "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/arr_s5").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_s5/.zarray"), zarr_s);
    auto arr_s = grp->OpenMDArray("arr_s5", nullptr);
    ASSERT_NE(nullptr, arr_s);
    EXPECT_EQ(GEDTC_STRING, arr_s->GetDataType().GetClass());

    const std::string zarr_u = R"json({
      "zarr_format": 2,
      "shape": [2],
      "chunks": [2],
      "dtype": "<U3",
      "order": "C",
      "fill_value": null,
      "compressor": null,
      "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/arr_u3").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_u3/.zarray"), zarr_u);
    auto arr_u = grp->OpenMDArray("arr_u3", nullptr);
    ASSERT_NE(nullptr, arr_u);
    EXPECT_EQ(GEDTC_STRING, arr_u->GetDataType().GetClass());
  }

  // Compound dtype with alignment and nested types
  {
    const std::string zarr_comp = R"json({
      "zarr_format": 2,
      "shape": [2],
      "chunks": [2],
      "dtype": [["a", "<i4"], ["b", "<f8"], ["c", "|S4"]],
      "order": "C",
      "fill_value": null,
      "compressor": null,
      "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/arr_comp").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_comp/.zarray"), zarr_comp);
    auto arr_comp = grp->OpenMDArray("arr_comp", nullptr);
    ASSERT_NE(nullptr, arr_comp);
    EXPECT_EQ(GEDTC_COMPOUND, arr_comp->GetDataType().GetClass());
    EXPECT_EQ(3, arr_comp->GetDataType().GetComponents().size());
  }

  // Invalid dtypes
  {
    const char* const kBadDtypes[] = {
        "\"x\"",
        "\"|x1\"",
        "\"<i0\"",
        "\"<i9999\"",
        "[[\"a\"]]",
        "[[123, \"<i4\"]]",
        "[[\"a\", \"invalid_sub_dtype\"]]",
    };
    int bad_idx = 0;
    for (const char* bad_dt : kBadDtypes) {
      const std::string bad_name = std::format("bad_dt_{}", bad_idx++);
      const std::string zarray = std::format(
          R"json({{"zarr_format": 2, "shape": [2], "chunks": [2], "dtype": {}, "order": "C", "fill_value": null, "compressor": null, "filters": null}})json",
          bad_dt);
      const std::string bad_dir =
          test_dir.File(std::format("root/{}", bad_name));
      VSIMkdirRecursive(bad_dir.c_str(), 0755);
      ASSERT_TRUE(WriteVsiFile(
          test_dir.File(std::format("root/{}/.zarray", bad_name)), zarray));
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, grp->OpenMDArray(bad_name, nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("Invalid or unsupported format for dtype"));
    }
  }
}

// 2. Fill value parsing across all varieties
TEST_F(ZarrV2ArrayTest, FillValueVarieties) {
  const ScopedTestDir test_dir("zarr_v2_fill_value");
  const std::string root_dir = test_dir.File("root");
  VSIMkdirRecursive(root_dir.c_str(), 0755);
  WriteVsiFile(test_dir.File("root/.zgroup"), R"json({"zarr_format": 2})json");

  GDALDatasetUniquePtr ds(GDALDataset::Open(
      root_dir.c_str(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_UPDATE));
  ASSERT_NE(nullptr, ds);
  auto grp = ds->GetRootGroup();
  ASSERT_NE(nullptr, grp);

  // 1) null, missing, single-element array
  {
    const std::string zarr_null = R"json({
      "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "C",
      "fill_value": null, "compressor": null, "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/fv_null").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/fv_null/.zarray"), zarr_null);
    auto arr = grp->OpenMDArray("fv_null", nullptr);
    ASSERT_NE(nullptr, arr);
    EXPECT_EQ(nullptr, arr->GetRawNoDataValue());

    // Missing fill_value (warns but opens)
    const std::string zarr_missing = R"json({
      "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "C",
      "compressor": null, "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/fv_missing").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/fv_missing/.zarray"), zarr_missing);
    {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      auto arr_miss = grp->OpenMDArray("fv_missing", nullptr);
      EXPECT_NE(nullptr, arr_miss);
    }

    // NCZarr single-element array fill_value: [42]
    const std::string zarr_ncz = R"json({
      "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "C",
      "fill_value": [42], "compressor": null, "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/fv_ncz").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/fv_ncz/.zarray"), zarr_ncz);
    auto arr_ncz = grp->OpenMDArray("fv_ncz", nullptr);
    ASSERT_NE(nullptr, arr_ncz);
    bool has_nd = false;
    EXPECT_DOUBLE_EQ(42.0, arr_ncz->GetNoDataValueAsDouble(&has_nd));
    EXPECT_TRUE(has_nd);
  }

  // 2) Numeric strings: int64, uint64, float64, and specials "NaN", "Infinity",
  // "-Infinity"
  {
    const std::string zarr_str_num = R"json({
      "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i8", "order": "C",
      "fill_value": "-9876543210", "compressor": null, "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/fv_i64_str").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/fv_i64_str/.zarray"), zarr_str_num);
    auto arr_i64 = grp->OpenMDArray("fv_i64_str", nullptr);
    ASSERT_NE(nullptr, arr_i64);
    const auto* raw_nd =
        static_cast<const int64_t*>(arr_i64->GetRawNoDataValue());
    ASSERT_NE(nullptr, raw_nd);
    EXPECT_EQ(-9876543210LL, *raw_nd);

    const std::string zarr_u64_str = R"json({
      "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<u8", "order": "C",
      "fill_value": "9876543210", "compressor": null, "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/fv_u64_str").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/fv_u64_str/.zarray"), zarr_u64_str);
    auto arr_u64 = grp->OpenMDArray("fv_u64_str", nullptr);
    ASSERT_NE(nullptr, arr_u64);
    const auto* raw_u64 =
        static_cast<const uint64_t*>(arr_u64->GetRawNoDataValue());
    ASSERT_NE(nullptr, raw_u64);
    EXPECT_EQ(9876543210ULL, *raw_u64);

    const std::string zarr_nan = R"json({
      "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<f4", "order": "C",
      "fill_value": "NaN", "compressor": null, "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/fv_nan").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/fv_nan/.zarray"), zarr_nan);
    auto arr_nan = grp->OpenMDArray("fv_nan", nullptr);
    ASSERT_NE(nullptr, arr_nan);
    bool has_nd = false;
    EXPECT_TRUE(std::isnan(arr_nan->GetNoDataValueAsDouble(&has_nd)));

    const std::string zarr_inf = R"json({
      "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<f8", "order": "C",
      "fill_value": "Infinity", "compressor": null, "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/fv_inf").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/fv_inf/.zarray"), zarr_inf);
    auto arr_inf = grp->OpenMDArray("fv_inf", nullptr);
    ASSERT_NE(nullptr, arr_inf);
    EXPECT_TRUE(std::isinf(arr_inf->GetNoDataValueAsDouble(&has_nd)));

    const std::string zarr_ninf = R"json({
      "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<f8", "order": "C",
      "fill_value": "-Infinity", "compressor": null, "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/fv_ninf").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/fv_ninf/.zarray"), zarr_ninf);
    auto arr_ninf = grp->OpenMDArray("fv_ninf", nullptr);
    ASSERT_NE(nullptr, arr_ninf);
    EXPECT_TRUE(std::isinf(arr_ninf->GetNoDataValueAsDouble(&has_nd)));

    // TODO: b/335317901 - Upstream bug in zarr_v2_array.cpp:1777 ('if' instead
    // of 'else if' rejects "-Infinity" for Float16).
    const std::string zarr_f16_inf = R"json({
      "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<f2", "order": "C",
      "fill_value": "-Infinity", "compressor": null, "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/fv_f16_inf").c_str(), 0755);
    ASSERT_TRUE(
        WriteVsiFile(test_dir.File("root/fv_f16_inf/.zarray"), zarr_f16_inf));
    {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, grp->OpenMDArray("fv_f16_inf", nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid fill_value"));
    }
  }

  // 3) Base64 encoded fill_value on string and compound types, plus "0" on
  // string
  {
    // String with "0" fill_value (pinned upstream quirk)
    const std::string zarr_s0 = R"json({
      "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "|S4", "order": "C",
      "fill_value": "0", "compressor": null, "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/fv_s0").c_str(), 0755);
    ASSERT_TRUE(WriteVsiFile(test_dir.File("root/fv_s0/.zarray"), zarr_s0));
    auto arr_s0 = grp->OpenMDArray("fv_s0", nullptr);
    ASSERT_NE(nullptr, arr_s0);

    // String with base64 encoded fill_value
    const std::unique_ptr<char, CplFreeDeleter> b64_str(
        CPLBase64Encode(4, reinterpret_cast<const GByte*>("ABCD")));
    const std::string zarr_sb64 = std::format(
        R"json({{"zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "|S4", "order": "C", "fill_value": "{}", "compressor": null, "filters": null}})json",
        b64_str.get());
    VSIMkdirRecursive(test_dir.File("root/fv_sb64").c_str(), 0755);
    ASSERT_TRUE(WriteVsiFile(test_dir.File("root/fv_sb64/.zarray"), zarr_sb64));
    auto arr_sb64 = grp->OpenMDArray("fv_sb64", nullptr);
    ASSERT_NE(nullptr, arr_sb64);

    // Compound with valid base64 encoded fill_value (int32 + float32 = 8 bytes)
    const int32_t val_i = 100;
    const float val_f = 2.5f;
    GByte comp_buf[8];
    memcpy(comp_buf, &val_i, 4);
    memcpy(comp_buf + 4, &val_f, 4);
    const std::unique_ptr<char, CplFreeDeleter> b64_comp(
        CPLBase64Encode(8, comp_buf));
    const std::string zarr_cb64 = std::format(
        R"json({{"zarr_format": 2, "shape": [2], "chunks": [2], "dtype": [["x", "<i4"], ["y", "<f4"]], "order": "C", "fill_value": "{}", "compressor": null, "filters": null}})json",
        b64_comp.get());
    VSIMkdirRecursive(test_dir.File("root/fv_cb64").c_str(), 0755);
    ASSERT_TRUE(WriteVsiFile(test_dir.File("root/fv_cb64/.zarray"), zarr_cb64));
    auto arr_cb64 = grp->OpenMDArray("fv_cb64", nullptr);
    ASSERT_NE(nullptr, arr_cb64);

    // Invalid base64 length on compound
    const std::unique_ptr<char, CplFreeDeleter> b64_short(
        CPLBase64Encode(4, comp_buf));
    const std::string zarr_cbad = std::format(
        R"json({{"zarr_format": 2, "shape": [2], "chunks": [2], "dtype": [["x", "<i4"], ["y", "<f4"]], "order": "C", "fill_value": "{}", "compressor": null, "filters": null}})json",
        b64_short.get());
    VSIMkdirRecursive(test_dir.File("root/fv_cbad").c_str(), 0755);
    ASSERT_TRUE(WriteVsiFile(test_dir.File("root/fv_cbad/.zarray"), zarr_cbad));
    {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, grp->OpenMDArray("fv_cbad", nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid fill_value"));
    }
  }

  // 4) Invalid fill_value types
  {
    constexpr auto kBadFillValues = std::to_array<std::string_view>({
        "\"invalid_string\"",
        "[1, 2, 3]",
        "{\"key\": 1}",
    });
    int bad_fv_idx = 0;
    for (std::string_view bad_fv : kBadFillValues) {
      const std::string bad_name = std::format("bad_fv_{}", bad_fv_idx++);
      const std::string zarray = std::format(
          R"json({{"zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "C", "fill_value": {}, "compressor": null, "filters": null}})json",
          bad_fv);
      const std::string bad_dir =
          test_dir.File(std::format("root/{}", bad_name));
      VSIMkdirRecursive(bad_dir.c_str(), 0755);
      ASSERT_TRUE(WriteVsiFile(
          test_dir.File(std::format("root/{}/.zarray", bad_name)), zarray));
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, grp->OpenMDArray(bad_name, nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid fill_value"));
    }
  }
}

// 3. Order, dimension separators, compressors, and filters
TEST_F(ZarrV2ArrayTest, OrderSeparatorsCompressorsFilters) {
  const ScopedTestDir test_dir("zarr_v2_opts");
  const std::string root_dir = test_dir.File("root");
  VSIMkdirRecursive(root_dir.c_str(), 0755);
  WriteVsiFile(test_dir.File("root/.zgroup"), R"json({"zarr_format": 2})json");

  GDALDatasetUniquePtr ds(GDALDataset::Open(
      root_dir.c_str(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_UPDATE));
  ASSERT_NE(nullptr, ds);
  auto grp = ds->GetRootGroup();
  ASSERT_NE(nullptr, grp);

  // Fortran order array
  {
    const std::string zarr_f = R"json({
      "zarr_format": 2, "shape": [3, 2], "chunks": [3, 2], "dtype": "<i4", "order": "F",
      "fill_value": 0, "compressor": null, "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/arr_f").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_f/.zarray"), zarr_f);
    auto arr_f = grp->OpenMDArray("arr_f", nullptr);
    ASSERT_NE(nullptr, arr_f);
  }

  // Dimension separators: "/" and default "."
  {
    const std::string zarr_slash = R"json({
      "zarr_format": 2, "shape": [4, 4], "chunks": [2, 2], "dtype": "<i4", "order": "C",
      "dimension_separator": "/", "fill_value": 0, "compressor": null, "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/arr_slash").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_slash/.zarray"), zarr_slash);
    auto arr_slash = grp->OpenMDArray("arr_slash", nullptr);
    ASSERT_NE(nullptr, arr_slash);
  }

  // Compressors: null, zlib, blosc, imagecodecs_tiff, invalid
  {
    const std::string zarr_zlib = R"json({
      "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "C",
      "fill_value": 0, "compressor": {"id": "zlib", "level": 1}, "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/arr_zlib").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_zlib/.zarray"), zarr_zlib);
    auto arr_zlib = grp->OpenMDArray("arr_zlib", nullptr);
    ASSERT_NE(nullptr, arr_zlib);

    const std::string zarr_blosc = R"json({
      "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "C",
      "fill_value": 0,
      "compressor": {"id": "blosc", "cname": "zstd", "clevel": 1, "shuffle": 1, "num_threads": 4, "typesize": 4},
      "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/arr_blosc").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_blosc/.zarray"), zarr_blosc);
    auto arr_blosc = grp->OpenMDArray("arr_blosc", nullptr);
    ASSERT_NE(nullptr, arr_blosc);

    // imagecodecs_tiff
    const std::string zarr_tiff = R"json({
      "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "C",
      "fill_value": 0, "compressor": {"id": "imagecodecs_tiff"}, "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/arr_tiff").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_tiff/.zarray"), zarr_tiff);
    auto arr_tiff = grp->OpenMDArray("arr_tiff", nullptr);
    ASSERT_NE(nullptr, arr_tiff);

    // Invalid compressor
    const std::string zarr_bad_comp = R"json({
      "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "C",
      "fill_value": 0, "compressor": {"id": "nonexistent_comp"}, "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/arr_bad_comp").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_bad_comp/.zarray"), zarr_bad_comp);
    {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, grp->OpenMDArray("arr_bad_comp", nullptr));
    }
  }

  // Filters: delta, shuffle, quantize, fixedscaleoffset
  {
    const std::string zarr_filt = R"json({
      "zarr_format": 2, "shape": [4], "chunks": [4], "dtype": "<i4", "order": "C",
      "fill_value": 0, "compressor": null,
      "filters": [
        {"id": "delta", "dtype": "<i4"},
        {"id": "shuffle", "elementsize": 4}
      ]
    })json";
    VSIMkdirRecursive(test_dir.File("root/arr_filt").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_filt/.zarray"), zarr_filt);
    auto arr_filt = grp->OpenMDArray("arr_filt", nullptr);
    ASSERT_NE(nullptr, arr_filt);

    const std::string zarr_quant = R"json({
      "zarr_format": 2, "shape": [4], "chunks": [4], "dtype": "<f4", "order": "C",
      "fill_value": 0, "compressor": null,
      "filters": [{"id": "quantize", "digits": 2, "dtype": "<f4"}]
    })json";
    VSIMkdirRecursive(test_dir.File("root/arr_quant").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_quant/.zarray"), zarr_quant);
    auto arr_quant = grp->OpenMDArray("arr_quant", nullptr);
    ASSERT_NE(nullptr, arr_quant);

    const std::string zarr_fso = R"json({
      "zarr_format": 2, "shape": [4], "chunks": [4], "dtype": "<f4", "order": "C",
      "fill_value": 0, "compressor": null,
      "filters": [{"id": "fixedscaleoffset", "scale": 10.0, "offset": 0.0, "dtype": "<f4"}]
    })json";
    VSIMkdirRecursive(test_dir.File("root/arr_fso").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_fso/.zarray"), zarr_fso);
    auto arr_fso = grp->OpenMDArray("arr_fso", nullptr);
    ASSERT_NE(nullptr, arr_fso);

    // Invalid filter id
    const std::string zarr_bad_filt = R"json({
      "zarr_format": 2, "shape": [4], "chunks": [4], "dtype": "<i4", "order": "C",
      "fill_value": 0, "compressor": null,
      "filters": [{"id": "nonexistent_filter"}]
    })json";
    VSIMkdirRecursive(test_dir.File("root/arr_bad_filt").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_bad_filt/.zarray"), zarr_bad_filt);
    {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, grp->OpenMDArray("arr_bad_filt", nullptr));
    }
  }

  // Attributes: _ARRAY_DIMENSIONS and _NCZARR_ARRAY
  {
    const std::string zarr_xarray = R"json({
      "zarr_format": 2, "shape": [3, 2], "chunks": [3, 2], "dtype": "<i4", "order": "C",
      "fill_value": 0, "compressor": null, "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/arr_xarray").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_xarray/.zarray"), zarr_xarray);
    WriteVsiFile(test_dir.File("root/arr_xarray/.zattrs"),
                 R"json({"_ARRAY_DIMENSIONS": ["lat", "lon"]})json");
    auto arr_xarray = grp->OpenMDArray("arr_xarray", nullptr);
    ASSERT_NE(nullptr, arr_xarray);
    const auto dims = arr_xarray->GetDimensions();
    ASSERT_EQ(2, dims.size());
    EXPECT_EQ("lat", dims[0]->GetName());
    EXPECT_EQ("lon", dims[1]->GetName());

    // _NCZARR_ARRAY dimrefs matching array name (indexing variable)
    const std::string zarr_ncz_idx = R"json({
      "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<f8", "order": "C",
      "fill_value": null, "compressor": null, "filters": null,
      "_NCZARR_ARRAY": {"dimrefs": ["/ncz_idx"]}
    })json";
    const std::string zattrs_ncz_idx = R"json({
      "standard_name": "longitude", "units": "degrees_east"
    })json";
    VSIMkdirRecursive(test_dir.File("root/ncz_idx").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/ncz_idx/.zarray"), zarr_ncz_idx);
    WriteVsiFile(test_dir.File("root/ncz_idx/.zattrs"), zattrs_ncz_idx);
    {
      auto arr_ncz_idx = grp->OpenMDArray("ncz_idx", nullptr);
      ASSERT_NE(nullptr, arr_ncz_idx);
      auto idx_dims = arr_ncz_idx->GetDimensions();
      ASSERT_EQ(1, idx_dims.size());
      EXPECT_EQ("ncz_idx", idx_dims[0]->GetName());
      EXPECT_EQ(GDAL_DIM_TYPE_HORIZONTAL_X, idx_dims[0]->GetType());
      EXPECT_EQ("EAST", idx_dims[0]->GetDirection());
    }

    // Auto-load external dimension array without .zmetadata
    const std::string zarr_dim_auto = R"json({
      "zarr_format": 2, "shape": [4], "chunks": [4], "dtype": "<f8", "order": "C",
      "fill_value": null, "compressor": null, "filters": null
    })json";
    const std::string zattrs_dim_auto = R"json({
      "_ARRAY_DIMENSIONS": ["dim_auto"], "standard_name": "projection_x_coordinate"
    })json";
    VSIMkdirRecursive(test_dir.File("root/dim_auto").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/dim_auto/.zarray"), zarr_dim_auto);
    WriteVsiFile(test_dir.File("root/dim_auto/.zattrs"), zattrs_dim_auto);

    const std::string zarr_arr_dim_auto = R"json({
      "zarr_format": 2, "shape": [4], "chunks": [4], "dtype": "<i4", "order": "C",
      "fill_value": 0, "compressor": null, "filters": null
    })json";
    const std::string zattrs_arr_dim_auto = R"json({
      "_ARRAY_DIMENSIONS": ["dim_auto"]
    })json";
    VSIMkdirRecursive(test_dir.File("root/arr_dim_auto").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_dim_auto/.zarray"), zarr_arr_dim_auto);
    WriteVsiFile(test_dir.File("root/arr_dim_auto/.zattrs"),
                 zattrs_arr_dim_auto);

    {
      auto arr_auto = grp->OpenMDArray("arr_dim_auto", nullptr);
      ASSERT_NE(nullptr, arr_auto);
      auto auto_dims = arr_auto->GetDimensions();
      ASSERT_EQ(1, auto_dims.size());
      EXPECT_EQ("dim_auto", auto_dims[0]->GetName());
      EXPECT_NE(nullptr, auto_dims[0]->GetIndexingVariable());
    }

    // Numeric string fill_value on numeric array
    const std::string zarr_fv_numstr = R"json({
      "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<f8", "order": "C",
      "fill_value": "123.5", "compressor": null, "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/fv_numstr").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/fv_numstr/.zarray"), zarr_fv_numstr);
    {
      auto arr_fv_str = grp->OpenMDArray("fv_numstr", nullptr);
      ASSERT_NE(nullptr, arr_fv_str);
      EXPECT_DOUBLE_EQ(123.5, arr_fv_str->GetNoDataValueAsDouble());
    }

    // Boolean fill_value on non-numeric array (invalid)
    const std::string zarr_fv_bool_bad = R"json({
      "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "|S4", "order": "C",
      "fill_value": true, "compressor": null, "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/fv_bool_bad").c_str(), 0755);
    ASSERT_TRUE(WriteVsiFile(test_dir.File("root/fv_bool_bad/.zarray"),
                             zarr_fv_bool_bad));
    {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, grp->OpenMDArray("fv_bool_bad", nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid fill_value"));
    }

    // _NCZARR_ARRAY dimrefs (external dim error branch)
    const std::string zarr_ncz = R"json({
      "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "C",
      "fill_value": 0, "compressor": null, "filters": null,
      "_NCZARR_ARRAY": {"dimrefs": ["/nonexistent_ncz_dim"]}
    })json";
    VSIMkdirRecursive(test_dir.File("root/arr_nczarr").c_str(), 0755);
    ASSERT_TRUE(
        WriteVsiFile(test_dir.File("root/arr_nczarr/.zarray"), zarr_ncz));
    {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      auto arr_ncz = grp->OpenMDArray("arr_nczarr", nullptr);
      EXPECT_NE(nullptr, arr_ncz);
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("Cannot find NCZarr dimension"));
    }
  }
}

// 4. Creation, Read/Write Round-Trips, Fortran order, Filters, and Tile
// Deletion
TEST_F(ZarrV2ArrayTest, CreationAndReadWriteRoundTrips) {
  const ScopedTestDir test_dir("zarr_v2_rw");
  const std::string root_dir = test_dir.File("rw_store");
  auto drv = GetGDALDriverManager()->GetDriverByName("Zarr");
  ASSERT_NE(nullptr, drv);

  const char* const ds_opts[] = {"FORMAT=ZARR_V2", nullptr};
  GDALDatasetUniquePtr ds(
      drv->CreateMultiDimensional(root_dir.c_str(), nullptr, ds_opts));
  ASSERT_NE(nullptr, ds);
  auto grp = ds->GetRootGroup();
  ASSERT_NE(nullptr, grp);

  auto dim_y = grp->CreateDimension("y", "", "", 4, nullptr);
  auto dim_x = grp->CreateDimension("x", "", "", 4, nullptr);
  ASSERT_NE(nullptr, dim_y);
  ASSERT_NE(nullptr, dim_x);

  // 1) 2D array in C order with ZLIB compression
  {
    const char* const arr_opts[] = {"BLOCKSIZE=2,2", "COMPRESS=ZLIB",
                                    "DIM_SEPARATOR=/", nullptr};
    auto arr =
        grp->CreateMDArray("arr_c", {dim_y, dim_x},
                           GDALExtendedDataType::Create(GDT_Int32), arr_opts);
    ASSERT_NE(nullptr, arr);

    const GUInt64 start[] = {0, 0};
    const size_t count[] = {2, 2};
    const GInt64 step[] = {1, 1};
    const GPtrDiff_t stride[] = {2, 1};
    const int wdata[4] = {10, 20, 30, 40};
    EXPECT_TRUE(arr->Write(start, count, step, stride,
                           GDALExtendedDataType::Create(GDT_Int32), wdata));
    EXPECT_TRUE(std::dynamic_pointer_cast<ZarrArray>(arr)->Flush());

    int rdata[4] = {0};
    EXPECT_TRUE(arr->Read(start, count, step, stride,
                          GDALExtendedDataType::Create(GDT_Int32), rdata));
    EXPECT_EQ(10, rdata[0]);
    EXPECT_EQ(20, rdata[1]);
    EXPECT_EQ(30, rdata[2]);
    EXPECT_EQ(40, rdata[3]);

    // Overwrite tile with fill_value (0) to trigger tile file deletion (unlink)
    const int zeros[4] = {0, 0, 0, 0};
    EXPECT_TRUE(arr->Write(start, count, step, stride,
                           GDALExtendedDataType::Create(GDT_Int32), zeros));
    EXPECT_TRUE(std::dynamic_pointer_cast<ZarrArray>(arr)->Flush());
    // File root_dir/arr_c/0/0 should be unlinked
    VSIStatBufL sStat;
    EXPECT_NE(0, VSIStatL(test_dir.File("rw_store/arr_c/0/0").c_str(), &sStat));
  }

  // 2) 2D array in Fortran order (CHUNK_MEMORY_LAYOUT=F) with BLOSC
  {
    const char* const arr_opts[] = {"BLOCKSIZE=2,2", "CHUNK_MEMORY_LAYOUT=F",
                                    "COMPRESS=BLOSC", nullptr};
    auto arr_f =
        grp->CreateMDArray("arr_f", {dim_y, dim_x},
                           GDALExtendedDataType::Create(GDT_Int32), arr_opts);
    ASSERT_NE(nullptr, arr_f);

    const GUInt64 start[] = {0, 0};
    const size_t count[] = {2, 2};
    const GInt64 step[] = {1, 1};
    const GPtrDiff_t stride[] = {2, 1};
    const int wdata[4] = {100, 200, 300, 400};
    EXPECT_TRUE(arr_f->Write(start, count, step, stride,
                             GDALExtendedDataType::Create(GDT_Int32), wdata));
    EXPECT_TRUE(std::dynamic_pointer_cast<ZarrArray>(arr_f)->Flush());
  }

  // 3) 2D array with FILTER=DELTA in C order
  {
    const char* const arr_opts[] = {"BLOCKSIZE=2,2", "FILTER=DELTA", nullptr};
    auto arr_delta =
        grp->CreateMDArray("arr_delta", {dim_y, dim_x},
                           GDALExtendedDataType::Create(GDT_Int32), arr_opts);
    ASSERT_NE(nullptr, arr_delta);

    const GUInt64 start[] = {0, 0};
    const size_t count[] = {2, 2};
    const GInt64 step[] = {1, 1};
    const GPtrDiff_t stride[] = {2, 1};
    const int wdata[4] = {1, 2, 3, 4};
    EXPECT_TRUE(arr_delta->Write(start, count, step, stride,
                                 GDALExtendedDataType::Create(GDT_Int32),
                                 wdata));
    EXPECT_TRUE(std::dynamic_pointer_cast<ZarrArray>(arr_delta)->Flush());
  }

  // 4) 0-dimensional scalar array
  {
    auto arr_0d = grp->CreateMDArray(
        "arr_0d", {}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
    ASSERT_NE(nullptr, arr_0d);
    const int wval = 777;
    EXPECT_TRUE(arr_0d->Write(nullptr, nullptr, nullptr, nullptr,
                              GDALExtendedDataType::Create(GDT_Int32), &wval));
    EXPECT_TRUE(std::dynamic_pointer_cast<ZarrArray>(arr_0d)->Flush());
  }

  // Re-open dataset to verify on-disk read paths (Fortran layout transpose,
  // Delta filter decoding, and 0D)
  dim_y.reset();
  dim_x.reset();
  grp.reset();
  ds.reset();
  ds.reset(GDALDataset::Open(root_dir.c_str(), GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, ds);
  grp = ds->GetRootGroup();
  ASSERT_NE(nullptr, grp);

  {
    auto arr_f = grp->OpenMDArray("arr_f", nullptr);
    ASSERT_NE(nullptr, arr_f);
    const GUInt64 start[] = {0, 0};
    const size_t count[] = {2, 2};
    const GInt64 step[] = {1, 1};
    const GPtrDiff_t stride[] = {2, 1};
    int rdata[4] = {0};
    EXPECT_TRUE(arr_f->Read(start, count, step, stride,
                            GDALExtendedDataType::Create(GDT_Int32), rdata));
    EXPECT_EQ(100, rdata[0]);
    EXPECT_EQ(200, rdata[1]);
    EXPECT_EQ(300, rdata[2]);
    EXPECT_EQ(400, rdata[3]);

    // AdviseRead with NUM_THREADS=1 and NUM_THREADS=2
    const GUInt64 adv_start[] = {0, 0};
    const size_t adv_count[] = {4, 4};
    const char* const opts1[] = {"NUM_THREADS=1", nullptr};
    EXPECT_TRUE(arr_f->AdviseRead(adv_start, adv_count, opts1));
    const char* const opts2[] = {"NUM_THREADS=2", "CACHE_SIZE=1048576",
                                 nullptr};
    EXPECT_TRUE(arr_f->AdviseRead(adv_start, adv_count, opts2));
  }

  {
    auto arr_delta = grp->OpenMDArray("arr_delta", nullptr);
    ASSERT_NE(nullptr, arr_delta);
    const GUInt64 start[] = {0, 0};
    const size_t count[] = {2, 2};
    const GInt64 step[] = {1, 1};
    const GPtrDiff_t stride[] = {2, 1};
    int rdata[4] = {0};
    EXPECT_TRUE(arr_delta->Read(start, count, step, stride,
                                GDALExtendedDataType::Create(GDT_Int32),
                                rdata));
    EXPECT_EQ(1, rdata[0]);
    EXPECT_EQ(2, rdata[1]);
    EXPECT_EQ(3, rdata[2]);
    EXPECT_EQ(4, rdata[3]);
  }

  {
    auto arr_0d = grp->OpenMDArray("arr_0d", nullptr);
    ASSERT_NE(nullptr, arr_0d);
    int rval = 0;
    EXPECT_TRUE(arr_0d->Read(nullptr, nullptr, nullptr, nullptr,
                             GDALExtendedDataType::Create(GDT_Int32), &rval));
    EXPECT_EQ(777, rval);
  }
}

// 5. White-box testing of NeedDecodedBuffer, AllocateWorkingBuffers, and error
// paths
TEST_F(ZarrV2ArrayTest, WhiteBoxAndEdgeCases) {
  const ScopedTestDir test_dir("zarr_v2_wb");
  auto shared = ZarrSharedResource::Create(test_dir.File("wb_store"),
                                           /*bUpdatable=*/true);
  ASSERT_NE(nullptr, shared);
  VSIMkdirRecursive(test_dir.File("wb_store/wb_arr").c_str(), 0755);

  auto dim0 = std::make_shared<ZarrDimension>(
      shared, std::weak_ptr<ZarrGroupBase>(), "/", "dim0", "", "", 4);
  auto dim1 = std::make_shared<ZarrDimension>(
      shared, std::weak_ptr<ZarrGroupBase>(), "/", "dim1", "", "", 4);
  std::vector<std::shared_ptr<GDALDimension>> dims = {dim0, dim1};

  // NeedDecodedBuffer() == true with byte swapping
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

  auto arr = ZarrV2Array::Create(shared, "/", "wb_arr", dims, elt.gdalType,
                                 elts, block_size, false);
  ASSERT_NE(nullptr, arr);
  arr->SetUpdatable(true);
  arr->SetFilename(test_dir.File("wb_store/wb_arr/.zarray"));
  arr->SetDimSeparator("/");
  CPLJSONDocument dtype_doc;
  dtype_doc.LoadMemory(R"(">i4")");
  arr->SetDtype(dtype_doc.GetRoot());

  const GUInt64 start[] = {0, 0};
  const size_t count[] = {2, 2};
  const GInt64 step[] = {1, 1};
  const GPtrDiff_t stride[] = {2, 1};
  const int wdata[4] = {0x01020304, 0x05060708, 0x11223344, 0x55667788};
  EXPECT_TRUE(arr->Write(start, count, step, stride,
                         GDALExtendedDataType::Create(GDT_Int32), wdata));
  EXPECT_TRUE(arr->Flush());

  // AdviseRead on array with NeedDecodedBuffer() == true
  const GUInt64 full_start[] = {0, 0};
  const size_t full_count[] = {4, 4};
  const char* const adv_opts[] = {"NUM_THREADS=2", "CACHE_SIZE=1048576",
                                  nullptr};
  EXPECT_TRUE(arr->AdviseRead(full_start, full_count, adv_opts));

  int rdata[4] = {0};
  EXPECT_TRUE(arr->Read(start, count, step, stride,
                        GDALExtendedDataType::Create(GDT_Int32), rdata));
  EXPECT_EQ(0x01020304, rdata[0]);
  EXPECT_EQ(0x05060708, rdata[1]);
  EXPECT_EQ(0x11223344, rdata[2]);
  EXPECT_EQ(0x55667788, rdata[3]);

  // Unit, offset, scale modification and serialization
  arr->SetUnit("meters");
  arr->SetOffset(10.0, GDT_Float64);
  arr->SetScale(2.5, GDT_Float64);
  const int32_t nd = -9999;
  EXPECT_TRUE(arr->SetRawNoDataValue(&nd));
  dim0->SetXArrayDimension();
  dim1->SetXArrayDimension();
  EXPECT_TRUE(dim0->Rename("dim0_renamed"));
  EXPECT_TRUE(arr->Flush());
  EXPECT_THAT(ReadVsiFile(test_dir.File("wb_store/wb_arr/.zarray")),
              HasSubstr("-9999"));
  EXPECT_THAT(ReadVsiFile(test_dir.File("wb_store/wb_arr/.zattrs")),
              HasSubstr("dim0_renamed"));

  // Tile creation error: block write by creating a regular file where directory
  // is needed
  WriteVsiFile(test_dir.File("wb_store/wb_err/0"),
               "regular_file_blocking_mkdir");
  arr->SetFilename(test_dir.File("wb_store/wb_err/.zarray"));
  EXPECT_TRUE(arr->Write(start, count, step, stride,
                         GDALExtendedDataType::Create(GDT_Int32), wdata));
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(arr->Flush());
  }

  // ZARR_ALLOW_BIG_TILE_SIZE=NO check with large chunk
  {
    std::vector<GUInt64> huge_block = {
        300, 1024, 1024};  // 300 * 1024 * 1024 * 4B > 1.17 GB
    auto dim_h0 = std::make_shared<ZarrDimension>(
        shared, std::weak_ptr<ZarrGroupBase>(), "/", "h0", "", "", 300);
    auto dim_h1 = std::make_shared<ZarrDimension>(
        shared, std::weak_ptr<ZarrGroupBase>(), "/", "h1", "", "", 1024);
    auto dim_h2 = std::make_shared<ZarrDimension>(
        shared, std::weak_ptr<ZarrGroupBase>(), "/", "h2", "", "", 1024);
    auto huge_arr =
        ZarrV2Array::Create(shared, "/", "huge_arr", {dim_h0, dim_h1, dim_h2},
                            elt.gdalType, elts, huge_block, false);
    ASSERT_NE(nullptr, huge_arr);
    huge_arr->SetFilename(test_dir.File("wb_store/huge_arr/.zarray"));
    const CPLConfigOptionSetter setter("ZARR_ALLOW_BIG_TILE_SIZE", "NO", false);
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    int dummy = 0;
    const GUInt64 h_start[] = {0, 0, 0};
    const size_t h_count[] = {1, 1, 1};
    EXPECT_FALSE(huge_arr->Read(h_start, h_count, nullptr, nullptr,
                                GDALExtendedDataType::Create(GDT_Int32),
                                &dummy));
  }

  // Tile presence cache (.gmac) on Zarr V2
  {
    VSIMkdirRecursive(test_dir.File("wb_store/wb_cache").c_str(), 0755);
    auto arr_cache = ZarrV2Array::Create(shared, "/", "wb_cache", dims,
                                         elt.gdalType, elts, block_size, false);
    ASSERT_NE(nullptr, arr_cache);
    arr_cache->SetUpdatable(true);
    arr_cache->SetFilename(test_dir.File("wb_store/wb_cache/.zarray"));

    const std::string gmac_path =
        test_dir.File("wb_store/wb_cache/.zarray.gmac");
    auto drv = GetGDALDriverManager()->GetDriverByName("Zarr");
    ASSERT_NE(nullptr, drv);
    GDALDatasetUniquePtr ds_gmac(
        drv->CreateMultiDimensional(gmac_path.c_str(), nullptr, nullptr));
    EXPECT_TRUE(arr_cache->CacheTilePresence());
  }
}

// 6. Upward directory recursion, path traversal, and load errors
TEST_F(ZarrV2ArrayTest, UpwardRecursionAndPathTraversal) {
  const ScopedTestDir test_dir("zarr_v2_recurse");
  const std::string root_dir = test_dir.File("root");
  VSIMkdirRecursive(test_dir.File("root/sub").c_str(), 0755);
  WriteVsiFile(test_dir.File("root/.zgroup"), R"json({"zarr_format": 2})json");
  WriteVsiFile(test_dir.File("root/sub/.zgroup"),
               R"json({"zarr_format": 2})json");

  GDALDatasetUniquePtr ds(GDALDataset::Open(
      root_dir.c_str(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_UPDATE));
  ASSERT_NE(nullptr, ds);
  auto grp = ds->GetRootGroup();
  ASSERT_NE(nullptr, grp);
  auto sub_grp = grp->OpenGroup("sub", nullptr);
  ASSERT_NE(nullptr, sub_grp);

  // Upward dimension recursion
  const std::string zarr_rec = R"json({
    "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "C",
    "fill_value": 0, "compressor": null, "filters": null
  })json";
  VSIMkdirRecursive(test_dir.File("root/sub/arr_rec").c_str(), 0755);
  WriteVsiFile(test_dir.File("root/sub/arr_rec/.zarray"), zarr_rec);
  WriteVsiFile(test_dir.File("root/sub/arr_rec/.zattrs"),
               R"json({"_ARRAY_DIMENSIONS": ["nonexistent_dim"]})json");
  auto arr_rec = sub_grp->OpenMDArray("arr_rec", nullptr);
  ASSERT_NE(nullptr, arr_rec);

  // Path traversal in dimension name
  const std::string zarr_trav = R"json({
    "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "C",
    "fill_value": 0, "compressor": null, "filters": null
  })json";
  VSIMkdirRecursive(test_dir.File("root/arr_trav").c_str(), 0755);
  ASSERT_TRUE(WriteVsiFile(test_dir.File("root/arr_trav/.zarray"), zarr_trav));
  ASSERT_TRUE(
      WriteVsiFile(test_dir.File("root/arr_trav/.zattrs"),
                   R"json({"_ARRAY_DIMENSIONS": ["../../escape"]})json"));
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_NE(nullptr, grp->OpenMDArray("arr_trav", nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Path traversal"));
  }

  // Path traversal in grid_mapping
  const std::string zarr_gm_trav = R"json({
    "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "C",
    "fill_value": 0, "compressor": null, "filters": null
  })json";
  VSIMkdirRecursive(test_dir.File("root/arr_gm_trav").c_str(), 0755);
  ASSERT_TRUE(
      WriteVsiFile(test_dir.File("root/arr_gm_trav/.zarray"), zarr_gm_trav));
  ASSERT_TRUE(WriteVsiFile(test_dir.File("root/arr_gm_trav/.zattrs"),
                           R"json({"grid_mapping": "../../escape_gm"})json"));
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, grp->OpenMDArray("arr_gm_trav", nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Path traversal"));
  }

  // LoadArray error cases:
  struct BadArrayCase {
    std::string_view name;
    std::string_view json;
    std::string_view expected_err;
  };
  constexpr auto kBadArrays = std::to_array<BadArrayCase>({
      {.name = "bad_fmt",
       .json =
           R"json({"zarr_format": 3, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "C"})json",
       .expected_err = "Invalid value for zarr_format"},
      {.name = "bad_order",
       .json =
           R"json({"zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "X"})json",
       .expected_err = "Invalid value for order"},
      {.name = "bad_shape",
       .json =
           R"json({"zarr_format": 2, "shape": 2, "chunks": [2], "dtype": "<i4", "order": "C"})json",
       .expected_err = "shape missing or not an array"},
      {.name = "bad_chunks",
       .json =
           R"json({"zarr_format": 2, "shape": [2], "chunks": 2, "dtype": "<i4", "order": "C"})json",
       .expected_err = "chunks missing or not an array"},
      {.name = "bad_mismatch",
       .json =
           R"json({"zarr_format": 2, "shape": [2, 2], "chunks": [2], "dtype": "<i4", "order": "C"})json",
       .expected_err = "shape and chunks arrays are of different size"},
      {.name = "bad_shape_0",
       .json =
           R"json({"zarr_format": 2, "shape": [0], "chunks": [0], "dtype": "<i4", "order": "C"})json",
       .expected_err = "Invalid content for shape"},
      {.name = "bad_nodt",
       .json =
           R"json({"zarr_format": 2, "shape": [2], "chunks": [2], "order": "C"})json",
       .expected_err = "dtype missing"},
  });
  for (const auto& tc : kBadArrays) {
    SCOPED_TRACE(tc.name);
    VSIMkdirRecursive(test_dir.File(std::format("root/{}", tc.name)).c_str(),
                      0755);
    ASSERT_TRUE(WriteVsiFile(
        test_dir.File(std::format("root/{}/.zarray", tc.name)), tc.json));
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, grp->OpenMDArray(std::string(tc.name), nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr(tc.expected_err));
  }
}

// 7. Comprehensive edge cases for > 90% coverage
TEST_F(ZarrV2ArrayTest, AdditionalCoverageAndEdgeCases) {
  const ScopedTestDir test_dir("zarr_v2_add_cov");
  const std::string root_dir = test_dir.File("root");
  VSIMkdirRecursive(root_dir.c_str(), 0755);
  WriteVsiFile(test_dir.File("root/.zgroup"), R"json({"zarr_format": 2})json");

  GDALDatasetUniquePtr ds(GDALDataset::Open(
      root_dir.c_str(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_UPDATE));
  ASSERT_NE(nullptr, ds);
  auto grp = ds->GetRootGroup();
  ASSERT_NE(nullptr, grp);

  // 1) Serialize() on GEDTC_STRING (null and non-null) and GEDTC_COMPOUND (with
  // non-null fill_value)
  {
    auto shared = ZarrSharedResource::Create(test_dir.File("add_wb"),
                                             /*bUpdatable=*/true);
    ASSERT_NE(nullptr, shared);
    auto dim0 = std::make_shared<ZarrDimension>(
        shared, std::weak_ptr<ZarrGroupBase>(), "/", "y", "", "", 2);
    DtypeElt s_elt;
    s_elt.nativeType = DtypeElt::NativeType::STRING_ASCII;
    s_elt.nativeSize = 4;
    s_elt.gdalType = GDALExtendedDataType::CreateString(4);
    s_elt.gdalSize = s_elt.gdalType.GetSize();
    auto s_arr = ZarrV2Array::Create(shared, "/", "s_arr", {dim0},
                                     s_elt.gdalType, {s_elt}, {2}, false);
    ASSERT_NE(nullptr, s_arr);
    s_arr->SetUpdatable(true);
    s_arr->SetFilename(test_dir.File("add_wb/s_arr/.zarray"));
    s_arr->SetDefinitionModified(true);
    EXPECT_TRUE(s_arr->Flush());

    const std::unique_ptr<char, CplFreeDeleter> test_str(CPLStrdup("ABCD"));
    const char* raw_str = test_str.get();
    EXPECT_TRUE(s_arr->SetRawNoDataValue(&raw_str));
    s_arr->SetDefinitionModified(true);
    EXPECT_TRUE(s_arr->Flush());
    EXPECT_THAT(ReadVsiFile(test_dir.File("add_wb/s_arr/.zarray")),
                HasSubstr("QUJDRA=="));

    // Compound serialize with fill_value
    DtypeElt c_elt1;
    c_elt1.nativeType = DtypeElt::NativeType::SIGNED_INT;
    c_elt1.nativeSize = 4;
    c_elt1.gdalType = GDALExtendedDataType::Create(GDT_Int32);
    c_elt1.gdalSize = 4;
    DtypeElt c_elt2;
    c_elt2.nativeType = DtypeElt::NativeType::IEEEFP;
    c_elt2.nativeOffset = 4;
    c_elt2.nativeSize = 8;
    c_elt2.gdalType = GDALExtendedDataType::Create(GDT_Float64);
    c_elt2.gdalOffset = 8;  // with alignment padding
    c_elt2.gdalSize = 8;
    std::vector<std::unique_ptr<GDALEDTComponent>> comps;
    comps.emplace_back(
        std::make_unique<GDALEDTComponent>("a", 0, c_elt1.gdalType));
    comps.emplace_back(
        std::make_unique<GDALEDTComponent>("b", 8, c_elt2.gdalType));
    auto comp_dt = GDALExtendedDataType::Create("comp", 16, std::move(comps));
    auto c_arr = ZarrV2Array::Create(shared, "/", "c_arr", {dim0}, comp_dt,
                                     {c_elt1, c_elt2}, {2}, false);
    ASSERT_NE(nullptr, c_arr);
    c_arr->SetUpdatable(true);
    c_arr->SetFilename(test_dir.File("add_wb/c_arr/.zarray"));
    GByte c_nd[16] = {0};
    EXPECT_TRUE(c_arr->SetRawNoDataValue(c_nd));
    c_arr->SetDefinitionModified(true);
    EXPECT_TRUE(c_arr->Flush());
  }

  // 2) Tile presence cache (.gmac) fast-skip in LoadTileData
  {
    auto shared = ZarrSharedResource::Create(test_dir.File("cache_wb"),
                                             /*bUpdatable=*/true);
    auto dim0 = std::make_shared<ZarrDimension>(
        shared, std::weak_ptr<ZarrGroupBase>(), "/", "d0", "", "", 4);
    auto dim1 = std::make_shared<ZarrDimension>(
        shared, std::weak_ptr<ZarrGroupBase>(), "/", "d1", "", "", 4);
    DtypeElt elt;
    elt.nativeType = DtypeElt::NativeType::SIGNED_INT;
    elt.nativeSize = 4;
    elt.gdalType = GDALExtendedDataType::Create(GDT_Int32);
    elt.gdalSize = 4;
    auto arr_cache = ZarrV2Array::Create(shared, "/", "arr_cache", {dim0, dim1},
                                         elt.gdalType, {elt}, {2, 2}, false);
    ASSERT_NE(nullptr, arr_cache);
    arr_cache->SetUpdatable(true);
    arr_cache->SetFilename(test_dir.File("cache_wb/arr_cache/.zarray"));
    const std::string gmac_path =
        test_dir.File("cache_wb/arr_cache/.zarray.gmac");
    auto drv = GetGDALDriverManager()->GetDriverByName("Zarr");
    ASSERT_NE(nullptr, drv);
    GDALDatasetUniquePtr ds_gmac(
        drv->CreateMultiDimensional(gmac_path.c_str(), nullptr, nullptr));
    EXPECT_TRUE(arr_cache->CacheTilePresence());
    const GUInt64 empty_start[] = {2, 2};
    const size_t empty_count[] = {2, 2};
    int empty_buf[4] = {-1, -1, -1, -1};
    EXPECT_TRUE(arr_cache->Read(empty_start, empty_count, nullptr, nullptr,
                                GDALExtendedDataType::Create(GDT_Int32),
                                empty_buf));
    EXPECT_EQ(0, empty_buf[0]);
  }

  // 3) Read-only filter write error branches (quantize, fixedscaleoffset,
  // imagecodecs_tiff)
  {
    const std::string zarr_quant = R"json({
      "zarr_format": 2, "shape": [4], "chunks": [4], "dtype": "<f4", "order": "C",
      "fill_value": 0, "compressor": null,
      "filters": [{"id": "quantize", "digits": 2, "dtype": "<f4"}]
    })json";
    VSIMkdirRecursive(test_dir.File("root/arr_quant_ro").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_quant_ro/.zarray"), zarr_quant);
    auto arr_q = grp->OpenMDArray("arr_quant_ro", nullptr);
    ASSERT_NE(nullptr, arr_q);
    const GUInt64 q_start[] = {0};
    const size_t q_count[] = {1};
    float q_val = 1.0f;
    EXPECT_TRUE(arr_q->Write(q_start, q_count, nullptr, nullptr,
                             GDALExtendedDataType::Create(GDT_Float32),
                             &q_val));
    {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_FALSE(std::dynamic_pointer_cast<ZarrArray>(arr_q)->Flush());
    }

    const std::string zarr_tiff = R"json({
      "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "C",
      "fill_value": 0, "compressor": {"id": "imagecodecs_tiff"}, "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/arr_tiff_ro").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_tiff_ro/.zarray"), zarr_tiff);
    auto arr_t = grp->OpenMDArray("arr_tiff_ro", nullptr);
    ASSERT_NE(nullptr, arr_t);
    int t_val = 1;
    EXPECT_TRUE(arr_t->Write(q_start, q_count, nullptr, nullptr,
                             GDALExtendedDataType::Create(GDT_Int32), &t_val));
    {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_FALSE(std::dynamic_pointer_cast<ZarrArray>(arr_t)->Flush());
    }
  }

  // 4) Corrupt compressed tile read failure
  {
    const std::string zarr_zlib = R"json({
      "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "C",
      "fill_value": 0, "compressor": {"id": "zlib", "level": 1}, "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/arr_zlib_bad").c_str(), 0755);
    WriteVsiFile(test_dir.File("root/arr_zlib_bad/.zarray"), zarr_zlib);
    WriteVsiFile(test_dir.File("root/arr_zlib_bad/0"),
                 "corrupted_non_zlib_data");
    auto arr_z = grp->OpenMDArray("arr_zlib_bad", nullptr);
    ASSERT_NE(nullptr, arr_z);
    int z_buf[2] = {0};
    const GUInt64 z_start[] = {0};
    const size_t z_count[] = {2};
    {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_FALSE(arr_z->Read(z_start, z_count, nullptr, nullptr,
                               GDALExtendedDataType::Create(GDT_Int32), z_buf));
    }
  }

  // 5) _NCZARR_ARRAY.dimrefs: 1D indexing variable, matching dim, mismatched
  // dim
  {
    const std::string zarr_ncz_idx = R"json({
      "zarr_format": 2, "shape": [4], "chunks": [4], "dtype": "<i4", "order": "C",
      "fill_value": 0, "compressor": null, "filters": null,
      "_NCZARR_ARRAY": {"dimrefs": ["/ncz_idx"]}
    })json";
    VSIMkdirRecursive(test_dir.File("root/ncz_idx").c_str(), 0755);
    ASSERT_TRUE(
        WriteVsiFile(test_dir.File("root/ncz_idx/.zarray"), zarr_ncz_idx));
    ASSERT_TRUE(WriteVsiFile(
        test_dir.File("root/ncz_idx/.zattrs"),
        R"json({"standard_name": "projection_x_coordinate"})json"));
    auto ncz_idx = grp->OpenMDArray("ncz_idx", nullptr);
    ASSERT_NE(nullptr, ncz_idx);
    EXPECT_EQ(GDAL_DIM_TYPE_HORIZONTAL_X,
              ncz_idx->GetDimensions()[0]->GetType());

    auto root_dim = grp->CreateDimension("ncz_dim", "", "", 4, nullptr);
    ASSERT_NE(nullptr, root_dim);
    const std::string zarr_ncz_dim = R"json({
      "zarr_format": 2, "shape": [4], "chunks": [4], "dtype": "<i4", "order": "C",
      "fill_value": 0, "compressor": null, "filters": null,
      "_NCZARR_ARRAY": {"dimrefs": ["/ncz_dim"]}
    })json";
    VSIMkdirRecursive(test_dir.File("root/ncz_match").c_str(), 0755);
    ASSERT_TRUE(
        WriteVsiFile(test_dir.File("root/ncz_match/.zarray"), zarr_ncz_dim));
    auto ncz_match = grp->OpenMDArray("ncz_match", nullptr);
    ASSERT_NE(nullptr, ncz_match);
    EXPECT_EQ("ncz_dim", ncz_match->GetDimensions()[0]->GetName());

    const std::string zarr_ncz_mismatch = R"json({
      "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "C",
      "fill_value": 0, "compressor": null, "filters": null,
      "_NCZARR_ARRAY": {"dimrefs": ["/ncz_dim"]}
    })json";
    VSIMkdirRecursive(test_dir.File("root/ncz_mismatch").c_str(), 0755);
    ASSERT_TRUE(WriteVsiFile(test_dir.File("root/ncz_mismatch/.zarray"),
                             zarr_ncz_mismatch));
    {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      auto ncz_mis = grp->OpenMDArray("ncz_mismatch", nullptr);
      EXPECT_NE(nullptr, ncz_mis);
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Inconsistency in size"));
    }
  }

  // 6) Valid grid_mapping attribute auto-opening referenced array
  {
    const std::string zarr_crs = R"json({
      "zarr_format": 2, "shape": [], "chunks": [], "dtype": "<i4", "order": "C",
      "fill_value": 0, "compressor": null, "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/crs").c_str(), 0755);
    ASSERT_TRUE(WriteVsiFile(test_dir.File("root/crs/.zarray"), zarr_crs));

    const std::string zarr_gm_valid = R"json({
      "zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "C",
      "fill_value": 0, "compressor": null, "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/arr_gm_valid").c_str(), 0755);
    ASSERT_TRUE(WriteVsiFile(test_dir.File("root/arr_gm_valid/.zarray"),
                             zarr_gm_valid));
    ASSERT_TRUE(WriteVsiFile(test_dir.File("root/arr_gm_valid/.zattrs"),
                             R"json({"grid_mapping": "crs"})json"));
    auto arr_gm_v = grp->OpenMDArray("arr_gm_valid", nullptr);
    ASSERT_NE(nullptr, arr_gm_v);
  }

  // 7) CACHE_TILE_PRESENCE=YES open option
  {
    const std::string multi_dir = test_dir.File("root/arr_multi_cp");
    VSIMkdirRecursive(multi_dir.c_str(), 0755);
    const std::string zarr_multi = R"json({
      "zarr_format": 2, "shape": [4, 4], "chunks": [2, 2], "dtype": "<i4", "order": "C",
      "fill_value": 0, "compressor": null, "filters": null
    })json";
    ASSERT_TRUE(
        WriteVsiFile(test_dir.File("root/arr_multi_cp/.zarray"), zarr_multi));
    const std::string gmac_file =
        test_dir.File("root/arr_multi_cp/.zarray.gmac");
    auto drv = GetGDALDriverManager()->GetDriverByName("Zarr");
    ASSERT_NE(nullptr, drv);
    {
      GDALDatasetUniquePtr ds_gmac(
          drv->CreateMultiDimensional(gmac_file.c_str(), nullptr, nullptr));
      ASSERT_NE(nullptr, ds_gmac);
    }

    const char* const cache_opts[] = {"CACHE_TILE_PRESENCE=YES", nullptr};
    GDALDatasetUniquePtr ds_cp(GDALDataset::Open(root_dir.c_str(),
                                                 GDAL_OF_MULTIDIM_RASTER,
                                                 nullptr, cache_opts, nullptr));
    ASSERT_NE(nullptr, ds_cp);
    auto grp_cp = ds_cp->GetRootGroup();
    ASSERT_NE(nullptr, grp_cp);
    auto arr_cp = grp_cp->OpenMDArray("arr_multi_cp", nullptr);
    ASSERT_NE(nullptr, arr_cp);
    ds_cp.reset();

    GDALDatasetUniquePtr ds_gmac_check(
        GDALDataset::Open(gmac_file.c_str(), GDAL_OF_MULTIDIM_RASTER));
    ASSERT_NE(nullptr, ds_gmac_check);
    auto gmac_root = ds_gmac_check->GetRootGroup();
    ASSERT_NE(nullptr, gmac_root);
    EXPECT_FALSE(gmac_root->GetMDArrayNames().empty());
  }

  // 8) GetAlignment with complex, nested compound, and short string
  {
    const std::string zarr_align = R"json({
      "zarr_format": 2, "shape": [2], "chunks": [2], "order": "C",
      "dtype": [["c8", "<c8"], ["c16", "<c16"], ["nested", [["x", "<i4"]]], ["str", "|S3"]],
      "fill_value": null, "compressor": null, "filters": null
    })json";
    VSIMkdirRecursive(test_dir.File("root/arr_align").c_str(), 0755);
    ASSERT_TRUE(
        WriteVsiFile(test_dir.File("root/arr_align/.zarray"), zarr_align));
    auto arr_align = grp->OpenMDArray("arr_align", nullptr);
    ASSERT_NE(nullptr, arr_align);
  }

  // 9) Missing / invalid compressor and filters error checks
  {
    ASSERT_TRUE(WriteVsiFile(
        test_dir.File("root/no_comp/.zarray"),
        R"json({"zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "C", "filters": null})json"));
    ASSERT_TRUE(WriteVsiFile(
        test_dir.File("root/bad_comp_id/.zarray"),
        R"json({"zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "C", "compressor": {}, "filters": null})json"));
    ASSERT_TRUE(WriteVsiFile(
        test_dir.File("root/bad_comp_type/.zarray"),
        R"json({"zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "C", "compressor": 123, "filters": null})json"));
    ASSERT_TRUE(WriteVsiFile(
        test_dir.File("root/no_filt/.zarray"),
        R"json({"zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "C", "compressor": null})json"));
    ASSERT_TRUE(WriteVsiFile(
        test_dir.File("root/bad_filt_type/.zarray"),
        R"json({"zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "C", "compressor": null, "filters": 123})json"));
    ASSERT_TRUE(WriteVsiFile(
        test_dir.File("root/bad_filt_id/.zarray"),
        R"json({"zarr_format": 2, "shape": [2], "chunks": [2], "dtype": "<i4", "order": "C", "compressor": null, "filters": [{}]})json"));

    const char* const kBadOptArrays[] = {"no_comp",       "bad_comp_id",
                                         "bad_comp_type", "no_filt",
                                         "bad_filt_type", "bad_filt_id"};
    for (const char* bname : kBadOptArrays) {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, grp->OpenMDArray(bname, nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_STRNE("", CPLGetLastErrorMsg());
    }
  }

  // 10) .zmetadata with subgroup referencing parent dimension
  {
    const std::string zmeta_root = test_dir.File("zmeta_store");
    VSIMkdirRecursive(zmeta_root.c_str(), 0755);
    const std::string zmeta_json = R"json({
      "metadata": {
        ".zgroup": {"zarr_format": 2},
        ".zattrs": {},
        "dim_x/.zarray": {"zarr_format": 2, "shape": [4], "chunks": [4], "dtype": "<i4", "order": "C", "fill_value": 0, "compressor": null, "filters": null},
        "dim_x/.zattrs": {"_ARRAY_DIMENSIONS": ["dim_x"]},
        "sub/.zgroup": {"zarr_format": 2},
        "sub/.zattrs": {},
        "sub/arr_sub/.zarray": {"zarr_format": 2, "shape": [4], "chunks": [4], "dtype": "<i4", "order": "C", "fill_value": 0, "compressor": null, "filters": null},
        "sub/arr_sub/.zattrs": {"_ARRAY_DIMENSIONS": ["dim_x"]}
      },
      "zarr_consolidated_format": 1
    })json";
    ASSERT_TRUE(
        WriteVsiFile(test_dir.File("zmeta_store/.zmetadata"), zmeta_json));
    ASSERT_TRUE(WriteVsiFile(test_dir.File("zmeta_store/.zgroup"),
                             R"json({"zarr_format": 2})json"));
    GDALDatasetUniquePtr ds_zm(
        GDALDataset::Open(zmeta_root.c_str(), GDAL_OF_MULTIDIM_RASTER));
    ASSERT_NE(nullptr, ds_zm);
    auto grp_zm = ds_zm->GetRootGroup();
    ASSERT_NE(nullptr, grp_zm);
    auto arr_dim_x = grp_zm->OpenMDArray("dim_x", nullptr);
    ASSERT_NE(nullptr, arr_dim_x);
    auto sub_zm = grp_zm->OpenGroup("sub", nullptr);
    ASSERT_NE(nullptr, sub_zm);
    auto arr_sub = sub_zm->OpenMDArray("arr_sub", nullptr);
    ASSERT_NE(nullptr, arr_sub);
    EXPECT_EQ(arr_dim_x->GetDimensions()[0].get(),
              arr_sub->GetDimensions()[0].get());
  }
}

// TODO: b/335317901 - Test additional Zarr V2 legacy codecs and custom
// numcodecs filters.

}  // namespace
}  // namespace autotest2
