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

// Unit tests for third_party/geos/src/precision/CommonBitsOp.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/precision/CommonBitsOp.cpp

#include "third_party/geos/include/geos/precision/CommonBitsOp.h"

#include <limits>
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::MultiPoint;
using geos::geom::MultiPolygon;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::precision::CommonBitsOp;
using geos::util::IllegalArgumentException;

std::unique_ptr<Polygon> MakeBox(const GeometryFactory* factory, double x0,
                                 double y0, double x1, double y1) {
  return factory->createPolygon(CoordinateSequence{
      Coordinate(x0, y0),
      Coordinate(x1, y0),
      Coordinate(x1, y1),
      Coordinate(x0, y1),
      Coordinate(x0, y0),
  });
}

void ExpectEnvelopeNear(const Geometry* geom, double min_x, double min_y,
                        double max_x, double max_y, double tol) {
  const Envelope* env = geom->getEnvelopeInternal();
  ASSERT_FALSE(env->isNull());
  EXPECT_NEAR(env->getMinX(), min_x, tol);
  EXPECT_NEAR(env->getMinY(), min_y, tol);
  EXPECT_NEAR(env->getMaxX(), max_x, tol);
  EXPECT_NEAR(env->getMaxY(), max_y, tol);
}

TEST(CommonBitsOpTest, DefaultConstructorReshiftsAllBinaryOperations) {
  constexpr double kBaseX = 1000000000.0;
  constexpr double kBaseY = 2000000000.0;

  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> p0 =
      MakeBox(factory.get(), kBaseX, kBaseY, kBaseX + 2.0, kBaseY + 2.0);
  std::unique_ptr<Polygon> p1 = MakeBox(
      factory.get(), kBaseX + 1.0, kBaseY + 1.0, kBaseX + 3.0, kBaseY + 3.0);
  Geometry::Ptr p0_snapshot = p0->clone();
  Geometry::Ptr p1_snapshot = p1->clone();

  CommonBitsOp op;

  std::unique_ptr<Geometry> inter = op.intersection(p0.get(), p1.get());
  ASSERT_NE(inter, nullptr);
  EXPECT_TRUE(inter->isValid());
  EXPECT_DOUBLE_EQ(inter->getArea(), 1.0);
  ExpectEnvelopeNear(inter.get(), kBaseX + 1.0, kBaseY + 1.0, kBaseX + 2.0,
                     kBaseY + 2.0, 0.0);

  std::unique_ptr<Polygon> expected_inter = MakeBox(
      factory.get(), kBaseX + 1.0, kBaseY + 1.0, kBaseX + 2.0, kBaseY + 2.0);
  inter->normalize();
  expected_inter->normalize();
  EXPECT_TRUE(inter->equalsExact(expected_inter.get(), 0.0));

  std::unique_ptr<Geometry> uni = op.Union(p0.get(), p1.get());
  ASSERT_NE(uni, nullptr);
  EXPECT_TRUE(uni->isValid());
  EXPECT_DOUBLE_EQ(uni->getArea(), 7.0);
  ExpectEnvelopeNear(uni.get(), kBaseX, kBaseY, kBaseX + 3.0, kBaseY + 3.0,
                     0.0);

  std::unique_ptr<Geometry> diff = op.difference(p0.get(), p1.get());
  ASSERT_NE(diff, nullptr);
  EXPECT_TRUE(diff->isValid());
  EXPECT_DOUBLE_EQ(diff->getArea(), 3.0);
  ExpectEnvelopeNear(diff.get(), kBaseX, kBaseY, kBaseX + 2.0, kBaseY + 2.0,
                     0.0);

  std::unique_ptr<Geometry> sym = op.symDifference(p0.get(), p1.get());
  ASSERT_NE(sym, nullptr);
  EXPECT_TRUE(sym->isValid());
  EXPECT_DOUBLE_EQ(sym->getArea(), 6.0);
  ExpectEnvelopeNear(sym.get(), kBaseX, kBaseY, kBaseX + 3.0, kBaseY + 3.0,
                     0.0);

  // Inputs must never be mutated by CommonBitsOp.
  EXPECT_TRUE(p0->equalsExact(p0_snapshot.get(), 0.0));
  EXPECT_TRUE(p1->equalsExact(p1_snapshot.get(), 0.0));
}

