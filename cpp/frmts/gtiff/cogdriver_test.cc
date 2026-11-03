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
// Tests Cloud Optimized GeoTIFF (COG) driver creation, warping helpers, and
// codec pass-through options (cogdriver.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/gtiff/cogdriver.cpp
//   https://gdal.org/drivers/raster/cog.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/cog.py

#include "third_party/gdal/frmts/gtiff/cogdriver.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kWorkDir[] = "/vsimem/cogdriver_test";

class CogDriverTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_COG();
    GDALRegister_GTiff();
    GDALRegister_MEM();
    GDALRegister_VRT();
    GDALRegister_JPEG();
    GDALRegister_WEBP();
    CPLErrorReset();

    cog_drv_ = GetGDALDriverManager()->GetDriverByName("COG");
    gtiff_drv_ = GetGDALDriverManager()->GetDriverByName("GTiff");
    mem_drv_ = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, cog_drv_);
    ASSERT_NE(nullptr, gtiff_drv_);
    ASSERT_NE(nullptr, mem_drv_);
  }

  void TearDown() override {
    VSIRmdirRecursive(kWorkDir);
    CPLErrorReset();
  }

  GDALDatasetUniquePtr CreateGeorefMem(int w, int h, int bands, GDALDataType dt,
                                       const char* srs_str, const double* gt) {
    GDALDatasetUniquePtr ds(mem_drv_->Create("", w, h, bands, dt, nullptr));
    if (ds == nullptr) return nullptr;
    if (srs_str != nullptr) {
      OGRSpatialReference srs;
      srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
      srs.SetFromUserInput(srs_str);
      (void)ds->SetSpatialRef(&srs);
    }
    if (gt != nullptr) {
      (void)ds->SetGeoTransform(gt);
    }
    return ds;
  }

  GDALDriver* cog_drv_ = nullptr;
  GDALDriver* gtiff_drv_ = nullptr;
  GDALDriver* mem_drv_ = nullptr;
};

