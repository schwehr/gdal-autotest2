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

// Unit tests for third_party/geos/src/geom/prep/BasicPreparedGeometry.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/prep/BasicPreparedGeometry.cpp
//   https://github.com/libgeos/geos/blob/main/include/geos/geom/prep/BasicPreparedGeometry.h

#include "third_party/geos/include/geos/geom/prep/BasicPreparedGeometry.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/IntersectionMatrix.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::IntersectionMatrix;
using geos::geom::prep::BasicPreparedGeometry;
using geos::io::WKTReader;

class TestableBasicPreparedGeometry final : public BasicPreparedGeometry {
 public:
  using BasicPreparedGeometry::BasicPreparedGeometry;
  using BasicPreparedGeometry::envelopeCovers;
  using BasicPreparedGeometry::envelopesIntersect;
  using BasicPreparedGeometry::setGeometry;
};

TEST(BasicPreparedGeometryTest, ConstructGetGeometryAndRepresentativePoints) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr point = reader.read("POINT (3 4)");
  BasicPreparedGeometry pg_point(point.get());
  EXPECT_EQ(&pg_point.getGeometry(), point.get());
  const std::vector<const CoordinateXY*>* pt_reps =
      pg_point.getRepresentativePoints();
  ASSERT_NE(pt_reps, nullptr);
  ASSERT_EQ(pt_reps->size(), 1u);
  EXPECT_DOUBLE_EQ((*pt_reps)[0]->x, 3.0);
  EXPECT_DOUBLE_EQ((*pt_reps)[0]->y, 4.0);

  Geometry::Ptr poly_with_hole = reader.read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 4 2, 4 4, 2 4, 2 2))");
  BasicPreparedGeometry pg_poly(poly_with_hole.get());
  EXPECT_EQ(&pg_poly.getGeometry(), poly_with_hole.get());
  const std::vector<const CoordinateXY*>* poly_reps =
      pg_poly.getRepresentativePoints();
  ASSERT_NE(poly_reps, nullptr);
  EXPECT_EQ(poly_reps->size(), 2u);

  Geometry::Ptr collection = reader.read(
      "GEOMETRYCOLLECTION (POINT (1 2), LINESTRING (3 4, 5 6), "
      "POLYGON ((10 10, 12 10, 12 12, 10 12, 10 10)))");
  BasicPreparedGeometry pg_gc(collection.get());
  const std::vector<const CoordinateXY*>* gc_reps =
      pg_gc.getRepresentativePoints();
  ASSERT_NE(gc_reps, nullptr);
  EXPECT_EQ(gc_reps->size(), 3u);

  Geometry::Ptr empty_pt = reader.read("POINT EMPTY");
  TestableBasicPreparedGeometry pg_mutable(empty_pt.get());
  EXPECT_TRUE(pg_mutable.getRepresentativePoints()->empty());
  pg_mutable.setGeometry(point.get());
  EXPECT_EQ(&pg_mutable.getGeometry(), point.get());
  ASSERT_EQ(pg_mutable.getRepresentativePoints()->size(), 1u);
}

TEST(BasicPreparedGeometryTest, EnvelopesIntersectAndEnvelopeCovers) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr target = reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  TestableBasicPreparedGeometry pg(target.get());

  Geometry::Ptr empty_pt = reader.read("POINT EMPTY");
  Geometry::Ptr pt_inside = reader.read("POINT (5 5)");
  Geometry::Ptr pt_boundary = reader.read("POINT (10 10)");
  Geometry::Ptr pt_outside = reader.read("POINT (15 5)");
  Geometry::Ptr line_inside = reader.read("LINESTRING (1 1, 9 9)");
  Geometry::Ptr line_overlap_env = reader.read("LINESTRING (5 5, 15 5)");
  Geometry::Ptr line_disjoint = reader.read("LINESTRING (20 20, 25 25)");
  Geometry::Ptr empty_poly = reader.read("POLYGON EMPTY");

  EXPECT_FALSE(pg.envelopesIntersect(empty_pt.get()));
  EXPECT_TRUE(pg.envelopesIntersect(pt_inside.get()));
  EXPECT_TRUE(pg.envelopesIntersect(pt_boundary.get()));
  EXPECT_FALSE(pg.envelopesIntersect(pt_outside.get()));
  EXPECT_TRUE(pg.envelopesIntersect(line_inside.get()));
  EXPECT_TRUE(pg.envelopesIntersect(line_overlap_env.get()));
  EXPECT_FALSE(pg.envelopesIntersect(line_disjoint.get()));
  EXPECT_FALSE(pg.envelopesIntersect(empty_poly.get()));

  EXPECT_FALSE(pg.envelopeCovers(empty_pt.get()));
  EXPECT_TRUE(pg.envelopeCovers(pt_inside.get()));
  EXPECT_TRUE(pg.envelopeCovers(pt_boundary.get()));
  EXPECT_FALSE(pg.envelopeCovers(pt_outside.get()));
  EXPECT_TRUE(pg.envelopeCovers(line_inside.get()));
  EXPECT_FALSE(pg.envelopeCovers(line_overlap_env.get()));
  EXPECT_FALSE(pg.envelopeCovers(line_disjoint.get()));
  EXPECT_FALSE(pg.envelopeCovers(empty_poly.get()));

  TestableBasicPreparedGeometry pg_empty(empty_poly.get());
  EXPECT_FALSE(pg_empty.envelopesIntersect(pt_inside.get()));
  EXPECT_FALSE(pg_empty.envelopesIntersect(line_inside.get()));
  EXPECT_FALSE(pg_empty.envelopeCovers(pt_inside.get()));
  EXPECT_FALSE(pg_empty.envelopeCovers(line_inside.get()));
}

