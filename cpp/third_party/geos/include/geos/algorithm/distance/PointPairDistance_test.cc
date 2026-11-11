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

// Unit tests for
// third_party/geos/include/geos/algorithm/distance/PointPairDistance.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/algorithm/distance/PointPairDistance.h

#include "third_party/geos/include/geos/algorithm/distance/PointPairDistance.h"

#include <array>
#include <cmath>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"

namespace geos_google {
namespace {

using geos::DoubleNotANumber;
using geos::algorithm::distance::PointPairDistance;
using geos::geom::Coordinate;
using geos::geom::CoordinateXY;

TEST(PointPairDistanceTest, DefaultConstructionIsNullWithNaNDistance) {
  PointPairDistance ppd;
  const PointPairDistance& const_ppd = ppd;

  EXPECT_TRUE(ppd.getIsNull());
  EXPECT_TRUE(std::isnan(const_ppd.getDistance()));

  const std::array<CoordinateXY, 2>& coords = const_ppd.getCoordinates();
  EXPECT_EQ(coords[0], CoordinateXY(0.0, 0.0));
  EXPECT_EQ(coords[1], CoordinateXY(0.0, 0.0));
  EXPECT_EQ(const_ppd.getCoordinate(0), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(const_ppd.getCoordinate(1), CoordinateXY(0.0, 0.0));
}

TEST(PointPairDistanceTest, InitializeWithPointsAndReset) {
  PointPairDistance ppd;
  const CoordinateXY p0(1.0, 2.0);
  const CoordinateXY p1(4.0, 6.0);

  ppd.initialize(p0, p1);
  EXPECT_FALSE(ppd.getIsNull());
  EXPECT_DOUBLE_EQ(ppd.getDistance(), 5.0);
  EXPECT_EQ(ppd.getCoordinate(0), p0);
  EXPECT_EQ(ppd.getCoordinate(1), p1);
  EXPECT_EQ(ppd.getCoordinates()[0], p0);
  EXPECT_EQ(ppd.getCoordinates()[1], p1);

  // Coincident points produce non-null state with zero distance.
  ppd.initialize(CoordinateXY(7.0, -3.0), CoordinateXY(7.0, -3.0));
  EXPECT_FALSE(ppd.getIsNull());
  EXPECT_DOUBLE_EQ(ppd.getDistance(), 0.0);

  // Resetting marks null so a subsequent larger distance still initializes.
  ppd.initialize();
  EXPECT_TRUE(ppd.getIsNull());
  ppd.setMinimum(CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0));
  EXPECT_FALSE(ppd.getIsNull());
  EXPECT_DOUBLE_EQ(ppd.getDistance(), 10.0);

  // Resetting again allows a smaller distance via setMaximum to initialize.
  ppd.initialize();
  EXPECT_TRUE(ppd.getIsNull());
  ppd.setMaximum(CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 0.0));
  EXPECT_FALSE(ppd.getIsNull());
  EXPECT_DOUBLE_EQ(ppd.getDistance(), 2.0);
}

TEST(PointPairDistanceTest, InitializeWithPrecomputedDistanceSquared) {
  PointPairDistance ppd;
  const CoordinateXY p0(-1.0, -2.0);
  const CoordinateXY p1(2.0, 2.0);

  ppd.initialize(p0, p1, 25.0);
  EXPECT_FALSE(ppd.getIsNull());
  EXPECT_DOUBLE_EQ(ppd.getDistance(), 5.0);
  EXPECT_EQ(ppd.getCoordinate(0), p0);
  EXPECT_EQ(ppd.getCoordinate(1), p1);

  // Stores caller-supplied distSquared directly without recomputing.
  ppd.initialize(CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0), 100.0);
  EXPECT_DOUBLE_EQ(ppd.getDistance(), 10.0);

  ppd.setMaximum(CoordinateXY(0.0, 0.0), CoordinateXY(5.0, 0.0));
  EXPECT_DOUBLE_EQ(ppd.getDistance(), 10.0);

  ppd.setMaximum(CoordinateXY(0.0, 0.0), CoordinateXY(11.0, 0.0));
  EXPECT_DOUBLE_EQ(ppd.getDistance(), 11.0);

  ppd.initialize(CoordinateXY(3.0, 4.0), CoordinateXY(3.0, 4.0), 0.0);
  EXPECT_FALSE(ppd.getIsNull());
  EXPECT_DOUBLE_EQ(ppd.getDistance(), 0.0);
}

