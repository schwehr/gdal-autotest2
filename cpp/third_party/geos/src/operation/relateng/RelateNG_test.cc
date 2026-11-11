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

// Unit tests for third_party/geos/src/operation/relateng/RelateNG.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.

#include "third_party/geos/include/geos/operation/relateng/RelateNG.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/BoundaryNodeRule.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/IntersectionMatrix.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/relateng/RelatePredicate.h"
#include "third_party/geos/include/geos/operation/relateng/TopologyPredicate.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::algorithm::BoundaryNodeRule;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::io::WKTReader;
using geos::operation::relateng::RelateNG;
using geos::operation::relateng::RelatePredicate;
using geos::operation::relateng::TopologyPredicate;
using geos::util::IllegalArgumentException;
using geos::util::UnsupportedOperationException;

struct RelationCase {
  std::string_view name;
  std::string_view wkt_a;
  std::string_view wkt_b;
  std::string_view matrix;
  bool intersects;
  bool touches;
  bool crosses;
  bool within;
  bool contains;
  bool overlaps;
  bool covers;
  bool covered_by;
  bool equals_topo;
};

constexpr std::array<RelationCase, 46> kRelationCases = {{
    {"SamePoint", "POINT (5 5)", "POINT (5 5)", "0FFFFFFF2", true, false, false,
     true, true, false, true, true, true},
    {"DisjointPoints", "POINT (1 1)", "POINT (2 2)", "FF0FFF0F2", false, false,
     false, false, false, false, false, false, false},
    {"PointZIgnored", "POINT Z (5 5 10)", "POINT Z (5 5 99)", "0FFFFFFF2", true,
     false, false, true, true, false, true, true, true},
    {"MultiPointSubsetEqualEnvelope", "MULTIPOINT ((0 0), (5 5), (10 10))",
     "MULTIPOINT ((0 0), (10 10))", "0F0FFFFF2", true, false, false, false,
     true, false, true, false, false},
    {"MultiPointOverlapEqualEnvelope", "MULTIPOINT ((0 0), (5 5), (10 10))",
     "MULTIPOINT ((0 0), (6 6), (10 10))", "0F0FFF0F2", true, false, false,
     false, false, true, false, false, false},
    {"MultiPointEqualWithDuplicates", "MULTIPOINT ((0 0), (0 0), (10 10))",
     "MULTIPOINT ((10 10), (0 0))", "0FFFFFFF2", true, false, false, true, true,
     false, true, true, true},
    {"ZeroLengthLineEqualsPoint", "LINESTRING (5 5, 5 5)", "POINT (5 5)",
     "0FFFFFFF2", true, false, false, true, true, false, true, true, true},
    {"ZeroLengthLineEqualsZeroLengthLine", "LINESTRING (5 5, 5 5)",
     "LINESTRING (5 5, 5 5, 5 5)", "0FFFFFFF2", true, false, false, true, true,
     false, true, true, true},
    {"PointInLineInterior", "POINT (5 5)", "LINESTRING (0 0, 10 10)",
     "0FFFFF102", true, false, false, true, false, false, false, true, false},
    {"LineContainsPoint", "LINESTRING (0 0, 10 10)", "POINT (5 5)", "0F1FF0FF2",
     true, false, false, false, true, false, true, false, false},
    {"PointOnLineBoundary", "POINT (0 0)", "LINESTRING (0 0, 10 10)",
     "F0FFFF102", true, true, false, false, false, false, false, true, false},
    {"LineCoversBoundaryPoint", "LINESTRING (0 0, 10 10)", "POINT (0 0)",
     "FF10F0FF2", true, true, false, false, false, false, true, false, false},
    {"PointOnClosedLine", "POINT (0 0)",
     "LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)", "0FFFFF1F2", true, false,
     false, true, false, false, false, true, false},
    {"MultiPointCrossesLine", "MULTIPOINT ((5 5), (20 20))",
     "LINESTRING (0 0, 10 10)", "0F0FFF102", true, false, true, false, false,
     false, false, false, false},
    {"LineCrossesMultiPoint", "LINESTRING (0 0, 10 10)",
     "MULTIPOINT ((5 5), (20 20))", "0F1FF00F2", true, false, true, false,
     false, false, false, false, false},
    {"PointInPolygonInterior", "POINT (5 5)",
     "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))", "0FFFFF212", true, false, false,
     true, false, false, false, true, false},
    {"PolygonContainsPoint", "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
     "POINT (5 5)", "0F2FF1FF2", true, false, false, false, true, false, true,
     false, false},
    {"PointOnPolygonBoundary", "POINT (0 5)",
     "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))", "F0FFFF212", true, true, false,
     false, false, false, false, true, false},
    {"PointInPolygonHole", "POINT (5 5)",
     "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), (3 3, 7 3, 7 7, 3 7, 3 3))",
     "FF0FFF212", false, false, false, false, false, false, false, false,
     false},
    {"MultiPointCrossesPolygon", "MULTIPOINT ((5 5), (15 15))",
     "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))", "0F0FFF212", true, false, true,
     false, false, false, false, false, false},
    {"LinesCrossing", "LINESTRING (0 5, 10 5)", "LINESTRING (5 0, 5 10)",
     "0F1FF0102", true, false, true, false, false, false, false, false, false},
    {"LinesOverlapping", "LINESTRING (0 0, 10 0)", "LINESTRING (5 0, 15 0)",
     "1010F0102", true, false, false, false, false, true, false, false, false},
    {"LinesTouchAtEndpoint", "LINESTRING (0 0, 5 0)", "LINESTRING (5 0, 10 0)",
     "FF1F00102", true, true, false, false, false, false, false, false, false},
    {"LinesTouchTJunction", "LINESTRING (0 0, 10 0)", "LINESTRING (5 0, 5 5)",
     "F01FF0102", true, true, false, false, false, false, false, false, false},
    {"LineWithinLine", "LINESTRING (2 0, 8 0)", "LINESTRING (0 0, 10 0)",
     "1FF0FF102", true, false, false, true, false, false, false, true, false},
    {"LinesEqualSplitReversed", "LINESTRING (0 0, 10 0)",
     "LINESTRING (10 0, 5 0, 0 0)", "1FFF0FFF2", true, false, false, true, true,
     false, true, true, true},
    {"LinearRingEqualsClosedLineString",
     "LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)",
     "LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)", "1FFFFFFF2", true, false,
     false, true, true, false, true, true, true},
    {"LinesDisjointOverlappingEnvelopes", "LINESTRING (0 0, 10 10)",
     "LINESTRING (0 8, 2 10)", "FF1FF0102", false, false, false, false, false,
     false, false, false, false},
    {"LinesDisjointSeparatedEnvelopes", "LINESTRING (0 0, 5 5)",
     "LINESTRING (10 10, 15 15)", "FF1FF0102", false, false, false, false,
     false, false, false, false, false},
    {"LineWithinPolygon", "LINESTRING (2 2, 8 8)",
     "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))", "1FF0FF212", true, false, false,
     true, false, false, false, true, false},
    {"LineOnPolygonBoundary", "LINESTRING (0 0, 0 10)",
     "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))", "F1FF0F212", true, true, false,
     false, false, false, false, true, false},
    {"PolygonCoversBoundaryLine", "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
     "LINESTRING (0 0, 0 10)", "FF2101FF2", true, true, false, false, false,
     false, true, false, false},
    {"LineCrossesPolygon", "LINESTRING (-5 5, 15 5)",
     "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))", "101FF0212", true, false, true,
     false, false, false, false, false, false},
    {"LineDiagonalCoveringPolygonEnvelope", "LINESTRING (0 0, 10 10)",
     "POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))", "101FF0212", true, false, true,
     false, false, false, false, false, false},
    {"LineEndsOutsideThenInsidePolygon", "LINESTRING (-5 5, 5 5)",
     "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))", "1010F0212", true, false, true,
     false, false, false, false, false, false},
    {"PolygonsEqual", "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
     "POLYGON ((10 10, 0 10, 0 0, 10 0, 10 0, 10 10))", "2FFF1FFF2", true,
     false, false, true, true, false, true, true, true},
    {"PolygonStrictlyWithinPolygon", "POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))",
     "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))", "2FF1FF212", true, false, false,
     true, false, false, false, true, false},
    {"PolygonContainsPolygon", "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
     "POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))", "212FF1FF2", true, false, false,
     false, true, false, true, false, false},
    {"PolygonWithinSharingBoundary", "POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0))",
     "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))", "2FF11F212", true, false, false,
     true, false, false, false, true, false},
    {"PolygonsOverlappingCrossingEdges",
     "POLYGON ((0 2, 10 2, 10 8, 0 8, 0 2))",
     "POLYGON ((2 0, 8 0, 8 10, 2 10, 2 0))", "212101212", true, false, false,
     false, false, true, false, false, false},
    {"PolygonsOverlappingCollinearEdges", "POLYGON ((0 0, 6 0, 6 6, 0 6, 0 0))",
     "POLYGON ((4 0, 10 0, 10 6, 4 6, 4 0))", "212111212", true, false, false,
     false, false, true, false, false, false},
    {"PolygonsTouchAtEdge", "POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0))",
     "POLYGON ((5 0, 10 0, 10 5, 5 5, 5 0))", "FF2F11212", true, true, false,
     false, false, false, false, false, false},
    {"PolygonsTouchAtPoint", "POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0))",
     "POLYGON ((5 5, 10 5, 10 10, 5 10, 5 5))", "FF2F01212", true, true, false,
     false, false, false, false, false, false},
    {"PolygonInsideHoleDisjoint",
     "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), (5 5, 15 5, 15 15, 5 15, 5 5))",
     "POLYGON ((7 7, 13 7, 13 13, 7 13, 7 7))", "FF2FF1212", false, false,
     false, false, false, false, false, false, false},
    {"PolygonOverlapsHoleVertex", "POLYGON ((4 4, 6 4, 6 6, 4 6, 4 4))",
     "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), (5 5, 15 5, 15 15, 5 15, 5 5))",
     "212101212", true, false, false, false, false, true, false, false, false},
    {"PolygonDisjointSeparatedEnvelopes", "POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0))",
     "POLYGON ((10 10, 15 10, 15 15, 10 15, 10 10))", "FF2FF1212", false, false,
     false, false, false, false, false, false, false},
}};

