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

#include "third_party/gdal/frmts/gtiff/cogdriver.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/frmts/gtiff/gtiff.h"
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

class CogDriverTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALAllRegister();
    GTiffOneTimeInit();
    cog_drv_ = static_cast<GDALDriver*>(GDALGetDriverByName("COG"));
    gtiff_drv_ = static_cast<GDALDriver*>(GDALGetDriverByName("GTiff"));
    mem_drv_ = static_cast<GDALDriver*>(GDALGetDriverByName("MEM"));
    ASSERT_NE(cog_drv_, nullptr);
    ASSERT_NE(gtiff_drv_, nullptr);
    ASSERT_NE(mem_drv_, nullptr);
  }

  GDALDataset* CreateGeorefMem(int w, int h, int bands, GDALDataType dt,
                               const char* srs_str, const double* gt) {
    GDALDataset* ds = mem_drv_->Create("", w, h, bands, dt, nullptr);
    if (srs_str != nullptr) {
      OGRSpatialReference srs;
      srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
      srs.SetFromUserInput(srs_str);
      (void)ds->SetSpatialRef(&srs);
    }
    if (gt != nullptr) {
      (void)ds->SetGeoTransform(const_cast<double*>(gt));
    }
    return ds;
  }

  GDALDriver* cog_drv_ = nullptr;
  GDALDriver* gtiff_drv_ = nullptr;
  GDALDriver* mem_drv_ = nullptr;
};

