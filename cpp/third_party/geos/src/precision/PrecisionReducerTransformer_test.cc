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
// third_party/geos/src/precision/PrecisionReducerTransformer.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/precision/PrecisionReducerTransformer.cpp

#include "third_party/geos/include/geos/precision/PrecisionReducerTransformer.h"

#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

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
using geos::geom::LineString;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::precision::PrecisionReducerTransformer;
using geos::util::IllegalArgumentException;

class PrecisionReducerTransformerTest : public ::testing::Test {
 protected:
  PrecisionReducerTransformerTest()
      : factory_(GeometryFactory::create()), reader_(factory_.get()) {}

  std::unique_ptr<Geometry> ReadWkt(std::string_view wkt) const {
    return reader_.read(std::string(wkt));
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(PrecisionReducerTransformerTest, ReducesPointsAcrossPrecisionModels) {
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
        PrecisionReducerTransformer::reduce(*input, pm);
    ASSERT_NE(reduced, nullptr);
    EXPECT_EQ(reduced->getFactory(), factory_.get());
    const auto* pt = dynamic_cast<const Point*>(reduced.get());
    ASSERT_NE(pt, nullptr);
    EXPECT_NEAR(pt->getX(), c.expected_x, c.tolerance);
    EXPECT_NEAR(pt->getY(), c.expected_y, c.tolerance);
    EXPECT_FALSE(pt->hasZ());
    EXPECT_FALSE(pt->hasM());
    EXPECT_TRUE(std::isnan(pt->getZ()));
    EXPECT_TRUE(std::isnan(pt->getM()));
  }
}

TEST_F(PrecisionReducerTransformerTest, InstanceTransformMatchesStaticReduce) {
  PrecisionModel pm(2.0);
  std::unique_ptr<Geometry> input =
      ReadWkt("LINESTRING (0.1 0.1, 0.2 0.2, 1.3 1.7)");

  PrecisionReducerTransformer trans_default(pm);
  PrecisionReducerTransformer trans_remove(pm, true);
  std::unique_ptr<Geometry> inst_default = trans_default.transform(input.get());
  std::unique_ptr<Geometry> inst_remove = trans_remove.transform(input.get());
  std::unique_ptr<Geometry> stat_default =
      PrecisionReducerTransformer::reduce(*input, pm);

  EXPECT_TRUE(inst_default->equalsIdentical(stat_default.get()));
  EXPECT_TRUE(inst_remove->equalsIdentical(stat_default.get()));
}

TEST_F(PrecisionReducerTransformerTest,
       HandlesAllCoordinateDimensionsAndPreservesZM) {
  PrecisionModel pm(1.0);

  // XYZ
  std::unique_ptr<Geometry> pt_z = ReadWkt("POINT Z (1.26 3.74 9.875)");
  std::unique_ptr<Geometry> pt_z_out =
      PrecisionReducerTransformer::reduce(*pt_z, pm);
  const auto* pz = dynamic_cast<const Point*>(pt_z_out.get());
  ASSERT_NE(pz, nullptr);
  EXPECT_TRUE(pz->hasZ());
  EXPECT_FALSE(pz->hasM());
  EXPECT_DOUBLE_EQ(pz->getX(), 1.0);
  EXPECT_DOUBLE_EQ(pz->getY(), 4.0);
  EXPECT_DOUBLE_EQ(pz->getZ(), 9.875);
  EXPECT_TRUE(std::isnan(pz->getM()));

  // XYM
  std::unique_ptr<Geometry> pt_m = ReadWkt("POINT M (1.26 3.74 12.25)");
  std::unique_ptr<Geometry> pt_m_out =
      PrecisionReducerTransformer::reduce(*pt_m, pm);
  const auto* p_m = dynamic_cast<const Point*>(pt_m_out.get());
  ASSERT_NE(p_m, nullptr);
  EXPECT_FALSE(p_m->hasZ());
  EXPECT_TRUE(p_m->hasM());
  EXPECT_DOUBLE_EQ(p_m->getX(), 1.0);
  EXPECT_DOUBLE_EQ(p_m->getY(), 4.0);
  EXPECT_TRUE(std::isnan(p_m->getZ()));
  EXPECT_DOUBLE_EQ(p_m->getM(), 12.25);

  // XYZM
  std::unique_ptr<Geometry> pt_zm = ReadWkt("POINT ZM (1.26 3.74 9.5 12.25)");
  std::unique_ptr<Geometry> pt_zm_out =
      PrecisionReducerTransformer::reduce(*pt_zm, pm);
  const auto* pzm = dynamic_cast<const Point*>(pt_zm_out.get());
  ASSERT_NE(pzm, nullptr);
  EXPECT_TRUE(pzm->hasZ());
  EXPECT_TRUE(pzm->hasM());
  EXPECT_DOUBLE_EQ(pzm->getX(), 1.0);
  EXPECT_DOUBLE_EQ(pzm->getY(), 4.0);
  EXPECT_DOUBLE_EQ(pzm->getZ(), 9.5);
  EXPECT_DOUBLE_EQ(pzm->getM(), 12.25);

  // LineString duplicate removal across XYZ, XYM, and XYZM
  std::unique_ptr<Geometry> line_z =
      ReadWkt("LINESTRING Z (0.1 0.1 10, 0.2 0.2 20, 2.4 2.6 30)");
  std::unique_ptr<Geometry> line_z_out =
      PrecisionReducerTransformer::reduce(*line_z, pm);
  EXPECT_TRUE(line_z_out->equalsIdentical(
      ReadWkt("LINESTRING Z (0 0 10, 2 3 30)").get()));

  std::unique_ptr<Geometry> line_m =
      ReadWkt("LINESTRING M (0.1 0.1 10, 0.2 0.2 20, 2.4 2.6 30)");
  std::unique_ptr<Geometry> line_m_out =
      PrecisionReducerTransformer::reduce(*line_m, pm);
  EXPECT_TRUE(line_m_out->equalsIdentical(
      ReadWkt("LINESTRING M (0 0 10, 2 3 30)").get()));

  std::unique_ptr<Geometry> line_zm =
      ReadWkt("LINESTRING ZM (0.1 0.1 5 10, 0.2 0.2 6 20, 2.4 2.6 7 30)");
  std::unique_ptr<Geometry> line_zm_out =
      PrecisionReducerTransformer::reduce(*line_zm, pm);
  EXPECT_TRUE(line_zm_out->equalsIdentical(
      ReadWkt("LINESTRING ZM (0 0 5 10, 2 3 7 30)").get()));
}

TEST_F(PrecisionReducerTransformerTest, HandlesEmptyGeometries) {
  struct EmptyCase {
    std::string_view wkt;
    GeometryTypeId type_id;
  };

  constexpr auto kEmptyCases = std::to_array<EmptyCase>({
      {.wkt = "POINT EMPTY", .type_id = GEOS_POINT},
      {.wkt = "POINT Z EMPTY", .type_id = GEOS_POINT},
      {.wkt = "POINT M EMPTY", .type_id = GEOS_POINT},
      {.wkt = "POINT ZM EMPTY", .type_id = GEOS_POINT},
      {.wkt = "LINESTRING EMPTY", .type_id = GEOS_LINESTRING},
      {.wkt = "LINESTRING Z EMPTY", .type_id = GEOS_LINESTRING},
      {.wkt = "LINEARRING EMPTY", .type_id = GEOS_LINEARRING},
      {.wkt = "POLYGON EMPTY", .type_id = GEOS_POLYGON},
      {.wkt = "POLYGON Z EMPTY", .type_id = GEOS_POLYGON},
      {.wkt = "MULTIPOINT EMPTY", .type_id = GEOS_MULTIPOINT},
      {.wkt = "MULTILINESTRING EMPTY", .type_id = GEOS_MULTILINESTRING},
      // transformMultiPolygon delegates to overlayng::PrecisionReducer, which
      // canonicalizes empty 2D results to an empty Polygon.
      {.wkt = "MULTIPOLYGON EMPTY", .type_id = GEOS_POLYGON},
      {.wkt = "MULTIPOLYGON Z EMPTY", .type_id = GEOS_POLYGON},
      {.wkt = "GEOMETRYCOLLECTION EMPTY", .type_id = GEOS_GEOMETRYCOLLECTION},
  });

  PrecisionModel pm(1.0);
  for (const EmptyCase& c : kEmptyCases) {
    SCOPED_TRACE(c.wkt);
    std::unique_ptr<Geometry> input = ReadWkt(c.wkt);
    std::unique_ptr<Geometry> reduced =
        PrecisionReducerTransformer::reduce(*input, pm);
    ASSERT_NE(reduced, nullptr);
    EXPECT_TRUE(reduced->isEmpty());
    EXPECT_EQ(reduced->getGeometryTypeId(), c.type_id);
  }
}

TEST_F(PrecisionReducerTransformerTest,
       LineStringRemovesConsecutiveRepeatedPoints) {
  PrecisionModel pm(1.0);
  std::unique_ptr<Geometry> input =
      ReadWkt("LINESTRING (0.1 0.1, 0.2 0.2, 1.1 0.1, 1.2 0.2, 2.0 0.0)");
  std::unique_ptr<Geometry> expected = ReadWkt("LINESTRING (0 0, 1 0, 2 0)");

  for (bool remove_collapsed : std::array{false, true}) {
    std::unique_ptr<Geometry> reduced =
        PrecisionReducerTransformer::reduce(*input, pm, remove_collapsed);
    EXPECT_EQ(reduced->getGeometryTypeId(), GEOS_LINESTRING);
    EXPECT_EQ(reduced->getNumPoints(), 3u);
    EXPECT_TRUE(reduced->equalsExact(expected.get()));
  }
}

TEST_F(PrecisionReducerTransformerTest,
       CollapsedLineStringExtendsOrRemovesBasedOnFlag) {
  PrecisionModel pm(1.0);
  std::unique_ptr<Geometry> input =
      ReadWkt("LINESTRING (0.1 0.1, 0.2 0.2, 0.3 0.3)");

  // Default / isRemoveCollapsed == false: extends collapsed sequence to 2 pts.
  std::unique_ptr<Geometry> extended =
      PrecisionReducerTransformer::reduce(*input, pm, false);
  const auto* line = dynamic_cast<const LineString*>(extended.get());
  ASSERT_NE(line, nullptr);
  EXPECT_FALSE(line->isEmpty());
  EXPECT_EQ(line->getNumPoints(), 2u);
  EXPECT_DOUBLE_EQ(line->getLength(), 0.0);
  const CoordinateSequence* seq = line->getCoordinatesRO();
  EXPECT_DOUBLE_EQ(seq->getX(0), 0.0);
  EXPECT_DOUBLE_EQ(seq->getY(0), 0.0);
  EXPECT_DOUBLE_EQ(seq->getX(1), 0.0);
  EXPECT_DOUBLE_EQ(seq->getY(1), 0.0);

  // isRemoveCollapsed == true: returns empty LineString.
  std::unique_ptr<Geometry> removed =
      PrecisionReducerTransformer::reduce(*input, pm, true);
  EXPECT_EQ(removed->getGeometryTypeId(), GEOS_LINESTRING);
  EXPECT_TRUE(removed->isEmpty());

  // Verify Z coordinate is preserved across extend().
  std::unique_ptr<Geometry> line_z =
      ReadWkt("LINESTRING Z (0.1 0.1 5.5, 0.2 0.2 7.5)");
  std::unique_ptr<Geometry> line_z_ext =
      PrecisionReducerTransformer::reduce(*line_z, pm, false);
  EXPECT_TRUE(line_z_ext->equalsIdentical(
      ReadWkt("LINESTRING Z (0 0 5.5, 0 0 5.5)").get()));
}

TEST_F(PrecisionReducerTransformerTest, LinearRingReductionAndCollapse) {
  PrecisionModel pm(1.0);

  // Valid ring remains a LinearRing.
  std::unique_ptr<Geometry> valid_in =
      ReadWkt("LINEARRING (0.1 0.1, 2.1 0.1, 2.1 2.1, 0.1 2.1, 0.1 0.1)");
  std::unique_ptr<Geometry> valid_out =
      PrecisionReducerTransformer::reduce(*valid_in, pm);
  EXPECT_EQ(valid_out->getGeometryTypeId(), GEOS_LINEARRING);
  EXPECT_TRUE(valid_out->equalsExact(
      ReadWkt("LINEARRING (0 0, 2 0, 2 2, 0 2, 0 0)").get()));

  // Partial collapse removing one duplicate point preserves 4-pt LinearRing.
  std::unique_ptr<Geometry> partial_in =
      ReadWkt("LINEARRING (0.1 0.1, 2.1 0.1, 2.1 0.2, 2.1 2.1, 0.1 0.1)");
  std::unique_ptr<Geometry> partial_out =
      PrecisionReducerTransformer::reduce(*partial_in, pm, true);
  EXPECT_EQ(partial_out->getGeometryTypeId(), GEOS_LINEARRING);
  EXPECT_EQ(partial_out->getNumPoints(), 4u);
  EXPECT_TRUE(partial_out->equalsExact(
      ReadWkt("LINEARRING (0 0, 2 0, 2 2, 0 0)").get()));

  // Collapse to 3 points hits minLength == LinearRing::MINIMUM_VALID_SIZE (3),
  // and GeometryTransformer downgrades < 4 coordinates to a LineString.
  std::unique_ptr<Geometry> tri_collapse_in =
      ReadWkt("LINEARRING (0.1 0.1, 2.1 0.1, 2.1 0.2, 0.1 0.1)");
  std::unique_ptr<Geometry> tri_collapse_out =
      PrecisionReducerTransformer::reduce(*tri_collapse_in, pm, true);
  EXPECT_EQ(tri_collapse_out->getGeometryTypeId(), GEOS_LINESTRING);
  EXPECT_EQ(tri_collapse_out->getNumPoints(), 3u);
  EXPECT_TRUE(tri_collapse_out->equalsExact(
      ReadWkt("LINESTRING (0 0, 2 0, 0 0)").get()));

  // Collapse to 1 point (< minLength == 3): removed or extended to 3 pts.
  std::unique_ptr<Geometry> full_collapse_in =
      ReadWkt("LINEARRING (0.1 0.1, 0.2 0.1, 0.2 0.2, 0.1 0.2, 0.1 0.1)");
  std::unique_ptr<Geometry> removed =
      PrecisionReducerTransformer::reduce(*full_collapse_in, pm, true);
  EXPECT_EQ(removed->getGeometryTypeId(), GEOS_LINEARRING);
  EXPECT_TRUE(removed->isEmpty());

  std::unique_ptr<Geometry> extended =
      PrecisionReducerTransformer::reduce(*full_collapse_in, pm, false);
  EXPECT_EQ(extended->getGeometryTypeId(), GEOS_LINESTRING);
  EXPECT_FALSE(extended->isEmpty());
  EXPECT_EQ(extended->getNumPoints(), 3u);
  EXPECT_TRUE(
      extended->equalsExact(ReadWkt("LINESTRING (0 0, 0 0, 0 0)").get()));
}

TEST_F(PrecisionReducerTransformerTest,
       PolygonUsesOverlayPrecisionReductionAndRepairsTopology) {
  PrecisionModel pm(1.0);

  // Valid polygon with hole.
  std::unique_ptr<Geometry> poly_in = ReadWkt(
      "POLYGON ((0.1 0.1, 4.1 0.1, 4.1 4.1, 0.1 4.1, 0.1 0.1), "
      "(1.1 1.1, 2.9 1.1, 2.9 2.9, 1.1 2.9, 1.1 1.1))");
  std::unique_ptr<Geometry> poly_out =
      PrecisionReducerTransformer::reduce(*poly_in, pm);
  std::unique_ptr<Geometry> poly_expected =
      ReadWkt("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), (1 1, 3 1, 3 3, 1 3, 1 1))");
  poly_out->normalize();
  poly_expected->normalize();
  EXPECT_TRUE(poly_out->isValid());
  EXPECT_TRUE(poly_out->equalsExact(poly_expected.get()));

  // Sliver polygon collapses to empty Polygon regardless of isRemoveCollapsed.
  std::unique_ptr<Geometry> sliver_in =
      ReadWkt("POLYGON ((0.1 0.1, 2.1 0.1, 2.1 0.2, 0.1 0.2, 0.1 0.1))");
  for (bool remove_collapsed : std::array{false, true}) {
    std::unique_ptr<Geometry> sliver_out =
        PrecisionReducerTransformer::reduce(*sliver_in, pm, remove_collapsed);
    EXPECT_EQ(sliver_out->getGeometryTypeId(), GEOS_POLYGON);
    EXPECT_TRUE(sliver_out->isEmpty());
  }

  // Polygon with a tiny hole loses the collapsed hole cleanly.
  std::unique_ptr<Geometry> small_hole_in = ReadWkt(
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), "
      "(1.1 1.1, 1.2 1.1, 1.2 1.2, 1.1 1.2, 1.1 1.1))");
  std::unique_ptr<Geometry> small_hole_out =
      PrecisionReducerTransformer::reduce(*small_hole_in, pm);
  const auto* poly = dynamic_cast<const Polygon*>(small_hole_out.get());
  ASSERT_NE(poly, nullptr);
  EXPECT_TRUE(poly->isValid());
  EXPECT_EQ(poly->getNumInteriorRing(), 0u);
  EXPECT_DOUBLE_EQ(poly->getArea(), 16.0);

