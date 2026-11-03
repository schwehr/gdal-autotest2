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
// Tests GDALRasterBand and related C/C++ raster band APIs
// (gcore/gdalrasterband.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalrasterband.cpp

#include <algorithm>
#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_colortable.h"
#include "third_party/gdal/gcore/gdal_computedrasterband.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_geotransform.h"
#include "third_party/gdal/gcore/gdal_multidim.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/gcore/gdal_rasterblock.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_float.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_virtualmem.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr auto kRealDataTypes = std::to_array<GDALDataType>({
    GDT_Byte,
    GDT_Int8,
    GDT_UInt16,
    GDT_Int16,
    GDT_UInt32,
    GDT_Int32,
    GDT_UInt64,
    GDT_Int64,
    GDT_Float16,
    GDT_Float32,
    GDT_Float64,
});
static_assert(kRealDataTypes.size() == 11);

constexpr auto kAllDataTypes = std::to_array<GDALDataType>({
    GDT_Byte,
    GDT_Int8,
    GDT_UInt16,
    GDT_Int16,
    GDT_UInt32,
    GDT_Int32,
    GDT_UInt64,
    GDT_Int64,
    GDT_Float16,
    GDT_Float32,
    GDT_Float64,
    GDT_CInt16,
    GDT_CInt32,
    GDT_CFloat16,
    GDT_CFloat32,
    GDT_CFloat64,
});
static_assert(kAllDataTypes.size() == 16);

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

  using GDALRasterBand::EnterReadWrite;
  using GDALRasterBand::HasBlockCache;
  using GDALRasterBand::HasDirtyBlocks;
  using GDALRasterBand::InitBlockInfo;
  using GDALRasterBand::InitRWLock;
  using GDALRasterBand::InvalidateMaskBand;
  using GDALRasterBand::LeaveReadWrite;
  using GDALRasterBand::SetValidPercent;
  using GDALRasterBand::SplitRasterIO;

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

TEST(GDALRasterBandTest, ConstructorsAndDestructorBranches) {
  // 1. Default and bForceCachedIO constructors via GDAL_FORCE_CACHING
  {
    CPLConfigOptionSetter setter("GDAL_FORCE_CACHING", "YES", false);
    TestRasterBand band_default(FALSE);
    EXPECT_EQ(16, band_default.GetXSize());
    EXPECT_EQ(16, band_default.GetYSize());
  }

  // 2. Destructor with MarkSuppressOnClose on dataset and active block cache
  {
    auto ds = std::make_unique<TestDataset>(8, 8, GA_Update);
    auto* band = new TestRasterBand(ds.get(), 1, GDT_Byte, 8, 8, 4, 4);
    ds->AddTestBand(band);
    GDALRasterBlock* blk = band->GetLockedBlockRef(0, 0, TRUE);
    ASSERT_NE(nullptr, blk);
    blk->MarkDirty();
    blk->DropLock();
    ds->MarkSuppressOnClose();
  }

  // 3. Destructor when nBlockReads > nBlocksPerRow * nBlocksPerColumn
  {
    auto ds = std::make_unique<TestDataset>(8, 8, GA_Update);
    auto* band = new TestRasterBand(ds.get(), 1, GDT_Byte, 8, 8, 4, 4);
    ds->AddTestBand(band);
    ASSERT_EQ(TRUE, band->InitBlockInfo());
    band->SetBlockReads(100);
  }

  // 4. Destructor with m_poPointsCache populated via InterpolateAtPoint
  {
    auto ds = std::make_unique<TestDataset>(8, 8, GA_Update);
    auto* band = new TestRasterBand(ds.get(), 1, GDT_Byte, 8, 8, 4, 4);
    ds->AddTestBand(band);
    double val = 0.0;
    EXPECT_EQ(CE_None, band->InterpolateAtPoint(2.5, 2.5, GRIORA_Bilinear, &val,
                                                nullptr));
  }
}

TEST(GDALRasterBandTest, RasterIOAndCWrappers) {
  auto ds = std::make_unique<TestDataset>(10, 10, GA_Update);
  auto* band = new TestRasterBand(ds.get(), 1, GDT_Byte, 10, 10, 5, 5, FALSE);
  ds->AddTestBand(band);
  GDALRasterBandH hBand = GDALRasterBand::ToHandle(band);

  std::array<GByte, 100> buf{};
  GDALRasterIOExtraArg extra;

  {
    WithQuietHandler quiet;
    CPLErrorReset();

    // Null hBand
    EXPECT_EQ(CE_Failure, GDALRasterIO(nullptr, GF_Read, 0, 0, 1, 1, buf.data(),
                                       1, 1, GDT_Byte, 0, 0));
    EXPECT_EQ(CE_Failure,
              GDALRasterIOEx(nullptr, GF_Read, 0, 0, 1, 1, buf.data(), 1, 1,
                             GDT_Byte, 0, 0, nullptr));

    // ExtraArg with invalid version
    INIT_RASTERIO_EXTRA_ARG(extra);
    extra.nVersion = RASTERIO_EXTRA_ARG_CURRENT_VERSION + 10;
    EXPECT_EQ(CE_Failure, band->RasterIO(GF_Read, 0, 0, 2, 2, buf.data(), 2, 2,
                                         GDT_Byte, 0, 0, &extra));

    // Null pData
    EXPECT_EQ(CE_Failure, band->RasterIO(GF_Read, 0, 0, 2, 2, nullptr, 2, 2,
                                         GDT_Byte, 0, 0, nullptr));

    // Zero/negative size -> returns CE_None with warning
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 0, 2, buf.data(), 2, 2,
                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 2, 0, buf.data(), 2, 2,
                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 2, 2, buf.data(), 0, 2,
                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 2, 2, buf.data(), 2, 0,
                                      GDT_Byte, 0, 0, nullptr));

    // Out of bounds window
    EXPECT_EQ(CE_Failure, band->RasterIO(GF_Read, -1, 0, 2, 2, buf.data(), 2, 2,
                                         GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_Failure, band->RasterIO(GF_Read, 0, -1, 2, 2, buf.data(), 2, 2,
                                         GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_Failure, band->RasterIO(GF_Read, 9, 0, 2, 2, buf.data(), 2, 2,
                                         GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_Failure, band->RasterIO(GF_Read, 0, 9, 2, 2, buf.data(), 2, 2,
                                         GDT_Byte, 0, 0, nullptr));

    // Invalid eBufType
    EXPECT_EQ(CE_Failure, band->RasterIO(GF_Read, 0, 0, 2, 2, buf.data(), 2, 2,
                                         GDT_Unknown, 0, 0, nullptr));
    EXPECT_EQ(CE_Failure, band->RasterIO(GF_Read, 0, 0, 2, 2, buf.data(), 2, 2,
                                         GDT_TypeCount, 0, 0, nullptr));

    CPLErrorReset();
  }

  // Valid write and read via GDALRasterIO and GDALRasterIOEx
  buf[0] = 42;
  buf[1] = 43;
  INIT_RASTERIO_EXTRA_ARG(extra);
  EXPECT_EQ(CE_None, GDALRasterIO(hBand, GF_Write, 0, 0, 2, 1, buf.data(), 2, 1,
                                  GDT_Byte, 0, 0));
  std::array<GByte, 2> read_buf{};
  EXPECT_EQ(CE_None, GDALRasterIOEx(hBand, GF_Read, 0, 0, 2, 1, read_buf.data(),
                                    2, 1, GDT_Byte, 1, 2, &extra));
  EXPECT_EQ(42, read_buf[0]);
  EXPECT_EQ(43, read_buf[1]);

  // bForceCachedIO = TRUE path
  auto* cached_band =
      new TestRasterBand(ds.get(), 2, GDT_Byte, 10, 10, 5, 5, TRUE);
  ds->AddTestBand(cached_band);
  EXPECT_EQ(CE_None, cached_band->RasterIO(GF_Write, 0, 0, 2, 1, buf.data(), 2,
                                           1, GDT_Byte, 0, 0, nullptr));
  read_buf[0] = 0;
  EXPECT_EQ(CE_None, cached_band->RasterIO(GF_Read, 0, 0, 2, 1, read_buf.data(),
                                           2, 1, GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(42, read_buf[0]);

  // Read-only write failure
  band->SetAccessMode(GA_ReadOnly);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band->RasterIO(GF_Write, 0, 0, 2, 1, buf.data(), 2, 1,
                                         GDT_Byte, 0, 0, nullptr));
    // Also with GMO_IGNORE_UNIMPLEMENTED
    band->SetMOFlags(band->GetMOFlags() | GMO_IGNORE_UNIMPLEMENTED);
    EXPECT_EQ(CE_Failure, band->RasterIO(GF_Write, 0, 0, 2, 1, buf.data(), 2, 1,
                                         GDT_Byte, 0, 0, nullptr));
    CPLErrorReset();
  }
  band->SetMOFlags(0);
  band->SetAccessMode(GA_Update);

  // eFlushBlockErr != CE_None on GF_Write
  band->fail_write_ = true;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterBlock* blk = band->GetLockedBlockRef(0, 0, TRUE);
    ASSERT_NE(nullptr, blk);
    blk->MarkDirty();
    blk->DropLock();
    GDALRasterBlock::FlushDirtyBlocks();
    EXPECT_EQ(CE_Failure, band->RasterIO(GF_Write, 0, 0, 2, 1, buf.data(), 2, 1,
                                         GDT_Byte, 0, 0, nullptr));
    CPLErrorReset();
  }
  band->fail_write_ = false;
}

TEST(GDALRasterBandTest, ReadRasterAllTypesAndEdgeCases) {
  auto ds = std::make_unique<TestDataset>(4, 4, GA_Update);
  auto* band = new TestRasterBand(ds.get(), 1, GDT_Byte, 4, 4, 4, 4);
  ds->AddTestBand(band);
  EXPECT_EQ(CE_None, band->Fill(7));

  auto test_type = [&](auto dummy) {
    using T = decltype(dummy);
    std::vector<T> vec;
    EXPECT_EQ(CE_None, band->ReadRaster(vec));
    EXPECT_EQ(16U, vec.size());

    std::array<T, 16> arr{};
    EXPECT_EQ(CE_None, band->ReadRaster(arr.data(), arr.size()));

    std::span<T> sp(arr);
    EXPECT_EQ(CE_None, band->ReadRaster(sp));
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
  std::array<uint8_t, 16> buf{};
  constexpr size_t kHuge = static_cast<size_t>(1) << 32;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              band->ReadRaster(buf.data(), buf.size(), 0, 0, 2, 2, kHuge, 2));
    EXPECT_EQ(CE_Failure,
              band->ReadRaster(buf.data(), buf.size(), -1.0, 0, 2, 2, 2, 2));
    EXPECT_EQ(CE_Failure,
              band->ReadRaster(buf.data(), buf.size(), 0, -1.0, 2, 2, 2, 2));
    EXPECT_EQ(CE_None,
              band->ReadRaster(buf.data(), buf.size(), 0, 0, 0.0, 2, 2, 2));
    EXPECT_EQ(CE_None,
              band->ReadRaster(buf.data(), buf.size(), 0, 0, 2, 0.0, 2, 2));
    EXPECT_EQ(CE_Failure,
              band->ReadRaster(buf.data(), buf.size(), 0, 0, 1.5, 2.0, 0, 0));
    EXPECT_EQ(CE_Failure,
              band->ReadRaster(buf.data(), buf.size(), 0, 0, 2.0, 1.5, 0, 0));
    EXPECT_EQ(CE_None,
              band->ReadRaster(buf.data(), buf.size(), 0, 0, 2, 2, 2, 0));
    EXPECT_EQ(CE_Failure, band->ReadRaster(buf.data(), 2, 0, 0, 4, 4, 4, 4));
    CPLErrorReset();
  }
  // Sub-pixel window with explicit nBufXSize/nBufYSize
  EXPECT_EQ(CE_None,
            band->ReadRaster(buf.data(), 4, 0.25, 0.25, 1.5, 1.5, 2, 2));

  // Edge cases for vector overload
  std::vector<uint8_t> vec;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band->ReadRaster(vec, 0, 0, 2, 2, kHuge, 2));
    EXPECT_EQ(CE_Failure, band->ReadRaster(vec, -1.0, 0, 2, 2, 2, 2));
    EXPECT_EQ(CE_Failure, band->ReadRaster(vec, 0, -1.0, 2, 2, 2, 2));
    EXPECT_EQ(CE_None, band->ReadRaster(vec, 0, 0, 0.0, 2, 2, 2));
    EXPECT_EQ(CE_None, band->ReadRaster(vec, 0, 0, 2, 0.0, 2, 2));
    EXPECT_EQ(CE_Failure, band->ReadRaster(vec, 0, 0, 1.5, 2.0, 0, 0));
    EXPECT_EQ(CE_Failure, band->ReadRaster(vec, 0, 0, 2.0, 1.5, 0, 0));
    EXPECT_EQ(CE_None, band->ReadRaster(vec, 0, 0, 2, 2, 2, 0));
    CPLErrorReset();
  }
  EXPECT_EQ(CE_None, band->ReadRaster(vec, 0.25, 0.25, 1.5, 1.5, 2, 2));
}

