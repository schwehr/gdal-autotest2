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

// Unit tests for third_party/geos/src/coverage/TPVWSimplifier.cpp and
// third_party/geos/include/geos/coverage/TPVWSimplifier.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/coverage/TPVWSimplifier.cpp

#include "third_party/geos/include/geos/coverage/TPVWSimplifier.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/io/WKTWriter.h"

namespace geos_google {
namespace {

using geos::coverage::TPVWSimplifier;
using geos::geom::Coordinate;
using geos::geom::Envelope;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::geom::MultiLineString;
using geos::io::WKTReader;
using geos::io::WKTWriter;

void ExpectMultiLineEqualsWkt(const WKTReader& reader,
                              const MultiLineString* actual,
                              const std::string& expected_wkt) {
  ASSERT_NE(actual, nullptr);
  std::unique_ptr<MultiLineString> expected =
      reader.read<MultiLineString>(expected_wkt);
  WKTWriter writer;
  EXPECT_TRUE(expected->equalsExact(actual))
      << "Actual:   " << writer.write(actual) << "\nExpected: " << expected_wkt;
}

TEST(TPVWSimplifierTest, EmptyMultiLineStringReturnsEmpty) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  auto empty_lines = reader.read<MultiLineString>("MULTILINESTRING EMPTY");
  auto result = TPVWSimplifier::simplify(empty_lines.get(), 1.0);
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isEmpty());
  EXPECT_EQ(result->getNumGeometries(), 0u);

  std::vector<bool> free_rings;
  auto empty_constraints =
      reader.read<MultiLineString>("MULTILINESTRING EMPTY");
  auto constrained_result = TPVWSimplifier::simplify(
      empty_lines.get(), free_rings, empty_constraints.get(), 1.0);
  ASSERT_NE(constrained_result, nullptr);
  EXPECT_TRUE(constrained_result->isEmpty());
}

TEST(TPVWSimplifierTest, TwoPointLinePreservedAtLargeTolerance) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  auto lines = reader.read<MultiLineString>("MULTILINESTRING ((0 0, 10 0))");
  auto result = TPVWSimplifier::simplify(lines.get(), 100.0);
  ExpectMultiLineEqualsWkt(reader, result.get(),
                           "MULTILINESTRING ((0 0, 10 0))");
}

TEST(TPVWSimplifierTest, ZeroToleranceRemovesOnlyCollinearVertices) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  auto lines = reader.read<MultiLineString>(
      "MULTILINESTRING ((0 0, 2 0, 5 0, 8 0, 10 0), (0 1, 5 2, 10 1))");
  auto result = TPVWSimplifier::simplify(lines.get(), 0.0);
  ExpectMultiLineEqualsWkt(reader, result.get(),
                           "MULTILINESTRING ((0 0, 10 0), (0 1, 5 2, 10 1))");
}

TEST(TPVWSimplifierTest, OpenLineSimplificationAndStaleQueueEntries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  auto lines = reader.read<MultiLineString>(
      "MULTILINESTRING ((0 0, 2 0.2, 5 1, 8 0.2, 10 0))");

  // Side corners have area 0.5 (<= 1.0) and are removed; middle apex then has
  // area 5.0 (> 1.0) and is retained.
  auto partial = TPVWSimplifier::simplify(lines.get(), 1.0);
  ExpectMultiLineEqualsWkt(reader, partial.get(),
                           "MULTILINESTRING ((0 0, 5 1, 10 0))");

  // At tolerance 3.0 (areaTolerance = 9.0), middle apex is initially queued at
  // area 2.4, invalidated as neighbors are removed, and eventually removed.
  auto full = TPVWSimplifier::simplify(lines.get(), 3.0);
  ExpectMultiLineEqualsWkt(reader, full.get(), "MULTILINESTRING ((0 0, 10 0))");
}

