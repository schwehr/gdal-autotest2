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
// third_party/geos/include/geos/geomgraph/index/EdgeSetIntersector.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/geomgraph/index/EdgeSetIntersector.h

#include "third_party/geos/include/geos/geomgraph/index/EdgeSetIntersector.h"

#include <array>
#include <initializer_list>
#include <memory>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geomgraph/Edge.h"
#include "third_party/geos/include/geos/geomgraph/EdgeIntersectionList.h"
#include "third_party/geos/include/geos/geomgraph/index/SegmentIntersector.h"
#include "third_party/geos/include/geos/geomgraph/index/SimpleEdgeSetIntersector.h"
#include "third_party/geos/include/geos/geomgraph/index/SimpleMCSweepLineIntersector.h"

namespace geos_google {
namespace {

using geos::algorithm::LineIntersector;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geomgraph::Edge;
using geos::geomgraph::index::EdgeSetIntersector;
using geos::geomgraph::index::SegmentIntersector;
using geos::geomgraph::index::SimpleEdgeSetIntersector;
using geos::geomgraph::index::SimpleMCSweepLineIntersector;

static_assert(std::is_abstract_v<EdgeSetIntersector>);
static_assert(std::has_virtual_destructor_v<EdgeSetIntersector>);
static_assert(!std::is_constructible_v<EdgeSetIntersector>);

std::unique_ptr<Edge> MakeEdge(std::initializer_list<Coordinate> coords) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return std::make_unique<Edge>(seq.release());
}

class RecordingEdgeSetIntersector final : public EdgeSetIntersector {
 public:
  struct SingleSetCall {
    std::vector<Edge*>* edges;
    SegmentIntersector* si;
    bool test_all_segments;
  };

  struct TwoSetCall {
    std::vector<Edge*>* edges0;
    std::vector<Edge*>* edges1;
    SegmentIntersector* si;
  };

  void computeIntersections(std::vector<Edge*>* edges, SegmentIntersector* si,
                            bool testAllSegments) override {
    single_set_calls.push_back({edges, si, testAllSegments});
  }

  void computeIntersections(std::vector<Edge*>* edges0,
                            std::vector<Edge*>* edges1,
                            SegmentIntersector* si) override {
    two_set_calls.push_back({edges0, edges1, si});
  }

  std::vector<SingleSetCall> single_set_calls;
  std::vector<TwoSetCall> two_set_calls;
};

class DestructorTrackingIntersector : public EdgeSetIntersector {
 public:
  explicit DestructorTrackingIntersector(bool* destroyed)
      : destroyed_(destroyed) {}

  ~DestructorTrackingIntersector() override { *destroyed_ = true; }

  void computeIntersections(std::vector<Edge*>* /*edges*/,
                            SegmentIntersector* /*si*/,
                            bool /*testAllSegments*/) override {}

  void computeIntersections(std::vector<Edge*>* /*edges0*/,
                            std::vector<Edge*>* /*edges1*/,
                            SegmentIntersector* /*si*/) override {}

