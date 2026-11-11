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

// Unit tests for third_party/geos/src/operation/polygonize/HoleAssigner.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/polygonize/HoleAssigner.cpp

#include "third_party/geos/include/geos/operation/polygonize/HoleAssigner.h"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/operation/polygonize/EdgeRing.h"
#include "third_party/geos/include/geos/operation/polygonize/PolygonizeDirectedEdge.h"
#include "third_party/geos/include/geos/operation/polygonize/PolygonizeEdge.h"
#include "third_party/geos/include/geos/planargraph/Node.h"
#include "third_party/geos/include/geos/util/GEOSException.h"
#include "third_party/geos/include/geos/util/Interrupt.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::geom::Polygon;
using geos::operation::polygonize::EdgeRing;
using geos::operation::polygonize::HoleAssigner;
using geos::operation::polygonize::PolygonizeDirectedEdge;
using geos::operation::polygonize::PolygonizeEdge;
using geos::planargraph::Node;
using geos::util::CurrentThreadInterrupt;
using geos::util::GEOSException;

// Owns synthetic closed single-edge EdgeRings and their backing graph objects.
class RingBuilder {
 public:
  explicit RingBuilder(const GeometryFactory* factory) : factory_(factory) {}

  // Clockwise coordinates yield a shell (isHole() == false); counter-clockwise
  // coordinates yield a hole (isHole() == true).
  EdgeRing* CreateRing(const std::vector<Coordinate>& coords) {
    auto seq = std::make_unique<CoordinateSequence>(0u, 2u);
    for (const Coordinate& pt : coords) {
      seq->add(pt);
    }
    auto line = factory_->createLineString(std::move(seq));
    const CoordinateSequence* pts = line->getCoordinatesRO();

    auto node = std::make_unique<Node>(pts->getAt(0));
    auto de_fwd = std::make_unique<PolygonizeDirectedEdge>(
        node.get(), node.get(), pts->getAt(1), true);
    auto de_rev = std::make_unique<PolygonizeDirectedEdge>(
        node.get(), node.get(), pts->getAt(pts->getSize() - 2), false);
    auto edge = std::make_unique<PolygonizeEdge>(line.get());
    edge->setDirectedEdges(de_fwd.get(), de_rev.get());
    de_fwd->setNext(de_fwd.get());
    de_rev->setNext(de_rev.get());

    auto er = std::make_unique<EdgeRing>(factory_);
    er->build(de_fwd.get());
    er->computeValid();
    er->computeHole();

    EdgeRing* result = er.get();
    lines_.push_back(std::move(line));
    nodes_.push_back(std::move(node));
    dir_edges_.push_back(std::move(de_fwd));
    dir_edges_.push_back(std::move(de_rev));
    edges_.push_back(std::move(edge));
    rings_.push_back(std::move(er));
    return result;
  }

  EdgeRing* CreateCwSquare(double min_x, double min_y, double max_x,
                           double max_y) {
    return CreateRing({
        Coordinate(min_x, min_y),
        Coordinate(min_x, max_y),
        Coordinate(max_x, max_y),
        Coordinate(max_x, min_y),
        Coordinate(min_x, min_y),
    });
  }

  EdgeRing* CreateCcwSquare(double min_x, double min_y, double max_x,
                            double max_y) {
    return CreateRing({
        Coordinate(min_x, min_y),
        Coordinate(max_x, min_y),
        Coordinate(max_x, max_y),
        Coordinate(min_x, max_y),
        Coordinate(min_x, min_y),
    });
  }

 private:
  const GeometryFactory* factory_;
  // Keep destruction order: rings_ before dir_edges_/edges_/nodes_/lines_.
  std::vector<std::unique_ptr<LineString>> lines_;
  std::vector<std::unique_ptr<Node>> nodes_;
  std::vector<std::unique_ptr<PolygonizeDirectedEdge>> dir_edges_;
  std::vector<std::unique_ptr<PolygonizeEdge>> edges_;
  std::vector<std::unique_ptr<EdgeRing>> rings_;
};

