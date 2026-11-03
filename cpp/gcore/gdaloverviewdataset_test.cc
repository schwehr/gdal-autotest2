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
// Tests GDALOverviewDataset and GDALOverviewBand
// (gdaloverviewdataset.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdaloverviewdataset.cpp

#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

// Custom dataset whose overview band has GetDataset() == nullptr.
class StandaloneOvrBand final : public GDALRasterBand {
 public:
  StandaloneOvrBand(int x, int y) {
    poDS = nullptr;
    nBand = 1;
    nRasterXSize = x;
    nRasterYSize = y;
    eDataType = GDT_Byte;
    nBlockXSize = x;
    nBlockYSize = y;
  }
  CPLErr IReadBlock(int, int, void* pImage) override {
    std::memset(pImage, 42, static_cast<size_t>(nBlockXSize) * nBlockYSize);
    return CE_None;
  }
};

class MainBandWithStandaloneOvr final : public GDALRasterBand {
 public:
  MainBandWithStandaloneOvr(GDALDataset* ds, int band, int ovr_x, int ovr_y)
      : m_ovr(std::make_unique<StandaloneOvrBand>(ovr_x, ovr_y)) {
    poDS = ds;
    nBand = band;
    nRasterXSize = 8;
    nRasterYSize = 8;
    eDataType = GDT_Byte;
    nBlockXSize = 8;
    nBlockYSize = 8;
  }
  CPLErr IReadBlock(int, int, void* pImage) override {
    std::memset(pImage, 1, 64);
    return CE_None;
  }
  int GetOverviewCount() override { return 1; }
  GDALRasterBand* GetOverview(int i) override {
    return i == 0 ? m_ovr.get() : nullptr;
  }

 private:
  std::unique_ptr<StandaloneOvrBand> m_ovr;
};

class MainDSWithStandaloneOvr final : public GDALDataset {
 public:
  MainDSWithStandaloneOvr(int ovr1_x, int ovr1_y, int ovr2_x, int ovr2_y,
                          GDALDriver* drv = nullptr) {
    poDriver = drv;
    nRasterXSize = 8;
    nRasterYSize = 8;
    SetBand(1, new MainBandWithStandaloneOvr(this, 1, ovr1_x, ovr1_y));
    SetBand(2, new MainBandWithStandaloneOvr(this, 2, ovr2_x, ovr2_y));
  }
};

class GdalOverviewDatasetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
  }
};