TEST(GDALRasterBandTest, BlockIOAndCacheManagement) {
  // 1. Invalid InitBlockInfo cases
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    {
      TestRasterBand bad_band;
      bad_band.SetDimsAndType(10, 10, 0, 10, GDT_Byte);
      std::array<GByte, 10> dummy{};
      EXPECT_EQ(CE_Failure, bad_band.ReadBlock(0, 0, dummy.data()));
      EXPECT_EQ(CE_Failure, bad_band.WriteBlock(0, 0, dummy.data()));
      EXPECT_EQ(nullptr, bad_band.GetLockedBlockRef(0, 0));
      int bx = 0, by = 0;
      bad_band.GetBlockSize(&bx, &by);
      bad_band.GetBlockSize(nullptr, nullptr);
    }
    {
      TestRasterBand bad_band;
      bad_band.SetDimsAndType(0, 10, 4, 4, GDT_Byte);
      EXPECT_EQ(FALSE, bad_band.InitBlockInfo());
    }
    {
      TestRasterBand bad_band;
      bad_band.SetDimsAndType(10, 10, 4, 4, GDT_Unknown);
      EXPECT_EQ(FALSE, bad_band.InitBlockInfo());
    }
    CPLErrorReset();
  }

  // 2. GDAL_BAND_BLOCK_CACHE configurations: HASHSET, ARRAY, AUTO with flag,
  // invalid
  {
    CPLConfigOptionSetter s1("GDAL_BAND_BLOCK_CACHE", "HASHSET", false);
    TestRasterBand b;
    EXPECT_EQ(TRUE, b.InitBlockInfo());
    EXPECT_EQ(TRUE, b.InitBlockInfo());  // already initialized
  }
  {
    CPLConfigOptionSetter s1("GDAL_BAND_BLOCK_CACHE", "ARRAY", false);
    TestRasterBand b;
    EXPECT_EQ(TRUE, b.InitBlockInfo());
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    CPLConfigOptionSetter s1("GDAL_BAND_BLOCK_CACHE", "INVALID_TYPE", false);
    TestRasterBand b;
    EXPECT_EQ(TRUE, b.InitBlockInfo());
    CPLErrorReset();
  }
  {
    TestDataset ds(16, 16, GA_Update);
    ds.SetOpenFlags(GDAL_OF_HASHSET_BLOCK_ACCESS);
    auto* b = new TestRasterBand(&ds, 1, GDT_Byte, 16, 16, 4, 4);
    ds.AddTestBand(b);
    EXPECT_EQ(TRUE, b->InitBlockInfo());
  }
  {
    // Many blocks (> 1000000) triggers HashSet in AUTO mode
    TestRasterBand b;
    b.SetDimsAndType(2000, 2000, 1, 1, GDT_Byte);
    b.storage_.clear();
    EXPECT_EQ(TRUE, b.InitBlockInfo());
  }

  // 3. ReadBlock / WriteBlock / GetActualBlockSize /
  // GetSuggestedBlockAccessPattern
  auto ds = std::make_unique<TestDataset>(10, 10, GA_Update);
  auto* band = new TestRasterBand(ds.get(), 1, GDT_Byte, 10, 10, 4, 4);
  ds->AddTestBand(band);
  GDALRasterBandH hBand = GDALRasterBand::ToHandle(band);

  EXPECT_EQ(GSBAP_UNKNOWN, band->GetSuggestedBlockAccessPattern());
  EXPECT_EQ(GDT_Unknown, GDALGetRasterDataType(nullptr));
  EXPECT_EQ(GDT_Byte, GDALGetRasterDataType(hBand));
  GDALGetBlockSize(nullptr, nullptr, nullptr);
  int bx = 0, by = 0;
  GDALGetBlockSize(hBand, &bx, &by);
  EXPECT_EQ(4, bx);
  EXPECT_EQ(4, by);

  std::array<GByte, 16> block_buf = {1, 2, 3, 4};
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALReadBlock(nullptr, 0, 0, block_buf.data()));
    EXPECT_EQ(CE_Failure, band->ReadBlock(-1, 0, block_buf.data()));
    EXPECT_EQ(CE_Failure, band->ReadBlock(0, -1, block_buf.data()));
    EXPECT_EQ(CE_Failure, band->ReadBlock(10, 0, block_buf.data()));
    EXPECT_EQ(CE_Failure, band->ReadBlock(0, 10, block_buf.data()));
    CPLErrorReset();
  }
  EXPECT_EQ(CE_None, GDALReadBlock(hBand, 0, 0, block_buf.data()));

  // Base IWriteBlock (not supported vs GMO_IGNORE_UNIMPLEMENTED)
  band->use_base_iwriteblock_ = true;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band->WriteBlock(0, 0, block_buf.data()));
    band->SetMOFlags(GMO_IGNORE_UNIMPLEMENTED);
    EXPECT_EQ(CE_Failure, band->WriteBlock(0, 0, block_buf.data()));
    CPLErrorReset();
  }
  band->SetMOFlags(0);
  band->use_base_iwriteblock_ = false;

  // WriteBlock error cases and valid case
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALWriteBlock(nullptr, 0, 0, block_buf.data()));
    EXPECT_EQ(CE_Failure, band->WriteBlock(-1, 0, block_buf.data()));
    EXPECT_EQ(CE_Failure, band->WriteBlock(0, -1, block_buf.data()));
    EXPECT_EQ(CE_Failure, band->WriteBlock(10, 0, block_buf.data()));
    EXPECT_EQ(CE_Failure, band->WriteBlock(0, 10, block_buf.data()));
    band->SetAccessMode(GA_ReadOnly);
    EXPECT_EQ(CE_Failure, band->WriteBlock(0, 0, block_buf.data()));
    CPLErrorReset();
  }
  band->SetAccessMode(GA_Update);
  block_buf[0] = 99;
  EXPECT_EQ(CE_None, GDALWriteBlock(hBand, 0, 0, block_buf.data()));

  // GetActualBlockSize
  int xvalid = 0, yvalid = 0;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALGetActualBlockSize(nullptr, 0, 0, &xvalid, &yvalid));
    EXPECT_EQ(CE_Failure, band->GetActualBlockSize(-1, 0, &xvalid, &yvalid));
    EXPECT_EQ(CE_Failure, band->GetActualBlockSize(0, -1, &xvalid, &yvalid));
    EXPECT_EQ(CE_Failure, band->GetActualBlockSize(3, 0, &xvalid, &yvalid));
    EXPECT_EQ(CE_Failure, band->GetActualBlockSize(0, 3, &xvalid, &yvalid));
    CPLErrorReset();
  }
  EXPECT_EQ(CE_None, GDALGetActualBlockSize(hBand, 0, 0, &xvalid, &yvalid));
  EXPECT_EQ(4, xvalid);
  EXPECT_EQ(4, yvalid);
  // Partial block at edge (block 2 covers pixels 8..9 -> 2 valid pixels)
  EXPECT_EQ(CE_None, band->GetActualBlockSize(2, 2, &xvalid, &yvalid));
  EXPECT_EQ(2, xvalid);
  EXPECT_EQ(2, yvalid);

  // 4. TryGetLockedBlockRef, GetLockedBlockRef, FlushBlock, FlushCache,
  // DropCache
  TestRasterBand uninit_band;
  EXPECT_EQ(nullptr, uninit_band.TryGetLockedBlockRef(0, 0));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, uninit_band.FlushBlock(0, 0));
    CPLErrorReset();
  }
  EXPECT_EQ(CE_None, uninit_band.DropCache());

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, band->TryGetLockedBlockRef(-1, 0));
    EXPECT_EQ(nullptr, band->TryGetLockedBlockRef(0, -1));
    EXPECT_EQ(nullptr, band->TryGetLockedBlockRef(10, 0));
    EXPECT_EQ(nullptr, band->TryGetLockedBlockRef(0, 10));
    CPLErrorReset();
  }
  EXPECT_EQ(nullptr, band->TryGetLockedBlockRef(0, 0));

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, band->GetLockedBlockRef(-1, 0));
    EXPECT_EQ(nullptr, band->GetLockedBlockRef(0, -1));
    EXPECT_EQ(nullptr, band->GetLockedBlockRef(10, 0));
    EXPECT_EQ(nullptr, band->GetLockedBlockRef(0, 10));
    CPLErrorReset();
  }

  // Cache miss -> loads block
  GDALRasterBlock* blk = band->GetLockedBlockRef(0, 0, FALSE);
  ASSERT_NE(nullptr, blk);
  blk->DropLock();

  // Cache hit via TryGetLockedBlockRef and
  // GetLockedBlockRef(bJustInitialize=TRUE)
  blk = band->TryGetLockedBlockRef(0, 0);
  ASSERT_NE(nullptr, blk);
  blk->DropLock();

  blk = band->GetLockedBlockRef(0, 0, TRUE);
  ASSERT_NE(nullptr, blk);
  blk->MarkDirty();
  EXPECT_TRUE(band->HasDirtyBlocks());
  EXPECT_TRUE(band->HasBlockCache());
  blk->DropLock();

  // Trigger global cache eviction to cover UnreferenceBlock &
  // AddBlockToFreeList; FlushCacheBlock writes back and evicts the dirty
  // block (0, 0).
  GDALFlushCacheBlock();
  EXPECT_FALSE(band->HasDirtyBlocks());
  EXPECT_EQ(nullptr, band->TryGetLockedBlockRef(0, 0));

  // FlushBlock edge cases
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band->FlushBlock(-1, 0));
    EXPECT_EQ(CE_Failure, band->FlushBlock(0, -1));
    EXPECT_EQ(CE_Failure, band->FlushBlock(10, 0));
    EXPECT_EQ(CE_Failure, band->FlushBlock(0, 10));
    CPLErrorReset();
  }
  EXPECT_EQ(CE_None, band->FlushBlock(0, 0));

  // Thrashing warning check in GetLockedBlockRef when nBlockReads ==
  // nBlocksPerRow * nBlocksPerColumn + 1
  band->SetBlockReads(3 * 3);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    blk = band->GetLockedBlockRef(1, 1, FALSE);
    CPLErrorReset();
  }
  ASSERT_NE(nullptr, blk);
  blk->DropLock();

  // IReadBlock failure in GetLockedBlockRef (without and with CPLError)
  band->fail_read_ = true;
  band->emit_error_on_fail_read_ = false;
  EXPECT_EQ(nullptr, band->GetLockedBlockRef(2, 0, FALSE));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    band->emit_error_on_fail_read_ = true;
    EXPECT_EQ(nullptr, band->GetLockedBlockRef(2, 1, FALSE));
    CPLErrorReset();
  }
  band->fail_read_ = false;

  // FlushCache and DropCache C wrappers and error propagation
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALFlushRasterCache(nullptr));
    EXPECT_EQ(CE_Failure, GDALDropRasterCache(nullptr));
    CPLErrorReset();
  }
  EXPECT_EQ(CE_None, GDALFlushRasterCache(hBand));
  EXPECT_EQ(CE_None, GDALDropRasterCache(hBand));

  // DropCache when eFlushBlockErr is set
  band->fail_write_ = true;
  blk = band->GetLockedBlockRef(0, 0, TRUE);
  ASSERT_NE(nullptr, blk);
  blk->MarkDirty();
  blk->DropLock();
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band->FlushBlock(0, 0, TRUE));
    EXPECT_EQ(CE_None, band->DropCache());
    CPLErrorReset();
  }
}

TEST(GDALRasterBandTest, FillRasterTests) {
  {
    WithQuietHandler quiet;
    CPLErrorReset();

    EXPECT_EQ(CE_Failure, GDALFillRaster(nullptr, 1.0, 0.0));

    // Read-only failure
    TestRasterBand ro_band;
    ro_band.SetAccessMode(GA_ReadOnly);
    EXPECT_EQ(CE_Failure, ro_band.Fill(5.0));

    // Invalid block info failure
    TestRasterBand bad_band;
    bad_band.SetDimsAndType(8, 8, 0, 8, GDT_Byte);
    EXPECT_EQ(CE_Failure, bad_band.Fill(5.0));

    CPLErrorReset();
  }

  // Real band Fill
  TestRasterBand byte_band;
  byte_band.SetDimsAndType(6, 6, 4, 4, GDT_Byte);
  EXPECT_EQ(CE_None,
            GDALFillRaster(GDALRasterBand::ToHandle(&byte_band), 123.0, 0.0));
  GByte v = 0;
  EXPECT_EQ(CE_None, byte_band.RasterIO(GF_Read, 5, 5, 1, 1, &v, 1, 1, GDT_Byte,
                                        0, 0, nullptr));
  EXPECT_EQ(123, v);

  // Complex band Fill
  TestRasterBand cfloat_band;
  cfloat_band.SetDimsAndType(4, 4, 4, 4, GDT_CFloat32);
  EXPECT_EQ(CE_None, cfloat_band.Fill(3.5, -2.5));
  std::array<float, 2> cv{};
  EXPECT_EQ(CE_None, cfloat_band.RasterIO(GF_Read, 1, 1, 1, 1, cv.data(), 1, 1,
                                          GDT_CFloat32, 0, 0, nullptr));
  EXPECT_FLOAT_EQ(3.5f, cv[0]);
  EXPECT_FLOAT_EQ(-2.5f, cv[1]);
}

