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

// Unit tests for third_party/geos/src/operation/overlayng/UnaryUnionNG.cpp.

#include "third_party/geos/include/geos/operation/overlayng/UnaryUnionNG.h"

#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/union/UnionStrategy.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::operation::geounion::UnionStrategy;
using geos::operation::overlayng::UnaryUnionNG;
using geos::util::UnsupportedOperationException;

TEST(UnaryUnionNGTest, UnionsPolygonsWithExplicitFloatingPrecisionModel) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  const PrecisionModel floating_pm(PrecisionModel::FLOATING);

  // Overlapping invalid MultiPolygon cleans into a single valid Polygon.
  Geometry::Ptr overlapping = reader.read(
      "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "((1 1, 3 1, 3 3, 1 3, 1 1)))");
  Geometry::Ptr expected_overlapping =
      reader.read("POLYGON ((0 0, 2 0, 2 1, 3 1, 3 3, 1 3, 1 2, 0 2, 0 0))");
  Geometry::Ptr overlapping_result =
      UnaryUnionNG::Union(overlapping.get(), floating_pm);
  ASSERT_NE(overlapping_result, nullptr);
  EXPECT_TRUE(overlapping_result->isValid());
  EXPECT_EQ(overlapping_result->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_NEAR(overlapping_result->getArea(), 7.0, 1e-9);
  EXPECT_TRUE(overlapping_result->equals(expected_overlapping.get()));

  // Adjacent squares enclosing an interior hole merge into a polygon with hole.
  Geometry::Ptr ring_of_rects = reader.read(
      "MULTIPOLYGON ("
      "((0 0, 3 0, 3 1, 0 1, 0 0)), "
      "((2 1, 3 1, 3 3, 2 3, 2 1)), "
      "((0 2, 2 2, 2 3, 0 3, 0 2)), "
      "((0 1, 1 1, 1 2, 0 2, 0 1)))");
  Geometry::Ptr expected_hole = reader.read(
      "POLYGON ((0 0, 3 0, 3 3, 0 3, 0 0), "
      "(1 1, 1 2, 2 2, 2 1, 1 1))");
  Geometry::Ptr hole_result =
      UnaryUnionNG::Union(ring_of_rects.get(), floating_pm);
  ASSERT_NE(hole_result, nullptr);
  EXPECT_TRUE(hole_result->isValid());
  EXPECT_NEAR(hole_result->getArea(), 8.0, 1e-9);
  EXPECT_TRUE(hole_result->equals(expected_hole.get()));

  // Disjoint polygons remain a MultiPolygon.
  Geometry::Ptr disjoint = reader.read(
      "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "((5 5, 6 5, 6 6, 5 6, 5 5)))");
  Geometry::Ptr disjoint_result =
      UnaryUnionNG::Union(disjoint.get(), floating_pm);
  ASSERT_NE(disjoint_result, nullptr);
  EXPECT_TRUE(disjoint_result->isValid());
  EXPECT_EQ(disjoint_result->getGeometryTypeId(),
            geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_NEAR(disjoint_result->getArea(), 2.0, 1e-9);
  EXPECT_TRUE(disjoint_result->equals(disjoint.get()));

  // Single polygon returns an equal polygon.
  Geometry::Ptr single_poly =
      reader.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  Geometry::Ptr single_result =
      UnaryUnionNG::Union(single_poly.get(), floating_pm);
  ASSERT_NE(single_result, nullptr);
  EXPECT_TRUE(single_result->equals(single_poly.get()));
}

TEST(UnaryUnionNGTest, UnionsWithFixedPrecisionModelSnapsToGrid) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  const PrecisionModel integer_pm(1.0);

  // Near-adjacent squares snap onto shared edge x = 2 and merge; the tiny
  // square collapses to a single point on the integer grid and drops out.
  Geometry::Ptr polys = reader.read(
      "MULTIPOLYGON ("
      "((0.1 0.1, 2.2 0.1, 2.2 2.1, 0.1 2.1, 0.1 0.1)), "
      "((1.9 0.2, 4.1 0.2, 4.1 2.2, 1.9 2.2, 1.9 0.2)), "
      "((5.1 5.1, 5.2 5.1, 5.2 5.2, 5.1 5.2, 5.1 5.1)))");
  Geometry::Ptr expected_poly =
      reader.read("POLYGON ((0 0, 4 0, 4 2, 0 2, 0 0))");
  Geometry::Ptr poly_result = UnaryUnionNG::Union(polys.get(), integer_pm);
  ASSERT_NE(poly_result, nullptr);
  EXPECT_TRUE(poly_result->isValid());
  EXPECT_NEAR(poly_result->getArea(), 8.0, 1e-9);
  EXPECT_TRUE(poly_result->equals(expected_poly.get()));

  // Nearby points snap to the same integer grid coordinate and deduplicate.
  Geometry::Ptr pts = reader.read("MULTIPOINT ((1.1 2.2), (1.4 2.4))");
  Geometry::Ptr expected_pt = reader.read("POINT (1 2)");
  Geometry::Ptr pt_result = UnaryUnionNG::Union(pts.get(), integer_pm);
  ASSERT_NE(pt_result, nullptr);
  EXPECT_EQ(pt_result->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(pt_result->equals(expected_pt.get()));

  // Near-collinear lines snap onto the x-axis from 0 to 4.
  Geometry::Ptr lines =
      reader.read("MULTILINESTRING ((0.1 0.2, 2.1 0.2), (1.9 0.1, 4.2 -0.1))");
  Geometry::Ptr expected_line = reader.read("LINESTRING (0 0, 4 0)");
  Geometry::Ptr line_result = UnaryUnionNG::Union(lines.get(), integer_pm);
  ASSERT_NE(line_result, nullptr);
  EXPECT_EQ(line_result->getDimension(), 1);
  EXPECT_NEAR(line_result->getLength(), 4.0, 1e-9);
  EXPECT_TRUE(line_result->equals(expected_line.get()));
}

TEST(UnaryUnionNGTest, UnionsLinearAndPuntalGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  const PrecisionModel floating_pm(PrecisionModel::FLOATING);

  // Overlapping and crossing linework is noded at intersections and dissolved.
  Geometry::Ptr lines =
      reader.read("MULTILINESTRING ((0 0, 2 0), (1 0, 3 0), (1 -1, 1 1))");
  Geometry::Ptr expected_lines =
      reader.read("MULTILINESTRING ((0 0, 3 0), (1 -1, 1 1))");
  Geometry::Ptr line_result = UnaryUnionNG::Union(lines.get(), floating_pm);
  ASSERT_NE(line_result, nullptr);
  EXPECT_TRUE(line_result->isValid());
  EXPECT_EQ(line_result->getDimension(), 1);
  EXPECT_NEAR(line_result->getLength(), 5.0, 1e-9);
  EXPECT_TRUE(line_result->equals(expected_lines.get()));

  // Duplicate points in MultiPoint are collapsed; single point stays Point.
  Geometry::Ptr multi_pt = reader.read("MULTIPOINT ((1 1), (2 2), (1 1))");
  Geometry::Ptr expected_pts = reader.read("MULTIPOINT ((1 1), (2 2))");
  Geometry::Ptr pt_result = UnaryUnionNG::Union(multi_pt.get(), floating_pm);
  ASSERT_NE(pt_result, nullptr);
  EXPECT_EQ(pt_result->getNumPoints(), 2u);
  EXPECT_TRUE(pt_result->equals(expected_pts.get()));

  Geometry::Ptr single_pt = reader.read("POINT (3 4)");
  Geometry::Ptr single_pt_result =
      UnaryUnionNG::Union(single_pt.get(), floating_pm);
  ASSERT_NE(single_pt_result, nullptr);
  EXPECT_TRUE(single_pt_result->equals(single_pt.get()));
}

TEST(UnaryUnionNGTest, UnionsHeterogeneousGeometryCollection) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  const PrecisionModel floating_pm(PrecisionModel::FLOATING);

  // Interior lines and points (including points on protruding lines) are
  // absorbed while exterior segments and points survive.
  Geometry::Ptr gc = reader.read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "LINESTRING (1 1, 3 1), "
      "LINESTRING (2 2, 6 2), "
      "POINT (2 3), "
      "POINT (5 2), "
      "POINT (10 10))");
  Geometry::Ptr expected = reader.read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "LINESTRING (4 2, 6 2), "
      "POINT (10 10))");

  Geometry::Ptr result = UnaryUnionNG::Union(gc.get(), floating_pm);
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(result->getNumGeometries(), 3u);
  EXPECT_NEAR(result->getArea(), 16.0, 1e-9);
  // Polygon perimeter (16.0) + protruding line (2.0).
  EXPECT_NEAR(result->getLength(), 18.0, 1e-9);
  EXPECT_TRUE(result->equals(expected.get()));
}