TEST(CommonBitsOpTest, ExplicitTrueConstructorReshiftsBufferAndIntersection) {
  constexpr double kBaseX = 1000000000.5;
  constexpr double kBaseY = 2000000000.25;

  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> pt = factory->createPoint(Coordinate(kBaseX, kBaseY));

  CommonBitsOp op(true);
  std::unique_ptr<Geometry> buffered = op.buffer(pt.get(), 2.0);
  ASSERT_NE(buffered, nullptr);
  EXPECT_TRUE(buffered->isValid());
  EXPECT_FALSE(buffered->isEmpty());
  ExpectEnvelopeNear(buffered.get(), kBaseX - 2.0, kBaseY - 2.0, kBaseX + 2.0,
                     kBaseY + 2.0, 1e-6);
  EXPECT_NEAR(buffered->getArea(), std::numbers::pi * 4.0, 0.2);

  EXPECT_DOUBLE_EQ(pt->getX(), kBaseX);
  EXPECT_DOUBLE_EQ(pt->getY(), kBaseY);

  std::unique_ptr<Polygon> p0 = MakeBox(
      factory.get(), 1000000000.0, 2000000000.0, 1000000002.0, 2000000002.0);
  std::unique_ptr<Polygon> p1 = MakeBox(
      factory.get(), 1000000001.0, 2000000001.0, 1000000003.0, 2000000003.0);
  std::unique_ptr<Geometry> inter = op.intersection(p0.get(), p1.get());
  EXPECT_DOUBLE_EQ(inter->getArea(), 1.0);
  ExpectEnvelopeNear(inter.get(), 1000000001.0, 2000000001.0, 1000000002.0,
                     2000000002.0, 0.0);
}

TEST(CommonBitsOpTest, FalseReturnToOriginalPrecisionKeepsShiftedOutput) {
  constexpr double kBaseX = 1000000000.0;
  constexpr double kBaseY = 2000000000.0;

  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> p0 =
      MakeBox(factory.get(), kBaseX, kBaseY, kBaseX + 2.0, kBaseY + 2.0);
  std::unique_ptr<Polygon> p1 = MakeBox(
      factory.get(), kBaseX + 1.0, kBaseY + 1.0, kBaseX + 3.0, kBaseY + 3.0);

  CommonBitsOp op(false);

  std::unique_ptr<Geometry> inter = op.intersection(p0.get(), p1.get());
  ASSERT_NE(inter, nullptr);
  EXPECT_DOUBLE_EQ(inter->getArea(), 1.0);
  ExpectEnvelopeNear(inter.get(), 1.0, 1.0, 2.0, 2.0, 0.0);

  std::unique_ptr<Geometry> uni = op.Union(p0.get(), p1.get());
  ASSERT_NE(uni, nullptr);
  EXPECT_DOUBLE_EQ(uni->getArea(), 7.0);
  ExpectEnvelopeNear(uni.get(), 0.0, 0.0, 3.0, 3.0, 0.0);

  std::unique_ptr<Geometry> diff = op.difference(p0.get(), p1.get());
  ASSERT_NE(diff, nullptr);
  EXPECT_DOUBLE_EQ(diff->getArea(), 3.0);
  ExpectEnvelopeNear(diff.get(), 0.0, 0.0, 2.0, 2.0, 0.0);

  std::unique_ptr<Geometry> sym = op.symDifference(p0.get(), p1.get());
  ASSERT_NE(sym, nullptr);
  EXPECT_DOUBLE_EQ(sym->getArea(), 6.0);
  ExpectEnvelopeNear(sym.get(), 0.0, 0.0, 3.0, 3.0, 0.0);

  std::unique_ptr<Point> pt =
      factory->createPoint(Coordinate(kBaseX + 2.5, kBaseY + 3.25));
  std::unique_ptr<Geometry> buffered = op.buffer(pt.get(), 1.5);
  ASSERT_NE(buffered, nullptr);
  ExpectEnvelopeNear(buffered.get(), -1.0, -1.25, 2.0, 1.75, 1e-9);
  EXPECT_NEAR(buffered->getArea(), std::numbers::pi * 2.25, 0.2);
}

