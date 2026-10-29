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
// limitations under the License.IT

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_float.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_virtualmem.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace gdal {
namespace {

class TestDataset;

class TestRasterBand : public GDALRasterBand {
 public:
  explicit TestRasterBand(int bForceCached = FALSE)
      : GDALRasterBand(bForceCached) {
    nRasterXSize = 16;
    nRasterYSize = 16;
    nBlockXSize = 8;
    nBlockYSize = 8;
    eDataType = GDT_Byte;
    eAccess = GA_Update;
    nBand = 1;
    AllocateStorage();
  }

  TestRasterBand(GDALDataset* ds, int band, GDALDataType dt, int xsize,
                 int ysize, int bxsize, int bysize, int bForceCached = FALSE)
      : GDALRasterBand(bForceCached) {
    poDS = ds;
    nBand = band;
    nRasterXSize = xsize;
    nRasterYSize = ysize;
    nBlockXSize = bxsize;
    nBlockYSize = bysize;
    eDataType = dt;
    eAccess = ds ? ds->GetAccess() : GA_Update;
    AllocateStorage();
  }

  void AllocateStorage() {
    if (nRasterXSize > 0 && nRasterYSize > 0 &&
        GDALGetDataTypeSizeBytes(eDataType) > 0) {
      storage_.assign(static_cast<size_t>(nRasterXSize) * nRasterYSize *
                          GDALGetDataTypeSizeBytes(eDataType),
                      0);
    }
  }

  void SetDimsAndType(int rx, int ry, int bx, int by, GDALDataType dt) {
    FlushCache();
    nRasterXSize = rx;
    nRasterYSize = ry;
    nBlockXSize = bx;
    nBlockYSize = by;
    eDataType = dt;
    AllocateStorage();
  }

  void SetBandNum(int b) { nBand = b; }
  void SetDS(GDALDataset* ds) { poDS = ds; }
  void SetAccessMode(GDALAccess acc) { eAccess = acc; }
  void SetBlockReads(int n) { nBlockReads = n; }

  using GDALRasterBand::AddBlockToFreeList;
  using GDALRasterBand::EmitErrorMessageIfWriteNotSupported;
  using GDALRasterBand::EnterReadWrite;
  using GDALRasterBand::GetMOFlags;
  using GDALRasterBand::HasBlockCache;
  using GDALRasterBand::HasDirtyBlocks;
  using GDALRasterBand::InitBlockInfo;
  using GDALRasterBand::InitRWLock;
  using GDALRasterBand::InvalidateMaskBand;
  using GDALRasterBand::LeaveReadWrite;
  using GDALRasterBand::SetMOFlags;
  using GDALRasterBand::SetValidPercent;
  using GDALRasterBand::SplitRasterIO;

  int BaseIGetDataCoverageStatus(int nXOff, int nYOff, int nXSize, int nYSize,
                                 int nMaskFlagStop, double* pdfDataPct) {
    return GDALRasterBand::IGetDataCoverageStatus(nXOff, nYOff, nXSize, nYSize,
                                                  nMaskFlagStop, pdfDataPct);
  }

  CPLErr BaseIWriteBlock(int x, int y, void* pImage) {
    return GDALRasterBand::IWriteBlock(x, y, pImage);
  }

  CPLErr IReadBlock(int nBlockXOff, int nBlockYOff, void* pImage) override {
    if (fail_read_) {
      if (emit_error_on_fail_read_) {
        ReportError(CE_Failure, CPLE_AppDefined, "Simulated read failure");
      }
      return CE_Failure;
    }
    const int dt_size = GDALGetDataTypeSizeBytes(eDataType);
    if (dt_size <= 0 || nBlockXSize <= 0 || nBlockYSize <= 0) {
      return CE_Failure;
    }
    std::memset(pImage, 0,
                static_cast<size_t>(nBlockXSize) * nBlockYSize * dt_size);
    if (storage_.empty()) return CE_None;
    for (int y = 0; y < nBlockYSize; ++y) {
      int src_y = nBlockYOff * nBlockYSize + y;
      if (src_y >= nRasterYSize) break;
      int copy_x =
          std::min(nBlockXSize, nRasterXSize - nBlockXOff * nBlockXSize);
      if (copy_x <= 0) continue;
      const size_t src_off = (static_cast<size_t>(src_y) * nRasterXSize +
                              static_cast<size_t>(nBlockXOff) * nBlockXSize) *
                             dt_size;
      const size_t dst_off = static_cast<size_t>(y) * nBlockXSize * dt_size;
      std::memcpy(static_cast<GByte*>(pImage) + dst_off,
                  storage_.data() + src_off,
                  static_cast<size_t>(copy_x) * dt_size);
    }
    return CE_None;
  }

  CPLErr IWriteBlock(int nBlockXOff, int nBlockYOff, void* pImage) override {
    if (use_base_iwriteblock_) {
      return GDALRasterBand::IWriteBlock(nBlockXOff, nBlockYOff, pImage);
    }
    if (fail_write_) {
      return CE_Failure;
    }
    const int dt_size = GDALGetDataTypeSizeBytes(eDataType);
    if (dt_size <= 0 || storage_.empty()) return CE_None;
    for (int y = 0; y < nBlockYSize; ++y) {
      int dst_y = nBlockYOff * nBlockYSize + y;
      if (dst_y >= nRasterYSize) break;
      int copy_x =
          std::min(nBlockXSize, nRasterXSize - nBlockXOff * nBlockXSize);
      if (copy_x <= 0) continue;
      const size_t dst_off = (static_cast<size_t>(dst_y) * nRasterXSize +
                              static_cast<size_t>(nBlockXOff) * nBlockXSize) *
                             dt_size;
      const size_t src_off = static_cast<size_t>(y) * nBlockXSize * dt_size;
      std::memcpy(storage_.data() + dst_off,
                  static_cast<const GByte*>(pImage) + src_off,
                  static_cast<size_t>(copy_x) * dt_size);
    }
    return CE_None;
  }

  CPLErr IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
                   int nYSize, void* pData, int nBufXSize, int nBufYSize,
                   GDALDataType eBufType, GSpacing nPixelSpace,
                   GSpacing nLineSpace,
                   GDALRasterIOExtraArg* psExtraArg) override {
    if (nPixelSpace == 0) nPixelSpace = GDALGetDataTypeSizeBytes(eBufType);
    if (nLineSpace == 0) nLineSpace = nPixelSpace * nBufXSize;
    return GDALRasterBand::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                     pData, nBufXSize, nBufYSize, eBufType,
                                     nPixelSpace, nLineSpace, psExtraArg);
  }

  int HasArbitraryOverviews() override {
    if (arbitrary_overviews_) return TRUE;
    return GDALRasterBand::HasArbitraryOverviews();
  }

  int GetOverviewCount() override {
    if (!overviews_.empty()) return static_cast<int>(overviews_.size());
    return GDALRasterBand::GetOverviewCount();
  }

  GDALRasterBand* GetOverview(int i) override {
    if (!overviews_.empty()) {
      if (i >= 0 && i < static_cast<int>(overviews_.size())) {
        return overviews_[i];
      }
      return nullptr;
    }
    return GDALRasterBand::GetOverview(i);
  }

  double GetNoDataValue(int* pbSuccess = nullptr) override {
    if (has_nodata_) {
      if (pbSuccess) *pbSuccess = TRUE;
      return nodata_val_;
    }
    return GDALRasterBand::GetNoDataValue(pbSuccess);
  }

  int64_t GetNoDataValueAsInt64(int* pbSuccess = nullptr) override {
    if (has_nodata_i64_) {
      if (pbSuccess) *pbSuccess = TRUE;
      return nodata_i64_;
    }
    return GDALRasterBand::GetNoDataValueAsInt64(pbSuccess);
  }

  uint64_t GetNoDataValueAsUInt64(int* pbSuccess = nullptr) override {
    if (has_nodata_u64_) {
      if (pbSuccess) *pbSuccess = TRUE;
      return nodata_u64_;
    }
    return GDALRasterBand::GetNoDataValueAsUInt64(pbSuccess);
  }

  CPLErr SetNoDataValue(double val) override {
    if (allow_set_nodata_) {
      InvalidateMaskBand();
      has_nodata_ = true;
      nodata_val_ = val;
      return CE_None;
    }
    return GDALRasterBand::SetNoDataValue(val);
  }

  CPLErr SetNoDataValueAsInt64(int64_t val) override {
    if (allow_set_nodata_) {
      InvalidateMaskBand();
      has_nodata_i64_ = true;
      nodata_i64_ = val;
      return CE_None;
    }
    return GDALRasterBand::SetNoDataValueAsInt64(val);
  }

  CPLErr SetNoDataValueAsUInt64(uint64_t val) override {
    if (allow_set_nodata_) {
      InvalidateMaskBand();
      has_nodata_u64_ = true;
      nodata_u64_ = val;
      return CE_None;
    }
    return GDALRasterBand::SetNoDataValueAsUInt64(val);
  }

  double GetMinimum(int* pbSuccess = nullptr) override {
    if (has_custom_min_) {
      if (pbSuccess) *pbSuccess = TRUE;
      return custom_min_;
    }
    return GDALRasterBand::GetMinimum(pbSuccess);
  }

  double GetMaximum(int* pbSuccess = nullptr) override {
    if (has_custom_max_) {
      if (pbSuccess) *pbSuccess = TRUE;
      return custom_max_;
    }
    return GDALRasterBand::GetMaximum(pbSuccess);
  }

  GDALRasterBand* GetMaskBand() override {
    if (custom_mask_band_) return custom_mask_band_;
    return GDALRasterBand::GetMaskBand();
  }

  int GetMaskFlags() override {
    if (custom_mask_band_) return custom_mask_flags_;
    return GDALRasterBand::GetMaskFlags();
  }

  GDALColorInterp GetColorInterpretation() override {
    if (custom_color_interp_ != GCI_Undefined) return custom_color_interp_;
    return GDALRasterBand::GetColorInterpretation();
  }

  GDALColorTable* GetColorTable() override {
    if (custom_color_table_) return custom_color_table_;
    return GDALRasterBand::GetColorTable();
  }

  const char* GetUnitType() override {
    if (!custom_unit_.empty()) return custom_unit_.c_str();
    return GDALRasterBand::GetUnitType();
  }

  double GetOffset(int* pbSuccess = nullptr) override {
    if (has_custom_offset_) {
      if (pbSuccess) *pbSuccess = TRUE;
      return custom_offset_;
    }
    return GDALRasterBand::GetOffset(pbSuccess);
  }

  double GetScale(int* pbSuccess = nullptr) override {
    if (has_custom_scale_) {
      if (pbSuccess) *pbSuccess = TRUE;
      return custom_scale_;
    }
    return GDALRasterBand::GetScale(pbSuccess);
  }

  bool fail_read_ = false;
  bool emit_error_on_fail_read_ = false;
  bool fail_write_ = false;
  bool use_base_iwriteblock_ = false;
  bool arbitrary_overviews_ = false;
  bool allow_set_nodata_ = false;
  bool has_nodata_ = false;
  double nodata_val_ = 0.0;
  bool has_nodata_i64_ = false;
  int64_t nodata_i64_ = 0;
  bool has_nodata_u64_ = false;
  uint64_t nodata_u64_ = 0;
  bool has_custom_min_ = false;
  double custom_min_ = 0.0;
  bool has_custom_max_ = false;
  double custom_max_ = 0.0;
  GDALRasterBand* custom_mask_band_ = nullptr;
  int custom_mask_flags_ = 0;
  GDALColorInterp custom_color_interp_ = GCI_Undefined;
  GDALColorTable* custom_color_table_ = nullptr;
  std::string custom_unit_;
  bool has_custom_offset_ = false;
  double custom_offset_ = 0.0;
  bool has_custom_scale_ = false;
  double custom_scale_ = 1.0;
  std::vector<GDALRasterBand*> overviews_;
  std::vector<GByte> storage_;
};

class TestDataset : public GDALDataset {
 public:
  TestDataset(int xsize = 16, int ysize = 16, GDALAccess access = GA_Update) {
    nRasterXSize = xsize;
    nRasterYSize = ysize;
    eAccess = access;
  }

  void SetOpenFlags(int flags) { nOpenFlags = flags; }

  void AddTestBand(TestRasterBand* band) {
    SetBand(GetRasterCount() + 1, band);
  }

  CPLErr GetGeoTransform(GDALGeoTransform& gt) const override {
    if (has_gt_) {
      gt = gt_;
      return CE_None;
    }
    return CE_Failure;
  }

  const OGRSpatialReference* GetSpatialRef() const override {
    if (has_srs_) return &srs_;
    return nullptr;
  }

  bool has_gt_ = false;
  GDALGeoTransform gt_{0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  bool has_srs_ = false;
  OGRSpatialReference srs_;
};

}  // namespace
namespace {

TEST(GDALRasterBandTest, ConstructorsAndDestructorBranches) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // 1. Default and bForceCachedIO constructors via GDAL_FORCE_CACHING
  {
    CPLConfigOptionSetter setter("GDAL_FORCE_CACHING", "YES", false);
    TestRasterBand band_default(FALSE);
    EXPECT_EQ(band_default.GetXSize(), 16);
    EXPECT_EQ(band_default.GetYSize(), 16);
  }

  // 2. Destructor with MarkSuppressOnClose on dataset and active block cache
  {
    auto ds = std::make_unique<TestDataset>(8, 8, GA_Update);
    auto* band = new TestRasterBand(ds.get(), 1, GDT_Byte, 8, 8, 4, 4);
    ds->AddTestBand(band);
    GDALRasterBlock* blk = band->GetLockedBlockRef(0, 0, TRUE);
    ASSERT_NE(blk, nullptr);
    blk->MarkDirty();
    blk->DropLock();
    ds->MarkSuppressOnClose();
  }

  // 3. Destructor when nBlockReads > nBlocksPerRow * nBlocksPerColumn
  {
    auto ds = std::make_unique<TestDataset>(8, 8, GA_Update);
    auto* band = new TestRasterBand(ds.get(), 1, GDT_Byte, 8, 8, 4, 4);
    ds->AddTestBand(band);
    ASSERT_TRUE(band->InitBlockInfo());
    band->SetBlockReads(100);
  }

  // 4. Destructor with m_poPointsCache populated via InterpolateAtPoint
  {
    auto ds = std::make_unique<TestDataset>(8, 8, GA_Update);
    auto* band = new TestRasterBand(ds.get(), 1, GDT_Byte, 8, 8, 4, 4);
    ds->AddTestBand(band);
    double val = 0.0;
    EXPECT_EQ(
        band->InterpolateAtPoint(2.5, 2.5, GRIORA_Bilinear, &val, nullptr),
        CE_None);
  }

  CPLPopErrorHandler();
}

