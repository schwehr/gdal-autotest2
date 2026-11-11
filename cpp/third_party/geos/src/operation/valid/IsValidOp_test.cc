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

// Unit tests for third_party/geos/src/operation/valid/IsValidOp.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - Uses an isolated GeometryFactory per test via GeometryFactory::create().
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/valid/IsValidOp.cpp

#include "third_party/geos/include/geos/operation/valid/IsValidOp.h"

#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/valid/TopologyValidationError.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::DoubleNotANumber;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::GeometryTypeId;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::MultiLineString;
using geos::geom::MultiPoint;
using geos::geom::MultiPolygon;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::operation::valid::IsValidOp;
using geos::operation::valid::TopologyValidationError;
using geos::util::IllegalArgumentException;
using geos::util::UnsupportedOperationException;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

struct WktValidationCase {
  std::string_view name;
  std::string_view wkt;
};

struct WktInvalidCase {
  std::string_view name;
  std::string_view wkt;
  int error_type;
  double x;
  double y;
};

class IsValidOpTest : public ::testing::Test {
 protected:
  IsValidOpTest()
      : factory_(GeometryFactory::create()), reader_(factory_.get()) {}

  std::unique_ptr<Geometry> ReadWkt(std::string_view wkt) const {
    return reader_.read(std::string(wkt));
  }

  static CoordinateSequence MakeCoords(
      std::initializer_list<CoordinateXY> coords) {
    return CoordinateSequence(coords);
  }

  std::unique_ptr<LineString> MakeLineString(
      std::initializer_list<CoordinateXY> coords) const {
    return factory_->createLineString(
        std::make_unique<CoordinateSequence>(coords));
  }

  std::unique_ptr<LinearRing> MakeRing(
      std::initializer_list<CoordinateXY> coords) const {
    return factory_->createLinearRing(
        std::make_unique<CoordinateSequence>(coords));
  }

  std::unique_ptr<Polygon> MakePolygon(
      std::initializer_list<CoordinateXY> shell_coords) const {
    return factory_->createPolygon(MakeRing(shell_coords));
  }

  std::unique_ptr<Polygon> MakePolygon(
      std::initializer_list<CoordinateXY> shell_coords,
      std::vector<std::unique_ptr<LinearRing>> holes) const {
    return factory_->createPolygon(MakeRing(shell_coords), std::move(holes));
  }

  void ExpectValid(const Geometry* geom,
                   bool self_touching_valid = false) const {
    ASSERT_NE(geom, nullptr);
    if (!self_touching_valid) {
      EXPECT_TRUE(IsValidOp::isValid(geom));
    }
    IsValidOp op(geom);
    op.setSelfTouchingRingFormingHoleValid(self_touching_valid);
    EXPECT_TRUE(op.isValid());
    EXPECT_EQ(op.getValidationError(), nullptr);
  }

  void ExpectInvalidWithCandidateCoords(
      const Geometry* geom, int expected_error,
      std::initializer_list<CoordinateXY> candidates,
      bool self_touching_valid = false) const {
    ASSERT_NE(geom, nullptr);
    if (!self_touching_valid) {
      EXPECT_FALSE(IsValidOp::isValid(geom));
    }
    IsValidOp op(geom);
    op.setSelfTouchingRingFormingHoleValid(self_touching_valid);
    EXPECT_FALSE(op.isValid());

    const TopologyValidationError* err = op.getValidationError();
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->getErrorType(), expected_error);
    EXPECT_FALSE(err->getMessage().empty());
    EXPECT_FALSE(err->toString().empty());

    const CoordinateXY& actual = err->getCoordinate();
    bool matched = false;
    for (const CoordinateXY& candidate : candidates) {
      if (actual.equals2D(candidate)) {
        matched = true;
        break;
      }
    }
    EXPECT_TRUE(matched) << "Unexpected validation error coordinate ("
                         << actual.x << ", " << actual.y << ")";
  }

  void ExpectInvalid(const Geometry* geom, int expected_error,
                     double expected_x, double expected_y,
                     bool self_touching_valid = false) const {
    ExpectInvalidWithCandidateCoords(
        geom, expected_error, {{expected_x, expected_y}}, self_touching_valid);
  }

  void ExpectInvalidNonFiniteCoord(const Geometry* geom) const {
    ASSERT_NE(geom, nullptr);
    EXPECT_FALSE(IsValidOp::isValid(geom));

    IsValidOp op(geom);
    EXPECT_FALSE(op.isValid());

    const TopologyValidationError* err = op.getValidationError();
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->getErrorType(), TopologyValidationError::eInvalidCoordinate);
    const CoordinateXY& pt = err->getCoordinate();
    EXPECT_FALSE(std::isfinite(pt.x) && std::isfinite(pt.y));
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(IsValidOpTest, CoordinateValidationFiniteAndNonFinite) {
  const std::array<CoordinateXY, 5> valid_xy = {{
      {0.0, 0.0},
      {-123.456, 789.012},
      {std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()},
      {std::numeric_limits<double>::lowest(),
       std::numeric_limits<double>::min()},
      {std::numeric_limits<double>::denorm_min(), -0.0},
  }};
  for (const CoordinateXY& coord : valid_xy) {
    EXPECT_TRUE(IsValidOp::isValid(&coord));
    EXPECT_TRUE(IsValidOp::isValid(coord));
  }

  // Z and M ordinates may be NaN as long as X and Y are finite.
  const Coordinate xyz_nan_z(1.0, 2.0, DoubleNotANumber);
  const CoordinateXYM xym_nan_m(1.0, 2.0, DoubleNotANumber);
  const CoordinateXYZM xyzm_nan_zm(1.0, 2.0, DoubleNotANumber,
                                   DoubleNotANumber);
  EXPECT_TRUE(IsValidOp::isValid(&xyz_nan_z));
  EXPECT_TRUE(IsValidOp::isValid(xyz_nan_z));
  EXPECT_TRUE(IsValidOp::isValid(&xym_nan_m));
  EXPECT_TRUE(IsValidOp::isValid(xym_nan_m));
  EXPECT_TRUE(IsValidOp::isValid(&xyzm_nan_zm));
  EXPECT_TRUE(IsValidOp::isValid(xyzm_nan_zm));

  const std::array<CoordinateXY, 7> invalid_xy = {{
      {kNaN, 0.0},
      {0.0, kNaN},
      {kNaN, kNaN},
      {kInf, 0.0},
      {-kInf, 0.0},
      {0.0, kInf},
      {0.0, -kInf},
  }};
  for (const CoordinateXY& coord : invalid_xy) {
    EXPECT_FALSE(IsValidOp::isValid(&coord));
    EXPECT_FALSE(IsValidOp::isValid(coord));
  }
}

TEST_F(IsValidOpTest, NullGeometryThrowsIllegalArgument) {
  const Geometry* null_geom = nullptr;
  EXPECT_THROW(IsValidOp::isValid(null_geom), IllegalArgumentException);

  IsValidOp op(null_geom);
  EXPECT_THROW(op.isValid(), IllegalArgumentException);
  EXPECT_THROW(op.getValidationError(), IllegalArgumentException);
}