TEST(BasicPreparedGeometryTest, IsAnyTargetComponentInTest) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr multi = reader.read(
      "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "((10 10, 14 10, 14 14, 10 14, 10 10)))");
  BasicPreparedGeometry pg(multi.get());

  Geometry::Ptr covers_second =
      reader.read("POLYGON ((9 9, 15 9, 15 15, 9 15, 9 9))");
  Geometry::Ptr touches_first_vertex =
      reader.read("POLYGON ((-2 -2, 0 -2, 0 0, -2 0, -2 -2))");
  Geometry::Ptr disjoint_box =
      reader.read("POLYGON ((20 20, 24 20, 24 24, 20 24, 20 20))");
  Geometry::Ptr empty_poly = reader.read("POLYGON EMPTY");

  EXPECT_TRUE(pg.isAnyTargetComponentInTest(covers_second.get()));
  EXPECT_TRUE(pg.isAnyTargetComponentInTest(touches_first_vertex.get()));
  EXPECT_FALSE(pg.isAnyTargetComponentInTest(disjoint_box.get()));
  EXPECT_FALSE(pg.isAnyTargetComponentInTest(empty_poly.get()));

  BasicPreparedGeometry pg_empty(empty_poly.get());
  EXPECT_FALSE(pg_empty.isAnyTargetComponentInTest(covers_second.get()));
}