TEST(UnaryUnionNGTest, UnionsEmptyGeometriesWithExplicitPrecisionModel) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  const PrecisionModel floating_pm(PrecisionModel::FLOATING);
  const PrecisionModel fixed_pm(10.0);

  struct EmptyCase {
    std::string_view wkt;
    geos::geom::GeometryTypeId expected_type;
  };
  // Points and LineStrings preserve their geometry type via OverlayNG;
  // empty Polygons are filtered out by CascadedPolygonUnion's spatial index
  // and empty collections extract no components, yielding GEOMETRYCOLLECTION.
  constexpr EmptyCase kEmptyCases[] = {
      {"POINT EMPTY", geos::geom::GEOS_POINT},
      {"LINESTRING EMPTY", geos::geom::GEOS_LINESTRING},
      {"POLYGON EMPTY", geos::geom::GEOS_GEOMETRYCOLLECTION},
      {"MULTIPOINT EMPTY", geos::geom::GEOS_GEOMETRYCOLLECTION},
      {"MULTILINESTRING EMPTY", geos::geom::GEOS_GEOMETRYCOLLECTION},
      {"MULTIPOLYGON EMPTY", geos::geom::GEOS_GEOMETRYCOLLECTION},
      {"GEOMETRYCOLLECTION EMPTY", geos::geom::GEOS_GEOMETRYCOLLECTION},
  };

  for (const EmptyCase& tc : kEmptyCases) {
    SCOPED_TRACE(tc.wkt);
    Geometry::Ptr empty_geom = reader.read(std::string(tc.wkt));
    ASSERT_NE(empty_geom, nullptr);

    Geometry::Ptr floating_res =
        UnaryUnionNG::Union(empty_geom.get(), floating_pm);
    ASSERT_NE(floating_res, nullptr);
    EXPECT_TRUE(floating_res->isEmpty());
    EXPECT_EQ(floating_res->getGeometryTypeId(), tc.expected_type);

    Geometry::Ptr fixed_res = UnaryUnionNG::Union(empty_geom.get(), fixed_pm);
    ASSERT_NE(fixed_res, nullptr);
    EXPECT_TRUE(fixed_res->isEmpty());
    EXPECT_EQ(fixed_res->getGeometryTypeId(), tc.expected_type);
  }

  // Empty components mixed with non-empty components drop out cleanly.
  Geometry::Ptr mixed_with_empty =
      reader.read("GEOMETRYCOLLECTION (POLYGON EMPTY, POINT (1 2))");
  Geometry::Ptr expected_pt = reader.read("POINT (1 2)");
  Geometry::Ptr mixed_res =
      UnaryUnionNG::Union(mixed_with_empty.get(), floating_pm);
  ASSERT_NE(mixed_res, nullptr);
  EXPECT_EQ(mixed_res->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(mixed_res->equals(expected_pt.get()));

  Geometry::Ptr polys_with_empty = reader.read(
      "GEOMETRYCOLLECTION (POLYGON EMPTY, "
      "POLYGON ((1 1, 3 1, 3 3, 1 3, 1 1)))");
  Geometry::Ptr expected_poly =
      reader.read("POLYGON ((1 1, 3 1, 3 3, 1 3, 1 1))");
  Geometry::Ptr poly_res = UnaryUnionNG::Union(polys_with_empty.get());
  ASSERT_NE(poly_res, nullptr);
  EXPECT_EQ(poly_res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(poly_res->equals(expected_poly.get()));
}

TEST(UnaryUnionNGTest, SingleArgumentOverloadUsesComputedRobustPrecisionModel) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Overlapping polygons with exact binary-decimal fractional coordinates.
  Geometry::Ptr polys = reader.read(
      "MULTIPOLYGON ("
      "((1.25 1.25, 3.5 1.25, 3.5 3.5, 1.25 3.5, 1.25 1.25)), "
      "((2.5 2.5, 4.75 2.5, 4.75 4.75, 2.5 4.75, 2.5 2.5)))");
  Geometry::Ptr poly_res = UnaryUnionNG::Union(polys.get());
  ASSERT_NE(poly_res, nullptr);
  EXPECT_TRUE(poly_res->isValid());
  EXPECT_NEAR(poly_res->getArea(), 9.125, 1e-9);

  // Large-coordinate polygons with many decimals trigger the safeScale bound
  // inside PrecisionUtil::robustPM.
  Geometry::Ptr large_polys = reader.read(
      "MULTIPOLYGON ("
      "((1000000.123456789012 1000000.0, 1000002.123456789012 1000000.0, "
      "1000002.123456789012 1000002.0, 1000000.123456789012 1000002.0, "
      "1000000.123456789012 1000000.0)), "
      "((1000001.0 1000001.0, 1000003.0 1000001.0, 1000003.0 1000003.0, "
      "1000001.0 1000003.0, 1000001.0 1000001.0)))");
  Geometry::Ptr large_res = UnaryUnionNG::Union(large_polys.get());
  ASSERT_NE(large_res, nullptr);
  EXPECT_TRUE(large_res->isValid());
  // Overlap is [1000001, 1000002.1234568] x [1000001, 1000002] (~1.1234568).
  EXPECT_NEAR(large_res->getArea(), 6.8765432, 1e-5);

  // Crossing linework and duplicate points (away from origin).
  Geometry::Ptr lines =
      reader.read("MULTILINESTRING ((1 1, 3 1), (2 1, 4 1), (2 0, 2 2))");
  Geometry::Ptr line_res = UnaryUnionNG::Union(lines.get());
  ASSERT_NE(line_res, nullptr);
  EXPECT_TRUE(line_res->isValid());
  EXPECT_NEAR(line_res->getLength(), 5.0, 1e-9);

  Geometry::Ptr pts = reader.read("MULTIPOINT ((1 2), (3 4), (1 2))");
  Geometry::Ptr pt_res = UnaryUnionNG::Union(pts.get());
  ASSERT_NE(pt_res, nullptr);
  EXPECT_EQ(pt_res->getNumPoints(), 2u);

  // Heterogeneous collection via single-argument overload.
  Geometry::Ptr gc = reader.read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((1 1, 3 1, 3 3, 1 3, 1 1)), "
      "LINESTRING (2 2, 5 2), "
      "POINT (8 8))");
  Geometry::Ptr gc_res = UnaryUnionNG::Union(gc.get());
  ASSERT_NE(gc_res, nullptr);
  EXPECT_TRUE(gc_res->isValid());
  EXPECT_EQ(gc_res->getNumGeometries(), 3u);
  EXPECT_NEAR(gc_res->getArea(), 4.0, 1e-9);
  EXPECT_NEAR(gc_res->getLength(), 10.0, 1e-9);
}

