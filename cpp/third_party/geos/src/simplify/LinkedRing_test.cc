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

#include "third_party/geos/include/geos/simplify/LinkedRing.h"

#include <cstddef>
#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::simplify::LinkedRing;

constexpr std::size_t kNoCoordIndex = geos::NO_COORD_INDEX;

TEST(LinkedRingTest, InitialStateLinksAndWrapAroundAccessors) {
  const CoordinateSequence coords{
      Coordinate{0.0, 0.0},  Coordinate{10.0, 0.0}, Coordinate{10.0, 10.0},
      Coordinate{0.0, 10.0}, Coordinate{0.0, 0.0},
  };
  LinkedRing ring(coords);

  EXPECT_EQ(ring.size(), 4u);

  EXPECT_EQ(ring.next(0), 1u);
  EXPECT_EQ(ring.next(1), 2u);
  EXPECT_EQ(ring.next(2), 3u);
  EXPECT_EQ(ring.next(3), 0u);

  EXPECT_EQ(ring.prev(0), 3u);
  EXPECT_EQ(ring.prev(1), 0u);
  EXPECT_EQ(ring.prev(2), 1u);
  EXPECT_EQ(ring.prev(3), 2u);

  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_TRUE(ring.hasCoordinate(i));
    EXPECT_EQ(ring.getCoordinate(i), coords.getAt(i));
  }
  EXPECT_FALSE(ring.hasCoordinate(4));
  EXPECT_FALSE(ring.hasCoordinate(100));
  EXPECT_FALSE(ring.hasCoordinate(kNoCoordIndex));

  EXPECT_EQ(ring.prevCoordinate(0), coords.getAt(3));
  EXPECT_EQ(ring.nextCoordinate(3), coords.getAt(0));
  EXPECT_EQ(ring.prevCoordinate(2), coords.getAt(1));
  EXPECT_EQ(ring.nextCoordinate(1), coords.getAt(2));

  auto extracted = ring.getCoordinates();
  EXPECT_TRUE(CoordinateSequence::equals(extracted.get(), &coords));
}

TEST(LinkedRingTest, RemoveInteriorVerticesUpdatesLinksAndOutput) {
  const CoordinateSequence coords{
      Coordinate{0.0, 0.0},  Coordinate{5.0, 0.0},  Coordinate{10.0, 5.0},
      Coordinate{8.0, 10.0}, Coordinate{2.0, 10.0}, Coordinate{0.0, 0.0},
  };
  LinkedRing ring(coords);

  EXPECT_EQ(ring.size(), 5u);

  ring.remove(2);
  EXPECT_EQ(ring.size(), 4u);
  EXPECT_FALSE(ring.hasCoordinate(2));
  EXPECT_EQ(ring.prev(2), kNoCoordIndex);
  EXPECT_EQ(ring.next(2), kNoCoordIndex);
  EXPECT_EQ(ring.next(1), 3u);
  EXPECT_EQ(ring.prev(3), 1u);
  EXPECT_EQ(ring.nextCoordinate(1), coords.getAt(3));
  EXPECT_EQ(ring.prevCoordinate(3), coords.getAt(1));

  ring.remove(3);
  EXPECT_EQ(ring.size(), 3u);
  EXPECT_FALSE(ring.hasCoordinate(3));
  EXPECT_EQ(ring.next(1), 4u);
  EXPECT_EQ(ring.prev(4), 1u);

  const CoordinateSequence expected{
      Coordinate{0.0, 0.0},
      Coordinate{5.0, 0.0},
      Coordinate{2.0, 10.0},
      Coordinate{0.0, 0.0},
  };
  auto extracted = ring.getCoordinates();
  EXPECT_TRUE(CoordinateSequence::equals(extracted.get(), &expected));
}

