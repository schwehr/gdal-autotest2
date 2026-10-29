// Copyright 2026 Google Inc. All Rights Reserved.
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

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal_pam.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

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
    memset(buf, 50, static_cast<size_t>(nBlockXSize) * nBlockYSize);
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
  ASSERT_NE(fp, nullptr);
  VSIFWriteL("dummy", 1, 5, fp);
  VSIFCloseL(fp);
}

TEST(GdalDefaultOverviewsTest, LevelAdjustComputeFactorAndResamplingNames) {
  EXPECT_EQ(GDALOvLevelAdjust(2, 100), 2);
  EXPECT_EQ(GDALOvLevelAdjust(256, 1200), 240);

  EXPECT_EQ(GDALOvLevelAdjust2(2, 100, 100), 2);
  EXPECT_EQ(GDALOvLevelAdjust2(2, 10, 100), 2);
  EXPECT_EQ(GDALOvLevelAdjust2(20, 10, 100), 20);

  EXPECT_EQ(GDALComputeOvFactor(50, 100, 50, 100), 2);
  EXPECT_EQ(GDALComputeOvFactor(33, 100, 33, 100), 3);
  EXPECT_EQ(GDALComputeOvFactor(1, 1, 50, 100), 2);
  EXPECT_EQ(GDALComputeOvFactor(1, 1, 33, 100), 3);

  EXPECT_STREQ(
      GDALGetNormalizedOvrResampling("AVERAGE_BIT2GRAYSCALE_MINISWHITE")
          .c_str(),
      "AVERAGE_BIT2GRAYSCALE_MINISWHITE");
  EXPECT_STREQ(GDALGetNormalizedOvrResampling("AVERAGE_BIT2").c_str(),
               "AVERAGE_BIT2GRAYSCALE");
  EXPECT_STREQ(GDALGetNormalizedOvrResampling("NEAR").c_str(), "NEAREST");
  EXPECT_STREQ(GDALGetNormalizedOvrResampling("AVERAGE_MAGPHASE").c_str(),
               "AVERAGE_MAGPHASE");
  EXPECT_STREQ(GDALGetNormalizedOvrResampling("AVER").c_str(), "AVERAGE");
  EXPECT_STREQ(GDALGetNormalizedOvrResampling("cubic").c_str(), "CUBIC");
  EXPECT_STREQ(GDALGetNormalizedOvrResampling("NONE").c_str(), "");
  EXPECT_STREQ(GDALGetNormalizedOvrResampling(nullptr).c_str(), "");
}

