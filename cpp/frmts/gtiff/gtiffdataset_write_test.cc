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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/frmts/gtiff/gtiff.h"
#include "third_party/gdal/frmts/gtiff/gtiffdataset.h"
#include "third_party/gdal/frmts/gtiff/gtiffrasterband.h"
#include "third_party/gdal/frmts/gtiff/tifvsi.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_float.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace {

class GTiffDatasetWriteTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { GDALAllRegister(); }

  void SetUp() override { CPLErrorReset(); }

  static GDALDriver* GetGTiffDriver() {
    return GetGDALDriverManager()->GetDriverByName("GTiff");
  }

  static GDALDriver* GetMemDriver() {
    return GetGDALDriverManager()->GetDriverByName("MEM");
  }
};

// ---------------------------------------------------------------------------
// 1. Option Parsers & CreateLL Validation / Error / Warning Branches
// ---------------------------------------------------------------------------
TEST_F(GTiffDatasetWriteTest, OptionParsersAndCreateLLValidation) {
  CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
  GDALDriver* drv = GetGTiffDriver();
  ASSERT_NE(drv, nullptr);

  // Invalid dimensions / bands. Results are held in unique_ptr so that an
  // unexpected success does not leak the dataset.
  {
    std::unique_ptr<GDALDataset> ds(
        drv->Create("/vsimem/gw_bad_dim1.tif", 0, 10, 1, GDT_Byte, nullptr));
    EXPECT_EQ(ds, nullptr);
  }
  {
    std::unique_ptr<GDALDataset> ds(
        drv->Create("/vsimem/gw_bad_dim2.tif", 10, 0, 1, GDT_Byte, nullptr));
    EXPECT_EQ(ds, nullptr);
  }
  {
    std::unique_ptr<GDALDataset> ds(
        drv->Create("/vsimem/gw_bad_dim3.tif", 10, 10, 0, GDT_Byte, nullptr));
    EXPECT_EQ(ds, nullptr);
  }
  {
    std::unique_ptr<GDALDataset> ds(drv->Create("/vsimem/gw_bad_dim4.tif", 10,
                                                10, 65536, GDT_Byte, nullptr));
    EXPECT_EQ(ds, nullptr);
  }

  // Block size validation.
  {
    CPLStringList opts;
    opts.SetNameValue("BLOCKXSIZE", "-1");
    EXPECT_EQ(
        drv->Create("/vsimem/gw_bx_neg.tif", 16, 16, 1, GDT_Byte, opts.List()),
        nullptr);
  }
  {
    CPLStringList opts;
    opts.SetNameValue("TILED", "YES");
    opts.SetNameValue("BLOCKXSIZE", "15");
    EXPECT_EQ(
        drv->Create("/vsimem/gw_bx_15.tif", 16, 16, 1, GDT_Byte, opts.List()),
        nullptr);
  }
  {
    CPLStringList opts;
    opts.SetNameValue("BLOCKYSIZE", "-1");
    EXPECT_EQ(
        drv->Create("/vsimem/gw_by_neg.tif", 16, 16, 1, GDT_Byte, opts.List()),
        nullptr);
  }
  {
    CPLStringList opts;
    opts.SetNameValue("TILED", "YES");
    opts.SetNameValue("BLOCKYSIZE", "15");
    EXPECT_EQ(
        drv->Create("/vsimem/gw_by_15.tif", 16, 16, 1, GDT_Byte, opts.List()),
        nullptr);
  }
  // Default 256x256 tile size when TILED=YES and BLOCKXSIZE/BLOCKYSIZE omitted,
  // plus invalid PROFILE warning.
  {
    const char* path = "/vsimem/gw_tiled_def.tif";
    absl::Cleanup cl = [&] { VSIUnlink(path); };
    CPLStringList opts;
    opts.SetNameValue("TILED", "YES");
    opts.SetNameValue("PROFILE", "INVALID_PROFILE");
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path, 16, 16, 1, GDT_Byte, opts.List()));
    ASSERT_NE(ds, nullptr);
  }
  // BLOCKXSIZE with TILED=NO warning.
  {
    const char* path = "/vsimem/gw_bx_untiled.tif";
    absl::Cleanup cl = [&] { VSIUnlink(path); };
    CPLStringList opts;
    opts.SetNameValue("TILED", "NO");
    opts.SetNameValue("BLOCKXSIZE", "16");
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path, 16, 16, 1, GDT_Byte, opts.List()));
    ASSERT_NE(ds, nullptr);
  }

  // Invalid INTERLEAVE and COMPRESS.
  {
    CPLStringList opts;
    opts.SetNameValue("INTERLEAVE", "INVALID");
    EXPECT_EQ(
        drv->Create("/vsimem/gw_bad_il.tif", 16, 16, 2, GDT_Byte, opts.List()),
        nullptr);
  }
  {
    CPLStringList opts;
    opts.SetNameValue("COMPRESS", "NO_SUCH_CODEC");
    std::unique_ptr<GDALDataset> ds_unk(drv->Create(
        "/vsimem/gw_bad_comp.tif", 16, 16, 1, GDT_Byte, opts.List()));
    EXPECT_NE(ds_unk, nullptr);
    ds_unk.reset();
    VSIUnlink("/vsimem/gw_bad_comp.tif");
  }

  // WEBP dimension limits (> 16383).
  {
    CPLStringList opts;
    opts.SetNameValue("COMPRESS", "WEBP");
    opts.SetNameValue("TILED", "NO");
    EXPECT_EQ(drv->Create("/vsimem/gw_webp_w.tif", 16400, 1, 3, GDT_Byte,
                          opts.List()),
              nullptr);
  }
  {
    CPLStringList opts;
    opts.SetNameValue("COMPRESS", "WEBP");
    opts.SetNameValue("TILED", "YES");
    opts.SetNameValue("BLOCKXSIZE", "16400");
    opts.SetNameValue("BLOCKYSIZE", "16");
    EXPECT_EQ(
        drv->Create("/vsimem/gw_webp_bx.tif", 16, 16, 3, GDT_Byte, opts.List()),
        nullptr);
  }
  {
    CPLStringList opts;
    opts.SetNameValue("COMPRESS", "WEBP");
    opts.SetNameValue("TILED", "YES");
    opts.SetNameValue("BLOCKXSIZE", "16");
    opts.SetNameValue("BLOCKYSIZE", "16400");
    EXPECT_EQ(
        drv->Create("/vsimem/gw_webp_by.tif", 16, 16, 3, GDT_Byte, opts.List()),
        nullptr);
  }

  // NBITS clamping and warnings across data types.
  const struct {
    GDALDataType dt;
    const char* nbits;
  } nbits_cases[] = {
      {GDT_Byte, "0"},     {GDT_Byte, "9"},     {GDT_Byte, "4"},
      {GDT_UInt16, "8"},   {GDT_UInt16, "17"},  {GDT_UInt16, "12"},
      {GDT_UInt32, "16"},  {GDT_UInt32, "33"},  {GDT_UInt32, "20"},
      {GDT_Float32, "24"}, {GDT_Float32, "16"}, {GDT_Int16, "12"},
  };
  for (size_t i = 0; i < sizeof(nbits_cases) / sizeof(nbits_cases[0]); ++i) {
    std::string path = CPLSPrintf("/vsimem/gw_nbits_%zu.tif", i);
    SCOPED_TRACE(path);
    absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
    CPLStringList opts;
    opts.SetNameValue("NBITS", nbits_cases[i].nbits);
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path.c_str(), 8, 8, 1, nbits_cases[i].dt, opts.List()));
    ASSERT_NE(ds, nullptr);
  }

  // JXL compression validation and options.
  {
    CPLStringList opts;
    opts.SetNameValue("COMPRESS", "JXL");
    EXPECT_EQ(
        drv->Create("/vsimem/gw_jxl_dt.tif", 8, 8, 1, GDT_Int16, opts.List()),
        nullptr);
    opts.SetNameValue("NBITS", "4");
    EXPECT_EQ(
        drv->Create("/vsimem/gw_jxl_nb4.tif", 8, 8, 1, GDT_Byte, opts.List()),
        nullptr);
    opts.SetNameValue("NBITS", "12");
    EXPECT_EQ(drv->Create("/vsimem/gw_jxl_nb12.tif", 8, 8, 1, GDT_UInt16,
                          opts.List()),
              nullptr);
  }
  {
    // Implicit and explicit JXL_LOSSLESS=YES warnings when JXL_DISTANCE /
    // JXL_ALPHA_DISTANCE are specified.
    CPLStringList opts;
    opts.SetNameValue("COMPRESS", "JXL");
    opts.SetNameValue("JXL_DISTANCE", "2.0");
    opts.SetNameValue("JXL_ALPHA_DISTANCE", "1.0");
    std::unique_ptr<GDALDataset> ds1(drv->Create("/vsimem/gw_jxl_warn1.tif", 8,
                                                 8, 2, GDT_Byte, opts.List()));
    ASSERT_NE(ds1, nullptr);
    ds1.reset();
    VSIUnlink("/vsimem/gw_jxl_warn1.tif");

    opts.SetNameValue("JXL_LOSSLESS", "YES");
    std::unique_ptr<GDALDataset> ds2(drv->Create("/vsimem/gw_jxl_warn2.tif", 8,
                                                 8, 2, GDT_Byte, opts.List()));
    ASSERT_NE(ds2, nullptr);
    ds2.reset();
    VSIUnlink("/vsimem/gw_jxl_warn2.tif");

    opts.SetNameValue("COMPRESS", "JXL_DNG_1_7");
    opts.SetNameValue("JXL_LOSSLESS", "NO");
    opts.SetNameValue("JXL_EFFORT", "3");
    std::unique_ptr<GDALDataset> ds3(drv->Create("/vsimem/gw_jxl_lossy.tif", 8,
                                                 8, 4, GDT_Byte, opts.List()));
    ASSERT_NE(ds3, nullptr);
    ds3.reset();
    VSIUnlink("/vsimem/gw_jxl_lossy.tif");
  }

  // PREDICTOR validation.
  {
    CPLStringList opts;
    opts.SetNameValue("COMPRESS", "NONE");
    opts.SetNameValue("PREDICTOR", "2");
    std::unique_ptr<GDALDataset> ds(drv->Create("/vsimem/gw_pred_none.tif", 8,
                                                8, 1, GDT_Byte, opts.List()));
    ASSERT_NE(ds, nullptr);
    ds.reset();
    VSIUnlink("/vsimem/gw_pred_none.tif");
  }
  {
    CPLStringList opts;
    opts.SetNameValue("COMPRESS", "DEFLATE");
    opts.SetNameValue("PREDICTOR", "2");
    opts.SetNameValue("NBITS", "4");
    EXPECT_EQ(
        drv->Create("/vsimem/gw_pred2_b4.tif", 8, 8, 1, GDT_Byte, opts.List()),
        nullptr);
    opts.SetNameValue("NBITS", "12");
    EXPECT_EQ(drv->Create("/vsimem/gw_pred2_b12.tif", 8, 8, 1, GDT_UInt16,
                          opts.List()),
              nullptr);
    opts.SetNameValue("NBITS", "20");
    EXPECT_EQ(drv->Create("/vsimem/gw_pred2_b20.tif", 8, 8, 1, GDT_UInt32,
                          opts.List()),
              nullptr);
  }
  {
    CPLStringList opts;
    opts.SetNameValue("COMPRESS", "DEFLATE");
    opts.SetNameValue("PREDICTOR", "3");
    EXPECT_EQ(drv->Create("/vsimem/gw_pred3_byte.tif", 8, 8, 1, GDT_Byte,
                          opts.List()),
              nullptr);
    std::unique_ptr<GDALDataset> ds(drv->Create(
        "/vsimem/gw_pred3_f32.tif", 8, 8, 1, GDT_Float32, opts.List()));
    ASSERT_NE(ds, nullptr);
    ds.reset();
    VSIUnlink("/vsimem/gw_pred3_f32.tif");

    opts.SetNameValue("PREDICTOR", "99");
    EXPECT_EQ(
        drv->Create("/vsimem/gw_pred99.tif", 8, 8, 1, GDT_Byte, opts.List()),
        nullptr);
  }

  // Codec preset/level option parsers (valid, capped, and out-of-range).
  {
    CPLStringList opts;
    opts.SetNameValue("COMPRESS", "WEBP");
    opts.SetNameValue("WEBP_LEVEL", "0");
    opts.SetNameValue("LZMA_PRESET", "99");
    opts.SetNameValue("ZSTD_LEVEL", "99");
    opts.SetNameValue("ZLEVEL", "99");
    opts.SetNameValue("JPEG_QUALITY", "105");
    std::unique_ptr<GDALDataset> ds1(
        drv->Create("/vsimem/gw_presets1.tif", 8, 8, 3, GDT_Byte, opts.List()));
    ASSERT_NE(ds1, nullptr);
    ds1.reset();
    VSIUnlink("/vsimem/gw_presets1.tif");

    CPLStringList opts2;
    opts2.SetNameValue("COMPRESS", "WEBP");
    opts2.SetNameValue("WEBP_LEVEL", "50");
    opts2.SetNameValue("WEBP_LOSSLESS", "YES");
    std::unique_ptr<GDALDataset> ds2(drv->Create("/vsimem/gw_presets2.tif", 8,
                                                 8, 3, GDT_Byte, opts2.List()));
    ASSERT_NE(ds2, nullptr);
    ds2.reset();
    VSIUnlink("/vsimem/gw_presets2.tif");

    CPLStringList opts3;
    opts3.SetNameValue("COMPRESS", "DEFLATE");
    opts3.SetNameValue("ZLEVEL", "11");
    std::unique_ptr<GDALDataset> ds3(drv->Create("/vsimem/gw_presets3.tif", 8,
                                                 8, 1, GDT_Byte, opts3.List()));
    ASSERT_NE(ds3, nullptr);
    ds3.reset();
    VSIUnlink("/vsimem/gw_presets3.tif");

    CPLStringList opts4;
    opts4.SetNameValue("COMPRESS", "LZMA");
    opts4.SetNameValue("LZMA_PRESET", "3");
    std::unique_ptr<GDALDataset> ds4(drv->Create("/vsimem/gw_presets4.tif", 8,
                                                 8, 1, GDT_Byte, opts4.List()));
    ASSERT_NE(ds4, nullptr);
    ds4.reset();
    VSIUnlink("/vsimem/gw_presets4.tif");

    CPLStringList opts5;
    opts5.SetNameValue("COMPRESS", "LERC_DEFLATE");
    opts5.SetNameValue("ZLEVEL", "5");
    opts5.SetNameValue("MAX_Z_ERROR", "0.5");
    opts5.SetNameValue("MAX_Z_ERROR_OVERVIEW", "1.0");
    std::unique_ptr<GDALDataset> ds5(drv->Create(
        "/vsimem/gw_presets5.tif", 8, 8, 1, GDT_Float32, opts5.List()));
    ASSERT_NE(ds5, nullptr);
    ds5.reset();
    VSIUnlink("/vsimem/gw_presets5.tif");

    CPLStringList opts6;
    opts6.SetNameValue("COMPRESS", "LERC_ZSTD");
    opts6.SetNameValue("ZSTD_LEVEL", "3");
    std::unique_ptr<GDALDataset> ds6(drv->Create(
        "/vsimem/gw_presets6.tif", 8, 8, 1, GDT_Float32, opts6.List()));
    ASSERT_NE(ds6, nullptr);
    ds6.reset();
    VSIUnlink("/vsimem/gw_presets6.tif");
  }

  // STREAMABLE_OUTPUT incompatibilities.
  {
    CPLStringList opts;
    opts.SetNameValue("STREAMABLE_OUTPUT", "YES");
    opts.SetNameValue("COMPRESS", "DEFLATE");
    EXPECT_EQ(
        drv->Create("/vsimem/gw_str_comp.tif", 8, 8, 1, GDT_Byte, opts.List()),
        nullptr);
  }
  {
    CPLStringList opts;
    opts.SetNameValue("STREAMABLE_OUTPUT", "YES");
    opts.SetNameValue("SPARSE_OK", "YES");
    EXPECT_EQ(
        drv->Create("/vsimem/gw_str_sp.tif", 8, 8, 1, GDT_Byte, opts.List()),
        nullptr);
  }
  {
    CPLStringList opts;
    opts.SetNameValue("STREAMABLE_OUTPUT", "YES");
    opts.SetNameValue("COPY_SRC_OVERVIEWS", "YES");
    EXPECT_EQ(
        drv->Create("/vsimem/gw_str_ovr.tif", 8, 8, 1, GDT_Byte, opts.List()),
        nullptr);
  }

  // BIGTIFF=NO > 4GB failure and tile array > 2GB failure (no allocation).
  {
    CPLStringList opts;
    opts.SetNameValue("BIGTIFF", "NO");
    EXPECT_EQ(drv->Create("/vsimem/gw_big_fail.tif", 100000, 50000, 1, GDT_Byte,
                          opts.List()),
              nullptr);
  }
  {
    CPLStringList opts;
    opts.SetNameValue("TILED", "YES");
    opts.SetNameValue("BLOCKXSIZE", "16");
    opts.SetNameValue("BLOCKYSIZE", "16");
    opts.SetNameValue("BIGTIFF", "YES");
    opts.SetNameValue("COMPRESS", "DEFLATE");
    EXPECT_EQ(drv->Create("/vsimem/gw_tile_2gb.tif", 400000, 400000, 1,
                          GDT_Byte, opts.List()),
              nullptr);
  }

  // ENDIANNESS, PIXELTYPE=SIGNEDBYTE, APPEND_SUBDATASET, @SUPPRESS_ASAP,
  // WRITE_EMPTY_TILES_SYNCHRONOUSLY.
  {
    CPLStringList opts;
    opts.SetNameValue("ENDIANNESS", "INVALID_ENDIAN");
    opts.SetNameValue("PIXELTYPE", "SIGNEDBYTE");
    opts.SetNameValue("WRITE_EMPTY_TILES_SYNCHRONOUSLY", "YES");
    opts.SetNameValue("@CREATE_ONLY_VISIBLE_AT_CLOSE_TIME", "YES");
    std::unique_ptr<GDALDataset> ds(
        drv->Create("/vsimem/gw_append.tif", 8, 8, 1, GDT_Byte, opts.List()));
    ASSERT_NE(ds, nullptr);
    GByte v = 1;
    ASSERT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 1, 1, &v, 1, 1,
                                             GDT_Byte, 0, 0, nullptr),
              CE_None);
    ds.reset();

    CPLStringList app_opts;
    app_opts.SetNameValue("APPEND_SUBDATASET", "YES");
    app_opts.SetNameValue("ENDIANNESS", "BIG");
    app_opts.SetNameValue("BIGTIFF", "NO");
    std::unique_ptr<GDALDataset> ds2(drv->Create("/vsimem/gw_append.tif", 8, 8,
                                                 1, GDT_Byte, app_opts.List()));
    ASSERT_NE(ds2, nullptr);
    ds2.reset();
    VSIUnlink("/vsimem/gw_append.tif");

    CPLStringList sup_opts;
    sup_opts.SetNameValue("@SUPPRESS_ASAP", "YES");
    std::unique_ptr<GDALDataset> ds3(drv->Create(
        "/vsimem/gw_suppress.tif", 8, 8, 1, GDT_Byte, sup_opts.List()));
    ASSERT_NE(ds3, nullptr);
    ds3.reset();
  }

  // Sample formats across all types.
  const GDALDataType dts[] = {GDT_Int8,     GDT_CInt16,  GDT_CInt32,
                              GDT_Float16,  GDT_Float64, GDT_CFloat16,
                              GDT_CFloat32, GDT_CFloat64};
  for (GDALDataType dt : dts) {
    SCOPED_TRACE(GDALGetDataTypeName(dt));
    std::string path = "/vsimem/gw_dt_samplefmt.tif";
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path.c_str(), 4, 4, 1, dt, nullptr));
    ASSERT_NE(ds, nullptr);
    ds.reset();
    VSIUnlink(path.c_str());
  }

  // PHOTOMETRIC options & errors.
  {
    const char* photos[] = {"MINISBLACK", "MINISWHITE", "CIELAB",
                            "ICCLAB",     "ITULAB",     "INVALID_PHOTO"};
    for (const char* ph : photos) {
      SCOPED_TRACE(ph);
      CPLStringList opts;
      opts.SetNameValue("PHOTOMETRIC", ph);
      std::unique_ptr<GDALDataset> ds(
          drv->Create("/vsimem/gw_ph.tif", 8, 8, 3, GDT_Byte, opts.List()));
      ASSERT_NE(ds, nullptr);
      ds.reset();
      VSIUnlink("/vsimem/gw_ph.tif");
    }
    // PHOTOMETRIC=RGB with 1 band (warning) and 5 bands (extra samples).
    for (int bands : {1, 4, 5}) {
      SCOPED_TRACE(bands);
      CPLStringList opts;
      opts.SetNameValue("PHOTOMETRIC", "RGB");
      std::unique_ptr<GDALDataset> ds(drv->Create(
          "/vsimem/gw_ph_rgb.tif", 8, 8, bands, GDT_Byte, opts.List()));
      ASSERT_NE(ds, nullptr);
      ds.reset();
      VSIUnlink("/vsimem/gw_ph_rgb.tif");
    }
    // PHOTOMETRIC=CMYK with 4 and 5 bands.
    for (int bands : {4, 5}) {
      SCOPED_TRACE(bands);
      CPLStringList opts;
      opts.SetNameValue("PHOTOMETRIC", "CMYK");
      std::unique_ptr<GDALDataset> ds(drv->Create(
          "/vsimem/gw_ph_cmyk.tif", 8, 8, bands, GDT_Byte, opts.List()));
      ASSERT_NE(ds, nullptr);
      ds.reset();
      VSIUnlink("/vsimem/gw_ph_cmyk.tif");
    }
    // PHOTOMETRIC=PALETTE on GDT_Byte, GDT_UInt16, and GDT_Int16 (warning).
    for (GDALDataType dt : {GDT_Byte, GDT_UInt16, GDT_Int16}) {
      SCOPED_TRACE(GDALGetDataTypeName(dt));
      CPLStringList opts;
      opts.SetNameValue("PHOTOMETRIC", "PALETTE");
      std::unique_ptr<GDALDataset> ds(
          drv->Create("/vsimem/gw_ph_pal.tif", 8, 8, 1, dt, opts.List()));
      ASSERT_NE(ds, nullptr);
      ds.reset();
      VSIUnlink("/vsimem/gw_ph_pal.tif");
    }
    // PHOTOMETRIC=YCBCR error paths and valid path.
    {
      CPLStringList opts;
      opts.SetNameValue("PHOTOMETRIC", "YCBCR");
      EXPECT_EQ(drv->Create("/vsimem/gw_ycbcr1.tif", 16, 16, 3, GDT_Byte,
                            opts.List()),
                nullptr);
      opts.SetNameValue("COMPRESS", "JPEG");
      opts.SetNameValue("INTERLEAVE", "BAND");
      EXPECT_EQ(drv->Create("/vsimem/gw_ycbcr2.tif", 16, 16, 3, GDT_Byte,
                            opts.List()),
                nullptr);
      opts.SetNameValue("INTERLEAVE", "PIXEL");
      EXPECT_EQ(drv->Create("/vsimem/gw_ycbcr3.tif", 16, 16, 2, GDT_Byte,
                            opts.List()),
                nullptr);
      std::unique_ptr<GDALDataset> ds(drv->Create(
          "/vsimem/gw_ycbcr_ok.tif", 16, 16, 3, GDT_Byte, opts.List()));
      ASSERT_NE(ds, nullptr);
      ds.reset();
      VSIUnlink("/vsimem/gw_ycbcr_ok.tif");
    }
  }
}