// Custom TopologyPredicate tracking lifecycle calls and checking whether A's
// interior intersects B's boundary.
class InteriorMeetsBoundaryPredicate final : public TopologyPredicate {
 public:
  explicit InteriorMeetsBoundaryPredicate(bool require_interaction = true)
      : require_interaction_(require_interaction) {}

  std::string name() const override { return "interiorMeetsBoundary"; }

  bool requireInteraction() const override { return require_interaction_; }

  void init(int dA, int dB) override {
    ++init_dim_calls_;
    if (dA < 0 || dB < 0) {
      known_ = true;
      val_ = false;
    }
  }

  void init(const geos::geom::Envelope& envA,
            const geos::geom::Envelope& envB) override {
    ++init_env_calls_;
    if (envA.disjoint(&envB)) {
      known_ = true;
      val_ = false;
    }
  }

  void updateDimension(geos::geom::Location locA, geos::geom::Location locB,
                       int dimension) override {
    ++update_calls_;
    if (locA == geos::geom::Location::INTERIOR &&
        locB == geos::geom::Location::BOUNDARY && dimension >= 0) {
      known_ = true;
      val_ = true;
    }
  }

  void finish() override {
    ++finish_calls_;
    if (!known_) {
      known_ = true;
      val_ = false;
    }
  }

  bool isKnown() const override { return known_; }
  bool value() const override { return val_; }

  int init_dim_calls() const { return init_dim_calls_; }
  int init_env_calls() const { return init_env_calls_; }
  int update_calls() const { return update_calls_; }
  int finish_calls() const { return finish_calls_; }

 private:
  bool require_interaction_ = true;
  bool known_ = false;
  bool val_ = false;
  int init_dim_calls_ = 0;
  int init_env_calls_ = 0;
  int update_calls_ = 0;
  int finish_calls_ = 0;
};