TEST(GdalDefaultOverviewsTest, BuildRefreshCleanAndMaskOverviews) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  const char* base_file = "/vsimem/def_ovr_base.raw";
  WriteDummyFile(base_file);

  {
    CustomOvrDataset ds(base_file, 32, 32, 3, 16, 16);
    ds.GetRasterBand(1)->SetNoDataValue(0.0);

    // Initialize via GDALOpenInfo (with sibling files loaded):
    GDALOpenInfo open_info(base_file, GDAL_OF_RASTER | GDAL_OF_UPDATE);
    open_info.GetSiblingFiles();
    ds.oOvManager.Initialize(&ds, &open_info, base_file, true);
    EXPECT_TRUE(ds.oOvManager.IsInitialized());

    // Error when building TIFF overviews on subset of bands (1 of 3):
    int ovr_levels[] = {2, 4, 8, 16};
    int one_band[] = {1};
    EXPECT_EQ(ds.oOvManager.BuildOverviews(nullptr, "NEAREST", 2, ovr_levels, 1,
                                           one_band, nullptr, nullptr, nullptr),
              CE_Failure);

    // Create per-dataset mask band (16x16 tiled) and test mask overviews too:
    EXPECT_EQ(ds.oOvManager.CreateMaskBand(GMF_PER_DATASET, 0), CE_None);
    // Attempt to create mask band for band 2 on a PER_DATASET .msk fails:
    EXPECT_EQ(ds.oOvManager.CreateMaskBand(0, 2), CE_Failure);
    EXPECT_NE(ds.oOvManager.GetMaskBand(1), nullptr);
    EXPECT_EQ(ds.oOvManager.GetMaskFlags(1), GMF_PER_DATASET);

    // Build overviews on all 3 bands (also builds mask overviews with
    // JPEG/YCBCR options sanitized):
    int all_bands[] = {1, 2, 3};
    const char* opts[] = {"COMPRESS=JPEG", "PHOTOMETRIC=YCBCR", nullptr};
    EXPECT_EQ(ds.oOvManager.BuildOverviews(nullptr, "NEAREST", 4, ovr_levels, 3,
                                           all_bands, nullptr, nullptr, opts),
              CE_None);
    EXPECT_GE(ds.oOvManager.GetOverviewCount(1), 2);
    EXPECT_NE(ds.oOvManager.GetOverview(1, 0), nullptr);
    EXPECT_NE(ds.oOvManager.GetOverview(1, 1), nullptr);
    EXPECT_EQ(ds.oOvManager.GetOverview(1, 99), nullptr);
    EXPECT_EQ(ds.oOvManager.GetOverview(99, 0), nullptr);

    // Check overview's mask band via poBaseDS:
    GDALRasterBand* ovr0 = ds.oOvManager.GetOverview(1, 0);
    ASSERT_NE(ovr0, nullptr);
    EXPECT_NE(ovr0->GetMaskBand(), nullptr);

    // Refresh existing overviews with different resampling ("AVERAGE") to cover
    // RESAMPLING metadata update and regeneration loop:
    EXPECT_EQ(
        ds.oOvManager.BuildOverviews(nullptr, "AVERAGE", 2, ovr_levels, 3,
                                     all_bands, nullptr, nullptr, nullptr),
        CE_None);

    // Re-initialize when poODS is already open:
    ds.oOvManager.Initialize(&ds, base_file);
    EXPECT_TRUE(ds.oOvManager.IsInitialized());
    EXPECT_GE(ds.oOvManager.GetOverviewCount(1), 2);
  }

  // Also test opening a new dataset on base_file (after ds closed its handle)
  // to cover HaveMaskFile opening existing .msk and CreateOrOpenOverviewFile
  // reopening read-only poODS as read-write:
  {
    int ovr_levels[] = {2, 4};
    int all_bands[] = {1, 2, 3};
    CustomOvrDataset ds_reopen(base_file, 32, 32, 3, 16, 16);
    ds_reopen.SetAccess(GA_ReadOnly);
    ds_reopen.oOvManager.Initialize(&ds_reopen, base_file);
    EXPECT_TRUE(ds_reopen.oOvManager.IsInitialized());
    EXPECT_GE(ds_reopen.oOvManager.GetOverviewCount(1), 2);
    // Before calling HaveMaskFile, switch dataset to GA_Update so .msk will be
    // opened read-write, while poODS (opened read-only above) triggers reopen
    // read-write in CreateOrOpenOverviewFile:
    ds_reopen.SetAccess(GA_Update);
    EXPECT_TRUE(ds_reopen.oOvManager.HaveMaskFile());
    EXPECT_NE(ds_reopen.oOvManager.GetMaskBand(1), nullptr);
    EXPECT_EQ(ds_reopen.oOvManager.BuildOverviews(nullptr, "NEAREST", 2,
                                                  ovr_levels, 3, all_bands,
                                                  nullptr, nullptr, nullptr),
              CE_None);
    // CleanOverviews (via nOverviews == 0):
    EXPECT_EQ(
        ds_reopen.oOvManager.BuildOverviews(nullptr, "NEAREST", 0, nullptr, 0,
                                            nullptr, nullptr, nullptr, nullptr),
        CE_None);
    EXPECT_EQ(ds_reopen.oOvManager.GetOverviewCount(1), 0);
  }

  VSIUnlink(base_file);
  VSIUnlink("/vsimem/def_ovr_base.raw.ovr");
  VSIUnlink("/vsimem/def_ovr_base.raw.msk");
  VSIUnlink("/vsimem/def_ovr_base.raw.msk.ovr");

  // Test HFA (.aux / USE_RRD=YES) overviews, BuildOverviewsSubDataset, and
  // ProxyOverviewRequest fallback:
  {
    CPLConfigOptionSetter pam_enabled("GDAL_PAM_ENABLED", "YES", false);
    CPLConfigOptionSetter pam_proxy("GDAL_PAM_PROXY_DIR", "/vsimem/ovr_proxy",
                                    false);
    VSIMkdir("/vsimem/ovr_proxy", 0755);
    PamCleanProxyDB();

    const char* rrd_file = "/vsimem/def_ovr_rrd.raw";
    WriteDummyFile(rrd_file);
    CustomOvrDataset ds(rrd_file, 16, 16, 1, 10, 10);
    ds.oOvManager.Initialize(&ds, rrd_file);
    int levels[] = {2};
    int bands[] = {1};
    const char* rrd_opts[] = {"USE_RRD=YES", nullptr};
    EXPECT_EQ(ds.oOvManager.BuildOverviews(nullptr, "NEAREST", 1, levels, 1,
                                           bands, nullptr, nullptr, rrd_opts),
              CE_None);
    // Second call refreshes existing .aux overview (nNewOverviews == 0):
    EXPECT_EQ(ds.oOvManager.BuildOverviews(nullptr, "NEAREST", 1, levels, 1,
                                           bands, nullptr, nullptr, rrd_opts),
              CE_None);

    // Open a new dataset on rrd_file to cover Initialize finding existing .aux:
    std::string real_rrd = testing::TempDir() + "/def_ovr_real_rrd.raw";
    WriteDummyFile(real_rrd.c_str());
    {
      CustomOvrDataset rds(real_rrd.c_str(), 16, 16, 1, 10, 10);
      rds.oOvManager.Initialize(&rds, real_rrd.c_str());
      EXPECT_EQ(
          rds.oOvManager.BuildOverviews(nullptr, "NEAREST", 1, levels, 1, bands,
                                        nullptr, nullptr, rrd_opts),
          CE_None);
      // AddOverviews on .aux fails (lines 780-785):
      CustomOvrDataset dummy8("/vsimem/dummy8.raw", 8, 8, 1);
      EXPECT_EQ(rds.oOvManager.AddOverviews(nullptr, {&dummy8}, nullptr,
                                            nullptr, nullptr),
                CE_Failure);
    }
    {
      CustomOvrDataset rds_aux(real_rrd.c_str(), 16, 16, 1, 10, 10);
      rds_aux.oOvManager.Initialize(&rds_aux, real_rrd.c_str());
      EXPECT_TRUE(rds_aux.oOvManager.IsInitialized());
      EXPECT_GE(rds_aux.oOvManager.GetOverviewCount(1), 1);
      CPLConfigOptionSetter use_rrd_cfg("USE_RRD", "YES", false);
      EXPECT_EQ(rds_aux.oOvManager.CleanOverviews(), CE_None);
    }
    VSIUnlink(real_rrd.c_str());

    EXPECT_EQ(ds.oOvManager.CleanOverviews(), CE_None);

    // BuildOverviewsSubDataset (creates :::BASE:::def_ovr_rrd.raw_0.ovr):
    {
      CustomOvrDataset sub_ds(rrd_file, 16, 16, 1, 10, 10);
      sub_ds.oOvManager.Initialize(&sub_ds, rrd_file);
      EXPECT_EQ(sub_ds.oOvManager.BuildOverviewsSubDataset(
                    rrd_file, "NEAREST", 1, levels, 1, bands, nullptr, nullptr,
                    nullptr),
                CE_None);
    }

    // Open a new dataset with OVERVIEW_FILE = :::BASE:::def_ovr_rrd.raw_0.ovr:
    {
      CustomOvrDataset sub_reopen(rrd_file, 16, 16, 1, 10, 10);
      sub_reopen.SetPhysicalFilename(rrd_file);
      sub_reopen.SetMetadataItem(
          "OVERVIEW_FILE", ":::BASE:::def_ovr_rrd.raw_0.ovr", "OVERVIEWS");
      sub_reopen.oOvManager.Initialize(&sub_reopen,
                                       "/vsimem/no_direct_ovr.raw");
      EXPECT_TRUE(sub_reopen.oOvManager.IsInitialized());
      EXPECT_GE(sub_reopen.oOvManager.GetOverviewCount(1), 1);
    }

    // ProxyOverviewRequest fallback when primary overview path is unwritable
    // (both BuildOverviews and AddOverviews):
    {
      const char* unwritable_path = "/vsizip//vsimem/nonexistent.zip/ro.tif";
      CustomOvrDataset ro_ds(unwritable_path, 16, 16, 1, 16, 16);
      ro_ds.SetPamFlags(0);
      ro_ds.oOvManager.Initialize(&ro_ds, unwritable_path);
      EXPECT_EQ(
          ro_ds.oOvManager.BuildOverviews(nullptr, "NEAREST", 1, levels, 1,
                                          bands, nullptr, nullptr, nullptr),
          CE_None);
      PamCleanProxyDB();

      const char* unwritable_path2 = "/vsizip//vsimem/nonexistent.zip/ro2.tif";
      CustomOvrDataset ro_ds2(unwritable_path2, 16, 16, 1, 16, 16);
      ro_ds2.SetPamFlags(0);
      ro_ds2.oOvManager.Initialize(&ro_ds2, unwritable_path2);
      CustomOvrDataset src8("/vsimem/src8.raw", 8, 8, 1);
      EXPECT_EQ(ro_ds2.oOvManager.AddOverviews(nullptr, {&src8}, nullptr,
                                               nullptr, nullptr),
                CE_None);
    }
    PamCleanProxyDB();

    // Additional edge cases: per-band mask (non-PER_DATASET) and uninitialized
    // HaveMaskFile:
    {
      CustomOvrDataset uninit_ds("/vsimem/uninit.raw", 8, 8, 2);
      EXPECT_FALSE(uninit_ds.oOvManager.HaveMaskFile());
      EXPECT_EQ(uninit_ds.oOvManager.GetMaskFlags(1), 0);
      EXPECT_EQ(uninit_ds.oOvManager.CleanOverviews(), CE_None);

      const char* pb_mask_file = "/vsimem/pb_mask.raw";
      WriteDummyFile(pb_mask_file);
      CustomOvrDataset pb_ds(pb_mask_file, 16, 16, 2, 10, 10);
      const char* sibs[] = {"pb_mask.raw.ovr", nullptr};
      pb_ds.oOvManager.Initialize(&pb_ds, nullptr, sibs, true);
      EXPECT_TRUE(pb_ds.oOvManager.IsInitialized());
      EXPECT_EQ(pb_ds.oOvManager.CreateMaskBand(0, 1), CE_None);
      EXPECT_NE(pb_ds.oOvManager.GetMaskBand(1), nullptr);
      // Band 2 has no INTERNAL_MASK_FLAGS_2 yet -> MISSING_FLAGS:
      EXPECT_EQ(pb_ds.oOvManager.GetMaskBand(2), nullptr);
      EXPECT_EQ(pb_ds.oOvManager.GetMaskBand(0), nullptr);
      VSIUnlink(pb_mask_file);
      VSIUnlink("/vsimem/pb_mask.raw.msk");
    }

    VSIUnlink(rrd_file);
    VSIUnlink("/vsimem/def_ovr_rrd.raw.aux");
    VSIUnlink("/vsimem/def_ovr_rrd.raw_0.ovr");
  }

  CPLPopErrorHandler();
}