// ---------------------------------------------------------------------------
// 2. FillEmptyTiles, HasOnlyNoData, and IsFirstPixelEqualToNoData
// ---------------------------------------------------------------------------
TEST_F(GTiffDatasetWriteTest, FillEmptyTilesAndSparseNoData) {
  CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
  GDALDriver* drv = GetGTiffDriver();

  // 2a. FillEmptyTiles with non-zero NoData on GDT_Byte, GDT_Int64, GDT_UInt64.
  for (int mode = 0; mode < 3; ++mode) {
    std::string path = CPLSPrintf("/vsimem/gw_fill_nd_%d.tif", mode);
    SCOPED_TRACE(path);
    absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
    CPLStringList opts;
    opts.SetNameValue("TILED", "YES");
    opts.SetNameValue("BLOCKXSIZE", "16");
    opts.SetNameValue("BLOCKYSIZE", "16");
    GDALDataType dt =
        (mode == 0) ? GDT_Byte : (mode == 1 ? GDT_Int64 : GDT_UInt64);
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path.c_str(), 32, 32, 1, dt, opts.List()));
    ASSERT_NE(ds, nullptr);
    if (mode == 0) {
      ASSERT_EQ(ds->GetRasterBand(1)->SetNoDataValue(42.0), CE_None);
    } else if (mode == 1) {
      ASSERT_EQ(ds->GetRasterBand(1)->SetNoDataValueAsInt64(-123456789LL),
                CE_None);
    } else {
      ASSERT_EQ(ds->GetRasterBand(1)->SetNoDataValueAsUInt64(123456789ULL),
                CE_None);
    }
    // Write only pixel (0,0) in tile 0; leave tiles 1, 2, 3 unwritten so
    // FillEmptyTiles fills them at closing.
    uint64_t one = 1;
    ASSERT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 1, 1, &one, 1, 1,
                                             GDT_UInt64, 0, 0, nullptr),
              CE_None);
    ds.reset();

    std::unique_ptr<GDALDataset> ro(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(ro, nullptr);
    double val = 0.0;
    ASSERT_EQ(ro->GetRasterBand(1)->RasterIO(GF_Read, 20, 20, 1, 1, &val, 1, 1,
                                             GDT_Float64, 0, 0, nullptr),
              CE_None);
    if (mode == 0) {
      EXPECT_DOUBLE_EQ(val, 42.0);
    } else if (mode == 1) {
      EXPECT_DOUBLE_EQ(val, -123456789.0);
    } else {
      EXPECT_DOUBLE_EQ(val, 123456789.0);
    }
  }

  // 2b. FillEmptyTiles with non-power-of-two depth (NBITS=4) and non-zero
  // NoData: both 1-band and 2-band CONTIG with partial edge tiles (20x20).
  for (int bands : {1, 2}) {
    std::string path = CPLSPrintf("/vsimem/gw_fill_oddbits_%d.tif", bands);
    SCOPED_TRACE(path);
    absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
    CPLStringList opts;
    opts.SetNameValue("TILED", "YES");
    opts.SetNameValue("BLOCKXSIZE", "16");
    opts.SetNameValue("BLOCKYSIZE", "16");
    opts.SetNameValue("NBITS", "4");
    opts.SetNameValue("INTERLEAVE", "PIXEL");
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path.c_str(), 20, 20, bands, GDT_Byte, opts.List()));
    ASSERT_NE(ds, nullptr);
    ASSERT_EQ(ds->GetRasterBand(1)->SetNoDataValue(7.0), CE_None);
    GByte px = 3;
    ASSERT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 1, 1, &px, 1, 1,
                                             GDT_Byte, 0, 0, nullptr),
              CE_None);
    ds.reset();

    std::unique_ptr<GDALDataset> ro(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(ro, nullptr);
    GByte out = 0;
    ASSERT_EQ(ro->GetRasterBand(1)->RasterIO(GF_Read, 18, 18, 1, 1, &out, 1, 1,
                                             GDT_Byte, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(out, 7);
  }

  // 2c. FillEmptyTiles with COMPRESS=NONE and 0/unset NoData (tiled & stripped,
  // >= 4 blocks with only block 0 written -> seeks to EOF and truncates).
  for (bool tiled : {true, false}) {
    std::string path = CPLSPrintf("/vsimem/gw_fill_zero_%d.tif", tiled ? 1 : 0);
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
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path.c_str(), 32, 32, 1, GDT_Byte, opts.List()));
    ASSERT_NE(ds, nullptr);
    GByte px = 9;
    ASSERT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 1, 1, &px, 1, 1,
                                             GDT_Byte, 0, 0, nullptr),
              CE_None);
    ds.reset();
  }

  // 2d. FillEmptyTiles with COMPRESS=DEFLATE and 0/unset NoData (>= 4 tiles,
  // only tile 0 written -> compresses 1 empty tile and reuses raw bytes via
  // WriteRawStripOrTile).
  {
    std::string path = "/vsimem/gw_fill_deflate.tif";
    absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
    CPLStringList opts;
    opts.SetNameValue("TILED", "YES");
    opts.SetNameValue("BLOCKXSIZE", "16");
    opts.SetNameValue("BLOCKYSIZE", "16");
    opts.SetNameValue("COMPRESS", "DEFLATE");
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path.c_str(), 32, 32, 1, GDT_Byte, opts.List()));
    ASSERT_NE(ds, nullptr);
    GByte px = 9;
    ASSERT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 1, 1, &px, 1, 1,
                                             GDT_Byte, 0, 0, nullptr),
              CE_None);
    ds.reset();
  }

  // 2e. HasOnlyNoData & IsFirstPixelEqualToNoData with SPARSE_OK=YES across
  // all data types (tiled & stripped, including NaN NoData and Int64/UInt64).
  const GDALDataType sparse_types[] = {GDT_Byte,    GDT_Int8,   GDT_UInt16,
                                       GDT_Int16,   GDT_UInt32, GDT_Int32,
                                       GDT_UInt64,  GDT_Int64,  GDT_Float32,
                                       GDT_Float64, GDT_CInt16, GDT_CFloat32};
  for (bool tiled : {true, false}) {
    for (GDALDataType dt : sparse_types) {
      for (int nan_mode = 0;
           nan_mode < ((dt == GDT_Float32 || dt == GDT_Float64 ||
                        dt == GDT_Int64 || dt == GDT_UInt64)
                           ? 2
                           : 1);
           ++nan_mode) {
        std::string path =
            CPLSPrintf("/vsimem/gw_sparse_%d_%d_%d.tif", tiled ? 1 : 0,
                       static_cast<int>(dt), nan_mode);
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
        std::unique_ptr<GDALDataset> ds(
            drv->Create(path.c_str(), 16, 16, 1, dt, opts.List()));
        ASSERT_NE(ds, nullptr);

        std::vector<double> buf(16 * 16, 0.0);
        if ((dt == GDT_Float32 || dt == GDT_Float64) && nan_mode == 1) {
          const double nan_val = std::numeric_limits<double>::quiet_NaN();
          ASSERT_EQ(ds->GetRasterBand(1)->SetNoDataValue(nan_val), CE_None);
          std::fill(buf.begin(), buf.end(), nan_val);
        } else if (dt == GDT_Int64 && nan_mode == 1) {
          ASSERT_EQ(ds->GetRasterBand(1)->SetNoDataValueAsInt64(-50), CE_None);
          std::fill(buf.begin(), buf.end(), -50.0);
        } else if (dt == GDT_UInt64 && nan_mode == 1) {
          ASSERT_EQ(ds->GetRasterBand(1)->SetNoDataValueAsUInt64(50), CE_None);
          std::fill(buf.begin(), buf.end(), 50.0);
        } else {
          ASSERT_EQ(ds->GetRasterBand(1)->SetNoDataValue(5.0), CE_None);
          std::fill(buf.begin(), buf.end(), 5.0);
        }

        ASSERT_EQ(
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 16, 16, buf.data(),
                                           16, 16, GDT_Float64, 0, 0, nullptr),
            CE_None);
        ds->FlushCache(false);
        ds.reset();

        // An all-nodata block must not have been materialized on disk.
        std::unique_ptr<GDALDataset> rd(
            GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
        ASSERT_NE(rd, nullptr);
        SCOPED_TRACE(path);
        EXPECT_EQ(rd->GetMetadataItem("BLOCK_OFFSET_0_0", "TIFF"), nullptr);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 3. WriteDealWithLercAndNan, WriteEncodedTile, WriteEncodedStrip, Streaming
// ---------------------------------------------------------------------------
TEST_F(GTiffDatasetWriteTest, WriteEncodedTileAndStripEdgeCases) {
  CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
  GDALDriver* drv = GetGTiffDriver();

  // 3a. WriteDealWithLercAndNan: partial tiles (20x20 with 16x16 tiles) and
  // strips (16x10 with 8-row strips) on GDT_Float32 & GDT_Float64, 2-band
  // INTERLEAVE=PIXEL with NaN pixels.
  for (bool tiled : {true, false}) {
    for (GDALDataType dt : {GDT_Float32, GDT_Float64}) {
      std::string path = CPLSPrintf("/vsimem/gw_lerc_nan_%d_%d.tif",
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
      std::unique_ptr<GDALDataset> ds(
          drv->Create(path.c_str(), 20, 20, 2, dt, opts.List()));
      ASSERT_NE(ds, nullptr);
      std::vector<double> buf(20 * 20, 1.5);
      buf[0] = std::numeric_limits<double>::quiet_NaN();
      for (int b = 1; b <= 2; ++b) {
        ASSERT_EQ(
            ds->GetRasterBand(b)->RasterIO(GF_Write, 0, 0, 20, 20, buf.data(),
                                           20, 20, GDT_Float64, 0, 0, nullptr),
            CE_None);
      }
      ds.reset();
    }
  }

  // 3b. Partial JPEG 8-bit edge tile padding (20x20 with 16x16 tiles) +
  // ENDIANNESS=INVERTED byte-swapped temp buffer in WriteEncodedTile/Strip.
  {
    std::string path = "/vsimem/gw_jpeg_partial.tif";
    absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
    CPLStringList opts;
    opts.SetNameValue("COMPRESS", "JPEG");
    opts.SetNameValue("TILED", "YES");
    opts.SetNameValue("BLOCKXSIZE", "16");
    opts.SetNameValue("BLOCKYSIZE", "16");
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path.c_str(), 20, 20, 3, GDT_Byte, opts.List()));
    ASSERT_NE(ds, nullptr);
    std::vector<GByte> buf(20 * 20 * 3, 120);
    ASSERT_EQ(ds->RasterIO(GF_Write, 0, 0, 20, 20, buf.data(), 20, 20, GDT_Byte,
                           3, nullptr, 0, 0, 0, nullptr),
              CE_None);
    ds.reset();
  }
  for (bool tiled : {true, false}) {
    std::string path =
        CPLSPrintf("/vsimem/gw_inv_endian_%d.tif", tiled ? 1 : 0);
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
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path.c_str(), 20, 20, 1, GDT_UInt16, opts.List()));
    ASSERT_NE(ds, nullptr);
    std::vector<uint16_t> buf(20 * 20, 0x1234);
    ASSERT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 20, 20, buf.data(),
                                             20, 20, GDT_UInt16, 0, 0, nullptr),
              CE_None);
    ds.reset();
  }

  // 3c. STREAMABLE_OUTPUT=YES (tiled & stripped): sequential write success,
  // out-of-order block write failure, and post-crystalize SetSpatialRef /
  // SetGeoTransform / SetMetadata / SetMetadataItem errors.
  for (bool tiled : {true, false}) {
    std::string path =
        CPLSPrintf("/vsimem/gw_stream_out_%d.tif", tiled ? 1 : 0);
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
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path.c_str(), 32, 20, 1, GDT_Byte, opts.List()));
    ASSERT_NE(ds, nullptr);
    std::vector<GByte> block(32 * 16, 77);
    // Out-of-order write of block (0, 1) before (0, 0) fails!
    EXPECT_NE(ds->GetRasterBand(1)->WriteBlock(0, 1, block.data()), CE_None);
    // Now write block 0 so it is crystalized, then verify setters fail on
    // streamed output after crystalize.
    EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, block.data()), CE_None);
    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    EXPECT_EQ(ds->SetSpatialRef(&srs), CE_Failure);
    GDALGeoTransform gt{0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    EXPECT_EQ(ds->SetGeoTransform(gt), CE_Failure);
    EXPECT_EQ(ds->SetMetadata(nullptr), CE_Failure);
    EXPECT_EQ(ds->SetMetadataItem("FOO", "BAR"), CE_Failure);
    ds.reset();
  }
}