TEST(CommonBitsOpTest, ComputeResultPrecisionAppliesOrSkipsReshift) {
  constexpr double kBaseX = 1000000000.0;
  constexpr double kBaseY = 2000000000.0;

  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> p0 =
      MakeBox(factory.get(), kBaseX, kBaseY, kBaseX + 2.0, kBaseY + 2.0);
  std::unique_ptr<Polygon> p1 = MakeBox(
      factory.get(), kBaseX + 1.0, kBaseY + 1.0, kBaseX + 3.0, kBaseY + 3.0);
  std::unique_ptr<Point> local_pt = factory->createPoint(Coordinate(1.5, 2.5));

  CommonBitsOp op_reshift(true);
  std::unique_ptr<Geometry> prior1 =
      op_reshift.intersection(p0.get(), p1.get());
  ASSERT_NE(prior1, nullptr);
  EXPECT_FALSE(prior1->isEmpty());
  std::unique_ptr<Geometry> reshifted =
      op_reshift.computeResultPrecision(local_pt->clone());
  const CoordinateXY* reshifted_coord = reshifted->getCoordinate();
  ASSERT_NE(reshifted_coord, nullptr);
  EXPECT_DOUBLE_EQ(reshifted_coord->x, kBaseX + 1.5);
  EXPECT_DOUBLE_EQ(reshifted_coord->y, kBaseY + 2.5);

  CommonBitsOp op_keep_shifted(false);
  std::unique_ptr<Geometry> prior2 =
      op_keep_shifted.intersection(p0.get(), p1.get());
  ASSERT_NE(prior2, nullptr);
  EXPECT_FALSE(prior2->isEmpty());
  std::unique_ptr<Geometry> unshifted =
      op_keep_shifted.computeResultPrecision(local_pt->clone());
  const CoordinateXY* unshifted_coord = unshifted->getCoordinate();
  ASSERT_NE(unshifted_coord, nullptr);
  EXPECT_DOUBLE_EQ(unshifted_coord->x, 1.5);
  EXPECT_DOUBLE_EQ(unshifted_coord->y, 2.5);
}

TEST(CommonBitsOpTest, CrossingLinesWithLargeCommonOffsetIntersectAccurately) {
  constexpr double kOffset = 1099511627776.0;  // 2^40

  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> line0 =
      factory->createLineString(CoordinateSequence{
          Coordinate(kOffset + 0.125, kOffset + 0.125),
          Coordinate(kOffset + 0.875, kOffset + 0.875),
      });
  std::unique_ptr<LineString> line1 =
      factory->createLineString(CoordinateSequence{
          Coordinate(kOffset + 0.125, kOffset + 0.875),
          Coordinate(kOffset + 0.875, kOffset + 0.125),
      });

  CommonBitsOp op;
  std::unique_ptr<Geometry> inter = op.intersection(line0.get(), line1.get());
  ASSERT_NE(inter, nullptr);
  EXPECT_EQ(inter->getGeometryTypeId(), geos::geom::GEOS_POINT);
  const CoordinateXY* coord = inter->getCoordinate();
  ASSERT_NE(coord, nullptr);
  EXPECT_DOUBLE_EQ(coord->x, kOffset + 0.5);
  EXPECT_DOUBLE_EQ(coord->y, kOffset + 0.5);
}

