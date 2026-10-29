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
#include <thread>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace gdal {
namespace {

class CustomRAT final : public GDALRasterAttributeTable {
 public:
  GDALRasterAttributeTable* Clone() const override { return new CustomRAT(); }
  int GetColumnCount() const override { return 0; }
  const char* GetNameOfCol(int) const override { return nullptr; }
  GDALRATFieldUsage GetUsageOfCol(int) const override { return GFU_Generic; }
  GDALRATFieldType GetTypeOfCol(int) const override { return GFT_Integer; }
  int GetColOfUsage(GDALRATFieldUsage) const override { return -1; }
  int GetRowCount() const override { return 0; }
  const char* GetValueAsString(int, int) const override { return nullptr; }
  int GetValueAsInt(int, int) const override { return 0; }
  double GetValueAsDouble(int, int) const override { return 0.0; }
  bool GetValueAsBoolean(int, int) const override { return false; }
  GDALRATDateTime GetValueAsDateTime(int, int) const override { return {}; }
  const GByte* GetValueAsWKBGeometry(int, int, size_t&) const override {
    return nullptr;
  }
  CPLErr ValuesIO(GDALRWFlag, int, int, int, double*) override {
    return CE_Failure;
  }
  CPLErr ValuesIO(GDALRWFlag, int, int, int, int*) override {
    return CE_Failure;
  }
  CPLErr ValuesIO(GDALRWFlag, int, int, int, char**) override {
    return CE_Failure;
  }
  CPLErr ValuesIO(GDALRWFlag, int, int, int, bool*) override {
    return CE_Failure;
  }
  CPLErr ValuesIO(GDALRWFlag, int, int, int, GDALRATDateTime*) override {
    return CE_Failure;
  }
  CPLErr ValuesIO(GDALRWFlag, int, int, int, GByte**, size_t*) override {
    return CE_Failure;
  }
  CPLErr SetValue(int, int, const char*) override { return CE_Failure; }
  CPLErr SetValue(int, int, int) override { return CE_Failure; }
  CPLErr SetValue(int, int, double) override { return CE_Failure; }
  CPLErr SetValue(int, int, bool) override { return CE_Failure; }
  CPLErr SetValue(int, int, const GDALRATDateTime&) override {
    return CE_Failure;
  }
  CPLErr SetValue(int, int, const void*, size_t) override { return CE_Failure; }
  int ChangesAreWrittenToFile() override { return FALSE; }
  CPLErr SetTableType(const GDALRATTableType) override { return CE_Failure; }
  void RemoveStatistics() override {}
  GDALRATTableType GetTableType() const override { return GRTT_THEMATIC; }
};

class NonClonableDataset final : public GDALDataset {
 public:
  NonClonableDataset() {
    nRasterXSize = 4;
    nRasterYSize = 4;
  }
};

class BadCloneBand final : public GDALRasterBand {
 public:
  BadCloneBand(GDALDataset* ds, int x, int y, GDALDataType dt, int bx, int by) {
    poDS = ds;
    nBand = 1;
    nRasterXSize = x;
    nRasterYSize = y;
    eDataType = dt;
    nBlockXSize = bx;
    nBlockYSize = by;
  }
  CPLErr IReadBlock(int, int, void*) override { return CE_None; }
  GDALRasterAttributeTable* GetDefaultRAT() override { return m_rat; }
  GDALRasterAttributeTable* m_rat = nullptr;
};

class BadCloneDataset final : public GDALDataset {
 public:
  BadCloneDataset(int x, int y, GDALDataType dt = GDT_Byte, int bx = 4,
                  int by = 4) {
    nRasterXSize = x;
    nRasterYSize = y;
    auto* b = new BadCloneBand(this, x, y, dt, bx, by);
    SetBand(1, b);
  }
  bool CanBeCloned(int /*nScopeFlags*/,
                   bool /*bCanShareState*/) const override {
    return true;
  }
  std::unique_ptr<GDALDataset> Clone(int /*nScopeFlags*/,
                                     bool /*bCanShareState*/) const override {
    if (m_clone_mode == 1) {
      return std::make_unique<BadCloneDataset>(nRasterXSize + 1, nRasterYSize);
    }
    if (m_clone_mode == 2) {
      return std::make_unique<BadCloneDataset>(nRasterXSize, nRasterYSize,
                                               GDT_Int16, 4, 4);
    }
    if (m_clone_mode == 3) {
      return std::make_unique<BadCloneDataset>(nRasterXSize, nRasterYSize,
                                               GDT_Byte, 2, 2);
    }
    return nullptr;
  }
  mutable int m_clone_mode = 0;
};

TEST(GdalThreadSafeDatasetTest, ErrorCasesAndMismatchClones) {
  GDALAllRegister();
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);