// ---------------------------------------------------------------------------
// 4. Multi-Threaded Compression & COG WriteRawStripOrTile In-Place / Break
// ---------------------------------------------------------------------------
TEST_F(GTiffDatasetWriteTest, MultiThreadedCompressionAndCOGWriteRaw) {
  CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
  GDALDriver* drv = GetGTiffDriver();

  // 4a. NUM_THREADS invalid warnings and multi-threaded compression with
  // DEFLATE+PREDICTOR+EXTRASAMPLES and LERC, plus WaitCompletionForBlock and
  // uncompressed overview flushing parent queue.
  {
    CPLStringList bad_opts;
    bad_opts.SetNameValue("NUM_THREADS", "INVALID_THREADS");
    std::unique_ptr<GDALDataset> ds_bad(drv->Create(
        "/vsimem/gw_mt_bad.tif", 32, 32, 1, GDT_Byte, bad_opts.List()));
    ASSERT_NE(ds_bad, nullptr);
    ds_bad.reset();
    VSIUnlink("/vsimem/gw_mt_bad.tif");
  }
  for (int codec = 0; codec < 2; ++codec) {
    std::string path = CPLSPrintf("/vsimem/gw_mt_%d.tif", codec);
    SCOPED_TRACE(path);
    absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
    CPLStringList opts;
    opts.SetNameValue("TILED", "YES");
    opts.SetNameValue("BLOCKXSIZE", "16");
    opts.SetNameValue("BLOCKYSIZE", "16");
    opts.SetNameValue("NUM_THREADS", "2");
    if (codec == 0) {
      opts.SetNameValue("COMPRESS", "DEFLATE");
      opts.SetNameValue("PREDICTOR", "2");
      opts.SetNameValue("ENDIANNESS", "BIG");
    } else {
      opts.SetNameValue("COMPRESS", "LERC");
    }
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path.c_str(), 64, 64, 4, GDT_Byte, opts.List()));
    ASSERT_NE(ds, nullptr);

    // Also create an uncompressed overview so writing to the overview while
    // base DS has a compression queue exercises the m_nCompression == NONE
    // WaitCompletion branch in SubmitCompressionJob.
    int ovr_level = 2;
    CPLStringList ovr_opts;
    ovr_opts.SetNameValue("COMPRESS_OVERVIEW", "NONE");
    ASSERT_EQ(ds->BuildOverviews("NEAREST", 1, &ovr_level, 0, nullptr, nullptr,
                                 nullptr, ovr_opts.List()),
              CE_None);

    std::vector<GByte> tile(16 * 16 * 4, 42);
    for (int ty = 0; ty < 4; ++ty) {
      for (int tx = 0; tx < 4; ++tx) {
        tile[0] = static_cast<GByte>(ty * 4 + tx);
        ASSERT_EQ(ds->RasterIO(GF_Write, tx * 16, ty * 16, 16, 16, tile.data(),
                               16, 16, GDT_Byte, 4, nullptr, 0, 0, 0, nullptr),
                  CE_None);
      }
    }
    // Trigger WaitCompletionForBlock before FlushCache by querying block
    // coverage status and writing to the uncompressed overview.
    (void)ds->GetRasterBand(1)->GetDataCoverageStatus(0, 0, 64, 64, 0, nullptr);
    GDALRasterBand* ovr_band = ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(ovr_band, nullptr);
    int ovr_bx = 0;
    int ovr_by = 0;
    ovr_band->GetBlockSize(&ovr_bx, &ovr_by);
    // The overview block may be larger than the base tile; size the buffer
    // to the overview's actual block size.
    std::vector<GByte> ovr_block(static_cast<size_t>(ovr_bx) * ovr_by, 42);
    ASSERT_EQ(ovr_band->WriteBlock(0, 0, ovr_block.data()), CE_None);
    ds->FlushCache(false);
  }

  // 4b. COG in-place vs out-of-place tile rewrite (BLOCK_ORDER and
  // MASK_INTERLEAVED_WITH_IMAGERY invalidation + leader/trailer handling).
  for (bool with_mask : {false, true}) {
    std::string path =
        CPLSPrintf("/vsimem/gw_cog_edit_%d.tif", with_mask ? 1 : 0);
    SCOPED_TRACE(path);
    absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };

    std::unique_ptr<GDALDataset> src(
        GetMemDriver()->Create("", 32, 32, 1, GDT_Byte, nullptr));
    ASSERT_NE(src, nullptr);
    std::vector<GByte> initial(32 * 32);
    for (size_t i = 0; i < initial.size(); ++i) {
      initial[i] = static_cast<GByte>((i * 7) % 13);
    }
    ASSERT_EQ(
        src->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 32, 32, initial.data(),
                                        32, 32, GDT_Byte, 0, 0, nullptr),
        CE_None);
    if (with_mask) {
      ASSERT_EQ(src->CreateMaskBand(GMF_PER_DATASET), CE_None);
      ASSERT_EQ(src->GetRasterBand(1)->GetMaskBand()->RasterIO(
                    GF_Write, 0, 0, 32, 32, initial.data(), 32, 32, GDT_Byte, 0,
                    0, nullptr),
                CE_None);
    }
    int ovr = 2;
    ASSERT_EQ(
        src->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr, nullptr, nullptr),
        CE_None);

    CPLStringList copy_opts;
    copy_opts.SetNameValue("TILED", "YES");
    copy_opts.SetNameValue("BLOCKXSIZE", "16");
    copy_opts.SetNameValue("BLOCKYSIZE", "16");
    copy_opts.SetNameValue("COPY_SRC_OVERVIEWS", "YES");
    copy_opts.SetNameValue("COMPRESS", "DEFLATE");
    std::unique_ptr<GDALDataset> cog(drv->CreateCopy(
        path.c_str(), src.get(), FALSE, copy_opts.List(), nullptr, nullptr));
    ASSERT_NE(cog, nullptr);
    cog.reset();

    // Reopen in GA_Update with IGNORE_COG_LAYOUT_BREAK=YES.
    const char* const open_opts[] = {"IGNORE_COG_LAYOUT_BREAK=YES", nullptr};
    std::unique_ptr<GDALDataset> upd(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE,
                          nullptr, open_opts, nullptr));
    ASSERT_NE(upd, nullptr);

    // 1) Write all-zeros tile (smaller compressed size than original).
    // Without mask: rewrites in place with leader + trailer.
    // With mask: size != panByteCounts -> invalidates
    // MASK_INTERLEAVED_WITH_IMAGERY and zeros out the 4-byte leader!
    std::vector<GByte> zeros(16 * 16, 0);
    ASSERT_EQ(upd->GetRasterBand(1)->WriteBlock(0, 0, zeros.data()), CE_None);
    upd->FlushCache(false);

    // 2) Write high-entropy tile (larger compressed size than original) ->
    // invalidates BLOCK_ORDER optimization and appends at EOF.
    std::vector<GByte> random_tile(16 * 16);
    uint32_t seed = 123456789U;
    for (size_t i = 0; i < random_tile.size(); ++i) {
      seed = seed * 1664525U + 1013904223U;
      random_tile[i] = static_cast<GByte>((seed >> 16) & 0xFF);
    }
    ASSERT_EQ(upd->GetRasterBand(1)->WriteBlock(1, 0, random_tile.data()),
              CE_None);
    upd->FlushCache(false);
  }
}