TEST(CommonBitsOpTest, HandlesDisjointEmptyZeroCommonBitsAndNegativeBuffer) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> neg = MakeBox(factory.get(), -2.0, -2.0, 0.0, 0.0);
  std::unique_ptr<Polygon> pos = MakeBox(factory.get(), 1.0, 1.0, 3.0, 3.0);

  CommonBitsOp op;
  EXPECT_TRUE(op.intersection(neg.get(), pos.get())->isEmpty());
  EXPECT_DOUBLE_EQ(op.Union(neg.get(), pos.get())->getArea(), 8.0);
  std::unique_ptr<Geometry> diff_neg = op.difference(neg.get(), pos.get());
  ASSERT_NE(diff_neg, nullptr);
  EXPECT_TRUE(diff_neg->equals(neg.get()));
  diff_neg->normalize();
  std::unique_ptr<Geometry> neg_norm = neg->clone();
  neg_norm->normalize();
  EXPECT_TRUE(diff_neg->equalsExact(neg_norm.get(), 0.0));
  EXPECT_DOUBLE_EQ(op.symDifference(neg.get(), pos.get())->getArea(), 8.0);

  std::unique_ptr<Polygon> far_box = MakeBox(
      factory.get(), 1000000000.0, 2000000000.0, 1000000004.0, 2000000004.0);
  std::unique_ptr<Polygon> empty_poly = factory->createPolygon();

  EXPECT_TRUE(op.intersection(far_box.get(), empty_poly.get())->isEmpty());
  std::unique_ptr<Geometry> uni_empty =
      op.Union(far_box.get(), empty_poly.get());
  uni_empty->normalize();
  std::unique_ptr<Geometry> far_norm = far_box->clone();
  far_norm->normalize();
  EXPECT_TRUE(uni_empty->equalsExact(far_norm.get(), 0.0));

  std::unique_ptr<Geometry> diff_empty =
      op.difference(far_box.get(), empty_poly.get());
  diff_empty->normalize();
  EXPECT_TRUE(diff_empty->equalsExact(far_norm.get(), 0.0));
  EXPECT_TRUE(op.difference(empty_poly.get(), far_box.get())->isEmpty());

  std::unique_ptr<Geometry> sym_empty_right =
      op.symDifference(far_box.get(), empty_poly.get());
  sym_empty_right->normalize();
  EXPECT_TRUE(sym_empty_right->equalsExact(far_norm.get(), 0.0));

  std::unique_ptr<Geometry> sym_empty_left =
      op.symDifference(empty_poly.get(), far_box.get());
  sym_empty_left->normalize();
  EXPECT_TRUE(sym_empty_left->equalsExact(far_norm.get(), 0.0));

  EXPECT_TRUE(op.intersection(empty_poly.get(), empty_poly.get())->isEmpty());
  EXPECT_TRUE(op.Union(empty_poly.get(), empty_poly.get())->isEmpty());
  EXPECT_TRUE(op.difference(empty_poly.get(), empty_poly.get())->isEmpty());
  EXPECT_TRUE(op.symDifference(empty_poly.get(), empty_poly.get())->isEmpty());
  EXPECT_TRUE(op.buffer(empty_poly.get(), 1.0)->isEmpty());

  std::unique_ptr<Geometry> zero_buf = op.buffer(far_box.get(), 0.0);
  EXPECT_NEAR(zero_buf->getArea(), 16.0, 1e-5);
  ExpectEnvelopeNear(zero_buf.get(), 1000000000.0, 2000000000.0, 1000000004.0,
                     2000000004.0, 1e-6);

  std::unique_ptr<Geometry> shrunk = op.buffer(far_box.get(), -1.0);
  EXPECT_NEAR(shrunk->getArea(), 4.0, 1e-5);
  ExpectEnvelopeNear(shrunk.get(), 1000000001.0, 2000000001.0, 1000000003.0,
                     2000000003.0, 1e-6);

  EXPECT_TRUE(op.buffer(far_box.get(), -3.0)->isEmpty());
}

