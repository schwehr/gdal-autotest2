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
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/frmts/vrt/gdal_vrt.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_proxy.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

extern void GDALNullifyProxyPoolSingleton();
extern void GDALDatasetPoolPreventDestroy();
extern void GDALDatasetPoolForceDestroy();

namespace {

void CreateTestGeoTiff(const char* filename, int xsize = 16, int ysize = 16,
                       int bands = 2, bool with_srs_gt = true,
                       bool with_gcps = false, bool with_ovr_and_mask = false) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(drv, nullptr);
  GDALDataset* ds =
      drv->Create(filename, xsize, ysize, bands, GDT_Byte, nullptr);
  ASSERT_NE(ds, nullptr);

  if (with_srs_gt) {
    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    EXPECT_EQ(ds->SetSpatialRef(&srs), CE_None);
    GDALGeoTransform gt(10.0, 1.0, 0.0, 20.0, 0.0, -1.0);
    EXPECT_EQ(ds->SetGeoTransform(gt), CE_None);
  }
  if (with_gcps) {
    OGRSpatialReference gcp_srs;
    gcp_srs.SetWellKnownGeogCS("WGS84");
    GDAL_GCP gcp;
    GDALInitGCPs(1, &gcp);
    gcp.dfGCPPixel = 1.0;
    gcp.dfGCPLine = 2.0;
    gcp.dfGCPX = 100.0;
    gcp.dfGCPY = 200.0;
    EXPECT_EQ(ds->SetGCPs(1, &gcp, &gcp_srs), CE_None);
    GDALDeinitGCPs(1, &gcp);
  }

  ds->SetMetadataItem("DS_KEY", "DS_VAL");
  ds->SetMetadataItem("SUB_KEY", "SUB_VAL", "CUSTOM_DOMAIN");

  GDALRasterBand* b1 = ds->GetRasterBand(1);
  b1->Fill(42);
  b1->SetMetadataItem("BAND_KEY", "BAND_VAL");
  b1->SetUnitType("meters");
  const char* cats[] = {"Cat0", "Cat1", nullptr};
  b1->SetCategoryNames(const_cast<char**>(cats));
  GDALColorTable ct;
  GDALColorEntry entry = {10, 20, 30, 255};
  ct.SetColorEntry(0, &entry);
  b1->SetColorTable(&ct);

  if (with_ovr_and_mask) {
    int ovr_list[] = {2};
    EXPECT_EQ(ds->BuildOverviews("NEAREST", 1, ovr_list, 0, nullptr, nullptr,
                                 nullptr),
              CE_None);
    EXPECT_EQ(b1->CreateMaskBand(GMF_PER_DATASET), CE_None);
  }

  GDALClose(ds);
}

