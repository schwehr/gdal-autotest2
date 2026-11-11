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

// Unit tests for third_party/geos/src/coverage/Corner.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/coverage/Corner.cpp

#include "third_party/geos/include/geos/coverage/Corner.h"

#include <cmath>
#include <memory>
#include <sstream>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/simplify/LinkedLine.h"

namespace geos_google {
namespace {

using geos::coverage::Corner;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::io::WKTReader;
using geos::simplify::LinkedLine;

TEST(CornerTest, OpenLineCornerAccessorsAndArea) {
  CoordinateSequence seq({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(3.0, 0.0),
      CoordinateXY(3.0, 4.0),
      CoordinateXY(6.0, 4.0),
  });
  LinkedLine edge(seq);
  ASSERT_FALSE(edge.isRing());

  Corner corner(&edge, 1);
  EXPECT_EQ(corner.getIndex(), 1u);
  EXPECT_DOUBLE_EQ(corner.getArea(), 6.0);
  EXPECT_TRUE(corner.prev().equals2D(Coordinate(0.0, 0.0)));
  EXPECT_TRUE(corner.getCoordinate().equals2D(Coordinate(3.0, 0.0)));
  EXPECT_TRUE(corner.next().equals2D(Coordinate(3.0, 4.0)));

  CoordinateSequence collinear_seq({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(2.0, 0.0),
      CoordinateXY(5.0, 0.0),
      CoordinateXY(5.0, 3.0),
  });
  LinkedLine collinear_edge(collinear_seq);
  Corner collinear(&collinear_edge, 1);
  EXPECT_DOUBLE_EQ(collinear.getArea(), 0.0);
}

TEST(CornerTest, ClosedRingWrapsPrevAndNextAtEndpoints) {
  CoordinateSequence ring_seq({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(4.0, 0.0),
      CoordinateXY(4.0, 3.0),
      CoordinateXY(0.0, 3.0),
      CoordinateXY(0.0, 0.0),
  });
  LinkedLine edge(ring_seq);
  ASSERT_TRUE(edge.isRing());
  ASSERT_EQ(edge.size(), 4u);

  Corner c0(&edge, 0);
  EXPECT_EQ(c0.getIndex(), 0u);
  EXPECT_DOUBLE_EQ(c0.getArea(), 6.0);
  EXPECT_TRUE(c0.prev().equals2D(Coordinate(0.0, 3.0)));
  EXPECT_TRUE(c0.getCoordinate().equals2D(Coordinate(0.0, 0.0)));
  EXPECT_TRUE(c0.next().equals2D(Coordinate(4.0, 0.0)));

  Corner c3(&edge, 3);
  EXPECT_EQ(c3.getIndex(), 3u);
  EXPECT_DOUBLE_EQ(c3.getArea(), 6.0);
  EXPECT_TRUE(c3.prev().equals2D(Coordinate(4.0, 3.0)));
  EXPECT_TRUE(c3.getCoordinate().equals2D(Coordinate(0.0, 3.0)));
  EXPECT_TRUE(c3.next().equals2D(Coordinate(0.0, 0.0)));
}

TEST(CornerTest, IsVertexByIndexAndCoordinate) {
  CoordinateSequence ring_seq({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(4.0, 0.0),
      CoordinateXY(4.0, 3.0),
      CoordinateXY(0.0, 3.0),
      CoordinateXY(0.0, 0.0),
  });
  LinkedLine edge(ring_seq);
  Corner corner(&edge, 1);  // vertices: prev=0 (0,0), idx=1 (4,0), next=2 (4,3)

  const std::size_t no_coord = geos::NO_COORD_INDEX;
  EXPECT_TRUE(corner.isVertex(0));
  EXPECT_TRUE(corner.isVertex(1));
  EXPECT_TRUE(corner.isVertex(2));
  EXPECT_FALSE(corner.isVertex(3));
  EXPECT_FALSE(corner.isVertex(no_coord));

  EXPECT_TRUE(corner.isVertex(Coordinate(0.0, 0.0, 42.0)));
  EXPECT_TRUE(corner.isVertex(Coordinate(4.0, 0.0)));
  EXPECT_TRUE(corner.isVertex(Coordinate(4.0, 3.0)));
  EXPECT_FALSE(corner.isVertex(Coordinate(0.0, 3.0)));
  EXPECT_FALSE(corner.isVertex(Coordinate(2.0, 1.5)));
}

TEST(CornerTest, IsBaselineMatchesEitherEndpointOrder) {
  CoordinateSequence seq({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(4.0, 0.0),
      CoordinateXY(4.0, 3.0),
      CoordinateXY(0.0, 3.0),
  });
  LinkedLine edge(seq);
  Corner corner(&edge, 1);  // prev=(0,0), vertex=(4,0), next=(4,3)

  EXPECT_TRUE(corner.isBaseline(Coordinate(0.0, 0.0), Coordinate(4.0, 3.0)));
  EXPECT_TRUE(corner.isBaseline(Coordinate(4.0, 3.0), Coordinate(0.0, 0.0)));

  EXPECT_FALSE(corner.isBaseline(Coordinate(0.0, 0.0), Coordinate(4.0, 0.0)));
  EXPECT_FALSE(corner.isBaseline(Coordinate(4.0, 0.0), Coordinate(4.0, 3.0)));
  EXPECT_FALSE(corner.isBaseline(Coordinate(4.0, 3.0), Coordinate(4.0, 0.0)));
  EXPECT_FALSE(corner.isBaseline(Coordinate(1.0, 1.0), Coordinate(2.0, 2.0)));
}

TEST(CornerTest, EnvelopeCoversAllThreeVertices) {
  // Vertex p=(5,6) extends strictly beyond baseline (0,0)-(2,1).
  CoordinateSequence seq_outside({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(5.0, 6.0),
      CoordinateXY(2.0, 1.0),
  });
  LinkedLine edge_outside(seq_outside);
  Corner corner_outside(&edge_outside, 1);
  Envelope env_outside = corner_outside.envelope();
  EXPECT_DOUBLE_EQ(env_outside.getMinX(), 0.0);
  EXPECT_DOUBLE_EQ(env_outside.getMaxX(), 5.0);
  EXPECT_DOUBLE_EQ(env_outside.getMinY(), 0.0);
  EXPECT_DOUBLE_EQ(env_outside.getMaxY(), 6.0);

  // Vertex p=(2,2) lies inside baseline (0,0)-(4,4).
  CoordinateSequence seq_inside({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(2.0, 2.0),
      CoordinateXY(4.0, 4.0),
  });
  LinkedLine edge_inside(seq_inside);
  Corner corner_inside(&edge_inside, 1);
  Envelope env_inside = corner_inside.envelope();
  EXPECT_DOUBLE_EQ(env_inside.getMinX(), 0.0);
  EXPECT_DOUBLE_EQ(env_inside.getMaxX(), 4.0);
  EXPECT_DOUBLE_EQ(env_inside.getMinY(), 0.0);
  EXPECT_DOUBLE_EQ(env_inside.getMaxY(), 4.0);
}

TEST(CornerTest, IntersectsPointInTriangleForCcwAndCwCorners) {
  CoordinateSequence ccw_seq({
      CoordinateXY(4.0, 0.0),
      CoordinateXY(0.0, 0.0),
      CoordinateXY(0.0, 4.0),
  });
  LinkedLine ccw_edge(ccw_seq);
  Corner ccw(&ccw_edge, 1);
  EXPECT_TRUE(ccw.intersects(Coordinate(1.0, 1.0)));
  EXPECT_TRUE(ccw.intersects(Coordinate(2.0, 2.0)));
  EXPECT_TRUE(ccw.intersects(Coordinate(0.0, 0.0)));
  EXPECT_FALSE(ccw.intersects(Coordinate(3.0, 3.0)));
  EXPECT_FALSE(ccw.intersects(Coordinate(-1.0, 1.0)));

  CoordinateSequence cw_seq({
      CoordinateXY(0.0, 4.0),
      CoordinateXY(0.0, 0.0),
      CoordinateXY(4.0, 0.0),
  });
  LinkedLine cw_edge(cw_seq);
  Corner cw(&cw_edge, 1);
  EXPECT_TRUE(cw.intersects(Coordinate(1.0, 1.0)));
  EXPECT_TRUE(cw.intersects(Coordinate(2.0, 0.0)));
  EXPECT_FALSE(cw.intersects(Coordinate(3.0, 3.0)));
}

TEST(CornerTest, IsRemovedTracksRemovalOfSelfPrevOrNext) {
  const CoordinateSequence seq({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(1.0, 1.0),
      CoordinateXY(2.0, 0.0),
      CoordinateXY(3.0, 2.0),
      CoordinateXY(4.0, 0.0),
      CoordinateXY(5.0, 0.0),
  });

  // Removing an unrelated vertex leaves corner intact; removing next flags it.
  {
    LinkedLine edge(seq);
    Corner corner(&edge, 3);  // prev=2, next=4
    EXPECT_FALSE(corner.isRemoved());
    edge.remove(1);
    EXPECT_FALSE(corner.isRemoved());
    edge.remove(4);  // removes next while prev remains 2
    EXPECT_TRUE(corner.isRemoved());
  }

  // Removing prev flags corner.
  {
    LinkedLine edge(seq);
    Corner corner(&edge, 3);
    edge.remove(2);
    EXPECT_TRUE(corner.isRemoved());
  }

  // Removing self flags corner.
  {
    LinkedLine edge(seq);
    Corner corner(&edge, 3);
    edge.remove(3);
    EXPECT_TRUE(corner.isRemoved());
  }
}

TEST(CornerTest, ToLineStringAndStreamOperator) {
  CoordinateSequence seq({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(2.0, 3.0),
      CoordinateXY(4.0, 0.0),
  });
  LinkedLine edge(seq);
  Corner corner(&edge, 1);

  std::unique_ptr<LineString> ls = corner.toLineString();
  ASSERT_NE(ls, nullptr);
  EXPECT_EQ(ls->getNumPoints(), 3u);
  EXPECT_TRUE(ls->getCoordinateN(0).equals2D(Coordinate(0.0, 0.0)));
  EXPECT_TRUE(ls->getCoordinateN(1).equals2D(Coordinate(2.0, 3.0)));
  EXPECT_TRUE(ls->getCoordinateN(2).equals2D(Coordinate(4.0, 0.0)));

  std::ostringstream os;
  os << corner;
  EXPECT_EQ(os.str(), "LINESTRING (0 0, 2 3, 4 0)");

  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  auto parsed = reader.read<LineString>(os.str());
  EXPECT_TRUE(parsed->equalsExact(ls.get(), 1e-12));
}

TEST(CornerTest, ToLineStringPreservesNaNForNullCoordinates) {
  CoordinateSequence seq({
      Coordinate(0.0, 0.0, 0.0),
      Coordinate::getNull(),
      Coordinate(1.0, 1.0, 0.0),
      Coordinate(2.0, 0.0, 0.0),
      Coordinate::getNull(),
  });
  LinkedLine edge(seq);

  // Vertex p is null at index 1.
  {
    Corner c_p(&edge, 1);
    auto ls = c_p.toLineString();
    ASSERT_EQ(ls->getNumPoints(), 3u);
    EXPECT_TRUE(ls->getCoordinateN(0).equals2D(Coordinate(0.0, 0.0)));
    EXPECT_TRUE(std::isnan(ls->getCoordinateN(1).x));
    EXPECT_TRUE(std::isnan(ls->getCoordinateN(1).y));
    EXPECT_TRUE(ls->getCoordinateN(2).equals2D(Coordinate(1.0, 1.0)));
  }

  // Vertex prev is null at index 2.
  {
    Corner c_pp(&edge, 2);
    auto ls = c_pp.toLineString();
    ASSERT_EQ(ls->getNumPoints(), 3u);
    EXPECT_TRUE(std::isnan(ls->getCoordinateN(0).x));
    EXPECT_TRUE(std::isnan(ls->getCoordinateN(0).y));
    EXPECT_TRUE(ls->getCoordinateN(1).equals2D(Coordinate(1.0, 1.0)));
    EXPECT_TRUE(ls->getCoordinateN(2).equals2D(Coordinate(2.0, 0.0)));
  }

  // Vertex next is null at index 3.
  {
    Corner c_pn(&edge, 3);
    auto ls = c_pn.toLineString();
    ASSERT_EQ(ls->getNumPoints(), 3u);
    EXPECT_TRUE(ls->getCoordinateN(0).equals2D(Coordinate(1.0, 1.0)));
    EXPECT_TRUE(ls->getCoordinateN(1).equals2D(Coordinate(2.0, 0.0)));
    EXPECT_TRUE(std::isnan(ls->getCoordinateN(2).x));
    EXPECT_TRUE(std::isnan(ls->getCoordinateN(2).y));
  }
}

TEST(CornerTest, ComparisonsAndPriorityQueueOrderByAreaThenIndex) {
  CoordinateSequence seq_a({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(1.0, 2.0),
      CoordinateXY(2.0, 0.0),  // idx 1 area = 0.5 * 2 * 2 = 2.0
  });
  CoordinateSequence seq_b({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(1.0, 0.0),
      CoordinateXY(2.0, 1.0),
      CoordinateXY(3.0, 0.0),  // idx 2 area = 0.5 * 2 * 1 = 1.0
  });
  CoordinateSequence seq_c({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(1.0, 0.0),
      CoordinateXY(2.0, 0.0),
      CoordinateXY(3.0, 2.0),
      CoordinateXY(4.0, 0.0),  // idx 3 area = 0.5 * 2 * 2 = 2.0
  });
  CoordinateSequence seq_d({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(1.0, 0.0),
      CoordinateXY(2.0, 0.0),
      CoordinateXY(3.0, 0.0),
      CoordinateXY(4.0, 5.0),
      CoordinateXY(5.0, 0.0),  // idx 4 area = 0.5 * 2 * 5 = 5.0
  });

  LinkedLine edge_a(seq_a);
  LinkedLine edge_b(seq_b);
  LinkedLine edge_c(seq_c);
  LinkedLine edge_d(seq_d);

  Corner c_tie_low(&edge_a, 1);   // area 2.0, index 1
  Corner c_small(&edge_b, 2);     // area 1.0, index 2
  Corner c_tie_high(&edge_c, 3);  // area 2.0, index 3
  Corner c_large(&edge_d, 4);     // area 5.0, index 4

  EXPECT_DOUBLE_EQ(c_small.getArea(), 1.0);
  EXPECT_DOUBLE_EQ(c_tie_low.getArea(), 2.0);
  EXPECT_DOUBLE_EQ(c_tie_high.getArea(), 2.0);
  EXPECT_DOUBLE_EQ(c_large.getArea(), 5.0);

  EXPECT_EQ(c_small.compareTo(c_tie_low), -1);
  EXPECT_EQ(c_large.compareTo(c_small), 1);
  EXPECT_EQ(c_tie_low.compareTo(c_tie_high), -1);
  EXPECT_EQ(c_tie_high.compareTo(c_tie_low), 1);
  EXPECT_EQ(c_tie_low.compareTo(c_tie_low), 0);

  EXPECT_TRUE(c_small < c_tie_low);
  EXPECT_TRUE(c_tie_low < c_tie_high);
  EXPECT_TRUE(c_large > c_tie_high);
  EXPECT_TRUE(c_tie_high > c_tie_low);
  EXPECT_TRUE(c_tie_low == c_tie_low);

  Corner::PriorityQueue pq;
  pq.push(c_large);
  pq.push(c_tie_high);
  pq.push(c_small);
  pq.push(c_tie_low);

  ASSERT_EQ(pq.size(), 4u);
  EXPECT_EQ(pq.top().getIndex(), 2u);
  pq.pop();
  EXPECT_EQ(pq.top().getIndex(), 1u);
  pq.pop();
  EXPECT_EQ(pq.top().getIndex(), 3u);
  pq.pop();
  EXPECT_EQ(pq.top().getIndex(), 4u);
  pq.pop();
}

}  // namespace
}  // namespace geos_google
