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

// Unit tests for third_party/geos/src/simplify/TaggedLinesSimplifier.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/simplify/TaggedLinesSimplifier.cpp

#include "third_party/geos/include/geos/simplify/TaggedLinesSimplifier.h"

#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/simplify/TaggedLineString.h"

namespace geos_google {
namespace {

using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::simplify::TaggedLinesSimplifier;
using geos::simplify::TaggedLineString;

class TaggedLinesSimplifierTest : public ::testing::Test {
 protected:
  std::unique_ptr<LineString> ReadLineString(std::string_view wkt) {
    return reader_.read<LineString>(std::string(wkt));
  }

  std::unique_ptr<LinearRing> ReadLinearRing(std::string_view wkt) {
    return reader_.read<LinearRing>(std::string(wkt));
  }

  std::unique_ptr<Polygon> RingToPolygon(std::unique_ptr<Geometry> ring_geom) {
    auto* lr = dynamic_cast<LinearRing*>(ring_geom.get());
    if (lr == nullptr) {
      return nullptr;
    }
    ring_geom.release();
    return factory_->createPolygon(std::unique_ptr<LinearRing>(lr));
  }

  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{factory_.get()};
};

TEST_F(TaggedLinesSimplifierTest, EmptyCollectionIsNoOp) {
  TaggedLinesSimplifier simplifier;
  simplifier.setDistanceTolerance(1.0);

  std::vector<TaggedLineString*> empty_lines;
  simplifier.simplify(empty_lines);
  EXPECT_TRUE(empty_lines.empty());
}

TEST_F(TaggedLinesSimplifierTest, EmptyTaggedLineStringProducesEmptyResult) {
  std::unique_ptr<LineString> empty_line = factory_->createLineString();
  TaggedLineString tagged(empty_line.get(), /*minimumSize=*/2,
                          /*bIsRing=*/false);
  std::vector<TaggedLineString*> lines{&tagged};

  TaggedLinesSimplifier simplifier;
  simplifier.setDistanceTolerance(1.0);
  simplifier.simplify(lines);

  EXPECT_EQ(tagged.getResultSize(), 0U);
  std::unique_ptr<Geometry> result = tagged.asLineString();
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isEmpty());
}

TEST_F(TaggedLinesSimplifierTest,
       DefaultZeroToleranceSimplifiesCollinearPointsOnly) {
  std::unique_ptr<LineString> collinear =
      ReadLineString("LINESTRING (0 0, 1 0, 2 0, 5 0)");
  std::unique_ptr<LineString> slightly_bent =
      ReadLineString("LINESTRING (0 10, 2 10.000001, 5 10)");

  TaggedLineString tagged_collinear(collinear.get(), 2, false);
  TaggedLineString tagged_bent(slightly_bent.get(), 2, false);
  std::vector<TaggedLineString*> lines{&tagged_collinear, &tagged_bent};

  // Default tolerance is 0.0 without calling setDistanceTolerance.
  TaggedLinesSimplifier simplifier;
  simplifier.simplify(lines);

  std::unique_ptr<LineString> expected_collinear =
      ReadLineString("LINESTRING (0 0, 5 0)");
  std::unique_ptr<Geometry> out_collinear = tagged_collinear.asLineString();
  std::unique_ptr<Geometry> out_bent = tagged_bent.asLineString();

  EXPECT_EQ(tagged_collinear.getResultSize(), 2U);
  EXPECT_TRUE(out_collinear->equalsExact(expected_collinear.get()));
  EXPECT_EQ(tagged_bent.getResultSize(), 3U);
  EXPECT_TRUE(out_bent->equalsExact(slightly_bent.get()));
}

TEST_F(TaggedLinesSimplifierTest,
       OpenLineSimplifiesWithinToleranceAndHonorsMinSizeTwo) {
  std::unique_ptr<LineString> line =
      ReadLineString("LINESTRING (0 0, 1.5 1.05, 3 2, 4.5 0.95, 6 0)");

  {
    TaggedLineString tagged(line.get(), 2, false);
    std::vector<TaggedLineString*> lines{&tagged};
    TaggedLinesSimplifier simplifier;
    simplifier.setDistanceTolerance(0.25);
    simplifier.simplify(lines);

    std::unique_ptr<LineString> expected =
        ReadLineString("LINESTRING (0 0, 3 2, 6 0)");
    EXPECT_EQ(tagged.getResultSize(), 3U);
    EXPECT_TRUE(tagged.asLineString()->equalsExact(expected.get()));
  }

  {
    TaggedLineString tagged(line.get(), 2, false);
    std::vector<TaggedLineString*> lines{&tagged};
    TaggedLinesSimplifier simplifier;
    simplifier.setDistanceTolerance(5.0);
    simplifier.simplify(lines);

    std::unique_ptr<LineString> expected =
        ReadLineString("LINESTRING (0 0, 6 0)");
    EXPECT_EQ(tagged.getResultSize(), 2U);
    EXPECT_TRUE(tagged.asLineString()->equalsExact(expected.get()));
  }
}

TEST_F(TaggedLinesSimplifierTest,
       RingHonorsMinimumSizeFourAndSimplifiesEndpoint) {
  std::unique_ptr<LinearRing> ring =
      ReadLinearRing("LINEARRING (5 0.05, 10 0, 10 10, 0 10, 0 0, 5 0.05)");

  {
    TaggedLineString tagged(ring.get(), /*minimumSize=*/4, /*bIsRing=*/true);
    std::vector<TaggedLineString*> lines{&tagged};
    TaggedLinesSimplifier simplifier;
    simplifier.setDistanceTolerance(0.2);
    simplifier.simplify(lines);

    std::unique_ptr<LinearRing> expected =
        ReadLinearRing("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)");
    std::unique_ptr<Geometry> out = tagged.asLinearRing();
    EXPECT_EQ(tagged.getResultSize(), 5U);
    EXPECT_TRUE(out->isValid());
    EXPECT_TRUE(out->equalsExact(expected.get()));
  }

  {
    TaggedLineString tagged(ring.get(), /*minimumSize=*/4, /*bIsRing=*/true);
    std::vector<TaggedLineString*> lines{&tagged};
    TaggedLinesSimplifier simplifier;
    simplifier.setDistanceTolerance(100.0);
    simplifier.simplify(lines);

    std::unique_ptr<Geometry> out = tagged.asLinearRing();
    EXPECT_EQ(tagged.getResultSize(), 4U);
    EXPECT_EQ(out->getNumPoints(), 4U);
    EXPECT_TRUE(out->isValid());
  }
}

TEST_F(TaggedLinesSimplifierTest, PreventsSelfIntersectionOnFoldedLine) {
  // With tolerance 1.0, (10, 0) simplifies away, and (5, 0.8) lies within 0.8
  // of the chord (0, 0)-(12, 0). Flattening across (0, 0)-(12, 0) would cross
  // (5, -10)-(5, 0.4), so topology preservation retains (5, 0.8).
  {
    std::unique_ptr<LineString> folded = ReadLineString(
        "LINESTRING (0 0, 5 0.8, 10 0, 12 0, 12 -10, 5 -10, 5 0.4)");
    ASSERT_TRUE(folded->isSimple());

    TaggedLineString tagged(folded.get(), 2, false);
    std::vector<TaggedLineString*> lines{&tagged};
    TaggedLinesSimplifier simplifier;
    simplifier.setDistanceTolerance(1.0);
    simplifier.simplify(lines);

    std::unique_ptr<Geometry> out = tagged.asLineString();
    std::unique_ptr<LineString> expected =
        ReadLineString("LINESTRING (0 0, 5 0.8, 12 0, 12 -10, 5 -10, 5 0.4)");
    EXPECT_TRUE(out->isSimple());
    EXPECT_EQ(out->getNumPoints(), 6U);
    EXPECT_TRUE(out->equalsExact(expected.get()));
  }

  // On a tight spiral at tolerance 8.0, section [2, 8] collapses cleanly to
  // (10, 10)-(4.5, 3.5), while section [0, 2] retains (10, 0) because the chord
  // (0, 0)-(10, 10) would cross (4, 8)-(4.5, 3.5).
  {
    std::unique_ptr<LineString> spiral = ReadLineString(
        "LINESTRING (0 0, 10 0, 10 10, 2 10, 2 2, 8 2, 8 8, 4 8, 4.5 3.5)");
    ASSERT_TRUE(spiral->isSimple());

    TaggedLineString tagged(spiral.get(), 2, false);
    std::vector<TaggedLineString*> lines{&tagged};
    TaggedLinesSimplifier simplifier;
    simplifier.setDistanceTolerance(8.0);
    simplifier.simplify(lines);

    std::unique_ptr<Geometry> out = tagged.asLineString();
    std::unique_ptr<LineString> expected =
        ReadLineString("LINESTRING (0 0, 10 0, 10 10, 4.5 3.5)");
    EXPECT_TRUE(out->isSimple());
    EXPECT_EQ(out->getNumPoints(), 4U);
    EXPECT_TRUE(out->equalsExact(expected.get()));
  }
}

TEST_F(TaggedLinesSimplifierTest,
       PreventsNewIntersectionsBetweenSeparateLinesInBothOrders) {
  // line_b crosses the baseline (0,0)-(10,0) of line_a while staying disjoint
  // from line_a itself, so flattening line_a across y=0 is invalid regardless
  // of which line is processed first.
  std::unique_ptr<LineString> line_a =
      ReadLineString("LINESTRING (0 0, 5 4, 10 0)");
  std::unique_ptr<LineString> line_b =
      ReadLineString("LINESTRING (5 3, 5 1, 5 -1)");
  ASSERT_TRUE(line_a->disjoint(line_b.get()));

  {
    TaggedLineString tagged_a(line_a.get(), 2, false);
    TaggedLineString tagged_b(line_b.get(), 2, false);
    std::vector<TaggedLineString*> lines{&tagged_a, &tagged_b};

    TaggedLinesSimplifier simplifier;
    simplifier.setDistanceTolerance(100.0);
    simplifier.simplify(lines);

    std::unique_ptr<Geometry> out_a = tagged_a.asLineString();
    std::unique_ptr<Geometry> out_b = tagged_b.asLineString();
    EXPECT_TRUE(out_a->disjoint(out_b.get()));
    EXPECT_TRUE(out_a->equalsExact(line_a.get()));
    EXPECT_EQ(out_b->getNumPoints(), 2U);
  }

  {
    TaggedLineString tagged_a(line_a.get(), 2, false);
    TaggedLineString tagged_b(line_b.get(), 2, false);
    std::vector<TaggedLineString*> lines{&tagged_b, &tagged_a};

    TaggedLinesSimplifier simplifier;
    simplifier.setDistanceTolerance(100.0);
    simplifier.simplify(lines);

    std::unique_ptr<Geometry> out_a = tagged_a.asLineString();
    std::unique_ptr<Geometry> out_b = tagged_b.asLineString();
    EXPECT_TRUE(out_a->disjoint(out_b.get()));
    EXPECT_TRUE(out_a->equalsExact(line_a.get()));
    EXPECT_EQ(out_b->getNumPoints(), 2U);
  }
}

TEST_F(TaggedLinesSimplifierTest, PreventsComponentJumpOverNestedComponent) {
  std::unique_ptr<LinearRing> outer_ring = ReadLinearRing(
      "LINEARRING (0 0, 10 0, 10 10, 6 10, 6 2, 4 2, 4 10, 0 10, 0 0)");
  std::unique_ptr<LinearRing> inner_ring =
      ReadLinearRing("LINEARRING (4.5 5, 5.5 5, 5.5 6, 4.5 6, 4.5 5)");

  TaggedLineString tagged_outer(outer_ring.get(), 4, true);
  TaggedLineString tagged_inner(inner_ring.get(), 4, true);
  std::vector<TaggedLineString*> lines{&tagged_outer, &tagged_inner};

  TaggedLinesSimplifier simplifier;
  simplifier.setDistanceTolerance(10.0);
  simplifier.simplify(lines);

  std::unique_ptr<Polygon> outer_poly =
      RingToPolygon(tagged_outer.asLinearRing());
  std::unique_ptr<Polygon> inner_poly =
      RingToPolygon(tagged_inner.asLinearRing());
  ASSERT_NE(outer_poly, nullptr);
  ASSERT_NE(inner_poly, nullptr);
  EXPECT_TRUE(outer_poly->isValid());
  EXPECT_TRUE(inner_poly->isValid());
  EXPECT_TRUE(outer_poly->disjoint(inner_poly.get()));
  EXPECT_FALSE(outer_poly->contains(inner_poly.get()));
}

TEST_F(TaggedLinesSimplifierTest, NegativeTolerancePreservesAllVertices) {
  std::unique_ptr<LineString> line =
      ReadLineString("LINESTRING (0 0, 1 0, 2 0, 3 0)");
  TaggedLineString tagged(line.get(), 2, false);
  std::vector<TaggedLineString*> lines{&tagged};

  TaggedLinesSimplifier simplifier;
  simplifier.setDistanceTolerance(-1.0);
  simplifier.simplify(lines);

  EXPECT_EQ(tagged.getResultSize(), 4U);
  EXPECT_TRUE(tagged.asLineString()->equalsExact(line.get()));
}

TEST_F(TaggedLinesSimplifierTest, PreservesZCoordinates) {
  std::unique_ptr<LineString> line_z =
      ReadLineString("LINESTRING Z (0 0 10, 5 0.05 99, 10 0 25)");
  TaggedLineString tagged(line_z.get(), 2, false);
  std::vector<TaggedLineString*> lines{&tagged};

  TaggedLinesSimplifier simplifier;
  simplifier.setDistanceTolerance(0.2);
  simplifier.simplify(lines);

  std::unique_ptr<Geometry> out_geom = tagged.asLineString();
  auto* out = dynamic_cast<LineString*>(out_geom.get());
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->getNumPoints(), 2U);
  EXPECT_TRUE(out->hasZ());
  EXPECT_DOUBLE_EQ(out->getCoordinateN(0).z, 10.0);
  EXPECT_DOUBLE_EQ(out->getCoordinateN(1).z, 25.0);
}

