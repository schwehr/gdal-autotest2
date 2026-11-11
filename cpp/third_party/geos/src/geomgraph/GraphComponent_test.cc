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

// Unit tests for third_party/geos/src/geomgraph/GraphComponent.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geomgraph/GraphComponent.cpp

#include "third_party/geos/include/geos/geomgraph/GraphComponent.h"

#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/IntersectionMatrix.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/geomgraph/DirectedEdgeStar.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/geomgraph/Node.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Dimension;
using geos::geom::IntersectionMatrix;
using geos::geom::Location;
using geos::geom::Position;
using geos::geomgraph::DirectedEdgeStar;
using geos::geomgraph::Edge;
using geos::geomgraph::GraphComponent;
using geos::geomgraph::Label;
using geos::geomgraph::Node;

class DummyGraphComponent final : public GraphComponent {
 public:
  using GraphComponent::GraphComponent;

  bool isIsolated() const override { return isolated_; }
  void setIsolated(bool isolated) { isolated_ = isolated; }

  int computeImCalls() const { return compute_im_calls_; }
  int lastDim() const { return last_dim_; }

 protected:
  void computeIM(IntersectionMatrix& im) override {
    ++compute_im_calls_;
    im.set(label.getLocation(0), label.getLocation(1), last_dim_);
  }

 private:
  bool isolated_ = false;
  int compute_im_calls_ = 0;
  int last_dim_ = Dimension::P;
};

TEST(GraphComponentTest, DefaultConstructorInitializesDefaults) {
  DummyGraphComponent comp;
  const GraphComponent& const_comp = comp;

  EXPECT_TRUE(const_comp.getLabel().isNull());
  EXPECT_EQ(const_comp.getLabel().getGeometryCount(), 0);
  EXPECT_FALSE(const_comp.isInResult());
  EXPECT_FALSE(const_comp.isCovered());
  EXPECT_FALSE(const_comp.isCoveredSet());
  EXPECT_FALSE(const_comp.isVisited());
  EXPECT_FALSE(const_comp.isIsolated());
  EXPECT_EQ(comp.computeImCalls(), 0);
}

TEST(GraphComponentTest, LabelConstructorCopiesLabelAndInitializesFlags) {
  Label lbl(Location::INTERIOR);
  lbl.setLocation(1, Location::BOUNDARY);

  DummyGraphComponent comp(lbl);
  lbl.setLocation(0, Location::EXTERIOR);

  EXPECT_FALSE(comp.getLabel().isNull());
  EXPECT_EQ(comp.getLabel().getGeometryCount(), 2);
  EXPECT_EQ(comp.getLabel().getLocation(0), Location::INTERIOR);
  EXPECT_EQ(comp.getLabel().getLocation(1), Location::BOUNDARY);
  EXPECT_FALSE(comp.isInResult());
  EXPECT_FALSE(comp.isCovered());
  EXPECT_FALSE(comp.isCoveredSet());
  EXPECT_FALSE(comp.isVisited());
}

TEST(GraphComponentTest, LabelAccessorsAndSetLabelUpdateLabel) {
  DummyGraphComponent comp;
  const GraphComponent& const_comp = comp;

  Label initial(0, Location::BOUNDARY);
  comp.setLabel(initial);
  EXPECT_EQ(const_comp.getLabel().getGeometryCount(), 1);
  EXPECT_EQ(const_comp.getLabel().getLocation(0), Location::BOUNDARY);
  EXPECT_EQ(const_comp.getLabel().getLocation(1), Location::NONE);

  comp.getLabel().setLocation(1, Location::INTERIOR);
  EXPECT_EQ(const_comp.getLabel().getGeometryCount(), 2);
  EXPECT_EQ(const_comp.getLabel().getLocation(1), Location::INTERIOR);
}

TEST(GraphComponentTest, InResultAndVisitedFlagsToggleIndependently) {
  DummyGraphComponent comp;

  comp.setInResult(true);
  EXPECT_TRUE(comp.isInResult());
  EXPECT_FALSE(comp.isVisited());
  EXPECT_FALSE(comp.isCovered());
  EXPECT_FALSE(comp.isCoveredSet());

  comp.setVisited(true);
  EXPECT_TRUE(comp.isVisited());
  EXPECT_TRUE(comp.isInResult());

  comp.setInResult(false);
  EXPECT_FALSE(comp.isInResult());
  EXPECT_TRUE(comp.isVisited());

  comp.setVisited(false);
  EXPECT_FALSE(comp.isVisited());
}

