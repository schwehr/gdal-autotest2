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
// Tests GDALThreadSafeDataset wrapper and thread-local dataset caching
// (gcore/gdalthreadsafedataset.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalthreadsafedataset.cpp

#include <array>
#include <memory>
#include <thread>  // NOLINT
#include <utility>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_colortable.h"
#include "third_party/gdal/gcore/gdal_cpp_functions.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
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
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(GDALDatasetIsThreadSafe(nullptr, GDAL_OF_RASTER, nullptr));
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALGetThreadSafeDataset(static_cast<GDALDatasetH>(nullptr),
                                       GDAL_OF_RASTER, nullptr));
    CPLErrorReset();

    // Non-clonable dataset & invalid scope flags
    auto non_clonable = std::make_unique<NonClonableDataset>();
    EXPECT_FALSE(non_clonable->IsThreadSafe(GDAL_OF_RASTER));
    EXPECT_EQ(nullptr,
              GDALGetThreadSafeDataset(non_clonable.get(), GDAL_OF_VECTOR));
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALGetThreadSafeDataset(non_clonable.get(), GDAL_OF_RASTER));
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALGetThreadSafeDataset(std::make_unique<NonClonableDataset>(),
                                       GDAL_OF_VECTOR));
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALGetThreadSafeDataset(std::make_unique<NonClonableDataset>(),
                                       GDAL_OF_RASTER));
    CPLErrorReset();
  }

  // Clone failures and mismatched clone characteristics
  BadCloneDataset bad_ds(4, 4);
  auto* band0 = static_cast<BadCloneBand*>(bad_ds.GetRasterBand(1));
  CustomRAT custom_rat;
  band0->m_rat = &custom_rat;

  std::unique_ptr<GDALDataset> ts_bad(
      GDALGetThreadSafeDataset(&bad_ds, GDAL_OF_RASTER));
  ASSERT_NE(nullptr, ts_bad);

  // Non-GDALDefaultRasterAttributeTable RAT returns nullptr
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ts_bad->GetRasterBand(1)->GetDefaultRAT());
    CPLErrorReset();
  }
  band0->m_rat = nullptr;
  EXPECT_EQ(nullptr, ts_bad->GetRasterBand(1)->GetDefaultRAT());

  // Mode 0: Clone returns nullptr
  bad_ds.m_clone_mode = 0;
  EXPECT_EQ(CE_None, ts_bad->FlushCache(false));
  std::array<GByte, 16> buf{};
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, ts_bad->GetRasterBand(1)->RasterIO(
                              GF_Read, 0, 0, 4, 4, buf.data(), 4, 4, GDT_Byte,
                              0, 0, nullptr));
    CPLErrorReset();
  }

  // Mode 1: Clone returns dataset with different XSize
  bad_ds.m_clone_mode = 1;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, ts_bad->GetRasterBand(1)->RasterIO(
                              GF_Read, 0, 0, 4, 4, buf.data(), 4, 4, GDT_Byte,
                              0, 0, nullptr));
    CPLErrorReset();
  }

  // Mode 2: Clone returns band with different DataType
  bad_ds.m_clone_mode = 2;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, ts_bad->GetRasterBand(1)->RasterIO(
                              GF_Read, 0, 0, 4, 4, buf.data(), 4, 4, GDT_Byte,
                              0, 0, nullptr));
    CPLErrorReset();
  }

  // Mode 3: Clone returns band with different BlockSize
  BadCloneDataset bad_ds3(4, 4);
  bad_ds3.m_clone_mode = 3;
  std::unique_ptr<GDALDataset> ts_bad3(
      GDALGetThreadSafeDataset(&bad_ds3, GDAL_OF_RASTER));
  ASSERT_NE(nullptr, ts_bad3);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, ts_bad3->GetRasterBand(1)->RasterIO(
                              GF_Read, 0, 0, 4, 4, buf.data(), 4, 4, GDT_Byte,
                              0, 0, nullptr));
    CPLErrorReset();
  }
}