TEST_F(GdalOverviewDatasetTest, CreationAndAllMethods) {
  GDALDriver* tiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, tiff_drv);

  // 0-band dataset returns nullptr.
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr zero_band_ds(
      mem_drv->Create("zero", 8, 8, 0, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, zero_band_ds);
  EXPECT_EQ(nullptr, GDALCreateOverviewDataset(zero_band_ds.get(), 0, false));

  // Create GTiff with 2 bands, overviews (level 0: 8x8, level 1: 4x4), and
  // per-dataset mask.
  constexpr char kSuiteDir[] = "/vsimem/gdaloverviewdataset_test";
  constexpr char kTiffPath[] =
      "/vsimem/gdaloverviewdataset_test/ovr_ds_test.tif";
  absl::Cleanup cleanup_dir = [kSuiteDir] { VSIRmdirRecursive(kSuiteDir); };

  GDALDatasetUniquePtr main_ds(
      tiff_drv->Create(kTiffPath, 16, 16, 2, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, main_ds);
  EXPECT_EQ(CE_None, main_ds->CreateMaskBand(GMF_PER_DATASET));
  int ovr_list[2] = {2, 4};
  EXPECT_EQ(CE_None, main_ds->BuildOverviews("NEAREST", 2, ovr_list, 0, nullptr,
                                             nullptr, nullptr));
  // BuildOverviews on GTiff with internal per-dataset mask automatically builds
  // overviews on the mask band too.
  EXPECT_GE(main_ds->GetRasterBand(1)->GetMaskBand()->GetOverviewCount(), 1);

  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  EXPECT_EQ(CE_None, main_ds->SetSpatialRef(&srs));
  GDALGeoTransform gt(10.0, 1.0, 0.0, 20.0, 0.0, -1.0);
  EXPECT_EQ(CE_None, main_ds->SetGeoTransform(gt));

  // Set RPC and GEOLOCATION metadata.
  const char* rpc_md[] = {"LINE_OFF=10", "LINE_SCALE=20", "SAMP_OFF=30",
                          "SAMP_SCALE=40", nullptr};
  EXPECT_EQ(CE_None, main_ds->SetMetadata(const_cast<char**>(rpc_md), "RPC"));
  const char* geoloc_md[] = {"PIXEL_OFFSET=0", "LINE_OFFSET=0", "PIXEL_STEP=1",
                             "LINE_STEP=1", nullptr};
  EXPECT_EQ(CE_None,
            main_ds->SetMetadata(const_cast<char**>(geoloc_md), "GEOLOCATION"));
  EXPECT_EQ(CE_None, main_ds->SetMetadataItem("MAIN_KEY", "MAIN_VAL", ""));

  // Invalid overview level returns nullptr.
  EXPECT_EQ(nullptr, GDALCreateOverviewDataset(main_ds.get(), 5, false));

  // Create overview dataset at level 0 with bThisLevelOnly = false.
  GDALDatasetUniquePtr ovr_ds(
      GDALCreateOverviewDataset(main_ds.get(), 0, false));
  ASSERT_NE(nullptr, ovr_ds);
  EXPECT_EQ(8, ovr_ds->GetRasterXSize());
  EXPECT_EQ(8, ovr_ds->GetRasterYSize());
  EXPECT_NE(nullptr, ovr_ds->GetDriver());

  EXPECT_NE(nullptr, ovr_ds->GetSpatialRef());
  GDALGeoTransform ovr_gt;
  EXPECT_EQ(CE_None, ovr_ds->GetGeoTransform(ovr_gt));
  EXPECT_DOUBLE_EQ(2.0, ovr_gt[1]);

  // Metadata: RPC, GEOLOCATION, and default domain.
  EXPECT_NE(nullptr, ovr_ds->GetMetadata("RPC"));
  EXPECT_NE(nullptr, ovr_ds->GetMetadata("RPC"));  // cached second call
  EXPECT_NE(nullptr, ovr_ds->GetMetadataItem("LINE_OFF", "RPC"));
  EXPECT_NE(nullptr, ovr_ds->GetMetadata("GEOLOCATION"));
  EXPECT_NE(nullptr, ovr_ds->GetMetadata("GEOLOCATION"));  // cached second call
  EXPECT_NE(nullptr, ovr_ds->GetMetadataItem("PIXEL_STEP", "GEOLOCATION"));
  EXPECT_STREQ("MAIN_VAL", ovr_ds->GetMetadataItem("MAIN_KEY", ""));

  // Overview band methods.
  GDALRasterBand* b1 = ovr_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, b1);
  EXPECT_EQ(1, b1->GetOverviewCount());
  EXPECT_NE(nullptr, b1->GetOverview(0));
  EXPECT_EQ(nullptr, b1->GetOverview(-1));
  EXPECT_EQ(nullptr, b1->GetOverview(5));
  EXPECT_EQ(GMF_PER_DATASET, b1->GetMaskFlags());
  GDALRasterBand* mb = b1->GetMaskBand();
  ASSERT_NE(nullptr, mb);
  EXPECT_GE(mb->GetOverviewCount(), 0);
  mb->GetOverview(0);
  mb->GetMaskFlags();
  mb->GetMaskBand();

  // RasterIO at full ovr resolution and subsampled (triggers
  // TryOverviewRasterIO).
  std::vector<GByte> buf(128, 0);
  EXPECT_EQ(CE_None, ovr_ds->RasterIO(GF_Read, 0, 0, 8, 8, buf.data(), 8, 8,
                                      GDT_Byte, 2, nullptr, 0, 0, 0, nullptr));
  EXPECT_EQ(CE_None, ovr_ds->RasterIO(GF_Read, 0, 0, 8, 8, buf.data(), 4, 4,
                                      GDT_Byte, 2, nullptr, 0, 0, 0, nullptr));
  EXPECT_EQ(CE_None, b1->RasterIO(GF_Read, 0, 0, 8, 8, buf.data(), 8, 8,
                                  GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(CE_None, b1->RasterIO(GF_Read, 0, 0, 8, 8, buf.data(), 4, 4,
                                  GDT_Byte, 0, 0, nullptr));

  // Also test bThisLevelOnly = true.
  GDALDatasetUniquePtr ovr_only_ds(
      GDALCreateOverviewDataset(main_ds.get(), 0, true));
  ASSERT_NE(nullptr, ovr_only_ds);
  EXPECT_EQ(0, ovr_only_ds->GetRasterBand(1)->GetOverviewCount());
  EXPECT_EQ(CE_None, ovr_only_ds->GetRasterBand(1)->RasterIO(
                         GF_Read, 0, 0, 8, 8, buf.data(), 8, 8, GDT_Byte, 0, 0,
                         nullptr));
  EXPECT_EQ(CE_None, ovr_only_ds->FlushCache(false));

  // Also test nOvrLevel = -1.
  GDALDatasetUniquePtr level_neg1_ds(
      GDALCreateOverviewDataset(main_ds.get(), -1, true));
  ASSERT_NE(nullptr, level_neg1_ds);
}

TEST_F(GdalOverviewDatasetTest, GCPsAndStandaloneOverviewBands) {
  // Mismatched overview band dimensions -> returns nullptr.
  MainDSWithStandaloneOvr mismatch_ds(4, 4, 2, 2);
  EXPECT_EQ(nullptr, GDALCreateOverviewDataset(&mismatch_ds, 0, false));

  // Standalone overview bands (poOvrDS == nullptr) + GCPs.
  MainDSWithStandaloneOvr standalone_ds(4, 4, 4, 4);
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr gcp_mem_ds(
      mem_drv->Create("gcp_mem", 8, 8, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, gcp_mem_ds);
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  GDAL_GCP gcp;
  GDALInitGCPs(1, &gcp);
  absl::Cleanup deinit_gcp = [&gcp] { GDALDeinitGCPs(1, &gcp); };
  gcp.dfGCPPixel = 4.0;
  gcp.dfGCPLine = 4.0;
  EXPECT_EQ(CE_None, gcp_mem_ds->SetGCPs(1, &gcp, &srs));

  GDALDatasetUniquePtr ovr_gcp_ds(
      GDALCreateOverviewDataset(gcp_mem_ds.get(), -1, false));
  ASSERT_NE(nullptr, ovr_gcp_ds);
  EXPECT_EQ(1, ovr_gcp_ds->GetGCPCount());
  EXPECT_NE(nullptr, ovr_gcp_ds->GetGCPSpatialRef());
  const GDAL_GCP* gcps = ovr_gcp_ds->GetGCPs();
  ASSERT_NE(nullptr, gcps);
  EXPECT_EQ(gcps, ovr_gcp_ds->GetGCPs());  // cached second call

  // GetGeoTransform failure when main_ds has no GeoTransform.
  GDALGeoTransform gt;
  EXPECT_EQ(CE_Failure, ovr_gcp_ds->GetGeoTransform(gt));

  // IRasterIO when poOvrDS == nullptr (iterates over bands).
  GDALDatasetUniquePtr standalone_ovr(
      GDALCreateOverviewDataset(&standalone_ds, 0, true));
  ASSERT_NE(nullptr, standalone_ovr);
  std::vector<GByte> buf(32, 0);
  EXPECT_EQ(CE_None,
            standalone_ovr->RasterIO(GF_Read, 0, 0, 4, 4, buf.data(), 4, 4,
                                     GDT_Byte, 2, nullptr, 0, 0, 0, nullptr));
  EXPECT_EQ(42, buf[0]);
  EXPECT_EQ(nullptr, standalone_ovr->GetGCPs());
}

}  // namespace
}  // namespace autotest2