class RelateNGTest : public ::testing::Test {
 protected:
  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};

  std::unique_ptr<Geometry> ReadWkt(std::string_view wkt) const {
    return reader_.read(std::string(wkt));
  }

  void VerifyRelationCase(const RelationCase& c) const {
    SCOPED_TRACE(std::string(c.name));
    auto a = ReadWkt(c.wkt_a);
    auto b = ReadWkt(c.wkt_b);
    const std::string expected_matrix(c.matrix);

    // Static predicates and relate overloads.
    EXPECT_EQ(RelateNG::intersects(a.get(), b.get()), c.intersects);
    EXPECT_EQ(RelateNG::disjoint(a.get(), b.get()), !c.intersects);
    EXPECT_EQ(RelateNG::touches(a.get(), b.get()), c.touches);
    EXPECT_EQ(RelateNG::crosses(a.get(), b.get()), c.crosses);
    EXPECT_EQ(RelateNG::within(a.get(), b.get()), c.within);
    EXPECT_EQ(RelateNG::contains(a.get(), b.get()), c.contains);
    EXPECT_EQ(RelateNG::overlaps(a.get(), b.get()), c.overlaps);
    EXPECT_EQ(RelateNG::covers(a.get(), b.get()), c.covers);
    EXPECT_EQ(RelateNG::coveredBy(a.get(), b.get()), c.covered_by);
    EXPECT_EQ(RelateNG::equalsTopo(a.get(), b.get()), c.equals_topo);
    const std::string other_matrix =
        expected_matrix == "FFFFFFFF2" ? "0FFFFFFF2" : "FFFFFFFF2";
    EXPECT_EQ(RelateNG::relate(a.get(), b.get())->toString(), expected_matrix);
    EXPECT_TRUE(RelateNG::relate(a.get(), b.get(), expected_matrix));
    EXPECT_FALSE(RelateNG::relate(a.get(), b.get(), other_matrix));

    // Prepared RelateNG instance predicates, relate, and evaluate methods.
    auto prep = RelateNG::prepare(a.get());
    EXPECT_EQ(prep->intersects(b.get()), c.intersects);
    EXPECT_EQ(prep->disjoint(b.get()), !c.intersects);
    EXPECT_EQ(prep->touches(b.get()), c.touches);
    EXPECT_EQ(prep->crosses(b.get()), c.crosses);
    EXPECT_EQ(prep->within(b.get()), c.within);
    EXPECT_EQ(prep->contains(b.get()), c.contains);
    EXPECT_EQ(prep->overlaps(b.get()), c.overlaps);
    EXPECT_EQ(prep->covers(b.get()), c.covers);
    EXPECT_EQ(prep->coveredBy(b.get()), c.covered_by);
    EXPECT_EQ(prep->equalsTopo(b.get()), c.equals_topo);
    EXPECT_EQ(prep->relate(b.get())->toString(), expected_matrix);
    EXPECT_TRUE(prep->relate(b.get(), expected_matrix));
    EXPECT_FALSE(prep->relate(b.get(), other_matrix));
    EXPECT_EQ(prep->evaluate(b.get())->toString(), expected_matrix);
    EXPECT_TRUE(prep->evaluate(b.get(), expected_matrix));
    EXPECT_FALSE(prep->evaluate(b.get(), other_matrix));
  }
};

TEST_F(RelateNGTest, StandardCasesStaticAndPrepared) {
  for (const auto& c : kRelationCases) {
    VerifyRelationCase(c);
  }
}

TEST_F(RelateNGTest, EmptyGeometryCombinations) {
  struct EmptyCase {
    std::string_view name;
    std::string_view wkt_a;
    std::string_view wkt_b;
    std::string_view matrix;
    bool equals_topo;
  };
  constexpr std::array<EmptyCase, 7> kEmptyCases = {{
      {"PointEmptyVsPointEmpty", "POINT EMPTY", "POINT EMPTY", "FFFFFFFF2",
       true},
      {"LineEmptyVsPolygonEmpty", "LINESTRING EMPTY", "POLYGON EMPTY",
       "FFFFFFFF2", true},
      {"CollectionEmptyVsPoint", "GEOMETRYCOLLECTION EMPTY", "POINT (0 0)",
       "FFFFFF0F2", false},
      {"PointVsPolygonEmpty", "POINT (1 1)", "POLYGON EMPTY", "FF0FFFFF2",
       false},
      {"LineVsPointEmpty", "LINESTRING (0 0, 10 0)", "POINT EMPTY", "FF1FF0FF2",
       false},
      {"PolygonVsPointEmpty", "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
       "POINT EMPTY", "FF2FF1FF2", false},
      {"PolygonEmptyVsLine", "POLYGON EMPTY", "LINESTRING (0 0, 10 0)",
       "FFFFFF102", false},
  }};

  for (const auto& c : kEmptyCases) {
    SCOPED_TRACE(std::string(c.name));
    auto a = ReadWkt(c.wkt_a);
    auto b = ReadWkt(c.wkt_b);

    EXPECT_FALSE(RelateNG::intersects(a.get(), b.get()));
    EXPECT_TRUE(RelateNG::disjoint(a.get(), b.get()));
    EXPECT_FALSE(RelateNG::touches(a.get(), b.get()));
    EXPECT_FALSE(RelateNG::crosses(a.get(), b.get()));
    EXPECT_FALSE(RelateNG::within(a.get(), b.get()));
    EXPECT_FALSE(RelateNG::contains(a.get(), b.get()));
    EXPECT_FALSE(RelateNG::overlaps(a.get(), b.get()));
    EXPECT_FALSE(RelateNG::covers(a.get(), b.get()));
    EXPECT_FALSE(RelateNG::coveredBy(a.get(), b.get()));
    EXPECT_EQ(RelateNG::equalsTopo(a.get(), b.get()), c.equals_topo);
    EXPECT_EQ(RelateNG::relate(a.get(), b.get())->toString(), c.matrix);
    EXPECT_TRUE(RelateNG::relate(a.get(), b.get(), std::string(c.matrix)));

    auto prep = RelateNG::prepare(a.get());
    EXPECT_EQ(prep->equalsTopo(b.get()), c.equals_topo);
    EXPECT_TRUE(prep->disjoint(b.get()));
    EXPECT_FALSE(prep->intersects(b.get()));
    EXPECT_EQ(prep->relate(b.get())->toString(), c.matrix);
  }
}