// ---------------------------------------------------------------------------
// 5. DiscardLsb & GetDiscardLsbOption Across All 11 Data Types
// ---------------------------------------------------------------------------
TEST_F(GTiffDatasetWriteTest, DiscardLsbAllTypesAndWarnings) {
  CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
  GDALDriver* drv = GetGTiffDriver();

  // 5a. GetDiscardLsbOption warning branches.
  {
    CPLStringList opts;
    opts.SetNameValue("PHOTOMETRIC", "PALETTE");
    opts.SetNameValue("DISCARD_LSB", "2");
    std::unique_ptr<GDALDataset> ds1(
        drv->Create("/vsimem/gw_dlsb_pal.tif", 8, 8, 1, GDT_Byte, opts.List()));
    ASSERT_NE(ds1, nullptr);
    ds1.reset();
    VSIUnlink("/vsimem/gw_dlsb_pal.tif");
  }
  {
    CPLStringList opts;
    opts.SetNameValue("NBITS", "4");
    opts.SetNameValue("DISCARD_LSB", "2");
    std::unique_ptr<GDALDataset> ds2(
        drv->Create("/vsimem/gw_dlsb_nb4.tif", 8, 8, 1, GDT_Byte, opts.List()));
    ASSERT_NE(ds2, nullptr);
    ds2.reset();
    VSIUnlink("/vsimem/gw_dlsb_nb4.tif");
  }
  {
    CPLStringList opts;
    opts.SetNameValue("DISCARD_LSB", "1,2,3");
    std::unique_ptr<GDALDataset> ds3(
        drv->Create("/vsimem/gw_dlsb_tok.tif", 8, 8, 2, GDT_Byte, opts.List()));
    ASSERT_NE(ds3, nullptr);
    ds3.reset();
    VSIUnlink("/vsimem/gw_dlsb_tok.tif");
  }
  {
    CPLStringList opts;
    opts.SetNameValue("DISCARD_LSB", "8");
    std::unique_ptr<GDALDataset> ds4(
        drv->Create("/vsimem/gw_dlsb_rng.tif", 8, 8, 1, GDT_Byte, opts.List()));
    ASSERT_NE(ds4, nullptr);
    ds4.reset();
    VSIUnlink("/vsimem/gw_dlsb_rng.tif");
  }

  // 5b. Functional DiscardLsb on all 11 supported types in both
  // INTERLEAVE=PIXEL and INTERLEAVE=BAND, exercising exact nodata, rounding
  // onto nodata (AdjustValue), max-value clamping, negative signed values, 255
  // on Byte, NaN/Inf on floats, and out-of-range nodata values.
  const GDALDataType dlsb_types[] = {
      GDT_Byte,  GDT_Int8,   GDT_Int16,   GDT_UInt16,  GDT_Int32,  GDT_UInt32,
      GDT_Int64, GDT_UInt64, GDT_Float16, GDT_Float32, GDT_Float64};

  for (GDALDataType dt : dlsb_types) {
    for (const char* interleave : {"PIXEL", "BAND"}) {
      for (int nd_mode = 0; nd_mode < 3; ++nd_mode) {
        std::string path =
            CPLSPrintf("/vsimem/gw_dlsb_%d_%s_%d.tif", static_cast<int>(dt),
                       interleave, nd_mode);
        absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
        CPLStringList opts;
        opts.SetNameValue("INTERLEAVE", interleave);
        opts.SetNameValue("DISCARD_LSB", "2,2");
        opts.SetNameValue("COMPRESS", "DEFLATE");
        std::unique_ptr<GDALDataset> ds(
            drv->Create(path.c_str(), 16, 1, 2, dt, opts.List()));
        ASSERT_NE(ds, nullptr);

        if (nd_mode == 0) {
          // NoData = 8.0: values 7 and 9 round/mask to 8.0 and trigger
          // AdjustValue(8, 2) -> 12!
          ASSERT_EQ(ds->GetRasterBand(1)->SetNoDataValue(8.0), CE_None);
        } else if (nd_mode == 1) {
          // NoData near max so AdjustValueInt hits the >= max - (round << 1)
          // subtraction branch!
          double max_nd = 124.0;
          if (dt == GDT_Byte)
            max_nd = 252.0;
          else if (dt == GDT_Int8)
            max_nd = 124.0;
          else if (dt == GDT_Int16)
            max_nd = 32764.0;
          else if (dt == GDT_UInt16)
            max_nd = 65532.0;
          else if (dt == GDT_Int32)
            max_nd = 2147483644.0;
          else if (dt == GDT_UInt32)
            max_nd = 4294967292.0;
          ASSERT_EQ(ds->GetRasterBand(1)->SetNoDataValue(max_nd), CE_None);
        } else {
          // Out-of-range NoData for integer types (triggers bHasNoData =
          // false).
          ASSERT_EQ(ds->GetRasterBand(1)->SetNoDataValue(1e100), CE_None);
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
          ASSERT_EQ(
              ds->GetRasterBand(b)->RasterIO(GF_Write, 0, 0, 16, 1, vals, 16, 1,
                                             GDT_Float64, 0, 0, nullptr),
              CE_None);
        }
        if (dt == GDT_Int64) {
          int64_t i64_vals[16] = {0,
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
                                  16};
          for (int b = 1; b <= 2; ++b) {
            ASSERT_EQ(
                ds->GetRasterBand(b)->RasterIO(GF_Write, 0, 0, 16, 1, i64_vals,
                                               16, 1, GDT_Int64, 0, 0, nullptr),
                CE_None);
          }
        } else if (dt == GDT_UInt64) {
          uint64_t u64_vals[16] = {0,
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
                                   18};
          for (int b = 1; b <= 2; ++b) {
            ASSERT_EQ(ds->GetRasterBand(b)->RasterIO(GF_Write, 0, 0, 16, 1,
                                                     u64_vals, 16, 1,
                                                     GDT_UInt64, 0, 0, nullptr),
                      CE_None);
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
        std::unique_ptr<GDALDataset> rd(
            GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
        ASSERT_NE(rd, nullptr);
        const bool is_float = GDALDataTypeIsFloating(dt);
        const bool is_signed = GDALDataTypeIsSigned(dt);
        const double collide = (nd_mode == 0) ? 12.0 : 8.0;
        for (int b = 1; b <= 2; ++b) {
          SCOPED_TRACE(CPLSPrintf("dt=%s interleave=%s nd_mode=%d band=%d",
                                  GDALGetDataTypeName(dt), interleave, nd_mode,
                                  b));
          double got[16] = {};
          ASSERT_EQ(rd->GetRasterBand(b)->RasterIO(GF_Read, 0, 0, 16, 1, got,
                                                   16, 1, GDT_Float64, 0, 0,
                                                   nullptr),
                    CE_None);
          EXPECT_EQ(got[0], 0.0);
          EXPECT_EQ(got[1], 8.0);
          if (is_float) {
            EXPECT_EQ(got[2], 9.0);
            EXPECT_EQ(got[3], 7.0);
            EXPECT_EQ(got[4], 10.0);
            EXPECT_EQ(got[5], 11.0);
            EXPECT_TRUE(std::isnan(got[14]));
            EXPECT_TRUE(std::isinf(got[15]));
          } else {
            EXPECT_EQ(got[2], collide);
            EXPECT_EQ(got[3], collide);
            if (dt == GDT_Int64) {
              // i64_vals[4..5] = {-7, -8}: negative values are only masked.
              EXPECT_EQ(got[4], -8.0);
              EXPECT_EQ(got[5], -8.0);
            } else {
              EXPECT_EQ(got[4], 12.0);
              EXPECT_EQ(got[5], 12.0);
              if (dt != GDT_UInt64) {
                // vals[6..7] = {-7, -8}: masked for signed types, clamped to
                // 0 on write for unsigned types.
                EXPECT_EQ(got[6], is_signed ? -8.0 : 0.0);
                EXPECT_EQ(got[7], is_signed ? -8.0 : 0.0);
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
static int CPL_STDCALL FailingProgress(double, const char*, void*) { return 0; }

TEST_F(GTiffDatasetWriteTest, FlushDirectoryAndOverviews) {
  CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
  GDALDriver* drv = GetGTiffDriver();

  // 6a. FlushDirectory: ReloadAllOtherDirectories (base + mask + overview +
  // overview mask), unsetting RPC, and DeleteNoDataValue (UnsetNoDataValue).
  {
    std::string path = "/vsimem/gw_flush_dir.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(path.c_str());
      VSIUnlink("/vsimem/gw_flush_dir.RPB");
      VSIUnlink("/vsimem/gw_flush_dir_RPC.TXT");
    };
    CPLStringList opts;
    opts.SetNameValue("TILED", "YES");
    opts.SetNameValue("BLOCKXSIZE", "16");
    opts.SetNameValue("BLOCKYSIZE", "16");
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path.c_str(), 32, 32, 1, GDT_Byte, opts.List()));
    ASSERT_NE(ds, nullptr);
    ASSERT_EQ(ds->GetRasterBand(1)->SetNoDataValue(99.0), CE_None);
    ASSERT_EQ(ds->CreateMaskBand(GMF_PER_DATASET), CE_None);
    int ovr = 2;
    ASSERT_EQ(
        ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr, nullptr, nullptr),
        CE_None);

    // Set RPC metadata, crystalize/flush, then unset RPC and NoData!
    const char* const rpc_md[] = {
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
        "SAMP_DEN_COEFF=1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0",
        nullptr};
    ASSERT_EQ(ds->SetMetadata(const_cast<char**>(rpc_md), "RPC"), CE_None);
    GByte v = 5;
    ASSERT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 1, 1, &v, 1, 1,
                                             GDT_Byte, 0, 0, nullptr),
              CE_None);
    ds->FlushCache(false);

    // Unset RPC and NoData, forcing TIFFRewriteDirectory &
    // ReloadAllOtherDirectories.
    ASSERT_EQ(ds->SetMetadata(nullptr, "RPC"), CE_None);
    ASSERT_EQ(ds->GetRasterBand(1)->DeleteNoDataValue(), CE_None);
    ds->FlushCache(false);

    // Also modify overview metadata so ReloadAllOtherDirectories runs when
    // flushing the overview (poBaseDS != this).
    GDALRasterBand* ovr_band = ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(ovr_band, nullptr);
    GDALDataset* ovr_ds = ovr_band->GetDataset();
    ASSERT_NE(ovr_ds, nullptr);
    ASSERT_EQ(ovr_ds->SetMetadataItem("OVR_KEY", "OVR_VAL"), CE_None);
    ovr_ds->FlushCache(false);

    // CleanOverviews when overviews & mask overviews exist.
    ASSERT_EQ(
        ds->BuildOverviews("NONE", 0, nullptr, 0, nullptr, nullptr, nullptr),
        CE_None);
    // CleanOverviews when 0 overviews exist.
    ASSERT_EQ(
        ds->BuildOverviews("NONE", 0, nullptr, 0, nullptr, nullptr, nullptr),
        CE_None);
  }

  // 6b. IBuildOverviews: options, duplicate 1x1 skip, resampling update,
  // SEPARATE band-by-band path, AVERAGE_BIT2GRAYSCALE, palette (8/4/16-bit),
  // external mask (.msk), and error/external paths.
  {
    // Palette 8-bit, 4-bit, and 16-bit overview building.
    for (int depth : {8, 4, 16}) {
      std::string path = CPLSPrintf("/vsimem/gw_ovr_pal_%d.tif", depth);
      SCOPED_TRACE(path);
      absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
      CPLStringList opts;
      if (depth == 4) opts.SetNameValue("NBITS", "4");
      GDALDataType dt = (depth == 16) ? GDT_UInt16 : GDT_Byte;
      std::unique_ptr<GDALDataset> ds(
          drv->Create(path.c_str(), 16, 16, 1, dt, opts.List()));
      ASSERT_NE(ds, nullptr);
      GDALColorTable ct;
      GDALColorEntry ce = {10, 20, 30, 255};
      ct.SetColorEntry(0, &ce);
      ASSERT_EQ(ds->GetRasterBand(1)->SetColorTable(&ct), CE_None);
      int ovr = 2;
      ASSERT_EQ(
          ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr, nullptr, nullptr),
          CE_None);
    }

    // NBITS=1 with AVERAGE_BIT2GRAYSCALE and duplicate 1x1 overview levels.
    {
      std::string path = "/vsimem/gw_ovr_bit2gray.tif";
      absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
      CPLStringList opts;
      opts.SetNameValue("NBITS", "1");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(path.c_str(), 8, 8, 1, GDT_Byte, opts.List()));
      ASSERT_NE(ds, nullptr);
      int levels[] = {2, 8, 16};
      ASSERT_EQ(ds->BuildOverviews("AVERAGE_BIT2GRAYSCALE", 3, levels, 0,
                                   nullptr, nullptr, nullptr),
                CE_None);
      EXPECT_EQ(ds->GetRasterBand(1)->GetOverviewCount(), 2);
      // Rebuild with NEAREST to update RESAMPLING metadata item on existing
      // overview.
      ASSERT_EQ(ds->BuildOverviews("NEAREST", 2, levels, 0, nullptr, nullptr,
                                   nullptr),
                CE_None);
    }

    // Overview options (WEBP, ZLEVEL, ZSTD, LERC, JXL, JPEG, PREDICTOR,
    // INTERLEAVE, SPARSE_OK) and band-by-band SEPARATE regeneration.
    {
      std::string path = "/vsimem/gw_ovr_opts.tif";
      absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
      CPLStringList opts;
      opts.SetNameValue("INTERLEAVE", "BAND");
      opts.SetNameValue("WEBP_LOSSLESS", "YES");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(path.c_str(), 32, 32, 3, GDT_Byte, opts.List()));
      ASSERT_NE(ds, nullptr);
      ASSERT_EQ(ds->GetRasterBand(1)->SetNoDataValue(0.0), CE_None);

      // Subset of bands fails.
      int one_band = 1;
      int ovr2 = 2;
      EXPECT_EQ(ds->BuildOverviews("NEAREST", 1, &ovr2, 1, &one_band, nullptr,
                                   nullptr),
                CE_Failure);
      // Progress termination fails.
      EXPECT_EQ(ds->BuildOverviews("NEAREST", 1, &ovr2, 0, nullptr,
                                   FailingProgress, nullptr),
                CE_Failure);

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
      ASSERT_EQ(ds->BuildOverviews("AVERAGE", 1, &ovr2, 0, nullptr, nullptr,
                                   nullptr, ovr_opts.List()),
                CE_None);

      // Adding external overviews when internal overviews already exist fails.
      CPLStringList ext_opts;
      ext_opts.SetNameValue("LOCATION", "EXTERNAL");
      EXPECT_EQ(ds->BuildOverviews("NEAREST", 1, &ovr2, 0, nullptr, nullptr,
                                   nullptr, ext_opts.List()),
                CE_Failure);
    }

    // External mask (.msk) overview generation + LOCATION=EXTERNAL /
    // LOCATION=INTERNAL on RO.
    {
      std::string path = "/vsimem/gw_ovr_extmask.tif";
      absl::Cleanup cl = [&] {
        VSIUnlink(path.c_str());
        VSIUnlink("/vsimem/gw_ovr_extmask.tif.msk");
        VSIUnlink("/vsimem/gw_ovr_extmask.tif.ovr");
      };
      CPLConfigOptionSetter ext_mask("GDAL_TIFF_INTERNAL_MASK", "NO", false);
      std::unique_ptr<GDALDataset> ds(
          drv->Create(path.c_str(), 16, 16, 1, GDT_Byte, nullptr));
      ASSERT_NE(ds, nullptr);
      ASSERT_EQ(ds->CreateMaskBand(GMF_PER_DATASET), CE_None);
      int ovr = 2;
      ASSERT_EQ(
          ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr, nullptr, nullptr),
          CE_None);
    }
    {
      std::string path = "/vsimem/gw_ovr_ro.tif";
      absl::Cleanup cl = [&] {
        VSIUnlink(path.c_str());
        VSIUnlink("/vsimem/gw_ovr_ro.tif.ovr");
      };
      CPLStringList opts;
      opts.SetNameValue("SPARSE_OK", "YES");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(path.c_str(), 16, 16, 1, GDT_Byte, opts.List()));
      ASSERT_NE(ds, nullptr);
      ASSERT_EQ(ds->CreateMaskBand(GMF_PER_DATASET), CE_None);
      ds.reset();

      std::unique_ptr<GDALDataset> ro(
          GDALDataset::Open(path.c_str(), GDAL_OF_RASTER));
      ASSERT_NE(ro, nullptr);
      int ovr = 2;
      CPLStringList int_opts;
      int_opts.SetNameValue("LOCATION", "INTERNAL");
      EXPECT_EQ(ro->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr, nullptr,
                                   nullptr, int_opts.List()),
                CE_Failure);
      // Read-only without LOCATION builds external .ovr and warns about
      // internal mask.
      EXPECT_EQ(
          ro->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr, nullptr, nullptr),
          CE_None);
    }
  }

  // 6c. AddOverviews (GA_ReadOnly and GA_Update, including reopened COG).
  {
    std::string path = "/vsimem/gw_add_ovr.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(path.c_str());
      VSIUnlink("/vsimem/gw_add_ovr.tif.ovr");
    };
    std::unique_ptr<GDALDataset> src(
        GetMemDriver()->Create("", 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(src, nullptr);
    GDALColorTable ct;
    GDALColorEntry ce = {50, 100, 150, 255};
    ct.SetColorEntry(0, &ce);
    ASSERT_EQ(src->GetRasterBand(1)->SetColorTable(&ct), CE_None);
    int ovr2 = 2;
    ASSERT_EQ(
        src->BuildOverviews("NEAREST", 1, &ovr2, 0, nullptr, nullptr, nullptr),
        CE_None);

    CPLStringList copy_opts;
    copy_opts.SetNameValue("TILED", "YES");
    copy_opts.SetNameValue("BLOCKXSIZE", "16");
    copy_opts.SetNameValue("BLOCKYSIZE", "16");
    copy_opts.SetNameValue("COPY_SRC_OVERVIEWS", "YES");
    std::unique_ptr<GDALDataset> cog(drv->CreateCopy(
        path.c_str(), src.get(), FALSE, copy_opts.List(), nullptr, nullptr));
    ASSERT_NE(cog, nullptr);
    cog.reset();

    const char* const open_opts[] = {"IGNORE_COG_LAYOUT_BREAK=YES", nullptr};
    std::unique_ptr<GDALDataset> upd(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE,
                          nullptr, open_opts, nullptr));
    ASSERT_NE(upd, nullptr);

    std::unique_ptr<GDALDataset> mem_4x4(
        GetMemDriver()->Create("", 4, 4, 1, GDT_Byte, nullptr));
    std::unique_ptr<GDALDataset> mem_8x8(
        GetMemDriver()->Create("", 8, 8, 1, GDT_Byte, nullptr));
    std::unique_ptr<GDALDataset> mem_bad(
        GetMemDriver()->Create("", 4, 4, 2, GDT_Byte, nullptr));

    // Inconsistent overview fails.
    EXPECT_EQ(upd->AddOverviews({mem_bad.get()}, nullptr, nullptr, nullptr),
              CE_Failure);
    // Pass 4x4 then 8x8 (unsorted; 8x8 already exists, 4x4 is new and triggers
    // LAYOUT=IFDS_BEFORE_DATA warning on the reopened COG!).
    EXPECT_EQ(upd->AddOverviews({mem_4x4.get(), mem_8x8.get()}, nullptr,
                                nullptr, nullptr),
              CE_None);
    // Also trigger FlushDirectory COG IFD growth warning at EOF.
    ASSERT_EQ(upd->SetMetadataItem("LARGE_TAG", std::string(512, 'X').c_str()),
              CE_None);
    upd->FlushCache(false);
    upd.reset();

    // AddOverviews in GA_ReadOnly mode on a fresh dataset with internal mask.
    std::string ro_path = "/vsimem/gw_add_ovr_ro.tif";
    absl::Cleanup cl2 = [&] {
      VSIUnlink(ro_path.c_str());
      VSIUnlink("/vsimem/gw_add_ovr_ro.tif.ovr");
    };
    std::unique_ptr<GDALDataset> ds_ro_create(
        drv->Create(ro_path.c_str(), 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_EQ(ds_ro_create->CreateMaskBand(GMF_PER_DATASET), CE_None);
    ds_ro_create.reset();

    std::unique_ptr<GDALDataset> ro(
        GDALDataset::Open(ro_path.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(ro, nullptr);
    EXPECT_EQ(ro->AddOverviews({mem_8x8.get()}, nullptr, nullptr, nullptr),
              CE_None);
  }
}

// ---------------------------------------------------------------------------
// 7. Georeferencing: WriteGeoTIFFInfo, SetSpatialRef, SetGeoTransform, SetGCPs
// ---------------------------------------------------------------------------
TEST_F(GTiffDatasetWriteTest, GeoreferencingWriteAndSetters) {
  CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
  CPLConfigOptionSetter pam_on("GDAL_PAM_ENABLED", "YES", false);
  GDALDriver* drv = GetGTiffDriver();

  // 7a. North-up & rotated GeoTransform, AREA_OR_POINT=Point, TFW/WORLDFILE,
  // SRS replacement (GTiffWriteDummyGeokeyDirectory), clearing SRS/GT/GCPs,
  // and >10922 GCPs PAM fallback.
  {
    std::string path = "/vsimem/gw_georef.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(path.c_str());
      VSIUnlink("/vsimem/gw_georef.tfw");
      VSIUnlink("/vsimem/gw_georef.tif.aux.xml");
    };
    CPLStringList opts;
    opts.SetNameValue("TFW", "YES");
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path.c_str(), 16, 16, 1, GDT_Byte, opts.List()));
    ASSERT_NE(ds, nullptr);

    OGRSpatialReference srs1;
    srs1.importFromEPSG(4326);
    ASSERT_EQ(ds->SetSpatialRef(&srs1), CE_None);
    ASSERT_EQ(ds->SetMetadataItem("AREA_OR_POINT", "Point"), CE_None);

    GDALGeoTransform gt_north{100.0, 1.0, 0.0, 200.0, 0.0, -1.0};
    ASSERT_EQ(ds->SetGeoTransform(gt_north), CE_None);
    GByte px = 1;
    ASSERT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 1, 1, &px, 1, 1,
                                             GDT_Byte, 0, 0, nullptr),
              CE_None);
    ds->FlushCache(false);

    // Replace SRS (hits GTiffWriteDummyGeokeyDirectory) and set rotated GT.
    OGRSpatialReference srs2;
    srs2.importFromEPSG(32631);
    ASSERT_EQ(ds->SetSpatialRef(&srs2), CE_None);
    GDALGeoTransform gt_rot{100.0, 1.0, 0.2, 200.0, 0.3, -1.0};
    ASSERT_EQ(ds->SetGeoTransform(gt_rot), CE_None);
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
    ASSERT_EQ(ds->SetGCPs(1, &gcp, &srs2), CE_None);
    ds->FlushCache(false);

    // Setting > 10922 GCPs falls back to PAM!
    std::vector<GDAL_GCP> many_gcps(10923, gcp);
    ASSERT_EQ(ds->SetGCPs(static_cast<int>(many_gcps.size()), many_gcps.data(),
                          &srs2),
              CE_None);
    // Now set <= 10922 GCPs to cancel PAM GCPs.
    ASSERT_EQ(ds->SetGCPs(1, &gcp, &srs2), CE_None);
    // Clear GCPs (SetGCPs(0, ...)) and clear SRS (SetSpatialRef(nullptr)).
    ASSERT_EQ(ds->SetGCPs(0, nullptr,
                          static_cast<const OGRSpatialReference*>(nullptr)),
              CE_None);
    ASSERT_EQ(ds->SetSpatialRef(nullptr), CE_None);
    ds->FlushCache(false);

    // Set GCPs again, then set GeoTransform (clears GCPs), then clear GT
    // ({0,0,0,0,0,0}).
    ASSERT_EQ(ds->SetGCPs(1, &gcp, &srs1), CE_None);
    ASSERT_EQ(ds->SetGeoTransform(gt_north), CE_None);
    GDALGeoTransform gt_zero{0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    ASSERT_EQ(ds->SetGeoTransform(gt_zero), CE_None);
    ds->FlushCache(false);
  }

  // 7b. Vertical 1-band SRS scale/offset encoding, Compound SRS unit type,
  // incompatible SRS PAM fallback, PROFILE=BASELINE, and GA_ReadOnly setters.
  {
    std::string path = "/vsimem/gw_vert.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(path.c_str());
      VSIUnlink("/vsimem/gw_vert.wld");
      VSIUnlink("/vsimem/gw_vert.tif.aux.xml");
    };
    CPLStringList opts;
    opts.SetNameValue("WORLDFILE", "YES");
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path.c_str(), 8, 8, 1, GDT_Float32, opts.List()));
    ASSERT_NE(ds, nullptr);
    OGRSpatialReference vert_srs;
    ASSERT_EQ(
        vert_srs.importFromWkt(
            "VERT_CS[\"NAVD88 height\",VERT_DATUM[\"North American Vertical "
            "Datum 1988\",2005],UNIT[\"metre\",1.0],AXIS[\"Up\",UP]]"),
        OGRERR_NONE);
    ASSERT_EQ(ds->SetSpatialRef(&vert_srs), CE_None);
    ASSERT_EQ(ds->GetRasterBand(1)->SetScale(2.5), CE_None);
    ASSERT_EQ(ds->GetRasterBand(1)->SetOffset(10.0), CE_None);
    GDALGeoTransform gt{0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ASSERT_EQ(ds->SetGeoTransform(gt), CE_None);
    ds->FlushCache(false);

    // Incompatible 3D Projected SRS falls back to PAM.
    OGRSpatialReference proj3d;
    ASSERT_EQ(proj3d.SetFromUserInput("EPSG:4979"), OGRERR_NONE);
    proj3d.SetUTM(31, TRUE);
    ASSERT_EQ(ds->SetSpatialRef(&proj3d), CE_None);
    ds->FlushCache(false);
    ds.reset();

    // Reopen in GA_ReadOnly and call SetSpatialRef, SetGeoTransform, SetGCPs,
    // SetMetadata, SetMetadataItem -> all go to PAM.
    std::unique_ptr<GDALDataset> ro(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(ro, nullptr);
    EXPECT_EQ(ro->SetSpatialRef(&vert_srs), CE_None);
    EXPECT_EQ(ro->SetGeoTransform(gt), CE_None);
    GDAL_GCP gcp{};
    char id[] = "1";
    char info[] = "";
    gcp.pszId = id;
    gcp.pszInfo = info;
    EXPECT_EQ(ro->SetGCPs(1, &gcp, &vert_srs), CE_None);
    EXPECT_EQ(ro->SetMetadataItem("RO_KEY", "RO_VAL"), CE_None);
    const char* const ro_md[] = {"RO_A=B", nullptr};
    EXPECT_EQ(ro->SetMetadata(const_cast<char**>(ro_md)), CE_None);
  }

  // 7c. PROFILE=BASELINE SetSpatialRef, SetGeoTransform, SetGCPs.
  {
    std::string path = "/vsimem/gw_baseline_geo.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(path.c_str());
      VSIUnlink("/vsimem/gw_baseline_geo.tif.aux.xml");
    };
    CPLStringList opts;
    opts.SetNameValue("PROFILE", "BASELINE");
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path.c_str(), 8, 8, 1, GDT_Byte, opts.List()));
    ASSERT_NE(ds, nullptr);
    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    EXPECT_EQ(ds->SetSpatialRef(&srs), CE_None);
    GDALGeoTransform gt{0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    EXPECT_EQ(ds->SetGeoTransform(gt), CE_None);
    GDAL_GCP gcp{};
    char id[] = "1";
    char info[] = "";
    gcp.pszId = id;
    gcp.pszInfo = info;
    EXPECT_EQ(ds->SetGCPs(1, &gcp, &srs), CE_None);
  }
}

