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

#include <memory>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace gdal {
namespace {

// Custom dataset whose overview band has GetDataset() == nullptr
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
    memset(pImage, 42, static_cast<size_t>(nBlockXSize) * nBlockYSize);
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
    memset(pImage, 1, 64);
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

TEST(GdalOverviewDatasetTest, CreationAndAllMethods) {
  GDALAllRegister();
  GDALDriver* tiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(tiff_drv, nullptr);

  // 0-band dataset returns nullptr
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  std::unique_ptr<GDALDataset> zero_band_ds(
      mem_drv->Create("zero", 8, 8, 0, GDT_Byte, nullptr));
  EXPECT_EQ(GDALCreateOverviewDataset(zero_band_ds.get(), 0, false), nullptr);

  // Create GTiff with 2 bands, overviews (level 0: 8x8, level 1: 4x4), and
  // per-dataset mask
  const char* tiff_path = "/vsimem/ovr_ds_test.tif";
  std::unique_ptr<GDALDataset> main_ds(
      tiff_drv->Create(tiff_path, 16, 16, 2, GDT_Byte, nullptr));
  ASSERT_NE(main_ds, nullptr);
  EXPECT_EQ(main_ds->CreateMaskBand(GMF_PER_DATASET), CE_None);
  int ovr_list[2] = {2, 4};
  EXPECT_EQ(main_ds->BuildOverviews("NEAREST", 2, ovr_list, 0, nullptr, nullptr,
                                    nullptr),
            CE_None);
  // BuildOverviews on GTiff with internal per-dataset mask automatically builds
  // overviews on the mask band too!
  EXPECT_GE(main_ds->GetRasterBand(1)->GetMaskBand()->GetOverviewCount(), 1);

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  EXPECT_EQ(main_ds->SetSpatialRef(&srs), CE_None);
  GDALGeoTransform gt(10.0, 1.0, 0.0, 20.0, 0.0, -1.0);
  EXPECT_EQ(main_ds->SetGeoTransform(gt), CE_None);

  // Set RPC and GEOLOCATION metadata
  const char* rpc_md[] = {"LINE_OFF=10", "LINE_SCALE=20", "SAMP_OFF=30",
                          "SAMP_SCALE=40", nullptr};
  EXPECT_EQ(main_ds->SetMetadata(const_cast<char**>(rpc_md), "RPC"), CE_None);
  const char* geoloc_md[] = {"PIXEL_OFFSET=0", "LINE_OFFSET=0", "PIXEL_STEP=1",
                             "LINE_STEP=1", nullptr};
  EXPECT_EQ(main_ds->SetMetadata(const_cast<char**>(geoloc_md), "GEOLOCATION"),
            CE_None);
  EXPECT_EQ(main_ds->SetMetadataItem("MAIN_KEY", "MAIN_VAL", ""), CE_None);

  // Invalid overview level returns nullptr
  EXPECT_EQ(GDALCreateOverviewDataset(main_ds.get(), 5, false), nullptr);

  // Create overview dataset at level 0 with bThisLevelOnly = false
  std::unique_ptr<GDALDataset> ovr_ds(
      GDALCreateOverviewDataset(main_ds.get(), 0, false));
  ASSERT_NE(ovr_ds, nullptr);
  EXPECT_EQ(ovr_ds->GetRasterXSize(), 8);
  EXPECT_EQ(ovr_ds->GetRasterYSize(), 8);
  EXPECT_NE(ovr_ds->GetDriver(), nullptr);

  EXPECT_NE(ovr_ds->GetSpatialRef(), nullptr);
  GDALGeoTransform ovr_gt;
  EXPECT_EQ(ovr_ds->GetGeoTransform(ovr_gt), CE_None);
  EXPECT_DOUBLE_EQ(ovr_gt[1], 2.0);

  // Metadata: RPC, GEOLOCATION, and default domain
  EXPECT_NE(ovr_ds->GetMetadata("RPC"), nullptr);
  EXPECT_NE(ovr_ds->GetMetadata("RPC"), nullptr);  // cached second call
  EXPECT_NE(ovr_ds->GetMetadataItem("LINE_OFF", "RPC"), nullptr);
  EXPECT_NE(ovr_ds->GetMetadata("GEOLOCATION"), nullptr);
  EXPECT_NE(ovr_ds->GetMetadata("GEOLOCATION"), nullptr);  // cached second call
  EXPECT_NE(ovr_ds->GetMetadataItem("PIXEL_STEP", "GEOLOCATION"), nullptr);
  EXPECT_STREQ(ovr_ds->GetMetadataItem("MAIN_KEY", ""), "MAIN_VAL");

  // Overview band methods
  GDALRasterBand* b1 = ovr_ds->GetRasterBand(1);
  ASSERT_NE(b1, nullptr);
  EXPECT_EQ(b1->GetOverviewCount(), 1);
  EXPECT_NE(b1->GetOverview(0), nullptr);
  EXPECT_EQ(b1->GetOverview(-1), nullptr);
  EXPECT_EQ(b1->GetOverview(5), nullptr);
  EXPECT_EQ(b1->GetMaskFlags(), GMF_PER_DATASET);
  GDALRasterBand* mb = b1->GetMaskBand();
  ASSERT_NE(mb, nullptr);
  EXPECT_GE(mb->GetOverviewCount(), 0);
  mb->GetOverview(0);
  mb->GetMaskFlags();
  mb->GetMaskBand();

  // RasterIO at full ovr resolution and subsampled (triggers
  // TryOverviewRasterIO)
  GByte buf[128] = {0};
  EXPECT_EQ(ovr_ds->RasterIO(GF_Read, 0, 0, 8, 8, buf, 8, 8, GDT_Byte, 2,
                             nullptr, 0, 0, 0, nullptr),
            CE_None);
  EXPECT_EQ(ovr_ds->RasterIO(GF_Read, 0, 0, 8, 8, buf, 4, 4, GDT_Byte, 2,
                             nullptr, 0, 0, 0, nullptr),
            CE_None);
  EXPECT_EQ(
      b1->RasterIO(GF_Read, 0, 0, 8, 8, buf, 8, 8, GDT_Byte, 0, 0, nullptr),
      CE_None);
  EXPECT_EQ(
      b1->RasterIO(GF_Read, 0, 0, 8, 8, buf, 4, 4, GDT_Byte, 0, 0, nullptr),
      CE_None);

  // Also test bThisLevelOnly = true
  std::unique_ptr<GDALDataset> ovr_only_ds(
      GDALCreateOverviewDataset(main_ds.get(), 0, true));
  ASSERT_NE(ovr_only_ds, nullptr);
  EXPECT_EQ(ovr_only_ds->GetRasterBand(1)->GetOverviewCount(), 0);
  EXPECT_EQ(ovr_only_ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 8, 8, buf, 8,
                                                    8, GDT_Byte, 0, 0, nullptr),
            CE_None);
  EXPECT_EQ(ovr_only_ds->FlushCache(false), CE_None);

