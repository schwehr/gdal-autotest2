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

#include "third_party/geos/include/geos/geom/prep/PreparedLineString.h"

#include <cmath>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/prep/PreparedGeometry.h"
#include "third_party/geos/include/geos/geom/prep/PreparedGeometryFactory.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/noding/FastSegmentSetIntersectionFinder.h"
#include "third_party/geos/include/geos/operation/distance/IndexedFacetDistance.h"
#include "third_party/geos/include/geos/util/GEOSException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::prep::PreparedGeometry;
using geos::geom::prep::PreparedGeometryFactory;
using geos::geom::prep::PreparedLineString;
using geos::io::WKTReader;
using geos::noding::FastSegmentSetIntersectionFinder;
using geos::operation::distance::IndexedFacetDistance;
using geos::util::GEOSException;
using geos::util::UnsupportedOperationException;

class PreparedLineStringTest : public ::testing::Test {
 protected:
  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};
};

TEST_F(PreparedLineStringTest,
       DestructorSucceedsWithAndWithoutSegmentExtraction) {
  auto line = ReadWkt("LINESTRING (0 0, 10 10)");
  auto multi_line =
      ReadWkt("MULTILINESTRING ((0 0, 2 2), (4 4, 6 6), (8 8, 10 10))");
  auto ring = ReadWkt("LINEARRING (0 0, 4 0, 4 4, 0 4, 0 0)");
  auto far_pt = ReadWkt("POINT (50 50)");

  {
    // Constructed and destroyed without extracting segments.
    PreparedLineString unused_prep(line.get());
    EXPECT_EQ(&unused_prep.getGeometry(), line.get());
  }

  {
    // Envelope-disjoint intersects short-circuits before segment extraction.
    PreparedLineString env_only_prep(line.get());
    EXPECT_FALSE(env_only_prep.intersects(far_pt.get()));
  }

  {
    // Destructor frees multiple SegmentStrings when intersection finder used.
    PreparedLineString mls_prep(multi_line.get());
    EXPECT_NE(mls_prep.getIntersectionFinder(), nullptr);
  }

  {
    // Destructor frees LinearRing SegmentString when intersection finder used.
    PreparedLineString ring_prep(ring.get());
    EXPECT_NE(ring_prep.getIntersectionFinder(), nullptr);
  }
}

TEST_F(PreparedLineStringTest, GetIntersectionFinderCachesInstance) {
  auto mls = ReadWkt("MULTILINESTRING ((0 0, 4 0), (0 2, 4 2))");
  PreparedLineString prep(mls.get());

  FastSegmentSetIntersectionFinder* first = prep.getIntersectionFinder();
  FastSegmentSetIntersectionFinder* second = prep.getIntersectionFinder();
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first, second);
}

TEST_F(PreparedLineStringTest, GetIndexedFacetDistanceCachesInstance) {
  auto line = ReadWkt("LINESTRING (0 0, 10 0)");
  const PreparedLineString prep(line.get());

  IndexedFacetDistance* first = prep.getIndexedFacetDistance();
  IndexedFacetDistance* second = prep.getIndexedFacetDistance();
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first, second);
}

TEST_F(PreparedLineStringTest,
       IntersectsThrowsUnsupportedOperationForCurvedInput) {
  auto line = ReadWkt("LINESTRING (0 0, 10 0)");
  auto curve = ReadWkt("CIRCULARSTRING (0 0, 1 1, 2 0)");
  auto compound =
      ReadWkt("COMPOUNDCURVE (CIRCULARSTRING (0 0, 1 1, 2 0), (2 0, 3 0))");

  PreparedLineString prep(line.get());
  EXPECT_THROW(prep.intersects(curve.get()), UnsupportedOperationException);
  EXPECT_THROW(prep.intersects(compound.get()), UnsupportedOperationException);
}