TEST(CommonBitsOpTest, HandlesPolygonWithHoleAndMultiPolygon) {
  constexpr double kBaseX = 1000000000.0;
  constexpr double kBaseY = 2000000000.0;

  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LinearRing> shell =
      factory->createLinearRing(CoordinateSequence{
          Coordinate(kBaseX, kBaseY),
          Coordinate(kBaseX + 10.0, kBaseY),
          Coordinate(kBaseX + 10.0, kBaseY + 10.0),
          Coordinate(kBaseX, kBaseY + 10.0),
          Coordinate(kBaseX, kBaseY),
      });
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(factory->createLinearRing(CoordinateSequence{
      Coordinate(kBaseX + 2.0, kBaseY + 2.0),
      Coordinate(kBaseX + 4.0, kBaseY + 2.0),
      Coordinate(kBaseX + 4.0, kBaseY + 4.0),
      Coordinate(kBaseX + 2.0, kBaseY + 4.0),
      Coordinate(kBaseX + 2.0, kBaseY + 2.0),
  }));
  std::unique_ptr<Polygon> with_hole =
      factory->createPolygon(std::move(shell), std::move(holes));
  std::unique_ptr<Polygon> clip = MakeBox(
      factory.get(), kBaseX + 1.0, kBaseY + 1.0, kBaseX + 5.0, kBaseY + 5.0);

  CommonBitsOp op;
  std::unique_ptr<Geometry> clipped =
      op.intersection(with_hole.get(), clip.get());
  ASSERT_NE(clipped, nullptr);
  EXPECT_TRUE(clipped->isValid());
  EXPECT_DOUBLE_EQ(clipped->getArea(), 12.0);
  ExpectEnvelopeNear(clipped.get(), kBaseX + 1.0, kBaseY + 1.0, kBaseX + 5.0,
                     kBaseY + 5.0, 0.0);

  std::vector<std::unique_ptr<Polygon>> polys;
  polys.push_back(
      MakeBox(factory.get(), kBaseX, kBaseY, kBaseX + 2.0, kBaseY + 2.0));
  polys.push_back(
      MakeBox(factory.get(), kBaseX + 3.0, kBaseY, kBaseX + 5.0, kBaseY + 2.0));
  std::unique_ptr<MultiPolygon> mp =
      factory->createMultiPolygon(std::move(polys));
  std::unique_ptr<Polygon> bridge =
      MakeBox(factory.get(), kBaseX + 1.0, kBaseY, kBaseX + 4.0, kBaseY + 2.0);

  std::unique_ptr<Geometry> merged = op.Union(mp.get(), bridge.get());
  ASSERT_NE(merged, nullptr);
  EXPECT_EQ(merged->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(merged->getArea(), 10.0);
  ExpectEnvelopeNear(merged.get(), kBaseX, kBaseY, kBaseX + 5.0, kBaseY + 2.0,
                     0.0);
}

TEST(CommonBitsOpTest, BufferPropagatesNonFiniteDistanceException) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> pt =
      factory->createPoint(Coordinate(1000000000.0, 2000000000.0));

  CommonBitsOp op;
  EXPECT_THROW(op.buffer(pt.get(), std::numeric_limits<double>::infinity()),
               IllegalArgumentException);
  EXPECT_THROW(op.buffer(pt.get(), std::numeric_limits<double>::quiet_NaN()),
               IllegalArgumentException);
}

