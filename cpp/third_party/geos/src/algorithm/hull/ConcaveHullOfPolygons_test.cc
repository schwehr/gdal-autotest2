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

// Unit tests for third_party/geos/src/algorithm/hull/ConcaveHullOfPolygons.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - Uses per-test isolated GeometryFactory::create() instances.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/algorithm/hull/ConcaveHullOfPolygons.cpp

#include "third_party/geos/include/geos/algorithm/hull/ConcaveHullOfPolygons.h"

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::algorithm::hull::ConcaveHullOfPolygons;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::MultiPolygon;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::util::IllegalArgumentException;
using geos::util::UnsupportedOperationException;

struct GridOrigin {
  double x;
  double y;
};

// 16 squares of size 4x4 spaced by gap 1.0 around a 16x16 courtyard
// [4,20]x[4,20]. Outer bounding square is [0,24]x[0,24] (area = 576). Each 1x4
// gap has diagonal sqrt(17) ~= 4.123; every chord spanning across the courtyard
// has length >= sqrt(26) ~= 5.099.
constexpr auto kSingleHoleRingOrigins = std::to_array<GridOrigin>({
    {.x = 0, .y = 0},
    {.x = 5, .y = 0},
    {.x = 10, .y = 0},
    {.x = 15, .y = 0},
    {.x = 20, .y = 0},
    {.x = 20, .y = 5},
    {.x = 20, .y = 10},
    {.x = 20, .y = 15},
    {.x = 20, .y = 20},
    {.x = 15, .y = 20},
    {.x = 10, .y = 20},
    {.x = 5, .y = 20},
    {.x = 0, .y = 20},
    {.x = 0, .y = 15},
    {.x = 0, .y = 10},
    {.x = 0, .y = 5},
});

// 27 squares of size 4x4 spaced by gap 1.0 forming two adjacent 16x16
// courtyards inside [0,44]x[0,24] (outer area = 1056).
constexpr auto kTwoHoleRingOrigins = std::to_array<GridOrigin>({
    {.x = 0, .y = 0},   {.x = 5, .y = 0},   {.x = 10, .y = 0},
    {.x = 15, .y = 0},  {.x = 20, .y = 0},  {.x = 25, .y = 0},
    {.x = 30, .y = 0},  {.x = 35, .y = 0},  {.x = 40, .y = 0},
    {.x = 40, .y = 5},  {.x = 40, .y = 10}, {.x = 40, .y = 15},
    {.x = 40, .y = 20}, {.x = 35, .y = 20}, {.x = 30, .y = 20},
    {.x = 25, .y = 20}, {.x = 20, .y = 20}, {.x = 15, .y = 20},
    {.x = 10, .y = 20}, {.x = 5, .y = 20},  {.x = 0, .y = 20},
    {.x = 0, .y = 15},  {.x = 0, .y = 10},  {.x = 0, .y = 5},
    {.x = 20, .y = 5},  {.x = 20, .y = 10}, {.x = 20, .y = 15},
});

constexpr std::string_view kTwoSquaresWkt =
    "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0)), "
    "((12 0, 22 0, 22 10, 12 10, 12 0)))";

constexpr std::string_view kLPolyWkt =
    "POLYGON ((0 0, 10 0, 10 2, 2 2, 2 10, 0 10, 0 0))";

constexpr std::string_view kFunnelBayWkt =
    "POLYGON ((0 0, 12 0, 12 12, 10 12, 8 6, 7 3, 5 3, 4 6, 2 12, 0 12, 0 0))";

constexpr std::string_view kLeftSquarePlusLPolyWkt =
    "MULTIPOLYGON (((-4 0, -2 0, -2 10, -4 10, -4 0)), "
    "((0 0, 10 0, 10 2, 2 2, 2 10, 0 10, 0 0)))";

class ConcaveHullOfPolygonsTest : public ::testing::Test {
 protected:
  ConcaveHullOfPolygonsTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadWkt(std::string_view wkt) const {
    return reader_.read(std::string(wkt));
  }