TEST_F(TaggedLinesSimplifierTest, TwoPointLineRemainsUnchanged) {
  std::unique_ptr<LineString> line = ReadLineString("LINESTRING (0 0, 10 0)");
  TaggedLineString tagged(line.get(), /*minimumSize=*/2, /*bIsRing=*/false);
  std::vector<TaggedLineString*> lines{&tagged};

  TaggedLinesSimplifier simplifier;
  simplifier.setDistanceTolerance(100.0);
  simplifier.simplify(lines);

  EXPECT_EQ(tagged.getResultSize(), 2U);
  EXPECT_TRUE(tagged.asLineString()->equalsExact(line.get()));
}

TEST_F(TaggedLinesSimplifierTest,
       CollinearRingEndpointIsSimplifiedAtZeroTolerance) {
  std::unique_ptr<LinearRing> ring =
      ReadLinearRing("LINEARRING (5 0, 10 0, 10 10, 0 10, 0 0, 5 0)");
  TaggedLineString tagged(ring.get(), /*minimumSize=*/4, /*bIsRing=*/true);
  std::vector<TaggedLineString*> lines{&tagged};

  TaggedLinesSimplifier simplifier;
  simplifier.setDistanceTolerance(0.0);
  simplifier.simplify(lines);

  std::unique_ptr<LinearRing> expected =
      ReadLinearRing("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  std::unique_ptr<Geometry> out = tagged.asLinearRing();
  EXPECT_EQ(tagged.getResultSize(), 5U);
  EXPECT_TRUE(out->isValid());
  EXPECT_TRUE(out->equalsExact(expected.get()));
}

TEST_F(TaggedLinesSimplifierTest,
       ClosedLineStringWithMinSizeFourPreservesClosure) {
  std::unique_ptr<LineString> closed_line =
      ReadLineString("LINESTRING (0 0, 5 0, 10 0, 5 10, 0 0)");
  TaggedLineString tagged(closed_line.get(), /*minimumSize=*/4,
                          /*bIsRing=*/false);
  std::vector<TaggedLineString*> lines{&tagged};

  TaggedLinesSimplifier simplifier;
  simplifier.setDistanceTolerance(100.0);
  simplifier.simplify(lines);

  std::unique_ptr<LineString> expected =
      ReadLineString("LINESTRING (0 0, 10 0, 5 10, 0 0)");
  std::unique_ptr<Geometry> out_geom = tagged.asLineString();
  auto* out = dynamic_cast<LineString*>(out_geom.get());
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->getNumPoints(), 4U);
  EXPECT_TRUE(out->isClosed());
  EXPECT_TRUE(out->equalsExact(expected.get()));
}

