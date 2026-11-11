// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Unit tests for third_party/geos/src/operation/overlayng/PrecisionUtil.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/overlayng/PrecisionUtil.cpp

#include "third_party/geos/include/geos/operation/overlayng/PrecisionUtil.h"

#include <memory>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::operation::overlayng::PrecisionUtil;

TEST(PrecisionUtilTest, ConstantsAndDefaultConstructor) {
  PrecisionUtil util;
  (void)util;
  EXPECT_EQ(+PrecisionUtil::MAX_ROBUST_DP_DIGITS, 14);
}

TEST(PrecisionUtilTest, NumberOfDecimalsHandlesIntegersDecimalsAndCap) {
  EXPECT_EQ(PrecisionUtil::numberOfDecimals(0.0), 0);
  EXPECT_EQ(PrecisionUtil::numberOfDecimals(1.0), 0);
  EXPECT_EQ(PrecisionUtil::numberOfDecimals(-42.0), 0);
  EXPECT_EQ(PrecisionUtil::numberOfDecimals(1000000.0), 0);

  EXPECT_EQ(PrecisionUtil::numberOfDecimals(0.5), 1);
  EXPECT_EQ(PrecisionUtil::numberOfDecimals(1.25), 2);
  EXPECT_EQ(PrecisionUtil::numberOfDecimals(-12.375), 3);
  EXPECT_EQ(PrecisionUtil::numberOfDecimals(0.0001), 4);

  // Differences <= 5e-5 are treated as zero decimal places; > 5e-5 scales up.
  EXPECT_EQ(PrecisionUtil::numberOfDecimals(0.00004), 0);
  EXPECT_EQ(PrecisionUtil::numberOfDecimals(0.00005), 0);
  EXPECT_EQ(PrecisionUtil::numberOfDecimals(-0.00001), 0);
  EXPECT_EQ(PrecisionUtil::numberOfDecimals(10.00001), 0);
  EXPECT_EQ(PrecisionUtil::numberOfDecimals(0.000051), 6);

  // High-precision decimal expansion hits the 17-digit loop cap for values
  // whose scaled fractional part remains > 5e-5 through 16 multiplications,
  // whereas 0.12345678901234567 and pi round to an integer earlier (16 and 15).
  EXPECT_EQ(PrecisionUtil::numberOfDecimals(0.012345678901234567), 17);
  EXPECT_EQ(PrecisionUtil::numberOfDecimals(-0.012345678901234567), 17);
  EXPECT_EQ(PrecisionUtil::numberOfDecimals(1.0 / 3.0), 17);
  EXPECT_EQ(PrecisionUtil::numberOfDecimals(0.12345678901234567), 16);
  EXPECT_EQ(PrecisionUtil::numberOfDecimals(std::numbers::pi), 15);
}

TEST(PrecisionUtilTest, InherentScaleScalarPreservesExactDecimalValues) {
  EXPECT_DOUBLE_EQ(PrecisionUtil::inherentScale(0.0), 1.0);
  EXPECT_DOUBLE_EQ(PrecisionUtil::inherentScale(7.0), 1.0);
  EXPECT_DOUBLE_EQ(PrecisionUtil::inherentScale(1.5), 10.0);
  EXPECT_DOUBLE_EQ(PrecisionUtil::inherentScale(-2.25), 100.0);
  EXPECT_DOUBLE_EQ(PrecisionUtil::inherentScale(0.001), 1000.0);
  EXPECT_DOUBLE_EQ(PrecisionUtil::inherentScale(0.012345678901234567), 1e17);

  for (double value : {0.0, 42.0, 1.5, -2.25, 0.125}) {
    PrecisionModel pm(PrecisionUtil::inherentScale(value));
    EXPECT_DOUBLE_EQ(pm.makePrecise(value), value);
  }
}

