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

// Unit tests for third_party/geos/src/operation/overlayng/OverlayPoints.cpp.

#include "third_party/geos/include/geos/operation/overlayng/OverlayPoints.h"

#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayNG.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GeometryTypeId;
using geos::geom::Point;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::operation::overlayng::OverlayNG;
using geos::operation::overlayng::OverlayPoints;

struct OverlayPointsCase {
  std::string_view name;
  int op_code;
  std::string_view wkt0;
  std::string_view wkt1;
  std::string_view expected_wkt;
};

constexpr auto kBasicOverlayCases = std::to_array<OverlayPointsCase>({
    {
        .name = "intersection_same_point",
        .op_code = OverlayNG::INTERSECTION,
        .wkt0 = "POINT (1 2)",
        .wkt1 = "POINT (1 2)",
        .expected_wkt = "POINT (1 2)",
    },
    {
        .name = "intersection_multipoints_single_hit",
        .op_code = OverlayNG::INTERSECTION,
        .wkt0 = "MULTIPOINT ((0 0), (1 2), (3 4))",
        .wkt1 = "MULTIPOINT ((1 2), (5 6))",
        .expected_wkt = "POINT (1 2)",
    },
    {
        .name = "intersection_multipoints_multi_hit",
        .op_code = OverlayNG::INTERSECTION,
        .wkt0 = "MULTIPOINT ((0 0), (1 2), (3 4))",
        .wkt1 = "MULTIPOINT ((3 4), (0 0), (9 9))",
        .expected_wkt = "MULTIPOINT ((0 0), (3 4))",
    },
    {
        .name = "intersection_disjoint_returns_point_empty",
        .op_code = OverlayNG::INTERSECTION,
        .wkt0 = "MULTIPOINT ((0 0), (1 1))",
        .wkt1 = "MULTIPOINT ((2 2), (3 3))",
        .expected_wkt = "POINT EMPTY",
    },
    {
        .name = "union_merges_duplicates",
        .op_code = OverlayNG::UNION,
        .wkt0 = "MULTIPOINT ((0 0), (1 1), (0 0))",
        .wkt1 = "MULTIPOINT ((1 1), (2 2), (2 2))",
        .expected_wkt = "MULTIPOINT ((0 0), (1 1), (2 2))",
    },
    {
        .name = "union_identical_single_points",
        .op_code = OverlayNG::UNION,
        .wkt0 = "POINT (4 5)",
        .wkt1 = "POINT (4 5)",
        .expected_wkt = "POINT (4 5)",
    },
    {
        .name = "difference_removes_matching_points",
        .op_code = OverlayNG::DIFFERENCE,
        .wkt0 = "MULTIPOINT ((0 0), (1 1), (2 2))",
        .wkt1 = "MULTIPOINT ((1 1), (9 9))",
        .expected_wkt = "MULTIPOINT ((0 0), (2 2))",
    },
    {
        .name = "difference_all_removed_returns_point_empty",
        .op_code = OverlayNG::DIFFERENCE,
        .wkt0 = "MULTIPOINT ((1 1), (2 2))",
        .wkt1 = "MULTIPOINT ((0 0), (1 1), (2 2))",
        .expected_wkt = "POINT EMPTY",
    },
    {
        .name = "symdifference_partial_overlap",
        .op_code = OverlayNG::SYMDIFFERENCE,
        .wkt0 = "MULTIPOINT ((0 0), (1 1))",
        .wkt1 = "MULTIPOINT ((1 1), (2 2))",
        .expected_wkt = "MULTIPOINT ((0 0), (2 2))",
    },
    {
        .name = "symdifference_identical_returns_point_empty",
        .op_code = OverlayNG::SYMDIFFERENCE,
        .wkt0 = "MULTIPOINT ((0 0), (1 1))",
        .wkt1 = "MULTIPOINT ((1 1), (0 0))",
        .expected_wkt = "POINT EMPTY",
    },
});

TEST(OverlayPointsTest, ComputesStandardSetOperationsWithNullPm) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const OverlayPointsCase& tc : kBasicOverlayCases) {
    SCOPED_TRACE(tc.name);
    Geometry::Ptr g0 = reader.read(std::string(tc.wkt0));
    Geometry::Ptr g1 = reader.read(std::string(tc.wkt1));
    Geometry::Ptr expected = reader.read(std::string(tc.expected_wkt));

    Geometry::Ptr result =
        OverlayPoints::overlay(tc.op_code, g0.get(), g1.get(), nullptr);
    ASSERT_NE(result, nullptr);
    if (expected->isEmpty()) {
      EXPECT_TRUE(result->isEmpty());
      EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_POINT);
    } else {
      result->normalize();
      expected->normalize();
      EXPECT_TRUE(result->equalsExact(expected.get(), 1e-12));
    }
  }
}