TEST_F(CogDriverTest, DriverMetadataRegistrationAndPublicHelpers) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);

  // Early-return branch of GDALRegister_COG() when already registered.
  GDALRegister_COG();

  // Driver GetMetadataItem (CREATIONOPTIONLIST vs other) and GetMetadata
  // (exercises InitializeCreationOptionList AND m_bInitialized early return!).
  const char* co_list = cog_drv_->GetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST);
  ASSERT_NE(co_list, nullptr);
  EXPECT_NE(std::strstr(co_list, "TILING_SCHEME"), nullptr);
  EXPECT_NE(cog_drv_->GetMetadataItem(GDAL_DMD_LONGNAME), nullptr);
  EXPECT_NE(cog_drv_->GetMetadata(), nullptr);

  // Public helpers in cogdriver.h:
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
  EXPECT_STREQ(wopts.FetchNameValue("COMPRESS"), "LZW");

  // COGGetResampling: WARP_RESAMPLING -> RESAMPLING -> ColorTable -> Complex ->
  // default CUBIC.
  {
    GDALDataset* ds_norm = mem_drv_->Create("", 4, 4, 1, GDT_Byte, nullptr);
    EXPECT_EQ(COGGetResampling(ds_norm, nullptr), "CUBIC");

    CPLStringList r1;
    r1.SetNameValue("WARP_RESAMPLING", "BILINEAR");
    EXPECT_EQ(COGGetResampling(ds_norm, r1.List()), "BILINEAR");

    CPLStringList r2;
    r2.SetNameValue("RESAMPLING", "AVERAGE");
    EXPECT_EQ(COGGetResampling(ds_norm, r2.List()), "AVERAGE");

    GDALColorTable ct;
    ds_norm->GetRasterBand(1)->SetColorTable(&ct);
    EXPECT_EQ(COGGetResampling(ds_norm, nullptr), "NEAREST");
    GDALClose(ds_norm);

    GDALDataset* ds_cplx = mem_drv_->Create("", 4, 4, 1, GDT_CInt16, nullptr);
    EXPECT_EQ(COGGetResampling(ds_cplx, nullptr), "NEAREST");
    GDALClose(ds_cplx);
  }

  // COGGetTargetSRS (2-arg) + custom inline TMS validations!
  {
    CPLString target_srs;
    EXPECT_FALSE(COGGetTargetSRS(nullptr, target_srs));

    CPLStringList custom_srs;
    custom_srs.SetNameValue("TARGET_SRS", "EPSG:4326");
    EXPECT_TRUE(COGGetTargetSRS(custom_srs.List(), target_srs));
    EXPECT_STREQ(target_srs.c_str(), "EPSG:4326");

    // Both TILING_SCHEME and TARGET_SRS -> warns "Ignoring TARGET_SRS option".
    CPLStringList both;
    both.SetNameValue("TILING_SCHEME", "GoogleMapsCompatible");
    both.SetNameValue("TARGET_SRS", "EPSG:4326");
    EXPECT_TRUE(COGGetTargetSRS(both.List(), target_srs));
    EXPECT_STREQ(target_srs.c_str(), "EPSG:3857");

    // Non-existent scheme.
    CPLStringList bad_tm;
    bad_tm.SetNameValue("TILING_SCHEME", "DOES_NOT_EXIST_TMS");
    EXPECT_FALSE(COGGetTargetSRS(bad_tm.List(), target_srs));

    // Inline JSON TMS 1: mismatched topLeftCorner (!haveAllLevelsSameTopLeft).
    const char* kDiffTopLeft = R"json({
      "type": "TileMatrixSetType", "identifier": "DiffTL",
      "supportedCRS": "http://www.opengis.net/def/crs/EPSG/0/3857",
      "tileMatrix": [
        {"identifier": "0", "scaleDenominator": 10000, "topLeftCorner": [0, 1000],
         "tileWidth": 256, "tileHeight": 256, "matrixWidth": 1, "matrixHeight": 1},
        {"identifier": "1", "scaleDenominator": 5000, "topLeftCorner": [10, 1000],
         "tileWidth": 256, "tileHeight": 256, "matrixWidth": 2, "matrixHeight": 2}
      ]
    })json";
    CPLStringList o_tl;
    o_tl.SetNameValue("TILING_SCHEME", kDiffTopLeft);
    EXPECT_FALSE(COGGetTargetSRS(o_tl.List(), target_srs));

    // Inline JSON TMS 2: mismatched tileWidth (!haveAllLevelsSameTileSize).
    const char* kDiffTileSz = R"json({
      "type": "TileMatrixSetType", "identifier": "DiffTS",
      "supportedCRS": "http://www.opengis.net/def/crs/EPSG/0/3857",
      "tileMatrix": [
        {"identifier": "0", "scaleDenominator": 10000, "topLeftCorner": [0, 1000],
         "tileWidth": 256, "tileHeight": 256, "matrixWidth": 2, "matrixHeight": 2},
        {"identifier": "1", "scaleDenominator": 5000, "topLeftCorner": [0, 1000],
         "tileWidth": 512, "tileHeight": 512, "matrixWidth": 2, "matrixHeight": 2}
      ]
    })json";
    CPLStringList o_ts;
    o_ts.SetNameValue("TILING_SCHEME", kDiffTileSz);
    EXPECT_FALSE(COGGetTargetSRS(o_ts.List(), target_srs));

    // Inline JSON TMS 3: variableMatrixWidths (hasVariableMatrixWidth).
    const char* kVarMW = R"json({
      "crs": "http://www.opengis.net/def/crs/EPSG/0/3857",
      "tileMatrices": [{
        "id": "0", "scaleDenominator": 10000, "pointOfOrigin": [0, 1000],
        "tileWidth": 256, "tileHeight": 256, "matrixWidth": 2, "matrixHeight": 2,
        "variableMatrixWidths": [{"coalesce": 2, "minTileRow": 0, "maxTileRow": 0}]
      }]
    })json";
    CPLStringList o_vmw;
    o_vmw.SetNameValue("TILING_SCHEME", kVarMW);
    EXPECT_FALSE(COGGetTargetSRS(o_vmw.List(), target_srs));

    // Inline JSON TMS 4: custom PROJ string without authority code!
    const char* kNoAuth = R"json({
      "type": "TileMatrixSetType", "identifier": "NoAuth",
      "supportedCRS": "+proj=merc +a=6378137 +b=6378137 +units=m +no_defs",
      "tileMatrix": [
        {"identifier": "0", "scaleDenominator": 100000, "topLeftCorner": [0, 10000],
         "tileWidth": 256, "tileHeight": 256, "matrixWidth": 4, "matrixHeight": 4}
      ]
    })json";
    CPLStringList o_na;
    o_na.SetNameValue("TILING_SCHEME", kNoAuth);
    EXPECT_TRUE(COGGetTargetSRS(o_na.List(), target_srs));
  }
}