TEST(GDALRasterBandTest, RasterIOAndCWrappers) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  auto ds = std::make_unique<TestDataset>(10, 10, GA_Update);
  auto* band = new TestRasterBand(ds.get(), 1, GDT_Byte, 10, 10, 5, 5, FALSE);
  ds->AddTestBand(band);
  GDALRasterBandH hBand = GDALRasterBand::ToHandle(band);

  GByte buf[100] = {0};

  // Null hBand
  EXPECT_EQ(
      GDALRasterIO(nullptr, GF_Read, 0, 0, 1, 1, buf, 1, 1, GDT_Byte, 0, 0),
      CE_Failure);
  EXPECT_EQ(GDALRasterIOEx(nullptr, GF_Read, 0, 0, 1, 1, buf, 1, 1, GDT_Byte, 0,
                           0, nullptr),
            CE_Failure);

  // ExtraArg with invalid version
  GDALRasterIOExtraArg extra;
  INIT_RASTERIO_EXTRA_ARG(extra);
  extra.nVersion = RASTERIO_EXTRA_ARG_CURRENT_VERSION + 10;
  EXPECT_EQ(
      band->RasterIO(GF_Read, 0, 0, 2, 2, buf, 2, 2, GDT_Byte, 0, 0, &extra),
      CE_Failure);

  // Null pData
  EXPECT_EQ(band->RasterIO(GF_Read, 0, 0, 2, 2, nullptr, 2, 2, GDT_Byte, 0, 0,
                           nullptr),
            CE_Failure);

  // Zero/negative size -> returns CE_None with warning
  EXPECT_EQ(
      band->RasterIO(GF_Read, 0, 0, 0, 2, buf, 2, 2, GDT_Byte, 0, 0, nullptr),
      CE_None);
  EXPECT_EQ(
      band->RasterIO(GF_Read, 0, 0, 2, 0, buf, 2, 2, GDT_Byte, 0, 0, nullptr),
      CE_None);
  EXPECT_EQ(
      band->RasterIO(GF_Read, 0, 0, 2, 2, buf, 0, 2, GDT_Byte, 0, 0, nullptr),
      CE_None);
  EXPECT_EQ(
      band->RasterIO(GF_Read, 0, 0, 2, 2, buf, 2, 0, GDT_Byte, 0, 0, nullptr),
      CE_None);

  // Out of bounds window
  EXPECT_EQ(
      band->RasterIO(GF_Read, -1, 0, 2, 2, buf, 2, 2, GDT_Byte, 0, 0, nullptr),
      CE_Failure);
  EXPECT_EQ(
      band->RasterIO(GF_Read, 0, -1, 2, 2, buf, 2, 2, GDT_Byte, 0, 0, nullptr),
      CE_Failure);
  EXPECT_EQ(
      band->RasterIO(GF_Read, 9, 0, 2, 2, buf, 2, 2, GDT_Byte, 0, 0, nullptr),
      CE_Failure);
  EXPECT_EQ(
      band->RasterIO(GF_Read, 0, 9, 2, 2, buf, 2, 2, GDT_Byte, 0, 0, nullptr),
      CE_Failure);

  // Invalid eBufType
  EXPECT_EQ(band->RasterIO(GF_Read, 0, 0, 2, 2, buf, 2, 2, GDT_Unknown, 0, 0,
                           nullptr),
            CE_Failure);
  EXPECT_EQ(band->RasterIO(GF_Read, 0, 0, 2, 2, buf, 2, 2, GDT_TypeCount, 0, 0,
                           nullptr),
            CE_Failure);

  // Valid write and read via GDALRasterIO and GDALRasterIOEx
  buf[0] = 42;
  buf[1] = 43;
  INIT_RASTERIO_EXTRA_ARG(extra);
  EXPECT_EQ(
      GDALRasterIO(hBand, GF_Write, 0, 0, 2, 1, buf, 2, 1, GDT_Byte, 0, 0),
      CE_None);
  GByte read_buf[2] = {0};
  EXPECT_EQ(GDALRasterIOEx(hBand, GF_Read, 0, 0, 2, 1, read_buf, 2, 1, GDT_Byte,
                           1, 2, &extra),
            CE_None);
  EXPECT_EQ(read_buf[0], 42);
  EXPECT_EQ(read_buf[1], 43);

  // bForceCachedIO = TRUE path
  auto* cached_band =
      new TestRasterBand(ds.get(), 2, GDT_Byte, 10, 10, 5, 5, TRUE);
  ds->AddTestBand(cached_band);
  EXPECT_EQ(cached_band->RasterIO(GF_Write, 0, 0, 2, 1, buf, 2, 1, GDT_Byte, 0,
                                  0, nullptr),
            CE_None);
  read_buf[0] = 0;
  EXPECT_EQ(cached_band->RasterIO(GF_Read, 0, 0, 2, 1, read_buf, 2, 1, GDT_Byte,
                                  0, 0, nullptr),
            CE_None);
  EXPECT_EQ(read_buf[0], 42);

  // Read-only write failure
  band->SetAccessMode(GA_ReadOnly);
  EXPECT_EQ(
      band->RasterIO(GF_Write, 0, 0, 2, 1, buf, 2, 1, GDT_Byte, 0, 0, nullptr),
      CE_Failure);
  // Also with GMO_IGNORE_UNIMPLEMENTED
  band->SetMOFlags(band->GetMOFlags() | GMO_IGNORE_UNIMPLEMENTED);
  EXPECT_EQ(
      band->RasterIO(GF_Write, 0, 0, 2, 1, buf, 2, 1, GDT_Byte, 0, 0, nullptr),
      CE_Failure);
  band->SetMOFlags(0);
  band->SetAccessMode(GA_Update);

  // eFlushBlockErr != CE_None on GF_Write
  band->fail_write_ = true;
  GDALRasterBlock* blk = band->GetLockedBlockRef(0, 0, TRUE);
  ASSERT_NE(blk, nullptr);
  blk->MarkDirty();
  blk->DropLock();
  GDALRasterBlock::FlushDirtyBlocks();
  EXPECT_EQ(
      band->RasterIO(GF_Write, 0, 0, 2, 1, buf, 2, 1, GDT_Byte, 0, 0, nullptr),
      CE_Failure);
  band->fail_write_ = false;

  CPLPopErrorHandler();
}

TEST(GDALRasterBandTest, ReadRasterAllTypesAndEdgeCases) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  auto ds = std::make_unique<TestDataset>(4, 4, GA_Update);
  auto* band = new TestRasterBand(ds.get(), 1, GDT_Byte, 4, 4, 4, 4);
  ds->AddTestBand(band);
  band->Fill(7);

  auto test_type = [&](auto dummy) {
    using T = decltype(dummy);
    std::vector<T> vec;
    EXPECT_EQ(band->ReadRaster(vec), CE_None);
    EXPECT_EQ(vec.size(), 16U);

    T arr[16];
    EXPECT_EQ(band->ReadRaster(arr, 16), CE_None);

    std::span<T> sp(arr, 16);
    EXPECT_EQ(band->ReadRaster(sp), CE_None);
  };

  test_type(uint8_t{0});
  test_type(int8_t{0});
  test_type(uint16_t{0});
  test_type(int16_t{0});
  test_type(uint32_t{0});
  test_type(int32_t{0});
  test_type(uint64_t{0});
  test_type(int64_t{0});
  test_type(GFloat16{0.0f});
  test_type(float{0.0f});
  test_type(double{0.0});
  test_type(std::complex<float>{0.0f, 0.0f});
  test_type(std::complex<double>{0.0, 0.0});

  // Edge cases for pointer overload
  uint8_t buf[16] = {0};
  const size_t huge = static_cast<size_t>(1) << 32;
  EXPECT_EQ(band->ReadRaster(buf, 16, 0, 0, 2, 2, huge, 2), CE_Failure);
  EXPECT_EQ(band->ReadRaster(buf, 16, -1.0, 0, 2, 2, 2, 2), CE_Failure);
  EXPECT_EQ(band->ReadRaster(buf, 16, 0, -1.0, 2, 2, 2, 2), CE_Failure);
  EXPECT_EQ(band->ReadRaster(buf, 16, 0, 0, 0.0, 2, 2, 2), CE_None);
  EXPECT_EQ(band->ReadRaster(buf, 16, 0, 0, 2, 0.0, 2, 2), CE_None);
  EXPECT_EQ(band->ReadRaster(buf, 16, 0, 0, 1.5, 2.0, 0, 0), CE_Failure);
  EXPECT_EQ(band->ReadRaster(buf, 16, 0, 0, 2.0, 1.5, 0, 0), CE_Failure);
  EXPECT_EQ(band->ReadRaster(buf, 16, 0, 0, 2, 2, 2, 0), CE_None);
  EXPECT_EQ(band->ReadRaster(buf, 2, 0, 0, 4, 4, 4, 4), CE_Failure);
  // Sub-pixel window with explicit nBufXSize/nBufYSize
  EXPECT_EQ(band->ReadRaster(buf, 4, 0.25, 0.25, 1.5, 1.5, 2, 2), CE_None);

  // Edge cases for vector overload
  std::vector<uint8_t> vec;
  EXPECT_EQ(band->ReadRaster(vec, 0, 0, 2, 2, huge, 2), CE_Failure);
  EXPECT_EQ(band->ReadRaster(vec, -1.0, 0, 2, 2, 2, 2), CE_Failure);
  EXPECT_EQ(band->ReadRaster(vec, 0, -1.0, 2, 2, 2, 2), CE_Failure);
  EXPECT_EQ(band->ReadRaster(vec, 0, 0, 0.0, 2, 2, 2), CE_None);
  EXPECT_EQ(band->ReadRaster(vec, 0, 0, 2, 0.0, 2, 2), CE_None);
  EXPECT_EQ(band->ReadRaster(vec, 0, 0, 1.5, 2.0, 0, 0), CE_Failure);
  EXPECT_EQ(band->ReadRaster(vec, 0, 0, 2.0, 1.5, 0, 0), CE_Failure);
  EXPECT_EQ(band->ReadRaster(vec, 0, 0, 2, 2, 2, 0), CE_None);
  EXPECT_EQ(band->ReadRaster(vec, 0.25, 0.25, 1.5, 1.5, 2, 2), CE_None);

  CPLPopErrorHandler();
}

TEST(GDALRasterBandTest, BlockIOAndCacheManagement) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // 1. Invalid InitBlockInfo cases
  {
    TestRasterBand bad_band;
    bad_band.SetDimsAndType(10, 10, 0, 10, GDT_Byte);
    GByte dummy[10];
    EXPECT_EQ(bad_band.ReadBlock(0, 0, dummy), CE_Failure);
    EXPECT_EQ(bad_band.WriteBlock(0, 0, dummy), CE_Failure);
    EXPECT_EQ(bad_band.GetLockedBlockRef(0, 0), nullptr);
    int bx = 0, by = 0;
    bad_band.GetBlockSize(&bx, &by);
    bad_band.GetBlockSize(nullptr, nullptr);
  }
  {
    TestRasterBand bad_band;
    bad_band.SetDimsAndType(0, 10, 4, 4, GDT_Byte);
    EXPECT_FALSE(bad_band.InitBlockInfo());
  }
  {
    TestRasterBand bad_band;
    bad_band.SetDimsAndType(10, 10, 4, 4, GDT_Unknown);
    EXPECT_FALSE(bad_band.InitBlockInfo());
  }

  // 2. GDAL_BAND_BLOCK_CACHE configurations: HASHSET, ARRAY, AUTO with flag,
  // invalid
  {
    CPLConfigOptionSetter s1("GDAL_BAND_BLOCK_CACHE", "HASHSET", false);
    TestRasterBand b;
    EXPECT_TRUE(b.InitBlockInfo());
    EXPECT_TRUE(b.InitBlockInfo());  // already initialized
  }
  {
    CPLConfigOptionSetter s1("GDAL_BAND_BLOCK_CACHE", "ARRAY", false);
    TestRasterBand b;
    EXPECT_TRUE(b.InitBlockInfo());
  }
  {
    CPLConfigOptionSetter s1("GDAL_BAND_BLOCK_CACHE", "INVALID_TYPE", false);
    TestRasterBand b;
    EXPECT_TRUE(b.InitBlockInfo());
  }
  {
    TestDataset ds(16, 16, GA_Update);
    ds.SetOpenFlags(GDAL_OF_HASHSET_BLOCK_ACCESS);
    auto* b = new TestRasterBand(&ds, 1, GDT_Byte, 16, 16, 4, 4);
    ds.AddTestBand(b);
    EXPECT_TRUE(b->InitBlockInfo());
  }
  {
    // Many blocks (> 1000000) triggers HashSet in AUTO mode
    TestRasterBand b;
    b.SetDimsAndType(2000, 2000, 1, 1, GDT_Byte);
    b.storage_.clear();
    EXPECT_TRUE(b.InitBlockInfo());
  }

  // 3. ReadBlock / WriteBlock / GetActualBlockSize /
  // GetSuggestedBlockAccessPattern
  auto ds = std::make_unique<TestDataset>(10, 10, GA_Update);
  auto* band = new TestRasterBand(ds.get(), 1, GDT_Byte, 10, 10, 4, 4);
  ds->AddTestBand(band);
  GDALRasterBandH hBand = GDALRasterBand::ToHandle(band);

  EXPECT_EQ(band->GetSuggestedBlockAccessPattern(), GSBAP_UNKNOWN);
  EXPECT_EQ(GDALGetRasterDataType(nullptr), GDT_Unknown);
  EXPECT_EQ(GDALGetRasterDataType(hBand), GDT_Byte);
  GDALGetBlockSize(nullptr, nullptr, nullptr);
  int bx = 0, by = 0;
  GDALGetBlockSize(hBand, &bx, &by);
  EXPECT_EQ(bx, 4);
  EXPECT_EQ(by, 4);

  GByte block_buf[16] = {1, 2, 3, 4};
  EXPECT_EQ(GDALReadBlock(nullptr, 0, 0, block_buf), CE_Failure);
  EXPECT_EQ(band->ReadBlock(-1, 0, block_buf), CE_Failure);
  EXPECT_EQ(band->ReadBlock(0, -1, block_buf), CE_Failure);
  EXPECT_EQ(band->ReadBlock(10, 0, block_buf), CE_Failure);
  EXPECT_EQ(band->ReadBlock(0, 10, block_buf), CE_Failure);
  EXPECT_EQ(GDALReadBlock(hBand, 0, 0, block_buf), CE_None);

  // Base IWriteBlock (not supported vs GMO_IGNORE_UNIMPLEMENTED)
  band->use_base_iwriteblock_ = true;
  EXPECT_EQ(band->WriteBlock(0, 0, block_buf), CE_Failure);
  band->SetMOFlags(GMO_IGNORE_UNIMPLEMENTED);
  EXPECT_EQ(band->WriteBlock(0, 0, block_buf), CE_Failure);
  band->SetMOFlags(0);
  band->use_base_iwriteblock_ = false;

  // WriteBlock error cases and valid case
  EXPECT_EQ(GDALWriteBlock(nullptr, 0, 0, block_buf), CE_Failure);
  EXPECT_EQ(band->WriteBlock(-1, 0, block_buf), CE_Failure);
  EXPECT_EQ(band->WriteBlock(0, -1, block_buf), CE_Failure);
  EXPECT_EQ(band->WriteBlock(10, 0, block_buf), CE_Failure);
  EXPECT_EQ(band->WriteBlock(0, 10, block_buf), CE_Failure);
  band->SetAccessMode(GA_ReadOnly);
  EXPECT_EQ(band->WriteBlock(0, 0, block_buf), CE_Failure);
  band->SetAccessMode(GA_Update);
  block_buf[0] = 99;
  EXPECT_EQ(GDALWriteBlock(hBand, 0, 0, block_buf), CE_None);

  // GetActualBlockSize
  int xvalid = 0, yvalid = 0;
  EXPECT_EQ(GDALGetActualBlockSize(nullptr, 0, 0, &xvalid, &yvalid),
            CE_Failure);
  EXPECT_EQ(band->GetActualBlockSize(-1, 0, &xvalid, &yvalid), CE_Failure);
  EXPECT_EQ(band->GetActualBlockSize(0, -1, &xvalid, &yvalid), CE_Failure);
  EXPECT_EQ(band->GetActualBlockSize(3, 0, &xvalid, &yvalid), CE_Failure);
  EXPECT_EQ(band->GetActualBlockSize(0, 3, &xvalid, &yvalid), CE_Failure);
  EXPECT_EQ(GDALGetActualBlockSize(hBand, 0, 0, &xvalid, &yvalid), CE_None);
  EXPECT_EQ(xvalid, 4);
  EXPECT_EQ(yvalid, 4);
  // Partial block at edge (block 2 covers pixels 8..9 -> 2 valid pixels)
  EXPECT_EQ(band->GetActualBlockSize(2, 2, &xvalid, &yvalid), CE_None);
  EXPECT_EQ(xvalid, 2);
  EXPECT_EQ(yvalid, 2);

  // 4. TryGetLockedBlockRef, GetLockedBlockRef, FlushBlock, FlushCache,
  // DropCache
  TestRasterBand uninit_band;
  EXPECT_EQ(uninit_band.TryGetLockedBlockRef(0, 0), nullptr);
  EXPECT_EQ(uninit_band.FlushBlock(0, 0), CE_Failure);
  EXPECT_EQ(uninit_band.DropCache(), CE_None);

  EXPECT_EQ(band->TryGetLockedBlockRef(-1, 0), nullptr);
  EXPECT_EQ(band->TryGetLockedBlockRef(0, -1), nullptr);
  EXPECT_EQ(band->TryGetLockedBlockRef(10, 0), nullptr);
  EXPECT_EQ(band->TryGetLockedBlockRef(0, 10), nullptr);
  EXPECT_EQ(band->TryGetLockedBlockRef(0, 0), nullptr);

  EXPECT_EQ(band->GetLockedBlockRef(-1, 0), nullptr);
  EXPECT_EQ(band->GetLockedBlockRef(0, -1), nullptr);
  EXPECT_EQ(band->GetLockedBlockRef(10, 0), nullptr);
  EXPECT_EQ(band->GetLockedBlockRef(0, 10), nullptr);

  // Cache miss -> loads block
  GDALRasterBlock* blk = band->GetLockedBlockRef(0, 0, FALSE);
  ASSERT_NE(blk, nullptr);
  blk->DropLock();

  // Cache hit via TryGetLockedBlockRef and
  // GetLockedBlockRef(bJustInitialize=TRUE)
  blk = band->TryGetLockedBlockRef(0, 0);
  ASSERT_NE(blk, nullptr);
  blk->DropLock();

  blk = band->GetLockedBlockRef(0, 0, TRUE);
  ASSERT_NE(blk, nullptr);
  blk->MarkDirty();
  EXPECT_TRUE(band->HasDirtyBlocks());
  EXPECT_TRUE(band->HasBlockCache());
  blk->DropLock();

  // Trigger global cache eviction to cover UnreferenceBlock &
  // AddBlockToFreeList
  GDALFlushCacheBlock();

  // FlushBlock edge cases
  EXPECT_EQ(band->FlushBlock(-1, 0), CE_Failure);
  EXPECT_EQ(band->FlushBlock(0, -1), CE_Failure);
  EXPECT_EQ(band->FlushBlock(10, 0), CE_Failure);
  EXPECT_EQ(band->FlushBlock(0, 10), CE_Failure);
  EXPECT_EQ(band->FlushBlock(0, 0), CE_None);

  // Thrashing warning check in GetLockedBlockRef when nBlockReads ==
  // nBlocksPerRow * nBlocksPerColumn + 1
  band->SetBlockReads(3 * 3);
  blk = band->GetLockedBlockRef(1, 1, FALSE);
  ASSERT_NE(blk, nullptr);
  blk->DropLock();

  // IReadBlock failure in GetLockedBlockRef (without and with CPLError)
  band->fail_read_ = true;
  band->emit_error_on_fail_read_ = false;
  EXPECT_EQ(band->GetLockedBlockRef(2, 0, FALSE), nullptr);
  band->emit_error_on_fail_read_ = true;
  EXPECT_EQ(band->GetLockedBlockRef(2, 1, FALSE), nullptr);
  band->fail_read_ = false;

  // FlushCache and DropCache C wrappers and error propagation
  EXPECT_EQ(GDALFlushRasterCache(nullptr), CE_Failure);
  EXPECT_EQ(GDALDropRasterCache(nullptr), CE_Failure);
  EXPECT_EQ(GDALFlushRasterCache(hBand), CE_None);
  EXPECT_EQ(GDALDropRasterCache(hBand), CE_None);

  // DropCache when eFlushBlockErr is set
  band->fail_write_ = true;
  blk = band->GetLockedBlockRef(0, 0, TRUE);
  ASSERT_NE(blk, nullptr);
  blk->MarkDirty();
  blk->DropLock();
  EXPECT_EQ(band->FlushBlock(0, 0, TRUE), CE_Failure);
  EXPECT_EQ(band->DropCache(), CE_None);

  CPLPopErrorHandler();
}

