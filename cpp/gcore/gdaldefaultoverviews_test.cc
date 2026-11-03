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
// Tests GDALDefaultOverviews manager and overview/mask lifecycle
// (gcore/gdaldefaultoverviews.cpp).
//
// Build caveat in google3: //third_party/gdal/BUILD compiles with PAM_ENABLED
// undefined, so GDAL_PAM_ENABLED defaults to "NO" unless overridden via
// CPLConfigOptionSetter.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdaldefaultoverviews.cpp

#include <array>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_cpp_functions.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_pam.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

class CustomOvrDataset : public GDALPamDataset {
 public:
  using GDALDataset::oOvManager;
  using GDALPamDataset::SetPhysicalFilename;
  void SetAccess(GDALAccess eAccessIn) { eAccess = eAccessIn; }

  CustomOvrDataset(const char* filename, int xsize, int ysize, int bands,
                   int block_x = 16, int block_y = 16);
  ~CustomOvrDataset() override { FlushCache(true); }

  int GetOverviewCount(int band) { return oOvManager.GetOverviewCount(band); }
  GDALRasterBand* GetOverview(int band, int idx) {
    return oOvManager.GetOverview(band, idx);
  }
};

class CustomOvrBand : public GDALPamRasterBand {
 public:
  CustomOvrBand(GDALDataset* ds, int band_num, int block_x, int block_y) {
    poDS = ds;
    nBand = band_num;
    eDataType = GDT_Byte;
    nRasterXSize = ds->GetRasterXSize();
    nRasterYSize = ds->GetRasterYSize();
    nBlockXSize = block_x;
    nBlockYSize = block_y;
  }

  CPLErr IReadBlock(int /*x*/, int /*y*/, void* buf) override {
    std::memset(buf, 50, static_cast<size_t>(nBlockXSize) * nBlockYSize);
    return CE_None;
  }

  int GetOverviewCount() override {
    auto* ds = static_cast<CustomOvrDataset*>(poDS);
    if (ds->oOvManager.IsInitialized()) {
      return ds->oOvManager.GetOverviewCount(nBand);
    }
    return GDALPamRasterBand::GetOverviewCount();
  }

  GDALRasterBand* GetOverview(int i) override {
    auto* ds = static_cast<CustomOvrDataset*>(poDS);
    if (ds->oOvManager.IsInitialized()) {
      return ds->oOvManager.GetOverview(nBand, i);
    }
    return GDALPamRasterBand::GetOverview(i);
  }

  GDALRasterBand* GetMaskBand() override {
    auto* ds = static_cast<CustomOvrDataset*>(poDS);
    if (ds->oOvManager.HaveMaskFile()) {
      GDALRasterBand* mb = ds->oOvManager.GetMaskBand(nBand);
      if (mb != nullptr) return mb;
    }
    return GDALPamRasterBand::GetMaskBand();
  }

  int GetMaskFlags() override {
    auto* ds = static_cast<CustomOvrDataset*>(poDS);
    if (ds->oOvManager.HaveMaskFile()) {
      return ds->oOvManager.GetMaskFlags(nBand);
    }
    return GDALPamRasterBand::GetMaskFlags();
  }
};

CustomOvrDataset::CustomOvrDataset(const char* filename, int xsize, int ysize,
                                   int bands, int block_x, int block_y) {
  nRasterXSize = xsize;
  nRasterYSize = ysize;
  eAccess = GA_Update;
  SetDescription(filename);
  SetPamFlags(GPF_NOSAVE);
  for (int i = 1; i <= bands; ++i) {
    SetBand(i, new CustomOvrBand(this, i, block_x, block_y));
  }
}

void WriteDummyFile(const char* path) {
  VSILFILE* fp = VSIFOpenL(path, "wb");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup close_fp = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
  EXPECT_EQ(5u, VSIFWriteL("dummy", 1, 5, fp));
}