TEST_F(IsValidOpTest, EmptyGeometriesOfAllTypesAreValid) {
  constexpr auto kEmptyTypeIds = std::to_array<GeometryTypeId>({
      GeometryTypeId::GEOS_POINT,
      GeometryTypeId::GEOS_LINESTRING,
      GeometryTypeId::GEOS_LINEARRING,
      GeometryTypeId::GEOS_POLYGON,
      GeometryTypeId::GEOS_MULTIPOINT,
      GeometryTypeId::GEOS_MULTILINESTRING,
      GeometryTypeId::GEOS_MULTIPOLYGON,
      GeometryTypeId::GEOS_GEOMETRYCOLLECTION,
      GeometryTypeId::GEOS_CIRCULARSTRING,
      GeometryTypeId::GEOS_COMPOUNDCURVE,
      GeometryTypeId::GEOS_CURVEPOLYGON,
      GeometryTypeId::GEOS_MULTICURVE,
      GeometryTypeId::GEOS_MULTISURFACE,
  });

  for (GeometryTypeId type_id : kEmptyTypeIds) {
    SCOPED_TRACE(static_cast<int>(type_id));
    std::unique_ptr<Geometry> empty = factory_->createEmptyGeometry(type_id);
    ASSERT_TRUE(empty->isEmpty());
    ExpectValid(empty.get());
  }

  constexpr auto kEmptyWktCases = std::to_array<WktValidationCase>({
      {.name = "point", .wkt = "POINT EMPTY"},
      {.name = "linestring", .wkt = "LINESTRING EMPTY"},
      {.name = "linearring", .wkt = "LINEARRING EMPTY"},
      {.name = "polygon", .wkt = "POLYGON EMPTY"},
      {.name = "multipoint", .wkt = "MULTIPOINT EMPTY"},
      {.name = "multilinestring", .wkt = "MULTILINESTRING EMPTY"},
      {.name = "multipolygon", .wkt = "MULTIPOLYGON EMPTY"},
      {.name = "collection", .wkt = "GEOMETRYCOLLECTION EMPTY"},
      {.name = "circularstring", .wkt = "CIRCULARSTRING EMPTY"},
      {.name = "compoundcurve", .wkt = "COMPOUNDCURVE EMPTY"},
      {.name = "curvepolygon", .wkt = "CURVEPOLYGON EMPTY"},
      {.name = "multicurve", .wkt = "MULTICURVE EMPTY"},
      {.name = "multisurface", .wkt = "MULTISURFACE EMPTY"},
  });

  for (const auto& tc : kEmptyWktCases) {
    SCOPED_TRACE(tc.name);
    std::unique_ptr<Geometry> geom = ReadWkt(tc.wkt);
    ExpectValid(geom.get());
  }
}

TEST_F(IsValidOpTest, NonEmptyCurvedTypesThrowUnsupportedOperation) {
  constexpr auto kCurvedWktCases = std::to_array<WktValidationCase>({
      {.name = "circularstring", .wkt = "CIRCULARSTRING (0 0, 1 1, 2 0)"},
      {.name = "compoundcurve",
       .wkt = "COMPOUNDCURVE (CIRCULARSTRING (0 0, 1 1, 2 0), (2 0, 3 0))"},
      {.name = "curvepolygon",
       .wkt = "CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0))"},
      {.name = "multicurve", .wkt = "MULTICURVE ((0 0, 1 1))"},
      {.name = "multisurface",
       .wkt = "MULTISURFACE (((0 0, 1 0, 1 1, 0 1, 0 0)))"},
  });

  for (const auto& tc : kCurvedWktCases) {
    SCOPED_TRACE(tc.name);
    std::unique_ptr<Geometry> geom = ReadWkt(tc.wkt);
    EXPECT_THROW(IsValidOp::isValid(geom.get()), UnsupportedOperationException);

    IsValidOp op(geom.get());
    EXPECT_THROW(op.isValid(), UnsupportedOperationException);
    EXPECT_THROW(op.getValidationError(), UnsupportedOperationException);
  }
}

TEST_F(IsValidOpTest, PointValidation) {
  std::unique_ptr<Point> p2d = factory_->createPoint(CoordinateXY(3.5, -2.5));
  ExpectValid(p2d.get());

  std::unique_ptr<Point> p_nan_z =
      factory_->createPoint(Coordinate(1.0, 2.0, DoubleNotANumber));
  ExpectValid(p_nan_z.get());

  std::unique_ptr<Point> p_nan_m =
      factory_->createPoint(CoordinateXYM(1.0, 2.0, DoubleNotANumber));
  ExpectValid(p_nan_m.get());

  std::unique_ptr<Point> p_nan_x =
      factory_->createPoint(CoordinateXY(kNaN, 2.0));
  ExpectInvalidNonFiniteCoord(p_nan_x.get());

  std::unique_ptr<Point> p_nan_y =
      factory_->createPoint(CoordinateXY(1.0, kNaN));
  ExpectInvalidNonFiniteCoord(p_nan_y.get());

  std::unique_ptr<Point> p_inf_x =
      factory_->createPoint(CoordinateXY(kInf, 2.0));
  ExpectInvalidNonFiniteCoord(p_inf_x.get());

  std::unique_ptr<Point> p_neg_inf_y =
      factory_->createPoint(CoordinateXY(1.0, -kInf));
  ExpectInvalidNonFiniteCoord(p_neg_inf_y.get());
}

TEST_F(IsValidOpTest, MultiPointValidation) {
  std::unique_ptr<Geometry> valid_distinct =
      ReadWkt("MULTIPOINT ((0 0), (1 2), (-3 4))");
  ExpectValid(valid_distinct.get());

  std::unique_ptr<Geometry> valid_duplicate =
      ReadWkt("MULTIPOINT ((1 2), (1 2))");
  ExpectValid(valid_duplicate.get());

  // Empty components inside a non-empty MultiPoint should be skipped cleanly.
  std::unique_ptr<Geometry> valid_with_empty =
      ReadWkt("MULTIPOINT (EMPTY, (1 2), EMPTY)");
  ExpectValid(valid_with_empty.get());

  std::vector<std::unique_ptr<Point>> pts_first_bad;
  pts_first_bad.push_back(factory_->createPoint(CoordinateXY(kNaN, 1.0)));
  pts_first_bad.push_back(factory_->createPoint(CoordinateXY(2.0, 3.0)));
  std::unique_ptr<MultiPoint> mp_first_bad =
      factory_->createMultiPoint(std::move(pts_first_bad));
  ExpectInvalidNonFiniteCoord(mp_first_bad.get());

  std::vector<std::unique_ptr<Point>> pts_second_bad;
  pts_second_bad.push_back(factory_->createPoint());
  pts_second_bad.push_back(factory_->createPoint(CoordinateXY(1.0, 2.0)));
  pts_second_bad.push_back(factory_->createPoint(CoordinateXY(3.0, kInf)));
  std::unique_ptr<MultiPoint> mp_second_bad =
      factory_->createMultiPoint(std::move(pts_second_bad));
  ExpectInvalidNonFiniteCoord(mp_second_bad.get());
}

