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

#include "third_party/geos/include/geos/geom/util/Densifier.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/GEOSException.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::geom::Geometry;
using ::geos::geom::GeometryCollection;
using ::geos::geom::GeometryFactory;
using ::geos::geom::LinearRing;
using ::geos::geom::LineString;
using ::geos::geom::MultiLineString;
using ::geos::geom::MultiPolygon;
using ::geos::geom::Polygon;
using ::geos::geom::PrecisionModel;
using ::geos::geom::util::Densifier;
using ::geos::io::WKTReader;

std::unique_ptr<Geometry> ReadWkt(const GeometryFactory* factory,
                                  const std::string& wkt) {
  WKTReader reader(factory);
  return reader.read(wkt);
}

void ExpectMaxSegmentLengthAtMost(const CoordinateSequence& seq,
                                  double max_len) {
  ASSERT_GE(seq.size(), 2u);
  for (std::size_t i = 1; i < seq.size(); ++i) {
    const double seg_len =
        seq.getAt<CoordinateXY>(i - 1).distance(seq.getAt<CoordinateXY>(i));
    EXPECT_LE(seg_len, max_len + 1e-12);
  }
}

TEST(DensifierTest, EmptyGeometriesAreClonedWithFactoryAndSrid) {
  PrecisionModel pm;
  auto factory = GeometryFactory::create(&pm, 32610);

  const std::vector<std::string> empty_wkts = {
      "POINT EMPTY",        "LINESTRING EMPTY",         "LINEARRING EMPTY",
      "POLYGON EMPTY",      "MULTIPOINT EMPTY",         "MULTILINESTRING EMPTY",
      "MULTIPOLYGON EMPTY", "GEOMETRYCOLLECTION EMPTY",
  };

  for (const std::string& wkt : empty_wkts) {
    auto input = ReadWkt(factory.get(), wkt);
    ASSERT_TRUE(input->isEmpty()) << wkt;
    auto result = Densifier::densify(input.get(), 1.0);
    ASSERT_NE(result, nullptr) << wkt;
    EXPECT_TRUE(result->isEmpty()) << wkt;
    EXPECT_EQ(result->getGeometryTypeId(), input->getGeometryTypeId()) << wkt;
    EXPECT_EQ(result->getFactory(), factory.get()) << wkt;
    EXPECT_EQ(result->getSRID(), 32610) << wkt;
  }
}

TEST(DensifierTest, NonPositiveOrNanToleranceThrowsIllegalArgument) {
  auto factory = GeometryFactory::create();
  auto line = ReadWkt(factory.get(), "LINESTRING (0 0, 10 0)");

  const double bad_tolerances[] = {
      0.0,
      -0.0,
      -2.5,
      -std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN(),
  };

  for (double tol : bad_tolerances) {
    EXPECT_THROW(Densifier::densify(line.get(), tol),
                 geos::util::IllegalArgumentException);
    Densifier densifier(line.get());
    EXPECT_THROW(densifier.setDistanceTolerance(tol),
                 geos::util::IllegalArgumentException);
  }
}

TEST(DensifierTest, ToleranceTooSmallForLengthThrowsGeosException) {
  auto factory = GeometryFactory::create();
  auto huge_line = ReadWkt(factory.get(), "LINESTRING (0 0, 3000000000 0)");
  EXPECT_THROW(Densifier::densify(huge_line.get(), 1.0),
               geos::util::GEOSException);

  auto short_line = ReadWkt(factory.get(), "LINESTRING (0 0, 10 0)");
  EXPECT_THROW(Densifier::densify(short_line.get(), 1e-12),
               geos::util::GEOSException);
}

