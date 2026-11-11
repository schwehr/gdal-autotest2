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

// Unit tests for third_party/geos/src/triangulate/quadedge/QuadEdge.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/triangulate/quadedge/QuadEdge.cpp

#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdge.h"

#include <deque>
#include <memory>
#include <sstream>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/LineSegment.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdgeQuartet.h"
#include "third_party/geos/include/geos/triangulate/quadedge/Vertex.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::LineSegment;
using geos::triangulate::quadedge::QuadEdge;
using geos::triangulate::quadedge::QuadEdgeQuartet;
using geos::triangulate::quadedge::Vertex;

TEST(QuadEdgeTest, MakeEdgeInitializesVerticesFlagsAndQuartetAlgebra) {
  std::deque<QuadEdgeQuartet> edges;
  const Vertex orig(1.0, 2.0);
  const Vertex dest(4.0, 6.0);

  QuadEdge* e = QuadEdge::makeEdge(orig, dest, edges);
  ASSERT_EQ(edges.size(), 1u);
  ASSERT_NE(e, nullptr);

  EXPECT_TRUE(e->isLive());
  EXPECT_FALSE(e->isVisited());
  EXPECT_TRUE(e->orig().equals(orig));
  EXPECT_TRUE(e->dest().equals(dest));
  EXPECT_TRUE(e->sym().orig().equals(dest));
  EXPECT_TRUE(e->sym().dest().equals(orig));
  EXPECT_DOUBLE_EQ(e->getLength(), 5.0);
  EXPECT_DOUBLE_EQ(e->sym().getLength(), 5.0);

  // Verify quartet rotation identities across all 4 slots (num = 0..3).
  QuadEdge& r1 = e->rot();
  QuadEdge& r2 = r1.rot();
  QuadEdge& r3 = r2.rot();
  QuadEdge& r4 = r3.rot();
  EXPECT_EQ(&r4, e);
  EXPECT_EQ(&r2, &e->sym());
  EXPECT_EQ(&e->sym().sym(), e);
  EXPECT_EQ(&r1.sym(), &r3);
  EXPECT_EQ(&r3.sym(), &r1);

  EXPECT_EQ(&e->invRot(), &r3);
  EXPECT_EQ(&r3.invRot(), &r2);
  EXPECT_EQ(&r2.invRot(), &r1);
  EXPECT_EQ(&r1.invRot(), e);

  // Verify const overloads and isolated-edge rings.
  const QuadEdge& ce = *e;
  EXPECT_EQ(&ce.rot().rot().rot().rot(), e);
  EXPECT_EQ(&ce.invRot().invRot().invRot().invRot(), e);
  EXPECT_EQ(&ce.sym(), &r2);
  EXPECT_EQ(&ce.oNext(), e);
  EXPECT_EQ(&ce.oPrev(), e);
  EXPECT_EQ(&ce.sym().oNext(), &ce.sym());
  EXPECT_EQ(&ce.lNext(), &ce.sym());
  EXPECT_EQ(&ce.lPrev(), &ce.sym());
  EXPECT_EQ(&ce.rNext(), &ce.sym());
  EXPECT_EQ(&ce.rPrev(), &ce.sym());
  EXPECT_EQ(&ce.dNext(), e);
  EXPECT_EQ(&ce.dPrev(), e);
}

TEST(QuadEdgeTest, SettersUpdateEndpointsAndVisitedState) {
  std::deque<QuadEdgeQuartet> edges;
  QuadEdge* e = QuadEdge::makeEdge(Vertex(0.0, 0.0), Vertex(0.0, 0.0), edges);
  EXPECT_DOUBLE_EQ(e->getLength(), 0.0);

  e->setOrig(Vertex(1.0, 1.0));
  e->setDest(Vertex(4.0, 5.0));
  EXPECT_TRUE(e->orig().equals(Vertex(1.0, 1.0)));
  EXPECT_TRUE(e->dest().equals(Vertex(4.0, 5.0)));
  EXPECT_TRUE(e->sym().orig().equals(Vertex(4.0, 5.0)));
  EXPECT_DOUBLE_EQ(e->getLength(), 5.0);

  e->setVisited(true);
  EXPECT_TRUE(e->isVisited());
  EXPECT_FALSE(e->sym().isVisited());
  e->setVisited(false);
  EXPECT_FALSE(e->isVisited());
}