TEST_F(IsValidOpTest, LineStringValidation) {
  constexpr auto kValidLines = std::to_array<WktValidationCase>({
      {.name = "two_points", .wkt = "LINESTRING (0 0, 1 0)"},
      {.name = "three_points", .wkt = "LINESTRING (0 0, 1 0, 2 1)"},
      {.name = "repeated_plus_distinct", .wkt = "LINESTRING (0 0, 0 0, 1 1)"},
      {.name = "self_intersecting", .wkt = "LINESTRING (0 0, 2 2, 0 2, 2 0)"},
      {.name = "closed_loop", .wkt = "LINESTRING (0 0, 1 0, 0 0)"},
  });
  for (const auto& tc : kValidLines) {
    SCOPED_TRACE(tc.name);
    std::unique_ptr<Geometry> line = ReadWkt(tc.wkt);
    ExpectValid(line.get());
  }

  std::unique_ptr<LineString> too_few_two =
      MakeLineString({{3.0, 4.0}, {3.0, 4.0}});
  ExpectInvalid(too_few_two.get(), TopologyValidationError::eTooFewPoints, 3.0,
                4.0);

  std::unique_ptr<LineString> too_few_three =
      MakeLineString({{5.0, 6.0}, {5.0, 6.0}, {5.0, 6.0}});
  ExpectInvalid(too_few_three.get(), TopologyValidationError::eTooFewPoints,
                5.0, 6.0);

  std::unique_ptr<LineString> line_nan_start =
      MakeLineString({{kNaN, 0.0}, {1.0, 1.0}});
  ExpectInvalidNonFiniteCoord(line_nan_start.get());

  std::unique_ptr<LineString> line_inf_mid =
      MakeLineString({{0.0, 0.0}, {1.0, kInf}, {2.0, 0.0}});
  ExpectInvalidNonFiniteCoord(line_inf_mid.get());

  // Repeated points followed by a single distinct endpoint at the very end.
  std::unique_ptr<LineString> valid_tail_distinct =
      MakeLineString({{0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {1.0, 1.0}});
  ExpectValid(valid_tail_distinct.get());

  // LineString with XYZM coordinates: NaN Z/M is valid, NaN X/Y is invalid.
  CoordinateSequence line_xyzm_valid{
      CoordinateXYZM(0.0, 0.0, kNaN, kNaN),
      CoordinateXYZM(2.0, 3.0, kNaN, kNaN),
  };
  std::unique_ptr<LineString> valid_xyzm_line =
      factory_->createLineString(line_xyzm_valid);
  ExpectValid(valid_xyzm_line.get());

  CoordinateSequence line_xyzm_bad_y{
      CoordinateXYZM(0.0, 0.0, 1.0, 2.0),
      CoordinateXYZM(2.0, kNaN, 1.0, 2.0),
  };
  std::unique_ptr<LineString> invalid_xyzm_line =
      factory_->createLineString(line_xyzm_bad_y);
  ExpectInvalidNonFiniteCoord(invalid_xyzm_line.get());
}

TEST_F(IsValidOpTest, LinearRingValidation) {
  std::unique_ptr<LinearRing> valid_triangle =
      MakeRing({{0, 0}, {10, 0}, {5, 10}, {0, 0}});
  ExpectValid(valid_triangle.get());

  std::unique_ptr<LinearRing> valid_with_repeated =
      MakeRing({{0, 0}, {0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}});
  ExpectValid(valid_with_repeated.get());

  std::unique_ptr<LinearRing> nan_ring =
      MakeRing({{0, 0}, {kNaN, 1}, {1, 1}, {0, 0}});
  ExpectInvalidNonFiniteCoord(nan_ring.get());

  std::unique_ptr<LinearRing> inf_ring =
      MakeRing({{0, 0}, {1, 0}, {1, -kInf}, {0, 0}});
  ExpectInvalidNonFiniteCoord(inf_ring.get());

  // Unclosed ring constructed via setPoints().
  std::unique_ptr<LinearRing> unclosed_ring = factory_->createLinearRing();
  const CoordinateSequence unclosed_seq =
      MakeCoords({{2, 3}, {10, 3}, {10, 10}, {2, 10}});
  unclosed_ring->setPoints(&unclosed_seq);
  unclosed_ring->geometryChanged();
  ExpectInvalid(unclosed_ring.get(), TopologyValidationError::eRingNotClosed,
                2.0, 3.0);

  // Collapsed 3-point closed ring (allowed by constructor, invalid in
  // IsValidOp).
  std::unique_ptr<LinearRing> three_pt_ring =
      MakeRing({{1, 2}, {4, 5}, {1, 2}});
  ExpectInvalid(three_pt_ring.get(), TopologyValidationError::eTooFewPoints,
                1.0, 2.0);

  // 4-point closed ring with repeated vertex -> only 3 distinct vertices.
  std::unique_ptr<LinearRing> repeated_three_pt_ring =
      MakeRing({{1, 2}, {1, 2}, {4, 5}, {1, 2}});
  ExpectInvalid(repeated_three_pt_ring.get(),
                TopologyValidationError::eTooFewPoints, 1.0, 2.0);

  // 1-point ring via setPoints() is closed (front == back) but too small.
  std::unique_ptr<LinearRing> one_pt_ring = factory_->createLinearRing();
  const CoordinateSequence one_pt_seq = MakeCoords({{7, 8}});
  one_pt_ring->setPoints(&one_pt_seq);
  one_pt_ring->geometryChanged();
  ExpectInvalid(one_pt_ring.get(), TopologyValidationError::eTooFewPoints, 7.0,
                8.0);

  // Self-crossing figure-8 LinearRing.
  std::unique_ptr<LinearRing> bowtie_ring =
      MakeRing({{0, 0}, {10, 10}, {10, 0}, {0, 10}, {0, 0}});
  ExpectInvalid(bowtie_ring.get(),
                TopologyValidationError::eRingSelfIntersection, 5.0, 5.0);

  // Self-touching LinearRing is invalid even when inverted-ring flag is true.
  std::unique_ptr<LinearRing> self_touch_ring = MakeRing({{0, 0},
                                                          {5, 0},
                                                          {3, 4},
                                                          {7, 4},
                                                          {5, 0},
                                                          {10, 0},
                                                          {10, 10},
                                                          {0, 10},
                                                          {0, 0}});
  ExpectInvalid(self_touch_ring.get(),
                TopologyValidationError::eRingSelfIntersection, 5.0, 0.0,
                /*self_touching_valid=*/false);
  ExpectInvalid(self_touch_ring.get(),
                TopologyValidationError::eRingSelfIntersection, 5.0, 0.0,
                /*self_touching_valid=*/true);
}

TEST_F(IsValidOpTest, ValidPolygons) {
  constexpr auto kValidPolygons = std::to_array<WktValidationCase>({
      {.name = "ccw_square", .wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))"},
      {.name = "cw_square", .wkt = "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))"},
      {.name = "single_hole",
       .wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
              "(2 2, 2 8, 8 8, 8 2, 2 2))"},
      {.name = "multiple_disjoint_holes",
       .wkt = "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
              "(2 2, 2 6, 6 6, 6 2, 2 2), "
              "(12 12, 12 16, 16 16, 16 12, 12 12))"},
      {.name = "hole_touches_shell_at_one_vertex_start_interior",
       .wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
              "(2 4, 8 4, 5 0, 2 4))"},
      {.name = "hole_touches_shell_at_start_vertex",
       .wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
              "(5 0, 2 4, 8 4, 5 0))"},
      {.name = "two_holes_touch_at_one_vertex",
       .wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
              "(2 2, 5 5, 2 8, 2 2), "
              "(5 5, 8 2, 8 8, 5 5))"},
      {.name = "repeated_consecutive_vertices",
       .wkt = "POLYGON ((0 0, 0 0, 10 0, 10 10, 0 10, 0 0), "
              "(2 2, 2 2, 2 8, 8 8, 8 2, 2 2))"},
  });

  for (const auto& tc : kValidPolygons) {
    SCOPED_TRACE(tc.name);
    std::unique_ptr<Geometry> poly = ReadWkt(tc.wkt);
    ExpectValid(poly.get());
  }

  // Polygon with a non-empty shell and an empty hole ring.
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(factory_->createLinearRing());
  std::unique_ptr<Polygon> poly_with_empty_hole = MakePolygon(
      {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}}, std::move(holes));
  ExpectValid(poly_with_empty_hole.get());

  // Polygon with XYZM coordinates where Z and M are NaN.
  CoordinateSequence xyzm_seq{
      CoordinateXYZM(0, 0, kNaN, kNaN), CoordinateXYZM(4, 0, kNaN, kNaN),
      CoordinateXYZM(4, 4, kNaN, kNaN), CoordinateXYZM(0, 4, kNaN, kNaN),
      CoordinateXYZM(0, 0, kNaN, kNaN)};
  std::unique_ptr<Polygon> poly_xyzm =
      factory_->createPolygon(factory_->createLinearRing(xyzm_seq));
  ExpectValid(poly_xyzm.get());
}