TEST(DensifierTest, PointAndMultiPointRemainUnchanged) {
  auto factory = GeometryFactory::create();

  auto pt = ReadWkt(factory.get(), "POINT (3.5 -2.25)");
  auto pt_result = Densifier::densify(pt.get(), 0.5);
  EXPECT_EQ(pt_result->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(pt_result->equalsExact(pt.get(), 0.0));

  auto mpt = ReadWkt(factory.get(), "MULTIPOINT ((0 0), (10 0), (10 10))");
  auto mpt_result = Densifier::densify(mpt.get(), 0.5);
  EXPECT_EQ(mpt_result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_TRUE(mpt_result->equalsExact(mpt.get(), 0.0));
}

TEST(DensifierTest, LineStringWithinToleranceIsUnchanged) {
  auto factory = GeometryFactory::create();
  auto line = ReadWkt(factory.get(), "LINESTRING (0 0, 3 4)");  // Length 5.0

  for (double tol : {5.0, 10.0, std::numeric_limits<double>::infinity()}) {
    auto result = Densifier::densify(line.get(), tol);
    EXPECT_EQ(result->getNumPoints(), 2u);
    EXPECT_TRUE(result->equalsExact(line.get(), 0.0));
  }
}

TEST(DensifierTest, LineStringSubdividesEvenlyAndDeduplicatesRepeatedPoints) {
  auto factory = GeometryFactory::create();

  auto line = ReadWkt(factory.get(), "LINESTRING (0 0, 10 0)");
  // Exact division: 10 / 2.5 = 4 segments.
  auto exact_result = Densifier::densify(line.get(), 2.5);
  const auto* exact_ls = dynamic_cast<const LineString*>(exact_result.get());
  ASSERT_NE(exact_ls, nullptr);
  ASSERT_EQ(exact_ls->getNumPoints(), 5u);
  EXPECT_EQ(exact_ls->getCoordinateN(0), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(exact_ls->getCoordinateN(1), CoordinateXY(2.5, 0.0));
  EXPECT_EQ(exact_ls->getCoordinateN(2), CoordinateXY(5.0, 0.0));
  EXPECT_EQ(exact_ls->getCoordinateN(3), CoordinateXY(7.5, 0.0));
  EXPECT_EQ(exact_ls->getCoordinateN(4), CoordinateXY(10.0, 0.0));

  // Non-exact division: ceil(10 / 3.0) = 4 segments of equal length 2.5.
  auto uneven_result = Densifier::densify(line.get(), 3.0);
  EXPECT_TRUE(uneven_result->equalsExact(exact_result.get(), 1e-12));

  // Mixed segments including a zero-length repeated segment.
  auto mixed = ReadWkt(factory.get(), "LINESTRING (0 0, 0 0, 1 0, 11 0)");
  auto mixed_result = Densifier::densify(mixed.get(), 3.0);
  const auto* mixed_ls = dynamic_cast<const LineString*>(mixed_result.get());
  ASSERT_NE(mixed_ls, nullptr);
  ASSERT_EQ(mixed_ls->getNumPoints(), 6u);
  EXPECT_EQ(mixed_ls->getCoordinateN(0), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(mixed_ls->getCoordinateN(1), CoordinateXY(1.0, 0.0));
  EXPECT_EQ(mixed_ls->getCoordinateN(2), CoordinateXY(3.5, 0.0));
  EXPECT_EQ(mixed_ls->getCoordinateN(3), CoordinateXY(6.0, 0.0));
  EXPECT_EQ(mixed_ls->getCoordinateN(4), CoordinateXY(8.5, 0.0));
  EXPECT_EQ(mixed_ls->getCoordinateN(5), CoordinateXY(11.0, 0.0));
}

TEST(DensifierTest, InterpolatesZAndPreservesMissingZAsNaN) {
  auto factory = GeometryFactory::create();

  auto line3d = ReadWkt(factory.get(), "LINESTRING Z (0 0 10, 10 0 20)");
  auto res3d = Densifier::densify(line3d.get(), 5.0);
  const auto* ls3d = dynamic_cast<const LineString*>(res3d.get());
  ASSERT_NE(ls3d, nullptr);
  EXPECT_TRUE(ls3d->hasZ());
  ASSERT_EQ(ls3d->getNumPoints(), 3u);
  EXPECT_DOUBLE_EQ(ls3d->getCoordinateN(0).z, 10.0);
  EXPECT_DOUBLE_EQ(ls3d->getCoordinateN(1).z, 15.0);
  EXPECT_DOUBLE_EQ(ls3d->getCoordinateN(2).z, 20.0);

  auto line2d = ReadWkt(factory.get(), "LINESTRING (0 0, 10 0)");
  auto res2d = Densifier::densify(line2d.get(), 5.0);
  const auto* ls2d = dynamic_cast<const LineString*>(res2d.get());
  ASSERT_NE(ls2d, nullptr);
  EXPECT_FALSE(ls2d->hasZ());
  ASSERT_EQ(ls2d->getNumPoints(), 3u);
  EXPECT_TRUE(std::isnan(ls2d->getCoordinateN(1).z));
}

TEST(DensifierTest, LinearRingAndMultiLineStringDensification) {
  auto factory = GeometryFactory::create();

  auto ring = ReadWkt(factory.get(), "LINEARRING (0 0, 4 0, 4 4, 0 4, 0 0)");
  auto ring_res = Densifier::densify(ring.get(), 2.0);
  const auto* densified_ring = dynamic_cast<const LinearRing*>(ring_res.get());
  ASSERT_NE(densified_ring, nullptr);
  EXPECT_TRUE(densified_ring->isClosed());
  EXPECT_EQ(densified_ring->getNumPoints(), 9u);
  ExpectMaxSegmentLengthAtMost(*densified_ring->getCoordinatesRO(), 2.0);

  auto mls = ReadWkt(factory.get(), "MULTILINESTRING ((0 0, 9 0), (1 1, 2 1))");
  auto mls_res = Densifier::densify(mls.get(), 3.0);
  const auto* densified_mls =
      dynamic_cast<const MultiLineString*>(mls_res.get());
  ASSERT_NE(densified_mls, nullptr);
  ASSERT_EQ(densified_mls->getNumGeometries(), 2u);
  EXPECT_EQ(densified_mls->getGeometryN(0)->getNumPoints(), 4u);
  EXPECT_EQ(densified_mls->getGeometryN(1)->getNumPoints(), 2u);
  ExpectMaxSegmentLengthAtMost(
      *densified_mls->getGeometryN(0)->getCoordinatesRO(), 3.0);
}

TEST(DensifierTest, PolygonWithHolePreservesValidityAndArea) {
  auto factory = GeometryFactory::create();
  auto poly = ReadWkt(
      factory.get(),
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 8, 8 8, 8 2, 2 2))");

  auto result = Densifier::densify(poly.get(), 3.0);
  const auto* densified_poly = dynamic_cast<const Polygon*>(result.get());
  ASSERT_NE(densified_poly, nullptr);
  EXPECT_TRUE(densified_poly->isValid());
  ASSERT_EQ(densified_poly->getNumInteriorRing(), 1u);
  EXPECT_NEAR(densified_poly->getArea(), 64.0, 1e-12);
  EXPECT_GT(densified_poly->getExteriorRing()->getNumPoints(), 5u);
  EXPECT_GT(densified_poly->getInteriorRingN(0)->getNumPoints(), 5u);
  ExpectMaxSegmentLengthAtMost(
      *densified_poly->getExteriorRing()->getCoordinatesRO(), 3.0);
  ExpectMaxSegmentLengthAtMost(
      *densified_poly->getInteriorRingN(0)->getCoordinatesRO(), 3.0);
}

TEST(DensifierTest, MultiPolygonDensifiedValid) {
  auto factory = GeometryFactory::create();
  auto mp = ReadWkt(factory.get(),
                    "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
                    "((10 0, 14 0, 14 4, 10 4, 10 0)))");

  auto result = Densifier::densify(mp.get(), 1.5);
  const auto* densified_mp = dynamic_cast<const MultiPolygon*>(result.get());
  ASSERT_NE(densified_mp, nullptr);
  EXPECT_TRUE(densified_mp->isValid());
  ASSERT_EQ(densified_mp->getNumGeometries(), 2u);
  EXPECT_NEAR(densified_mp->getArea(), 32.0, 1e-12);
  for (std::size_t i = 0; i < 2; ++i) {
    const Polygon* part = densified_mp->getGeometryN(i);
    EXPECT_EQ(part->getExteriorRing()->getNumPoints(), 13u);
    ExpectMaxSegmentLengthAtMost(*part->getExteriorRing()->getCoordinatesRO(),
                                 1.5);
  }
}

TEST(DensifierTest, GeometryCollectionDensifiesAllComponentTypes) {
  auto factory = GeometryFactory::create();
  auto gc = ReadWkt(factory.get(),
                    "GEOMETRYCOLLECTION (POINT (1 2), LINESTRING (0 0, 6 0), "
                    "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0)), "
                    "MULTIPOLYGON (((10 10, 14 10, 14 14, 10 14, 10 10)), "
                    "((20 10, 24 10, 24 14, 20 14, 20 10))))");

  auto result = Densifier::densify(gc.get(), 2.0);
  const auto* densified_gc =
      dynamic_cast<const GeometryCollection*>(result.get());
  ASSERT_NE(densified_gc, nullptr);
  ASSERT_EQ(densified_gc->getNumGeometries(), 4u);
  EXPECT_EQ(densified_gc->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_POINT);
  EXPECT_EQ(densified_gc->getGeometryN(1)->getNumPoints(), 4u);
  EXPECT_EQ(densified_gc->getGeometryN(2)->getGeometryTypeId(),
            geos::geom::GEOS_POLYGON);
  EXPECT_NEAR(densified_gc->getGeometryN(2)->getArea(), 16.0, 1e-12);
  EXPECT_EQ(densified_gc->getGeometryN(3)->getGeometryTypeId(),
            geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(densified_gc->getGeometryN(3)->getNumGeometries(), 2u);
  EXPECT_NEAR(densified_gc->getGeometryN(3)->getArea(), 32.0, 1e-12);
}

TEST(DensifierTest, SinglePolygonMultiPolygonSimplifiesToPolygon) {
  auto factory = GeometryFactory::create();
  auto mp = ReadWkt(factory.get(),
                    "MULTIPOLYGON (((10 10, 14 10, 14 14, 10 14, 10 10)))");

  // GeometryTransformer::transformMultiPolygon delegates to buildGeometry,
  // which unwraps single-element collections into a Polygon.
  auto result = Densifier::densify(mp.get(), 2.0);
  const auto* poly = dynamic_cast<const Polygon*>(result.get());
  ASSERT_NE(poly, nullptr);
  EXPECT_EQ(poly->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(poly->isValid());
  EXPECT_NEAR(poly->getArea(), 16.0, 1e-12);
  EXPECT_EQ(poly->getExteriorRing()->getNumPoints(), 9u);
}

TEST(DensifierTest, NestedGeometryCollectionWithMultiPointAndMultiLineString) {
  auto factory = GeometryFactory::create();
  auto gc = ReadWkt(factory.get(),
                    "GEOMETRYCOLLECTION (MULTIPOINT ((0 0), (5 0)), "
                    "MULTILINESTRING ((0 0, 4 0), (1 1, 1 5)), "
                    "GEOMETRYCOLLECTION (LINESTRING (0 0, 0 6)))");

  auto result = Densifier::densify(gc.get(), 2.0);
  const auto* outer = dynamic_cast<const GeometryCollection*>(result.get());
  ASSERT_NE(outer, nullptr);
  ASSERT_EQ(outer->getNumGeometries(), 3u);

  EXPECT_EQ(outer->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(outer->getGeometryN(0)->getNumPoints(), 2u);

  EXPECT_EQ(outer->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(outer->getGeometryN(1)->getNumPoints(), 6u);

  const auto* inner =
      dynamic_cast<const GeometryCollection*>(outer->getGeometryN(2));
  ASSERT_NE(inner, nullptr);
  ASSERT_EQ(inner->getNumGeometries(), 1u);
  EXPECT_EQ(inner->getGeometryN(0)->getGeometryTypeId(),
            geos::geom::GEOS_LINESTRING);
  EXPECT_EQ(inner->getGeometryN(0)->getNumPoints(), 4u);
}

TEST(DensifierTest, FixedPrecisionModelRoundsAndDeduplicatesVertices) {
  PrecisionModel pm(1.0);
  auto factory = GeometryFactory::create(&pm);

  auto line1 = ReadWkt(factory.get(), "LINESTRING (0 0, 5 0)");
  auto res1 = Densifier::densify(line1.get(), 2.0);
  const auto* ls1 = dynamic_cast<const LineString*>(res1.get());
  ASSERT_NE(ls1, nullptr);
  ASSERT_EQ(ls1->getNumPoints(), 4u);
  EXPECT_EQ(ls1->getCoordinateN(0), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(ls1->getCoordinateN(1), CoordinateXY(2.0, 0.0));
  EXPECT_EQ(ls1->getCoordinateN(2), CoordinateXY(3.0, 0.0));
  EXPECT_EQ(ls1->getCoordinateN(3), CoordinateXY(5.0, 0.0));

  // Tolerance 0.6 divides length 2 into 4 steps (0.5, 1.0, 1.5, 2.0), which
  // snap to (1, 1, 2, 2) and are deduplicated by allowRepeated=false.
  auto line2 = ReadWkt(factory.get(), "LINESTRING (0 0, 2 0)");
  auto res2 = Densifier::densify(line2.get(), 0.6);
  const auto* ls2 = dynamic_cast<const LineString*>(res2.get());
  ASSERT_NE(ls2, nullptr);
  ASSERT_EQ(ls2->getNumPoints(), 3u);
  EXPECT_EQ(ls2->getCoordinateN(0), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(ls2->getCoordinateN(1), CoordinateXY(1.0, 0.0));
  EXPECT_EQ(ls2->getCoordinateN(2), CoordinateXY(2.0, 0.0));
}

TEST(DensifierTest, FixedPrecisionSelfTouchRepairedByCreateValidArea) {
  PrecisionModel pm(1.0);
  auto factory = GeometryFactory::create(&pm);

  // Segment (0,0)->(4,3) has length 5 and midpoint (2, 1.5), which snaps to
  // (2, 2) on the integer grid and pinches the exterior ring at vertex (2, 2).
  auto poly =
      ReadWkt(factory.get(), "POLYGON ((0 0, 4 3, 4 4, 2 2, 0 2, 0 0))");
  ASSERT_TRUE(poly->isValid());

  auto poly_res = Densifier::densify(poly.get(), 3.0);
  ASSERT_NE(poly_res, nullptr);
  EXPECT_TRUE(poly_res->isValid());
  EXPECT_FALSE(poly_res->isEmpty());
  EXPECT_NEAR(poly_res->getArea(), 3.0, 1e-9);

  // Same pinched geometry inside a MultiPolygon exercises
  // transformMultiPolygon -> createValidArea -> buffer(0.0).
  auto mp = ReadWkt(factory.get(),
                    "MULTIPOLYGON (((0 0, 4 3, 4 4, 2 2, 0 2, 0 0)), "
                    "((10 0, 14 0, 14 4, 10 4, 10 0)))");
  ASSERT_TRUE(mp->isValid());

  auto mp_res = Densifier::densify(mp.get(), 3.0);
  ASSERT_NE(mp_res, nullptr);
  EXPECT_TRUE(mp_res->isValid());
  EXPECT_FALSE(mp_res->isEmpty());
  EXPECT_NEAR(mp_res->getArea(), 19.0, 1e-9);
}

TEST(DensifierTest, InstanceApiSupportsUpdatingDistanceTolerance) {
  auto factory = GeometryFactory::create();
  auto line = ReadWkt(factory.get(), "LINESTRING (0 0, 10 0)");

  Densifier densifier(line.get());
  densifier.setDistanceTolerance(5.0);
  auto coarse = densifier.getResultGeometry();
  EXPECT_EQ(coarse->getNumPoints(), 3u);

  densifier.setDistanceTolerance(1.0);
  auto fine = densifier.getResultGeometry();
  EXPECT_EQ(fine->getNumPoints(), 11u);
}

TEST(DensifierTest, NegativeScaleGridSizePrecisionModelAndDiagonalZ) {
  PrecisionModel grid_pm(-2.0);
  auto grid_factory = GeometryFactory::create(&grid_pm);
  auto line = ReadWkt(grid_factory.get(), "LINESTRING (0 0, 10 0)");
  auto grid_res = Densifier::densify(line.get(), 3.0);
  const auto* grid_ls = dynamic_cast<const LineString*>(grid_res.get());
  ASSERT_NE(grid_ls, nullptr);
  ASSERT_EQ(grid_ls->getNumPoints(), 5u);
  EXPECT_EQ(grid_ls->getCoordinateN(0), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(grid_ls->getCoordinateN(1), CoordinateXY(2.0, 0.0));
  EXPECT_EQ(grid_ls->getCoordinateN(2), CoordinateXY(6.0, 0.0));
  EXPECT_EQ(grid_ls->getCoordinateN(3), CoordinateXY(8.0, 0.0));
  EXPECT_EQ(grid_ls->getCoordinateN(4), CoordinateXY(10.0, 0.0));

  auto float_factory = GeometryFactory::create();
  auto diag3d = ReadWkt(float_factory.get(), "LINESTRING Z (0 0 0, 3 4 10)");
  auto diag_res = Densifier::densify(diag3d.get(), 2.0);
  const auto* diag_ls = dynamic_cast<const LineString*>(diag_res.get());
  ASSERT_NE(diag_ls, nullptr);
  ASSERT_EQ(diag_ls->getNumPoints(), 4u);
  EXPECT_NEAR(diag_ls->getCoordinateN(1).x, 1.0, 1e-12);
  EXPECT_NEAR(diag_ls->getCoordinateN(1).y, 4.0 / 3.0, 1e-12);
  EXPECT_NEAR(diag_ls->getCoordinateN(1).z, 10.0 / 3.0, 1e-12);
  EXPECT_NEAR(diag_ls->getCoordinateN(2).x, 2.0, 1e-12);
  EXPECT_NEAR(diag_ls->getCoordinateN(2).y, 8.0 / 3.0, 1e-12);
  EXPECT_NEAR(diag_ls->getCoordinateN(2).z, 20.0 / 3.0, 1e-12);
}

}  // namespace
}  // namespace geos_google