TEST(GDALRasterBandTest, FillRasterTests) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  EXPECT_EQ(GDALFillRaster(nullptr, 1.0, 0.0), CE_Failure);

  // Read-only failure
  TestRasterBand ro_band;
  ro_band.SetAccessMode(GA_ReadOnly);
  EXPECT_EQ(ro_band.Fill(5.0), CE_Failure);

  // Invalid block info failure
  TestRasterBand bad_band;
  bad_band.SetDimsAndType(8, 8, 0, 8, GDT_Byte);
  EXPECT_EQ(bad_band.Fill(5.0), CE_Failure);

  // Real band Fill
  TestRasterBand byte_band;
  byte_band.SetDimsAndType(6, 6, 4, 4, GDT_Byte);
  EXPECT_EQ(GDALFillRaster(GDALRasterBand::ToHandle(&byte_band), 123.0, 0.0),
            CE_None);
  GByte v = 0;
  EXPECT_EQ(byte_band.RasterIO(GF_Read, 5, 5, 1, 1, &v, 1, 1, GDT_Byte, 0, 0,
                               nullptr),
            CE_None);
  EXPECT_EQ(v, 123);

  // Complex band Fill
  TestRasterBand cfloat_band;
  cfloat_band.SetDimsAndType(4, 4, 4, 4, GDT_CFloat32);
  EXPECT_EQ(cfloat_band.Fill(3.5, -2.5), CE_None);
  float cv[2] = {0.0f, 0.0f};
  EXPECT_EQ(cfloat_band.RasterIO(GF_Read, 1, 1, 1, 1, cv, 1, 1, GDT_CFloat32, 0,
                                 0, nullptr),
            CE_None);
  EXPECT_FLOAT_EQ(cv[0], 3.5f);
  EXPECT_FLOAT_EQ(cv[1], -2.5f);

  CPLPopErrorHandler();
}

}  // namespace
namespace {

TEST(GDALRasterBandTest, PropertiesMetadataNoDataMinMaxOverviews) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  auto ds = std::make_unique<TestDataset>(16, 16, GA_Update);
  auto* band = new TestRasterBand(ds.get(), 1, GDT_Byte, 16, 16, 8, 8);
  ds->AddTestBand(band);
  GDALRasterBandH hBand = GDALRasterBand::ToHandle(band);

  // Access, XSize, YSize, Band, Dataset
  EXPECT_EQ(GDALGetRasterAccess(nullptr), GA_ReadOnly);
  EXPECT_EQ(GDALGetRasterAccess(hBand), GA_Update);
  EXPECT_EQ(band->GetAccess(), GA_Update);

  EXPECT_EQ(GDALGetRasterBandXSize(nullptr), 0);
  EXPECT_EQ(GDALGetRasterBandXSize(hBand), 16);
  EXPECT_EQ(GDALGetRasterBandYSize(nullptr), 0);
  EXPECT_EQ(GDALGetRasterBandYSize(hBand), 16);
  EXPECT_EQ(GDALGetBandNumber(nullptr), 0);
  EXPECT_EQ(GDALGetBandNumber(hBand), 1);
  EXPECT_EQ(GDALGetBandDataset(nullptr), nullptr);
  EXPECT_EQ(GDALGetBandDataset(hBand), GDALDataset::ToHandle(ds.get()));

  // CategoryNames
  EXPECT_EQ(GDALGetRasterCategoryNames(nullptr), nullptr);
  EXPECT_EQ(GDALGetRasterCategoryNames(hBand), nullptr);
  const char* cats[] = {"cat1", "cat2", nullptr};
  EXPECT_EQ(GDALSetRasterCategoryNames(nullptr, cats), CE_Failure);
  EXPECT_EQ(GDALSetRasterCategoryNames(hBand, cats), CE_Failure);
  band->SetMOFlags(GMO_IGNORE_UNIMPLEMENTED);
  EXPECT_EQ(GDALSetRasterCategoryNames(hBand, cats), CE_Failure);
  band->SetMOFlags(0);

  // NoData getters and setters (base class unimplemented vs
  // GMO_IGNORE_UNIMPLEMENTED)
  int success = 1;
  EXPECT_DOUBLE_EQ(GDALGetRasterNoDataValue(nullptr, &success), 0.0);
  EXPECT_DOUBLE_EQ(GDALGetRasterNoDataValue(hBand, &success), -1e10);
  EXPECT_EQ(success, FALSE);
  EXPECT_DOUBLE_EQ(band->GetNoDataValue(nullptr), -1e10);

  success = 1;
  EXPECT_EQ(GDALGetRasterNoDataValueAsInt64(nullptr, &success),
            std::numeric_limits<int64_t>::min());
  EXPECT_EQ(GDALGetRasterNoDataValueAsInt64(hBand, &success),
            std::numeric_limits<int64_t>::min());
  EXPECT_EQ(success, FALSE);

  success = 1;
  EXPECT_EQ(GDALGetRasterNoDataValueAsUInt64(nullptr, &success),
            std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(GDALGetRasterNoDataValueAsUInt64(hBand, &success),
            std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(success, FALSE);

  EXPECT_EQ(GDALSetRasterNoDataValue(nullptr, 0.0), CE_Failure);
  EXPECT_EQ(GDALSetRasterNoDataValue(hBand, 0.0), CE_Failure);
  EXPECT_EQ(GDALSetRasterNoDataValueAsInt64(nullptr, 0), CE_Failure);
  EXPECT_EQ(GDALSetRasterNoDataValueAsInt64(hBand, 0), CE_Failure);
  EXPECT_EQ(GDALSetRasterNoDataValueAsUInt64(nullptr, 0), CE_Failure);
  EXPECT_EQ(GDALSetRasterNoDataValueAsUInt64(hBand, 0), CE_Failure);
  EXPECT_EQ(GDALDeleteRasterNoDataValue(nullptr), CE_Failure);
  EXPECT_EQ(GDALDeleteRasterNoDataValue(hBand), CE_Failure);

  band->SetMOFlags(GMO_IGNORE_UNIMPLEMENTED);
  EXPECT_EQ(GDALSetRasterNoDataValue(hBand, 0.0), CE_Failure);
  EXPECT_EQ(GDALSetRasterNoDataValueAsInt64(hBand, 0), CE_Failure);
  EXPECT_EQ(GDALSetRasterNoDataValueAsUInt64(hBand, 0), CE_Failure);
  EXPECT_EQ(GDALDeleteRasterNoDataValue(hBand), CE_Failure);
  band->SetMOFlags(0);

  // SetNoDataValueAsString across types
  band->allow_set_nodata_ = true;
  bool cannot_rep = false;

  // GDT_Int64
  band->SetDimsAndType(4, 4, 4, 4, GDT_Int64);
  EXPECT_EQ(band->SetNoDataValueAsString("-12345", &cannot_rep), CE_None);
  EXPECT_FALSE(cannot_rep);
  EXPECT_EQ(band->SetNoDataValueAsString("100.0", &cannot_rep), CE_None);
  EXPECT_FALSE(cannot_rep);
  EXPECT_EQ(band->SetNoDataValueAsString("100.5", &cannot_rep), CE_Failure);
  EXPECT_TRUE(cannot_rep);
  EXPECT_EQ(band->SetNoDataValueAsString("99999999999999999999999999999",
                                         &cannot_rep),
            CE_Failure);
  EXPECT_TRUE(cannot_rep);
  EXPECT_EQ(band->SetNoDataValueAsString("not_a_number", &cannot_rep),
            CE_Failure);

  // GDT_UInt64
  band->SetDimsAndType(4, 4, 4, 4, GDT_UInt64);
  EXPECT_EQ(band->SetNoDataValueAsString("12345", &cannot_rep), CE_None);
  EXPECT_FALSE(cannot_rep);
  EXPECT_EQ(band->SetNoDataValueAsString("100.0", &cannot_rep), CE_None);
  EXPECT_FALSE(cannot_rep);
  EXPECT_EQ(band->SetNoDataValueAsString("100.5", &cannot_rep), CE_Failure);
  EXPECT_TRUE(cannot_rep);
  EXPECT_EQ(band->SetNoDataValueAsString("-5", &cannot_rep), CE_None);
  EXPECT_FALSE(cannot_rep);
  EXPECT_EQ(band->SetNoDataValueAsString("99999999999999999999999999999",
                                         &cannot_rep),
            CE_Failure);
  EXPECT_TRUE(cannot_rep);
  EXPECT_EQ(band->SetNoDataValueAsString("not_a_number", &cannot_rep),
            CE_Failure);

  // GDT_Float32
  band->SetDimsAndType(4, 4, 4, 4, GDT_Float32);
  EXPECT_EQ(band->SetNoDataValueAsString("1.5", &cannot_rep), CE_None);
  EXPECT_FALSE(cannot_rep);
  EXPECT_EQ(band->SetNoDataValueAsString("nan", &cannot_rep), CE_None);
  EXPECT_FALSE(cannot_rep);
  EXPECT_EQ(band->SetNoDataValueAsString("invalid_float", &cannot_rep),
            CE_Failure);

  // GDT_Byte and GDT_Float64
  band->SetDimsAndType(4, 4, 4, 4, GDT_Byte);
  EXPECT_EQ(band->SetNoDataValueAsString("200", &cannot_rep), CE_None);
  EXPECT_FALSE(cannot_rep);
  EXPECT_EQ(band->SetNoDataValueAsString("300", &cannot_rep), CE_Failure);
  EXPECT_TRUE(cannot_rep);
  EXPECT_EQ(band->SetNoDataValueAsString("invalid_byte", nullptr), CE_Failure);

  band->SetDimsAndType(4, 4, 4, 4, GDT_Float64);
  EXPECT_EQ(band->SetNoDataValueAsString("123.456", &cannot_rep), CE_None);
  EXPECT_FALSE(cannot_rep);

  // GetMinimum and GetMaximum across all data types
  EXPECT_DOUBLE_EQ(GDALGetRasterMinimum(nullptr, &success), 0.0);
  EXPECT_DOUBLE_EQ(GDALGetRasterMaximum(nullptr, &success), 0.0);

  const GDALDataType all_types[] = {
      GDT_Byte,     GDT_Int8,     GDT_UInt16,  GDT_Int16,    GDT_CInt16,
      GDT_Int32,    GDT_CInt32,   GDT_UInt32,  GDT_Int64,    GDT_UInt64,
      GDT_Float16,  GDT_CFloat16, GDT_Float32, GDT_CFloat32, GDT_Float64,
      GDT_CFloat64, GDT_Unknown};
  for (GDALDataType dt : all_types) {
    band->SetDimsAndType(4, 4, 4, 4, dt);
    int s_min = 1, s_max = 1;
    (void)GDALGetRasterMinimum(hBand, &s_min);
    (void)GDALGetRasterMaximum(hBand, &s_max);
    EXPECT_EQ(s_min, FALSE);
    EXPECT_EQ(s_max, FALSE);
    (void)band->GetMinimum(nullptr);
    (void)band->GetMaximum(nullptr);
  }

  // GDT_Byte with PIXELTYPE=SIGNEDBYTE
  band->SetDimsAndType(4, 4, 4, 4, GDT_Byte);
  band->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE");
  EXPECT_DOUBLE_EQ(band->GetMinimum(&success), -128.0);
  EXPECT_EQ(success, FALSE);
  EXPECT_DOUBLE_EQ(band->GetMaximum(&success), 127.0);
  EXPECT_EQ(success, FALSE);
  band->SetMetadata(nullptr, "IMAGE_STRUCTURE");

  // With STATISTICS_MINIMUM and STATISTICS_MAXIMUM metadata
  band->SetMetadataItem("STATISTICS_MINIMUM", "10.5");
  band->SetMetadataItem("STATISTICS_MAXIMUM", "90.5");
  EXPECT_DOUBLE_EQ(GDALGetRasterMinimum(hBand, &success), 10.5);
  EXPECT_EQ(success, TRUE);
  EXPECT_DOUBLE_EQ(GDALGetRasterMaximum(hBand, &success), 90.5);
  EXPECT_EQ(success, TRUE);
  band->SetMetadata(nullptr);

  // ColorInterpretation & ColorTable
  EXPECT_EQ(GDALGetRasterColorInterpretation(nullptr), GCI_Undefined);
  EXPECT_EQ(GDALGetRasterColorInterpretation(hBand), GCI_Undefined);
  EXPECT_EQ(GDALSetRasterColorInterpretation(nullptr, GCI_RedBand), CE_Failure);
  EXPECT_EQ(GDALSetRasterColorInterpretation(hBand, GCI_RedBand), CE_Failure);

  EXPECT_EQ(GDALGetRasterColorTable(nullptr), nullptr);
  EXPECT_EQ(GDALGetRasterColorTable(hBand), nullptr);
  EXPECT_EQ(GDALSetRasterColorTable(nullptr, nullptr), CE_Failure);
  EXPECT_EQ(GDALSetRasterColorTable(hBand, nullptr), CE_Failure);

  // Offset, Scale, UnitType
  EXPECT_DOUBLE_EQ(GDALGetRasterOffset(nullptr, &success), 0.0);
  EXPECT_DOUBLE_EQ(GDALGetRasterOffset(hBand, &success), 0.0);
  EXPECT_EQ(success, FALSE);
  EXPECT_DOUBLE_EQ(band->GetOffset(nullptr), 0.0);
  EXPECT_EQ(GDALSetRasterOffset(nullptr, 1.0), CE_Failure);
  EXPECT_EQ(GDALSetRasterOffset(hBand, 1.0), CE_Failure);

  EXPECT_DOUBLE_EQ(GDALGetRasterScale(nullptr, &success), 0.0);
  EXPECT_DOUBLE_EQ(GDALGetRasterScale(hBand, &success), 1.0);
  EXPECT_EQ(success, FALSE);
  EXPECT_DOUBLE_EQ(band->GetScale(nullptr), 1.0);
  EXPECT_EQ(GDALSetRasterScale(nullptr, 2.0), CE_Failure);
  EXPECT_EQ(GDALSetRasterScale(hBand, 2.0), CE_Failure);

  EXPECT_EQ(GDALGetRasterUnitType(nullptr), nullptr);
  EXPECT_STREQ(GDALGetRasterUnitType(hBand), "");
  EXPECT_EQ(GDALSetRasterUnitType(nullptr, "m"), CE_Failure);
  EXPECT_EQ(GDALSetRasterUnitType(hBand, "m"), CE_Failure);

  band->SetMOFlags(GMO_IGNORE_UNIMPLEMENTED);
  EXPECT_EQ(GDALSetRasterColorInterpretation(hBand, GCI_RedBand), CE_Failure);
  EXPECT_EQ(GDALSetRasterColorTable(hBand, nullptr), CE_Failure);
  EXPECT_EQ(GDALSetRasterOffset(hBand, 1.0), CE_Failure);
  EXPECT_EQ(GDALSetRasterScale(hBand, 2.0), CE_Failure);
  EXPECT_EQ(GDALSetRasterUnitType(hBand, "m"), CE_Failure);
  band->SetMOFlags(0);

  // Overviews & GetRasterSampleOverview
  EXPECT_EQ(GDALHasArbitraryOverviews(nullptr), 0);
  EXPECT_EQ(GDALHasArbitraryOverviews(hBand), 0);
  EXPECT_EQ(GDALGetOverviewCount(nullptr), 0);
  EXPECT_EQ(GDALGetOverviewCount(hBand), 0);
  EXPECT_EQ(GDALGetOverview(nullptr, 0), nullptr);
  EXPECT_EQ(GDALGetOverview(hBand, 0), nullptr);
  EXPECT_EQ(GDALGetRasterSampleOverview(nullptr, 100), nullptr);
  EXPECT_EQ(GDALGetRasterSampleOverviewEx(nullptr, 100), nullptr);
  EXPECT_EQ(GDALGetRasterSampleOverview(hBand, -1), hBand);
  EXPECT_EQ(GDALGetRasterSampleOverviewEx(hBand, 0), hBand);

  // Add synthetic overviews to test GetRasterSampleOverview selection logic
  band->SetDimsAndType(100, 100, 10, 10, GDT_Byte);
  TestRasterBand ovr1(nullptr, 1, GDT_Byte, 50, 50, 10, 10);
  TestRasterBand ovr2(nullptr, 1, GDT_Byte, 20, 20, 10, 10);
  band->overviews_ = {&ovr1, &ovr2};
  EXPECT_EQ(GDALGetOverviewCount(hBand), 2);
  EXPECT_EQ(GDALGetOverview(hBand, -1), nullptr);
  EXPECT_EQ(GDALGetOverview(hBand, 2), nullptr);
  EXPECT_EQ(GDALGetOverview(hBand, 0), GDALRasterBand::ToHandle(&ovr1));
  EXPECT_EQ(GDALGetRasterSampleOverview(hBand, 1000),
            GDALRasterBand::ToHandle(&ovr1));
  EXPECT_EQ(GDALGetRasterSampleOverviewEx(hBand, 100),
            GDALRasterBand::ToHandle(&ovr2));
  EXPECT_EQ(GDALGetRasterSampleOverviewEx(hBand, 9000), hBand);
  band->overviews_.clear();

  // BuildOverviews on base band
  int ovr_list[1] = {2};
  EXPECT_EQ(
      band->BuildOverviews("NEAREST", 1, ovr_list, nullptr, nullptr, nullptr),
      CE_Failure);

  // AdviseRead
  EXPECT_EQ(GDALRasterAdviseRead(nullptr, 0, 0, 4, 4, 4, 4, GDT_Byte, nullptr),
            CE_Failure);
  EXPECT_EQ(GDALRasterAdviseRead(hBand, 0, 0, 4, 4, 4, 4, GDT_Byte, nullptr),
            CE_None);

  CPLPopErrorHandler();
}

}  // namespace
namespace {

int CPL_STDCALL CancelAtStartProgress(double, const char*, void*) {
  return FALSE;
}

int CPL_STDCALL CancelMidwayProgress(double dfComplete, const char*,
                                     void* pData) {
  int* calls = static_cast<int*>(pData);
  (*calls)++;
  if (*calls > 1 || dfComplete > 0.0) return FALSE;
  return TRUE;
}

TEST(GDALRasterBandTest, HistogramAndDefaultHistogramAllPaths) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  auto ds = std::make_unique<TestDataset>(8, 8, GA_Update);
  auto* band = new TestRasterBand(ds.get(), 1, GDT_Byte, 8, 8, 4, 4);
  ds->AddTestBand(band);
  GDALRasterBandH hBand = GDALRasterBand::ToHandle(band);

