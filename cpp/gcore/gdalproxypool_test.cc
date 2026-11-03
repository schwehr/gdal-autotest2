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
// Unit tests for gcore/gdalproxypool.cpp.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalproxypool.cpp

#include <array>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_proxy.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

// Internal helper symbols in gcore/gdalproxypool.cpp not exported by a public
// header; keep in sync with third_party/gdal/gcore/gdalproxypool.cpp.
extern void GDALDatasetPoolPreventDestroy();
extern void GDALDatasetPoolForceDestroy();

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalproxypool_test";

void CreateTestGeoTiff(const std::string& filename, int xsize = 16,
                       int ysize = 16, int bands = 2, bool with_srs_gt = true,
                       bool with_gcps = false, bool with_ovr_and_mask = false) {
  GDALDriver* const drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(filename.c_str(), xsize, ysize, bands, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);

  if (with_srs_gt) {
    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    EXPECT_EQ(CE_None, ds->SetSpatialRef(&srs));
    GDALGeoTransform gt(10.0, 1.0, 0.0, 20.0, 0.0, -1.0);
    EXPECT_EQ(CE_None, ds->SetGeoTransform(gt));
  }
  if (with_gcps) {
    OGRSpatialReference gcp_srs;
    gcp_srs.SetWellKnownGeogCS("WGS84");
    GDAL_GCP gcp;
    GDALInitGCPs(1, &gcp);
    absl::Cleanup deinit_gcp = [&gcp]() { GDALDeinitGCPs(1, &gcp); };
    gcp.dfGCPPixel = 1.0;
    gcp.dfGCPLine = 2.0;
    gcp.dfGCPX = 100.0;
    gcp.dfGCPY = 200.0;
    EXPECT_EQ(CE_None, ds->SetGCPs(1, &gcp, &gcp_srs));
  }

  ds->SetMetadataItem("DS_KEY", "DS_VAL");
  ds->SetMetadataItem("SUB_KEY", "SUB_VAL", "CUSTOM_DOMAIN");

  GDALRasterBand* const b1 = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, b1);
  b1->Fill(42);
  b1->SetMetadataItem("BAND_KEY", "BAND_VAL");
  b1->SetUnitType("meters");
  constexpr auto cats = std::to_array<const char*>({"Cat0", "Cat1", nullptr});
  b1->SetCategoryNames(const_cast<char**>(cats.data()));
  GDALColorTable ct;
  GDALColorEntry entry = {10, 20, 30, 255};
  ct.SetColorEntry(0, &entry);
  b1->SetColorTable(&ct);

  if (with_ovr_and_mask) {
    int ovr_list[] = {2};
    EXPECT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, ovr_list, 0, nullptr,
                                          nullptr, nullptr));
    EXPECT_EQ(CE_None, b1->CreateMaskBand(GMF_PER_DATASET));
  }
}