TEST(GraphComponentTest, SetCoveredMarksCoveredSetEvenWhenFalse) {
  DummyGraphComponent comp;
  EXPECT_FALSE(comp.isCovered());
  EXPECT_FALSE(comp.isCoveredSet());

  comp.setCovered(false);
  EXPECT_FALSE(comp.isCovered());
  EXPECT_TRUE(comp.isCoveredSet());

  comp.setCovered(true);
  EXPECT_TRUE(comp.isCovered());
  EXPECT_TRUE(comp.isCoveredSet());

  comp.setCovered(false);
  EXPECT_FALSE(comp.isCovered());
  EXPECT_TRUE(comp.isCoveredSet());
}

TEST(GraphComponentTest, UpdateImInvokesComputeImForTwoGeometryLabel) {
  Label lbl(Location::INTERIOR);
  lbl.setLocation(1, Location::BOUNDARY);

  DummyGraphComponent comp(lbl);
  IntersectionMatrix im;
  comp.updateIM(im);

  EXPECT_EQ(comp.computeImCalls(), 1);
  EXPECT_EQ(im.get(Location::INTERIOR, Location::BOUNDARY), +Dimension::P);
  EXPECT_EQ(im.get(Location::INTERIOR, Location::INTERIOR), +Dimension::False);

  comp.setLabel(Label(Location::BOUNDARY));
  comp.updateIM(im);
  EXPECT_EQ(comp.computeImCalls(), 2);
  EXPECT_EQ(im.get(Location::BOUNDARY, Location::BOUNDARY), +Dimension::P);
}

TEST(GraphComponentTest, PolymorphicEdgeAndNodeLifecycleAndUpdateIm) {
  auto edge_pts = std::make_unique<CoordinateSequence>();
  edge_pts->add(Coordinate(0.0, 0.0));
  edge_pts->add(Coordinate(1.0, 1.0));
  Label area_label(Location::BOUNDARY, Location::INTERIOR, Location::EXTERIOR);

  std::unique_ptr<GraphComponent> edge =
      std::make_unique<Edge>(edge_pts.release(), area_label);
  EXPECT_TRUE(edge->isIsolated());
  edge->setVisited(true);
  edge->setInResult(true);
  edge->setCovered(true);
  EXPECT_TRUE(edge->isVisited());
  EXPECT_TRUE(edge->isInResult());
  EXPECT_TRUE(edge->isCovered());
  EXPECT_TRUE(edge->isCoveredSet());

  IntersectionMatrix im;
  edge->updateIM(im);
  EXPECT_EQ(im.get(Location::BOUNDARY, Location::BOUNDARY), +Dimension::L);
  EXPECT_EQ(im.get(Location::INTERIOR, Location::INTERIOR), +Dimension::A);
  EXPECT_EQ(im.get(Location::EXTERIOR, Location::EXTERIOR), +Dimension::A);

  auto default_pts = std::make_unique<CoordinateSequence>();
  default_pts->add(Coordinate(0.0, 0.0));
  default_pts->add(Coordinate(2.0, 0.0));
  std::unique_ptr<GraphComponent> default_edge =
      std::make_unique<Edge>(default_pts.release());
  EXPECT_TRUE(default_edge->getLabel().isNull());
  EXPECT_FALSE(default_edge->isCoveredSet());

  auto star = std::make_unique<DirectedEdgeStar>();
  std::unique_ptr<GraphComponent> node =
      std::make_unique<Node>(Coordinate(1.0, 2.0), star.release());
  EXPECT_TRUE(node->getLabel().isNull());
  node->setLabel(Label(Location::INTERIOR));
  EXPECT_FALSE(node->isIsolated());
  IntersectionMatrix node_im;
  node->updateIM(node_im);
  EXPECT_EQ(node_im.get(Location::INTERIOR, Location::INTERIOR),
            +Dimension::False);
  EXPECT_EQ(edge->getLabel().getLocation(0, Position::LEFT),
            Location::INTERIOR);
}

}  // namespace
}  // namespace geos_google