  GUIntBig hist[256] = {0};

  // Null checks
  EXPECT_EQ(GDALGetRasterHistogramEx(nullptr, 0, 255, 256, hist, TRUE, FALSE,
                                     nullptr, nullptr),
            CE_Failure);
  EXPECT_EQ(GDALGetRasterHistogramEx(hBand, 0, 255, 256, nullptr, TRUE, FALSE,
                                     nullptr, nullptr),
            CE_Failure);
  EXPECT_EQ(GDALGetRasterHistogramEx(hBand, 0, 255, 0, hist, TRUE, FALSE,
                                     nullptr, nullptr),
            CE_Failure);

  // Invalid dfMin >= dfMax or non-finite dfScale
  EXPECT_EQ(
      band->GetHistogram(10.0, 5.0, 10, hist, TRUE, FALSE, nullptr, nullptr),
      CE_Failure);
  EXPECT_EQ(band->GetHistogram(-1e308, 1e308, 10, hist, TRUE, FALSE, nullptr,
                               nullptr),
            CE_Failure);

  // Progress cancel at start and midway
  EXPECT_EQ(band->GetHistogram(-0.5, 255.5, 256, hist, TRUE, FALSE,
                               CancelAtStartProgress, nullptr),
            CE_Failure);
  int calls = 0;
  EXPECT_EQ(band->GetHistogram(-0.5, 255.5, 10, hist, TRUE, FALSE,
                               CancelMidwayProgress, &calls),
            CE_Failure);

  // Overview delegation when bApproxOK=TRUE and GetOverviewCount() > 0
  TestRasterBand ovr(nullptr, 1, GDT_Byte, 4, 4, 4, 4);
  ovr.Fill(10);
  band->overviews_ = {&ovr};
  EXPECT_EQ(
      band->GetHistogram(-0.5, 255.5, 256, hist, TRUE, TRUE, nullptr, nullptr),
      CE_None);
  EXPECT_EQ(hist[10], 16U);
  band->overviews_.clear();

  // Fast path: GDT_Byte, !bSignedByte, dfScale == 1.0, dfMin == -0.5, nBuckets
  // == 256
  for (int i = 0; i < 64; ++i) {
    band->storage_[i] = static_cast<GByte>(i * 4);
  }
  band->FlushCache();
  band->has_nodata_ = true;
  band->nodata_val_ = 0.0;
  EXPECT_EQ(
      band->GetHistogram(-0.5, 255.5, 256, hist, TRUE, FALSE, nullptr, nullptr),
      CE_None);
  EXPECT_EQ(hist[0], 0U);
  EXPECT_EQ(hist[4], 1U);

  // Fast path with custom mask band
  TestRasterBand mask_band(ds.get(), 1, GDT_Byte, 8, 8, 4, 4);
  mask_band.Fill(255);
  mask_band.storage_[1] = 0;  // mask out pixel 1
  band->has_nodata_ = false;
  band->custom_mask_band_ = &mask_band;
  band->custom_mask_flags_ = GMF_PER_DATASET;
  EXPECT_EQ(
      band->GetHistogram(-0.5, 255.5, 256, hist, TRUE, FALSE, nullptr, nullptr),
      CE_None);
  EXPECT_EQ(hist[4], 1U);

  // Helper to populate diverse values (below min, in range, above max, nodata)
  const GDALDataType hist_types[] = {
      GDT_Byte,    GDT_Int8,     GDT_UInt16,   GDT_Int16,
      GDT_UInt32,  GDT_Int32,    GDT_UInt64,   GDT_Int64,
      GDT_Float16, GDT_Float32,  GDT_Float64,  GDT_CInt16,
      GDT_CInt32,  GDT_CFloat16, GDT_CFloat32, GDT_CFloat64};

  for (GDALDataType dt : hist_types) {
    auto loop_ds = std::make_unique<TestDataset>(8, 8, GA_Update);
    auto* loop_band = new TestRasterBand(loop_ds.get(), 1, dt, 8, 8, 4, 4);
    loop_ds->AddTestBand(loop_band);
    double vals[4] = {1.0, 5.0, 15.0, 99.0};
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 8; ++x) {
        double v = vals[(x + y) % 4];
        EXPECT_EQ(loop_band->RasterIO(GF_Write, x, y, 1, 1, &v, 1, 1,
                                      GDT_Float64, 0, 0, nullptr),
                  CE_None);
      }
    }
    loop_band->FlushCache();

    loop_band->has_nodata_ = true;
    loop_band->nodata_val_ = 99.0;
    loop_band->has_nodata_i64_ = true;
    loop_band->nodata_i64_ = 99;
    loop_band->has_nodata_u64_ = true;
    loop_band->nodata_u64_ = 99U;

    GUIntBig h10[10] = {0};
    // 1. Arbitrary overviews path (bApproxOK=TRUE,
    // HasArbitraryOverviews()=TRUE)
    loop_band->arbitrary_overviews_ = true;
    for (int inc_out : {TRUE, FALSE}) {
      EXPECT_EQ(loop_band->GetHistogram(2.0, 12.0, 10, h10, inc_out, TRUE,
                                        nullptr, nullptr),
                CE_None);
    }

    // 2. General block path (bApproxOK=FALSE)
    loop_band->arbitrary_overviews_ = false;
    for (int inc_out : {TRUE, FALSE}) {
      EXPECT_EQ(loop_band->GetHistogram(2.0, 12.0, 10, h10, inc_out, FALSE,
                                        nullptr, nullptr),
                CE_None);
    }

    // Also GDT_Byte with PIXELTYPE=SIGNEDBYTE
    if (dt == GDT_Byte) {
      loop_band->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE");
      loop_band->arbitrary_overviews_ = true;
      EXPECT_EQ(loop_band->GetHistogram(-10.0, 20.0, 10, h10, TRUE, TRUE,
                                        nullptr, nullptr),
                CE_None);
      loop_band->arbitrary_overviews_ = false;
      EXPECT_EQ(loop_band->GetHistogram(-10.0, 20.0, 10, h10, TRUE, FALSE,
                                        nullptr, nullptr),
                CE_None);
    }
  }

  // Deprecated GDALGetRasterHistogram wrapper
  TestRasterBand dep_band(nullptr, 1, GDT_Byte, 4, 4, 4, 4);
  dep_band.Fill(5);
  int int_hist[10] = {0};
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
  EXPECT_EQ(GDALGetRasterHistogram(nullptr, 0, 10, 10, int_hist, TRUE, FALSE,
                                   nullptr, nullptr),
            CE_Failure);
  EXPECT_EQ(GDALGetRasterHistogram(GDALRasterBand::ToHandle(&dep_band), 0, 10,
                                   10, nullptr, TRUE, FALSE, nullptr, nullptr),
            CE_Failure);
  EXPECT_EQ(GDALGetRasterHistogram(GDALRasterBand::ToHandle(&dep_band), 0, 10,
                                   10, int_hist, TRUE, FALSE, nullptr, nullptr),
            CE_None);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

  // GetDefaultHistogram & GDALGetDefaultHistogramEx & GDALGetDefaultHistogram
  double hmin = 0.0, hmax = 0.0;
  int nbuckets = 0;
  GUIntBig* def_hist = nullptr;

  EXPECT_EQ(GDALGetDefaultHistogramEx(nullptr, &hmin, &hmax, &nbuckets,
                                      &def_hist, TRUE, nullptr, nullptr),
            CE_Failure);
  // bForce = FALSE -> CE_Warning
  EXPECT_EQ(GDALGetDefaultHistogramEx(hBand, &hmin, &hmax, &nbuckets, &def_hist,
                                      FALSE, nullptr, nullptr),
            CE_Warning);

  // bForce = TRUE on GDT_Byte
  EXPECT_EQ(GDALGetDefaultHistogramEx(hBand, &hmin, &hmax, &nbuckets, &def_hist,
                                      TRUE, nullptr, nullptr),
            CE_None);
  EXPECT_EQ(nbuckets, 256);
  VSIFree(def_hist);
  def_hist = nullptr;

  // bForce = TRUE on GDT_Int8
  band->SetDimsAndType(4, 4, 4, 4, GDT_Int8);
  band->Fill(-5);
  EXPECT_EQ(band->GetDefaultHistogram(&hmin, &hmax, &nbuckets, &def_hist, TRUE,
                                      nullptr, nullptr),
            CE_None);
  EXPECT_EQ(nbuckets, 256);
  VSIFree(def_hist);
  def_hist = nullptr;

  // bForce = TRUE on GDT_Float32 with constant value (*pdfMin == *pdfMax -> 1
  // bucket)
  band->SetDimsAndType(4, 4, 4, 4, GDT_Float32);
  band->Fill(42.0);
  EXPECT_EQ(band->GetDefaultHistogram(&hmin, &hmax, &nbuckets, &def_hist, TRUE,
                                      nullptr, nullptr),
            CE_None);
  EXPECT_EQ(nbuckets, 1);
  VSIFree(def_hist);
  def_hist = nullptr;

  // bForce = TRUE on GDT_Float32 with varying values (*pdfMin < *pdfMax -> 256
  // buckets)
  float fvals[2] = {10.0f, 20.0f};
  EXPECT_EQ(band->RasterIO(GF_Write, 0, 0, 2, 1, fvals, 2, 1, GDT_Float32, 0, 0,
                           nullptr),
            CE_None);
  band->FlushCache();
  int* def_hist_int = nullptr;
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
  EXPECT_EQ(GDALGetDefaultHistogram(nullptr, &hmin, &hmax, &nbuckets,
                                    &def_hist_int, TRUE, nullptr, nullptr),
            CE_Failure);
  EXPECT_EQ(GDALGetDefaultHistogram(hBand, &hmin, &hmax, &nbuckets,
                                    &def_hist_int, TRUE, nullptr, nullptr),
            CE_None);
  VSIFree(def_hist_int);

  // SetDefaultHistogram & C wrappers
  EXPECT_EQ(GDALSetDefaultHistogram(nullptr, 0, 10, 10, int_hist), CE_Failure);
  EXPECT_EQ(GDALSetDefaultHistogram(hBand, 0, 10, 10, int_hist), CE_Failure);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
  EXPECT_EQ(GDALSetDefaultHistogramEx(nullptr, 0, 10, 10, hist), CE_Failure);
  EXPECT_EQ(GDALSetDefaultHistogramEx(hBand, 0, 10, 10, hist), CE_Failure);
  band->SetMOFlags(GMO_IGNORE_UNIMPLEMENTED);
  EXPECT_EQ(GDALSetDefaultHistogramEx(hBand, 0, 10, 10, hist), CE_Failure);
  band->SetMOFlags(0);

  CPLPopErrorHandler();
}

}  // namespace
namespace {

TEST(GDALRasterBandTest, StatisticsAllPaths) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  auto ds = std::make_unique<TestDataset>(40, 4, GA_Update);
  auto* band = new TestRasterBand(ds.get(), 1, GDT_Byte, 40, 4, 32, 2);
  ds->AddTestBand(band);
  GDALRasterBandH hBand = GDALRasterBand::ToHandle(band);

  double smin = 0, smax = 0, smean = 0, sstddev = 0;

  // Null checks
  EXPECT_EQ(GDALGetRasterStatistics(nullptr, TRUE, TRUE, &smin, &smax, &smean,
                                    &sstddev),
            CE_Failure);
  EXPECT_EQ(GDALComputeRasterStatistics(nullptr, TRUE, &smin, &smax, &smean,
                                        &sstddev, nullptr, nullptr),
            CE_Failure);
  EXPECT_EQ(GDALSetRasterStatistics(nullptr, 1, 2, 1.5, 0.5), CE_Failure);

  // SetValidPercent edge cases
  band->SetValidPercent(100, 0);
  EXPECT_STREQ(band->GetMetadataItem("STATISTICS_VALID_PERCENT"), "0");
  band->SetValidPercent(100000, 99999);
  EXPECT_STREQ(band->GetMetadataItem("STATISTICS_VALID_PERCENT"), "99.999");
  band->SetValidPercent(100, 50);
  EXPECT_STREQ(band->GetMetadataItem("STATISTICS_VALID_PERCENT"), "50");
  band->SetValidPercent(100, 100);
  EXPECT_STREQ(band->GetMetadataItem("STATISTICS_VALID_PERCENT"), "100");
  band->SetMetadata(nullptr);

  // GetStatistics with SetStatistics metadata
  EXPECT_EQ(GDALSetRasterStatistics(hBand, 1.0, 9.0, 5.0, 2.0), CE_None);
  EXPECT_EQ(GDALGetRasterStatistics(hBand, FALSE, FALSE, &smin, &smax, &smean,
                                    &sstddev),
            CE_None);
  EXPECT_DOUBLE_EQ(smin, 1.0);
  EXPECT_DOUBLE_EQ(smax, 9.0);
  EXPECT_DOUBLE_EQ(smean, 5.0);
  EXPECT_DOUBLE_EQ(sstddev, 2.0);

  // With STATISTICS_APPROXIMATE=YES and bApproxOK=FALSE vs TRUE
  band->SetMetadataItem("STATISTICS_APPROXIMATE", "YES");
  EXPECT_EQ(band->GetStatistics(TRUE, FALSE, &smin, &smax, &smean, &sstddev),
            CE_None);
  EXPECT_EQ(band->GetStatistics(FALSE, FALSE, &smin, &smax, &smean, &sstddev),
            CE_Warning);
  band->SetMetadata(nullptr);