TEST(GdalProxyPoolTest, PoolSizingAndSingletonLifecycle) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  {
    CPLConfigOptionSetter s1("GDAL_MAX_DATASET_POOL_SIZE", "1", false);
    EXPECT_EQ(GDALGetMaxDatasetPoolSize(), 2);
  }
  {
    CPLConfigOptionSetter s2("GDAL_MAX_DATASET_POOL_SIZE", "5000", false);
    EXPECT_EQ(GDALGetMaxDatasetPoolSize(), 1000);
  }

  CreateTestGeoTiff("/vsimem/pool1.tif");
  CreateTestGeoTiff("/vsimem/pool2.tif");
  CreateTestGeoTiff("/vsimem/pool3.tif");

  // Test GDAL_MAX_DATASET_POOL_RAM_USAGE with MB and GB suffixes, and
  // PreventDestroy / ForceDestroy:
  {
    CPLConfigOptionSetter s_size("GDAL_MAX_DATASET_POOL_SIZE", "2", false);
    CPLConfigOptionSetter s_ram("GDAL_MAX_DATASET_POOL_RAM_USAGE", "1MB",
                                false);
    GDALDatasetPoolPreventDestroy();  // no-op when singleton is null
    GDALDatasetPoolForceDestroy();    // no-op when singleton is null

    auto* p1 = GDALProxyPoolDataset::Create("/vsimem/pool1.tif", nullptr,
                                            GA_ReadOnly, TRUE, "owner1");
    ASSERT_NE(p1, nullptr);
    GDALDatasetPoolPreventDestroy();
    delete p1;
    // Singleton is still alive due to PreventDestroy:
    GDALDatasetPoolForceDestroy();
  }

  {
    CPLConfigOptionSetter s_ram("GDAL_MAX_DATASET_POOL_RAM_USAGE", "1GB",
                                false);
    auto* p1 = GDALProxyPoolDataset::Create("/vsimem/pool1.tif");
    ASSERT_NE(p1, nullptr);
    delete p1;
  }

  // Test LRU cache hits, moving entries to head, and eviction at maxSize=2:
  {
    CPLConfigOptionSetter s_size("GDAL_MAX_DATASET_POOL_SIZE", "2", false);
    const char* oo[] = {"NUM_THREADS=1", nullptr};
    auto* p1 = GDALProxyPoolDataset::Create("/vsimem/pool1.tif", oo,
                                            GA_ReadOnly, TRUE, "ownerA");
    auto* p2 = GDALProxyPoolDataset::Create("/vsimem/pool2.tif", oo,
                                            GA_ReadOnly, TRUE, "ownerA");
    auto* p3 = GDALProxyPoolDataset::Create("/vsimem/pool3.tif", oo,
                                            GA_ReadOnly, FALSE, nullptr);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    ASSERT_NE(p3, nullptr);

    // Access p1, then p2, then p1 again (moves p1 from tail to head), then p3
    // (evicts LRU entry with refCount == 0):
    EXPECT_NE(p1->GetMetadata(""), nullptr);
    EXPECT_NE(p2->GetMetadata(""), nullptr);
    EXPECT_NE(p1->GetMetadataItem("DS_KEY", ""), nullptr);
    EXPECT_NE(p3->GetMetadata(""), nullptr);
    // Access p3 again when it is already at firstEntry:
    EXPECT_NE(p3->GetMetadata(""), nullptr);

    delete p1;
    delete p2;
    delete p3;
  }

  // Create on non-existent dataset returns nullptr:
  EXPECT_EQ(GDALProxyPoolDataset::Create("/vsimem/does_not_exist.tif"),
            nullptr);

  VSIUnlink("/vsimem/pool1.tif");
  VSIUnlink("/vsimem/pool2.tif");
  VSIUnlink("/vsimem/pool3.tif");
  CPLPopErrorHandler();
}