TEST(GdalProxyPoolTest, PoolSizingAndSingletonLifecycle) {
  absl::Cleanup cleanup_dir = []() { VSIRmdirRecursive(kTestDir); };
  GDALRegister_GTiff();
  ASSERT_NE(nullptr, GetGDALDriverManager()->GetDriverByName("GTiff"));

  {
    CPLConfigOptionSetter s1("GDAL_MAX_DATASET_POOL_SIZE", "1", false);
    EXPECT_EQ(2, GDALGetMaxDatasetPoolSize());
  }
  {
    CPLConfigOptionSetter s2("GDAL_MAX_DATASET_POOL_SIZE", "5000", false);
    EXPECT_EQ(1000, GDALGetMaxDatasetPoolSize());
  }

  const std::string pool1_path = std::string(kTestDir) + "/pool1.tif";
  const std::string pool2_path = std::string(kTestDir) + "/pool2.tif";
  const std::string pool3_path = std::string(kTestDir) + "/pool3.tif";
  CreateTestGeoTiff(pool1_path);
  CreateTestGeoTiff(pool2_path);
  CreateTestGeoTiff(pool3_path);

  // Test GDAL_MAX_DATASET_POOL_RAM_USAGE with MB and GB suffixes, and
  // PreventDestroy / ForceDestroy:
  {
    CPLConfigOptionSetter s_size("GDAL_MAX_DATASET_POOL_SIZE", "2", false);
    CPLConfigOptionSetter s_ram("GDAL_MAX_DATASET_POOL_RAM_USAGE", "1MB",
                                false);
    GDALDatasetPoolPreventDestroy();  // no-op when singleton is null
    GDALDatasetPoolForceDestroy();    // no-op when singleton is null

    std::unique_ptr<GDALProxyPoolDataset> p1(GDALProxyPoolDataset::Create(
        pool1_path.c_str(), nullptr, GA_ReadOnly, TRUE, "owner1"));
    ASSERT_NE(nullptr, p1);
    GDALDatasetPoolPreventDestroy();
    p1.reset();
    // Singleton is still alive due to PreventDestroy:
    GDALDatasetPoolForceDestroy();
  }

  {
    CPLConfigOptionSetter s_ram("GDAL_MAX_DATASET_POOL_RAM_USAGE", "1GB",
                                false);
    std::unique_ptr<GDALProxyPoolDataset> p1(
        GDALProxyPoolDataset::Create(pool1_path.c_str()));
    ASSERT_NE(nullptr, p1);
  }

  // Test LRU cache hits, moving entries to head, and eviction at maxSize=2:
  {
    CPLConfigOptionSetter s_size("GDAL_MAX_DATASET_POOL_SIZE", "2", false);
    constexpr auto oo = std::to_array<const char*>({"NUM_THREADS=1", nullptr});
    std::unique_ptr<GDALProxyPoolDataset> p1(GDALProxyPoolDataset::Create(
        pool1_path.c_str(), const_cast<char**>(oo.data()), GA_ReadOnly, TRUE,
        "ownerA"));
    std::unique_ptr<GDALProxyPoolDataset> p2(GDALProxyPoolDataset::Create(
        pool2_path.c_str(), const_cast<char**>(oo.data()), GA_ReadOnly, TRUE,
        "ownerA"));
    std::unique_ptr<GDALProxyPoolDataset> p3(GDALProxyPoolDataset::Create(
        pool3_path.c_str(), const_cast<char**>(oo.data()), GA_ReadOnly, FALSE,
        nullptr));
    ASSERT_NE(nullptr, p1);
    ASSERT_NE(nullptr, p2);
    ASSERT_NE(nullptr, p3);

    // Access p1, then p2, then p1 again (moves p1 from tail to head), then p3
    // (evicts LRU entry with refCount == 0):
    EXPECT_NE(nullptr, p1->GetMetadata(""));
    EXPECT_NE(nullptr, p2->GetMetadata(""));
    EXPECT_NE(nullptr, p1->GetMetadataItem("DS_KEY", ""));
    EXPECT_NE(nullptr, p3->GetMetadata(""));
    // Access p3 again when it is already at firstEntry:
    EXPECT_NE(nullptr, p3->GetMetadata(""));
  }

  // Create on non-existent dataset returns nullptr:
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    const std::string missing_path =
        std::string(kTestDir) + "/does_not_exist.tif";
    EXPECT_EQ(nullptr, GDALProxyPoolDataset::Create(missing_path.c_str()));
    CPLErrorReset();
  }
}