TEST(GdalThreadSafeDatasetTest, FullMultiThreadedOperations) {
  GDALRegister_GTiff();
  absl::Cleanup cleanup_dir = [] {
    VSIRmdirRecursive("/vsimem/gdalthreadsafedataset_test");
  };

  GDALDriver* tiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, tiff_drv);
  constexpr char kTiffPath[] =
      "/vsimem/gdalthreadsafedataset_test/ts_dataset_test.tif";

  // Create GTiff with overviews, mask, SRS, GCPs, metadata, RAT, color table,
  // unit
  constexpr char kTiffSrsPath[] =
      "/vsimem/gdalthreadsafedataset_test/ts_dataset_srs.tif";
  {
    GDALDatasetUniquePtr ds_srs(
        tiff_drv->Create(kTiffSrsPath, 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds_srs);
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
    EXPECT_EQ(CE_None, ds_srs->SetSpatialRef(&srs));
    GDALGeoTransform gt(10.0, 1.0, 0.0, 20.0, 0.0, -1.0);
    EXPECT_EQ(CE_None, ds_srs->SetGeoTransform(gt));
  }
  {
    std::unique_ptr<GDALDataset> src_srs_ds(
        GDALDataset::Open(kTiffSrsPath, GDAL_OF_RASTER));
    ASSERT_NE(nullptr, src_srs_ds);
    std::unique_ptr<GDALDataset> ts_srs_ds =
        GDALGetThreadSafeDataset(std::move(src_srs_ds), GDAL_OF_RASTER);
    ASSERT_NE(nullptr, ts_srs_ds);
    EXPECT_NE(nullptr, ts_srs_ds->GetSpatialRef());
    EXPECT_NE(nullptr, ts_srs_ds->GetSpatialRef());  // cached
  }

  {
    GDALDatasetUniquePtr ds(
        tiff_drv->Create(kTiffPath, 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
    GDAL_GCP gcp;
    GDALInitGCPs(1, &gcp);
    {
      absl::Cleanup deinit_gcp = [&gcp] { GDALDeinitGCPs(1, &gcp); };
      gcp.dfGCPPixel = 1.0;
      gcp.dfGCPLine = 2.0;
      EXPECT_EQ(CE_None, ds->SetGCPs(1, &gcp, &srs));
    }
    EXPECT_EQ(CE_None, ds->SetMetadataItem("DS_KEY", "DS_VAL", ""));

    GDALRasterBand* b = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, b);
    EXPECT_EQ(CE_None, b->Fill(77.0));
    EXPECT_EQ(CE_None, b->SetMetadataItem("B_KEY", "B_VAL", ""));
    EXPECT_EQ(CE_None, b->SetUnitType("m"));
    GDALColorTable ct;
    constexpr GDALColorEntry kColorEntry = {1, 2, 3, 255};
    ct.SetColorEntry(0, &kColorEntry);
    EXPECT_EQ(CE_None, b->SetColorTable(&ct));
    GDALDefaultRasterAttributeTable rat;
    EXPECT_EQ(CE_None, rat.CreateColumn("val", GFT_Integer, GFU_Generic));
    EXPECT_EQ(CE_None, b->SetDefaultRAT(&rat));

    EXPECT_EQ(CE_None, ds->CreateMaskBand(GMF_PER_DATASET));
    constexpr std::array<int, 1> kOvrLevels = {2};
    EXPECT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, kOvrLevels.data(), 0,
                                          nullptr, nullptr, nullptr));
  }

  std::unique_ptr<GDALDataset> src_ds(
      GDALDataset::Open(kTiffPath, GDAL_OF_RASTER));
  ASSERT_NE(nullptr, src_ds);

  // Test C API and raw pointer API
  {
    GDALDatasetH h_ts = GDALGetThreadSafeDataset(
        GDALDataset::ToHandle(src_ds.get()), GDAL_OF_RASTER, nullptr);
    ASSERT_NE(nullptr, h_ts);
    absl::Cleanup release_h_ts = [h_ts] { GDALReleaseDataset(h_ts); };
    EXPECT_TRUE(GDALDatasetIsThreadSafe(h_ts, GDAL_OF_RASTER, nullptr));

    // Passing an already thread-safe dataset returns it with incremented ref
    GDALDataset* ts_raw = GDALDataset::FromHandle(h_ts);
    GDALDataset* ts_again = GDALGetThreadSafeDataset(ts_raw, GDAL_OF_RASTER);
    ASSERT_EQ(ts_raw, ts_again);
    ts_again->ReleaseRef();
  }

  // Test unique_ptr API
  std::unique_ptr<GDALDataset> ts_ds =
      GDALGetThreadSafeDataset(std::move(src_ds), GDAL_OF_RASTER);
  ASSERT_NE(nullptr, ts_ds);
  // Passing already thread-safe unique_ptr returns the same unique_ptr
  GDALDataset* raw_ptr = ts_ds.get();
  ts_ds = GDALGetThreadSafeDataset(std::move(ts_ds), GDAL_OF_RASTER);
  EXPECT_EQ(raw_ptr, ts_ds.get());

  // Prototype forwarding methods on dataset
  EXPECT_EQ(nullptr, ts_ds->GetSpatialRef());
  EXPECT_NE(nullptr, ts_ds->GetGCPSpatialRef());
  EXPECT_NE(nullptr, ts_ds->GetGCPSpatialRef());  // cached
  EXPECT_NE(nullptr, ts_ds->GetGCPs());
  EXPECT_STREQ("DS_VAL", ts_ds->GetMetadataItem("DS_KEY", ""));
  EXPECT_NE(nullptr, ts_ds->GetMetadata(""));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              ts_ds->BeginAsyncReader(0, 0, 4, 4, nullptr, 4, 4, GDT_Byte, 1,
                                      nullptr, 0, 0, 0, nullptr));
    CPLErrorReset();
  }

  // Prototype forwarding methods on band
  GDALRasterBand* ts_b = ts_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, ts_b);
  EXPECT_STREQ("B_VAL", ts_b->GetMetadataItem("B_KEY", ""));
  EXPECT_NE(nullptr, ts_b->GetMetadata(""));
  EXPECT_STREQ("m", ts_b->GetUnitType());
  EXPECT_NE(nullptr, ts_b->GetColorTable());
  EXPECT_NE(nullptr, ts_b->GetDefaultRAT());
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    int pspace = 0;
    GIntBig lspace = 0;
    EXPECT_EQ(nullptr,
              ts_b->GetVirtualMemAuto(GF_Read, &pspace, &lspace, nullptr));
    CPLErrorReset();
  }

  EXPECT_EQ(1, ts_b->GetOverviewCount());
  EXPECT_EQ(nullptr, ts_b->GetOverview(-1));
  EXPECT_EQ(nullptr, ts_b->GetOverview(5));
  GDALRasterBand* ts_ovr = ts_b->GetOverview(0);
  ASSERT_NE(nullptr, ts_ovr);
  EXPECT_NE(nullptr, ts_b->GetRasterSampleOverview(4));

  GDALRasterBand* ts_mask = ts_b->GetMaskBand();
  ASSERT_NE(nullptr, ts_mask);
  GDALRasterBand* ts_mask_mask = ts_mask->GetMaskBand();
  ASSERT_NE(nullptr, ts_mask_mask);

  // Worker thread reading from dataset, band, overview, mask, and mask-of-mask
  {
    std::jthread worker([&]() {
      std::array<GByte, 256> buf{};
      EXPECT_EQ(CE_None,
                ts_ds->RasterIO(GF_Read, 0, 0, 16, 16, buf.data(), 16, 16,
                                GDT_Byte, 1, nullptr, 0, 0, 0, nullptr));
      // Second call hits the thread-local LRU cache!
      EXPECT_EQ(CE_None, ts_b->RasterIO(GF_Read, 0, 0, 16, 16, buf.data(), 16,
                                        16, GDT_Byte, 0, 0, nullptr));
      EXPECT_EQ(77, buf[0]);
      EXPECT_EQ(CE_None, ts_ovr->RasterIO(GF_Read, 0, 0, 8, 8, buf.data(), 8, 8,
                                          GDT_Byte, 0, 0, nullptr));
      EXPECT_EQ(CE_None, ts_mask->RasterIO(GF_Read, 0, 0, 16, 16, buf.data(),
                                           16, 16, GDT_Byte, 0, 0, nullptr));
      EXPECT_EQ(CE_None,
                ts_mask_mask->RasterIO(GF_Read, 0, 0, 16, 16, buf.data(), 16,
                                       16, GDT_Byte, 0, 0, nullptr));
    });
  }

  // Also read on main thread so main thread's TLS cache has an entry when ts_ds
  // is destroyed
  std::array<GByte, 256> main_buf{};
  EXPECT_EQ(CE_None, ts_b->RasterIO(GF_Read, 0, 0, 16, 16, main_buf.data(), 16,
                                    16, GDT_Byte, 0, 0, nullptr));
}

}  // namespace
}  // namespace autotest2