TEST(GDALRasterBandTest, PropertiesMetadataNoDataMinMaxOverviews) {
  auto ds = std::make_unique<TestDataset>(16, 16, GA_Update);
  auto* band = new TestRasterBand(ds.get(), 1, GDT_Byte, 16, 16, 8, 8);
  ds->AddTestBand(band);
  GDALRasterBandH hBand = GDALRasterBand::ToHandle(band);

  // Access, XSize, YSize, Band, Dataset
  EXPECT_EQ(GA_ReadOnly, GDALGetRasterAccess(nullptr));
  EXPECT_EQ(GA_Update, GDALGetRasterAccess(hBand));
  EXPECT_EQ(GA_Update, band->GetAccess());

  EXPECT_EQ(0, GDALGetRasterBandXSize(nullptr));
  EXPECT_EQ(16, GDALGetRasterBandXSize(hBand));
  EXPECT_EQ(0, GDALGetRasterBandYSize(nullptr));
  EXPECT_EQ(16, GDALGetRasterBandYSize(hBand));
  EXPECT_EQ(0, GDALGetBandNumber(nullptr));
  EXPECT_EQ(1, GDALGetBandNumber(hBand));
  EXPECT_EQ(nullptr, GDALGetBandDataset(nullptr));
  EXPECT_EQ(GDALDataset::ToHandle(ds.get()), GDALGetBandDataset(hBand));

  // CategoryNames
  EXPECT_EQ(nullptr, GDALGetRasterCategoryNames(nullptr));
  EXPECT_EQ(nullptr, GDALGetRasterCategoryNames(hBand));
  const char* const cats[] = {"cat1", "cat2", nullptr};
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALSetRasterCategoryNames(nullptr, cats));
    EXPECT_EQ(CE_Failure, GDALSetRasterCategoryNames(hBand, cats));
    band->SetMOFlags(GMO_IGNORE_UNIMPLEMENTED);
    EXPECT_EQ(CE_Failure, GDALSetRasterCategoryNames(hBand, cats));
    CPLErrorReset();
  }
  band->SetMOFlags(0);

  // NoData getters and setters (base class unimplemented vs
  // GMO_IGNORE_UNIMPLEMENTED)
  int success = 1;
  EXPECT_DOUBLE_EQ(0.0, GDALGetRasterNoDataValue(nullptr, &success));
  EXPECT_DOUBLE_EQ(-1e10, GDALGetRasterNoDataValue(hBand, &success));
  EXPECT_EQ(FALSE, success);
  EXPECT_DOUBLE_EQ(-1e10, band->GetNoDataValue(nullptr));

  success = 1;
  EXPECT_EQ(std::numeric_limits<int64_t>::min(),
            GDALGetRasterNoDataValueAsInt64(nullptr, &success));
  EXPECT_EQ(std::numeric_limits<int64_t>::min(),
            GDALGetRasterNoDataValueAsInt64(hBand, &success));
  EXPECT_EQ(FALSE, success);

  success = 1;
  EXPECT_EQ(std::numeric_limits<uint64_t>::max(),
            GDALGetRasterNoDataValueAsUInt64(nullptr, &success));
  EXPECT_EQ(std::numeric_limits<uint64_t>::max(),
            GDALGetRasterNoDataValueAsUInt64(hBand, &success));
  EXPECT_EQ(FALSE, success);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALSetRasterNoDataValue(nullptr, 0.0));
    EXPECT_EQ(CE_Failure, GDALSetRasterNoDataValue(hBand, 0.0));
    EXPECT_EQ(CE_Failure, GDALSetRasterNoDataValueAsInt64(nullptr, 0));
    EXPECT_EQ(CE_Failure, GDALSetRasterNoDataValueAsInt64(hBand, 0));
    EXPECT_EQ(CE_Failure, GDALSetRasterNoDataValueAsUInt64(nullptr, 0));
    EXPECT_EQ(CE_Failure, GDALSetRasterNoDataValueAsUInt64(hBand, 0));
    EXPECT_EQ(CE_Failure, GDALDeleteRasterNoDataValue(nullptr));
    EXPECT_EQ(CE_Failure, GDALDeleteRasterNoDataValue(hBand));

    band->SetMOFlags(GMO_IGNORE_UNIMPLEMENTED);
    EXPECT_EQ(CE_Failure, GDALSetRasterNoDataValue(hBand, 0.0));
    EXPECT_EQ(CE_Failure, GDALSetRasterNoDataValueAsInt64(hBand, 0));
    EXPECT_EQ(CE_Failure, GDALSetRasterNoDataValueAsUInt64(hBand, 0));
    EXPECT_EQ(CE_Failure, GDALDeleteRasterNoDataValue(hBand));
    CPLErrorReset();
  }
  band->SetMOFlags(0);

  // SetNoDataValueAsString across types
  band->allow_set_nodata_ = true;
  bool cannot_rep = false;

  // GDT_Int64
  band->SetDimsAndType(4, 4, 4, 4, GDT_Int64);
  EXPECT_EQ(CE_None, band->SetNoDataValueAsString("-12345", &cannot_rep));
  EXPECT_FALSE(cannot_rep);
  EXPECT_EQ(CE_None, band->SetNoDataValueAsString("100.0", &cannot_rep));
  EXPECT_FALSE(cannot_rep);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band->SetNoDataValueAsString("100.5", &cannot_rep));
    EXPECT_TRUE(cannot_rep);
    EXPECT_EQ(CE_Failure, band->SetNoDataValueAsString(
                              "99999999999999999999999999999", &cannot_rep));
    EXPECT_TRUE(cannot_rep);
    EXPECT_EQ(CE_Failure,
              band->SetNoDataValueAsString("not_a_number", &cannot_rep));
    CPLErrorReset();
  }

  // GDT_UInt64
  band->SetDimsAndType(4, 4, 4, 4, GDT_UInt64);
  EXPECT_EQ(CE_None, band->SetNoDataValueAsString("12345", &cannot_rep));
  EXPECT_FALSE(cannot_rep);
  EXPECT_EQ(CE_None, band->SetNoDataValueAsString("100.0", &cannot_rep));
  EXPECT_FALSE(cannot_rep);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band->SetNoDataValueAsString("100.5", &cannot_rep));
    EXPECT_TRUE(cannot_rep);
    CPLErrorReset();
  }
  EXPECT_EQ(CE_None, band->SetNoDataValueAsString("-5", &cannot_rep));
  EXPECT_FALSE(cannot_rep);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band->SetNoDataValueAsString(
                              "99999999999999999999999999999", &cannot_rep));
    EXPECT_TRUE(cannot_rep);
    EXPECT_EQ(CE_Failure,
              band->SetNoDataValueAsString("not_a_number", &cannot_rep));
    CPLErrorReset();
  }

  // GDT_Float32
  band->SetDimsAndType(4, 4, 4, 4, GDT_Float32);
  EXPECT_EQ(CE_None, band->SetNoDataValueAsString("1.5", &cannot_rep));
  EXPECT_FALSE(cannot_rep);
  EXPECT_EQ(CE_None, band->SetNoDataValueAsString("nan", &cannot_rep));
  EXPECT_FALSE(cannot_rep);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              band->SetNoDataValueAsString("invalid_float", &cannot_rep));
    CPLErrorReset();
  }

  // GDT_Byte and GDT_Float64
  band->SetDimsAndType(4, 4, 4, 4, GDT_Byte);
  EXPECT_EQ(CE_None, band->SetNoDataValueAsString("200", &cannot_rep));
  EXPECT_FALSE(cannot_rep);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band->SetNoDataValueAsString("300", &cannot_rep));
    EXPECT_TRUE(cannot_rep);
    EXPECT_EQ(CE_Failure,
              band->SetNoDataValueAsString("invalid_byte", nullptr));
    CPLErrorReset();
  }

  band->SetDimsAndType(4, 4, 4, 4, GDT_Float64);
  EXPECT_EQ(CE_None, band->SetNoDataValueAsString("123.456", &cannot_rep));
  EXPECT_FALSE(cannot_rep);

  // GetMinimum and GetMaximum across all data types
  EXPECT_DOUBLE_EQ(0.0, GDALGetRasterMinimum(nullptr, &success));
  EXPECT_DOUBLE_EQ(0.0, GDALGetRasterMaximum(nullptr, &success));

  constexpr auto all_types_with_unknown = std::to_array<GDALDataType>({
      GDT_Byte,
      GDT_Int8,
      GDT_UInt16,
      GDT_Int16,
      GDT_CInt16,
      GDT_Int32,
      GDT_CInt32,
      GDT_UInt32,
      GDT_Int64,
      GDT_UInt64,
      GDT_Float16,
      GDT_CFloat16,
      GDT_Float32,
      GDT_CFloat32,
      GDT_Float64,
      GDT_CFloat64,
      GDT_Unknown,
  });
  for (GDALDataType dt : all_types_with_unknown) {
    band->SetDimsAndType(4, 4, 4, 4, dt);
    int s_min = 1, s_max = 1;
    (void)GDALGetRasterMinimum(hBand, &s_min);
    (void)GDALGetRasterMaximum(hBand, &s_max);
    EXPECT_EQ(FALSE, s_min);
    EXPECT_EQ(FALSE, s_max);
    (void)band->GetMinimum(nullptr);
    (void)band->GetMaximum(nullptr);
  }

  // GDT_Byte with PIXELTYPE=SIGNEDBYTE
  band->SetDimsAndType(4, 4, 4, 4, GDT_Byte);
  band->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE");
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_DOUBLE_EQ(-128.0, band->GetMinimum(&success));
    EXPECT_EQ(FALSE, success);
    EXPECT_DOUBLE_EQ(127.0, band->GetMaximum(&success));
    EXPECT_EQ(FALSE, success);
    CPLErrorReset();
  }
  band->SetMetadata(nullptr, "IMAGE_STRUCTURE");

  // With STATISTICS_MINIMUM and STATISTICS_MAXIMUM metadata
  band->SetMetadataItem("STATISTICS_MINIMUM", "10.5");
  band->SetMetadataItem("STATISTICS_MAXIMUM", "90.5");
  EXPECT_DOUBLE_EQ(10.5, GDALGetRasterMinimum(hBand, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_DOUBLE_EQ(90.5, GDALGetRasterMaximum(hBand, &success));
  EXPECT_EQ(TRUE, success);
  band->SetMetadata(nullptr);

  // ColorInterpretation & ColorTable
  EXPECT_EQ(GCI_Undefined, GDALGetRasterColorInterpretation(nullptr));
  EXPECT_EQ(GCI_Undefined, GDALGetRasterColorInterpretation(hBand));
  EXPECT_EQ(nullptr, GDALGetRasterColorTable(nullptr));
  EXPECT_EQ(nullptr, GDALGetRasterColorTable(hBand));

  // Offset, Scale, UnitType getters
  EXPECT_DOUBLE_EQ(0.0, GDALGetRasterOffset(nullptr, &success));
  EXPECT_DOUBLE_EQ(0.0, GDALGetRasterOffset(hBand, &success));
  EXPECT_EQ(FALSE, success);
  EXPECT_DOUBLE_EQ(0.0, band->GetOffset(nullptr));

  EXPECT_DOUBLE_EQ(0.0, GDALGetRasterScale(nullptr, &success));
  EXPECT_DOUBLE_EQ(1.0, GDALGetRasterScale(hBand, &success));
  EXPECT_EQ(FALSE, success);
  EXPECT_DOUBLE_EQ(1.0, band->GetScale(nullptr));

  EXPECT_EQ(nullptr, GDALGetRasterUnitType(nullptr));
  EXPECT_STREQ("", GDALGetRasterUnitType(hBand));

  // Setters for ColorInterpretation, ColorTable, Offset, Scale, UnitType
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALSetRasterColorInterpretation(nullptr, GCI_RedBand));
    EXPECT_EQ(CE_Failure, GDALSetRasterColorInterpretation(hBand, GCI_RedBand));
    EXPECT_EQ(CE_Failure, GDALSetRasterColorTable(nullptr, nullptr));
    EXPECT_EQ(CE_Failure, GDALSetRasterColorTable(hBand, nullptr));
    EXPECT_EQ(CE_Failure, GDALSetRasterOffset(nullptr, 1.0));
    EXPECT_EQ(CE_Failure, GDALSetRasterOffset(hBand, 1.0));
    EXPECT_EQ(CE_Failure, GDALSetRasterScale(nullptr, 2.0));
    EXPECT_EQ(CE_Failure, GDALSetRasterScale(hBand, 2.0));
    EXPECT_EQ(CE_Failure, GDALSetRasterUnitType(nullptr, "m"));
    EXPECT_EQ(CE_Failure, GDALSetRasterUnitType(hBand, "m"));

    band->SetMOFlags(GMO_IGNORE_UNIMPLEMENTED);
    EXPECT_EQ(CE_Failure, GDALSetRasterColorInterpretation(hBand, GCI_RedBand));
    EXPECT_EQ(CE_Failure, GDALSetRasterColorTable(hBand, nullptr));
    EXPECT_EQ(CE_Failure, GDALSetRasterOffset(hBand, 1.0));
    EXPECT_EQ(CE_Failure, GDALSetRasterScale(hBand, 2.0));
    EXPECT_EQ(CE_Failure, GDALSetRasterUnitType(hBand, "m"));
    CPLErrorReset();
  }
  band->SetMOFlags(0);

  // Overviews & GetRasterSampleOverview
  EXPECT_EQ(0, GDALHasArbitraryOverviews(nullptr));
  EXPECT_EQ(0, GDALHasArbitraryOverviews(hBand));
  EXPECT_EQ(0, GDALGetOverviewCount(nullptr));
  EXPECT_EQ(0, GDALGetOverviewCount(hBand));
  EXPECT_EQ(nullptr, GDALGetOverview(nullptr, 0));
  EXPECT_EQ(nullptr, GDALGetOverview(hBand, 0));
  EXPECT_EQ(nullptr, GDALGetRasterSampleOverview(nullptr, 100));
  EXPECT_EQ(nullptr, GDALGetRasterSampleOverviewEx(nullptr, 100));
  EXPECT_EQ(hBand, GDALGetRasterSampleOverview(hBand, -1));
  EXPECT_EQ(hBand, GDALGetRasterSampleOverviewEx(hBand, 0));

  // Add synthetic overviews to test GetRasterSampleOverview selection logic
  band->SetDimsAndType(100, 100, 10, 10, GDT_Byte);
  TestRasterBand ovr1(nullptr, 1, GDT_Byte, 50, 50, 10, 10);
  TestRasterBand ovr2(nullptr, 1, GDT_Byte, 20, 20, 10, 10);
  band->overviews_ = {&ovr1, &ovr2};
  EXPECT_EQ(2, GDALGetOverviewCount(hBand));
  EXPECT_EQ(nullptr, GDALGetOverview(hBand, -1));
  EXPECT_EQ(nullptr, GDALGetOverview(hBand, 2));
  EXPECT_EQ(GDALRasterBand::ToHandle(&ovr1), GDALGetOverview(hBand, 0));
  EXPECT_EQ(GDALRasterBand::ToHandle(&ovr1),
            GDALGetRasterSampleOverview(hBand, 1000));
  EXPECT_EQ(GDALRasterBand::ToHandle(&ovr2),
            GDALGetRasterSampleOverviewEx(hBand, 100));
  EXPECT_EQ(hBand, GDALGetRasterSampleOverviewEx(hBand, 9000));
  band->overviews_.clear();

  // BuildOverviews on base band
  constexpr std::array<int, 1> ovr_list = {2};
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              band->BuildOverviews("NEAREST", ovr_list.size(), ovr_list.data(),
                                   nullptr, nullptr, nullptr));
    CPLErrorReset();
  }

  // AdviseRead
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALRasterAdviseRead(nullptr, 0, 0, 4, 4, 4, 4,
                                               GDT_Byte, nullptr));
    CPLErrorReset();
  }
  EXPECT_EQ(CE_None,
            GDALRasterAdviseRead(hBand, 0, 0, 4, 4, 4, 4, GDT_Byte, nullptr));
}

