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
// Tests GeoTIFF dataset creation, writing, compression, metadata, overviews,
// georeferencing, masks, and CreateCopy (gtiffdataset_write.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/gtiff/gtiffdataset_write.cpp

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>  // NOLINT
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

int CPL_STDCALL FailingProgress(double /*dfComplete*/,
                                const char* /*pszMessage*/,
                                void* /*pProgressArg*/) {
  return 0;
}

class GTiffDatasetWriteTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALRegister_GTiff();
    // Register JPEG and MEM for cross-driver CreateCopy tests.
    GDALRegister_JPEG();
    GDALRegister_MEM();
  }

  void SetUp() override {
    CPLErrorReset();
    gtiff_driver_ = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, gtiff_driver_);
    mem_driver_ = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, mem_driver_);
  }

  void TearDown() override { CPLErrorReset(); }

  GDALDriver* gtiff_driver_ = nullptr;
  GDALDriver* mem_driver_ = nullptr;
};

// ---------------------------------------------------------------------------
// 1. Option Parsers & CreateLL Validation / Error / Warning Branches
// ---------------------------------------------------------------------------
TEST_F(GTiffDatasetWriteTest, OptionParsersAndCreateLLValidation) {
  // Invalid dimensions / bands. Results are held in GDALDatasetUniquePtr so
  // that an unexpected success does not leak the dataset.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds1(gtiff_driver_->Create("/vsimem/gw_bad_dim1.tif", 0,
                                                   10, 1, GDT_Byte, nullptr));
    EXPECT_EQ(nullptr, ds1);
    GDALDatasetUniquePtr ds2(gtiff_driver_->Create(
        "/vsimem/gw_bad_dim2.tif", 10, 0, 1, GDT_Byte, nullptr));
    EXPECT_EQ(nullptr, ds2);
    GDALDatasetUniquePtr ds3(gtiff_driver_->Create(
        "/vsimem/gw_bad_dim3.tif", 10, 10, 0, GDT_Byte, nullptr));
    EXPECT_EQ(nullptr, ds3);
    GDALDatasetUniquePtr ds4(gtiff_driver_->Create(
        "/vsimem/gw_bad_dim4.tif", 10, 10, 65536, GDT_Byte, nullptr));
    EXPECT_EQ(nullptr, ds4);
    CPLErrorReset();
  }

  // Block size validation.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    CPLStringList opts1;
    opts1.SetNameValue("BLOCKXSIZE", "-1");
    GDALDatasetUniquePtr ds1(gtiff_driver_->Create(
        "/vsimem/gw_bx_neg.tif", 16, 16, 1, GDT_Byte, opts1.List()));
    EXPECT_EQ(nullptr, ds1);

    CPLStringList opts2;
    opts2.SetNameValue("TILED", "YES");
    opts2.SetNameValue("BLOCKXSIZE", "15");
    GDALDatasetUniquePtr ds2(gtiff_driver_->Create(
        "/vsimem/gw_bx_15.tif", 16, 16, 1, GDT_Byte, opts2.List()));
    EXPECT_EQ(nullptr, ds2);

    CPLStringList opts3;
    opts3.SetNameValue("BLOCKYSIZE", "-1");
    GDALDatasetUniquePtr ds3(gtiff_driver_->Create(
        "/vsimem/gw_by_neg.tif", 16, 16, 1, GDT_Byte, opts3.List()));
    EXPECT_EQ(nullptr, ds3);

    CPLStringList opts4;
    opts4.SetNameValue("TILED", "YES");
    opts4.SetNameValue("BLOCKYSIZE", "15");
    GDALDatasetUniquePtr ds4(gtiff_driver_->Create(
        "/vsimem/gw_by_15.tif", 16, 16, 1, GDT_Byte, opts4.List()));
    EXPECT_EQ(nullptr, ds4);
    CPLErrorReset();
  }

  // Default 256x256 tile size when TILED=YES and BLOCKXSIZE/BLOCKYSIZE omitted,
  // plus invalid PROFILE warning.
  {
    constexpr char kPath[] = "/vsimem/gw_tiled_def.tif";
    absl::Cleanup cl = [&] { VSIUnlink(kPath); };
    CPLStringList opts;
    opts.SetNameValue("TILED", "YES");
    opts.SetNameValue("PROFILE", "INVALID_PROFILE");
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kPath, 16, 16, 1, GDT_Byte, opts.List()));
    CPLErrorReset();
    ASSERT_NE(nullptr, ds);
  }

  // BLOCKXSIZE with TILED=NO warning.
  {
    constexpr char kPath[] = "/vsimem/gw_bx_untiled.tif";
    absl::Cleanup cl = [&] { VSIUnlink(kPath); };
    CPLStringList opts;
    opts.SetNameValue("TILED", "NO");
    opts.SetNameValue("BLOCKXSIZE", "16");
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kPath, 16, 16, 1, GDT_Byte, opts.List()));
    CPLErrorReset();
    ASSERT_NE(nullptr, ds);
  }

  // Invalid INTERLEAVE (error) and COMPRESS (warning -> fallback to NONE).
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    CPLStringList opts_il;
    opts_il.SetNameValue("INTERLEAVE", "INVALID");
    GDALDatasetUniquePtr ds_il(gtiff_driver_->Create(
        "/vsimem/gw_bad_il.tif", 16, 16, 2, GDT_Byte, opts_il.List()));
    EXPECT_EQ(nullptr, ds_il);

    constexpr char kBadCompPath[] = "/vsimem/gw_bad_comp.tif";
    absl::Cleanup cl = [&] { VSIUnlink(kBadCompPath); };
    CPLStringList opts_comp;
    opts_comp.SetNameValue("COMPRESS", "NO_SUCH_CODEC");
    GDALDatasetUniquePtr ds_unk(gtiff_driver_->Create(
        kBadCompPath, 16, 16, 1, GDT_Byte, opts_comp.List()));
    CPLErrorReset();
    EXPECT_NE(nullptr, ds_unk);
  }

  // WEBP dimension limits (> 16383).
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    CPLStringList opts_w;
    opts_w.SetNameValue("COMPRESS", "WEBP");
    opts_w.SetNameValue("TILED", "NO");
    GDALDatasetUniquePtr ds_w(gtiff_driver_->Create(
        "/vsimem/gw_webp_w.tif", 16400, 1, 3, GDT_Byte, opts_w.List()));
    EXPECT_EQ(nullptr, ds_w);

    CPLStringList opts_bx;
    opts_bx.SetNameValue("COMPRESS", "WEBP");
    opts_bx.SetNameValue("TILED", "YES");
    opts_bx.SetNameValue("BLOCKXSIZE", "16400");
    opts_bx.SetNameValue("BLOCKYSIZE", "16");
    GDALDatasetUniquePtr ds_bx(gtiff_driver_->Create(
        "/vsimem/gw_webp_bx.tif", 16, 16, 3, GDT_Byte, opts_bx.List()));
    EXPECT_EQ(nullptr, ds_bx);

    CPLStringList opts_by;
    opts_by.SetNameValue("COMPRESS", "WEBP");
    opts_by.SetNameValue("TILED", "YES");
    opts_by.SetNameValue("BLOCKXSIZE", "16");
    opts_by.SetNameValue("BLOCKYSIZE", "16400");
    GDALDatasetUniquePtr ds_by(gtiff_driver_->Create(
        "/vsimem/gw_webp_by.tif", 16, 16, 3, GDT_Byte, opts_by.List()));
    EXPECT_EQ(nullptr, ds_by);
    CPLErrorReset();
  }

  // NBITS clamping and warnings across data types.
  struct NbitsCase {
    GDALDataType dt;
    const char* nbits;
  };
  constexpr NbitsCase kNbitsCases[] = {
      {.dt = GDT_Byte, .nbits = "0"},     {.dt = GDT_Byte, .nbits = "9"},
      {.dt = GDT_Byte, .nbits = "4"},     {.dt = GDT_UInt16, .nbits = "8"},
      {.dt = GDT_UInt16, .nbits = "17"},  {.dt = GDT_UInt16, .nbits = "12"},
      {.dt = GDT_UInt32, .nbits = "16"},  {.dt = GDT_UInt32, .nbits = "33"},
      {.dt = GDT_UInt32, .nbits = "20"},  {.dt = GDT_Float32, .nbits = "24"},
      {.dt = GDT_Float32, .nbits = "16"}, {.dt = GDT_Int16, .nbits = "12"},
  };
  for (size_t i = 0; i < std::size(kNbitsCases); ++i) {
    const std::string path = std::format("/vsimem/gw_nbits_{}.tif", i);
    SCOPED_TRACE(path);
    absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
    CPLStringList opts;
    opts.SetNameValue("NBITS", kNbitsCases[i].nbits);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds(gtiff_driver_->Create(
        path.c_str(), 8, 8, 1, kNbitsCases[i].dt, opts.List()));
    CPLErrorReset();
    ASSERT_NE(nullptr, ds);
  }

  // JXL compression validation and options.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    CPLStringList opts;
    opts.SetNameValue("COMPRESS", "JXL");
    GDALDatasetUniquePtr ds_dt(gtiff_driver_->Create(
        "/vsimem/gw_jxl_dt.tif", 8, 8, 1, GDT_Int16, opts.List()));
    EXPECT_EQ(nullptr, ds_dt);
    opts.SetNameValue("NBITS", "4");
    GDALDatasetUniquePtr ds_nb4(gtiff_driver_->Create(
        "/vsimem/gw_jxl_nb4.tif", 8, 8, 1, GDT_Byte, opts.List()));
    EXPECT_EQ(nullptr, ds_nb4);
    opts.SetNameValue("NBITS", "12");
    GDALDatasetUniquePtr ds_nb12(gtiff_driver_->Create(
        "/vsimem/gw_jxl_nb12.tif", 8, 8, 1, GDT_UInt16, opts.List()));
    EXPECT_EQ(nullptr, ds_nb12);
    CPLErrorReset();
  }
  {
    // Implicit and explicit JXL_LOSSLESS=YES warnings when JXL_DISTANCE /
    // JXL_ALPHA_DISTANCE are specified.
    constexpr char kWarn1Path[] = "/vsimem/gw_jxl_warn1.tif";
    constexpr char kWarn2Path[] = "/vsimem/gw_jxl_warn2.tif";
    constexpr char kLossyPath[] = "/vsimem/gw_jxl_lossy.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(kWarn1Path);
      VSIUnlink(kWarn2Path);
      VSIUnlink(kLossyPath);
    };
    CPLStringList opts;
    opts.SetNameValue("COMPRESS", "JXL");
    opts.SetNameValue("JXL_DISTANCE", "2.0");
    opts.SetNameValue("JXL_ALPHA_DISTANCE", "1.0");
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      GDALDatasetUniquePtr ds1(
          gtiff_driver_->Create(kWarn1Path, 8, 8, 2, GDT_Byte, opts.List()));
      EXPECT_NE(nullptr, ds1);
      opts.SetNameValue("JXL_LOSSLESS", "YES");
      GDALDatasetUniquePtr ds2(
          gtiff_driver_->Create(kWarn2Path, 8, 8, 2, GDT_Byte, opts.List()));
      EXPECT_NE(nullptr, ds2);
      CPLErrorReset();
    }

    opts.SetNameValue("COMPRESS", "JXL_DNG_1_7");
    opts.SetNameValue("JXL_LOSSLESS", "NO");
    opts.SetNameValue("JXL_EFFORT", "3");
    GDALDatasetUniquePtr ds3(
        gtiff_driver_->Create(kLossyPath, 8, 8, 4, GDT_Byte, opts.List()));
    ASSERT_NE(nullptr, ds3);
  }

  // PREDICTOR validation.
  {
    constexpr char kPredNonePath[] = "/vsimem/gw_pred_none.tif";
    constexpr char kPred3F32Path[] = "/vsimem/gw_pred3_f32.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(kPredNonePath);
      VSIUnlink(kPred3F32Path);
    };
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      CPLStringList opts;
      opts.SetNameValue("COMPRESS", "NONE");
      opts.SetNameValue("PREDICTOR", "2");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPredNonePath, 8, 8, 1, GDT_Byte, opts.List()));
      EXPECT_NE(nullptr, ds);
      CPLErrorReset();
    }
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      CPLStringList opts;
      opts.SetNameValue("COMPRESS", "DEFLATE");
      opts.SetNameValue("PREDICTOR", "2");
      opts.SetNameValue("NBITS", "4");
      GDALDatasetUniquePtr ds_b4(gtiff_driver_->Create(
          "/vsimem/gw_pred2_b4.tif", 8, 8, 1, GDT_Byte, opts.List()));
      EXPECT_EQ(nullptr, ds_b4);
      opts.SetNameValue("NBITS", "12");
      GDALDatasetUniquePtr ds_b12(gtiff_driver_->Create(
          "/vsimem/gw_pred2_b12.tif", 8, 8, 1, GDT_UInt16, opts.List()));
      EXPECT_EQ(nullptr, ds_b12);
      opts.SetNameValue("NBITS", "20");
      GDALDatasetUniquePtr ds_b20(gtiff_driver_->Create(
          "/vsimem/gw_pred2_b20.tif", 8, 8, 1, GDT_UInt32, opts.List()));
      EXPECT_EQ(nullptr, ds_b20);

      CPLStringList opts_p3;
      opts_p3.SetNameValue("COMPRESS", "DEFLATE");
      opts_p3.SetNameValue("PREDICTOR", "3");
      GDALDatasetUniquePtr ds_p3_byte(gtiff_driver_->Create(
          "/vsimem/gw_pred3_byte.tif", 8, 8, 1, GDT_Byte, opts_p3.List()));
      EXPECT_EQ(nullptr, ds_p3_byte);

      opts_p3.SetNameValue("PREDICTOR", "99");
      GDALDatasetUniquePtr ds_p99(gtiff_driver_->Create(
          "/vsimem/gw_pred99.tif", 8, 8, 1, GDT_Byte, opts_p3.List()));
      EXPECT_EQ(nullptr, ds_p99);
      CPLErrorReset();
    }
    {
      CPLStringList opts;
      opts.SetNameValue("COMPRESS", "DEFLATE");
      opts.SetNameValue("PREDICTOR", "3");
      GDALDatasetUniquePtr ds(gtiff_driver_->Create(kPred3F32Path, 8, 8, 1,
                                                    GDT_Float32, opts.List()));
      ASSERT_NE(nullptr, ds);
    }
  }

  // Codec preset/level option parsers (valid, capped, and out-of-range).
  {
    constexpr char kPath1[] = "/vsimem/gw_presets1.tif";
    constexpr char kPath2[] = "/vsimem/gw_presets2.tif";
    constexpr char kPath3[] = "/vsimem/gw_presets3.tif";
    constexpr char kPath4[] = "/vsimem/gw_presets4.tif";
    constexpr char kPath5[] = "/vsimem/gw_presets5.tif";
    constexpr char kPath6[] = "/vsimem/gw_presets6.tif";
    absl::Cleanup cl = [&] {
      for (const char* p : {kPath1, kPath2, kPath3, kPath4, kPath5, kPath6}) {
        VSIUnlink(p);
      }
    };
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      CPLStringList opts;
      opts.SetNameValue("COMPRESS", "WEBP");
      opts.SetNameValue("WEBP_LEVEL", "0");
      opts.SetNameValue("LZMA_PRESET", "99");
      opts.SetNameValue("ZSTD_LEVEL", "99");
      opts.SetNameValue("ZLEVEL", "99");
      opts.SetNameValue("JPEG_QUALITY", "105");
      GDALDatasetUniquePtr ds1(
          gtiff_driver_->Create(kPath1, 8, 8, 3, GDT_Byte, opts.List()));
      EXPECT_NE(nullptr, ds1);

      CPLStringList opts3;
      opts3.SetNameValue("COMPRESS", "DEFLATE");
      opts3.SetNameValue("ZLEVEL", "11");
      GDALDatasetUniquePtr ds3(
          gtiff_driver_->Create(kPath3, 8, 8, 1, GDT_Byte, opts3.List()));
      EXPECT_NE(nullptr, ds3);
      CPLErrorReset();
    }

    CPLStringList opts2;
    opts2.SetNameValue("COMPRESS", "WEBP");
    opts2.SetNameValue("WEBP_LEVEL", "50");
    opts2.SetNameValue("WEBP_LOSSLESS", "YES");
    GDALDatasetUniquePtr ds2(
        gtiff_driver_->Create(kPath2, 8, 8, 3, GDT_Byte, opts2.List()));
    ASSERT_NE(nullptr, ds2);

    CPLStringList opts4;
    opts4.SetNameValue("COMPRESS", "LZMA");
    opts4.SetNameValue("LZMA_PRESET", "3");
    GDALDatasetUniquePtr ds4(
        gtiff_driver_->Create(kPath4, 8, 8, 1, GDT_Byte, opts4.List()));
    ASSERT_NE(nullptr, ds4);

    CPLStringList opts5;
    opts5.SetNameValue("COMPRESS", "LERC_DEFLATE");
    opts5.SetNameValue("ZLEVEL", "5");
    opts5.SetNameValue("MAX_Z_ERROR", "0.5");
    opts5.SetNameValue("MAX_Z_ERROR_OVERVIEW", "1.0");
    GDALDatasetUniquePtr ds5(
        gtiff_driver_->Create(kPath5, 8, 8, 1, GDT_Float32, opts5.List()));
    ASSERT_NE(nullptr, ds5);

    CPLStringList opts6;
    opts6.SetNameValue("COMPRESS", "LERC_ZSTD");
    opts6.SetNameValue("ZSTD_LEVEL", "3");
    GDALDatasetUniquePtr ds6(
        gtiff_driver_->Create(kPath6, 8, 8, 1, GDT_Float32, opts6.List()));
    ASSERT_NE(nullptr, ds6);
  }

  // STREAMABLE_OUTPUT incompatibilities.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    CPLStringList opts_comp;
    opts_comp.SetNameValue("STREAMABLE_OUTPUT", "YES");
    opts_comp.SetNameValue("COMPRESS", "DEFLATE");
    GDALDatasetUniquePtr ds_comp(gtiff_driver_->Create(
        "/vsimem/gw_str_comp.tif", 8, 8, 1, GDT_Byte, opts_comp.List()));
    EXPECT_EQ(nullptr, ds_comp);

    CPLStringList opts_sp;
    opts_sp.SetNameValue("STREAMABLE_OUTPUT", "YES");
    opts_sp.SetNameValue("SPARSE_OK", "YES");
    GDALDatasetUniquePtr ds_sp(gtiff_driver_->Create(
        "/vsimem/gw_str_sp.tif", 8, 8, 1, GDT_Byte, opts_sp.List()));
    EXPECT_EQ(nullptr, ds_sp);

    CPLStringList opts_ovr;
    opts_ovr.SetNameValue("STREAMABLE_OUTPUT", "YES");
    opts_ovr.SetNameValue("COPY_SRC_OVERVIEWS", "YES");
    GDALDatasetUniquePtr ds_ovr(gtiff_driver_->Create(
        "/vsimem/gw_str_ovr.tif", 8, 8, 1, GDT_Byte, opts_ovr.List()));
    EXPECT_EQ(nullptr, ds_ovr);
    CPLErrorReset();
  }

  // BIGTIFF=NO > 4GB failure and tile array > 2GB failure (no allocation).
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    CPLStringList opts_big;
    opts_big.SetNameValue("BIGTIFF", "NO");
    GDALDatasetUniquePtr ds_big(
        gtiff_driver_->Create("/vsimem/gw_big_fail.tif", 100000, 50000, 1,
                              GDT_Byte, opts_big.List()));
    EXPECT_EQ(nullptr, ds_big);

    CPLStringList opts_tile;
    opts_tile.SetNameValue("TILED", "YES");
    opts_tile.SetNameValue("BLOCKXSIZE", "16");
    opts_tile.SetNameValue("BLOCKYSIZE", "16");
    opts_tile.SetNameValue("BIGTIFF", "YES");
    opts_tile.SetNameValue("COMPRESS", "DEFLATE");
    GDALDatasetUniquePtr ds_tile(
        gtiff_driver_->Create("/vsimem/gw_tile_2gb.tif", 400000, 400000, 1,
                              GDT_Byte, opts_tile.List()));
    EXPECT_EQ(nullptr, ds_tile);
    CPLErrorReset();
  }

  // ENDIANNESS, PIXELTYPE=SIGNEDBYTE, APPEND_SUBDATASET, @SUPPRESS_ASAP,
  // WRITE_EMPTY_TILES_SYNCHRONOUSLY.
  {
    constexpr char kAppendPath[] = "/vsimem/gw_append.tif";
    constexpr char kSuppressPath[] = "/vsimem/gw_suppress.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(kAppendPath);
      VSIUnlink(kSuppressPath);
    };
    CPLStringList opts;
    opts.SetNameValue("ENDIANNESS", "INVALID_ENDIAN");
    opts.SetNameValue("PIXELTYPE", "SIGNEDBYTE");
    opts.SetNameValue("WRITE_EMPTY_TILES_SYNCHRONOUSLY", "YES");
    opts.SetNameValue("@CREATE_ONLY_VISIBLE_AT_CLOSE_TIME", "YES");
    GDALDatasetUniquePtr ds;
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      ds.reset(
          gtiff_driver_->Create(kAppendPath, 8, 8, 1, GDT_Byte, opts.List()));
      CPLErrorReset();
    }
    ASSERT_NE(nullptr, ds);
    GByte v = 1;
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 1, 1, &v, 1, 1,
                                             GDT_Byte, 0, 0, nullptr));
    ds.reset();

    {
      WithQuietHandler quiet;
      CPLErrorReset();
      CPLStringList app_opts;
      app_opts.SetNameValue("APPEND_SUBDATASET", "YES");
      app_opts.SetNameValue("ENDIANNESS", "BIG");
      app_opts.SetNameValue("BIGTIFF", "NO");
      GDALDatasetUniquePtr ds2(gtiff_driver_->Create(
          kAppendPath, 8, 8, 1, GDT_Byte, app_opts.List()));
      CPLErrorReset();
      EXPECT_NE(nullptr, ds2);
    }

    CPLStringList sup_opts;
    sup_opts.SetNameValue("@SUPPRESS_ASAP", "YES");
    GDALDatasetUniquePtr ds3(gtiff_driver_->Create(kSuppressPath, 8, 8, 1,
                                                   GDT_Byte, sup_opts.List()));
    ASSERT_NE(nullptr, ds3);
  }

  // Sample formats across all types.
  constexpr GDALDataType kSampleFmtTypes[] = {
      GDT_Int8,    GDT_CInt16,   GDT_CInt32,   GDT_Float16,
      GDT_Float64, GDT_CFloat16, GDT_CFloat32, GDT_CFloat64};
  for (GDALDataType dt : kSampleFmtTypes) {
    SCOPED_TRACE(GDALGetDataTypeName(dt));
    constexpr char kPath[] = "/vsimem/gw_dt_samplefmt.tif";
    absl::Cleanup cl = [&] { VSIUnlink(kPath); };
    GDALDatasetUniquePtr ds(gtiff_driver_->Create(kPath, 4, 4, 1, dt, nullptr));
    ASSERT_NE(nullptr, ds);
  }

  // PHOTOMETRIC options & errors.
  {
    constexpr const char* kPhotos[] = {"MINISBLACK", "MINISWHITE",
                                       "CIELAB",     "ICCLAB",
                                       "ITULAB",     "INVALID_PHOTO"};
    for (const char* ph : kPhotos) {
      SCOPED_TRACE(ph);
      constexpr char kPath[] = "/vsimem/gw_ph.tif";
      absl::Cleanup cl = [&] { VSIUnlink(kPath); };
      CPLStringList opts;
      opts.SetNameValue("PHOTOMETRIC", ph);
      WithQuietHandler quiet;
      CPLErrorReset();
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPath, 8, 8, 3, GDT_Byte, opts.List()));
      CPLErrorReset();
      ASSERT_NE(nullptr, ds);
    }
    // PHOTOMETRIC=RGB with 1 band (warning) and 4/5 bands (extra samples).
    for (int bands : {1, 4, 5}) {
      SCOPED_TRACE(bands);
      constexpr char kPath[] = "/vsimem/gw_ph_rgb.tif";
      absl::Cleanup cl = [&] { VSIUnlink(kPath); };
      CPLStringList opts;
      opts.SetNameValue("PHOTOMETRIC", "RGB");
      GDALDatasetUniquePtr ds;
      if (bands == 1) {
        WithQuietHandler quiet;
        CPLErrorReset();
        ds.reset(
            gtiff_driver_->Create(kPath, 8, 8, bands, GDT_Byte, opts.List()));
        CPLErrorReset();
      } else {
        ds.reset(
            gtiff_driver_->Create(kPath, 8, 8, bands, GDT_Byte, opts.List()));
      }
      ASSERT_NE(nullptr, ds);
    }
    // PHOTOMETRIC=CMYK with 4 and 5 bands.
    for (int bands : {4, 5}) {
      SCOPED_TRACE(bands);
      constexpr char kPath[] = "/vsimem/gw_ph_cmyk.tif";
      absl::Cleanup cl = [&] { VSIUnlink(kPath); };
      CPLStringList opts;
      opts.SetNameValue("PHOTOMETRIC", "CMYK");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPath, 8, 8, bands, GDT_Byte, opts.List()));
      ASSERT_NE(nullptr, ds);
    }
    // PHOTOMETRIC=PALETTE on GDT_Byte, GDT_UInt16, and GDT_Int16 (warning).
    for (GDALDataType dt : {GDT_Byte, GDT_UInt16, GDT_Int16}) {
      SCOPED_TRACE(GDALGetDataTypeName(dt));
      constexpr char kPath[] = "/vsimem/gw_ph_pal.tif";
      absl::Cleanup cl = [&] { VSIUnlink(kPath); };
      CPLStringList opts;
      opts.SetNameValue("PHOTOMETRIC", "PALETTE");
      GDALDatasetUniquePtr ds;
      if (dt == GDT_Int16) {
        WithQuietHandler quiet;
        CPLErrorReset();
        ds.reset(gtiff_driver_->Create(kPath, 8, 8, 1, dt, opts.List()));
        CPLErrorReset();
      } else {
        ds.reset(gtiff_driver_->Create(kPath, 8, 8, 1, dt, opts.List()));
      }
      ASSERT_NE(nullptr, ds);
    }
    // PHOTOMETRIC=YCBCR error paths and valid path.
    {
      constexpr char kYcbcr1Path[] = "/vsimem/gw_ycbcr1.tif";
      constexpr char kYcbcr2Path[] = "/vsimem/gw_ycbcr2.tif";
      constexpr char kYcbcr3Path[] = "/vsimem/gw_ycbcr3.tif";
      constexpr char kOkPath[] = "/vsimem/gw_ycbcr_ok.tif";
      absl::Cleanup cl = [&] {
        for (const char* p : {kYcbcr1Path, kYcbcr2Path, kYcbcr3Path, kOkPath}) {
          VSIUnlink(p);
        }
      };
      WithQuietHandler quiet;
      CPLErrorReset();
      CPLStringList opts;
      opts.SetNameValue("PHOTOMETRIC", "YCBCR");
      GDALDatasetUniquePtr ds1(
          gtiff_driver_->Create(kYcbcr1Path, 16, 16, 3, GDT_Byte, opts.List()));
      EXPECT_EQ(nullptr, ds1);
      opts.SetNameValue("COMPRESS", "JPEG");
      opts.SetNameValue("INTERLEAVE", "BAND");
      GDALDatasetUniquePtr ds2(
          gtiff_driver_->Create(kYcbcr2Path, 16, 16, 3, GDT_Byte, opts.List()));
      EXPECT_EQ(nullptr, ds2);
      opts.SetNameValue("INTERLEAVE", "PIXEL");
      GDALDatasetUniquePtr ds3(
          gtiff_driver_->Create(kYcbcr3Path, 16, 16, 2, GDT_Byte, opts.List()));
      EXPECT_EQ(nullptr, ds3);
      CPLErrorReset();

      GDALDatasetUniquePtr ds_ok(
          gtiff_driver_->Create(kOkPath, 16, 16, 3, GDT_Byte, opts.List()));
      ASSERT_NE(nullptr, ds_ok);
    }
  }
}

