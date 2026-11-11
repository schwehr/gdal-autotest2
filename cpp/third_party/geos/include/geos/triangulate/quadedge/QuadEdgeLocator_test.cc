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
// third_party/geos/include/geos/triangulate/quadedge/QuadEdgeLocator.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/triangulate/quadedge/QuadEdgeLocator.h

#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdgeLocator.h"

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/triangulate/IncrementalDelaunayTriangulator.h"
#include "third_party/geos/include/geos/triangulate/quadedge/LastFoundQuadEdgeLocator.h"
#include "third_party/geos/include/geos/triangulate/quadedge/LocateFailureException.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdge.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdgeQuartet.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdgeSubdivision.h"
#include "third_party/geos/include/geos/triangulate/quadedge/Vertex.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::Envelope;
using geos::triangulate::IncrementalDelaunayTriangulator;
using geos::triangulate::quadedge::LastFoundQuadEdgeLocator;
using geos::triangulate::quadedge::LocateFailureException;
using geos::triangulate::quadedge::QuadEdge;
using geos::triangulate::quadedge::QuadEdgeLocator;
using geos::triangulate::quadedge::QuadEdgeSubdivision;
using geos::triangulate::quadedge::Vertex;

static_assert(std::is_abstract_v<QuadEdgeLocator>);
static_assert(std::is_polymorphic_v<QuadEdgeLocator>);
static_assert(std::has_virtual_destructor_v<QuadEdgeLocator>);
static_assert(!std::is_default_constructible_v<QuadEdgeLocator>);

// Custom QuadEdgeLocator that searches live edges in a subdivision for an
// edge incident on the query vertex or framing a triangle that contains it.
class RecordingScanLocator final : public QuadEdgeLocator {
 public:
  RecordingScanLocator(QuadEdgeSubdivision* subdiv,
                       std::vector<Vertex>* queries, bool* destroyed)
      : subdiv_(subdiv), queries_(queries), destroyed_(destroyed) {}

  ~RecordingScanLocator() override {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
  }

  QuadEdge* locate(const Vertex& v) override {
    if (queries_ != nullptr) {
      queries_->push_back(v);
    }
    if (subdiv_ == nullptr || subdiv_->getEdges().empty()) {
      return nullptr;
    }
    for (auto& quartet : subdiv_->getEdges()) {
      QuadEdge* base = &quartet.base();
      if (!base->isLive()) {
        continue;
      }
      if (v.equals(base->orig()) || v.equals(base->dest())) {
        return base;
      }
    }
    return subdiv_->locateFromEdge(v, subdiv_->getEdges().front().base());
  }

 private:
  QuadEdgeSubdivision* subdiv_;
  std::vector<Vertex>* queries_;
  bool* destroyed_;
};

// Custom QuadEdgeLocator that always reports no edge found.
class NullLocator final : public QuadEdgeLocator {
 public:
  explicit NullLocator(int* call_count = nullptr, bool* destroyed = nullptr)
      : call_count_(call_count), destroyed_(destroyed) {}

  ~NullLocator() override {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
  }

  QuadEdge* locate(const Vertex&) override {
    if (call_count_ != nullptr) {
      ++(*call_count_);
    }
    return nullptr;
  }

 private:
  int* call_count_;
  bool* destroyed_;
};

TEST(QuadEdgeLocatorTest, DirectAndPolymorphicDestruction) {
  bool stack_destroyed = false;
  {
    int calls = 0;
    NullLocator stack_locator(&calls, &stack_destroyed);
    QuadEdgeLocator& iface = stack_locator;
    EXPECT_EQ(iface.locate(Vertex(1.0, 2.0)), nullptr);
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(stack_destroyed);
  }
  EXPECT_TRUE(stack_destroyed);

  bool heap_destroyed = false;
  int heap_calls = 0;
  std::unique_ptr<QuadEdgeLocator> heap_locator =
      std::make_unique<NullLocator>(&heap_calls, &heap_destroyed);
  EXPECT_EQ(heap_locator->locate(Vertex(3.0, 4.0)), nullptr);
  EXPECT_EQ(heap_calls, 1);
  EXPECT_FALSE(heap_destroyed);

  heap_locator.reset();
  EXPECT_TRUE(heap_destroyed);
}