TEST_F(CogDriverTest, IdentifyDoesNotExist) {
  // COG is a write-only CreateCopy generator; existing COGs are read by GTiff.
  EXPECT_EQ(nullptr, cog_drv_->pfnOpen);
  WithQuietHandler quiet;
  CPLErrorReset();
  GDALDatasetUniquePtr missing(GDALDataset::Open(
      "/does_not_exist",
      GDAL_OF_RASTER | GDAL_OF_READONLY | GDAL_OF_VERBOSE_ERROR));
  EXPECT_EQ(nullptr, missing);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(CogDriverTest, DriverMetadataRegistrationAndPublicHelpers) {
  // 1. Early-return branch of GDALRegister_COG() when already registered.
  GDALRegister_COG();

  // 2. Driver GetMetadataItem and GetMetadata (exercises
  // InitializeCreationOptionList and its m_bInitialized early return).
  const char* co_list = cog_drv_->GetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST);
  ASSERT_NE(nullptr, co_list);
  EXPECT_THAT(co_list, HasSubstr("TILING_SCHEME"));
  EXPECT_NE(nullptr, cog_drv_->GetMetadataItem(GDAL_DMD_LONGNAME));
  EXPECT_NE(nullptr, cog_drv_->GetMetadata());

  // 3. Public helpers in cogdriver.h.
  EXPECT_FALSE(COGHasWarpingOptions(nullptr));
  CPLStringList wopts;
  wopts.SetNameValue("TARGET_SRS", "EPSG:3857");
  wopts.SetNameValue("TILING_SCHEME", "GoogleMapsCompatible");
  wopts.SetNameValue("EXTENT", "0,0,100,100");
  wopts.SetNameValue("RES", "10");
  wopts.SetNameValue("ALIGNED_LEVELS", "2");
  wopts.SetNameValue("ZOOM_LEVEL_STRATEGY", "AUTO");
  wopts.SetNameValue("COMPRESS", "LZW");
  EXPECT_TRUE(COGHasWarpingOptions(wopts.List()));
  COGRemoveWarpingOptions(wopts);
  EXPECT_FALSE(COGHasWarpingOptions(wopts.List()));
  EXPECT_STREQ("LZW", wopts.FetchNameValue("COMPRESS"));

  // 4. COGGetResampling: WARP_RESAMPLING -> RESAMPLING -> ColorTable -> Complex
  // -> default CUBIC.
  {
    GDALDatasetUniquePtr ds_norm(
        mem_drv_->Create("", 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds_norm);
    EXPECT_EQ("CUBIC", COGGetResampling(ds_norm.get(), nullptr));

    CPLStringList r1;
    r1.SetNameValue("WARP_RESAMPLING", "BILINEAR");
    EXPECT_EQ("BILINEAR", COGGetResampling(ds_norm.get(), r1.List()));

    CPLStringList r2;
    r2.SetNameValue("RESAMPLING", "AVERAGE");
    EXPECT_EQ("AVERAGE", COGGetResampling(ds_norm.get(), r2.List()));

    GDALColorTable ct;
    ds_norm->GetRasterBand(1)->SetColorTable(&ct);
    EXPECT_EQ("NEAREST", COGGetResampling(ds_norm.get(), nullptr));

    GDALDatasetUniquePtr ds_cplx(
        mem_drv_->Create("", 4, 4, 1, GDT_CInt16, nullptr));
    ASSERT_NE(nullptr, ds_cplx);
    EXPECT_EQ("NEAREST", COGGetResampling(ds_cplx.get(), nullptr));
  }

  // 5. COGGetTargetSRS (2-arg) + custom inline TMS validations.
  {
    CPLString target_srs;
    EXPECT_FALSE(COGGetTargetSRS(nullptr, target_srs));

    CPLStringList custom_srs;
    custom_srs.SetNameValue("TARGET_SRS", "EPSG:4326");
    EXPECT_TRUE(COGGetTargetSRS(custom_srs.List(), target_srs));
    EXPECT_STREQ("EPSG:4326", target_srs.c_str());

    // Both TILING_SCHEME and TARGET_SRS -> warns "Ignoring TARGET_SRS option".
    CPLStringList both;
    both.SetNameValue("TILING_SCHEME", "GoogleMapsCompatible");
    both.SetNameValue("TARGET_SRS", "EPSG:4326");
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_TRUE(COGGetTargetSRS(both.List(), target_srs));
      EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("TARGET_SRS"));
    }
    EXPECT_STREQ("EPSG:3857", target_srs.c_str());

    // Non-existent scheme.
    CPLStringList bad_tm;
    bad_tm.SetNameValue("TILING_SCHEME", "DOES_NOT_EXIST_TMS");
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_FALSE(COGGetTargetSRS(bad_tm.List(), target_srs));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }

    struct TmsCase {
      std::string_view name;
      const char* json;
      bool expect_ok;
    };
    constexpr TmsCase kCases[] = {
        // Inline JSON TMS 1: mismatched topLeftCorner
        // (!haveAllLevelsSameTopLeft).
        {.name = "diff_top_left",
         .json = R"json({
      "type": "TileMatrixSetType", "identifier": "DiffTL",
      "supportedCRS": "http://www.opengis.net/def/crs/EPSG/0/3857",
      "tileMatrix": [
        {"identifier": "0", "scaleDenominator": 10000, "topLeftCorner": [0, 1000],
         "tileWidth": 256, "tileHeight": 256, "matrixWidth": 1, "matrixHeight": 1},
        {"identifier": "1", "scaleDenominator": 5000, "topLeftCorner": [10, 1000],
         "tileWidth": 256, "tileHeight": 256, "matrixWidth": 2, "matrixHeight": 2}
      ]
    })json",
         .expect_ok = false},
        // Inline JSON TMS 2: mismatched tileWidth
        // (!haveAllLevelsSameTileSize).
        {.name = "diff_tile_size",
         .json = R"json({
      "type": "TileMatrixSetType", "identifier": "DiffTS",
      "supportedCRS": "http://www.opengis.net/def/crs/EPSG/0/3857",
      "tileMatrix": [
        {"identifier": "0", "scaleDenominator": 10000, "topLeftCorner": [0, 1000],
         "tileWidth": 256, "tileHeight": 256, "matrixWidth": 2, "matrixHeight": 2},
        {"identifier": "1", "scaleDenominator": 5000, "topLeftCorner": [0, 1000],
         "tileWidth": 512, "tileHeight": 512, "matrixWidth": 2, "matrixHeight": 2}
      ]
    })json",
         .expect_ok = false},
        // Inline JSON TMS 3: variableMatrixWidths (hasVariableMatrixWidth).
        {.name = "variable_matrix_width",
         .json = R"json({
      "crs": "http://www.opengis.net/def/crs/EPSG/0/3857",
      "tileMatrices": [{
        "id": "0", "scaleDenominator": 10000, "pointOfOrigin": [0, 1000],
        "tileWidth": 256, "tileHeight": 256, "matrixWidth": 2, "matrixHeight": 2,
        "variableMatrixWidths": [{"coalesce": 2, "minTileRow": 0, "maxTileRow": 0}]
      }]
    })json",
         .expect_ok = false},
        // Inline JSON TMS 4: custom PROJ string without authority code.
        {.name = "no_authority_code",
         .json = R"json({
      "type": "TileMatrixSetType", "identifier": "NoAuth",
      "supportedCRS": "+proj=merc +a=6378137 +b=6378137 +units=m +no_defs",
      "tileMatrix": [
        {"identifier": "0", "scaleDenominator": 100000, "topLeftCorner": [0, 10000],
         "tileWidth": 256, "tileHeight": 256, "matrixWidth": 4, "matrixHeight": 4}
      ]
    })json",
         .expect_ok = true},
    };
    for (const TmsCase& tc : kCases) {
      SCOPED_TRACE(tc.name);
      std::optional<WithQuietHandler> quiet;
      if (!tc.expect_ok) {
        quiet.emplace();
      }
      CPLStringList opts;
      opts.SetNameValue("TILING_SCHEME", tc.json);
      CPLErrorReset();
      EXPECT_EQ(tc.expect_ok, COGGetTargetSRS(opts.List(), target_srs));
      EXPECT_EQ(tc.expect_ok ? CE_None : CE_Failure, CPLGetLastErrorType());
    }
  }
}