// ---------------------------------------------------------------------------
// 2. FillEmptyTiles, HasOnlyNoData, and IsFirstPixelEqualToNoData
// ---------------------------------------------------------------------------
TEST_F(GTiffDatasetWriteTest, FillEmptyTilesAndSparseNoData) {
  // 2a. FillEmptyTiles with non-zero NoData on GDT_Byte, GDT_Int64, GDT_UInt64.
  for (int mode = 0; mode < 3; ++mode) {
    const std::string path = std::format("/vsimem/gw_fill_nd_{}.tif", mode);
    SCOPED_TRACE(path);
    absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
    CPLStringList opts;
    opts.SetNameValue("TILED", "YES");
    opts.SetNameValue("BLOCKXSIZE", "16");
    opts.SetNameValue("BLOCKYSIZE", "16");
    const GDALDataType dt =
        (mode == 0) ? GDT_Byte : (mode == 1 ? GDT_Int64 : GDT_UInt64);
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(path.c_str(), 32, 32, 1, dt, opts.List()));
    ASSERT_NE(nullptr, ds);
    if (mode == 0) {
      ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(42.0));
    } else if (mode == 1) {
      ASSERT_EQ(CE_None,
                ds->GetRasterBand(1)->SetNoDataValueAsInt64(-123456789LL));
    } else {
      ASSERT_EQ(CE_None,
                ds->GetRasterBand(1)->SetNoDataValueAsUInt64(123456789ULL));
    }
    // Write only pixel (0,0) in tile 0; leave tiles 1, 2, 3 unwritten so
    // FillEmptyTiles fills them at closing.
    uint64_t one = 1;
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 1, 1, &one, 1, 1,
                                             GDT_UInt64, 0, 0, nullptr));
    ds.reset();

    GDALDatasetUniquePtr ro(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ro);
    double val = 0.0;
    ASSERT_EQ(CE_None,
              ro->GetRasterBand(1)->RasterIO(GF_Read, 20, 20, 1, 1, &val, 1, 1,
                                             GDT_Float64, 0, 0, nullptr));
    if (mode == 0) {
      EXPECT_DOUBLE_EQ(42.0, val);
    } else if (mode == 1) {
      EXPECT_DOUBLE_EQ(-123456789.0, val);
    } else {
      EXPECT_DOUBLE_EQ(123456789.0, val);
    }
  }

  // 2b. FillEmptyTiles with non-power-of-two depth (NBITS=4) and non-zero
  // NoData: both 1-band and 2-band CONTIG with partial edge tiles (20x20).
  for (int bands : {1, 2}) {
    const std::string path =
        std::format("/vsimem/gw_fill_oddbits_{}.tif", bands);
    SCOPED_TRACE(path);
    absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
    CPLStringList opts;
    opts.SetNameValue("TILED", "YES");
    opts.SetNameValue("BLOCKXSIZE", "16");
    opts.SetNameValue("BLOCKYSIZE", "16");
    opts.SetNameValue("NBITS", "4");
    opts.SetNameValue("INTERLEAVE", "PIXEL");
    GDALDatasetUniquePtr ds(gtiff_driver_->Create(path.c_str(), 20, 20, bands,
                                                  GDT_Byte, opts.List()));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(7.0));
    GByte px = 3;
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 1, 1, &px, 1, 1,
                                             GDT_Byte, 0, 0, nullptr));
    ds.reset();

    GDALDatasetUniquePtr ro(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ro);
    GByte out = 0;
    ASSERT_EQ(CE_None,
              ro->GetRasterBand(1)->RasterIO(GF_Read, 18, 18, 1, 1, &out, 1, 1,
                                             GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(7, out);
  }

  // 2c. FillEmptyTiles with COMPRESS=NONE and 0/unset NoData (tiled & stripped,
  // >= 4 blocks with only block 0 written -> seeks to EOF and truncates).
  for (bool tiled : {true, false}) {
    const std::string path =
        std::format("/vsimem/gw_fill_zero_{}.tif", tiled ? 1 : 0);
    SCOPED_TRACE(path);
    absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
    CPLStringList opts;
    opts.SetNameValue("TILED", tiled ? "YES" : "NO");
    if (tiled) {
      opts.SetNameValue("BLOCKXSIZE", "16");
      opts.SetNameValue("BLOCKYSIZE", "16");
    } else {
      opts.SetNameValue("BLOCKYSIZE", "8");
    }
    opts.SetNameValue("COMPRESS", "NONE");
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(path.c_str(), 32, 32, 1, GDT_Byte, opts.List()));
    ASSERT_NE(nullptr, ds);
    GByte px = 9;
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 1, 1, &px, 1, 1,
                                             GDT_Byte, 0, 0, nullptr));
    ds.reset();
  }

  // 2d. FillEmptyTiles with COMPRESS=DEFLATE and 0/unset NoData (>= 4 tiles,
  // only tile 0 written -> compresses 1 empty tile and reuses raw bytes via
  // WriteRawStripOrTile).
  {
    constexpr char kPath[] = "/vsimem/gw_fill_deflate.tif";
    absl::Cleanup cl = [&] { VSIUnlink(kPath); };
    CPLStringList opts;
    opts.SetNameValue("TILED", "YES");
    opts.SetNameValue("BLOCKXSIZE", "16");
    opts.SetNameValue("BLOCKYSIZE", "16");
    opts.SetNameValue("COMPRESS", "DEFLATE");
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kPath, 32, 32, 1, GDT_Byte, opts.List()));
    ASSERT_NE(nullptr, ds);
    GByte px = 9;
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 1, 1, &px, 1, 1,
                                             GDT_Byte, 0, 0, nullptr));
    ds.reset();
  }

  // 2e. HasOnlyNoData & IsFirstPixelEqualToNoData with SPARSE_OK=YES across
  // data types (tiled & stripped, including NaN NoData and Int64/UInt64).
  constexpr GDALDataType kSparseTypes[] = {
      GDT_Byte,    GDT_Int8,    GDT_UInt16, GDT_Int16,
      GDT_UInt32,  GDT_Int32,   GDT_UInt64, GDT_Int64,
      GDT_Float32, GDT_Float64, GDT_CInt16, GDT_CFloat32};
  for (bool tiled : {true, false}) {
    for (GDALDataType dt : kSparseTypes) {
      const int max_modes = (dt == GDT_Float32 || dt == GDT_Float64 ||
                             dt == GDT_Int64 || dt == GDT_UInt64)
                                ? 2
                                : 1;
      for (int nan_mode = 0; nan_mode < max_modes; ++nan_mode) {
        const std::string path =
            std::format("/vsimem/gw_sparse_{}_{}_{}.tif", tiled ? 1 : 0,
                        static_cast<int>(dt), nan_mode);
        SCOPED_TRACE(path);
        absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
        CPLStringList opts;
        opts.SetNameValue("SPARSE_OK", "YES");
        opts.SetNameValue("TILED", tiled ? "YES" : "NO");
        if (tiled) {
          opts.SetNameValue("BLOCKXSIZE", "16");
          opts.SetNameValue("BLOCKYSIZE", "16");
        } else {
          opts.SetNameValue("BLOCKYSIZE", "8");
        }
        GDALDatasetUniquePtr ds(
            gtiff_driver_->Create(path.c_str(), 16, 16, 1, dt, opts.List()));
        ASSERT_NE(nullptr, ds);

        std::vector<double> buf(16 * 16, 0.0);
        if ((dt == GDT_Float32 || dt == GDT_Float64) && nan_mode == 1) {
          const double nan_val = std::numeric_limits<double>::quiet_NaN();
          ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(nan_val));
          std::fill(buf.begin(), buf.end(), nan_val);
        } else if (dt == GDT_Int64 && nan_mode == 1) {
          ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValueAsInt64(-50));
          std::fill(buf.begin(), buf.end(), -50.0);
        } else if (dt == GDT_UInt64 && nan_mode == 1) {
          ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValueAsUInt64(50));
          std::fill(buf.begin(), buf.end(), 50.0);
        } else {
          ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(5.0));
          std::fill(buf.begin(), buf.end(), 5.0);
        }

        ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                               GF_Write, 0, 0, 16, 16, buf.data(), 16, 16,
                               GDT_Float64, 0, 0, nullptr));
        ds->FlushCache(false);
        ds.reset();

        // Query BLOCK_OFFSET_0_0 on band 1 (where block metadata is exposed).
        // HasOnlyNoData returns false for complex sample formats and when
        // NoData was set via SetNoDataValueAsInt64/SetNoDataValueAsUInt64
        // (nan_mode == 1); otherwise the all-nodata block is elided.
        GDALDatasetUniquePtr rd(
            GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
        ASSERT_NE(nullptr, rd);
        const bool must_materialize =
            GDALDataTypeIsComplex(dt) || dt == GDT_Int64 || dt == GDT_UInt64;
        const char* off =
            rd->GetRasterBand(1)->GetMetadataItem("BLOCK_OFFSET_0_0", "TIFF");
        if (must_materialize) {
          EXPECT_NE(nullptr, off);
        } else {
          EXPECT_EQ(nullptr, off);
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 3. WriteDealWithLercAndNan, WriteEncodedTile, WriteEncodedStrip, Streaming
// ---------------------------------------------------------------------------
TEST_F(GTiffDatasetWriteTest, WriteEncodedTileAndStripEdgeCases) {
  // 3a. WriteDealWithLercAndNan: partial tiles (20x20 with 16x16 tiles) and
  // strips (20x20 with 8-row strips) on GDT_Float32 & GDT_Float64, 2-band
  // INTERLEAVE=PIXEL with NaN pixels.
  for (bool tiled : {true, false}) {
    for (GDALDataType dt : {GDT_Float32, GDT_Float64}) {
      const std::string path = std::format("/vsimem/gw_lerc_nan_{}_{}.tif",
                                           tiled ? 1 : 0, static_cast<int>(dt));
      SCOPED_TRACE(path);
      absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
      CPLStringList opts;
      opts.SetNameValue("COMPRESS", "LERC");
      opts.SetNameValue("INTERLEAVE", "PIXEL");
      opts.SetNameValue("TILED", tiled ? "YES" : "NO");
      if (tiled) {
        opts.SetNameValue("BLOCKXSIZE", "16");
        opts.SetNameValue("BLOCKYSIZE", "16");
      } else {
        opts.SetNameValue("BLOCKYSIZE", "8");
      }
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(path.c_str(), 20, 20, 2, dt, opts.List()));
      ASSERT_NE(nullptr, ds);
      std::vector<double> buf(20 * 20, 1.5);
      buf[0] = std::numeric_limits<double>::quiet_NaN();
      for (int b = 1; b <= 2; ++b) {
        ASSERT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                               GF_Write, 0, 0, 20, 20, buf.data(), 20, 20,
                               GDT_Float64, 0, 0, nullptr));
      }
      WithQuietHandler quiet;
      CPLErrorReset();
      ds.reset();
      CPLErrorReset();
    }
  }

  // 3b. Partial JPEG 8-bit edge tile padding (20x20 with 16x16 tiles) +
  // ENDIANNESS=INVERTED byte-swapped temp buffer in WriteEncodedTile/Strip.
  {
    constexpr char kPath[] = "/vsimem/gw_jpeg_partial.tif";
    absl::Cleanup cl = [&] { VSIUnlink(kPath); };
    CPLStringList opts;
    opts.SetNameValue("COMPRESS", "JPEG");
    opts.SetNameValue("TILED", "YES");
    opts.SetNameValue("BLOCKXSIZE", "16");
    opts.SetNameValue("BLOCKYSIZE", "16");
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kPath, 20, 20, 3, GDT_Byte, opts.List()));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> buf(20 * 20 * 3, 120);
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Write, 0, 0, 20, 20, buf.data(), 20, 20,
                                    GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    ds.reset();
  }
  for (bool tiled : {true, false}) {
    const std::string path =
        std::format("/vsimem/gw_inv_endian_{}.tif", tiled ? 1 : 0);
    SCOPED_TRACE(path);
    absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
    CPLStringList opts;
    opts.SetNameValue("ENDIANNESS", "INVERTED");
    opts.SetNameValue("TILED", tiled ? "YES" : "NO");
    if (tiled) {
      opts.SetNameValue("BLOCKXSIZE", "16");
      opts.SetNameValue("BLOCKYSIZE", "16");
    } else {
      opts.SetNameValue("BLOCKYSIZE", "8");
    }
    GDALDatasetUniquePtr ds(gtiff_driver_->Create(path.c_str(), 20, 20, 1,
                                                  GDT_UInt16, opts.List()));
    ASSERT_NE(nullptr, ds);
    std::vector<uint16_t> buf(20 * 20, 0x1234);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 20, 20, buf.data(), 20, 20,
                           GDT_UInt16, 0, 0, nullptr));
    ds.reset();
  }

  // 3c. STREAMABLE_OUTPUT=YES (tiled & stripped): sequential write success,
  // out-of-order block write failure, and post-crystalize SetSpatialRef /
  // SetGeoTransform / SetMetadata / SetMetadataItem errors.
  for (bool tiled : {true, false}) {
    const std::string path =
        std::format("/vsimem/gw_stream_out_{}.tif", tiled ? 1 : 0);
    SCOPED_TRACE(path);
    absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
    CPLStringList opts;
    opts.SetNameValue("STREAMABLE_OUTPUT", "YES");
    opts.SetNameValue("TILED", tiled ? "YES" : "NO");
    if (tiled) {
      opts.SetNameValue("BLOCKXSIZE", "16");
      opts.SetNameValue("BLOCKYSIZE", "16");
    } else {
      opts.SetNameValue("BLOCKYSIZE", "8");
    }
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(path.c_str(), 32, 20, 1, GDT_Byte, opts.List()));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> block(32 * 16, 77);
    // Out-of-order write of block (0, 1) before (0, 0) fails.
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_NE(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 1, block.data()));
      CPLErrorReset();
    }
    // Now write block 0 so it is crystalized, then verify setters fail on
    // streamed output after crystalize.
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, block.data()));
    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    GDALGeoTransform gt{0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, ds->SetSpatialRef(&srs));
      EXPECT_EQ(CE_Failure, ds->SetGeoTransform(gt));
      EXPECT_EQ(CE_Failure, ds->SetMetadata(nullptr));
      EXPECT_EQ(CE_Failure, ds->SetMetadataItem("FOO", "BAR"));
      CPLErrorReset();
    }
    ds.reset();
  }
}

