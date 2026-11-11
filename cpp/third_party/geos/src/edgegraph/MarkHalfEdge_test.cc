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

// Unit tests for third_party/geos/src/edgegraph/MarkHalfEdge.cpp and
// third_party/geos/include/geos/edgegraph/MarkHalfEdge.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/edgegraph/MarkHalfEdge.cpp

#include "third_party/geos/include/geos/edgegraph/MarkHalfEdge.h"

#include <cmath>
#include <cstddef>
#include <deque>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/edgegraph/EdgeGraph.h"
#include "third_party/geos/include/geos/edgegraph/HalfEdge.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"

namespace geos_google {
namespace {

using geos::edgegraph::EdgeGraph;
using geos::edgegraph::HalfEdge;
using geos::edgegraph::MarkHalfEdge;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYZM;

class MarkEdgeGraph final : public EdgeGraph {
 public:
  std::size_t edge_count() const { return mark_edges_.size(); }

 protected:
  HalfEdge* createEdge(const CoordinateXYZM& orig) override {
    mark_edges_.emplace_back(orig);
    return &mark_edges_.back();
  }

 private:
  std::deque<MarkHalfEdge> mark_edges_;
};

TEST(MarkHalfEdgeTest, InitialStateIsUnmarkedAndPreservesOrigin) {
  const CoordinateXYZM p0(1.0, 2.0, 3.0, 4.0);
  const CoordinateXYZM p1(5.0, 6.0, geos::DoubleNotANumber,
                          geos::DoubleNotANumber);
  MarkHalfEdge e0(p0);
  MarkHalfEdge e1(p1);
  e0.link(&e1);

  EXPECT_FALSE(e0.isMarked());
  EXPECT_FALSE(e1.isMarked());
  EXPECT_FALSE(MarkHalfEdge::isMarked(&e0));
  EXPECT_FALSE(MarkHalfEdge::isMarked(&e1));

  EXPECT_TRUE(e0.orig().equals4D(p0));
  EXPECT_TRUE(e0.dest().equals4D(p1));
  EXPECT_TRUE(std::isnan(e1.orig().z));
  EXPECT_TRUE(std::isnan(e1.orig().m));
}

TEST(MarkHalfEdgeTest, InstanceMarkAndSetMarkAffectOnlyTargetEdge) {
  MarkHalfEdge e0(CoordinateXYZM(0.0, 0.0, 0.0, 0.0));
  MarkHalfEdge e1(CoordinateXYZM(1.0, 0.0, 0.0, 0.0));
  e0.link(&e1);

  e0.mark();
  EXPECT_TRUE(e0.isMarked());
  EXPECT_FALSE(e1.isMarked());

  // Calling mark() when already marked is idempotent.
  e0.mark();
  EXPECT_TRUE(e0.isMarked());

  e0.setMark(false);
  EXPECT_FALSE(e0.isMarked());
  EXPECT_FALSE(e1.isMarked());

  e1.setMark(true);
  EXPECT_FALSE(e0.isMarked());
  EXPECT_TRUE(e1.isMarked());
}

TEST(MarkHalfEdgeTest, StaticMarkAndSetMarkThroughBasePointers) {
  MarkHalfEdge e0(CoordinateXYZM(0.0, 0.0, 0.0, 0.0));
  MarkHalfEdge e1(CoordinateXYZM(0.0, 2.0, 0.0, 0.0));
  e0.link(&e1);

  HalfEdge* h0 = &e0;
  HalfEdge* h1 = &e1;

  EXPECT_FALSE(MarkHalfEdge::isMarked(h0));
  EXPECT_FALSE(MarkHalfEdge::isMarked(h1));

  MarkHalfEdge::mark(h0);
  EXPECT_TRUE(MarkHalfEdge::isMarked(h0));
  EXPECT_FALSE(MarkHalfEdge::isMarked(h1));

  MarkHalfEdge::setMark(h0, false);
  EXPECT_FALSE(MarkHalfEdge::isMarked(h0));
  EXPECT_FALSE(MarkHalfEdge::isMarked(h1));

  MarkHalfEdge::setMark(h1, true);
  EXPECT_FALSE(MarkHalfEdge::isMarked(h0));
  EXPECT_TRUE(MarkHalfEdge::isMarked(h1));
}

TEST(MarkHalfEdgeTest, StaticMarkBothMarksEdgeAndSymmetricTwin) {
  MarkHalfEdge e0(CoordinateXYZM(1.0, 1.0, 0.0, 0.0));
  MarkHalfEdge e1(CoordinateXYZM(4.0, 5.0, 0.0, 0.0));
  e0.link(&e1);
  HalfEdge* h0 = &e0;
  HalfEdge* h1 = &e1;

  MarkHalfEdge::markBoth(h0);
  EXPECT_TRUE(MarkHalfEdge::isMarked(h0));
  EXPECT_TRUE(MarkHalfEdge::isMarked(h1));

  // Also test when called via sym on a half-marked pair.
  MarkHalfEdge e2(CoordinateXYZM(0.0, 0.0, 0.0, 0.0));
  MarkHalfEdge e3(CoordinateXYZM(2.0, 3.0, 0.0, 0.0));
  e2.link(&e3);
  MarkHalfEdge::mark(&e2);
  EXPECT_TRUE(MarkHalfEdge::isMarked(&e2));
  EXPECT_FALSE(MarkHalfEdge::isMarked(&e3));

  MarkHalfEdge::markBoth(&e3);
  EXPECT_TRUE(MarkHalfEdge::isMarked(&e2));
  EXPECT_TRUE(MarkHalfEdge::isMarked(&e3));
}

TEST(MarkHalfEdgeTest, StaticSetMarkBothSetsAndClearsSymmetricPair) {
  MarkHalfEdge e0(CoordinateXYZM(-1.0, 0.0, 0.0, 0.0));
  MarkHalfEdge e1(CoordinateXYZM(1.0, 0.0, 0.0, 0.0));
  e0.link(&e1);
  HalfEdge* h0 = &e0;
  HalfEdge* h1 = &e1;

  // Start with asymmetric mark state and synchronize to true.
  MarkHalfEdge::mark(h0);
  MarkHalfEdge::setMarkBoth(h0, true);
  EXPECT_TRUE(MarkHalfEdge::isMarked(h0));
  EXPECT_TRUE(MarkHalfEdge::isMarked(h1));

  // Clear both from the first edge.
  MarkHalfEdge::setMarkBoth(h0, false);
  EXPECT_FALSE(MarkHalfEdge::isMarked(h0));
  EXPECT_FALSE(MarkHalfEdge::isMarked(h1));

  // Asymmetric mark on twin, then synchronize to true and false from h0.
  MarkHalfEdge::mark(h1);
  EXPECT_FALSE(MarkHalfEdge::isMarked(h0));
  EXPECT_TRUE(MarkHalfEdge::isMarked(h1));

  MarkHalfEdge::setMarkBoth(h0, false);
  EXPECT_FALSE(MarkHalfEdge::isMarked(h0));
  EXPECT_FALSE(MarkHalfEdge::isMarked(h1));

  MarkHalfEdge::setMarkBoth(h1, true);
  EXPECT_TRUE(MarkHalfEdge::isMarked(h0));
  EXPECT_TRUE(MarkHalfEdge::isMarked(h1));
}

TEST(MarkHalfEdgeTest, PolymorphicEdgeGraphMarksEdgesIndependently) {
  MarkEdgeGraph graph;
  const CoordinateXYZM a(0.0, 0.0, 0.0, 0.0);
  const CoordinateXYZM b(1.0, 0.0, 0.0, 0.0);
  const CoordinateXYZM c(0.0, 1.0, 0.0, 0.0);
  const CoordinateXYZM d(2.0, 0.0, 0.0, 0.0);

  HalfEdge* ab = graph.addEdge(a, b);
  HalfEdge* bc = graph.addEdge(b, c);
  HalfEdge* ca = graph.addEdge(c, a);
  HalfEdge* bd = graph.addEdge(b, d);

  ASSERT_NE(ab, nullptr);
  ASSERT_NE(bc, nullptr);
  ASSERT_NE(ca, nullptr);
  ASSERT_NE(bd, nullptr);
  EXPECT_EQ(graph.edge_count(), 8U);

  EXPECT_EQ(graph.findEdge(CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0)), ab);
  EXPECT_FALSE(MarkHalfEdge::isMarked(ab));
  EXPECT_FALSE(MarkHalfEdge::isMarked(ab->sym()));
  EXPECT_FALSE(MarkHalfEdge::isMarked(bc));
  EXPECT_FALSE(MarkHalfEdge::isMarked(ca));
  EXPECT_FALSE(MarkHalfEdge::isMarked(bd));

  MarkHalfEdge::markBoth(ab);
  EXPECT_TRUE(MarkHalfEdge::isMarked(ab));
  EXPECT_TRUE(MarkHalfEdge::isMarked(ab->sym()));
  EXPECT_FALSE(MarkHalfEdge::isMarked(bc));
  EXPECT_FALSE(MarkHalfEdge::isMarked(ca));
  EXPECT_FALSE(MarkHalfEdge::isMarked(bd));

  MarkHalfEdge::mark(bc);
  EXPECT_TRUE(MarkHalfEdge::isMarked(bc));
  EXPECT_FALSE(MarkHalfEdge::isMarked(bc->sym()));

  MarkHalfEdge::setMarkBoth(ab, false);
  EXPECT_FALSE(MarkHalfEdge::isMarked(ab));
  EXPECT_FALSE(MarkHalfEdge::isMarked(ab->sym()));
  EXPECT_TRUE(MarkHalfEdge::isMarked(bc));
  EXPECT_FALSE(MarkHalfEdge::isMarked(bd));
}

}  // namespace
}  // namespace geos_google