TEST(GDALRasterBandTest, HistogramAndDefaultHistogramAllPaths) {
  auto ds = std::make_unique<TestDataset>(8, 8, GA_Update);
  auto* band = new TestRasterBand(ds.get(), 1, GDT_Byte, 8, 8, 4, 4);
  ds->AddTestBand(band);
  GDALRasterBandH hBand = GDALRasterBand::ToHandle(band);

  std::array<GUIntBig, 256> hist{};

  {
    WithQuietHandler quiet;
    CPLErrorReset();

    // Null checks
    EXPECT_EQ(CE_Failure,
              GDALGetRasterHistogramEx(nullptr, 0, 255, 256, hist.data(), TRUE,
                                       FALSE, nullptr, nullptr));
    EXPECT_EQ(CE_Failure,
              GDALGetRasterHistogramEx(hBand, 0, 255, 256, nullptr, TRUE, FALSE,
                                       nullptr, nullptr));
    EXPECT_EQ(CE_Failure,
              GDALGetRasterHistogramEx(hBand, 0, 255, 0, hist.data(), TRUE,
                                       FALSE, nullptr, nullptr));

    // Invalid dfMin >= dfMax or non-finite dfScale
    EXPECT_EQ(CE_Failure, band->GetHistogram(10.0, 5.0, 10, hist.data(), TRUE,
                                             FALSE, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, band->GetHistogram(-1e308, 1e308, 10, hist.data(),
                                             TRUE, FALSE, nullptr, nullptr));

    // Progress cancel at start and midway
    EXPECT_EQ(CE_Failure,
              band->GetHistogram(-0.5, 255.5, 256, hist.data(), TRUE, FALSE,
                                 CancelAtStartProgress, nullptr));
    int calls = 0;
    EXPECT_EQ(CE_Failure,
              band->GetHistogram(-0.5, 255.5, 10, hist.data(), TRUE, FALSE,
                                 CancelMidwayProgress, &calls));

    CPLErrorReset();
  }

  // Overview delegation when bApproxOK=TRUE and GetOverviewCount() > 0
  TestRasterBand ovr(nullptr, 1, GDT_Byte, 4, 4, 4, 4);
  EXPECT_EQ(CE_None, ovr.Fill(10));
  band->overviews_ = {&ovr};
  EXPECT_EQ(CE_None, band->GetHistogram(-0.5, 255.5, 256, hist.data(), TRUE,
                                        TRUE, nullptr, nullptr));
  EXPECT_EQ(16U, hist[10]);
  band->overviews_.clear();

  // Fast path: GDT_Byte, !bSignedByte, dfScale == 1.0, dfMin == -0.5, nBuckets
  // == 256
  for (int i = 0; i < 64; ++i) {
    band->storage_[i] = static_cast<GByte>(i * 4);
  }
  band->FlushCache();
  band->has_nodata_ = true;
  band->nodata_val_ = 0.0;
  EXPECT_EQ(CE_None, band->GetHistogram(-0.5, 255.5, 256, hist.data(), TRUE,
                                        FALSE, nullptr, nullptr));
  EXPECT_EQ(0U, hist[0]);
  EXPECT_EQ(1U, hist[4]);

  // Fast path with custom mask band
  TestRasterBand mask_band(ds.get(), 1, GDT_Byte, 8, 8, 4, 4);
  EXPECT_EQ(CE_None, mask_band.Fill(255));
  mask_band.storage_[1] = 0;  // mask out pixel 1
  band->has_nodata_ = false;
  band->custom_mask_band_ = &mask_band;
  band->custom_mask_flags_ = GMF_PER_DATASET;
  EXPECT_EQ(CE_None, band->GetHistogram(-0.5, 255.5, 256, hist.data(), TRUE,
                                        FALSE, nullptr, nullptr));
  EXPECT_EQ(1U, hist[4]);

  // Populate diverse values (below min, in range, above max, nodata)
  constexpr auto kVals = std::to_array<double>({1.0, 5.0, 15.0, 99.0});
  for (GDALDataType dt : kAllDataTypes) {
    auto loop_ds = std::make_unique<TestDataset>(8, 8, GA_Update);
    auto* loop_band = new TestRasterBand(loop_ds.get(), 1, dt, 8, 8, 4, 4);
    loop_ds->AddTestBand(loop_band);
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 8; ++x) {
        double v = kVals[(x + y) % kVals.size()];
        EXPECT_EQ(CE_None, loop_band->RasterIO(GF_Write, x, y, 1, 1, &v, 1, 1,
                                               GDT_Float64, 0, 0, nullptr));
      }
    }
    loop_band->FlushCache();

    loop_band->has_nodata_ = true;
    loop_band->nodata_val_ = 99.0;
    loop_band->has_nodata_i64_ = true;
    loop_band->nodata_i64_ = 99;
    loop_band->has_nodata_u64_ = true;
    loop_band->nodata_u64_ = 99U;

    std::array<GUIntBig, 10> h10{};
    // 1. Arbitrary overviews path (bApproxOK=TRUE,
    // HasArbitraryOverviews()=TRUE)
    loop_band->arbitrary_overviews_ = true;
    for (int inc_out : {TRUE, FALSE}) {
      EXPECT_EQ(CE_None,
                loop_band->GetHistogram(2.0, 12.0, 10, h10.data(), inc_out,
                                        TRUE, nullptr, nullptr));
    }

    // 2. General block path (bApproxOK=FALSE)
    loop_band->arbitrary_overviews_ = false;
    for (int inc_out : {TRUE, FALSE}) {
      EXPECT_EQ(CE_None,
                loop_band->GetHistogram(2.0, 12.0, 10, h10.data(), inc_out,
                                        FALSE, nullptr, nullptr));
    }

    // Also GDT_Byte with PIXELTYPE=SIGNEDBYTE
    if (dt == GDT_Byte) {
      WithQuietHandler quiet;
      CPLErrorReset();
      loop_band->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE");
      loop_band->arbitrary_overviews_ = true;
      EXPECT_EQ(CE_None, loop_band->GetHistogram(-10.0, 20.0, 10, h10.data(),
                                                 TRUE, TRUE, nullptr, nullptr));
      loop_band->arbitrary_overviews_ = false;
      EXPECT_EQ(CE_None,
                loop_band->GetHistogram(-10.0, 20.0, 10, h10.data(), TRUE,
                                        FALSE, nullptr, nullptr));
      CPLErrorReset();
    }
  }

  // Deprecated GDALGetRasterHistogram wrapper
  TestRasterBand dep_band(nullptr, 1, GDT_Byte, 4, 4, 4, 4);
  EXPECT_EQ(CE_None, dep_band.Fill(5));
  std::array<int, 10> int_hist{};
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALGetRasterHistogram(nullptr, 0, 10, 10, int_hist.data(), TRUE,
                                     FALSE, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, GDALGetRasterHistogram(
                              GDALRasterBand::ToHandle(&dep_band), 0, 10, 10,
                              nullptr, TRUE, FALSE, nullptr, nullptr));
    CPLErrorReset();
  }
  EXPECT_EQ(CE_None, GDALGetRasterHistogram(GDALRasterBand::ToHandle(&dep_band),
                                            0, 10, 10, int_hist.data(), TRUE,
                                            FALSE, nullptr, nullptr));
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

  // GetDefaultHistogram & GDALGetDefaultHistogramEx & GDALGetDefaultHistogram
  double hmin = 0.0, hmax = 0.0;
  int nbuckets = 0;

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GUIntBig* def_hist_null = nullptr;
    EXPECT_EQ(CE_Failure, GDALGetDefaultHistogramEx(nullptr, &hmin, &hmax,
                                                    &nbuckets, &def_hist_null,
                                                    TRUE, nullptr, nullptr));
    // bForce = FALSE -> CE_Warning
    EXPECT_EQ(CE_Warning, GDALGetDefaultHistogramEx(hBand, &hmin, &hmax,
                                                    &nbuckets, &def_hist_null,
                                                    FALSE, nullptr, nullptr));
    CPLErrorReset();
  }

  // bForce = TRUE on GDT_Byte
  {
    GUIntBig* def_hist_raw = nullptr;
    EXPECT_EQ(CE_None,
              GDALGetDefaultHistogramEx(hBand, &hmin, &hmax, &nbuckets,
                                        &def_hist_raw, TRUE, nullptr, nullptr));
    std::unique_ptr<GUIntBig, decltype(&VSIFree)> def_hist(def_hist_raw,
                                                           VSIFree);
    EXPECT_EQ(256, nbuckets);
  }

  // bForce = TRUE on GDT_Int8
  band->SetDimsAndType(4, 4, 4, 4, GDT_Int8);
  EXPECT_EQ(CE_None, band->Fill(-5));
  {
    GUIntBig* def_hist_raw = nullptr;
    EXPECT_EQ(CE_None,
              band->GetDefaultHistogram(&hmin, &hmax, &nbuckets, &def_hist_raw,
                                        TRUE, nullptr, nullptr));
    std::unique_ptr<GUIntBig, decltype(&VSIFree)> def_hist(def_hist_raw,
                                                           VSIFree);
    EXPECT_EQ(256, nbuckets);
  }

  // bForce = TRUE on GDT_Float32 with constant value (*pdfMin == *pdfMax -> 1
  // bucket)
  band->SetDimsAndType(4, 4, 4, 4, GDT_Float32);
  EXPECT_EQ(CE_None, band->Fill(42.0));
  {
    GUIntBig* def_hist_raw = nullptr;
    EXPECT_EQ(CE_None,
              band->GetDefaultHistogram(&hmin, &hmax, &nbuckets, &def_hist_raw,
                                        TRUE, nullptr, nullptr));
    std::unique_ptr<GUIntBig, decltype(&VSIFree)> def_hist(def_hist_raw,
                                                           VSIFree);
    EXPECT_EQ(1, nbuckets);
  }

  // bForce = TRUE on GDT_Float32 with varying values (*pdfMin < *pdfMax -> 256
  // buckets); clear cached statistics metadata from the constant fill first.
  band->SetMetadata(nullptr);
  std::array<float, 2> fvals = {10.0f, 20.0f};
  EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 2, 1, fvals.data(), 2, 1,
                                    GDT_Float32, 0, 0, nullptr));
  band->FlushCache();
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    int* def_hist_null = nullptr;
    EXPECT_EQ(CE_Failure,
              GDALGetDefaultHistogram(nullptr, &hmin, &hmax, &nbuckets,
                                      &def_hist_null, TRUE, nullptr, nullptr));
    CPLErrorReset();
  }
  {
    int* def_hist_int_raw = nullptr;
    EXPECT_EQ(CE_None, GDALGetDefaultHistogram(hBand, &hmin, &hmax, &nbuckets,
                                               &def_hist_int_raw, TRUE, nullptr,
                                               nullptr));
    std::unique_ptr<int, decltype(&VSIFree)> def_hist_int(def_hist_int_raw,
                                                          VSIFree);
    EXPECT_EQ(256, nbuckets);
  }

  // SetDefaultHistogram & C wrappers
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALSetDefaultHistogram(nullptr, 0, 10, 10, int_hist.data()));
    EXPECT_EQ(CE_Failure,
              GDALSetDefaultHistogram(hBand, 0, 10, 10, int_hist.data()));
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    EXPECT_EQ(CE_Failure,
              GDALSetDefaultHistogramEx(nullptr, 0, 10, 10, hist.data()));
    EXPECT_EQ(CE_Failure,
              GDALSetDefaultHistogramEx(hBand, 0, 10, 10, hist.data()));
    band->SetMOFlags(GMO_IGNORE_UNIMPLEMENTED);
    EXPECT_EQ(CE_Failure,
              GDALSetDefaultHistogramEx(hBand, 0, 10, 10, hist.data()));
    CPLErrorReset();
  }
  band->SetMOFlags(0);
}

TEST(GDALRasterBandTest, StatisticsAllPaths) {
  auto ds = std::make_unique<TestDataset>(40, 4, GA_Update);
  auto* band = new TestRasterBand(ds.get(), 1, GDT_Byte, 40, 4, 32, 2);
  ds->AddTestBand(band);
  GDALRasterBandH hBand = GDALRasterBand::ToHandle(band);

  double smin = 0, smax = 0, smean = 0, sstddev = 0;

  // Null checks
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALGetRasterStatistics(nullptr, TRUE, TRUE, &smin,
                                                  &smax, &smean, &sstddev));
    EXPECT_EQ(CE_Failure,
              GDALComputeRasterStatistics(nullptr, TRUE, &smin, &smax, &smean,
                                          &sstddev, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, GDALSetRasterStatistics(nullptr, 1, 2, 1.5, 0.5));
    CPLErrorReset();
  }

  // SetValidPercent edge cases
  struct ValidPercentCase {
    GIntBig total;
    GIntBig valid;
    std::string_view expected;
  };
  constexpr auto kValidPercentCases = std::to_array<ValidPercentCase>({
      {.total = 100, .valid = 0, .expected = "0"},
      {.total = 100000, .valid = 99999, .expected = "99.999"},
      {.total = 100, .valid = 50, .expected = "50"},
      {.total = 100, .valid = 100, .expected = "100"},
  });
  for (const auto& tc : kValidPercentCases) {
    band->SetValidPercent(tc.total, tc.valid);
    const char* item = band->GetMetadataItem("STATISTICS_VALID_PERCENT");
    ASSERT_NE(nullptr, item);
    EXPECT_EQ(tc.expected, std::string_view(item));
  }
  band->SetMetadata(nullptr);

  // GetStatistics with SetStatistics metadata
  EXPECT_EQ(CE_None, GDALSetRasterStatistics(hBand, 1.0, 9.0, 5.0, 2.0));
  EXPECT_EQ(CE_None, GDALGetRasterStatistics(hBand, FALSE, FALSE, &smin, &smax,
                                             &smean, &sstddev));
  EXPECT_DOUBLE_EQ(1.0, smin);
  EXPECT_DOUBLE_EQ(9.0, smax);
  EXPECT_DOUBLE_EQ(5.0, smean);
  EXPECT_DOUBLE_EQ(2.0, sstddev);

  // With STATISTICS_APPROXIMATE=YES and bApproxOK=FALSE vs TRUE
  band->SetMetadataItem("STATISTICS_APPROXIMATE", "YES");
  EXPECT_EQ(CE_None,
            band->GetStatistics(TRUE, FALSE, &smin, &smax, &smean, &sstddev));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Warning, band->GetStatistics(FALSE, FALSE, &smin, &smax,
                                              &smean, &sstddev));
    CPLErrorReset();
  }
  band->SetMetadata(nullptr);

  // GetStatistics with bApproxOK=TRUE, pdfMean=nullptr, padfStdDev=nullptr and
  // custom min/max
  band->has_custom_min_ = true;
  band->custom_min_ = 3.0;
  band->has_custom_max_ = true;
  band->custom_max_ = 7.0;
  EXPECT_EQ(CE_None,
            band->GetStatistics(TRUE, FALSE, &smin, &smax, nullptr, nullptr));
  EXPECT_DOUBLE_EQ(3.0, smin);
  EXPECT_DOUBLE_EQ(7.0, smax);
  band->has_custom_min_ = false;
  band->has_custom_max_ = false;

  // GetStatistics with bForce=FALSE -> CE_Warning, bForce=TRUE -> computes
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Warning, band->GetStatistics(FALSE, FALSE, &smin, &smax,
                                              &smean, &sstddev));
    CPLErrorReset();
  }
  EXPECT_EQ(CE_None, band->Fill(10));
  EXPECT_EQ(CE_None,
            band->GetStatistics(FALSE, TRUE, &smin, &smax, &smean, &sstddev));
  EXPECT_DOUBLE_EQ(10.0, smin);

  // Overview delegation in ComputeStatistics
  {
    auto ds_ovr = std::make_unique<TestDataset>(200, 200, GA_Update);
    auto* b_ovr_main =
        new TestRasterBand(ds_ovr.get(), 1, GDT_Byte, 200, 200, 50, 50);
    ds_ovr->AddTestBand(b_ovr_main);
    TestRasterBand ovr(nullptr, 1, GDT_Byte, 60, 60, 60, 60);
    std::fill(ovr.storage_.begin(), ovr.storage_.end(), 25);
    b_ovr_main->overviews_ = {&ovr};
    EXPECT_EQ(CE_None,
              b_ovr_main->ComputeStatistics(TRUE, &smin, &smax, &smean,
                                            &sstddev, nullptr, nullptr));
    EXPECT_DOUBLE_EQ(25.0, smin);
    b_ovr_main->overviews_.clear();
  }

  // Progress cancellation
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              band->ComputeStatistics(FALSE, &smin, &smax, &smean, &sstddev,
                                      CancelAtStartProgress, nullptr));
    int calls = 0;
    EXPECT_EQ(CE_Failure,
              band->ComputeStatistics(FALSE, &smin, &smax, &smean, &sstddev,
                                      CancelMidwayProgress, &calls));
    CPLErrorReset();
  }

  // Fast GDT_Byte integer stats paths:
  // 1) Block 0 has [0, 255] -> Block 1 hits nMin==0 && nMax==255
  EXPECT_EQ(CE_None, band->DropCache());
  band->SetMetadata(nullptr);
  std::fill(band->storage_.begin(), band->storage_.end(), 50);
  band->storage_[0] = 0;
  band->storage_[1] = 255;
  EXPECT_EQ(CE_None, band->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                             &sstddev, nullptr, nullptr));
  EXPECT_DOUBLE_EQ(0.0, smin);
  EXPECT_DOUBLE_EQ(255.0, smax);

  // 2) Block 0 has [0, 100] -> Block 1 hits nMin==0 && nMax<255
  EXPECT_EQ(CE_None, band->DropCache());
  band->SetMetadata(nullptr);
  std::fill(band->storage_.begin(), band->storage_.end(), 50);
  band->storage_[0] = 0;
  band->storage_[1] = 100;
  EXPECT_EQ(CE_None, band->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                             &sstddev, nullptr, nullptr));

  // 3) Block 0 has [10, 255] -> Block 1 hits nMin>0 && nMax==255
  EXPECT_EQ(CE_None, band->DropCache());
  band->SetMetadata(nullptr);
  std::fill(band->storage_.begin(), band->storage_.end(), 50);
  band->storage_[0] = 10;
  band->storage_[1] = 255;
  EXPECT_EQ(CE_None, band->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                             &sstddev, nullptr, nullptr));

  // 4) With nodata on GDT_Byte
  EXPECT_EQ(CE_None, band->DropCache());
  band->SetMetadata(nullptr);
  band->has_nodata_ = true;
  band->nodata_val_ = 50.0;
  EXPECT_EQ(CE_None, band->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                             &sstddev, nullptr, nullptr));

  // 5) All nodata on GDT_Byte -> CE_Failure
  EXPECT_EQ(CE_None, band->Fill(50));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                                  &sstddev, nullptr, nullptr));
    CPLErrorReset();
  }
  band->has_nodata_ = false;

  // 6) GDAL_STATS_USE_INTEGER_STATS = NO on GDT_Byte
  {
    CPLConfigOptionSetter s("GDAL_STATS_USE_INTEGER_STATS", "NO", false);
    EXPECT_EQ(CE_None, band->Fill(20));
    band->storage_[0] = 10;
    band->FlushCache();
    EXPECT_EQ(CE_None, band->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                               &sstddev, nullptr, nullptr));
  }

  // Fast GDT_UInt16 integer stats paths (without nodata, with nodata, and with
  // INTEGER_STATS=NO)
  {
    auto ds_u16 = std::make_unique<TestDataset>(40, 4, GA_Update);
    auto* band_u16 =
        new TestRasterBand(ds_u16.get(), 1, GDT_UInt16, 40, 4, 32, 2);
    ds_u16->AddTestBand(band_u16);
    EXPECT_EQ(CE_None, band_u16->Fill(100));
    std::array<uint16_t, 2> u16_vals = {10, 500};
    EXPECT_EQ(CE_None, band_u16->RasterIO(GF_Write, 0, 0, 2, 1, u16_vals.data(),
                                          2, 1, GDT_UInt16, 0, 0, nullptr));
    band_u16->FlushCache();
    EXPECT_EQ(CE_None, band_u16->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                                   &sstddev, nullptr, nullptr));
    band_u16->has_nodata_ = true;
    band_u16->nodata_val_ = 100.0;
    EXPECT_EQ(CE_None, band_u16->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                                   &sstddev, nullptr, nullptr));
    {
      CPLConfigOptionSetter s("GDAL_STATS_USE_INTEGER_STATS", "NO", false);
      EXPECT_EQ(CE_None,
                band_u16->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                            &sstddev, nullptr, nullptr));
    }
  }

  // GDT_Float32 SSE2/AVX2 and generic paths
  {
    auto ds_f32 = std::make_unique<TestDataset>(40, 4, GA_Update);
    auto* band_f32 =
        new TestRasterBand(ds_f32.get(), 1, GDT_Float32, 40, 4, 32, 2);
    ds_f32->AddTestBand(band_f32);
    EXPECT_EQ(CE_None, band_f32->Fill(10.0));
    std::array<float, 4> f32_vals = {
        2.0f, 50.0f, std::numeric_limits<float>::quiet_NaN(), 10.0f};
    EXPECT_EQ(CE_None, band_f32->RasterIO(GF_Write, 0, 2, 4, 1, f32_vals.data(),
                                          4, 1, GDT_Float32, 0, 0, nullptr));
    band_f32->FlushCache();
    EXPECT_EQ(CE_None, band_f32->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                                   &sstddev, nullptr, nullptr));
    EXPECT_DOUBLE_EQ(2.0, smin);
    EXPECT_DOUBLE_EQ(50.0, smax);

    band_f32->has_nodata_ = true;
    band_f32->nodata_val_ = 10.0;
    EXPECT_EQ(CE_None, band_f32->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                                   &sstddev, nullptr, nullptr));
    {
      CPLConfigOptionSetter s("GDAL_STATS_USE_FLOAT32_OPTIM", "NO", false);
      EXPECT_EQ(CE_None,
                band_f32->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                            &sstddev, nullptr, nullptr));
    }
  }

  // GDT_Float64 SSE2/AVX2 and generic paths
  {
    auto ds_f64 = std::make_unique<TestDataset>(40, 4, GA_Update);
    auto* band_f64 =
        new TestRasterBand(ds_f64.get(), 1, GDT_Float64, 40, 4, 32, 2);
    ds_f64->AddTestBand(band_f64);
    EXPECT_EQ(CE_None, band_f64->Fill(10.0));
    std::array<double, 4> f64_vals = {
        2.0, 50.0, std::numeric_limits<double>::quiet_NaN(), 10.0};
    EXPECT_EQ(CE_None, band_f64->RasterIO(GF_Write, 0, 2, 4, 1, f64_vals.data(),
                                          4, 1, GDT_Float64, 0, 0, nullptr));
    band_f64->FlushCache();
    EXPECT_EQ(CE_None, band_f64->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                                   &sstddev, nullptr, nullptr));
    EXPECT_DOUBLE_EQ(2.0, smin);
    EXPECT_DOUBLE_EQ(50.0, smax);

    band_f64->has_nodata_ = true;
    band_f64->nodata_val_ = 10.0;
    EXPECT_EQ(CE_None, band_f64->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                                   &sstddev, nullptr, nullptr));
    {
      CPLConfigOptionSetter s("GDAL_STATS_USE_FLOAT64_OPTIM", "NO", false);
      EXPECT_EQ(CE_None,
                band_f64->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                            &sstddev, nullptr, nullptr));
    }
  }

  // All 16 data types for both ArbitraryOverviews (bApproxOK=TRUE) and Generic
  // Block path (bApproxOK=FALSE), with mask and nodata
  constexpr auto kStatsVals = std::to_array<double>({2.0, 6.0, 12.0, 99.0});
  for (GDALDataType dt : kAllDataTypes) {
    auto loop_ds = std::make_unique<TestDataset>(8, 8, GA_Update);
    auto* loop_band = new TestRasterBand(loop_ds.get(), 1, dt, 8, 8, 4, 4);
    loop_ds->AddTestBand(loop_band);
    TestRasterBand mask_band(loop_ds.get(), 2, GDT_Byte, 8, 8, 4, 4);
    EXPECT_EQ(CE_None, mask_band.Fill(255));
    mask_band.storage_[0] = 0;

    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 8; ++x) {
        double v = kStatsVals[(x + y) % kStatsVals.size()];
        EXPECT_EQ(CE_None, loop_band->RasterIO(GF_Write, x, y, 1, 1, &v, 1, 1,
                                               GDT_Float64, 0, 0, nullptr));
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
    EXPECT_EQ(CE_None,
              loop_band->ComputeStatistics(TRUE, &smin, &smax, &smean, &sstddev,
                                           nullptr, nullptr));

    // Block path with mask band
    loop_band->arbitrary_overviews_ = false;
    EXPECT_EQ(CE_None,
              loop_band->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                           &sstddev, nullptr, nullptr));

    // Block path without custom mask band
    loop_band->custom_mask_band_ = nullptr;
    loop_band->custom_mask_flags_ = 0;
    EXPECT_EQ(CE_None,
              loop_band->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                           &sstddev, nullptr, nullptr));

    if (dt == GDT_Byte) {
      WithQuietHandler quiet;
      CPLErrorReset();
      loop_band->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE");
      loop_band->arbitrary_overviews_ = true;
      EXPECT_EQ(CE_None,
                loop_band->ComputeStatistics(TRUE, &smin, &smax, &smean,
                                             &sstddev, nullptr, nullptr));
      loop_band->arbitrary_overviews_ = false;
      EXPECT_EQ(CE_None,
                loop_band->ComputeStatistics(FALSE, &smin, &smax, &smean,
                                             &sstddev, nullptr, nullptr));
      CPLErrorReset();
    }
  }
}