TEST(OverlayPointsTest, HandlesInstanceConstructionAndFloatingPrecisionModel) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  PrecisionModel pm_float(PrecisionModel::FLOATING);

  Geometry::Ptr g0 = reader.read("MULTIPOINT ((1.2 2.2), (3.7 4.7))");
  Geometry::Ptr g1 = reader.read("POINT (1.2 2.2)");

  OverlayPoints op(OverlayNG::INTERSECTION, g0.get(), g1.get(), &pm_float);
  Geometry::Ptr result = op.getResult();
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_POINT);
  EXPECT_TRUE(result->equalsExact(g1.get(), 1e-12));
}

TEST(OverlayPointsTest, ExtractsFromNestedGeometryCollectionsAndSkipsEmpty) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr g0 = reader.read(
      "GEOMETRYCOLLECTION ("
      "POINT EMPTY, "
      "POINT (1 1), "
      "MULTIPOINT (EMPTY, (2 2), (1 1)), "
      "GEOMETRYCOLLECTION (POINT (3 3), POINT EMPTY))");
  Geometry::Ptr g1 =
      reader.read("GEOMETRYCOLLECTION (POINT (2 2), POINT (4 4), POINT EMPTY)");

  Geometry::Ptr inter = OverlayPoints::overlay(OverlayNG::INTERSECTION,
                                               g0.get(), g1.get(), nullptr);
  ASSERT_NE(inter, nullptr);
  Geometry::Ptr expected_inter = reader.read("POINT (2 2)");
  EXPECT_TRUE(inter->equalsExact(expected_inter.get(), 1e-12));

  Geometry::Ptr uni =
      OverlayPoints::overlay(OverlayNG::UNION, g0.get(), g1.get(), nullptr);
  ASSERT_NE(uni, nullptr);
  Geometry::Ptr expected_uni =
      reader.read("MULTIPOINT ((1 1), (2 2), (3 3), (4 4))");
  uni->normalize();
  expected_uni->normalize();
  EXPECT_TRUE(uni->equalsExact(expected_uni.get(), 1e-12));

  Geometry::Ptr empty_pt = reader.read("POINT EMPTY");
  Geometry::Ptr empty_mp = reader.read("MULTIPOINT EMPTY");
  Geometry::Ptr empty_uni = OverlayPoints::overlay(
      OverlayNG::UNION, empty_pt.get(), empty_mp.get(), nullptr);
  ASSERT_NE(empty_uni, nullptr);
  EXPECT_TRUE(empty_uni->isEmpty());
  EXPECT_EQ(empty_uni->getGeometryTypeId(), GeometryTypeId::GEOS_POINT);
}

TEST(OverlayPointsTest, SkipsNonPointComponentsInFilter) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // Non-point components are ignored by PointExtractingFilter.
  Geometry::Ptr line = reader.read("LINESTRING (0 0, 5 5)");
  Geometry::Ptr poly = reader.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  Geometry::Ptr pt = reader.read("POINT (1 1)");

  Geometry::Ptr res =
      OverlayPoints::overlay(OverlayNG::UNION, line.get(), pt.get(), nullptr);
  ASSERT_NE(res, nullptr);
  EXPECT_TRUE(res->equalsExact(pt.get(), 1e-12));

  Geometry::Ptr both_non_pt =
      OverlayPoints::overlay(OverlayNG::UNION, line.get(), poly.get(), nullptr);
  ASSERT_NE(both_non_pt, nullptr);
  EXPECT_TRUE(both_non_pt->isEmpty());
  EXPECT_EQ(both_non_pt->getGeometryTypeId(), GeometryTypeId::GEOS_POINT);
}

