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
// Tests default async reader (gcore/gdaldefaultasync.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdaldefaultasync.cpp

#include <array>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

// Internal helper symbol in gcore/gdaldefaultasync.cpp not exported by a public
// header; keep in sync with third_party/gdal/gcore/gdaldefaultasync.cpp.
CPL_C_START
GDALAsyncReader* GDALGetDefaultAsyncReader(GDALDataset* poDS, int nXOff,
                                           int nYOff, int nXSize, int nYSize,
                                           void* pBuf, int nBufXSize,
                                           int nBufYSize, GDALDataType eBufType,
                                           int nBandCount, int* panBandMap,
                                           int nPixelSpace, int nLineSpace,
                                           int nBandSpace, char** papszOptions);
CPL_C_END

namespace autotest2 {
namespace {

TEST(GdalDefaultAsyncTest, DefaultAsyncReaderSuccessAndError) {
  GDALRegister_MEM();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  GDALDatasetUniquePtr ds(mem_driver->Create("", 4, 4, 2, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);

  EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(11));
  EXPECT_EQ(CE_None, ds->GetRasterBand(2)->Fill(22));

  std::vector<GByte> buf(4 * 4 * 2, 0);
  int x_off = -1;
  int y_off = -1;
  int x_size = -1;
  int y_size = -1;

  // Case 1: panBandMap is nullptr (default 1..nBandCount).
  {
    CPLStringList options;
    options.SetNameValue("KEY", "VALUE");
    GDALAsyncReader* reader =
        ds->BeginAsyncReader(0, 0, 4, 4, buf.data(), 4, 4, GDT_Byte, 2, nullptr,
                             0, 0, 0, options.List());
    ASSERT_NE(nullptr, reader);
    absl::Cleanup end_reader = [&ds, reader] { ds->EndAsyncReader(reader); };

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

    EXPECT_EQ(GARIO_COMPLETE,
              GDALARGetNextUpdatedRegion(handle, 0.0, &x_off, &y_off, &x_size,
                                         &y_size));
    EXPECT_EQ(0, x_off);
    EXPECT_EQ(0, y_off);
    EXPECT_EQ(4, x_size);
    EXPECT_EQ(4, y_size);
    EXPECT_EQ(11, buf[0]);
    EXPECT_EQ(22, buf[16]);
  }

  // Case 2: Explicit panBandMap and out-of-bounds read to trigger GARIO_ERROR.
  {
    std::array<int, 1> band_map = {2};
    GDALAsyncReader* err_reader = GDALGetDefaultAsyncReader(
        ds.get(), 0, 0, 100, 100, buf.data(), 4, 4, GDT_Byte, 1,
        band_map.data(), 0, 0, 0, nullptr);
    ASSERT_NE(nullptr, err_reader);
    absl::Cleanup end_err_reader = [&ds, err_reader] {
      ds->EndAsyncReader(err_reader);
    };

    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(GARIO_ERROR, err_reader->GetNextUpdatedRegion(
                                 0.0, &x_off, &y_off, &x_size, &y_size));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      CPLErrorReset();

      // Null pointer checks on C API.
      EXPECT_EQ(GARIO_ERROR,
                GDALARGetNextUpdatedRegion(nullptr, 0.0, &x_off, &y_off,
                                           &x_size, &y_size));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      CPLErrorReset();

      EXPECT_FALSE(GDALARLockBuffer(nullptr, 0.0));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      CPLErrorReset();

      GDALARUnlockBuffer(nullptr);
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      CPLErrorReset();
    }
  }
}

}  // namespace
}  // namespace autotest2
