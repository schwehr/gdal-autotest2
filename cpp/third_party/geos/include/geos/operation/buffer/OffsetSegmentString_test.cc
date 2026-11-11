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

#include "third_party/geos/include/geos/operation/buffer/OffsetSegmentString.h"

#include <cstddef>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"

namespace geos_google {
namespace {

static_assert(!std::is_copy_constructible_v<
              geos::operation::buffer::OffsetSegmentString>);
static_assert(
    !std::is_copy_assignable_v<geos::operation::buffer::OffsetSegmentString>);

TEST(OffsetSegmentStringTest, DefaultStateIsEmptyAndConsumesCleanly) {
  geos::operation::buffer::OffsetSegmentString oss;
  EXPECT_EQ(oss.size(), 0u);

  oss.closeRing();
  EXPECT_EQ(oss.size(), 0u);

  std::ostringstream before;
  before << oss;
  EXPECT_EQ(before.str(), "()");

  std::unique_ptr<geos::geom::CoordinateSequence> coords(oss.getCoordinates());
  ASSERT_NE(coords, nullptr);
  EXPECT_TRUE(coords->isEmpty());
  EXPECT_EQ(oss.size(), 0u);

  std::ostringstream after;
  after << oss;
  EXPECT_EQ(after.str(), "empty (consumed?)");
}

TEST(OffsetSegmentStringTest, DestructorFreesUnconsumedCoordinates) {
  geos::geom::PrecisionModel pm(geos::geom::PrecisionModel::FLOATING);
  geos::operation::buffer::OffsetSegmentString oss;
  oss.setPrecisionModel(&pm);
  oss.addPt(geos::geom::Coordinate(1.0, 2.0));
  EXPECT_EQ(oss.size(), 1u);
}

TEST(OffsetSegmentStringTest,
     AddPtWithFloatingPrecisionAndZeroMinDistPreservesZAndDuplicates) {
  geos::geom::PrecisionModel pm(geos::geom::PrecisionModel::FLOATING);
  geos::operation::buffer::OffsetSegmentString oss;
  oss.setPrecisionModel(&pm);

  oss.addPt(geos::geom::Coordinate(1.25, 2.5, 9.0));
  EXPECT_EQ(oss.size(), 1u);

  // With default minimumVertexDistance == 0.0, ptDist < 0.0 is false, so exact
  // duplicates are retained.
  oss.addPt(geos::geom::Coordinate(1.25, 2.5, 9.0));
  EXPECT_EQ(oss.size(), 2u);

  oss.addPt(geos::geom::Coordinate(4.5, 6.5, 11.0));
  EXPECT_EQ(oss.size(), 3u);

  std::ostringstream os;
  os << oss;
  EXPECT_EQ(os.str(), "(1.25 2.5 9, 1.25 2.5 9, 4.5 6.5 11)");

  std::unique_ptr<geos::geom::CoordinateSequence> coords(oss.getCoordinates());
  ASSERT_NE(coords, nullptr);
  ASSERT_EQ(coords->size(), 4u);
  EXPECT_TRUE(
      coords->getAt(0).equals3D(geos::geom::Coordinate(1.25, 2.5, 9.0)));
  EXPECT_TRUE(
      coords->getAt(1).equals3D(geos::geom::Coordinate(1.25, 2.5, 9.0)));
  EXPECT_TRUE(
      coords->getAt(2).equals3D(geos::geom::Coordinate(4.5, 6.5, 11.0)));
  EXPECT_TRUE(
      coords->getAt(3).equals3D(geos::geom::Coordinate(1.25, 2.5, 9.0)));
  EXPECT_EQ(oss.size(), 0u);
}

TEST(OffsetSegmentStringTest, AddPtAppliesPrecisionModelBeforeRedundancyCheck) {
  geos::geom::PrecisionModel pm_fixed(4.0);
  geos::operation::buffer::OffsetSegmentString oss;
  oss.setPrecisionModel(&pm_fixed);
  oss.setMinimumVertexDistance(0.05);

  oss.addPt(geos::geom::Coordinate(1.04, 2.04));
  oss.addPt(geos::geom::Coordinate(1.09, 2.09));
  oss.addPt(geos::geom::Coordinate(1.24, 2.01));

  ASSERT_EQ(oss.size(), 2u);
  std::unique_ptr<geos::geom::CoordinateSequence> coords(oss.getCoordinates());
  ASSERT_NE(coords, nullptr);
  ASSERT_EQ(coords->size(), 3u);
  EXPECT_TRUE(coords->getAt<geos::geom::CoordinateXY>(0).equals2D(
      geos::geom::CoordinateXY(1.0, 2.0)));
  EXPECT_TRUE(coords->getAt<geos::geom::CoordinateXY>(1).equals2D(
      geos::geom::CoordinateXY(1.25, 2.0)));
  EXPECT_TRUE(coords->getAt<geos::geom::CoordinateXY>(2).equals2D(
      geos::geom::CoordinateXY(1.0, 2.0)));

  geos::geom::PrecisionModel pm_single(
      geos::geom::PrecisionModel::FLOATING_SINGLE);
  geos::operation::buffer::OffsetSegmentString oss_single;
  oss_single.setPrecisionModel(&pm_single);
  oss_single.setMinimumVertexDistance(1e-9);

  oss_single.addPt(geos::geom::Coordinate(1.0, 2.0));
  oss_single.addPt(geos::geom::Coordinate(1.0 + 1e-10, 2.0));
  oss_single.addPt(geos::geom::Coordinate(1.5, 2.0));

  ASSERT_EQ(oss_single.size(), 2u);
  std::unique_ptr<geos::geom::CoordinateSequence> single_coords(
      oss_single.getCoordinates());
  ASSERT_NE(single_coords, nullptr);
  ASSERT_EQ(single_coords->size(), 3u);
  EXPECT_TRUE(single_coords->getAt<geos::geom::CoordinateXY>(0).equals2D(
      geos::geom::CoordinateXY(1.0, 2.0)));
  EXPECT_TRUE(single_coords->getAt<geos::geom::CoordinateXY>(1).equals2D(
      geos::geom::CoordinateXY(1.5, 2.0)));
  EXPECT_TRUE(single_coords->getAt<geos::geom::CoordinateXY>(2).equals2D(
      geos::geom::CoordinateXY(1.0, 2.0)));
}

TEST(OffsetSegmentStringTest,
     MinimumVertexDistanceFiltersAgainstLastVertexAtExactThreshold) {
  geos::geom::PrecisionModel pm(geos::geom::PrecisionModel::FLOATING);
  geos::operation::buffer::OffsetSegmentString oss;
  oss.setPrecisionModel(&pm);
  oss.setMinimumVertexDistance(1.0);

  oss.addPt(geos::geom::Coordinate(0.0, 0.0, 1.0));
  EXPECT_EQ(oss.size(), 1u);

  // Redundancy check uses 2D Euclidean distance; differing Z does not retain
  // a point whose 2D distance is below minimumVertexDistance.
  oss.addPt(geos::geom::Coordinate(0.0, 0.0, 99.0));
  EXPECT_EQ(oss.size(), 1u);

  // Distance is sqrt(0.36 + 0.49) < 1.0, so filtered.
  oss.addPt(geos::geom::Coordinate(0.6, 0.7));
  EXPECT_EQ(oss.size(), 1u);

  // Distance == 1.0 is not strictly less than minimumVertexDistance, so added.
  oss.addPt(geos::geom::Coordinate(1.0, 0.0));
  EXPECT_EQ(oss.size(), 2u);

  // Distance from (1.0, 0.0) is 0.99 < 1.0, so filtered.
  oss.addPt(geos::geom::Coordinate(1.0, 0.99));
  EXPECT_EQ(oss.size(), 2u);

  // Returning to (0.0, 0.0) has distance 1.0 from (1.0, 0.0), so added.
  oss.addPt(geos::geom::Coordinate(0.0, 0.0));
  EXPECT_EQ(oss.size(), 3u);

  // Already closed; getCoordinates() does not add a duplicate closing vertex.
  std::unique_ptr<geos::geom::CoordinateSequence> coords(oss.getCoordinates());
  ASSERT_NE(coords, nullptr);
  ASSERT_EQ(coords->size(), 3u);
  EXPECT_TRUE(coords->getAt<geos::geom::CoordinateXY>(0).equals2D(
      geos::geom::CoordinateXY(0.0, 0.0)));
  EXPECT_TRUE(coords->getAt<geos::geom::CoordinateXY>(1).equals2D(
      geos::geom::CoordinateXY(1.0, 0.0)));
  EXPECT_TRUE(coords->getAt<geos::geom::CoordinateXY>(2).equals2D(
      geos::geom::CoordinateXY(0.0, 0.0)));
}

TEST(OffsetSegmentStringTest, AddPtsSupportsEmptySingleForwardAndReverse) {
  geos::geom::PrecisionModel pm(geos::geom::PrecisionModel::FLOATING);
  geos::operation::buffer::OffsetSegmentString oss;
  oss.setPrecisionModel(&pm);
  oss.setMinimumVertexDistance(0.01);

  geos::geom::CoordinateSequence empty_seq;
  oss.addPts(empty_seq, /*isForward=*/true);
  EXPECT_EQ(oss.size(), 0u);
  oss.addPts(empty_seq, /*isForward=*/false);
  EXPECT_EQ(oss.size(), 0u);

  geos::geom::CoordinateSequence seq{
      geos::geom::Coordinate(0.0, 0.0),
      geos::geom::Coordinate(0.005, 0.0),
      geos::geom::Coordinate(2.0, 0.0),
      geos::geom::Coordinate(2.0, 2.0),
  };
  oss.addPts(seq, /*isForward=*/true);
  ASSERT_EQ(oss.size(), 3u);

  std::unique_ptr<geos::geom::CoordinateSequence> forward_coords(
      oss.getCoordinates());
  ASSERT_NE(forward_coords, nullptr);
  ASSERT_EQ(forward_coords->size(), 4u);
  EXPECT_TRUE(forward_coords->getAt<geos::geom::CoordinateXY>(0).equals2D(
      geos::geom::CoordinateXY(0.0, 0.0)));
  EXPECT_TRUE(forward_coords->getAt<geos::geom::CoordinateXY>(1).equals2D(
      geos::geom::CoordinateXY(2.0, 0.0)));
  EXPECT_TRUE(forward_coords->getAt<geos::geom::CoordinateXY>(2).equals2D(
      geos::geom::CoordinateXY(2.0, 2.0)));
  EXPECT_TRUE(forward_coords->getAt<geos::geom::CoordinateXY>(3).equals2D(
      geos::geom::CoordinateXY(0.0, 0.0)));

  oss.reset();
  oss.setPrecisionModel(&pm);
  oss.setMinimumVertexDistance(0.01);

  geos::geom::CoordinateSequence single{geos::geom::Coordinate(5.0, 5.0)};
  oss.addPts(single, /*isForward=*/false);
  EXPECT_EQ(oss.size(), 1u);

  oss.reset();
  oss.setPrecisionModel(&pm);
  oss.setMinimumVertexDistance(0.01);

  geos::geom::CoordinateSequence seq2{
      geos::geom::Coordinate(0.0, 0.0),
      geos::geom::Coordinate(2.0, 0.0),
      geos::geom::Coordinate(2.0, 2.0),
  };
  oss.addPts(seq2, /*isForward=*/false);
  ASSERT_EQ(oss.size(), 3u);

  std::unique_ptr<geos::geom::CoordinateSequence> reverse_coords(
      oss.getCoordinates());
  ASSERT_NE(reverse_coords, nullptr);
  ASSERT_EQ(reverse_coords->size(), 4u);
  EXPECT_TRUE(reverse_coords->getAt<geos::geom::CoordinateXY>(0).equals2D(
      geos::geom::CoordinateXY(2.0, 2.0)));
  EXPECT_TRUE(reverse_coords->getAt<geos::geom::CoordinateXY>(1).equals2D(
      geos::geom::CoordinateXY(2.0, 0.0)));
  EXPECT_TRUE(reverse_coords->getAt<geos::geom::CoordinateXY>(2).equals2D(
      geos::geom::CoordinateXY(0.0, 0.0)));
  EXPECT_TRUE(reverse_coords->getAt<geos::geom::CoordinateXY>(3).equals2D(
      geos::geom::CoordinateXY(2.0, 2.0)));
}

TEST(OffsetSegmentStringTest,
     CloseRingHandlesSinglePointNearClosureAndReallocation) {
  geos::geom::PrecisionModel pm(geos::geom::PrecisionModel::FLOATING);
  geos::operation::buffer::OffsetSegmentString oss;
  oss.setPrecisionModel(&pm);
  oss.addPt(geos::geom::Coordinate(3.0, 4.0));
  oss.closeRing();
  EXPECT_EQ(oss.size(), 1u);

  oss.reset();
  oss.setPrecisionModel(&pm);
  oss.setMinimumVertexDistance(1.0);
  oss.addPt(geos::geom::Coordinate(0.0, 0.0));
  oss.addPt(geos::geom::Coordinate(2.0, 0.0));
  // Distance to start (0, 0) is 0.2 < 1.0, yet closeRing checks exact equality
  // and must still append the start point.
  oss.addPt(geos::geom::Coordinate(0.2, 0.0));
  ASSERT_EQ(oss.size(), 3u);

  oss.closeRing();
  EXPECT_EQ(oss.size(), 4u);
  // Idempotent on already-closed ring.
  oss.closeRing();
  EXPECT_EQ(oss.size(), 4u);

  std::unique_ptr<geos::geom::CoordinateSequence> near_coords(
      oss.getCoordinates());
  ASSERT_NE(near_coords, nullptr);
  ASSERT_EQ(near_coords->size(), 4u);
  EXPECT_TRUE(near_coords->getAt<geos::geom::CoordinateXY>(3).equals2D(
      geos::geom::CoordinateXY(0.0, 0.0)));

  oss.reset();
  oss.setPrecisionModel(&pm);
  for (int i = 0; i < 64; ++i) {
    oss.addPt(geos::geom::Coordinate(static_cast<double>(i), 1.0));
  }
  ASSERT_EQ(oss.size(), 64u);
  oss.closeRing();
  ASSERT_EQ(oss.size(), 65u);

  std::unique_ptr<geos::geom::CoordinateSequence> large_coords(
      oss.getCoordinates());
  ASSERT_NE(large_coords, nullptr);
  ASSERT_EQ(large_coords->size(), 65u);
  EXPECT_TRUE(large_coords->getAt<geos::geom::CoordinateXY>(64).equals2D(
      geos::geom::CoordinateXY(0.0, 1.0)));
}

TEST(OffsetSegmentStringTest,
     ResetClearsUnconsumedStateAndReinitializesConsumedState) {
  geos::geom::PrecisionModel pm(geos::geom::PrecisionModel::FLOATING);
  geos::operation::buffer::OffsetSegmentString oss;
  oss.setPrecisionModel(&pm);
  oss.setMinimumVertexDistance(5.0);
  oss.addPt(geos::geom::Coordinate(0.0, 0.0));
  oss.addPt(geos::geom::Coordinate(10.0, 0.0));
  ASSERT_EQ(oss.size(), 2u);

  // Unconsumed reset clears points and resets minimumVertexDistance to 0.0.
  oss.reset();
  EXPECT_EQ(oss.size(), 0u);
  {
    std::ostringstream os;
    os << oss;
    EXPECT_EQ(os.str(), "()");
  }

  oss.setPrecisionModel(&pm);
  oss.addPt(geos::geom::Coordinate(1.0, 1.0));
  oss.addPt(geos::geom::Coordinate(1.0, 1.0));
  oss.addPt(geos::geom::Coordinate(1.001, 1.001));
  EXPECT_EQ(oss.size(), 3u);

  std::unique_ptr<geos::geom::CoordinateSequence> first_coords(
      oss.getCoordinates());
  ASSERT_NE(first_coords, nullptr);
  EXPECT_EQ(oss.size(), 0u);
  {
    std::ostringstream os;
    os << oss;
    EXPECT_EQ(os.str(), "empty (consumed?)");
  }

  // Consumed reset allocates a fresh CoordinateSequence.
  oss.reset();
  EXPECT_EQ(oss.size(), 0u);
  {
    std::ostringstream os;
    os << oss;
    EXPECT_EQ(os.str(), "()");
  }

  oss.setPrecisionModel(&pm);
  oss.addPt(geos::geom::Coordinate(7.0, 8.0));
  oss.addPt(geos::geom::Coordinate(9.0, 10.0));
  EXPECT_EQ(oss.size(), 2u);

  std::unique_ptr<geos::geom::CoordinateSequence> second_coords(
      oss.getCoordinates());
  ASSERT_NE(second_coords, nullptr);
  ASSERT_EQ(second_coords->size(), 3u);
  EXPECT_TRUE(second_coords->getAt<geos::geom::CoordinateXY>(0).equals2D(
      geos::geom::CoordinateXY(7.0, 8.0)));
  EXPECT_TRUE(second_coords->getAt<geos::geom::CoordinateXY>(1).equals2D(
      geos::geom::CoordinateXY(9.0, 10.0)));
  EXPECT_TRUE(second_coords->getAt<geos::geom::CoordinateXY>(2).equals2D(
      geos::geom::CoordinateXY(7.0, 8.0)));
}

TEST(OffsetSegmentStringTest, StreamOperatorChainsAndFormatsStates) {
  geos::geom::PrecisionModel pm(geos::geom::PrecisionModel::FLOATING);
  geos::operation::buffer::OffsetSegmentString oss;
  oss.setPrecisionModel(&pm);

  std::ostringstream os;
  os << oss << "|";
  oss.addPt(geos::geom::Coordinate(1.0, 2.0));
  oss.addPt(geos::geom::Coordinate(3.0, 4.0));
  os << oss << "|";

  std::unique_ptr<geos::geom::CoordinateSequence> coords(oss.getCoordinates());
  ASSERT_NE(coords, nullptr);
  os << oss;

  EXPECT_EQ(os.str(), "()|(1 2, 3 4)|empty (consumed?)");
}

}  // namespace
}  // namespace geos_google