TEST_F(RelateNGTest, GeometryCollectionsWithEmptyAndMixedComponents) {
  // Collection with empty point + non-empty point against line hits
  // computePoints empty-element skipping.
  {
    auto gc = ReadWkt("GEOMETRYCOLLECTION (POINT EMPTY, POINT (5 5))");
    auto line = ReadWkt("LINESTRING (0 0, 10 10)");
    EXPECT_TRUE(RelateNG::within(gc.get(), line.get()));
    EXPECT_EQ(RelateNG::relate(gc.get(), line.get())->toString(), "0FFFFF102");
  }

  // Collection with empty line + non-empty line against polygon hits
  // computeLineEnds empty-element skipping.
  {
    auto gc = ReadWkt(
        "GEOMETRYCOLLECTION (LINESTRING EMPTY, LINESTRING (0 0, 10 0))");
    auto poly = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
    EXPECT_TRUE(RelateNG::coveredBy(gc.get(), poly.get()));
    EXPECT_TRUE(RelateNG::touches(gc.get(), poly.get()));
    EXPECT_EQ(RelateNG::relate(gc.get(), poly.get())->toString(), "F1FF0F212");
  }

  // Collection with empty polygon + non-empty polygon hits computeAreaVertex
  // empty-element skipping.
  {
    auto gc = ReadWkt(
        "GEOMETRYCOLLECTION (POLYGON EMPTY, POLYGON ((0 0, 10 0, 10 10, 0 10, "
        "0 0)))");
    auto inner = ReadWkt("POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))");
    EXPECT_TRUE(RelateNG::contains(gc.get(), inner.get()));
    EXPECT_EQ(RelateNG::relate(gc.get(), inner.get())->toString(), "212FF1FF2");
  }

  // Mixed collection where a LineString endpoint lies inside the collection's
  // own Polygon: tests dimLineEnd != Dimension::L skip in computeLineEnd, plus
  // non-matching element type skips in computeLineEnds/computeAreaVertex, and
  // hasAreaAndLine() self-noding when passed as B.
  {
    auto mixed = ReadWkt(
        "GEOMETRYCOLLECTION ("
        "  POINT (20 20),"
        "  LINESTRING (5 5, 15 5),"
        "  POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)))");
    auto poly_b = ReadWkt("POLYGON ((12 0, 20 0, 20 10, 12 10, 12 0))");
    EXPECT_EQ(RelateNG::relate(mixed.get(), poly_b.get())->toString(),
              "1020F1212");
    EXPECT_EQ(RelateNG::relate(poly_b.get(), mixed.get())->toString(),
              "1020F1212");
    EXPECT_TRUE(RelateNG::intersects(mixed.get(), poly_b.get()));
    // Both inputs have dimension 2 (A), and 1020F1212 matches T*T***T**.
    EXPECT_TRUE(RelateNG::overlaps(mixed.get(), poly_b.get()));
    EXPECT_FALSE(RelateNG::touches(mixed.get(), poly_b.get()));
  }

  // Overlapping polygons in a GeometryCollection follow union semantics.
  {
    auto gc_polys = ReadWkt(
        "GEOMETRYCOLLECTION ("
        "  POLYGON ((0 0, 6 0, 6 6, 0 6, 0 0)),"
        "  POLYGON ((4 0, 10 0, 10 6, 4 6, 4 0)))");
    auto union_poly = ReadWkt("POLYGON ((0 0, 10 0, 10 6, 0 6, 0 0))");
    auto internal_line = ReadWkt("LINESTRING (4 1, 4 5)");
    EXPECT_TRUE(RelateNG::equalsTopo(gc_polys.get(), union_poly.get()));
    EXPECT_EQ(RelateNG::relate(gc_polys.get(), union_poly.get())->toString(),
              "2FFF1FFF2");
    EXPECT_TRUE(RelateNG::contains(gc_polys.get(), internal_line.get()));
  }
}

TEST_F(RelateNGTest, MultiPartDisjointElementSkippingOptimization) {
  // First line has an endpoint in exterior of poly (recording exterior
  // intersection without concluding relate()), second line has disjoint
  // envelope and hits the skip optimization in computeLineEnds.
  auto mline = ReadWkt("MULTILINESTRING ((0 5, 5 5), (100 100, 105 105))");
  auto box = ReadWkt("POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))");
  EXPECT_EQ(RelateNG::relate(mline.get(), box.get())->toString(), "1010F0212");
  EXPECT_TRUE(RelateNG::crosses(mline.get(), box.get()));

  // First line has an endpoint in exterior of poly, second line has
  // overlapping envelope (so the disjoint-envelope skip is not taken).
  auto mline_overlapping_env =
      ReadWkt("MULTILINESTRING ((0 5, 1 5), (4 4, 6 4))");
  EXPECT_EQ(
      RelateNG::relate(mline_overlapping_env.get(), box.get())->toString(),
      "1F10F0212");
  EXPECT_TRUE(RelateNG::crosses(mline_overlapping_env.get(), box.get()));

  // First polygon has a vertex in exterior of target (recording exterior
  // intersection without concluding relate()), second polygon has disjoint
  // envelope and hits the skip optimization in computeAreaVertex.
  auto mpoly = ReadWkt(
      "MULTIPOLYGON ("
      "  ((0 0, 5 0, 5 5, 0 5, 0 0)),"
      "  ((100 100, 105 100, 105 105, 100 105, 100 100)))");
  auto overlap_box = ReadWkt("POLYGON ((3 3, 8 3, 8 8, 3 8, 3 3))");
  EXPECT_EQ(RelateNG::relate(mpoly.get(), overlap_box.get())->toString(),
            "212101212");
  EXPECT_TRUE(RelateNG::overlaps(mpoly.get(), overlap_box.get()));
}

TEST_F(RelateNGTest, SelfNodingSelfCrossingAndRepeatedPoints) {
  // Self-crossing line tested against a segment identical to one of its arms
  // and lines passing through its self-intersection point.
  auto bowtie = ReadWkt("LINESTRING (0 0, 10 10, 10 0, 0 10)");
  auto arm = ReadWkt("LINESTRING (0 0, 10 10)");
  auto mid_cross = ReadWkt("LINESTRING (0 5, 10 5)");
  auto full_cross = ReadWkt("LINESTRING (-2 5, 12 5)");
  auto mid_pt = ReadWkt("POINT (5 5)");

  EXPECT_EQ(RelateNG::relate(bowtie.get(), arm.get())->toString(), "101F00FF2");
  EXPECT_TRUE(RelateNG::contains(bowtie.get(), arm.get()));
  EXPECT_TRUE(RelateNG::covers(bowtie.get(), arm.get()));
  EXPECT_FALSE(RelateNG::equalsTopo(bowtie.get(), arm.get()));

  // Endpoint (10, 5) of mid_cross lies on bowtie's interior segment
  // (10,10)-(10,0), yielding IM[I,B] = 0.
  EXPECT_TRUE(RelateNG::crosses(bowtie.get(), mid_cross.get()));
  EXPECT_EQ(RelateNG::relate(bowtie.get(), mid_cross.get())->toString(),
            "001FF0102");
  EXPECT_TRUE(RelateNG::crosses(bowtie.get(), full_cross.get()));
  EXPECT_EQ(RelateNG::relate(bowtie.get(), full_cross.get())->toString(),
            "0F1FF0102");
  EXPECT_TRUE(RelateNG::contains(bowtie.get(), mid_pt.get()));

  // Repeated consecutive vertices are conditioned without altering topology.
  auto dup_line = ReadWkt("LINESTRING (0 0, 0 0, 5 5, 5 5, 10 10)");
  auto clean_line = ReadWkt("LINESTRING (0 0, 10 10)");
  EXPECT_TRUE(RelateNG::equalsTopo(dup_line.get(), clean_line.get()));
}