TEST(CommonBitsOpTest, HandlesLargeNegativeCommonOffsetAndReshiftToggle) {
  constexpr double kNegX = -1000000000.0;
  constexpr double kNegY = -2000000000.0;

  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> p0 =
      MakeBox(factory.get(), kNegX, kNegY, kNegX + 2.0, kNegY + 2.0);
  std::unique_ptr<Polygon> p1 = MakeBox(factory.get(), kNegX + 1.0, kNegY + 1.0,
                                        kNegX + 3.0, kNegY + 3.0);
  std::unique_ptr<Point> pt =
      factory->createPoint(Coordinate(kNegX - 0.5, kNegY - 0.25));

  CommonBitsOp op_reshift(true);
  std::unique_ptr<Geometry> inter_reshift =
      op_reshift.intersection(p0.get(), p1.get());
  ASSERT_NE(inter_reshift, nullptr);
  EXPECT_DOUBLE_EQ(inter_reshift->getArea(), 1.0);
  ExpectEnvelopeNear(inter_reshift.get(), kNegX + 1.0, kNegY + 1.0, kNegX + 2.0,
                     kNegY + 2.0, 0.0);

  std::unique_ptr<Geometry> uni_reshift = op_reshift.Union(p0.get(), p1.get());
  ASSERT_NE(uni_reshift, nullptr);
  EXPECT_DOUBLE_EQ(uni_reshift->getArea(), 7.0);
  ExpectEnvelopeNear(uni_reshift.get(), kNegX, kNegY, kNegX + 3.0, kNegY + 3.0,
                     0.0);

  std::unique_ptr<Geometry> diff_reshift =
      op_reshift.difference(p0.get(), p1.get());
  ASSERT_NE(diff_reshift, nullptr);
  EXPECT_DOUBLE_EQ(diff_reshift->getArea(), 3.0);
  ExpectEnvelopeNear(diff_reshift.get(), kNegX, kNegY, kNegX + 2.0, kNegY + 2.0,
                     0.0);

  std::unique_ptr<Geometry> sym_reshift =
      op_reshift.symDifference(p0.get(), p1.get());
  ASSERT_NE(sym_reshift, nullptr);
  EXPECT_DOUBLE_EQ(sym_reshift->getArea(), 6.0);
  ExpectEnvelopeNear(sym_reshift.get(), kNegX, kNegY, kNegX + 3.0, kNegY + 3.0,
                     0.0);

  std::unique_ptr<Geometry> buf_reshift = op_reshift.buffer(pt.get(), 2.0);
  ASSERT_NE(buf_reshift, nullptr);
  EXPECT_NEAR(buf_reshift->getArea(), std::numbers::pi * 4.0, 0.2);
  ExpectEnvelopeNear(buf_reshift.get(), kNegX - 2.5, kNegY - 2.25, kNegX + 1.5,
                     kNegY + 1.75, 1e-6);

  CommonBitsOp op_shifted(false);
  std::unique_ptr<Geometry> inter_shifted =
      op_shifted.intersection(p0.get(), p1.get());
  ASSERT_NE(inter_shifted, nullptr);
  EXPECT_DOUBLE_EQ(inter_shifted->getArea(), 1.0);
  ExpectEnvelopeNear(inter_shifted.get(), 1.0, 1.0, 2.0, 2.0, 0.0);

  std::unique_ptr<Geometry> buf_shifted = op_shifted.buffer(pt.get(), 2.0);
  ASSERT_NE(buf_shifted, nullptr);
  EXPECT_NEAR(buf_shifted->getArea(), std::numbers::pi * 4.0, 0.2);
  ExpectEnvelopeNear(buf_shifted.get(), -2.5, -2.25, 1.5, 1.75, 1e-9);
}

TEST(CommonBitsOpTest,
     ReuseSameInstanceAcrossDistinctRegionsAndAfterException) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  CommonBitsOp op;

  std::unique_ptr<Polygon> pos0 = MakeBox(
      factory.get(), 1000000000.0, 2000000000.0, 1000000002.0, 2000000002.0);
  std::unique_ptr<Polygon> pos1 = MakeBox(
      factory.get(), 1000000001.0, 2000000001.0, 1000000003.0, 2000000003.0);
  std::unique_ptr<Geometry> inter_pos = op.intersection(pos0.get(), pos1.get());
  EXPECT_DOUBLE_EQ(inter_pos->getArea(), 1.0);
  ExpectEnvelopeNear(inter_pos.get(), 1000000001.0, 2000000001.0, 1000000002.0,
                     2000000002.0, 0.0);

  std::unique_ptr<Polygon> neg0 = MakeBox(
      factory.get(), -1000000000.0, -2000000000.0, -999999998.0, -1999999998.0);
  std::unique_ptr<Polygon> neg1 = MakeBox(
      factory.get(), -999999999.0, -1999999999.0, -999999997.0, -1999999997.0);
  std::unique_ptr<Geometry> uni_neg = op.Union(neg0.get(), neg1.get());
  EXPECT_DOUBLE_EQ(uni_neg->getArea(), 7.0);
  ExpectEnvelopeNear(uni_neg.get(), -1000000000.0, -2000000000.0, -999999997.0,
                     -1999999997.0, 0.0);

  std::unique_ptr<Point> pt =
      factory->createPoint(Coordinate(1000000000.5, 2000000000.25));
  std::unique_ptr<Geometry> buf_pt = op.buffer(pt.get(), 1.0);
  EXPECT_NEAR(buf_pt->getArea(), std::numbers::pi, 0.1);
  ExpectEnvelopeNear(buf_pt.get(), 999999999.5, 1999999999.25, 1000000001.5,
                     2000000001.25, 1e-6);

  EXPECT_THROW(op.buffer(pt.get(), std::numeric_limits<double>::quiet_NaN()),
               IllegalArgumentException);

  std::unique_ptr<Polygon> mix0 = MakeBox(
      factory.get(), 500000000.0, -500000000.0, 500000002.0, -499999998.0);
  std::unique_ptr<Polygon> mix1 = MakeBox(
      factory.get(), 500000001.0, -499999999.0, 500000003.0, -499999997.0);
  std::unique_ptr<Geometry> diff_mix = op.difference(mix0.get(), mix1.get());
  EXPECT_DOUBLE_EQ(diff_mix->getArea(), 3.0);
  ExpectEnvelopeNear(diff_mix.get(), 500000000.0, -500000000.0, 500000002.0,
                     -499999998.0, 0.0);
}