  EXPECT_FALSE(GDALDatasetIsThreadSafe(nullptr, GDAL_OF_RASTER, nullptr));
  EXPECT_EQ(GDALGetThreadSafeDataset(static_cast<GDALDatasetH>(nullptr),
                                     GDAL_OF_RASTER, nullptr),
            nullptr);

  // Non-clonable dataset & invalid scope flags
  auto non_clonable = std::make_unique<NonClonableDataset>();
  EXPECT_FALSE(non_clonable->IsThreadSafe(GDAL_OF_RASTER));
  EXPECT_EQ(GDALGetThreadSafeDataset(non_clonable.get(), GDAL_OF_VECTOR),
            nullptr);
  EXPECT_EQ(GDALGetThreadSafeDataset(non_clonable.get(), GDAL_OF_RASTER),
            nullptr);
  EXPECT_EQ(GDALGetThreadSafeDataset(std::make_unique<NonClonableDataset>(),
                                     GDAL_OF_VECTOR),
            nullptr);
  EXPECT_EQ(GDALGetThreadSafeDataset(std::make_unique<NonClonableDataset>(),
                                     GDAL_OF_RASTER),
            nullptr);

  // Clone failures and mismatched clone characteristics
  BadCloneDataset bad_ds(4, 4);
  auto* band0 = static_cast<BadCloneBand*>(bad_ds.GetRasterBand(1));
  CustomRAT custom_rat;
  band0->m_rat = &custom_rat;

  std::unique_ptr<GDALDataset> ts_bad(
      GDALGetThreadSafeDataset(&bad_ds, GDAL_OF_RASTER));
  ASSERT_NE(ts_bad, nullptr);

  // Non-GDALDefaultRasterAttributeTable RAT returns nullptr
  EXPECT_EQ(ts_bad->GetRasterBand(1)->GetDefaultRAT(), nullptr);
  band0->m_rat = nullptr;
  EXPECT_EQ(ts_bad->GetRasterBand(1)->GetDefaultRAT(), nullptr);

  // Mode 0: Clone returns nullptr
  bad_ds.m_clone_mode = 0;
  EXPECT_EQ(ts_bad->FlushCache(false), CE_None);
  GByte buf[16] = {0};
  EXPECT_EQ(ts_bad->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 4, 4, buf, 4, 4,
                                               GDT_Byte, 0, 0, nullptr),
            CE_Failure);

  // Mode 1: Clone returns dataset with different XSize
  bad_ds.m_clone_mode = 1;
  EXPECT_EQ(ts_bad->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 4, 4, buf, 4, 4,
                                               GDT_Byte, 0, 0, nullptr),
            CE_Failure);

  // Mode 2: Clone returns band with different DataType
  bad_ds.m_clone_mode = 2;
  EXPECT_EQ(ts_bad->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 4, 4, buf, 4, 4,
                                               GDT_Byte, 0, 0, nullptr),
            CE_Failure);

  // Mode 3: Clone returns band with different BlockSize
  BadCloneDataset bad_ds3(4, 4);
  bad_ds3.m_clone_mode = 3;
  std::unique_ptr<GDALDataset> ts_bad3(
      GDALGetThreadSafeDataset(&bad_ds3, GDAL_OF_RASTER));
  EXPECT_EQ(ts_bad3->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 4, 4, buf, 4, 4,
                                                GDT_Byte, 0, 0, nullptr),
            CE_Failure);
}

