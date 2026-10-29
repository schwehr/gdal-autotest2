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
//
// Tests gcore/gdaldefaultasync.cpp.

#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"

CPL_C_START
GDALAsyncReader* GDALGetDefaultAsyncReader(GDALDataset* poDS, int nXOff,
                                           int nYOff, int nXSize, int nYSize,
                                           void* pBuf, int nBufXSize,
                                           int nBufYSize, GDALDataType eBufType,
                                           int nBandCount, int* panBandMap,
                                           int nPixelSpace, int nLineSpace,
                                           int nBandSpace, char** papszOptions);
CPL_C_END

namespace {

TEST(GdaldefaultasyncTest, DefaultAsyncReaderSuccessAndError) {
  GDALAllRegister();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  std::unique_ptr<GDALDataset> ds(
      mem_driver->Create("", 4, 4, 2, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);

  EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(11));
  EXPECT_EQ(CE_None, ds->GetRasterBand(2)->Fill(22));

  // Case 1: panBandMap is nullptr (default 1..nBandCount).
  std::vector<GByte> buf(4 * 4 * 2, 0);
  CPLStringList options;
  options.SetNameValue("KEY", "VALUE");
  GDALAsyncReader* reader =
      ds->BeginAsyncReader(0, 0, 4, 4, buf.data(), 4, 4, GDT_Byte, 2, nullptr,
                           0, 0, 0, options.List());
  ASSERT_NE(nullptr, reader);
  EXPECT_EQ(ds.get(), reader->GetGDALDataset());
  EXPECT_EQ(0, reader->GetXOffset());
  EXPECT_EQ(0, reader->GetYOffset());
  EXPECT_EQ(4, reader->GetXSize());
  EXPECT_EQ(4, reader->GetYSize());
  EXPECT_EQ(buf.data(), reader->GetBuffer());
  EXPECT_EQ(4, reader->GetBufferXSize());
  EXPECT_EQ(4, reader->GetBufferYSize());
  EXPECT_EQ(GDT_Byte, reader->GetBufferType());
  EXPECT_EQ(2, reader->GetBandCount());
  EXPECT_NE(nullptr, reader->GetBandMap());
  EXPECT_EQ(0, reader->GetPixelSpace());
  EXPECT_EQ(0, reader->GetLineSpace());
  EXPECT_EQ(0, reader->GetBandSpace());

  GDALAsyncReaderH handle = static_cast<GDALAsyncReaderH>(reader);

  EXPECT_TRUE(GDALARLockBuffer(handle, 0.0));
  GDALARUnlockBuffer(handle);

  int x_off = -1;
  int y_off = -1;
  int x_size = -1;
  int y_size = -1;
  EXPECT_EQ(GARIO_COMPLETE, GDALARGetNextUpdatedRegion(
                                handle, 0.0, &x_off, &y_off, &x_size, &y_size));
  EXPECT_EQ(0, x_off);
  EXPECT_EQ(0, y_off);
  EXPECT_EQ(4, x_size);
  EXPECT_EQ(4, y_size);
  EXPECT_EQ(11, buf[0]);
  EXPECT_EQ(22, buf[16]);

  ds->EndAsyncReader(reader);

  // Case 2: Explicit panBandMap and out-of-bounds read to trigger GARIO_ERROR.
  int band_map[1] = {2};
  GDALAsyncReader* err_reader =
      GDALGetDefaultAsyncReader(ds.get(), 0, 0, 100, 100, buf.data(), 4, 4,
                                GDT_Byte, 1, band_map, 0, 0, 0, nullptr);
  ASSERT_NE(nullptr, err_reader);

  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(GARIO_ERROR, err_reader->GetNextUpdatedRegion(0.0, &x_off, &y_off,
                                                          &x_size, &y_size));
  // Null pointer checks on C API.
  EXPECT_EQ(GARIO_ERROR, GDALARGetNextUpdatedRegion(nullptr, 0.0, &x_off,
                                                    &y_off, &x_size, &y_size));
  EXPECT_FALSE(GDALARLockBuffer(nullptr, 0.0));
  GDALARUnlockBuffer(nullptr);
  CPLPopErrorHandler();

  ds->EndAsyncReader(err_reader);
}

}  // namespace