// ---------------------------------------------------------------------------
// 8. Metadata, RPC, IMD, XMP, PAM PushMetadataToPam, RAT, and SaveICCProfile
// ---------------------------------------------------------------------------
TEST_F(GTiffDatasetWriteTest, MetadataRPCIMDXMPAndColorProfile) {
  CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
  CPLConfigOptionSetter pam_on("GDAL_PAM_ENABLED", "YES", false);
  GDALDriver* drv = GetGTiffDriver();

  // 8a. TIFFTAG_* (string, float, short, resolutionunit=0, byte_string,
  // unhandled warning) + unsetting tags + xml:XMP + RAT + TILING_SCHEME +
  // SaveICCProfile (both ICC base64 and colorimetric primaries/whitepoint/
  // transferfunction/transferrange).
  {
    std::string path = "/vsimem/gw_md_tags.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(path.c_str());
      VSIUnlink("/vsimem/gw_md_tags.IMD");
    };
    std::string tf_256;
    for (int i = 0; i < 256; ++i) {
      if (i > 0) tf_256 += ",";
      tf_256 += CPLSPrintf("%d", i * 257);
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

    std::unique_ptr<GDALDataset> ds(
        drv->Create(path.c_str(), 8, 8, 1, GDT_Byte, opts.List()));
    ASSERT_NE(ds, nullptr);

    ASSERT_EQ(ds->SetMetadataItem("TIFFTAG_DOCUMENTNAME", "Doc"), CE_None);
    ASSERT_EQ(ds->SetMetadataItem("TIFFTAG_XRESOLUTION", "72.5"), CE_None);
    ASSERT_EQ(ds->SetMetadataItem("TIFFTAG_RESOLUTIONUNIT", "0"), CE_None);
    ASSERT_EQ(ds->SetMetadataItem("TIFFTAG_MINSAMPLEVALUE", "1"), CE_None);
    ASSERT_EQ(ds->SetMetadataItem("GEO_METADATA", "GeoMD"), CE_None);
    ASSERT_EQ(ds->SetMetadataItem("TIFF_RSID", "RSID"), CE_None);
    ASSERT_EQ(ds->SetMetadataItem("TIFFTAG_UNKNOWN_TAG", "123"), CE_None);
    ASSERT_EQ(ds->SetMetadataItem("CUSTOM_KEY", "VAL", "CUSTOM_DOM"), CE_None);

    const char* const xml_md[] = {"<root>hello</root>", nullptr};
    ASSERT_EQ(ds->SetMetadata(const_cast<char**>(xml_md), "xml:custom"),
              CE_None);
    const char* const xmp_md[] = {"<x:xmpmeta/>", nullptr};
    ASSERT_EQ(ds->SetMetadata(const_cast<char**>(xmp_md), "xml:XMP"), CE_None);
    const char* const imd_md[] = {"version = \"P\";", nullptr};
    ASSERT_EQ(ds->SetMetadata(const_cast<char**>(imd_md), "IMD"), CE_None);

    // Band description, unit, scale, offset, colorinterp, and RAT.
    GDALRasterBand* b1 = ds->GetRasterBand(1);
    b1->SetDescription("BandOne");
    ASSERT_EQ(b1->SetUnitType("foot"), CE_None);
    ASSERT_EQ(b1->SetScale(2.0), CE_None);
    ASSERT_EQ(b1->SetOffset(1.0), CE_None);
    ASSERT_EQ(b1->SetColorInterpretation(GCI_RedBand), CE_None);
    GDALDefaultRasterAttributeTable rat;
    rat.CreateColumn("Value", GFT_Integer, GFU_Generic);
    rat.SetRowCount(1);
    rat.SetValue(0, 0, 1);
    ASSERT_EQ(b1->SetDefaultRAT(&rat), CE_None);

    GByte px = 1;
    ASSERT_EQ(
        b1->RasterIO(GF_Write, 0, 0, 1, 1, &px, 1, 1, GDT_Byte, 0, 0, nullptr),
        CE_None);
    ds->FlushCache(false);

    // Now clear default metadata, XMP, RAT, description, unit, scale/offset so
    // WriteMetadata unsets TIFFTAG_* and TIFFTAG_GDAL_METADATA!
    ASSERT_EQ(ds->SetMetadata(nullptr), CE_None);
    ASSERT_EQ(ds->SetMetadata(nullptr, "CUSTOM_DOM"), CE_None);
    ASSERT_EQ(ds->SetMetadata(nullptr, "xml:custom"), CE_None);
    ASSERT_EQ(ds->SetMetadata(nullptr, "xml:XMP"), CE_None);
    b1->SetDescription("");
    ASSERT_EQ(b1->SetUnitType(""), CE_None);
    ASSERT_EQ(b1->SetScale(1.0), CE_None);
    ASSERT_EQ(b1->SetOffset(0.0), CE_None);
    ASSERT_EQ(b1->SetColorInterpretation(GCI_GrayIndex), CE_None);
    ASSERT_EQ(b1->SetDefaultRAT(nullptr), CE_None);

    // Also test SaveICCProfile via pDS (SetMetadataItem on COLOR_PROFILE)
    // with both invalid colorimetric strings and a valid base64 ICC profile.
    ASSERT_EQ(ds->SetMetadataItem("SOURCE_PRIMARIES_RED", "0.64,0.33,2.0",
                                  "COLOR_PROFILE"),
              CE_None);
    ASSERT_EQ(ds->SetMetadataItem("SOURCE_WHITEPOINT", "0.31,0.32,2.0",
                                  "COLOR_PROFILE"),
              CE_None);
    ASSERT_EQ(ds->SetMetadataItem("TIFFTAG_TRANSFERRANGE_BLACK", "0,0",
                                  "COLOR_PROFILE"),
              CE_None);
    ds.reset();

    // Reopen in GA_Update and set SOURCE_ICC_PROFILE via SetMetadata.
    std::unique_ptr<GDALDataset> upd(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE));
    ASSERT_NE(upd, nullptr);
    const char* const icc_md[] = {"SOURCE_ICC_PROFILE=AAAA", nullptr};
    ASSERT_EQ(upd->SetMetadata(const_cast<char**>(icc_md), "COLOR_PROFILE"),
              CE_None);
  }

  // 8b. PushMetadataToPam & WriteRPC to PAM (PROFILE=BASELINE, RPB=NO).
  {
    std::string path = "/vsimem/gw_pam_push.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(path.c_str());
      VSIUnlink("/vsimem/gw_pam_push.tif.aux.xml");
    };
    CPLStringList opts;
    opts.SetNameValue("PROFILE", "BASELINE");
    opts.SetNameValue("RPB", "NO");
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path.c_str(), 8, 8, 1, GDT_Byte, opts.List()));
    ASSERT_NE(ds, nullptr);
    ASSERT_EQ(ds->SetMetadataItem("TIFFTAG_ARTIST", "Artist"), CE_None);
    ASSERT_EQ(ds->SetMetadataItem("USER_KEY", "USER_VAL"), CE_None);
    ASSERT_EQ(ds->GetRasterBand(1)->SetMetadataItem("B_KEY", "B_VAL"), CE_None);
    ASSERT_EQ(ds->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand),
              CE_None);
    ds->FlushCache(false);
  }
}