  // Also test nOvrLevel = -1
  std::unique_ptr<GDALDataset> level_neg1_ds(
      GDALCreateOverviewDataset(main_ds.get(), -1, true));
  ASSERT_NE(level_neg1_ds, nullptr);

  ovr_ds.reset();
  ovr_only_ds.reset();
  level_neg1_ds.reset();
  main_ds.reset();
  VSIUnlink(tiff_path);
}

TEST(GdalOverviewDatasetTest, GCPsAndStandaloneOverviewBands) {
  GDALAllRegister();
  // Mismatched overview band dimensions -> returns nullptr
  MainDSWithStandaloneOvr mismatch_ds(4, 4, 2, 2);
  EXPECT_EQ(GDALCreateOverviewDataset(&mismatch_ds, 0, false), nullptr);

  // Standalone overview bands (poOvrDS == nullptr) + GCPs
  MainDSWithStandaloneOvr standalone_ds(4, 4, 4, 4);
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  std::unique_ptr<GDALDataset> gcp_mem_ds(
      mem_drv->Create("gcp_mem", 8, 8, 1, GDT_Byte, nullptr));
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  GDAL_GCP gcp;
  GDALInitGCPs(1, &gcp);
  gcp.dfGCPPixel = 4.0;
  gcp.dfGCPLine = 4.0;
  EXPECT_EQ(gcp_mem_ds->SetGCPs(1, &gcp, &srs), CE_None);
  GDALDeinitGCPs(1, &gcp);

  std::unique_ptr<GDALDataset> ovr_gcp_ds(
      GDALCreateOverviewDataset(gcp_mem_ds.get(), -1, false));
  ASSERT_NE(ovr_gcp_ds, nullptr);
  EXPECT_EQ(ovr_gcp_ds->GetGCPCount(), 1);
  EXPECT_NE(ovr_gcp_ds->GetGCPSpatialRef(), nullptr);
  const GDAL_GCP* gcps = ovr_gcp_ds->GetGCPs();
  ASSERT_NE(gcps, nullptr);
  EXPECT_EQ(ovr_gcp_ds->GetGCPs(), gcps);  // cached second call

  // GetGeoTransform failure when main_ds has no GeoTransform
  GDALGeoTransform gt;
  EXPECT_EQ(ovr_gcp_ds->GetGeoTransform(gt), CE_Failure);

  // IRasterIO when poOvrDS == nullptr (iterates over bands)
  std::unique_ptr<GDALDataset> standalone_ovr(
      GDALCreateOverviewDataset(&standalone_ds, 0, true));
  ASSERT_NE(standalone_ovr, nullptr);
  GByte buf[32] = {0};
  EXPECT_EQ(standalone_ovr->RasterIO(GF_Read, 0, 0, 4, 4, buf, 4, 4, GDT_Byte,
                                     2, nullptr, 0, 0, 0, nullptr),
            CE_None);
  EXPECT_EQ(buf[0], 42);
  EXPECT_EQ(standalone_ovr->GetGCPs(), nullptr);
}

}  // namespace
}  // namespace gdal