TEST_F(TaggedLinesSimplifierTest, OverwritingDistanceToleranceUsesLatestValue) {
  std::unique_ptr<LineString> line =
      ReadLineString("LINESTRING (0 0, 5 0.5, 10 0)");
  TaggedLineString tagged(line.get(), /*minimumSize=*/2, /*bIsRing=*/false);
  std::vector<TaggedLineString*> lines{&tagged};

  TaggedLinesSimplifier simplifier;
  simplifier.setDistanceTolerance(1.0);
  simplifier.setDistanceTolerance(0.1);
  simplifier.simplify(lines);

  EXPECT_EQ(tagged.getResultSize(), 3U);
  EXPECT_TRUE(tagged.asLineString()->equalsExact(line.get()));
}

TEST_F(TaggedLinesSimplifierTest,
       InfiniteToleranceSimplifiesOpenLineToEndpoints) {
  std::unique_ptr<LineString> line =
      ReadLineString("LINESTRING (0 0, 3 5, 6 -4, 10 0)");
  TaggedLineString tagged(line.get(), /*minimumSize=*/2, /*bIsRing=*/false);
  std::vector<TaggedLineString*> lines{&tagged};

  TaggedLinesSimplifier simplifier;
  simplifier.setDistanceTolerance(std::numeric_limits<double>::infinity());
  simplifier.simplify(lines);

  std::unique_ptr<LineString> expected =
      ReadLineString("LINESTRING (0 0, 10 0)");
  EXPECT_EQ(tagged.getResultSize(), 2U);
  EXPECT_TRUE(tagged.asLineString()->equalsExact(expected.get()));
}

}  // namespace
}  // namespace geos_google
