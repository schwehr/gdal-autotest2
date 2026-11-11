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
// third_party/geos/src/precision/SimpleGeometryPrecisionReducer.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/precision/SimpleGeometryPrecisionReducer.cpp

#include "third_party/geos/include/geos/precision/SimpleGeometryPrecisionReducer.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GeometryTypeId;
using geos::geom::GEOS_GEOMETRYCOLLECTION;
using geos::geom::GEOS_LINEARRING;
using geos::geom::GEOS_LINESTRING;
using geos::geom::GEOS_MULTILINESTRING;
using geos::geom::GEOS_MULTIPOINT;
using geos::geom::GEOS_MULTIPOLYGON;
using geos::geom::GEOS_POINT;
using geos::geom::GEOS_POLYGON;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::precision::SimpleGeometryPrecisionReducer;
using geos::util::UnsupportedOperationException;

class SimpleGeometryPrecisionReducerTest : public ::testing::Test {
 protected:
  SimpleGeometryPrecisionReducerTest()
      : factory_(GeometryFactory::create()), reader_(factory_.get()) {}

  std::unique_ptr<Geometry> ReadWkt(std::string_view wkt) const {
    return reader_.read(std::string(wkt));
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(SimpleGeometryPrecisionReducerTest, ConstructorDefaultsAndAccessors) {
  PrecisionModel pm(10.0);
  SimpleGeometryPrecisionReducer reducer(&pm);

  EXPECT_EQ(reducer.getPrecisionModel(), &pm);
  EXPECT_TRUE(reducer.getRemoveCollapsed());

  reducer.setRemoveCollapsedComponents(false);
  EXPECT_FALSE(reducer.getRemoveCollapsed());

  reducer.setRemoveCollapsedComponents(true);
  EXPECT_TRUE(reducer.getRemoveCollapsed());
}

TEST_F(SimpleGeometryPrecisionReducerTest, ReducesPointsAcrossPrecisionModels) {
  struct Case {
    std::string_view name;
    PrecisionModel::Type type;
    double scale;
    std::string_view input_wkt;
    double expected_x;
    double expected_y;
    double tolerance;
  };

  constexpr auto kCases = std::to_array<Case>({
      {
          .name = "fixed_scale_1_positive",
          .type = PrecisionModel::FIXED,
          .scale = 1.0,
          .input_wkt = "POINT (1.24 2.76)",
          .expected_x = 1.0,
          .expected_y = 3.0,
          .tolerance = 0.0,
      },
      {
          .name = "fixed_scale_1_negative",
          .type = PrecisionModel::FIXED,
          .scale = 1.0,
          .input_wkt = "POINT (-1.24 -2.76)",
          .expected_x = -1.0,
          .expected_y = -3.0,
          .tolerance = 0.0,
      },
      {
          .name = "fixed_scale_4_binary_grid",
          .type = PrecisionModel::FIXED,
          .scale = 4.0,
          .input_wkt = "POINT (1.12 2.38)",
          .expected_x = 1.0,
          .expected_y = 2.5,
          .tolerance = 0.0,
      },
      {
          .name = "fixed_scale_10_decimal",
          .type = PrecisionModel::FIXED,
          .scale = 10.0,
          .input_wkt = "POINT (1.234 5.678)",
          .expected_x = 1.2,
          .expected_y = 5.7,
          .tolerance = 1e-12,
      },
      {
          .name = "negative_scale_grid_10",
          .type = PrecisionModel::FIXED,
          .scale = -10.0,
          .input_wkt = "POINT (14.9 25.1)",
          .expected_x = 10.0,
          .expected_y = 30.0,
          .tolerance = 0.0,
      },
      {
          .name = "floating_noop",
          .type = PrecisionModel::FLOATING,
          .scale = 0.0,
          .input_wkt = "POINT (1.23456789 9.87654321)",
          .expected_x = 1.23456789,
          .expected_y = 9.87654321,
          .tolerance = 0.0,
      },
      {
          .name = "floating_single",
          .type = PrecisionModel::FLOATING_SINGLE,
          .scale = 0.0,
          .input_wkt = "POINT (1.123456789 2.987654321)",
          .expected_x = static_cast<double>(static_cast<float>(1.123456789)),
          .expected_y = static_cast<double>(static_cast<float>(2.987654321)),
          .tolerance = 0.0,
      },
  });

  for (const Case& c : kCases) {
    SCOPED_TRACE(c.name);
    PrecisionModel pm = (c.type == PrecisionModel::FIXED)
                            ? PrecisionModel(c.scale)
                            : PrecisionModel(c.type);
    SimpleGeometryPrecisionReducer reducer(&pm);
    std::unique_ptr<Geometry> input = ReadWkt(c.input_wkt);
    std::unique_ptr<Geometry> reduced = reducer.reduce(input.get());
    ASSERT_NE(reduced, nullptr);
    EXPECT_EQ(reduced->getFactory(), factory_.get());
    EXPECT_TRUE(reduced->getPrecisionModel()->isFloating());
    const auto* pt = dynamic_cast<const Point*>(reduced.get());
    ASSERT_NE(pt, nullptr);
    EXPECT_NEAR(pt->getX(), c.expected_x, c.tolerance);
    EXPECT_NEAR(pt->getY(), c.expected_y, c.tolerance);
    EXPECT_FALSE(pt->hasZ());
    EXPECT_TRUE(std::isnan(pt->getZ()));
  }

  PrecisionModel pm_z(1.0);
  SimpleGeometryPrecisionReducer reducer_z(&pm_z);
  std::unique_ptr<Geometry> pt_z_in = ReadWkt("POINT Z (1.26 3.74 9.5)");
  std::unique_ptr<Geometry> pt_z_out = reducer_z.reduce(pt_z_in.get());
  const auto* pz = dynamic_cast<const Point*>(pt_z_out.get());
  ASSERT_NE(pz, nullptr);
  EXPECT_TRUE(pz->hasZ());
  EXPECT_DOUBLE_EQ(pz->getX(), 1.0);
  EXPECT_DOUBLE_EQ(pz->getY(), 4.0);
  EXPECT_DOUBLE_EQ(pz->getZ(), 9.5);
}

TEST_F(SimpleGeometryPrecisionReducerTest, HandlesEmptyGeometries) {
  struct EmptyCase {
    std::string_view wkt;
    GeometryTypeId type_id;
  };

  constexpr auto kEmptyCases = std::to_array<EmptyCase>({
      {.wkt = "POINT EMPTY", .type_id = GEOS_POINT},
      {.wkt = "LINESTRING EMPTY", .type_id = GEOS_LINESTRING},
      {.wkt = "LINEARRING EMPTY", .type_id = GEOS_LINEARRING},
      {.wkt = "POLYGON EMPTY", .type_id = GEOS_POLYGON},
      {.wkt = "MULTIPOINT EMPTY", .type_id = GEOS_MULTIPOINT},
      {.wkt = "MULTILINESTRING EMPTY", .type_id = GEOS_MULTILINESTRING},
      {.wkt = "MULTIPOLYGON EMPTY", .type_id = GEOS_MULTIPOLYGON},
      {.wkt = "GEOMETRYCOLLECTION EMPTY", .type_id = GEOS_GEOMETRYCOLLECTION},
  });

  PrecisionModel pm(1.0);
  SimpleGeometryPrecisionReducer reducer(&pm);
  for (const EmptyCase& c : kEmptyCases) {
    SCOPED_TRACE(c.wkt);
    std::unique_ptr<Geometry> input = ReadWkt(c.wkt);
    std::unique_ptr<Geometry> reduced = reducer.reduce(input.get());
    ASSERT_NE(reduced, nullptr);
    EXPECT_TRUE(reduced->isEmpty());
    EXPECT_EQ(reduced->getGeometryTypeId(), c.type_id);
  }
}

TEST_F(SimpleGeometryPrecisionReducerTest,
       LineStringRemovesRepeatedPointsAndHandlesCollapse) {
  PrecisionModel pm(1.0);
  SimpleGeometryPrecisionReducer reducer(&pm);

  std::unique_ptr<Geometry> input =
      ReadWkt("LINESTRING (0.1 0.1, 0.2 0.2, 1.9 2.1)");
  std::unique_ptr<Geometry> reduced = reducer.reduce(input.get());
  EXPECT_EQ(reduced->getGeometryTypeId(), GEOS_LINESTRING);
  EXPECT_EQ(reduced->getNumPoints(), 2u);
  EXPECT_TRUE(reduced->equalsExact(ReadWkt("LINESTRING (0 0, 2 2)").get()));

  // Collapse to 1 unique point (< minLength 2): removed by default.
  std::unique_ptr<Geometry> collapsing =
      ReadWkt("LINESTRING (0.1 0.1, 0.2 0.2, 0.3 0.3)");
  std::unique_ptr<Geometry> removed = reducer.reduce(collapsing.get());
  EXPECT_EQ(removed->getGeometryTypeId(), GEOS_LINESTRING);
  EXPECT_TRUE(removed->isEmpty());

  // When removeCollapsed == false, original coordinate sequence length is kept.
  reducer.setRemoveCollapsedComponents(false);
  std::unique_ptr<Geometry> kept = reducer.reduce(collapsing.get());
  const auto* line = dynamic_cast<const LineString*>(kept.get());
  ASSERT_NE(line, nullptr);
  EXPECT_FALSE(line->isEmpty());
  EXPECT_EQ(line->getNumPoints(), 3u);
  EXPECT_DOUBLE_EQ(line->getLength(), 0.0);
  const CoordinateSequence* seq = line->getCoordinatesRO();
  for (std::size_t i = 0; i < seq->size(); ++i) {
    EXPECT_DOUBLE_EQ(seq->getX(i), 0.0);
    EXPECT_DOUBLE_EQ(seq->getY(i), 0.0);
  }
}

TEST_F(SimpleGeometryPrecisionReducerTest, LinearRingReductionAndCollapse) {
  PrecisionModel pm(1.0);
  SimpleGeometryPrecisionReducer reducer(&pm);

  // Valid 5-point ring stays 5-point LinearRing.
  std::unique_ptr<Geometry> valid_in =
      ReadWkt("LINEARRING (0.1 0.1, 2.1 0.1, 2.1 2.1, 0.1 2.1, 0.1 0.1)");
  std::unique_ptr<Geometry> valid_out = reducer.reduce(valid_in.get());
  EXPECT_EQ(valid_out->getGeometryTypeId(), GEOS_LINEARRING);
  EXPECT_TRUE(valid_out->equalsExact(
      ReadWkt("LINEARRING (0 0, 2 0, 2 2, 0 2, 0 0)").get()));

  // 5-point ring collapsing one edge to 4 points (== minLength 4) succeeds.
  std::unique_ptr<Geometry> partial_in =
      ReadWkt("LINEARRING (0.1 0.1, 2.1 0.1, 2.1 0.2, 2.1 2.1, 0.1 0.1)");
  std::unique_ptr<Geometry> partial_out = reducer.reduce(partial_in.get());
  EXPECT_EQ(partial_out->getGeometryTypeId(), GEOS_LINEARRING);
  EXPECT_EQ(partial_out->getNumPoints(), 4u);
  EXPECT_TRUE(partial_out->equalsExact(
      ReadWkt("LINEARRING (0 0, 2 0, 2 2, 0 0)").get()));

  // 4-point ring collapsing to 3 unique points (< minLength 4).
  std::unique_ptr<Geometry> tri_in =
      ReadWkt("LINEARRING (0.1 0.1, 2.1 0.1, 2.1 0.2, 0.1 0.1)");
  std::unique_ptr<Geometry> tri_removed = reducer.reduce(tri_in.get());
  EXPECT_EQ(tri_removed->getGeometryTypeId(), GEOS_LINEARRING);
  EXPECT_TRUE(tri_removed->isEmpty());

  reducer.setRemoveCollapsedComponents(false);
  std::unique_ptr<Geometry> tri_kept = reducer.reduce(tri_in.get());
  const auto* kept_tri_ring = dynamic_cast<const LinearRing*>(tri_kept.get());
  ASSERT_NE(kept_tri_ring, nullptr);
  EXPECT_EQ(kept_tri_ring->getNumPoints(), 4u);
  EXPECT_TRUE(kept_tri_ring->equalsExact(
      ReadWkt("LINEARRING (0 0, 2 0, 2 0, 0 0)").get()));

  // 5-point ring collapsing to 1 unique point (< minLength 4).
  std::unique_ptr<Geometry> full_in =
      ReadWkt("LINEARRING (0.1 0.1, 0.2 0.1, 0.2 0.2, 0.1 0.2, 0.1 0.1)");
  reducer.setRemoveCollapsedComponents(true);
  std::unique_ptr<Geometry> full_removed = reducer.reduce(full_in.get());
  EXPECT_EQ(full_removed->getGeometryTypeId(), GEOS_LINEARRING);
  EXPECT_TRUE(full_removed->isEmpty());

  reducer.setRemoveCollapsedComponents(false);
  std::unique_ptr<Geometry> full_kept = reducer.reduce(full_in.get());
  const auto* kept_full_ring = dynamic_cast<const LinearRing*>(full_kept.get());
  ASSERT_NE(kept_full_ring, nullptr);
  EXPECT_EQ(kept_full_ring->getNumPoints(), 5u);
  EXPECT_FALSE(kept_full_ring->isValid());
}

TEST_F(SimpleGeometryPrecisionReducerTest, PolygonAndHoleCollapseBehavior) {
  PrecisionModel pm(1.0);
  SimpleGeometryPrecisionReducer reducer(&pm);

  // Valid polygon with surviving hole.
  std::unique_ptr<Geometry> valid_in = ReadWkt(
      "POLYGON ((0.1 0.1, 4.1 0.1, 4.1 4.1, 0.1 4.1, 0.1 0.1), "
      "(1.1 1.1, 2.9 1.1, 2.9 2.9, 1.1 2.9, 1.1 1.1))");
  std::unique_ptr<Geometry> valid_out = reducer.reduce(valid_in.get());
  EXPECT_EQ(valid_out->getGeometryTypeId(), GEOS_POLYGON);
  EXPECT_TRUE(valid_out->isValid());
  EXPECT_TRUE(
      valid_out->equalsExact(ReadWkt("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), "
                                     "(1 1, 3 1, 3 3, 1 3, 1 1))")
                                 .get()));

  // Sliver hole is dropped when removeCollapsed == true, kept when false.
  std::unique_ptr<Geometry> hole_sliver_in = ReadWkt(
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), "
      "(1.1 1.1, 2.9 1.1, 2.9 1.2, 1.1 1.2, 1.1 1.1))");
  std::unique_ptr<Geometry> hole_removed = reducer.reduce(hole_sliver_in.get());
  const auto* poly_no_hole = dynamic_cast<const Polygon*>(hole_removed.get());
  ASSERT_NE(poly_no_hole, nullptr);
  EXPECT_TRUE(poly_no_hole->isValid());
  EXPECT_EQ(poly_no_hole->getNumInteriorRing(), 0u);
  EXPECT_DOUBLE_EQ(poly_no_hole->getArea(), 16.0);

  reducer.setRemoveCollapsedComponents(false);
  std::unique_ptr<Geometry> hole_kept = reducer.reduce(hole_sliver_in.get());
  const auto* poly_with_hole = dynamic_cast<const Polygon*>(hole_kept.get());
  ASSERT_NE(poly_with_hole, nullptr);
  EXPECT_EQ(poly_with_hole->getNumInteriorRing(), 1u);
  EXPECT_EQ(poly_with_hole->getInteriorRingN(0)->getNumPoints(), 5u);
  EXPECT_FALSE(poly_with_hole->isValid());

  // Sliver exterior collapses polygon to empty when removeCollapsed == true,
  // or retains an invalid degenerate polygon when false.
  std::unique_ptr<Geometry> shell_sliver_in =
      ReadWkt("POLYGON ((0.1 0.1, 2.1 0.1, 2.1 0.2, 0.1 0.2, 0.1 0.1))");
  reducer.setRemoveCollapsedComponents(true);
  std::unique_ptr<Geometry> shell_removed =
      reducer.reduce(shell_sliver_in.get());
  EXPECT_EQ(shell_removed->getGeometryTypeId(), GEOS_POLYGON);
  EXPECT_TRUE(shell_removed->isEmpty());

  reducer.setRemoveCollapsedComponents(false);
  std::unique_ptr<Geometry> shell_kept = reducer.reduce(shell_sliver_in.get());
  const auto* collapsed_poly = dynamic_cast<const Polygon*>(shell_kept.get());
  ASSERT_NE(collapsed_poly, nullptr);
  EXPECT_FALSE(collapsed_poly->isEmpty());
  EXPECT_EQ(collapsed_poly->getNumPoints(), 5u);
  EXPECT_DOUBLE_EQ(collapsed_poly->getArea(), 0.0);
  EXPECT_FALSE(collapsed_poly->isValid());
}

TEST_F(SimpleGeometryPrecisionReducerTest,
       HandlesMultiGeometriesAndGeometryCollections) {
  PrecisionModel pm(1.0);
  SimpleGeometryPrecisionReducer reducer(&pm);

  // MultiPoint keeps all points (each Point has 1 coordinate).
  std::unique_ptr<Geometry> mp_in =
      ReadWkt("MULTIPOINT ((0.1 0.2), (0.2 0.3), (2.1 2.2))");
  std::unique_ptr<Geometry> mp_out = reducer.reduce(mp_in.get());
  EXPECT_EQ(mp_out->getGeometryTypeId(), GEOS_MULTIPOINT);
  EXPECT_EQ(mp_out->getNumGeometries(), 3u);
  EXPECT_TRUE(
      mp_out->equalsExact(ReadWkt("MULTIPOINT ((0 0), (0 0), (2 2))").get()));

  // MultiLineString prunes collapsed lines when removeCollapsed == true.
  std::unique_ptr<Geometry> mls_in =
      ReadWkt("MULTILINESTRING ((0.1 0.1, 0.2 0.2), (1.1 1.1, 3.1 1.1))");
  std::unique_ptr<Geometry> mls_removed = reducer.reduce(mls_in.get());
  EXPECT_EQ(mls_removed->getGeometryTypeId(), GEOS_MULTILINESTRING);
  EXPECT_EQ(mls_removed->getNumGeometries(), 1u);
  EXPECT_TRUE(
      mls_removed->equalsExact(ReadWkt("MULTILINESTRING ((1 1, 3 1))").get()));

  reducer.setRemoveCollapsedComponents(false);
  std::unique_ptr<Geometry> mls_kept = reducer.reduce(mls_in.get());
  EXPECT_EQ(mls_kept->getGeometryTypeId(), GEOS_MULTILINESTRING);
  EXPECT_EQ(mls_kept->getNumGeometries(), 2u);

  // MultiPolygon keeps distinct components without topological unioning.
  std::unique_ptr<Geometry> mpoly_in = ReadWkt(
      "MULTIPOLYGON (((0 0, 0.9 0, 0.9 2, 0 2, 0 0)), "
      "((1.1 0, 2 0, 2 2, 1.1 2, 1.1 0)), "
      "((3.1 3.1, 4.1 3.1, 4.1 3.2, 3.1 3.2, 3.1 3.1)))");
  reducer.setRemoveCollapsedComponents(true);
  std::unique_ptr<Geometry> mpoly_removed = reducer.reduce(mpoly_in.get());
  EXPECT_EQ(mpoly_removed->getGeometryTypeId(), GEOS_MULTIPOLYGON);
  EXPECT_EQ(mpoly_removed->getNumGeometries(), 2u);

  reducer.setRemoveCollapsedComponents(false);
  std::unique_ptr<Geometry> mpoly_kept = reducer.reduce(mpoly_in.get());
  EXPECT_EQ(mpoly_kept->getGeometryTypeId(), GEOS_MULTIPOLYGON);
  EXPECT_EQ(mpoly_kept->getNumGeometries(), 3u);

  // GeometryCollection respects removeCollapsed across mixed components.
  std::unique_ptr<Geometry> gc_in = ReadWkt(
      "GEOMETRYCOLLECTION (POINT (1.2 2.8), "
      "LINESTRING (0.1 0.1, 0.2 0.2), "
      "POLYGON ((0.1 0.1, 2.1 0.1, 2.1 0.2, 0.1 0.2, 0.1 0.1)), "
      "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)))");
  reducer.setRemoveCollapsedComponents(true);
  std::unique_ptr<Geometry> gc_removed = reducer.reduce(gc_in.get());
  EXPECT_EQ(gc_removed->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(gc_removed->getNumGeometries(), 2u);

  reducer.setRemoveCollapsedComponents(false);
  std::unique_ptr<Geometry> gc_kept = reducer.reduce(gc_in.get());
  EXPECT_EQ(gc_kept->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(gc_kept->getNumGeometries(), 4u);
}

TEST_F(SimpleGeometryPrecisionReducerTest, ThrowsOnCurvedGeometry) {
  PrecisionModel pm(1.0);
  SimpleGeometryPrecisionReducer reducer(&pm);
  std::unique_ptr<Geometry> curve = ReadWkt("CIRCULARSTRING (0 0, 1 1, 2 0)");
  EXPECT_THROW(reducer.reduce(curve.get()), UnsupportedOperationException);
}

}  // namespace
}  // namespace geos_google