  template <std::size_t N>
  std::unique_ptr<MultiPolygon> MakeSquareRing(
      const std::array<GridOrigin, N>& origins, double size) const {
    std::vector<std::unique_ptr<Polygon>> polys;
    polys.reserve(N);
    for (const GridOrigin& origin : origins) {
      CoordinateSequence seq{
          Coordinate(origin.x, origin.y),
          Coordinate(origin.x + size, origin.y),
          Coordinate(origin.x + size, origin.y + size),
          Coordinate(origin.x, origin.y + size),
          Coordinate(origin.x, origin.y),
      };
      polys.push_back(factory_->createPolygon(std::move(seq)));
    }
    return factory_->createMultiPolygon(std::move(polys));
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(ConcaveHullOfPolygonsTest, ThrowsOnNonPolygonalInputs) {
  struct InvalidCase {
    std::string_view name;
    std::string_view wkt;
  };
  constexpr auto kCases = std::to_array<InvalidCase>({
      {.name = "point", .wkt = "POINT (1 2)"},
      {.name = "linestring", .wkt = "LINESTRING (0 0, 1 1)"},
      {.name = "multipoint", .wkt = "MULTIPOINT ((0 0), (1 1))"},
      {.name = "multilinestring",
       .wkt = "MULTILINESTRING ((0 0, 1 0), (0 1, 1 1))"},
      {.name = "mixed_collection",
       .wkt =
           "GEOMETRYCOLLECTION (POLYGON ((0 0, 1 0, 1 1, 0 0)), POINT (5 5))"},
  });

  for (const InvalidCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    std::unique_ptr<Geometry> geom = ReadWkt(tc.wkt);
    EXPECT_THROW(ConcaveHullOfPolygons(geom.get()), IllegalArgumentException);
  }
}

TEST_F(ConcaveHullOfPolygonsTest, ThrowsOnCurvedInputs) {
  constexpr auto kCurvedWkts = std::to_array<std::string_view>({
      "CIRCULARSTRING (0 0, 1 1, 2 0)",
      "CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0))",
      "MULTISURFACE (CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0)))",
  });

  for (std::string_view wkt : kCurvedWkts) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<Geometry> geom = ReadWkt(wkt);
    EXPECT_THROW(ConcaveHullOfPolygons(geom.get()),
                 UnsupportedOperationException);
  }
}

TEST_F(ConcaveHullOfPolygonsTest, ThrowsOnInvalidLengthParameters) {
  std::unique_ptr<Geometry> poly = ReadWkt(kTwoSquaresWkt);
  ConcaveHullOfPolygons hull(poly.get());

  EXPECT_THROW(hull.setMaximumEdgeLength(-0.001), IllegalArgumentException);
  EXPECT_THROW(hull.setMaximumEdgeLength(-10.0), IllegalArgumentException);
  EXPECT_THROW(hull.setMaximumEdgeLengthRatio(-0.1), IllegalArgumentException);
  EXPECT_THROW(hull.setMaximumEdgeLengthRatio(1.01), IllegalArgumentException);

  EXPECT_THROW(ConcaveHullOfPolygons::concaveHullByLength(poly.get(), -1.0),
               IllegalArgumentException);
  EXPECT_THROW(ConcaveHullOfPolygons::concaveFillByLength(poly.get(), -1.0),
               IllegalArgumentException);
  EXPECT_THROW(
      ConcaveHullOfPolygons::concaveHullByLengthRatio(poly.get(), -0.01),
      IllegalArgumentException);
  EXPECT_THROW(ConcaveHullOfPolygons::concaveHullByLengthRatio(poly.get(), 1.5),
               IllegalArgumentException);
  EXPECT_THROW(ConcaveHullOfPolygons::concaveFillByLengthRatio(poly.get(), 1.1),
               IllegalArgumentException);
}

TEST_F(ConcaveHullOfPolygonsTest,
       EmptyInputsReturnEmptyPolygonAndPreserveSrid) {
  PrecisionModel pm;
  GeometryFactory::Ptr custom_factory = GeometryFactory::create(&pm, 4326);
  WKTReader custom_reader(*custom_factory);

  constexpr auto kEmptyWkts = std::to_array<std::string_view>({
      "POLYGON EMPTY",
      "MULTIPOLYGON EMPTY",
      "GEOMETRYCOLLECTION EMPTY",
  });

  for (std::string_view wkt : kEmptyWkts) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<Geometry> empty_geom = custom_reader.read(std::string(wkt));

    std::unique_ptr<Geometry> hull_len =
        ConcaveHullOfPolygons::concaveHullByLength(empty_geom.get(), 5.0);
    EXPECT_TRUE(hull_len->isEmpty());
    EXPECT_EQ(hull_len->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_EQ(hull_len->getFactory(), custom_factory.get());
    EXPECT_EQ(hull_len->getSRID(), 4326);

    std::unique_ptr<Geometry> hull_ratio =
        ConcaveHullOfPolygons::concaveHullByLengthRatio(empty_geom.get(), 0.5);
    EXPECT_TRUE(hull_ratio->isEmpty());
    EXPECT_EQ(hull_ratio->getGeometryTypeId(), geos::geom::GEOS_POLYGON);

    std::unique_ptr<Geometry> fill_len =
        ConcaveHullOfPolygons::concaveFillByLength(empty_geom.get(), 5.0);
    EXPECT_TRUE(fill_len->isEmpty());
    EXPECT_EQ(fill_len->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_EQ(fill_len->getSRID(), 4326);

    std::unique_ptr<Geometry> fill_ratio =
        ConcaveHullOfPolygons::concaveFillByLengthRatio(empty_geom.get(), 0.5);
    EXPECT_TRUE(fill_ratio->isEmpty());
    EXPECT_EQ(fill_ratio->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  }
}

TEST_F(ConcaveHullOfPolygonsTest, ZeroAreaNonEmptyPolygonReturnsEmptyHull) {
  constexpr auto kZeroAreaWkts = std::to_array<std::string_view>({
      "POLYGON ((0 0, 2 0, 1 0, 0 0))",
      "MULTIPOLYGON (((0 0, 2 0, 1 0, 0 0)), ((5 0, 7 0, 6 0, 5 0)))",
  });

  for (std::string_view wkt : kZeroAreaWkts) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<Geometry> geom = ReadWkt(wkt);
    ASSERT_FALSE(geom->isEmpty());
    ASSERT_DOUBLE_EQ(geom->getArea(), 0.0);

    std::unique_ptr<Geometry> hull =
        ConcaveHullOfPolygons::concaveHullByLength(geom.get(), 5.0);
    EXPECT_TRUE(hull->isEmpty());
    EXPECT_EQ(hull->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  }
}

TEST_F(ConcaveHullOfPolygonsTest, DefaultInstanceParametersWithoutSetters) {
  std::unique_ptr<Geometry> input = ReadWkt(kTwoSquaresWkt);

  ConcaveHullOfPolygons hull_builder(input.get());
  std::unique_ptr<Geometry> hull = hull_builder.getHull();
  EXPECT_TRUE(hull->isValid());
  EXPECT_TRUE(hull->equals(input.get()));
  EXPECT_DOUBLE_EQ(hull->getArea(), 200.0);

  ConcaveHullOfPolygons fill_builder(input.get());
  std::unique_ptr<Geometry> fill = fill_builder.getFill();
  EXPECT_TRUE(fill->isEmpty());
  EXPECT_EQ(fill->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
}

TEST_F(ConcaveHullOfPolygonsTest, ParameterSetterOverridePrecedence) {
  std::unique_ptr<Geometry> input = ReadWkt(kTwoSquaresWkt);

  // setMaximumEdgeLength clears any previously configured ratio.
  ConcaveHullOfPolygons len_after_ratio(input.get());
  len_after_ratio.setMaximumEdgeLengthRatio(1.0);
  len_after_ratio.setMaximumEdgeLength(1.5);
  std::unique_ptr<Geometry> hull_len = len_after_ratio.getHull();
  EXPECT_TRUE(hull_len->equals(input.get()));
  EXPECT_DOUBLE_EQ(hull_len->getArea(), 200.0);

  // setMaximumEdgeLengthRatio overrides a previously configured max length.
  ConcaveHullOfPolygons ratio_after_len(input.get());
  ratio_after_len.setMaximumEdgeLength(100.0);
  ratio_after_len.setMaximumEdgeLengthRatio(0.0);
  std::unique_ptr<Geometry> hull_ratio = ratio_after_len.getHull();
  EXPECT_TRUE(hull_ratio->equals(input.get()));
  EXPECT_DOUBLE_EQ(hull_ratio->getArea(), 200.0);
}

TEST_F(ConcaveHullOfPolygonsTest, SingleConvexPolygonUnchangedAndEmptyFill) {
  std::unique_ptr<Geometry> square =
      ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");

  for (double len : {0.0, 5.0, 100.0}) {
    SCOPED_TRACE(len);
    std::unique_ptr<Geometry> hull =
        ConcaveHullOfPolygons::concaveHullByLength(square.get(), len);
    EXPECT_TRUE(hull->isValid());
    EXPECT_TRUE(hull->equals(square.get()));
    EXPECT_DOUBLE_EQ(hull->getArea(), 100.0);

    std::unique_ptr<Geometry> fill =
        ConcaveHullOfPolygons::concaveFillByLength(square.get(), len);
    EXPECT_TRUE(fill->isEmpty());
    EXPECT_EQ(fill->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  }

  for (double ratio : {0.0, 0.5, 1.0}) {
    SCOPED_TRACE(ratio);
    std::unique_ptr<Geometry> hull =
        ConcaveHullOfPolygons::concaveHullByLengthRatio(square.get(), ratio);
    EXPECT_TRUE(hull->isValid());
    EXPECT_TRUE(hull->equals(square.get()));
    EXPECT_DOUBLE_EQ(hull->getArea(), 100.0);

    std::unique_ptr<Geometry> fill =
        ConcaveHullOfPolygons::concaveFillByLengthRatio(square.get(), ratio);
    EXPECT_TRUE(fill->isEmpty());
    EXPECT_EQ(fill->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  }
}

TEST_F(ConcaveHullOfPolygonsTest, SingleConcaveLShapeLengthAndTightness) {
  std::unique_ptr<Geometry> l_poly = ReadWkt(kLPolyWkt);
  std::unique_ptr<Geometry> convex = l_poly->convexHull();
  ASSERT_DOUBLE_EQ(l_poly->getArea(), 36.0);
  ASSERT_DOUBLE_EQ(convex->getArea(), 68.0);

  // Large length + non-tight bridges the notch to produce the convex hull.
  std::unique_ptr<Geometry> hull_loose =
      ConcaveHullOfPolygons::concaveHullByLength(l_poly.get(), 20.0, false,
                                                 false);
  EXPECT_TRUE(hull_loose->isValid());
  EXPECT_TRUE(hull_loose->equals(convex.get()));
  EXPECT_DOUBLE_EQ(hull_loose->getArea(), 68.0);

  // Tight mode removes tris touching a single polygon even at large max length.
  std::unique_ptr<Geometry> hull_tight =
      ConcaveHullOfPolygons::concaveHullByLength(l_poly.get(), 20.0, true,
                                                 false);
  EXPECT_TRUE(hull_tight->isValid());
  EXPECT_TRUE(hull_tight->equals(l_poly.get()));
  EXPECT_DOUBLE_EQ(hull_tight->getArea(), 36.0);

  // Short length erodes the notch even when not tight.
  std::unique_ptr<Geometry> hull_short =
      ConcaveHullOfPolygons::concaveHullByLength(l_poly.get(), 5.0, false,
                                                 false);
  EXPECT_TRUE(hull_short->equals(l_poly.get()));
  EXPECT_DOUBLE_EQ(hull_short->getArea(), 36.0);

  // Length ratios 1.0 (loose vs tight) and 0.0.
  std::unique_ptr<Geometry> ratio_one_loose =
      ConcaveHullOfPolygons::concaveHullByLengthRatio(l_poly.get(), 1.0, false,
                                                      false);
  EXPECT_TRUE(ratio_one_loose->equals(convex.get()));
  EXPECT_DOUBLE_EQ(ratio_one_loose->getArea(), 68.0);

  std::unique_ptr<Geometry> ratio_one_tight =
      ConcaveHullOfPolygons::concaveHullByLengthRatio(l_poly.get(), 1.0, true,
                                                      false);
  EXPECT_TRUE(ratio_one_tight->equals(l_poly.get()));
  EXPECT_DOUBLE_EQ(ratio_one_tight->getArea(), 36.0);

  std::unique_ptr<Geometry> ratio_zero =
      ConcaveHullOfPolygons::concaveHullByLengthRatio(l_poly.get(), 0.0);
  EXPECT_TRUE(ratio_zero->equals(l_poly.get()));

  // getFill forces isTight = true, so a single concave polygon has empty fill.
  EXPECT_TRUE(ConcaveHullOfPolygons::concaveFillByLength(l_poly.get(), 20.0)
                  ->isEmpty());
  EXPECT_TRUE(ConcaveHullOfPolygons::concaveFillByLengthRatio(l_poly.get(), 1.0)
                  ->isEmpty());
}

TEST_F(ConcaveHullOfPolygonsTest, FunnelBayPartialConcavityByLength) {
  std::unique_ptr<Geometry> funnel = ReadWkt(kFunnelBayWkt);
  ASSERT_DOUBLE_EQ(funnel->getArea(), 99.0);

  // Upper bay edges are >= 8.0; lower pocket edges are <= 4.243.
  std::unique_ptr<Geometry> hull_full =
      ConcaveHullOfPolygons::concaveHullByLength(funnel.get(), 10.0, false,
                                                 false);
  EXPECT_TRUE(hull_full->isValid());
  EXPECT_DOUBLE_EQ(hull_full->getArea(), 144.0);

  // maxLength = 5.0 erodes the upper trapezoid (area 36) and retains the lower
  // pocket (area 9).
  std::unique_ptr<Geometry> hull_partial =
      ConcaveHullOfPolygons::concaveHullByLength(funnel.get(), 5.0, false,
                                                 false);
  EXPECT_TRUE(hull_partial->isValid());
  EXPECT_TRUE(hull_partial->covers(funnel.get()));
  EXPECT_DOUBLE_EQ(hull_partial->getArea(), 108.0);

  // Tight mode at maxLength = 5.0 removes the lower pocket too.
  std::unique_ptr<Geometry> hull_tight =
      ConcaveHullOfPolygons::concaveHullByLength(funnel.get(), 5.0, true,
                                                 false);
  EXPECT_TRUE(hull_tight->isValid());
  EXPECT_TRUE(hull_tight->equals(funnel.get()));
  EXPECT_DOUBLE_EQ(hull_tight->getArea(), 99.0);

  // maxLength = 1.5 erodes the entire bay by length alone.
  std::unique_ptr<Geometry> hull_eroded =
      ConcaveHullOfPolygons::concaveHullByLength(funnel.get(), 1.5, false,
                                                 false);
  EXPECT_TRUE(hull_eroded->equals(funnel.get()));
  EXPECT_DOUBLE_EQ(hull_eroded->getArea(), 99.0);

  std::unique_ptr<Geometry> hull_ratio =
      ConcaveHullOfPolygons::concaveHullByLengthRatio(funnel.get(), 0.5, false,
                                                      false);
  EXPECT_TRUE(hull_ratio->isValid());
  EXPECT_TRUE(hull_ratio->covers(funnel.get()));
  EXPECT_GE(hull_ratio->getArea(), 99.0);
  EXPECT_LE(hull_ratio->getArea(), 144.0);
}

TEST_F(ConcaveHullOfPolygonsTest, TwoSquaresByLengthAndFill) {
  std::unique_ptr<Geometry> input = ReadWkt(kTwoSquaresWkt);
  std::unique_ptr<Geometry> expected_full =
      ReadWkt("POLYGON ((0 0, 22 0, 22 10, 0 10, 0 0))");
  std::unique_ptr<Geometry> expected_gap =
      ReadWkt("POLYGON ((10 0, 12 0, 12 10, 10 10, 10 0))");

  // Gap width is 2.0: lengths < 2.0 leave squares disconnected and fill empty.
  for (double len : {0.0, 1.9}) {
    SCOPED_TRACE(len);
    std::unique_ptr<Geometry> hull =
        ConcaveHullOfPolygons::concaveHullByLength(input.get(), len);
    EXPECT_TRUE(hull->isValid());
    EXPECT_TRUE(hull->equals(input.get()));
    EXPECT_DOUBLE_EQ(hull->getArea(), 200.0);

    std::unique_ptr<Geometry> fill =
        ConcaveHullOfPolygons::concaveFillByLength(input.get(), len);
    EXPECT_TRUE(fill->isEmpty());
  }

  // maxLength = 2.5 bridges the 2.0 gap while preserving bridging tris even
  // when isTight = true.
  for (bool is_tight : {false, true}) {
    SCOPED_TRACE(is_tight);
    std::unique_ptr<Geometry> hull = ConcaveHullOfPolygons::concaveHullByLength(
        input.get(), 2.5, is_tight, false);
    EXPECT_TRUE(hull->isValid());
    EXPECT_EQ(hull->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(hull->equals(expected_full.get()));
    EXPECT_DOUBLE_EQ(hull->getArea(), 220.0);
  }

  std::unique_ptr<Geometry> fill =
      ConcaveHullOfPolygons::concaveFillByLength(input.get(), 2.5);
  EXPECT_TRUE(fill->isValid());
  EXPECT_EQ(fill->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(fill->equals(expected_gap.get()));
  EXPECT_DOUBLE_EQ(fill->getArea(), 20.0);
}

TEST_F(ConcaveHullOfPolygonsTest, TwoSquaresByLengthRatioAndFillRatio) {
  std::unique_ptr<Geometry> input = ReadWkt(kTwoSquaresWkt);
  std::unique_ptr<Geometry> expected_full =
      ReadWkt("POLYGON ((0 0, 22 0, 22 10, 0 10, 0 0))");
  std::unique_ptr<Geometry> expected_gap =
      ReadWkt("POLYGON ((10 0, 12 0, 12 10, 10 10, 10 0))");

  std::unique_ptr<Geometry> hull_zero =
      ConcaveHullOfPolygons::concaveHullByLengthRatio(input.get(), 0.0);
  EXPECT_TRUE(hull_zero->equals(input.get()));
  EXPECT_TRUE(ConcaveHullOfPolygons::concaveFillByLengthRatio(input.get(), 0.0)
                  ->isEmpty());

  for (double ratio : {0.5, 1.0}) {
    SCOPED_TRACE(ratio);
    std::unique_ptr<Geometry> hull =
        ConcaveHullOfPolygons::concaveHullByLengthRatio(input.get(), ratio);
    EXPECT_TRUE(hull->isValid());
    EXPECT_TRUE(hull->equals(expected_full.get()));
    EXPECT_DOUBLE_EQ(hull->getArea(), 220.0);

    std::unique_ptr<Geometry> fill =
        ConcaveHullOfPolygons::concaveFillByLengthRatio(input.get(), ratio);
    EXPECT_TRUE(fill->isValid());
    EXPECT_TRUE(fill->equals(expected_gap.get()));
    EXPECT_DOUBLE_EQ(fill->getArea(), 20.0);
  }
}

TEST_F(ConcaveHullOfPolygonsTest, TightBridgingPreservesBridgeAndTrimsNotch) {
  std::unique_ptr<Geometry> input = ReadWkt(kLeftSquarePlusLPolyWkt);
  ASSERT_DOUBLE_EQ(input->getArea(), 56.0);

  // maxLength = 5.0 bridges only the [-2,0]x[0,10] gap (area 20).
  std::unique_ptr<Geometry> fill_short =
      ConcaveHullOfPolygons::concaveFillByLength(input.get(), 5.0);
  EXPECT_TRUE(fill_short->isValid());
  EXPECT_DOUBLE_EQ(fill_short->getArea(), 20.0);

  std::unique_ptr<Geometry> hull_short =
      ConcaveHullOfPolygons::concaveHullByLength(input.get(), 5.0, false,
                                                 false);
  EXPECT_TRUE(hull_short->isValid());
  EXPECT_DOUBLE_EQ(hull_short->getArea(), 76.0);

  // At maxLength = 20.0, non-tight fills both the gap (area 20) and the L-notch
  // (area 32) => 108, while tight mode trims triangles touching only the
  // L-shape and keeps the bridging triangles across [-2,0]x[0,10] => 76.
  std::unique_ptr<Geometry> hull_loose =
      ConcaveHullOfPolygons::concaveHullByLength(input.get(), 20.0, false,
                                                 false);
  std::unique_ptr<Geometry> hull_tight =
      ConcaveHullOfPolygons::concaveHullByLength(input.get(), 20.0, true,
                                                 false);
  EXPECT_TRUE(hull_loose->isValid());
  EXPECT_TRUE(hull_tight->isValid());
  EXPECT_TRUE(hull_tight->covers(input.get()));
  EXPECT_TRUE(hull_loose->covers(hull_tight.get()));
  EXPECT_DOUBLE_EQ(hull_loose->getArea(), 108.0);
  EXPECT_DOUBLE_EQ(hull_tight->getArea(), 76.0);
}

TEST_F(ConcaveHullOfPolygonsTest, SingleHoleRingAllowsAndDisallowsHoles) {
  std::unique_ptr<MultiPolygon> ring =
      MakeSquareRing(kSingleHoleRingOrigins, 4.0);
  ASSERT_DOUBLE_EQ(ring->getArea(), 256.0);

  // Without holes allowed at maxLength = 4.5, the entire [0,24]x[0,24] box is
  // filled.
  std::unique_ptr<Geometry> hull_no_holes =
      ConcaveHullOfPolygons::concaveHullByLength(ring.get(), 4.5, false, false);
  ASSERT_EQ(hull_no_holes->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  const auto* poly_no_holes = dynamic_cast<const Polygon*>(hull_no_holes.get());
  ASSERT_NE(poly_no_holes, nullptr);
  EXPECT_TRUE(poly_no_holes->isValid());
  EXPECT_EQ(poly_no_holes->getNumInteriorRing(), 0u);
  EXPECT_DOUBLE_EQ(poly_no_holes->getArea(), 576.0);

  // With holes allowed at maxLength = 4.5, the central courtyard is opened as
  // one interior ring (chamfered 16x16 hole of area 254 => hull area 322).
  std::unique_ptr<Geometry> hull_with_hole =
      ConcaveHullOfPolygons::concaveHullByLength(ring.get(), 4.5, false, true);
  ASSERT_EQ(hull_with_hole->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  const auto* poly_with_hole =
      dynamic_cast<const Polygon*>(hull_with_hole.get());
  ASSERT_NE(poly_with_hole, nullptr);
  EXPECT_TRUE(poly_with_hole->isValid());
  EXPECT_TRUE(poly_with_hole->covers(ring.get()));
  EXPECT_EQ(poly_with_hole->getNumInteriorRing(), 1u);
  EXPECT_DOUBLE_EQ(poly_with_hole->getArea(), 322.0);

  // With holes allowed but maxLength larger than courtyard chords, no hole seed
  // exists and no hole is opened.
  std::unique_ptr<Geometry> hull_large_len =
      ConcaveHullOfPolygons::concaveHullByLength(ring.get(), 30.0, false, true);
  ASSERT_EQ(hull_large_len->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  const auto* poly_large_len =
      dynamic_cast<const Polygon*>(hull_large_len.get());
  ASSERT_NE(poly_large_len, nullptr);
  EXPECT_EQ(poly_large_len->getNumInteriorRing(), 0u);
  EXPECT_DOUBLE_EQ(poly_large_len->getArea(), 576.0);

  // Ratio-based hole control.
  std::unique_ptr<Geometry> ratio_no_hole =
      ConcaveHullOfPolygons::concaveHullByLengthRatio(ring.get(), 0.25, false,
                                                      false);
  const auto* poly_ratio_no_hole =
      dynamic_cast<const Polygon*>(ratio_no_hole.get());
  ASSERT_NE(poly_ratio_no_hole, nullptr);
  EXPECT_EQ(poly_ratio_no_hole->getNumInteriorRing(), 0u);

  std::unique_ptr<Geometry> ratio_with_hole =
      ConcaveHullOfPolygons::concaveHullByLengthRatio(ring.get(), 0.25, false,
                                                      true);
  const auto* poly_ratio_with_hole =
      dynamic_cast<const Polygon*>(ratio_with_hole.get());
  ASSERT_NE(poly_ratio_with_hole, nullptr);
  EXPECT_TRUE(poly_ratio_with_hole->isValid());
  EXPECT_EQ(poly_ratio_with_hole->getNumInteriorRing(), 1u);
  EXPECT_LT(poly_ratio_with_hole->getArea(), 400.0);

  std::unique_ptr<Geometry> ratio_one_with_hole =
      ConcaveHullOfPolygons::concaveHullByLengthRatio(ring.get(), 1.0, false,
                                                      true);
  const auto* poly_ratio_one =
      dynamic_cast<const Polygon*>(ratio_one_with_hole.get());
  ASSERT_NE(poly_ratio_one, nullptr);
  EXPECT_EQ(poly_ratio_one->getNumInteriorRing(), 0u);
  EXPECT_DOUBLE_EQ(poly_ratio_one->getArea(), 576.0);

  // Concave fill without holes vs with holes enabled via setter.
  // Without holes, the filled courtyard glues all 16 gaps into one polygon;
  // with the courtyard opened as a hole, the 16 input squares disconnect the
  // fill into 12 components (4 L-shaped corners + 8 middle 1x4 gaps).
  std::unique_ptr<Geometry> courtyard_center = ReadWkt("POINT (12 12)");
  std::unique_ptr<Geometry> fill_no_holes =
      ConcaveHullOfPolygons::concaveFillByLength(ring.get(), 4.5);
  ASSERT_EQ(fill_no_holes->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  const auto* fill_closed_poly =
      dynamic_cast<const Polygon*>(fill_no_holes.get());
  ASSERT_NE(fill_closed_poly, nullptr);
  EXPECT_TRUE(fill_closed_poly->isValid());
  EXPECT_EQ(fill_closed_poly->getNumInteriorRing(), 0u);
  EXPECT_TRUE(fill_closed_poly->covers(courtyard_center.get()));
  EXPECT_DOUBLE_EQ(fill_closed_poly->getArea(), 320.0);

  ConcaveHullOfPolygons fill_with_hole_builder(ring.get());
  fill_with_hole_builder.setMaximumEdgeLength(4.5);
  fill_with_hole_builder.setHolesAllowed(true);
  std::unique_ptr<Geometry> fill_with_hole = fill_with_hole_builder.getFill();
  ASSERT_EQ(fill_with_hole->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  const auto* fill_mp = dynamic_cast<const MultiPolygon*>(fill_with_hole.get());
  ASSERT_NE(fill_mp, nullptr);
  EXPECT_TRUE(fill_mp->isValid());
  EXPECT_EQ(fill_mp->getNumGeometries(), 12u);
  EXPECT_FALSE(fill_mp->intersects(courtyard_center.get()));
  EXPECT_TRUE(poly_with_hole->covers(fill_mp));
  EXPECT_DOUBLE_EQ(fill_mp->getArea(), 66.0);
}

TEST_F(ConcaveHullOfPolygonsTest, TwoHoleRingOpensMultipleHoles) {
  std::unique_ptr<MultiPolygon> two_hole_ring =
      MakeSquareRing(kTwoHoleRingOrigins, 4.0);
  ASSERT_DOUBLE_EQ(two_hole_ring->getArea(), 432.0);

  std::unique_ptr<Geometry> hull_closed =
      ConcaveHullOfPolygons::concaveHullByLength(two_hole_ring.get(), 4.5,
                                                 false, false);
  const auto* poly_closed = dynamic_cast<const Polygon*>(hull_closed.get());
  ASSERT_NE(poly_closed, nullptr);
  EXPECT_TRUE(poly_closed->isValid());
  EXPECT_EQ(poly_closed->getNumInteriorRing(), 0u);
  EXPECT_DOUBLE_EQ(poly_closed->getArea(), 1056.0);

  std::unique_ptr<Geometry> hull_two_holes =
      ConcaveHullOfPolygons::concaveHullByLength(two_hole_ring.get(), 4.5,
                                                 false, true);
  const auto* poly_two_holes =
      dynamic_cast<const Polygon*>(hull_two_holes.get());
  ASSERT_NE(poly_two_holes, nullptr);
  EXPECT_TRUE(poly_two_holes->isValid());
  EXPECT_TRUE(poly_two_holes->covers(two_hole_ring.get()));
  EXPECT_EQ(poly_two_holes->getNumInteriorRing(), 2u);
  EXPECT_DOUBLE_EQ(poly_two_holes->getArea(), 548.0);

  std::unique_ptr<Geometry> left_center = ReadWkt("POINT (12 12)");
  std::unique_ptr<Geometry> right_center = ReadWkt("POINT (32 12)");

  // Without holes allowed in fill, the 3 interior divider squares become
  // 3 interior rings of a single fill polygon covering both courtyards.
  std::unique_ptr<Geometry> fill_closed =
      ConcaveHullOfPolygons::concaveFillByLength(two_hole_ring.get(), 4.5);
  ASSERT_EQ(fill_closed->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  const auto* fill_closed_poly =
      dynamic_cast<const Polygon*>(fill_closed.get());
  ASSERT_NE(fill_closed_poly, nullptr);
  EXPECT_TRUE(fill_closed_poly->isValid());
  EXPECT_EQ(fill_closed_poly->getNumInteriorRing(), 3u);
  EXPECT_TRUE(fill_closed_poly->covers(left_center.get()));
  EXPECT_TRUE(fill_closed_poly->covers(right_center.get()));
  EXPECT_DOUBLE_EQ(fill_closed_poly->getArea(), 624.0);

  // With holes allowed in fill, opening both courtyards splits the fill into
  // 20 disjoint components (4 corners + 2 T-junctions + 14 middle 1x4 gaps).
  ConcaveHullOfPolygons fill_builder(two_hole_ring.get());
  fill_builder.setMaximumEdgeLength(4.5);
  fill_builder.setHolesAllowed(true);
  std::unique_ptr<Geometry> fill = fill_builder.getFill();
  ASSERT_EQ(fill->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  const auto* fill_mp = dynamic_cast<const MultiPolygon*>(fill.get());
  ASSERT_NE(fill_mp, nullptr);
  EXPECT_TRUE(fill_mp->isValid());
  EXPECT_EQ(fill_mp->getNumGeometries(), 20u);
  EXPECT_FALSE(fill_mp->intersects(left_center.get()));
  EXPECT_FALSE(fill_mp->intersects(right_center.get()));
  EXPECT_TRUE(poly_two_holes->covers(fill_mp));
  EXPECT_DOUBLE_EQ(fill_mp->getArea(), 116.0);
}

TEST_F(ConcaveHullOfPolygonsTest, PolygonsWithInteriorHolesPreserveHoles) {
  std::unique_ptr<Geometry> input = ReadWkt(
      "MULTIPOLYGON ("
      "((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 8, 8 8, 8 2, 2 2)), "
      "((12 0, 22 0, 22 10, 12 10, 12 0), (14 2, 14 8, 20 8, 20 2, 14 2)))");
  ASSERT_DOUBLE_EQ(input->getArea(), 128.0);

  std::unique_ptr<Geometry> hull =
      ConcaveHullOfPolygons::concaveHullByLength(input.get(), 3.0);
  ASSERT_EQ(hull->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  const auto* poly = dynamic_cast<const Polygon*>(hull.get());
  ASSERT_NE(poly, nullptr);
  EXPECT_TRUE(poly->isValid());
  EXPECT_TRUE(poly->covers(input.get()));
  EXPECT_EQ(poly->getNumInteriorRing(), 2u);
  EXPECT_DOUBLE_EQ(poly->getArea(), 148.0);

  std::unique_ptr<Geometry> fill =
      ConcaveHullOfPolygons::concaveFillByLength(input.get(), 3.0);
  EXPECT_TRUE(fill->isValid());
  EXPECT_EQ(fill->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(fill->getArea(), 20.0);
}

TEST_F(ConcaveHullOfPolygonsTest, NestedIslandInsideHoleHandledCleanly) {
  std::unique_ptr<Geometry> input = ReadWkt(
      "MULTIPOLYGON ("
      "((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 8, 8 8, 8 2, 2 2)), "
      "((4 4, 6 4, 6 6, 4 6, 4 4)))");
  ASSERT_DOUBLE_EQ(input->getArea(), 68.0);

  std::unique_ptr<Geometry> hull =
      ConcaveHullOfPolygons::concaveHullByLength(input.get(), 5.0);
  EXPECT_TRUE(hull->isValid());
  EXPECT_TRUE(hull->equals(input.get()));
  EXPECT_DOUBLE_EQ(hull->getArea(), 68.0);

  std::unique_ptr<Geometry> fill =
      ConcaveHullOfPolygons::concaveFillByLength(input.get(), 5.0);
  EXPECT_TRUE(fill->isEmpty());
}

TEST_F(ConcaveHullOfPolygonsTest, GeometryCollectionOfPolygonsSupported) {
  std::vector<std::unique_ptr<Geometry>> parts;
  parts.push_back(ReadWkt("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))"));
  parts.push_back(ReadWkt("POLYGON ((6 0, 10 0, 10 4, 6 4, 6 0))"));
  std::unique_ptr<GeometryCollection> gc =
      factory_->createGeometryCollection(std::move(parts));

  std::unique_ptr<Geometry> hull =
      ConcaveHullOfPolygons::concaveHullByLength(gc.get(), 3.0);
  EXPECT_TRUE(hull->isValid());
  EXPECT_EQ(hull->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(hull->getArea(), 40.0);

  std::unique_ptr<Geometry> fill =
      ConcaveHullOfPolygons::concaveFillByLength(gc.get(), 3.0);
  EXPECT_TRUE(fill->isValid());
  EXPECT_EQ(fill->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(fill->getArea(), 8.0);

  // Gap of width 2.0 is unbridged at maxLength = 1.0, returning an identical
  // clone of the input GeometryCollection and an empty fill.
  std::unique_ptr<Geometry> unbridged_hull =
      ConcaveHullOfPolygons::concaveHullByLength(gc.get(), 1.0);
  EXPECT_TRUE(unbridged_hull->equals(gc.get()));
  EXPECT_DOUBLE_EQ(unbridged_hull->getArea(), 32.0);

  std::unique_ptr<Geometry> unbridged_fill =
      ConcaveHullOfPolygons::concaveFillByLength(gc.get(), 1.0);
  EXPECT_TRUE(unbridged_fill->isEmpty());

  std::unique_ptr<Geometry> ratio_zero_hull =
      ConcaveHullOfPolygons::concaveHullByLengthRatio(gc.get(), 0.0);
  EXPECT_TRUE(ratio_zero_hull->equals(gc.get()));
  EXPECT_TRUE(ConcaveHullOfPolygons::concaveFillByLengthRatio(gc.get(), 0.0)
                  ->isEmpty());

  std::unique_ptr<Geometry> ratio_one_hull =
      ConcaveHullOfPolygons::concaveHullByLengthRatio(gc.get(), 1.0);
  EXPECT_EQ(ratio_one_hull->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(ratio_one_hull->getArea(), 40.0);

  std::unique_ptr<Geometry> ratio_one_fill =
      ConcaveHullOfPolygons::concaveFillByLengthRatio(gc.get(), 1.0);
  EXPECT_EQ(ratio_one_fill->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(ratio_one_fill->getArea(), 8.0);
}

TEST_F(ConcaveHullOfPolygonsTest,
       TightAndHolesAllowedTogetherByLengthAndRatio) {
  // Replace square (10, 0) in the 16-square ring with a U-shaped polygon that
  // protrudes down to y = -3 and has a 2x3 outward notch [11,13]x[-3,0].
  std::vector<std::unique_ptr<Polygon>> polys;
  polys.reserve(kSingleHoleRingOrigins.size());
  for (const GridOrigin& origin : kSingleHoleRingOrigins) {
    if (origin.x == 10 && origin.y == 0) {
      std::unique_ptr<Geometry> u_geom = ReadWkt(
          "POLYGON ((10 -3, 11 -3, 11 0, 13 0, 13 -3, 14 -3, 14 4, "
          "10 4, 10 -3))");
      auto* u_poly = dynamic_cast<Polygon*>(u_geom.get());
      ASSERT_NE(u_poly, nullptr);
      u_geom.release();
      polys.emplace_back(u_poly);
      continue;
    }
    CoordinateSequence seq{
        Coordinate(origin.x, origin.y),
        Coordinate(origin.x + 4.0, origin.y),
        Coordinate(origin.x + 4.0, origin.y + 4.0),
        Coordinate(origin.x, origin.y + 4.0),
        Coordinate(origin.x, origin.y),
    };
    polys.push_back(factory_->createPolygon(std::move(seq)));
  }
  std::unique_ptr<MultiPolygon> ring_with_u =
      factory_->createMultiPolygon(std::move(polys));
  ASSERT_DOUBLE_EQ(ring_with_u->getArea(), 262.0);

  std::unique_ptr<Geometry> notch_center = ReadWkt("POINT (12 -1.5)");
  std::unique_ptr<Geometry> courtyard_center = ReadWkt("POINT (12 12)");

  // Compare all 4 (isTight, isHolesAllowed) combinations at maxLength = 4.5.
  std::unique_ptr<Geometry> loose_closed =
      ConcaveHullOfPolygons::concaveHullByLength(ring_with_u.get(), 4.5, false,
                                                 false);
  std::unique_ptr<Geometry> tight_closed =
      ConcaveHullOfPolygons::concaveHullByLength(ring_with_u.get(), 4.5, true,
                                                 false);
  std::unique_ptr<Geometry> loose_hole =
      ConcaveHullOfPolygons::concaveHullByLength(ring_with_u.get(), 4.5, false,
                                                 true);
  std::unique_ptr<Geometry> tight_hole =
      ConcaveHullOfPolygons::concaveHullByLength(ring_with_u.get(), 4.5, true,
                                                 true);

  const auto* p_loose_closed = dynamic_cast<const Polygon*>(loose_closed.get());
  const auto* p_tight_closed = dynamic_cast<const Polygon*>(tight_closed.get());
  const auto* p_loose_hole = dynamic_cast<const Polygon*>(loose_hole.get());
  const auto* p_tight_hole = dynamic_cast<const Polygon*>(tight_hole.get());
  ASSERT_NE(p_loose_closed, nullptr);
  ASSERT_NE(p_tight_closed, nullptr);
  ASSERT_NE(p_loose_hole, nullptr);
  ASSERT_NE(p_tight_hole, nullptr);

  EXPECT_TRUE(p_loose_closed->isValid());
  EXPECT_TRUE(p_tight_closed->isValid());
  EXPECT_TRUE(p_loose_hole->isValid());
  EXPECT_TRUE(p_tight_hole->isValid());

  EXPECT_EQ(p_loose_closed->getNumInteriorRing(), 0u);
  EXPECT_EQ(p_tight_closed->getNumInteriorRing(), 0u);
  EXPECT_EQ(p_loose_hole->getNumInteriorRing(), 1u);
  EXPECT_EQ(p_tight_hole->getNumInteriorRing(), 1u);

  EXPECT_DOUBLE_EQ(p_loose_closed->getArea(), 591.0);
  EXPECT_DOUBLE_EQ(p_tight_closed->getArea(), 585.0);
  EXPECT_DOUBLE_EQ(p_loose_hole->getArea(), 337.0);
  EXPECT_DOUBLE_EQ(p_tight_hole->getArea(), 331.0);

  EXPECT_TRUE(p_loose_hole->covers(notch_center.get()));
  EXPECT_FALSE(p_tight_hole->intersects(notch_center.get()));
  EXPECT_FALSE(p_tight_hole->intersects(courtyard_center.get()));
  EXPECT_TRUE(p_tight_hole->covers(ring_with_u.get()));
  EXPECT_TRUE(p_loose_hole->covers(p_tight_hole));

  // Verify the same tightness + hole combination via length ratio.
  std::unique_ptr<Geometry> ratio_loose_hole =
      ConcaveHullOfPolygons::concaveHullByLengthRatio(ring_with_u.get(), 0.25,
                                                      false, true);
  std::unique_ptr<Geometry> ratio_tight_hole =
      ConcaveHullOfPolygons::concaveHullByLengthRatio(ring_with_u.get(), 0.25,
                                                      true, true);
  const auto* p_ratio_loose =
      dynamic_cast<const Polygon*>(ratio_loose_hole.get());
  const auto* p_ratio_tight =
      dynamic_cast<const Polygon*>(ratio_tight_hole.get());
  ASSERT_NE(p_ratio_loose, nullptr);
  ASSERT_NE(p_ratio_tight, nullptr);
  EXPECT_TRUE(p_ratio_loose->isValid());
  EXPECT_TRUE(p_ratio_tight->isValid());
  EXPECT_EQ(p_ratio_loose->getNumInteriorRing(), 1u);
  EXPECT_EQ(p_ratio_tight->getNumInteriorRing(), 1u);
  EXPECT_TRUE(p_ratio_loose->covers(notch_center.get()));
  EXPECT_FALSE(p_ratio_tight->intersects(notch_center.get()));
  EXPECT_LT(p_ratio_tight->getArea(), p_ratio_loose->getArea());
}

TEST_F(ConcaveHullOfPolygonsTest,
       TouchingPolygonsAtSingleVertexBridgesCorners) {
  std::unique_ptr<Geometry> input = ReadWkt(
      "MULTIPOLYGON (((0 0, 5 0, 5 5, 0 5, 0 0)), "
      "((5 5, 10 5, 10 10, 5 10, 5 5)))");
  std::unique_ptr<Geometry> expected_convex =
      ReadWkt("POLYGON ((0 0, 5 0, 10 5, 10 10, 5 10, 0 5, 0 0))");
  ASSERT_DOUBLE_EQ(input->getArea(), 50.0);

  // Diagonal across both corners is sqrt(50) ~= 7.071.
  std::unique_ptr<Geometry> hull_unbridged =
      ConcaveHullOfPolygons::concaveHullByLength(input.get(), 6.0);
  EXPECT_TRUE(hull_unbridged->isValid());
  EXPECT_TRUE(hull_unbridged->equals(input.get()));
  EXPECT_TRUE(
      ConcaveHullOfPolygons::concaveFillByLength(input.get(), 6.0)->isEmpty());

  for (bool is_tight : {false, true}) {
    SCOPED_TRACE(is_tight);
    std::unique_ptr<Geometry> hull = ConcaveHullOfPolygons::concaveHullByLength(
        input.get(), 8.0, is_tight, false);
    EXPECT_TRUE(hull->isValid());
    EXPECT_TRUE(hull->equals(expected_convex.get()));
    EXPECT_DOUBLE_EQ(hull->getArea(), 75.0);
  }

  std::unique_ptr<Geometry> fill =
      ConcaveHullOfPolygons::concaveFillByLength(input.get(), 8.0);
  EXPECT_TRUE(fill->isValid());
  EXPECT_DOUBLE_EQ(fill->getArea(), 25.0);

  EXPECT_TRUE(ConcaveHullOfPolygons::concaveHullByLengthRatio(input.get(), 0.0)
                  ->equals(input.get()));
  EXPECT_TRUE(ConcaveHullOfPolygons::concaveHullByLengthRatio(input.get(), 1.0)
                  ->equals(expected_convex.get()));
}

TEST_F(ConcaveHullOfPolygonsTest,
       InfiniteMaxLengthMatchesConvexHullWhileFillRemainsTight) {
  std::unique_ptr<Geometry> input = ReadWkt(kLeftSquarePlusLPolyWkt);
  const double inf = std::numeric_limits<double>::infinity();

  std::unique_ptr<Geometry> hull_loose =
      ConcaveHullOfPolygons::concaveHullByLength(input.get(), inf, false,
                                                 false);
  EXPECT_TRUE(hull_loose->isValid());
  EXPECT_DOUBLE_EQ(hull_loose->getArea(), 108.0);

  std::unique_ptr<Geometry> hull_tight =
      ConcaveHullOfPolygons::concaveHullByLength(input.get(), inf, true, false);
  EXPECT_TRUE(hull_tight->isValid());
  EXPECT_DOUBLE_EQ(hull_tight->getArea(), 76.0);

  ConcaveHullOfPolygons fill_builder(input.get());
  fill_builder.setMaximumEdgeLength(inf);
  std::unique_ptr<Geometry> fill = fill_builder.getFill();
  EXPECT_TRUE(fill->isValid());
  EXPECT_DOUBLE_EQ(fill->getArea(), 20.0);
}

TEST_F(ConcaveHullOfPolygonsTest, ThreeDimensionalPolygonsSupported) {
  std::unique_ptr<Geometry> input = ReadWkt(
      "MULTIPOLYGON Z (((0 0 5, 4 0 5, 4 4 5, 0 4 5, 0 0 5)), "
      "((6 0 5, 10 0 5, 10 4 5, 6 4 5, 6 0 5)))");

  std::unique_ptr<Geometry> hull =
      ConcaveHullOfPolygons::concaveHullByLength(input.get(), 3.0);
  EXPECT_TRUE(hull->isValid());
  EXPECT_EQ(hull->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(hull->getArea(), 40.0);

  std::unique_ptr<Geometry> fill =
      ConcaveHullOfPolygons::concaveFillByLength(input.get(), 3.0);
  EXPECT_TRUE(fill->isValid());
  EXPECT_EQ(fill->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(fill->getArea(), 8.0);
}

}  // namespace
}  // namespace geos_google
