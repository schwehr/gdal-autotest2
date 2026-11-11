// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "third_party/geos/include/geos/operation/overlay/snap/GeometrySnapper.h"

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GeomPtrPair;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::operation::overlay::snap::GeometrySnapper;
using geos::util::IllegalArgumentException;

std::unique_ptr<Geometry> ReadWkt(const GeometryFactory& factory,
                                  const std::string& wkt) {
  WKTReader reader(factory);
  return reader.read(wkt);
}

TEST(GeometrySnapperTest, ComputeSizeBasedSnapTolerance) {
  auto factory = GeometryFactory::create();

  auto wide_poly = ReadWkt(*factory, "POLYGON ((0 0, 10 0, 10 2, 0 2, 0 0))");
  EXPECT_DOUBLE_EQ(GeometrySnapper::computeSizeBasedSnapTolerance(*wide_poly),
                   2.0 * 1e-9);

  auto tall_poly = ReadWkt(*factory, "POLYGON ((0 0, 3 0, 3 10, 0 10, 0 0))");
  EXPECT_DOUBLE_EQ(GeometrySnapper::computeSizeBasedSnapTolerance(*tall_poly),
                   3.0 * 1e-9);

  auto horiz_line = ReadWkt(*factory, "LINESTRING (0 0, 10 0)");
  EXPECT_DOUBLE_EQ(GeometrySnapper::computeSizeBasedSnapTolerance(*horiz_line),
                   0.0);

  auto point = ReadWkt(*factory, "POINT (4 5)");
  EXPECT_DOUBLE_EQ(GeometrySnapper::computeSizeBasedSnapTolerance(*point), 0.0);

  auto empty_poly = ReadWkt(*factory, "POLYGON EMPTY");
  EXPECT_DOUBLE_EQ(GeometrySnapper::computeSizeBasedSnapTolerance(*empty_poly),
                   0.0);

  auto neg_square =
      ReadWkt(*factory, "POLYGON ((-5 -5, 5 -5, 5 5, -5 5, -5 -5))");
  EXPECT_DOUBLE_EQ(GeometrySnapper::computeSizeBasedSnapTolerance(*neg_square),
                   10.0 * 1e-9);

  auto empty_gc = ReadWkt(*factory, "GEOMETRYCOLLECTION EMPTY");
  EXPECT_DOUBLE_EQ(GeometrySnapper::computeSizeBasedSnapTolerance(*empty_gc),
                   0.0);
}

TEST(GeometrySnapperTest, ComputeOverlaySnapToleranceSingleGeometry) {
  auto floating_factory = GeometryFactory::create();
  auto unit_square =
      ReadWkt(*floating_factory, "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  EXPECT_DOUBLE_EQ(GeometrySnapper::computeOverlaySnapTolerance(*unit_square),
                   1e-9);

  PrecisionModel single_pm(PrecisionModel::FLOATING_SINGLE);
  auto single_factory = GeometryFactory::create(&single_pm);
  auto single_square =
      ReadWkt(*single_factory, "POLYGON ((0 0, 4 0, 4 5, 0 5, 0 0))");
  EXPECT_DOUBLE_EQ(GeometrySnapper::computeOverlaySnapTolerance(*single_square),
                   4.0 * 1e-9);

  // Coarse FIXED grid: fixedSnapTol = (1 / 10.0) * 2 / 1.415 > 1e-9.
  PrecisionModel coarse_pm(10.0);
  auto coarse_factory = GeometryFactory::create(&coarse_pm);
  auto coarse_square =
      ReadWkt(*coarse_factory, "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  EXPECT_DOUBLE_EQ(GeometrySnapper::computeOverlaySnapTolerance(*coarse_square),
                   (1.0 / 10.0) * 2.0 / 1.415);

  // Empty FIXED-grid geometry still uses fixedSnapTol since size tol is 0.
  auto coarse_empty = ReadWkt(*coarse_factory, "POLYGON EMPTY");
  EXPECT_DOUBLE_EQ(GeometrySnapper::computeOverlaySnapTolerance(*coarse_empty),
                   (1.0 / 10.0) * 2.0 / 1.415);

  // Very fine FIXED grid on a large geometry: size-based tolerance dominates.
  PrecisionModel fine_pm(1e11);
  auto fine_factory = GeometryFactory::create(&fine_pm);
  auto large_square =
      ReadWkt(*fine_factory, "POLYGON ((0 0, 100 0, 100 100, 0 100, 0 0))");
  EXPECT_DOUBLE_EQ(GeometrySnapper::computeOverlaySnapTolerance(*large_square),
                   100.0 * 1e-9);
}

