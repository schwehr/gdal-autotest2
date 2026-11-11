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

#include "third_party/geos/include/geos/simplify/LinkedLine.h"

#include <cstddef>
#include <memory>
#include <sstream>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::simplify::LinkedLine;

constexpr std::size_t kNoCoordIndex = geos::NO_COORD_INDEX;

TEST(LinkedLineTest, OpenLineInitialLinksAndAccessors) {
  const CoordinateSequence coords{
      Coordinate{0.0, 0.0},
      Coordinate{1.0, 2.0},
      Coordinate{3.0, 1.0},
      Coordinate{4.0, 5.0},
  };
  LinkedLine line(coords);

  EXPECT_FALSE(line.isRing());
  EXPECT_EQ(line.size(), 4u);

  EXPECT_FALSE(line.isCorner(0));
  EXPECT_TRUE(line.isCorner(1));
  EXPECT_TRUE(line.isCorner(2));
  EXPECT_FALSE(line.isCorner(3));

  EXPECT_EQ(line.prev(0), kNoCoordIndex);
  EXPECT_EQ(line.next(0), 1u);
  EXPECT_EQ(line.prev(1), 0u);
  EXPECT_EQ(line.next(1), 2u);
  EXPECT_EQ(line.prev(2), 1u);
  EXPECT_EQ(line.next(2), 3u);
  EXPECT_EQ(line.prev(3), 2u);
  EXPECT_EQ(line.next(3), kNoCoordIndex);

  for (std::size_t i = 0; i < coords.size(); ++i) {
    EXPECT_EQ(line.getCoordinate(i), coords.getAt(i));
    EXPECT_TRUE(line.hasCoordinate(i));
  }
  EXPECT_FALSE(line.hasCoordinate(4));
  EXPECT_FALSE(line.hasCoordinate(kNoCoordIndex));

  EXPECT_EQ(line.nextCoordinate(0), coords.getAt(1));
  EXPECT_EQ(line.prevCoordinate(1), coords.getAt(0));
  EXPECT_EQ(line.nextCoordinate(1), coords.getAt(2));
  EXPECT_EQ(line.prevCoordinate(3), coords.getAt(2));

  auto extracted = line.getCoordinates();
  ASSERT_EQ(extracted->size(), 4u);
  EXPECT_TRUE(CoordinateSequence::equals(extracted.get(), &coords));
}

TEST(LinkedLineTest, OpenLineRemoveInteriorVerticesAndStreamOutput) {
  const CoordinateSequence coords{
      Coordinate{0.0, 0.0}, Coordinate{1.0, 1.0}, Coordinate{2.0, 0.0},
      Coordinate{3.0, 1.0}, Coordinate{4.0, 0.0},
  };
  LinkedLine line(coords);

  line.remove(1);
  EXPECT_EQ(line.size(), 4u);
  EXPECT_FALSE(line.hasCoordinate(1));
  EXPECT_TRUE(line.hasCoordinate(0));
  EXPECT_TRUE(line.hasCoordinate(2));
  EXPECT_EQ(line.next(0), 2u);
  EXPECT_EQ(line.prev(2), 0u);
  EXPECT_EQ(line.prev(1), kNoCoordIndex);
  EXPECT_EQ(line.next(1), kNoCoordIndex);
  EXPECT_EQ(line.nextCoordinate(0), coords.getAt(2));
  EXPECT_EQ(line.prevCoordinate(2), coords.getAt(0));

  line.remove(2);
  EXPECT_EQ(line.size(), 3u);
  EXPECT_FALSE(line.hasCoordinate(2));
  EXPECT_EQ(line.next(0), 3u);
  EXPECT_EQ(line.prev(3), 0u);

  auto extracted = line.getCoordinates();
  const CoordinateSequence expected{
      Coordinate{0.0, 0.0},
      Coordinate{3.0, 1.0},
      Coordinate{4.0, 0.0},
  };
  EXPECT_TRUE(CoordinateSequence::equals(extracted.get(), &expected));

  std::ostringstream oss;
  oss << line;
  EXPECT_EQ(oss.str(), "LINESTRING (0 0, 3 1, 4 0)");
}