TEST(CommonBitsOpTest, PuntalMultiPointOperationsWithLargeCommonOffset) {
  constexpr double kOffset = 1099511627776.0;  // 2^40

  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<MultiPoint> mp0 =
      factory->createMultiPoint(CoordinateSequence{
          Coordinate(kOffset + 0.25, kOffset + 0.25),
          Coordinate(kOffset + 0.5, kOffset + 0.5),
      });
  std::unique_ptr<MultiPoint> mp1 =
      factory->createMultiPoint(CoordinateSequence{
          Coordinate(kOffset + 0.5, kOffset + 0.5),
          Coordinate(kOffset + 0.75, kOffset + 0.75),
      });

  CommonBitsOp op;

  std::unique_ptr<Geometry> inter = op.intersection(mp0.get(), mp1.get());
  ASSERT_NE(inter, nullptr);
  EXPECT_EQ(inter->getGeometryTypeId(), geos::geom::GEOS_POINT);
  const CoordinateXY* inter_coord = inter->getCoordinate();
  ASSERT_NE(inter_coord, nullptr);
  EXPECT_DOUBLE_EQ(inter_coord->x, kOffset + 0.5);
  EXPECT_DOUBLE_EQ(inter_coord->y, kOffset + 0.5);

  std::unique_ptr<Geometry> uni = op.Union(mp0.get(), mp1.get());
  ASSERT_NE(uni, nullptr);
  EXPECT_EQ(uni->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(uni->getNumGeometries(), 3u);
  ExpectEnvelopeNear(uni.get(), kOffset + 0.25, kOffset + 0.25, kOffset + 0.75,
                     kOffset + 0.75, 0.0);

  std::unique_ptr<Geometry> diff = op.difference(mp0.get(), mp1.get());
  ASSERT_NE(diff, nullptr);
  EXPECT_EQ(diff->getGeometryTypeId(), geos::geom::GEOS_POINT);
  const CoordinateXY* diff_coord = diff->getCoordinate();
  ASSERT_NE(diff_coord, nullptr);
  EXPECT_DOUBLE_EQ(diff_coord->x, kOffset + 0.25);
  EXPECT_DOUBLE_EQ(diff_coord->y, kOffset + 0.25);

  std::unique_ptr<Geometry> sym = op.symDifference(mp0.get(), mp1.get());
  ASSERT_NE(sym, nullptr);
  EXPECT_EQ(sym->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(sym->getNumGeometries(), 2u);
  ExpectEnvelopeNear(sym.get(), kOffset + 0.25, kOffset + 0.25, kOffset + 0.75,
                     kOffset + 0.75, 0.0);
}

}  // namespace
}  // namespace geos_google