  // C-shaped polygon whose narrow gap pinches shut when rounded becomes a valid
  // polygon with an interior hole.
  std::unique_ptr<Geometry> pinch_in = ReadWkt(
      "POLYGON ((0 0, 4 0, 4 1.9, 3 1.9, 3 1, 1 1, 1 3, 3 3, 3 2.1, 4 2.1, "
      "4 4, 0 4, 0 0))");
  ASSERT_TRUE(pinch_in->isValid());
  std::unique_ptr<Geometry> pinch_out =
      PrecisionReducerTransformer::reduce(*pinch_in, pm);
  const auto* pinch_poly = dynamic_cast<const Polygon*>(pinch_out.get());
  ASSERT_NE(pinch_poly, nullptr);
  EXPECT_TRUE(pinch_poly->isValid());
  EXPECT_EQ(pinch_poly->getNumInteriorRing(), 1u);
  EXPECT_DOUBLE_EQ(pinch_poly->getArea(), 12.0);
}

TEST_F(PrecisionReducerTransformerTest,
       MultiPolygonMergesAdjacentAndDropsCollapsed) {
  PrecisionModel pm(1.0);

  // Two disjoint rectangles separated by < 0.5 touch along x = 1 after
  // rounding and are unioned into a single valid Polygon (retaining the shared
  // noding vertices along x = 1 on the merged boundary).
  std::unique_ptr<Geometry> touching_mp = ReadWkt(
      "MULTIPOLYGON (((0 0, 0.9 0, 0.9 2, 0 2, 0 0)), "
      "((1.1 0, 2 0, 2 2, 1.1 2, 1.1 0)))");
  std::unique_ptr<Geometry> merged =
      PrecisionReducerTransformer::reduce(*touching_mp, pm);
  std::unique_ptr<Geometry> merged_expected =
      ReadWkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  EXPECT_EQ(merged->getGeometryTypeId(), GEOS_POLYGON);
  EXPECT_TRUE(merged->isValid());
  EXPECT_DOUBLE_EQ(merged->getArea(), 4.0);
  EXPECT_DOUBLE_EQ(merged->getLength(), 8.0);
  EXPECT_TRUE(merged->equals(merged_expected.get()));

  // MultiPolygon with one sliver and two surviving polygons drops the sliver.
  std::unique_ptr<Geometry> partial_mp = ReadWkt(
      "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "((2.1 2.1, 3.1 2.1, 3.1 2.2, 2.1 2.2, 2.1 2.1)), "
      "((4 4, 5 4, 5 5, 4 5, 4 4)))");
  std::unique_ptr<Geometry> partial_out =
      PrecisionReducerTransformer::reduce(*partial_mp, pm);
  EXPECT_EQ(partial_out->getGeometryTypeId(), GEOS_MULTIPOLYGON);
  EXPECT_EQ(partial_out->getNumGeometries(), 2u);
  EXPECT_TRUE(partial_out->isValid());
  EXPECT_DOUBLE_EQ(partial_out->getArea(), 2.0);

  // MultiPolygon whose components all collapse returns empty polygonal result.
  std::unique_ptr<Geometry> all_collapsed = ReadWkt(
      "MULTIPOLYGON (((0.1 0.1, 2.1 0.1, 2.1 0.2, 0.1 0.2, 0.1 0.1)), "
      "((4.1 4.1, 4.2 4.1, 4.2 4.2, 4.1 4.2, 4.1 4.1)))");
  std::unique_ptr<Geometry> collapsed_out =
      PrecisionReducerTransformer::reduce(*all_collapsed, pm);
  EXPECT_TRUE(collapsed_out->isEmpty());
  EXPECT_TRUE(collapsed_out->isPolygonal());
}

