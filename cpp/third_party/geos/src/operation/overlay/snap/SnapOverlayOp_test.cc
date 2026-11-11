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

#include "third_party/geos/include/geos/operation/overlay/snap/SnapOverlayOp.h"

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayNG.h"
#include "third_party/geos/include/geos/util/GEOSException.h"

namespace geos_google {
namespace {

using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::operation::overlay::snap::SnapOverlayOp;
using geos::operation::overlayng::OverlayNG;

std::unique_ptr<Geometry> ReadWkt(const GeometryFactory& factory,
                                  const std::string& wkt) {
  WKTReader reader(factory);
  return reader.read(wkt);
}

TEST(SnapOverlayOpTest, StaticOperationsOnOverlappingPolygons) {
  auto factory = GeometryFactory::create();
  auto poly0 = ReadWkt(*factory, "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto poly1 = ReadWkt(*factory, "POLYGON ((5 0, 15 0, 15 10, 5 10, 5 0))");

  auto inter = SnapOverlayOp::intersection(*poly0, *poly1);
  auto expected_inter =
      ReadWkt(*factory, "POLYGON ((5 0, 10 0, 10 10, 5 10, 5 0))");
  EXPECT_TRUE(inter->isValid());
  EXPECT_TRUE(inter->equals(expected_inter.get()));
  EXPECT_DOUBLE_EQ(inter->getArea(), 50.0);

  auto uni = SnapOverlayOp::Union(*poly0, *poly1);
  auto expected_uni =
      ReadWkt(*factory, "POLYGON ((0 0, 15 0, 15 10, 0 10, 0 0))");
  EXPECT_TRUE(uni->isValid());
  EXPECT_TRUE(uni->equals(expected_uni.get()));
  EXPECT_DOUBLE_EQ(uni->getArea(), 150.0);

  auto diff = SnapOverlayOp::difference(*poly0, *poly1);
  auto expected_diff =
      ReadWkt(*factory, "POLYGON ((0 0, 5 0, 5 10, 0 10, 0 0))");
  EXPECT_TRUE(diff->isValid());
  EXPECT_TRUE(diff->equals(expected_diff.get()));
  EXPECT_DOUBLE_EQ(diff->getArea(), 50.0);

  auto sym_diff = SnapOverlayOp::symDifference(*poly0, *poly1);
  auto expected_sym_diff =
      ReadWkt(*factory,
              "MULTIPOLYGON (((0 0, 5 0, 5 10, 0 10, 0 0)), "
              "((10 0, 15 0, 15 10, 10 10, 10 0)))");
  EXPECT_TRUE(sym_diff->isValid());
  EXPECT_TRUE(sym_diff->equals(expected_sym_diff.get()));
  EXPECT_DOUBLE_EQ(sym_diff->getArea(), 100.0);
}

TEST(SnapOverlayOpTest, OverlayOpAndReusableInstanceGetResultGeometry) {
  auto factory = GeometryFactory::create();
  auto poly0 = ReadWkt(*factory, "POLYGON ((0 0, 6 0, 6 6, 0 6, 0 0))");
  auto poly1 = ReadWkt(*factory, "POLYGON ((4 0, 10 0, 10 6, 4 6, 4 0))");

  auto static_inter =
      SnapOverlayOp::overlayOp(*poly0, *poly1, +OverlayNG::INTERSECTION);
  EXPECT_DOUBLE_EQ(static_inter->getArea(), 12.0);

  SnapOverlayOp op(*poly0, *poly1);
  auto inst_inter = op.getResultGeometry(+OverlayNG::INTERSECTION);
  auto inst_union = op.getResultGeometry(+OverlayNG::UNION);
  auto inst_diff = op.getResultGeometry(+OverlayNG::DIFFERENCE);
  auto inst_sym = op.getResultGeometry(+OverlayNG::SYMDIFFERENCE);

  auto expected_union =
      ReadWkt(*factory, "POLYGON ((0 0, 10 0, 10 6, 0 6, 0 0))");
  auto expected_diff = ReadWkt(*factory, "POLYGON ((0 0, 4 0, 4 6, 0 6, 0 0))");
  auto expected_sym = ReadWkt(*factory,
                              "MULTIPOLYGON (((0 0, 4 0, 4 6, 0 6, 0 0)), "
                              "((6 0, 10 0, 10 6, 6 6, 6 0)))");

  EXPECT_TRUE(inst_inter->equals(static_inter.get()));
  EXPECT_TRUE(inst_union->equals(expected_union.get()));
  EXPECT_DOUBLE_EQ(inst_union->getArea(), 60.0);
  EXPECT_TRUE(inst_diff->equals(expected_diff.get()));
  EXPECT_DOUBLE_EQ(inst_diff->getArea(), 24.0);
  EXPECT_TRUE(inst_sym->equals(expected_sym.get()));
  EXPECT_DOUBLE_EQ(inst_sym->getArea(), 48.0);

  EXPECT_TRUE(poly0->equalsExact(
      ReadWkt(*factory, "POLYGON ((0 0, 6 0, 6 6, 0 6, 0 0))").get()));
  EXPECT_TRUE(poly1->equalsExact(
      ReadWkt(*factory, "POLYGON ((4 0, 10 0, 10 6, 4 6, 4 0))").get()));
}

TEST(SnapOverlayOpTest, SnapsNearlyCoincidentEdgesWithoutSliver) {
  auto factory = GeometryFactory::create();
  // Offset 1e-10 is smaller than computeOverlaySnapTolerance (~1e-8 for size
  // 10), so snapping aligns the shared edge cleanly.
  auto poly0 = ReadWkt(*factory, "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto poly1 = ReadWkt(*factory,
                       "POLYGON ((10.0000000001 0, 20 0, 20 10, 10.0000000001 "
                       "10, 10.0000000001 0))");

  auto uni = SnapOverlayOp::Union(*poly0, *poly1);
  EXPECT_TRUE(uni->isValid());
  EXPECT_EQ(uni->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_NEAR(uni->getArea(), 200.0, 1e-9);

  auto line0 = ReadWkt(*factory, "LINESTRING (0 0, 10 0, 10 10)");
  auto line1 = ReadWkt(*factory, "LINESTRING (10 0.0000000001, 20 0, 20 10)");
  auto line_inter = SnapOverlayOp::intersection(*line0, *line1);
  auto expected_pt = ReadWkt(*factory, "POINT (10 0.0000000001)");
  EXPECT_FALSE(line_inter->isEmpty());
  EXPECT_EQ(line_inter->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(line_inter->equalsExact(expected_pt.get(), 1e-12));

  // Diagonal segment cracks onto a nearly touching endpoint within tolerance.
  auto diag0 = ReadWkt(*factory, "LINESTRING (0 0, 10 10)");
  auto diag1 = ReadWkt(*factory, "LINESTRING (5 5.0000000001, 0 10)");
  auto diag_inter = SnapOverlayOp::intersection(*diag0, *diag1);
  auto expected_diag_pt = ReadWkt(*factory, "POINT (5 5.0000000001)");
  EXPECT_EQ(diag_inter->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(diag_inter->equalsExact(expected_diag_pt.get(), 1e-12));
}

TEST(SnapOverlayOpTest, LargeCoordinateOffsetPreservedByCommonBitsRemover) {
  auto factory = GeometryFactory::create();
  // Shared offset 1e8 exercises CommonBitsRemover + prepareResult restoration.
  auto poly0 = ReadWkt(
      *factory,
      "POLYGON ((100000000 100000000, 100000010 100000000, "
      "100000010 100000010, 100000000 100000010, 100000000 100000000))");
  auto poly1 = ReadWkt(
      *factory,
      "POLYGON ((100000005 100000000, 100000015 100000000, "
      "100000015 100000010, 100000005 100000010, 100000005 100000000))");

  auto inter = SnapOverlayOp::intersection(*poly0, *poly1);
  auto expected_inter = ReadWkt(
      *factory,
      "POLYGON ((100000005 100000000, 100000010 100000000, "
      "100000010 100000010, 100000005 100000010, 100000005 100000000))");
  EXPECT_TRUE(inter->isValid());
  EXPECT_TRUE(inter->equals(expected_inter.get()));
  EXPECT_NEAR(inter->getArea(), 50.0, 1e-6);

  auto uni = SnapOverlayOp::Union(*poly0, *poly1);
  auto expected_uni = ReadWkt(
      *factory,
      "POLYGON ((100000000 100000000, 100000015 100000000, "
      "100000015 100000010, 100000000 100000010, 100000000 100000000))");
  EXPECT_TRUE(uni->isValid());
  EXPECT_TRUE(uni->equals(expected_uni.get()));
  EXPECT_NEAR(uni->getArea(), 150.0, 1e-6);

  auto diff = SnapOverlayOp::difference(*poly0, *poly1);
  auto expected_diff = ReadWkt(
      *factory,
      "POLYGON ((100000000 100000000, 100000005 100000000, "
      "100000005 100000010, 100000000 100000010, 100000000 100000000))");
  EXPECT_TRUE(diff->isValid());
  EXPECT_TRUE(diff->equals(expected_diff.get()));
  EXPECT_NEAR(diff->getArea(), 50.0, 1e-6);

  auto sym = SnapOverlayOp::symDifference(*poly0, *poly1);
  EXPECT_TRUE(sym->isValid());
  EXPECT_NEAR(sym->getArea(), 100.0, 1e-6);

  // CommonBitsRemover preserves tiny relative gap precision at 1e6 offset.
  auto near0 = ReadWkt(*factory,
                       "POLYGON ((1000000 1000000, 1000010 1000000, "
                       "1000010 1000010, 1000000 1000010, 1000000 1000000))");
  auto near1 = ReadWkt(*factory,
                       "POLYGON ((1000010.000000001 1000000, 1000020 1000000, "
                       "1000020 1000010, 1000010.000000001 1000010, "
                       "1000010.000000001 1000000))");
  auto near_uni = SnapOverlayOp::Union(*near0, *near1);
  EXPECT_TRUE(near_uni->isValid());
  EXPECT_EQ(near_uni->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_NEAR(near_uni->getArea(), 200.0, 1e-7);
}

TEST(SnapOverlayOpTest, FixedPrecisionModelOverlay) {
  PrecisionModel fixed_pm(100.0);
  auto factory = GeometryFactory::create(&fixed_pm);
  auto poly0 = ReadWkt(*factory, "POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0))");
  auto poly1 = ReadWkt(*factory, "POLYGON ((2 2, 7 2, 7 7, 2 7, 2 2))");

  auto inter = SnapOverlayOp::intersection(*poly0, *poly1);
  auto expected = ReadWkt(*factory, "POLYGON ((2 2, 5 2, 5 5, 2 5, 2 2))");
  EXPECT_TRUE(inter->isValid());
  EXPECT_TRUE(inter->equals(expected.get()));

  // Scale 10.0 gives fixed snap tolerance ~0.1413, bridging a 0.1 grid gap.
  PrecisionModel coarse_pm(10.0);
  auto coarse_factory = GeometryFactory::create(&coarse_pm);
  auto left =
      ReadWkt(*coarse_factory, "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto right = ReadWkt(*coarse_factory,
                       "POLYGON ((10.1 0, 20 0, 20 10, 10.1 10, 10.1 0))");
  auto bridged = SnapOverlayOp::Union(*left, *right);
  auto expected_bridged =
      ReadWkt(*coarse_factory, "POLYGON ((0 0, 20 0, 20 10, 0 10, 0 0))");
  EXPECT_TRUE(bridged->isValid());
  EXPECT_EQ(bridged->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(bridged->getPrecisionModel()->getScale(), 10.0);
  EXPECT_TRUE(bridged->equals(expected_bridged.get()));
  EXPECT_DOUBLE_EQ(bridged->getArea(), 200.0);
}

TEST(SnapOverlayOpTest, PolygonWithHoleAndMultiPointOverlays) {
  auto factory = GeometryFactory::create();
  auto holed = ReadWkt(
      *factory,
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 8 2, 8 8, 2 8, 2 2))");
  auto overlap = ReadWkt(*factory, "POLYGON ((4 4, 12 4, 12 12, 4 12, 4 4))");

  auto inter = SnapOverlayOp::intersection(*holed, *overlap);
  auto uni = SnapOverlayOp::Union(*holed, *overlap);
  auto diff = SnapOverlayOp::difference(*holed, *overlap);
  auto sym = SnapOverlayOp::symDifference(*holed, *overlap);

  EXPECT_TRUE(inter->isValid());
  EXPECT_DOUBLE_EQ(inter->getArea(), 20.0);
  EXPECT_TRUE(uni->isValid());
  EXPECT_DOUBLE_EQ(uni->getArea(), 108.0);
  EXPECT_TRUE(diff->isValid());
  EXPECT_DOUBLE_EQ(diff->getArea(), 44.0);
  EXPECT_TRUE(sym->isValid());
  EXPECT_DOUBLE_EQ(sym->getArea(), 88.0);

  auto mp0 = ReadWkt(*factory, "MULTIPOINT ((0 0), (5 5), (10 10))");
  auto mp1 =
      ReadWkt(*factory, "MULTIPOINT ((5 5), (10 10.0000000001), (20 20))");
  auto mp_inter = SnapOverlayOp::intersection(*mp0, *mp1);
  auto expected_mp_inter =
      ReadWkt(*factory, "MULTIPOINT ((5 5), (10 10.0000000001))");
  EXPECT_EQ(mp_inter->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(mp_inter->getNumGeometries(), 2U);
  EXPECT_TRUE(mp_inter->equalsExact(expected_mp_inter.get(), 1e-12));

  auto mp_uni = SnapOverlayOp::Union(*mp0, *mp1);
  EXPECT_EQ(mp_uni->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(mp_uni->getNumGeometries(), 4U);
}

TEST(SnapOverlayOpTest, DisjointEmptyMixedInputsAndInvalidInputThrows) {
  auto factory = GeometryFactory::create();
  auto poly0 = ReadWkt(*factory, "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  auto poly1 = ReadWkt(*factory, "POLYGON ((10 0, 12 0, 12 2, 10 2, 10 0))");
  auto empty_poly = ReadWkt(*factory, "POLYGON EMPTY");

  EXPECT_TRUE(SnapOverlayOp::intersection(*poly0, *poly1)->isEmpty());
  EXPECT_DOUBLE_EQ(SnapOverlayOp::Union(*poly0, *poly1)->getArea(), 8.0);
  EXPECT_TRUE(SnapOverlayOp::difference(*poly0, *poly1)->equals(poly0.get()));

  EXPECT_TRUE(SnapOverlayOp::intersection(*poly0, *empty_poly)->isEmpty());
  EXPECT_TRUE(SnapOverlayOp::Union(*poly0, *empty_poly)->equals(poly0.get()));
  EXPECT_TRUE(SnapOverlayOp::difference(*empty_poly, *poly0)->isEmpty());
  EXPECT_TRUE(SnapOverlayOp::Union(*empty_poly, *empty_poly)->isEmpty());

  auto pt = ReadWkt(*factory, "POINT (1 1)");
  EXPECT_TRUE(SnapOverlayOp::intersection(*pt, *poly0)->equals(pt.get()));
  EXPECT_TRUE(SnapOverlayOp::difference(*pt, *poly0)->isEmpty());

  // Mixed-dimension GeometryCollection is rejected by OverlayNG.
  auto mixed = ReadWkt(
      *factory, "GEOMETRYCOLLECTION (POINT (1 1), LINESTRING (0 0, 2 2))");
  EXPECT_THROW(SnapOverlayOp::intersection(*mixed, *poly0),
               geos::util::GEOSException);
}

}  // namespace
}  // namespace geos_google