TEST(GeometrySnapperTest, ComputeOverlaySnapTolerancePairUsesMinimum) {
  auto floating_factory = GeometryFactory::create();
  auto small_poly =
      ReadWkt(*floating_factory, "POLYGON ((0 0, 2 0, 2 10, 0 10, 0 0))");
  auto large_poly =
      ReadWkt(*floating_factory, "POLYGON ((0 0, 8 0, 8 8, 0 8, 0 0))");

  EXPECT_DOUBLE_EQ(
      GeometrySnapper::computeOverlaySnapTolerance(*small_poly, *large_poly),
      2.0 * 1e-9);
  EXPECT_DOUBLE_EQ(
      GeometrySnapper::computeOverlaySnapTolerance(*large_poly, *small_poly),
      2.0 * 1e-9);

  PrecisionModel fixed_pm(10.0);
  auto fixed_factory = GeometryFactory::create(&fixed_pm);
  auto fixed_poly =
      ReadWkt(*fixed_factory, "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  EXPECT_DOUBLE_EQ(
      GeometrySnapper::computeOverlaySnapTolerance(*fixed_poly, *large_poly),
      8.0 * 1e-9);

  auto empty_geom = ReadWkt(*floating_factory, "LINESTRING EMPTY");
  EXPECT_DOUBLE_EQ(
      GeometrySnapper::computeOverlaySnapTolerance(*empty_geom, *large_poly),
      0.0);
}

TEST(GeometrySnapperTest, SnapToPointAndMultiPoint) {
  auto factory = GeometryFactory::create();
  auto src_pt = ReadWkt(*factory, "POINT (1.02 2.0)");
  auto target_pt = ReadWkt(*factory, "POINT (1.0 2.0)");

  GeometrySnapper snapper(*src_pt);
  auto snapped_within = snapper.snapTo(*target_pt, 0.05);
  EXPECT_TRUE(snapped_within->equalsExact(target_pt.get()));

  auto unchanged_outside = snapper.snapTo(*target_pt, 0.01);
  EXPECT_TRUE(unchanged_outside->equalsExact(src_pt.get()));

  auto multi_pt = ReadWkt(*factory, "MULTIPOINT ((0.02 0), (10 10))");
  auto target_poly = ReadWkt(*factory, "POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0))");
  GeometrySnapper mp_snapper(*multi_pt);
  auto snapped_mp = mp_snapper.snapTo(*target_poly, 0.1);
  auto expected_mp = ReadWkt(*factory, "MULTIPOINT ((0 0), (10 10))");
  EXPECT_TRUE(snapped_mp->equalsExact(expected_mp.get()));
}

TEST(GeometrySnapperTest, SnapToLineStringVerticesSegmentsAndDeduplication) {
  auto factory = GeometryFactory::create();
  auto src_line = ReadWkt(*factory, "LINESTRING (0 0, 10 0)");
  // Closed target ring repeats (0 0); extractTargetCoordinates deduplicates it.
  auto target_ring =
      ReadWkt(*factory, "LINEARRING (0.04 0, 5 0.05, 5 5, 0.04 0)");

  GeometrySnapper snapper(*src_line);
  auto snapped = snapper.snapTo(*target_ring, 0.1);
  auto expected = ReadWkt(*factory, "LINESTRING (0.04 0, 5 0.05, 10 0)");
  EXPECT_TRUE(snapped->equalsExact(expected.get()));

  // Zero and negative tolerances perform no snapping.
  EXPECT_TRUE(snapper.snapTo(*target_ring, 0.0)->equalsExact(src_line.get()));
  EXPECT_TRUE(snapper.snapTo(*target_ring, -1.0)->equalsExact(src_line.get()));
}

