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
// third_party/geos/src/simplify/TopologyPreservingSimplifier.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/simplify/TopologyPreservingSimplifier.cpp

#include "third_party/geos/include/geos/simplify/TopologyPreservingSimplifier.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::GeometryTypeId;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::MultiLineString;
using geos::geom::MultiPolygon;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::simplify::TopologyPreservingSimplifier;
using geos::util::IllegalArgumentException;

class TopologyPreservingSimplifierTest : public ::testing::Test {
 protected:
  std::unique_ptr<Geometry> Read(std::string_view wkt) {
    return reader_.read(std::string(wkt));
  }

  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{factory_.get()};
};

TEST_F(TopologyPreservingSimplifierTest, ThrowsOnNegativeDistanceTolerance) {
  std::unique_ptr<Geometry> line = Read("LINESTRING (0 0, 1 1, 2 0)");

  TopologyPreservingSimplifier simplifier(line.get());
  EXPECT_THROW(simplifier.setDistanceTolerance(-1.0), IllegalArgumentException);
  EXPECT_THROW(simplifier.setDistanceTolerance(-1e-12),
               IllegalArgumentException);
  EXPECT_THROW(TopologyPreservingSimplifier::simplify(line.get(), -0.5),
               IllegalArgumentException);
}

TEST_F(TopologyPreservingSimplifierTest,
       EmptyGeometriesReturnEmptyCloneOfSameType) {
  struct EmptyCase {
    std::string_view name;
    std::string_view wkt;
    GeometryTypeId expected_type;
  };

  constexpr auto kEmptyCases = std::to_array<EmptyCase>({
      {.name = "point",
       .wkt = "POINT EMPTY",
       .expected_type = geos::geom::GEOS_POINT},
      {.name = "linestring",
       .wkt = "LINESTRING EMPTY",
       .expected_type = geos::geom::GEOS_LINESTRING},
      {.name = "linearring",
       .wkt = "LINEARRING EMPTY",
       .expected_type = geos::geom::GEOS_LINEARRING},
      {.name = "polygon",
       .wkt = "POLYGON EMPTY",
       .expected_type = geos::geom::GEOS_POLYGON},
      {.name = "multipoint",
       .wkt = "MULTIPOINT EMPTY",
       .expected_type = geos::geom::GEOS_MULTIPOINT},
      {.name = "multilinestring",
       .wkt = "MULTILINESTRING EMPTY",
       .expected_type = geos::geom::GEOS_MULTILINESTRING},
      {.name = "multipolygon",
       .wkt = "MULTIPOLYGON EMPTY",
       .expected_type = geos::geom::GEOS_MULTIPOLYGON},
      {.name = "geometrycollection",
       .wkt = "GEOMETRYCOLLECTION EMPTY",
       .expected_type = geos::geom::GEOS_GEOMETRYCOLLECTION},
  });

  for (const auto& c : kEmptyCases) {
    SCOPED_TRACE(c.name);
    std::unique_ptr<Geometry> input = Read(c.wkt);
    ASSERT_TRUE(input->isEmpty());

    std::unique_ptr<Geometry> result =
        TopologyPreservingSimplifier::simplify(input.get(), 2.0);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->isEmpty());
    EXPECT_EQ(result->getGeometryTypeId(), c.expected_type);
    EXPECT_NE(result.get(), input.get());
  }
}

TEST_F(TopologyPreservingSimplifierTest,
       DefaultAndZeroTolerancePreserveNonCollinearAndDropCollinear) {
  std::unique_ptr<Geometry> bent = Read("LINESTRING (0 0, 1 0.1, 2 0)");
  std::unique_ptr<Geometry> collinear = Read("LINESTRING (0 0, 1 0, 2 0)");
  std::unique_ptr<Geometry> expected_collinear = Read("LINESTRING (0 0, 2 0)");

  // Default tolerance (without calling setDistanceTolerance) is 0.0.
  TopologyPreservingSimplifier bent_simplifier(bent.get());
  std::unique_ptr<Geometry> bent_default = bent_simplifier.getResultGeometry();
  EXPECT_TRUE(bent_default->equalsExact(bent.get()));

  TopologyPreservingSimplifier collinear_simplifier(collinear.get());
  std::unique_ptr<Geometry> collinear_default =
      collinear_simplifier.getResultGeometry();
  EXPECT_TRUE(collinear_default->equalsExact(expected_collinear.get()));

  std::unique_ptr<Geometry> bent_zero =
      TopologyPreservingSimplifier::simplify(bent.get(), 0.0);
  std::unique_ptr<Geometry> collinear_zero =
      TopologyPreservingSimplifier::simplify(collinear.get(), 0.0);
  EXPECT_TRUE(bent_zero->equalsExact(bent.get()));
  EXPECT_TRUE(collinear_zero->equalsExact(expected_collinear.get()));
}

