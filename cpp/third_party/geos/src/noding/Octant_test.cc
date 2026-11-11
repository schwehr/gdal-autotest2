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

#include "third_party/geos/include/geos/noding/Octant.h"

#include <limits>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateXY;
using ::geos::geom::CoordinateXYZM;
using ::geos::noding::Octant;
using ::geos::util::IllegalArgumentException;
using ::testing::HasSubstr;

TEST(OctantTest, OctantFromDisplacementsAllEightOctants) {
  EXPECT_EQ(Octant::octant(2.0, 1.0), 0);
  EXPECT_EQ(Octant::octant(1.0, 2.0), 1);
  EXPECT_EQ(Octant::octant(-1.0, 2.0), 2);
  EXPECT_EQ(Octant::octant(-2.0, 1.0), 3);
  EXPECT_EQ(Octant::octant(-2.0, -1.0), 4);
  EXPECT_EQ(Octant::octant(-1.0, -2.0), 5);
  EXPECT_EQ(Octant::octant(1.0, -2.0), 6);
  EXPECT_EQ(Octant::octant(2.0, -1.0), 7);
}

TEST(OctantTest, OctantAxisDiagonalAndSignedZeroBoundaries) {
  // Coordinate axes.
  EXPECT_EQ(Octant::octant(1.0, 0.0), 0);
  EXPECT_EQ(Octant::octant(0.0, 1.0), 1);
  EXPECT_EQ(Octant::octant(-1.0, 0.0), 3);
  EXPECT_EQ(Octant::octant(0.0, -1.0), 6);

  // 45-degree diagonals (|dx| == |dy| chooses the horizontal-adjacent octant).
  EXPECT_EQ(Octant::octant(1.0, 1.0), 0);
  EXPECT_EQ(Octant::octant(-1.0, 1.0), 3);
  EXPECT_EQ(Octant::octant(-1.0, -1.0), 4);
  EXPECT_EQ(Octant::octant(1.0, -1.0), 7);

  // Signed negative zero checks.
  EXPECT_EQ(Octant::octant(1.0, -0.0), 0);
  EXPECT_EQ(Octant::octant(-0.0, 1.0), 1);
  EXPECT_EQ(Octant::octant(-1.0, -0.0), 3);
  EXPECT_EQ(Octant::octant(-0.0, -1.0), 6);
}

TEST(OctantTest, OctantInfiniteDisplacements) {
  const double inf = std::numeric_limits<double>::infinity();
  EXPECT_EQ(Octant::octant(inf, 1.0), 0);
  EXPECT_EQ(Octant::octant(1.0, inf), 1);
  EXPECT_EQ(Octant::octant(-1.0, inf), 2);
  EXPECT_EQ(Octant::octant(-inf, 1.0), 3);
  EXPECT_EQ(Octant::octant(-inf, -1.0), 4);
  EXPECT_EQ(Octant::octant(-1.0, -inf), 5);
  EXPECT_EQ(Octant::octant(1.0, -inf), 6);
  EXPECT_EQ(Octant::octant(inf, -1.0), 7);

  EXPECT_EQ(Octant::octant(inf, inf), 0);
  EXPECT_EQ(Octant::octant(-inf, inf), 3);
  EXPECT_EQ(Octant::octant(-inf, -inf), 4);
  EXPECT_EQ(Octant::octant(inf, -inf), 7);
}

TEST(OctantTest, OctantZeroDisplacementThrows) {
  try {
    Octant::octant(0.0, 0.0);
    FAIL() << "Expected IllegalArgumentException for (0.0, 0.0)";
  } catch (const IllegalArgumentException& e) {
    EXPECT_THAT(e.what(), HasSubstr("Cannot compute the octant for point"));
  }

  EXPECT_THROW(Octant::octant(-0.0, -0.0), IllegalArgumentException);
  EXPECT_THROW(Octant::octant(0.0, -0.0), IllegalArgumentException);
  EXPECT_THROW(Octant::octant(-0.0, 0.0), IllegalArgumentException);
}

TEST(OctantTest, OctantFromCoordinateReferences) {
  const CoordinateXY origin(10.0, -20.0);

  EXPECT_EQ(Octant::octant(origin, CoordinateXY(12.0, -19.0)), 0);
  EXPECT_EQ(Octant::octant(origin, CoordinateXY(11.0, -18.0)), 1);
  EXPECT_EQ(Octant::octant(origin, CoordinateXY(9.0, -18.0)), 2);
  EXPECT_EQ(Octant::octant(origin, CoordinateXY(8.0, -19.0)), 3);
  EXPECT_EQ(Octant::octant(origin, CoordinateXY(8.0, -21.0)), 4);
  EXPECT_EQ(Octant::octant(origin, CoordinateXY(9.0, -22.0)), 5);
  EXPECT_EQ(Octant::octant(origin, CoordinateXY(11.0, -22.0)), 6);
  EXPECT_EQ(Octant::octant(origin, CoordinateXY(12.0, -21.0)), 7);

  // Higher-dimensional Coordinates decay cleanly to CoordinateXY.
  EXPECT_EQ(
      Octant::octant(Coordinate(1.0, 2.0, 100.0), Coordinate(3.0, 3.0, -50.0)),
      0);
  EXPECT_EQ(Octant::octant(CoordinateXYZM(1.0, 2.0, 3.0, 4.0),
                           CoordinateXYZM(0.0, 0.0, 99.0, -99.0)),
            5);

  // Identical coordinates throw IllegalArgumentException.
  try {
    Octant::octant(origin, CoordinateXY(10.0, -20.0));
    FAIL() << "Expected IllegalArgumentException for identical coordinates";
  } catch (const IllegalArgumentException& e) {
    EXPECT_THAT(e.what(), HasSubstr("Cannot compute the octant for point"));
  }
}

TEST(OctantTest, OctantFromCoordinatePointers) {
  const CoordinateXY origin(-5.0, 5.0);
  const CoordinateXY targets[8] = {
      CoordinateXY(-3.0, 6.0), CoordinateXY(-4.0, 7.0), CoordinateXY(-6.0, 7.0),
      CoordinateXY(-7.0, 6.0), CoordinateXY(-7.0, 4.0), CoordinateXY(-6.0, 3.0),
      CoordinateXY(-4.0, 3.0), CoordinateXY(-3.0, 4.0),
  };

  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(Octant::octant(&origin, &targets[i]), i);
  }

  const CoordinateXY same(-5.0, 5.0);
  EXPECT_THROW(Octant::octant(&origin, &same), IllegalArgumentException);
}

}  // namespace
}  // namespace geos_google