TEST(HoleAssignerTest, EmptyHolesAndEmptyShells) {
  std::vector<EdgeRing*> holes;
  std::vector<EdgeRing*> shells;
  HoleAssigner::assignHolesToShells(holes, shells);
  EXPECT_TRUE(holes.empty());
  EXPECT_TRUE(shells.empty());
}

TEST(HoleAssignerTest, EmptyHolesLeavesShellWithoutInteriorRings) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  RingBuilder builder(factory.get());

  EdgeRing* shell = builder.CreateCwSquare(0.0, 0.0, 10.0, 10.0);
  ASSERT_FALSE(shell->isHole());

  std::vector<EdgeRing*> holes;
  std::vector<EdgeRing*> shells = {shell};
  HoleAssigner::assignHolesToShells(holes, shells);

  std::unique_ptr<Polygon> poly = shell->getPolygon();
  ASSERT_NE(poly, nullptr);
  EXPECT_EQ(poly->getNumInteriorRing(), 0u);
  EXPECT_DOUBLE_EQ(poly->getArea(), 100.0);
}

TEST(HoleAssignerTest, EmptyShellsLeavesHolesUnassigned) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  RingBuilder builder(factory.get());

  EdgeRing* hole0 = builder.CreateCcwSquare(1.0, 1.0, 3.0, 3.0);
  EdgeRing* hole1 = builder.CreateCcwSquare(5.0, 5.0, 7.0, 7.0);
  ASSERT_TRUE(hole0->isHole());
  ASSERT_TRUE(hole1->isHole());

  std::vector<EdgeRing*> holes = {hole0, hole1};
  std::vector<EdgeRing*> shells;
  HoleAssigner::assignHolesToShells(holes, shells);

  EXPECT_FALSE(hole0->hasShell());
  EXPECT_FALSE(hole1->hasShell());
  EXPECT_TRUE(hole0->isOuterHole());
  EXPECT_TRUE(hole1->isOuterHole());
}

TEST(HoleAssignerTest, AssignsSingleHoleToContainingShell) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  RingBuilder builder(factory.get());

  EdgeRing* shell = builder.CreateCwSquare(0.0, 0.0, 10.0, 10.0);
  EdgeRing* hole = builder.CreateCcwSquare(2.0, 2.0, 8.0, 8.0);
  ASSERT_FALSE(shell->isHole());
  ASSERT_TRUE(hole->isHole());

  std::vector<EdgeRing*> holes = {hole};
  std::vector<EdgeRing*> shells = {shell};
  HoleAssigner::assignHolesToShells(holes, shells);

  EXPECT_TRUE(hole->hasShell());
  EXPECT_EQ(hole->getShell(), shell);
  EXPECT_FALSE(hole->isOuterHole());

  std::unique_ptr<Polygon> poly = shell->getPolygon();
  ASSERT_NE(poly, nullptr);
  EXPECT_EQ(poly->getNumInteriorRing(), 1u);
  EXPECT_DOUBLE_EQ(poly->getArea(), 100.0 - 36.0);
  EXPECT_TRUE(poly->isValid());
}

TEST(HoleAssignerTest, AssignsMultipleHolesToSameShell) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  RingBuilder builder(factory.get());

  EdgeRing* shell = builder.CreateCwSquare(0.0, 0.0, 20.0, 20.0);
  EdgeRing* hole0 = builder.CreateCcwSquare(2.0, 2.0, 8.0, 8.0);
  EdgeRing* hole1 = builder.CreateCcwSquare(12.0, 12.0, 18.0, 18.0);

  std::vector<EdgeRing*> holes = {hole0, hole1};
  std::vector<EdgeRing*> shells = {shell};
  HoleAssigner::assignHolesToShells(holes, shells);

  EXPECT_EQ(hole0->getShell(), shell);
  EXPECT_EQ(hole1->getShell(), shell);

  std::unique_ptr<Polygon> poly = shell->getPolygon();
  ASSERT_NE(poly, nullptr);
  EXPECT_EQ(poly->getNumInteriorRing(), 2u);
  EXPECT_DOUBLE_EQ(poly->getArea(), 400.0 - 36.0 - 36.0);
  EXPECT_TRUE(poly->isValid());
}