// ---------------------------------------------------------------------------
// 9. GuessJPEGQuality (MD5 & Brute-Force 2-Band Search) & TablesMode
// ---------------------------------------------------------------------------
TEST_F(GTiffDatasetWriteTest, GuessJPEGQualityAndUpdateMode) {
  CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
  GDALDriver* drv = GetGTiffDriver();

  const struct {
    int bands;
    const char* photo;
    const char* interleave;
    const char* quality;
    const char* tablesmode;
  } cases[] = {
      {1, "MINISBLACK", "PIXEL", "82", "1"},  // 1-band 8-bit MD5
      {3, "RGB", "PIXEL", "75", "3"},         // 3-band RGB + Huffman table
      {3, "YCBCR", "PIXEL", "77", "1"},       // 3-band YCbCr MD5
      {2, "MINISBLACK", "BAND", "75", "1"},   // 2-band brute-force (nQuality=0)
      {2, "MINISBLACK", "BAND", "50", "1"},  // 2-band brute-force (nQuality=50)
      {1, "MINISBLACK", "PIXEL", "80", "0"},  // JPEGTABLESMODE=0 (no table tag)
      {1, "MINISBLACK", "PIXEL", "80", "2"},  // JPEGTABLESMODE=2 (Huffman only)
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    std::string path = CPLSPrintf("/vsimem/gw_jpeg_q_%zu.tif", i);
    SCOPED_TRACE(path);
    absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
    CPLStringList opts;
    opts.SetNameValue("COMPRESS", "JPEG");
    opts.SetNameValue("PHOTOMETRIC", cases[i].photo);
    opts.SetNameValue("INTERLEAVE", cases[i].interleave);
    opts.SetNameValue("JPEG_QUALITY", cases[i].quality);
    opts.SetNameValue("JPEGTABLESMODE", cases[i].tablesmode);
    std::unique_ptr<GDALDataset> ds(drv->Create(
        path.c_str(), 16, 16, cases[i].bands, GDT_Byte, opts.List()));
    ASSERT_NE(ds, nullptr);
    std::vector<GByte> buf(16 * 16, 100);
    for (int b = 1; b <= cases[i].bands; ++b) {
      ASSERT_EQ(
          ds->GetRasterBand(b)->RasterIO(GF_Write, 0, 0, 16, 16, buf.data(), 16,
                                         16, GDT_Byte, 0, 0, nullptr),
          CE_None);
    }
    ds.reset();

    // Reopening in GA_Update calls GuessJPEGQuality and
    // SetJPEGQualityAndTablesModeFromFile!
    std::unique_ptr<GDALDataset> upd(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE));
    ASSERT_NE(upd, nullptr);
    // GuessJPEGQuality() needs a quantization table in the JPEGTables tag:
    // JPEGTABLESMODE=0 writes no JPEGTables tag and JPEGTABLESMODE=2 writes
    // only Huffman tables, so no quality/tables-mode metadata is derived for
    // those two cases. Otherwise the original quality is recovered (by MD5
    // lookup or brute force) and JPEGTABLESMODE reflects the tables present.
    if (EQUAL(cases[i].tablesmode, "0") || EQUAL(cases[i].tablesmode, "2")) {
      EXPECT_EQ(upd->GetMetadataItem("JPEG_QUALITY", "IMAGE_STRUCTURE"),
                nullptr);
      EXPECT_EQ(upd->GetMetadataItem("JPEGTABLESMODE", "IMAGE_STRUCTURE"),
                nullptr);
    } else {
      EXPECT_STREQ(upd->GetMetadataItem("JPEG_QUALITY", "IMAGE_STRUCTURE"),
                   cases[i].quality);
      EXPECT_STREQ(upd->GetMetadataItem("JPEGTABLESMODE", "IMAGE_STRUCTURE"),
                   cases[i].tablesmode);
    }
  }
}