TEST(GdalDefaultOverviewsTest, LevelAdjustComputeFactorAndResamplingNames) {
  EXPECT_EQ(2, GDALOvLevelAdjust(2, 100));
  EXPECT_EQ(240, GDALOvLevelAdjust(256, 1200));

  EXPECT_EQ(2, GDALOvLevelAdjust2(2, 100, 100));
  EXPECT_EQ(2, GDALOvLevelAdjust2(2, 10, 100));
  EXPECT_EQ(20, GDALOvLevelAdjust2(20, 10, 100));

  EXPECT_EQ(2, GDALComputeOvFactor(50, 100, 50, 100));
  EXPECT_EQ(3, GDALComputeOvFactor(33, 100, 33, 100));
  EXPECT_EQ(2, GDALComputeOvFactor(1, 1, 50, 100));
  EXPECT_EQ(3, GDALComputeOvFactor(1, 1, 33, 100));

  EXPECT_STREQ(
      "AVERAGE_BIT2GRAYSCALE_MINISWHITE",
      GDALGetNormalizedOvrResampling("AVERAGE_BIT2GRAYSCALE_MINISWHITE")
          .c_str());
  EXPECT_STREQ("AVERAGE_BIT2GRAYSCALE",
               GDALGetNormalizedOvrResampling("AVERAGE_BIT2").c_str());
  EXPECT_STREQ("NEAREST", GDALGetNormalizedOvrResampling("NEAR").c_str());
  EXPECT_STREQ("AVERAGE_MAGPHASE",
               GDALGetNormalizedOvrResampling("AVERAGE_MAGPHASE").c_str());
  EXPECT_STREQ("AVERAGE", GDALGetNormalizedOvrResampling("AVER").c_str());
  EXPECT_STREQ("CUBIC", GDALGetNormalizedOvrResampling("cubic").c_str());
  EXPECT_STREQ("", GDALGetNormalizedOvrResampling("NONE").c_str());
  EXPECT_STREQ("", GDALGetNormalizedOvrResampling(nullptr).c_str());
}