  // GetStatistics with bApproxOK=TRUE, pdfMean=nullptr, padfStdDev=nullptr and
  // custom min/max
  band->has_custom_min_ = true;
  band->custom_min_ = 3.0;
  band->has_custom_max_ = true;
  band->custom_max_ = 7.0;
  EXPECT_EQ(band->GetStatistics(TRUE, FALSE, &smin, &smax, nullptr, nullptr),
            CE_None);
  EXPECT_DOUBLE_EQ(smin, 3.0);
  EXPECT_DOUBLE_EQ(smax, 7.0);
  band->has_custom_min_ = false;
  band->has_custom_max_ = false;

  // GetStatistics with bForce=FALSE -> CE_Warning, bForce=TRUE -> computes
  EXPECT_EQ(band->GetStatistics(FALSE, FALSE, &smin, &smax, &smean, &sstddev),
            CE_Warning);
  band->Fill(10);
  EXPECT_EQ(band->GetStatistics(FALSE, TRUE, &smin, &smax, &smean, &sstddev),
            CE_None);
  EXPECT_DOUBLE_EQ(smin, 10.0);

  // Overview delegation in ComputeStatistics
  {
    auto ds_ovr = std::make_unique<TestDataset>(200, 200, GA_Update);
    auto* b_ovr_main =
        new TestRasterBand(ds_ovr.get(), 1, GDT_Byte, 200, 200, 50, 50);
    ds_ovr->AddTestBand(b_ovr_main);
    TestRasterBand ovr(nullptr, 1, GDT_Byte, 60, 60, 60, 60);
    std::fill(ovr.storage_.begin(), ovr.storage_.end(), 25);
    b_ovr_main->overviews_ = {&ovr};
    EXPECT_EQ(b_ovr_main->ComputeStatistics(TRUE, &smin, &smax, &smean,
                                            &sstddev, nullptr, nullptr),
              CE_None);
    EXPECT_DOUBLE_EQ(smin, 25.0);
    b_ovr_main->overviews_.clear();
  }

  // Progress cancellation
  EXPECT_EQ(band->ComputeStatistics(FALSE, &smin, &smax, &smean, &sstddev,
                                    CancelAtStartProgress, nullptr),
            CE_Failure);
  int calls = 0;
  EXPECT_EQ(band->ComputeStatistics(FALSE, &smin, &smax, &smean, &sstddev,
                                    CancelMidwayProgress, &calls),
            CE_Failure);

  // Fast GDT_Byte integer stats paths:
  // 1) Block 0 has [0, 255] -> Block 1 hits nMin==0 && nMax==255
  band->DropCache();
  band->SetMetadata(nullptr);
  std::fill(band->storage_.begin(), band->storage_.end(), 50);
  band->storage_[0] = 0;
  band->storage_[1] = 255;
  EXPECT_EQ(band->ComputeStatistics(FALSE, &smin, &smax, &smean, &sstddev,
                                    nullptr, nullptr),
            CE_None);
  EXPECT_DOUBLE_EQ(smin, 0.0);
  EXPECT_DOUBLE_EQ(smax, 255.0);

  // 2) Block 0 has [0, 100] -> Block 1 hits nMin==0 && nMax<255
  band->DropCache();
  band->SetMetadata(nullptr);
  std::fill(band->storage_.begin(), band->storage_.end(), 50);
  band->storage_[0] = 0;
  band->storage_[1] = 100;
  EXPECT_EQ(band->ComputeStatistics(FALSE, &smin, &smax, &smean, &sstddev,
                                    nullptr, nullptr),
            CE_None);

  // 3) Block 0 has [10, 255] -> Block 1 hits nMin>0 && nMax==255
  band->DropCache();
  band->SetMetadata(nullptr);
  std::fill(band->storage_.begin(), band->storage_.end(), 50);
  band->storage_[0] = 10;
  band->storage_[1] = 255;
  EXPECT_EQ(band->ComputeStatistics(FALSE, &smin, &smax, &smean, &sstddev,
                                    nullptr, nullptr),
            CE_None);

  // 4) With nodata on GDT_Byte
  band->DropCache();
  band->SetMetadata(nullptr);
  band->has_nodata_ = true;
  band->nodata_val_ = 50.0;
  EXPECT_EQ(band->ComputeStatistics(FALSE, &smin, &smax, &smean, &sstddev,
                                    nullptr, nullptr),
            CE_None);

  // 5) All nodata on GDT_Byte -> CE_Failure
  band->Fill(50);
  EXPECT_EQ(band->ComputeStatistics(FALSE, &smin, &smax, &smean, &sstddev,
                                    nullptr, nullptr),
            CE_Failure);
  band->has_nodata_ = false;

  // 6) GDAL_STATS_USE_INTEGER_STATS = NO on GDT_Byte
  {
    CPLConfigOptionSetter s("GDAL_STATS_USE_INTEGER_STATS", "NO", false);
    band->Fill(20);
    band->storage_[0] = 10;
    band->FlushCache();
    EXPECT_EQ(band->ComputeStatistics(FALSE, &smin, &smax, &smean, &sstddev,
                                      nullptr, nullptr),
              CE_None);
  }

  // Fast GDT_UInt16 integer stats paths (without nodata, with nodata, and with
  // INTEGER_STATS=NO)
  {
    auto ds_u16 = std::make_unique<TestDataset>(40, 4, GA_Update);
    auto* band_u16 =
        new TestRasterBand(ds_u16.get(), 1, GDT_UInt16, 40, 4, 32, 2);
    ds_u16->AddTestBand(band_u16);
    band_u16->Fill(100);
    uint16_t u16_vals[2] = {10, 500};
    EXPECT_EQ(band_u16->RasterIO(GF_Write, 0, 0, 2, 1, u16_vals, 2, 1,
                                 GDT_UInt16, 0, 0, nullptr),
              CE_None);
    band_u16->FlushCache();
    EXPECT_EQ(band_u16->ComputeStatistics(FALSE, &smin, &smax, &smean, &sstddev,
                                          nullptr, nullptr),
              CE_None);
    band_u16->has_nodata_ = true;
    band_u16->nodata_val_ = 100.0;
    EXPECT_EQ(band_u16->ComputeStatistics(FALSE, &smin, &smax, &smean, &sstddev,
                                          nullptr, nullptr),
              CE_None);
    {
      CPLConfigOptionSetter s("GDAL_STATS_USE_INTEGER_STATS", "NO", false);
      EXPECT_EQ(band_u16->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                            &sstddev, nullptr, nullptr),
                CE_None);
    }
  }

  // GDT_Float32 SSE2/AVX2 and generic paths
  {
    auto ds_f32 = std::make_unique<TestDataset>(40, 4, GA_Update);
    auto* band_f32 =
        new TestRasterBand(ds_f32.get(), 1, GDT_Float32, 40, 4, 32, 2);
    ds_f32->AddTestBand(band_f32);
    band_f32->Fill(10.0);
    float f32_vals[4] = {2.0f, 50.0f, std::numeric_limits<float>::quiet_NaN(),
                         10.0f};
    EXPECT_EQ(band_f32->RasterIO(GF_Write, 0, 2, 4, 1, f32_vals, 4, 1,
                                 GDT_Float32, 0, 0, nullptr),
              CE_None);
    band_f32->FlushCache();
    EXPECT_EQ(band_f32->ComputeStatistics(FALSE, &smin, &smax, &smean, &sstddev,
                                          nullptr, nullptr),
              CE_None);
    EXPECT_DOUBLE_EQ(smin, 2.0);
    EXPECT_DOUBLE_EQ(smax, 50.0);

    band_f32->has_nodata_ = true;
    band_f32->nodata_val_ = 10.0;
    EXPECT_EQ(band_f32->ComputeStatistics(FALSE, &smin, &smax, &smean, &sstddev,
                                          nullptr, nullptr),
              CE_None);
    {
      CPLConfigOptionSetter s("GDAL_STATS_USE_FLOAT32_OPTIM", "NO", false);
      EXPECT_EQ(band_f32->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                            &sstddev, nullptr, nullptr),
                CE_None);
    }
  }

  // GDT_Float64 SSE2/AVX2 and generic paths
  {
    auto ds_f64 = std::make_unique<TestDataset>(40, 4, GA_Update);
    auto* band_f64 =
        new TestRasterBand(ds_f64.get(), 1, GDT_Float64, 40, 4, 32, 2);
    ds_f64->AddTestBand(band_f64);
    band_f64->Fill(10.0);
    double f64_vals[4] = {2.0, 50.0, std::numeric_limits<double>::quiet_NaN(),
                          10.0};
    EXPECT_EQ(band_f64->RasterIO(GF_Write, 0, 2, 4, 1, f64_vals, 4, 1,
                                 GDT_Float64, 0, 0, nullptr),
              CE_None);
    band_f64->FlushCache();
    EXPECT_EQ(band_f64->ComputeStatistics(FALSE, &smin, &smax, &smean, &sstddev,
                                          nullptr, nullptr),
              CE_None);
    EXPECT_DOUBLE_EQ(smin, 2.0);
    EXPECT_DOUBLE_EQ(smax, 50.0);

    band_f64->has_nodata_ = true;
    band_f64->nodata_val_ = 10.0;
    EXPECT_EQ(band_f64->ComputeStatistics(FALSE, &smin, &smax, &smean, &sstddev,
                                          nullptr, nullptr),
              CE_None);
    {
      CPLConfigOptionSetter s("GDAL_STATS_USE_FLOAT64_OPTIM", "NO", false);
      EXPECT_EQ(band_f64->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                            &sstddev, nullptr, nullptr),
                CE_None);
    }
  }

  // All 16 data types for both ArbitraryOverviews (bApproxOK=TRUE) and Generic
  // Block path (bApproxOK=FALSE), with mask and nodata
  const GDALDataType all_types[] = {
      GDT_Byte,    GDT_Int8,     GDT_UInt16,   GDT_Int16,
      GDT_UInt32,  GDT_Int32,    GDT_UInt64,   GDT_Int64,
      GDT_Float16, GDT_Float32,  GDT_Float64,  GDT_CInt16,
      GDT_CInt32,  GDT_CFloat16, GDT_CFloat32, GDT_CFloat64};

  for (GDALDataType dt : all_types) {
    auto loop_ds = std::make_unique<TestDataset>(8, 8, GA_Update);
    auto* loop_band = new TestRasterBand(loop_ds.get(), 1, dt, 8, 8, 4, 4);
    loop_ds->AddTestBand(loop_band);
    TestRasterBand mask_band(loop_ds.get(), 2, GDT_Byte, 8, 8, 4, 4);
    mask_band.Fill(255);
    mask_band.storage_[0] = 0;

    double vals[4] = {2.0, 6.0, 12.0, 99.0};
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 8; ++x) {
        double v = vals[(x + y) % 4];
        EXPECT_EQ(loop_band->RasterIO(GF_Write, x, y, 1, 1, &v, 1, 1,
                                      GDT_Float64, 0, 0, nullptr),
                  CE_None);
      }
    }
    loop_band->FlushCache();

    loop_band->has_nodata_ = true;
    loop_band->nodata_val_ = 99.0;
    loop_band->has_nodata_i64_ = true;
    loop_band->nodata_i64_ = 99;
    loop_band->has_nodata_u64_ = true;
    loop_band->nodata_u64_ = 99U;
    loop_band->custom_mask_band_ = &mask_band;
    loop_band->custom_mask_flags_ = GMF_PER_DATASET;

    // Arbitrary overviews path
    loop_band->arbitrary_overviews_ = true;
    EXPECT_EQ(loop_band->ComputeStatistics(TRUE, &smin, &smax, &smean, &sstddev,
                                           nullptr, nullptr),
              CE_None);

    // Block path with mask band
    loop_band->arbitrary_overviews_ = false;
    EXPECT_EQ(loop_band->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                           &sstddev, nullptr, nullptr),
              CE_None);

    // Block path without custom mask band
    loop_band->custom_mask_band_ = nullptr;
    loop_band->custom_mask_flags_ = 0;
    EXPECT_EQ(loop_band->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                           &sstddev, nullptr, nullptr),
              CE_None);

    if (dt == GDT_Byte) {
      loop_band->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE");
      loop_band->arbitrary_overviews_ = true;
      EXPECT_EQ(loop_band->ComputeStatistics(TRUE, &smin, &smax, &smean,
                                             &sstddev, nullptr, nullptr),
                CE_None);
      loop_band->arbitrary_overviews_ = false;
      EXPECT_EQ(loop_band->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                             &sstddev, nullptr, nullptr),
                CE_None);
    }
  }

  CPLPopErrorHandler();
}

}  // namespace
namespace {

TEST(GDALRasterBandTest, ComputeRasterMinMaxAndLocationAllPaths) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  auto ds = std::make_unique<TestDataset>(70, 4, GA_Update);
  auto* band = new TestRasterBand(ds.get(), 1, GDT_Byte, 70, 4, 32, 2);
  ds->AddTestBand(band);
  GDALRasterBandH hBand = GDALRasterBand::ToHandle(band);

  double minmax[2] = {0.0, 0.0};
  EXPECT_EQ(GDALComputeRasterMinMax(nullptr, TRUE, minmax), CE_Failure);

  // 1. Tight GetMinimum / GetMaximum with bApproxOK=TRUE
  band->has_custom_min_ = true;
  band->custom_min_ = 4.0;
  band->has_custom_max_ = true;
  band->custom_max_ = 88.0;
  EXPECT_EQ(GDALComputeRasterMinMax(hBand, TRUE, minmax), CE_None);
  EXPECT_DOUBLE_EQ(minmax[0], 4.0);
  EXPECT_DOUBLE_EQ(minmax[1], 88.0);
  band->has_custom_min_ = false;
  band->has_custom_max_ = false;

  // 2. Overview delegation with bApproxOK=TRUE
  {
    auto ds_ovr = std::make_unique<TestDataset>(200, 200, GA_Update);
    auto* b_ovr_main =
        new TestRasterBand(ds_ovr.get(), 1, GDT_Byte, 200, 200, 50, 50);
    ds_ovr->AddTestBand(b_ovr_main);
    TestRasterBand ovr(nullptr, 1, GDT_Byte, 60, 60, 60, 60);
    std::fill(ovr.storage_.begin(), ovr.storage_.end(), 33);
    b_ovr_main->overviews_ = {&ovr};
    EXPECT_EQ(b_ovr_main->ComputeRasterMinMax(TRUE, minmax), CE_None);
    EXPECT_DOUBLE_EQ(minmax[0], 33.0);
    EXPECT_DOUBLE_EQ(minmax[1], 33.0);
    b_ovr_main->overviews_.clear();
  }

