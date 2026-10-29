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

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"

namespace gdal {
namespace {

TEST(GdalNoDataMaskBandTest, IsNoDataInRangeAllTypes) {
  EXPECT_TRUE(GDALNoDataMaskBand::IsNoDataInRange(0.0, GDT_Byte));
  EXPECT_FALSE(GDALNoDataMaskBand::IsNoDataInRange(300.0, GDT_Byte));
  EXPECT_TRUE(GDALNoDataMaskBand::IsNoDataInRange(-5.0, GDT_Int8));
  EXPECT_TRUE(GDALNoDataMaskBand::IsNoDataInRange(-1000.0, GDT_Int16));
  EXPECT_TRUE(GDALNoDataMaskBand::IsNoDataInRange(1000.0, GDT_UInt16));
  EXPECT_TRUE(GDALNoDataMaskBand::IsNoDataInRange(100000.0, GDT_UInt32));
  EXPECT_TRUE(GDALNoDataMaskBand::IsNoDataInRange(-100000.0, GDT_Int32));
  EXPECT_TRUE(GDALNoDataMaskBand::IsNoDataInRange(100000.0, GDT_UInt64));
  EXPECT_TRUE(GDALNoDataMaskBand::IsNoDataInRange(-100000.0, GDT_Int64));
  EXPECT_TRUE(GDALNoDataMaskBand::IsNoDataInRange(
      std::numeric_limits<double>::quiet_NaN(), GDT_Float16));
  EXPECT_TRUE(GDALNoDataMaskBand::IsNoDataInRange(
      std::numeric_limits<double>::infinity(), GDT_Float32));
  EXPECT_TRUE(GDALNoDataMaskBand::IsNoDataInRange(1.5, GDT_Float32));
  EXPECT_TRUE(GDALNoDataMaskBand::IsNoDataInRange(1e100, GDT_Float64));
}

void TestMaskForDataType(GDALDriver* drv, GDALDataType dt, double nodata_val) {
  std::unique_ptr<GDALDataset> ds(drv->Create("mem", 4, 4, 1, dt, nullptr));
  ASSERT_NE(ds, nullptr);
  GDALRasterBand* band = ds->GetRasterBand(1);
  if (dt == GDT_Int64) {
    EXPECT_EQ(band->SetNoDataValueAsInt64(static_cast<int64_t>(nodata_val)),
              CE_None);
  } else if (dt == GDT_UInt64) {
    EXPECT_EQ(band->SetNoDataValueAsUInt64(static_cast<uint64_t>(nodata_val)),
              CE_None);
  } else {
    EXPECT_EQ(band->SetNoDataValue(nodata_val), CE_None);
  }

  double vals[16];
  for (int i = 0; i < 16; ++i) {
    vals[i] = (i % 2 == 0) ? nodata_val : 10.0;
  }
  EXPECT_EQ(band->RasterIO(GF_Write, 0, 0, 4, 4, vals, 4, 4, GDT_Float64, 0, 0,
                           nullptr),
            CE_None);

  // Test both constructors of GDALNoDataMaskBand
  GDALNoDataMaskBand mask1(band);
  GDALNoDataMaskBand mask2(band, nodata_val);

  // Contiguous Byte read (nPixelSpace == 1, nLineSpace == nBufXSize)
  GByte out1[16] = {0};
  EXPECT_EQ(
      mask1.RasterIO(GF_Read, 0, 0, 4, 4, out1, 4, 4, GDT_Byte, 1, 4, nullptr),
      CE_None);
  EXPECT_EQ(out1[0], 0);
  EXPECT_EQ(out1[1], 255);

  // Strided line Byte read (nPixelSpace == 1, nLineSpace > nBufXSize)
  GByte out_stride[32] = {0};
  EXPECT_EQ(mask1.RasterIO(GF_Read, 0, 0, 4, 4, out_stride, 4, 4, GDT_Byte, 1,
                           8, nullptr),
            CE_None);
  EXPECT_EQ(out_stride[0], 0);
  EXPECT_EQ(out_stride[1], 255);

  // Strided pixel Byte read (nPixelSpace == 2)
  GByte out_px[32] = {0};
  EXPECT_EQ(mask2.RasterIO(GF_Read, 0, 0, 4, 4, out_px, 4, 4, GDT_Byte, 2, 8,
                           nullptr),
            CE_None);
  EXPECT_EQ(out_px[0], 0);
  EXPECT_EQ(out_px[2], 255);

  // Non-Byte output buffer (e.g. GDT_Int32)
  int32_t out_int32[16] = {0};
  EXPECT_EQ(mask1.RasterIO(GF_Read, 0, 0, 4, 4, out_int32, 4, 4, GDT_Int32, 4,
                           16, nullptr),
            CE_None);
  EXPECT_EQ(out_int32[0], 0);
  EXPECT_EQ(out_int32[1], 255);
}

TEST(GdalNoDataMaskBandTest, AllDataTypesAndBranches) {
  GDALAllRegister();
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(drv, nullptr);

  TestMaskForDataType(drv, GDT_Byte, 5.0);
  TestMaskForDataType(drv, GDT_Int16, -5.0);
  TestMaskForDataType(drv, GDT_UInt16, 5.0);
  TestMaskForDataType(drv, GDT_UInt32, 5.0);
  TestMaskForDataType(drv, GDT_Int32, -5.0);
  TestMaskForDataType(drv, GDT_Float32, 5.0);
  TestMaskForDataType(drv, GDT_Float32,
                      std::numeric_limits<double>::quiet_NaN());
  TestMaskForDataType(drv, GDT_Float64, 5.0);
  TestMaskForDataType(drv, GDT_Float64,
                      std::numeric_limits<double>::quiet_NaN());
  TestMaskForDataType(drv, GDT_Int64, -5.0);
  TestMaskForDataType(drv, GDT_UInt64, 5.0);

  // Test partial block read in IReadBlock (using GTiff with tiled 16x16 on
  // 10x10 raster)
  GDALDriver* tiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(tiff_drv, nullptr);
  const char* tiff_path = "/vsimem/nodata_mask_partial.tif";
  const char* opts[] = {"TILED=YES", "BLOCKXSIZE=16", "BLOCKYSIZE=16", nullptr};
  std::unique_ptr<GDALDataset> tiff_ds(tiff_drv->Create(
      tiff_path, 10, 10, 1, GDT_Int16, const_cast<char**>(opts)));
  ASSERT_NE(tiff_ds, nullptr);
  tiff_ds->SetDescription("__debug__");
  GDALRasterBand* tband = tiff_ds->GetRasterBand(1);
  EXPECT_EQ(tband->SetNoDataValue(0.0), CE_None);
  GDALNoDataMaskBand tmask(tband);

  GByte block_buf[256] = {0};
  EXPECT_EQ(tmask.ReadBlock(0, 0, block_buf), CE_None);

  // Test simulated memory allocation failure fallback ("YES" and "ALWAYS")
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);
  {
    CPLConfigOptionSetter opt_yes(
        "GDAL_SIMUL_MEM_ALLOC_FAILURE_NODATA_MASK_BAND", "YES", false);
    GByte out_buf[100] = {0};
    EXPECT_EQ(tmask.RasterIO(GF_Read, 0, 0, 10, 10, out_buf, 10, 10, GDT_Byte,
                             1, 10, nullptr),
              CE_None);
  }
  {
    CPLConfigOptionSetter opt_always(
        "GDAL_SIMUL_MEM_ALLOC_FAILURE_NODATA_MASK_BAND", "ALWAYS", false);
    GByte out_buf[100] = {0};
    EXPECT_EQ(tmask.RasterIO(GF_Read, 0, 0, 10, 10, out_buf, 10, 10, GDT_Byte,
                             1, 10, nullptr),
              CE_None);
  }

  // Test write failure and EmitErrorMessageIfWriteNotSupported (via Fill)
  GByte dummy[4] = {0};
  EXPECT_EQ(tmask.RasterIO(GF_Write, 0, 0, 2, 2, dummy, 2, 2, GDT_Byte, 1, 2,
                           nullptr),
            CE_Failure);
  EXPECT_EQ(tmask.Fill(0.0), CE_Failure);

  tiff_ds.reset();
  VSIUnlink(tiff_path);
}

}  // namespace
}  // namespace gdal