// ---------------------------------------------------------------------------
// 4. Multi-Threaded Compression & COG WriteRawStripOrTile In-Place / Break
// ---------------------------------------------------------------------------
TEST_F(GTiffDatasetWriteTest, MultiThreadedCompressionAndCOGWriteRaw) {
  // 4a. NUM_THREADS invalid warnings and multi-threaded compression with
  // DEFLATE+PREDICTOR+EXTRASAMPLES and LERC, plus WaitCompletionForBlock and
  // uncompressed overview flushing parent queue.
  {
    constexpr char kBadPath[] = "/vsimem/gw_mt_bad.tif";
    absl::Cleanup cl = [&] { VSIUnlink(kBadPath); };
    CPLStringList bad_opts;
    bad_opts.SetNameValue("NUM_THREADS", "INVALID_THREADS");
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds_bad(
        gtiff_driver_->Create(kBadPath, 32, 32, 1, GDT_Byte, bad_opts.List()));
    CPLErrorReset();
    ASSERT_NE(nullptr, ds_bad);
  }
  for (int codec = 0; codec < 2; ++codec) {
    const std::string path = std::format("/vsimem/gw_mt_{}.tif", codec);
    SCOPED_TRACE(path);
    absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
    CPLStringList opts;
    opts.SetNameValue("TILED", "YES");
    opts.SetNameValue("BLOCKXSIZE", "16");
    opts.SetNameValue("BLOCKYSIZE", "16");
#if defined(THREAD_SANITIZER) || defined(__SANITIZE_THREAD__)
    // Upstream GTiffDataset::FlushCacheInternal() clears bit-field
    // m_bLoadedBlockDirty before WaitCompletion(), racing with worker threads
    // reading sibling bit-fields in RestoreVolatileParameters()
    // (gtiffdataset.h:256, gtiffdataset_write.cpp:2188).
    opts.SetNameValue("NUM_THREADS", "1");
#else
    opts.SetNameValue("NUM_THREADS", "2");
#endif
    if (codec == 0) {
      opts.SetNameValue("COMPRESS", "DEFLATE");
      opts.SetNameValue("PREDICTOR", "2");
      opts.SetNameValue("ENDIANNESS", "BIG");
    } else {
      opts.SetNameValue("COMPRESS", "LERC");
    }
    GDALDatasetUniquePtr ds;
    if (codec == 0) {
      WithQuietHandler quiet;
      CPLErrorReset();
      ds.reset(gtiff_driver_->Create(path.c_str(), 64, 64, 4, GDT_Byte,
                                     opts.List()));
      CPLErrorReset();
    } else {
      ds.reset(gtiff_driver_->Create(path.c_str(), 64, 64, 4, GDT_Byte,
                                     opts.List()));
    }
    ASSERT_NE(nullptr, ds);

    // Also create an uncompressed overview so writing to the overview while
    // base DS has a compression queue exercises the m_nCompression == NONE
    // WaitCompletion branch in SubmitCompressionJob.
    int ovr_level = 2;
    CPLStringList ovr_opts;
    ovr_opts.SetNameValue("COMPRESS_OVERVIEW", "NONE");
    ASSERT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, &ovr_level, 0, nullptr,
                                          nullptr, nullptr, ovr_opts.List()));

    std::vector<GByte> tile(16 * 16 * 4, 42);
    for (int ty = 0; ty < 4; ++ty) {
      for (int tx = 0; tx < 4; ++tx) {
        tile[0] = static_cast<GByte>(ty * 4 + tx);
        ASSERT_EQ(CE_None,
                  ds->RasterIO(GF_Write, tx * 16, ty * 16, 16, 16, tile.data(),
                               16, 16, GDT_Byte, 4, nullptr, 0, 0, 0, nullptr));
      }
    }
    // Trigger WaitCompletionForBlock before FlushCache by querying block
    // coverage status and writing to the uncompressed overview.
    (void)ds->GetRasterBand(1)->GetDataCoverageStatus(0, 0, 64, 64, 0, nullptr);
    GDALRasterBand* ovr_band = ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(nullptr, ovr_band);
    int ovr_bx = 0;
    int ovr_by = 0;
    ovr_band->GetBlockSize(&ovr_bx, &ovr_by);
    // The overview block may be larger than the base tile; size the buffer
    // to the overview's actual block size.
    std::vector<GByte> ovr_block(static_cast<size_t>(ovr_bx) * ovr_by, 42);
    ASSERT_EQ(CE_None, ovr_band->WriteBlock(0, 0, ovr_block.data()));
    ds->FlushCache(false);
  }

  // 4b. COG in-place vs out-of-place tile rewrite (BLOCK_ORDER and
  // MASK_INTERLEAVED_WITH_IMAGERY invalidation + leader/trailer handling).
  {
    CPLConfigOptionSetter int_msk("GDAL_TIFF_INTERNAL_MASK", "YES", false);
    for (bool with_mask : {false, true}) {
      const std::string path =
          std::format("/vsimem/gw_cog_edit_{}.tif", with_mask ? 1 : 0);
      SCOPED_TRACE(path);
      absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };

      GDALDatasetUniquePtr src(
          mem_driver_->Create("", 32, 32, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, src);
      std::vector<GByte> initial(32 * 32);
      for (size_t i = 0; i < initial.size(); ++i) {
        initial[i] = static_cast<GByte>((i * 7) % 13);
      }
      ASSERT_EQ(CE_None, src->GetRasterBand(1)->RasterIO(
                             GF_Write, 0, 0, 32, 32, initial.data(), 32, 32,
                             GDT_Byte, 0, 0, nullptr));
      if (with_mask) {
        ASSERT_EQ(CE_None, src->CreateMaskBand(GMF_PER_DATASET));
        ASSERT_EQ(CE_None, src->GetRasterBand(1)->GetMaskBand()->RasterIO(
                               GF_Write, 0, 0, 32, 32, initial.data(), 32, 32,
                               GDT_Byte, 0, 0, nullptr));
      }
      int ovr = 2;
      ASSERT_EQ(CE_None, src->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr,
                                             nullptr, nullptr));

      CPLStringList copy_opts;
      copy_opts.SetNameValue("TILED", "YES");
      copy_opts.SetNameValue("BLOCKXSIZE", "16");
      copy_opts.SetNameValue("BLOCKYSIZE", "16");
      copy_opts.SetNameValue("COPY_SRC_OVERVIEWS", "YES");
      copy_opts.SetNameValue("COMPRESS", "DEFLATE");
      GDALDatasetUniquePtr cog(gtiff_driver_->CreateCopy(
          path.c_str(), src.get(), FALSE, copy_opts.List(), nullptr, nullptr));
      ASSERT_NE(nullptr, cog);
      cog.reset();

      // Reopen in GA_Update with IGNORE_COG_LAYOUT_BREAK=YES.
      const CPLStringList open_opts{"IGNORE_COG_LAYOUT_BREAK=YES"};
      GDALDatasetUniquePtr upd(
          GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE,
                            nullptr, open_opts.List()));
      ASSERT_NE(nullptr, upd);
      if (with_mask) {
        // Trigger lazy opening of m_poMaskDS before writing tile (0, 0).
        EXPECT_EQ(GMF_PER_DATASET, upd->GetRasterBand(1)->GetMaskFlags());
      }

      WithQuietHandler quiet;
      CPLErrorReset();
      // 1) Write all-zeros tile (smaller compressed size than original).
      // Without mask: rewrites in place with leader + trailer.
      // With mask: size != panByteCounts -> invalidates
      // MASK_INTERLEAVED_WITH_IMAGERY and zeros out the 4-byte leader.
      std::vector<GByte> zeros(16 * 16, 0);
      ASSERT_EQ(CE_None, upd->GetRasterBand(1)->WriteBlock(0, 0, zeros.data()));
      upd->FlushCache(false);
      if (with_mask) {
        EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
        EXPECT_THAT(CPLGetLastErrorMsg(),
                    HasSubstr("invalidates the MASK_INTERLEAVED_WITH_IMAGERY"));
        CPLErrorReset();
      }

      // 2) Write high-entropy tile (larger compressed size than original) ->
      // invalidates BLOCK_ORDER optimization and appends at EOF.
      std::vector<GByte> random_tile(16 * 16);
      uint32_t seed = 123456789U;
      for (size_t i = 0; i < random_tile.size(); ++i) {
        seed = seed * 1664525U + 1013904223U;
        random_tile[i] = static_cast<GByte>((seed >> 16) & 0xFF);
      }
      ASSERT_EQ(CE_None,
                upd->GetRasterBand(1)->WriteBlock(1, 0, random_tile.data()));
      upd->FlushCache(false);
      CPLErrorReset();
    }
  }
}