TEST(GeometrySnapperTest, SnapToPolygonWithHoleCollectionsAndEmpty) {
  auto factory = GeometryFactory::create();
  auto src_poly = ReadWkt(
      *factory,
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 8 2, 8 8, 2 8, 2 2))");
  auto target_pts = ReadWkt(*factory, "MULTIPOINT ((0.03 0.02), (2.02 2))");

  GeometrySnapper poly_snapper(*src_poly);
  auto snapped_poly = poly_snapper.snapTo(*target_pts, 0.1);
  auto expected_poly = ReadWkt(*factory,
                               "POLYGON ((0.03 0.02, 10 0, 10 10, 0 10, "
                               "0.03 0.02), (2.02 2, 8 2, 8 8, 2 8, 2.02 2))");
  EXPECT_TRUE(snapped_poly->equalsExact(expected_poly.get()));

  // GeometryTransformer simplifies a single-element MultiLineString to a
  // LineString via GeometryFactory::buildGeometry.
  auto src_gc = ReadWkt(
      *factory,
      "GEOMETRYCOLLECTION (POINT (0.02 0), MULTILINESTRING ((0 0, 10 0)))");
  auto target_pt = ReadWkt(*factory, "POINT (0 0)");
  GeometrySnapper gc_snapper(*src_gc);
  auto snapped_gc = gc_snapper.snapTo(*target_pt, 0.1);
  auto expected_gc = ReadWkt(
      *factory, "GEOMETRYCOLLECTION (POINT (0 0), LINESTRING (0 0, 10 0))");
  EXPECT_TRUE(snapped_gc->equalsExact(expected_gc.get()));

  // A multi-element MultiLineString retains its type inside a collection.
  auto src_multi_gc =
      ReadWkt(*factory,
              "GEOMETRYCOLLECTION (POINT (0.02 0), "
              "MULTILINESTRING ((0 0, 10 0), (20 0.03, 30 0)))");
  auto target_mp = ReadWkt(*factory, "MULTIPOINT ((0 0), (20 0))");
  auto snapped_multi_gc =
      GeometrySnapper(*src_multi_gc).snapTo(*target_mp, 0.1);
  auto expected_multi_gc =
      ReadWkt(*factory,
              "GEOMETRYCOLLECTION (POINT (0 0), "
              "MULTILINESTRING ((0 0, 10 0), (20 0, 30 0)))");
  EXPECT_TRUE(snapped_multi_gc->equalsExact(expected_multi_gc.get()));

  auto empty_line = ReadWkt(*factory, "LINESTRING EMPTY");
  GeometrySnapper empty_snapper(*empty_line);
  auto snapped_empty = empty_snapper.snapTo(*target_pt, 0.1);
  EXPECT_TRUE(snapped_empty->isEmpty());
  EXPECT_EQ(snapped_empty->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);

  auto empty_gc = ReadWkt(*factory, "GEOMETRYCOLLECTION EMPTY");
  auto snapped_empty_gc = GeometrySnapper(*empty_gc).snapTo(*target_pt, 0.1);
  EXPECT_TRUE(snapped_empty_gc->isEmpty());
  EXPECT_EQ(snapped_empty_gc->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);

  auto empty_target = ReadWkt(*factory, "POINT EMPTY");
  EXPECT_TRUE(
      poly_snapper.snapTo(*empty_target, 0.1)->equalsExact(src_poly.get()));
}