TEST_F(IsValidOpTest, InvalidPolygonBasicChecks) {
  std::unique_ptr<Polygon> shell_nan =
      MakePolygon({{0, 0}, {kNaN, 0}, {10, 10}, {0, 10}, {0, 0}});
  ExpectInvalidNonFiniteCoord(shell_nan.get());

  std::vector<std::unique_ptr<LinearRing>> hole_inf;
  hole_inf.push_back(MakeRing({{2, 2}, {kInf, 2}, {8, 8}, {2, 8}, {2, 2}}));
  std::unique_ptr<Polygon> poly_hole_inf = MakePolygon(
      {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}}, std::move(hole_inf));
  ExpectInvalidNonFiniteCoord(poly_hole_inf.get());

  // Unclosed shell via setPoints().
  std::unique_ptr<Polygon> unclosed_shell =
      MakePolygon({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}});
  const CoordinateSequence open_seq =
      MakeCoords({{1, 1}, {10, 1}, {10, 10}, {1, 10}});
  unclosed_shell->getExteriorRing()->setPoints(&open_seq);
  unclosed_shell->geometryChanged();
  ExpectInvalid(unclosed_shell.get(), TopologyValidationError::eRingNotClosed,
                1.0, 1.0);

  // Unclosed hole via setPoints().
  std::vector<std::unique_ptr<LinearRing>> hole_to_unclose;
  hole_to_unclose.push_back(MakeRing({{2, 2}, {8, 2}, {8, 8}, {2, 8}, {2, 2}}));
  std::unique_ptr<Polygon> unclosed_hole = MakePolygon(
      {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}}, std::move(hole_to_unclose));
  const CoordinateSequence open_hole_seq =
      MakeCoords({{2, 2}, {8, 2}, {8, 8}, {2, 8}});
  unclosed_hole->getInteriorRingN(0)->setPoints(&open_hole_seq);
  unclosed_hole->geometryChanged();
  ExpectInvalid(unclosed_hole.get(), TopologyValidationError::eRingNotClosed,
                2.0, 2.0);

  // Too few non-repeated points in shell.
  std::unique_ptr<Polygon> small_shell = MakePolygon({{1, 1}, {5, 5}, {1, 1}});
  ExpectInvalid(small_shell.get(), TopologyValidationError::eTooFewPoints, 1.0,
                1.0);

  // Too few non-repeated points in hole.
  std::vector<std::unique_ptr<LinearRing>> small_hole;
  small_hole.push_back(MakeRing({{3, 3}, {3, 3}, {6, 6}, {3, 3}}));
  std::unique_ptr<Polygon> poly_small_hole = MakePolygon(
      {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}}, std::move(small_hole));
  ExpectInvalid(poly_small_hole.get(), TopologyValidationError::eTooFewPoints,
                3.0, 3.0);

  // Valid first hole followed by second hole with non-finite coordinate.
  std::vector<std::unique_ptr<LinearRing>> two_holes_nan;
  two_holes_nan.push_back(MakeRing({{1, 1}, {4, 1}, {4, 4}, {1, 4}, {1, 1}}));
  two_holes_nan.push_back(
      MakeRing({{6, 6}, {kNaN, 6}, {8, 8}, {6, 8}, {6, 6}}));
  std::unique_ptr<Polygon> poly_second_hole_nan = MakePolygon(
      {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}}, std::move(two_holes_nan));
  ExpectInvalidNonFiniteCoord(poly_second_hole_nan.get());

  // Valid first hole followed by unclosed second hole.
  std::vector<std::unique_ptr<LinearRing>> two_holes_unclosed;
  two_holes_unclosed.push_back(
      MakeRing({{1, 1}, {4, 1}, {4, 4}, {1, 4}, {1, 1}}));
  two_holes_unclosed.push_back(
      MakeRing({{6, 6}, {9, 6}, {9, 9}, {6, 9}, {6, 6}}));
  std::unique_ptr<Polygon> poly_second_hole_unclosed =
      MakePolygon({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}},
                  std::move(two_holes_unclosed));
  const CoordinateSequence open_hole1_seq =
      MakeCoords({{6, 6}, {9, 6}, {9, 9}, {6, 9}});
  poly_second_hole_unclosed->getInteriorRingN(1)->setPoints(&open_hole1_seq);
  poly_second_hole_unclosed->geometryChanged();
  ExpectInvalid(poly_second_hole_unclosed.get(),
                TopologyValidationError::eRingNotClosed, 6.0, 6.0);

  // Valid first hole followed by second hole with too few distinct points.
  std::vector<std::unique_ptr<LinearRing>> two_holes_small;
  two_holes_small.push_back(MakeRing({{1, 1}, {4, 1}, {4, 4}, {1, 4}, {1, 1}}));
  two_holes_small.push_back(MakeRing({{6, 6}, {6, 6}, {8, 8}, {6, 6}}));
  std::unique_ptr<Polygon> poly_second_hole_small = MakePolygon(
      {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}}, std::move(two_holes_small));
  ExpectInvalid(poly_second_hole_small.get(),
                TopologyValidationError::eTooFewPoints, 6.0, 6.0);

  // Empty hole, valid hole, and outside hole exercises checkHolesInShell loop.
  std::vector<std::unique_ptr<LinearRing>> mixed_holes;
  mixed_holes.push_back(factory_->createLinearRing());
  mixed_holes.push_back(MakeRing({{2, 2}, {8, 2}, {8, 8}, {2, 8}, {2, 2}}));
  mixed_holes.push_back(
      MakeRing({{20, 20}, {24, 20}, {24, 24}, {20, 24}, {20, 20}}));
  std::unique_ptr<Polygon> poly_mixed_holes = MakePolygon(
      {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}}, std::move(mixed_holes));
  ExpectInvalid(poly_mixed_holes.get(),
                TopologyValidationError::eHoleOutsideShell, 20.0, 20.0);
}