TEST(GdalDefaultOverviewsTest, AddOverviewsAndConsistencyChecks) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  const char* base_file = "/vsimem/add_ovr_base.raw";
  WriteDummyFile(base_file);
  CustomOvrDataset base_ds(base_file, 32, 32, 1, 16, 16);
  OGRSpatialReference wgs84;
  wgs84.SetWellKnownGeogCS("WGS84");
  base_ds.SetSpatialRef(&wgs84);
  GDALGeoTransform gt(0.0, 1.0, 0.0, 32.0, 0.0, -1.0);
  base_ds.SetGeoTransform(gt);
  base_ds.oOvManager.Initialize(&base_ds, base_file);

  // 1. Consistency check failures:
  // Larger than base:
  {
    CustomOvrDataset bigger("/vsimem/bigger.raw", 64, 64, 1);
    EXPECT_EQ(base_ds.oOvManager.AddOverviews(nullptr, {&bigger}, nullptr,
                                              nullptr, nullptr),
              CE_Failure);
  }
  // Different band count:
  {
    CustomOvrDataset two_bands("/vsimem/two_bands.raw", 16, 16, 2);
    EXPECT_EQ(base_ds.oOvManager.AddOverviews(nullptr, {&two_bands}, nullptr,
                                              nullptr, nullptr),
              CE_Failure);
  }
  // Different CRS:
  {
    CustomOvrDataset diff_crs("/vsimem/diff_crs.raw", 16, 16, 1);
    OGRSpatialReference nad83;
    nad83.SetWellKnownGeogCS("NAD83");
    diff_crs.SetSpatialRef(&nad83);
    EXPECT_EQ(base_ds.oOvManager.AddOverviews(nullptr, {&diff_crs}, nullptr,
                                              nullptr, nullptr),
              CE_Failure);
  }
  // Different GeoTransform extent:
  {
    CustomOvrDataset diff_gt("/vsimem/diff_gt.raw", 16, 16, 1);
    diff_gt.SetSpatialRef(&wgs84);
    GDALGeoTransform bad_gt(500.0, 2.0, 0.0, 32.0, 0.0, -2.0);
    diff_gt.SetGeoTransform(bad_gt);
    EXPECT_EQ(base_ds.oOvManager.AddOverviews(nullptr, {&diff_gt}, nullptr,
                                              nullptr, nullptr),
              CE_Failure);
  }

  // 2. Valid AddOverviews (with 8x8 and 16x16 overviews, sorted descending):
  {
    CustomOvrDataset ovr8("/vsimem/ovr8.raw", 8, 8, 1);
    ovr8.SetSpatialRef(&wgs84);
    GDALGeoTransform gt8(0.0, 4.0, 0.0, 32.0, 0.0, -4.0);
    ovr8.SetGeoTransform(gt8);

    CustomOvrDataset ovr16("/vsimem/ovr16.raw", 16, 16, 1);
    ovr16.SetSpatialRef(&wgs84);
    GDALGeoTransform gt16(0.0, 2.0, 0.0, 32.0, 0.0, -2.0);
    ovr16.SetGeoTransform(gt16);

    EXPECT_EQ(base_ds.oOvManager.AddOverviews(nullptr, {&ovr8, &ovr16}, nullptr,
                                              nullptr, nullptr),
              CE_None);
    EXPECT_EQ(base_ds.oOvManager.GetOverviewCount(1), 2);

    // Calling AddOverviews again with already-existing overview size (16x16):
    EXPECT_EQ(base_ds.oOvManager.AddOverviews(nullptr, {&ovr16}, nullptr,
                                              nullptr, nullptr),
              CE_None);
  }

  VSIUnlink(base_file);
  VSIUnlink("/vsimem/add_ovr_base.raw.ovr");
  CPLPopErrorHandler();
}

}  // namespace