TEST(GdalDefaultOverviewsTest, BuildRefreshCleanAndMaskOverviews) {
  GDALRegister_GTiff();
  GDALRegister_HFA();
  absl::Cleanup cleanup_dir = [] {
    VSIRmdirRecursive("/vsimem/gdaldefaultoverviews_test");
  };

  constexpr char kBaseFile[] =
      "/vsimem/gdaldefaultoverviews_test/def_ovr_base.raw";
  {
    VsiMemTempWrapper base_tmp(kBaseFile, "dummy");

    {
      CustomOvrDataset ds(kBaseFile, 32, 32, 3, 16, 16);
      EXPECT_EQ(CE_None, ds.GetRasterBand(1)->SetNoDataValue(0.0));

      // 1. Initialize via GDALOpenInfo (with sibling files loaded):
      GDALOpenInfo open_info(kBaseFile, GDAL_OF_RASTER | GDAL_OF_UPDATE);
      open_info.GetSiblingFiles();
      ds.oOvManager.Initialize(&ds, &open_info, kBaseFile, true);
      EXPECT_TRUE(ds.oOvManager.IsInitialized());

      // 2. Error when building TIFF overviews on subset of bands (1 of 3):
      constexpr std::array<int, 4> kOvrLevels = {2, 4, 8, 16};
      constexpr std::array<int, 1> kOneBand = {1};
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        EXPECT_EQ(CE_Failure, ds.oOvManager.BuildOverviews(
                                  nullptr, "NEAREST", 2, kOvrLevels.data(), 1,
                                  kOneBand.data(), nullptr, nullptr, nullptr));
        CPLErrorReset();
      }

      // 3. Create per-dataset mask band (16x16 tiled) and test mask overviews:
      EXPECT_EQ(CE_None, ds.oOvManager.CreateMaskBand(GMF_PER_DATASET, 0));
      // Attempt to create mask band for band 2 on a PER_DATASET .msk fails:
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        EXPECT_EQ(CE_Failure, ds.oOvManager.CreateMaskBand(0, 2));
        CPLErrorReset();
      }
      EXPECT_NE(nullptr, ds.oOvManager.GetMaskBand(1));
      EXPECT_EQ(GMF_PER_DATASET, ds.oOvManager.GetMaskFlags(1));

      // 4. Build overviews on all 3 bands (also builds mask overviews with
      //    JPEG/YCBCR options sanitized):
      constexpr std::array<int, 3> kAllBands = {1, 2, 3};
      const char* opts[] = {"COMPRESS=JPEG", "PHOTOMETRIC=YCBCR", nullptr};
      EXPECT_EQ(CE_None, ds.oOvManager.BuildOverviews(
                             nullptr, "NEAREST", 4, kOvrLevels.data(), 3,
                             kAllBands.data(), nullptr, nullptr, opts));
      EXPECT_GE(ds.oOvManager.GetOverviewCount(1), 2);
      EXPECT_NE(nullptr, ds.oOvManager.GetOverview(1, 0));
      EXPECT_NE(nullptr, ds.oOvManager.GetOverview(1, 1));
      EXPECT_EQ(nullptr, ds.oOvManager.GetOverview(1, 99));
      EXPECT_EQ(nullptr, ds.oOvManager.GetOverview(99, 0));

      // Check overview's mask band via poBaseDS:
      GDALRasterBand* ovr0 = ds.oOvManager.GetOverview(1, 0);
      ASSERT_NE(nullptr, ovr0);
      EXPECT_NE(nullptr, ovr0->GetMaskBand());

      // Refresh existing overviews with different resampling ("AVERAGE") to
      // cover RESAMPLING metadata update and regeneration loop:
      EXPECT_EQ(CE_None, ds.oOvManager.BuildOverviews(
                             nullptr, "AVERAGE", 2, kOvrLevels.data(), 3,
                             kAllBands.data(), nullptr, nullptr, nullptr));

      // Re-initialize when poODS is already open:
      ds.oOvManager.Initialize(&ds, kBaseFile);
      EXPECT_TRUE(ds.oOvManager.IsInitialized());
      EXPECT_GE(ds.oOvManager.GetOverviewCount(1), 2);
    }

    // 5. Open a new dataset on kBaseFile (after ds closed its handle) to cover
    //    HaveMaskFile opening existing .msk and CreateOrOpenOverviewFile
    //    reopening read-only poODS as read-write:
    {
      constexpr std::array<int, 2> kOvrLevels = {2, 4};
      constexpr std::array<int, 3> kAllBands = {1, 2, 3};
      CustomOvrDataset ds_reopen(kBaseFile, 32, 32, 3, 16, 16);
      ds_reopen.SetAccess(GA_ReadOnly);
      ds_reopen.oOvManager.Initialize(&ds_reopen, kBaseFile);
      EXPECT_TRUE(ds_reopen.oOvManager.IsInitialized());
      EXPECT_GE(ds_reopen.oOvManager.GetOverviewCount(1), 2);
      // Before calling HaveMaskFile, switch dataset to GA_Update so .msk will
      // be opened read-write, while poODS (opened read-only above) triggers
      // reopen read-write in CreateOrOpenOverviewFile:
      ds_reopen.SetAccess(GA_Update);
      EXPECT_TRUE(ds_reopen.oOvManager.HaveMaskFile());
      EXPECT_NE(nullptr, ds_reopen.oOvManager.GetMaskBand(1));
      EXPECT_EQ(CE_None, ds_reopen.oOvManager.BuildOverviews(
                             nullptr, "NEAREST", 2, kOvrLevels.data(), 3,
                             kAllBands.data(), nullptr, nullptr, nullptr));
      // CleanOverviews (via nOverviews == 0):
      EXPECT_EQ(CE_None, ds_reopen.oOvManager.BuildOverviews(
                             nullptr, "NEAREST", 0, nullptr, 0, nullptr,
                             nullptr, nullptr, nullptr));
      EXPECT_EQ(0, ds_reopen.oOvManager.GetOverviewCount(1));
    }
  }

  // 6. Test HFA (.aux / USE_RRD=YES) overviews, BuildOverviewsSubDataset, and
  //    ProxyOverviewRequest fallback:
  {
    PamCleanProxyDB();
    absl::Cleanup cleanup_proxy_db = [] { PamCleanProxyDB(); };
    CPLConfigOptionSetter pam_enabled("GDAL_PAM_ENABLED", "YES", false);
    constexpr char kProxyDir[] = "/vsimem/gdaldefaultoverviews_test/ovr_proxy";
    CPLConfigOptionSetter pam_proxy("GDAL_PAM_PROXY_DIR", kProxyDir, false);
    EXPECT_EQ(0, VSIMkdir(kProxyDir, 0755));

    constexpr char kRrdFile[] =
        "/vsimem/gdaldefaultoverviews_test/def_ovr_rrd.raw";
    VsiMemTempWrapper rrd_tmp(kRrdFile, "dummy");
    CustomOvrDataset ds(kRrdFile, 16, 16, 1, 10, 10);
    ds.oOvManager.Initialize(&ds, kRrdFile);
    constexpr std::array<int, 1> kLevels = {2};
    constexpr std::array<int, 1> kBands = {1};
    const char* rrd_opts[] = {"USE_RRD=YES", nullptr};
    EXPECT_EQ(CE_None, ds.oOvManager.BuildOverviews(
                           nullptr, "NEAREST", 1, kLevels.data(), 1,
                           kBands.data(), nullptr, nullptr, rrd_opts));
    // Second call refreshes existing .aux overview (nNewOverviews == 0):
    EXPECT_EQ(CE_None, ds.oOvManager.BuildOverviews(
                           nullptr, "NEAREST", 1, kLevels.data(), 1,
                           kBands.data(), nullptr, nullptr, rrd_opts));

    // Open a new dataset on a physical temp file to cover Initialize finding
    // existing .aux:
    const std::string real_rrd =
        (std::filesystem::path(::testing::TempDir()) / "def_ovr_real_rrd.raw")
            .string();
    absl::Cleanup cleanup_real_rrd = [&real_rrd] {
      VSIUnlink(real_rrd.c_str());
      VSIUnlink((real_rrd + ".aux").c_str());
    };
    WriteDummyFile(real_rrd.c_str());
    {
      CustomOvrDataset rds(real_rrd.c_str(), 16, 16, 1, 10, 10);
      rds.oOvManager.Initialize(&rds, real_rrd.c_str());
      EXPECT_EQ(CE_None, rds.oOvManager.BuildOverviews(
                             nullptr, "NEAREST", 1, kLevels.data(), 1,
                             kBands.data(), nullptr, nullptr, rrd_opts));
      // AddOverviews on .aux fails:
      CustomOvrDataset dummy8("/vsimem/gdaldefaultoverviews_test/dummy8.raw", 8,
                              8, 1);
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, rds.oOvManager.AddOverviews(
                                nullptr, {&dummy8}, nullptr, nullptr, nullptr));
      CPLErrorReset();
    }
    {
      CustomOvrDataset rds_aux(real_rrd.c_str(), 16, 16, 1, 10, 10);
      rds_aux.oOvManager.Initialize(&rds_aux, real_rrd.c_str());
      EXPECT_TRUE(rds_aux.oOvManager.IsInitialized());
      EXPECT_GE(rds_aux.oOvManager.GetOverviewCount(1), 1);
      CPLConfigOptionSetter use_rrd_cfg("USE_RRD", "YES", false);
      EXPECT_EQ(CE_None, rds_aux.oOvManager.CleanOverviews());
    }

    EXPECT_EQ(CE_None, ds.oOvManager.CleanOverviews());

    // BuildOverviewsSubDataset (creates :::BASE:::def_ovr_rrd.raw_0.ovr):
    {
      CustomOvrDataset sub_ds(kRrdFile, 16, 16, 1, 10, 10);
      sub_ds.oOvManager.Initialize(&sub_ds, kRrdFile);
      EXPECT_EQ(CE_None, sub_ds.oOvManager.BuildOverviewsSubDataset(
                             kRrdFile, "NEAREST", 1, kLevels.data(), 1,
                             kBands.data(), nullptr, nullptr, nullptr));
    }

    // Open a new dataset with OVERVIEW_FILE = :::BASE:::def_ovr_rrd.raw_0.ovr:
    {
      CustomOvrDataset sub_reopen(kRrdFile, 16, 16, 1, 10, 10);
      sub_reopen.SetPhysicalFilename(kRrdFile);
      EXPECT_EQ(CE_None, sub_reopen.SetMetadataItem(
                             "OVERVIEW_FILE", ":::BASE:::def_ovr_rrd.raw_0.ovr",
                             "OVERVIEWS"));
      sub_reopen.oOvManager.Initialize(
          &sub_reopen, "/vsimem/gdaldefaultoverviews_test/no_direct_ovr.raw");
      EXPECT_TRUE(sub_reopen.oOvManager.IsInitialized());
      EXPECT_GE(sub_reopen.oOvManager.GetOverviewCount(1), 1);
    }

    // ProxyOverviewRequest fallback when primary overview path is unwritable
    // (both BuildOverviews and AddOverviews):
    {
      constexpr char kUnwritablePath[] =
          "/vsizip//vsimem/gdaldefaultoverviews_test/nonexistent.zip/ro.tif";
      CustomOvrDataset ro_ds(kUnwritablePath, 16, 16, 1, 16, 16);
      ro_ds.SetPamFlags(0);
      ro_ds.oOvManager.Initialize(&ro_ds, kUnwritablePath);
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        EXPECT_EQ(CE_None, ro_ds.oOvManager.BuildOverviews(
                               nullptr, "NEAREST", 1, kLevels.data(), 1,
                               kBands.data(), nullptr, nullptr, nullptr));
        CPLErrorReset();
      }
      PamCleanProxyDB();

      constexpr char kUnwritablePath2[] =
          "/vsizip//vsimem/gdaldefaultoverviews_test/nonexistent.zip/ro2.tif";
      CustomOvrDataset ro_ds2(kUnwritablePath2, 16, 16, 1, 16, 16);
      ro_ds2.SetPamFlags(0);
      ro_ds2.oOvManager.Initialize(&ro_ds2, kUnwritablePath2);
      CustomOvrDataset src8("/vsimem/gdaldefaultoverviews_test/src8.raw", 8, 8,
                            1);
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        EXPECT_EQ(CE_None, ro_ds2.oOvManager.AddOverviews(
                               nullptr, {&src8}, nullptr, nullptr, nullptr));
        CPLErrorReset();
      }
    }

    // Additional edge cases: per-band mask (non-PER_DATASET) and uninitialized
    // HaveMaskFile:
    {
      CustomOvrDataset uninit_ds("/vsimem/gdaldefaultoverviews_test/uninit.raw",
                                 8, 8, 2);
      EXPECT_FALSE(uninit_ds.oOvManager.HaveMaskFile());
      EXPECT_EQ(0, uninit_ds.oOvManager.GetMaskFlags(1));
      EXPECT_EQ(CE_None, uninit_ds.oOvManager.CleanOverviews());

      constexpr char kPbMaskFile[] =
          "/vsimem/gdaldefaultoverviews_test/pb_mask.raw";
      VsiMemTempWrapper pb_tmp(kPbMaskFile, "dummy");
      CustomOvrDataset pb_ds(kPbMaskFile, 16, 16, 2, 10, 10);
      const char* sibs[] = {"pb_mask.raw.ovr", nullptr};
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        pb_ds.oOvManager.Initialize(&pb_ds, nullptr, sibs, true);
        CPLErrorReset();
      }
      EXPECT_TRUE(pb_ds.oOvManager.IsInitialized());
      EXPECT_EQ(CE_None, pb_ds.oOvManager.CreateMaskBand(0, 1));
      EXPECT_NE(nullptr, pb_ds.oOvManager.GetMaskBand(1));
      // Band 2 has no INTERNAL_MASK_FLAGS_2 yet -> MISSING_FLAGS:
      EXPECT_EQ(nullptr, pb_ds.oOvManager.GetMaskBand(2));
      EXPECT_EQ(nullptr, pb_ds.oOvManager.GetMaskBand(0));
    }
  }
}