TEST_F(RelateNGTest, BoundaryNodeRulesStaticAndPrepared) {
  // Closed LineString meeting an open LineString at the closed ring's
  // start/end.
  auto closed_line = ReadWkt("LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  auto spoke = ReadWkt("LINESTRING (0 0, -5 -5)");

  EXPECT_EQ(RelateNG::relate(closed_line.get(), spoke.get(),
                             BoundaryNodeRule::getBoundaryRuleMod2())
                ->toString(),
            "F01FFF102");
  EXPECT_EQ(RelateNG::relate(closed_line.get(), spoke.get(),
                             BoundaryNodeRule::getBoundaryOGCSFS())
                ->toString(),
            "F01FFF102");
  EXPECT_EQ(RelateNG::relate(closed_line.get(), spoke.get(),
                             BoundaryNodeRule::getBoundaryEndPoint())
                ->toString(),
            "FF1F0F102");
  EXPECT_EQ(RelateNG::relate(closed_line.get(), spoke.get(),
                             BoundaryNodeRule::getBoundaryMultivalentEndPoint())
                ->toString(),
            "FF10FF1F2");
  EXPECT_EQ(RelateNG::relate(closed_line.get(), spoke.get(),
                             BoundaryNodeRule::getBoundaryMonovalentEndPoint())
                ->toString(),
            "F01FFF102");

  // MultiLineString with valency-2 junction at (5,0) and valency-1 ends at
  // (0,0) and (10,0), tested against POINT(5 0).
  auto split_line = ReadWkt("MULTILINESTRING ((0 0, 5 0), (5 0, 10 0))");
  auto junction_pt = ReadWkt("POINT (5 0)");
  auto end_pt = ReadWkt("POINT (0 0)");

  EXPECT_EQ(RelateNG::relate(split_line.get(), junction_pt.get(),
                             BoundaryNodeRule::getBoundaryRuleMod2())
                ->toString(),
            "0F1FF0FF2");
  EXPECT_EQ(RelateNG::relate(split_line.get(), junction_pt.get(),
                             BoundaryNodeRule::getBoundaryEndPoint())
                ->toString(),
            "FF10F0FF2");
  EXPECT_EQ(RelateNG::relate(split_line.get(), junction_pt.get(),
                             BoundaryNodeRule::getBoundaryMultivalentEndPoint())
                ->toString(),
            "FF10FFFF2");
  EXPECT_EQ(RelateNG::relate(split_line.get(), junction_pt.get(),
                             BoundaryNodeRule::getBoundaryMonovalentEndPoint())
                ->toString(),
            "0F1FF0FF2");

  // Static relate(a, b, predicate, bnRule).
  {
    RelatePredicate::TouchesPredicate pred_mod2;
    EXPECT_FALSE(RelateNG::relate(split_line.get(), junction_pt.get(),
                                  pred_mod2,
                                  BoundaryNodeRule::getBoundaryRuleMod2()));

    RelatePredicate::TouchesPredicate pred_endpoint;
    EXPECT_TRUE(RelateNG::relate(split_line.get(), junction_pt.get(),
                                 pred_endpoint,
                                 BoundaryNodeRule::getBoundaryEndPoint()));

    auto matcher_mod2 = RelatePredicate::matches("F0FFFF102");
    EXPECT_FALSE(RelateNG::relate(junction_pt.get(), split_line.get(),
                                  *matcher_mod2,
                                  BoundaryNodeRule::getBoundaryRuleMod2()));
    auto matcher_endpoint = RelatePredicate::matches("F0FFFF102");
    EXPECT_TRUE(RelateNG::relate(junction_pt.get(), split_line.get(),
                                 *matcher_endpoint,
                                 BoundaryNodeRule::getBoundaryEndPoint()));
  }

  // Prepared RelateNG::prepare(a, bnRule) across multiple queries.
  {
    auto prep_ep = RelateNG::prepare(split_line.get(),
                                     BoundaryNodeRule::getBoundaryEndPoint());
    EXPECT_TRUE(prep_ep->touches(junction_pt.get()));
    EXPECT_FALSE(prep_ep->contains(junction_pt.get()));
    EXPECT_TRUE(prep_ep->covers(junction_pt.get()));
    EXPECT_TRUE(prep_ep->touches(end_pt.get()));
    EXPECT_EQ(prep_ep->relate(junction_pt.get())->toString(), "FF10F0FF2");
  }
}

TEST_F(RelateNGTest, CustomTopologyPredicateAndEarlyExits) {
  auto big_poly = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto boundary_pt = ReadWkt("POINT (0 5)");
  auto interior_pt = ReadWkt("POINT (5 5)");
  auto disjoint_pt = ReadWkt("POINT (20 20)");
  auto split_line = ReadWkt("MULTILINESTRING ((0 0, 5 0), (5 0, 10 0))");
  auto junction_pt = ReadWkt("POINT (5 0)");
  auto empty_pt = ReadWkt("POINT EMPTY");

  // Static relate(a, b, pred) with a polygon interior intersecting a point:
  // interior of big_poly does not meet boundary_pt's boundary (empty).
  {
    InteriorMeetsBoundaryPredicate pred;
    EXPECT_FALSE(RelateNG::relate(big_poly.get(), boundary_pt.get(), pred));
    EXPECT_EQ(pred.init_dim_calls(), 1);
    EXPECT_EQ(pred.init_env_calls(), 1);
    EXPECT_EQ(pred.finish_calls(), 1);
  }

  // Reversed: interior of boundary_pt (I) meets boundary of big_poly (B),
  // short-circuiting inside computeAtPoints without calling finish().
  {
    InteriorMeetsBoundaryPredicate pred;
    EXPECT_TRUE(RelateNG::relate(boundary_pt.get(), big_poly.get(), pred));
    EXPECT_TRUE(pred.isKnown());
    EXPECT_GE(pred.update_calls(), 1);
    EXPECT_EQ(pred.finish_calls(), 0);
  }

  // With requireInteraction() == true, disjoint envelopes short-circuit in
  // hasRequiredEnvelopeInteraction() before init() or finish().
  {
    InteriorMeetsBoundaryPredicate pred(/*require_interaction=*/true);
    EXPECT_FALSE(RelateNG::relate(boundary_pt.get(), disjoint_pt.get(), pred));
    EXPECT_EQ(pred.init_dim_calls(), 0);
    EXPECT_EQ(pred.init_env_calls(), 0);
    EXPECT_EQ(pred.update_calls(), 0);
    EXPECT_EQ(pred.finish_calls(), 0);
  }

  // With requireInteraction() == false, disjoint envelopes trigger
  // init(envA, envB) early exit -> finishValue().
  {
    InteriorMeetsBoundaryPredicate pred(/*require_interaction=*/false);
    EXPECT_FALSE(RelateNG::relate(boundary_pt.get(), disjoint_pt.get(), pred));
    EXPECT_EQ(pred.init_dim_calls(), 1);
    EXPECT_EQ(pred.init_env_calls(), 1);
    EXPECT_EQ(pred.update_calls(), 0);
    EXPECT_EQ(pred.finish_calls(), 1);
  }

  // With requireInteraction() == false, empty geometry triggers init(dimA,
  // dimB) early exit -> finishValue().
  {
    InteriorMeetsBoundaryPredicate pred(/*require_interaction=*/false);
    EXPECT_FALSE(RelateNG::relate(empty_pt.get(), big_poly.get(), pred));
    EXPECT_EQ(pred.init_dim_calls(), 1);
    EXPECT_EQ(pred.init_env_calls(), 0);
    EXPECT_EQ(pred.update_calls(), 0);
    EXPECT_EQ(pred.finish_calls(), 1);
  }

  // Prepared evaluate(b, pred) and relate(a, b, pred, bnRule).
  {
    auto prep = RelateNG::prepare(interior_pt.get());
    InteriorMeetsBoundaryPredicate pred;
    EXPECT_FALSE(prep->evaluate(big_poly.get(), pred));
  }
  {
    InteriorMeetsBoundaryPredicate pred;
    EXPECT_TRUE(RelateNG::relate(junction_pt.get(), split_line.get(), pred,
                                 BoundaryNodeRule::getBoundaryEndPoint()));
  }
}

TEST_F(RelateNGTest, PredicateShortCircuitPaths) {
  auto small_box = ReadWkt("POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))");
  auto big_box = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto diag_line = ReadWkt("LINESTRING (0 0, 10 10)");
  auto pt_at_5 = ReadWkt("POINT (5 5)");
  auto pt_at_6 = ReadWkt("POINT (6 6)");
  auto disjoint_box = ReadWkt("POLYGON ((20 20, 30 20, 30 30, 20 30, 20 20))");
  auto line_out_then_in = ReadWkt("LINESTRING (-5 5, 5 5)");
  auto line_in_then_out = ReadWkt("LINESTRING (5 5, -5 5)");

  // Envelope containment short-circuits in hasRequiredEnvelopeInteraction.
  EXPECT_FALSE(RelateNG::contains(small_box.get(), big_box.get()));
  EXPECT_FALSE(RelateNG::covers(small_box.get(), big_box.get()));
  EXPECT_FALSE(RelateNG::within(big_box.get(), small_box.get()));
  EXPECT_FALSE(RelateNG::coveredBy(big_box.get(), small_box.get()));
  EXPECT_FALSE(RelateNG::intersects(small_box.get(), disjoint_box.get()));
  EXPECT_FALSE(RelateNG::touches(small_box.get(), disjoint_box.get()));
  EXPECT_FALSE(RelateNG::overlaps(small_box.get(), disjoint_box.get()));

  // Dimension short-circuits via predicate.init(dimA, dimB) after passing
  // envelope checks.
  EXPECT_FALSE(RelateNG::touches(pt_at_5.get(), pt_at_5.get()));
  EXPECT_FALSE(RelateNG::crosses(pt_at_5.get(), pt_at_5.get()));
  EXPECT_FALSE(RelateNG::crosses(small_box.get(), big_box.get()));
  EXPECT_FALSE(RelateNG::overlaps(pt_at_5.get(), diag_line.get()));
  EXPECT_FALSE(RelateNG::contains(diag_line.get(), small_box.get()));
  EXPECT_FALSE(RelateNG::within(small_box.get(), diag_line.get()));

  // Envelope equality / disjointness short-circuits via init(envA, envB).
  EXPECT_TRUE(RelateNG::disjoint(small_box.get(), disjoint_box.get()));
  EXPECT_FALSE(RelateNG::equalsTopo(small_box.get(), big_box.get()));
  EXPECT_FALSE(RelateNG::equalsTopo(pt_at_5.get(), pt_at_6.get()));

  // Line endpoint early-exit branches in computeLineEnds (e0 vs e1).
  EXPECT_TRUE(RelateNG::intersects(line_in_then_out.get(), big_box.get()));
  EXPECT_TRUE(RelateNG::intersects(line_out_then_in.get(), big_box.get()));
  EXPECT_FALSE(RelateNG::within(line_in_then_out.get(), big_box.get()));
  EXPECT_FALSE(RelateNG::within(line_out_then_in.get(), big_box.get()));

  // IMPatternMatcher with pattern requiring interaction vs not requiring it.
  EXPECT_FALSE(
      RelateNG::relate(small_box.get(), disjoint_box.get(), "T********"));
  EXPECT_TRUE(
      RelateNG::relate(small_box.get(), disjoint_box.get(), "FF*FF****"));

  // computePP early exits on matched point (intersects) and unmatched point
  // (equalsTopo with identical envelopes).
  auto mp_a = ReadWkt("MULTIPOINT ((0 0), (5 5), (10 10))");
  auto mp_b = ReadWkt("MULTIPOINT ((0 0), (6 6), (10 10))");
  EXPECT_TRUE(RelateNG::intersects(mp_a.get(), mp_b.get()));
  EXPECT_FALSE(RelateNG::equalsTopo(mp_a.get(), mp_b.get()));

  // computeAtPoints early exits after computePoints and after computeLineEnds.
  auto gc_pt_line =
      ReadWkt("GEOMETRYCOLLECTION (POINT (5 5), LINESTRING (20 20, 30 30))");
  auto gc_line_poly = ReadWkt(
      "GEOMETRYCOLLECTION ("
      "  LINESTRING (2 2, 8 2),"
      "  POLYGON ((20 20, 30 20, 30 30, 20 30, 20 20)))");
  EXPECT_TRUE(RelateNG::intersects(gc_pt_line.get(), big_box.get()));
  EXPECT_TRUE(RelateNG::intersects(gc_line_poly.get(), big_box.get()));

  // computeAreaVertex early exits on exterior ring and interior ring.
  auto poly_with_hole = ReadWkt(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), (5 5, 15 5, 15 15, 5 15, 5 5))");
  auto hole_inner = ReadWkt("POLYGON ((7 7, 13 7, 13 13, 7 13, 7 7))");
  auto cover_hole = ReadWkt("POLYGON ((2 2, 18 2, 18 18, 2 18, 2 2))");
  EXPECT_FALSE(RelateNG::within(hole_inner.get(), poly_with_hole.get()));
  EXPECT_FALSE(RelateNG::contains(poly_with_hole.get(), cover_hole.get()));

  // computeAtEdges early exit before evaluateNodes() on edge intersection.
  auto cross_h = ReadWkt("LINESTRING (0 5, 10 5)");
  auto cross_v = ReadWkt("LINESTRING (5 0, 5 10)");
  EXPECT_TRUE(RelateNG::intersects(cross_h.get(), cross_v.get()));
  EXPECT_FALSE(RelateNG::touches(cross_h.get(), cross_v.get()));
}

