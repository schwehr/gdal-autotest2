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
// Tests private libtiff bridge and memory wrappers (geo_tiffp.c).
//
// See also:
//   https://github.com/OSGeo/libgeotiff/blob/master/libgeotiff/geo_tiffp.c

#include "third_party/libgeotiff/geo_tiffp.h"

#include <array>
#include <cstdint>
#include <cstring>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/types/span.h"
#include "third_party/libgeotiff/geotiff.h"
#include "third_party/libgeotiff/google/util/scoped_geotiff.h"
#include "third_party/libgeotiff/google/util/tiff_mem_stream.h"
#include "third_party/libgeotiff/libxtiff/xtiffio.h"  // IWYU pragma: keep

namespace libgeotiff {
namespace {

using ::testing::Each;

TEST(GeoTiffpTest, MemoryWrappersZeroAllocateReallocateCopyAndFree) {
  auto* buf = static_cast<uint8_t*>(_GTIFcalloc(16));
  ASSERT_NE(nullptr, buf);
  ScopedRawGtifMem buf_guard(buf);

  EXPECT_THAT(absl::MakeConstSpan(buf, 16), Each(0));

  std::array<uint8_t, 4> pattern = {0xAA, 0xBB, 0xCC, 0xDD};
  _GTIFmemcpy(buf, pattern.data(), pattern.size());

  auto* grown = static_cast<uint8_t*>(_GTIFrealloc(buf_guard.release(), 32));
  if (grown == nullptr) {
    buf_guard.reset(buf);
  }
  ASSERT_NE(nullptr, grown);
  buf_guard.reset(grown);
  EXPECT_EQ(0, std::memcmp(grown, pattern.data(), pattern.size()));

  _GTIFFree(nullptr);
}

TEST(GeoTiffpTest, DefaultMethodsMapTagTypesAndRoundTripOverLibtiff) {
  _GTIFSetDefaultTIFF(nullptr);

  TIFFMethod methods{};
  _GTIFSetDefaultTIFF(&methods);
  ASSERT_NE(nullptr, methods.get);
  ASSERT_NE(nullptr, methods.set);
  ASSERT_NE(nullptr, methods.type);

  EXPECT_EQ(TYPE_ASCII, methods.type(nullptr, GTIFF_ASCIIPARAMS));
  EXPECT_EQ(TYPE_DOUBLE, methods.type(nullptr, GTIFF_PIXELSCALE));
  EXPECT_EQ(TYPE_DOUBLE, methods.type(nullptr, GTIFF_TRANSMATRIX));
  EXPECT_EQ(TYPE_DOUBLE, methods.type(nullptr, GTIFF_TIEPOINTS));
  EXPECT_EQ(TYPE_DOUBLE, methods.type(nullptr, GTIFF_DOUBLEPARAMS));
  EXPECT_EQ(TYPE_SHORT, methods.type(nullptr, GTIFF_GEOKEYDIRECTORY));
  EXPECT_EQ(TYPE_UNKNOWN, methods.type(nullptr, 12345));

  // 1. Write all GeoTIFF tags through TIFFMethod::set into a memory stream.
  MemoryTiffStream stream;
  {
    ScopedTiff tif = stream.Open("w");
    ASSERT_NE(nullptr, tif);
    ASSERT_TRUE(WriteMinimalBaselineImage(tif.get()));

    char ascii[] = "WGS 84 UTM|";
    std::array<pinfo_t, 8> key_dir = {1, 1, 0, 1, 1024, 0, 1, 1};
    std::array<double, 2> doubles = {6378137.0, 298.257223563};
    std::array<double, 3> pixel_scale = {10.0, 10.0, 0.0};
    std::array<double, 6> tiepoints = {0.0, 0.0, 0.0, 500000.0, 4000000.0, 0.0};
    std::array<double, 16> matrix = {
        1.0, 0.0, 0.0, 500000.0, 0.0, -1.0, 0.0, 4000000.0,
        0.0, 0.0, 1.0, 0.0,      0.0, 0.0,  0.0, 1.0,
    };

    EXPECT_EQ(1, methods.set(tif.get(), GTIFF_ASCIIPARAMS, 0, ascii));
    EXPECT_EQ(1, methods.set(tif.get(), GTIFF_GEOKEYDIRECTORY,
                             static_cast<int>(key_dir.size()), key_dir.data()));
    EXPECT_EQ(1, methods.set(tif.get(), GTIFF_DOUBLEPARAMS,
                             static_cast<int>(doubles.size()), doubles.data()));
    EXPECT_EQ(1, methods.set(tif.get(), GTIFF_PIXELSCALE,
                             static_cast<int>(pixel_scale.size()),
                             pixel_scale.data()));
    EXPECT_EQ(
        1, methods.set(tif.get(), GTIFF_TIEPOINTS,
                       static_cast<int>(tiepoints.size()), tiepoints.data()));
    EXPECT_EQ(1, methods.set(tif.get(), GTIFF_TRANSMATRIX,
                             static_cast<int>(matrix.size()), matrix.data()));
    ASSERT_EQ(1, TIFFWriteDirectory(tif.get()));
  }

  // 2. Read back all written tags through TIFFMethod::get.
  {
    ScopedTiff tif = stream.Open("r");
    ASSERT_NE(nullptr, tif);

    int count = 0;
    char* ascii = nullptr;
    ASSERT_EQ(1, methods.get(tif.get(), GTIFF_ASCIIPARAMS, &count, &ascii));
    ScopedRawGtifMem ascii_guard(ascii);
    EXPECT_EQ(12, count);
    EXPECT_STREQ("WGS 84 UTM|", ascii);

    pinfo_t* key_dir = nullptr;
    ASSERT_EQ(1,
              methods.get(tif.get(), GTIFF_GEOKEYDIRECTORY, &count, &key_dir));
    ScopedRawGtifMem dir_guard(key_dir);
    EXPECT_EQ(8, count);
    EXPECT_EQ(1024, key_dir[4]);

    double* doubles = nullptr;
    ASSERT_EQ(1, methods.get(tif.get(), GTIFF_DOUBLEPARAMS, &count, &doubles));
    ScopedRawGtifMem doubles_guard(doubles);
    EXPECT_EQ(2, count);
    EXPECT_DOUBLE_EQ(6378137.0, doubles[0]);

    double* pixel_scale = nullptr;
    ASSERT_EQ(1,
              methods.get(tif.get(), GTIFF_PIXELSCALE, &count, &pixel_scale));
    ScopedRawGtifMem scale_guard(pixel_scale);
    EXPECT_EQ(3, count);
    EXPECT_DOUBLE_EQ(10.0, pixel_scale[0]);

    double* tiepoints = nullptr;
    ASSERT_EQ(1, methods.get(tif.get(), GTIFF_TIEPOINTS, &count, &tiepoints));
    ScopedRawGtifMem tie_guard(tiepoints);
    EXPECT_EQ(6, count);
    EXPECT_DOUBLE_EQ(500000.0, tiepoints[3]);

    double* matrix = nullptr;
    ASSERT_EQ(1, methods.get(tif.get(), GTIFF_TRANSMATRIX, &count, &matrix));
    ScopedRawGtifMem mat_guard(matrix);
    EXPECT_EQ(16, count);
    EXPECT_DOUBLE_EQ(500000.0, matrix[3]);
  }

  // 3. Missing ASCII and array tags return 0 without allocating.
  MemoryTiffStream empty_tags_stream;
  {
    ScopedTiff tif = empty_tags_stream.Open("w");
    ASSERT_NE(nullptr, tif);
    ASSERT_TRUE(WriteMinimalBaselineImage(tif.get()));
    ASSERT_EQ(1, TIFFWriteDirectory(tif.get()));
  }
  {
    ScopedTiff tif = empty_tags_stream.Open("r");
    ASSERT_NE(nullptr, tif);
    int count = 0;
    void* val = nullptr;
    EXPECT_EQ(0, methods.get(tif.get(), GTIFF_ASCIIPARAMS, &count, &val));
    EXPECT_EQ(0, methods.get(tif.get(), GTIFF_DOUBLEPARAMS, &count, &val));
  }
}

}  // namespace
}  // namespace libgeotiff