TEST_F(IsValidOpTest, InvalidPolygonDeterministicPointErrors) {
  constexpr auto kDeterministicPolygons = std::to_array<WktInvalidCase>({
      {.name = "bowtie_shell_self_intersection",
       .wkt = "POLYGON ((0 0, 10 10, 10 0, 0 10, 0 0))",
       .error_type = TopologyValidationError::eSelfIntersection,
       .x = 5.0,
       .y = 5.0},
      {.name = "hole_disjoint_outside_shell",
       .wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
              "(20 20, 25 20, 25 25, 20 25, 20 20))",
       .error_type = TopologyValidationError::eHoleOutsideShell,
       .x = 20.0,
       .y = 20.0},
      {.name = "hole_in_shell_concavity_inside_envelope",
       .wkt = "POLYGON ((0 0, 10 0, 10 5, 5 5, 5 10, 0 10, 0 0), "
              "(6 6, 9 6, 9 9, 6 9, 6 6))",
       .error_type = TopologyValidationError::eHoleOutsideShell,
       .x = 6.0,
       .y = 6.0},
      {.name = "hole_touches_concave_shell_from_outside",
       .wkt = "POLYGON ((0 0, 10 0, 10 5, 5 5, 5 10, 0 10, 0 0), "
              "(5 5, 9 6, 9 9, 6 9, 5 5))",
       .error_type = TopologyValidationError::eHoleOutsideShell,
       .x = 5.0,
       .y = 5.0},
      {.name = "nested_holes",
       .wkt = "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
              "(2 2, 18 2, 18 18, 2 18, 2 2), "
              "(5 5, 15 5, 15 15, 5 15, 5 5))",
       .error_type = TopologyValidationError::eNestedHoles,
       .x = 5.0,
       .y = 5.0},
      {.name = "nested_holes_sharing_vertex",
       .wkt = "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
              "(2 2, 18 2, 18 18, 2 18, 2 2), "
              "(2 2, 10 5, 5 10, 2 2))",
       .error_type = TopologyValidationError::eNestedHoles,
       .x = 2.0,
       .y = 2.0},
  });

  for (const auto& tc : kDeterministicPolygons) {
    SCOPED_TRACE(tc.name);
    std::unique_ptr<Geometry> poly = ReadWkt(tc.wkt);
    ExpectInvalid(poly.get(), tc.error_type, tc.x, tc.y);
  }
}

TEST_F(IsValidOpTest, InvalidPolygonMultiIntersectionAndCycleErrors) {
  {
    SCOPED_TRACE("collinear_spike_on_shell");
    std::unique_ptr<Geometry> poly =
        ReadWkt("POLYGON ((0 0, 10 0, 10 10, 5 10, 5 5, 5 10, 0 10, 0 0))");
    // Under default OGC rules, the spike base at (5, 10) is flagged first as a
    // non-adjacent ring self-touch.
    ExpectInvalid(poly.get(), TopologyValidationError::eRingSelfIntersection,
                  5.0, 10.0, /*self_touching_valid=*/false);
    // When self-touching rings are permitted, the collinear fold reports
    // eSelfIntersection.
    ExpectInvalidWithCandidateCoords(
        poly.get(), TopologyValidationError::eSelfIntersection,
        {{5.0, 5.0}, {5.0, 10.0}}, /*self_touching_valid=*/true);
  }
  {
    SCOPED_TRACE("collinear_overlap_on_shell");
    std::unique_ptr<Geometry> poly =
        ReadWkt("POLYGON ((0 0, 10 0, 10 10, 2 10, 6 10, 0 10, 0 0))");
    ExpectInvalidWithCandidateCoords(poly.get(),
                                     TopologyValidationError::eSelfIntersection,
                                     {{2.0, 10.0}, {6.0, 10.0}});
  }
  {
    SCOPED_TRACE("hole_crosses_shell");
    std::unique_ptr<Geometry> poly = ReadWkt(
        "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
        "(5 5, 15 5, 15 15, 5 15, 5 5))");
    ExpectInvalidWithCandidateCoords(poly.get(),
                                     TopologyValidationError::eSelfIntersection,
                                     {{10.0, 5.0}, {5.0, 10.0}});
  }
  {
    SCOPED_TRACE("hole_shares_edge_with_shell");
    std::unique_ptr<Geometry> poly = ReadWkt(
        "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
        "(2 0, 8 0, 8 4, 2 4, 2 0))");
    ExpectInvalidWithCandidateCoords(poly.get(),
                                     TopologyValidationError::eSelfIntersection,
                                     {{2.0, 0.0}, {8.0, 0.0}});
  }
  {
    SCOPED_TRACE("two_holes_cross");
    std::unique_ptr<Geometry> poly = ReadWkt(
        "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
        "(2 2, 10 2, 10 10, 2 10, 2 2), "
        "(6 6, 14 6, 14 14, 6 14, 6 6))");
    ExpectInvalidWithCandidateCoords(poly.get(),
                                     TopologyValidationError::eSelfIntersection,
                                     {{10.0, 6.0}, {6.0, 10.0}});
  }
  {
    SCOPED_TRACE("duplicate_holes");
    std::unique_ptr<Geometry> poly = ReadWkt(
        "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
        "(2 2, 8 2, 8 8, 2 8, 2 2), "
        "(2 2, 8 2, 8 8, 2 8, 2 2))");
    ExpectInvalidWithCandidateCoords(
        poly.get(), TopologyValidationError::eSelfIntersection,
        {{2.0, 2.0}, {8.0, 2.0}, {8.0, 8.0}, {2.0, 8.0}});
  }
  {
    SCOPED_TRACE("hole_touches_shell_twice_disconnects_interior");
    std::unique_ptr<Geometry> poly = ReadWkt(
        "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
        "(5 0, 8 5, 5 10, 2 5, 5 0))");
    ExpectInvalidWithCandidateCoords(
        poly.get(), TopologyValidationError::eDisconnectedInterior,
        {{5.0, 0.0}, {5.0, 10.0}});
  }
  {
    SCOPED_TRACE("two_holes_touch_twice_disconnect_interior");
    std::unique_ptr<Geometry> poly = ReadWkt(
        "POLYGON ((0 0, 12 0, 12 12, 0 12, 0 0), "
        "(6 2, 3 6, 6 10, 4 6, 6 2), "
        "(6 2, 8 6, 6 10, 9 6, 6 2))");
    ExpectInvalidWithCandidateCoords(
        poly.get(), TopologyValidationError::eDisconnectedInterior,
        {{6.0, 2.0}, {6.0, 10.0}});
  }
  {
    SCOPED_TRACE("three_hole_cycle_disconnects_interior");
    std::unique_ptr<Geometry> poly = ReadWkt(
        "POLYGON ((0 0, 12 0, 12 12, 0 12, 0 0), "
        "(4 4, 8 4, 6 2, 4 4), "
        "(8 4, 6 8, 9 6, 8 4), "
        "(6 8, 4 4, 3 6, 6 8))");
    ExpectInvalidWithCandidateCoords(
        poly.get(), TopologyValidationError::eDisconnectedInterior,
        {{4.0, 4.0}, {8.0, 4.0}, {6.0, 8.0}});
  }
  {
    SCOPED_TRACE("shell_and_two_holes_cycle_disconnects_interior");
    std::unique_ptr<Geometry> poly = ReadWkt(
        "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
        "(5 0, 4 4, 2 2, 5 0), "
        "(4 4, 6 6, 5 10, 8 5, 4 4))");
    ExpectInvalidWithCandidateCoords(
        poly.get(), TopologyValidationError::eDisconnectedInterior,
        {{5.0, 0.0}, {4.0, 4.0}, {5.0, 10.0}});
  }
}