TEST_F(RelateNGTest, PreparedModeIndexReuseAcrossMultipleTargets) {
  // Reuse prepared Polygon edge index across diverse subsequent targets.
  auto poly_a = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto prep_poly = RelateNG::prepare(poly_a.get());

  auto disjoint_poly = ReadWkt("POLYGON ((20 20, 25 20, 25 25, 20 25, 20 20))");
  auto overlap_poly = ReadWkt("POLYGON ((5 5, 15 5, 15 15, 5 15, 5 5))");
  auto touch_poly = ReadWkt("POLYGON ((10 0, 15 0, 15 10, 10 10, 10 0))");
  auto inner_poly = ReadWkt("POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))");
  auto cross_line = ReadWkt("LINESTRING (-5 5, 15 5)");
  auto inner_pt = ReadWkt("POINT (5 5)");

  EXPECT_FALSE(prep_poly->intersects(disjoint_poly.get()));
  EXPECT_EQ(prep_poly->relate(disjoint_poly.get())->toString(), "FF2FF1212");
  // First edge intersection builds edgeMutualInt.
  EXPECT_TRUE(prep_poly->intersects(overlap_poly.get()));
  // Subsequent calls reuse edgeMutualInt.
  EXPECT_TRUE(prep_poly->overlaps(overlap_poly.get()));
  EXPECT_TRUE(prep_poly->touches(touch_poly.get()));
  EXPECT_TRUE(prep_poly->contains(inner_poly.get()));
  EXPECT_TRUE(prep_poly->crosses(cross_line.get()));
  EXPECT_TRUE(prep_poly->contains(inner_pt.get()));
  EXPECT_EQ(prep_poly->relate(overlap_poly.get())->toString(), "212101212");

  // Reuse prepared LineString across mutual and self-noded queries.
  auto line_a = ReadWkt("LINESTRING (0 0, 10 0, 10 10)");
  auto prep_line = RelateNG::prepare(line_a.get());
  auto line_b = ReadWkt("LINESTRING (5 -5, 5 5)");
  auto line_c = ReadWkt("LINESTRING (0 5, 15 5)");
  auto line_d = ReadWkt("LINESTRING (0 0, 10 0)");

  EXPECT_TRUE(prep_line->intersects(line_b.get()));
  EXPECT_TRUE(prep_line->intersects(line_c.get()));
  EXPECT_TRUE(prep_line->crosses(line_b.get()));
  EXPECT_TRUE(prep_line->covers(line_d.get()));
  EXPECT_EQ(prep_line->relate(line_c.get())->toString(), "0F1FF0102");

  // Reuse prepared MultiPoint and mixed GeometryCollection.
  auto mp = ReadWkt("MULTIPOINT ((0 0), (5 5), (10 10))");
  auto prep_mp = RelateNG::prepare(mp.get());
  EXPECT_TRUE(prep_mp->contains(inner_pt.get()));
  EXPECT_TRUE(prep_mp->within(poly_a.get()));
  EXPECT_FALSE(prep_mp->disjoint(cross_line.get()));

  auto gc = ReadWkt(
      "GEOMETRYCOLLECTION ("
      "  POINT (20 20),"
      "  LINESTRING (5 5, 15 5),"
      "  POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)))");
  auto prep_gc = RelateNG::prepare(gc.get());
  EXPECT_TRUE(prep_gc->intersects(overlap_poly.get()));
  EXPECT_TRUE(prep_gc->contains(inner_pt.get()));
  EXPECT_FALSE(prep_gc->disjoint(cross_line.get()));
}