TEST(BasicPreparedGeometryTest, TopologicalPredicatesAndRelate) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr poly = reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  BasicPreparedGeometry pg(poly.get());

  Geometry::Ptr inner_pt = reader.read("POINT (5 5)");
  Geometry::Ptr boundary_pt = reader.read("POINT (0 5)");
  Geometry::Ptr outside_pt = reader.read("POINT (20 20)");
  Geometry::Ptr outer_poly =
      reader.read("POLYGON ((-2 -2, 12 -2, 12 12, -2 12, -2 -2))");
  Geometry::Ptr same_poly =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  Geometry::Ptr overlap_poly =
      reader.read("POLYGON ((5 5, 15 5, 15 15, 5 15, 5 5))");
  Geometry::Ptr touch_poly =
      reader.read("POLYGON ((10 0, 15 0, 15 5, 10 5, 10 0))");
  Geometry::Ptr crossing_line = reader.read("LINESTRING (-5 5, 15 5)");
  Geometry::Ptr interior_line = reader.read("LINESTRING (2 2, 8 8)");
  Geometry::Ptr boundary_line = reader.read("LINESTRING (0 2, 0 8)");

  // First call lazily creates RelateNG; subsequent calls reuse cached instance.
  EXPECT_TRUE(pg.contains(inner_pt.get()));
  EXPECT_FALSE(pg.contains(boundary_pt.get()));
  EXPECT_TRUE(pg.contains(interior_line.get()));
  EXPECT_FALSE(pg.contains(boundary_line.get()));

  EXPECT_TRUE(pg.containsProperly(inner_pt.get()));
  EXPECT_TRUE(pg.containsProperly(interior_line.get()));
  EXPECT_FALSE(pg.containsProperly(boundary_pt.get()));
  EXPECT_FALSE(pg.containsProperly(same_poly.get()));

  EXPECT_TRUE(pg.within(outer_poly.get()));
  EXPECT_TRUE(pg.within(same_poly.get()));
  EXPECT_FALSE(pg.within(overlap_poly.get()));

  EXPECT_TRUE(pg.covers(boundary_pt.get()));
  EXPECT_TRUE(pg.covers(boundary_line.get()));
  EXPECT_FALSE(pg.covers(outside_pt.get()));

  EXPECT_TRUE(pg.coveredBy(outer_poly.get()));
  EXPECT_TRUE(pg.coveredBy(same_poly.get()));
  EXPECT_FALSE(pg.coveredBy(overlap_poly.get()));

  EXPECT_TRUE(pg.crosses(crossing_line.get()));
  EXPECT_FALSE(pg.crosses(interior_line.get()));
  EXPECT_FALSE(pg.crosses(overlap_poly.get()));

  EXPECT_TRUE(pg.disjoint(outside_pt.get()));
  EXPECT_FALSE(pg.disjoint(touch_poly.get()));

  EXPECT_TRUE(pg.intersects(touch_poly.get()));
  EXPECT_FALSE(pg.intersects(outside_pt.get()));

  EXPECT_TRUE(pg.overlaps(overlap_poly.get()));
  EXPECT_FALSE(pg.overlaps(same_poly.get()));
  EXPECT_FALSE(pg.overlaps(touch_poly.get()));

  EXPECT_TRUE(pg.touches(touch_poly.get()));
  EXPECT_FALSE(pg.touches(overlap_poly.get()));
  EXPECT_FALSE(pg.touches(outside_pt.get()));

  EXPECT_TRUE(pg.relate(overlap_poly.get(), "212101212"));
  EXPECT_FALSE(pg.relate(overlap_poly.get(), "FF*FF****"));

  std::unique_ptr<IntersectionMatrix> im = pg.relate(touch_poly.get());
  ASSERT_NE(im, nullptr);
  EXPECT_EQ(im->toString(), "FF2F11212");

  // GeometryCollection is supported by RelateNG.
  Geometry::Ptr gc =
      reader.read("GEOMETRYCOLLECTION (POINT (5 5), LINESTRING (1 1, 2 2))");
  EXPECT_TRUE(pg.contains(gc.get()));
  EXPECT_TRUE(pg.intersects(gc.get()));
}

TEST(BasicPreparedGeometryTest,
     DistanceNearestPointsIsWithinDistanceAndToString) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr poly = reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  BasicPreparedGeometry pg(poly.get());

  Geometry::Ptr pt_far = reader.read("POINT (13 14)");
  Geometry::Ptr pt_inside = reader.read("POINT (4 4)");
  Geometry::Ptr pt_empty = reader.read("POINT EMPTY");

  // Nearest point on [0,10]x[0,10] to (13,14) is (10,10), distance 5.
  std::unique_ptr<CoordinateSequence> nearest = pg.nearestPoints(pt_far.get());
  ASSERT_NE(nearest, nullptr);
  ASSERT_EQ(nearest->size(), 2u);
  EXPECT_DOUBLE_EQ(nearest->getX(0), 10.0);
  EXPECT_DOUBLE_EQ(nearest->getY(0), 10.0);
  EXPECT_DOUBLE_EQ(nearest->getX(1), 13.0);
  EXPECT_DOUBLE_EQ(nearest->getY(1), 14.0);

  EXPECT_DOUBLE_EQ(pg.distance(pt_far.get()), 5.0);
  EXPECT_DOUBLE_EQ(pg.distance(pt_inside.get()), 0.0);

  EXPECT_TRUE(pg.isWithinDistance(pt_far.get(), 5.0));
  EXPECT_TRUE(pg.isWithinDistance(pt_far.get(), 6.0));
  EXPECT_FALSE(pg.isWithinDistance(pt_far.get(), 4.999));

  // Empty test geometry: nearestPoints returns nullptr, distance returns +inf.
  EXPECT_EQ(pg.nearestPoints(pt_empty.get()), nullptr);
  const double empty_dist = pg.distance(pt_empty.get());
  EXPECT_TRUE(std::isinf(empty_dist));
  EXPECT_GT(empty_dist, 0.0);
  EXPECT_FALSE(pg.isWithinDistance(pt_empty.get(), 10.0));

  // Empty base geometry behaves symmetrically.
  BasicPreparedGeometry pg_empty(pt_empty.get());
  EXPECT_EQ(pg_empty.nearestPoints(pt_far.get()), nullptr);
  EXPECT_TRUE(std::isinf(pg_empty.distance(pt_far.get())));
  EXPECT_FALSE(pg_empty.isWithinDistance(pt_far.get(), 10.0));

  EXPECT_EQ(pg.toString(), poly->toString());
}

}  // namespace
}  // namespace geos_google