TEST(GeometrySnapperTest, StaticSnapPairSnapsBothGeometries) {
  auto factory = GeometryFactory::create();
  auto g0 = ReadWkt(*factory, "LINESTRING (0 0, 5 0.05, 10 0)");
  auto g1 = ReadWkt(*factory, "LINESTRING (0.04 0, 10 0)");

  GeomPtrPair snapped;
  GeometrySnapper::snap(*g0, *g1, 0.1, snapped);
  ASSERT_NE(snapped.first, nullptr);
  ASSERT_NE(snapped.second, nullptr);

  // g0 snaps vertex (0,0) -> (0.04,0), while g1 then snaps to g0's (5, 0.05).
  auto expected0 = ReadWkt(*factory, "LINESTRING (0.04 0, 5 0.05, 10 0)");
  auto expected1 = ReadWkt(*factory, "LINESTRING (0.04 0, 5 0.05, 10 0)");
  EXPECT_TRUE(snapped.first->equalsExact(expected0.get()));
  EXPECT_TRUE(snapped.second->equalsExact(expected1.get()));
}

TEST(GeometrySnapperTest, SnapToSelfPolygonCleanAndUnclean) {
  auto factory = GeometryFactory::create();
  // Hole bottom edge lies 0.05 below the outer shell bottom edge; snapping the
  // shell edge onto the hole vertices makes the shell touch the hole along a
  // segment, producing an invalid polygon unless cleaned with buffer(0).
  auto poly_with_near_hole = ReadWkt(*factory,
                                     "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
                                     "(2 0.05, 8 0.05, 8 8, 2 8, 2 0.05))");
  ASSERT_TRUE(poly_with_near_hole->isValid());

  GeometrySnapper snapper(*poly_with_near_hole);
  auto uncleaned = snapper.snapToSelf(0.2, false);
  EXPECT_EQ(uncleaned->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_FALSE(uncleaned->isValid());

  auto cleaned_member = snapper.snapToSelf(0.2, true);
  EXPECT_EQ(cleaned_member->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(cleaned_member->isValid());
  EXPECT_NEAR(cleaned_member->getArea(), uncleaned->getArea(), 1e-9);

  auto cleaned_static =
      GeometrySnapper::snapToSelf(*poly_with_near_hole, 0.2, true);
  EXPECT_TRUE(cleaned_static->isValid());
  EXPECT_TRUE(cleaned_static->equals(cleaned_member.get()));
}

TEST(GeometrySnapperTest, SnapToSelfMultiPolygonClean) {
  auto factory = GeometryFactory::create();
  // Two polygons separated by a 0.05 gap; self-snapping cracks the top shell
  // down onto the lower polygon's vertices, creating an invalid MultiPolygon
  // sharing an edge, which cleanResult=true repairs via buffer(0).
  auto multipoly = ReadWkt(*factory,
                           "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0)), "
                           "((2 0.05, 8 0.05, 8 -5, 2 -5, 2 0.05)))");

  auto uncleaned = GeometrySnapper::snapToSelf(*multipoly, 0.2, false);
  EXPECT_EQ(uncleaned->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_FALSE(uncleaned->isValid());

  auto cleaned = GeometrySnapper::snapToSelf(*multipoly, 0.2, true);
  EXPECT_EQ(cleaned->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(cleaned->isValid());
}

TEST(GeometrySnapperTest, SnapToSelfNonPolygonalIgnoresCleanFlag) {
  auto factory = GeometryFactory::create();
  auto mls = ReadWkt(*factory, "MULTILINESTRING ((0 0, 10 0), (5 0.05, 5 5))");
  auto snapped_mls = GeometrySnapper::snapToSelf(*mls, 0.2, true);
  auto expected_mls =
      ReadWkt(*factory, "MULTILINESTRING ((0 0, 5 0.05, 10 0), (5 0.05, 5 5))");
  EXPECT_EQ(snapped_mls->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_TRUE(snapped_mls->equalsExact(expected_mls.get()));

  auto pt = ReadWkt(*factory, "POINT (1 2)");
  auto snapped_pt = GeometrySnapper::snapToSelf(*pt, 0.2, true);
  EXPECT_EQ(snapped_pt->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(snapped_pt->equalsExact(pt.get()));
}

TEST(GeometrySnapperTest, SnapToLinearRingKeepsRingClosed) {
  auto factory = GeometryFactory::create();
  auto src_ring = ReadWkt(*factory, "LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  auto target_pt = ReadWkt(*factory, "POINT (0.02 0.02)");

  auto snapped = GeometrySnapper(*src_ring).snapTo(*target_pt, 0.1);
  EXPECT_EQ(snapped->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  auto expected =
      ReadWkt(*factory, "LINEARRING (0.02 0.02, 10 0, 10 10, 0 10, 0.02 0.02)");
  EXPECT_TRUE(snapped->equalsExact(expected.get()));
}

TEST(GeometrySnapperTest, StaticSnapPairWithPolygonsAndEmpty) {
  auto factory = GeometryFactory::create();
  auto g0 =
      ReadWkt(*factory, "POLYGON ((0 0, 5 0.03, 10 0, 10 10, 0 10, 0 0))");
  auto g1 = ReadWkt(
      *factory, "POLYGON ((0.03 -5, 10 -5, 10 -0.04, 0.03 -0.04, 0.03 -5))");

  GeomPtrPair snapped;
  GeometrySnapper::snap(*g0, *g1, 0.1, snapped);
  ASSERT_NE(snapped.first, nullptr);
  ASSERT_NE(snapped.second, nullptr);
  EXPECT_TRUE(snapped.first->isValid());
  EXPECT_TRUE(snapped.second->isValid());

  auto expected0 = ReadWkt(
      *factory,
      "POLYGON ((0.03 -0.04, 5 0.03, 10 -0.04, 10 10, 0 10, 0.03 -0.04))");
  auto expected1 = ReadWkt(
      *factory,
      "POLYGON ((0.03 -5, 10 -5, 10 -0.04, 5 0.03, 0.03 -0.04, 0.03 -5))");
  EXPECT_TRUE(snapped.first->equalsExact(expected0.get()));
  EXPECT_TRUE(snapped.second->equalsExact(expected1.get()));

  auto empty_line = ReadWkt(*factory, "LINESTRING EMPTY");
  auto line = ReadWkt(*factory, "LINESTRING (0 0, 1 1)");
  GeomPtrPair empty_pair;
  GeometrySnapper::snap(*empty_line, *line, 0.1, empty_pair);
  EXPECT_TRUE(empty_pair.first->isEmpty());
  EXPECT_TRUE(empty_pair.second->equalsExact(line.get()));
}

TEST(GeometrySnapperTest, SnapToSelfEmptyAndCollection) {
  auto factory = GeometryFactory::create();
  auto empty_poly = ReadWkt(*factory, "POLYGON EMPTY");
  // GeometryTransformer::transformPolygon treats an empty shell ring as
  // non-polygonal and falls back to buildGeometry over the empty LinearRing,
  // so cleanResult=true does not invoke buffer(0).
  auto empty_poly_uncleaned =
      GeometrySnapper::snapToSelf(*empty_poly, 0.1, false);
  EXPECT_TRUE(empty_poly_uncleaned->isEmpty());
  EXPECT_EQ(empty_poly_uncleaned->getGeometryTypeId(),
            geos::geom::GEOS_LINEARRING);

  auto empty_poly_cleaned = GeometrySnapper::snapToSelf(*empty_poly, 0.1, true);
  EXPECT_TRUE(empty_poly_cleaned->isEmpty());
  EXPECT_EQ(empty_poly_cleaned->getGeometryTypeId(),
            geos::geom::GEOS_LINEARRING);

  auto empty_mp = ReadWkt(*factory, "MULTIPOLYGON EMPTY");
  auto empty_mp_uncleaned = GeometrySnapper::snapToSelf(*empty_mp, 0.1, false);
  EXPECT_TRUE(empty_mp_uncleaned->isEmpty());
  EXPECT_EQ(empty_mp_uncleaned->getGeometryTypeId(),
            geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_TRUE(GeometrySnapper::snapToSelf(*empty_mp, 0.1, true)->isEmpty());

  auto empty_pt = ReadWkt(*factory, "POINT EMPTY");
  auto snapped_empty_pt = GeometrySnapper::snapToSelf(*empty_pt, 0.1, true);
  EXPECT_TRUE(snapped_empty_pt->isEmpty());
  EXPECT_EQ(snapped_empty_pt->getGeometryTypeId(), geos::geom::GEOS_POINT);

  auto empty_ls = ReadWkt(*factory, "LINESTRING EMPTY");
  auto snapped_empty_ls = GeometrySnapper::snapToSelf(*empty_ls, 0.1, true);
  EXPECT_TRUE(snapped_empty_ls->isEmpty());
  EXPECT_EQ(snapped_empty_ls->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);

  auto empty_mpt = ReadWkt(*factory, "MULTIPOINT EMPTY");
  auto snapped_empty_mpt = GeometrySnapper::snapToSelf(*empty_mpt, 0.1, true);
  EXPECT_TRUE(snapped_empty_mpt->isEmpty());
  EXPECT_EQ(snapped_empty_mpt->getGeometryTypeId(),
            geos::geom::GEOS_MULTIPOINT);

  auto empty_mls = ReadWkt(*factory, "MULTILINESTRING EMPTY");
  auto snapped_empty_mls = GeometrySnapper::snapToSelf(*empty_mls, 0.1, true);
  EXPECT_TRUE(snapped_empty_mls->isEmpty());
  EXPECT_EQ(snapped_empty_mls->getGeometryTypeId(),
            geos::geom::GEOS_MULTILINESTRING);

  auto gc =
      ReadWkt(*factory,
              "GEOMETRYCOLLECTION (POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
              "(2 0.05, 8 0.05, 8 8, 2 8, 2 0.05)), POINT (5 5))");
  auto snapped_gc = GeometrySnapper::snapToSelf(*gc, 0.2, true);
  EXPECT_EQ(snapped_gc->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(snapped_gc->getNumGeometries(), 2u);
  EXPECT_FALSE(snapped_gc->getGeometryN(0)->isValid());
}

TEST(GeometrySnapperTest, SnapToHandles3DZCoordinates) {
  auto factory = GeometryFactory::create();
  auto src_line = ReadWkt(*factory, "LINESTRING Z (0 0 10, 10 0 20)");
  auto target_pt = ReadWkt(*factory, "POINT Z (0.02 0 99)");

  auto snapped = GeometrySnapper(*src_line).snapTo(*target_pt, 0.1);
  auto coords = snapped->getCoordinates();
  ASSERT_EQ(coords->size(), 2u);
  EXPECT_TRUE(coords->hasZ());
  EXPECT_DOUBLE_EQ(coords->getX(0), 0.02);
  EXPECT_DOUBLE_EQ(coords->getY(0), 0.0);
  EXPECT_DOUBLE_EQ(coords->getOrdinate(0, CoordinateSequence::Z), 99.0);
  EXPECT_DOUBLE_EQ(coords->getX(1), 10.0);
  EXPECT_DOUBLE_EQ(coords->getY(1), 0.0);
  EXPECT_DOUBLE_EQ(coords->getOrdinate(1, CoordinateSequence::Z), 20.0);
}

TEST(GeometrySnapperTest, ComputeOverlaySnapToleranceFixedGridVariants) {
  PrecisionModel default_fixed_pm(PrecisionModel::FIXED);
  auto default_fixed_factory = GeometryFactory::create(&default_fixed_pm);
  auto unit_square =
      ReadWkt(*default_fixed_factory, "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  EXPECT_DOUBLE_EQ(GeometrySnapper::computeOverlaySnapTolerance(*unit_square),
                   2.0 / 1.415);

  // Negative scale specifies grid size (10), storing scale = 0.1.
  PrecisionModel grid_size_pm(-10.0);
  auto grid_size_factory = GeometryFactory::create(&grid_size_pm);
  auto grid_square =
      ReadWkt(*grid_size_factory, "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  EXPECT_DOUBLE_EQ(GeometrySnapper::computeOverlaySnapTolerance(*grid_square),
                   (1.0 / 0.1) * 2.0 / 1.415);

  PrecisionModel finer_fixed_pm(100.0);
  auto finer_factory = GeometryFactory::create(&finer_fixed_pm);
  auto finer_square =
      ReadWkt(*finer_factory, "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  EXPECT_DOUBLE_EQ(
      GeometrySnapper::computeOverlaySnapTolerance(*grid_square, *finer_square),
      (1.0 / 100.0) * 2.0 / 1.415);
}

TEST(GeometrySnapperTest,
     SnapToSameGeometryMatchesSnapToSelfAndStaticSnapReassignsPair) {
  auto factory = GeometryFactory::create();
  auto poly = ReadWkt(*factory,
                      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
                      "(2 0.05, 8 0.05, 8 8, 2 8, 2 0.05))");

  GeometrySnapper snapper(*poly);
  auto via_snap_to = snapper.snapTo(*poly, 0.2);
  auto via_snap_to_self = snapper.snapToSelf(0.2, false);
  EXPECT_TRUE(via_snap_to->equalsExact(via_snap_to_self.get()));

  GeomPtrPair snapped;
  snapped.first = ReadWkt(*factory, "POINT (99 99)");
  snapped.second = ReadWkt(*factory, "POINT (88 88)");
  auto line0 = ReadWkt(*factory, "LINESTRING (0 0, 10 0)");
  auto line1 = ReadWkt(*factory, "LINESTRING (0 0, 10 0)");
  GeometrySnapper::snap(*line0, *line1, 0.1, snapped);
  EXPECT_TRUE(snapped.first->equalsExact(line0.get()));
  EXPECT_TRUE(snapped.second->equalsExact(line1.get()));
}

TEST(GeometrySnapperTest, SnapToSelfSingleElementMultiPolygonClean) {
  auto factory = GeometryFactory::create();
  // Single-element MultiPolygon simplifies to Polygon during transformation,
  // then cleanResult=true repairs the resulting shell/hole edge touch.
  auto single_mp = ReadWkt(*factory,
                           "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0), "
                           "(2 0.05, 8 0.05, 8 8, 2 8, 2 0.05)))");

  auto uncleaned = GeometrySnapper::snapToSelf(*single_mp, 0.2, false);
  EXPECT_EQ(uncleaned->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_FALSE(uncleaned->isValid());

  auto cleaned = GeometrySnapper::snapToSelf(*single_mp, 0.2, true);
  EXPECT_EQ(cleaned->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(cleaned->isValid());
}

TEST(GeometrySnapperTest, SnapToAndSnapToSelfUnsupportedCurvedGeometryThrows) {
  auto factory = GeometryFactory::create();
  auto arc = ReadWkt(*factory, "CIRCULARSTRING (0 0, 1 1, 2 0)");
  auto target = ReadWkt(*factory, "POINT (0 0)");

  GeometrySnapper snapper(*arc);
  EXPECT_THROW(snapper.snapTo(*target, 0.1), IllegalArgumentException);
  EXPECT_THROW(snapper.snapToSelf(0.1, false), IllegalArgumentException);
  EXPECT_THROW(GeometrySnapper::snapToSelf(*arc, 0.1, true),
               IllegalArgumentException);
}

}  // namespace
}  // namespace geos_google