TEST_F(IsValidOpTest, SelfTouchingRingFormingHoleFlag) {
  // Inverted shell forming an interior hole while keeping interior connected.
  std::unique_ptr<Geometry> inverted_shell =
      ReadWkt("POLYGON ((0 0, 5 0, 3 4, 7 4, 5 0, 10 0, 10 10, 0 10, 0 0))");
  ExpectInvalid(inverted_shell.get(),
                TopologyValidationError::eRingSelfIntersection, 5.0, 0.0,
                /*self_touching_valid=*/false);
  ExpectValid(inverted_shell.get(), /*self_touching_valid=*/true);

  // Hole ring self-touching to form two touching holes with connected interior.
  std::unique_ptr<Geometry> self_touch_hole = ReadWkt(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 2 8, 5 5, 8 8, 8 2, 5 5, 2 2))");
  ExpectInvalid(self_touch_hole.get(),
                TopologyValidationError::eRingSelfIntersection, 5.0, 5.0,
                /*self_touching_valid=*/false);
  ExpectValid(self_touch_hole.get(), /*self_touching_valid=*/true);

  // Exverted bow-tie shell touching at a single vertex disconnects interior.
  std::unique_ptr<Geometry> exverted_shell =
      ReadWkt("POLYGON ((0 0, 5 0, 5 5, 10 5, 10 10, 5 10, 5 5, 0 5, 0 0))");
  ExpectInvalid(exverted_shell.get(),
                TopologyValidationError::eRingSelfIntersection, 5.0, 5.0,
                /*self_touching_valid=*/false);
  ExpectInvalid(exverted_shell.get(),
                TopologyValidationError::eDisconnectedInterior, 5.0, 5.0,
                /*self_touching_valid=*/true);

  // Inverted C-shaped hole touching at a single vertex creates an island.
  std::unique_ptr<Geometry> inverted_hole = ReadWkt(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 5 2, 4 6, 6 6, 5 2, 8 2, 8 8, 2 8, 2 2))");
  ExpectInvalid(inverted_hole.get(),
                TopologyValidationError::eRingSelfIntersection, 5.0, 2.0,
                /*self_touching_valid=*/false);
  ExpectInvalid(inverted_hole.get(),
                TopologyValidationError::eDisconnectedInterior, 5.0, 2.0,
                /*self_touching_valid=*/true);

  // Inverted shell whose inversion touches the opposite side of the shell.
  std::unique_ptr<Geometry> inverted_shell_double_touch = ReadWkt(
      "POLYGON ((0 0, 5 0, 3 5, 5 10, 7 5, 5 0, 10 0, 10 10, 0 10, 0 0))");
  ExpectInvalidWithCandidateCoords(
      inverted_shell_double_touch.get(),
      TopologyValidationError::eRingSelfIntersection, {{5.0, 0.0}, {5.0, 10.0}},
      /*self_touching_valid=*/false);
  ExpectInvalidWithCandidateCoords(
      inverted_shell_double_touch.get(),
      TopologyValidationError::eDisconnectedInterior, {{5.0, 0.0}, {5.0, 10.0}},
      /*self_touching_valid=*/true);

  // Calling getValidationError() directly before isValid() triggers validation.
  IsValidOp direct_invalid(inverted_shell.get());
  ASSERT_NE(direct_invalid.getValidationError(), nullptr);
  EXPECT_EQ(direct_invalid.getValidationError()->getErrorType(),
            TopologyValidationError::eRingSelfIntersection);

  std::unique_ptr<Geometry> valid_square =
      ReadWkt("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  IsValidOp direct_valid(valid_square.get());
  EXPECT_EQ(direct_valid.getValidationError(), nullptr);
  EXPECT_TRUE(direct_valid.isValid());

  // Reusing the same IsValidOp instance clears validErr on re-evaluation.
  IsValidOp reusable(inverted_shell.get());
  EXPECT_FALSE(reusable.isValid());
  ASSERT_NE(reusable.getValidationError(), nullptr);
  EXPECT_EQ(reusable.getValidationError()->getErrorType(),
            TopologyValidationError::eRingSelfIntersection);

  reusable.setSelfTouchingRingFormingHoleValid(true);
  EXPECT_TRUE(reusable.isValid());
  EXPECT_EQ(reusable.getValidationError(), nullptr);

  reusable.setSelfTouchingRingFormingHoleValid(false);
  ASSERT_NE(reusable.getValidationError(), nullptr);
  EXPECT_EQ(reusable.getValidationError()->getErrorType(),
            TopologyValidationError::eRingSelfIntersection);
}

TEST_F(IsValidOpTest, ValidMultiPolygons) {
  constexpr auto kValidMultiPolygons = std::to_array<WktValidationCase>({
      {.name = "single_polygon",
       .wkt = "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)))"},
      {.name = "empty_and_nonempty_polygon",
       .wkt = "MULTIPOLYGON (EMPTY, ((0 0, 4 0, 4 4, 0 4, 0 0)))"},
      {.name = "nonempty_and_empty_polygon",
       .wkt = "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), EMPTY)"},
      {.name = "disjoint_polygons",
       .wkt = "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
              "((10 10, 14 10, 14 14, 10 14, 10 10)))"},
      {.name = "touch_at_single_vertex",
       .wkt = "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
              "((4 4, 8 4, 8 8, 4 8, 4 4)))"},
      {.name = "touch_at_two_discrete_vertices",
       .wkt = "MULTIPOLYGON ("
              "((0 0, 4 0, 4 1, 2 1, 2 3, 4 3, 4 4, 0 4, 0 0)), "
              "((4 1, 6 1, 6 3, 4 3, 8 4, 8 0, 4 1)))"},
      {.name = "island_inside_hole",
       .wkt = "MULTIPOLYGON ("
              "((0 0, 20 0, 20 20, 0 20, 0 0), "
              "(2 2, 18 2, 18 18, 2 18, 2 2)), "
              "((5 5, 15 5, 15 15, 5 15, 5 5)))"},
      {.name = "island_touching_hole_boundary_at_vertices",
       .wkt = "MULTIPOLYGON ("
              "((0 0, 20 0, 20 20, 0 20, 0 0), "
              "(2 2, 18 2, 18 18, 2 18, 2 2)), "
              "((10 2, 18 10, 10 18, 2 10, 10 2)))"},
  });

  for (const auto& tc : kValidMultiPolygons) {
    SCOPED_TRACE(tc.name);
    std::unique_ptr<Geometry> mp = ReadWkt(tc.wkt);
    ExpectValid(mp.get());
  }
}