TEST_F(CogDriverTest, WarpingCharacteristicsAllBranches) {
  CPLConfigOptionSetter tmpdir("CPL_TMPDIR", "/vsimem/cogdriver_test/tmp",
                               /*bSetOnlyIfUndefined=*/false);

  CPLString resamp;
  CPLString tsrs;
  int nx = 0;
  int ny = 0;
  double minx = 0.0;
  double miny = 0.0;
  double maxx = 0.0;
  double maxy = 0.0;

  // 1. Source with NO SRS / NO GT -> hTransformArg == nullptr failure.
  {
    GDALDatasetUniquePtr no_geo(
        mem_drv_->Create("", 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, no_geo);
    CPLStringList opts;
    opts.SetNameValue("TARGET_SRS", "EPSG:3857");
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(COGGetWarpingCharacteristics(no_geo.get(), opts.List(), resamp,
                                              tsrs, nx, ny, minx, miny, maxx,
                                              maxy));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 2. EPSG:4326 polar [-180, 90] to [180, -90] -> triggers EPSG:3857 +/-85.051
  // latitude clamping VRT, custom EXTENT (valid 4 tokens, invalid 3 tokens,
  // with and without RES).
  {
    constexpr double kGtWorld[6] = {-180.0, 10.0, 0.0, 90.0, 0.0, -10.0};
    GDALDatasetUniquePtr world =
        CreateGeorefMem(36, 18, 1, GDT_Byte, "EPSG:4326", kGtWorld);
    ASSERT_NE(nullptr, world);

    CPLStringList opts;
    opts.SetNameValue("TARGET_SRS", "EPSG:3857");
    EXPECT_TRUE(COGGetWarpingCharacteristics(world.get(), opts.List(), resamp,
                                             tsrs, nx, ny, minx, miny, maxx,
                                             maxy));

    // Invalid EXTENT (3 tokens).
    opts.SetNameValue("EXTENT", "0,0,100");
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_FALSE(COGGetWarpingCharacteristics(world.get(), opts.List(),
                                                resamp, tsrs, nx, ny, minx,
                                                miny, maxx, maxy));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }

    // Valid EXTENT without RES vs with RES.
    opts.SetNameValue("EXTENT", "0,0,1000000,1000000");
    EXPECT_TRUE(COGGetWarpingCharacteristics(world.get(), opts.List(), resamp,
                                             tsrs, nx, ny, minx, miny, maxx,
                                             maxy));
    opts.SetNameValue("RES", "10000");
    EXPECT_TRUE(COGGetWarpingCharacteristics(world.get(), opts.List(), resamp,
                                             tsrs, nx, ny, minx, miny, maxx,
                                             maxy));
    EXPECT_EQ(100, nx);
    EXPECT_EQ(100, ny);
  }

  // 3. TILING_SCHEME options: EXTENT/RES ignored warnings, ZOOM_LEVEL invalid
  // & valid, ZOOM_LEVEL_STRATEGY (LOWER, UPPER, AUTO coarser & finer),
  // ALIGNED_LEVELS (valid + non-integer ratio error + coprime ratio error),
  // outside bbox error, partially outside bbox clamping, and bInvertAxis=true.
  {
    constexpr double kGtMerc[6] = {0.0, 50000.0, 0.0, 1000000.0, 0.0, -50000.0};
    GDALDatasetUniquePtr mds =
        CreateGeorefMem(20, 20, 1, GDT_Byte, "EPSG:3857", kGtMerc);
    ASSERT_NE(nullptr, mds);

    // EXTENT + RES warnings + ZOOM_LEVEL_STRATEGY=LOWER / UPPER / AUTO.
    constexpr const char* kStrategies[] = {"LOWER", "UPPER", "AUTO"};
    for (const char* strat : kStrategies) {
      SCOPED_TRACE(strat);
      CPLStringList opts;
      opts.SetNameValue("TILING_SCHEME", "GoogleMapsCompatible");
      opts.SetNameValue("EXTENT", "0,0,10,10");
      opts.SetNameValue("RES", "100");
      opts.SetNameValue("ZOOM_LEVEL_STRATEGY", strat);
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_TRUE(COGGetWarpingCharacteristics(mds.get(), opts.List(), resamp,
                                               tsrs, nx, ny, minx, miny, maxx,
                                               maxy));
      EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    }

    // Coarser resolution closer to prev zoom level in AUTO:
    // Zoom 1 res = 78271.517, Zoom 2 res = 39135.758; geometric mean ~55346.
    // 50000 is < 55346 (closer to Zoom 2); 70000 is > 55346 (closer to Zoom 1).
    {
      constexpr double kGtCoarse[6] = {0.0,       70000.0, 0.0,
                                       1400000.0, 0.0,     -70000.0};
      GDALDatasetUniquePtr cds =
          CreateGeorefMem(20, 20, 1, GDT_Byte, "EPSG:3857", kGtCoarse);
      ASSERT_NE(nullptr, cds);
      CPLStringList opts;
      opts.SetNameValue("TILING_SCHEME", "GoogleMapsCompatible");
      EXPECT_TRUE(COGGetWarpingCharacteristics(cds.get(), opts.List(), resamp,
                                               tsrs, nx, ny, minx, miny, maxx,
                                               maxy));
    }

    // Ultra-fine resolution finer than max zoom level -> fails.
    {
      constexpr double kGtTiny[6] = {0.0, 1e-6, 0.0, 1.0, 0.0, -1e-6};
      GDALDatasetUniquePtr tds =
          CreateGeorefMem(16, 16, 1, GDT_Byte, "EPSG:3857", kGtTiny);
      ASSERT_NE(nullptr, tds);
      CPLStringList opts;
      opts.SetNameValue("TILING_SCHEME", "GoogleMapsCompatible");
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_FALSE(COGGetWarpingCharacteristics(tds.get(), opts.List(), resamp,
                                                tsrs, nx, ny, minx, miny, maxx,
                                                maxy));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }

    // Invalid ZOOM_LEVEL=99 and valid ZOOM_LEVEL=2 + ALIGNED_LEVELS=2.
    {
      CPLStringList o_bad;
      o_bad.SetNameValue("TILING_SCHEME", "GoogleMapsCompatible");
      o_bad.SetNameValue("ZOOM_LEVEL", "99");
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        EXPECT_FALSE(COGGetWarpingCharacteristics(mds.get(), o_bad.List(),
                                                  resamp, tsrs, nx, ny, minx,
                                                  miny, maxx, maxy));
        EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      }

      CPLStringList o_al;
      o_al.SetNameValue("TILING_SCHEME", "GoogleMapsCompatible");
      o_al.SetNameValue("ZOOM_LEVEL", "2");
      o_al.SetNameValue("ALIGNED_LEVELS", "2");
      EXPECT_TRUE(COGGetWarpingCharacteristics(mds.get(), o_al.List(), resamp,
                                               tsrs, nx, ny, minx, miny, maxx,
                                               maxy));
    }

    // Partially outside TMS bbox clamping: [19000000, 0] to [21000000,
    // 2000000].
    {
      constexpr double kGtEdge[6] = {19000000.0, 100000.0, 0.0,
                                     2000000.0,  0.0,      -100000.0};
      GDALDatasetUniquePtr eds =
          CreateGeorefMem(20, 20, 1, GDT_Byte, "EPSG:3857", kGtEdge);
      ASSERT_NE(nullptr, eds);
      CPLStringList opts;
      opts.SetNameValue("TILING_SCHEME", "GoogleMapsCompatible");
      opts.SetNameValue("ZOOM_LEVEL", "1");
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_TRUE(COGGetWarpingCharacteristics(eds.get(), opts.List(), resamp,
                                               tsrs, nx, ny, minx, miny, maxx,
                                               maxy));
      EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    }

    // Completely outside TMS bbox + ALIGNED_LEVELS unsupported ratios +
    // bInvertAxis (EPSG:4326 TMS).
    {
      constexpr const char* kBoxTms = R"json({
        "type": "TileMatrixSetType", "identifier": "SmallBox",
        "supportedCRS": "http://www.opengis.net/def/crs/EPSG/0/3857",
        "boundingBox": {
          "crs": "http://www.opengis.net/def/crs/EPSG/0/3857",
          "lowerCorner": [-5000000, -5000000],
          "upperCorner": [-4000000, -4000000]
        },
        "tileMatrix": [
          {"identifier": "0", "scaleDenominator": 100000, "topLeftCorner": [-5000000, -4000000],
           "tileWidth": 256, "tileHeight": 256, "matrixWidth": 4, "matrixHeight": 4}
        ]
      })json";
      CPLStringList o_out;
      o_out.SetNameValue("TILING_SCHEME", kBoxTms);
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        EXPECT_FALSE(COGGetWarpingCharacteristics(mds.get(), o_out.List(),
                                                  resamp, tsrs, nx, ny, minx,
                                                  miny, maxx, maxy));
        EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      }

      // Non-integer 60/ratio: scaleDenominator 100000000 and 11000000
      // (60*11/100=6.6). Requires ZOOM_LEVEL=2 and ALIGNED_LEVELS=2 so
      // nAlignedLevels=min(2,2)=2.
      constexpr const char* kBadRatio1 = R"json({
        "type": "TileMatrixSetType", "identifier": "BadR1",
        "supportedCRS": "http://www.opengis.net/def/crs/EPSG/0/3857",
        "tileMatrix": [
          {"identifier": "0", "scaleDenominator": 200000000, "topLeftCorner": [0, 2000000],
           "tileWidth": 256, "tileHeight": 256, "matrixWidth": 2, "matrixHeight": 2},
          {"identifier": "1", "scaleDenominator": 100000000, "topLeftCorner": [0, 2000000],
           "tileWidth": 256, "tileHeight": 256, "matrixWidth": 4, "matrixHeight": 4},
          {"identifier": "2", "scaleDenominator": 11000000, "topLeftCorner": [0, 2000000],
           "tileWidth": 256, "tileHeight": 256, "matrixWidth": 40, "matrixHeight": 40}
        ]
      })json";
      CPLStringList o_r1;
      o_r1.SetNameValue("TILING_SCHEME", kBadRatio1);
      o_r1.SetNameValue("ZOOM_LEVEL", "2");
      o_r1.SetNameValue("ALIGNED_LEVELS", "2");
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        EXPECT_FALSE(COGGetWarpingCharacteristics(mds.get(), o_r1.List(),
                                                  resamp, tsrs, nx, ny, minx,
                                                  miny, maxx, maxy));
        EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      }

      // Coprime integer 60/ratio = 7: scaleDenominator 60000000 and 7000000.
      constexpr const char* kBadRatio2 = R"json({
        "type": "TileMatrixSetType", "identifier": "BadR2",
        "supportedCRS": "http://www.opengis.net/def/crs/EPSG/0/3857",
        "tileMatrix": [
          {"identifier": "0", "scaleDenominator": 120000000, "topLeftCorner": [0, 2000000],
           "tileWidth": 256, "tileHeight": 256, "matrixWidth": 2, "matrixHeight": 2},
          {"identifier": "1", "scaleDenominator": 60000000, "topLeftCorner": [0, 2000000],
           "tileWidth": 256, "tileHeight": 256, "matrixWidth": 4, "matrixHeight": 4},
          {"identifier": "2", "scaleDenominator": 7000000, "topLeftCorner": [0, 2000000],
           "tileWidth": 256, "tileHeight": 256, "matrixWidth": 40, "matrixHeight": 40}
        ]
      })json";
      CPLStringList o_r2;
      o_r2.SetNameValue("TILING_SCHEME", kBadRatio2);
      o_r2.SetNameValue("ZOOM_LEVEL", "2");
      o_r2.SetNameValue("ALIGNED_LEVELS", "2");
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        EXPECT_FALSE(COGGetWarpingCharacteristics(mds.get(), o_r2.List(),
                                                  resamp, tsrs, nx, ny, minx,
                                                  miny, maxx, maxy));
        EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      }

      // EPSG:4326 TMS -> bInvertAxis == true.
      constexpr const char* kLatLonTms = R"json({
        "type": "TileMatrixSetType", "identifier": "LL4326",
        "supportedCRS": "http://www.opengis.net/def/crs/EPSG/0/4326",
        "boundingBox": {
          "crs": "http://www.opengis.net/def/crs/EPSG/0/4326",
          "lowerCorner": [-90, -180],
          "upperCorner": [90, 180]
        },
        "tileMatrix": [
          {"identifier": "0", "scaleDenominator": 279541132.0143589, "topLeftCorner": [90, -180],
           "tileWidth": 256, "tileHeight": 256, "matrixWidth": 2, "matrixHeight": 1}
        ]
      })json";
      CPLStringList o_ll;
      o_ll.SetNameValue("TILING_SCHEME", kLatLonTms);
      o_ll.SetNameValue("ZOOM_LEVEL", "0");
      EXPECT_TRUE(COGGetWarpingCharacteristics(mds.get(), o_ll.List(), resamp,
                                               tsrs, nx, ny, minx, miny, maxx,
                                               maxy));
    }
  }
}