TEST(GdalProxyPoolTest, DatasetAndBandMethodsAndCAPI) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  CreateTestGeoTiff("/vsimem/proxy_full.tif", 16, 16, 2, true, false, true);
  CreateTestGeoTiff("/vsimem/proxy_gcp.tif", 16, 16, 1, false, true, false);

  // Also create a VRT with CategoryNames so GetCategoryNames() returns
  // non-null:
  const char* vrt_xml =
      "<VRTDataset rasterXSize=\"16\" rasterYSize=\"16\">"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\">"
      "    "
      "<CategoryNames><Category>Cat0</Category><Category>Cat1</Category></"
      "CategoryNames>"
      "    <SimpleSource>"
      "      <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/proxy_full.tif</SourceFilename>"
      "      <SourceBand>1</SourceBand>"
      "    </SimpleSource>"
      "  </VRTRasterBand>"
      "</VRTDataset>";
  VSILFILE* fp_vrt = VSIFOpenL("/vsimem/proxy_cats.vrt", "wb");
  ASSERT_NE(fp_vrt, nullptr);
  VSIFWriteL(vrt_xml, 1, strlen(vrt_xml), fp_vrt);
  VSIFCloseL(fp_vrt);
  {
    auto* cats_proxy = GDALProxyPoolDataset::Create("/vsimem/proxy_cats.vrt");
    ASSERT_NE(cats_proxy, nullptr);
    EXPECT_NE(cats_proxy->GetRasterBand(1)->GetCategoryNames(), nullptr);
    EXPECT_NE(cats_proxy->GetRasterBand(1)->GetCategoryNames(), nullptr);
    delete cats_proxy;
  }
  VSIUnlink("/vsimem/proxy_cats.vrt");

  // 1. C API:
  double gt_arr[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  GDALProxyPoolDatasetH hDS =
      GDALProxyPoolDatasetCreate("/vsimem/proxy_full.tif", 16, 16, GA_Update,
                                 FALSE, SRS_WKT_WGS84_LAT_LONG, gt_arr);
  ASSERT_NE(hDS, nullptr);
  GDALProxyPoolDatasetAddSrcBandDescription(hDS, GDT_Byte, 16, 16);
  // Add second band with 0x0 block size so RefUnderlyingRasterBand loads it:
  auto* cpp_ds = reinterpret_cast<GDALProxyPoolDataset*>(hDS);
  cpp_ds->AddSrcBand(2, GDT_Byte, 0, 0);

  // FlushCache before underlying dataset is opened:
  EXPECT_EQ(cpp_ds->FlushCache(false), CE_None);

  // SpatialRef and GeoTransform (cached from constructor vs delegated):
  EXPECT_NE(cpp_ds->GetSpatialRef(), nullptr);
  OGRSpatialReference srs2;
  srs2.SetWellKnownGeogCS("NAD83");
  EXPECT_EQ(cpp_ds->SetSpatialRef(&srs2), CE_None);
  EXPECT_NE(cpp_ds->GetSpatialRef(), nullptr);

  GDALGeoTransform gt_out;
  EXPECT_EQ(cpp_ds->GetGeoTransform(gt_out), CE_None);
  EXPECT_DOUBLE_EQ(gt_out[1], 1.0);
  GDALGeoTransform gt_new(5.0, 2.0, 0.0, 10.0, 0.0, -2.0);
  EXPECT_EQ(cpp_ds->SetGeoTransform(gt_new), CE_None);
  EXPECT_EQ(cpp_ds->GetGeoTransform(gt_out), CE_None);
  EXPECT_DOUBLE_EQ(gt_out[0], 5.0);

  // Metadata and MetadataItem on dataset:
  EXPECT_NE(cpp_ds->GetMetadata(""), nullptr);
  EXPECT_NE(cpp_ds->GetMetadata("CUSTOM_DOMAIN"), nullptr);
  EXPECT_NE(cpp_ds->GetMetadata(""),
            nullptr);  // repeat to test hash set replace
  EXPECT_STREQ(cpp_ds->GetMetadataItem("DS_KEY", ""), "DS_VAL");
  EXPECT_STREQ(cpp_ds->GetMetadataItem("DS_KEY", ""), "DS_VAL");
  EXPECT_EQ(cpp_ds->GetMetadataItem("MISSING", ""), nullptr);

  // GetInternalHandle:
  EXPECT_EQ(cpp_ds->GetInternalHandle(" dummy "), nullptr);

  // Band 1 & Band 2 operations:
  auto* band1 =
      dynamic_cast<GDALProxyPoolRasterBand*>(cpp_ds->GetRasterBand(1));
  auto* band2 =
      dynamic_cast<GDALProxyPoolRasterBand*>(cpp_ds->GetRasterBand(2));
  ASSERT_NE(band1, nullptr);
  ASSERT_NE(band2, nullptr);

  // Trigger RefUnderlyingRasterBand on band2 (which has block size 0x0):
  EXPECT_EQ(band2->FlushCache(false), CE_None);
  int bx = 0, by = 0;
  band2->GetBlockSize(&bx, &by);
  EXPECT_GT(bx, 0);
  EXPECT_GT(by, 0);

  // Band metadata, category names, unit type, color table (call twice):
  EXPECT_NE(band1->GetMetadata(""), nullptr);
  EXPECT_NE(band1->GetMetadata(""), nullptr);
  EXPECT_STREQ(band1->GetMetadataItem("BAND_KEY", ""), "BAND_VAL");
  EXPECT_STREQ(band1->GetMetadataItem("BAND_KEY", ""), "BAND_VAL");
  EXPECT_EQ(band1->GetCategoryNames(), nullptr);
  EXPECT_EQ(band1->GetCategoryNames(), nullptr);
  EXPECT_STREQ(band1->GetUnitType(), "meters");
  EXPECT_STREQ(band1->GetUnitType(), "meters");
  EXPECT_NE(band1->GetColorTable(), nullptr);
  EXPECT_NE(band1->GetColorTable(), nullptr);

  // Overviews:
  EXPECT_EQ(band1->GetOverview(-1), nullptr);
  EXPECT_EQ(band1->GetOverview(99), nullptr);
  GDALRasterBand* ovr0 = band1->GetOverview(0);
  ASSERT_NE(ovr0, nullptr);
  EXPECT_EQ(band1->GetOverview(0), ovr0);  // cached
  GByte pixel = 0;
  EXPECT_EQ(ovr0->RasterIO(GF_Read, 0, 0, 1, 1, &pixel, 1, 1, GDT_Byte, 0, 0,
                           nullptr),
            CE_None);
  EXPECT_EQ(ovr0->FlushCache(false), CE_None);

  // GetRasterSampleOverview (unimplemented -> nullptr):
  EXPECT_EQ(band1->GetRasterSampleOverview(10), nullptr);

  // MaskBand via GetMaskBand() on band1, and AddSrcMaskBandDescription on
  // band2:
  GDALRasterBand* mask1 = band1->GetMaskBand();
  ASSERT_NE(mask1, nullptr);
  EXPECT_EQ(band1->GetMaskBand(), mask1);
  EXPECT_TRUE(mask1->IsMaskBand());
  EXPECT_EQ(mask1->RasterIO(GF_Read, 0, 0, 1, 1, &pixel, 1, 1, GDT_Byte, 0, 0,
                            nullptr),
            CE_None);

  band2->AddSrcMaskBandDescription(GDT_Byte, 16, 16);
  EXPECT_NE(band2->GetMaskBand(), nullptr);

  // FlushCache when underlying dataset is open:
  EXPECT_EQ(cpp_ds->FlushCache(false), CE_None);
  EXPECT_EQ(band1->FlushCache(false), CE_None);

  GDALProxyPoolDatasetDelete(hDS);

  // 2. Test AddSrcMaskBandDescriptionFromUnderlying and GCPs on proxy_gcp.tif:
  auto* gcp_proxy = GDALProxyPoolDataset::Create("/vsimem/proxy_gcp.tif");
  ASSERT_NE(gcp_proxy, nullptr);
  auto* gcp_band =
      dynamic_cast<GDALProxyPoolRasterBand*>(gcp_proxy->GetRasterBand(1));
  ASSERT_NE(gcp_band, nullptr);
  gcp_band->AddSrcMaskBandDescriptionFromUnderlying();
  gcp_band->AddSrcMaskBandDescriptionFromUnderlying();  // idempotent

  EXPECT_EQ(gcp_proxy->GetGCPCount(), 1);
  // Note: GDALProxyPoolDataset::GetGCPSpatialRef() in upstream GDAL calls
  // m_poGCPSRS->Release() unconditionally when m_poGCPSRS is nullptr, so we
  // test GetGCPCount() and GetGCPs() instead.
  EXPECT_NE(gcp_proxy->GetGCPs(), nullptr);
  EXPECT_NE(gcp_proxy->GetGCPs(), nullptr);  // second call frees first list
  delete gcp_proxy;

  VSIUnlink("/vsimem/proxy_full.tif");
  VSIUnlink("/vsimem/proxy_full.tif.msk");
  VSIUnlink("/vsimem/proxy_gcp.tif");
  CPLPopErrorHandler();
}

}  // namespace