// ---------------------------------------------------------------------------
// 5. DiscardLsb & GetDiscardLsbOption Across All 11 Data Types
// ---------------------------------------------------------------------------
TEST_F(GTiffDatasetWriteTest, DiscardLsbAllTypesAndWarnings) {
  // 5a. GetDiscardLsbOption warning branches.
  {
    constexpr char kPalPath[] = "/vsimem/gw_dlsb_pal.tif";
    constexpr char kNb4Path[] = "/vsimem/gw_dlsb_nb4.tif";
    constexpr char kTokPath[] = "/vsimem/gw_dlsb_tok.tif";
    constexpr char kRngPath[] = "/vsimem/gw_dlsb_rng.tif";
    absl::Cleanup cl = [&] {
      for (const char* p : {kPalPath, kNb4Path, kTokPath, kRngPath}) {
        VSIUnlink(p);
      }
    };
    WithQuietHandler quiet;
    CPLErrorReset();

    CPLStringList opts1;
    opts1.SetNameValue("PHOTOMETRIC", "PALETTE");
    opts1.SetNameValue("DISCARD_LSB", "2");
    GDALDatasetUniquePtr ds1(
        gtiff_driver_->Create(kPalPath, 8, 8, 1, GDT_Byte, opts1.List()));
    ASSERT_NE(nullptr, ds1);

    CPLStringList opts2;
    opts2.SetNameValue("NBITS", "4");
    opts2.SetNameValue("DISCARD_LSB", "2");
    GDALDatasetUniquePtr ds2(
        gtiff_driver_->Create(kNb4Path, 8, 8, 1, GDT_Byte, opts2.List()));
    ASSERT_NE(nullptr, ds2);

    CPLStringList opts3;
    opts3.SetNameValue("DISCARD_LSB", "1,2,3");
    GDALDatasetUniquePtr ds3(
        gtiff_driver_->Create(kTokPath, 8, 8, 2, GDT_Byte, opts3.List()));
    ASSERT_NE(nullptr, ds3);

    CPLStringList opts4;
    opts4.SetNameValue("DISCARD_LSB", "8");
    GDALDatasetUniquePtr ds4(
        gtiff_driver_->Create(kRngPath, 8, 8, 1, GDT_Byte, opts4.List()));
    ASSERT_NE(nullptr, ds4);
    CPLErrorReset();
  }

  // 5b. Functional DiscardLsb on all 11 supported types in both
  // INTERLEAVE=PIXEL and INTERLEAVE=BAND, exercising exact nodata, rounding
  // onto nodata (AdjustValue), max-value clamping, negative signed values, 255
  // on Byte, NaN/Inf on floats, and out-of-range nodata values.
  constexpr GDALDataType kDiscardLsbTypes[] = {
      GDT_Byte,  GDT_Int8,   GDT_Int16,   GDT_UInt16,  GDT_Int32,  GDT_UInt32,
      GDT_Int64, GDT_UInt64, GDT_Float16, GDT_Float32, GDT_Float64};

  for (GDALDataType dt : kDiscardLsbTypes) {
    for (const char* interleave : {"PIXEL", "BAND"}) {
      for (int nd_mode = 0; nd_mode < 3; ++nd_mode) {
        const std::string path =
            std::format("/vsimem/gw_dlsb_{}_{}_{}.tif", static_cast<int>(dt),
                        interleave, nd_mode);
        absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
        CPLStringList opts;
        opts.SetNameValue("INTERLEAVE", interleave);
        opts.SetNameValue("DISCARD_LSB", "2,2");
        opts.SetNameValue("COMPRESS", "DEFLATE");
        GDALDatasetUniquePtr ds(
            gtiff_driver_->Create(path.c_str(), 16, 1, 2, dt, opts.List()));
        ASSERT_NE(nullptr, ds);

        if (nd_mode == 0) {
          // NoData = 8.0: values 7 and 9 round/mask to 8.0 and trigger
          // AdjustValue(8, 2) -> 12.
          ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(8.0));
        } else if (nd_mode == 1) {
          // NoData near max so AdjustValueInt hits the >= max - (round << 1)
          // subtraction branch.
          double max_nd = 124.0;
          if (dt == GDT_Byte) {
            max_nd = 252.0;
          } else if (dt == GDT_Int8) {
            max_nd = 124.0;
          } else if (dt == GDT_Int16) {
            max_nd = 32764.0;
          } else if (dt == GDT_UInt16) {
            max_nd = 65532.0;
          } else if (dt == GDT_Int32) {
            max_nd = 2147483644.0;
          } else if (dt == GDT_UInt32) {
            max_nd = 4294967292.0;
          }
          ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(max_nd));
        } else {
          // Out-of-range NoData for integer types (triggers bHasNoData =
          // false with a warning).
          WithQuietHandler quiet;
          CPLErrorReset();
          ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(1e100));
          CPLErrorReset();
        }

        double vals[16] = {
            0.0,
            8.0,   // exact nodata
            9.0,   // masks to 8 -> AdjustValue
            7.0,   // rounds up to 8 -> AdjustValue
            10.0,  // rounds up to 12
            11.0,  // rounds up to 12
            -7.0,  // negative signed with roundup bit set
            -8.0,  // negative signed without roundup bit set
            124.0,
            125.0,
            126.0,  // near int8 max (127)
            252.0,
            253.0,
            255.0,  // Byte 255 alpha preservation
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::infinity(),
        };
        if (dt == GDT_Int16) {
          vals[8] = 32764.0;
          vals[9] = 32765.0;
          vals[10] = 32767.0;
        } else if (dt == GDT_UInt16) {
          vals[8] = 65532.0;
          vals[9] = 65533.0;
          vals[10] = 65535.0;
        } else if (dt == GDT_Int32) {
          vals[8] = 2147483644.0;
          vals[9] = 2147483645.0;
          vals[10] = 2147483647.0;
        } else if (dt == GDT_UInt32) {
          vals[8] = 4294967292.0;
          vals[9] = 4294967293.0;
          vals[10] = 4294967295.0;
        }
        for (int b = 1; b <= 2; ++b) {
          ASSERT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                                 GF_Write, 0, 0, 16, 1, vals, 16, 1,
                                 GDT_Float64, 0, 0, nullptr));
        }
        if (dt == GDT_Int64) {
          int64_t i64_vals[16] = {
              0,
              8,
              9,
              7,
              -7,
              -8,
              std::numeric_limits<int64_t>::max() - 3,
              std::numeric_limits<int64_t>::max() - 2,
              std::numeric_limits<int64_t>::max(),
              10,
              11,
              12,
              13,
              14,
              15,
              16,
          };
          for (int b = 1; b <= 2; ++b) {
            ASSERT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                                   GF_Write, 0, 0, 16, 1, i64_vals, 16, 1,
                                   GDT_Int64, 0, 0, nullptr));
          }
        } else if (dt == GDT_UInt64) {
          uint64_t u64_vals[16] = {
              0,
              8,
              9,
              7,
              10,
              11,
              std::numeric_limits<uint64_t>::max() - 3,
              std::numeric_limits<uint64_t>::max() - 2,
              std::numeric_limits<uint64_t>::max(),
              12,
              13,
              14,
              15,
              16,
              17,
              18,
          };
          for (int b = 1; b <= 2; ++b) {
            ASSERT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                                   GF_Write, 0, 0, 16, 1, u64_vals, 16, 1,
                                   GDT_UInt64, 0, 0, nullptr));
          }
        }
        ds->FlushCache(false);
        ds.reset();

        // Read back and verify the DISCARD_LSB=2 semantics (mask ~3, round-up
        // bit 2): 0 -> 0; exact nodata is preserved; 9 masks to 8 and 7 rounds
        // up to 8, which collide with nodata=8 (nd_mode 0) and are pushed to
        // 12 by AdjustValue; 10 and 11 round up to 12. Floats only lose
        // mantissa LSBs, so small integers survive unchanged and NaN/Inf are
        // left untouched.
        GDALDatasetUniquePtr rd(
            GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
        ASSERT_NE(nullptr, rd);
        const bool is_float = GDALDataTypeIsFloating(dt);
        const bool is_signed = GDALDataTypeIsSigned(dt);
        const double collide = (nd_mode == 0) ? 12.0 : 8.0;
        for (int b = 1; b <= 2; ++b) {
          SCOPED_TRACE(std::format("dt={} interleave={} nd_mode={} band={}",
                                   GDALGetDataTypeName(dt), interleave, nd_mode,
                                   b));
          double got[16] = {};
          ASSERT_EQ(CE_None, rd->GetRasterBand(b)->RasterIO(
                                 GF_Read, 0, 0, 16, 1, got, 16, 1, GDT_Float64,
                                 0, 0, nullptr));
          EXPECT_EQ(0.0, got[0]);
          EXPECT_EQ(8.0, got[1]);
          if (is_float) {
            EXPECT_EQ(9.0, got[2]);
            EXPECT_EQ(7.0, got[3]);
            EXPECT_EQ(10.0, got[4]);
            EXPECT_EQ(11.0, got[5]);
            EXPECT_TRUE(std::isnan(got[14]));
            EXPECT_TRUE(std::isinf(got[15]));
          } else {
            EXPECT_EQ(collide, got[2]);
            EXPECT_EQ(collide, got[3]);
            if (dt == GDT_Int64) {
              // i64_vals[4..5] = {-7, -8}: negative values are only masked.
              EXPECT_EQ(-8.0, got[4]);
              EXPECT_EQ(-8.0, got[5]);
            } else {
              EXPECT_EQ(12.0, got[4]);
              EXPECT_EQ(12.0, got[5]);
              if (dt != GDT_UInt64) {
                // vals[6..7] = {-7, -8}: masked for signed types, clamped to
                // 0 on write for unsigned types.
                EXPECT_EQ(is_signed ? -8.0 : 0.0, got[6]);
                EXPECT_EQ(is_signed ? -8.0 : 0.0, got[7]);
              }
            }
          }
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 6. FlushDirectory, Overviews (IBuildOverviews, CleanOverviews, AddOverviews)
// ---------------------------------------------------------------------------
TEST_F(GTiffDatasetWriteTest, FlushDirectoryAndOverviews) {
  // 6a. FlushDirectory: ReloadAllOtherDirectories (base + mask + overview +
  // overview mask), unsetting RPC, and DeleteNoDataValue (UnsetNoDataValue).
  {
    constexpr char kPath[] = "/vsimem/gw_flush_dir.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(kPath);
      VSIUnlink("/vsimem/gw_flush_dir.RPB");
      VSIUnlink("/vsimem/gw_flush_dir_RPC.TXT");
    };
    CPLStringList opts;
    opts.SetNameValue("TILED", "YES");
    opts.SetNameValue("BLOCKXSIZE", "16");
    opts.SetNameValue("BLOCKYSIZE", "16");
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kPath, 32, 32, 1, GDT_Byte, opts.List()));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(99.0));
    ASSERT_EQ(CE_None, ds->CreateMaskBand(GMF_PER_DATASET));
    int ovr = 2;
    ASSERT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr,
                                          nullptr, nullptr));

    // Set RPC metadata, crystalize/flush, then unset RPC and NoData.
    CPLStringList rpc_md{
        "LINE_OFF=0",
        "LINE_SCALE=1",
        "SAMP_OFF=0",
        "SAMP_SCALE=1",
        "LAT_OFF=0",
        "LAT_SCALE=1",
        "LONG_OFF=0",
        "LONG_SCALE=1",
        "HEIGHT_OFF=0",
        "HEIGHT_SCALE=1",
        "LINE_NUM_COEFF=1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0",
        "LINE_DEN_COEFF=1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0",
        "SAMP_NUM_COEFF=1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0",
        "SAMP_DEN_COEFF=1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0"};
    ASSERT_EQ(CE_None, ds->SetMetadata(rpc_md.List(), "RPC"));
    GByte v = 5;
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 1, 1, &v, 1, 1,
                                             GDT_Byte, 0, 0, nullptr));
    ds->FlushCache(false);

    // Unset RPC and NoData, forcing TIFFRewriteDirectory &
    // ReloadAllOtherDirectories.
    ASSERT_EQ(CE_None, ds->SetMetadata(nullptr, "RPC"));
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->DeleteNoDataValue());
    ds->FlushCache(false);

    // Also modify overview metadata so ReloadAllOtherDirectories runs when
    // flushing the overview (poBaseDS != this).
    GDALRasterBand* ovr_band = ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(nullptr, ovr_band);
    GDALDataset* ovr_ds = ovr_band->GetDataset();
    ASSERT_NE(nullptr, ovr_ds);
    ASSERT_EQ(CE_None, ovr_ds->SetMetadataItem("OVR_KEY", "OVR_VAL"));
    ovr_ds->FlushCache(false);

    // CleanOverviews when overviews & mask overviews exist.
    ASSERT_EQ(CE_None, ds->BuildOverviews("NONE", 0, nullptr, 0, nullptr,
                                          nullptr, nullptr));
    // CleanOverviews when 0 overviews exist.
    ASSERT_EQ(CE_None, ds->BuildOverviews("NONE", 0, nullptr, 0, nullptr,
                                          nullptr, nullptr));
  }

  // 6b. IBuildOverviews: options, duplicate 1x1 skip, resampling update,
  // SEPARATE band-by-band path, AVERAGE_BIT2GRAYSCALE, palette (8/4/16-bit),
  // external mask (.msk), and error/external paths.
  {
    // Palette 8-bit, 4-bit, and 16-bit overview building.
    for (int depth : {8, 4, 16}) {
      const std::string path = std::format("/vsimem/gw_ovr_pal_{}.tif", depth);
      SCOPED_TRACE(path);
      absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
      CPLStringList opts;
      if (depth == 4) opts.SetNameValue("NBITS", "4");
      const GDALDataType dt = (depth == 16) ? GDT_UInt16 : GDT_Byte;
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(path.c_str(), 16, 16, 1, dt, opts.List()));
      ASSERT_NE(nullptr, ds);
      GDALColorTable ct;
      GDALColorEntry ce = {10, 20, 30, 255};
      ct.SetColorEntry(0, &ce);
      ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetColorTable(&ct));
      int ovr = 2;
      ASSERT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr,
                                            nullptr, nullptr));
    }

    // NBITS=1 with AVERAGE_BIT2GRAYSCALE and duplicate 1x1 overview levels.
    {
      constexpr char kPath[] = "/vsimem/gw_ovr_bit2gray.tif";
      absl::Cleanup cl = [&] { VSIUnlink(kPath); };
      CPLStringList opts;
      opts.SetNameValue("NBITS", "1");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPath, 8, 8, 1, GDT_Byte, opts.List()));
      ASSERT_NE(nullptr, ds);
      int levels[] = {2, 8, 16};
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        ASSERT_EQ(CE_None,
                  ds->BuildOverviews("AVERAGE_BIT2GRAYSCALE", 3, levels, 0,
                                     nullptr, nullptr, nullptr));
        CPLErrorReset();
      }
      EXPECT_EQ(2, ds->GetRasterBand(1)->GetOverviewCount());
      // Rebuild with NEAREST to update RESAMPLING metadata item on existing
      // overview.
      ASSERT_EQ(CE_None, ds->BuildOverviews("NEAREST", 2, levels, 0, nullptr,
                                            nullptr, nullptr));
    }

    // Overview options (WEBP, ZLEVEL, ZSTD, LERC, JXL, JPEG, PREDICTOR,
    // INTERLEAVE, SPARSE_OK) and band-by-band SEPARATE regeneration.
    {
      constexpr char kPath[] = "/vsimem/gw_ovr_opts.tif";
      absl::Cleanup cl = [&] { VSIUnlink(kPath); };
      CPLStringList opts;
      opts.SetNameValue("INTERLEAVE", "BAND");
      opts.SetNameValue("WEBP_LOSSLESS", "YES");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPath, 32, 32, 3, GDT_Byte, opts.List()));
      ASSERT_NE(nullptr, ds);
      ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(0.0));

      int one_band = 1;
      int ovr2 = 2;
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        // Subset of bands fails.
        EXPECT_EQ(CE_Failure, ds->BuildOverviews("NEAREST", 1, &ovr2, 1,
                                                 &one_band, nullptr, nullptr));
        // Progress termination fails.
        EXPECT_EQ(CE_Failure,
                  ds->BuildOverviews("NEAREST", 1, &ovr2, 0, nullptr,
                                     FailingProgress, nullptr));
        CPLErrorReset();
      }

      CPLStringList ovr_opts;
      ovr_opts.SetNameValue("COMPRESS_OVERVIEW", "DEFLATE");
      ovr_opts.SetNameValue("ZLEVEL_OVERVIEW", "6");
      ovr_opts.SetNameValue("ZSTD_LEVEL_OVERVIEW", "3");
      ovr_opts.SetNameValue("WEBP_LEVEL_OVERVIEW", "80");
      ovr_opts.SetNameValue("MAX_Z_ERROR_OVERVIEW", "0.5");
      ovr_opts.SetNameValue("JPEG_TABLESMODE_OVERVIEW", "1");
      ovr_opts.SetNameValue("JXL_LOSSLESS_OVERVIEW", "NO");
      ovr_opts.SetNameValue("JXL_DISTANCE_OVERVIEW", "1.5");
      ovr_opts.SetNameValue("JXL_ALPHA_DISTANCE_OVERVIEW", "0.5");
      ovr_opts.SetNameValue("JXL_EFFORT_OVERVIEW", "3");
      ovr_opts.SetNameValue("SPARSE_OK_OVERVIEW", "YES");
      ovr_opts.SetNameValue("INTERLEAVE_OVERVIEW", "INVALID_IL");
      ovr_opts.SetNameValue("PREDICTOR_OVERVIEW", "2");
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        ASSERT_EQ(CE_None,
                  ds->BuildOverviews("AVERAGE", 1, &ovr2, 0, nullptr, nullptr,
                                     nullptr, ovr_opts.List()));

        // Adding external overviews when internal overviews already exist
        // fails.
        CPLStringList ext_opts;
        ext_opts.SetNameValue("LOCATION", "EXTERNAL");
        EXPECT_EQ(CE_Failure,
                  ds->BuildOverviews("NEAREST", 1, &ovr2, 0, nullptr, nullptr,
                                     nullptr, ext_opts.List()));
        CPLErrorReset();
      }
    }

    // External mask (.msk) overview generation + LOCATION=EXTERNAL /
    // LOCATION=INTERNAL on RO.
    {
      constexpr char kPath[] = "/vsimem/gw_ovr_extmask.tif";
      absl::Cleanup cl = [&] {
        VSIUnlink(kPath);
        VSIUnlink("/vsimem/gw_ovr_extmask.tif.msk");
        VSIUnlink("/vsimem/gw_ovr_extmask.tif.ovr");
      };
      CPLConfigOptionSetter ext_mask("GDAL_TIFF_INTERNAL_MASK", "NO", false);
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPath, 16, 16, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
      ASSERT_EQ(CE_None, ds->CreateMaskBand(GMF_PER_DATASET));
      int ovr = 2;
      ASSERT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr,
                                            nullptr, nullptr));
    }
    {
      constexpr char kPath[] = "/vsimem/gw_ovr_ro.tif";
      absl::Cleanup cl = [&] {
        VSIUnlink(kPath);
        VSIUnlink("/vsimem/gw_ovr_ro.tif.ovr");
      };
      CPLStringList opts;
      opts.SetNameValue("SPARSE_OK", "YES");
      GDALDatasetUniquePtr ds(
          gtiff_driver_->Create(kPath, 16, 16, 1, GDT_Byte, opts.List()));
      ASSERT_NE(nullptr, ds);
      ASSERT_EQ(CE_None, ds->CreateMaskBand(GMF_PER_DATASET));
      ds.reset();

      GDALDatasetUniquePtr ro(
          GDALDataset::Open(kPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ro);
      int ovr = 2;
      CPLStringList int_opts;
      int_opts.SetNameValue("LOCATION", "INTERNAL");
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure,
                ro->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr, nullptr,
                                   nullptr, int_opts.List()));
      // Read-only without LOCATION builds external .ovr and warns about
      // internal mask.
      EXPECT_EQ(CE_None, ro->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr,
                                            nullptr, nullptr));
      CPLErrorReset();
    }
  }

  // 6c. AddOverviews (GA_ReadOnly and GA_Update, including reopened COG).
  {
    constexpr char kPath[] = "/vsimem/gw_add_ovr.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(kPath);
      VSIUnlink("/vsimem/gw_add_ovr.tif.ovr");
    };
    GDALDatasetUniquePtr src(
        mem_driver_->Create("", 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, src);
    GDALColorTable ct;
    GDALColorEntry ce = {50, 100, 150, 255};
    ct.SetColorEntry(0, &ce);
    ASSERT_EQ(CE_None, src->GetRasterBand(1)->SetColorTable(&ct));
    int ovr2 = 2;
    ASSERT_EQ(CE_None, src->BuildOverviews("NEAREST", 1, &ovr2, 0, nullptr,
                                           nullptr, nullptr));

    CPLStringList copy_opts;
    copy_opts.SetNameValue("TILED", "YES");
    copy_opts.SetNameValue("BLOCKXSIZE", "16");
    copy_opts.SetNameValue("BLOCKYSIZE", "16");
    copy_opts.SetNameValue("COPY_SRC_OVERVIEWS", "YES");
    GDALDatasetUniquePtr cog(gtiff_driver_->CreateCopy(
        kPath, src.get(), FALSE, copy_opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, cog);
    cog.reset();

    const CPLStringList open_opts{"IGNORE_COG_LAYOUT_BREAK=YES"};
    GDALDatasetUniquePtr upd(GDALDataset::Open(
        kPath, GDAL_OF_RASTER | GDAL_OF_UPDATE, nullptr, open_opts.List()));
    ASSERT_NE(nullptr, upd);

    GDALDatasetUniquePtr mem_4x4(
        mem_driver_->Create("", 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, mem_4x4);
    GDALDatasetUniquePtr mem_8x8(
        mem_driver_->Create("", 8, 8, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, mem_8x8);
    GDALDatasetUniquePtr mem_bad(
        mem_driver_->Create("", 4, 4, 2, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, mem_bad);

    {
      WithQuietHandler quiet;
      CPLErrorReset();
      // Inconsistent overview fails.
      EXPECT_EQ(CE_Failure,
                upd->AddOverviews({mem_bad.get()}, nullptr, nullptr, nullptr));
      // Pass 4x4 then 8x8 (unsorted; 8x8 already exists, 4x4 is new and
      // triggers LAYOUT=IFDS_BEFORE_DATA warning on the reopened COG).
      EXPECT_EQ(CE_None, upd->AddOverviews({mem_4x4.get(), mem_8x8.get()},
                                           nullptr, nullptr, nullptr));
      // Also set a large metadata tag and close so FlushDirectory() emits a
      // COG IFD growth warning at EOF.
      ASSERT_EQ(CE_None, upd->SetMetadataItem("LARGE_TAG",
                                              std::string(512, 'X').c_str()));
      EXPECT_EQ(CE_None, upd->Close());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("LAYOUT=IFDS_BEFORE_DATA"));
      CPLErrorReset();
    }

    // AddOverviews in GA_ReadOnly mode on a fresh dataset with internal mask.
    constexpr char kRoPath[] = "/vsimem/gw_add_ovr_ro.tif";
    absl::Cleanup cl2 = [&] {
      VSIUnlink(kRoPath);
      VSIUnlink("/vsimem/gw_add_ovr_ro.tif.ovr");
    };
    GDALDatasetUniquePtr ds_ro_create(
        gtiff_driver_->Create(kRoPath, 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds_ro_create);
    ASSERT_EQ(CE_None, ds_ro_create->CreateMaskBand(GMF_PER_DATASET));
    ds_ro_create.reset();

    GDALDatasetUniquePtr ro(
        GDALDataset::Open(kRoPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ro);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_None,
              ro->AddOverviews({mem_8x8.get()}, nullptr, nullptr, nullptr));
    CPLErrorReset();
  }
}

// ---------------------------------------------------------------------------
// 7. Georeferencing: WriteGeoTIFFInfo, SetSpatialRef, SetGeoTransform, SetGCPs
// ---------------------------------------------------------------------------
TEST_F(GTiffDatasetWriteTest, GeoreferencingWriteAndSetters) {
  CPLConfigOptionSetter pam_on("GDAL_PAM_ENABLED", "YES", false);

  // 7a. North-up & rotated GeoTransform, AREA_OR_POINT=Point, TFW/WORLDFILE,
  // SRS replacement (GTiffWriteDummyGeokeyDirectory), clearing SRS/GT/GCPs,
  // and >10922 GCPs PAM fallback.
  {
    constexpr char kPath[] = "/vsimem/gw_georef.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(kPath);
      VSIUnlink("/vsimem/gw_georef.tfw");
      VSIUnlink("/vsimem/gw_georef.tif.aux.xml");
    };
    CPLStringList opts;
    opts.SetNameValue("TFW", "YES");
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kPath, 16, 16, 1, GDT_Byte, opts.List()));
    ASSERT_NE(nullptr, ds);

    OGRSpatialReference srs1;
    srs1.importFromEPSG(4326);
    ASSERT_EQ(CE_None, ds->SetSpatialRef(&srs1));
    ASSERT_EQ(CE_None, ds->SetMetadataItem("AREA_OR_POINT", "Point"));

    GDALGeoTransform gt_north{100.0, 1.0, 0.0, 200.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt_north));
    GByte px = 1;
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 1, 1, &px, 1, 1,
                                             GDT_Byte, 0, 0, nullptr));
    ds->FlushCache(false);

    // Replace SRS (hits GTiffWriteDummyGeokeyDirectory) and set rotated GT.
    OGRSpatialReference srs2;
    srs2.importFromEPSG(32631);
    ASSERT_EQ(CE_None, ds->SetSpatialRef(&srs2));
    GDALGeoTransform gt_rot{100.0, 1.0, 0.2, 200.0, 0.3, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt_rot));
    ds->FlushCache(false);

    // Setting GCPs clears previous GeoTransform with a warning.
    GDAL_GCP gcp{};
    char id[] = "1";
    char info[] = "";
    gcp.pszId = id;
    gcp.pszInfo = info;
    gcp.dfGCPPixel = 1.0;
    gcp.dfGCPLine = 2.0;
    gcp.dfGCPX = 10.0;
    gcp.dfGCPY = 20.0;
    gcp.dfGCPZ = 30.0;
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      ASSERT_EQ(CE_None, ds->SetGCPs(1, &gcp, &srs2));
      ds->FlushCache(false);

      // Setting > 10922 GCPs falls back to PAM.
      std::vector<GDAL_GCP> many_gcps(10923, gcp);
      ASSERT_EQ(CE_None, ds->SetGCPs(static_cast<int>(many_gcps.size()),
                                     many_gcps.data(), &srs2));
      CPLErrorReset();
    }
    // Now set <= 10922 GCPs to cancel PAM GCPs.
    ASSERT_EQ(CE_None, ds->SetGCPs(1, &gcp, &srs2));
    // Clear GCPs (SetGCPs(0, ...)) and clear SRS (SetSpatialRef(nullptr)).
    ASSERT_EQ(CE_None,
              ds->SetGCPs(0, nullptr,
                          static_cast<const OGRSpatialReference*>(nullptr)));
    ASSERT_EQ(CE_None, ds->SetSpatialRef(nullptr));
    ds->FlushCache(false);

    // Set GCPs again, then set GeoTransform (clears GCPs with warning), then
    // clear GT ({0,0,0,0,0,0}).
    ASSERT_EQ(CE_None, ds->SetGCPs(1, &gcp, &srs1));
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      ASSERT_EQ(CE_None, ds->SetGeoTransform(gt_north));
      CPLErrorReset();
    }
    GDALGeoTransform gt_zero{0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt_zero));
    ds->FlushCache(false);
  }

  // 7b. Vertical 1-band SRS scale/offset encoding, Compound SRS unit type,
  // incompatible SRS PAM fallback, PROFILE=BASELINE, and GA_ReadOnly setters.
  {
    constexpr char kPath[] = "/vsimem/gw_vert.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(kPath);
      VSIUnlink("/vsimem/gw_vert.wld");
      VSIUnlink("/vsimem/gw_vert.tif.aux.xml");
    };
    CPLStringList opts;
    opts.SetNameValue("WORLDFILE", "YES");
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kPath, 8, 8, 1, GDT_Float32, opts.List()));
    ASSERT_NE(nullptr, ds);
    OGRSpatialReference vert_srs;
    ASSERT_EQ(OGRERR_NONE,
              vert_srs.importFromWkt(
                  "VERT_CS[\"NAVD88 height\",VERT_DATUM[\"North American "
                  "Vertical Datum 1988\",2005],UNIT[\"metre\",1.0],"
                  "AXIS[\"Up\",UP]]"));
    ASSERT_EQ(CE_None, ds->SetSpatialRef(&vert_srs));
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetScale(2.5));
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetOffset(10.0));
    GDALGeoTransform gt{0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
    ds->FlushCache(false);

    // Incompatible 3D Projected SRS falls back to PAM.
    OGRSpatialReference proj3d;
    ASSERT_EQ(OGRERR_NONE, proj3d.SetFromUserInput("EPSG:4979"));
    proj3d.SetUTM(31, TRUE);
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      ASSERT_EQ(CE_None, ds->SetSpatialRef(&proj3d));
      ds->FlushCache(false);
      CPLErrorReset();
    }
    ds.reset();

    // Reopen in GA_ReadOnly and call SetSpatialRef, SetGeoTransform, SetGCPs,
    // SetMetadata, SetMetadataItem -> all go to PAM.
    GDALDatasetUniquePtr ro(
        GDALDataset::Open(kPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ro);
    EXPECT_EQ(CE_None, ro->SetSpatialRef(&vert_srs));
    EXPECT_EQ(CE_None, ro->SetGeoTransform(gt));
    GDAL_GCP gcp{};
    char id[] = "1";
    char info[] = "";
    gcp.pszId = id;
    gcp.pszInfo = info;
    EXPECT_EQ(CE_None, ro->SetGCPs(1, &gcp, &vert_srs));
    EXPECT_EQ(CE_None, ro->SetMetadataItem("RO_KEY", "RO_VAL"));
    CPLStringList ro_md{"RO_A=B"};
    EXPECT_EQ(CE_None, ro->SetMetadata(ro_md.List()));
  }

  // 7c. PROFILE=BASELINE SetSpatialRef, SetGeoTransform, SetGCPs.
  {
    constexpr char kPath[] = "/vsimem/gw_baseline_geo.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(kPath);
      VSIUnlink("/vsimem/gw_baseline_geo.tif.aux.xml");
    };
    CPLStringList opts;
    opts.SetNameValue("PROFILE", "BASELINE");
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kPath, 8, 8, 1, GDT_Byte, opts.List()));
    ASSERT_NE(nullptr, ds);
    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_None, ds->SetSpatialRef(&srs));
    GDALGeoTransform gt{0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    EXPECT_EQ(CE_None, ds->SetGeoTransform(gt));
    GDAL_GCP gcp{};
    char id[] = "1";
    char info[] = "";
    gcp.pszId = id;
    gcp.pszInfo = info;
    EXPECT_EQ(CE_None, ds->SetGCPs(1, &gcp, &srs));
    CPLErrorReset();
  }
}