TEST_F(RelateNGTest, CurvedGeometriesAndInvalidPatternsThrow) {
  auto pt = ReadWkt("POINT (0 0)");
  auto circular = ReadWkt("CIRCULARSTRING (0 0, 1 1, 2 0)");
  auto compound =
      ReadWkt("COMPOUNDCURVE ((0 0, 1 1), CIRCULARSTRING (1 1, 2 0, 3 1))");
  auto curve_poly =
      ReadWkt("CURVEPOLYGON (CIRCULARSTRING (0 0, 2 2, 4 0, 2 -2, 0 0))");

  EXPECT_THROW(RelateNG::relate(circular.get(), pt.get()),
               UnsupportedOperationException);
  EXPECT_THROW(RelateNG::relate(pt.get(), circular.get()),
               UnsupportedOperationException);
  EXPECT_THROW(RelateNG::intersects(compound.get(), pt.get()),
               UnsupportedOperationException);
  EXPECT_THROW(RelateNG::intersects(pt.get(), curve_poly.get()),
               UnsupportedOperationException);

  auto prep_circular = RelateNG::prepare(circular.get());
  EXPECT_THROW(prep_circular->relate(pt.get()), UnsupportedOperationException);

  auto gc_curved = ReadWkt(
      "GEOMETRYCOLLECTION (POINT (0 0), CIRCULARSTRING (0 0, 1 1, 2 0))");
  auto multi_curve =
      ReadWkt("MULTICURVE ((0 0, 1 1), CIRCULARSTRING (1 1, 2 0, 3 1))");
  EXPECT_THROW(RelateNG::relate(gc_curved.get(), pt.get()),
               UnsupportedOperationException);
  EXPECT_THROW(RelateNG::intersects(pt.get(), multi_curve.get()),
               UnsupportedOperationException);

  auto pt_b = ReadWkt("POINT (1 1)");
  EXPECT_THROW(RelateNG::relate(pt.get(), pt_b.get(), "BAD"),
               IllegalArgumentException);
  EXPECT_THROW(RelateNG::relate(pt.get(), pt_b.get(), "12345678X"),
               IllegalArgumentException);
}

