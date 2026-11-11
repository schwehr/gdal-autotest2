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
// third_party/geos/src/precision/PointwisePrecisionReducerTransformer.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/precision/PointwisePrecisionReducerTransformer.cpp

#include "third_party/geos/include/geos/precision/PointwisePrecisionReducerTransformer.h"

#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
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
using geos::precision::PointwisePrecisionReducerTransformer;
using geos::util::IllegalArgumentException;

class PointwisePrecisionReducerTransformerTest : public ::testing::Test {
 protected:
  PointwisePrecisionReducerTransformerTest()
      : factory_(GeometryFactory::create()), reader_(factory_.get()) {}

  std::unique_ptr<Geometry> ReadWkt(std::string_view wkt) const {
    return reader_.read(std::string(wkt));
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(PointwisePrecisionReducerTransformerTest, ReducesPointsAcrossModels) {
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
    std::unique_ptr<Geometry> input = ReadWkt(c.input_wkt);
    std::unique_ptr<Geometry> reduced =
        PointwisePrecisionReducerTransformer::reduce(*input, pm);
    ASSERT_NE(reduced, nullptr);
    EXPECT_EQ(reduced->getFactory(), factory_.get());
    const auto* pt = dynamic_cast<const Point*>(reduced.get());
    ASSERT_NE(pt, nullptr);
    EXPECT_NEAR(pt->getX(), c.expected_x, c.tolerance);
    EXPECT_NEAR(pt->getY(), c.expected_y, c.tolerance);
    EXPECT_FALSE(pt->hasZ());
    EXPECT_TRUE(std::isnan(pt->getZ()));
  }
}

TEST_F(PointwisePrecisionReducerTransformerTest,
       InstanceTransformMatchesStaticReduce) {
  PrecisionModel pm(2.0);
  std::unique_ptr<Geometry> input = ReadWkt("LINESTRING (0.1 0.2, 1.3 1.7)");

  PointwisePrecisionReducerTransformer transformer(pm);
  std::unique_ptr<Geometry> from_instance = transformer.transform(input.get());
  std::unique_ptr<Geometry> from_static =
      PointwisePrecisionReducerTransformer::reduce(*input, pm);

  ASSERT_NE(from_instance, nullptr);
  ASSERT_NE(from_static, nullptr);
  EXPECT_TRUE(from_instance->equalsIdentical(from_static.get()));
}

TEST_F(PointwisePrecisionReducerTransformerTest, PreservesEmptyGeometries) {
  struct EmptyCase {
    std::string_view wkt;
    GeometryTypeId type_id;
    bool expect_z;
  };

  constexpr auto kEmptyCases = std::to_array<EmptyCase>({
      {.wkt = "POINT EMPTY", .type_id = GEOS_POINT, .expect_z = false},
      {.wkt = "POINT Z EMPTY", .type_id = GEOS_POINT, .expect_z = true},
      {.wkt = "POINT ZM EMPTY", .type_id = GEOS_POINT, .expect_z = true},
      {.wkt = "LINESTRING EMPTY",
       .type_id = GEOS_LINESTRING,
       .expect_z = false},
      {.wkt = "LINEARRING EMPTY",
       .type_id = GEOS_LINEARRING,
       .expect_z = false},
      {.wkt = "MULTIPOINT EMPTY",
       .type_id = GEOS_MULTIPOINT,
       .expect_z = false},
      {.wkt = "MULTILINESTRING EMPTY",
       .type_id = GEOS_MULTILINESTRING,
       .expect_z = false},
      {.wkt = "MULTIPOLYGON EMPTY",
       .type_id = GEOS_MULTIPOLYGON,
       .expect_z = false},
      {.wkt = "GEOMETRYCOLLECTION EMPTY",
       .type_id = GEOS_GEOMETRYCOLLECTION,
       .expect_z = false},
      {.wkt = "GEOMETRYCOLLECTION (POINT EMPTY, LINESTRING EMPTY)",
       .type_id = GEOS_GEOMETRYCOLLECTION,
       .expect_z = false},
  });

  PrecisionModel pm(1.0);
  for (const EmptyCase& c : kEmptyCases) {
    SCOPED_TRACE(c.wkt);
    std::unique_ptr<Geometry> input = ReadWkt(c.wkt);
    std::unique_ptr<Geometry> reduced =
        PointwisePrecisionReducerTransformer::reduce(*input, pm);
    ASSERT_NE(reduced, nullptr);
    EXPECT_TRUE(reduced->isEmpty());
    EXPECT_EQ(reduced->getGeometryTypeId(), c.type_id);
    EXPECT_EQ(reduced->hasZ(), c.expect_z);
  }
}

// GeometryTransformer::transformPolygon treats an empty exterior ring as
// invalid and falls back to buildGeometry(), which unwraps the single empty
// LinearRing component.
TEST_F(PointwisePrecisionReducerTransformerTest,
       EmptyPolygonTransformsToEmptyLinearRing) {
  struct Case {
    std::string_view wkt;
    bool expect_z;
  };
  constexpr auto kCases = std::to_array<Case>({
      {.wkt = "POLYGON EMPTY", .expect_z = false},
      {.wkt = "POLYGON Z EMPTY", .expect_z = true},
  });

  PrecisionModel pm(1.0);
  for (const Case& c : kCases) {
    SCOPED_TRACE(c.wkt);
    std::unique_ptr<Geometry> input = ReadWkt(c.wkt);
    std::unique_ptr<Geometry> reduced =
        PointwisePrecisionReducerTransformer::reduce(*input, pm);
    ASSERT_NE(reduced, nullptr);
    EXPECT_TRUE(reduced->isEmpty());
    EXPECT_EQ(reduced->getGeometryTypeId(), GEOS_LINEARRING);
    EXPECT_EQ(reduced->hasZ(), c.expect_z);
  }
}

TEST_F(PointwisePrecisionReducerTransformerTest, PreservesZAndRoundsXY) {
  PrecisionModel pm(1.0);
  std::unique_ptr<Geometry> pt_in = ReadWkt("POINT Z (1.26 3.74 9.875)");
  std::unique_ptr<Geometry> pt_out =
      PointwisePrecisionReducerTransformer::reduce(*pt_in, pm);
  const auto* pt = dynamic_cast<const Point*>(pt_out.get());
  ASSERT_NE(pt, nullptr);
  EXPECT_TRUE(pt->hasZ());
  EXPECT_DOUBLE_EQ(pt->getX(), 1.0);
  EXPECT_DOUBLE_EQ(pt->getY(), 4.0);
  EXPECT_DOUBLE_EQ(pt->getZ(), 9.875);

  std::unique_ptr<Geometry> line_in =
      ReadWkt("LINESTRING Z (0.1 0.2 10.5, 1.9 2.8 20.25)");
  std::unique_ptr<Geometry> line_out =
      PointwisePrecisionReducerTransformer::reduce(*line_in, pm);
  std::unique_ptr<Geometry> expected =
      ReadWkt("LINESTRING Z (0 0 10.5, 2 3 20.25)");
  EXPECT_TRUE(line_out->equalsIdentical(expected.get()));
}

TEST_F(PointwisePrecisionReducerTransformerTest,
       LineStringPreservesDuplicatePointsAndCollapse) {
  PrecisionModel pm(1.0);

  std::unique_ptr<Geometry> input =
      ReadWkt("LINESTRING (0.1 0.1, 0.2 0.2, 1.4 1.4)");
  std::unique_ptr<Geometry> reduced =
      PointwisePrecisionReducerTransformer::reduce(*input, pm);
  std::unique_ptr<Geometry> expected = ReadWkt("LINESTRING (0 0, 0 0, 1 1)");
  EXPECT_EQ(reduced->getGeometryTypeId(), GEOS_LINESTRING);
  EXPECT_EQ(reduced->getNumPoints(), 3u);
  EXPECT_TRUE(reduced->equalsExact(expected.get()));

  std::unique_ptr<Geometry> collapsed_input =
      ReadWkt("LINESTRING (0.1 0.1, 0.2 0.2)");
  std::unique_ptr<Geometry> collapsed =
      PointwisePrecisionReducerTransformer::reduce(*collapsed_input, pm);
  const auto* line = dynamic_cast<const LineString*>(collapsed.get());
  ASSERT_NE(line, nullptr);
  EXPECT_FALSE(line->isEmpty());
  EXPECT_EQ(line->getNumPoints(), 2u);
  EXPECT_DOUBLE_EQ(line->getLength(), 0.0);
  const CoordinateSequence* seq = line->getCoordinatesRO();
  EXPECT_DOUBLE_EQ(seq->getX(0), 0.0);
  EXPECT_DOUBLE_EQ(seq->getY(0), 0.0);
  EXPECT_DOUBLE_EQ(seq->getX(1), 0.0);
  EXPECT_DOUBLE_EQ(seq->getY(1), 0.0);
}

TEST_F(PointwisePrecisionReducerTransformerTest,
       LinearRingReducesPointwiseWithoutRemovingDuplicates) {
  PrecisionModel pm(1.0);

  std::unique_ptr<Geometry> valid_in =
      ReadWkt("LINEARRING (0.1 0.1, 1.9 0.1, 1.9 1.9, 0.1 1.9, 0.1 0.1)");
  std::unique_ptr<Geometry> valid_out =
      PointwisePrecisionReducerTransformer::reduce(*valid_in, pm);
  std::unique_ptr<Geometry> valid_expected =
      ReadWkt("LINEARRING (0 0, 2 0, 2 2, 0 2, 0 0)");
  EXPECT_EQ(valid_out->getGeometryTypeId(), GEOS_LINEARRING);
  EXPECT_TRUE(valid_out->equalsExact(valid_expected.get()));

  std::unique_ptr<Geometry> collapsed_in =
      ReadWkt("LINEARRING (0.1 0.1, 0.2 0.1, 0.2 0.2, 0.1 0.2, 0.1 0.1)");
  std::unique_ptr<Geometry> collapsed_out =
      PointwisePrecisionReducerTransformer::reduce(*collapsed_in, pm);
  const auto* ring = dynamic_cast<const LinearRing*>(collapsed_out.get());
  ASSERT_NE(ring, nullptr);
  EXPECT_FALSE(ring->isEmpty());
  EXPECT_EQ(ring->getNumPoints(), 5u);
  EXPECT_FALSE(ring->isValid());
}

TEST_F(PointwisePrecisionReducerTransformerTest,
       PolygonAndMultiPolygonReducePointwise) {
  PrecisionModel pm(1.0);

  std::unique_ptr<Geometry> poly_in = ReadWkt(
      "POLYGON ((0.1 0.1, 4.1 0.1, 4.1 4.1, 0.1 4.1, 0.1 0.1), "
      "(1.1 1.1, 2.9 1.1, 2.9 2.9, 1.1 2.9, 1.1 1.1))");
  std::unique_ptr<Geometry> poly_out =
      PointwisePrecisionReducerTransformer::reduce(*poly_in, pm);
  std::unique_ptr<Geometry> poly_expected =
      ReadWkt("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), (1 1, 3 1, 3 3, 1 3, 1 1))");
  EXPECT_EQ(poly_out->getGeometryTypeId(), GEOS_POLYGON);
  EXPECT_TRUE(poly_out->isValid());
  EXPECT_TRUE(poly_out->equalsExact(poly_expected.get()));

  // Unlike PrecisionReducerTransformer, PointwisePrecisionReducerTransformer
  // keeps degenerate collapsed polygons as invalid polygons.
  std::unique_ptr<Geometry> sliver_in =
      ReadWkt("POLYGON ((0.1 0.1, 2.1 0.1, 2.1 0.2, 0.1 0.2, 0.1 0.1))");
  std::unique_ptr<Geometry> sliver_out =
      PointwisePrecisionReducerTransformer::reduce(*sliver_in, pm);
  const auto* sliver = dynamic_cast<const Polygon*>(sliver_out.get());
  ASSERT_NE(sliver, nullptr);
  EXPECT_FALSE(sliver->isEmpty());
  EXPECT_EQ(sliver->getNumPoints(), 5u);
  EXPECT_DOUBLE_EQ(sliver->getArea(), 0.0);
  EXPECT_FALSE(sliver->isValid());

  std::unique_ptr<Geometry> mp_in = ReadWkt(
      "MULTIPOLYGON (((0 0, 0.9 0, 0.9 2, 0 2, 0 0)), "
      "((1.1 0, 2 0, 2 2, 1.1 2, 1.1 0)))");
  std::unique_ptr<Geometry> mp_out =
      PointwisePrecisionReducerTransformer::reduce(*mp_in, pm);
  std::unique_ptr<Geometry> mp_expected = ReadWkt(
      "MULTIPOLYGON (((0 0, 1 0, 1 2, 0 2, 0 0)), "
      "((1 0, 2 0, 2 2, 1 2, 1 0)))");
  EXPECT_EQ(mp_out->getGeometryTypeId(), GEOS_MULTIPOLYGON);
  EXPECT_EQ(mp_out->getNumGeometries(), 2u);
  EXPECT_TRUE(mp_out->equalsExact(mp_expected.get()));
}

TEST_F(PointwisePrecisionReducerTransformerTest,
       HandlesCollectionsAndMultiGeometries) {
  PrecisionModel pm(1.0);

  std::unique_ptr<Geometry> mp_in =
      ReadWkt("MULTIPOINT ((0.1 0.2), (0.3 0.4), (1.8 2.2))");
  std::unique_ptr<Geometry> mp_out =
      PointwisePrecisionReducerTransformer::reduce(*mp_in, pm);
  std::unique_ptr<Geometry> mp_expected =
      ReadWkt("MULTIPOINT ((0 0), (0 0), (2 2))");
  EXPECT_EQ(mp_out->getGeometryTypeId(), GEOS_MULTIPOINT);
  EXPECT_EQ(mp_out->getNumGeometries(), 3u);
  EXPECT_TRUE(mp_out->equalsExact(mp_expected.get()));

  std::unique_ptr<Geometry> mls_in =
      ReadWkt("MULTILINESTRING ((0.1 0.1, 1.9 0.1), (2.1 2.1, 2.2 2.2))");
  std::unique_ptr<Geometry> mls_out =
      PointwisePrecisionReducerTransformer::reduce(*mls_in, pm);
  std::unique_ptr<Geometry> mls_expected =
      ReadWkt("MULTILINESTRING ((0 0, 2 0), (2 2, 2 2))");
  EXPECT_EQ(mls_out->getGeometryTypeId(), GEOS_MULTILINESTRING);
  EXPECT_EQ(mls_out->getNumGeometries(), 2u);
  EXPECT_TRUE(mls_out->equalsExact(mls_expected.get()));

  std::unique_ptr<Geometry> gc_in = ReadWkt(
      "GEOMETRYCOLLECTION (POINT (1.2 2.8), LINESTRING EMPTY, "
      "POLYGON ((0.1 0.1, 1.9 0.1, 1.9 1.9, 0.1 1.9, 0.1 0.1)))");
  std::unique_ptr<Geometry> gc_out =
      PointwisePrecisionReducerTransformer::reduce(*gc_in, pm);
  std::unique_ptr<Geometry> gc_expected = ReadWkt(
      "GEOMETRYCOLLECTION (POINT (1 3), "
      "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)))");
  EXPECT_EQ(gc_out->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(gc_out->getNumGeometries(), 2u);
  EXPECT_TRUE(gc_out->equalsExact(gc_expected.get()));
}

TEST_F(PointwisePrecisionReducerTransformerTest, ThrowsOnCurvedGeometry) {
  PrecisionModel pm(1.0);
  std::unique_ptr<Geometry> curve = ReadWkt("CIRCULARSTRING (0 0, 1 1, 2 0)");
  EXPECT_THROW(PointwisePrecisionReducerTransformer::reduce(*curve, pm),
               IllegalArgumentException);
}

TEST_F(PointwisePrecisionReducerTransformerTest,
       AsymmetricHalfwayRoundingAndPositiveFractionalScale) {
  PrecisionModel unit_pm(1.0);
  std::unique_ptr<Geometry> pos_half = ReadWkt("POINT (0.5 1.5)");
  std::unique_ptr<Geometry> pos_out =
      PointwisePrecisionReducerTransformer::reduce(*pos_half, unit_pm);
  const auto* pos_pt = dynamic_cast<const Point*>(pos_out.get());
  ASSERT_NE(pos_pt, nullptr);
  EXPECT_DOUBLE_EQ(pos_pt->getX(), 1.0);
  EXPECT_DOUBLE_EQ(pos_pt->getY(), 2.0);

  std::unique_ptr<Geometry> neg_half = ReadWkt("POINT (-0.5 -1.5)");
  std::unique_ptr<Geometry> neg_out =
      PointwisePrecisionReducerTransformer::reduce(*neg_half, unit_pm);
  const auto* neg_pt = dynamic_cast<const Point*>(neg_out.get());
  ASSERT_NE(neg_pt, nullptr);
  EXPECT_EQ(neg_pt->getX(), 0.0);
  EXPECT_DOUBLE_EQ(neg_pt->getY(), -1.0);

  PrecisionModel grid10_pm(0.1);
  std::unique_ptr<Geometry> grid_in = ReadWkt("POINT (14.9 25.1)");
  std::unique_ptr<Geometry> grid_out =
      PointwisePrecisionReducerTransformer::reduce(*grid_in, grid10_pm);
  const auto* grid_pt = dynamic_cast<const Point*>(grid_out.get());
  ASSERT_NE(grid_pt, nullptr);
  EXPECT_DOUBLE_EQ(grid_pt->getX(), 10.0);
  EXPECT_DOUBLE_EQ(grid_pt->getY(), 30.0);
}

class ExposedPointwisePrecisionReducerTransformer
    : public PointwisePrecisionReducerTransformer {
 public:
  using PointwisePrecisionReducerTransformer::
      PointwisePrecisionReducerTransformer;
  using PointwisePrecisionReducerTransformer::transformCoordinates;
};

TEST_F(PointwisePrecisionReducerTransformerTest,
       TransformCoordinatesHandlesEmptyAndNonEmptySequences) {
  PrecisionModel pm(1.0);
  ExposedPointwisePrecisionReducerTransformer transformer(pm);

  CoordinateSequence empty_xy = CoordinateSequence::XY(0);
  std::unique_ptr<CoordinateSequence> out_xy =
      transformer.transformCoordinates(&empty_xy, nullptr);
  ASSERT_NE(out_xy, nullptr);
  EXPECT_TRUE(out_xy->isEmpty());
  EXPECT_EQ(out_xy->getDimension(), 2u);
  EXPECT_FALSE(out_xy->hasZ());
  EXPECT_FALSE(out_xy->hasM());

  CoordinateSequence empty_xyz = CoordinateSequence::XYZ(0);
  std::unique_ptr<CoordinateSequence> out_xyz =
      transformer.transformCoordinates(&empty_xyz, nullptr);
  ASSERT_NE(out_xyz, nullptr);
  EXPECT_TRUE(out_xyz->isEmpty());
  EXPECT_EQ(out_xyz->getDimension(), 3u);
  EXPECT_TRUE(out_xyz->hasZ());
  EXPECT_FALSE(out_xyz->hasM());

  CoordinateSequence empty_xyzm = CoordinateSequence::XYZM(0);
  std::unique_ptr<CoordinateSequence> out_xyzm =
      transformer.transformCoordinates(&empty_xyzm, nullptr);
  ASSERT_NE(out_xyzm, nullptr);
  EXPECT_TRUE(out_xyzm->isEmpty());
  EXPECT_EQ(out_xyzm->getDimension(), 4u);
  EXPECT_TRUE(out_xyzm->hasZ());
  EXPECT_TRUE(out_xyzm->hasM());

  CoordinateSequence non_empty = {
      Coordinate(0.2, 0.3, 4.5),
      Coordinate(0.4, 0.4, 5.5),
      Coordinate(1.6, 2.4),
  };
  std::unique_ptr<Geometry> parent = ReadWkt("LINESTRING (0 0, 1 1)");
  std::unique_ptr<CoordinateSequence> out_seq =
      transformer.transformCoordinates(&non_empty, parent.get());
  ASSERT_NE(out_seq, nullptr);
  ASSERT_EQ(out_seq->size(), 3u);
  EXPECT_DOUBLE_EQ(out_seq->getX(0), 0.0);
  EXPECT_DOUBLE_EQ(out_seq->getY(0), 0.0);
  EXPECT_DOUBLE_EQ(out_seq->getAt(0).z, 4.5);
  EXPECT_DOUBLE_EQ(out_seq->getX(1), 0.0);
  EXPECT_DOUBLE_EQ(out_seq->getY(1), 0.0);
  EXPECT_DOUBLE_EQ(out_seq->getAt(1).z, 5.5);
  EXPECT_DOUBLE_EQ(out_seq->getX(2), 2.0);
  EXPECT_DOUBLE_EQ(out_seq->getY(2), 2.0);
  EXPECT_TRUE(std::isnan(out_seq->getAt(2).z));
}

}  // namespace
}  // namespace geos_google