TEST(TPVWSimplifierTest, RingEndpointBehaviorForFixedAndFreeRings) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  auto rings = reader.read<MultiLineString>(
      "MULTILINESTRING ("
      "(5 0.2, 10 0, 10 10, 0 10, 0 0, 5 0.2),"
      "(5 20.2, 10 20, 10 30, 0 30, 0 20, 5 20.2))");

  // Two-argument overload treats rings as non-free, preserving vertex 0.
  auto default_result = TPVWSimplifier::simplify(rings.get(), 2.0);
  ExpectMultiLineEqualsWkt(reader, default_result.get(),
                           "MULTILINESTRING ("
                           "(5 0.2, 10 0, 10 10, 0 10, 0 0, 5 0.2),"
                           "(5 20.2, 10 20, 10 30, 0 30, 0 20, 5 20.2))");

  // With freeRings = {true, false} and tolerance 2.0 (areaTolerance = 4.0):
  // Ring 0 drops vertex 0 (area 1.0 <= 4.0) and re-closes at (10, 0);
  // Ring 1 preserves vertex 0.
  std::vector<bool> free_rings = {true, false};
  auto mixed_result =
      TPVWSimplifier::simplify(rings.get(), free_rings, nullptr, 2.0);
  ExpectMultiLineEqualsWkt(reader, mixed_result.get(),
                           "MULTILINESTRING ("
                           "(10 0, 10 10, 0 10, 0 0, 10 0),"
                           "(5 20.2, 10 20, 10 30, 0 30, 0 20, 5 20.2))");

  // With tolerance 10.0 (areaTolerance = 100.0), free ring stops at
  // minEdgeSize == 3 (closed 4-coordinate triangle), while fixed ring keeps
  // endpoint (5, 20.2) and simplifies to a 4-coordinate closed triangle
  // (removing the two area-25 corners at (10, 20) and (0, 20)).
  auto min_size_result =
      TPVWSimplifier::simplify(rings.get(), free_rings, nullptr, 10.0);
  ASSERT_EQ(min_size_result->getNumGeometries(), 2u);
  const LineString* ring0 = min_size_result->getGeometryN(0);
  const LineString* ring1 = min_size_result->getGeometryN(1);
  EXPECT_TRUE(ring0->isRing());
  EXPECT_EQ(ring0->getNumPoints(), 4u);
  EXPECT_TRUE(ring1->isRing());
  EXPECT_EQ(ring1->getNumPoints(), 4u);
  ExpectMultiLineEqualsWkt(reader, min_size_result.get(),
                           "MULTILINESTRING ("
                           "(10 10, 0 10, 0 0, 10 10),"
                           "(5 20.2, 10 30, 0 30, 5 20.2))");

  // Minimal 4-coordinate closed triangle ring (size == minEdgeSize == 3)
  // cannot be simplified further even when marked free with large tolerance.
  auto tri_ring =
      reader.read<MultiLineString>("MULTILINESTRING ((0 0, 10 0, 5 10, 0 0))");
  std::vector<bool> tri_free = {true};
  auto tri_result =
      TPVWSimplifier::simplify(tri_ring.get(), tri_free, nullptr, 100.0);
  ExpectMultiLineEqualsWkt(reader, tri_result.get(),
                           "MULTILINESTRING ((0 0, 10 0, 5 10, 0 0))");
}

TEST(TPVWSimplifierTest, PreventsSelfIntersectionWithSameEdgeVertex) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // Only corner (5, 1) has area <= 25.0 (area = 5.0), but vertex (5, 0.5) of
  // the same edge lies inside triangle ((0,0), (5,1), (10,0)).
  const std::string wkt =
      "MULTILINESTRING ((0 0, 5 1, 10 0, 100 -100, 5 -100, 5 0.5))";
  auto lines = reader.read<MultiLineString>(wkt);
  auto result = TPVWSimplifier::simplify(lines.get(), 5.0);
  ExpectMultiLineEqualsWkt(reader, result.get(), wkt);
}

TEST(TPVWSimplifierTest, RespectsOtherLinesAndConstraintVertices) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // Other input line has a vertex (5, 0.5) inside corner ((0,0),(5,1),(10,0)).
  {
    const std::string wkt = "MULTILINESTRING ((0 0, 5 1, 10 0), (5 0.5, 5 -5))";
    auto lines = reader.read<MultiLineString>(wkt);
    auto result = TPVWSimplifier::simplify(lines.get(), 10.0);
    ExpectMultiLineEqualsWkt(reader, result.get(), wkt);
  }

  // Constraint line has vertex (5, 0.5) inside corner triangle -> blocked.
  {
    auto lines =
        reader.read<MultiLineString>("MULTILINESTRING ((0 0, 5 1, 10 0))");
    auto constraints =
        reader.read<MultiLineString>("MULTILINESTRING ((5 0.5, 5 -5))");
    std::vector<bool> free_rings = {false};
    auto result = TPVWSimplifier::simplify(lines.get(), free_rings,
                                           constraints.get(), 10.0);
    ExpectMultiLineEqualsWkt(reader, result.get(),
                             "MULTILINESTRING ((0 0, 5 1, 10 0))");
  }

  // Constraint vertex (9, 0.9) is inside cornerEnv [0,10]x[0,1] yet outside
  // triangle ((0,0),(5,1),(10,0)) -> does not block simplification.
  {
    auto lines =
        reader.read<MultiLineString>("MULTILINESTRING ((0 0, 5 1, 10 0))");
    auto constraints =
        reader.read<MultiLineString>("MULTILINESTRING ((9 0.9, 20 0.9))");
    std::vector<bool> free_rings;
    auto result = TPVWSimplifier::simplify(lines.get(), free_rings,
                                           constraints.get(), 10.0);
    ExpectMultiLineEqualsWkt(reader, result.get(),
                             "MULTILINESTRING ((0 0, 10 0))");
  }

  // Constraint touching only at corner endpoint (0, 0) -> corner.isVertex skips
  // it and simplification succeeds.
  {
    auto lines =
        reader.read<MultiLineString>("MULTILINESTRING ((0 0, 5 1, 10 0))");
    auto constraints =
        reader.read<MultiLineString>("MULTILINESTRING ((0 0, 0 -5))");
    std::vector<bool> free_rings;
    auto result = TPVWSimplifier::simplify(lines.get(), free_rings,
                                           constraints.get(), 10.0);
    ExpectMultiLineEqualsWkt(reader, result.get(),
                             "MULTILINESTRING ((0 0, 10 0))");
  }
}

