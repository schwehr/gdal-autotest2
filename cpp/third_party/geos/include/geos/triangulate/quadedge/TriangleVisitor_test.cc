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
// third_party/geos/include/geos/triangulate/quadedge/TriangleVisitor.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/triangulate/quadedge/TriangleVisitor.h

#include "third_party/geos/include/geos/triangulate/quadedge/TriangleVisitor.h"

#include <array>
#include <cstddef>
#include <deque>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/triangulate/IncrementalDelaunayTriangulator.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdge.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdgeQuartet.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdgeSubdivision.h"
#include "third_party/geos/include/geos/triangulate/quadedge/Vertex.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::Envelope;
using geos::triangulate::IncrementalDelaunayTriangulator;
using geos::triangulate::quadedge::QuadEdge;
using geos::triangulate::quadedge::QuadEdgeQuartet;
using geos::triangulate::quadedge::QuadEdgeSubdivision;
using geos::triangulate::quadedge::TriangleVisitor;
using geos::triangulate::quadedge::Vertex;

static_assert(std::is_abstract_v<TriangleVisitor>);
static_assert(std::is_polymorphic_v<TriangleVisitor>);
static_assert(std::has_virtual_destructor_v<TriangleVisitor>);
static_assert(!std::is_default_constructible_v<TriangleVisitor>);

struct VisitedTriangle {
  std::array<Coordinate, 3> vertices;
  bool forms_closed_left_cycle = false;
  bool all_edges_live = false;
  bool is_ccw = false;
};

class RecordingTriangleVisitor final : public TriangleVisitor {
 public:
  explicit RecordingTriangleVisitor(std::vector<VisitedTriangle>* out,
                                    bool* destroyed = nullptr)
      : out_(out), destroyed_(destroyed) {}

  ~RecordingTriangleVisitor() override {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
  }

  void visit(std::array<QuadEdge*, 3>& triEdges) override {
    VisitedTriangle record;
    record.forms_closed_left_cycle =
        triEdges[0] != nullptr && triEdges[1] != nullptr &&
        triEdges[2] != nullptr && &triEdges[0]->lNext() == triEdges[1] &&
        &triEdges[1]->lNext() == triEdges[2] &&
        &triEdges[2]->lNext() == triEdges[0];
    record.all_edges_live = triEdges[0] != nullptr && triEdges[1] != nullptr &&
                            triEdges[2] != nullptr && triEdges[0]->isLive() &&
                            triEdges[1]->isLive() && triEdges[2]->isLive();

    for (std::size_t i = 0; i < 3; ++i) {
      record.vertices[i] = triEdges[i]->orig().getCoordinate();
    }
    record.is_ccw =
        triEdges[0]->orig().isCCW(triEdges[1]->orig(), triEdges[2]->orig());
    out_->push_back(record);
  }

 private:
  std::vector<VisitedTriangle>* out_;
  bool* destroyed_;
};

// Visitor that exercises non-const access to both the array parameter and the
// QuadEdge instances inside it.
class MutatingTriangleVisitor final : public TriangleVisitor {
 public:
  void visit(std::array<QuadEdge*, 3>& triEdges) override {
    for (QuadEdge* edge : triEdges) {
      edge->rot().setOrig(Vertex(42.0, 24.0));
    }
    std::swap(triEdges[0], triEdges[2]);
  }
};

TEST(TriangleVisitorTest, DirectAndPolymorphicDestructionAndVisit) {
  std::deque<QuadEdgeQuartet> edges;
  const Vertex v0(0.0, 0.0);
  const Vertex v1(10.0, 0.0);
  const Vertex v2(5.0, 10.0);

  QuadEdge* e0 = QuadEdge::makeEdge(v0, v1, edges);
  QuadEdge* e1 = QuadEdge::makeEdge(v1, v2, edges);
  QuadEdge::splice(e0->sym(), *e1);
  QuadEdge* e2 = QuadEdge::connect(*e1, *e0, edges);
  std::array<QuadEdge*, 3> tri_edges = {e0, e1, e2};

  std::vector<VisitedTriangle> recorded;
  bool stack_destroyed = false;
  {
    RecordingTriangleVisitor stack_visitor(&recorded, &stack_destroyed);
    TriangleVisitor& iface = stack_visitor;
    iface.visit(tri_edges);
    ASSERT_EQ(recorded.size(), 1u);
    EXPECT_TRUE(recorded[0].forms_closed_left_cycle);
    EXPECT_TRUE(recorded[0].all_edges_live);
    EXPECT_TRUE(recorded[0].is_ccw);
    EXPECT_TRUE(recorded[0].vertices[0].equals2D(v0.getCoordinate()));
    EXPECT_TRUE(recorded[0].vertices[1].equals2D(v1.getCoordinate()));
    EXPECT_TRUE(recorded[0].vertices[2].equals2D(v2.getCoordinate()));
    EXPECT_FALSE(stack_destroyed);
  }
  EXPECT_TRUE(stack_destroyed);

  bool heap_destroyed = false;
  std::unique_ptr<TriangleVisitor> heap_visitor =
      std::make_unique<RecordingTriangleVisitor>(&recorded, &heap_destroyed);
  heap_visitor->visit(tri_edges);
  ASSERT_EQ(recorded.size(), 2u);
  EXPECT_FALSE(heap_destroyed);

  heap_visitor.reset();
  EXPECT_TRUE(heap_destroyed);
}