TEST(GDALRasterBandTest, ComputeRasterMinMaxAndLocationAllPaths) {
  auto ds = std::make_unique<TestDataset>(70, 4, GA_Update);
  auto* band = new TestRasterBand(ds.get(), 1, GDT_Byte, 70, 4, 32, 2);
  ds->AddTestBand(band);
  GDALRasterBandH hBand = GDALRasterBand::ToHandle(band);

  std::array<double, 2> minmax{};
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALComputeRasterMinMax(nullptr, TRUE, minmax.data()));
    CPLErrorReset();
  }

  // 1. Tight GetMinimum / GetMaximum with bApproxOK=TRUE
  band->has_custom_min_ = true;
  band->custom_min_ = 4.0;
  band->has_custom_max_ = true;
  band->custom_max_ = 88.0;
  EXPECT_EQ(CE_None, GDALComputeRasterMinMax(hBand, TRUE, minmax.data()));
  EXPECT_DOUBLE_EQ(4.0, minmax[0]);
  EXPECT_DOUBLE_EQ(88.0, minmax[1]);
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
    EXPECT_EQ(CE_None, b_ovr_main->ComputeRasterMinMax(TRUE, minmax.data()));
    EXPECT_DOUBLE_EQ(33.0, minmax[0]);
    EXPECT_DOUBLE_EQ(33.0, minmax[1]);
    b_ovr_main->overviews_.clear();
  }

  // 3. Optimized path (GDT_Byte, GDT_UInt16, GDT_Int16) on 70x4 band with 32x2
  // blocks
  constexpr auto kOptTypes =
      std::to_array<GDALDataType>({GDT_Byte, GDT_UInt16, GDT_Int16});
  for (GDALDataType opt_dt : kOptTypes) {
    auto ds_opt = std::make_unique<TestDataset>(70, 4, GA_Update);
    auto* band_opt = new TestRasterBand(ds_opt.get(), 1, opt_dt, 70, 4, 32, 2);
    ds_opt->AddTestBand(band_opt);
    EXPECT_EQ(CE_None, band_opt->Fill(20));
    double v_low = 5.0, v_high = 90.0;
    EXPECT_EQ(CE_None, band_opt->RasterIO(GF_Write, 0, 0, 1, 1, &v_low, 1, 1,
                                          GDT_Float64, 0, 0, nullptr));
    EXPECT_EQ(CE_None, band_opt->RasterIO(GF_Write, 68, 3, 1, 1, &v_high, 1, 1,
                                          GDT_Float64, 0, 0, nullptr));
    band_opt->FlushCache();

    // Without nodata: block path and arbitrary overview path
    band_opt->arbitrary_overviews_ = false;
    EXPECT_EQ(CE_None, band_opt->ComputeRasterMinMax(FALSE, minmax.data()));
    EXPECT_DOUBLE_EQ(5.0, minmax[0]);
    EXPECT_DOUBLE_EQ(90.0, minmax[1]);

    band_opt->arbitrary_overviews_ = true;
    EXPECT_EQ(CE_None, band_opt->ComputeRasterMinMax(TRUE, minmax.data()));

    // With nodata: block path and arbitrary overview path
    band_opt->has_nodata_ = true;
    band_opt->nodata_val_ = 20.0;
    band_opt->arbitrary_overviews_ = false;
    EXPECT_EQ(CE_None, band_opt->ComputeRasterMinMax(FALSE, minmax.data()));
    band_opt->arbitrary_overviews_ = true;
    EXPECT_EQ(CE_None, band_opt->ComputeRasterMinMax(TRUE, minmax.data()));

    // Partial block with nXCheck >= 32 and (nBlockXSize % 32) == 0
    auto ds_part = std::make_unique<TestDataset>(40, 2, GA_Update);
    auto* band_part =
        new TestRasterBand(ds_part.get(), 1, opt_dt, 40, 2, 64, 2);
    ds_part->AddTestBand(band_part);
    EXPECT_EQ(CE_None, band_part->Fill(15));
    EXPECT_EQ(CE_None, band_part->ComputeRasterMinMax(FALSE, minmax.data()));
    band_part->has_nodata_ = true;
    band_part->nodata_val_ = 99.0;
    EXPECT_EQ(CE_None, band_part->ComputeRasterMinMax(FALSE, minmax.data()));
  }

  // 4. Generic path across all 16 data types (both bApproxOK=TRUE with
  // arbitrary_overviews_ and bApproxOK=FALSE, with mask and nodata)
  constexpr auto kMinMaxVals = std::to_array<double>({3.0, 8.0, 19.0, 99.0});
  for (GDALDataType dt : kAllDataTypes) {
    auto loop_ds = std::make_unique<TestDataset>(8, 8, GA_Update);
    auto* loop_band = new TestRasterBand(loop_ds.get(), 1, dt, 8, 8, 4, 4);
    loop_ds->AddTestBand(loop_band);
    TestRasterBand mask_band(loop_ds.get(), 2, GDT_Byte, 8, 8, 4, 4);
    EXPECT_EQ(CE_None, mask_band.Fill(255));
    mask_band.storage_[0] = 0;

    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 8; ++x) {
        double v = kMinMaxVals[(x + y) % kMinMaxVals.size()];
        EXPECT_EQ(CE_None, loop_band->RasterIO(GF_Write, x, y, 1, 1, &v, 1, 1,
                                               GDT_Float64, 0, 0, nullptr));
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
    EXPECT_EQ(CE_None, loop_band->ComputeRasterMinMax(TRUE, minmax.data()));

    // Block generic path with mask
    loop_band->arbitrary_overviews_ = false;
    EXPECT_EQ(CE_None, loop_band->ComputeRasterMinMax(FALSE, minmax.data()));

    // Block generic path without mask
    loop_band->custom_mask_band_ = nullptr;
    loop_band->custom_mask_flags_ = 0;
    EXPECT_EQ(CE_None, loop_band->ComputeRasterMinMax(FALSE, minmax.data()));

    if (dt == GDT_Byte) {
      WithQuietHandler quiet;
      CPLErrorReset();
      loop_band->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE");
      loop_band->arbitrary_overviews_ = true;
      EXPECT_EQ(CE_None, loop_band->ComputeRasterMinMax(TRUE, minmax.data()));
      loop_band->arbitrary_overviews_ = false;
      EXPECT_EQ(CE_None, loop_band->ComputeRasterMinMax(FALSE, minmax.data()));
      CPLErrorReset();
    }
  }

  // All nodata -> CE_Failure
  TestRasterBand all_nd_band(nullptr, 1, GDT_Byte, 4, 4, 4, 4);
  EXPECT_EQ(CE_None, all_nd_band.Fill(99));
  all_nd_band.has_nodata_ = true;
  all_nd_band.nodata_val_ = 99.0;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              all_nd_band.ComputeRasterMinMax(FALSE, minmax.data()));
    CPLErrorReset();
  }

  // 5. ComputeRasterMinMaxLocation & GDALComputeRasterMinMaxLocation
  double loc_min = 0, loc_max = 0;
  int min_x = 0, min_y = 0, max_x = 0, max_y = 0;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALComputeRasterMinMaxLocation(nullptr, &loc_min, &loc_max,
                                              &min_x, &min_y, &max_x, &max_y));

    // Complex type -> CE_Failure
    TestRasterBand c_band(nullptr, 1, GDT_CFloat32, 4, 4, 4, 4);
    EXPECT_EQ(CE_Failure,
              c_band.ComputeRasterMinMaxLocation(&loc_min, &loc_max, &min_x,
                                                 &min_y, &max_x, &max_y));
    CPLErrorReset();
  }

  // All 11 real types:
  for (GDALDataType dt : kRealDataTypes) {
    // (a) Full block (with nodata configured but no pixel matching nodata)
    auto loop_ds = std::make_unique<TestDataset>(8, 8, GA_Update);
    auto* loop_band = new TestRasterBand(loop_ds.get(), 1, dt, 8, 8, 4, 4);
    loop_ds->AddTestBand(loop_band);
    EXPECT_EQ(CE_None, loop_band->Fill(10));
    double v_min = 2.0, v_max = 80.0;
    EXPECT_EQ(CE_None, loop_band->RasterIO(GF_Write, 1, 2, 1, 1, &v_min, 1, 1,
                                           GDT_Float64, 0, 0, nullptr));
    EXPECT_EQ(CE_None, loop_band->RasterIO(GF_Write, 5, 6, 1, 1, &v_max, 1, 1,
                                           GDT_Float64, 0, 0, nullptr));
    loop_band->FlushCache();

    loop_band->has_nodata_ = true;
    loop_band->nodata_val_ = 99.0;
    loop_band->has_nodata_i64_ = true;
    loop_band->nodata_i64_ = 99;
    loop_band->has_nodata_u64_ = true;
    loop_band->nodata_u64_ = 99U;

    EXPECT_EQ(CE_None, GDALComputeRasterMinMaxLocation(
                           GDALRasterBand::ToHandle(loop_band), &loc_min,
                           &loc_max, &min_x, &min_y, &max_x, &max_y));
    EXPECT_EQ(1, min_x);
    EXPECT_EQ(2, min_y);
    EXPECT_EQ(5, max_x);
    EXPECT_EQ(6, max_y);

    // Min only
    EXPECT_EQ(CE_None,
              loop_band->ComputeRasterMinMaxLocation(&loc_min, nullptr, &min_x,
                                                     &min_y, nullptr, nullptr));
    // Max only
    EXPECT_EQ(CE_None,
              loop_band->ComputeRasterMinMaxLocation(nullptr, &loc_max, nullptr,
                                                     nullptr, &max_x, &max_y));

    if (dt == GDT_Byte) {
      double v0 = 0.0, v255 = 255.0;
      EXPECT_EQ(CE_None, loop_band->RasterIO(GF_Write, 0, 1, 1, 1, &v0, 1, 1,
                                             GDT_Float64, 0, 0, nullptr));
      EXPECT_EQ(CE_None, loop_band->RasterIO(GF_Write, 1, 1, 1, 1, &v255, 1, 1,
                                             GDT_Float64, 0, 0, nullptr));
      loop_band->FlushCache();
      EXPECT_EQ(CE_None,
                loop_band->ComputeRasterMinMaxLocation(
                    &loc_min, &loc_max, &min_x, &min_y, &max_x, &max_y));
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        loop_band->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE",
                                   "IMAGE_STRUCTURE");
        EXPECT_EQ(CE_None,
                  loop_band->ComputeRasterMinMaxLocation(
                      &loc_min, &loc_max, &min_x, &min_y, &max_x, &max_y));
        CPLErrorReset();
      }
      loop_band->SetMetadata(nullptr, "IMAGE_STRUCTURE");
    }