TEST_F(IsValidOpTest, InvalidMultiPolygonsBasicChecksAndEmptyShellHole) {
  // Invalid coordinate in second polygon.
  std::vector<std::unique_ptr<Polygon>> polys_nan;
  polys_nan.push_back(MakePolygon({{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 0}}));
  polys_nan.push_back(
      MakePolygon({{10, 10}, {kNaN, 10}, {14, 14}, {10, 14}, {10, 10}}));
  std::unique_ptr<MultiPolygon> mp_nan =
      factory_->createMultiPolygon(std::move(polys_nan));
  ExpectInvalidNonFiniteCoord(mp_nan.get());

  // Unclosed ring in second polygon.
  std::vector<std::unique_ptr<Polygon>> polys_unclosed;
  polys_unclosed.push_back(
      MakePolygon({{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 0}}));
  std::unique_ptr<Polygon> p_unclosed =
      MakePolygon({{10, 10}, {14, 10}, {14, 14}, {10, 14}, {10, 10}});
  const CoordinateSequence open_seq =
      MakeCoords({{10, 10}, {14, 10}, {14, 14}, {10, 14}});
  p_unclosed->getExteriorRing()->setPoints(&open_seq);
  p_unclosed->geometryChanged();
  polys_unclosed.push_back(std::move(p_unclosed));
  std::unique_ptr<MultiPolygon> mp_unclosed =
      factory_->createMultiPolygon(std::move(polys_unclosed));
  ExpectInvalid(mp_unclosed.get(), TopologyValidationError::eRingNotClosed,
                10.0, 10.0);

  // Too few points in second polygon.
  std::vector<std::unique_ptr<Polygon>> polys_small;
  polys_small.push_back(MakePolygon({{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 0}}));
  polys_small.push_back(MakePolygon({{10, 10}, {12, 12}, {10, 10}}));
  std::unique_ptr<MultiPolygon> mp_small =
      factory_->createMultiPolygon(std::move(polys_small));
  ExpectInvalid(mp_small.get(), TopologyValidationError::eTooFewPoints, 10.0,
                10.0);

  // MultiPolygon where second polygon has an empty shell and a non-empty hole
  // (exercises the isShellEmpty branch of checkHolesInShell).
  std::vector<std::unique_ptr<Polygon>> polys_empty_shell_hole;
  polys_empty_shell_hole.push_back(
      MakePolygon({{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 0}}));
  std::vector<std::unique_ptr<LinearRing>> kept_holes;
  kept_holes.push_back(
      MakeRing({{12, 12}, {18, 12}, {18, 18}, {12, 18}, {12, 12}}));
  std::unique_ptr<Polygon> p_empty_shell =
      MakePolygon({{10, 10}, {20, 10}, {20, 20}, {10, 20}, {10, 10}},
                  std::move(kept_holes));
  CoordinateSequence empty_seq(0u, false, false);
  p_empty_shell->getExteriorRing()->setPoints(&empty_seq);
  p_empty_shell->geometryChanged();
  polys_empty_shell_hole.push_back(std::move(p_empty_shell));
  std::unique_ptr<MultiPolygon> mp_empty_shell_hole =
      factory_->createMultiPolygon(std::move(polys_empty_shell_hole));
  ExpectInvalid(mp_empty_shell_hole.get(),
                TopologyValidationError::eHoleOutsideShell, 12.0, 12.0);

  // Invalid coordinate in a hole of the second polygon.
  std::vector<std::unique_ptr<Polygon>> polys_hole_nan;
  polys_hole_nan.push_back(
      MakePolygon({{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 0}}));
  std::vector<std::unique_ptr<LinearRing>> hole_nan;
  hole_nan.push_back(
      MakeRing({{12, 12}, {kNaN, 12}, {18, 18}, {12, 18}, {12, 12}}));
  polys_hole_nan.push_back(MakePolygon(
      {{10, 10}, {20, 10}, {20, 20}, {10, 20}, {10, 10}}, std::move(hole_nan)));
  std::unique_ptr<MultiPolygon> mp_hole_nan =
      factory_->createMultiPolygon(std::move(polys_hole_nan));
  ExpectInvalidNonFiniteCoord(mp_hole_nan.get());

  // Unclosed hole in the second polygon.
  std::vector<std::unique_ptr<Polygon>> polys_hole_unclosed;
  polys_hole_unclosed.push_back(
      MakePolygon({{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 0}}));
  std::vector<std::unique_ptr<LinearRing>> hole_unclosed_vec;
  hole_unclosed_vec.push_back(
      MakeRing({{12, 12}, {18, 12}, {18, 18}, {12, 18}, {12, 12}}));
  std::unique_ptr<Polygon> p_hole_unclosed =
      MakePolygon({{10, 10}, {20, 10}, {20, 20}, {10, 20}, {10, 10}},
                  std::move(hole_unclosed_vec));
  const CoordinateSequence open_mp_hole_seq =
      MakeCoords({{12, 12}, {18, 12}, {18, 18}, {12, 18}});
  p_hole_unclosed->getInteriorRingN(0)->setPoints(&open_mp_hole_seq);
  p_hole_unclosed->geometryChanged();
  polys_hole_unclosed.push_back(std::move(p_hole_unclosed));
  std::unique_ptr<MultiPolygon> mp_hole_unclosed =
      factory_->createMultiPolygon(std::move(polys_hole_unclosed));
  ExpectInvalid(mp_hole_unclosed.get(), TopologyValidationError::eRingNotClosed,
                12.0, 12.0);

  // Too few distinct points in a hole of the second polygon.
  std::vector<std::unique_ptr<Polygon>> polys_hole_small;
  polys_hole_small.push_back(
      MakePolygon({{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 0}}));
  std::vector<std::unique_ptr<LinearRing>> hole_small_vec;
  hole_small_vec.push_back(MakeRing({{12, 12}, {12, 12}, {16, 16}, {12, 12}}));
  polys_hole_small.push_back(
      MakePolygon({{10, 10}, {20, 10}, {20, 20}, {10, 20}, {10, 10}},
                  std::move(hole_small_vec)));
  std::unique_ptr<MultiPolygon> mp_hole_small =
      factory_->createMultiPolygon(std::move(polys_hole_small));
  ExpectInvalid(mp_hole_small.get(), TopologyValidationError::eTooFewPoints,
                12.0, 12.0);
}