TEST(GdalDefaultOverviewsTest, AddOverviewsAndConsistencyChecks) {
  GDALRegister_GTiff();
  absl::Cleanup cleanup_dir = [] {
    VSIRmdirRecursive("/vsimem/gdaldefaultoverviews_test");
  };

  constexpr char kBaseFile[] =
      "/vsimem/gdaldefaultoverviews_test/add_ovr_base.raw";
  VsiMemTempWrapper base_tmp(kBaseFile, "dummy");
  CustomOvrDataset base_ds(kBaseFile, 32, 32, 1, 16, 16);
  OGRSpatialReference wgs84;
  EXPECT_EQ(OGRERR_NONE, wgs84.SetWellKnownGeogCS("WGS84"));
  EXPECT_EQ(CE_None, base_ds.SetSpatialRef(&wgs84));
  GDALGeoTransform gt(0.0, 1.0, 0.0, 32.0, 0.0, -1.0);
  EXPECT_EQ(CE_None, base_ds.SetGeoTransform(gt));
  base_ds.oOvManager.Initialize(&base_ds, kBaseFile);

  // 1. Consistency check failures:
  // Larger than base:
  {
    CustomOvrDataset bigger("/vsimem/gdaldefaultoverviews_test/bigger.raw", 64,
                            64, 1);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, base_ds.oOvManager.AddOverviews(
                              nullptr, {&bigger}, nullptr, nullptr, nullptr));
    CPLErrorReset();
  }
  // Different band count:
  {
    CustomOvrDataset two_bands(
        "/vsimem/gdaldefaultoverviews_test/two_bands.raw", 16, 16, 2);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              base_ds.oOvManager.AddOverviews(nullptr, {&two_bands}, nullptr,
                                              nullptr, nullptr));
    CPLErrorReset();
  }
  // Different CRS:
  {
    CustomOvrDataset diff_crs("/vsimem/gdaldefaultoverviews_test/diff_crs.raw",
                              16, 16, 1);
    OGRSpatialReference nad83;
    EXPECT_EQ(OGRERR_NONE, nad83.SetWellKnownGeogCS("NAD83"));
    EXPECT_EQ(CE_None, diff_crs.SetSpatialRef(&nad83));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, base_ds.oOvManager.AddOverviews(
                              nullptr, {&diff_crs}, nullptr, nullptr, nullptr));
    CPLErrorReset();
  }
  // Different GeoTransform extent:
  {
    CustomOvrDataset diff_gt("/vsimem/gdaldefaultoverviews_test/diff_gt.raw",
                             16, 16, 1);
    EXPECT_EQ(CE_None, diff_gt.SetSpatialRef(&wgs84));
    GDALGeoTransform bad_gt(500.0, 2.0, 0.0, 32.0, 0.0, -2.0);
    EXPECT_EQ(CE_None, diff_gt.SetGeoTransform(bad_gt));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, base_ds.oOvManager.AddOverviews(
                              nullptr, {&diff_gt}, nullptr, nullptr, nullptr));
    CPLErrorReset();
  }

  // 2. Valid AddOverviews (with 8x8 and 16x16 overviews, sorted descending):
  {
    CustomOvrDataset ovr8("/vsimem/gdaldefaultoverviews_test/ovr8.raw", 8, 8,
                          1);
    EXPECT_EQ(CE_None, ovr8.SetSpatialRef(&wgs84));
    GDALGeoTransform gt8(0.0, 4.0, 0.0, 32.0, 0.0, -4.0);
    EXPECT_EQ(CE_None, ovr8.SetGeoTransform(gt8));

    CustomOvrDataset ovr16("/vsimem/gdaldefaultoverviews_test/ovr16.raw", 16,
                           16, 1);
    EXPECT_EQ(CE_None, ovr16.SetSpatialRef(&wgs84));
    GDALGeoTransform gt16(0.0, 2.0, 0.0, 32.0, 0.0, -2.0);
    EXPECT_EQ(CE_None, ovr16.SetGeoTransform(gt16));

    EXPECT_EQ(CE_None,
              base_ds.oOvManager.AddOverviews(nullptr, {&ovr8, &ovr16}, nullptr,
                                              nullptr, nullptr));
    EXPECT_EQ(2, base_ds.oOvManager.GetOverviewCount(1));

    // Calling AddOverviews again with already-existing overview size (16x16):
    EXPECT_EQ(CE_None, base_ds.oOvManager.AddOverviews(
                           nullptr, {&ovr16}, nullptr, nullptr, nullptr));
  }
}

}  // namespace
}  // namespace autotest2