TEST(OverlayPointsTest, RoundsCoordinatesToFixedPrecisionModel) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  PrecisionModel pm_unit(1.0);

  Geometry::Ptr g0 =
      reader.read("MULTIPOINT ((1.1 2.1), (1.3 2.2), (5.4 6.4))");
  Geometry::Ptr g1 = reader.read("MULTIPOINT ((0.9 1.8), (7.6 8.6))");

  Geometry::Ptr inter = OverlayPoints::overlay(OverlayNG::INTERSECTION,
                                               g0.get(), g1.get(), &pm_unit);
  ASSERT_NE(inter, nullptr);
  Geometry::Ptr expected_inter = reader.read("POINT (1 2)");
  EXPECT_TRUE(inter->equalsExact(expected_inter.get(), 1e-12));

  Geometry::Ptr uni =
      OverlayPoints::overlay(OverlayNG::UNION, g0.get(), g1.get(), &pm_unit);
  ASSERT_NE(uni, nullptr);
  Geometry::Ptr expected_uni = reader.read("MULTIPOINT ((1 2), (5 6), (8 9))");
  uni->normalize();
  expected_uni->normalize();
  EXPECT_TRUE(uni->equalsExact(expected_uni.get(), 1e-12));

  Geometry::Ptr diff = OverlayPoints::overlay(OverlayNG::DIFFERENCE, g0.get(),
                                              g1.get(), &pm_unit);
  ASSERT_NE(diff, nullptr);
  Geometry::Ptr expected_diff = reader.read("POINT (5 6)");
  EXPECT_TRUE(diff->equalsExact(expected_diff.get(), 1e-12));

  Geometry::Ptr symdiff = OverlayPoints::overlay(OverlayNG::SYMDIFFERENCE,
                                                 g0.get(), g1.get(), &pm_unit);
  ASSERT_NE(symdiff, nullptr);
  Geometry::Ptr expected_sym = reader.read("MULTIPOINT ((5 6), (8 9))");
  symdiff->normalize();
  expected_sym->normalize();
  EXPECT_TRUE(symdiff->equalsExact(expected_sym.get(), 1e-12));
}

TEST(OverlayPointsTest, PreservesZAndMValuesWithFirstOccurrenceSemantics) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Two points rounding to (1, 2) inside g0: first Z=10.5 wins over Z=99.0.
  auto pt0_a = factory->createPoint(Coordinate(1.1, 2.1, 10.5));
  auto pt0_b = factory->createPoint(Coordinate(1.2, 2.2, 99.0));
  std::vector<std::unique_ptr<Point>> pts0;
  pts0.push_back(std::move(pt0_a));
  pts0.push_back(std::move(pt0_b));
  auto mp0 = factory->createMultiPoint(std::move(pts0));

  // Matching point in g1 with different Z=42.0; g0's Z=10.5 must win.
  auto pt1 = factory->createPoint(Coordinate(0.9, 1.9, 42.0));
  PrecisionModel pm_unit(1.0);

  Geometry::Ptr inter_z = OverlayPoints::overlay(
      OverlayNG::INTERSECTION, mp0.get(), pt1.get(), &pm_unit);
  ASSERT_NE(inter_z, nullptr);
  EXPECT_EQ(inter_z->getGeometryTypeId(), GeometryTypeId::GEOS_POINT);
  EXPECT_TRUE(inter_z->hasZ());
  EXPECT_FALSE(inter_z->hasM());
  auto inter_coords = inter_z->getCoordinates();
  ASSERT_EQ(inter_coords->size(), 1u);
  EXPECT_DOUBLE_EQ(inter_coords->getX(0), 1.0);
  EXPECT_DOUBLE_EQ(inter_coords->getY(0), 2.0);
  EXPECT_DOUBLE_EQ(inter_coords->getOrdinate(0, CoordinateSequence::Z), 10.5);
  EXPECT_TRUE(std::isnan(inter_coords->getOrdinate(0, CoordinateSequence::M)));

  Geometry::Ptr union_z =
      OverlayPoints::overlay(OverlayNG::UNION, mp0.get(), pt1.get(), &pm_unit);
  ASSERT_NE(union_z, nullptr);
  EXPECT_EQ(union_z->getGeometryTypeId(), GeometryTypeId::GEOS_POINT);
  EXPECT_DOUBLE_EQ(
      union_z->getCoordinates()->getOrdinate(0, CoordinateSequence::Z), 10.5);

  // XYM points preserve M and leave Z absent.
  auto pt_m0 = factory->createPoint(CoordinateXYM(3.0, 4.0, 7.25));
  auto pt_m1 = factory->createPoint(CoordinateXYM(3.0, 4.0, 88.0));
  Geometry::Ptr inter_m = OverlayPoints::overlay(
      OverlayNG::INTERSECTION, pt_m0.get(), pt_m1.get(), nullptr);
  ASSERT_NE(inter_m, nullptr);
  EXPECT_FALSE(inter_m->hasZ());
  EXPECT_TRUE(inter_m->hasM());
  auto inter_m_coords = inter_m->getCoordinates();
  CoordinateXYM out_xym;
  inter_m_coords->getAt(0, out_xym);
  EXPECT_DOUBLE_EQ(out_xym.x, 3.0);
  EXPECT_DOUBLE_EQ(out_xym.y, 4.0);
  EXPECT_DOUBLE_EQ(out_xym.m, 7.25);
  EXPECT_TRUE(
      std::isnan(inter_m_coords->getOrdinate(0, CoordinateSequence::Z)));

  // XYZM points preserve both Z and M after fixed precision rounding.
  auto pt_zm0 = factory->createPoint(CoordinateXYZM(5.1, 6.1, 12.5, 99.5));
  auto pt_zm1 = factory->createPoint(CoordinateXYZM(4.9, 5.9, 1.0, 2.0));
  Geometry::Ptr inter_zm = OverlayPoints::overlay(
      OverlayNG::INTERSECTION, pt_zm0.get(), pt_zm1.get(), &pm_unit);
  ASSERT_NE(inter_zm, nullptr);
  EXPECT_TRUE(inter_zm->hasZ());
  EXPECT_TRUE(inter_zm->hasM());
  auto zm_coords = inter_zm->getCoordinates();
  CoordinateXYZM out_xyzm;
  zm_coords->getAt(0, out_xyzm);
  EXPECT_DOUBLE_EQ(out_xyzm.x, 5.0);
  EXPECT_DOUBLE_EQ(out_xyzm.y, 6.0);
  EXPECT_DOUBLE_EQ(out_xyzm.z, 12.5);
  EXPECT_DOUBLE_EQ(out_xyzm.m, 99.5);
}