TEST_F(IsValidOpTest, InvalidMultiPolygonTopologyErrors) {
  constexpr auto kDeterministicMultiPolygons = std::to_array<WktInvalidCase>({
      {.name = "hole_outside_shell_in_second_polygon",
       .wkt = "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
              "((10 10, 20 10, 20 20, 10 20, 10 10), "
              "(30 30, 35 30, 35 35, 30 35, 30 30)))",
       .error_type = TopologyValidationError::eHoleOutsideShell,
       .x = 30.0,
       .y = 30.0},
      {.name = "nested_holes_in_second_polygon",
       .wkt = "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
              "((10 10, 30 10, 30 30, 10 30, 10 10), "
              "(12 12, 28 12, 28 28, 12 28, 12 12), "
              "(15 15, 25 15, 25 25, 15 25, 15 15)))",
       .error_type = TopologyValidationError::eNestedHoles,
       .x = 15.0,
       .y = 15.0},
      {.name = "nested_shells_second_in_first",
       .wkt = "MULTIPOLYGON (((0 0, 20 0, 20 20, 0 20, 0 0)), "
              "((4 4, 16 4, 16 16, 4 16, 4 4)))",
       .error_type = TopologyValidationError::eNestedShells,
       .x = 4.0,
       .y = 4.0},
      {.name = "nested_shells_first_in_second",
       .wkt = "MULTIPOLYGON (((4 4, 16 4, 16 16, 4 16, 4 4)), "
              "((0 0, 20 0, 20 20, 0 20, 0 0)))",
       .error_type = TopologyValidationError::eNestedShells,
       .x = 4.0,
       .y = 4.0},
      {.name = "nested_shells_touching_at_vertices",
       .wkt = "MULTIPOLYGON (((0 0, 20 0, 20 20, 0 20, 0 0)), "
              "((10 0, 20 10, 10 20, 0 10, 10 0)))",
       .error_type = TopologyValidationError::eNestedShells,
       .x = 10.0,
       .y = 0.0},
      {.name = "nested_shell_outside_hole_of_outer",
       .wkt = "MULTIPOLYGON ("
              "((0 0, 20 0, 20 20, 0 20, 0 0), "
              "(2 2, 6 2, 6 6, 2 6, 2 2)), "
              "((10 10, 16 10, 16 16, 10 16, 10 10)))",
       .error_type = TopologyValidationError::eNestedShells,
       .x = 10.0,
       .y = 10.0},
  });

  for (const auto& tc : kDeterministicMultiPolygons) {
    SCOPED_TRACE(tc.name);
    std::unique_ptr<Geometry> mp = ReadWkt(tc.wkt);
    ExpectInvalid(mp.get(), tc.error_type, tc.x, tc.y);
  }

  {
    SCOPED_TRACE("overlapping_shells");
    std::unique_ptr<Geometry> mp = ReadWkt(
        "MULTIPOLYGON (((0 0, 6 0, 6 6, 0 6, 0 0)), "
        "((4 4, 10 4, 10 10, 4 10, 4 4)))");
    ExpectInvalidWithCandidateCoords(mp.get(),
                                     TopologyValidationError::eSelfIntersection,
                                     {{6.0, 4.0}, {4.0, 6.0}});
  }
  {
    SCOPED_TRACE("edge_sharing_shells");
    std::unique_ptr<Geometry> mp = ReadWkt(
        "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
        "((4 0, 8 0, 8 4, 4 4, 4 0)))");
    ExpectInvalidWithCandidateCoords(mp.get(),
                                     TopologyValidationError::eSelfIntersection,
                                     {{4.0, 0.0}, {4.0, 4.0}});
  }
  {
    SCOPED_TRACE("duplicate_polygons");
    std::unique_ptr<Geometry> mp = ReadWkt(
        "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
        "((0 0, 4 0, 4 4, 0 4, 0 0)))");
    ExpectInvalidWithCandidateCoords(
        mp.get(), TopologyValidationError::eSelfIntersection,
        {{0.0, 0.0}, {4.0, 0.0}, {4.0, 4.0}, {0.0, 4.0}});
  }
  {
    SCOPED_TRACE("disconnected_interior_in_second_polygon");
    std::unique_ptr<Geometry> mp = ReadWkt(
        "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
        "((10 10, 20 10, 20 20, 10 20, 10 10), "
        "(15 10, 18 15, 15 20, 12 15, 15 10)))");
    ExpectInvalidWithCandidateCoords(
        mp.get(), TopologyValidationError::eDisconnectedInterior,
        {{15.0, 10.0}, {15.0, 20.0}});
  }

  // Inverted shell inside a MultiPolygon respects self-touching flag.
  std::unique_ptr<Geometry> mp_inverted_shell = ReadWkt(
      "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "((10 0, 15 0, 13 4, 17 4, 15 0, 20 0, 20 10, 10 10, 10 0)))");
  ExpectInvalid(mp_inverted_shell.get(),
                TopologyValidationError::eRingSelfIntersection, 15.0, 0.0,
                /*self_touching_valid=*/false);
  ExpectValid(mp_inverted_shell.get(), /*self_touching_valid=*/true);

  // Exverted bow-tie shell inside a MultiPolygon still disconnects interior.
  std::unique_ptr<Geometry> mp_exverted_shell = ReadWkt(
      "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "((10 0, 15 0, 15 5, 20 5, 20 10, 15 10, 15 5, 10 5, 10 0)))");
  ExpectInvalid(mp_exverted_shell.get(),
                TopologyValidationError::eDisconnectedInterior, 15.0, 5.0,
                /*self_touching_valid=*/true);

  // Self-touching hole inside a MultiPolygon respects self-touching flag.
  std::unique_ptr<Geometry> mp_self_touch_hole = ReadWkt(
      "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "((10 0, 20 0, 20 10, 10 10, 10 0), "
      "(12 2, 12 8, 15 5, 18 8, 18 2, 15 5, 12 2)))");
  ExpectInvalid(mp_self_touch_hole.get(),
                TopologyValidationError::eRingSelfIntersection, 15.0, 5.0,
                /*self_touching_valid=*/false);
  ExpectValid(mp_self_touch_hole.get(), /*self_touching_valid=*/true);
}

TEST_F(IsValidOpTest, MultiLineStringAndGeometryCollection) {
  std::unique_ptr<Geometry> valid_mls =
      ReadWkt("MULTILINESTRING ((0 0, 10 10), (0 10, 10 0), EMPTY)");
  ExpectValid(valid_mls.get());

  std::unique_ptr<Geometry> invalid_mls =
      ReadWkt("MULTILINESTRING ((0 0, 10 10), (5 5, 5 5))");
  ExpectInvalid(invalid_mls.get(), TopologyValidationError::eTooFewPoints, 5.0,
                5.0);

  std::vector<std::unique_ptr<LineString>> mls_nan_lines;
  mls_nan_lines.push_back(MakeLineString({{0.0, 0.0}, {1.0, 1.0}}));
  mls_nan_lines.push_back(MakeLineString({{2.0, 2.0}, {kNaN, 3.0}}));
  std::unique_ptr<MultiLineString> mls_nan =
      factory_->createMultiLineString(std::move(mls_nan_lines));
  ExpectInvalidNonFiniteCoord(mls_nan.get());

  std::unique_ptr<Geometry> valid_gc = ReadWkt(
      "GEOMETRYCOLLECTION ("
      "POINT (1 1), "
      "LINESTRING (0 0, 2 2), "
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "GEOMETRYCOLLECTION (POINT EMPTY, MULTIPOINT ((3 3), (4 4))))");
  ExpectValid(valid_gc.get());

  // Invalid nested polygon inside a GeometryCollection.
  std::unique_ptr<Geometry> invalid_gc_nested = ReadWkt(
      "GEOMETRYCOLLECTION ("
      "POINT (1 1), "
      "GEOMETRYCOLLECTION ("
      "LINESTRING (0 0, 1 1), "
      "POLYGON ((0 0, 10 10, 10 0, 0 10, 0 0))))");
  ExpectInvalid(invalid_gc_nested.get(),
                TopologyValidationError::eSelfIntersection, 5.0, 5.0);

  // Invalid Point inside a GeometryCollection.
  std::vector<std::unique_ptr<Geometry>> geoms_bad_pt;
  geoms_bad_pt.push_back(factory_->createPoint(CoordinateXY(1.0, 2.0)));
  geoms_bad_pt.push_back(factory_->createPoint(CoordinateXY(kNaN, 2.0)));
  std::unique_ptr<GeometryCollection> gc_bad_pt =
      factory_->createGeometryCollection(std::move(geoms_bad_pt));
  ExpectInvalidNonFiniteCoord(gc_bad_pt.get());

  // GeometryCollection with empty element preceding an invalid LineString.
  std::unique_ptr<Geometry> gc_empty_then_invalid =
      ReadWkt("GEOMETRYCOLLECTION (POINT EMPTY, LINESTRING (7 7, 7 7))");
  ExpectInvalid(gc_empty_then_invalid.get(),
                TopologyValidationError::eTooFewPoints, 7.0, 7.0);

  // GeometryCollection containing a non-empty curved geometry throws.
  std::unique_ptr<Geometry> gc_with_curve = ReadWkt(
      "GEOMETRYCOLLECTION (POINT (0 0), CIRCULARSTRING (0 0, 1 1, 2 0))");
  EXPECT_THROW(IsValidOp::isValid(gc_with_curve.get()),
               UnsupportedOperationException);
}

}  // namespace
}  // namespace geos_google
