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
// Tests SSSE3/NEON interleaved and transposed RasterIO specializations
// (gcore/rasterio_ssse3.cpp).
//
// Build caveat in google3: //third_party/gdal/BUILD does not define
// HAVE_SSSE3_AT_COMPILE_TIME or USE_NEON_OPTIMIZATIONS, so
// gcore/rasterio_ssse3.cpp compiles to 0 lines in google3 builds. This test
// exercises the 3/4/5-band interleaved and 2D transpose RasterIO paths (>16
// pixels to cross both SIMD vector width and scalar tail when enabled), which
// route through generic GDALDeinterleave/GDALTranspose2D in google3.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/rasterio_ssse3.cpp

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

TEST(RasterioSsse3Test, InterleavedAndTransposedRasterIO) {
  GDALRegister_MEM();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  // Use kSize = 35 (> 16-pixel SIMD block width) so both full SIMD vectors and
  // the scalar tail execute when SSSE3/NEON optimizations are compiled in.
  constexpr int kSize = 35;
  constexpr auto kBandCounts = std::to_array<int>({3, 4, 5});
  for (int bands : kBandCounts) {
    GDALDatasetUniquePtr ds(
        mem_driver->Create("", kSize, kSize, bands, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);

    std::vector<GByte> buf(static_cast<size_t>(kSize) * kSize * bands, 0);
    for (size_t i = 0; i < buf.size(); ++i) {
      buf[i] = static_cast<GByte>(i & 0xFF);
    }

    // Write pixel-interleaved data.
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Write, 0, 0, kSize, kSize, buf.data(),
                                    kSize, kSize, GDT_Byte, bands, nullptr,
                                    bands, kSize * bands, 1, nullptr));

    // Read back pixel-interleaved data.
    std::vector<GByte> out(static_cast<size_t>(kSize) * kSize * bands, 0);
    EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, kSize, kSize, out.data(),
                                    kSize, kSize, GDT_Byte, bands, nullptr,
                                    bands, kSize * bands, 1, nullptr));
    EXPECT_EQ(buf, out);

    // Read transposed single band.
    std::vector<GByte> transposed(static_cast<size_t>(kSize) * kSize, 0);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, kSize, kSize, transposed.data(),
                           kSize, kSize, GDT_Byte, kSize, 1, nullptr));
    for (int y = 0; y < kSize; ++y) {
      for (int x = 0; x < kSize; ++x) {
        EXPECT_EQ(buf[(static_cast<size_t>(y) * kSize + x) * bands],
                  transposed[static_cast<size_t>(x) * kSize + y]);
      }
    }
  }
}

}  // namespace
}  // namespace autotest2
