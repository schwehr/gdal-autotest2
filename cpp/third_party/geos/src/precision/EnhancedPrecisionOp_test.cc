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

// Unit tests for third_party/geos/src/precision/EnhancedPrecisionOp.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - Tests all five static operations (intersection, Union, difference,
//   symDifference, buffer) across standard, empty, nested-hole, and
//   large-common-bit inputs as well as exception capture, CommonBitsOp(true)
//   recovery, and original-exception rethrow paths when reshifted results
//   become invalid or retry throws.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/precision/EnhancedPrecisionOp.cpp

#include "third_party/geos/include/geos/precision/EnhancedPrecisionOp.h"

#include <initializer_list>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/GEOSException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::Dimension;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::MultiPolygon;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::precision::EnhancedPrecisionOp;
using geos::util::GEOSException;

// 2^51: ULP is 0.5, so adding common bits rounds local offsets to 0.5*Z.
constexpr double kBaseCoord = 2251799813685248.0;
// Local origin inside [2^51, 2^52) so negative local offsets still share the
// 2^51 bit in CommonBitsRemover across all binary overlay fallback fixtures.
constexpr double kLocalOrigin = kBaseCoord + 512.0;

std::unique_ptr<LineString> MakeLineWithOrigin(
    const GeometryFactory& factory, double origin,
    std::initializer_list<std::pair<double, double>> offsets) {
  CoordinateSequence seq = CoordinateSequence::XY(0);
  seq.reserve(offsets.size());
  for (const auto& [dx, dy] : offsets) {
    seq.add(origin + dx, origin + dy);
  }
  return factory.createLineString(seq);
}

std::unique_ptr<Polygon> MakePolygonWithOrigin(
    const GeometryFactory& factory, double origin,
    std::initializer_list<std::pair<double, double>> offsets) {
  CoordinateSequence seq = CoordinateSequence::XY(0);
  seq.reserve(offsets.size());
  for (const auto& [dx, dy] : offsets) {
    seq.add(origin + dx, origin + dy);
  }
  std::unique_ptr<LinearRing> shell = factory.createLinearRing(seq);
  return factory.createPolygon(std::move(shell));
}

// Single valid CCW polygon with an oblique wedge across x=80 where the upper
// slope edge S1 (y(80)=62.24) and lower slope edge S3 (y(80)=62.034) both
// round to (80, 62.0) near 2^51, causing a primary side-location conflict
// while reshifting moves (80, 62.24) across (89, 0)->(89, 68).
std::unique_ptr<Polygon> MakeSingleWedgePolygon(
    const GeometryFactory& factory) {
  return MakePolygonWithOrigin(factory, kLocalOrigin,
                               {{0.0, 10.0},
                                {1000.0, 663.0},
                                {1000.0, 800.0},
                                {-100.0, 800.0},
                                {-100.0, -100.0},
                                {89.0, -100.0},
                                {89.0, 68.0},
                                {0.0, 9.0},
                                {0.0, 10.0}});
}