TEST_F(TopologyPreservingSimplifierTest,
       PointsAndMultiPointsArePreservedUnchanged) {
  std::unique_ptr<Geometry> pt = Read("POINT (3 4)");
  std::unique_ptr<Geometry> mpt =
      Read("MULTIPOINT ((0 0), (0.1 0.1), (0.2 0))");

  std::unique_ptr<Geometry> pt_out =
      TopologyPreservingSimplifier::simplify(pt.get(), 10.0);
  std::unique_ptr<Geometry> mpt_out =
      TopologyPreservingSimplifier::simplify(mpt.get(), 10.0);

  EXPECT_EQ(pt_out->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(pt_out->equalsExact(pt.get()));
  EXPECT_EQ(mpt_out->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_TRUE(mpt_out->equalsExact(mpt.get()));
}

TEST_F(TopologyPreservingSimplifierTest,
       OpenLineStringSimplifiesWithinToleranceAndHonorsMinSizeTwo) {
  std::unique_ptr<Geometry> line =
      Read("LINESTRING (0 0, 1.5 1.05, 3 2, 4.5 0.95, 6 0)");

  std::unique_ptr<Geometry> mid_tol =
      TopologyPreservingSimplifier::simplify(line.get(), 0.25);
  std::unique_ptr<Geometry> expected_mid = Read("LINESTRING (0 0, 3 2, 6 0)");
  EXPECT_EQ(mid_tol->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(mid_tol->equalsExact(expected_mid.get()));

  std::unique_ptr<Geometry> high_tol =
      TopologyPreservingSimplifier::simplify(line.get(), 10.0);
  std::unique_ptr<Geometry> expected_high = Read("LINESTRING (0 0, 6 0)");
  EXPECT_EQ(high_tol->getNumPoints(), 2U);
  EXPECT_TRUE(high_tol->equalsExact(expected_high.get()));

  std::unique_ptr<Geometry> segment = Read("LINESTRING (0 0, 5 0)");
  std::unique_ptr<Geometry> seg_out =
      TopologyPreservingSimplifier::simplify(segment.get(), 10.0);
  EXPECT_TRUE(seg_out->equalsExact(segment.get()));
}

TEST_F(TopologyPreservingSimplifierTest,
       ClosedLineStringHonorsMinSizeFourWithoutDroppingEndpoint) {
  std::unique_ptr<Geometry> closed_line =
      Read("LINESTRING (0 0, 2 0.05, 4 0, 2 10, 0 0)");
  std::unique_ptr<Geometry> out =
      TopologyPreservingSimplifier::simplify(closed_line.get(), 100.0);

  auto* out_ls = dynamic_cast<LineString*>(out.get());
  ASSERT_NE(out_ls, nullptr);
  EXPECT_EQ(out_ls->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(out_ls->isClosed());
  EXPECT_EQ(out_ls->getNumPoints(), 4U);

  std::unique_ptr<Geometry> expected = Read("LINESTRING (0 0, 4 0, 2 10, 0 0)");
  EXPECT_TRUE(out_ls->equalsExact(expected.get()));

  // Unlike LinearRing, a closed LineString has isRing=false, so its start/end
  // vertex is retained even when collinear within tolerance.
  std::unique_ptr<Geometry> closed_perturbed =
      Read("LINESTRING (5 0.05, 10 0, 10 10, 0 10, 0 0, 5 0.05)");
  std::unique_ptr<Geometry> perturbed_out =
      TopologyPreservingSimplifier::simplify(closed_perturbed.get(), 0.2);
  EXPECT_EQ(perturbed_out->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_EQ(perturbed_out->getNumPoints(), 6U);
  EXPECT_TRUE(perturbed_out->equalsExact(closed_perturbed.get()));
}

TEST_F(TopologyPreservingSimplifierTest,
       LinearRingSimplifiesEndpointAndHonorsMinSizeFour) {
  std::unique_ptr<Geometry> ring =
      Read("LINEARRING (5 0.05, 10 0, 10 10, 0 10, 0 0, 5 0.05)");

  std::unique_ptr<Geometry> out_mid =
      TopologyPreservingSimplifier::simplify(ring.get(), 0.2);
  std::unique_ptr<Geometry> expected_mid =
      Read("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  EXPECT_EQ(out_mid->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  EXPECT_TRUE(out_mid->isValid());
  EXPECT_TRUE(out_mid->equalsExact(expected_mid.get()));

  std::unique_ptr<Geometry> out_high =
      TopologyPreservingSimplifier::simplify(ring.get(), 100.0);
  auto* out_ring = dynamic_cast<LinearRing*>(out_high.get());
  ASSERT_NE(out_ring, nullptr);
  EXPECT_EQ(out_ring->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  EXPECT_EQ(out_ring->getNumPoints(), 4U);
  EXPECT_TRUE(out_ring->isClosed());
  EXPECT_TRUE(out_ring->isValid());
}

TEST_F(TopologyPreservingSimplifierTest,
       PolygonWithHolesPreservesRingCountOrderAndValidity) {
  std::unique_ptr<Geometry> poly = Read(
      "POLYGON ((0 0, 10 0.1, 20 0, 20 20, 0 20, 0 0), "
      "(2 2, 5 2.05, 8 2, 8 8, 2 8, 2 2), "
      "(12 2, 15 2.05, 18 2, 18 8, 12 8, 12 2))");

  {
    std::unique_ptr<Geometry> out =
        TopologyPreservingSimplifier::simplify(poly.get(), 0.5);
    auto* out_poly = dynamic_cast<Polygon*>(out.get());
    ASSERT_NE(out_poly, nullptr);
    EXPECT_TRUE(out_poly->isValid());
    ASSERT_EQ(out_poly->getNumInteriorRing(), 2U);
    EXPECT_EQ(out_poly->getExteriorRing()->getNumPoints(), 5U);
    EXPECT_EQ(out_poly->getInteriorRingN(0)->getNumPoints(), 5U);
    EXPECT_EQ(out_poly->getInteriorRingN(1)->getNumPoints(), 5U);
    // Hole ordering is preserved from input (left hole before right hole).
    EXPECT_LT(out_poly->getInteriorRingN(0)->getEnvelopeInternal()->getMaxX(),
              out_poly->getInteriorRingN(1)->getEnvelopeInternal()->getMinX());
  }

  {
    std::unique_ptr<Geometry> out =
        TopologyPreservingSimplifier::simplify(poly.get(), 100.0);
    auto* out_poly = dynamic_cast<Polygon*>(out.get());
    ASSERT_NE(out_poly, nullptr);
    EXPECT_TRUE(out_poly->isValid());
    ASSERT_EQ(out_poly->getNumInteriorRing(), 2U);
    EXPECT_GE(out_poly->getExteriorRing()->getNumPoints(), 4U);
    EXPECT_GE(out_poly->getInteriorRingN(0)->getNumPoints(), 4U);
    EXPECT_GE(out_poly->getInteriorRingN(1)->getNumPoints(), 4U);
  }
}

TEST_F(TopologyPreservingSimplifierTest,
       PolygonPreventsSelfIntersectionAndHoleCrossingShell) {
  std::unique_ptr<Geometry> notched = Read(
      "POLYGON ((0 0, 10 0, 10 10, 5.2 10, 5.2 2, 4.8 2, 4.8 10, 0 10, 0 0))");
  ASSERT_TRUE(notched->isValid());

  std::unique_ptr<Geometry> notched_out =
      TopologyPreservingSimplifier::simplify(notched.get(), 2.0);
  auto* notched_poly = dynamic_cast<Polygon*>(notched_out.get());
  ASSERT_NE(notched_poly, nullptr);
  EXPECT_TRUE(notched_poly->isValid());
  EXPECT_EQ(notched_poly->getNumInteriorRing(), 0U);

  std::unique_ptr<Geometry> with_hole = Read(
      "POLYGON ((0 0, 10 0, 10 10, 6 10, 6 7, 4 7, 4 10, 0 10, 0 0), "
      "(2 2, 8 2, 8 5, 2 5, 2 2))");
  ASSERT_TRUE(with_hole->isValid());

  std::unique_ptr<Geometry> hole_out =
      TopologyPreservingSimplifier::simplify(with_hole.get(), 5.0);
  auto* hole_poly = dynamic_cast<Polygon*>(hole_out.get());
  ASSERT_NE(hole_poly, nullptr);
  EXPECT_TRUE(hole_poly->isValid());
  EXPECT_EQ(hole_poly->getNumInteriorRing(), 1U);
}

TEST_F(TopologyPreservingSimplifierTest,
       MultiLineStringPreservesComponentDisjointness) {
  std::unique_ptr<Geometry> mls_ab =
      Read("MULTILINESTRING ((0 0, 5 4, 10 0), (5 3, 5 1, 5 -1))");
  std::unique_ptr<Geometry> out_ab =
      TopologyPreservingSimplifier::simplify(mls_ab.get(), 100.0);

  auto* res_ab = dynamic_cast<MultiLineString*>(out_ab.get());
  ASSERT_NE(res_ab, nullptr);
  ASSERT_EQ(res_ab->getNumGeometries(), 2U);
  EXPECT_EQ(res_ab->getGeometryN(0)->getNumPoints(), 3U);
  EXPECT_EQ(res_ab->getGeometryN(1)->getNumPoints(), 2U);
  EXPECT_TRUE(res_ab->getGeometryN(0)->disjoint(res_ab->getGeometryN(1)));

  std::unique_ptr<Geometry> mls_ba =
      Read("MULTILINESTRING ((5 3, 5 1, 5 -1), (0 0, 5 4, 10 0))");
  std::unique_ptr<Geometry> out_ba =
      TopologyPreservingSimplifier::simplify(mls_ba.get(), 100.0);

  auto* res_ba = dynamic_cast<MultiLineString*>(out_ba.get());
  ASSERT_NE(res_ba, nullptr);
  ASSERT_EQ(res_ba->getNumGeometries(), 2U);
  EXPECT_EQ(res_ba->getGeometryN(0)->getNumPoints(), 2U);
  EXPECT_EQ(res_ba->getGeometryN(1)->getNumPoints(), 3U);
  EXPECT_TRUE(res_ba->getGeometryN(0)->disjoint(res_ba->getGeometryN(1)));
}

TEST_F(TopologyPreservingSimplifierTest,
       MultiPolygonPreservesComponentSeparationAndNestedIslands) {
  std::unique_ptr<Geometry> bay_and_island = Read(
      "MULTIPOLYGON (((0 0, 10 0, 10 10, 6 10, 6 2, 4 2, 4 10, 0 10, 0 0)), "
      "((4.5 5, 5.5 5, 5.5 6, 4.5 6, 4.5 5)))");
  ASSERT_TRUE(bay_and_island->isValid());

  std::unique_ptr<Geometry> bay_out =
      TopologyPreservingSimplifier::simplify(bay_and_island.get(), 5.0);
  auto* bay_mp = dynamic_cast<MultiPolygon*>(bay_out.get());
  ASSERT_NE(bay_mp, nullptr);
  EXPECT_TRUE(bay_mp->isValid());
  ASSERT_EQ(bay_mp->getNumGeometries(), 2U);
  EXPECT_TRUE(bay_mp->getGeometryN(0)->disjoint(bay_mp->getGeometryN(1)));

  std::unique_ptr<Geometry> nested = Read(
      "MULTIPOLYGON (((0 0, 10 0.1, 20 0, 20 20, 0 20, 0 0), "
      "(2 2, 10 2.1, 18 2, 18 18, 2 18, 2 2)), "
      "((6 6, 10 6.05, 14 6, 14 14, 6 14, 6 6)))");
  ASSERT_TRUE(nested->isValid());

  std::unique_ptr<Geometry> nested_out =
      TopologyPreservingSimplifier::simplify(nested.get(), 10.0);
  auto* nested_mp = dynamic_cast<MultiPolygon*>(nested_out.get());
  ASSERT_NE(nested_mp, nullptr);
  EXPECT_TRUE(nested_mp->isValid());
  ASSERT_EQ(nested_mp->getNumGeometries(), 2U);
  EXPECT_EQ(nested_mp->getGeometryN(0)->getNumInteriorRing(), 1U);
  EXPECT_TRUE(nested_mp->getGeometryN(0)->disjoint(nested_mp->getGeometryN(1)));
}

TEST_F(TopologyPreservingSimplifierTest,
       GeometryCollectionHandlesMixedTypesEmptyComponentsAndCrossTopology) {
  std::unique_ptr<Geometry> mixed = Read(
      "GEOMETRYCOLLECTION (POINT (1 2), POINT EMPTY, LINESTRING EMPTY, "
      "POLYGON EMPTY, LINESTRING (0 0, 5 0.05, 10 0), "
      "POLYGON ((20 0, 25 0.05, 30 0, 30 10, 20 10, 20 0)))");
  std::unique_ptr<Geometry> expected_mixed = Read(
      "GEOMETRYCOLLECTION (POINT (1 2), LINESTRING (0 0, 10 0), "
      "POLYGON ((20 0, 30 0, 30 10, 20 10, 20 0)))");

  std::unique_ptr<Geometry> mixed_out =
      TopologyPreservingSimplifier::simplify(mixed.get(), 0.5);
  EXPECT_EQ(mixed_out->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_TRUE(mixed_out->equalsExact(expected_mixed.get()));

  std::unique_ptr<Geometry> points_only =
      Read("GEOMETRYCOLLECTION (POINT (3 4), POINT EMPTY)");
  std::unique_ptr<Geometry> points_out =
      TopologyPreservingSimplifier::simplify(points_only.get(), 1.0);
  std::unique_ptr<Geometry> expected_points =
      Read("GEOMETRYCOLLECTION (POINT (3 4))");
  EXPECT_TRUE(points_out->equalsExact(expected_points.get()));

  std::unique_ptr<Geometry> cross_gc = Read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 10 0, 10 10, 6 10, 6 2, 4 2, 4 10, 0 10, 0 0)), "
      "LINESTRING (5 5, 5 12))");
  std::unique_ptr<Geometry> cross_out =
      TopologyPreservingSimplifier::simplify(cross_gc.get(), 10.0);
  auto* gc = dynamic_cast<GeometryCollection*>(cross_out.get());
  ASSERT_NE(gc, nullptr);
  ASSERT_EQ(gc->getNumGeometries(), 2U);
  EXPECT_TRUE(gc->getGeometryN(0)->isValid());
  EXPECT_TRUE(gc->getGeometryN(0)->disjoint(gc->getGeometryN(1)));
}

TEST_F(TopologyPreservingSimplifierTest, PreservesZCoordinates) {
  std::unique_ptr<Geometry> line_z =
      Read("LINESTRING Z (0 0 10, 5 0.05 42, 10 0 20)");
  std::unique_ptr<Geometry> out_geom =
      TopologyPreservingSimplifier::simplify(line_z.get(), 0.2);

  auto* out_ls = dynamic_cast<LineString*>(out_geom.get());
  ASSERT_NE(out_ls, nullptr);
  ASSERT_EQ(out_ls->getNumPoints(), 2U);
  EXPECT_TRUE(out_ls->hasZ());
  EXPECT_DOUBLE_EQ(out_ls->getCoordinateN(0).z, 10.0);
  EXPECT_DOUBLE_EQ(out_ls->getCoordinateN(1).z, 20.0);
}

TEST_F(TopologyPreservingSimplifierTest,
       ThrowsAndCleansUpOnUnsupportedCurvedGeometry) {
  std::unique_ptr<Geometry> curve = Read("CIRCULARSTRING (0 0, 1 1, 2 0)");
  EXPECT_THROW(TopologyPreservingSimplifier::simplify(curve.get(), 0.5),
               IllegalArgumentException);

  // Include a non-empty LineString alongside the CircularString so
  // linestringMap is populated prior to GeometryTransformer throwing,
  // exercising catch(...) cleanup without leaking TaggedLineString instances.
  std::unique_ptr<Geometry> mixed_with_curve = Read(
      "GEOMETRYCOLLECTION (LINESTRING (0 0, 5 0.05, 10 0), "
      "CIRCULARSTRING (0 0, 1 1, 2 0))");
  EXPECT_THROW(
      TopologyPreservingSimplifier::simplify(mixed_with_curve.get(), 0.5),
      IllegalArgumentException);
}

}  // namespace
}  // namespace geos_google