// ---------------------------------------------------------------------------
// 8. Metadata, RPC, IMD, XMP, PAM PushMetadataToPam, RAT, and SaveICCProfile
// ---------------------------------------------------------------------------
TEST_F(GTiffDatasetWriteTest, MetadataRPCIMDXMPAndColorProfile) {
  CPLConfigOptionSetter pam_on("GDAL_PAM_ENABLED", "YES", false);

  // 8a. TIFFTAG_* (string, float, short, resolutionunit=0, byte_string,
  // unhandled warning) + unsetting tags + xml:XMP + RAT + TILING_SCHEME +
  // SaveICCProfile (both ICC base64 and colorimetric primaries/whitepoint/
  // transferfunction/transferrange).
  {
    constexpr char kPath[] = "/vsimem/gw_md_tags.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(kPath);
      VSIUnlink("/vsimem/gw_md_tags.IMD");
    };
    std::string tf_256;
    for (int i = 0; i < 256; ++i) {
      if (i > 0) tf_256 += ',';
      tf_256 += std::format("{}", i * 257);
    }
    CPLStringList opts;
    opts.SetNameValue("@TILING_SCHEME_NAME", "GoogleMapsCompatible");
    opts.SetNameValue("@TILING_SCHEME_ZOOM_LEVEL", "10");
    opts.SetNameValue("@TILING_SCHEME_ALIGNED_LEVELS", "2");
    opts.SetNameValue("@OVERVIEW_RESAMPLING", "AVERAGE");
    opts.SetNameValue("SOURCE_PRIMARIES_RED", "0.64, 0.33, 1.0");
    opts.SetNameValue("SOURCE_PRIMARIES_GREEN", "0.30, 0.60, 1.0");
    opts.SetNameValue("SOURCE_PRIMARIES_BLUE", "0.15, 0.06, 1.0");
    opts.SetNameValue("SOURCE_WHITEPOINT", "0.3127, 0.3290, 1.0");
    opts.SetNameValue("TIFFTAG_TRANSFERFUNCTION_RED", tf_256.c_str());
    opts.SetNameValue("TIFFTAG_TRANSFERFUNCTION_GREEN", tf_256.c_str());
    opts.SetNameValue("TIFFTAG_TRANSFERFUNCTION_BLUE", tf_256.c_str());
    opts.SetNameValue("TIFFTAG_TRANSFERRANGE_BLACK", "0, 0, 0");
    opts.SetNameValue("TIFFTAG_TRANSFERRANGE_WHITE", "255, 255, 255");

    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kPath, 8, 8, 1, GDT_Byte, opts.List()));
    ASSERT_NE(nullptr, ds);

    ASSERT_EQ(CE_None, ds->SetMetadataItem("TIFFTAG_DOCUMENTNAME", "Doc"));
    ASSERT_EQ(CE_None, ds->SetMetadataItem("TIFFTAG_XRESOLUTION", "72.5"));
    ASSERT_EQ(CE_None, ds->SetMetadataItem("TIFFTAG_RESOLUTIONUNIT", "0"));
    ASSERT_EQ(CE_None, ds->SetMetadataItem("TIFFTAG_MINSAMPLEVALUE", "1"));
    ASSERT_EQ(CE_None, ds->SetMetadataItem("GEO_METADATA", "GeoMD"));
    ASSERT_EQ(CE_None, ds->SetMetadataItem("TIFF_RSID", "RSID"));
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      ASSERT_EQ(CE_None, ds->SetMetadataItem("TIFFTAG_UNKNOWN_TAG", "123"));
      CPLErrorReset();
    }
    ASSERT_EQ(CE_None, ds->SetMetadataItem("CUSTOM_KEY", "VAL", "CUSTOM_DOM"));

    CPLStringList xml_md{"<root>hello</root>"};
    ASSERT_EQ(CE_None, ds->SetMetadata(xml_md.List(), "xml:custom"));
    CPLStringList xmp_md{"<x:xmpmeta/>"};
    ASSERT_EQ(CE_None, ds->SetMetadata(xmp_md.List(), "xml:XMP"));
    CPLStringList imd_md{"version = \"P\";"};
    ASSERT_EQ(CE_None, ds->SetMetadata(imd_md.List(), "IMD"));

    // Band description, unit, scale, offset, colorinterp, and RAT.
    GDALRasterBand* b1 = ds->GetRasterBand(1);
    b1->SetDescription("BandOne");
    ASSERT_EQ(CE_None, b1->SetUnitType("foot"));
    ASSERT_EQ(CE_None, b1->SetScale(2.0));
    ASSERT_EQ(CE_None, b1->SetOffset(1.0));
    ASSERT_EQ(CE_None, b1->SetColorInterpretation(GCI_RedBand));
    GDALDefaultRasterAttributeTable rat;
    rat.CreateColumn("Value", GFT_Integer, GFU_Generic);
    rat.SetRowCount(1);
    rat.SetValue(0, 0, 1);
    ASSERT_EQ(CE_None, b1->SetDefaultRAT(&rat));

    GByte px = 1;
    ASSERT_EQ(CE_None, b1->RasterIO(GF_Write, 0, 0, 1, 1, &px, 1, 1, GDT_Byte,
                                    0, 0, nullptr));
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      ds->FlushCache(false);
      CPLErrorReset();
    }

    // Now clear default metadata, XMP, RAT, description, unit, scale/offset so
    // WriteMetadata unsets TIFFTAG_* and TIFFTAG_GDAL_METADATA.
    ASSERT_EQ(CE_None, ds->SetMetadata(nullptr));
    ASSERT_EQ(CE_None, ds->SetMetadata(nullptr, "CUSTOM_DOM"));
    ASSERT_EQ(CE_None, ds->SetMetadata(nullptr, "xml:custom"));
    ASSERT_EQ(CE_None, ds->SetMetadata(nullptr, "xml:XMP"));
    b1->SetDescription("");
    ASSERT_EQ(CE_None, b1->SetUnitType(""));
    ASSERT_EQ(CE_None, b1->SetScale(1.0));
    ASSERT_EQ(CE_None, b1->SetOffset(0.0));
    ASSERT_EQ(CE_None, b1->SetColorInterpretation(GCI_GrayIndex));
    ASSERT_EQ(CE_None, b1->SetDefaultRAT(nullptr));

    // Also test SaveICCProfile via pDS (SetMetadataItem on COLOR_PROFILE)
    // with invalid colorimetric strings (emitting warnings) and a valid base64
    // ICC profile.
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      ASSERT_EQ(CE_None, ds->SetMetadataItem("SOURCE_PRIMARIES_RED",
                                             "0.64,0.33,2.0", "COLOR_PROFILE"));
      ASSERT_EQ(CE_None, ds->SetMetadataItem("SOURCE_WHITEPOINT",
                                             "0.31,0.32,2.0", "COLOR_PROFILE"));
      ASSERT_EQ(CE_None, ds->SetMetadataItem("TIFFTAG_TRANSFERRANGE_BLACK",
                                             "0,0", "COLOR_PROFILE"));
      CPLErrorReset();
    }
    ds.reset();

    // Reopen in GA_Update and set SOURCE_ICC_PROFILE via SetMetadata.
    GDALDatasetUniquePtr upd(
        GDALDataset::Open(kPath, GDAL_OF_RASTER | GDAL_OF_UPDATE));
    ASSERT_NE(nullptr, upd);
    CPLStringList icc_md{"SOURCE_ICC_PROFILE=AAAA"};
    EXPECT_EQ(CE_None, upd->SetMetadata(icc_md.List(), "COLOR_PROFILE"));
  }

  // 8b. PushMetadataToPam & WriteRPC to PAM (PROFILE=BASELINE, RPB=NO).
  {
    constexpr char kPath[] = "/vsimem/gw_pam_push.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(kPath);
      VSIUnlink("/vsimem/gw_pam_push.tif.aux.xml");
    };
    CPLStringList opts;
    opts.SetNameValue("PROFILE", "BASELINE");
    opts.SetNameValue("RPB", "NO");
    GDALDatasetUniquePtr ds;
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      ds.reset(gtiff_driver_->Create(kPath, 8, 8, 1, GDT_Byte, opts.List()));
      CPLErrorReset();
    }
    ASSERT_NE(nullptr, ds);
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      ASSERT_EQ(CE_None, ds->SetMetadataItem("TIFFTAG_ARTIST", "Artist"));
      ASSERT_EQ(CE_None, ds->SetMetadataItem("USER_KEY", "USER_VAL"));
      ASSERT_EQ(CE_None,
                ds->GetRasterBand(1)->SetMetadataItem("B_KEY", "B_VAL"));
      ASSERT_EQ(CE_None,
                ds->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand));
      ds->FlushCache(false);
      CPLErrorReset();
    }
  }
}