TEST(OverlayPointsTest,
     EmptyResultPreservesMinimumCoordinateDimensionAndFactory) {
  PrecisionModel pm;
  GeometryFactory::Ptr factory_srid = GeometryFactory::create(&pm, 4326);

  // Both XYZ -> empty result has coordinate dimension 3 (XYZ).
  auto z0 = factory_srid->createPoint(Coordinate(0.0, 0.0, 1.0));
  auto z1 = factory_srid->createPoint(Coordinate(10.0, 10.0, 2.0));
  Geometry::Ptr empty_z = OverlayPoints::overlay(OverlayNG::INTERSECTION,
                                                 z0.get(), z1.get(), nullptr);
  ASSERT_NE(empty_z, nullptr);
  EXPECT_TRUE(empty_z->isEmpty());
  EXPECT_EQ(empty_z->getGeometryTypeId(), GeometryTypeId::GEOS_POINT);
  EXPECT_EQ(empty_z->getCoordinateDimension(), 3);
  EXPECT_TRUE(empty_z->hasZ());
  EXPECT_EQ(empty_z->getSRID(), 4326);

  // Both XYZM -> empty result has coordinate dimension 4 (XYZM).
  auto zm0 = factory_srid->createPoint(CoordinateXYZM(0.0, 0.0, 1.0, 2.0));
  auto zm1 = factory_srid->createPoint(CoordinateXYZM(5.0, 5.0, 3.0, 4.0));
  Geometry::Ptr empty_zm = OverlayPoints::overlay(
      OverlayNG::INTERSECTION, zm0.get(), zm1.get(), nullptr);
  ASSERT_NE(empty_zm, nullptr);
  EXPECT_TRUE(empty_zm->isEmpty());
  EXPECT_EQ(empty_zm->getCoordinateDimension(), 4);
  EXPECT_TRUE(empty_zm->hasZ());
  EXPECT_TRUE(empty_zm->hasM());

  // Mixed XYZ and XY -> empty result reduces to coordinate dimension 2 (XY).
  auto xy1 = factory_srid->createPoint(CoordinateXY(5.0, 5.0));
  Geometry::Ptr empty_mixed = OverlayPoints::overlay(
      OverlayNG::INTERSECTION, z0.get(), xy1.get(), nullptr);
  ASSERT_NE(empty_mixed, nullptr);
  EXPECT_TRUE(empty_mixed->isEmpty());
  EXPECT_EQ(empty_mixed->getCoordinateDimension(), 2);
  EXPECT_FALSE(empty_mixed->hasZ());
  EXPECT_FALSE(empty_mixed->hasM());

  // Unknown opCode falls through switch to empty result.
  Geometry::Ptr same_xy = factory_srid->createPoint(CoordinateXY(5.0, 5.0));
  Geometry::Ptr unknown_op =
      OverlayPoints::overlay(99, xy1.get(), same_xy.get(), nullptr);
  ASSERT_NE(unknown_op, nullptr);
  EXPECT_TRUE(unknown_op->isEmpty());
  EXPECT_EQ(unknown_op->getGeometryTypeId(), GeometryTypeId::GEOS_POINT);
}

}  // namespace
}  // namespace geos_google