TEST(HoleAssignerTest, AssignsHolesAcrossManyDisjointShellsUsingSpatialIndex) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  RingBuilder builder(factory.get());

  // Create 16 shells on a 4x4 grid so TemplateSTRtree creates multi-level
  // nodes.
  std::vector<EdgeRing*> shells;
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 4; ++col) {
      const double x0 = col * 10.0;
      const double y0 = row * 10.0;
      shells.push_back(builder.CreateCwSquare(x0, y0, x0 + 8.0, y0 + 8.0));
    }
  }

  EdgeRing* hole_in_0 = builder.CreateCcwSquare(2.0, 2.0, 6.0, 6.0);
  EdgeRing* hole_in_5 = builder.CreateCcwSquare(12.0, 12.0, 16.0, 16.0);
  EdgeRing* hole_in_15 = builder.CreateCcwSquare(32.0, 32.0, 36.0, 36.0);
  EdgeRing* outer_hole = builder.CreateCcwSquare(100.0, 100.0, 110.0, 110.0);

  std::vector<EdgeRing*> holes = {hole_in_0, hole_in_5, hole_in_15, outer_hole};
  HoleAssigner::assignHolesToShells(holes, shells);

  EXPECT_EQ(hole_in_0->getShell(), shells[0]);
  EXPECT_EQ(hole_in_5->getShell(), shells[5]);
  EXPECT_EQ(hole_in_15->getShell(), shells[15]);
  EXPECT_FALSE(outer_hole->hasShell());
  EXPECT_TRUE(outer_hole->isOuterHole());

  for (std::size_t i = 0; i < shells.size(); ++i) {
    std::unique_ptr<Polygon> poly = shells[i]->getPolygon();
    ASSERT_NE(poly, nullptr);
    const std::size_t expected_holes = (i == 0 || i == 5 || i == 15) ? 1u : 0u;
    EXPECT_EQ(poly->getNumInteriorRing(), expected_holes);
  }
}

TEST(HoleAssignerTest, NestedShellsAssignHoleToInnermostEnclosingShell) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  RingBuilder builder(factory.get());

  EdgeRing* outer_shell = builder.CreateCwSquare(0.0, 0.0, 100.0, 100.0);
  EdgeRing* outer_hole = builder.CreateCcwSquare(0.0, 0.0, 100.0, 100.0);
  EdgeRing* middle_hole = builder.CreateCcwSquare(10.0, 10.0, 90.0, 90.0);
  EdgeRing* inner_shell = builder.CreateCwSquare(10.0, 10.0, 90.0, 90.0);
  EdgeRing* inner_hole = builder.CreateCcwSquare(20.0, 20.0, 80.0, 80.0);
  EdgeRing* island_shell = builder.CreateCwSquare(20.0, 20.0, 80.0, 80.0);

  // Place outer_shell before inner_shell and island_shell; inner_hole must
  // still bind to inner_shell (not outer_shell, and not island_shell with equal
  // box).
  std::vector<EdgeRing*> shells = {outer_shell, inner_shell, island_shell};
  std::vector<EdgeRing*> holes = {outer_hole, middle_hole, inner_hole};
  HoleAssigner::assignHolesToShells(holes, shells);

  EXPECT_FALSE(outer_hole->hasShell());
  EXPECT_TRUE(outer_hole->isOuterHole());
  EXPECT_EQ(middle_hole->getShell(), outer_shell);
  EXPECT_EQ(inner_hole->getShell(), inner_shell);

  std::unique_ptr<Polygon> p_outer = outer_shell->getPolygon();
  std::unique_ptr<Polygon> p_inner = inner_shell->getPolygon();
  std::unique_ptr<Polygon> p_island = island_shell->getPolygon();
  EXPECT_EQ(p_outer->getNumInteriorRing(), 1u);
  EXPECT_DOUBLE_EQ(p_outer->getArea(), 10000.0 - 6400.0);
  EXPECT_EQ(p_inner->getNumInteriorRing(), 1u);
  EXPECT_DOUBLE_EQ(p_inner->getArea(), 6400.0 - 3600.0);
  EXPECT_EQ(p_island->getNumInteriorRing(), 0u);
  EXPECT_DOUBLE_EQ(p_island->getArea(), 3600.0);
}