// Appends three disjoint polygons near (kBaseCoord, kBaseCoord) whose d=435
// buffer fails primary and snap-rounding passes (depth mismatch caused by a
// vertex/edge snap collision across a shallow slope), while CommonBitsOp(true)
// merges them cleanly into a valid polygon.
std::vector<std::unique_ptr<Polygon>> MakeBufferFallbackPolygons(
    const GeometryFactory& factory) {
  std::vector<std::unique_ptr<Polygon>> polys;
  // P1 (CW): top edge slope 17/144, d=435 left offset (1000,1000)->(3160,1255).
  polys.push_back(MakePolygonWithOrigin(factory, kBaseCoord,
                                        {{1051.0, 568.0},
                                         {3211.0, 823.0},
                                         {3211.0, 200.0},
                                         {1051.0, 200.0},
                                         {1051.0, 568.0}}));
  // P2 (CCW): inside turn at (1936, 1562.5) places V at (2081, 1127.5),
  // 0.118 below P1's offset line, and extends horizontally across x = 2082.
  polys.push_back(MakePolygonWithOrigin(factory, kBaseCoord,
                                        {{1480.0, 1220.5},
                                         {1936.0, 1562.5},
                                         {2100.0, 1562.5},
                                         {2100.0, 2200.0},
                                         {1480.0, 2200.0},
                                         {1480.0, 1220.5}}));
  // P3 (CW): left edge x=2517 shifts left by 435 to x=2082, snapping both
  // P1's and P2's offsets at (2082, 1127.5) in the fixed-precision passes.
  polys.push_back(MakePolygonWithOrigin(factory, kBaseCoord,
                                        {{2517.0, 1120.0},
                                         {2517.0, 1200.0},
                                         {2550.0, 1200.0},
                                         {2550.0, 1120.0},
                                         {2517.0, 1120.0}}));
  return polys;
}

class EnhancedPrecisionOpTest : public ::testing::Test {
 protected:
  EnhancedPrecisionOpTest()
      : factory_(GeometryFactory::create()), reader_(factory_.get()) {}