TEST(UnaryUnionNGTest, PreservesZCoordinatesInUnion) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  const PrecisionModel floating_pm(PrecisionModel::FLOATING);

  Geometry::Ptr polys_z = reader.read(
      "MULTIPOLYGON Z ("
      "((0 0 1, 2 0 2, 2 2 3, 0 2 4, 0 0 1)), "
      "((2 0 2, 4 0 5, 4 2 6, 2 2 3, 2 0 2)))");
  Geometry::Ptr result = UnaryUnionNG::Union(polys_z.get(), floating_pm);
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->hasZ());
  EXPECT_TRUE(result->isValid());
  EXPECT_NEAR(result->getArea(), 8.0, 1e-9);
}

TEST(UnaryUnionNGTest, NGUnionStrategyFloatingAndFixedPrecisionBehavior) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  Geometry::Ptr poly_a =
      reader.read("POLYGON ((0.1 0.1, 2.2 0.1, 2.2 2.1, 0.1 2.1, 0.1 0.1))");
  Geometry::Ptr poly_b =
      reader.read("POLYGON ((1.9 0.2, 4.1 0.2, 4.1 2.2, 1.9 2.2, 1.9 0.2))");

  const PrecisionModel pm_floating(PrecisionModel::FLOATING);
  UnaryUnionNG::NGUnionStrategy strat_floating(pm_floating);
  const UnionStrategy& floating_iface = strat_floating;
  EXPECT_TRUE(floating_iface.isFloatingPrecision());

  Geometry::Ptr float_union = strat_floating.Union(poly_a.get(), poly_b.get());
  ASSERT_NE(float_union, nullptr);
  EXPECT_TRUE(float_union->isValid());
  // Overlap rectangle is [1.9, 2.2] x [0.2, 2.1] -> area 0.3 * 1.9 = 0.57.
  // Total = 2.1 * 2.0 + 2.2 * 2.0 - 0.57 = 4.2 + 4.4 - 0.57 = 8.03.
  EXPECT_NEAR(float_union->getArea(), 8.03, 1e-9);

  const PrecisionModel pm_single(PrecisionModel::FLOATING_SINGLE);
  UnaryUnionNG::NGUnionStrategy strat_single(pm_single);
  const UnionStrategy& single_iface = strat_single;
  EXPECT_TRUE(single_iface.isFloatingPrecision());

  const PrecisionModel pm_fixed(1.0);
  UnaryUnionNG::NGUnionStrategy strat_fixed(pm_fixed);
  const UnionStrategy& fixed_iface = strat_fixed;
  EXPECT_FALSE(fixed_iface.isFloatingPrecision());

  Geometry::Ptr fixed_union = strat_fixed.Union(poly_a.get(), poly_b.get());
  Geometry::Ptr expected_fixed =
      reader.read("POLYGON ((0 0, 4 0, 4 2, 0 2, 0 0))");
  ASSERT_NE(fixed_union, nullptr);
  EXPECT_TRUE(fixed_union->isValid());
  EXPECT_NEAR(fixed_union->getArea(), 8.0, 1e-9);
  EXPECT_TRUE(fixed_union->equals(expected_fixed.get()));
}

TEST(UnaryUnionNGTest, ThrowsOnCurvedGeometry) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  const PrecisionModel floating_pm(PrecisionModel::FLOATING);

  Geometry::Ptr curve = reader.read("CIRCULARSTRING (1 1, 2 2, 3 1)");
  EXPECT_THROW(static_cast<void>(UnaryUnionNG::Union(curve.get(), floating_pm)),
               UnsupportedOperationException);
  EXPECT_THROW(static_cast<void>(UnaryUnionNG::Union(curve.get())),
               UnsupportedOperationException);
}

}  // namespace
}  // namespace geos_google
