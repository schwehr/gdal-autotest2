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
// Tests GDALNoDataMaskBand (gcore/gdalnodatamaskband.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalnodatamaskband.cpp

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_maskbands.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::Each;

constexpr char kTestDir[] = "/vsimem/gdalnodatamaskband_test";
constexpr char kTiffPath[] =
    "/vsimem/gdalnodatamaskband_test/nodata_mask_partial.tif";

struct NoDataRangeCase {
  double value;
  GDALDataType data_type;
  bool expected;
};

constexpr NoDataRangeCase kNoDataRangeCases[] = {
    {.value = 0.0, .data_type = GDT_Byte, .expected = true},
    {.value = 300.0, .data_type = GDT_Byte, .expected = false},
    {.value = -5.0, .data_type = GDT_Int8, .expected = true},
    {.value = -1000.0, .data_type = GDT_Int16, .expected = true},
    {.value = 1000.0, .data_type = GDT_UInt16, .expected = true},
    {.value = 100000.0, .data_type = GDT_UInt32, .expected = true},
    {.value = -100000.0, .data_type = GDT_Int32, .expected = true},
    {.value = 100000.0, .data_type = GDT_UInt64, .expected = true},
    {.value = -100000.0, .data_type = GDT_Int64, .expected = true},
    {.value = std::numeric_limits<double>::quiet_NaN(),
     .data_type = GDT_Float16,
     .expected = true},
    {.value = std::numeric_limits<double>::infinity(),
     .data_type = GDT_Float32,
     .expected = true},
    {.value = 1.5, .data_type = GDT_Float32, .expected = true},
    {.value = 1e100, .data_type = GDT_Float64, .expected = true},
};

struct DataTypeMaskCase {
  GDALDataType data_type;
  double nodata;
};

constexpr DataTypeMaskCase kDataTypeMaskCases[] = {
    {.data_type = GDT_Byte, .nodata = 5.0},
    {.data_type = GDT_Int16, .nodata = -5.0},
    {.data_type = GDT_UInt16, .nodata = 5.0},
    {.data_type = GDT_UInt32, .nodata = 5.0},
    {.data_type = GDT_Int32, .nodata = -5.0},
    {.data_type = GDT_Float32, .nodata = 5.0},
    {.data_type = GDT_Float32,
     .nodata = std::numeric_limits<double>::quiet_NaN()},
    {.data_type = GDT_Float64, .nodata = 5.0},
    {.data_type = GDT_Float64,
     .nodata = std::numeric_limits<double>::quiet_NaN()},
    {.data_type = GDT_Int64, .nodata = -5.0},
    {.data_type = GDT_UInt64, .nodata = 5.0},
};

TEST(GdalNoDataMaskBandTest, IsNoDataInRangeAllTypes) {
  for (const auto& tc : kNoDataRangeCases) {
    EXPECT_EQ(tc.expected,
              GDALNoDataMaskBand::IsNoDataInRange(tc.value, tc.data_type));
  }
}

void TestMaskForDataType(GDALDriver* drv, GDALDataType dt, double nodata_val) {
  GDALDatasetUniquePtr ds(drv->Create("mem", 4, 4, 1, dt, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  if (dt == GDT_Int64) {
    EXPECT_EQ(CE_None,
              band->SetNoDataValueAsInt64(static_cast<int64_t>(nodata_val)));
  } else if (dt == GDT_UInt64) {
    EXPECT_EQ(CE_None,
              band->SetNoDataValueAsUInt64(static_cast<uint64_t>(nodata_val)));
  } else {
    EXPECT_EQ(CE_None, band->SetNoDataValue(nodata_val));
  }

  std::array<double, 16> vals = {};
  for (int i = 0; i < 16; ++i) {
    vals[i] = (i % 2 == 0) ? nodata_val : 10.0;
  }
  ASSERT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 4, 4, vals.data(), 4, 4,
                                    GDT_Float64, 0, 0, nullptr));

  // Test both constructors of GDALNoDataMaskBand.
  GDALNoDataMaskBand mask1(band);
  GDALNoDataMaskBand mask2(band, nodata_val);

  // Contiguous Byte read (nPixelSpace == 1, nLineSpace == nBufXSize).
  std::vector<GByte> out1(16, 99);
  EXPECT_EQ(CE_None, mask1.RasterIO(GF_Read, 0, 0, 4, 4, out1.data(), 4, 4,
                                    GDT_Byte, 1, 4, nullptr));
  EXPECT_EQ(0, out1[0]);
  EXPECT_EQ(255, out1[1]);

  // Strided line Byte read (nPixelSpace == 1, nLineSpace > nBufXSize).
  std::vector<GByte> out_stride(32, 99);
  EXPECT_EQ(CE_None, mask1.RasterIO(GF_Read, 0, 0, 4, 4, out_stride.data(), 4,
                                    4, GDT_Byte, 1, 8, nullptr));
  EXPECT_EQ(0, out_stride[0]);
  EXPECT_EQ(255, out_stride[1]);

  // Strided pixel Byte read (nPixelSpace == 2).
  std::vector<GByte> out_px(32, 99);
  EXPECT_EQ(CE_None, mask2.RasterIO(GF_Read, 0, 0, 4, 4, out_px.data(), 4, 4,
                                    GDT_Byte, 2, 8, nullptr));
  EXPECT_EQ(0, out_px[0]);
  EXPECT_EQ(255, out_px[2]);

  // Non-Byte output buffer (GDT_Int32).
  std::vector<int32_t> out_int32(16, -1);
  EXPECT_EQ(CE_None, mask1.RasterIO(GF_Read, 0, 0, 4, 4, out_int32.data(), 4, 4,
                                    GDT_Int32, 4, 16, nullptr));
  EXPECT_EQ(0, out_int32[0]);
  EXPECT_EQ(255, out_int32[1]);
}