TEST_F(CogDriverTest, WarpingCharacteristicsAllBranches) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);
  CPLConfigOptionSetter tmpdir("CPL_TMPDIR", "/vsimem/cog_tmp", false);

  CPLString resamp, tsrs;
  int nx = 0, ny = 0;
  double minx = 0, miny = 0, maxx = 0, maxy = 0;

  // 1. Source with NO SRS / NO GT -> hTransformArg == nullptr failure!
  {
    GDALDataset* no_geo = mem_drv_->Create("", 16, 16, 1, GDT_Byte, nullptr);
    CPLStringList opts;
    opts.SetNameValue("TARGET_SRS", "EPSG:3857");
    EXPECT_FALSE(COGGetWarpingCharacteristics(no_geo, opts.List(), resamp, tsrs,
                                              nx, ny, minx, miny, maxx, maxy));
    GDALClose(no_geo);
  }

  // 2. EPSG:4326 polar [-180, 90] to [180, -90] -> triggers EPSG:3857 +/-85.051
  // latitude clamping VRT! Also test custom EXTENT (valid 4 tokens, invalid 3
  // tokens, with and without RES).
  {
    const double gt_world[6] = {-180.0, 10.0, 0.0, 90.0, 0.0, -10.0};
    GDALDataset* world =
        CreateGeorefMem(36, 18, 1, GDT_Byte, "EPSG:4326", gt_world);

    CPLStringList opts;
    opts.SetNameValue("TARGET_SRS", "EPSG:3857");
    EXPECT_TRUE(COGGetWarpingCharacteristics(world, opts.List(), resamp, tsrs,
                                             nx, ny, minx, miny, maxx, maxy));

    // Invalid EXTENT (3 tokens).
    opts.SetNameValue("EXTENT", "0,0,100");
    EXPECT_FALSE(COGGetWarpingCharacteristics(world, opts.List(), resamp, tsrs,
                                              nx, ny, minx, miny, maxx, maxy));

    // Valid EXTENT without RES vs with RES.
    opts.SetNameValue("EXTENT", "0,0,1000000,1000000");
    EXPECT_TRUE(COGGetWarpingCharacteristics(world, opts.List(), resamp, tsrs,
                                             nx, ny, minx, miny, maxx, maxy));
    opts.SetNameValue("RES", "10000");
    EXPECT_TRUE(COGGetWarpingCharacteristics(world, opts.List(), resamp, tsrs,
                                             nx, ny, minx, miny, maxx, maxy));
    EXPECT_EQ(nx, 100);
    EXPECT_EQ(ny, 100);
    GDALClose(world);
  }

  // 3. TILING_SCHEME options: EXTENT/RES ignored warnings, ZOOM_LEVEL invalid
  // & valid, ZOOM_LEVEL_STRATEGY (LOWER, UPPER, AUTO coarser & finer),
  // ALIGNED_LEVELS (valid + non-integer ratio error + coprime ratio error),
  // outside bbox error, partially outside bbox clamping, and bInvertAxis=true!
  {
    const double gt_merc[6] = {0.0, 50000.0, 0.0, 1000000.0, 0.0, -50000.0};
    GDALDataset* mds =
        CreateGeorefMem(20, 20, 1, GDT_Byte, "EPSG:3857", gt_merc);

    // EXTENT + RES warnings + ZOOM_LEVEL_STRATEGY=LOWER / UPPER / AUTO.
    for (const char* strat : {"LOWER", "UPPER", "AUTO"}) {
      SCOPED_TRACE(strat);
      CPLStringList opts;
      opts.SetNameValue("TILING_SCHEME", "GoogleMapsCompatible");
      opts.SetNameValue("EXTENT", "0,0,10,10");
      opts.SetNameValue("RES", "100");
      opts.SetNameValue("ZOOM_LEVEL_STRATEGY", strat);
      EXPECT_TRUE(COGGetWarpingCharacteristics(mds, opts.List(), resamp, tsrs,
                                               nx, ny, minx, miny, maxx, maxy));
    }

    // Coarser resolution closer to prev zoom level in AUTO:
    // Zoom 1 res = 78271.517, Zoom 2 res = 39135.758; geometric mean ~55346.
    // 50000 is < 55346 (closer to Zoom 2); 70000 is > 55346 (closer to Zoom 1)!
    {
      const double gt_coarse[6] = {0.0, 70000.0, 0.0, 1400000.0, 0.0, -70000.0};
      GDALDataset* cds =
          CreateGeorefMem(20, 20, 1, GDT_Byte, "EPSG:3857", gt_coarse);
      CPLStringList opts;
      opts.SetNameValue("TILING_SCHEME", "GoogleMapsCompatible");
      EXPECT_TRUE(COGGetWarpingCharacteristics(cds, opts.List(), resamp, tsrs,
                                               nx, ny, minx, miny, maxx, maxy));
      GDALClose(cds);
    }

    // Ultra-fine resolution finer than max zoom level -> fails!
    {
      const double gt_tiny[6] = {0.0, 1e-6, 0.0, 1.0, 0.0, -1e-6};
      GDALDataset* tds =
          CreateGeorefMem(16, 16, 1, GDT_Byte, "EPSG:3857", gt_tiny);
      CPLStringList opts;
      opts.SetNameValue("TILING_SCHEME", "GoogleMapsCompatible");
      EXPECT_FALSE(COGGetWarpingCharacteristics(
          tds, opts.List(), resamp, tsrs, nx, ny, minx, miny, maxx, maxy));
      GDALClose(tds);
    }

    // Invalid ZOOM_LEVEL=-1 and 99; valid ZOOM_LEVEL=2 + ALIGNED_LEVELS=2.
    {
      CPLStringList o_bad;
      o_bad.SetNameValue("TILING_SCHEME", "GoogleMapsCompatible");
      o_bad.SetNameValue("ZOOM_LEVEL", "99");
      EXPECT_FALSE(COGGetWarpingCharacteristics(
          mds, o_bad.List(), resamp, tsrs, nx, ny, minx, miny, maxx, maxy));

      CPLStringList o_al;
      o_al.SetNameValue("TILING_SCHEME", "GoogleMapsCompatible");
      o_al.SetNameValue("ZOOM_LEVEL", "2");
      o_al.SetNameValue("ALIGNED_LEVELS", "2");
      EXPECT_TRUE(COGGetWarpingCharacteristics(mds, o_al.List(), resamp, tsrs,
                                               nx, ny, minx, miny, maxx, maxy));
    }

    // Partially outside TMS bbox clamping: [19000000, 0] to [21000000,
    // 2000000].
    {
      const double gt_edge[6] = {19000000.0, 100000.0, 0.0,
                                 2000000.0,  0.0,      -100000.0};
      GDALDataset* eds =
          CreateGeorefMem(20, 20, 1, GDT_Byte, "EPSG:3857", gt_edge);
      CPLStringList opts;
      opts.SetNameValue("TILING_SCHEME", "GoogleMapsCompatible");
      opts.SetNameValue("ZOOM_LEVEL", "1");
      EXPECT_TRUE(COGGetWarpingCharacteristics(eds, opts.List(), resamp, tsrs,
                                               nx, ny, minx, miny, maxx, maxy));
      GDALClose(eds);
    }

    // Completely outside TMS bbox + ALIGNED_LEVELS unsupported ratios +
    // bInvertAxis (EPSG:4326 TMS)!
    {
      const char* kBoxTMS = R"json({
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
      o_out.SetNameValue("TILING_SCHEME", kBoxTMS);
      EXPECT_FALSE(COGGetWarpingCharacteristics(
          mds, o_out.List(), resamp, tsrs, nx, ny, minx, miny, maxx, maxy));

      // Non-integer 60/ratio: scaleDenominator 100000 and 11000
      // (60*11/100=6.6). Requires ZOOM_LEVEL=2 and ALIGNED_LEVELS=2 so
      // nAlignedLevels=min(2,2)=2!
      const char* kBadRatio1 = R"json({
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
      EXPECT_FALSE(COGGetWarpingCharacteristics(
          mds, o_r1.List(), resamp, tsrs, nx, ny, minx, miny, maxx, maxy));

      // Coprime integer 60/ratio = 7: scaleDenominator 60000000 and 7000000.
      const char* kBadRatio2 = R"json({
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
      EXPECT_FALSE(COGGetWarpingCharacteristics(
          mds, o_r2.List(), resamp, tsrs, nx, ny, minx, miny, maxx, maxy));

      // EPSG:4326 TMS -> bInvertAxis == true!
      const char* kLatLonTMS = R"json({
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
      o_ll.SetNameValue("TILING_SCHEME", kLatLonTMS);
      o_ll.SetNameValue("ZOOM_LEVEL", "0");
      EXPECT_TRUE(COGGetWarpingCharacteristics(mds, o_ll.List(), resamp, tsrs,
                                               nx, ny, minx, miny, maxx, maxy));
    }

    GDALClose(mds);
  }
}

