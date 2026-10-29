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
// Tests gcore/rasterio_ssse3.cpp.

#include "third_party/gdal/gcore/rasterio_ssse3.h"

#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"

namespace {

TEST(RasterioSsse3Test, InterleavedAndTransposedRasterIO) {
  GDALAllRegister();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  // Exercise 3-band and 4-band interleaved read/write and 2D transpose paths.
  for (int bands : {3, 4, 5}) {
    std::unique_ptr<GDALDataset> ds(
        mem_driver->Create("", 35, 35, bands, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);

    std::vector<GByte> buf(35 * 35 * bands, 0);
    for (size_t i = 0; i < buf.size(); ++i) {
      buf[i] = static_cast<GByte>(i & 0xFF);
    }

    // Write pixel-interleaved data.
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Write, 0, 0, 35, 35, buf.data(), 35, 35, GDT_Byte,
                           bands, nullptr, bands, 35 * bands, 1, nullptr));

    // Read back pixel-interleaved data.
    std::vector<GByte> out(35 * 35 * bands, 0);
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, 35, 35, out.data(), 35, 35, GDT_Byte,
                           bands, nullptr, bands, 35 * bands, 1, nullptr));
    EXPECT_EQ(buf, out);

    // Read transposed single band.
    std::vector<GByte> transposed(35 * 35, 0);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 35, 35, transposed.data(), 35, 35,
                           GDT_Byte, 35, 1, nullptr));
  }
}

}  // namespace