TEST_F(PreparedLineStringTest, IntersectsShortCircuitsOnDisjointEnvelopes) {
  auto line = ReadWkt("LINESTRING (0 0, 10 10)");
  auto far_pt = ReadWkt("POINT (20 20)");
  auto far_line = ReadWkt("LINESTRING (15 0, 20 5)");
  auto far_poly = ReadWkt("POLYGON ((20 0, 25 0, 25 5, 20 5, 20 0))");
  auto empty_pt = ReadWkt("POINT EMPTY");
  auto empty_line = ReadWkt("LINESTRING EMPTY");

  PreparedLineString prep(line.get());
  EXPECT_FALSE(prep.intersects(far_pt.get()));
  EXPECT_FALSE(prep.intersects(far_line.get()));
  EXPECT_FALSE(prep.intersects(far_poly.get()));
  EXPECT_FALSE(prep.intersects(empty_pt.get()));
  EXPECT_FALSE(prep.intersects(empty_line.get()));

  PreparedLineString empty_prep(empty_line.get());
  EXPECT_FALSE(empty_prep.intersects(line.get()));
  EXPECT_FALSE(empty_prep.intersects(empty_pt.get()));
}

TEST_F(PreparedLineStringTest, IntersectsHandlesPointLineAndPolygonCases) {
  auto diag = ReadWkt("LINESTRING (0 0, 10 10)");
  PreparedLineString prep(diag.get());

  // Point / MultiPoint inside envelope: on vs off line.
  auto pt_on_interior = ReadWkt("POINT (5 5)");
  auto pt_on_endpoint = ReadWkt("POINT (0 0)");
  auto pt_off_in_env = ReadWkt("POINT (2 8)");
  auto mpt_hit = ReadWkt("MULTIPOINT ((2 8), (5 5))");
  auto mpt_miss = ReadWkt("MULTIPOINT ((2 8), (8 2))");

  EXPECT_TRUE(prep.intersects(pt_on_interior.get()));
  EXPECT_TRUE(prep.intersects(pt_on_endpoint.get()));
  EXPECT_FALSE(prep.intersects(pt_off_in_env.get()));
  EXPECT_TRUE(prep.intersects(mpt_hit.get()));
  EXPECT_FALSE(prep.intersects(mpt_miss.get()));

  // Lineal inputs with overlapping envelopes.
  auto crossing_line = ReadWkt("LINESTRING (0 10, 10 0)");
  auto touching_line = ReadWkt("LINESTRING (10 10, 15 10)");
  auto overlap_line = ReadWkt("LINESTRING (2 2, 6 6)");
  auto disjoint_in_env = ReadWkt("LINESTRING (0 8, 2 10)");
  auto ring_cross = ReadWkt("LINEARRING (2 0, 6 0, 6 4, 2 4, 2 0)");
  auto ring_miss = ReadWkt("LINEARRING (0 7, 3 7, 3 10, 0 10, 0 7)");
  auto mls_hit = ReadWkt("MULTILINESTRING ((0 8, 2 10), (3 0, 3 6))");
  auto mls_miss = ReadWkt("MULTILINESTRING ((0 8, 2 10), (8 0, 10 2))");

  EXPECT_TRUE(prep.intersects(crossing_line.get()));
  EXPECT_TRUE(prep.intersects(touching_line.get()));
  EXPECT_TRUE(prep.intersects(overlap_line.get()));
  EXPECT_FALSE(prep.intersects(disjoint_in_env.get()));
  EXPECT_TRUE(prep.intersects(ring_cross.get()));
  EXPECT_FALSE(prep.intersects(ring_miss.get()));
  EXPECT_TRUE(prep.intersects(mls_hit.get()));
  EXPECT_FALSE(prep.intersects(mls_miss.get()));

  // Areal inputs: boundary crossing, strict enclosure, hole exclusion.
  auto enclosing_poly =
      ReadWkt("POLYGON ((-5 -5, 15 -5, 15 15, -5 15, -5 -5))");
  auto poly_with_hole = ReadWkt(
      "POLYGON ((-5 -5, 15 -5, 15 15, -5 15, -5 -5), "
      "(-1 -1, 11 -1, 11 11, -1 11, -1 -1))");
  auto crossing_poly = ReadWkt("POLYGON ((2 0, 8 0, 8 5, 2 5, 2 0))");
  auto mpoly_hit = ReadWkt(
      "MULTIPOLYGON (((20 20, 22 20, 22 22, 20 22, 20 20)), "
      "((-5 -5, 15 -5, 15 15, -5 15, -5 -5)))");
  auto gc_hit = ReadWkt(
      "GEOMETRYCOLLECTION (POINT (1 9), POLYGON ((2 0, 8 0, 8 5, 2 5, 2 0)))");
  auto gc_miss =
      ReadWkt("GEOMETRYCOLLECTION (POINT (1 9), LINESTRING (8 0, 10 2))");

  EXPECT_TRUE(prep.intersects(enclosing_poly.get()));
  EXPECT_FALSE(prep.intersects(poly_with_hole.get()));
  EXPECT_TRUE(prep.intersects(crossing_poly.get()));
  EXPECT_TRUE(prep.intersects(mpoly_hit.get()));
  EXPECT_TRUE(prep.intersects(gc_hit.get()));
  EXPECT_FALSE(prep.intersects(gc_miss.get()));
}