#if defined(__x86_64__) || defined(_M_X64)
    // Upstream third_party/gdal/gcore/gdal_minmax_element.hpp lines 277 & 303
    // pass `(b == noDataValue) ? true : (a == noDataValue) ? false : a < b` to
    // std::min_element, returning true for comp(noDataValue, noDataValue). On
    // non-SSE2 ARM64 builds, libc++ hardening aborts (comp_ref_type.h:47:
    // !__comp_(__l, __r)) whenever a buffer contains a pixel equal to nodata.
    if (dt == GDT_Byte) {
      double v_fill = 10.0;
      EXPECT_EQ(CE_None, loop_band->RasterIO(GF_Write, 0, 1, 1, 1, &v_fill, 1,
                                             1, GDT_Float64, 0, 0, nullptr));
      EXPECT_EQ(CE_None, loop_band->RasterIO(GF_Write, 1, 1, 1, 1, &v_fill, 1,
                                             1, GDT_Float64, 0, 0, nullptr));
    }
    double v_nd = 99.0;
    EXPECT_EQ(CE_None, loop_band->RasterIO(GF_Write, 0, 0, 1, 1, &v_nd, 1, 1,
                                           GDT_Float64, 0, 0, nullptr));
    loop_band->FlushCache();

    EXPECT_EQ(CE_None, GDALComputeRasterMinMaxLocation(
                           GDALRasterBand::ToHandle(loop_band), &loc_min,
                           &loc_max, &min_x, &min_y, &max_x, &max_y));
    EXPECT_EQ(1, min_x);
    EXPECT_EQ(2, min_y);
    EXPECT_EQ(5, max_x);
    EXPECT_EQ(6, max_y);

    EXPECT_EQ(CE_None,
              loop_band->ComputeRasterMinMaxLocation(&loc_min, nullptr, &min_x,
                                                     &min_y, nullptr, nullptr));
    EXPECT_EQ(CE_None,
              loop_band->ComputeRasterMinMaxLocation(nullptr, &loc_max, nullptr,
                                                     nullptr, &max_x, &max_y));

    if (dt == GDT_Byte) {
      double v0 = 0.0, v255 = 255.0;
      EXPECT_EQ(CE_None, loop_band->RasterIO(GF_Write, 0, 1, 1, 1, &v0, 1, 1,
                                             GDT_Float64, 0, 0, nullptr));
      EXPECT_EQ(CE_None, loop_band->RasterIO(GF_Write, 1, 1, 1, 1, &v255, 1, 1,
                                             GDT_Float64, 0, 0, nullptr));
      loop_band->FlushCache();
      EXPECT_EQ(CE_None,
                loop_band->ComputeRasterMinMaxLocation(
                    &loc_min, &loc_max, &min_x, &min_y, &max_x, &max_y));
      WithQuietHandler quiet;
      CPLErrorReset();
      loop_band->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE");
      EXPECT_EQ(CE_None,
                loop_band->ComputeRasterMinMaxLocation(
                    &loc_min, &loc_max, &min_x, &min_y, &max_x, &max_y));
      CPLErrorReset();
    }
#endif

    // (b) Partial block (6x6 with 4x4 block) and custom mask band
    auto part_ds = std::make_unique<TestDataset>(6, 6, GA_Update);
    auto* part_band = new TestRasterBand(part_ds.get(), 1, dt, 6, 6, 4, 4);
    part_ds->AddTestBand(part_band);
    EXPECT_EQ(CE_None, part_band->Fill(10));
    EXPECT_EQ(CE_None, part_band->RasterIO(GF_Write, 1, 1, 1, 1, &v_min, 1, 1,
                                           GDT_Float64, 0, 0, nullptr));
    EXPECT_EQ(CE_None, part_band->RasterIO(GF_Write, 5, 5, 1, 1, &v_max, 1, 1,
                                           GDT_Float64, 0, 0, nullptr));
    part_band->FlushCache();

    TestRasterBand m6(part_ds.get(), 2, GDT_Byte, 6, 6, 4, 4);
    EXPECT_EQ(CE_None, m6.Fill(255));
    m6.storage_[0] = 0;
    part_band->custom_mask_band_ = &m6;
    part_band->custom_mask_flags_ = GMF_PER_DATASET;

    EXPECT_EQ(CE_None, part_band->ComputeRasterMinMaxLocation(
                           &loc_min, &loc_max, &min_x, &min_y, &max_x, &max_y));
    EXPECT_EQ(CE_None,
              part_band->ComputeRasterMinMaxLocation(&loc_min, nullptr, &min_x,
                                                     &min_y, nullptr, nullptr));
    EXPECT_EQ(CE_None,
              part_band->ComputeRasterMinMaxLocation(nullptr, &loc_max, nullptr,
                                                     nullptr, &max_x, &max_y));
  }

#if defined(__x86_64__) || defined(_M_X64)
  // All nodata in ComputeRasterMinMaxLocation -> CE_Warning
  // Guarded on non-x86_64 due to the same gdal_minmax_element.hpp lines 277 &
  // 303 strict-weak-ordering comparator bug when pixels equal nodata.
  TestRasterBand loc_nd_band(nullptr, 1, GDT_Byte, 4, 4, 4, 4);
  EXPECT_EQ(CE_None, loc_nd_band.Fill(99));
  loc_nd_band.has_nodata_ = true;
  loc_nd_band.nodata_val_ = 99.0;
  EXPECT_EQ(CE_Warning,
            loc_nd_band.ComputeRasterMinMaxLocation(&loc_min, &loc_max, &min_x,
                                                    &min_y, &max_x, &max_y));
#endif
}

TEST(GDALRasterBandTest, RATMaskBandsAndColorTranslation) {
  // 1. RAT
  TestRasterBand standalone_band;
  GDALRasterBandH hStandalone = GDALRasterBand::ToHandle(&standalone_band);
  EXPECT_EQ(nullptr, GDALGetDefaultRAT(nullptr));
  EXPECT_EQ(nullptr, GDALGetDefaultRAT(hStandalone));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALSetDefaultRAT(nullptr, nullptr));
    EXPECT_EQ(CE_Failure, GDALSetDefaultRAT(hStandalone, nullptr));
    standalone_band.SetMOFlags(GMO_IGNORE_UNIMPLEMENTED);
    EXPECT_EQ(CE_Failure, GDALSetDefaultRAT(hStandalone, nullptr));
    CPLErrorReset();
  }
  standalone_band.SetMOFlags(0);

  // 2. MaskBand null & standalone checks
  EXPECT_EQ(nullptr, GDALGetMaskBand(nullptr));
  EXPECT_EQ(GMF_ALL_VALID, GDALGetMaskFlags(nullptr));
  EXPECT_FALSE(GDALIsMaskBand(nullptr));
  EXPECT_FALSE(GDALIsMaskBand(hStandalone));
  EXPECT_EQ(GMVR_UNKNOWN, standalone_band.GetMaskValueRange());
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALCreateMaskBand(nullptr, GMF_PER_DATASET));
    // CreateMaskBand when poDS == nullptr
    EXPECT_EQ(CE_Failure, GDALCreateMaskBand(hStandalone, GMF_PER_DATASET));
    standalone_band.SetMOFlags(GMO_IGNORE_UNIMPLEMENTED);
    EXPECT_EQ(CE_Failure, GDALCreateMaskBand(hStandalone, GMF_PER_DATASET));
    CPLErrorReset();
  }
  standalone_band.SetMOFlags(0);

  // 3. MaskBand lifecycle: AllValid -> NoData -> changed NoData across Byte,
  // Float64(NaN), Int64, UInt64
  {
    auto ds = std::make_unique<TestDataset>(8, 8, GA_Update);
    auto* b_byte = new TestRasterBand(ds.get(), 1, GDT_Byte, 8, 8, 4, 4);
    ds->AddTestBand(b_byte);
    EXPECT_EQ(GMF_ALL_VALID,
              GDALGetMaskFlags(GDALRasterBand::ToHandle(b_byte)));
    GDALRasterBand* mb = b_byte->GetMaskBand();
    ASSERT_NE(nullptr, mb);
    EXPECT_TRUE(GDALIsMaskBand(GDALRasterBand::ToHandle(mb)));
    EXPECT_EQ(GMVR_0_AND_255_ONLY, mb->GetMaskValueRange());

    // Switch from AllValid to NoData
    b_byte->has_nodata_ = true;
    b_byte->nodata_val_ = 10.0;
    b_byte->InvalidateMaskBand();
    EXPECT_EQ(GMF_NODATA, b_byte->GetMaskFlags());
    mb = b_byte->GetMaskBand();
    ASSERT_NE(nullptr, mb);
    // Change nodata value -> recreates NoDataMaskBand
    b_byte->nodata_val_ = 20.0;
    mb = b_byte->GetMaskBand();
    ASSERT_NE(nullptr, mb);
  }
  {
    auto ds = std::make_unique<TestDataset>(8, 8, GA_Update);
    auto* b_f64 = new TestRasterBand(ds.get(), 1, GDT_Float64, 8, 8, 4, 4);
    ds->AddTestBand(b_f64);
    (void)b_f64->GetMaskBand();  // AllValid
    b_f64->has_nodata_ = true;
    b_f64->nodata_val_ = std::numeric_limits<double>::quiet_NaN();
    b_f64->InvalidateMaskBand();
    EXPECT_NE(nullptr, b_f64->GetMaskBand());
    EXPECT_NE(nullptr, b_f64->GetMaskBand());  // Same NaN nodata
    b_f64->nodata_val_ = 5.0;
    EXPECT_NE(nullptr, b_f64->GetMaskBand());  // Changed from NaN to 5.0
  }
  {
    auto ds = std::make_unique<TestDataset>(8, 8, GA_Update);
    auto* b_i64 = new TestRasterBand(ds.get(), 1, GDT_Int64, 8, 8, 4, 4);
    ds->AddTestBand(b_i64);
    (void)b_i64->GetMaskBand();  // AllValid
    b_i64->has_nodata_i64_ = true;
    b_i64->nodata_i64_ = -100;
    b_i64->InvalidateMaskBand();
    EXPECT_EQ(GMF_NODATA, b_i64->GetMaskFlags());
    EXPECT_NE(nullptr, b_i64->GetMaskBand());
    b_i64->nodata_i64_ = -200;
    EXPECT_NE(nullptr, b_i64->GetMaskBand());
  }
  {
    auto ds = std::make_unique<TestDataset>(8, 8, GA_Update);
    auto* b_u64 = new TestRasterBand(ds.get(), 1, GDT_UInt64, 8, 8, 4, 4);
    ds->AddTestBand(b_u64);
    (void)b_u64->GetMaskBand();  // AllValid
    b_u64->has_nodata_u64_ = true;
    b_u64->nodata_u64_ = 100U;
    b_u64->InvalidateMaskBand();
    EXPECT_EQ(GMF_NODATA, b_u64->GetMaskFlags());
    EXPECT_NE(nullptr, b_u64->GetMaskBand());
    b_u64->nodata_u64_ = 200U;
    EXPECT_NE(nullptr, b_u64->GetMaskBand());
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
    EXPECT_EQ(GMF_PER_DATASET | GMF_NODATA, b1->GetMaskFlags());
    GDALRasterBand* mb = b1->GetMaskBand();
    ASSERT_NE(nullptr, mb);
    GByte mval = 0;
    EXPECT_EQ(CE_None, mb->RasterIO(GF_Read, 0, 0, 1, 1, &mval, 1, 1, GDT_Byte,
                                    0, 0, nullptr));
  }
  {
    // Mismatched count in NODATA_VALUES
    auto ds = std::make_unique<TestDataset>(4, 4, GA_Update);
    auto* b1 = new TestRasterBand(ds.get(), 1, GDT_Byte, 4, 4, 4, 4);
    auto* b2 = new TestRasterBand(ds.get(), 2, GDT_Byte, 4, 4, 4, 4);
    ds->AddTestBand(b1);
    ds->AddTestBand(b2);
    ds->SetMetadataItem("NODATA_VALUES", "10");
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(GMF_ALL_VALID, b1->GetMaskFlags());
    EXPECT_NE(nullptr, b1->GetMaskBand());
    CPLErrorReset();
  }
  {
    // Mismatched band data types with NODATA_VALUES
    auto ds = std::make_unique<TestDataset>(4, 4, GA_Update);
    auto* b1 = new TestRasterBand(ds.get(), 1, GDT_Byte, 4, 4, 4, 4);
    auto* b2 = new TestRasterBand(ds.get(), 2, GDT_UInt16, 4, 4, 4, 4);
    ds->AddTestBand(b1);
    ds->AddTestBand(b2);
    ds->SetMetadataItem("NODATA_VALUES", "10 20");
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(GMF_ALL_VALID, b1->GetMaskFlags());
    EXPECT_NE(nullptr, b1->GetMaskBand());
    CPLErrorReset();
  }

  // 5. Alpha band masks (2-band and 4-band, both GDT_Byte and GDT_UInt16)
  constexpr auto kBandCounts = std::to_array<int>({2, 4});
  constexpr auto kAlphaTypes =
      std::to_array<GDALDataType>({GDT_Byte, GDT_UInt16});
  for (int nbands : kBandCounts) {
    for (GDALDataType alpha_dt : kAlphaTypes) {
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
      EXPECT_EQ(GMF_ALPHA | GMF_PER_DATASET, b1->GetMaskFlags());
      GDALRasterBand* mb = b1->GetMaskBand();
      ASSERT_NE(nullptr, mb);
      EXPECT_EQ(GMVR_UNKNOWN, mb->GetMaskValueRange());
      GByte mval = 0;
      EXPECT_EQ(CE_None, mb->RasterIO(GF_Read, 0, 0, 1, 1, &mval, 1, 1,
                                      GDT_Byte, 0, 0, nullptr));
    }
  }

  // 6. CreateMaskBand on a GTiff in /vsimem/
  GDALRegister_GTiff();
  GDALDriver* gtiff = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff);
  {
    constexpr char kMaskPath[] = "/vsimem/gdalrasterband_test/mask.tif";
    absl::Cleanup cleanup_dir = [] {
      VSIRmdirRecursive("/vsimem/gdalrasterband_test");
    };
    GDALDatasetUniquePtr mds(
        gtiff->Create(kMaskPath, 8, 8, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, mds);
    GDALRasterBand* mb = mds->GetRasterBand(1);
    ASSERT_NE(nullptr, mb);
    EXPECT_EQ(CE_None, GDALCreateMaskBand(GDALRasterBand::ToHandle(mb),
                                          GMF_PER_DATASET));
    EXPECT_NE(nullptr, mb->GetMaskBand());
  }

  // 7. GetIndexColorTranslationTo
  TestRasterBand src_band;
  TestRasterBand ref_band;
  EXPECT_EQ(nullptr, src_band.GetIndexColorTranslationTo(nullptr));
  EXPECT_EQ(nullptr, src_band.GetIndexColorTranslationTo(&ref_band));

  GDALColorTable ct_src;
  constexpr GDALColorEntry c0 = {10, 20, 30, 255};
  constexpr GDALColorEntry c1 = {40, 50, 60, 255};
  constexpr GDALColorEntry c2 = {70, 80, 90, 255};
  ct_src.SetColorEntry(0, &c0);
  ct_src.SetColorEntry(1, &c1);
  ct_src.SetColorEntry(2, &c2);

  src_band.custom_color_interp_ = GCI_PaletteIndex;
  src_band.custom_color_table_ = &ct_src;
  EXPECT_EQ(nullptr, src_band.GetIndexColorTranslationTo(&ref_band));

  ref_band.custom_color_interp_ = GCI_PaletteIndex;
  ref_band.custom_color_table_ = &ct_src;
  // Identical palette -> returns nullptr (no translation needed) with approx=0
  int approx = 1;
  EXPECT_EQ(nullptr,
            src_band.GetIndexColorTranslationTo(&ref_band, nullptr, &approx));
  EXPECT_EQ(0, approx);

  // Reordered + approximate match palette + nodata values
  GDALColorTable ct_ref;
  constexpr GDALColorEntry c1_near = {42, 51, 59, 255};
  ct_ref.SetColorEntry(0, &c2);
  ct_ref.SetColorEntry(1, &c0);
  ct_ref.SetColorEntry(2, &c1_near);
  ref_band.custom_color_table_ = &ct_ref;

  src_band.has_nodata_ = true;
  src_band.nodata_val_ = 2.0;
  ref_band.has_nodata_ = true;
  ref_band.nodata_val_ = 0.0;

  std::array<unsigned char, 256> custom_lut{};
  unsigned char* res_lut = src_band.GetIndexColorTranslationTo(
      &ref_band, custom_lut.data(), &approx);
  EXPECT_EQ(custom_lut.data(), res_lut);
  EXPECT_EQ(1, approx);
  EXPECT_EQ(1, custom_lut[0]);  // c0 -> index 1 in ref
  EXPECT_EQ(2, custom_lut[1]);  // c1 -> closest is c1_near at index 2

  // Also test allocated table (pTranslationTable == nullptr)
  std::unique_ptr<unsigned char, CplFreeDeleter> alloc_lut(
      src_band.GetIndexColorTranslationTo(&ref_band, nullptr, nullptr));
  ASSERT_NE(nullptr, alloc_lut);
}