TEST(TPVWSimplifierTest, PreventsCollapseOntoTwoPointBaselineEdge) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // Two-point input edge (0,0)-(10,0) matches corner baseline -> blocks
  // collapse of (0,0)-(5,0.5)-(10,0).
  {
    const std::string wkt = "MULTILINESTRING ((0 0, 5 0.5, 10 0), (0 0, 10 0))";
    auto lines = reader.read<MultiLineString>(wkt);
    auto result = TPVWSimplifier::simplify(lines.get(), 10.0);
    ExpectMultiLineEqualsWkt(reader, result.get(), wkt);
  }

  // Reversed two-point constraint line (10,0)-(0,0) also matches baseline.
  {
    auto lines =
        reader.read<MultiLineString>("MULTILINESTRING ((0 0, 5 0.5, 10 0))");
    auto constraints =
        reader.read<MultiLineString>("MULTILINESTRING ((10 0, 0 0))");
    std::vector<bool> free_rings;
    auto result = TPVWSimplifier::simplify(lines.get(), free_rings,
                                           constraints.get(), 10.0);
    ExpectMultiLineEqualsWkt(reader, result.get(),
                             "MULTILINESTRING ((0 0, 5 0.5, 10 0))");
  }

  // Two-point constraint line with different endpoint (0,0)-(20,0) is not the
  // baseline -> simplification proceeds.
  {
    auto lines =
        reader.read<MultiLineString>("MULTILINESTRING ((0 0, 5 0.5, 10 0))");
    auto constraints =
        reader.read<MultiLineString>("MULTILINESTRING ((0 0, 20 0))");
    std::vector<bool> free_rings;
    auto result = TPVWSimplifier::simplify(lines.get(), free_rings,
                                           constraints.get(), 10.0);
    ExpectMultiLineEqualsWkt(reader, result.get(),
                             "MULTILINESTRING ((0 0, 10 0))");
  }

  // Three-point constraint line (size != 2) with vertices outside corner
  // triangle -> does not trigger baseline rejection.
  {
    auto lines =
        reader.read<MultiLineString>("MULTILINESTRING ((0 0, 5 0.5, 10 0))");
    auto constraints =
        reader.read<MultiLineString>("MULTILINESTRING ((0 0, 5 -2, 10 0))");
    std::vector<bool> free_rings;
    auto result = TPVWSimplifier::simplify(lines.get(), free_rings,
                                           constraints.get(), 10.0);
    ExpectMultiLineEqualsWkt(reader, result.get(),
                             "MULTILINESTRING ((0 0, 10 0))");
  }
}

TEST(TPVWSimplifierTest, EdgeAndEdgeIndexAccessors) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  auto ls_open = reader.read<LineString>("LINESTRING (0 0, 5 1, 10 0)");
  auto ls_ring =
      reader.read<LineString>("LINESTRING (20 0, 30 0, 30 10, 20 10, 20 0)");

  std::vector<TPVWSimplifier::Edge> edges;
  edges.emplace_back(ls_open.get(), /*p_isFreeRing=*/false, 4.0);
  edges.emplace_back(ls_ring.get(), /*p_isFreeRing=*/true, 4.0);

  EXPECT_EQ(edges[0].size(), 3u);
  EXPECT_EQ(edges[1].size(), 4u);
  EXPECT_TRUE(edges[0].getCoordinate(1).equals2D(Coordinate(5.0, 1.0)));
  EXPECT_TRUE(edges[1].getCoordinate(0).equals2D(Coordinate(20.0, 0.0)));
  EXPECT_DOUBLE_EQ(edges[0].getEnvelopeInternal()->getMaxX(), 10.0);
  EXPECT_DOUBLE_EQ(edges[1].getEnvelopeInternal()->getMaxY(), 10.0);

  TPVWSimplifier::EdgeIndex index;
  index.add(edges);

  Envelope open_query(0.0, 10.0, 0.0, 1.0);
  std::vector<const TPVWSimplifier::Edge*> hits = index.query(open_query);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0], &edges[0]);

  std::vector<std::size_t> vert_hits =
      edges[1].query(Envelope(25.0, 35.0, 5.0, 15.0));
  ASSERT_EQ(vert_hits.size(), 1u);
  EXPECT_EQ(vert_hits[0], 2u);
}