TEST(LinkedRingTest, RemoveStartAndEndVerticesUpdatesWrapAround) {
  const CoordinateSequence coords{
      Coordinate{0.0, 0.0},  Coordinate{10.0, 0.0}, Coordinate{10.0, 10.0},
      Coordinate{0.0, 10.0}, Coordinate{0.0, 0.0},
  };
  LinkedRing ring(coords);

  ring.remove(0);
  EXPECT_EQ(ring.size(), 3u);
  EXPECT_FALSE(ring.hasCoordinate(0));
  EXPECT_EQ(ring.next(3), 1u);
  EXPECT_EQ(ring.prev(1), 3u);
  EXPECT_EQ(ring.nextCoordinate(3), coords.getAt(1));
  EXPECT_EQ(ring.prevCoordinate(1), coords.getAt(3));

  const CoordinateSequence expected_after_zero{
      Coordinate{10.0, 0.0},
      Coordinate{10.0, 10.0},
      Coordinate{0.0, 10.0},
      Coordinate{10.0, 0.0},
  };
  auto after_zero = ring.getCoordinates();
  EXPECT_TRUE(
      CoordinateSequence::equals(after_zero.get(), &expected_after_zero));

  ring.remove(3);
  EXPECT_EQ(ring.size(), 2u);
  EXPECT_FALSE(ring.hasCoordinate(3));
  EXPECT_EQ(ring.next(2), 1u);
  EXPECT_EQ(ring.prev(1), 2u);
  EXPECT_EQ(ring.nextCoordinate(2), coords.getAt(1));
  EXPECT_EQ(ring.prevCoordinate(1), coords.getAt(2));

  const CoordinateSequence expected_two_pts{
      Coordinate{10.0, 0.0},
      Coordinate{10.0, 10.0},
      Coordinate{10.0, 0.0},
  };
  auto after_three = ring.getCoordinates();
  EXPECT_TRUE(CoordinateSequence::equals(after_three.get(), &expected_two_pts));
}

TEST(LinkedRingTest, RemoveAllVerticesInTriangleRing) {
  const CoordinateSequence coords{
      Coordinate{0.0, 0.0},
      Coordinate{4.0, 0.0},
      Coordinate{2.0, 3.0},
      Coordinate{0.0, 0.0},
  };
  LinkedRing ring(coords);
  EXPECT_EQ(ring.size(), 3u);

  ring.remove(1);
  EXPECT_EQ(ring.size(), 2u);
  EXPECT_EQ(ring.next(0), 2u);
  EXPECT_EQ(ring.prev(2), 0u);

  ring.remove(2);
  EXPECT_EQ(ring.size(), 1u);
  EXPECT_EQ(ring.next(0), 0u);
  EXPECT_EQ(ring.prev(0), 0u);
  EXPECT_EQ(ring.nextCoordinate(0), coords.getAt(0));
  EXPECT_EQ(ring.prevCoordinate(0), coords.getAt(0));

  auto one_pt = ring.getCoordinates();
  ASSERT_EQ(one_pt->size(), 1u);
  EXPECT_EQ(one_pt->getAt(0), coords.getAt(0));

  ring.remove(0);
  EXPECT_EQ(ring.size(), 0u);
  EXPECT_FALSE(ring.hasCoordinate(0));
  auto empty_coords = ring.getCoordinates();
  EXPECT_TRUE(empty_coords->isEmpty());
}

TEST(LinkedRingTest, GetCoordinatesPreservesZAndDeduplicatesConsecutive) {
  const CoordinateSequence coords{
      Coordinate{0.0, 0.0, 1.0}, Coordinate{5.0, 0.0, 2.0},
      Coordinate{5.0, 0.0, 3.0}, Coordinate{5.0, 5.0, 4.0},
      Coordinate{0.0, 0.0, 1.0},
  };
  LinkedRing ring(coords);

  auto extracted = ring.getCoordinates();
  ASSERT_EQ(extracted->size(), 4u);
  EXPECT_TRUE(extracted->getAt(0).equals3D(Coordinate{0.0, 0.0, 1.0}));
  EXPECT_TRUE(extracted->getAt(1).equals3D(Coordinate{5.0, 0.0, 2.0}));
  EXPECT_TRUE(extracted->getAt(2).equals3D(Coordinate{5.0, 5.0, 4.0}));
  EXPECT_TRUE(extracted->getAt(3).equals3D(Coordinate{0.0, 0.0, 1.0}));
}

}  // namespace
}  // namespace geos_google