  Geometry::Ptr ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(EnhancedPrecisionOpTest, BinaryOperationsOnOverlappingSquares) {
  Geometry::Ptr a = ReadWkt("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  Geometry::Ptr b = ReadWkt("POLYGON ((2 2, 6 2, 6 6, 2 6, 2 2))");

  Geometry::Ptr inter = EnhancedPrecisionOp::intersection(a.get(), b.get());
  ASSERT_NE(inter, nullptr);
  EXPECT_TRUE(inter->isValid());
  EXPECT_DOUBLE_EQ(inter->getArea(), 4.0);

  Geometry::Ptr uni = EnhancedPrecisionOp::Union(a.get(), b.get());
  ASSERT_NE(uni, nullptr);
  EXPECT_TRUE(uni->isValid());
  EXPECT_DOUBLE_EQ(uni->getArea(), 28.0);

  Geometry::Ptr diff_ab = EnhancedPrecisionOp::difference(a.get(), b.get());
  ASSERT_NE(diff_ab, nullptr);
  EXPECT_TRUE(diff_ab->isValid());
  EXPECT_DOUBLE_EQ(diff_ab->getArea(), 12.0);

  Geometry::Ptr diff_ba = EnhancedPrecisionOp::difference(b.get(), a.get());
  ASSERT_NE(diff_ba, nullptr);
  EXPECT_TRUE(diff_ba->isValid());
  EXPECT_DOUBLE_EQ(diff_ba->getArea(), 12.0);

  Geometry::Ptr sym = EnhancedPrecisionOp::symDifference(a.get(), b.get());
  ASSERT_NE(sym, nullptr);
  EXPECT_TRUE(sym->isValid());
  EXPECT_DOUBLE_EQ(sym->getArea(), 24.0);
}

TEST_F(EnhancedPrecisionOpTest, BinaryOperationsWithHoleAndEmptyInputs) {
  Geometry::Ptr holed = ReadWkt(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(3 3, 7 3, 7 7, 3 7, 3 3))");
  Geometry::Ptr right = ReadWkt("POLYGON ((5 0, 12 0, 12 10, 5 10, 5 0))");

  EXPECT_DOUBLE_EQ(
      EnhancedPrecisionOp::intersection(holed.get(), right.get())->getArea(),
      42.0);
  EXPECT_DOUBLE_EQ(
      EnhancedPrecisionOp::Union(holed.get(), right.get())->getArea(), 112.0);
  EXPECT_DOUBLE_EQ(
      EnhancedPrecisionOp::difference(holed.get(), right.get())->getArea(),
      42.0);
  EXPECT_DOUBLE_EQ(
      EnhancedPrecisionOp::symDifference(holed.get(), right.get())->getArea(),
      70.0);

  Geometry::Ptr empty_poly = ReadWkt("POLYGON EMPTY");
  EXPECT_TRUE(EnhancedPrecisionOp::intersection(holed.get(), empty_poly.get())
                  ->isEmpty());
  EXPECT_DOUBLE_EQ(
      EnhancedPrecisionOp::Union(holed.get(), empty_poly.get())->getArea(),
      84.0);
  EXPECT_DOUBLE_EQ(
      EnhancedPrecisionOp::difference(holed.get(), empty_poly.get())->getArea(),
      84.0);
  EXPECT_TRUE(EnhancedPrecisionOp::difference(empty_poly.get(), holed.get())
                  ->isEmpty());
  EXPECT_DOUBLE_EQ(
      EnhancedPrecisionOp::symDifference(holed.get(), empty_poly.get())
          ->getArea(),
      84.0);
}

TEST_F(EnhancedPrecisionOpTest, BinaryOperationsAndBufferWithLargeCommonBits) {
  // Coordinates share ~30 leading bits (~1e9) while fitting cleanly below 2^52.
  Geometry::Ptr a = ReadWkt(
      "POLYGON ((1000000000 1000000000, 1000000004 1000000000, "
      "1000000004 1000000004, 1000000000 1000000004, 1000000000 1000000000))");
  Geometry::Ptr b = ReadWkt(
      "POLYGON ((1000000002 1000000002, 1000000006 1000000002, "
      "1000000006 1000000006, 1000000002 1000000006, 1000000002 1000000002))");

  EXPECT_DOUBLE_EQ(
      EnhancedPrecisionOp::intersection(a.get(), b.get())->getArea(), 4.0);
  EXPECT_DOUBLE_EQ(EnhancedPrecisionOp::Union(a.get(), b.get())->getArea(),
                   28.0);
  EXPECT_DOUBLE_EQ(EnhancedPrecisionOp::difference(a.get(), b.get())->getArea(),
                   12.0);
  EXPECT_DOUBLE_EQ(
      EnhancedPrecisionOp::symDifference(a.get(), b.get())->getArea(), 24.0);

  Geometry::Ptr buffered = EnhancedPrecisionOp::buffer(a.get(), 1.0);
  ASSERT_NE(buffered, nullptr);
  EXPECT_TRUE(buffered->isValid());
  EXPECT_NEAR(buffered->getArea(), 32.0 + std::numbers::pi, 0.1);
}

TEST_F(EnhancedPrecisionOpTest, BufferSupportsPositiveZeroNegativeAndEmpty) {
  Geometry::Ptr square = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");

  Geometry::Ptr grown = EnhancedPrecisionOp::buffer(square.get(), 2.0);
  ASSERT_NE(grown, nullptr);
  EXPECT_TRUE(grown->isValid());
  // 10x10 square (100) + 4 side bands (4*10*2=80) + circle of r=2 (4*pi).
  EXPECT_NEAR(grown->getArea(), 180.0 + 4.0 * std::numbers::pi, 0.25);

  Geometry::Ptr zero_buf = EnhancedPrecisionOp::buffer(square.get(), 0.0);
  ASSERT_NE(zero_buf, nullptr);
  EXPECT_DOUBLE_EQ(zero_buf->getArea(), 100.0);
  EXPECT_TRUE(zero_buf->equals(square.get()));

  Geometry::Ptr shrunk = EnhancedPrecisionOp::buffer(square.get(), -2.0);
  ASSERT_NE(shrunk, nullptr);
  EXPECT_DOUBLE_EQ(shrunk->getArea(), 36.0);

  Geometry::Ptr collapsed = EnhancedPrecisionOp::buffer(square.get(), -6.0);
  ASSERT_NE(collapsed, nullptr);
  EXPECT_TRUE(collapsed->isEmpty());

  Geometry::Ptr empty_pt = ReadWkt("POINT EMPTY");
  Geometry::Ptr empty_buf = EnhancedPrecisionOp::buffer(empty_pt.get(), 2.0);
  ASSERT_NE(empty_buf, nullptr);
  EXPECT_TRUE(empty_buf->isEmpty());
}

TEST_F(EnhancedPrecisionOpTest, MixedLineAndPointOperations) {
  Geometry::Ptr line = ReadWkt("LINESTRING (0 2, 10 2)");
  Geometry::Ptr box = ReadWkt("POLYGON ((2 0, 6 0, 6 4, 2 4, 2 0))");

  Geometry::Ptr clipped =
      EnhancedPrecisionOp::intersection(line.get(), box.get());
  ASSERT_NE(clipped, nullptr);
  EXPECT_DOUBLE_EQ(clipped->getLength(), 4.0);

  Geometry::Ptr line_buf = EnhancedPrecisionOp::buffer(line.get(), 1.0);
  ASSERT_NE(line_buf, nullptr);
  EXPECT_TRUE(line_buf->isValid());
  // Stadium of length 10 and radius 1: 20 + pi.
  EXPECT_NEAR(line_buf->getArea(), 20.0 + std::numbers::pi, 0.1);

  Geometry::Ptr pt_inside = ReadWkt("POINT (4 2)");
  Geometry::Ptr pt_clipped =
      EnhancedPrecisionOp::intersection(pt_inside.get(), box.get());
  ASSERT_NE(pt_clipped, nullptr);
  EXPECT_TRUE(pt_clipped->equalsExact(pt_inside.get()));

  Geometry::Ptr pt_outside = ReadWkt("POINT (10 10)");
  EXPECT_TRUE(EnhancedPrecisionOp::intersection(pt_outside.get(), box.get())
                  ->isEmpty());
}

TEST_F(EnhancedPrecisionOpTest, RethrowsOriginalExceptionWhenFallbackFails) {
  // Overlapping envelopes force HeuristicOverlay through OverlayNGRobust rather
  // than disjoint combine, triggering UnsupportedOperationException in both the
  // primary call and the CommonBitsOp retry before rethrowing originalEx.
  Geometry::Ptr curve = ReadWkt(
      "CIRCULARSTRING (1000000000 1000000000, "
      "1000000001 1000000001, 1000000002 1000000000)");
  Geometry::Ptr box = ReadWkt(
      "POLYGON ((1000000000 1000000000, 1000000002 1000000000, "
      "1000000002 1000000002, 1000000000 1000000002, 1000000000 1000000000))");

  EXPECT_THROW(EnhancedPrecisionOp::intersection(curve.get(), box.get()),
               GEOSException);
  EXPECT_THROW(EnhancedPrecisionOp::Union(curve.get(), box.get()),
               GEOSException);
  EXPECT_THROW(EnhancedPrecisionOp::difference(curve.get(), box.get()),
               GEOSException);
  EXPECT_THROW(EnhancedPrecisionOp::symDifference(curve.get(), box.get()),
               GEOSException);
  EXPECT_THROW(EnhancedPrecisionOp::buffer(curve.get(), 1.0), GEOSException);

  EXPECT_THROW(EnhancedPrecisionOp::buffer(
                   box.get(), std::numeric_limits<double>::infinity()),
               GEOSException);
  EXPECT_THROW(EnhancedPrecisionOp::buffer(
                   box.get(), std::numeric_limits<double>::quiet_NaN()),
               GEOSException);
}

TEST_F(EnhancedPrecisionOpTest, BinaryOperationsValidCommonBitsFallback) {
  // Non-floating PrecisionModel(1024.0) sends OverlayNGRobust directly to fixed
  // snap-rounding near 2^51 (where HotPixel tolerance collapses to 0 and
  // intersections round to 0.5*Z), causing a side-location conflict at
  // (80, 62.0). CommonBitsOp(true) succeeds and reshifts into valid results.
  PrecisionModel fixed_pm(1024.0);
  GeometryFactory::Ptr fixed_factory = GeometryFactory::create(&fixed_pm);

  std::vector<std::unique_ptr<Polygon>> polys_a;
  polys_a.push_back(MakePolygonWithOrigin(*fixed_factory, kLocalOrigin,
                                          {{0.0, 10.0},
                                           {1000.0, 663.0},
                                           {1000.0, 800.0},
                                           {-100.0, 800.0},
                                           {-100.0, 10.0},
                                           {0.0, 10.0}}));
  polys_a.push_back(MakePolygonWithOrigin(*fixed_factory, kLocalOrigin,
                                          {{-100.0, -100.0},
                                           {89.0, -100.0},
                                           {89.0, 68.0},
                                           {0.0, 9.0},
                                           {-100.0, 9.0},
                                           {-100.0, -100.0}}));
  std::unique_ptr<MultiPolygon> geom_a =
      fixed_factory->createMultiPolygon(std::move(polys_a));
  ASSERT_TRUE(geom_a->isValid());

  // 1) intersection: clipped edges remain disjoint except at (80, 62.0), which
  // forms a valid non-crossing touch between the two result polygons.
  std::vector<std::unique_ptr<Polygon>> polys_b_left;
  polys_b_left.push_back(MakePolygonWithOrigin(*fixed_factory, kLocalOrigin,
                                               {{-150.0, -150.0},
                                                {80.0, -150.0},
                                                {80.0, 750.0},
                                                {-150.0, 750.0},
                                                {-150.0, -150.0}}));
  polys_b_left.push_back(MakePolygonWithOrigin(*fixed_factory, kLocalOrigin,
                                               {{990.0, 780.0},
                                                {1050.0, 780.0},
                                                {1050.0, 820.0},
                                                {990.0, 820.0},
                                                {990.0, 780.0}}));
  std::unique_ptr<MultiPolygon> b_left_valid =
      fixed_factory->createMultiPolygon(std::move(polys_b_left));
  ASSERT_TRUE(b_left_valid->isValid());

  Geometry::Ptr inter =
      EnhancedPrecisionOp::intersection(geom_a.get(), b_left_valid.get());
  ASSERT_NE(inter, nullptr);
  EXPECT_TRUE(inter->isValid());
  EXPECT_FALSE(inter->isEmpty());
  EXPECT_EQ(inter->getDimension(), Dimension::A);
  EXPECT_GT(inter->getArea(), 100000.0);

  // 2) Union and difference: 6-vertex cover avoids RectangleIntersection,
  // triggers the primary side-location conflict at (80, 62.0), and yields valid
  // reshifted polygons from CommonBitsOp(true).
  std::unique_ptr<Polygon> b_right_valid =
      MakePolygonWithOrigin(*fixed_factory, kLocalOrigin,
                            {{80.0, -200.0},
                             {1100.0, -200.0},
                             {1100.0, 300.0},
                             {1050.0, 850.0},
                             {80.0, 850.0},
                             {80.0, -200.0}});
  ASSERT_TRUE(b_right_valid->isValid());

  Geometry::Ptr uni =
      EnhancedPrecisionOp::Union(geom_a.get(), b_right_valid.get());
  ASSERT_NE(uni, nullptr);
  EXPECT_TRUE(uni->isValid());
  EXPECT_FALSE(uni->isEmpty());
  EXPECT_EQ(uni->getDimension(), Dimension::A);
  EXPECT_GT(uni->getArea(), 900000.0);

  Geometry::Ptr diff =
      EnhancedPrecisionOp::difference(geom_a.get(), b_right_valid.get());
  ASSERT_NE(diff, nullptr);
  EXPECT_TRUE(diff->isValid());
  EXPECT_FALSE(diff->isEmpty());
  EXPECT_EQ(diff->getDimension(), Dimension::A);
  EXPECT_GT(diff->getArea(), 100000.0);

  // 3) symDifference via StructuredCollection: line/polygon crossing rounds
  // onto P2's (80, 62.0) vertex in the primary pass (side-location conflict),
  // while CommonBitsOp(true) separates the split to x=79.861 and stays valid.
  std::vector<std::unique_ptr<Geometry>> geoms_sym;
  geoms_sym.push_back(MakePolygonWithOrigin(*fixed_factory, kLocalOrigin,
                                            {{0.0, 10.0},
                                             {1000.0, 663.0},
                                             {1000.0, 800.0},
                                             {-100.0, 800.0},
                                             {-100.0, 10.0},
                                             {0.0, 10.0}}));
  geoms_sym.push_back(MakePolygonWithOrigin(*fixed_factory, kLocalOrigin,
                                            {{-100.0, -100.0},
                                             {89.0, -100.0},
                                             {89.0, 68.0},
                                             {80.0, 62.0},
                                             {0.0, -10.0},
                                             {-100.0, -10.0},
                                             {-100.0, -100.0}}));
  geoms_sym.push_back(MakeLineWithOrigin(*fixed_factory, kLocalOrigin,
                                         {{70.0, 55.0},
                                          {90.0, 69.5},
                                          {1050.0, 750.0},
                                          {1050.0, -150.0},
                                          {-150.0, -150.0},
                                          {-150.0, 850.0}}));
  std::unique_ptr<GeometryCollection> geom_a_gc =
      fixed_factory->createGeometryCollection(std::move(geoms_sym));
  ASSERT_TRUE(geom_a_gc->isValid());

  std::unique_ptr<Polygon> geom_b_box =
      MakePolygonWithOrigin(*fixed_factory, kLocalOrigin,
                            {{200.0, 700.0},
                             {300.0, 700.0},
                             {300.0, 780.0},
                             {250.0, 790.0},
                             {200.0, 780.0},
                             {200.0, 700.0}});
  ASSERT_TRUE(geom_b_box->isValid());

  Geometry::Ptr sym =
      EnhancedPrecisionOp::symDifference(geom_a_gc.get(), geom_b_box.get());
  ASSERT_NE(sym, nullptr);
  EXPECT_TRUE(sym->isValid());
  EXPECT_FALSE(sym->isEmpty());
  EXPECT_EQ(sym->getDimension(), Dimension::A);
  EXPECT_GT(sym->getArea(), 400000.0);
}

TEST_F(EnhancedPrecisionOpTest, IntersectionRethrowsWhenReshiftBecomesInvalid) {
  PrecisionModel fixed_pm(1024.0);
  GeometryFactory::Ptr fixed_factory = GeometryFactory::create(&fixed_pm);
  std::unique_ptr<Polygon> geom_a = MakeSingleWedgePolygon(*fixed_factory);
  ASSERT_TRUE(geom_a->isValid());

  // Covers x >= 80 plus the upper band y in [700, 750] so clipEnv keeps the
  // entire oblique wedge unclipped in Primary (side-location conflict at
  // (80, 62.0)); CommonBitsOp(true) succeeds, then reshifting crosses S1 over
  // (89, -100)->(89, 68) at (89, 67.879) so !resultEP->isValid() rethrows.
  std::unique_ptr<Polygon> geom_b =
      MakePolygonWithOrigin(*fixed_factory, kLocalOrigin,
                            {{80.0, -200.0},
                             {1100.0, -200.0},
                             {1100.0, 750.0},
                             {-150.0, 750.0},
                             {-150.0, 700.0},
                             {80.0, 700.0},
                             {80.0, -200.0}});
  ASSERT_TRUE(geom_b->isValid());

  EXPECT_THROW(EnhancedPrecisionOp::intersection(geom_a.get(), geom_b.get()),
               GEOSException);
}

TEST_F(EnhancedPrecisionOpTest,
       UnionDifferenceSymDiffRethrowWhenReshiftBecomesInvalid) {
  PrecisionModel fixed_pm(1024.0);
  GeometryFactory::Ptr fixed_factory = GeometryFactory::create(&fixed_pm);
  std::unique_ptr<Polygon> geom_a = MakeSingleWedgePolygon(*fixed_factory);
  ASSERT_TRUE(geom_a->isValid());

  // Covers x <= 80 plus upper bar y in [700, 750]: Primary hits the
  // side-location conflict at (80, 62.0), while Union(A, B), difference(A, B),
  // and symDifference(A, B) all preserve S1[80..1000] and (89, 68), which
  // self-intersect after reshift so all three rethrow originalEx.
  std::unique_ptr<Polygon> geom_b =
      MakePolygonWithOrigin(*fixed_factory, kLocalOrigin,
                            {{-150.0, -200.0},
                             {80.0, -200.0},
                             {80.0, 700.0},
                             {1100.0, 700.0},
                             {1100.0, 750.0},
                             {-150.0, 750.0},
                             {-150.0, -200.0}});
  ASSERT_TRUE(geom_b->isValid());

  EXPECT_THROW(EnhancedPrecisionOp::Union(geom_a.get(), geom_b.get()),
               GEOSException);
  EXPECT_THROW(EnhancedPrecisionOp::difference(geom_a.get(), geom_b.get()),
               GEOSException);
  EXPECT_THROW(EnhancedPrecisionOp::symDifference(geom_a.get(), geom_b.get()),
               GEOSException);
}

TEST_F(EnhancedPrecisionOpTest, BufferValidCommonBitsFallback) {
  PrecisionModel fixed_pm(1024.0);
  GeometryFactory::Ptr fixed_factory = GeometryFactory::create(&fixed_pm);
  std::vector<std::unique_ptr<Polygon>> polys =
      MakeBufferFallbackPolygons(*fixed_factory);
  std::unique_ptr<MultiPolygon> mp =
      fixed_factory->createMultiPolygon(std::move(polys));

  Geometry::Ptr buffered = EnhancedPrecisionOp::buffer(mp.get(), 435.0);
  ASSERT_NE(buffered, nullptr);
  EXPECT_TRUE(buffered->isValid());
  EXPECT_FALSE(buffered->isEmpty());
  EXPECT_EQ(buffered->getDimension(), Dimension::A);
  EXPECT_GT(buffered->getArea(), 1000000.0);
}

TEST_F(EnhancedPrecisionOpTest, BufferRethrowsWhenReshiftBecomesInvalid) {
  PrecisionModel fixed_pm(1024.0);
  GeometryFactory::Ptr fixed_factory = GeometryFactory::create(&fixed_pm);
  std::vector<std::unique_ptr<Polygon>> polys =
      MakeBufferFallbackPolygons(*fixed_factory);

  std::vector<std::unique_ptr<Geometry>> geoms;
  geoms.reserve(polys.size() + 2);
  for (auto& poly : polys) {
    geoms.push_back(std::move(poly));
  }
  // Disjoint near the origin (~0.10 unit gap at d=435), but reshifting back to
  // kBaseCoord rounds the upper arc vertex into the lower polygon boundary,
  // producing an invalid MultiPolygon so EnhancedPrecisionOp rethrows.
  geoms.push_back(MakeLineWithOrigin(*fixed_factory, kBaseCoord,
                                     {{4650.0, 1980.0}, {5000.0, 2000.0}}));
  geoms.push_back(MakePolygonWithOrigin(*fixed_factory, kBaseCoord,
                                        {{5561.5, 2701.5},
                                         {6161.5, 1901.5},
                                         {6400.0, 1901.5},
                                         {6400.0, 2701.5},
                                         {5561.5, 2701.5}}));
  std::unique_ptr<GeometryCollection> gc =
      fixed_factory->createGeometryCollection(std::move(geoms));

  EXPECT_THROW(EnhancedPrecisionOp::buffer(gc.get(), 435.0), GEOSException);
}

}  // namespace
}  // namespace geos_google
