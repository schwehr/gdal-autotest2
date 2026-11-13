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
// Tests pixel/line <-> PCS coordinate transforms (geo_trans.c).
//
// See also:
//   https://github.com/OSGeo/libgeotiff/blob/master/libgeotiff/geo_trans.c

#include <array>

#include "testing/base/public/gunit.h"
#include "third_party/libgeotiff/geo_simpletags.h"
#include "third_party/libgeotiff/geotiff.h"
#include "third_party/libgeotiff/google/util/scoped_geotiff.h"
#include "third_party/libgeotiff/libxtiff/xtiffio.h"  // IWYU pragma: keep

namespace libgeotiff {
namespace {

TEST(GeoTransTest, MissingAndInsufficientTagsFail) {
  SimpleTagsGtif empty = MakeSimpleTagsGtif();
  ASSERT_NE(nullptr, empty.gtif);
  double x = 1.0;
  double y = 2.0;
  EXPECT_EQ(0, GTIFImageToPCS(empty.gtif.get(), &x, &y));
  EXPECT_EQ(0, GTIFPCSToImage(empty.gtif.get(), &x, &y));

  // Insufficient tiepoint/pixelscale/matrix counts fall through cleanly.
  SimpleTagsGtif bad = MakeSimpleTagsGtif();
  ASSERT_NE(nullptr, bad.gtif);
  std::array<double, 3> short_tie = {0.0, 0.0, 0.0};
  std::array<double, 2> short_scale = {10.0, 10.0};
  std::array<double, 4> short_matrix = {1.0, 0.0, 0.0, 0.0};
  ASSERT_EQ(1, SetSimpleTag(bad.st.get(), TIFFTAG_GEOTIEPOINTS, STT_DOUBLE,
                            short_tie));
  ASSERT_EQ(1, SetSimpleTag(bad.st.get(), TIFFTAG_GEOPIXELSCALE, STT_DOUBLE,
                            short_scale));
  ASSERT_EQ(1, SetSimpleTag(bad.st.get(), TIFFTAG_GEOTRANSMATRIX, STT_DOUBLE,
                            short_matrix));
  EXPECT_EQ(0, GTIFImageToPCS(bad.gtif.get(), &x, &y));
  EXPECT_EQ(0, GTIFPCSToImage(bad.gtif.get(), &x, &y));
}

TEST(GeoTransTest, TiepointsOnlyReturnsFalse) {
  SimpleTagsGtif handle = MakeSimpleTagsGtif();
  ASSERT_NE(nullptr, handle.gtif);

  // More than one tiepoint (> 6 values) and no ModelPixelScaleTag hits
  // GTIFTiepointTranslate, which is unimplemented and returns 0.
  std::array<double, 12> multi_tiepoints = {
      0.0,   0.0,  0.0, 500000.0, 4000000.0, 0.0,
      100.0, 50.0, 0.0, 501000.0, 3999500.0, 0.0,
  };
  ASSERT_EQ(1, SetSimpleTag(handle.st.get(), TIFFTAG_GEOTIEPOINTS, STT_DOUBLE,
                            multi_tiepoints));

  double x = 25.0;
  double y = 10.0;
  EXPECT_EQ(0, GTIFImageToPCS(handle.gtif.get(), &x, &y));
  EXPECT_EQ(0, GTIFPCSToImage(handle.gtif.get(), &x, &y));
}

TEST(GeoTransTest, TiepointAndPixelScaleRoundTrip) {
  SimpleTagsGtif handle = MakeSimpleTagsGtif();
  ASSERT_NE(nullptr, handle.gtif);

  std::array<double, 6> tiepoints = {10.0, 20.0, 0.0, 500000.0, 4000000.0, 0.0};
  std::array<double, 3> pixel_scale = {15.0, 30.0, 0.0};
  ASSERT_EQ(1, SetSimpleTag(handle.st.get(), TIFFTAG_GEOTIEPOINTS, STT_DOUBLE,
                            tiepoints));
  ASSERT_EQ(1, SetSimpleTag(handle.st.get(), TIFFTAG_GEOPIXELSCALE, STT_DOUBLE,
                            pixel_scale));

  double x = 14.0;
  double y = 25.0;
  ASSERT_EQ(1, GTIFImageToPCS(handle.gtif.get(), &x, &y));
  EXPECT_DOUBLE_EQ(500060.0, x);
  EXPECT_DOUBLE_EQ(3999850.0, y);

  ASSERT_EQ(1, GTIFPCSToImage(handle.gtif.get(), &x, &y));
  EXPECT_DOUBLE_EQ(14.0, x);
  EXPECT_DOUBLE_EQ(25.0, y);
}

TEST(GeoTransTest, InvertibleAndSingularGeoTransMatrix) {
  // 1. Invertible affine matrix with rotation/shear terms.
  {
    SimpleTagsGtif handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, handle.gtif);

    std::array<double, 16> matrix = {
        2.0, 0.5, 0.0, 1000.0, -0.5, 3.0, 0.0, 2000.0,
        0.0, 0.0, 1.0, 0.0,    0.0,  0.0, 0.0, 1.0,
    };
    ASSERT_EQ(1, SetSimpleTag(handle.st.get(), TIFFTAG_GEOTRANSMATRIX,
                              STT_DOUBLE, matrix));

    double x = 10.0;
    double y = 20.0;
    ASSERT_EQ(1, GTIFImageToPCS(handle.gtif.get(), &x, &y));
    EXPECT_DOUBLE_EQ(1030.0, x);
    EXPECT_DOUBLE_EQ(2055.0, y);

    ASSERT_EQ(1, GTIFPCSToImage(handle.gtif.get(), &x, &y));
    EXPECT_NEAR(10.0, x, 1e-12);
    EXPECT_NEAR(20.0, y, 1e-12);
  }

  // 2. Singular matrix (determinant == 0) succeeds forward, fails inverse.
  {
    SimpleTagsGtif handle = MakeSimpleTagsGtif();
    ASSERT_NE(nullptr, handle.gtif);

    std::array<double, 16> singular = {
        1.0, 2.0, 0.0, 10.0, 2.0, 4.0, 0.0, 20.0,
        0.0, 0.0, 1.0, 0.0,  0.0, 0.0, 0.0, 1.0,
    };
    ASSERT_EQ(1, SetSimpleTag(handle.st.get(), TIFFTAG_GEOTRANSMATRIX,
                              STT_DOUBLE, singular));

    double x = 3.0;
    double y = 4.0;
    ASSERT_EQ(1, GTIFImageToPCS(handle.gtif.get(), &x, &y));
    EXPECT_DOUBLE_EQ(21.0, x);
    EXPECT_DOUBLE_EQ(42.0, y);

    EXPECT_EQ(0, GTIFPCSToImage(handle.gtif.get(), &x, &y));
  }
}

}  // namespace
}  // namespace libgeotiff