TEST_F(PreparedLineStringTest, IntersectsWorksForLinearRingAndMultiLineString) {
  auto ring = ReadWkt("LINEARRING (0 0, 4 0, 4 4, 0 4, 0 0)");
  auto mls = ReadWkt("MULTILINESTRING ((0 0, 4 0), (10 10, 14 10))");
  auto pt_on_ring = ReadWkt("POINT (4 2)");
  auto pt_in_ring_interior = ReadWkt("POINT (2 2)");
  auto ring_enclosing_poly =
      ReadWkt("POLYGON ((-1 -1, 5 -1, 5 5, -1 5, -1 -1))");
  auto pt_on_first_ls = ReadWkt("POINT (2 0)");
  auto pt_on_second_ls = ReadWkt("POINT (12 10)");
  auto pt_between = ReadWkt("POINT (7 5)");
  auto poly_over_second_ls = ReadWkt("POLYGON ((9 9, 15 9, 15 11, 9 11, 9 9))");

  PreparedLineString prep_ring(ring.get());
  EXPECT_TRUE(prep_ring.intersects(pt_on_ring.get()));
  EXPECT_FALSE(prep_ring.intersects(pt_in_ring_interior.get()));
  EXPECT_TRUE(prep_ring.intersects(ring_enclosing_poly.get()));

  PreparedLineString prep_mls(mls.get());
  EXPECT_TRUE(prep_mls.intersects(pt_on_first_ls.get()));
  EXPECT_TRUE(prep_mls.intersects(pt_on_second_ls.get()));
  EXPECT_FALSE(prep_mls.intersects(pt_on_ring.get()));
  EXPECT_FALSE(prep_mls.intersects(pt_between.get()));
  EXPECT_TRUE(prep_mls.intersects(poly_over_second_ls.get()));
}