TEST_F(CogDriverTest, CreateCopyCodecsWarpingStatsMasksAndOverviews) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);
  CPLConfigOptionSetter tmpdir("CPL_TMPDIR", "/vsimem/cog_tmp", false);

  auto run_copy = [&](GDALDataset* src, const char* path, CPLStringList& opts,
                      bool expect_ok = true, GDALProgressFunc prog = nullptr) {
    absl::Cleanup c = [path]() {
      VSIUnlink(path);
      VSIUnlink((std::string(path) + ".ovr.tmp").c_str());
    };
    GDALDataset* out =
        cog_drv_->CreateCopy(path, src, FALSE, opts.List(), prog, nullptr);
    if (expect_ok) {
      EXPECT_NE(out, nullptr) << path;
    } else {
      EXPECT_EQ(out, nullptr) << path;
    }
    if (out == nullptr) return;

    // Cheap oracles: when the requested codec maps 1:1 onto the GTiff
    // COMPRESSION name, check it was honoured.
    const char* comp = opts.FetchNameValue("COMPRESS");
    if (comp != nullptr &&
        (EQUAL(comp, "DEFLATE") || EQUAL(comp, "ZSTD") || EQUAL(comp, "LZMA") ||
         STARTS_WITH(comp, "LERC"))) {
      EXPECT_STREQ(out->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"), comp)
          << path;
    }
    GDALClose(out);

    // LAYOUT=COG is only derived from the file header when a GTiff is opened
    // from disk (not on the handle returned by CreateCopy), so reopen the
    // result read-only to verify it. @SUPPRESS_ASAP outputs are deleted on
    // close and cannot be reopened.
    if (!CPLTestBool(opts.FetchNameValueDef("@SUPPRESS_ASAP", "NO"))) {
      std::unique_ptr<GDALDataset> reopened(
          GDALDataset::Open(path, GDAL_OF_RASTER | GDAL_OF_READONLY));
      EXPECT_NE(reopened, nullptr) << path;
      if (reopened) {
        EXPECT_STREQ(reopened->GetMetadataItem("LAYOUT", "IMAGE_STRUCTURE"),
                     "COG")
            << path;
      }
    }
  };

  // 1. 0-band error + WEBP INTERLEAVE=BAND error.
  {
    GDALDataset* ds0 = mem_drv_->Create("", 16, 16, 0, GDT_Byte, nullptr);
    CPLStringList empty;
    run_copy(ds0, "/vsimem/cog_0band.tif", empty, false);
    GDALClose(ds0);

    GDALDataset* ds3 = mem_drv_->Create("", 16, 16, 3, GDT_Byte, nullptr);
    CPLStringList bad_webp;
    bad_webp.SetNameValue("COMPRESS", "WEBP");
    bad_webp.SetNameValue("INTERLEAVE", "BAND");
    run_copy(ds3, "/vsimem/cog_badwebp.tif", bad_webp, false);
    GDALClose(ds3);
  }

  // 2. Warping via CreateReprojectedDS:
  // (a) Full warp with mask + overviews + NUM_THREADS=2 + BIGTIFF=YES +
  // STATISTICS=YES
  // (b) Inconsistent -tr (`EXTENT=0,0,100,100`, `RES=33`, `ADD_ALPHA=NO`)
  // (c) Skip reprojection step when source ALREADY matches GoogleMapsCompatible
  // zoom 1!
  // (d) Warp error (`WARP_RESAMPLING=INVALID`) & warp progress cancel!
  {
    const double gt[6] = {0.0, 20000.0, 0.0, 600000.0, 0.0, -20000.0};
    GDALDataset* src = CreateGeorefMem(32, 32, 1, GDT_Byte, "EPSG:3857", gt);
    (void)src->CreateMaskBand(GMF_PER_DATASET);
    (void)src->GetRasterBand(1)->GetMaskBand()->Fill(255);

    CPLStringList o1;
    o1.SetNameValue("TILING_SCHEME", "GoogleMapsCompatible");
    o1.SetNameValue("ZOOM_LEVEL", "2");
    o1.SetNameValue("ALIGNED_LEVELS", "2");
    o1.SetNameValue("BIGTIFF", "YES");
    o1.SetNameValue("NUM_THREADS", "2");
    o1.SetNameValue("STATISTICS", "YES");
    run_copy(src, "/vsimem/cog_warp_full.tif", o1, true, GDALDummyProgress);

    // Overview count = 5 on zoom 0 -> hits nCurLevel < 1 & 1x1 clamp!
    CPLStringList o_z0;
    o_z0.SetNameValue("TILING_SCHEME", "GoogleMapsCompatible");
    o_z0.SetNameValue("ZOOM_LEVEL", "0");
    o_z0.SetNameValue("OVERVIEW_COUNT", "5");
    run_copy(src, "/vsimem/cog_warp_z0.tif", o_z0, true);

    // Inconsistent -tr + ADD_ALPHA=NO + OVERVIEWS=NONE.
    CPLStringList o2;
    o2.SetNameValue("TARGET_SRS", "EPSG:3857");
    o2.SetNameValue("EXTENT", "0,0,100000,100000");
    o2.SetNameValue("RES", "33333");
    o2.SetNameValue("ADD_ALPHA", "NO");
    o2.SetNameValue("OVERVIEWS", "NONE");
    run_copy(src, "/vsimem/cog_warp_inconsist.tif", o2, true);

    // Invalid WARP_RESAMPLING -> CreateReprojectedDS returns nullptr.
    CPLStringList o_badw;
    o_badw.SetNameValue("TARGET_SRS", "EPSG:3857");
    o_badw.SetNameValue("WARP_RESAMPLING", "BOGUS_RESAMP");
    run_copy(src, "/vsimem/cog_warp_bad.tif", o_badw, false);

    // Progress cancel during warp.
    auto cancel_first = [](double, const char*, void*) -> int { return 0; };
    CPLStringList o_canc;
    o_canc.SetNameValue("TILING_SCHEME", "GoogleMapsCompatible");
    o_canc.SetNameValue("ZOOM_LEVEL", "1");
    run_copy(src, "/vsimem/cog_warp_cancel.tif", o_canc, false, cancel_first);

    GDALClose(src);

    // Source already matching GoogleMapsCompatible zoom 1 (512x512) -> skips
    // CreateReprojectedDS!
    const double half = 20037508.342789244;
    const double res1 = (2.0 * half) / 512.0;
    const double gt_exact[6] = {-half, res1, 0.0, half, 0.0, -res1};
    GDALDataset* exact_ds =
        CreateGeorefMem(512, 512, 1, GDT_Byte, "EPSG:3857", gt_exact);
    CPLStringList o_skip;
    o_skip.SetNameValue("TILING_SCHEME", "GoogleMapsCompatible");
    o_skip.SetNameValue("ZOOM_LEVEL", "1");
    o_skip.SetNameValue("OVERVIEWS", "NONE");
    run_copy(exact_ds, "/vsimem/cog_warp_skip.tif", o_skip, true);
    GDALClose(exact_ds);
  }

  // 3. JPEG 2-band Gray+Alpha & 4-band RGBA -> m_poRGBMaskDS conversion +
  // STATISTICS=NO / AUTO with existing source stats!
  {
    for (int nb : {2, 4}) {
      SCOPED_TRACE(nb);
      GDALDataset* ds = mem_drv_->Create("", 160, 160, nb, GDT_Byte, nullptr);
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
      run_copy(ds, "/vsimem/cog_jmask_auto.tif", o_auto, true);

      CPLStringList o_nostat;
      o_nostat.SetNameValue("COMPRESS", "JPEG");
      o_nostat.SetNameValue("STATISTICS", "NO");
      o_nostat.SetNameValue("BLOCKSIZE", "128");
      run_copy(ds, "/vsimem/cog_jmask_nostat.tif", o_nostat, true);

      GDALClose(ds);
    }

    // STATISTICS=YES and STATISTICS=NO without warp/RGBMask (creates VRT!).
    GDALDataset* sds = mem_drv_->Create("", 16, 16, 1, GDT_Byte, nullptr);
    (void)sds->GetRasterBand(1)->Fill(42);
    CPLStringList o_syes;
    o_syes.SetNameValue("STATISTICS", "YES");
    run_copy(sds, "/vsimem/cog_stat_yes.tif", o_syes, true);

    sds->GetRasterBand(1)->SetMetadataItem("STATISTICS_MINIMUM", "42");
    sds->GetRasterBand(1)->SetMetadataItem("STATISTICS_MAXIMUM", "42");
    sds->GetRasterBand(1)->SetMetadataItem("STATISTICS_MEAN", "42");
    sds->GetRasterBand(1)->SetMetadataItem("STATISTICS_STDDEV", "0");
    CPLStringList o_sno;
    o_sno.SetNameValue("STATISTICS", "NO");
    run_copy(sds, "/vsimem/cog_stat_no.tif", o_sno, true);
    GDALClose(sds);
  }

  // 4. Existing overviews on source GTiff + !bGenerateOvr && bGenerateMskOvr +
  // invalid OVERVIEW_RESAMPLING failures + COG_DELETE_TEMP_FILES=NO (without
  // CPL_TMPDIR so GetTmpFilename else branch is hit too!).
  {
    constexpr char kSrcOvr[] = "/vsimem/cog_src_with_ovr.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kSrcOvr); };
    GDALDataset* gds =
        gtiff_drv_->Create(kSrcOvr, 256, 256, 1, GDT_Byte, nullptr);
    ASSERT_NE(gds, nullptr);
    int ovrs[2] = {2, 4};
    ASSERT_EQ(
        gds->BuildOverviews("AVERAGE", 2, ovrs, 0, nullptr, nullptr, nullptr),
        CE_None);
    // Add per-dataset mask AFTER BuildOverviews so imagery has 2 overviews
    // while mask has 0 overviews!
    ASSERT_EQ(gds->CreateMaskBand(GMF_PER_DATASET), CE_None);

    CPLStringList o_use;
    o_use.SetNameValue("BLOCKSIZE", "64");
    o_use.SetNameValue("OVERVIEW_COUNT", "1");
    run_copy(gds, "/vsimem/cog_existing_ovr_plus_msk.tif", o_use, true);

    // Mask overview failure with OVERVIEW_RESAMPLING=INVALID.
    CPLStringList o_bad_mskovr;
    o_bad_mskovr.SetNameValue("BLOCKSIZE", "64");
    o_bad_mskovr.SetNameValue("OVERVIEW_RESAMPLING", "INVALID_METHOD");
    run_copy(gds, "/vsimem/cog_bad_mskovr.tif", o_bad_mskovr, false);
    GDALClose(gds);

    // Imagery overview failure (no mask) + COG_DELETE_TEMP_FILES=NO without
    // CPL_TMPDIR!
    CPLConfigOptionSetter unset_tmp("CPL_TMPDIR", nullptr, false);
    CPLConfigOptionSetter keep_tmp("COG_DELETE_TEMP_FILES", "NO", false);
    GDALDataset* ds1 = mem_drv_->Create("", 160, 160, 1, GDT_Byte, nullptr);
    CPLStringList o_bad_ovr;
    o_bad_ovr.SetNameValue("BLOCKSIZE", "64");
    o_bad_ovr.SetNameValue("OVERVIEW_RESAMPLING", "INVALID_METHOD");
    run_copy(ds1, "/vsimem/cog_bad_imgovr.tif", o_bad_ovr, false);
    GDALClose(ds1);
  }

  // 5. All codecs & pass-through options:
  // WEBP (lossless QUALITY=100 & lossy QUALITY=80, OVERVIEW_COMPRESS=WEBP
  // OVERVIEW_QUALITY=100 & 75), JPEG + OVERVIEW_COMPRESS=JPEG, DEFLATE, ZSTD,
  // LZMA, LERC/LERC_DEFLATE/LERC_ZSTD + Float32 PREDICTOR=YES/FLOATING_POINT,
  // JXL, INTERLEAVE=TILE, COPY_SRC_MDD, SRC_MDD, GEOTIFF_VERSION=1.1,
  // SPARSE_OK=YES, NBITS=7, 1x4 OVERVIEW_COUNT=4!
  {
    GDALDataset* rgb = mem_drv_->Create("", 160, 160, 3, GDT_Byte, nullptr);
    rgb->SetMetadataItem("K", "V", "DOM1");

    // WEBP lossless + OVERVIEW_COMPRESS=WEBP lossless.
    CPLStringList o_wl;
    o_wl.SetNameValue("COMPRESS", "WEBP");
    o_wl.SetNameValue("QUALITY", "100");
    o_wl.SetNameValue("BLOCKSIZE", "64");
    o_wl.SetNameValue("OVERVIEW_COMPRESS", "WEBP");
    o_wl.SetNameValue("OVERVIEW_QUALITY", "100");
    run_copy(rgb, "/vsimem/cog_webp_lossless.tif", o_wl, true);

    // WEBP lossy + OVERVIEW_COMPRESS=WEBP lossy.
    CPLStringList o_wlossy;
    o_wlossy.SetNameValue("COMPRESS", "WEBP");
    o_wlossy.SetNameValue("QUALITY", "80");
    o_wlossy.SetNameValue("BLOCKSIZE", "64");
    o_wlossy.SetNameValue("OVERVIEW_COMPRESS", "WEBP");
    o_wlossy.SetNameValue("OVERVIEW_QUALITY", "75");
    run_copy(rgb, "/vsimem/cog_webp_lossy.tif", o_wlossy, true);

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
    run_copy(rgb, "/vsimem/cog_zstd_jpgovr.tif", o_zstd, true);

    // LZMA + INTERLEAVE=TILE + PREDICTOR=STANDARD +
    // OVERVIEW_PREDICTOR=STANDARD.
    CPLStringList o_lzma;
    o_lzma.SetNameValue("COMPRESS", "LZMA");
    o_lzma.SetNameValue("LEVEL", "1");
    o_lzma.SetNameValue("INTERLEAVE", "TILE");
    o_lzma.SetNameValue("PREDICTOR", "STANDARD");
    o_lzma.SetNameValue("OVERVIEW_PREDICTOR", "STANDARD");
    o_lzma.SetNameValue("BLOCKSIZE", "64");
    run_copy(rgb, "/vsimem/cog_lzma_tile.tif", o_lzma, true);

    GDALClose(rgb);

    // Float32 LERC / LERC_DEFLATE / LERC_ZSTD + PREDICTOR=YES & FLOATING_POINT.
    GDALDataset* fds = mem_drv_->Create("", 160, 160, 1, GDT_Float32, nullptr);
    for (const char* lerc_c : {"LERC", "LERC_DEFLATE", "LERC_ZSTD"}) {
      SCOPED_TRACE(lerc_c);
      CPLStringList ol;
      ol.SetNameValue("COMPRESS", lerc_c);
      ol.SetNameValue("LEVEL", "3");
      ol.SetNameValue("PREDICTOR", "YES");
      ol.SetNameValue("OVERVIEW_PREDICTOR", "FLOATING_POINT");
      ol.SetNameValue("MAX_Z_ERROR", "0.25");
      ol.SetNameValue("MAX_Z_ERROR_OVERVIEW", "0.5");
      ol.SetNameValue("BLOCKSIZE", "64");
      run_copy(fds, "/vsimem/cog_lerc.tif", ol, true);
    }
    GDALClose(fds);

    // JXL 4-band RGBA + GEOTIFF_VERSION=1.1 + SPARSE_OK=YES.
    GDALDataset* rgba = mem_drv_->Create("", 32, 32, 4, GDT_Byte, nullptr);
    CPLStringList ojxl;
    ojxl.SetNameValue("COMPRESS", "JXL");
    ojxl.SetNameValue("JXL_LOSSLESS", "NO");
    ojxl.SetNameValue("JXL_EFFORT", "3");
    ojxl.SetNameValue("JXL_DISTANCE", "1.5");
    ojxl.SetNameValue("JXL_ALPHA_DISTANCE", "0.5");
    ojxl.SetNameValue("GEOTIFF_VERSION", "1.1");
    ojxl.SetNameValue("SPARSE_OK", "YES");
    run_copy(rgba, "/vsimem/cog_jxl.tif", ojxl, true);
    GDALClose(rgba);

    // 1x4 DEFLATE + NBITS=7 + OVERVIEW_COUNT=4 + @SUPPRESS_ASAP=YES -> hits
    // nTmpXSize == 0 -> 1, nTmpYSize == 0 -> 1, 1x1 clamp!
    GDALDataset* thin = mem_drv_->Create("", 1, 4, 1, GDT_Byte, nullptr);
    CPLStringList othin;
    othin.SetNameValue("COMPRESS", "DEFLATE");
    othin.SetNameValue("LEVEL", "6");
    othin.SetNameValue("NBITS", "7");
    othin.SetNameValue("OVERVIEW_COUNT", "4");
    othin.SetNameValue("@SUPPRESS_ASAP", "YES");
    run_copy(thin, "/vsimem/cog_thin.tif", othin, true);
    GDALClose(thin);
  }
}

}  // namespace
}  // namespace autotest2