TEST(GdalThreadSafeDatasetTest, FullMultiThreadedOperations) {
  GDALAllRegister();
  GDALDriver* tiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(tiff_drv, nullptr);
  const char* tiff_path = "/vsimem/ts_dataset_test.tif";

  // Create GTiff with overviews, mask, SRS, GCPs, metadata, RAT, color table,
  // unit
  const char* tiff_srs_path = "/vsimem/ts_dataset_srs.tif";
  {
    std::unique_ptr<GDALDataset> ds_srs(
        tiff_drv->Create(tiff_srs_path, 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(ds_srs, nullptr);
    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    EXPECT_EQ(ds_srs->SetSpatialRef(&srs), CE_None);
    GDALGeoTransform gt(10.0, 1.0, 0.0, 20.0, 0.0, -1.0);
    EXPECT_EQ(ds_srs->SetGeoTransform(gt), CE_None);
  }
  {
    std::unique_ptr<GDALDataset> src_srs_ds(
        GDALDataset::Open(tiff_srs_path, GDAL_OF_RASTER));
    ASSERT_NE(src_srs_ds, nullptr);
    std::unique_ptr<GDALDataset> ts_srs_ds =
        GDALGetThreadSafeDataset(std::move(src_srs_ds), GDAL_OF_RASTER);
    ASSERT_NE(ts_srs_ds, nullptr);
    EXPECT_NE(ts_srs_ds->GetSpatialRef(), nullptr);
    EXPECT_NE(ts_srs_ds->GetSpatialRef(), nullptr);  // cached
  }
  VSIUnlink(tiff_srs_path);

  {
    std::unique_ptr<GDALDataset> ds(
        tiff_drv->Create(tiff_path, 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(ds, nullptr);
    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    GDAL_GCP gcp;
    GDALInitGCPs(1, &gcp);
    gcp.dfGCPPixel = 1.0;
    gcp.dfGCPLine = 2.0;
    EXPECT_EQ(ds->SetGCPs(1, &gcp, &srs), CE_None);
    GDALDeinitGCPs(1, &gcp);
    EXPECT_EQ(ds->SetMetadataItem("DS_KEY", "DS_VAL", ""), CE_None);

    GDALRasterBand* b = ds->GetRasterBand(1);
    EXPECT_EQ(b->Fill(77.0), CE_None);
    EXPECT_EQ(b->SetMetadataItem("B_KEY", "B_VAL", ""), CE_None);
    EXPECT_EQ(b->SetUnitType("m"), CE_None);
    GDALColorTable ct;
    GDALColorEntry ce = {1, 2, 3, 255};
    ct.SetColorEntry(0, &ce);
    EXPECT_EQ(b->SetColorTable(&ct), CE_None);
    GDALDefaultRasterAttributeTable rat;
    rat.CreateColumn("val", GFT_Integer, GFU_Generic);
    EXPECT_EQ(b->SetDefaultRAT(&rat), CE_None);

    EXPECT_EQ(ds->CreateMaskBand(GMF_PER_DATASET), CE_None);
    int ovr = 2;
    EXPECT_EQ(
        ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr, nullptr, nullptr),
        CE_None);
  }

  std::unique_ptr<GDALDataset> src_ds(
      GDALDataset::Open(tiff_path, GDAL_OF_RASTER));
  ASSERT_NE(src_ds, nullptr);

  // Test C API and raw pointer API
  GDALDatasetH h_ts = GDALGetThreadSafeDataset(
      GDALDataset::ToHandle(src_ds.get()), GDAL_OF_RASTER, nullptr);
  ASSERT_NE(h_ts, nullptr);
  EXPECT_TRUE(GDALDatasetIsThreadSafe(h_ts, GDAL_OF_RASTER, nullptr));

  // Passing an already thread-safe dataset returns it with incremented ref
  GDALDataset* ts_raw = GDALDataset::FromHandle(h_ts);
  GDALDataset* ts_again = GDALGetThreadSafeDataset(ts_raw, GDAL_OF_RASTER);
  EXPECT_EQ(ts_again, ts_raw);
  ts_again->ReleaseRef();
  GDALReleaseDataset(h_ts);

  // Test unique_ptr API
  std::unique_ptr<GDALDataset> ts_ds =
      GDALGetThreadSafeDataset(std::move(src_ds), GDAL_OF_RASTER);
  ASSERT_NE(ts_ds, nullptr);
  // Passing already thread-safe unique_ptr returns the same unique_ptr
  GDALDataset* raw_ptr = ts_ds.get();
  ts_ds = GDALGetThreadSafeDataset(std::move(ts_ds), GDAL_OF_RASTER);
  EXPECT_EQ(ts_ds.get(), raw_ptr);

  // Prototype forwarding methods on dataset
  EXPECT_EQ(ts_ds->GetSpatialRef(), nullptr);
  EXPECT_NE(ts_ds->GetGCPSpatialRef(), nullptr);
  EXPECT_NE(ts_ds->GetGCPSpatialRef(), nullptr);  // cached
  EXPECT_NE(ts_ds->GetGCPs(), nullptr);
  EXPECT_STREQ(ts_ds->GetMetadataItem("DS_KEY", ""), "DS_VAL");
  EXPECT_NE(ts_ds->GetMetadata(""), nullptr);
  {
    CPLErrorStateBackuper quiet(CPLQuietErrorHandler);
    EXPECT_EQ(ts_ds->BeginAsyncReader(0, 0, 4, 4, nullptr, 4, 4, GDT_Byte, 1,
                                      nullptr, 0, 0, 0, nullptr),
              nullptr);
  }

  // Prototype forwarding methods on band
  GDALRasterBand* ts_b = ts_ds->GetRasterBand(1);
  ASSERT_NE(ts_b, nullptr);
  EXPECT_STREQ(ts_b->GetMetadataItem("B_KEY", ""), "B_VAL");
  EXPECT_NE(ts_b->GetMetadata(""), nullptr);
  EXPECT_STREQ(ts_b->GetUnitType(), "m");
  EXPECT_NE(ts_b->GetColorTable(), nullptr);
  EXPECT_NE(ts_b->GetDefaultRAT(), nullptr);
  {
    CPLErrorStateBackuper quiet(CPLQuietErrorHandler);
    int pspace = 0;
    GIntBig lspace = 0;
    EXPECT_EQ(ts_b->GetVirtualMemAuto(GF_Read, &pspace, &lspace, nullptr),
              nullptr);
  }

  EXPECT_EQ(ts_b->GetOverviewCount(), 1);
  EXPECT_EQ(ts_b->GetOverview(-1), nullptr);
  EXPECT_EQ(ts_b->GetOverview(5), nullptr);
  GDALRasterBand* ts_ovr = ts_b->GetOverview(0);
  ASSERT_NE(ts_ovr, nullptr);
  EXPECT_NE(ts_b->GetRasterSampleOverview(4), nullptr);

  GDALRasterBand* ts_mask = ts_b->GetMaskBand();
  ASSERT_NE(ts_mask, nullptr);
  GDALRasterBand* ts_mask_mask = ts_mask->GetMaskBand();
  ASSERT_NE(ts_mask_mask, nullptr);

  // Worker thread reading from dataset, band, overview, mask, and mask-of-mask
  std::thread worker([&]() {
    GByte buf[256] = {0};
    EXPECT_EQ(ts_ds->RasterIO(GF_Read, 0, 0, 16, 16, buf, 16, 16, GDT_Byte, 1,
                              nullptr, 0, 0, 0, nullptr),
              CE_None);
    // Second call hits the thread-local LRU cache!
    EXPECT_EQ(ts_b->RasterIO(GF_Read, 0, 0, 16, 16, buf, 16, 16, GDT_Byte, 0, 0,
                             nullptr),
              CE_None);
    EXPECT_EQ(buf[0], 77);
    EXPECT_EQ(ts_ovr->RasterIO(GF_Read, 0, 0, 8, 8, buf, 8, 8, GDT_Byte, 0, 0,
                               nullptr),
              CE_None);
    EXPECT_EQ(ts_mask->RasterIO(GF_Read, 0, 0, 16, 16, buf, 16, 16, GDT_Byte, 0,
                                0, nullptr),
              CE_None);
    EXPECT_EQ(ts_mask_mask->RasterIO(GF_Read, 0, 0, 16, 16, buf, 16, 16,
                                     GDT_Byte, 0, 0, nullptr),
              CE_None);
  });
  worker.join();

  // Also read on main thread so main thread's TLS cache has an entry when ts_ds
  // is destroyed
  GByte main_buf[256] = {0};
  EXPECT_EQ(ts_b->RasterIO(GF_Read, 0, 0, 16, 16, main_buf, 16, 16, GDT_Byte, 0,
                           0, nullptr),
            CE_None);

  ts_ds.reset();
  VSIUnlink(tiff_path);
  VSIUnlink("/vsimem/ts_dataset_test.tif.aux.xml");
}

}  // namespace
}  // namespace gdal