TEST_F(PreparedLineStringTest,
       NearestPointsComputesClosestPairsAndThrowsEmpty) {
  auto line = ReadWkt("LINESTRING (0 0, 10 0)");
  PreparedLineString prep(line.get());

  auto pt = ReadWkt("POINT (3 4)");
  std::unique_ptr<CoordinateSequence> pts_pt = prep.nearestPoints(pt.get());
  ASSERT_NE(pts_pt, nullptr);
  ASSERT_EQ(pts_pt->size(), 2u);
  CoordinateXY p0;
  CoordinateXY p1;
  pts_pt->getAt(0, p0);
  pts_pt->getAt(1, p1);
  EXPECT_DOUBLE_EQ(p0.x, 3.0);
  EXPECT_DOUBLE_EQ(p0.y, 0.0);
  EXPECT_DOUBLE_EQ(p1.x, 3.0);
  EXPECT_DOUBLE_EQ(p1.y, 4.0);

  auto crossing = ReadWkt("LINESTRING (5 -2, 5 2)");
  std::unique_ptr<CoordinateSequence> pts_cross =
      prep.nearestPoints(crossing.get());
  ASSERT_EQ(pts_cross->size(), 2u);
  pts_cross->getAt(0, p0);
  pts_cross->getAt(1, p1);
  EXPECT_DOUBLE_EQ(p0.x, 5.0);
  EXPECT_DOUBLE_EQ(p0.y, 0.0);
  EXPECT_DOUBLE_EQ(p1.x, 5.0);
  EXPECT_DOUBLE_EQ(p1.y, 0.0);

  auto poly = ReadWkt("POLYGON ((2 3, 8 3, 8 7, 2 7, 2 3))");
  std::unique_ptr<CoordinateSequence> pts_poly = prep.nearestPoints(poly.get());
  ASSERT_EQ(pts_poly->size(), 2u);
  pts_poly->getAt(0, p0);
  pts_poly->getAt(1, p1);
  EXPECT_DOUBLE_EQ(p0.distance(p1), 3.0);

  auto empty_pt = ReadWkt("POINT EMPTY");
  EXPECT_THROW(prep.nearestPoints(empty_pt.get()), GEOSException);

  auto empty_line = ReadWkt("LINESTRING EMPTY");
  PreparedLineString empty_prep(empty_line.get());
  EXPECT_THROW(empty_prep.nearestPoints(pt.get()), GEOSException);
}

TEST_F(PreparedLineStringTest, DistanceAndIsWithinDistanceDelegateCorrectly) {
  auto line = ReadWkt("LINESTRING (2 2, 8 2)");
  auto pt_above = ReadWkt("POINT (5 5)");
  auto enclosing_poly = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto disjoint_poly = ReadWkt("POLYGON ((0 6, 10 6, 10 10, 0 10, 0 6))");
  auto empty_line = ReadWkt("LINESTRING EMPTY");

  PreparedLineString prep(line.get());
  EXPECT_DOUBLE_EQ(prep.distance(pt_above.get()), 3.0);
  EXPECT_TRUE(prep.isWithinDistance(pt_above.get(), 3.0));
  EXPECT_FALSE(prep.isWithinDistance(pt_above.get(), 2.9));

  // Strictly enclosed line has distance 0 despite positive boundary distance.
  EXPECT_DOUBLE_EQ(prep.distance(enclosing_poly.get()), 0.0);
  EXPECT_TRUE(prep.isWithinDistance(enclosing_poly.get(), 0.0));
  EXPECT_TRUE(prep.isWithinDistance(enclosing_poly.get(), 0.1));

  EXPECT_DOUBLE_EQ(prep.distance(disjoint_poly.get()), 4.0);
  EXPECT_FALSE(prep.isWithinDistance(disjoint_poly.get(), 3.9));
  EXPECT_TRUE(prep.isWithinDistance(disjoint_poly.get(), 4.0));

  const double expected_inf = geos::DoubleInfinity;
  const double empty_dist = prep.distance(empty_line.get());
  EXPECT_TRUE(std::isinf(empty_dist));
  EXPECT_EQ(empty_dist, expected_inf);
  EXPECT_FALSE(prep.isWithinDistance(empty_line.get(), 100.0));

  PreparedLineString empty_prep(empty_line.get());
  const double prep_empty_dist = empty_prep.distance(pt_above.get());
  EXPECT_TRUE(std::isinf(prep_empty_dist));
  EXPECT_EQ(prep_empty_dist, expected_inf);
  EXPECT_FALSE(empty_prep.isWithinDistance(pt_above.get(), 100.0));
}

