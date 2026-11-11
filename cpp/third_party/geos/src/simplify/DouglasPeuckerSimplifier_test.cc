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

#include "third_party/geos/include/geos/simplify/DouglasPeuckerSimplifier.h"

#include <initializer_list>
#include <limits>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Dimension;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::simplify::DouglasPeuckerSimplifier;

class DouglasPeuckerSimplifierTest : public ::testing::Test {
 protected:
  std::unique_ptr<Geometry> Read(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  std::unique_ptr<LinearRing> MakeLinearRing(
      std::initializer_list<CoordinateXY> coords) const {
    auto seq = std::make_unique<CoordinateSequence>(0, false, false);
    seq->reserve(coords.size());
    for (const CoordinateXY& coord : coords) {
      seq->add(coord);
    }
    return factory_->createLinearRing(std::move(seq));
  }

  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};
};

TEST_F(DouglasPeuckerSimplifierTest, NegativeToleranceThrows) {
  auto line = Read("LINESTRING (0 0, 1 1, 2 0)");
  EXPECT_THROW(DouglasPeuckerSimplifier::simplify(line.get(), -0.1),
               geos::util::IllegalArgumentException);

  DouglasPeuckerSimplifier simp(line.get());
  EXPECT_THROW(simp.setDistanceTolerance(-1.0),
               geos::util::IllegalArgumentException);
}

TEST_F(DouglasPeuckerSimplifierTest, NaNToleranceThrowsDuringSimplification) {
  auto line = Read("LINESTRING (0 0, 1 1, 2 0)");
  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_THROW(DouglasPeuckerSimplifier::simplify(line.get(), nan),
               geos::util::IllegalArgumentException);
}

TEST_F(DouglasPeuckerSimplifierTest, InstanceApiSimplifiesGeometry) {
  auto line = Read("LINESTRING (0 0, 5 0.5, 10 0)");
  auto expected = Read("LINESTRING (0 0, 10 0)");

  DouglasPeuckerSimplifier simp(line.get());
  simp.setDistanceTolerance(1.0);
  auto res = simp.getResultGeometry();

  ASSERT_NE(res, nullptr);
  EXPECT_TRUE(res->equalsExact(expected.get()));
}