TEST(HoleAssignerTest,
     ConcaveShellEnvelopeContainmentDoesNotAssignOutsideHole) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  RingBuilder builder(factory.get());

  // C-shaped clockwise shell whose bounding box is [0,10]x[0,10], with a
  // concave opening on the right covering [2,10]x[2,8].
  EdgeRing* concave_shell = builder.CreateRing({
      Coordinate(0.0, 0.0),
      Coordinate(0.0, 10.0),
      Coordinate(10.0, 10.0),
      Coordinate(10.0, 8.0),
      Coordinate(2.0, 8.0),
      Coordinate(2.0, 2.0),
      Coordinate(10.0, 2.0),
      Coordinate(10.0, 0.0),
      Coordinate(0.0, 0.0),
  });
  EdgeRing* outside_hole = builder.CreateCcwSquare(4.0, 4.0, 8.0, 6.0);

  std::vector<EdgeRing*> shells = {concave_shell};
  std::vector<EdgeRing*> holes = {outside_hole};
  HoleAssigner::assignHolesToShells(holes, shells);

  EXPECT_FALSE(outside_hole->hasShell());
  EXPECT_TRUE(outside_hole->isOuterHole());
  std::unique_ptr<Polygon> poly = concave_shell->getPolygon();
  EXPECT_EQ(poly->getNumInteriorRing(), 0u);
}

TEST(HoleAssignerTest, HoleTouchingShellAtSingleVertexIsAssigned) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  RingBuilder builder(factory.get());

  EdgeRing* shell = builder.CreateCwSquare(0.0, 0.0, 10.0, 10.0);
  // CCW diamond hole touching shell boundary only at (5,0).
  EdgeRing* touching_hole = builder.CreateRing({
      Coordinate(5.0, 0.0),
      Coordinate(8.0, 5.0),
      Coordinate(5.0, 8.0),
      Coordinate(2.0, 5.0),
      Coordinate(5.0, 0.0),
  });

  std::vector<EdgeRing*> shells = {shell};
  std::vector<EdgeRing*> holes = {touching_hole};
  HoleAssigner::assignHolesToShells(holes, shells);

  EXPECT_TRUE(touching_hole->hasShell());
  EXPECT_EQ(touching_hole->getShell(), shell);
  std::unique_ptr<Polygon> poly = shell->getPolygon();
  EXPECT_EQ(poly->getNumInteriorRing(), 1u);
  EXPECT_TRUE(poly->isValid());
}

int InterruptAfterFirstFlagCheck(void* userdata) {
  bool* should_interrupt = static_cast<bool*>(userdata);
  return *should_interrupt ? 1 : 0;
}

TEST(HoleAssignerTest, RespectsCurrentThreadInterruptDuringAssignment) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  RingBuilder builder(factory.get());

  EdgeRing* shell = builder.CreateCwSquare(0.0, 0.0, 20.0, 20.0);
  EdgeRing* hole0 = builder.CreateCcwSquare(2.0, 2.0, 8.0, 8.0);
  EdgeRing* hole1 = builder.CreateCcwSquare(12.0, 12.0, 18.0, 18.0);
  std::vector<EdgeRing*> shells = {shell};
  std::vector<EdgeRing*> holes = {hole0, hole1};

  bool should_interrupt = false;
  CurrentThreadInterrupt::ThreadCallback* prev_cb =
      CurrentThreadInterrupt::registerCallback(&InterruptAfterFirstFlagCheck,
                                               &should_interrupt);
  const absl::Cleanup restore_cb = [prev_cb]() {
    CurrentThreadInterrupt::registerCallback(prev_cb, nullptr);
  };

  should_interrupt = true;
  EXPECT_THROW(HoleAssigner::assignHolesToShells(holes, shells), GEOSException);
  EXPECT_TRUE(hole0->hasShell());
  EXPECT_FALSE(hole1->hasShell());
}

}  // namespace
}  // namespace geos_google