TEST_F(PrecisionReducerTransformerTest,
       HandlesMultiLineStringAndGeometryCollection) {
  PrecisionModel pm(1.0);

  std::unique_ptr<Geometry> mls_in =
      ReadWkt("MULTILINESTRING ((0.1 0.1, 0.2 0.2), (1.1 1.1, 3.1 1.1))");
  std::unique_ptr<Geometry> mls_removed =
      PrecisionReducerTransformer::reduce(*mls_in, pm, true);
  EXPECT_EQ(mls_removed->getGeometryTypeId(), GEOS_LINESTRING);
  EXPECT_TRUE(mls_removed->equalsExact(ReadWkt("LINESTRING (1 1, 3 1)").get()));

  std::unique_ptr<Geometry> mls_kept =
      PrecisionReducerTransformer::reduce(*mls_in, pm, false);
  EXPECT_EQ(mls_kept->getGeometryTypeId(), GEOS_MULTILINESTRING);
  EXPECT_EQ(mls_kept->getNumGeometries(), 2u);
  EXPECT_TRUE(mls_kept->equalsExact(
      ReadWkt("MULTILINESTRING ((0 0, 0 0), (1 1, 3 1))").get()));

  std::unique_ptr<Geometry> all_collapsing_mls =
      ReadWkt("MULTILINESTRING ((0.1 0.1, 0.2 0.2), (1.1 1.1, 1.2 1.2))");
  std::unique_ptr<Geometry> empty_mls =
      PrecisionReducerTransformer::reduce(*all_collapsing_mls, pm, true);
  EXPECT_EQ(empty_mls->getGeometryTypeId(), GEOS_MULTILINESTRING);
  EXPECT_TRUE(empty_mls->isEmpty());

  std::unique_ptr<Geometry> gc_in = ReadWkt(
      "GEOMETRYCOLLECTION (POINT (1.2 2.8), "
      "LINESTRING (0.1 0.1, 0.2 0.2), "
      "POLYGON ((0.1 0.1, 2.1 0.1, 2.1 2.1, 0.1 2.1, 0.1 0.1)), "
      "MULTIPOLYGON (((5 5, 6 5, 6 6, 5 6, 5 5))))");
  std::unique_ptr<Geometry> gc_removed =
      PrecisionReducerTransformer::reduce(*gc_in, pm, true);
  EXPECT_EQ(gc_removed->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(gc_removed->getNumGeometries(), 3u);

  std::unique_ptr<Geometry> gc_kept =
      PrecisionReducerTransformer::reduce(*gc_in, pm, false);
  EXPECT_EQ(gc_kept->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(gc_kept->getNumGeometries(), 4u);
}

TEST_F(PrecisionReducerTransformerTest, ThrowsOnCurvedGeometry) {
  PrecisionModel pm(1.0);
  std::unique_ptr<Geometry> curve = ReadWkt("CIRCULARSTRING (0 0, 1 1, 2 0)");
  EXPECT_THROW(PrecisionReducerTransformer::reduce(*curve, pm),
               IllegalArgumentException);
}

TEST_F(PrecisionReducerTransformerTest,
       MultiPointReducesComponentsAndPrunesEmpty) {
  PrecisionModel pm(1.0);

  std::unique_ptr<Geometry> mp_in =
      ReadWkt("MULTIPOINT ((0.1 0.1), (0.2 0.2), (1.4 2.6))");
  std::unique_ptr<Geometry> mp_out =
      PrecisionReducerTransformer::reduce(*mp_in, pm);
  EXPECT_EQ(mp_out->getGeometryTypeId(), GEOS_MULTIPOINT);
  EXPECT_EQ(mp_out->getNumGeometries(), 3u);
  EXPECT_TRUE(
      mp_out->equalsExact(ReadWkt("MULTIPOINT ((0 0), (0 0), (1 3))").get()));

  std::unique_ptr<Geometry> mp_with_empty =
      ReadWkt("MULTIPOINT (EMPTY, (1.2 2.8))");
  std::unique_ptr<Geometry> mp_pruned =
      PrecisionReducerTransformer::reduce(*mp_with_empty, pm);
  EXPECT_EQ(mp_pruned->getGeometryTypeId(), GEOS_POINT);
  EXPECT_TRUE(mp_pruned->equalsExact(ReadWkt("POINT (1 3)").get()));

  std::unique_ptr<Geometry> mp_all_empty = ReadWkt("MULTIPOINT (EMPTY, EMPTY)");
  std::unique_ptr<Geometry> mp_empty_out =
      PrecisionReducerTransformer::reduce(*mp_all_empty, pm);
  EXPECT_EQ(mp_empty_out->getGeometryTypeId(), GEOS_MULTIPOINT);
  EXPECT_TRUE(mp_empty_out->isEmpty());
}

TEST_F(PrecisionReducerTransformerTest,
       LineStringKeepsNonConsecutiveDuplicatesAndDeduplicatesFloating) {
  PrecisionModel pm_fixed(1.0);
  std::unique_ptr<Geometry> backtrack_in =
      ReadWkt("LINESTRING (0.1 0.1, 1.1 0.1, 0.2 0.2)");
  std::unique_ptr<Geometry> backtrack_out =
      PrecisionReducerTransformer::reduce(*backtrack_in, pm_fixed);
  EXPECT_EQ(backtrack_out->getGeometryTypeId(), GEOS_LINESTRING);
  EXPECT_EQ(backtrack_out->getNumPoints(), 3u);
  EXPECT_TRUE(
      backtrack_out->equalsExact(ReadWkt("LINESTRING (0 0, 1 0, 0 0)").get()));

  PrecisionModel pm_floating(PrecisionModel::FLOATING);
  std::unique_ptr<Geometry> dup_in =
      ReadWkt("LINESTRING (1.5 2.5, 1.5 2.5, 3.5 4.5)");
  std::unique_ptr<Geometry> dup_out =
      PrecisionReducerTransformer::reduce(*dup_in, pm_floating);
  EXPECT_EQ(dup_out->getGeometryTypeId(), GEOS_LINESTRING);
  EXPECT_EQ(dup_out->getNumPoints(), 2u);
  EXPECT_TRUE(
      dup_out->equalsExact(ReadWkt("LINESTRING (1.5 2.5, 3.5 4.5)").get()));

  PrecisionModel pm_single(PrecisionModel::FLOATING_SINGLE);
  std::unique_ptr<Geometry> single_in =
      ReadWkt("LINESTRING (0.123456789 0.123456789, 1.123456789 2.987654321)");
  std::unique_ptr<Geometry> single_out =
      PrecisionReducerTransformer::reduce(*single_in, pm_single);
  const auto* single_line = dynamic_cast<const LineString*>(single_out.get());
  ASSERT_NE(single_line, nullptr);
  ASSERT_EQ(single_line->getNumPoints(), 2u);
  const CoordinateSequence* single_seq = single_line->getCoordinatesRO();
  EXPECT_DOUBLE_EQ(single_seq->getX(0),
                   static_cast<double>(static_cast<float>(0.123456789)));
  EXPECT_DOUBLE_EQ(single_seq->getY(0),
                   static_cast<double>(static_cast<float>(0.123456789)));
  EXPECT_DOUBLE_EQ(single_seq->getX(1),
                   static_cast<double>(static_cast<float>(1.123456789)));
  EXPECT_DOUBLE_EQ(single_seq->getY(1),
                   static_cast<double>(static_cast<float>(2.987654321)));
}

TEST_F(PrecisionReducerTransformerTest,
       LinearRingCollapsePreservesZWhenExtended) {
  PrecisionModel pm(1.0);
  std::unique_ptr<Geometry> ring_z = ReadWkt(
      "LINEARRING Z (0.1 0.1 5.5, 0.2 0.1 6.5, 0.2 0.2 7.5, 0.1 0.1 5.5)");

  std::unique_ptr<Geometry> extended =
      PrecisionReducerTransformer::reduce(*ring_z, pm, false);
  EXPECT_EQ(extended->getGeometryTypeId(), GEOS_LINESTRING);
  EXPECT_EQ(extended->getNumPoints(), 3u);
  EXPECT_TRUE(extended->equalsIdentical(
      ReadWkt("LINESTRING Z (0 0 5.5, 0 0 5.5, 0 0 5.5)").get()));

  std::unique_ptr<Geometry> removed =
      PrecisionReducerTransformer::reduce(*ring_z, pm, true);
  EXPECT_EQ(removed->getGeometryTypeId(), GEOS_LINEARRING);
  EXPECT_TRUE(removed->isEmpty());
}

TEST_F(PrecisionReducerTransformerTest,
       GeometryCollectionPrunesCollapsedPolygonAndPolygonHandlesFloatingPM) {
  PrecisionModel pm_fixed(1.0);
  std::unique_ptr<Geometry> gc_sliver = ReadWkt(
      "GEOMETRYCOLLECTION (POINT (1.2 2.8), "
      "POLYGON ((0.1 0.1, 2.1 0.1, 2.1 0.2, 0.1 0.2, 0.1 0.1)))");
  std::unique_ptr<Geometry> gc_out =
      PrecisionReducerTransformer::reduce(*gc_sliver, pm_fixed, false);
  EXPECT_EQ(gc_out->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(gc_out->getNumGeometries(), 1u);
  EXPECT_TRUE(
      gc_out->getGeometryN(0)->equalsExact(ReadWkt("POINT (1 3)").get()));

  PrecisionModel pm_floating(PrecisionModel::FLOATING);
  std::unique_ptr<Geometry> poly_in = ReadWkt(
      "POLYGON ((0.125 0.25, 2.5 0.25, 2.5 2.75, 0.125 2.75, 0.125 0.25))");
  std::unique_ptr<Geometry> poly_out =
      PrecisionReducerTransformer::reduce(*poly_in, pm_floating);
  EXPECT_EQ(poly_out->getGeometryTypeId(), GEOS_POLYGON);
  EXPECT_TRUE(poly_out->isValid());
  EXPECT_TRUE(poly_out->equals(poly_in.get()));
}

}  // namespace
}  // namespace geos_google