TEST_F(DouglasPeuckerSimplifierTest, PointAndMultiPointArePreserved) {
  auto point = Read("POINT (3 4)");
  auto res_point = DouglasPeuckerSimplifier::simplify(point.get(), 5.0);
  EXPECT_TRUE(res_point->equalsExact(point.get()));

  auto empty_point = Read("POINT EMPTY");
  auto res_empty_point =
      DouglasPeuckerSimplifier::simplify(empty_point.get(), 1.0);
  EXPECT_TRUE(res_empty_point->isEmpty());
  EXPECT_EQ(res_empty_point->getGeometryTypeId(), geos::geom::GEOS_POINT);

  auto multipoint = Read("MULTIPOINT ((0 0), (1 2), (5 5))");
  auto res_mp = DouglasPeuckerSimplifier::simplify(multipoint.get(), 10.0);
  EXPECT_TRUE(res_mp->equalsExact(multipoint.get()));

  auto empty_mp = Read("MULTIPOINT EMPTY");
  auto res_empty_mp = DouglasPeuckerSimplifier::simplify(empty_mp.get(), 1.0);
  EXPECT_TRUE(res_empty_mp->isEmpty());
  EXPECT_EQ(res_empty_mp->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
}

TEST_F(DouglasPeuckerSimplifierTest,
       LineStringAndMultiLineStringSimplification) {
  auto line = Read("LINESTRING (0 0, 5 0.2, 10 0, 15 5)");
  auto expected_line = Read("LINESTRING (0 0, 10 0, 15 5)");
  auto res_line = DouglasPeuckerSimplifier::simplify(line.get(), 0.5);
  EXPECT_TRUE(res_line->equalsExact(expected_line.get()));

  // Zero tolerance preserves non-collinear vertices.
  auto res_zero = DouglasPeuckerSimplifier::simplify(line.get(), 0.0);
  EXPECT_TRUE(res_zero->equalsExact(line.get()));

  // Closed LineString is not a LinearRing, so its closed endpoint is kept.
  auto closed_line = Read("LINESTRING (5 0, 10 0, 10 10, 0 10, 0 0, 5 0)");
  auto res_closed = DouglasPeuckerSimplifier::simplify(closed_line.get(), 0.5);
  EXPECT_EQ(res_closed->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(res_closed->equalsExact(closed_line.get()));

  auto empty_line = Read("LINESTRING EMPTY");
  auto res_empty_line =
      DouglasPeuckerSimplifier::simplify(empty_line.get(), 1.0);
  EXPECT_TRUE(res_empty_line->isEmpty());
  EXPECT_EQ(res_empty_line->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);

  auto mls = Read("MULTILINESTRING ((0 0, 5 0.1, 10 0), (0 5, 5 5.1, 10 5))");
  auto expected_mls = Read("MULTILINESTRING ((0 0, 10 0), (0 5, 10 5))");
  auto res_mls = DouglasPeuckerSimplifier::simplify(mls.get(), 0.5);
  EXPECT_TRUE(res_mls->equalsExact(expected_mls.get()));

  auto empty_mls = Read("MULTILINESTRING EMPTY");
  auto res_empty_mls = DouglasPeuckerSimplifier::simplify(empty_mls.get(), 1.0);
  EXPECT_TRUE(res_empty_mls->isEmpty());
  EXPECT_EQ(res_empty_mls->getGeometryTypeId(),
            geos::geom::GEOS_MULTILINESTRING);

  // Z coordinates on kept vertices are preserved.
  auto line_z = Read("LINESTRING Z (0 0 10, 5 0.1 20, 10 0 30)");
  auto res_z = DouglasPeuckerSimplifier::simplify(line_z.get(), 0.5);
  auto coords_z = res_z->getCoordinates();
  ASSERT_EQ(coords_z->size(), 2u);
  EXPECT_TRUE(coords_z->getAt(0).equals3D(Coordinate(0.0, 0.0, 10.0)));
  EXPECT_TRUE(coords_z->getAt(1).equals3D(Coordinate(10.0, 0.0, 30.0)));
}

TEST_F(DouglasPeuckerSimplifierTest, StandaloneLinearRing) {
  auto ring = MakeLinearRing({
      {5.0, 0.0},
      {10.0, 0.0},
      {10.0, 10.0},
      {0.0, 10.0},
      {0.0, 0.0},
      {5.0, 0.0},
  });

  // For GEOS_LINEARRING, closed endpoint (5, 0) is simplified away.
  auto res_ring = DouglasPeuckerSimplifier::simplify(ring.get(), 0.5);
  ASSERT_EQ(res_ring->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  auto expected_ring = MakeLinearRing({
      {10.0, 0.0},
      {10.0, 10.0},
      {0.0, 10.0},
      {0.0, 0.0},
      {10.0, 0.0},
  });
  EXPECT_TRUE(res_ring->equalsExact(expected_ring.get()));

  // When standalone ring collapses below 4 points, GeometryTransformer
  // downgrades it to a LineString rather than pruning it to nullptr.
  auto collapsed = DouglasPeuckerSimplifier::simplify(ring.get(), 20.0);
  EXPECT_EQ(collapsed->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_EQ(collapsed->getNumPoints(), 2u);

  auto empty_ring = factory_->createLinearRing();
  auto res_empty = DouglasPeuckerSimplifier::simplify(empty_ring.get(), 1.0);
  EXPECT_TRUE(res_empty->isEmpty());
  EXPECT_EQ(res_empty->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
}

TEST_F(DouglasPeuckerSimplifierTest, PolygonValidAndEmptySimplification) {
  auto poly = Read("POLYGON ((0 0, 5 0.2, 10 0, 10 10, 0 10, 0 0))");
  auto expected = Read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");

  auto res = DouglasPeuckerSimplifier::simplify(poly.get(), 0.5);
  EXPECT_TRUE(res->isValid());
  EXPECT_TRUE(res->equalsExact(expected.get()));
  EXPECT_NEAR(res->getArea(), 100.0, 1e-9);

  auto empty_poly = Read("POLYGON EMPTY");
  auto res_empty = DouglasPeuckerSimplifier::simplify(empty_poly.get(), 1.0);
  EXPECT_TRUE(res_empty->isEmpty());
  EXPECT_EQ(res_empty->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
}

TEST_F(DouglasPeuckerSimplifierTest,
       PolygonDropsCollapsedHoleAndKeepsValidHole) {
  auto poly = Read(
      "POLYGON ((0 0, 100 0, 100 100, 0 100, 0 0), "
      "(10 10, 30 10, 30 30, 10 30, 10 10), "
      "(60 60, 62 60, 62 62, 60 62, 60 60))");

  auto res = DouglasPeuckerSimplifier::simplify(poly.get(), 5.0);
  ASSERT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(res->isValid());

  const auto* out_poly = dynamic_cast<const Polygon*>(res.get());
  ASSERT_NE(out_poly, nullptr);
  EXPECT_EQ(out_poly->getNumInteriorRing(), 1u);
  EXPECT_NEAR(out_poly->getArea(), 10000.0 - 400.0, 1e-9);
}

TEST_F(DouglasPeuckerSimplifierTest, PolygonShellCollapseReturnsEmptyPolygon) {
  auto small_poly = Read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");

  auto res = DouglasPeuckerSimplifier::simplify(small_poly.get(), 5.0);
  ASSERT_NE(res, nullptr);
  EXPECT_TRUE(res->isEmpty());
  EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(res->isValid());
}

TEST_F(DouglasPeuckerSimplifierTest,
       PolygonSelfIntersectionRepairedToValidArea) {
  // Valid polygon with a slight right bulge at (8.5, 5) protecting a deep
  // left bay tip at (8.2, 5). Simplifying with tolerance 1.0 removes (8.5, 5),
  // causing (8,0)-(8,10) to cross the bay and triggering buffer(0.0) repair.
  auto poly =
      Read("POLYGON ((0 0, 8 0, 8.5 5, 8 10, 0 10, 0 5.1, 8.2 5, 0 4.9, 0 0))");
  ASSERT_TRUE(poly->isValid());

  auto res = DouglasPeuckerSimplifier::simplify(poly.get(), 1.0);
  ASSERT_NE(res, nullptr);
  EXPECT_FALSE(res->isEmpty());
  EXPECT_TRUE(res->isValid());
  EXPECT_EQ(res->getDimension(), Dimension::A);
}

TEST_F(DouglasPeuckerSimplifierTest, MultiPolygonValidOverlapAndCollapse) {
  // Disjoint polygons where simplifying Poly 1 flattens its concave notch and
  // makes it overlap Poly 2, which DPTransformer repairs via buffer(0.0).
  auto overlapping_mp = Read(
      "MULTIPOLYGON ("
      "((0 0, 10 0, 8.5 10, 10 20, 0 20, 0 0)), "
      "((9 8, 16 8, 16 12, 9 12, 9 8))"
      ")");
  ASSERT_TRUE(overlapping_mp->isValid());

  auto merged = DouglasPeuckerSimplifier::simplify(overlapping_mp.get(), 2.0);
  ASSERT_NE(merged, nullptr);
  EXPECT_TRUE(merged->isValid());
  EXPECT_EQ(merged->getDimension(), Dimension::A);
  EXPECT_NEAR(merged->getArea(), 224.0, 1e-6);

  // Smaller tolerance keeps both polygons valid and disjoint.
  auto still_disjoint =
      DouglasPeuckerSimplifier::simplify(overlapping_mp.get(), 0.5);
  EXPECT_TRUE(still_disjoint->isValid());
  EXPECT_EQ(still_disjoint->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(still_disjoint->getNumGeometries(), 2u);

  // One component collapses while the other survives.
  auto mixed_size_mp = Read(
      "MULTIPOLYGON ("
      "((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "((20 20, 21 20, 21 21, 20 21, 20 20))"
      ")");
  auto one_left = DouglasPeuckerSimplifier::simplify(mixed_size_mp.get(), 2.0);
  EXPECT_TRUE(one_left->isValid());
  EXPECT_EQ(one_left->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_NEAR(one_left->getArea(), 100.0, 1e-9);

  // All components collapse -> empty MultiPolygon.
  auto all_collapsed =
      DouglasPeuckerSimplifier::simplify(mixed_size_mp.get(), 20.0);
  EXPECT_TRUE(all_collapsed->isEmpty());
  EXPECT_EQ(all_collapsed->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);

  auto empty_mp = Read("MULTIPOLYGON EMPTY");
  auto res_empty = DouglasPeuckerSimplifier::simplify(empty_mp.get(), 1.0);
  EXPECT_TRUE(res_empty->isEmpty());
  EXPECT_EQ(res_empty->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
}

TEST_F(DouglasPeuckerSimplifierTest, GeometryCollectionSimplifiesComponents) {
  auto gc = Read(
      "GEOMETRYCOLLECTION ("
      "POINT (1 1), "
      "LINESTRING (0 0, 5 0.1, 10 0), "
      "POLYGON ((0 0, 5 0.1, 10 0, 10 10, 0 10, 0 0))"
      ")");
  auto expected = Read(
      "GEOMETRYCOLLECTION ("
      "POINT (1 1), "
      "LINESTRING (0 0, 10 0), "
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))"
      ")");

  auto res = DouglasPeuckerSimplifier::simplify(gc.get(), 0.5);
  ASSERT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(res->getNumGeometries(), 3u);
  EXPECT_TRUE(res->equalsExact(expected.get()));

  // Collapsed polygon inside a collection becomes empty and gets pruned.
  auto gc_pruned = Read(
      "GEOMETRYCOLLECTION ("
      "POINT (1 1), "
      "POLYGON ((20 20, 21 20, 21 21, 20 21, 20 20))"
      ")");
  auto res_pruned = DouglasPeuckerSimplifier::simplify(gc_pruned.get(), 5.0);
  ASSERT_EQ(res_pruned->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(res_pruned->getNumGeometries(), 1u);

  auto empty_gc = Read("GEOMETRYCOLLECTION EMPTY");
  auto res_empty = DouglasPeuckerSimplifier::simplify(empty_gc.get(), 1.0);
  EXPECT_TRUE(res_empty->isEmpty());
  EXPECT_EQ(res_empty->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
}

}  // namespace
}  // namespace geos_google