TEST(QuadEdgeTest, SpliceCombinesAndSplitsVertexRings) {
  std::deque<QuadEdgeQuartet> edges;
  const Vertex v0(0.0, 0.0);
  QuadEdge* e0 = QuadEdge::makeEdge(v0, Vertex(10.0, 0.0), edges);
  QuadEdge* e1 = QuadEdge::makeEdge(v0, Vertex(0.0, 10.0), edges);

  EXPECT_EQ(&e0->oNext(), e0);
  EXPECT_EQ(&e1->oNext(), e1);

  // First splice joins the two origin rings into a single 2-edge cycle.
  QuadEdge::splice(*e0, *e1);
  EXPECT_EQ(&e0->oNext(), e1);
  EXPECT_EQ(&e1->oNext(), e0);
  EXPECT_EQ(&e0->oPrev(), e1);
  EXPECT_EQ(&e1->oPrev(), e0);

  // Splicing the same pair again is an involution and separates the rings.
  QuadEdge::splice(*e0, *e1);
  EXPECT_EQ(&e0->oNext(), e0);
  EXPECT_EQ(&e1->oNext(), e1);
}

TEST(QuadEdgeTest, ConnectBuildsTriangleWithConsistentFaceAndVertexRings) {
  std::deque<QuadEdgeQuartet> edges;
  const Vertex v0(0.0, 0.0);
  const Vertex v1(10.0, 0.0);
  const Vertex v2(5.0, 10.0);

  QuadEdge* e0 = QuadEdge::makeEdge(v0, v1, edges);
  QuadEdge* e1 = QuadEdge::makeEdge(v1, v2, edges);
  QuadEdge::splice(e0->sym(), *e1);

  QuadEdge* e2 = QuadEdge::connect(*e1, *e0, edges);
  ASSERT_EQ(edges.size(), 3u);
  ASSERT_NE(e2, nullptr);
  EXPECT_TRUE(e2->orig().equals(v2));
  EXPECT_TRUE(e2->dest().equals(v0));

  // Left face forms a directed triangle: e0 -> e1 -> e2 -> e0.
  EXPECT_EQ(&e0->lNext(), e1);
  EXPECT_EQ(&e1->lNext(), e2);
  EXPECT_EQ(&e2->lNext(), e0);
  EXPECT_EQ(&e0->lPrev(), e2);
  EXPECT_EQ(&e1->lPrev(), e0);
  EXPECT_EQ(&e2->lPrev(), e1);

  // Right (outer) face ring via const rNext / rPrev.
  const QuadEdge& ce0 = *e0;
  const QuadEdge& ce1 = *e1;
  const QuadEdge& ce2 = *e2;
  EXPECT_EQ(&ce0.rNext(), e2);
  EXPECT_EQ(&ce2.rNext(), e1);
  EXPECT_EQ(&ce1.rNext(), e0);
  EXPECT_EQ(&ce0.rPrev(), e1);
  EXPECT_EQ(&ce1.rPrev(), e2);
  EXPECT_EQ(&ce2.rPrev(), e0);
}