TEST_F(PreparedLineStringTest, BasePreparedGeometryPredicatesConsistent) {
  auto line = ReadWkt("LINESTRING (0 0, 10 0)");
  auto sub_line = ReadWkt("LINESTRING (2 0, 8 0)");
  auto cross_line = ReadWkt("LINESTRING (5 -5, 5 5)");
  auto touch_line = ReadWkt("LINESTRING (10 0, 12 0)");
  auto far_line = ReadWkt("LINESTRING (0 5, 10 5)");
  auto enclosing_poly = ReadWkt("POLYGON ((-1 -1, 11 -1, 11 1, -1 1, -1 -1))");

  std::unique_ptr<PreparedGeometry> pg =
      PreparedGeometryFactory::prepare(line.get());
  ASSERT_NE(pg, nullptr);
  EXPECT_EQ(&pg->getGeometry(), line.get());
  EXPECT_TRUE(pg->contains(sub_line.get()));
  EXPECT_TRUE(pg->covers(sub_line.get()));
  EXPECT_TRUE(pg->crosses(cross_line.get()));
  EXPECT_TRUE(pg->touches(touch_line.get()));
  EXPECT_TRUE(pg->disjoint(far_line.get()));
  EXPECT_TRUE(pg->within(enclosing_poly.get()));
  EXPECT_TRUE(pg->coveredBy(enclosing_poly.get()));
  EXPECT_TRUE(pg->relate(cross_line.get(), "0********"));
}

TEST_F(PreparedLineStringTest,
       FactoryPrepareCreatesPreparedLineStringAndDispatchesVirtually) {
  auto line = ReadWkt("LINESTRING (0 0, 6 0)");
  auto ring = ReadWkt("LINEARRING (0 0, 4 0, 4 4, 0 4, 0 0)");
  auto mls = ReadWkt("MULTILINESTRING ((0 0, 4 0), (10 0, 14 0))");
  auto pt_near = ReadWkt("POINT (12 3)");
  auto pt_on = ReadWkt("POINT (2 0)");

  std::unique_ptr<PreparedGeometry> pg_line =
      PreparedGeometryFactory::prepare(line.get());
  std::unique_ptr<PreparedGeometry> pg_ring =
      PreparedGeometryFactory::prepare(ring.get());
  std::unique_ptr<PreparedGeometry> pg_mls =
      PreparedGeometryFactory::prepare(mls.get());

  EXPECT_NE(dynamic_cast<const PreparedLineString*>(pg_line.get()), nullptr);
  EXPECT_NE(dynamic_cast<const PreparedLineString*>(pg_ring.get()), nullptr);
  EXPECT_NE(dynamic_cast<const PreparedLineString*>(pg_mls.get()), nullptr);

  EXPECT_FALSE(pg_mls->intersects(pt_near.get()));
  EXPECT_TRUE(pg_mls->intersects(pt_on.get()));
  EXPECT_DOUBLE_EQ(pg_mls->distance(pt_near.get()), 3.0);
  EXPECT_DOUBLE_EQ(pg_mls->distance(pt_on.get()), 0.0);
  EXPECT_TRUE(pg_mls->isWithinDistance(pt_near.get(), 3.0));
  EXPECT_FALSE(pg_mls->isWithinDistance(pt_near.get(), 2.99));

  std::unique_ptr<CoordinateSequence> pts =
      pg_mls->nearestPoints(pt_near.get());
  ASSERT_NE(pts, nullptr);
  ASSERT_EQ(pts->size(), 2u);
  CoordinateXY p0;
  CoordinateXY p1;
  pts->getAt(0, p0);
  pts->getAt(1, p1);
  EXPECT_DOUBLE_EQ(p0.x, 12.0);
  EXPECT_DOUBLE_EQ(p0.y, 0.0);
  EXPECT_DOUBLE_EQ(p1.x, 12.0);
  EXPECT_DOUBLE_EQ(p1.y, 3.0);
}

}  // namespace
}  // namespace geos_google
