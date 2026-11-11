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

// Unit tests for geos::operation::buffer::BufferOp.
// See also:
// https://github.com/libgeos/geos/blob/main/src/operation/buffer/BufferOp.cpp

#include "third_party/geos/include/geos/operation/buffer/BufferOp.h"

#include <limits>
#include <memory>
#include <numbers>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/buffer/BufferParameters.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using ::geos::geom::Envelope;
using ::geos::geom::Geometry;
using ::geos::geom::GeometryFactory;
using ::geos::geom::GeometryTypeId;
using ::geos::geom::PrecisionModel;
using ::geos::io::WKTReader;
using ::geos::operation::buffer::BufferOp;
using ::geos::operation::buffer::BufferParameters;

class BufferOpTest : public ::testing::Test {
 protected:
  BufferOpTest()
      : factory_(GeometryFactory::create()), reader_(factory_.get()) {}

  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(BufferOpTest, StaticBufferOpPointDistancesAndQuadrantSegments) {
  auto point = ReadWkt("POINT (3 4)");

  // Default parameters produce a circular polygonal approximation of area
  // pi*r^2.
  auto circle = BufferOp::bufferOp(point.get(), 2.0);
  ASSERT_NE(circle, nullptr);
  EXPECT_TRUE(circle->isValid());
  EXPECT_EQ(circle->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
  EXPECT_NEAR(circle->getArea(), 4.0 * std::numbers::pi, 0.25);

  // 1 quadrant segment yields a rotated square (diamond) of diagonal 2 and
  // area 2.
  auto diamond = BufferOp::bufferOp(point.get(), 1.0, 1, BufferOp::CAP_ROUND);
  ASSERT_NE(diamond, nullptr);
  EXPECT_TRUE(diamond->isValid());
  EXPECT_NEAR(diamond->getArea(), 2.0, 1e-9);

  // Zero and negative distances on a Point produce an empty Polygon.
  auto zero_buf = BufferOp::bufferOp(point.get(), 0.0);
  EXPECT_TRUE(zero_buf->isEmpty());
  auto neg_buf = BufferOp::bufferOp(point.get(), -1.0);
  EXPECT_TRUE(neg_buf->isEmpty());
}

TEST_F(BufferOpTest, StaticBufferOpLineStringEndCapStyles) {
  auto line = ReadWkt("LINESTRING (0 0, 10 0)");

  auto round_cap = BufferOp::bufferOp(line.get(), 1.0, 16, BufferOp::CAP_ROUND);
  EXPECT_TRUE(round_cap->isValid());
  EXPECT_NEAR(round_cap->getArea(), 20.0 + std::numbers::pi, 0.05);

  auto butt_cap = BufferOp::bufferOp(line.get(), 1.0, 8, BufferOp::CAP_BUTT);
  EXPECT_TRUE(butt_cap->isValid());
  EXPECT_NEAR(butt_cap->getArea(), 20.0, 1e-9);

  auto square_cap =
      BufferOp::bufferOp(line.get(), 1.0, 8, BufferOp::CAP_SQUARE);
  EXPECT_TRUE(square_cap->isValid());
  EXPECT_NEAR(square_cap->getArea(), 24.0, 1e-9);

  // Negative distance on a LineString yields an empty Polygon.
  auto neg_line = BufferOp::bufferOp(line.get(), -0.5);
  EXPECT_TRUE(neg_line->isEmpty());
}

TEST_F(BufferOpTest, BufferOpWithBufferParametersAndSingleSided) {
  auto corner = ReadWkt("LINESTRING (0 0, 10 0, 10 10)");

  BufferParameters mitre_params(4, BufferParameters::CAP_FLAT,
                                BufferParameters::JOIN_MITRE, 10.0);
  auto mitred = BufferOp::bufferOp(corner.get(), 1.0, mitre_params);
  ASSERT_NE(mitred, nullptr);
  EXPECT_TRUE(mitred->isValid());
  // Outer corner extends to (11, -1) while inner edge runs from (9, 1).
  EXPECT_NEAR(mitred->getEnvelopeInternal()->getMaxX(), 11.0, 1e-9);
  EXPECT_NEAR(mitred->getEnvelopeInternal()->getMinY(), -1.0, 1e-9);

  BufferParameters bevel_params(4, BufferParameters::CAP_FLAT,
                                BufferParameters::JOIN_BEVEL, 1.0);
  auto beveled = BufferOp::bufferOp(corner.get(), 1.0, bevel_params);
  ASSERT_NE(beveled, nullptr);
  EXPECT_TRUE(beveled->isValid());
  EXPECT_LT(beveled->getArea(), mitred->getArea());

  // Stateful BufferOp single-sided positive buffers left side, negative right.
  auto hline = ReadWkt("LINESTRING (0 0, 10 0)");
  BufferOp op(hline.get());
  op.setQuadrantSegments(8);
  op.setEndCapStyle(BufferOp::CAP_ROUND);
  op.setSingleSided(true);

  auto left = op.getResultGeometry(2.0);
  ASSERT_NE(left, nullptr);
  EXPECT_TRUE(left->isValid());
  const Envelope* left_env = left->getEnvelopeInternal();
  EXPECT_NEAR(left_env->getMinY(), 0.0, 1e-9);
  EXPECT_NEAR(left_env->getMaxY(), 2.0, 1e-9);
  EXPECT_NEAR(left->getArea(), 20.0, 1e-9);

  op.setSingleSided(true);
  auto right = op.getResultGeometry(-2.0);
  ASSERT_NE(right, nullptr);
  EXPECT_TRUE(right->isValid());
  const Envelope* right_env = right->getEnvelopeInternal();
  EXPECT_NEAR(right_env->getMinY(), -2.0, 1e-9);
  EXPECT_NEAR(right_env->getMaxY(), 0.0, 1e-9);
  EXPECT_NEAR(right->getArea(), 20.0, 1e-9);

  BufferOp op_with_params(hline.get(), mitre_params);
  op_with_params.setSingleSided(false);
  auto full = op_with_params.getResultGeometry(1.0);
  EXPECT_NEAR(full->getArea(), 20.0, 1e-9);
}

TEST_F(BufferOpTest, PolygonWithHoleEmptyAndCollectionInputs) {
  auto poly_with_hole = ReadWkt(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (3 3, 7 3, 7 7, 3 7, 3 3))");

  // Expanding by 1 grows outer ring to 12x12 (less rounded corners) and shrinks
  // hole to 2x2.
  auto grown = BufferOp::bufferOp(poly_with_hole.get(), 1.0);
  EXPECT_TRUE(grown->isValid());
  EXPECT_GT(grown->getArea(), poly_with_hole->getArea());

  // Eroding by -1 shrinks outer ring to a sharp 8x8 square (area 64) and grows
  // the 4x4 hole outward into a rounded 6x6 region (area 32 + pi), leaving
  // polygon area 32 - pi.
  auto eroded = BufferOp::bufferOp(poly_with_hole.get(), -1.0);
  EXPECT_TRUE(eroded->isValid());
  EXPECT_NEAR(eroded->getArea(), 32.0 - std::numbers::pi, 0.1);

  // Eroding further than polygon thickness collapses to empty.
  auto vanished = BufferOp::bufferOp(poly_with_hole.get(), -2.0);
  EXPECT_TRUE(vanished->isEmpty());

  auto empty_pt = ReadWkt("POINT EMPTY");
  EXPECT_TRUE(BufferOp::bufferOp(empty_pt.get(), 2.0)->isEmpty());
  auto empty_poly = ReadWkt("POLYGON EMPTY");
  EXPECT_TRUE(BufferOp::bufferOp(empty_poly.get(), 2.0)->isEmpty());

  auto collection =
      ReadWkt("GEOMETRYCOLLECTION (POINT (0 0), LINESTRING (10 0, 14 0))");
  // Flat/butt caps yield an empty offset curve for points, so only the 4x2
  // rectangle around the LineString contributes area.
  auto butt_coll_buf =
      BufferOp::bufferOp(collection.get(), 1.0, 8, BufferOp::CAP_BUTT);
  EXPECT_TRUE(butt_coll_buf->isValid());
  EXPECT_EQ(butt_coll_buf->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
  EXPECT_NEAR(butt_coll_buf->getArea(), 8.0, 1e-9);

  // Round caps buffer both components into a MultiPolygon of disk + stadium.
  auto round_coll_buf =
      BufferOp::bufferOp(collection.get(), 1.0, 8, BufferOp::CAP_ROUND);
  EXPECT_TRUE(round_coll_buf->isValid());
  EXPECT_EQ(round_coll_buf->getGeometryTypeId(),
            GeometryTypeId::GEOS_MULTIPOLYGON);
  EXPECT_EQ(round_coll_buf->getNumGeometries(), 2U);
  EXPECT_NEAR(round_coll_buf->getArea(), 8.0 + 2.0 * std::numbers::pi, 0.15);

  // Square caps buffer the point into a 2x2 square and the line into 6x2.
  auto square_coll_buf =
      BufferOp::bufferOp(collection.get(), 1.0, 8, BufferOp::CAP_SQUARE);
  EXPECT_TRUE(square_coll_buf->isValid());
  EXPECT_EQ(square_coll_buf->getGeometryTypeId(),
            GeometryTypeId::GEOS_MULTIPOLYGON);
  EXPECT_EQ(square_coll_buf->getNumGeometries(), 2U);
  EXPECT_NEAR(square_coll_buf->getArea(), 16.0, 1e-9);
}

TEST_F(BufferOpTest, NonFiniteDistanceThrowsIllegalArgumentException) {
  auto point = ReadWkt("POINT (0 0)");
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();

  EXPECT_THROW(BufferOp::bufferOp(point.get(), inf),
               geos::util::IllegalArgumentException);
  EXPECT_THROW(BufferOp::bufferOp(point.get(), -inf),
               geos::util::IllegalArgumentException);
  EXPECT_THROW(BufferOp::bufferOp(point.get(), nan),
               geos::util::IllegalArgumentException);
}

TEST_F(BufferOpTest, FixedPrecisionNormalAndFallbackPathViaSnapRounding) {
  PrecisionModel fixed_pm(1.0);
  GeometryFactory::Ptr fixed_factory = GeometryFactory::create(&fixed_pm);
  WKTReader fixed_reader(fixed_factory.get());

  auto simple = fixed_reader.read("LINESTRING (0 0, 10 0)");
  auto simple_buf =
      BufferOp::bufferOp(simple.get(), 2.0, 4, BufferOp::CAP_BUTT);
  ASSERT_NE(simple_buf, nullptr);
  EXPECT_TRUE(simple_buf->isValid());
  EXPECT_NEAR(simple_buf->getArea(), 40.0, 1e-6);

  // Three crossings round onto y=1 at x=3, 4, 5 without splitting edge
  // (5,1)->(3,1) at (4,1), leaving collinear outgoing edge directions at (3,1)
  // that cause bufferOriginalPrecision() to throw TopologyException and fall
  // back to bufferFixedPrecision(*argPM).
  auto tricky_fixed =
      fixed_reader.read("POLYGON ((0 0, 10 3, 10 1, 0 1, 0 2, 10 0, 0 0))");
  auto recovered = BufferOp::bufferOp(tricky_fixed.get(), 0.0);
  ASSERT_NE(recovered, nullptr);
  EXPECT_TRUE(recovered->isValid());
}

TEST_F(BufferOpTest, FloatingPrecisionReducedPrecisionFallbackPaths) {
  // Replicate the exact collinear-node noding failure in IEEE-754 double at
  // y0 = 1e8, where 1 ULP = 2^-26 ~= 1.4901161193847656e-8:
  //   y0 + 1*U == 100000000.000000015
  //   y0 + 2*U == 100000000.000000030
  //   y0 + 3*U == 100000000.000000045
  // bufferOriginalPrecision() throws TopologyException, exercising
  // bufferReducedPrecision(), precisionScaleFactor(), and
  // bufferFixedPrecision().
  auto zero_dist_poly = ReadWkt(
      "POLYGON ((0 100000000, 10 100000000.000000045, "
      "10 100000000.000000015, 0 100000000.000000015, "
      "0 100000000.000000030, 10 100000000, 0 100000000))");
  auto zero_dist_result = BufferOp::bufferOp(zero_dist_poly.get(), 0.0);
  ASSERT_NE(zero_dist_result, nullptr);
  EXPECT_TRUE(zero_dist_result->isValid());

  // Positive distance (+1.0) on three lines shifted to y = 99999999.0 + k*U
  // offsets top edges back onto y = 1e8 + k*U, triggering the same noding
  // failure and exercising precisionScaleFactor(..., distance=1.0, ...) to
  // recover a stadium polygon of width 12 and radius 1.
  auto pos_dist_lines = ReadWkt(
      "MULTILINESTRING ("
      "(-1 99999999.0, 11 99999999.000000045), "
      "(-1 99999999.000000015, 11 99999999.000000015), "
      "(-1 99999999.000000030, 11 99999999.0))");
  auto pos_dist_result = BufferOp::bufferOp(pos_dist_lines.get(), 1.0);
  ASSERT_NE(pos_dist_result, nullptr);
  EXPECT_FALSE(pos_dist_result->isEmpty());
  EXPECT_TRUE(pos_dist_result->isValid());
  EXPECT_NEAR(pos_dist_result->getArea(), 24.0 + std::numbers::pi, 0.25);

  // FLOATING_SINGLE PrecisionModel also routes through bufferReducedPrecision()
  // if bufferOriginalPrecision() fails, and buffers normal geometries directly.
  PrecisionModel float_single_pm(PrecisionModel::FLOATING_SINGLE);
  GeometryFactory::Ptr single_factory =
      GeometryFactory::create(&float_single_pm);
  WKTReader single_reader(single_factory.get());
  auto single_line = single_reader.read("LINESTRING (0 0, 5 0)");
  auto single_buf =
      BufferOp::bufferOp(single_line.get(), 1.0, 8, BufferOp::CAP_BUTT);
  ASSERT_NE(single_buf, nullptr);
  EXPECT_TRUE(single_buf->isValid());
  EXPECT_NEAR(single_buf->getArea(), 10.0, 1e-5);
}

TEST_F(BufferOpTest,
       ReducedPrecisionFallbackRecoversSharedEdgeAndTinyComponent) {
  // Combine:
  // 1) Two polygons sharing segment (0,0)->(2000,0) in opposing orientations
  //    (sum area = 1e7 + 1e7 = 2e7).
  // 2) An IEEE-754 collinear-node polygon at y = 1e8 (height ~4.5e-8) that
  // causes
  //    bufferOriginalPrecision() to throw TopologyException and enter
  //    bufferReducedPrecision().
  // With SNAP_WITH_NODER enabled in BufferOp::bufferFixedPrecision(),
  // HotPixelIndex marks the shared vertex (2000,0) as a node, SnapRoundingNoder
  // splits and merges components 1 & 2 cleanly into a valid Polygon, and the
  // sub-precision y = 1e8 component collapses on the precDigits=12 grid.
  auto mixed = ReadWkt(
      "MULTIPOLYGON ("
      "((0 0, 2000 0, 4000 2000, 0 4000, 0 0)), "
      "((0 0, 2000 0, 4000 -2000, 0 -4000, 0 0)), "
      "((0 100000000, 2000 100000000.000000045, "
      "2000 100000000.000000015, 0 100000000.000000015, "
      "0 100000000.000000030, 2000 100000000, 0 100000000)))");
  auto recovered = BufferOp::bufferOp(mixed.get(), 0.0);
  ASSERT_NE(recovered, nullptr);
  EXPECT_FALSE(recovered->isEmpty());
  EXPECT_TRUE(recovered->isValid());
  EXPECT_EQ(recovered->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
  EXPECT_NEAR(recovered->getArea(), 20000000.0, 1e-3);
}

TEST_F(BufferOpTest, LinearRingMultiPointAndCapStyleConstants) {
  EXPECT_EQ(+BufferOp::CAP_ROUND, +BufferParameters::CAP_ROUND);
  EXPECT_EQ(+BufferOp::CAP_BUTT, +BufferParameters::CAP_FLAT);
  EXPECT_EQ(+BufferOp::CAP_SQUARE, +BufferParameters::CAP_SQUARE);

  auto ring = ReadWkt("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  auto ring_buf = BufferOp::bufferOp(ring.get(), 1.0, 16, BufferOp::CAP_ROUND);
  ASSERT_NE(ring_buf, nullptr);
  EXPECT_TRUE(ring_buf->isValid());
  EXPECT_EQ(ring_buf->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
  EXPECT_NEAR(ring_buf->getArea(), 76.0 + std::numbers::pi, 0.25);

  auto mpoint = ReadWkt("MULTIPOINT ((0 0), (10 0))");
  auto mpoint_buf =
      BufferOp::bufferOp(mpoint.get(), 1.0, 16, BufferOp::CAP_ROUND);
  ASSERT_NE(mpoint_buf, nullptr);
  EXPECT_TRUE(mpoint_buf->isValid());
  EXPECT_EQ(mpoint_buf->getGeometryTypeId(), GeometryTypeId::GEOS_MULTIPOLYGON);
  EXPECT_EQ(mpoint_buf->getNumGeometries(), 2U);
  EXPECT_NEAR(mpoint_buf->getArea(), 2.0 * std::numbers::pi, 0.25);
}

TEST_F(BufferOpTest, BufferByZeroSingleAndBothOrientations) {
  auto square = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  // Simple valid polygon has empty inverted orientation -> returns buf0.
  auto square_single = BufferOp::bufferByZero(square.get(), false);
  EXPECT_NEAR(square_single->getArea(), 100.0, 1e-9);
  auto square_both = BufferOp::bufferByZero(square.get(), true);
  EXPECT_NEAR(square_both->getArea(), 100.0, 1e-9);

  // Point, LineString, and empty Polygon yield empty buf0 -> returns empty.
  auto line = ReadWkt("LINESTRING (0 0, 5 5)");
  EXPECT_TRUE(BufferOp::bufferByZero(line.get(), false)->isEmpty());
  EXPECT_TRUE(BufferOp::bufferByZero(line.get(), true)->isEmpty());

  auto empty_poly = ReadWkt("POLYGON EMPTY");
  EXPECT_TRUE(BufferOp::bufferByZero(empty_poly.get(), false)->isEmpty());
  EXPECT_TRUE(BufferOp::bufferByZero(empty_poly.get(), true)->isEmpty());

  // Unequal bowtie: left CW lobe area 100, right CCW lobe area 1, cross at
  // (0,0).
  auto unequal_bowtie =
      ReadWkt("POLYGON ((-10 -10, -10 10, 1 -1, 1 1, -10 -10))");
  auto dominant_only = BufferOp::bufferByZero(unequal_bowtie.get(), false);
  ASSERT_NE(dominant_only, nullptr);
  EXPECT_TRUE(dominant_only->isValid());
  EXPECT_EQ(dominant_only->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
  EXPECT_NEAR(dominant_only->getArea(), 100.0, 1e-9);

  auto both_lobes = BufferOp::bufferByZero(unequal_bowtie.get(), true);
  ASSERT_NE(both_lobes, nullptr);
  EXPECT_TRUE(both_lobes->isValid());
  EXPECT_EQ(both_lobes->getGeometryTypeId(), GeometryTypeId::GEOS_MULTIPOLYGON);
  EXPECT_EQ(both_lobes->getNumGeometries(), 2U);
  EXPECT_NEAR(both_lobes->getArea(), 101.0, 1e-9);

  // Equal-area bowtie has zero signed area; both orientations recover both
  // lobes.
  auto equal_bowtie =
      ReadWkt("POLYGON ((-10 -10, -10 10, 10 -10, 10 10, -10 -10))");
  auto equal_both = BufferOp::bufferByZero(equal_bowtie.get(), true);
  EXPECT_TRUE(equal_both->isValid());
  EXPECT_EQ(equal_both->getNumGeometries(), 2U);
  EXPECT_NEAR(equal_both->getArea(), 200.0, 1e-9);

  // MultiPolygon of two disjoint unequal bowties exercises MultiPolygon
  // extraction.
  auto multi_bowtie = ReadWkt(
      "MULTIPOLYGON ("
      "((-10 -10, -10 10, 1 -1, 1 1, -10 -10)), "
      "((40 -10, 40 10, 51 -1, 51 1, 40 -10)))");
  auto multi_both = BufferOp::bufferByZero(multi_bowtie.get(), true);
  ASSERT_NE(multi_both, nullptr);
  EXPECT_TRUE(multi_both->isValid());
  EXPECT_EQ(multi_both->getGeometryTypeId(), GeometryTypeId::GEOS_MULTIPOLYGON);
  EXPECT_EQ(multi_both->getNumGeometries(), 4U);
  EXPECT_NEAR(multi_both->getArea(), 202.0, 1e-9);
}

}  // namespace
}  // namespace geos_google