TEST(QuadEdgeTest, ConnectAndSwapRotateDiagonalCounterClockwise) {
  std::deque<QuadEdgeQuartet> edges;
  const Vertex v0(0.0, 0.0);
  const Vertex v1(10.0, 0.0);
  const Vertex v2(10.0, 10.0);
  const Vertex v3(0.0, 10.0);

  QuadEdge* e0 = QuadEdge::makeEdge(v0, v1, edges);
  QuadEdge* e1 = QuadEdge::makeEdge(v1, v2, edges);
  QuadEdge::splice(e0->sym(), *e1);
  QuadEdge* e2 = QuadEdge::makeEdge(v2, v3, edges);
  QuadEdge::splice(e1->sym(), *e2);
  QuadEdge* e3 = QuadEdge::connect(*e2, *e0, edges);

  // Split quad along diagonal v1 -> v3 using connect().
  QuadEdge* diag = QuadEdge::connect(*e0, *e3, edges);
  ASSERT_EQ(edges.size(), 5u);
  EXPECT_TRUE(diag->orig().equals(v1));
  EXPECT_TRUE(diag->dest().equals(v3));

  // Verify triangle face cycles on both sides of diagonal.
  EXPECT_EQ(&diag->lNext(), e3);
  EXPECT_EQ(&e3->lNext(), e0);
  EXPECT_EQ(&e0->lNext(), diag);
  EXPECT_EQ(&diag->sym().lNext(), e1);
  EXPECT_EQ(&e1->lNext(), e2);
  EXPECT_EQ(&e2->lNext(), &diag->sym());

  // Verify degree-3 vertex navigation around v1 (e1 -> diag -> e0.sym()).
  EXPECT_EQ(&e1->oNext(), diag);
  EXPECT_EQ(&diag->oNext(), &e0->sym());
  EXPECT_EQ(&e0->sym().oNext(), e1);
  EXPECT_EQ(&e0->dNext(), &e1->sym());
  EXPECT_EQ(&e1->sym().dNext(), &diag->sym());
  EXPECT_EQ(&diag->sym().dNext(), e0);
  EXPECT_EQ(&e0->dPrev(), &diag->sym());
  EXPECT_EQ(&diag->sym().dPrev(), &e1->sym());
  EXPECT_EQ(&e1->sym().dPrev(), e0);

  // First swap turns diagonal v1 -> v3 counterclockwise to v2 -> v0.
  QuadEdge::swap(*diag);
  EXPECT_TRUE(diag->orig().equals(v2));
  EXPECT_TRUE(diag->dest().equals(v0));
  EXPECT_EQ(&diag->lNext(), e0);
  EXPECT_EQ(&e0->lNext(), e1);
  EXPECT_EQ(&e1->lNext(), diag);
  EXPECT_EQ(&diag->sym().lNext(), e2);
  EXPECT_EQ(&e2->lNext(), e3);
  EXPECT_EQ(&e3->lNext(), &diag->sym());

  // Three more swaps step through v3 -> v1, v0 -> v2, and restore v1 -> v3.
  QuadEdge::swap(*diag);
  EXPECT_TRUE(diag->orig().equals(v3));
  EXPECT_TRUE(diag->dest().equals(v1));

  QuadEdge::swap(*diag);
  EXPECT_TRUE(diag->orig().equals(v0));
  EXPECT_TRUE(diag->dest().equals(v2));

  QuadEdge::swap(*diag);
  EXPECT_TRUE(diag->orig().equals(v1));
  EXPECT_TRUE(diag->dest().equals(v3));
}

TEST(QuadEdgeTest, GetPrimaryFollowsCoordinateOrdering) {
  std::deque<QuadEdgeQuartet> edges;

  // Ordered by x: orig.x < dest.x.
  QuadEdge* x_asc =
      QuadEdge::makeEdge(Vertex(1.0, 5.0), Vertex(3.0, 2.0), edges);
  EXPECT_EQ(&x_asc->getPrimary(), x_asc);
  EXPECT_EQ(&x_asc->sym().getPrimary(), x_asc);

  // Reversed x: orig.x > dest.x.
  QuadEdge* x_desc =
      QuadEdge::makeEdge(Vertex(3.0, 2.0), Vertex(1.0, 5.0), edges);
  EXPECT_EQ(&x_desc->getPrimary(), &x_desc->sym());
  EXPECT_EQ(&x_desc->sym().getPrimary(), &x_desc->sym());

  // Equal x, ordered by y: orig.y < dest.y and orig.y > dest.y.
  QuadEdge* y_asc =
      QuadEdge::makeEdge(Vertex(2.0, 1.0), Vertex(2.0, 9.0), edges);
  EXPECT_EQ(&y_asc->getPrimary(), y_asc);
  EXPECT_EQ(&y_asc->sym().getPrimary(), y_asc);

  QuadEdge* y_desc =
      QuadEdge::makeEdge(Vertex(2.0, 9.0), Vertex(2.0, 1.0), edges);
  EXPECT_EQ(&y_desc->getPrimary(), &y_desc->sym());
  EXPECT_EQ(&y_desc->sym().getPrimary(), &y_desc->sym());

  // Equal endpoints (compareTo == 0) returns *this on whichever side is
  // queried.
  QuadEdge* zero_len =
      QuadEdge::makeEdge(Vertex(2.0, 2.0), Vertex(2.0, 2.0), edges);
  EXPECT_EQ(&zero_len->getPrimary(), zero_len);
  EXPECT_EQ(&zero_len->sym().getPrimary(), &zero_len->sym());
}

TEST(QuadEdgeTest, RemoveMarksAllFourEdgesInQuartetNotLive) {
  std::deque<QuadEdgeQuartet> edges;
  QuadEdge* e0 = QuadEdge::makeEdge(Vertex(0.0, 0.0), Vertex(1.0, 0.0), edges);
  QuadEdge* e1 = QuadEdge::makeEdge(Vertex(2.0, 0.0), Vertex(3.0, 0.0), edges);

  e0->remove();
  EXPECT_FALSE(e0->isLive());
  EXPECT_FALSE(e0->rot().isLive());
  EXPECT_FALSE(e0->sym().isLive());
  EXPECT_FALSE(e0->invRot().isLive());
  EXPECT_TRUE(e1->isLive());

  // Calling remove() from a rotated dual edge also marks the whole quartet
  // dead.
  e1->rot().remove();
  EXPECT_FALSE(e1->isLive());
  EXPECT_FALSE(e1->rot().isLive());
  EXPECT_FALSE(e1->sym().isLive());
  EXPECT_FALSE(e1->invRot().isLive());
}