TEST(TriangleVisitorTest, MutatingVisitorUpdatesEdgesAndArrayReference) {
  std::deque<QuadEdgeQuartet> edges;
  QuadEdge* e0 = QuadEdge::makeEdge(Vertex(0.0, 0.0), Vertex(1.0, 0.0), edges);
  QuadEdge* e1 = QuadEdge::makeEdge(Vertex(1.0, 0.0), Vertex(0.0, 1.0), edges);
  QuadEdge::splice(e0->sym(), *e1);
  QuadEdge* e2 = QuadEdge::connect(*e1, *e0, edges);
  std::array<QuadEdge*, 3> tri_edges = {e0, e1, e2};

  MutatingTriangleVisitor mutator;
  TriangleVisitor* visitor = &mutator;
  visitor->visit(tri_edges);

  EXPECT_EQ(tri_edges[0], e2);
  EXPECT_EQ(tri_edges[1], e1);
  EXPECT_EQ(tri_edges[2], e0);
  for (const QuadEdge* edge : {e0, e1, e2}) {
    EXPECT_TRUE(edge->rot().orig().equals(Vertex(42.0, 24.0)));
  }
}

TEST(TriangleVisitorTest, VisitsSubdivisionTrianglesWithAndWithoutFrame) {
  QuadEdgeSubdivision empty_subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 0.0);

  std::vector<VisitedTriangle> empty_no_frame;
  RecordingTriangleVisitor no_frame_visitor(&empty_no_frame);
  empty_subdiv.visitTriangles(&no_frame_visitor, false);
  EXPECT_TRUE(empty_no_frame.empty());

  std::vector<VisitedTriangle> empty_with_frame;
  RecordingTriangleVisitor with_frame_visitor(&empty_with_frame);
  empty_subdiv.visitTriangles(&with_frame_visitor, true);
  EXPECT_EQ(empty_with_frame.size(), 2u);

  // Triangulate a single interior triangle (3 sites) and verify visitor counts
  // and edge invariants with includeFrame = false vs true.
  QuadEdgeSubdivision subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 1e-6);
  IncrementalDelaunayTriangulator triangulator(&subdiv);
  triangulator.insertSites({
      Vertex(0.0, 0.0),
      Vertex(10.0, 0.0),
      Vertex(5.0, 10.0),
  });

  std::vector<VisitedTriangle> interior_tris;
  RecordingTriangleVisitor interior_visitor(&interior_tris);
  subdiv.visitTriangles(&interior_visitor, false);
  ASSERT_EQ(interior_tris.size(), 1u);
  EXPECT_TRUE(interior_tris[0].forms_closed_left_cycle);
  EXPECT_TRUE(interior_tris[0].all_edges_live);
  EXPECT_TRUE(interior_tris[0].is_ccw);
  for (const Coordinate& c : interior_tris[0].vertices) {
    EXPECT_FALSE(subdiv.isFrameVertex(Vertex(c)));
  }

  std::vector<VisitedTriangle> all_tris;
  RecordingTriangleVisitor all_visitor(&all_tris);
  subdiv.visitTriangles(&all_visitor, true);
  EXPECT_GT(all_tris.size(), interior_tris.size());

  std::size_t frame_tri_count = 0;
  for (const VisitedTriangle& tri : all_tris) {
    EXPECT_TRUE(tri.forms_closed_left_cycle);
    EXPECT_TRUE(tri.all_edges_live);
    for (const Coordinate& c : tri.vertices) {
      if (subdiv.isFrameVertex(Vertex(c))) {
        ++frame_tri_count;
        break;
      }
    }
  }
  EXPECT_GT(frame_tri_count, 0u);
}

}  // namespace
}  // namespace geos_google