TEST(PrecisionUtilTest, InherentScaleGeometrySingleAndPair) {
  GeometryFactory::Ptr gf = GeometryFactory::create();
  WKTReader reader(*gf);

  std::unique_ptr<Geometry> empty_pt = reader.read("POINT EMPTY");
  std::unique_ptr<Geometry> empty_poly = reader.read("POLYGON EMPTY");
  EXPECT_DOUBLE_EQ(PrecisionUtil::inherentScale(empty_pt.get()), 0.0);
  EXPECT_DOUBLE_EQ(PrecisionUtil::inherentScale(empty_poly.get()), 0.0);

  std::unique_ptr<Geometry> origin = reader.read("POINT (0 0)");
  EXPECT_DOUBLE_EQ(PrecisionUtil::inherentScale(origin.get()), 1.0);

  std::unique_ptr<Geometry> y_more = reader.read("POINT (1 2.25)");
  EXPECT_DOUBLE_EQ(PrecisionUtil::inherentScale(y_more.get()), 100.0);

  std::unique_ptr<Geometry> x_more = reader.read("POINT (2.125 1)");
  EXPECT_DOUBLE_EQ(PrecisionUtil::inherentScale(x_more.get()), 1000.0);

  // Later vertices with fewer decimals must not reduce the accumulated maximum.
  std::unique_ptr<Geometry> line = reader.read("LINESTRING (1.125 2.5, 3 4)");
  EXPECT_DOUBLE_EQ(PrecisionUtil::inherentScale(line.get()), 1000.0);

  std::unique_ptr<Geometry> holed_poly = reader.read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(1.25 1.5, 2.5 1.5, 2.5 2.5, 1.25 2.5, 1.25 1.5))");
  EXPECT_DOUBLE_EQ(PrecisionUtil::inherentScale(holed_poly.get()), 100.0);

  // InherentScaleFilter inspects only X and Y ordinates, ignoring Z.
  std::unique_ptr<Geometry> pt_z = reader.read("POINT Z (1 2 3.125)");
  EXPECT_DOUBLE_EQ(PrecisionUtil::inherentScale(pt_z.get()), 1.0);

  // Two-geometry overloads, including nullptr second argument.
  EXPECT_DOUBLE_EQ(PrecisionUtil::inherentScale(y_more.get(), nullptr), 100.0);
  EXPECT_DOUBLE_EQ(PrecisionUtil::inherentScale(y_more.get(), x_more.get()),
                   1000.0);
  EXPECT_DOUBLE_EQ(PrecisionUtil::inherentScale(x_more.get(), y_more.get()),
                   1000.0);
}

TEST(PrecisionUtilTest, InherentScaleFilterTracksMaximumScale) {
  PrecisionUtil::InherentScaleFilter filter;
  EXPECT_DOUBLE_EQ(filter.getScale(), 0.0);

  Coordinate c1(1.5, 2.0);
  filter.filter_ro(&c1);
  EXPECT_DOUBLE_EQ(filter.getScale(), 10.0);

  Coordinate c2(3.0, 4.0);
  filter.filter_ro(&c2);
  EXPECT_DOUBLE_EQ(filter.getScale(), 10.0);

  Coordinate c3(3.0, 4.25);
  filter.filter_ro(&c3);
  EXPECT_DOUBLE_EQ(filter.getScale(), 100.0);
}

TEST(PrecisionUtilTest, SafeScaleScalarAndEnvelopeQuadrants) {
  // Values chosen away from exact powers of 10 so log10 truncation is exact.
  EXPECT_DOUBLE_EQ(PrecisionUtil::safeScale(0.05), 1e14);
  EXPECT_DOUBLE_EQ(PrecisionUtil::safeScale(0.005), 1e15);
  EXPECT_DOUBLE_EQ(PrecisionUtil::safeScale(5.0), 1e13);
  EXPECT_DOUBLE_EQ(PrecisionUtil::safeScale(250.0), 1e11);
  EXPECT_DOUBLE_EQ(PrecisionUtil::safeScale(15000.0), 1e9);
  EXPECT_DOUBLE_EQ(PrecisionUtil::safeScale(5e15), 0.01);

  GeometryFactory::Ptr gf = GeometryFactory::create();
  WKTReader reader(*gf);

  // Exercise maxBoundMagnitude across maxX, maxY, |minX|, and |minY|.
  std::unique_ptr<Geometry> max_x = reader.read("LINESTRING (1 1, 250 10)");
  std::unique_ptr<Geometry> max_y = reader.read("LINESTRING (1 1, 10 250)");
  std::unique_ptr<Geometry> min_x = reader.read("LINESTRING (-250 1, -1 10)");
  std::unique_ptr<Geometry> min_y = reader.read("LINESTRING (1 -250, 10 -1)");
  EXPECT_DOUBLE_EQ(PrecisionUtil::safeScale(max_x.get()), 1e11);
  EXPECT_DOUBLE_EQ(PrecisionUtil::safeScale(max_y.get()), 1e11);
  EXPECT_DOUBLE_EQ(PrecisionUtil::safeScale(min_x.get()), 1e11);
  EXPECT_DOUBLE_EQ(PrecisionUtil::safeScale(min_y.get()), 1e11);

  std::unique_ptr<Geometry> small = reader.read("POINT (5 5)");
  EXPECT_DOUBLE_EQ(PrecisionUtil::safeScale(small.get(), nullptr), 1e13);
  EXPECT_DOUBLE_EQ(PrecisionUtil::safeScale(small.get(), max_x.get()), 1e11);
  EXPECT_DOUBLE_EQ(PrecisionUtil::safeScale(max_x.get(), small.get()), 1e11);
}