TEST(GdalNoDataMaskBandTest, AllDataTypesAndBranches) {
  GDALRegister_MEM();
  GDALRegister_GTiff();
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);

  for (const auto& tc : kDataTypeMaskCases) {
    SCOPED_TRACE(GDALGetDataTypeName(tc.data_type));
    TestMaskForDataType(drv, tc.data_type, tc.nodata);
  }

  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup_vsimem = [] { VSIRmdirRecursive(kTestDir); };

  // Test partial block read in IReadBlock (using GTiff with tiled 16x16 on
  // 10x10 raster).
  GDALDriver* tiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, tiff_drv);
  CPLStringList options;
  options.SetNameValue("TILED", "YES");
  options.SetNameValue("BLOCKXSIZE", "16");
  options.SetNameValue("BLOCKYSIZE", "16");
  GDALDatasetUniquePtr tiff_ds(
      tiff_drv->Create(kTiffPath, 10, 10, 1, GDT_Int16, options.List()));
  ASSERT_NE(nullptr, tiff_ds);
  // Upstream magic hook in gcore/gdalnodatamaskband.cpp (IRasterIO) that
  // enables GDAL_SIMUL_MEM_ALLOC_FAILURE_NODATA_MASK_BAND on the dataset.
  tiff_ds->SetDescription("__debug__");
  GDALRasterBand* tband = tiff_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, tband);
  EXPECT_EQ(CE_None, tband->SetNoDataValue(0.0));
  GDALNoDataMaskBand tmask(tband);

  std::vector<GByte> block_buf(256, 99);
  EXPECT_EQ(CE_None, tmask.ReadBlock(0, 0, block_buf.data()));

  // Test simulated memory allocation failure fallback ("YES" and "ALWAYS").
  // GDALNoDataMaskBand falls back to returning CE_None with zero-filled mask
  // data and emits CE_Warning / CPLE_OutOfMemory.
  {
    WithQuietHandler quiet;
    {
      CPLErrorReset();
      CPLConfigOptionSetter opt_yes(
          "GDAL_SIMUL_MEM_ALLOC_FAILURE_NODATA_MASK_BAND", "YES",
          /*bSetOnlyIfUndefined=*/false);
      std::vector<GByte> out_buf(100, 99);
      EXPECT_EQ(CE_None, tmask.RasterIO(GF_Read, 0, 0, 10, 10, out_buf.data(),
                                        10, 10, GDT_Byte, 1, 10, nullptr));
      EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
      EXPECT_EQ(CPLE_OutOfMemory, CPLGetLastErrorNo());
      EXPECT_THAT(out_buf, Each(0));
    }
    {
      CPLErrorReset();
      CPLConfigOptionSetter opt_always(
          "GDAL_SIMUL_MEM_ALLOC_FAILURE_NODATA_MASK_BAND", "ALWAYS",
          /*bSetOnlyIfUndefined=*/false);
      std::vector<GByte> out_buf(100, 99);
      EXPECT_EQ(CE_None, tmask.RasterIO(GF_Read, 0, 0, 10, 10, out_buf.data(),
                                        10, 10, GDT_Byte, 1, 10, nullptr));
      EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
      EXPECT_EQ(CPLE_OutOfMemory, CPLGetLastErrorNo());
      EXPECT_THAT(out_buf, Each(0));
    }

    // Test write failure and EmitErrorMessageIfWriteNotSupported (via Fill).
    CPLErrorReset();
    std::vector<GByte> dummy(4, 0);
    EXPECT_EQ(CE_Failure, tmask.RasterIO(GF_Write, 0, 0, 2, 2, dummy.data(), 2,
                                         2, GDT_Byte, 1, 2, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLErrorReset();
    EXPECT_EQ(CE_Failure, tmask.Fill(0.0));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