  // 3. Optimized path (GDT_Byte, GDT_UInt16, GDT_Int16) on 70x4 band with 32x2
  // blocks
  for (GDALDataType opt_dt : {GDT_Byte, GDT_UInt16, GDT_Int16}) {
    auto ds_opt = std::make_unique<TestDataset>(70, 4, GA_Update);
    auto* band_opt = new TestRasterBand(ds_opt.get(), 1, opt_dt, 70, 4, 32, 2);
    ds_opt->AddTestBand(band_opt);
    band_opt->Fill(20);
    double v_low = 5.0, v_high = 90.0;
    EXPECT_EQ(band_opt->RasterIO(GF_Write, 0, 0, 1, 1, &v_low, 1, 1,
                                 GDT_Float64, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(band_opt->RasterIO(GF_Write, 68, 3, 1, 1, &v_high, 1, 1,
                                 GDT_Float64, 0, 0, nullptr),
              CE_None);
    band_opt->FlushCache();

    // Without nodata: block path and arbitrary overview path
    band_opt->arbitrary_overviews_ = false;
    EXPECT_EQ(band_opt->ComputeRasterMinMax(FALSE, minmax), CE_None);
    EXPECT_DOUBLE_EQ(minmax[0], 5.0);
    EXPECT_DOUBLE_EQ(minmax[1], 90.0);

    band_opt->arbitrary_overviews_ = true;
    EXPECT_EQ(band_opt->ComputeRasterMinMax(TRUE, minmax), CE_None);

    // With nodata: block path and arbitrary overview path
    band_opt->has_nodata_ = true;
    band_opt->nodata_val_ = 20.0;
    band_opt->arbitrary_overviews_ = false;
    EXPECT_EQ(band_opt->ComputeRasterMinMax(FALSE, minmax), CE_None);
    band_opt->arbitrary_overviews_ = true;
    EXPECT_EQ(band_opt->ComputeRasterMinMax(TRUE, minmax), CE_None);

    // Partial block with nXCheck >= 32 and (nBlockXSize % 32) == 0
    auto ds_part = std::make_unique<TestDataset>(40, 2, GA_Update);
    auto* band_part =
        new TestRasterBand(ds_part.get(), 1, opt_dt, 40, 2, 64, 2);
    ds_part->AddTestBand(band_part);
    band_part->Fill(15);
    EXPECT_EQ(band_part->ComputeRasterMinMax(FALSE, minmax), CE_None);
    band_part->has_nodata_ = true;
    band_part->nodata_val_ = 99.0;
    EXPECT_EQ(band_part->ComputeRasterMinMax(FALSE, minmax), CE_None);
  }

  // 4. Generic path across all 16 data types (both bApproxOK=TRUE with
  // arbitrary_overviews_ and bApproxOK=FALSE, with mask and nodata)
  const GDALDataType all_types[] = {
      GDT_Byte,    GDT_Int8,     GDT_UInt16,   GDT_Int16,
      GDT_UInt32,  GDT_Int32,    GDT_UInt64,   GDT_Int64,
      GDT_Float16, GDT_Float32,  GDT_Float64,  GDT_CInt16,
      GDT_CInt32,  GDT_CFloat16, GDT_CFloat32, GDT_CFloat64};

  for (GDALDataType dt : all_types) {
    auto loop_ds = std::make_unique<TestDataset>(8, 8, GA_Update);
    auto* loop_band = new TestRasterBand(loop_ds.get(), 1, dt, 8, 8, 4, 4);
    loop_ds->AddTestBand(loop_band);
    TestRasterBand mask_band(loop_ds.get(), 2, GDT_Byte, 8, 8, 4, 4);
    mask_band.Fill(255);
    mask_band.storage_[0] = 0;

    double vals[4] = {3.0, 8.0, 19.0, 99.0};
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 8; ++x) {
        double v = vals[(x + y) % 4];
        EXPECT_EQ(loop_band->RasterIO(GF_Write, x, y, 1, 1, &v, 1, 1,
                                      GDT_Float64, 0, 0, nullptr),
                  CE_None);
      }
    }
    loop_band->FlushCache();

    loop_band->has_nodata_ = true;
    loop_band->nodata_val_ = 99.0;
    loop_band->has_nodata_i64_ = true;
    loop_band->nodata_i64_ = 99;
    loop_band->has_nodata_u64_ = true;
    loop_band->nodata_u64_ = 99U;
    loop_band->custom_mask_band_ = &mask_band;
    loop_band->custom_mask_flags_ = GMF_PER_DATASET;

    // Arbitrary overviews generic path (mask forces bUseOptimizedPath=false)
    loop_band->arbitrary_overviews_ = true;
    EXPECT_EQ(loop_band->ComputeRasterMinMax(TRUE, minmax), CE_None);

    // Block generic path with mask
    loop_band->arbitrary_overviews_ = false;
    EXPECT_EQ(loop_band->ComputeRasterMinMax(FALSE, minmax), CE_None);

    // Block generic path without mask
    loop_band->custom_mask_band_ = nullptr;
    loop_band->custom_mask_flags_ = 0;
    EXPECT_EQ(loop_band->ComputeRasterMinMax(FALSE, minmax), CE_None);

    if (dt == GDT_Byte) {
      loop_band->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE");
      loop_band->arbitrary_overviews_ = true;
      EXPECT_EQ(loop_band->ComputeRasterMinMax(TRUE, minmax), CE_None);
      loop_band->arbitrary_overviews_ = false;
      EXPECT_EQ(loop_band->ComputeRasterMinMax(FALSE, minmax), CE_None);
    }
  }

  // All nodata -> CE_Failure
  TestRasterBand all_nd_band(nullptr, 1, GDT_Byte, 4, 4, 4, 4);
  all_nd_band.Fill(99);
  all_nd_band.has_nodata_ = true;
  all_nd_band.nodata_val_ = 99.0;
  EXPECT_EQ(all_nd_band.ComputeRasterMinMax(FALSE, minmax), CE_Failure);

  // 5. ComputeRasterMinMaxLocation & GDALComputeRasterMinMaxLocation
  double loc_min = 0, loc_max = 0;
  int min_x = 0, min_y = 0, max_x = 0, max_y = 0;
  EXPECT_EQ(GDALComputeRasterMinMaxLocation(nullptr, &loc_min, &loc_max, &min_x,
                                            &min_y, &max_x, &max_y),
            CE_Failure);

  // Complex type -> CE_Failure
  TestRasterBand c_band(nullptr, 1, GDT_CFloat32, 4, 4, 4, 4);
  EXPECT_EQ(c_band.ComputeRasterMinMaxLocation(&loc_min, &loc_max, &min_x,
                                               &min_y, &max_x, &max_y),
            CE_Failure);

  // All 11 real types:
  const GDALDataType real_types[] = {
      GDT_Byte,   GDT_Int8,  GDT_UInt16,  GDT_Int16,   GDT_UInt32, GDT_Int32,
      GDT_UInt64, GDT_Int64, GDT_Float16, GDT_Float32, GDT_Float64};