TEST(QuadEdgeLocatorTest, PolymorphicLocateFindsVerticesAndNullWhenEmpty) {
  std::vector<Vertex> empty_queries;
  bool empty_destroyed = false;
  std::unique_ptr<QuadEdgeLocator> empty_locator =
      std::make_unique<RecordingScanLocator>(nullptr, &empty_queries,
                                             &empty_destroyed);
  EXPECT_EQ(empty_locator->locate(Vertex(0.0, 0.0)), nullptr);
  ASSERT_EQ(empty_queries.size(), 1u);
  EXPECT_TRUE(empty_queries[0].equals(Vertex(0.0, 0.0)));
  empty_locator.reset();
  EXPECT_TRUE(empty_destroyed);

  QuadEdgeSubdivision subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 0.0);
  const Vertex site(5.0, 5.0);
  subdiv.insertSite(site);

  std::vector<Vertex> queries;
  bool destroyed = false;
  std::unique_ptr<QuadEdgeLocator> locator =
      std::make_unique<RecordingScanLocator>(&subdiv, &queries, &destroyed);

  QuadEdge* found_site = locator->locate(site);
  ASSERT_NE(found_site, nullptr);
  EXPECT_TRUE(found_site->isLive());
  EXPECT_TRUE(site.equals(found_site->orig()) ||
              site.equals(found_site->dest()));

  const Vertex interior(3.0, 4.0);
  QuadEdge* found_interior = locator->locate(interior);
  ASSERT_NE(found_interior, nullptr);
  EXPECT_TRUE(found_interior->isLive());

  ASSERT_EQ(queries.size(), 2u);
  EXPECT_TRUE(queries[0].equals(site));
  EXPECT_TRUE(queries[1].equals(interior));

  locator.reset();
  EXPECT_TRUE(destroyed);
}

TEST(QuadEdgeLocatorTest, IntegratesWithQuadEdgeSubdivisionSetLocator) {
  std::vector<Vertex> queries;
  bool custom_destroyed = false;
  bool null_destroyed = false;

  {
    QuadEdgeSubdivision subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 1e-6);
    subdiv.setLocator(std::make_unique<RecordingScanLocator>(
        &subdiv, &queries, &custom_destroyed));

    IncrementalDelaunayTriangulator triangulator(&subdiv);
    const IncrementalDelaunayTriangulator::VertexList sites = {
        Vertex(0.0, 0.0),
        Vertex(10.0, 0.0),
        Vertex(10.0, 10.0),
        Vertex(0.0, 10.0),
    };
    triangulator.insertSites(sites);

    const std::size_t queries_after_insert = queries.size();
    EXPECT_GE(queries_after_insert, sites.size());

    // Subdivision locate(Vertex), locate(Coordinate), and locate(p0, p1)
    // must all dispatch through the installed QuadEdgeLocator.
    QuadEdge* by_vertex = subdiv.locate(Vertex(10.0, 10.0));
    ASSERT_NE(by_vertex, nullptr);
    EXPECT_TRUE(Vertex(10.0, 10.0).equals(by_vertex->orig()) ||
                Vertex(10.0, 10.0).equals(by_vertex->dest()));

    QuadEdge* by_coord = subdiv.locate(Coordinate(2.0, 2.0));
    ASSERT_NE(by_coord, nullptr);
    EXPECT_TRUE(by_coord->isLive());

    QuadEdge* by_pair =
        subdiv.locate(Coordinate(0.0, 0.0), Coordinate(10.0, 0.0));
    ASSERT_NE(by_pair, nullptr);
    EXPECT_TRUE(by_pair->orig().equals(Vertex(0.0, 0.0)));
    EXPECT_TRUE(by_pair->dest().equals(Vertex(10.0, 0.0)));

    EXPECT_EQ(queries.size(), queries_after_insert + 3u);

    // Replacing the locator must destroy the previously installed locator.
    int null_calls = 0;
    subdiv.setLocator(
        std::make_unique<NullLocator>(&null_calls, &null_destroyed));
    EXPECT_TRUE(custom_destroyed);
    EXPECT_FALSE(null_destroyed);

    EXPECT_EQ(subdiv.locate(Vertex(0.0, 0.0)), nullptr);
    EXPECT_EQ(subdiv.locate(Coordinate(5.0, 5.0)), nullptr);
    EXPECT_EQ(subdiv.locate(Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)),
              nullptr);
    EXPECT_THROW(triangulator.insertSite(Vertex(5.0, 5.0)),
                 LocateFailureException);
    EXPECT_EQ(null_calls, 4);
  }

  // Destroying the subdivision must destroy the currently installed locator.
  EXPECT_TRUE(null_destroyed);
}

TEST(QuadEdgeLocatorTest, PolymorphicLastFoundQuadEdgeLocator) {
  QuadEdgeSubdivision subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 0.0);
  const Vertex site(4.0, 6.0);
  subdiv.insertSite(site);

  std::unique_ptr<QuadEdgeLocator> locator =
      std::make_unique<LastFoundQuadEdgeLocator>(&subdiv);

  QuadEdge* edge = locator->locate(site);
  ASSERT_NE(edge, nullptr);
  EXPECT_TRUE(edge->isLive());
  EXPECT_TRUE(site.equals(edge->orig()) || site.equals(edge->dest()));

  EXPECT_THROW(locator->locate(Vertex(-5000.0, -5000.0)),
               LocateFailureException);
}

}  // namespace
}  // namespace geos_google