TEST(PointPairDistanceTest, SetMinimumCoordinateOverload) {
  PointPairDistance ppd;

  // Initializes when null.
  ppd.setMinimum(CoordinateXY(0.0, 0.0), CoordinateXY(6.0, 8.0));
  EXPECT_FALSE(ppd.getIsNull());
  EXPECT_DOUBLE_EQ(ppd.getDistance(), 10.0);
  EXPECT_EQ(ppd.getCoordinate(0), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(ppd.getCoordinate(1), CoordinateXY(6.0, 8.0));

  // Larger distance leaves existing pair intact.
  ppd.setMinimum(CoordinateXY(0.0, 0.0), CoordinateXY(20.0, 0.0));
  EXPECT_DOUBLE_EQ(ppd.getDistance(), 10.0);
  EXPECT_EQ(ppd.getCoordinate(0), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(ppd.getCoordinate(1), CoordinateXY(6.0, 8.0));

  // Equal distance leaves original pair intact (strict '<').
  ppd.setMinimum(CoordinateXY(1.0, 1.0), CoordinateXY(11.0, 1.0));
  EXPECT_DOUBLE_EQ(ppd.getDistance(), 10.0);
  EXPECT_EQ(ppd.getCoordinate(0), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(ppd.getCoordinate(1), CoordinateXY(6.0, 8.0));

  // Strictly smaller distance replaces pair.
  ppd.setMinimum(CoordinateXY(1.0, 2.0), CoordinateXY(4.0, 6.0));
  EXPECT_DOUBLE_EQ(ppd.getDistance(), 5.0);
  EXPECT_EQ(ppd.getCoordinate(0), CoordinateXY(1.0, 2.0));
  EXPECT_EQ(ppd.getCoordinate(1), CoordinateXY(4.0, 6.0));

  // Zero distance replaces positive distance.
  ppd.setMinimum(CoordinateXY(9.0, 9.0), CoordinateXY(9.0, 9.0));
  EXPECT_DOUBLE_EQ(ppd.getDistance(), 0.0);
  EXPECT_EQ(ppd.getCoordinate(0), CoordinateXY(9.0, 9.0));
  EXPECT_EQ(ppd.getCoordinate(1), CoordinateXY(9.0, 9.0));
}

TEST(PointPairDistanceTest, SetMinimumPointPairOverload) {
  PointPairDistance target;
  PointPairDistance medium;
  medium.initialize(CoordinateXY(0.0, 0.0), CoordinateXY(0.0, 8.0));
  PointPairDistance large;
  large.initialize(CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0));
  PointPairDistance equal;
  equal.initialize(CoordinateXY(5.0, 5.0), CoordinateXY(13.0, 5.0));
  PointPairDistance small;
  small.initialize(CoordinateXY(2.0, 3.0), CoordinateXY(2.0, 6.0));

  target.setMinimum(medium);
  EXPECT_FALSE(target.getIsNull());
  EXPECT_DOUBLE_EQ(target.getDistance(), 8.0);
  EXPECT_EQ(target.getCoordinate(1), CoordinateXY(0.0, 8.0));

  target.setMinimum(large);
  EXPECT_DOUBLE_EQ(target.getDistance(), 8.0);
  EXPECT_EQ(target.getCoordinate(0), CoordinateXY(0.0, 0.0));

  target.setMinimum(equal);
  EXPECT_DOUBLE_EQ(target.getDistance(), 8.0);
  EXPECT_EQ(target.getCoordinate(0), CoordinateXY(0.0, 0.0));

  target.setMinimum(small);
  EXPECT_DOUBLE_EQ(target.getDistance(), 3.0);
  EXPECT_EQ(target.getCoordinate(0), CoordinateXY(2.0, 3.0));
  EXPECT_EQ(target.getCoordinate(1), CoordinateXY(2.0, 6.0));
}

TEST(PointPairDistanceTest, SetMaximumCoordinateOverload) {
  PointPairDistance ppd;

  // Initializes when null.
  ppd.setMaximum(CoordinateXY(1.0, 1.0), CoordinateXY(1.0, 4.0));
  EXPECT_FALSE(ppd.getIsNull());
  EXPECT_DOUBLE_EQ(ppd.getDistance(), 3.0);
  EXPECT_EQ(ppd.getCoordinate(0), CoordinateXY(1.0, 1.0));
  EXPECT_EQ(ppd.getCoordinate(1), CoordinateXY(1.0, 4.0));

  // Smaller distance leaves existing pair intact.
  ppd.setMaximum(CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0));
  EXPECT_DOUBLE_EQ(ppd.getDistance(), 3.0);
  EXPECT_EQ(ppd.getCoordinate(0), CoordinateXY(1.0, 1.0));
  EXPECT_EQ(ppd.getCoordinate(1), CoordinateXY(1.0, 4.0));

  // Equal distance leaves original pair intact (strict '>').
  ppd.setMaximum(CoordinateXY(0.0, 0.0), CoordinateXY(3.0, 0.0));
  EXPECT_DOUBLE_EQ(ppd.getDistance(), 3.0);
  EXPECT_EQ(ppd.getCoordinate(0), CoordinateXY(1.0, 1.0));
  EXPECT_EQ(ppd.getCoordinate(1), CoordinateXY(1.0, 4.0));

  // Strictly larger distance replaces pair.
  ppd.setMaximum(CoordinateXY(-3.0, 0.0), CoordinateXY(0.0, 4.0));
  EXPECT_DOUBLE_EQ(ppd.getDistance(), 5.0);
  EXPECT_EQ(ppd.getCoordinate(0), CoordinateXY(-3.0, 0.0));
  EXPECT_EQ(ppd.getCoordinate(1), CoordinateXY(0.0, 4.0));
}

TEST(PointPairDistanceTest, SetMaximumPointPairOverload) {
  PointPairDistance target;
  PointPairDistance small;
  small.initialize(CoordinateXY(0.0, 0.0), CoordinateXY(3.0, 0.0));
  PointPairDistance tiny;
  tiny.initialize(CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0));
  PointPairDistance equal;
  equal.initialize(CoordinateXY(2.0, 2.0), CoordinateXY(2.0, 5.0));
  PointPairDistance large;
  large.initialize(CoordinateXY(1.0, 1.0), CoordinateXY(7.0, 9.0));

  target.setMaximum(small);
  EXPECT_FALSE(target.getIsNull());
  EXPECT_DOUBLE_EQ(target.getDistance(), 3.0);
  EXPECT_EQ(target.getCoordinate(1), CoordinateXY(3.0, 0.0));

  target.setMaximum(tiny);
  EXPECT_DOUBLE_EQ(target.getDistance(), 3.0);
  EXPECT_EQ(target.getCoordinate(0), CoordinateXY(0.0, 0.0));

  target.setMaximum(equal);
  EXPECT_DOUBLE_EQ(target.getDistance(), 3.0);
  EXPECT_EQ(target.getCoordinate(0), CoordinateXY(0.0, 0.0));

  target.setMaximum(large);
  EXPECT_DOUBLE_EQ(target.getDistance(), 10.0);
  EXPECT_EQ(target.getCoordinate(0), CoordinateXY(1.0, 1.0));
  EXPECT_EQ(target.getCoordinate(1), CoordinateXY(7.0, 9.0));
}

