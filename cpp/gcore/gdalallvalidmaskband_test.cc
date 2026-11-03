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
// Tests GDALAllValidMaskBand (gcore/gdalallvalidmaskband.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalallvalidmaskband.cpp

#include <cstddef>
#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::Each;

TEST(GdalAllValidMaskBandTest, AllOperations) {
  GDALRegister_MEM();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  GDALDatasetUniquePtr ds(mem_driver->Create("", 4, 3, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);

  GDALAllValidMaskBand mask_band(ds->GetRasterBand(1));
  EXPECT_EQ(4, mask_band.GetXSize());
  EXPECT_EQ(3, mask_band.GetYSize());
  EXPECT_EQ(GDT_Byte, mask_band.GetRasterDataType());
  EXPECT_EQ(&mask_band, mask_band.GetMaskBand());
  EXPECT_EQ(GMF_ALL_VALID, mask_band.GetMaskFlags());

  int block_x = 0;
  int block_y = 0;
  mask_band.GetBlockSize(&block_x, &block_y);
  EXPECT_EQ(4, block_x);
  EXPECT_EQ(1, block_y);

  // ReadBlock triggers IReadBlock (which fills one block of size block_x *
  // block_y).
  std::vector<GByte> block(static_cast<size_t>(block_x) * block_y, 0);
  EXPECT_EQ(CE_None, mask_band.ReadBlock(0, 0, block.data()));
  EXPECT_THAT(block, Each(255));

  // RasterIO read triggers IRasterIO.
  GDALRasterIOExtraArg extra_arg;
  INIT_RASTERIO_EXTRA_ARG(extra_arg);
  std::vector<GByte> io_buf(4 * 3, 0);
  EXPECT_EQ(CE_None, mask_band.RasterIO(GF_Read, 0, 0, 4, 3, io_buf.data(), 4,
                                        3, GDT_Byte, 1, 4, &extra_arg));
  EXPECT_THAT(io_buf, Each(255));

  // Public RasterIO write and Fill trigger EmitErrorMessageIfWriteNotSupported.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              mask_band.RasterIO(GF_Write, 0, 0, 4, 3, io_buf.data(), 4, 3,
                                 GDT_Byte, 1, 4, &extra_arg));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NoWriteAccess, CPLGetLastErrorNo());
    CPLErrorReset();

    EXPECT_EQ(CE_Failure, mask_band.Fill(0));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NoWriteAccess, CPLGetLastErrorNo());
    CPLErrorReset();
  }

  // ComputeStatistics with non-null and null pointers.
  double min_val = 0.0;
  double max_val = 0.0;
  double mean_val = 0.0;
  double std_dev_val = -1.0;
  EXPECT_EQ(CE_None,
            mask_band.ComputeStatistics(FALSE, &min_val, &max_val, &mean_val,
                                        &std_dev_val, nullptr, nullptr));
  EXPECT_DOUBLE_EQ(255.0, min_val);
  EXPECT_DOUBLE_EQ(255.0, max_val);
  EXPECT_DOUBLE_EQ(255.0, mean_val);
  EXPECT_DOUBLE_EQ(0.0, std_dev_val);

  EXPECT_EQ(CE_None,
            mask_band.ComputeStatistics(TRUE, nullptr, nullptr, nullptr,
                                        nullptr, nullptr, nullptr));
}

}  // namespace
}  // namespace autotest2