TEST(TPVWSimplifierTest, CollinearFreeRingEndpointRemovedAtZeroTolerance) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  auto ring = reader.read<MultiLineString>(
      "MULTILINESTRING ((5 0, 10 0, 10 10, 0 10, 0 0, 5 0))");

  std::vector<bool> fixed = {false};
  auto fixed_result = TPVWSimplifier::simplify(ring.get(), fixed, nullptr, 0.0);
  ExpectMultiLineEqualsWkt(
      reader, fixed_result.get(),
      "MULTILINESTRING ((5 0, 10 0, 10 10, 0 10, 0 0, 5 0))");

  std::vector<bool> free = {true};
  auto free_result = TPVWSimplifier::simplify(ring.get(), free, nullptr, 0.0);
  ExpectMultiLineEqualsWkt(reader, free_result.get(),
                           "MULTILINESTRING ((10 0, 10 10, 0 10, 0 0, 10 0))");
}

TEST(TPVWSimplifierTest, MutualSimplificationPreservesTopologyAndBaseline) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // First edge simplifies to 2-point baseline (0,0)-(10,0); second edge then
  // sees edge 0 with size() == 2 and avoids collapsing onto that baseline.
  {
    auto lines = reader.read<MultiLineString>(
        "MULTILINESTRING ((0 0, 5 0.1, 10 0), (0 0, 5 0.5, 10 0))");
    auto result = TPVWSimplifier::simplify(lines.get(), 5.0);
    ExpectMultiLineEqualsWkt(
        reader, result.get(),
        "MULTILINESTRING ((0 0, 10 0), (0 0, 5 0.5, 10 0))");
  }

  // First edge removes (5, 0.2) from both linkedLine and vertexIndex, allowing
  // second edge to subsequently simplify (5, -0.1) without intersection.
  {
    auto lines = reader.read<MultiLineString>(
        "MULTILINESTRING ((0 0, 5 0.2, 10 0), (0 0.5, 5 -0.1, 10 0.5))");
    auto result = TPVWSimplifier::simplify(lines.get(), 5.0);
    ExpectMultiLineEqualsWkt(reader, result.get(),
                             "MULTILINESTRING ((0 0, 10 0), (0 0.5, 10 0.5))");
  }
}

TEST(TPVWSimplifierTest, PartialSimplificationAndTriangleBoundaryVertex) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // Constraint vertex blocks corner (5, 1), while corners (15, 0.2) and then
  // (10, 0) on the same line still simplify within tolerance 2.5 (area 6.25).
  {
    auto lines = reader.read<MultiLineString>(
        "MULTILINESTRING ((0 0, 5 1, 10 0, 15 0.2, 20 0))");
    auto constraints =
        reader.read<MultiLineString>("MULTILINESTRING ((5 0.5, 5 -5))");
    std::vector<bool> free_rings;
    auto result = TPVWSimplifier::simplify(lines.get(), free_rings,
                                           constraints.get(), 2.5);
    ExpectMultiLineEqualsWkt(reader, result.get(),
                             "MULTILINESTRING ((0 0, 5 1, 20 0))");
  }

  // Constraint endpoint strictly on the boundary of the corner triangle (not a
  // corner vertex) still blocks corner removal.
  {
    auto lines =
        reader.read<MultiLineString>("MULTILINESTRING ((0 0, 5 2, 10 0))");
    auto constraints =
        reader.read<MultiLineString>("MULTILINESTRING ((2.5 1, 0 5))");
    std::vector<bool> free_rings;
    auto result = TPVWSimplifier::simplify(lines.get(), free_rings,
                                           constraints.get(), 10.0);
    ExpectMultiLineEqualsWkt(reader, result.get(),
                             "MULTILINESTRING ((0 0, 5 2, 10 0))");
  }
}

}  // namespace
}  // namespace geos_google