 private:
  bool* destroyed_;
};

enum class ConcreteKind {
  kSimpleEdgeSet,
  kSimpleMCSweepLine,
};

constexpr auto kConcreteKinds = std::to_array<ConcreteKind>({
    ConcreteKind::kSimpleEdgeSet,
    ConcreteKind::kSimpleMCSweepLine,
});

std::unique_ptr<EdgeSetIntersector> CreateIntersector(ConcreteKind kind) {
  switch (kind) {
    case ConcreteKind::kSimpleEdgeSet:
      return std::make_unique<SimpleEdgeSetIntersector>();
    case ConcreteKind::kSimpleMCSweepLine:
      return std::make_unique<SimpleMCSweepLineIntersector>();
  }
  return nullptr;
}

TEST(EdgeSetIntersectorTest, SupportsPolymorphicDestructionViaBasePointer) {
  bool destroyed = false;
  std::unique_ptr<EdgeSetIntersector> tracked =
      std::make_unique<DestructorTrackingIntersector>(&destroyed);
  EXPECT_FALSE(destroyed);
  tracked.reset();
  EXPECT_TRUE(destroyed);

  std::unique_ptr<EdgeSetIntersector> recording =
      std::make_unique<RecordingEdgeSetIntersector>();
  recording.reset();

  for (ConcreteKind kind : kConcreteKinds) {
    std::unique_ptr<EdgeSetIntersector> concrete = CreateIntersector(kind);
    EXPECT_NE(concrete, nullptr);
  }
}

TEST(EdgeSetIntersectorTest, PolymorphicDispatchForwardsArgumentsAndFlags) {
  RecordingEdgeSetIntersector impl;
  EdgeSetIntersector* esi = &impl;

  auto e0 = MakeEdge({{0.0, 0.0}, {1.0, 1.0}});
  auto e1 = MakeEdge({{0.0, 1.0}, {1.0, 0.0}});
  std::vector<Edge*> edges0 = {e0.get()};
  std::vector<Edge*> edges1 = {e1.get()};

  LineIntersector li;
  SegmentIntersector si(&li, /*newIncludeProper=*/true,
                        /*newRecordIsolated=*/false);

  esi->computeIntersections(&edges0, &si, /*testAllSegments=*/false);
  esi->computeIntersections(&edges0, &si, /*testAllSegments=*/true);
  esi->computeIntersections(&edges0, &edges1, &si);

  ASSERT_EQ(impl.single_set_calls.size(), 2u);
  EXPECT_EQ(impl.single_set_calls[0].edges, &edges0);
  EXPECT_EQ(impl.single_set_calls[0].si, &si);
  EXPECT_FALSE(impl.single_set_calls[0].test_all_segments);

  EXPECT_EQ(impl.single_set_calls[1].edges, &edges0);
  EXPECT_EQ(impl.single_set_calls[1].si, &si);
  EXPECT_TRUE(impl.single_set_calls[1].test_all_segments);

  ASSERT_EQ(impl.two_set_calls.size(), 1u);
  EXPECT_EQ(impl.two_set_calls[0].edges0, &edges0);
  EXPECT_EQ(impl.two_set_calls[0].edges1, &edges1);
  EXPECT_EQ(impl.two_set_calls[0].si, &si);
}

TEST(EdgeSetIntersectorTest, SingleSetEmptyAndDisjointEdgesFindNoIntersection) {
  for (ConcreteKind kind : kConcreteKinds) {
    SCOPED_TRACE(static_cast<int>(kind));

    // Empty edge list.
    {
      std::unique_ptr<EdgeSetIntersector> esi = CreateIntersector(kind);
      std::vector<Edge*> empty_edges;
      LineIntersector li;
      SegmentIntersector si(&li, true, false);
      esi->computeIntersections(&empty_edges, &si, true);
      EXPECT_FALSE(si.hasIntersection());
      EXPECT_EQ(si.numTests, 0);
    }

    // Disjoint parallel edges.
    {
      std::unique_ptr<EdgeSetIntersector> esi = CreateIntersector(kind);
      auto e0 = MakeEdge({{0.0, 0.0}, {4.0, 0.0}});
      auto e1 = MakeEdge({{0.0, 2.0}, {4.0, 2.0}});
      std::vector<Edge*> edges = {e0.get(), e1.get()};

      LineIntersector li;
      SegmentIntersector si(&li, true, false);
      esi->computeIntersections(&edges, &si, false);
      EXPECT_FALSE(si.hasIntersection());
      EXPECT_FALSE(si.hasProperIntersection());
      EXPECT_TRUE(e0->getEdgeIntersectionList().isEmpty());
      EXPECT_TRUE(e1->getEdgeIntersectionList().isEmpty());
    }
  }
}

TEST(EdgeSetIntersectorTest, SingleSetCrossingEdgesFindProperIntersection) {
  for (ConcreteKind kind : kConcreteKinds) {
    SCOPED_TRACE(static_cast<int>(kind));
    std::unique_ptr<EdgeSetIntersector> esi = CreateIntersector(kind);

    auto e0 = MakeEdge({{0.0, 0.0}, {4.0, 4.0}});
    auto e1 = MakeEdge({{0.0, 4.0}, {4.0, 0.0}});
    std::vector<Edge*> edges = {e0.get(), e1.get()};

    LineIntersector li;
    SegmentIntersector si(&li, /*newIncludeProper=*/true,
                          /*newRecordIsolated=*/false);
    esi->computeIntersections(&edges, &si, /*testAllSegments=*/false);

    EXPECT_TRUE(si.hasIntersection());
    EXPECT_TRUE(si.hasProperIntersection());
    EXPECT_TRUE(si.hasProperInteriorIntersection());
    EXPECT_TRUE(si.getProperIntersectionPoint().equals2D(Coordinate(2.0, 2.0)));
    EXPECT_TRUE(
        e0->getEdgeIntersectionList().isIntersection(Coordinate(2.0, 2.0)));
    EXPECT_TRUE(
        e1->getEdgeIntersectionList().isIntersection(Coordinate(2.0, 2.0)));
  }
}

TEST(EdgeSetIntersectorTest, SingleSetSelfCrossingRespectsTestAllSegmentsFlag) {
  for (ConcreteKind kind : kConcreteKinds) {
    SCOPED_TRACE(static_cast<int>(kind));

    // Self-crossing bow-tie edge: segment 0 (0,0)-(4,4) crosses segment 2
    // (4,0)-(0,4).
    {
      auto edge = MakeEdge({{0.0, 0.0}, {4.0, 4.0}, {4.0, 0.0}, {0.0, 4.0}});
      std::vector<Edge*> edges = {edge.get()};

      LineIntersector li_false;
      SegmentIntersector si_false(&li_false, true, false);
      CreateIntersector(kind)->computeIntersections(&edges, &si_false, false);
      EXPECT_FALSE(si_false.hasIntersection());
      EXPECT_TRUE(edge->getEdgeIntersectionList().isEmpty());

      LineIntersector li_true;
      SegmentIntersector si_true(&li_true, true, false);
      CreateIntersector(kind)->computeIntersections(&edges, &si_true, true);
      EXPECT_TRUE(si_true.hasIntersection());
      EXPECT_TRUE(si_true.hasProperIntersection());
      EXPECT_TRUE(
          si_true.getProperIntersectionPoint().equals2D(Coordinate(2.0, 2.0)));
      EXPECT_TRUE(
          edge->getEdgeIntersectionList().isIntersection(Coordinate(2.0, 2.0)));
    }

    // Simple V-shape edge has only trivial shared endpoint at (2,2).
    {
      auto edge = MakeEdge({{0.0, 0.0}, {2.0, 2.0}, {4.0, 0.0}});
      std::vector<Edge*> edges = {edge.get()};

      LineIntersector li;
      SegmentIntersector si(&li, true, false);
      CreateIntersector(kind)->computeIntersections(&edges, &si, true);
      EXPECT_FALSE(si.hasIntersection());
      EXPECT_TRUE(edge->getEdgeIntersectionList().isEmpty());
    }
  }
}

TEST(EdgeSetIntersectorTest, TwoSetComputesOnlyMutualIntersections) {
  for (ConcreteKind kind : kConcreteKinds) {
    SCOPED_TRACE(static_cast<int>(kind));

    // Edges within set 0 cross each other at (1, 1); set 1 is disjoint.
    {
      auto a0 = MakeEdge({{0.0, 0.0}, {2.0, 2.0}});
      auto a1 = MakeEdge({{0.0, 2.0}, {2.0, 0.0}});
      auto b0 = MakeEdge({{10.0, 0.0}, {10.0, 5.0}});
      std::vector<Edge*> edges0 = {a0.get(), a1.get()};
      std::vector<Edge*> edges1 = {b0.get()};

      LineIntersector li;
      SegmentIntersector si(&li, true, false);
      CreateIntersector(kind)->computeIntersections(&edges0, &edges1, &si);
      EXPECT_FALSE(si.hasIntersection());
      EXPECT_TRUE(a0->getEdgeIntersectionList().isEmpty());
      EXPECT_TRUE(a1->getEdgeIntersectionList().isEmpty());
      EXPECT_TRUE(b0->getEdgeIntersectionList().isEmpty());
    }

    // Set 1 crosses both edges of set 0, while intra-set crossing at (1, 1) is
    // ignored.
    {
      auto a0 = MakeEdge({{0.0, 0.0}, {2.0, 2.0}});
      auto a1 = MakeEdge({{0.0, 2.0}, {2.0, 0.0}});
      auto b0 = MakeEdge({{1.5, 0.0}, {1.5, 3.0}});
      std::vector<Edge*> edges0 = {a0.get(), a1.get()};
      std::vector<Edge*> edges1 = {b0.get()};

      LineIntersector li;
      SegmentIntersector si(&li, true, false);
      CreateIntersector(kind)->computeIntersections(&edges0, &edges1, &si);

      EXPECT_TRUE(si.hasIntersection());
      EXPECT_TRUE(si.hasProperIntersection());
      EXPECT_FALSE(
          a0->getEdgeIntersectionList().isIntersection(Coordinate(1.0, 1.0)));
      EXPECT_TRUE(
          a0->getEdgeIntersectionList().isIntersection(Coordinate(1.5, 1.5)));
      EXPECT_TRUE(
          a1->getEdgeIntersectionList().isIntersection(Coordinate(1.5, 0.5)));
      EXPECT_TRUE(
          b0->getEdgeIntersectionList().isIntersection(Coordinate(1.5, 1.5)));
      EXPECT_TRUE(
          b0->getEdgeIntersectionList().isIntersection(Coordinate(1.5, 0.5)));
    }

    // Empty second set skips intersections.
    {
      auto a0 = MakeEdge({{0.0, 0.0}, {2.0, 2.0}});
      auto a1 = MakeEdge({{0.0, 2.0}, {2.0, 0.0}});
      std::vector<Edge*> edges0 = {a0.get(), a1.get()};
      std::vector<Edge*> empty_edges1;

      LineIntersector li;
      SegmentIntersector si(&li, true, false);
      CreateIntersector(kind)->computeIntersections(&edges0, &empty_edges1,
                                                    &si);
      EXPECT_FALSE(si.hasIntersection());
    }
  }
}

}  // namespace
}  // namespace geos_google