TEST_F(RelateNGTest, ZeroLengthLineAdvancedCases) {
  // MultiLineString of zero-length lines has effective dimension P.
  auto zero_mline = ReadWkt("MULTILINESTRING ((5 5, 5 5), (2 2, 2 2))");
  auto mpt = ReadWkt("MULTIPOINT ((5 5), (2 2))");
  EXPECT_TRUE(RelateNG::equalsTopo(zero_mline.get(), mpt.get()));
  EXPECT_EQ(RelateNG::relate(zero_mline.get(), mpt.get())->toString(),
            "0FFFFFFF2");

  // Mixed collection of Point + zero-length LineString also reduces to P.
  auto zero_gc =
      ReadWkt("GEOMETRYCOLLECTION (POINT (0 0), LINESTRING (5 5, 5 5))");
  auto mpt2 = ReadWkt("MULTIPOINT ((0 0), (5 5))");
  EXPECT_TRUE(RelateNG::equalsTopo(zero_gc.get(), mpt2.get()));
  EXPECT_EQ(RelateNG::relate(zero_gc.get(), mpt2.get())->toString(),
            "0FFFFFFF2");

  // Zero-length LineString in the interior of a non-degenerate LineString.
  auto zero_line = ReadWkt("LINESTRING (5 5, 5 5)");
  auto diag = ReadWkt("LINESTRING (0 0, 10 10)");
  EXPECT_TRUE(RelateNG::within(zero_line.get(), diag.get()));
  EXPECT_FALSE(RelateNG::touches(zero_line.get(), diag.get()));
  EXPECT_EQ(RelateNG::relate(zero_line.get(), diag.get())->toString(),
            "0FFFFF102");

  // Zero-length LineString at the boundary of a non-degenerate LineString:
  // full relate/touches/coveredBy evaluate the line endpoint against the target
  // line, whereas intersects/disjoint skip computeLineEnds when the target has
  // no area and removeRepeated collapses the zero-length segment to 1 vertex.
  auto zero_origin = ReadWkt("LINESTRING (0 0, 0 0)");
  auto seg_origin = ReadWkt("LINESTRING (0 0, 10 0)");
  EXPECT_EQ(RelateNG::relate(zero_origin.get(), seg_origin.get())->toString(),
            "F0FFFF102");
  EXPECT_TRUE(RelateNG::touches(zero_origin.get(), seg_origin.get()));
  EXPECT_TRUE(RelateNG::coveredBy(zero_origin.get(), seg_origin.get()));
  EXPECT_FALSE(RelateNG::within(zero_origin.get(), seg_origin.get()));
  EXPECT_FALSE(RelateNG::equalsTopo(zero_origin.get(), seg_origin.get()));
  EXPECT_FALSE(RelateNG::intersects(zero_origin.get(), seg_origin.get()));
  EXPECT_TRUE(RelateNG::disjoint(zero_origin.get(), seg_origin.get()));

  auto prep_zero = RelateNG::prepare(zero_origin.get());
  EXPECT_EQ(prep_zero->relate(seg_origin.get())->toString(), "F0FFFF102");
  EXPECT_TRUE(prep_zero->touches(seg_origin.get()));
  EXPECT_FALSE(prep_zero->intersects(seg_origin.get()));
  EXPECT_TRUE(prep_zero->disjoint(seg_origin.get()));

  // Zero-length LineString against a Polygon triggers computeLineEnds because
  // the target has areas, so intersects() and touches()/within() agree.
  auto poly = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  EXPECT_TRUE(RelateNG::intersects(zero_origin.get(), poly.get()));
  EXPECT_TRUE(RelateNG::touches(zero_origin.get(), poly.get()));
  EXPECT_TRUE(RelateNG::coveredBy(zero_origin.get(), poly.get()));
  EXPECT_EQ(RelateNG::relate(zero_origin.get(), poly.get())->toString(),
            "F0FFFF212");

  EXPECT_TRUE(RelateNG::intersects(zero_line.get(), poly.get()));
  EXPECT_TRUE(RelateNG::within(zero_line.get(), poly.get()));
  EXPECT_TRUE(RelateNG::coveredBy(zero_line.get(), poly.get()));
  EXPECT_FALSE(RelateNG::touches(zero_line.get(), poly.get()));
  // TopologyComputer::addLineEndOnArea sets INTERIOR/INTERIOR to Dimension::L
  // even for a closed zero-length LineString in a Polygon interior.
  EXPECT_EQ(RelateNG::relate(zero_line.get(), poly.get())->toString(),
            "1FFFFF212");

  // LinearRing (closed curve) strictly inside a Polygon.
  auto inner_ring = ReadWkt("LINEARRING (2 2, 8 2, 8 8, 2 8, 2 2)");
  EXPECT_TRUE(RelateNG::within(inner_ring.get(), poly.get()));
  EXPECT_TRUE(RelateNG::intersects(inner_ring.get(), poly.get()));
  EXPECT_EQ(RelateNG::relate(inner_ring.get(), poly.get())->toString(),
            "1FFFFF212");
}

TEST_F(RelateNGTest, PolygonWithMultipleHolesAndPreparedIndex) {
  auto poly_two_holes = ReadWkt(
      "POLYGON ("
      "  (0 0, 30 0, 30 20, 0 20, 0 0),"
      "  (2 2, 8 2, 8 8, 2 8, 2 2),"
      "  (12 2, 18 2, 18 8, 12 8, 12 2))");
  auto box_second_hole_corner =
      ReadWkt("POLYGON ((11 1, 13 1, 13 3, 11 3, 11 1))");
  auto hole0_inner_box = ReadWkt("POLYGON ((3 3, 7 3, 7 7, 3 7, 3 3))");
  auto pt_in_poly = ReadWkt("POINT (25 10)");
  auto pt_in_hole0 = ReadWkt("POINT (5 5)");

  // Exterior ring and first hole do not intersect box_second_hole_corner;
  // the second interior ring vertex triggers early intersection detection.
  EXPECT_TRUE(
      RelateNG::intersects(box_second_hole_corner.get(), poly_two_holes.get()));
  EXPECT_TRUE(
      RelateNG::overlaps(box_second_hole_corner.get(), poly_two_holes.get()));
  EXPECT_EQ(RelateNG::relate(box_second_hole_corner.get(), poly_two_holes.get())
                ->toString(),
            "212101212");

  auto prep_holes = RelateNG::prepare(poly_two_holes.get());
  EXPECT_TRUE(prep_holes->disjoint(hole0_inner_box.get()));
  EXPECT_EQ(prep_holes->relate(hole0_inner_box.get())->toString(), "FF2FF1212");
  EXPECT_TRUE(prep_holes->overlaps(box_second_hole_corner.get()));
  EXPECT_TRUE(prep_holes->contains(pt_in_poly.get()));
  EXPECT_TRUE(prep_holes->disjoint(pt_in_hole0.get()));
}

TEST_F(RelateNGTest, AdjacentAndNestedCollectionsUnionSemantics) {
  // Adjacent polygons in a GeometryCollection fuse across their shared edge.
  auto adj_gc = ReadWkt(
      "GEOMETRYCOLLECTION ("
      "  POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0)),"
      "  POLYGON ((5 0, 10 0, 10 5, 5 5, 5 0)))");
  auto merged = ReadWkt("POLYGON ((0 0, 10 0, 10 5, 0 5, 0 0))");
  auto seam_line = ReadWkt("LINESTRING (5 1, 5 4)");
  EXPECT_TRUE(RelateNG::equalsTopo(adj_gc.get(), merged.get()));
  EXPECT_EQ(RelateNG::relate(adj_gc.get(), merged.get())->toString(),
            "2FFF1FFF2");
  EXPECT_TRUE(RelateNG::contains(adj_gc.get(), seam_line.get()));

  // Nested GeometryCollection behaves identically to flattened collection.
  auto nested_gc = ReadWkt(
      "GEOMETRYCOLLECTION ("
      "  GEOMETRYCOLLECTION (POINT (20 20), LINESTRING (5 5, 15 5)),"
      "  POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)))");
  auto poly_b = ReadWkt("POLYGON ((12 0, 20 0, 20 10, 12 10, 12 0))");
  EXPECT_EQ(RelateNG::relate(nested_gc.get(), poly_b.get())->toString(),
            "1020F1212");
  EXPECT_TRUE(RelateNG::overlaps(nested_gc.get(), poly_b.get()));
}

}  // namespace
}  // namespace geos_google