  for (GDALDataType dt : real_types) {
    // (a) Full block
    auto loop_ds = std::make_unique<TestDataset>(8, 8, GA_Update);
    auto* loop_band = new TestRasterBand(loop_ds.get(), 1, dt, 8, 8, 4, 4);
    loop_ds->AddTestBand(loop_band);
    loop_band->Fill(10);
    double v_min = 2.0, v_max = 80.0, v_nd = 99.0;
    EXPECT_EQ(loop_band->RasterIO(GF_Write, 1, 2, 1, 1, &v_min, 1, 1,
                                  GDT_Float64, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(loop_band->RasterIO(GF_Write, 5, 6, 1, 1, &v_max, 1, 1,
                                  GDT_Float64, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(loop_band->RasterIO(GF_Write, 0, 0, 1, 1, &v_nd, 1, 1,
                                  GDT_Float64, 0, 0, nullptr),
              CE_None);
    loop_band->FlushCache();

    loop_band->has_nodata_ = true;
    loop_band->nodata_val_ = 99.0;
    loop_band->has_nodata_i64_ = true;
    loop_band->nodata_i64_ = 99;
    loop_band->has_nodata_u64_ = true;
    loop_band->nodata_u64_ = 99U;

    EXPECT_EQ(GDALComputeRasterMinMaxLocation(
                  GDALRasterBand::ToHandle(loop_band), &loc_min, &loc_max,
                  &min_x, &min_y, &max_x, &max_y),
              CE_None);
    EXPECT_EQ(min_x, 1);
    EXPECT_EQ(min_y, 2);
    EXPECT_EQ(max_x, 5);
    EXPECT_EQ(max_y, 6);

    // Min only
    EXPECT_EQ(loop_band->ComputeRasterMinMaxLocation(&loc_min, nullptr, &min_x,
                                                     &min_y, nullptr, nullptr),
              CE_None);
    // Max only
    EXPECT_EQ(loop_band->ComputeRasterMinMaxLocation(nullptr, &loc_max, nullptr,
                                                     nullptr, &max_x, &max_y),
              CE_None);

    if (dt == GDT_Byte) {
      double v0 = 0.0, v255 = 255.0;
      EXPECT_EQ(loop_band->RasterIO(GF_Write, 0, 1, 1, 1, &v0, 1, 1,
                                    GDT_Float64, 0, 0, nullptr),
                CE_None);
      EXPECT_EQ(loop_band->RasterIO(GF_Write, 1, 1, 1, 1, &v255, 1, 1,
                                    GDT_Float64, 0, 0, nullptr),
                CE_None);
      loop_band->FlushCache();
      EXPECT_EQ(loop_band->ComputeRasterMinMaxLocation(
                    &loc_min, &loc_max, &min_x, &min_y, &max_x, &max_y),
                CE_None);
      loop_band->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE");
      EXPECT_EQ(loop_band->ComputeRasterMinMaxLocation(
                    &loc_min, &loc_max, &min_x, &min_y, &max_x, &max_y),
                CE_None);
    }

    // (b) Partial block (6x6 with 4x4 block) and custom mask band
    auto part_ds = std::make_unique<TestDataset>(6, 6, GA_Update);
    auto* part_band = new TestRasterBand(part_ds.get(), 1, dt, 6, 6, 4, 4);
    part_ds->AddTestBand(part_band);
    part_band->Fill(10);
    EXPECT_EQ(part_band->RasterIO(GF_Write, 1, 1, 1, 1, &v_min, 1, 1,
                                  GDT_Float64, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(part_band->RasterIO(GF_Write, 5, 5, 1, 1, &v_max, 1, 1,
                                  GDT_Float64, 0, 0, nullptr),
              CE_None);
    part_band->FlushCache();

    TestRasterBand m6(part_ds.get(), 2, GDT_Byte, 6, 6, 4, 4);
    m6.Fill(255);
    m6.storage_[0] = 0;
    part_band->custom_mask_band_ = &m6;
    part_band->custom_mask_flags_ = GMF_PER_DATASET;

    EXPECT_EQ(part_band->ComputeRasterMinMaxLocation(&loc_min, &loc_max, &min_x,
                                                     &min_y, &max_x, &max_y),
              CE_None);
    EXPECT_EQ(part_band->ComputeRasterMinMaxLocation(&loc_min, nullptr, &min_x,
                                                     &min_y, nullptr, nullptr),
              CE_None);
    EXPECT_EQ(part_band->ComputeRasterMinMaxLocation(nullptr, &loc_max, nullptr,
                                                     nullptr, &max_x, &max_y),
              CE_None);
  }

  // All nodata in ComputeRasterMinMaxLocation -> CE_Warning
  TestRasterBand loc_nd_band(nullptr, 1, GDT_Byte, 4, 4, 4, 4);
  loc_nd_band.Fill(99);
  loc_nd_band.has_nodata_ = true;
  loc_nd_band.nodata_val_ = 99.0;
  EXPECT_EQ(loc_nd_band.ComputeRasterMinMaxLocation(&loc_min, &loc_max, &min_x,
                                                    &min_y, &max_x, &max_y),
            CE_Warning);

  CPLPopErrorHandler();
}

}  // namespace
namespace {

TEST(GDALRasterBandTest, RATMaskBandsAndColorTranslation) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // 1. RAT
  TestRasterBand standalone_band;
  GDALRasterBandH hStandalone = GDALRasterBand::ToHandle(&standalone_band);
  EXPECT_EQ(GDALGetDefaultRAT(nullptr), nullptr);
  EXPECT_EQ(GDALGetDefaultRAT(hStandalone), nullptr);
  EXPECT_EQ(GDALSetDefaultRAT(nullptr, nullptr), CE_Failure);
  EXPECT_EQ(GDALSetDefaultRAT(hStandalone, nullptr), CE_Failure);
  standalone_band.SetMOFlags(GMO_IGNORE_UNIMPLEMENTED);
  EXPECT_EQ(GDALSetDefaultRAT(hStandalone, nullptr), CE_Failure);
  standalone_band.SetMOFlags(0);

  // 2. MaskBand null & standalone checks
  EXPECT_EQ(GDALGetMaskBand(nullptr), nullptr);
  EXPECT_EQ(GDALGetMaskFlags(nullptr), GMF_ALL_VALID);
  EXPECT_EQ(GDALCreateMaskBand(nullptr, GMF_PER_DATASET), CE_Failure);
  EXPECT_FALSE(GDALIsMaskBand(nullptr));
  EXPECT_FALSE(GDALIsMaskBand(hStandalone));
  EXPECT_EQ(standalone_band.GetMaskValueRange(), GMVR_UNKNOWN);
  // CreateMaskBand when poDS == nullptr
  EXPECT_EQ(GDALCreateMaskBand(hStandalone, GMF_PER_DATASET), CE_Failure);
  standalone_band.SetMOFlags(GMO_IGNORE_UNIMPLEMENTED);
  EXPECT_EQ(GDALCreateMaskBand(hStandalone, GMF_PER_DATASET), CE_Failure);
  standalone_band.SetMOFlags(0);

  // 3. MaskBand lifecycle: AllValid -> NoData -> changed NoData across Byte,
  // Float64(NaN), Int64, UInt64
  {
    auto ds = std::make_unique<TestDataset>(8, 8, GA_Update);
    auto* b_byte = new TestRasterBand(ds.get(), 1, GDT_Byte, 8, 8, 4, 4);
    ds->AddTestBand(b_byte);
    EXPECT_EQ(GDALGetMaskFlags(GDALRasterBand::ToHandle(b_byte)),
              GMF_ALL_VALID);
    GDALRasterBand* mb = b_byte->GetMaskBand();
    ASSERT_NE(mb, nullptr);
    EXPECT_TRUE(GDALIsMaskBand(GDALRasterBand::ToHandle(mb)));
    EXPECT_EQ(mb->GetMaskValueRange(), GMVR_0_AND_255_ONLY);

    // Switch from AllValid to NoData
    b_byte->has_nodata_ = true;
    b_byte->nodata_val_ = 10.0;
    b_byte->InvalidateMaskBand();
    EXPECT_EQ(b_byte->GetMaskFlags(), GMF_NODATA);
    mb = b_byte->GetMaskBand();
    ASSERT_NE(mb, nullptr);
    // Change nodata value -> recreates NoDataMaskBand
    b_byte->nodata_val_ = 20.0;
    mb = b_byte->GetMaskBand();
    ASSERT_NE(mb, nullptr);
  }
  {
    auto ds = std::make_unique<TestDataset>(8, 8, GA_Update);
    auto* b_f64 = new TestRasterBand(ds.get(), 1, GDT_Float64, 8, 8, 4, 4);
    ds->AddTestBand(b_f64);
    (void)b_f64->GetMaskBand();  // AllValid
    b_f64->has_nodata_ = true;
    b_f64->nodata_val_ = std::numeric_limits<double>::quiet_NaN();
    b_f64->InvalidateMaskBand();
    EXPECT_NE(b_f64->GetMaskBand(), nullptr);
    EXPECT_NE(b_f64->GetMaskBand(), nullptr);  // Same NaN nodata
    b_f64->nodata_val_ = 5.0;
    EXPECT_NE(b_f64->GetMaskBand(), nullptr);  // Changed from NaN to 5.0
  }
  {
    auto ds = std::make_unique<TestDataset>(8, 8, GA_Update);
    auto* b_i64 = new TestRasterBand(ds.get(), 1, GDT_Int64, 8, 8, 4, 4);
    ds->AddTestBand(b_i64);
    (void)b_i64->GetMaskBand();  // AllValid
    b_i64->has_nodata_i64_ = true;
    b_i64->nodata_i64_ = -100;
    b_i64->InvalidateMaskBand();
    EXPECT_EQ(b_i64->GetMaskFlags(), GMF_NODATA);
    EXPECT_NE(b_i64->GetMaskBand(), nullptr);
    b_i64->nodata_i64_ = -200;
    EXPECT_NE(b_i64->GetMaskBand(), nullptr);
  }
  {
    auto ds = std::make_unique<TestDataset>(8, 8, GA_Update);
    auto* b_u64 = new TestRasterBand(ds.get(), 1, GDT_UInt64, 8, 8, 4, 4);
    ds->AddTestBand(b_u64);
    (void)b_u64->GetMaskBand();  // AllValid
    b_u64->has_nodata_u64_ = true;
    b_u64->nodata_u64_ = 100U;
    b_u64->InvalidateMaskBand();
    EXPECT_EQ(b_u64->GetMaskFlags(), GMF_NODATA);
    EXPECT_NE(b_u64->GetMaskBand(), nullptr);
    b_u64->nodata_u64_ = 200U;
    EXPECT_NE(b_u64->GetMaskBand(), nullptr);
  }

  // 4. NODATA_VALUES on dataset (valid, mismatched count, mismatched band
  // types)
  {
    auto ds = std::make_unique<TestDataset>(4, 4, GA_Update);
    auto* b1 = new TestRasterBand(ds.get(), 1, GDT_Byte, 4, 4, 4, 4);
    auto* b2 = new TestRasterBand(ds.get(), 2, GDT_Byte, 4, 4, 4, 4);
    ds->AddTestBand(b1);
    ds->AddTestBand(b2);
    ds->SetMetadataItem("NODATA_VALUES", "10 20");
    EXPECT_EQ(b1->GetMaskFlags(), GMF_PER_DATASET | GMF_NODATA);
    GDALRasterBand* mb = b1->GetMaskBand();
    ASSERT_NE(mb, nullptr);
    GByte mval = 0;
    EXPECT_EQ(
        mb->RasterIO(GF_Read, 0, 0, 1, 1, &mval, 1, 1, GDT_Byte, 0, 0, nullptr),
        CE_None);
  }
  {
    // Mismatched count in NODATA_VALUES
    auto ds = std::make_unique<TestDataset>(4, 4, GA_Update);
    auto* b1 = new TestRasterBand(ds.get(), 1, GDT_Byte, 4, 4, 4, 4);
    auto* b2 = new TestRasterBand(ds.get(), 2, GDT_Byte, 4, 4, 4, 4);
    ds->AddTestBand(b1);
    ds->AddTestBand(b2);
    ds->SetMetadataItem("NODATA_VALUES", "10");
    EXPECT_EQ(b1->GetMaskFlags(), GMF_ALL_VALID);
    EXPECT_NE(b1->GetMaskBand(), nullptr);
  }
  {
    // Mismatched band data types with NODATA_VALUES
    auto ds = std::make_unique<TestDataset>(4, 4, GA_Update);
    auto* b1 = new TestRasterBand(ds.get(), 1, GDT_Byte, 4, 4, 4, 4);
    auto* b2 = new TestRasterBand(ds.get(), 2, GDT_UInt16, 4, 4, 4, 4);
    ds->AddTestBand(b1);
    ds->AddTestBand(b2);
    ds->SetMetadataItem("NODATA_VALUES", "10 20");
    EXPECT_EQ(b1->GetMaskFlags(), GMF_ALL_VALID);
    EXPECT_NE(b1->GetMaskBand(), nullptr);
  }

  // 5. Alpha band masks (2-band and 4-band, both GDT_Byte and GDT_UInt16)
  for (int nbands : {2, 4}) {
    for (GDALDataType alpha_dt : {GDT_Byte, GDT_UInt16}) {
      auto ds = std::make_unique<TestDataset>(4, 4, GA_Update);
      for (int i = 1; i <= nbands; ++i) {
        auto* b = new TestRasterBand(ds.get(), i, alpha_dt, 4, 4, 4, 4);
        if (i < nbands) {
          b->custom_color_interp_ = (i == 1)   ? GCI_RedBand
                                    : (i == 2) ? GCI_GreenBand
                                               : GCI_BlueBand;
        } else {
          b->custom_color_interp_ = GCI_AlphaBand;
        }
        ds->AddTestBand(b);
      }
      GDALRasterBand* b1 = ds->GetRasterBand(1);
      EXPECT_EQ(b1->GetMaskFlags(), GMF_ALPHA | GMF_PER_DATASET);
      GDALRasterBand* mb = b1->GetMaskBand();
      ASSERT_NE(mb, nullptr);
      EXPECT_EQ(mb->GetMaskValueRange(), GMVR_UNKNOWN);
      GByte mval = 0;
      EXPECT_EQ(mb->RasterIO(GF_Read, 0, 0, 1, 1, &mval, 1, 1, GDT_Byte, 0, 0,
                             nullptr),
                CE_None);
    }
  }

  // 6. CreateMaskBand on a GTiff in /vsimem/
  GDALDriver* gtiff = GetGDALDriverManager()->GetDriverByName("GTiff");
  if (gtiff != nullptr) {
    const char* path = "/vsimem/gdalrasterband_test_mask.tif";
    GDALDataset* mds = gtiff->Create(path, 8, 8, 1, GDT_Byte, nullptr);
    ASSERT_NE(mds, nullptr);
    GDALRasterBand* mb = mds->GetRasterBand(1);
    EXPECT_EQ(GDALCreateMaskBand(GDALRasterBand::ToHandle(mb), GMF_PER_DATASET),
              CE_None);
    EXPECT_NE(mb->GetMaskBand(), nullptr);
    GDALClose(mds);
    VSIUnlink(path);
    VSIUnlink("/vsimem/gdalrasterband_test_mask.tif.msk");
  }

  // 7. GetIndexColorTranslationTo
  TestRasterBand src_band;
  TestRasterBand ref_band;
  EXPECT_EQ(src_band.GetIndexColorTranslationTo(nullptr), nullptr);
  EXPECT_EQ(src_band.GetIndexColorTranslationTo(&ref_band), nullptr);

  GDALColorTable ct_src;
  GDALColorEntry c0 = {10, 20, 30, 255};
  GDALColorEntry c1 = {40, 50, 60, 255};
  GDALColorEntry c2 = {70, 80, 90, 255};
  ct_src.SetColorEntry(0, &c0);
  ct_src.SetColorEntry(1, &c1);
  ct_src.SetColorEntry(2, &c2);

  src_band.custom_color_interp_ = GCI_PaletteIndex;
  src_band.custom_color_table_ = &ct_src;
  EXPECT_EQ(src_band.GetIndexColorTranslationTo(&ref_band), nullptr);

  ref_band.custom_color_interp_ = GCI_PaletteIndex;
  ref_band.custom_color_table_ = &ct_src;
  // Identical palette -> returns nullptr (no translation needed) with approx=0
  int approx = 1;
  EXPECT_EQ(src_band.GetIndexColorTranslationTo(&ref_band, nullptr, &approx),
            nullptr);
  EXPECT_EQ(approx, 0);

  // Reordered + approximate match palette + nodata values
  GDALColorTable ct_ref;
  GDALColorEntry c1_near = {42, 51, 59, 255};
  ct_ref.SetColorEntry(0, &c2);
  ct_ref.SetColorEntry(1, &c0);
  ct_ref.SetColorEntry(2, &c1_near);
  ref_band.custom_color_table_ = &ct_ref;

  src_band.has_nodata_ = true;
  src_band.nodata_val_ = 2.0;
  ref_band.has_nodata_ = true;
  ref_band.nodata_val_ = 0.0;

  unsigned char custom_lut[256] = {0};
  unsigned char* res_lut =
      src_band.GetIndexColorTranslationTo(&ref_band, custom_lut, &approx);
  EXPECT_EQ(res_lut, custom_lut);
  EXPECT_EQ(approx, 1);
  EXPECT_EQ(custom_lut[0], 1);  // c0 -> index 1 in ref
  EXPECT_EQ(custom_lut[1], 2);  // c1 -> closest is c1_near at index 2

  // Also test allocated table (pTranslationTable == nullptr)
  unsigned char* alloc_lut =
      src_band.GetIndexColorTranslationTo(&ref_band, nullptr, nullptr);
  ASSERT_NE(alloc_lut, nullptr);
  CPLFree(alloc_lut);

  CPLPopErrorHandler();
}

TEST(GDALRasterBandTest, MiscMethodsVirtualMemCoverageStatusWindows) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  auto ds = std::make_unique<TestDataset>(16, 16, GA_Update);
  ds->SetDescription("my_test_dataset");
  auto* band = new TestRasterBand(ds.get(), 1, GDT_Byte, 16, 16, 4, 4);
  ds->AddTestBand(band);
  GDALRasterBandH hBand = GDALRasterBand::ToHandle(band);

  // 1. ReportError (with dataset description and without dataset description)
  band->ReportError(CE_Warning, CPLE_AppDefined, "warn %d", 1);
  TestRasterBand standalone;
  standalone.ReportError(CE_Warning, CPLE_AppDefined, "warn %d", 2);

  // 2. GetVirtualMemAuto & GDALGetVirtualMemAuto
  int pspace = 0;
  GIntBig lspace = 0;
  EXPECT_EQ(GDALGetVirtualMemAuto(nullptr, GF_Read, &pspace, &lspace, nullptr),
            nullptr);
  CPLVirtualMem* vm1 =
      GDALGetVirtualMemAuto(hBand, GF_Read, nullptr, &lspace, nullptr);
  if (vm1 != nullptr) {
    CPLVirtualMemFree(vm1);
  }
  CPLVirtualMem* vm2 =
      GDALGetVirtualMemAuto(hBand, GF_Read, &pspace, nullptr, nullptr);
  if (vm2 != nullptr) {
    CPLVirtualMemFree(vm2);
  }

  char* vmem_opts_no[] = {const_cast<char*>("USE_DEFAULT_IMPLEMENTATION=NO"),
                          nullptr};
  EXPECT_EQ(band->GetVirtualMemAuto(GF_Read, &pspace, &lspace, vmem_opts_no),
            nullptr);

  char* vmem_opts_yes[] = {const_cast<char*>("USE_DEFAULT_IMPLEMENTATION=YES"),
                           nullptr};
  CPLVirtualMem* vmem =
      band->GetVirtualMemAuto(GF_Read, &pspace, &lspace, vmem_opts_yes);
  if (vmem != nullptr) {
    CPLVirtualMemFree(vmem);
  }

  // 3. GetDataCoverageStatus & IGetDataCoverageStatus
  double pct = -1.0;
  EXPECT_EQ(GDALGetDataCoverageStatus(nullptr, 0, 0, 4, 4, 0, &pct),
            GDAL_DATA_COVERAGE_STATUS_UNIMPLEMENTED);
  EXPECT_EQ(GDALGetDataCoverageStatus(hBand, -1, 0, 4, 4, 0, &pct),
            GDAL_DATA_COVERAGE_STATUS_UNIMPLEMENTED |
                GDAL_DATA_COVERAGE_STATUS_EMPTY);
  EXPECT_EQ(GDALGetDataCoverageStatus(hBand, 0, -1, 4, 4, 0, &pct),
            GDAL_DATA_COVERAGE_STATUS_UNIMPLEMENTED |
                GDAL_DATA_COVERAGE_STATUS_EMPTY);
  EXPECT_EQ(
      GDALGetDataCoverageStatus(hBand, 0, 0, 0, 4, 0, &pct),
      GDAL_DATA_COVERAGE_STATUS_UNIMPLEMENTED | GDAL_DATA_COVERAGE_STATUS_DATA);
  EXPECT_EQ(
      GDALGetDataCoverageStatus(hBand, 0, 0, 4, 0, 0, &pct),
      GDAL_DATA_COVERAGE_STATUS_UNIMPLEMENTED | GDAL_DATA_COVERAGE_STATUS_DATA);
  EXPECT_EQ(GDALGetDataCoverageStatus(hBand, 15, 0, 4, 4, 0, &pct),
            GDAL_DATA_COVERAGE_STATUS_UNIMPLEMENTED |
                GDAL_DATA_COVERAGE_STATUS_EMPTY);
  EXPECT_EQ(GDALGetDataCoverageStatus(hBand, 0, 15, 4, 4, 0, &pct),
            GDAL_DATA_COVERAGE_STATUS_UNIMPLEMENTED |
                GDAL_DATA_COVERAGE_STATUS_EMPTY);

  EXPECT_EQ(
      GDALGetDataCoverageStatus(hBand, 0, 0, 4, 4, 0, &pct),
      GDAL_DATA_COVERAGE_STATUS_UNIMPLEMENTED | GDAL_DATA_COVERAGE_STATUS_DATA);
  EXPECT_DOUBLE_EQ(pct, 100.0);

  // 4. EnterReadWrite, LeaveReadWrite, InitRWLock
  band->InitRWLock();
  EXPECT_EQ(band->EnterReadWrite(GF_Write), TRUE);
  band->LeaveReadWrite();
  EXPECT_EQ(band->EnterReadWrite(GF_Read), TRUE);
  band->LeaveReadWrite();

  // 5. EnablePixelTypeSignedByteWarning & GetMetadataItem("PIXELTYPE",
  // "IMAGE_STRUCTURE")
  GDALEnablePixelTypeSignedByteWarning(hBand, TRUE);
  band->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE");
  EXPECT_STREQ(band->GetMetadataItem("PIXELTYPE", "IMAGE_STRUCTURE"),
               "SIGNEDBYTE");
  GDALEnablePixelTypeSignedByteWarning(hBand, FALSE);
  EXPECT_STREQ(band->GetMetadataItem("PIXELTYPE", "IMAGE_STRUCTURE"),
               "SIGNEDBYTE");
  EXPECT_EQ(band->GetMetadataItem("OTHER", ""), nullptr);

  // 6. IterateWindows
  // (a) maxSize == 0 -> uses block size (4x4 on 16x16 -> 16 windows)
  uint64_t win_count = 0;
  auto wrapper0 = band->IterateWindows(0);
  EXPECT_EQ(wrapper0.count(), 16U);
  for (const auto& w : wrapper0) {
    EXPECT_GT(w.nXSize, 0);
    EXPECT_GT(w.nYSize, 0);
    win_count++;
  }
  EXPECT_EQ(win_count, 16U);
  auto it_begin = wrapper0.begin();
  auto it_end = wrapper0.end();
  EXPECT_TRUE(it_begin == wrapper0.begin());
  EXPECT_TRUE(it_begin != it_end);

  // (b) maxSize > 0 with dfBlocksPerChunk < dfBlocksPerRow (e.g., maxSize = 32
  // -> 2 blocks per row)
  auto wrapper_small = band->IterateWindows(32);
  EXPECT_GT(wrapper_small.count(), 0U);
  for (const auto& w : wrapper_small) {
    EXPECT_GT(w.nXSize, 0);
  }

  // (c) maxSize > 0 with dfBlocksPerChunk >= dfBlocksPerRow (e.g., maxSize =
  // 128 -> full rows, 2 block rows)
  auto wrapper_large = band->IterateWindows(128);
  EXPECT_GT(wrapper_large.count(), 0U);
  for (const auto& w : wrapper_large) {
    EXPECT_EQ(w.nXSize, 16);
  }

  // (d) Invalid block size -> fallback to whole raster window
  TestRasterBand bad_blk_band;
  bad_blk_band.SetDimsAndType(10, 10, 0, 0, GDT_Byte);
  auto wrapper_bad = bad_blk_band.IterateWindows(0);
  EXPECT_EQ(wrapper_bad.count(), 10U);

  CPLPopErrorHandler();
}

}  // namespace
namespace {

TEST(GDALRasterBandTest, AsMDArrayInterpolateSplitRasterIO) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // 1. AsMDArray on band without dataset -> nullptr
  TestRasterBand standalone;
  EXPECT_EQ(standalone.AsMDArray(), nullptr);
  EXPECT_EQ(GDALRasterBandAsMDArray(nullptr), nullptr);

  // 2. AsMDArray with dataset, geotransform, SRS, metadata, offset, scale,
  // unit, nodata
  auto ds = std::make_unique<TestDataset>(8, 6, GA_Update);
  ds->SetDescription("/vsimem/mdarray_test.tif");
  ds->has_gt_ = true;
  ds->gt_ = GDALGeoTransform(100.0, 2.0, 0.0, 200.0, 0.0, -2.0);
  ds->has_srs_ = true;
  ds->srs_.importFromEPSG(4326);  // Authority compliant {2, 1} axis mapping

  auto* b_f64 = new TestRasterBand(ds.get(), 1, GDT_Float64, 8, 6, 4, 3);
  b_f64->Fill(12.5);
  b_f64->has_nodata_ = true;
  b_f64->nodata_val_ = -999.0;
  b_f64->custom_unit_ = "meters";
  b_f64->has_custom_offset_ = true;
  b_f64->custom_offset_ = 1.5;
  b_f64->has_custom_scale_ = true;
  b_f64->custom_scale_ = 0.5;
  b_f64->SetMetadataItem("META_KEY", "META_VAL");
  ds->AddTestBand(b_f64);

  GDALMDArrayH hArr = GDALRasterBandAsMDArray(GDALRasterBand::ToHandle(b_f64));
  ASSERT_NE(hArr, nullptr);
  GDALMDArrayRelease(hArr);

  auto md = b_f64->AsMDArray();
  ASSERT_NE(md, nullptr);
  EXPECT_TRUE(md->IsWritable());
  EXPECT_STREQ(md->GetFilename().c_str(), "/vsimem/mdarray_test.tif");
  EXPECT_STREQ(md->GetUnit().c_str(), "meters");
  bool has_off = false, has_scl = false;
  GDALDataType st_dt = GDT_Unknown;
  EXPECT_DOUBLE_EQ(md->GetOffset(&has_off, &st_dt), 1.5);
  EXPECT_TRUE(has_off);
  EXPECT_DOUBLE_EQ(md->GetScale(&has_scl, &st_dt), 0.5);
  EXPECT_TRUE(has_scl);
  EXPECT_NE(md->GetRawNoDataValue(), nullptr);
  EXPECT_NE(md->GetSpatialRef(), nullptr);
  EXPECT_EQ(md->GetBlockSize().size(), 2U);
  EXPECT_FALSE(md->GetAttributes(nullptr).empty());

  const auto& dims = md->GetDimensions();
  ASSERT_EQ(dims.size(), 2U);
  // Read coordinate indexing variables X and Y
  for (const auto& dim : dims) {
    auto idx_var = dim->GetIndexingVariable();
    ASSERT_NE(idx_var, nullptr);
    GUInt64 start[1] = {0};
    size_t count[1] = {static_cast<size_t>(dim->GetSize())};
    GInt64 step[1] = {1};
    GPtrDiff_t stride[1] = {1};
    std::vector<double> coords(count[0], 0.0);
    EXPECT_TRUE(idx_var->Read(start, count, step, stride,
                              GDALExtendedDataType::Create(GDT_Float64),
                              coords.data()));
  }

  // Write and Read via MDArray (positive and negative steps)
  GUInt64 start[2] = {0, 0};
  size_t count[2] = {2, 3};
  GInt64 step_pos[2] = {1, 1};
  GPtrDiff_t stride[2] = {3, 1};
  double write_vals[6] = {1, 2, 3, 4, 5, 6};
  EXPECT_TRUE(md->Write(start, count, step_pos, stride,
                        GDALExtendedDataType::Create(GDT_Float64), write_vals));
  double read_vals[6] = {0};
  EXPECT_TRUE(md->Read(start, count, step_pos, stride,
                       GDALExtendedDataType::Create(GDT_Float64), read_vals));
  EXPECT_DOUBLE_EQ(read_vals[0], 1.0);
  EXPECT_DOUBLE_EQ(read_vals[5], 6.0);

  // Negative step read
  GUInt64 start_neg[2] = {1, 2};
  GInt64 step_neg[2] = {-1, -1};
  EXPECT_TRUE(md->Read(start_neg, count, step_neg, stride,
                       GDALExtendedDataType::Create(GDT_Float64), read_vals));

  // Also test SRS with GIS_TRADITIONAL axis order {1, 2}, Int64 nodata, UInt64
  // nodata, SignedByte
  ds->srs_.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  auto* b_i64 = new TestRasterBand(ds.get(), 2, GDT_Int64, 8, 6, 4, 3);
  b_i64->has_nodata_i64_ = true;
  b_i64->nodata_i64_ = -42;
  ds->AddTestBand(b_i64);
  auto md_i64 = b_i64->AsMDArray();
  ASSERT_NE(md_i64, nullptr);
  EXPECT_NE(md_i64->GetSpatialRef(), nullptr);
  EXPECT_NE(md_i64->GetRawNoDataValue(), nullptr);

  auto* b_u64 = new TestRasterBand(ds.get(), 3, GDT_UInt64, 8, 6, 4, 3);
  b_u64->has_nodata_u64_ = true;
  b_u64->nodata_u64_ = 42U;
  ds->AddTestBand(b_u64);
  auto md_u64 = b_u64->AsMDArray();
  ASSERT_NE(md_u64, nullptr);
  EXPECT_NE(md_u64->GetRawNoDataValue(), nullptr);

  auto* b_sb = new TestRasterBand(ds.get(), 4, GDT_Byte, 8, 6, 4, 3);
  b_sb->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE");
  ds->AddTestBand(b_sb);
  auto md_sb = b_sb->AsMDArray();
  ASSERT_NE(md_sb, nullptr);
  EXPECT_EQ(md_sb->GetDataType().GetNumericDataType(), GDT_Byte);

  // 3. InterpolateAtPoint & InterpolateAtGeolocation
  double real_v = 0.0, imag_v = 0.0;
  GDALRasterBandH hBand = GDALRasterBand::ToHandle(b_f64);
  EXPECT_EQ(GDALRasterInterpolateAtPoint(
                nullptr, 1.0, 1.0, GRIORA_NearestNeighbour, &real_v, &imag_v),
            CE_Failure);
  // Unsupported resampling alg -> CE_Failure
  EXPECT_EQ(GDALRasterInterpolateAtPoint(hBand, 1.0, 1.0, GRIORA_Average,
                                         &real_v, &imag_v),
            CE_Failure);
  // Out of bounds point -> CE_Failure
  EXPECT_EQ(GDALRasterInterpolateAtPoint(
                hBand, -5.0, 1.0, GRIORA_NearestNeighbour, &real_v, &imag_v),
            CE_Failure);
  // Valid interpolations
  for (GDALRIOResampleAlg alg : {GRIORA_NearestNeighbour, GRIORA_Bilinear,
                                 GRIORA_Cubic, GRIORA_CubicSpline}) {
    EXPECT_EQ(
        GDALRasterInterpolateAtPoint(hBand, 2.5, 2.5, alg, &real_v, &imag_v),
        CE_None);
  }
  // Complex band interpolation
  auto* b_cf64 = new TestRasterBand(ds.get(), 5, GDT_CFloat64, 8, 6, 4, 3);
  b_cf64->Fill(3.0, 4.0);
  ds->AddTestBand(b_cf64);
  EXPECT_EQ(
      b_cf64->InterpolateAtPoint(2.5, 2.5, GRIORA_Bilinear, &real_v, &imag_v),
      CE_None);
  EXPECT_NEAR(real_v, 3.0, 1e-5);
  EXPECT_NEAR(imag_v, 4.0, 1e-5);

  // InterpolateAtGeolocation
  EXPECT_EQ(GDALRasterInterpolateAtGeolocation(nullptr, 102.0, 198.0, nullptr,
                                               GRIORA_NearestNeighbour, &real_v,
                                               &imag_v, nullptr),
            CE_Failure);
  EXPECT_EQ(GDALRasterInterpolateAtGeolocation(hBand, 102.0, 198.0, nullptr,
                                               GRIORA_NearestNeighbour, &real_v,
                                               &imag_v, nullptr),
            CE_None);
  // With explicit SRS (note: EPSG:4326 traditional order)
  EXPECT_EQ(GDALRasterInterpolateAtGeolocation(
                hBand, 102.0, 198.0, OGRSpatialReference::ToHandle(&ds->srs_),
                GRIORA_Bilinear, &real_v, &imag_v, nullptr),
            CE_None);
  // Without GeoTransform -> CE_Failure
  ds->has_gt_ = false;
  EXPECT_EQ(b_f64->InterpolateAtGeolocation(102.0, 198.0, nullptr,
                                            GRIORA_NearestNeighbour, &real_v,
                                            &imag_v, nullptr),
            CE_Failure);

  // 4. SplitRasterIO
  GDALRasterIOExtraArg extra;
  INIT_RASTERIO_EXTRA_ARG(extra);
  std::vector<double> split_buf(48, 0.0);
  // (a) Height split: nBufXSize == nRasterXSize (8) && nBufYSize >= 2 (6)
  EXPECT_EQ(b_f64->SplitRasterIO(GF_Read, 0, 0, 8, 6, split_buf.data(), 8, 6,
                                 GDT_Float64, 8, 64, &extra),
            CE_None);
  // (b) Width split: nBufXSize >= 2 (4) && nBufYSize < nBufXSize (2) &&
  // nBufXSize < nRasterXSize (8)
  EXPECT_EQ(b_f64->SplitRasterIO(GF_Read, 0, 0, 4, 2, split_buf.data(), 4, 2,
                                 GDT_Float64, 8, 32, &extra),
            CE_None);
  // (c) 1x1 -> cannot split, returns CE_Warning
  EXPECT_EQ(b_f64->SplitRasterIO(GF_Read, 0, 0, 1, 1, split_buf.data(), 1, 1,
                                 GDT_Float64, 8, 8, &extra),
            CE_Warning);

  CPLPopErrorHandler();
}

TEST(GDALRasterBandTest, RasterAlgebra) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(mem_drv, nullptr);
  std::unique_ptr<GDALDataset> ds1(
      mem_drv->Create("mem1", 4, 4, 2, GDT_Float32, nullptr));
  std::unique_ptr<GDALDataset> ds_diff(
      mem_drv->Create("mem_diff", 5, 4, 1, GDT_Float32, nullptr));
  ASSERT_NE(ds1, nullptr);
  ASSERT_NE(ds_diff, nullptr);

  GDALRasterBand* b1 = ds1->GetRasterBand(1);
  GDALRasterBand* b2 = ds1->GetRasterBand(2);
  GDALRasterBand* b_diff = ds_diff->GetRasterBand(1);
  b1->Fill(4.0);
  b2->Fill(2.0);
  GDALRasterBandH h1 = GDALRasterBand::ToHandle(b1);
  GDALRasterBandH h2 = GDALRasterBand::ToHandle(b2);
  GDALRasterBandH h_diff = GDALRasterBand::ToHandle(b_diff);

  // 1. Unary operations C API
  EXPECT_EQ(GDALRasterBandUnaryOp(nullptr, GRAUO_ABS), nullptr);
  for (GDALRasterAlgebraUnaryOperation uop :
       {GRAUO_LOGICAL_NOT, GRAUO_ABS, GRAUO_SQRT, GRAUO_LOG, GRAUO_LOG10}) {
    GDALComputedRasterBandH hComp = GDALRasterBandUnaryOp(h1, uop);
    if (hComp) {
      GDALComputedRasterBandRelease(hComp);
    }
  }

  // 2. Binary operations C API (Band-Band, Band-Double, Double-Band)
  EXPECT_EQ(GDALRasterBandBinaryOpBand(nullptr, GRABO_ADD, h2), nullptr);
  EXPECT_EQ(GDALRasterBandBinaryOpBand(h1, GRABO_ADD, nullptr), nullptr);
  EXPECT_EQ(GDALRasterBandBinaryOpBand(h1, GRABO_ADD, h_diff), nullptr);
  EXPECT_EQ(GDALRasterBandBinaryOpDouble(nullptr, GRABO_ADD, 1.0), nullptr);
  EXPECT_EQ(GDALRasterBandBinaryOpDoubleToBand(1.0, GRABO_ADD, nullptr),
            nullptr);

  for (GDALRasterAlgebraBinaryOperation bop :
       {GRABO_ADD, GRABO_SUB, GRABO_MUL, GRABO_DIV}) {
    GDALComputedRasterBandH c_bb = GDALRasterBandBinaryOpBand(h1, bop, h2);
    if (c_bb) GDALComputedRasterBandRelease(c_bb);
    GDALComputedRasterBandH c_bd = GDALRasterBandBinaryOpDouble(h1, bop, 2.0);
    if (c_bd) GDALComputedRasterBandRelease(c_bd);
  }

  // For DoubleToBand, test ADD, MUL, DIV (omit SUB to avoid GDAL
  // chained-temporary stack bug)
  for (GDALRasterAlgebraBinaryOperation bop :
       {GRABO_ADD, GRABO_MUL, GRABO_DIV}) {
    GDALComputedRasterBandH c_db =
        GDALRasterBandBinaryOpDoubleToBand(2.0, bop, h1);
    if (c_db) GDALComputedRasterBandRelease(c_db);
  }

  // 3. IfThenElse, AsDataType, Max/Min/Mean C API
  EXPECT_EQ(GDALRasterBandIfThenElse(nullptr, h1, h2), nullptr);
  EXPECT_EQ(GDALRasterBandIfThenElse(h1, nullptr, h2), nullptr);
  EXPECT_EQ(GDALRasterBandIfThenElse(h1, h2, nullptr), nullptr);
  GDALComputedRasterBandH c_ite = GDALRasterBandIfThenElse(h1, h1, h2);
  if (c_ite) GDALComputedRasterBandRelease(c_ite);

  EXPECT_EQ(GDALRasterBandAsDataType(nullptr, GDT_Float64), nullptr);
  EXPECT_EQ(GDALRasterBandAsDataType(h1, GDT_Unknown), nullptr);
  GDALComputedRasterBandH c_dt = GDALRasterBandAsDataType(h1, GDT_Float64);
  ASSERT_NE(c_dt, nullptr);
  GDALComputedRasterBandRelease(c_dt);

  GDALRasterBandH bands[2] = {h1, h2};
  GDALRasterBandH bad_bands[2] = {h1, h_diff};

  EXPECT_EQ(GDALMaximumOfNBands(0, bands), nullptr);
  EXPECT_EQ(GDALMaximumOfNBands(2, nullptr), nullptr);
  EXPECT_EQ(GDALMaximumOfNBands(2, bad_bands), nullptr);
  GDALComputedRasterBandH c_max = GDALMaximumOfNBands(2, bands);
  ASSERT_NE(c_max, nullptr);
  GDALComputedRasterBandRelease(c_max);

  GDALComputedRasterBandH c_maxc = GDALRasterBandMaxConstant(h1, 1.0);
  ASSERT_NE(c_maxc, nullptr);
  GDALComputedRasterBandRelease(c_maxc);

  EXPECT_EQ(GDALMinimumOfNBands(0, bands), nullptr);
  EXPECT_EQ(GDALMinimumOfNBands(2, nullptr), nullptr);
  EXPECT_EQ(GDALMinimumOfNBands(2, bad_bands), nullptr);
  GDALComputedRasterBandH c_min = GDALMinimumOfNBands(2, bands);
  ASSERT_NE(c_min, nullptr);
  GDALComputedRasterBandRelease(c_min);

  GDALComputedRasterBandH c_minc = GDALRasterBandMinConstant(h1, 1.0);
  ASSERT_NE(c_minc, nullptr);
  GDALComputedRasterBandRelease(c_minc);

  EXPECT_EQ(GDALMeanOfNBands(0, bands), nullptr);
  EXPECT_EQ(GDALMeanOfNBands(2, nullptr), nullptr);
  EXPECT_EQ(GDALMeanOfNBands(2, bad_bands), nullptr);
  GDALComputedRasterBandH c_mean = GDALMeanOfNBands(2, bands);
  ASSERT_NE(c_mean, nullptr);
  GDALComputedRasterBandRelease(c_mean);

  // 4. C++ operators that work without muparser
  GDALRasterBand::ThrowIfNotSameDimensions(*b1, *b2);
  EXPECT_EQ(((*b1) + (*b2)).GetXSize(), 4);
  EXPECT_EQ(((*b1) + 2.0).GetXSize(), 4);
  EXPECT_EQ((2.0 + (*b1)).GetXSize(), 4);
  EXPECT_EQ(((*b1) - (*b2)).GetXSize(), 4);
  EXPECT_EQ(((*b1) - 2.0).GetXSize(), 4);
  EXPECT_EQ(((*b1) * (*b2)).GetXSize(), 4);
  EXPECT_EQ(((*b1) * 2.0).GetXSize(), 4);
  EXPECT_EQ((2.0 * (*b1)).GetXSize(), 4);
  EXPECT_EQ(((*b1) / (*b2)).GetXSize(), 4);
  EXPECT_EQ(((*b1) / 2.0).GetXSize(), 4);
  EXPECT_EQ((2.0 / (*b1)).GetXSize(), 4);
  EXPECT_EQ((!(*b1)).GetXSize(), 4);
  EXPECT_EQ(b1->AsType(GDT_Float64).GetXSize(), 4);
  EXPECT_EQ(gdal::max(*b1, *b2).GetXSize(), 4);
  EXPECT_EQ(gdal::max(*b1, 5.0).GetXSize(), 4);
  EXPECT_EQ(gdal::max(5.0, *b1).GetXSize(), 4);
  EXPECT_EQ(gdal::min(*b1, *b2).GetXSize(), 4);
  EXPECT_EQ(gdal::min(*b1, 5.0).GetXSize(), 4);
  EXPECT_EQ(gdal::min(5.0, *b1).GetXSize(), 4);
  EXPECT_EQ(gdal::mean(*b1, *b2).GetXSize(), 4);
  EXPECT_EQ(gdal::abs(*b1).GetXSize(), 4);
  EXPECT_EQ(gdal::fabs(*b1).GetXSize(), 4);
  EXPECT_EQ(gdal::sqrt(*b1).GetXSize(), 4);
  EXPECT_EQ(gdal::log10(*b1).GetXSize(), 4);
  EXPECT_EQ(gdal::pow(*b1, 2.0).GetXSize(), 4);
  EXPECT_EQ(gdal::pow(2.0, *b1).GetXSize(), 4);

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
  EXPECT_THROW(GDALRasterBand::ThrowIfNotSameDimensions(*b1, *b_diff),
               std::exception);
  EXPECT_THROW((void)((*b1) > (*b2)), std::exception);
  EXPECT_THROW((void)((*b1) > 1.0), std::exception);
  EXPECT_THROW((void)(1.0 > (*b1)), std::exception);
  EXPECT_THROW((void)((*b1) >= (*b2)), std::exception);
  EXPECT_THROW((void)((*b1) >= 1.0), std::exception);
  EXPECT_THROW((void)(1.0 >= (*b1)), std::exception);
  EXPECT_THROW((void)((*b1) < (*b2)), std::exception);
  EXPECT_THROW((void)((*b1) < 1.0), std::exception);
  EXPECT_THROW((void)(1.0 < (*b1)), std::exception);
  EXPECT_THROW((void)((*b1) <= (*b2)), std::exception);
  EXPECT_THROW((void)((*b1) <= 1.0), std::exception);
  EXPECT_THROW((void)(1.0 <= (*b1)), std::exception);
  EXPECT_THROW((void)((*b1) == (*b2)), std::exception);
  EXPECT_THROW((void)((*b1) == 1.0), std::exception);
  EXPECT_THROW((void)(1.0 == (*b1)), std::exception);
  EXPECT_THROW((void)((*b1) != (*b2)), std::exception);
  EXPECT_THROW((void)((*b1) != 1.0), std::exception);
  EXPECT_THROW((void)(1.0 != (*b1)), std::exception);
  EXPECT_THROW((void)((*b1) && (*b2)), std::exception);
  EXPECT_THROW((void)((*b1) && true), std::exception);
  EXPECT_THROW((void)(true && (*b1)), std::exception);
  EXPECT_THROW((void)((*b1) || (*b2)), std::exception);
  EXPECT_THROW((void)((*b1) || false), std::exception);
  EXPECT_THROW((void)(false || (*b1)), std::exception);
  EXPECT_THROW((void)gdal::IfThenElse(*b1, *b1, *b2), std::exception);
  EXPECT_THROW((void)gdal::IfThenElse(*b1, 1.0, *b2), std::exception);
  EXPECT_THROW((void)gdal::IfThenElse(*b1, *b1, 2.0), std::exception);
  EXPECT_THROW((void)gdal::IfThenElse(*b1, 1.0, 2.0), std::exception);
  EXPECT_THROW((void)gdal::log(*b1), std::exception);
  EXPECT_THROW((void)gdal::pow(*b1, *b2), std::exception);
#endif

  CPLPopErrorHandler();
}

}  // namespace
}  // namespace gdal