TEST(GdalProxyPoolTest, DatasetAndBandMethodsAndCAPI) {
  absl::Cleanup cleanup_dir = []() { VSIRmdirRecursive(kTestDir); };
  GDALRegister_GTiff();
  GDALRegister_VRT();
  ASSERT_NE(nullptr, GetGDALDriverManager()->GetDriverByName("GTiff"));
  ASSERT_NE(nullptr, GetGDALDriverManager()->GetDriverByName("VRT"));

  const std::string full_tif_path = std::string(kTestDir) + "/proxy_full.tif";
  const std::string gcp_tif_path = std::string(kTestDir) + "/proxy_gcp.tif";
  const std::string vrt_path = std::string(kTestDir) + "/proxy_cats.vrt";

  CreateTestGeoTiff(full_tif_path, 16, 16, 2, true, false, true);
  CreateTestGeoTiff(gcp_tif_path, 16, 16, 1, false, true, false);

  // Create a VRT with CategoryNames so GetCategoryNames() returns non-null:
  const std::string vrt_xml =
      std::string(
          "<VRTDataset rasterXSize=\"16\" rasterYSize=\"16\">"
          "  <VRTRasterBand dataType=\"Byte\" band=\"1\">"
          "    <CategoryNames><Category>Cat0</Category><Category>Cat1</"
          "Category></CategoryNames>"
          "    <SimpleSource>"
          "      <SourceFilename relativeToVRT=\"0\">") +
      full_tif_path +
      "</SourceFilename>"
      "      <SourceBand>1</SourceBand>"
      "    </SimpleSource>"
      "  </VRTRasterBand>"
      "</VRTDataset>";
  {
    VsiMemTempWrapper vrt_file(vrt_path, vrt_xml);
    std::unique_ptr<GDALProxyPoolDataset> cats_proxy(
        GDALProxyPoolDataset::Create(vrt_path.c_str()));
    ASSERT_NE(nullptr, cats_proxy);
    EXPECT_NE(nullptr, cats_proxy->GetRasterBand(1)->GetCategoryNames());
    EXPECT_NE(nullptr, cats_proxy->GetRasterBand(1)->GetCategoryNames());
  }

  // 1. C API:
  double gt_arr[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  GDALProxyPoolDatasetH h_ds =
      GDALProxyPoolDatasetCreate(full_tif_path.c_str(), 16, 16, GA_Update,
                                 FALSE, SRS_WKT_WGS84_LAT_LONG, gt_arr);
  ASSERT_NE(nullptr, h_ds);
  absl::Cleanup delete_h_ds = [h_ds]() { GDALProxyPoolDatasetDelete(h_ds); };

  GDALProxyPoolDatasetAddSrcBandDescription(h_ds, GDT_Byte, 16, 16);
  // Add second band with 0x0 block size so RefUnderlyingRasterBand loads it:
  auto* cpp_ds = reinterpret_cast<GDALProxyPoolDataset*>(h_ds);
  cpp_ds->AddSrcBand(2, GDT_Byte, 0, 0);

  // FlushCache before underlying dataset is opened:
  EXPECT_EQ(CE_None, cpp_ds->FlushCache(false));

  // SpatialRef and GeoTransform (cached from constructor vs delegated):
  EXPECT_NE(nullptr, cpp_ds->GetSpatialRef());
  OGRSpatialReference srs2;
  srs2.SetWellKnownGeogCS("NAD83");
  EXPECT_EQ(CE_None, cpp_ds->SetSpatialRef(&srs2));
  EXPECT_NE(nullptr, cpp_ds->GetSpatialRef());

  GDALGeoTransform gt_out;
  EXPECT_EQ(CE_None, cpp_ds->GetGeoTransform(gt_out));
  EXPECT_DOUBLE_EQ(1.0, gt_out[1]);
  GDALGeoTransform gt_new(5.0, 2.0, 0.0, 10.0, 0.0, -2.0);
  EXPECT_EQ(CE_None, cpp_ds->SetGeoTransform(gt_new));
  EXPECT_EQ(CE_None, cpp_ds->GetGeoTransform(gt_out));
  EXPECT_DOUBLE_EQ(5.0, gt_out[0]);

  // Metadata and MetadataItem on dataset:
  EXPECT_NE(nullptr, cpp_ds->GetMetadata(""));
  EXPECT_NE(nullptr, cpp_ds->GetMetadata("CUSTOM_DOMAIN"));
  EXPECT_NE(nullptr, cpp_ds->GetMetadata(""));  // repeat tests hash set replace
  EXPECT_STREQ("DS_VAL", cpp_ds->GetMetadataItem("DS_KEY", ""));
  EXPECT_STREQ("DS_VAL", cpp_ds->GetMetadataItem("DS_KEY", ""));
  EXPECT_EQ(nullptr, cpp_ds->GetMetadataItem("MISSING", ""));

  // GetInternalHandle:
  EXPECT_EQ(nullptr, cpp_ds->GetInternalHandle(" dummy "));

  // Band 1 & Band 2 operations:
  auto* band1 =
      dynamic_cast<GDALProxyPoolRasterBand*>(cpp_ds->GetRasterBand(1));
  auto* band2 =
      dynamic_cast<GDALProxyPoolRasterBand*>(cpp_ds->GetRasterBand(2));
  ASSERT_NE(nullptr, band1);
  ASSERT_NE(nullptr, band2);

  // Trigger RefUnderlyingRasterBand on band2 (which has block size 0x0):
  EXPECT_EQ(CE_None, band2->FlushCache(false));
  int bx = 0;
  int by = 0;
  band2->GetBlockSize(&bx, &by);
  EXPECT_GT(bx, 0);
  EXPECT_GT(by, 0);

  // Band metadata, category names, unit type, color table (call twice):
  EXPECT_NE(nullptr, band1->GetMetadata(""));
  EXPECT_NE(nullptr, band1->GetMetadata(""));
  EXPECT_STREQ("BAND_VAL", band1->GetMetadataItem("BAND_KEY", ""));
  EXPECT_STREQ("BAND_VAL", band1->GetMetadataItem("BAND_KEY", ""));
  EXPECT_EQ(nullptr, band1->GetCategoryNames());
  EXPECT_EQ(nullptr, band1->GetCategoryNames());
  EXPECT_STREQ("meters", band1->GetUnitType());
  EXPECT_STREQ("meters", band1->GetUnitType());
  EXPECT_NE(nullptr, band1->GetColorTable());
  EXPECT_NE(nullptr, band1->GetColorTable());

  // Overviews:
  EXPECT_EQ(nullptr, band1->GetOverview(-1));
  EXPECT_EQ(nullptr, band1->GetOverview(99));
  GDALRasterBand* const ovr0 = band1->GetOverview(0);
  ASSERT_NE(nullptr, ovr0);
  EXPECT_EQ(ovr0, band1->GetOverview(0));  // cached
  GByte pixel = 0;
  EXPECT_EQ(CE_None, ovr0->RasterIO(GF_Read, 0, 0, 1, 1, &pixel, 1, 1, GDT_Byte,
                                    0, 0, nullptr));
  EXPECT_EQ(CE_None, ovr0->FlushCache(false));

  // GetRasterSampleOverview (unimplemented -> nullptr):
  EXPECT_EQ(nullptr, band1->GetRasterSampleOverview(10));

  // MaskBand via GetMaskBand() on band1, and AddSrcMaskBandDescription on
  // band2:
  GDALRasterBand* const mask1 = band1->GetMaskBand();
  ASSERT_NE(nullptr, mask1);
  EXPECT_EQ(mask1, band1->GetMaskBand());
  EXPECT_TRUE(mask1->IsMaskBand());
  EXPECT_EQ(CE_None, mask1->RasterIO(GF_Read, 0, 0, 1, 1, &pixel, 1, 1,
                                     GDT_Byte, 0, 0, nullptr));

  band2->AddSrcMaskBandDescription(GDT_Byte, 16, 16);
  EXPECT_NE(nullptr, band2->GetMaskBand());

  // FlushCache when underlying dataset is open:
  EXPECT_EQ(CE_None, cpp_ds->FlushCache(false));
  EXPECT_EQ(CE_None, band1->FlushCache(false));

  // 2. Test AddSrcMaskBandDescriptionFromUnderlying and GCPs on proxy_gcp.tif:
  std::unique_ptr<GDALProxyPoolDataset> gcp_proxy(
      GDALProxyPoolDataset::Create(gcp_tif_path.c_str()));
  ASSERT_NE(nullptr, gcp_proxy);
  auto* gcp_band =
      dynamic_cast<GDALProxyPoolRasterBand*>(gcp_proxy->GetRasterBand(1));
  ASSERT_NE(nullptr, gcp_band);
  gcp_band->AddSrcMaskBandDescriptionFromUnderlying();
  gcp_band->AddSrcMaskBandDescriptionFromUnderlying();  // idempotent

  EXPECT_EQ(1, gcp_proxy->GetGCPCount());
  EXPECT_NE(nullptr, gcp_proxy->GetGCPs());
  EXPECT_NE(nullptr, gcp_proxy->GetGCPs());  // second call frees first list
}

}  // namespace
}  // namespace autotest2