TEST(GDALRasterBandTest, MiscMethodsVirtualMemCoverageStatusWindows) {
  auto ds = std::make_unique<TestDataset>(16, 16, GA_Update);
  ds->SetDescription("my_test_dataset");
  auto* band = new TestRasterBand(ds.get(), 1, GDT_Byte, 16, 16, 4, 4);
  ds->AddTestBand(band);
  GDALRasterBandH hBand = GDALRasterBand::ToHandle(band);

  // 1. ReportError (with dataset description and without dataset description)
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    band->ReportError(CE_Warning, CPLE_AppDefined, "warn %d", 1);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    TestRasterBand standalone;
    standalone.ReportError(CE_Warning, CPLE_AppDefined, "warn %d", 2);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 2. GetVirtualMemAuto & GDALGetVirtualMemAuto
  int pspace = 0;
  GIntBig lspace = 0;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALGetVirtualMemAuto(nullptr, GF_Read, &pspace, &lspace,
                                             nullptr));
    CPLErrorReset();
  }
  CPLVirtualMem* vm1 =
      GDALGetVirtualMemAuto(hBand, GF_Read, nullptr, &lspace, nullptr);
  absl::Cleanup free_vm1 = [vm1] { CPLVirtualMemFree(vm1); };
  CPLVirtualMem* vm2 =
      GDALGetVirtualMemAuto(hBand, GF_Read, &pspace, nullptr, nullptr);
  absl::Cleanup free_vm2 = [vm2] { CPLVirtualMemFree(vm2); };

  CPLStringList vmem_opts_no;
  vmem_opts_no.SetNameValue("USE_DEFAULT_IMPLEMENTATION", "NO");
  EXPECT_EQ(nullptr, band->GetVirtualMemAuto(GF_Read, &pspace, &lspace,
                                             vmem_opts_no.List()));

  CPLStringList vmem_opts_yes;
  vmem_opts_yes.SetNameValue("USE_DEFAULT_IMPLEMENTATION", "YES");
  CPLVirtualMem* vmem =
      band->GetVirtualMemAuto(GF_Read, &pspace, &lspace, vmem_opts_yes.List());
  absl::Cleanup free_vmem = [vmem] { CPLVirtualMemFree(vmem); };

  // 3. GetDataCoverageStatus & IGetDataCoverageStatus
  double pct = -1.0;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(GDAL_DATA_COVERAGE_STATUS_UNIMPLEMENTED,
              GDALGetDataCoverageStatus(nullptr, 0, 0, 4, 4, 0, &pct));
    EXPECT_EQ(GDAL_DATA_COVERAGE_STATUS_UNIMPLEMENTED |
                  GDAL_DATA_COVERAGE_STATUS_EMPTY,
              GDALGetDataCoverageStatus(hBand, -1, 0, 4, 4, 0, &pct));
    EXPECT_EQ(GDAL_DATA_COVERAGE_STATUS_UNIMPLEMENTED |
                  GDAL_DATA_COVERAGE_STATUS_EMPTY,
              GDALGetDataCoverageStatus(hBand, 0, -1, 4, 4, 0, &pct));
    EXPECT_EQ(GDAL_DATA_COVERAGE_STATUS_UNIMPLEMENTED |
                  GDAL_DATA_COVERAGE_STATUS_EMPTY,
              GDALGetDataCoverageStatus(hBand, 15, 0, 4, 4, 0, &pct));
    EXPECT_EQ(GDAL_DATA_COVERAGE_STATUS_UNIMPLEMENTED |
                  GDAL_DATA_COVERAGE_STATUS_EMPTY,
              GDALGetDataCoverageStatus(hBand, 0, 15, 4, 4, 0, &pct));
    CPLErrorReset();
  }
  EXPECT_EQ(
      GDAL_DATA_COVERAGE_STATUS_UNIMPLEMENTED | GDAL_DATA_COVERAGE_STATUS_DATA,
      GDALGetDataCoverageStatus(hBand, 0, 0, 0, 4, 0, &pct));
  EXPECT_EQ(
      GDAL_DATA_COVERAGE_STATUS_UNIMPLEMENTED | GDAL_DATA_COVERAGE_STATUS_DATA,
      GDALGetDataCoverageStatus(hBand, 0, 0, 4, 0, 0, &pct));
  EXPECT_EQ(
      GDAL_DATA_COVERAGE_STATUS_UNIMPLEMENTED | GDAL_DATA_COVERAGE_STATUS_DATA,
      GDALGetDataCoverageStatus(hBand, 0, 0, 4, 4, 0, &pct));
  EXPECT_DOUBLE_EQ(100.0, pct);

  // 4. EnterReadWrite, LeaveReadWrite, InitRWLock
  band->InitRWLock();
  EXPECT_EQ(TRUE, band->EnterReadWrite(GF_Write));
  band->LeaveReadWrite();
  EXPECT_EQ(TRUE, band->EnterReadWrite(GF_Read));
  band->LeaveReadWrite();

  // 5. EnablePixelTypeSignedByteWarning & GetMetadataItem("PIXELTYPE",
  // "IMAGE_STRUCTURE")
  GDALEnablePixelTypeSignedByteWarning(hBand, TRUE);
  band->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE");
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_STREQ("SIGNEDBYTE",
                 band->GetMetadataItem("PIXELTYPE", "IMAGE_STRUCTURE"));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    CPLErrorReset();
  }
  GDALEnablePixelTypeSignedByteWarning(hBand, FALSE);
  EXPECT_STREQ("SIGNEDBYTE",
               band->GetMetadataItem("PIXELTYPE", "IMAGE_STRUCTURE"));
  EXPECT_EQ(nullptr, band->GetMetadataItem("OTHER", ""));

  // 6. IterateWindows
  // (a) maxSize == 0 -> uses block size (4x4 on 16x16 -> 16 windows)
  uint64_t win_count = 0;
  auto wrapper0 = band->IterateWindows(0);
  EXPECT_EQ(16U, wrapper0.count());
  for (const auto& w : wrapper0) {
    EXPECT_GT(w.nXSize, 0);
    EXPECT_GT(w.nYSize, 0);
    win_count++;
  }
  EXPECT_EQ(16U, win_count);
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
    EXPECT_EQ(16, w.nXSize);
  }

  // (d) Invalid block size -> fallback to whole raster window
  TestRasterBand bad_blk_band;
  bad_blk_band.SetDimsAndType(10, 10, 0, 0, GDT_Byte);
  auto wrapper_bad = bad_blk_band.IterateWindows(0);
  EXPECT_EQ(10U, wrapper_bad.count());
}

TEST(GDALRasterBandTest, AsMDArrayInterpolateSplitRasterIO) {
  // 1. AsMDArray on band without dataset -> nullptr
  TestRasterBand standalone;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, standalone.AsMDArray());
    EXPECT_EQ(nullptr, GDALRasterBandAsMDArray(nullptr));
    CPLErrorReset();
  }

  // 2. AsMDArray with dataset, geotransform, SRS, metadata, offset, scale,
  // unit, nodata
  constexpr char kMdArrayFilename[] =
      "/vsimem/gdalrasterband_test/mdarray_test.tif";
  auto ds = std::make_unique<TestDataset>(8, 6, GA_Update);
  ds->SetDescription(kMdArrayFilename);
  ds->has_gt_ = true;
  ds->gt_ = GDALGeoTransform(100.0, 2.0, 0.0, 200.0, 0.0, -2.0);
  ds->has_srs_ = true;
  ASSERT_EQ(OGRERR_NONE, ds->srs_.importFromEPSG(
                             4326));  // Authority compliant {2, 1} axis mapping

  auto* b_f64 = new TestRasterBand(ds.get(), 1, GDT_Float64, 8, 6, 4, 3);
  EXPECT_EQ(CE_None, b_f64->Fill(12.5));
  b_f64->has_nodata_ = true;
  b_f64->nodata_val_ = -999.0;
  b_f64->custom_unit_ = "meters";
  b_f64->has_custom_offset_ = true;
  b_f64->custom_offset_ = 1.5;
  b_f64->has_custom_scale_ = true;
  b_f64->custom_scale_ = 0.5;
  b_f64->SetMetadataItem("META_KEY", "META_VAL");
  ds->AddTestBand(b_f64);

  {
    GDALMDArrayH hArr =
        GDALRasterBandAsMDArray(GDALRasterBand::ToHandle(b_f64));
    ASSERT_NE(nullptr, hArr);
    absl::Cleanup release_arr = [hArr] { GDALMDArrayRelease(hArr); };
  }

  auto md = b_f64->AsMDArray();
  ASSERT_NE(nullptr, md);
  EXPECT_TRUE(md->IsWritable());
  EXPECT_EQ(kMdArrayFilename, md->GetFilename());
  EXPECT_EQ("meters", md->GetUnit());
  bool has_off = false, has_scl = false;
  GDALDataType st_dt = GDT_Unknown;
  EXPECT_DOUBLE_EQ(1.5, md->GetOffset(&has_off, &st_dt));
  EXPECT_TRUE(has_off);
  EXPECT_DOUBLE_EQ(0.5, md->GetScale(&has_scl, &st_dt));
  EXPECT_TRUE(has_scl);
  EXPECT_NE(nullptr, md->GetRawNoDataValue());
  EXPECT_NE(nullptr, md->GetSpatialRef());
  EXPECT_EQ(2U, md->GetBlockSize().size());
  EXPECT_FALSE(md->GetAttributes(nullptr).empty());

  const auto& dims = md->GetDimensions();
  ASSERT_EQ(2U, dims.size());
  // Read coordinate indexing variables X and Y
  for (const auto& dim : dims) {
    auto idx_var = dim->GetIndexingVariable();
    ASSERT_NE(nullptr, idx_var);
    std::array<GUInt64, 1> start = {0};
    std::array<size_t, 1> count = {static_cast<size_t>(dim->GetSize())};
    std::array<GInt64, 1> step = {1};
    std::array<GPtrDiff_t, 1> stride = {1};
    std::vector<double> coords(count[0], 0.0);
    EXPECT_TRUE(idx_var->Read(
        start.data(), count.data(), step.data(), stride.data(),
        GDALExtendedDataType::Create(GDT_Float64), coords.data()));
  }

  // Write and Read via MDArray (positive and negative steps)
  std::array<GUInt64, 2> start = {0, 0};
  std::array<size_t, 2> count = {2, 3};
  std::array<GInt64, 2> step_pos = {1, 1};
  std::array<GPtrDiff_t, 2> stride = {3, 1};
  constexpr auto kWriteVals = std::to_array<double>({1, 2, 3, 4, 5, 6});
  EXPECT_TRUE(
      md->Write(start.data(), count.data(), step_pos.data(), stride.data(),
                GDALExtendedDataType::Create(GDT_Float64), kWriteVals.data()));
  std::array<double, 6> read_vals{};
  EXPECT_TRUE(md->Read(start.data(), count.data(), step_pos.data(),
                       stride.data(), GDALExtendedDataType::Create(GDT_Float64),
                       read_vals.data()));
  EXPECT_DOUBLE_EQ(1.0, read_vals[0]);
  EXPECT_DOUBLE_EQ(6.0, read_vals[5]);

  // Negative step read
  std::array<GUInt64, 2> start_neg = {1, 2};
  std::array<GInt64, 2> step_neg = {-1, -1};
  EXPECT_TRUE(md->Read(start_neg.data(), count.data(), step_neg.data(),
                       stride.data(), GDALExtendedDataType::Create(GDT_Float64),
                       read_vals.data()));

  // Also test SRS with GIS_TRADITIONAL axis order {1, 2}, Int64 nodata, UInt64
  // nodata, SignedByte
  ds->srs_.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  auto* b_i64 = new TestRasterBand(ds.get(), 2, GDT_Int64, 8, 6, 4, 3);
  b_i64->has_nodata_i64_ = true;
  b_i64->nodata_i64_ = -42;
  ds->AddTestBand(b_i64);
  auto md_i64 = b_i64->AsMDArray();
  ASSERT_NE(nullptr, md_i64);
  EXPECT_NE(nullptr, md_i64->GetSpatialRef());
  EXPECT_NE(nullptr, md_i64->GetRawNoDataValue());

  auto* b_u64 = new TestRasterBand(ds.get(), 3, GDT_UInt64, 8, 6, 4, 3);
  b_u64->has_nodata_u64_ = true;
  b_u64->nodata_u64_ = 42U;
  ds->AddTestBand(b_u64);
  auto md_u64 = b_u64->AsMDArray();
  ASSERT_NE(nullptr, md_u64);
  EXPECT_NE(nullptr, md_u64->GetRawNoDataValue());

  auto* b_sb = new TestRasterBand(ds.get(), 4, GDT_Byte, 8, 6, 4, 3);
  b_sb->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE");
  ds->AddTestBand(b_sb);
  auto md_sb = b_sb->AsMDArray();
  ASSERT_NE(nullptr, md_sb);
  EXPECT_EQ(GDT_Byte, md_sb->GetDataType().GetNumericDataType());

  // 3. InterpolateAtPoint & InterpolateAtGeolocation
  double real_v = 0.0, imag_v = 0.0;
  GDALRasterBandH hBand = GDALRasterBand::ToHandle(b_f64);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALRasterInterpolateAtPoint(nullptr, 1.0, 1.0,
                                                       GRIORA_NearestNeighbour,
                                                       &real_v, &imag_v));
    // Unsupported resampling alg -> CE_Failure
    EXPECT_EQ(CE_Failure,
              GDALRasterInterpolateAtPoint(hBand, 1.0, 1.0, GRIORA_Average,
                                           &real_v, &imag_v));
    // Out of bounds point -> CE_Failure
    EXPECT_EQ(CE_Failure,
              GDALRasterInterpolateAtPoint(
                  hBand, -5.0, 1.0, GRIORA_NearestNeighbour, &real_v, &imag_v));
    CPLErrorReset();
  }
  // Valid interpolations
  constexpr auto kResampleAlgs = std::to_array<GDALRIOResampleAlg>(
      {GRIORA_NearestNeighbour, GRIORA_Bilinear, GRIORA_Cubic,
       GRIORA_CubicSpline});
  for (GDALRIOResampleAlg alg : kResampleAlgs) {
    EXPECT_EQ(CE_None, GDALRasterInterpolateAtPoint(hBand, 2.5, 2.5, alg,
                                                    &real_v, &imag_v));
  }
  // Complex band interpolation
  auto* b_cf64 = new TestRasterBand(ds.get(), 5, GDT_CFloat64, 8, 6, 4, 3);
  EXPECT_EQ(CE_None, b_cf64->Fill(3.0, 4.0));
  ds->AddTestBand(b_cf64);
  EXPECT_EQ(CE_None, b_cf64->InterpolateAtPoint(2.5, 2.5, GRIORA_Bilinear,
                                                &real_v, &imag_v));
  EXPECT_NEAR(3.0, real_v, 1e-5);
  EXPECT_NEAR(4.0, imag_v, 1e-5);

  // InterpolateAtGeolocation
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALRasterInterpolateAtGeolocation(nullptr, 102.0, 198.0, nullptr,
                                                 GRIORA_NearestNeighbour,
                                                 &real_v, &imag_v, nullptr));
    CPLErrorReset();
  }
  EXPECT_EQ(CE_None, GDALRasterInterpolateAtGeolocation(
                         hBand, 102.0, 198.0, nullptr, GRIORA_NearestNeighbour,
                         &real_v, &imag_v, nullptr));
  // With explicit SRS (note: EPSG:4326 traditional order)
  EXPECT_EQ(CE_None,
            GDALRasterInterpolateAtGeolocation(
                hBand, 102.0, 198.0, OGRSpatialReference::ToHandle(&ds->srs_),
                GRIORA_Bilinear, &real_v, &imag_v, nullptr));
  // Without GeoTransform -> CE_Failure
  ds->has_gt_ = false;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, b_f64->InterpolateAtGeolocation(
                              102.0, 198.0, nullptr, GRIORA_NearestNeighbour,
                              &real_v, &imag_v, nullptr));
    CPLErrorReset();
  }

  // 4. SplitRasterIO
  GDALRasterIOExtraArg extra;
  INIT_RASTERIO_EXTRA_ARG(extra);
  std::vector<double> split_buf(48, 0.0);
  // (a) Height split: nBufXSize == nRasterXSize (8) && nBufYSize >= 2 (6)
  EXPECT_EQ(CE_None, b_f64->SplitRasterIO(GF_Read, 0, 0, 8, 6, split_buf.data(),
                                          8, 6, GDT_Float64, 8, 64, &extra));
  // (b) Width split: nBufXSize >= 2 (4) && nBufYSize < nBufXSize (2) &&
  // nBufXSize < nRasterXSize (8)
  EXPECT_EQ(CE_None, b_f64->SplitRasterIO(GF_Read, 0, 0, 4, 2, split_buf.data(),
                                          4, 2, GDT_Float64, 8, 32, &extra));
  // (c) 1x1 -> cannot split, returns CE_Warning
  EXPECT_EQ(CE_Warning,
            b_f64->SplitRasterIO(GF_Read, 0, 0, 1, 1, split_buf.data(), 1, 1,
                                 GDT_Float64, 8, 8, &extra));
}