TEST_F(CogDriverTest, CreateCopyCodecsWarpingStatsMasksAndOverviews) {
  CPLConfigOptionSetter tmpdir("CPL_TMPDIR", "/vsimem/cogdriver_test/tmp",
                               /*bSetOnlyIfUndefined=*/false);

  auto run_copy = [&](GDALDataset* src, const char* path,
                      const CPLStringList& opts, bool expect_ok = true,
                      GDALProgressFunc prog = nullptr) {
    absl::Cleanup clean_out = [path]() {
      VSIUnlink(path);
      VSIUnlink((std::string(path) + ".ovr.tmp").c_str());
    };
    const char* comp_opt = opts.FetchNameValueDef("COMPRESS", "");
    const CPLErr expected_ok_err =
        STARTS_WITH(comp_opt, "LERC") ? CE_Warning : CE_None;
    std::optional<WithQuietHandler> quiet;
    if (!expect_ok || expected_ok_err != CE_None ||
        opts.FetchNameValue("BLOCKSIZE") != nullptr) {
      quiet.emplace();
    }
    CPLErrorReset();
    GDALDatasetUniquePtr out(
        cog_drv_->CreateCopy(path, src, FALSE, opts.List(), prog, nullptr));
    if (expect_ok) {
      EXPECT_EQ(expected_ok_err, CPLGetLastErrorType()) << path;
      if (expected_ok_err == CE_Warning) {
        EXPECT_THAT(CPLGetLastErrorMsg(),
                    HasSubstr("PREDICTOR option is ignored"))
            << path;
      }
      CPLErrorReset();
      EXPECT_NE(nullptr, out) << path;
    } else {
      EXPECT_EQ(nullptr, out) << path;
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType()) << path;
    }
    if (out == nullptr) return;

    // Cheap oracles: when the requested codec maps 1:1 onto the GTiff
    // COMPRESSION name, check it was honoured.
    const char* comp = opts.FetchNameValue("COMPRESS");
    if (comp != nullptr && (EQUAL(comp, "DEFLATE") || EQUAL(comp, "ZSTD") ||
                            EQUAL(comp, "LZMA") || STARTS_WITH(comp, "LERC"))) {
      EXPECT_STREQ(comp, out->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"))
          << path;
    }
    out.reset();

    // LAYOUT=COG is only derived from the file header when a GTiff is opened
    // from disk (not on the handle returned by CreateCopy), so reopen the
    // result read-only to verify it. @SUPPRESS_ASAP outputs are deleted on
    // close and cannot be reopened.
    if (!CPLTestBool(opts.FetchNameValueDef("@SUPPRESS_ASAP", "NO"))) {
      GDALDatasetUniquePtr reopened(
          GDALDataset::Open(path, GDAL_OF_RASTER | GDAL_OF_READONLY));
      EXPECT_NE(nullptr, reopened) << path;
      if (reopened != nullptr) {
        EXPECT_STREQ("COG",
                     reopened->GetMetadataItem("LAYOUT", "IMAGE_STRUCTURE"))
            << path;
      }
    }
  };

  // 1. 0-band error and WEBP INTERLEAVE=BAND error.
  {
    GDALDatasetUniquePtr ds0(
        mem_drv_->Create("", 16, 16, 0, GDT_Byte, nullptr));
    CPLStringList empty;
    run_copy(ds0.get(), "/vsimem/cogdriver_test/0band.tif", empty, false);

    GDALDatasetUniquePtr ds3(
        mem_drv_->Create("", 16, 16, 3, GDT_Byte, nullptr));
    CPLStringList bad_webp;
    bad_webp.SetNameValue("COMPRESS", "WEBP");
    bad_webp.SetNameValue("INTERLEAVE", "BAND");
    run_copy(ds3.get(), "/vsimem/cogdriver_test/badwebp.tif", bad_webp, false);
  }

  // 2. Warping via CreateReprojectedDS:
  // (a) Full warp with mask + overviews + NUM_THREADS=2 + BIGTIFF=YES +
  // STATISTICS=YES.
  // (b) Inconsistent -tr (EXTENT=0,0,100000,100000, RES=33333, ADD_ALPHA=NO).
  // (c) Skip reprojection step when source already matches GoogleMapsCompatible
  // zoom 1.
  // (d) Warp error (WARP_RESAMPLING=BOGUS_RESAMP) and warp progress cancel.
  {
    constexpr double kGt[6] = {0.0, 20000.0, 0.0, 600000.0, 0.0, -20000.0};
    GDALDatasetUniquePtr src =
        CreateGeorefMem(32, 32, 1, GDT_Byte, "EPSG:3857", kGt);
    ASSERT_NE(nullptr, src);
    ASSERT_EQ(CE_None, src->CreateMaskBand(GMF_PER_DATASET));
    ASSERT_EQ(CE_None, src->GetRasterBand(1)->GetMaskBand()->Fill(255));

    CPLStringList o1;
    o1.SetNameValue("TILING_SCHEME", "GoogleMapsCompatible");
    o1.SetNameValue("ZOOM_LEVEL", "2");
    o1.SetNameValue("ALIGNED_LEVELS", "2");
    o1.SetNameValue("BIGTIFF", "YES");
#if defined(THREAD_SANITIZER) || defined(__SANITIZE_THREAD__)
    // Upstream GTiffDataset::FlushCacheInternal() clears bit-field
    // m_bLoadedBlockDirty before WaitCompletion(), racing with worker threads
    // reading sibling bit-fields in RestoreVolatileParameters()
    // (gtiffdataset.h:256, gtiffdataset_write.cpp:2188).
    o1.SetNameValue("NUM_THREADS", "1");
#else
    o1.SetNameValue("NUM_THREADS", "2");
#endif
    o1.SetNameValue("STATISTICS", "YES");
    run_copy(src.get(), "/vsimem/cogdriver_test/warp_full.tif", o1, true,
             GDALDummyProgress);

    // Zoom level 0 with OVERVIEW_COUNT=5: starts at coarsest zoom level 0
    // (256x256, BLOCKSIZE defaults to 256 so 1x1 tile) and generates 5
    // overview levels down to 128, 64, 32, 16, and 8 pixels, hitting the
    // nCurLevel < 1 branch in COG overview-level loop (cogdriver.cpp:1399)
    // and testing all zoom-0 overview levels.
    CPLStringList o_z0;
    o_z0.SetNameValue("TILING_SCHEME", "GoogleMapsCompatible");
    o_z0.SetNameValue("ZOOM_LEVEL", "0");
    o_z0.SetNameValue("OVERVIEW_COUNT", "5");
    run_copy(src.get(), "/vsimem/cogdriver_test/warp_z0.tif", o_z0, true);

    // Inconsistent -tr + ADD_ALPHA=NO + OVERVIEWS=NONE.
    CPLStringList o2;
    o2.SetNameValue("TARGET_SRS", "EPSG:3857");
    o2.SetNameValue("EXTENT", "0,0,100000,100000");
    o2.SetNameValue("RES", "33333");
    o2.SetNameValue("ADD_ALPHA", "NO");
    o2.SetNameValue("OVERVIEWS", "NONE");
    run_copy(src.get(), "/vsimem/cogdriver_test/warp_inconsist.tif", o2, true);

    // Invalid WARP_RESAMPLING -> CreateReprojectedDS returns nullptr.
    CPLStringList o_badw;
    o_badw.SetNameValue("TARGET_SRS", "EPSG:3857");
    o_badw.SetNameValue("WARP_RESAMPLING", "BOGUS_RESAMP");
    run_copy(src.get(), "/vsimem/cogdriver_test/warp_bad.tif", o_badw, false);

    // Progress cancel during warp.
    auto cancel_first = [](double, const char*, void*) -> int { return 0; };
    CPLStringList o_canc;
    o_canc.SetNameValue("TILING_SCHEME", "GoogleMapsCompatible");
    o_canc.SetNameValue("ZOOM_LEVEL", "1");
    run_copy(src.get(), "/vsimem/cogdriver_test/warp_cancel.tif", o_canc, false,
             cancel_first);

    // Source already matching GoogleMapsCompatible zoom 1 (512x512) -> skips
    // CreateReprojectedDS.
    constexpr double kHalf = 20037508.342789244;
    constexpr double kRes1 = (2.0 * kHalf) / 512.0;
    constexpr double kGtExact[6] = {-kHalf, kRes1, 0.0, kHalf, 0.0, -kRes1};
    GDALDatasetUniquePtr exact_ds =
        CreateGeorefMem(512, 512, 1, GDT_Byte, "EPSG:3857", kGtExact);
    ASSERT_NE(nullptr, exact_ds);
    CPLStringList o_skip;
    o_skip.SetNameValue("TILING_SCHEME", "GoogleMapsCompatible");
    o_skip.SetNameValue("ZOOM_LEVEL", "1");
    o_skip.SetNameValue("OVERVIEWS", "NONE");
    run_copy(exact_ds.get(), "/vsimem/cogdriver_test/warp_skip.tif", o_skip,
             true);
  }

  // 3. JPEG 2-band Gray+Alpha & 4-band RGBA -> m_poRGBMaskDS conversion and
  // STATISTICS=NO / AUTO with existing source stats.
  {
    constexpr int kBandCounts[] = {2, 4};
    for (int nb : kBandCounts) {
      SCOPED_TRACE(nb);
      GDALDatasetUniquePtr ds(
          mem_drv_->Create("", 160, 160, nb, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
      if (nb == 2) {
        ds->GetRasterBand(1)->SetColorInterpretation(GCI_GrayIndex);
        ds->GetRasterBand(2)->SetColorInterpretation(GCI_AlphaBand);
      } else {
        ds->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand);
        ds->GetRasterBand(2)->SetColorInterpretation(GCI_GreenBand);
        ds->GetRasterBand(3)->SetColorInterpretation(GCI_BlueBand);
        ds->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand);
      }
      ds->GetRasterBand(1)->SetMetadataItem("STATISTICS_MINIMUM", "0");
      ds->GetRasterBand(1)->SetMetadataItem("STATISTICS_MAXIMUM", "255");
      ds->GetRasterBand(1)->SetMetadataItem("STATISTICS_MEAN", "127");
      ds->GetRasterBand(1)->SetMetadataItem("STATISTICS_STDDEV", "10");

      CPLStringList o_auto;
      o_auto.SetNameValue("COMPRESS", "JPEG");
      o_auto.SetNameValue("BLOCKSIZE", "128");
      o_auto.SetNameValue("QUALITY", "80");
      run_copy(ds.get(), "/vsimem/cogdriver_test/jmask_auto.tif", o_auto, true);

      CPLStringList o_nostat;
      o_nostat.SetNameValue("COMPRESS", "JPEG");
      o_nostat.SetNameValue("STATISTICS", "NO");
      o_nostat.SetNameValue("BLOCKSIZE", "128");
      run_copy(ds.get(), "/vsimem/cogdriver_test/jmask_nostat.tif", o_nostat,
               true);
    }

    // STATISTICS=YES and STATISTICS=NO without warp/RGBMask (creates VRT).
    GDALDatasetUniquePtr sds(
        mem_drv_->Create("", 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, sds);
    ASSERT_EQ(CE_None, sds->GetRasterBand(1)->Fill(42));
    CPLStringList o_syes;
    o_syes.SetNameValue("STATISTICS", "YES");
    run_copy(sds.get(), "/vsimem/cogdriver_test/stat_yes.tif", o_syes, true);

    sds->GetRasterBand(1)->SetMetadataItem("STATISTICS_MINIMUM", "42");
    sds->GetRasterBand(1)->SetMetadataItem("STATISTICS_MAXIMUM", "42");
    sds->GetRasterBand(1)->SetMetadataItem("STATISTICS_MEAN", "42");
    sds->GetRasterBand(1)->SetMetadataItem("STATISTICS_STDDEV", "0");
    CPLStringList o_sno;
    o_sno.SetNameValue("STATISTICS", "NO");
    run_copy(sds.get(), "/vsimem/cogdriver_test/stat_no.tif", o_sno, true);
  }

  // 4. Existing overviews on source GTiff + !bGenerateOvr && bGenerateMskOvr +
  // invalid OVERVIEW_RESAMPLING failures + COG_DELETE_TEMP_FILES=NO (without
  // CPL_TMPDIR so GetTmpFilename else branch is hit too).
  {
    constexpr char kSrcOvr[] = "/vsimem/cogdriver_test/src_with_ovr.tif";
    GDALDatasetUniquePtr gds(
        gtiff_drv_->Create(kSrcOvr, 256, 256, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, gds);
    int ovrs[2] = {2, 4};
    ASSERT_EQ(CE_None, gds->BuildOverviews("AVERAGE", 2, ovrs, 0, nullptr,
                                           nullptr, nullptr));
    // Add per-dataset mask AFTER BuildOverviews so imagery has 2 overviews
    // while mask has 0 overviews.
    ASSERT_EQ(CE_None, gds->CreateMaskBand(GMF_PER_DATASET));

    CPLStringList o_use;
    o_use.SetNameValue("BLOCKSIZE", "64");
    o_use.SetNameValue("OVERVIEW_COUNT", "1");
    run_copy(gds.get(), "/vsimem/cogdriver_test/existing_ovr_plus_msk.tif",
             o_use, true);

    // Mask overview failure with OVERVIEW_RESAMPLING=INVALID.
    CPLStringList o_bad_mskovr;
    o_bad_mskovr.SetNameValue("BLOCKSIZE", "64");
    o_bad_mskovr.SetNameValue("OVERVIEW_RESAMPLING", "INVALID_METHOD");
    run_copy(gds.get(), "/vsimem/cogdriver_test/bad_mskovr.tif", o_bad_mskovr,
             false);
    gds.reset();

    // Imagery overview failure (no mask) + COG_DELETE_TEMP_FILES=NO without
    // CPL_TMPDIR.
    CPLConfigOptionSetter unset_tmp("CPL_TMPDIR", nullptr,
                                    /*bSetOnlyIfUndefined=*/false);
    CPLConfigOptionSetter keep_tmp("COG_DELETE_TEMP_FILES", "NO",
                                   /*bSetOnlyIfUndefined=*/false);
    GDALDatasetUniquePtr ds1(
        mem_drv_->Create("", 160, 160, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds1);
    CPLStringList o_bad_ovr;
    o_bad_ovr.SetNameValue("BLOCKSIZE", "64");
    o_bad_ovr.SetNameValue("OVERVIEW_RESAMPLING", "INVALID_METHOD");
    run_copy(ds1.get(), "/vsimem/cogdriver_test/bad_imgovr.tif", o_bad_ovr,
             false);
  }

  // 5. All codecs & pass-through options.
  {
    GDALDatasetUniquePtr rgb(
        mem_drv_->Create("", 160, 160, 3, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, rgb);
    rgb->SetMetadataItem("K", "V", "DOM1");

    // WEBP lossless + OVERVIEW_COMPRESS=WEBP lossless.
    CPLStringList o_wl;
    o_wl.SetNameValue("COMPRESS", "WEBP");
    o_wl.SetNameValue("QUALITY", "100");
    o_wl.SetNameValue("BLOCKSIZE", "64");
    o_wl.SetNameValue("OVERVIEW_COMPRESS", "WEBP");
    o_wl.SetNameValue("OVERVIEW_QUALITY", "100");
    run_copy(rgb.get(), "/vsimem/cogdriver_test/webp_lossless.tif", o_wl, true);

    // WEBP lossy + OVERVIEW_COMPRESS=WEBP lossy.
    CPLStringList o_wlossy;
    o_wlossy.SetNameValue("COMPRESS", "WEBP");
    o_wlossy.SetNameValue("QUALITY", "80");
    o_wlossy.SetNameValue("BLOCKSIZE", "64");
    o_wlossy.SetNameValue("OVERVIEW_COMPRESS", "WEBP");
    o_wlossy.SetNameValue("OVERVIEW_QUALITY", "75");
    run_copy(rgb.get(), "/vsimem/cogdriver_test/webp_lossy.tif", o_wlossy,
             true);

    // ZSTD + OVERVIEW_COMPRESS=JPEG + INTERLEAVE=PIXEL + SRC_MDD +
    // COPY_SRC_MDD.
    CPLStringList o_zstd;
    o_zstd.SetNameValue("COMPRESS", "ZSTD");
    o_zstd.SetNameValue("LEVEL", "3");
    o_zstd.SetNameValue("PREDICTOR", "YES");
    o_zstd.SetNameValue("BLOCKSIZE", "64");
    o_zstd.SetNameValue("OVERVIEW_COMPRESS", "JPEG");
    o_zstd.SetNameValue("COPY_SRC_MDD", "YES");
    o_zstd.AddNameValue("SRC_MDD", "DOM1");
    run_copy(rgb.get(), "/vsimem/cogdriver_test/zstd_jpgovr.tif", o_zstd, true);

    // LZMA + INTERLEAVE=TILE + PREDICTOR=STANDARD +
    // OVERVIEW_PREDICTOR=STANDARD.
    CPLStringList o_lzma;
    o_lzma.SetNameValue("COMPRESS", "LZMA");
    o_lzma.SetNameValue("LEVEL", "1");
    o_lzma.SetNameValue("INTERLEAVE", "TILE");
    o_lzma.SetNameValue("PREDICTOR", "STANDARD");
    o_lzma.SetNameValue("OVERVIEW_PREDICTOR", "STANDARD");
    o_lzma.SetNameValue("BLOCKSIZE", "64");
    run_copy(rgb.get(), "/vsimem/cogdriver_test/lzma_tile.tif", o_lzma, true);

    // Float32 LERC / LERC_DEFLATE / LERC_ZSTD + PREDICTOR=YES & FLOATING_POINT.
    GDALDatasetUniquePtr fds(
        mem_drv_->Create("", 160, 160, 1, GDT_Float32, nullptr));
    ASSERT_NE(nullptr, fds);
    constexpr const char* kLercCodecs[] = {"LERC", "LERC_DEFLATE", "LERC_ZSTD"};
    for (const char* lerc_c : kLercCodecs) {
      SCOPED_TRACE(lerc_c);
      CPLStringList ol;
      ol.SetNameValue("COMPRESS", lerc_c);
      ol.SetNameValue("LEVEL", "3");
      ol.SetNameValue("PREDICTOR", "YES");
      ol.SetNameValue("OVERVIEW_PREDICTOR", "FLOATING_POINT");
      ol.SetNameValue("MAX_Z_ERROR", "0.25");
      ol.SetNameValue("MAX_Z_ERROR_OVERVIEW", "0.5");
      ol.SetNameValue("BLOCKSIZE", "64");
      run_copy(fds.get(), "/vsimem/cogdriver_test/lerc.tif", ol, true);
    }

    // JXL 4-band RGBA + GEOTIFF_VERSION=1.1 + SPARSE_OK=YES.
    GDALDatasetUniquePtr rgba(
        mem_drv_->Create("", 32, 32, 4, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, rgba);
    CPLStringList ojxl;
    ojxl.SetNameValue("COMPRESS", "JXL");
    ojxl.SetNameValue("JXL_LOSSLESS", "NO");
    ojxl.SetNameValue("JXL_EFFORT", "3");
    ojxl.SetNameValue("JXL_DISTANCE", "1.5");
    ojxl.SetNameValue("JXL_ALPHA_DISTANCE", "0.5");
    ojxl.SetNameValue("GEOTIFF_VERSION", "1.1");
    ojxl.SetNameValue("SPARSE_OK", "YES");
    run_copy(rgba.get(), "/vsimem/cogdriver_test/jxl.tif", ojxl, true);

    // 1x4 DEFLATE + NBITS=7 + OVERVIEW_COUNT=4 + @SUPPRESS_ASAP=YES -> halves
    // 1x4 -> (1, 2) -> (1, 1) -> hits nTmpXSize == 0 -> 1 clamping and the
    // 1x1 early break (cogdriver.cpp:1429-1434) before level 3.
    GDALDatasetUniquePtr thin(mem_drv_->Create("", 1, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, thin);
    CPLStringList othin;
    othin.SetNameValue("COMPRESS", "DEFLATE");
    othin.SetNameValue("LEVEL", "6");
    othin.SetNameValue("NBITS", "7");
    othin.SetNameValue("OVERVIEW_COUNT", "4");
    othin.SetNameValue("@SUPPRESS_ASAP", "YES");
    run_copy(thin.get(), "/vsimem/cogdriver_test/thin.tif", othin, true);
  }
}

}  // namespace
}  // namespace autotest2