TEST(LinkedLineTest, OpenLineEndpointsAndShortSequences) {
  // 1-point sequence exercises size==1, both null-link branches in remove, and
  // the size==0 guard in remove.
  const CoordinateSequence single{Coordinate{7.0, 8.0}};
  LinkedLine single_line(single);
  EXPECT_FALSE(single_line.isRing());
  EXPECT_EQ(single_line.size(), 1u);
  EXPECT_FALSE(single_line.isCorner(0));
  EXPECT_EQ(single_line.prev(0), kNoCoordIndex);
  EXPECT_EQ(single_line.next(0), kNoCoordIndex);
  EXPECT_TRUE(single_line.hasCoordinate(0));

  single_line.remove(0);
  EXPECT_EQ(single_line.size(), 0u);
  // Second removal when size is already 0 keeps size at 0.
  single_line.remove(0);
  EXPECT_EQ(single_line.size(), 0u);
  // Open line endpoints remain reported as present by hasCoordinate().
  EXPECT_TRUE(single_line.hasCoordinate(0));

  // 2-point sequence: remove start and end endpoints.
  const CoordinateSequence pair{Coordinate{0.0, 0.0}, Coordinate{5.0, 5.0}};
  LinkedLine pair_line(pair);
  EXPECT_FALSE(pair_line.isRing());
  EXPECT_FALSE(pair_line.isCorner(0));
  EXPECT_FALSE(pair_line.isCorner(1));
  pair_line.remove(0);
  EXPECT_EQ(pair_line.size(), 1u);
  EXPECT_EQ(pair_line.prev(1), kNoCoordIndex);
  pair_line.remove(1);
  EXPECT_EQ(pair_line.size(), 0u);
  EXPECT_TRUE(pair_line.hasCoordinate(0));
  EXPECT_TRUE(pair_line.hasCoordinate(1));

  // 3-point sequence with equal endpoints is still not a ring (size < 4).
  const CoordinateSequence three_closed{
      Coordinate{0.0, 0.0},
      Coordinate{1.0, 1.0},
      Coordinate{0.0, 0.0},
  };
  LinkedLine three_line(three_closed);
  EXPECT_FALSE(three_line.isRing());
  EXPECT_FALSE(three_line.isCorner(0));
  EXPECT_TRUE(three_line.isCorner(1));
  EXPECT_FALSE(three_line.isCorner(2));
}

TEST(LinkedLineTest, RingInitialLinksWrapAroundAndRemovals) {
  const CoordinateSequence ring_coords{
      Coordinate{0.0, 0.0},  Coordinate{10.0, 0.0}, Coordinate{10.0, 10.0},
      Coordinate{0.0, 10.0}, Coordinate{0.0, 0.0},
  };
  LinkedLine ring(ring_coords);

  EXPECT_TRUE(ring.isRing());
  EXPECT_EQ(ring.size(), 4u);

  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_TRUE(ring.isCorner(i));
    EXPECT_TRUE(ring.hasCoordinate(i));
  }
  EXPECT_FALSE(ring.hasCoordinate(4));
  EXPECT_FALSE(ring.hasCoordinate(kNoCoordIndex));

  EXPECT_EQ(ring.prev(0), 3u);
  EXPECT_EQ(ring.next(3), 0u);
  EXPECT_EQ(ring.prevCoordinate(0), ring_coords.getAt(3));
  EXPECT_EQ(ring.nextCoordinate(3), ring_coords.getAt(0));

  auto initial_extracted = ring.getCoordinates();
  EXPECT_TRUE(
      CoordinateSequence::equals(initial_extracted.get(), &ring_coords));

  // Remove start vertex 0 and verify wrap-around links and re-closed output.
  ring.remove(0);
  EXPECT_EQ(ring.size(), 3u);
  EXPECT_FALSE(ring.hasCoordinate(0));
  EXPECT_EQ(ring.next(3), 1u);
  EXPECT_EQ(ring.prev(1), 3u);
  EXPECT_EQ(ring.nextCoordinate(3), ring_coords.getAt(1));
  EXPECT_EQ(ring.prevCoordinate(1), ring_coords.getAt(3));

  const CoordinateSequence expected_after_zero{
      Coordinate{10.0, 0.0},
      Coordinate{10.0, 10.0},
      Coordinate{0.0, 10.0},
      Coordinate{10.0, 0.0},
  };
  auto after_zero = ring.getCoordinates();
  EXPECT_TRUE(
      CoordinateSequence::equals(after_zero.get(), &expected_after_zero));

  std::ostringstream oss;
  oss << ring;
  EXPECT_EQ(oss.str(), "LINESTRING (10 0, 10 10, 0 10, 10 0)");

  // Remove remaining vertices down to empty.
  ring.remove(3);
  ring.remove(1);
  EXPECT_EQ(ring.size(), 1u);
  auto single_rem = ring.getCoordinates();
  EXPECT_EQ(single_rem->size(), 1u);
  EXPECT_EQ(single_rem->getAt(0), ring_coords.getAt(2));

  ring.remove(2);
  EXPECT_EQ(ring.size(), 0u);
  EXPECT_FALSE(ring.hasCoordinate(2));
  auto empty_rem = ring.getCoordinates();
  EXPECT_TRUE(empty_rem->isEmpty());

  std::ostringstream empty_oss;
  empty_oss << ring;
  EXPECT_EQ(empty_oss.str(), "LINESTRING EMPTY");
}

TEST(LinkedLineTest, GetCoordinatesDeduplicatesConsecutiveAndPreservesZ) {
  const CoordinateSequence coords{
      Coordinate{0.0, 0.0, 5.0},
      Coordinate{1.0, 1.0, 6.0},
      Coordinate{1.0, 1.0, 7.0},
      Coordinate{2.0, 0.0, 8.0},
  };
  LinkedLine line(coords);

  auto extracted = line.getCoordinates();
  ASSERT_EQ(extracted->size(), 3u);
  EXPECT_TRUE(extracted->getAt(0).equals3D(Coordinate{0.0, 0.0, 5.0}));
  EXPECT_TRUE(extracted->getAt(1).equals3D(Coordinate{1.0, 1.0, 6.0}));
  EXPECT_TRUE(extracted->getAt(2).equals3D(Coordinate{2.0, 0.0, 8.0}));
}

}  // namespace
}  // namespace geos_google