TEST(GDALRasterBandTest, RasterAlgebra) {
  GDALRegister_MEM();
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr ds1(
      mem_drv->Create("mem1", 4, 4, 2, GDT_Float32, nullptr));
  GDALDatasetUniquePtr ds_diff(
      mem_drv->Create("mem_diff", 5, 4, 1, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, ds1);
  ASSERT_NE(nullptr, ds_diff);

  GDALRasterBand* b1 = ds1->GetRasterBand(1);
  GDALRasterBand* b2 = ds1->GetRasterBand(2);
  GDALRasterBand* b_diff = ds_diff->GetRasterBand(1);
  ASSERT_NE(nullptr, b1);
  ASSERT_NE(nullptr, b2);
  ASSERT_NE(nullptr, b_diff);
  EXPECT_EQ(CE_None, b1->Fill(4.0));
  EXPECT_EQ(CE_None, b2->Fill(2.0));
  GDALRasterBandH h1 = GDALRasterBand::ToHandle(b1);
  GDALRasterBandH h2 = GDALRasterBand::ToHandle(b2);
  GDALRasterBandH h_diff = GDALRasterBand::ToHandle(b_diff);

  // 1. Unary operations C API
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALRasterBandUnaryOp(nullptr, GRAUO_ABS));
    CPLErrorReset();
  }
  constexpr auto kUnaryOps = std::to_array<GDALRasterAlgebraUnaryOperation>(
      {GRAUO_LOGICAL_NOT, GRAUO_ABS, GRAUO_SQRT, GRAUO_LOG10});
  for (GDALRasterAlgebraUnaryOperation uop : kUnaryOps) {
    GDALComputedRasterBandH hComp = GDALRasterBandUnaryOp(h1, uop);
    absl::Cleanup release_comp = [hComp] {
      GDALComputedRasterBandRelease(hComp);
    };
    ASSERT_NE(nullptr, hComp);
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALComputedRasterBandH hLog = GDALRasterBandUnaryOp(h1, GRAUO_LOG);
    absl::Cleanup release_log = [hLog] { GDALComputedRasterBandRelease(hLog); };
    EXPECT_EQ(nullptr, hLog);
    CPLErrorReset();
  }

  // 2. Binary operations C API (Band-Band, Band-Double, Double-Band)
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALRasterBandBinaryOpBand(nullptr, GRABO_ADD, h2));
    EXPECT_EQ(nullptr, GDALRasterBandBinaryOpBand(h1, GRABO_ADD, nullptr));
    EXPECT_EQ(nullptr, GDALRasterBandBinaryOpBand(h1, GRABO_ADD, h_diff));
    EXPECT_EQ(nullptr, GDALRasterBandBinaryOpDouble(nullptr, GRABO_ADD, 1.0));
    EXPECT_EQ(nullptr,
              GDALRasterBandBinaryOpDoubleToBand(1.0, GRABO_ADD, nullptr));
    CPLErrorReset();
  }

  constexpr auto kBinaryOps = std::to_array<GDALRasterAlgebraBinaryOperation>(
      {GRABO_ADD, GRABO_SUB, GRABO_MUL, GRABO_DIV});
  for (GDALRasterAlgebraBinaryOperation bop : kBinaryOps) {
    GDALComputedRasterBandH c_bb = GDALRasterBandBinaryOpBand(h1, bop, h2);
    absl::Cleanup release_bb = [c_bb] { GDALComputedRasterBandRelease(c_bb); };
    ASSERT_NE(nullptr, c_bb);

    GDALComputedRasterBandH c_bd = GDALRasterBandBinaryOpDouble(h1, bop, 2.0);
    absl::Cleanup release_bd = [c_bd] { GDALComputedRasterBandRelease(c_bd); };
    ASSERT_NE(nullptr, c_bd);
  }

  // For DoubleToBand, test ADD, MUL, DIV (omit SUB to avoid GDAL
  // chained-temporary stack bug)
  constexpr auto kDoubleToBandOps =
      std::to_array<GDALRasterAlgebraBinaryOperation>(
          {GRABO_ADD, GRABO_MUL, GRABO_DIV});
  for (GDALRasterAlgebraBinaryOperation bop : kDoubleToBandOps) {
    GDALComputedRasterBandH c_db =
        GDALRasterBandBinaryOpDoubleToBand(2.0, bop, h1);
    absl::Cleanup release_db = [c_db] { GDALComputedRasterBandRelease(c_db); };
    ASSERT_NE(nullptr, c_db);
  }

  // 3. IfThenElse, AsDataType, Max/Min/Mean C API
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALRasterBandIfThenElse(nullptr, h1, h2));
    EXPECT_EQ(nullptr, GDALRasterBandIfThenElse(h1, nullptr, h2));
    EXPECT_EQ(nullptr, GDALRasterBandIfThenElse(h1, h2, nullptr));
    GDALComputedRasterBandH c_ite = GDALRasterBandIfThenElse(h1, h1, h2);
    absl::Cleanup release_ite = [c_ite] {
      GDALComputedRasterBandRelease(c_ite);
    };
    EXPECT_EQ(nullptr, c_ite);

    EXPECT_EQ(nullptr, GDALRasterBandAsDataType(nullptr, GDT_Float64));
    EXPECT_EQ(nullptr, GDALRasterBandAsDataType(h1, GDT_Unknown));
    CPLErrorReset();
  }
  {
    GDALComputedRasterBandH c_dt = GDALRasterBandAsDataType(h1, GDT_Float64);
    absl::Cleanup release_dt = [c_dt] { GDALComputedRasterBandRelease(c_dt); };
    ASSERT_NE(nullptr, c_dt);
  }

  std::array<GDALRasterBandH, 2> bands = {h1, h2};
  std::array<GDALRasterBandH, 2> bad_bands = {h1, h_diff};

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALMaximumOfNBands(0, bands.data()));
    EXPECT_EQ(nullptr, GDALMaximumOfNBands(2, nullptr));
    EXPECT_EQ(nullptr, GDALMaximumOfNBands(2, bad_bands.data()));
    CPLErrorReset();
  }
  {
    GDALComputedRasterBandH c_max = GDALMaximumOfNBands(2, bands.data());
    absl::Cleanup release_max = [c_max] {
      GDALComputedRasterBandRelease(c_max);
    };
    ASSERT_NE(nullptr, c_max);
  }
  {
    GDALComputedRasterBandH c_maxc = GDALRasterBandMaxConstant(h1, 1.0);
    absl::Cleanup release_maxc = [c_maxc] {
      GDALComputedRasterBandRelease(c_maxc);
    };
    ASSERT_NE(nullptr, c_maxc);
  }

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALMinimumOfNBands(0, bands.data()));
    EXPECT_EQ(nullptr, GDALMinimumOfNBands(2, nullptr));
    EXPECT_EQ(nullptr, GDALMinimumOfNBands(2, bad_bands.data()));
    CPLErrorReset();
  }
  {
    GDALComputedRasterBandH c_min = GDALMinimumOfNBands(2, bands.data());
    absl::Cleanup release_min = [c_min] {
      GDALComputedRasterBandRelease(c_min);
    };
    ASSERT_NE(nullptr, c_min);
  }
  {
    GDALComputedRasterBandH c_minc = GDALRasterBandMinConstant(h1, 1.0);
    absl::Cleanup release_minc = [c_minc] {
      GDALComputedRasterBandRelease(c_minc);
    };
    ASSERT_NE(nullptr, c_minc);
  }

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALMeanOfNBands(0, bands.data()));
    EXPECT_EQ(nullptr, GDALMeanOfNBands(2, nullptr));
    EXPECT_EQ(nullptr, GDALMeanOfNBands(2, bad_bands.data()));
    CPLErrorReset();
  }
  {
    GDALComputedRasterBandH c_mean = GDALMeanOfNBands(2, bands.data());
    absl::Cleanup release_mean = [c_mean] {
      GDALComputedRasterBandRelease(c_mean);
    };
    ASSERT_NE(nullptr, c_mean);
  }

  // 4. C++ operators that work without muparser
  GDALRasterBand::ThrowIfNotSameDimensions(*b1, *b2);
  EXPECT_EQ(4, ((*b1) + (*b2)).GetXSize());
  EXPECT_EQ(4, ((*b1) + 2.0).GetXSize());
  EXPECT_EQ(4, (2.0 + (*b1)).GetXSize());
  EXPECT_EQ(4, ((*b1) - (*b2)).GetXSize());
  EXPECT_EQ(4, ((*b1) - 2.0).GetXSize());
  EXPECT_EQ(4, ((*b1) * (*b2)).GetXSize());
  EXPECT_EQ(4, ((*b1) * 2.0).GetXSize());
  EXPECT_EQ(4, (2.0 * (*b1)).GetXSize());
  EXPECT_EQ(4, ((*b1) / (*b2)).GetXSize());
  EXPECT_EQ(4, ((*b1) / 2.0).GetXSize());
  EXPECT_EQ(4, (2.0 / (*b1)).GetXSize());
  EXPECT_EQ(4, (!(*b1)).GetXSize());
  EXPECT_EQ(4, b1->AsType(GDT_Float64).GetXSize());
  EXPECT_EQ(4, gdal::max(*b1, *b2).GetXSize());
  EXPECT_EQ(4, gdal::max(*b1, 5.0).GetXSize());
  EXPECT_EQ(4, gdal::max(5.0, *b1).GetXSize());
  EXPECT_EQ(4, gdal::min(*b1, *b2).GetXSize());
  EXPECT_EQ(4, gdal::min(*b1, 5.0).GetXSize());
  EXPECT_EQ(4, gdal::min(5.0, *b1).GetXSize());
  EXPECT_EQ(4, gdal::mean(*b1, *b2).GetXSize());
  EXPECT_EQ(4, gdal::abs(*b1).GetXSize());
  EXPECT_EQ(4, gdal::fabs(*b1).GetXSize());
  EXPECT_EQ(4, gdal::sqrt(*b1).GetXSize());
  EXPECT_EQ(4, gdal::log10(*b1).GetXSize());
  EXPECT_EQ(4, gdal::pow(*b1, 2.0).GetXSize());
  EXPECT_EQ(4, gdal::pow(2.0, *b1).GetXSize());

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
  {
    WithQuietHandler quiet;
    CPLErrorReset();
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
    CPLErrorReset();
  }
#endif
}

}  // namespace
}  // namespace autotest2