// ---------------------------------------------------------------------------
// 9. GuessJPEGQuality (MD5 & Brute-Force 2-Band Search) & TablesMode
// ---------------------------------------------------------------------------
TEST_F(GTiffDatasetWriteTest, GuessJPEGQualityAndUpdateMode) {
  struct JpegQualityCase {
    int bands;
    const char* photo;
    const char* interleave;
    const char* quality;
    const char* tablesmode;
  };
  constexpr JpegQualityCase kCases[] = {
      {.bands = 1,
       .photo = "MINISBLACK",
       .interleave = "PIXEL",
       .quality = "82",
       .tablesmode = "1"},  // 1-band 8-bit MD5
      {.bands = 3,
       .photo = "RGB",
       .interleave = "PIXEL",
       .quality = "75",
       .tablesmode = "3"},  // 3-band RGB + Huffman table
      {.bands = 3,
       .photo = "YCBCR",
       .interleave = "PIXEL",
       .quality = "77",
       .tablesmode = "1"},  // 3-band YCbCr MD5
      {.bands = 2,
       .photo = "MINISBLACK",
       .interleave = "BAND",
       .quality = "75",
       .tablesmode = "1"},  // 2-band brute-force (nQuality=0)
      {.bands = 2,
       .photo = "MINISBLACK",
       .interleave = "BAND",
       .quality = "50",
       .tablesmode = "1"},  // 2-band brute-force (nQuality=50)
      {.bands = 1,
       .photo = "MINISBLACK",
       .interleave = "PIXEL",
       .quality = "80",
       .tablesmode = "0"},  // JPEGTABLESMODE=0 (no table tag)
      {.bands = 1,
       .photo = "MINISBLACK",
       .interleave = "PIXEL",
       .quality = "80",
       .tablesmode = "2"},  // JPEGTABLESMODE=2 (Huffman only)
  };

  for (size_t i = 0; i < std::size(kCases); ++i) {
    const std::string path = std::format("/vsimem/gw_jpeg_q_{}.tif", i);
    SCOPED_TRACE(path);
    absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
    CPLStringList opts;
    opts.SetNameValue("COMPRESS", "JPEG");
    opts.SetNameValue("PHOTOMETRIC", kCases[i].photo);
    opts.SetNameValue("INTERLEAVE", kCases[i].interleave);
    opts.SetNameValue("JPEG_QUALITY", kCases[i].quality);
    opts.SetNameValue("JPEGTABLESMODE", kCases[i].tablesmode);
    GDALDatasetUniquePtr ds(gtiff_driver_->Create(
        path.c_str(), 16, 16, kCases[i].bands, GDT_Byte, opts.List()));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> buf(16 * 16, 100);
    for (int b = 1; b <= kCases[i].bands; ++b) {
      ASSERT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                             GF_Write, 0, 0, 16, 16, buf.data(), 16, 16,
                             GDT_Byte, 0, 0, nullptr));
    }
    ds.reset();

    // Reopening in GA_Update calls GuessJPEGQuality and
    // SetJPEGQualityAndTablesModeFromFile.
    GDALDatasetUniquePtr upd(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE));
    ASSERT_NE(nullptr, upd);
    // GuessJPEGQuality() needs a quantization table in the JPEGTables tag:
    // JPEGTABLESMODE=0 writes no JPEGTables tag and JPEGTABLESMODE=2 writes
    // only Huffman tables, so no quality/tables-mode metadata is derived for
    // those two cases. Otherwise the original quality is recovered (by MD5
    // lookup or brute force) and JPEGTABLESMODE reflects the tables present.
    const std::string_view tables_mode(kCases[i].tablesmode);
    if (tables_mode == "0" || tables_mode == "2") {
      EXPECT_EQ(nullptr,
                upd->GetMetadataItem("JPEG_QUALITY", "IMAGE_STRUCTURE"));
      EXPECT_EQ(nullptr,
                upd->GetMetadataItem("JPEGTABLESMODE", "IMAGE_STRUCTURE"));
    } else {
      EXPECT_STREQ(kCases[i].quality,
                   upd->GetMetadataItem("JPEG_QUALITY", "IMAGE_STRUCTURE"));
      EXPECT_STREQ(kCases[i].tablesmode,
                   upd->GetMetadataItem("JPEGTABLESMODE", "IMAGE_STRUCTURE"));
    }
  }
}