TEST(PrecisionUtilTest, RobustScaleAndRobustPMSelectSmallerScale) {
  GeometryFactory::Ptr gf = GeometryFactory::create();
  WKTReader reader(*gf);

  // Inherent scale (100) < safe scale (1e13) -> inherent scale wins.
  std::unique_ptr<Geometry> low_prec =
      reader.read("LINESTRING (1.25 2.5, 3.0 4.0)");
  EXPECT_DOUBLE_EQ(PrecisionUtil::robustScale(low_prec.get()), 100.0);
  PrecisionModel pm_low = PrecisionUtil::robustPM(low_prec.get());
  EXPECT_EQ(pm_low.getType(), PrecisionModel::FIXED);
  EXPECT_DOUBLE_EQ(pm_low.getScale(), 100.0);

  // Inherent scale (1e16) > safe scale (1e11) -> safe scale wins.
  std::unique_ptr<Geometry> high_prec =
      reader.read("POINT (250.0 0.12345678901234567)");
  EXPECT_DOUBLE_EQ(PrecisionUtil::inherentScale(high_prec.get()), 1e16);
  EXPECT_DOUBLE_EQ(PrecisionUtil::safeScale(high_prec.get()), 1e11);
  EXPECT_DOUBLE_EQ(PrecisionUtil::robustScale(high_prec.get()), 1e11);
  PrecisionModel pm_high = PrecisionUtil::robustPM(high_prec.get());
  EXPECT_EQ(pm_high.getType(), PrecisionModel::FIXED);
  EXPECT_DOUBLE_EQ(pm_high.getScale(), 1e11);

  // Equal inherent and safe scale (both 1e9): inhSc <= sfScl branch holds.
  std::unique_ptr<Geometry> equal_scales =
      reader.read("LINESTRING (12345.0 0.123456789, 1.0 1.0)");
  EXPECT_DOUBLE_EQ(PrecisionUtil::inherentScale(equal_scales.get()), 1e9);
  EXPECT_DOUBLE_EQ(PrecisionUtil::safeScale(equal_scales.get()), 1e9);
  EXPECT_DOUBLE_EQ(PrecisionUtil::robustScale(equal_scales.get()), 1e9);

  // Two-geometry robustScale and robustPM, including nullptr second arg.
  std::unique_ptr<Geometry> int_geom = reader.read("LINESTRING (1 1, 2 2)");
  EXPECT_DOUBLE_EQ(PrecisionUtil::robustScale(low_prec.get(), nullptr), 100.0);
  PrecisionModel pm_single_pair =
      PrecisionUtil::robustPM(low_prec.get(), nullptr);
  EXPECT_EQ(pm_single_pair.getType(), PrecisionModel::FIXED);
  EXPECT_DOUBLE_EQ(pm_single_pair.getScale(), 100.0);

  EXPECT_DOUBLE_EQ(PrecisionUtil::robustScale(int_geom.get(), low_prec.get()),
                   100.0);
  PrecisionModel pm_pair =
      PrecisionUtil::robustPM(int_geom.get(), low_prec.get());
  EXPECT_EQ(pm_pair.getType(), PrecisionModel::FIXED);
  EXPECT_DOUBLE_EQ(pm_pair.getScale(), 100.0);

  // Large second geometry reduces safeScale below inherentScale of first.
  std::unique_ptr<Geometry> huge_geom =
      reader.read("POINT (5000000000000000 1)");
  EXPECT_DOUBLE_EQ(PrecisionUtil::robustScale(low_prec.get(), huge_geom.get()),
                   0.01);
  PrecisionModel pm_huge =
      PrecisionUtil::robustPM(low_prec.get(), huge_geom.get());
  EXPECT_EQ(pm_huge.getType(), PrecisionModel::FIXED);
  EXPECT_DOUBLE_EQ(pm_huge.getScale(), 0.01);
  EXPECT_DOUBLE_EQ(pm_huge.getGridSize(), 100.0);
}

}  // namespace
}  // namespace geos_google