TEST(QuadEdgeTest, EqualityOrientedAndNonOriented) {
  std::deque<QuadEdgeQuartet> edges;
  const QuadEdge* base =
      QuadEdge::makeEdge(Vertex(1.0, 2.0, 10.0), Vertex(4.0, 6.0, 20.0), edges);
  const QuadEdge* same_2d_diff_z =
      QuadEdge::makeEdge(Vertex(1.0, 2.0, 99.0), Vertex(4.0, 6.0, -5.0), edges);
  const QuadEdge* reversed =
      QuadEdge::makeEdge(Vertex(4.0, 6.0), Vertex(1.0, 2.0), edges);
  const QuadEdge* diff_dest =
      QuadEdge::makeEdge(Vertex(1.0, 2.0), Vertex(4.0, 7.0), edges);
  const QuadEdge* diff_orig =
      QuadEdge::makeEdge(Vertex(0.0, 2.0), Vertex(4.0, 6.0), edges);
  const QuadEdge* disjoint =
      QuadEdge::makeEdge(Vertex(5.0, 5.0), Vertex(9.0, 9.0), edges);

  EXPECT_TRUE(base->equalsOriented(*base));
  EXPECT_TRUE(base->equalsOriented(*same_2d_diff_z));
  EXPECT_FALSE(base->equalsOriented(*reversed));
  EXPECT_FALSE(base->equalsOriented(*diff_dest));
  EXPECT_FALSE(base->equalsOriented(*diff_orig));

  EXPECT_TRUE(base->equalsNonOriented(*base));
  EXPECT_TRUE(base->equalsNonOriented(*same_2d_diff_z));
  EXPECT_TRUE(base->equalsNonOriented(*reversed));
  EXPECT_TRUE(base->equalsNonOriented(base->sym()));
  EXPECT_FALSE(base->equalsNonOriented(*diff_dest));
  EXPECT_FALSE(base->equalsNonOriented(*diff_orig));
  EXPECT_FALSE(base->equalsNonOriented(*disjoint));
}

TEST(QuadEdgeTest, ToLineSegmentPreservesCoordinatesAndOrientation) {
  std::deque<QuadEdgeQuartet> edges;
  const QuadEdge* e =
      QuadEdge::makeEdge(Vertex(1.5, 2.5, 3.5), Vertex(4.5, 6.5, 7.5), edges);

  std::unique_ptr<LineSegment> seg = e->toLineSegment();
  ASSERT_NE(seg, nullptr);
  EXPECT_TRUE(seg->p0.equals2D(Coordinate(1.5, 2.5)));
  EXPECT_DOUBLE_EQ(seg->p0.z, 3.5);
  EXPECT_TRUE(seg->p1.equals2D(Coordinate(4.5, 6.5)));
  EXPECT_DOUBLE_EQ(seg->p1.z, 7.5);

  std::unique_ptr<LineSegment> rev_seg = e->sym().toLineSegment();
  ASSERT_NE(rev_seg, nullptr);
  EXPECT_TRUE(rev_seg->p0.equals2D(Coordinate(4.5, 6.5)));
  EXPECT_DOUBLE_EQ(rev_seg->p0.z, 7.5);
  EXPECT_TRUE(rev_seg->p1.equals2D(Coordinate(1.5, 2.5)));
  EXPECT_DOUBLE_EQ(rev_seg->p1.z, 3.5);
}

TEST(QuadEdgeTest, StreamOperatorFormatsOriginAndDestination) {
  std::deque<QuadEdgeQuartet> edges;
  const QuadEdge* e2d =
      QuadEdge::makeEdge(Vertex(1.0, 2.0), Vertex(3.0, 4.0), edges);
  const QuadEdge* e3d =
      QuadEdge::makeEdge(Vertex(1.0, 2.0, 5.0), Vertex(3.0, 4.0, 6.0), edges);

  std::ostringstream os_2d;
  os_2d << e2d;
  EXPECT_EQ(os_2d.str(), "( 1 2, 3 4 )");

  std::ostringstream os_3d;
  os_3d << e3d;
  EXPECT_EQ(os_3d.str(), "( 1 2 5, 3 4 6 )");
}

}  // namespace
}  // namespace geos_google