TEST(PointPairDistanceTest, HandlesCoordinateSubclassesExtremeScalesAndNaN) {
  // Coordinate (XYZ) slices to 2D CoordinateXY; Z does not affect distance.
  PointPairDistance ppd;
  ppd.initialize(Coordinate(0.0, 0.0, 1000.0), Coordinate(3.0, 4.0, -1000.0));
  EXPECT_DOUBLE_EQ(ppd.getDistance(), 5.0);

  // NaN distance comparisons do not overwrite an existing valid pair.
  ppd.setMinimum(CoordinateXY(DoubleNotANumber, 0.0), CoordinateXY(0.0, 0.0));
  EXPECT_DOUBLE_EQ(ppd.getDistance(), 5.0);
  ppd.setMaximum(CoordinateXY(DoubleNotANumber, 0.0), CoordinateXY(0.0, 0.0));
  EXPECT_DOUBLE_EQ(ppd.getDistance(), 5.0);

  // Small and large finite scales (exact binary64 powers avoid rounding drift).
  PointPairDistance small_scale;
  small_scale.initialize(CoordinateXY(0.0, 0.0),
                         CoordinateXY(3.0 * 0x1p-200, 4.0 * 0x1p-200));
  EXPECT_DOUBLE_EQ(small_scale.getDistance(), 5.0 * 0x1p-200);

  PointPairDistance large_scale;
  large_scale.initialize(CoordinateXY(-0x1p200, 0.0),
                         CoordinateXY(0x1p200, 0.0));
  EXPECT_DOUBLE_EQ(large_scale.getDistance(), 0x1p201);
}

}  // namespace
}  // namespace geos_google