// ---------------------------------------------------------------------------
// 10. CreateCopy, CopyImageryAndMask, Split Bands, JPEG Copy, & CreateMaskBand
// ---------------------------------------------------------------------------
TEST_F(GTiffDatasetWriteTest, CreateCopyAndMasksComprehensive) {
  CPLConfigOptionSetter pam_on("GDAL_PAM_ENABLED", "YES", false);

  // 10a. 0-band and mixed-band-type CreateCopy.
  {
    GDALDatasetUniquePtr ds0(
        mem_driver_->Create("", 8, 8, 0, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds0);
    GDALDatasetUniquePtr ds_mix(
        mem_driver_->Create("", 8, 8, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds_mix);
    ASSERT_EQ(CE_None, ds_mix->AddBand(GDT_UInt16, nullptr));

    {
      WithQuietHandler quiet;
      CPLErrorReset();
      GDALDatasetUniquePtr cc0(gtiff_driver_->CreateCopy(
          "/vsimem/gw_cc0.tif", ds0.get(), TRUE, nullptr, nullptr, nullptr));
      EXPECT_EQ(nullptr, cc0);

      GDALDatasetUniquePtr cc_mix_strict(gtiff_driver_->CreateCopy(
          "/vsimem/gw_cc_mix_strict.tif", ds_mix.get(), TRUE, nullptr, nullptr,
          nullptr));
      EXPECT_EQ(nullptr, cc_mix_strict);

      constexpr char kNonStrictPath[] = "/vsimem/gw_cc_mix_ns.tif";
      absl::Cleanup cl = [&] { VSIUnlink(kNonStrictPath); };
      GDALDatasetUniquePtr cc_non_strict(gtiff_driver_->CreateCopy(
          kNonStrictPath, ds_mix.get(), FALSE, nullptr, nullptr, nullptr));
      CPLErrorReset();
      ASSERT_NE(nullptr, cc_non_strict);
    }
  }

  // 10b. Source with NBITS, PIXELTYPE=SIGNEDBYTE, COLOR_PROFILE, json:ISIS3,
  // xml:ESRI (with ESRI_XML_PAM=YES), xml:XMP, GCPs + TFW=YES warning,
  // category names, and COPY_SRC_MDD=YES / SRC_MDD.
  {
    CPLConfigOptionSetter esri_pam("ESRI_XML_PAM", "YES", false);
    GDALDatasetUniquePtr src(
        mem_driver_->Create("", 8, 8, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, src);
    ASSERT_EQ(CE_None, src->GetRasterBand(1)->SetMetadataItem(
                           "NBITS", "8", "IMAGE_STRUCTURE"));
    ASSERT_EQ(CE_None, src->GetRasterBand(1)->SetMetadataItem(
                           "PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE"));
    ASSERT_EQ(CE_None, src->SetMetadataItem("SOURCE_ICC_PROFILE", "AAAA",
                                            "COLOR_PROFILE"));
    CPLStringList isis_md{"{\"isis\":1}"};
    ASSERT_EQ(CE_None, src->SetMetadata(isis_md.List(), "json:ISIS3"));
    CPLStringList esri_md{"<esri/>"};
    ASSERT_EQ(CE_None, src->SetMetadata(esri_md.List(), "xml:ESRI"));
    CPLStringList xmp_md{"<x:xmpmeta/>"};
    ASSERT_EQ(CE_None, src->SetMetadata(xmp_md.List(), "xml:XMP"));
    ASSERT_EQ(CE_None, src->SetMetadataItem("AREA_OR_POINT", "Point"));
    ASSERT_EQ(CE_None, src->GetRasterBand(1)->SetMetadataItem("BAND_MD", "1",
                                                              "IMAGERY"));
    CPLStringList cats{"Cat0", "Cat1"};
    ASSERT_EQ(CE_None, src->GetRasterBand(1)->SetCategoryNames(cats.List()));

    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    GDAL_GCP gcp{};
    char id[] = "1";
    char info[] = "";
    gcp.pszId = id;
    gcp.pszInfo = info;
    gcp.dfGCPPixel = 1.0;
    gcp.dfGCPLine = 1.0;
    ASSERT_EQ(CE_None, src->SetGCPs(1, &gcp, &srs));

    constexpr char kPath[] = "/vsimem/gw_cc_meta.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(kPath);
      VSIUnlink("/vsimem/gw_cc_meta.tif.aux.xml");
    };
    CPLStringList opts;
    opts.SetNameValue("TFW", "YES");
    opts.SetNameValue("COPY_SRC_MDD", "YES");
    opts.SetNameValue("@FLUSHCACHE", "YES");
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr dst(gtiff_driver_->CreateCopy(
        kPath, src.get(), FALSE, opts.List(), nullptr, nullptr));
    CPLErrorReset();
    ASSERT_NE(nullptr, dst);
  }

  // 10c. ColorTable CreateCopy: 1-band Byte (with out-of-range ClampCTEntry
  // warning), 2-band Byte (palette+alpha), 1-band UInt16, 3-band Byte
  // (warning), and 1-band Byte with COMPRESS=JPEG (error).
  {
    GDALColorTable ct;
    GDALColorEntry ce_neg = {-5, 300, 10, 255};
    ct.SetColorEntry(0, &ce_neg);

    for (int bands : {1, 2, 3}) {
      SCOPED_TRACE(bands);
      constexpr char kPath[] = "/vsimem/gw_cc_ct.tif";
      absl::Cleanup cl = [&] { VSIUnlink(kPath); };
      GDALDatasetUniquePtr src(
          mem_driver_->Create("", 8, 8, bands, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, src);
      ASSERT_EQ(CE_None, src->GetRasterBand(1)->SetColorTable(&ct));
      WithQuietHandler quiet;
      CPLErrorReset();
      GDALDatasetUniquePtr dst(gtiff_driver_->CreateCopy(
          kPath, src.get(), FALSE, nullptr, nullptr, nullptr));
      EXPECT_EQ(bands == 3 ? CE_Failure : CE_Warning, CPLGetLastErrorType());
      CPLErrorReset();
      ASSERT_NE(nullptr, dst);
    }
    // 1-band UInt16 with ColorTable.
    {
      constexpr char kPath[] = "/vsimem/gw_cc_ct16.tif";
      absl::Cleanup cl = [&] { VSIUnlink(kPath); };
      GDALDatasetUniquePtr src16(
          mem_driver_->Create("", 8, 8, 1, GDT_UInt16, nullptr));
      ASSERT_NE(nullptr, src16);
      ASSERT_EQ(CE_None, src16->GetRasterBand(1)->SetColorTable(&ct));
      GDALDatasetUniquePtr dst16(gtiff_driver_->CreateCopy(
          kPath, src16.get(), FALSE, nullptr, nullptr, nullptr));
      ASSERT_NE(nullptr, dst16);
    }
    // 1-band Byte with ColorTable and COMPRESS=JPEG fails.
    {
      constexpr char kJpgCtPath[] = "/vsimem/gw_cc_ct_jpg.tif";
      absl::Cleanup cl_jpg = [&] { VSIUnlink(kJpgCtPath); };
      GDALDatasetUniquePtr src_jpg(
          mem_driver_->Create("", 16, 16, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, src_jpg);
      ASSERT_EQ(CE_None, src_jpg->GetRasterBand(1)->SetColorTable(&ct));
      CPLStringList opts;
      opts.SetNameValue("COMPRESS", "JPEG");
      WithQuietHandler quiet;
      CPLErrorReset();
      GDALDatasetUniquePtr dst_jpg(gtiff_driver_->CreateCopy(
          kJpgCtPath, src_jpg.get(), FALSE, opts.List(), nullptr, nullptr));
      EXPECT_EQ(nullptr, dst_jpg);
      CPLErrorReset();
    }
  }

  // 10d. Split-band and split-bitmap scanline loops in CreateCopy (16x2001,
  // BLOCKYSIZE=2001, COMPRESS=PACKBITS on 2-band CONTIG, 1-band Byte, and
  // 1-band NBITS=1).
  for (int mode = 0; mode < 3; ++mode) {
    const std::string path = std::format("/vsimem/gw_cc_split_{}.tif", mode);
    SCOPED_TRACE(path);
    absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
    const int bands = (mode == 0) ? 2 : 1;
    GDALDatasetUniquePtr src(
        mem_driver_->Create("", 16, 2001, bands, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, src);
    CPLStringList opts;
    opts.SetNameValue("TILED", "NO");
    opts.SetNameValue("BLOCKYSIZE", "2001");
    opts.SetNameValue("COMPRESS", "PACKBITS");
    opts.SetNameValue("INTERLEAVE", "PIXEL");
    if (mode == 2) opts.SetNameValue("NBITS", "1");
    GDALDatasetUniquePtr dst(gtiff_driver_->CreateCopy(
        path.c_str(), src.get(), FALSE, opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, dst);
  }

  // 10e. CopyImageryAndMask via CreateCopy with COPY_SRC_OVERVIEWS=YES on
  // partial tiles (20x20 with 16x16 tiles): SEPARATE+mask, @TILE_INTERLEAVE,
  // CONTIG+mask, and CONTIG NBITS=4 odd-bits, plus @OVERVIEW_DATASET /
  // @MASK_OVERVIEW_DATASET / @OVERVIEW_COUNT.
  for (int mode = 0; mode < 4; ++mode) {
    CPLConfigOptionSetter int_mask("GDAL_TIFF_INTERNAL_MASK", "YES", false);
    const std::string src_path =
        std::format("/vsimem/gw_cc_cog_src_{}.tif", mode);
    const std::string path = std::format("/vsimem/gw_cc_cog_{}.tif", mode);
    SCOPED_TRACE(path);
    absl::Cleanup cl = [&] {
      VSIUnlink(src_path.c_str());
      VSIUnlink(path.c_str());
    };
    // NOTE: For the odd-bits CONTIG case (mode 3), CopyImageryAndMask() loads
    // the first n-1 bands into freshly allocated cache blocks and only fills
    // the partial-tile region, so GTiffOddBitsBand::IWriteBlock() reads
    // uninitialized padding when a block is not fully covered (flagged by
    // MSan). Use a raster whose base and overview levels are both block
    // aligned for that mode: 128x128 with 64x64 tiles (overview tiles inherit
    // the base block size only when it is a power of two in [64, 4096]).
    const int src_size = (mode == 3) ? 128 : 20;
    const char* block_size = (mode == 3) ? "64" : "16";
    GDALDatasetUniquePtr src(gtiff_driver_->Create(
        src_path.c_str(), src_size, src_size, 2, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, src);
    ASSERT_EQ(CE_None, src->CreateMaskBand(GMF_PER_DATASET));
    int ovr = 2;
    ASSERT_EQ(CE_None, src->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr,
                                           nullptr, nullptr));

    CPLStringList opts;
    opts.SetNameValue("TILED", "YES");
    opts.SetNameValue("BLOCKXSIZE", block_size);
    opts.SetNameValue("BLOCKYSIZE", block_size);
    opts.SetNameValue("COPY_SRC_OVERVIEWS", "YES");
    opts.SetNameValue("COMPRESS", "DEFLATE");
    if (mode == 0) {
      opts.SetNameValue("INTERLEAVE", "BAND");
    } else if (mode == 1) {
      opts.SetNameValue("@TILE_INTERLEAVE", "YES");
    } else if (mode == 2) {
      opts.SetNameValue("INTERLEAVE", "PIXEL");
    } else {
      opts.SetNameValue("INTERLEAVE", "PIXEL");
      opts.SetNameValue("NBITS", "4");
    }
    GDALDatasetUniquePtr dst(gtiff_driver_->CreateCopy(
        path.c_str(), src.get(), FALSE, opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, dst);
  }

  // @OVERVIEW_DATASET, @OVERVIEW_COUNT, @MASK_OVERVIEW_DATASET.
  {
    constexpr char kOvrPath[] = "/vsimem/gw_ext_ovr_src.tif";
    constexpr char kMskPath[] = "/vsimem/gw_ext_msk_src.tif";
    constexpr char kDstPath[] = "/vsimem/gw_cc_ext_ovr.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(kOvrPath);
      VSIUnlink(kMskPath);
      VSIUnlink(kDstPath);
    };
    GDALDatasetUniquePtr ovr_ds(
        gtiff_driver_->Create(kOvrPath, 8, 8, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ovr_ds);
    ovr_ds.reset();
    GDALDatasetUniquePtr msk_ds(
        gtiff_driver_->Create(kMskPath, 8, 8, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, msk_ds);
    msk_ds.reset();

    GDALDatasetUniquePtr src(
        mem_driver_->Create("", 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, src);
    ASSERT_EQ(CE_None, src->CreateMaskBand(GMF_PER_DATASET));

    CPLStringList opts;
    opts.SetNameValue("TILED", "YES");
    opts.SetNameValue("BLOCKXSIZE", "16");
    opts.SetNameValue("BLOCKYSIZE", "16");
    opts.SetNameValue("COPY_SRC_OVERVIEWS", "YES");
    opts.SetNameValue("@OVERVIEW_DATASET", kOvrPath);
    opts.SetNameValue("@OVERVIEW_COUNT", "1");
    opts.SetNameValue("@MASK_OVERVIEW_DATASET", kMskPath);
    GDALDatasetUniquePtr dst(gtiff_driver_->CreateCopy(
        kDstPath, src.get(), FALSE, opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, dst);
  }

  // 10f. Lossless DCT CreateCopy from a JPEG dataset in /vsimem/, plus progress
  // cancellation with GTIFF_DELETE_ON_ERROR=YES and NO.
  {
    GDALDriver* jpg_drv = GetGDALDriverManager()->GetDriverByName("JPEG");
    ASSERT_NE(nullptr, jpg_drv);
    constexpr char kJpgPath[] = "/vsimem/gw_src.jpg";
    constexpr char kTifPath[] = "/vsimem/gw_from_jpg.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(kJpgPath);
      VSIUnlink(kTifPath);
    };
    GDALDatasetUniquePtr mem_src(
        mem_driver_->Create("", 16, 16, 3, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, mem_src);
    GDALDatasetUniquePtr jpg_ds(jpg_drv->CreateCopy(
        kJpgPath, mem_src.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, jpg_ds);
    jpg_ds.reset();

    GDALDatasetUniquePtr jpg_ro(
        GDALDataset::Open(kJpgPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, jpg_ro);
    CPLStringList opts;
    opts.SetNameValue("COMPRESS", "JPEG");
    GDALDatasetUniquePtr tif_ds(gtiff_driver_->CreateCopy(
        kTifPath, jpg_ro.get(), FALSE, opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, tif_ds);
  }
  for (const char* del_on_err : {"YES", "NO"}) {
    SCOPED_TRACE(del_on_err);
    CPLConfigOptionSetter setter("GTIFF_DELETE_ON_ERROR", del_on_err, false);
    constexpr char kPath[] = "/vsimem/gw_cc_abort.tif";
    absl::Cleanup cl = [&] { VSIUnlink(kPath); };
    GDALDatasetUniquePtr src(
        mem_driver_->Create("", 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, src);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr dst(gtiff_driver_->CreateCopy(
        kPath, src.get(), FALSE, nullptr, FailingProgress, nullptr));
    EXPECT_EQ(nullptr, dst);
    CPLErrorReset();
  }

  // 10g. CreateMaskBand (dataset & band) error and fallback branches.
  {
    CPLConfigOptionSetter int_mask("GDAL_TIFF_INTERNAL_MASK", "YES", false);
    CPLConfigOptionSetter no_promote("GDAL_TIFF_INTERNAL_MASK_TO_8BIT", "NO",
                                     false);
    constexpr char kPath[] = "/vsimem/gw_masks.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(kPath);
      VSIUnlink("/vsimem/gw_masks.tif.msk");
    };
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kPath, 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      // Invalid flag for internal mask fails before creating m_poMaskDS.
      EXPECT_EQ(CE_Failure, ds->CreateMaskBand(0));
      CPLErrorReset();
    }
    // Valid GMF_PER_DATASET via band CreateMaskBand succeeds.
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->CreateMaskBand(GMF_PER_DATASET));
    WithQuietHandler quiet;
    CPLErrorReset();
    // Duplicate CreateMaskBand fails on both dataset and band.
    EXPECT_EQ(CE_Failure, ds->CreateMaskBand(GMF_PER_DATASET));
    EXPECT_EQ(CE_Failure,
              ds->GetRasterBand(1)->CreateMaskBand(GMF_PER_DATASET));
    // Creating a mask on the mask IFD itself fails.
    ASSERT_NE(nullptr, ds->GetRasterBand(1)->GetMaskBand()->GetDataset());
    EXPECT_EQ(CE_Failure,
              ds->GetRasterBand(1)->GetMaskBand()->GetDataset()->CreateMaskBand(
                  GMF_PER_DATASET));
    CPLErrorReset();
  }
  {
    // CreateMaskBand on read-only dataset creates external .msk.
    constexpr char kPath[] = "/vsimem/gw_mask_ro.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(kPath);
      VSIUnlink("/vsimem/gw_mask_ro.tif.msk");
    };
    GDALDatasetUniquePtr ds(
        gtiff_driver_->Create(kPath, 8, 8, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    ds.reset();
    GDALDatasetUniquePtr ro(
        GDALDataset::Open(kPath, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ro);
    EXPECT_EQ(CE_None, ro->CreateMaskBand(GMF_PER_DATASET));
  }

  // 10h. CreateCopy GeoTransform (north-up + rotated + AREA_OR_POINT=Point +
  // vertical SRS + TFW/WORLDFILE + BASELINE PAM), 64-bit NoData,
  // STREAMABLE_OUTPUT=YES, PIXELTYPE=SIGNEDBYTE, ALPHA/PHOTOMETRIC=RGB 5-band,
  // and WEBP/JXL/LZMA/ZSTD codecs in CreateCopy.
  {
    CPLConfigOptionSetter pam_on2("GDAL_PAM_ENABLED", "YES", false);
    constexpr char kGt1Path[] = "/vsimem/gw_cc_gt1.tif";
    constexpr char kGt1Tfw[] = "/vsimem/gw_cc_gt1.tfw";
    constexpr char kGt2Path[] = "/vsimem/gw_cc_gt2.tif";
    constexpr char kGt2Wld[] = "/vsimem/gw_cc_gt2.wld";
    constexpr char kBasePath[] = "/vsimem/gw_cc_base.tif";
    constexpr char kBaseAux[] = "/vsimem/gw_cc_base.tif.aux.xml";
    constexpr char kStreamPath[] = "/vsimem/gw_cc_stream.tif";
    constexpr char kRgb5Path[] = "/vsimem/gw_cc_rgb5.tif";
    constexpr char kRgb4Path[] = "/vsimem/gw_cc_rgb4.tif";
    absl::Cleanup cl = [&] {
      for (const char* p : {kGt1Path, kGt1Tfw, kGt2Path, kGt2Wld, kBasePath,
                            kBaseAux, kStreamPath, kRgb5Path, kRgb4Path}) {
        VSIUnlink(p);
      }
    };

    // North-up GeoTransform + Point + 3D Vertical SRS + TFW=YES + NoData:
    {
      GDALDatasetUniquePtr src(
          mem_driver_->Create("", 8, 8, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, src);
      GDALGeoTransform gt(100.0, 2.0, 0.0, 200.0, 0.0, -2.0);
      EXPECT_EQ(CE_None, src->SetGeoTransform(gt));
      EXPECT_EQ(CE_None,
                src->SetMetadataItem(GDALMD_AREA_OR_POINT, GDALMD_AOP_POINT));
      OGRSpatialReference srs;
      ASSERT_EQ(OGRERR_NONE, srs.SetFromUserInput("EPSG:4326+5773"));
      EXPECT_EQ(CE_None, src->SetSpatialRef(&srs));
      EXPECT_EQ(CE_None, src->GetRasterBand(1)->SetNoDataValue(99.0));
      EXPECT_EQ(CE_None, src->GetRasterBand(1)->SetOffset(5.0));

      CPLStringList co;
      co.SetNameValue("TFW", "YES");
      co.SetNameValue("PIXELTYPE", "SIGNEDBYTE");
      GDALDatasetUniquePtr dst(gtiff_driver_->CreateCopy(
          kGt1Path, src.get(), FALSE, co.List(), nullptr, nullptr));
      ASSERT_NE(nullptr, dst);

      // Same source copied with PROFILE=BASELINE -> exports SRS & GT to PAM.
      CPLStringList bco;
      bco.SetNameValue("PROFILE", "BASELINE");
      WithQuietHandler quiet;
      CPLErrorReset();
      GDALDatasetUniquePtr bdst(gtiff_driver_->CreateCopy(
          kBasePath, src.get(), FALSE, bco.List(), nullptr, nullptr));
      CPLErrorReset();
      ASSERT_NE(nullptr, bdst);
    }

    // Rotated GeoTransform + Point + WORLDFILE=YES + Int64/UInt64 NoData:
    for (GDALDataType dt : {GDT_Int64, GDT_UInt64}) {
      SCOPED_TRACE(GDALGetDataTypeName(dt));
      GDALDatasetUniquePtr src(mem_driver_->Create("", 8, 8, 1, dt, nullptr));
      ASSERT_NE(nullptr, src);
      GDALGeoTransform gt(10.0, 1.0, 0.2, 20.0, 0.3, -1.0);
      EXPECT_EQ(CE_None, src->SetGeoTransform(gt));
      EXPECT_EQ(CE_None,
                src->SetMetadataItem(GDALMD_AREA_OR_POINT, GDALMD_AOP_POINT));
      if (dt == GDT_Int64) {
        EXPECT_EQ(CE_None, src->GetRasterBand(1)->SetNoDataValueAsInt64(-42));
      } else {
        EXPECT_EQ(CE_None, src->GetRasterBand(1)->SetNoDataValueAsUInt64(42));
      }
      CPLStringList co;
      co.SetNameValue("WORLDFILE", "YES");
      GDALDatasetUniquePtr dst(gtiff_driver_->CreateCopy(
          kGt2Path, src.get(), FALSE, co.List(), nullptr, nullptr));
      ASSERT_NE(nullptr, dst);
    }

    // STREAMABLE_OUTPUT=YES in CreateCopy:
    {
      GDALDatasetUniquePtr src(
          mem_driver_->Create("", 16, 16, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, src);
      CPLStringList co;
      co.SetNameValue("STREAMABLE_OUTPUT", "YES");
      GDALDatasetUniquePtr dst(gtiff_driver_->CreateCopy(
          kStreamPath, src.get(), FALSE, co.List(), nullptr, nullptr));
      ASSERT_NE(nullptr, dst);
    }

    // 5-band PHOTOMETRIC=RGB + ALPHA metadata + 4-band ALPHA=NO + codecs in
    // CreateCopy (WEBP_LOSSLESS+WEBP_LEVEL warning, JXL, LZMA, ZSTD):
    {
      GDALDatasetUniquePtr src5(
          mem_driver_->Create("", 16, 16, 5, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, src5);
      src5->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand);
      src5->GetRasterBand(4)->SetMetadataItem("ALPHA", "PREMULTIPLIED",
                                              "IMAGE_STRUCTURE");
      CPLStringList co;
      co.SetNameValue("PHOTOMETRIC", "RGB");
      GDALDatasetUniquePtr dst5(gtiff_driver_->CreateCopy(
          kRgb5Path, src5.get(), FALSE, co.List(), nullptr, nullptr));
      ASSERT_NE(nullptr, dst5);

      GDALDatasetUniquePtr src4(
          mem_driver_->Create("", 16, 16, 4, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, src4);
      src4->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand);
      CPLStringList co4;
      co4.SetNameValue("ALPHA", "NO");
      GDALDatasetUniquePtr dst4(gtiff_driver_->CreateCopy(
          kRgb4Path, src4.get(), FALSE, co4.List(), nullptr, nullptr));
      ASSERT_NE(nullptr, dst4);

      // Each codec gets its own output path so that no two open datasets
      // share a /vsimem file.
      for (const char* comp : {"WEBP", "JXL", "LZMA", "ZSTD"}) {
        SCOPED_TRACE(comp);
        const std::string p_codec =
            std::format("/vsimem/gw_cc_codec_{}.tif", comp);
        absl::Cleanup codec_cl = [&] { VSIUnlink(p_codec.c_str()); };
        GDALDatasetUniquePtr s3(
            mem_driver_->Create("", 16, 16, 3, GDT_Byte, nullptr));
        ASSERT_NE(nullptr, s3);
        CPLStringList cco;
        cco.SetNameValue("COMPRESS", comp);
        const std::string_view comp_sv(comp);
        if (comp_sv == "WEBP") {
          cco.SetNameValue("WEBP_LOSSLESS", "YES");
          cco.SetNameValue("WEBP_LEVEL", "80");
        } else if (comp_sv == "LZMA") {
          cco.SetNameValue("LZMA_PRESET", "2");
        } else if (comp_sv == "ZSTD") {
          cco.SetNameValue("ZSTD_LEVEL", "3");
        }
        GDALDatasetUniquePtr d3;
        if (comp_sv == "WEBP") {
          WithQuietHandler quiet;
          CPLErrorReset();
          d3.reset(gtiff_driver_->CreateCopy(p_codec.c_str(), s3.get(), FALSE,
                                             cco.List(), nullptr, nullptr));
          EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("WEBP_LEVEL"));
          CPLErrorReset();
        } else {
          d3.reset(gtiff_driver_->CreateCopy(p_codec.c_str(), s3.get(), FALSE,
                                             cco.List(), nullptr, nullptr));
        }
        ASSERT_NE(nullptr, d3);
      }
    }
  }
}

// TODO: b/335317901 - Cover remaining I/O error branches and uncommon
// PHOTOMETRIC/compression option combinations in gtiffdataset_write.cpp.

}  // namespace
}  // namespace autotest2