// ---------------------------------------------------------------------------
// 10. CreateCopy, CopyImageryAndMask, Split Bands, JPEG Copy, & CreateMaskBand
// ---------------------------------------------------------------------------
TEST_F(GTiffDatasetWriteTest, CreateCopyAndMasksComprehensive) {
  CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
  CPLConfigOptionSetter pam_on("GDAL_PAM_ENABLED", "YES", false);
  GDALDriver* drv = GetGTiffDriver();
  GDALDriver* mem_drv = GetMemDriver();

  // 10a. 0-band and mixed-band-type CreateCopy.
  {
    std::unique_ptr<GDALDataset> ds0(
        mem_drv->Create("", 8, 8, 0, GDT_Byte, nullptr));
    EXPECT_EQ(drv->CreateCopy("/vsimem/gw_cc0.tif", ds0.get(), TRUE, nullptr,
                              nullptr, nullptr),
              nullptr);

    std::unique_ptr<GDALDataset> ds_mix(
        mem_drv->Create("", 8, 8, 1, GDT_Byte, nullptr));
    ASSERT_EQ(ds_mix->AddBand(GDT_UInt16, nullptr), CE_None);
    EXPECT_EQ(drv->CreateCopy("/vsimem/gw_cc_mix_strict.tif", ds_mix.get(),
                              TRUE, nullptr, nullptr, nullptr),
              nullptr);
    std::unique_ptr<GDALDataset> cc_non_strict(
        drv->CreateCopy("/vsimem/gw_cc_mix_ns.tif", ds_mix.get(), FALSE,
                        nullptr, nullptr, nullptr));
    ASSERT_NE(cc_non_strict, nullptr);
    cc_non_strict.reset();
    VSIUnlink("/vsimem/gw_cc_mix_ns.tif");
  }

  // 10b. Source with NBITS, PIXELTYPE=SIGNEDBYTE, COLOR_PROFILE, json:ISIS3,
  // xml:ESRI (with ESRI_XML_PAM=YES), xml:XMP, GCPs + TFW=YES warning,
  // category names, and COPY_SRC_MDD=YES / SRC_MDD.
  {
    CPLConfigOptionSetter esri_pam("ESRI_XML_PAM", "YES", false);
    std::unique_ptr<GDALDataset> src(
        mem_drv->Create("", 8, 8, 1, GDT_Byte, nullptr));
    ASSERT_EQ(
        src->GetRasterBand(1)->SetMetadataItem("NBITS", "8", "IMAGE_STRUCTURE"),
        CE_None);
    ASSERT_EQ(src->GetRasterBand(1)->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE",
                                                     "IMAGE_STRUCTURE"),
              CE_None);
    ASSERT_EQ(
        src->SetMetadataItem("SOURCE_ICC_PROFILE", "AAAA", "COLOR_PROFILE"),
        CE_None);
    const char* const isis_md[] = {"{\"isis\":1}", nullptr};
    ASSERT_EQ(src->SetMetadata(const_cast<char**>(isis_md), "json:ISIS3"),
              CE_None);
    const char* const esri_md[] = {"<esri/>", nullptr};
    ASSERT_EQ(src->SetMetadata(const_cast<char**>(esri_md), "xml:ESRI"),
              CE_None);
    const char* const xmp_md[] = {"<x:xmpmeta/>", nullptr};
    ASSERT_EQ(src->SetMetadata(const_cast<char**>(xmp_md), "xml:XMP"), CE_None);
    ASSERT_EQ(src->SetMetadataItem("AREA_OR_POINT", "Point"), CE_None);
    ASSERT_EQ(src->GetRasterBand(1)->SetMetadataItem("BAND_MD", "1", "IMAGERY"),
              CE_None);
    const char* const cats[] = {"Cat0", "Cat1", nullptr};
    ASSERT_EQ(src->GetRasterBand(1)->SetCategoryNames(const_cast<char**>(cats)),
              CE_None);

    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    GDAL_GCP gcp{};
    char id[] = "1";
    char info[] = "";
    gcp.pszId = id;
    gcp.pszInfo = info;
    gcp.dfGCPPixel = 1.0;
    gcp.dfGCPLine = 1.0;
    ASSERT_EQ(src->SetGCPs(1, &gcp, &srs), CE_None);

    CPLStringList opts;
    opts.SetNameValue("TFW", "YES");
    opts.SetNameValue("COPY_SRC_MDD", "YES");
    opts.SetNameValue("@FLUSHCACHE", "YES");
    std::unique_ptr<GDALDataset> dst(
        drv->CreateCopy("/vsimem/gw_cc_meta.tif", src.get(), FALSE, opts.List(),
                        nullptr, nullptr));
    ASSERT_NE(dst, nullptr);
    dst.reset();
    VSIUnlink("/vsimem/gw_cc_meta.tif");
    VSIUnlink("/vsimem/gw_cc_meta.tif.aux.xml");
  }

  // 10c. ColorTable CreateCopy: 1-band Byte (with out-of-range ClampCTEntry),
  // 2-band Byte (palette+alpha), 1-band UInt16, 3-band Byte (error), and
  // 1-band Byte with COMPRESS=JPEG (error).
  {
    GDALColorTable ct;
    GDALColorEntry ce_neg = {-5, 300, 10, 255};
    ct.SetColorEntry(0, &ce_neg);

    for (int bands : {1, 2, 3}) {
      SCOPED_TRACE(bands);
      std::unique_ptr<GDALDataset> src(
          mem_drv->Create("", 8, 8, bands, GDT_Byte, nullptr));
      ASSERT_EQ(src->GetRasterBand(1)->SetColorTable(&ct), CE_None);
      std::unique_ptr<GDALDataset> dst(drv->CreateCopy(
          "/vsimem/gw_cc_ct.tif", src.get(), FALSE, nullptr, nullptr, nullptr));
      ASSERT_NE(dst, nullptr);
      dst.reset();
      VSIUnlink("/vsimem/gw_cc_ct.tif");
    }
    // 1-band UInt16 with ColorTable.
    {
      std::unique_ptr<GDALDataset> src16(
          mem_drv->Create("", 8, 8, 1, GDT_UInt16, nullptr));
      ASSERT_EQ(src16->GetRasterBand(1)->SetColorTable(&ct), CE_None);
      std::unique_ptr<GDALDataset> dst16(
          drv->CreateCopy("/vsimem/gw_cc_ct16.tif", src16.get(), FALSE, nullptr,
                          nullptr, nullptr));
      ASSERT_NE(dst16, nullptr);
      dst16.reset();
      VSIUnlink("/vsimem/gw_cc_ct16.tif");
    }
    // 1-band Byte with ColorTable and COMPRESS=JPEG fails.
    {
      std::unique_ptr<GDALDataset> src_jpg(
          mem_drv->Create("", 16, 16, 1, GDT_Byte, nullptr));
      ASSERT_EQ(src_jpg->GetRasterBand(1)->SetColorTable(&ct), CE_None);
      CPLStringList opts;
      opts.SetNameValue("COMPRESS", "JPEG");
      EXPECT_EQ(drv->CreateCopy("/vsimem/gw_cc_ct_jpg.tif", src_jpg.get(),
                                FALSE, opts.List(), nullptr, nullptr),
                nullptr);
    }
  }

  // 10d. Split-band and split-bitmap scanline loops in CreateCopy (16x2001,
  // BLOCKYSIZE=2001, COMPRESS=PACKBITS on 2-band CONTIG, 1-band Byte, and
  // 1-band NBITS=1).
  for (int mode = 0; mode < 3; ++mode) {
    std::string path = CPLSPrintf("/vsimem/gw_cc_split_%d.tif", mode);
    SCOPED_TRACE(path);
    absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
    int bands = (mode == 0) ? 2 : 1;
    std::unique_ptr<GDALDataset> src(
        mem_drv->Create("", 16, 2001, bands, GDT_Byte, nullptr));
    CPLStringList opts;
    opts.SetNameValue("TILED", "NO");
    opts.SetNameValue("BLOCKYSIZE", "2001");
    opts.SetNameValue("COMPRESS", "PACKBITS");
    opts.SetNameValue("INTERLEAVE", "PIXEL");
    if (mode == 2) opts.SetNameValue("NBITS", "1");
    std::unique_ptr<GDALDataset> dst(drv->CreateCopy(
        path.c_str(), src.get(), FALSE, opts.List(), nullptr, nullptr));
    ASSERT_NE(dst, nullptr);
  }

  // 10e. CopyImageryAndMask via CreateCopy with COPY_SRC_OVERVIEWS=YES on
  // partial tiles (20x20 with 16x16 tiles): SEPARATE+mask, @TILE_INTERLEAVE,
  // CONTIG+mask, and CONTIG NBITS=4 odd-bits, plus @OVERVIEW_DATASET /
  // @MASK_OVERVIEW_DATASET / @OVERVIEW_COUNT.
  for (int mode = 0; mode < 4; ++mode) {
    CPLConfigOptionSetter int_mask("GDAL_TIFF_INTERNAL_MASK", "YES", false);
    std::string src_path = CPLSPrintf("/vsimem/gw_cc_cog_src_%d.tif", mode);
    std::string path = CPLSPrintf("/vsimem/gw_cc_cog_%d.tif", mode);
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
    std::unique_ptr<GDALDataset> src(drv->Create(src_path.c_str(), src_size,
                                                 src_size, 2, GDT_Byte,
                                                 nullptr));
    ASSERT_NE(src, nullptr);
    ASSERT_EQ(src->CreateMaskBand(GMF_PER_DATASET), CE_None);
    int ovr = 2;
    ASSERT_EQ(
        src->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr, nullptr, nullptr),
        CE_None);

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
    std::unique_ptr<GDALDataset> dst(drv->CreateCopy(
        path.c_str(), src.get(), FALSE, opts.List(), nullptr, nullptr));
    ASSERT_NE(dst, nullptr);
  }

  // @OVERVIEW_DATASET, @OVERVIEW_COUNT, @MASK_OVERVIEW_DATASET.
  {
    std::string ovr_path = "/vsimem/gw_ext_ovr_src.tif";
    std::string msk_path = "/vsimem/gw_ext_msk_src.tif";
    std::string dst_path = "/vsimem/gw_cc_ext_ovr.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(ovr_path.c_str());
      VSIUnlink(msk_path.c_str());
      VSIUnlink(dst_path.c_str());
    };
    std::unique_ptr<GDALDataset> ovr_ds(
        drv->Create(ovr_path.c_str(), 8, 8, 1, GDT_Byte, nullptr));
    ovr_ds.reset();
    std::unique_ptr<GDALDataset> msk_ds(
        drv->Create(msk_path.c_str(), 8, 8, 1, GDT_Byte, nullptr));
    msk_ds.reset();

    std::unique_ptr<GDALDataset> src(
        mem_drv->Create("", 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_EQ(src->CreateMaskBand(GMF_PER_DATASET), CE_None);

    CPLStringList opts;
    opts.SetNameValue("TILED", "YES");
    opts.SetNameValue("BLOCKXSIZE", "16");
    opts.SetNameValue("BLOCKYSIZE", "16");
    opts.SetNameValue("COPY_SRC_OVERVIEWS", "YES");
    opts.SetNameValue("@OVERVIEW_DATASET", ovr_path.c_str());
    opts.SetNameValue("@OVERVIEW_COUNT", "1");
    opts.SetNameValue("@MASK_OVERVIEW_DATASET", msk_path.c_str());
    std::unique_ptr<GDALDataset> dst(drv->CreateCopy(
        dst_path.c_str(), src.get(), FALSE, opts.List(), nullptr, nullptr));
    ASSERT_NE(dst, nullptr);
  }

  // 10f. Lossless DCT CreateCopy from a JPEG dataset in /vsimem/, plus progress
  // cancellation with GTIFF_DELETE_ON_ERROR=YES and NO.
  if (GDALDriver* jpg_drv = GetGDALDriverManager()->GetDriverByName("JPEG")) {
    std::string jpg_path = "/vsimem/gw_src.jpg";
    std::string tif_path = "/vsimem/gw_from_jpg.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(jpg_path.c_str());
      VSIUnlink(tif_path.c_str());
    };
    std::unique_ptr<GDALDataset> mem_src(
        mem_drv->Create("", 16, 16, 3, GDT_Byte, nullptr));
    std::unique_ptr<GDALDataset> jpg_ds(jpg_drv->CreateCopy(
        jpg_path.c_str(), mem_src.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(jpg_ds, nullptr);
    jpg_ds.reset();

    std::unique_ptr<GDALDataset> jpg_ro(
        GDALDataset::Open(jpg_path.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(jpg_ro, nullptr);
    CPLStringList opts;
    opts.SetNameValue("COMPRESS", "JPEG");
    std::unique_ptr<GDALDataset> tif_ds(drv->CreateCopy(
        tif_path.c_str(), jpg_ro.get(), FALSE, opts.List(), nullptr, nullptr));
    ASSERT_NE(tif_ds, nullptr);
  }
  for (const char* del_on_err : {"YES", "NO"}) {
    SCOPED_TRACE(del_on_err);
    CPLConfigOptionSetter setter("GTIFF_DELETE_ON_ERROR", del_on_err, false);
    std::string path = "/vsimem/gw_cc_abort.tif";
    absl::Cleanup cl = [&] { VSIUnlink(path.c_str()); };
    std::unique_ptr<GDALDataset> src(
        mem_drv->Create("", 16, 16, 1, GDT_Byte, nullptr));
    EXPECT_EQ(drv->CreateCopy(path.c_str(), src.get(), FALSE, nullptr,
                              FailingProgress, nullptr),
              nullptr);
  }

  // 10g. CreateMaskBand (dataset & band) error and fallback branches.
  {
    CPLConfigOptionSetter int_mask("GDAL_TIFF_INTERNAL_MASK", "YES", false);
    CPLConfigOptionSetter no_promote("GDAL_TIFF_INTERNAL_MASK_TO_8BIT", "NO",
                                     false);
    std::string path = "/vsimem/gw_masks.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(path.c_str());
      VSIUnlink("/vsimem/gw_masks.tif.msk");
    };
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path.c_str(), 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(ds, nullptr);
    // Invalid flag for internal mask fails.
    EXPECT_EQ(ds->CreateMaskBand(0), CE_Failure);
    // Valid GMF_PER_DATASET via band CreateMaskBand succeeds.
    EXPECT_EQ(ds->GetRasterBand(1)->CreateMaskBand(GMF_PER_DATASET), CE_None);
    // Duplicate CreateMaskBand fails on both dataset and band.
    EXPECT_EQ(ds->CreateMaskBand(GMF_PER_DATASET), CE_Failure);
    EXPECT_EQ(ds->GetRasterBand(1)->CreateMaskBand(GMF_PER_DATASET),
              CE_Failure);
    // Creating a mask on the mask IFD itself fails!
    ASSERT_NE(ds->GetRasterBand(1)->GetMaskBand()->GetDataset(), nullptr);
    EXPECT_EQ(ds->GetRasterBand(1)->GetMaskBand()->GetDataset()->CreateMaskBand(
                  GMF_PER_DATASET),
              CE_Failure);
  }
  {
    // CreateMaskBand on read-only dataset creates external .msk.
    std::string path = "/vsimem/gw_mask_ro.tif";
    absl::Cleanup cl = [&] {
      VSIUnlink(path.c_str());
      VSIUnlink("/vsimem/gw_mask_ro.tif.msk");
    };
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path.c_str(), 8, 8, 1, GDT_Byte, nullptr));
    ds.reset();
    std::unique_ptr<GDALDataset> ro(
        GDALDataset::Open(path.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(ro, nullptr);
    EXPECT_EQ(ro->CreateMaskBand(GMF_PER_DATASET), CE_None);
  }

  // 10h. CreateCopy GeoTransform (north-up + rotated + AREA_OR_POINT=Point +
  // vertical SRS + TFW/WORLDFILE + BASELINE PAM), 64-bit NoData,
  // STREAMABLE_OUTPUT=YES, PIXELTYPE=SIGNEDBYTE, ALPHA/PHOTOMETRIC=RGB 5-band,
  // WEBP/JXL/LZMA/ZSTD codecs in CreateCopy, and MASK_INTERLEAVED_WITH_IMAGERY
  // in-place rewrite invalidation!
  {
    CPLConfigOptionSetter pam_on("GDAL_PAM_ENABLED", "YES", false);
    const char* p_gt1 = "/vsimem/gw_cc_gt1.tif";
    const char* p_gt1_tfw = "/vsimem/gw_cc_gt1.tfw";
    const char* p_gt2 = "/vsimem/gw_cc_gt2.tif";
    const char* p_gt2_wld = "/vsimem/gw_cc_gt2.wld";
    const char* p_base = "/vsimem/gw_cc_base.tif";
    const char* p_base_aux = "/vsimem/gw_cc_base.tif.aux.xml";
    const char* p_stream = "/vsimem/gw_cc_stream.tif";
    const char* p_rgb5 = "/vsimem/gw_cc_rgb5.tif";
    const char* p_rgb4 = "/vsimem/gw_cc_rgb4.tif";
    const char* p_msk_inter = "/vsimem/gw_cc_msk_inter.tif";
    absl::Cleanup cl = [&] {
      for (const char* p : {p_gt1, p_gt1_tfw, p_gt2, p_gt2_wld, p_base,
                            p_base_aux, p_stream, p_rgb5, p_rgb4, p_msk_inter}) {
        VSIUnlink(p);
      }
    };

    // North-up GeoTransform + Point + 3D Vertical SRS + TFW=YES + NoData:
    {
      std::unique_ptr<GDALDataset> src(
          mem_drv->Create("", 8, 8, 1, GDT_Byte, nullptr));
      GDALGeoTransform gt(100.0, 2.0, 0.0, 200.0, 0.0, -2.0);
      EXPECT_EQ(src->SetGeoTransform(gt), CE_None);
      EXPECT_EQ(src->SetMetadataItem(GDALMD_AREA_OR_POINT, GDALMD_AOP_POINT),
                CE_None);
      OGRSpatialReference srs;
      ASSERT_EQ(srs.SetFromUserInput("EPSG:4326+5773"), OGRERR_NONE);
      EXPECT_EQ(src->SetSpatialRef(&srs), CE_None);
      EXPECT_EQ(src->GetRasterBand(1)->SetNoDataValue(99.0), CE_None);
      EXPECT_EQ(src->GetRasterBand(1)->SetOffset(5.0), CE_None);

      CPLStringList co;
      co.SetNameValue("TFW", "YES");
      co.SetNameValue("PIXELTYPE", "SIGNEDBYTE");
      std::unique_ptr<GDALDataset> dst(drv->CreateCopy(
          p_gt1, src.get(), FALSE, co.List(), nullptr, nullptr));
      ASSERT_NE(dst, nullptr);

      // Same source copied with PROFILE=BASELINE -> exports SRS & GT to PAM!
      CPLStringList bco;
      bco.SetNameValue("PROFILE", "BASELINE");
      std::unique_ptr<GDALDataset> bdst(drv->CreateCopy(
          p_base, src.get(), FALSE, bco.List(), nullptr, nullptr));
      ASSERT_NE(bdst, nullptr);
    }

    // Rotated GeoTransform + Point + WORLDFILE=YES + Int64/UInt64 NoData +
    // STREAMABLE_OUTPUT=YES:
    for (GDALDataType dt : {GDT_Int64, GDT_UInt64}) {
      SCOPED_TRACE(GDALGetDataTypeName(dt));
      std::unique_ptr<GDALDataset> src(
          mem_drv->Create("", 8, 8, 1, dt, nullptr));
      GDALGeoTransform gt(10.0, 1.0, 0.2, 20.0, 0.3, -1.0);
      EXPECT_EQ(src->SetGeoTransform(gt), CE_None);
      EXPECT_EQ(src->SetMetadataItem(GDALMD_AREA_OR_POINT, GDALMD_AOP_POINT),
                CE_None);
      if (dt == GDT_Int64) {
        EXPECT_EQ(src->GetRasterBand(1)->SetNoDataValueAsInt64(-42), CE_None);
      } else {
        EXPECT_EQ(src->GetRasterBand(1)->SetNoDataValueAsUInt64(42), CE_None);
      }
      CPLStringList co;
      co.SetNameValue("WORLDFILE", "YES");
      std::unique_ptr<GDALDataset> dst(drv->CreateCopy(
          p_gt2, src.get(), FALSE, co.List(), nullptr, nullptr));
      ASSERT_NE(dst, nullptr);
    }

    // STREAMABLE_OUTPUT=YES in CreateCopy:
    {
      std::unique_ptr<GDALDataset> src(
          mem_drv->Create("", 16, 16, 1, GDT_Byte, nullptr));
      CPLStringList co;
      co.SetNameValue("STREAMABLE_OUTPUT", "YES");
      std::unique_ptr<GDALDataset> dst(drv->CreateCopy(
          p_stream, src.get(), FALSE, co.List(), nullptr, nullptr));
      ASSERT_NE(dst, nullptr);
    }

    // 5-band PHOTOMETRIC=RGB + ALPHA metadata + 4-band ALPHA=NO + codecs in
    // CreateCopy (WEBP_LOSSLESS+WEBP_LEVEL warning, JXL, LZMA, ZSTD):
    {
      std::unique_ptr<GDALDataset> src5(
          mem_drv->Create("", 16, 16, 5, GDT_Byte, nullptr));
      src5->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand);
      src5->GetRasterBand(4)->SetMetadataItem("ALPHA", "PREMULTIPLIED",
                                              "IMAGE_STRUCTURE");
      CPLStringList co;
      co.SetNameValue("PHOTOMETRIC", "RGB");
      std::unique_ptr<GDALDataset> dst5(drv->CreateCopy(
          p_rgb5, src5.get(), FALSE, co.List(), nullptr, nullptr));
      ASSERT_NE(dst5, nullptr);

      std::unique_ptr<GDALDataset> src4(
          mem_drv->Create("", 16, 16, 4, GDT_Byte, nullptr));
      src4->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand);
      CPLStringList co4;
      co4.SetNameValue("ALPHA", "NO");
      std::unique_ptr<GDALDataset> dst4(drv->CreateCopy(
          p_rgb4, src4.get(), FALSE, co4.List(), nullptr, nullptr));
      ASSERT_NE(dst4, nullptr);

      // Each codec gets its own output path so that no two open datasets
      // share a /vsimem file.
      for (const char* comp : {"WEBP", "JXL", "LZMA", "ZSTD"}) {
        SCOPED_TRACE(comp);
        std::string p_codec = CPLSPrintf("/vsimem/gw_cc_codec_%s.tif", comp);
        absl::Cleanup codec_cl = [&] { VSIUnlink(p_codec.c_str()); };
        std::unique_ptr<GDALDataset> s3(
            mem_drv->Create("", 16, 16, 3, GDT_Byte, nullptr));
        CPLStringList cco;
        cco.SetNameValue("COMPRESS", comp);
        if (EQUAL(comp, "WEBP")) {
          cco.SetNameValue("WEBP_LOSSLESS", "YES");
          cco.SetNameValue("WEBP_LEVEL", "80");
        } else if (EQUAL(comp, "LZMA")) {
          cco.SetNameValue("LZMA_PRESET", "2");
        } else if (EQUAL(comp, "ZSTD")) {
          cco.SetNameValue("ZSTD_LEVEL", "3");
        }
        std::unique_ptr<GDALDataset> d3(drv->CreateCopy(
            p_codec.c_str(), s3.get(), FALSE, cco.List(), nullptr, nullptr));
        ASSERT_NE(d3, nullptr);
      }
    }

    // MASK_INTERLEAVED_WITH_IMAGERY in-place rewrite with smaller compressed
    // size -> invalidates leader & MASK_INTERLEAVED_WITH_IMAGERY optimization!
    {
      CPLConfigOptionSetter int_msk("GDAL_TIFF_INTERNAL_MASK", "YES", false);
      CPLStringList co;
      co.SetNameValue("TILED", "YES");
      co.SetNameValue("BLOCKXSIZE", "16");
      co.SetNameValue("BLOCKYSIZE", "16");
      co.SetNameValue("COMPRESS", "LZW");
      co.SetNameValue("BLOCK_ORDER", "ROW_MAJOR");
      co.SetNameValue("MASK_INTERLEAVED_WITH_IMAGERY", "YES");
      co.SetNameValue("BLOCK_LEADER", "SIZE_AS_UINT4");
      co.SetNameValue("BLOCK_TRAILER", "LAST_4_BYTES_REPEATED");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_msk_inter, 16, 16, 1, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      ASSERT_EQ(ds->CreateMaskBand(GMF_PER_DATASET), CE_None);
      std::vector<GByte> noisy(16 * 16);
      for (size_t i = 0; i < noisy.size(); ++i) {
        noisy[i] = static_cast<GByte>((i * 73 + 19) & 0xFF);
      }
      EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, noisy.data()), CE_None);
      EXPECT_EQ(
          ds->GetRasterBand(1)->GetMaskBand()->WriteBlock(0, 0, noisy.data()),
          CE_None);
      ds.reset();

      std::unique_ptr<GDALDataset> upd(
          GDALDataset::Open(p_msk_inter, GDAL_OF_RASTER | GDAL_OF_UPDATE));
      ASSERT_NE(upd, nullptr);
      std::vector<GByte> zeros(16 * 16, 0);
      EXPECT_EQ(upd->GetRasterBand(1)->WriteBlock(0, 0, zeros.data()), CE_None);
      EXPECT_EQ(upd->Close(), CE_None);
    }
  }
}

}  // namespace
