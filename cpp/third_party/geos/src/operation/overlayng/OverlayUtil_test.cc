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

#include "third_party/geos/include/geos/operation/overlayng/OverlayUtil.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/overlayng/Edge.h"
#include "third_party/geos/include/geos/operation/overlayng/EdgeSourceInfo.h"
#include "third_party/geos/include/geos/operation/overlayng/InputGeometry.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayEdge.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayGraph.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayNG.h"
#include "third_party/geos/include/geos/util/AssertionFailedException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::operation::overlayng::Edge;
using geos::operation::overlayng::EdgeSourceInfo;
using geos::operation::overlayng::InputGeometry;
using geos::operation::overlayng::OverlayEdge;
using geos::operation::overlayng::OverlayGraph;
using geos::operation::overlayng::OverlayNG;
using geos::operation::overlayng::OverlayUtil;

class OverlayUtilTest : public ::testing::Test {
 protected:
  OverlayUtilTest() : gf_(GeometryFactory::create()), reader_(*gf_) {}

  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  GeometryFactory::Ptr gf_;
  WKTReader reader_;
};

TEST_F(OverlayUtilTest, IsFloatingPrecisionModel) {
  EXPECT_TRUE(OverlayUtil::isFloating(nullptr));

  PrecisionModel floating(PrecisionModel::FLOATING);
  EXPECT_TRUE(OverlayUtil::isFloating(&floating));

  PrecisionModel floating_single(PrecisionModel::FLOATING_SINGLE);
  EXPECT_TRUE(OverlayUtil::isFloating(&floating_single));

  PrecisionModel fixed_default(PrecisionModel::FIXED);
  EXPECT_FALSE(OverlayUtil::isFloating(&fixed_default));

  PrecisionModel fixed_scaled(100.0);
  EXPECT_FALSE(OverlayUtil::isFloating(&fixed_scaled));
}

TEST_F(OverlayUtilTest, ClippingEnvelopeReturnsFalseForUnionAndSymDifference) {
  auto a = ReadWkt("POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))");
  auto b = ReadWkt("POLYGON ((5 5, 5 15, 15 15, 15 5, 5 5))");
  InputGeometry input(a.get(), b.get());
  PrecisionModel floating;
  PrecisionModel fixed(10.0);
  Envelope env;

  EXPECT_FALSE(
      OverlayUtil::clippingEnvelope(OverlayNG::UNION, &input, &floating, env));
  EXPECT_TRUE(env.isNull());

  EXPECT_FALSE(OverlayUtil::clippingEnvelope(OverlayNG::SYMDIFFERENCE, &input,
                                             &fixed, env));
  EXPECT_TRUE(env.isNull());

  EXPECT_FALSE(OverlayUtil::clippingEnvelope(999, &input, nullptr, env));
}

TEST_F(OverlayUtilTest, ClippingEnvelopeIntersectionFloatingAndFixed) {
  auto a = ReadWkt("POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))");
  auto b = ReadWkt("POLYGON ((5 5, 5 15, 15 15, 15 5, 5 5))");
  InputGeometry input(a.get(), b.get());

  // Floating precision: safeEnv expands 10x10 boxes by 1.0 -> [-1,11] & [4,16],
  // intersection is [4,11]x[4,11]. RobustClipEnvelopeComputer expands to
  // include segments of both polygons intersecting [4,11], yielding [0,15],
  // then safeEnv expands by 0.1 * 15 = 1.5 -> [-1.5, 16.5].
  Envelope float_env;
  EXPECT_TRUE(OverlayUtil::clippingEnvelope(OverlayNG::INTERSECTION, &input,
                                            nullptr, float_env));
  EXPECT_FALSE(float_env.isNull());
  EXPECT_DOUBLE_EQ(float_env.getMinX(), -1.5);
  EXPECT_DOUBLE_EQ(float_env.getMaxX(), 16.5);
  EXPECT_DOUBLE_EQ(float_env.getMinY(), -1.5);
  EXPECT_DOUBLE_EQ(float_env.getMaxY(), 16.5);

  // Fixed precision (scale = 1.0 => gridSize = 1.0 => expand = 3.0):
  // safeEnv(A) = [-3,13], safeEnv(B) = [2,18], intersection = [2,13],
  // RobustClipEnvelopeComputer expands to [0,15], final safeEnv = [-3, 18].
  PrecisionModel fixed(1.0);
  Envelope fixed_env;
  EXPECT_TRUE(OverlayUtil::clippingEnvelope(OverlayNG::INTERSECTION, &input,
                                            &fixed, fixed_env));
  EXPECT_DOUBLE_EQ(fixed_env.getMinX(), -3.0);
  EXPECT_DOUBLE_EQ(fixed_env.getMaxX(), 18.0);
  EXPECT_DOUBLE_EQ(fixed_env.getMinY(), -3.0);
  EXPECT_DOUBLE_EQ(fixed_env.getMaxY(), 18.0);

  // Fixed precision specified by negative scale (gridSize = 2.0 => scale = 0.5
  // => expand = 6.0): safeEnv(A) = [-6,16], safeEnv(B) = [-1,21],
  // intersection = [-1,16], RobustClipEnvelopeComputer expands to [-1,16],
  // final safeEnv = [-7, 22].
  PrecisionModel fixed_grid(-2.0);
  Envelope grid_env;
  EXPECT_TRUE(OverlayUtil::clippingEnvelope(OverlayNG::INTERSECTION, &input,
                                            &fixed_grid, grid_env));
  EXPECT_DOUBLE_EQ(grid_env.getMinX(), -7.0);
  EXPECT_DOUBLE_EQ(grid_env.getMaxX(), 22.0);
  EXPECT_DOUBLE_EQ(grid_env.getMinY(), -7.0);
  EXPECT_DOUBLE_EQ(grid_env.getMaxY(), 22.0);
}

TEST_F(OverlayUtilTest, ClippingEnvelopeZeroWidthAndNullIntersection) {
  // Horizontal line segments: minSize is 0, so safeExpandDistance falls back to
  // max(height, width).
  auto line_a = ReadWkt("LINESTRING (0 0, 10 0)");
  auto line_b = ReadWkt("LINESTRING (2 0, 8 0)");
  InputGeometry line_input(line_a.get(), line_b.get());
  Envelope line_env;
  EXPECT_TRUE(OverlayUtil::clippingEnvelope(OverlayNG::INTERSECTION,
                                            &line_input, nullptr, line_env));
  EXPECT_NEAR(line_env.getMinX(), 1.28, 1e-12);
  EXPECT_NEAR(line_env.getMaxX(), 8.72, 1e-12);
  EXPECT_NEAR(line_env.getMinY(), -0.72, 1e-12);
  EXPECT_NEAR(line_env.getMaxY(), 0.72, 1e-12);

  // Zero-extent point envelope: both width and height are 0.
  auto pt_a = ReadWkt("POINT (5 5)");
  auto pt_b = ReadWkt("POINT (5 5)");
  InputGeometry pt_input(pt_a.get(), pt_b.get());
  Envelope pt_env;
  EXPECT_TRUE(OverlayUtil::clippingEnvelope(OverlayNG::INTERSECTION, &pt_input,
                                            nullptr, pt_env));
  EXPECT_DOUBLE_EQ(pt_env.getMinX(), 5.0);
  EXPECT_DOUBLE_EQ(pt_env.getMaxX(), 5.0);
  EXPECT_DOUBLE_EQ(pt_env.getMinY(), 5.0);
  EXPECT_DOUBLE_EQ(pt_env.getMaxY(), 5.0);

  // Disjoint polygons far apart: safe intersection envelope is null.
  auto far_a = ReadWkt("POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0))");
  auto far_b =
      ReadWkt("POLYGON ((100 100, 100 101, 101 101, 101 100, 100 100))");
  InputGeometry far_input(far_a.get(), far_b.get());
  Envelope far_env;
  EXPECT_TRUE(OverlayUtil::clippingEnvelope(OverlayNG::INTERSECTION, &far_input,
                                            nullptr, far_env));
  EXPECT_TRUE(far_env.isNull());
}

TEST_F(OverlayUtilTest, ClippingEnvelopeDifference) {
  auto a = ReadWkt("POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))");
  auto b = ReadWkt("POLYGON ((5 5, 5 15, 15 15, 15 5, 5 5))");
  InputGeometry input(a.get(), b.get());

  Envelope diff_env;
  EXPECT_TRUE(OverlayUtil::clippingEnvelope(OverlayNG::DIFFERENCE, &input,
                                            nullptr, diff_env));
  EXPECT_DOUBLE_EQ(diff_env.getMinX(), -2.6);
  EXPECT_DOUBLE_EQ(diff_env.getMaxX(), 16.6);
  EXPECT_DOUBLE_EQ(diff_env.getMinY(), -2.6);
  EXPECT_DOUBLE_EQ(diff_env.getMaxY(), 16.6);

  // Vertical line with null second geometry under fixed precision.
  auto vline = ReadWkt("LINESTRING (5 0, 5 10)");
  InputGeometry single_input(vline.get(), nullptr);
  PrecisionModel fixed(10.0);
  Envelope single_env;
  EXPECT_TRUE(OverlayUtil::clippingEnvelope(OverlayNG::DIFFERENCE,
                                            &single_input, &fixed, single_env));
  EXPECT_DOUBLE_EQ(single_env.getMinX(), 4.4);
  EXPECT_DOUBLE_EQ(single_env.getMaxX(), 5.6);
  EXPECT_DOUBLE_EQ(single_env.getMinY(), -0.6);
  EXPECT_DOUBLE_EQ(single_env.getMaxY(), 10.6);
}

TEST_F(OverlayUtilTest, IsEmptyResultAllOpCodes) {
  auto poly_a = ReadWkt("POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))");
  auto poly_overlap = ReadWkt("POLYGON ((5 5, 5 15, 15 15, 15 5, 5 5))");
  auto poly_disjoint = ReadWkt("POLYGON ((20 20, 20 30, 30 30, 30 20, 20 20))");
  auto empty_poly = ReadWkt("POLYGON EMPTY");
  PrecisionModel floating;

  // INTERSECTION
  EXPECT_TRUE(OverlayUtil::isEmptyResult(OverlayNG::INTERSECTION, nullptr,
                                         poly_a.get(), &floating));
  EXPECT_TRUE(OverlayUtil::isEmptyResult(OverlayNG::INTERSECTION, poly_a.get(),
                                         nullptr, &floating));
  EXPECT_TRUE(OverlayUtil::isEmptyResult(
      OverlayNG::INTERSECTION, empty_poly.get(), poly_a.get(), &floating));
  EXPECT_TRUE(OverlayUtil::isEmptyResult(OverlayNG::INTERSECTION, poly_a.get(),
                                         empty_poly.get(), &floating));
  EXPECT_TRUE(OverlayUtil::isEmptyResult(OverlayNG::INTERSECTION, poly_a.get(),
                                         poly_disjoint.get(), &floating));
  EXPECT_FALSE(OverlayUtil::isEmptyResult(OverlayNG::INTERSECTION, poly_a.get(),
                                          poly_overlap.get(), &floating));

  // Fixed precision model can bring nearly-touching disjoint envelopes
  // together.
  PrecisionModel fixed_one(1.0);
  auto box_left = ReadWkt("POLYGON ((0 0, 0 1.4, 1.4 1.4, 1.4 0, 0 0))");
  auto box_near = ReadWkt("POLYGON ((1.45 0, 1.45 1, 3 1, 3 0, 1.45 0))");
  EXPECT_TRUE(OverlayUtil::isEmptyResult(
      OverlayNG::INTERSECTION, box_left.get(), box_near.get(), &floating));
  EXPECT_FALSE(OverlayUtil::isEmptyResult(
      OverlayNG::INTERSECTION, box_left.get(), box_near.get(), &fixed_one));
  EXPECT_TRUE(OverlayUtil::isEmptyResult(OverlayNG::INTERSECTION, poly_a.get(),
                                         poly_disjoint.get(), &fixed_one));

  // DIFFERENCE
  EXPECT_TRUE(OverlayUtil::isEmptyResult(OverlayNG::DIFFERENCE, nullptr,
                                         nullptr, &floating));
  EXPECT_TRUE(OverlayUtil::isEmptyResult(OverlayNG::DIFFERENCE, nullptr,
                                         poly_a.get(), &floating));
  EXPECT_TRUE(OverlayUtil::isEmptyResult(
      OverlayNG::DIFFERENCE, empty_poly.get(), poly_a.get(), &floating));
  EXPECT_FALSE(OverlayUtil::isEmptyResult(OverlayNG::DIFFERENCE, poly_a.get(),
                                          nullptr, &floating));
  EXPECT_FALSE(OverlayUtil::isEmptyResult(OverlayNG::DIFFERENCE, poly_a.get(),
                                          empty_poly.get(), &floating));
  EXPECT_FALSE(OverlayUtil::isEmptyResult(OverlayNG::DIFFERENCE, poly_a.get(),
                                          poly_disjoint.get(), &floating));

  // UNION and SYMDIFFERENCE
  for (int op : {+OverlayNG::UNION, +OverlayNG::SYMDIFFERENCE}) {
    EXPECT_TRUE(OverlayUtil::isEmptyResult(op, nullptr, nullptr, &floating));
    EXPECT_TRUE(
        OverlayUtil::isEmptyResult(op, empty_poly.get(), nullptr, &floating));
    EXPECT_TRUE(OverlayUtil::isEmptyResult(op, empty_poly.get(),
                                           empty_poly.get(), &floating));
    EXPECT_FALSE(OverlayUtil::isEmptyResult(op, poly_a.get(), empty_poly.get(),
                                            &floating));
    EXPECT_FALSE(OverlayUtil::isEmptyResult(op, empty_poly.get(), poly_a.get(),
                                            &floating));
    EXPECT_FALSE(OverlayUtil::isEmptyResult(op, poly_a.get(),
                                            poly_overlap.get(), &floating));
  }

  // Unknown opCode falls through to false.
  EXPECT_FALSE(OverlayUtil::isEmptyResult(0, nullptr, nullptr, &floating));
}

TEST_F(OverlayUtilTest, IsEnvDisjointFloatingAndFixedAllDirections) {
  auto empty_pt = ReadWkt("POINT EMPTY");
  auto box_origin = ReadWkt("POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0))");
  auto box_overlap = ReadWkt("POLYGON ((1 0, 1 1, 2 1, 2 0, 1 0))");
  auto box_far = ReadWkt("POLYGON ((5 5, 5 6, 6 6, 6 5, 5 5))");

  EXPECT_TRUE(OverlayUtil::isEnvDisjoint(nullptr, box_origin.get(), nullptr));
  EXPECT_TRUE(OverlayUtil::isEnvDisjoint(box_origin.get(), nullptr, nullptr));
  EXPECT_TRUE(
      OverlayUtil::isEnvDisjoint(empty_pt.get(), box_origin.get(), nullptr));
  EXPECT_TRUE(
      OverlayUtil::isEnvDisjoint(box_origin.get(), empty_pt.get(), nullptr));

  // Floating PM
  EXPECT_TRUE(
      OverlayUtil::isEnvDisjoint(box_origin.get(), box_far.get(), nullptr));
  EXPECT_FALSE(
      OverlayUtil::isEnvDisjoint(box_origin.get(), box_overlap.get(), nullptr));

  // Fixed PM with grid size = 1.0 (rounds .4 -> 0/1, .6 -> 1/2).
  PrecisionModel fixed(1.0);
  auto box_center = ReadWkt("POLYGON ((0 0, 0 1.4, 1.4 1.4, 1.4 0, 0 0))");
  auto box_right = ReadWkt("POLYGON ((1.6 0, 1.6 1, 3 1, 3 0, 1.6 0))");
  auto box_above = ReadWkt("POLYGON ((0 1.6, 0 3, 1 3, 1 1.6, 0 1.6))");
  auto box_almost_touching =
      ReadWkt("POLYGON ((1.45 0, 1.45 1, 3 1, 3 0, 1.45 0))");

  // B strictly right of A after rounding (2 > 1)
  EXPECT_TRUE(
      OverlayUtil::isEnvDisjoint(box_center.get(), box_right.get(), &fixed));
  // B strictly left of A after rounding (1 < 2)
  EXPECT_TRUE(
      OverlayUtil::isEnvDisjoint(box_right.get(), box_center.get(), &fixed));
  // B strictly above A after rounding (2 > 1)
  EXPECT_TRUE(
      OverlayUtil::isEnvDisjoint(box_center.get(), box_above.get(), &fixed));
  // B strictly below A after rounding (1 < 2)
  EXPECT_TRUE(
      OverlayUtil::isEnvDisjoint(box_above.get(), box_center.get(), &fixed));

  // Disjoint in floating precision, but NOT disjoint once rounded to grid=1.0
  // because 1.4 rounds to 1 and 1.45 also rounds to 1.
  EXPECT_TRUE(OverlayUtil::isEnvDisjoint(box_center.get(),
                                         box_almost_touching.get(), nullptr));
  EXPECT_FALSE(OverlayUtil::isEnvDisjoint(box_center.get(),
                                          box_almost_touching.get(), &fixed));
}

TEST_F(OverlayUtilTest, CreateEmptyResultAllDimensionsAndInvalid) {
  for (size_t coord_dim : {size_t{2}, size_t{3}, size_t{4}}) {
    auto pt = OverlayUtil::createEmptyResult(0, coord_dim, gf_.get());
    ASSERT_NE(pt, nullptr);
    EXPECT_EQ(pt->getGeometryTypeId(), geos::geom::GEOS_POINT);
    EXPECT_TRUE(pt->isEmpty());
    EXPECT_EQ(pt->getCoordinateDimension(), coord_dim);
    EXPECT_EQ(pt->hasZ(), coord_dim >= 3);
    EXPECT_EQ(pt->hasM(), coord_dim == 4);

    auto line = OverlayUtil::createEmptyResult(1, coord_dim, gf_.get());
    ASSERT_NE(line, nullptr);
    EXPECT_EQ(line->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
    EXPECT_TRUE(line->isEmpty());
    EXPECT_EQ(line->getCoordinateDimension(), coord_dim);
    EXPECT_EQ(line->hasZ(), coord_dim >= 3);
    EXPECT_EQ(line->hasM(), coord_dim == 4);

    auto poly = OverlayUtil::createEmptyResult(2, coord_dim, gf_.get());
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(poly->isEmpty());
    EXPECT_EQ(poly->getCoordinateDimension(), coord_dim);
    EXPECT_EQ(poly->hasZ(), coord_dim >= 3);
    EXPECT_EQ(poly->hasM(), coord_dim == 4);
  }

  auto gc = OverlayUtil::createEmptyResult(-1, 2, gf_.get());
  ASSERT_NE(gc, nullptr);
  EXPECT_EQ(gc->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_TRUE(gc->isEmpty());

  EXPECT_THROW(OverlayUtil::createEmptyResult(3, 2, gf_.get()),
               geos::util::AssertionFailedException);
  EXPECT_THROW(OverlayUtil::createEmptyResult(-2, 2, gf_.get()),
               geos::util::AssertionFailedException);
}

TEST_F(OverlayUtilTest, ResultDimensionAllOperations) {
  EXPECT_EQ(OverlayUtil::resultDimension(OverlayNG::INTERSECTION, 0, 2), 0);
  EXPECT_EQ(OverlayUtil::resultDimension(OverlayNG::INTERSECTION, 2, 1), 1);
  EXPECT_EQ(OverlayUtil::resultDimension(OverlayNG::INTERSECTION, -1, 2), -1);

  EXPECT_EQ(OverlayUtil::resultDimension(OverlayNG::UNION, 0, 2), 2);
  EXPECT_EQ(OverlayUtil::resultDimension(OverlayNG::UNION, 1, 0), 1);
  EXPECT_EQ(OverlayUtil::resultDimension(OverlayNG::UNION, -1, 1), 1);

  EXPECT_EQ(OverlayUtil::resultDimension(OverlayNG::DIFFERENCE, 2, 0), 2);
  EXPECT_EQ(OverlayUtil::resultDimension(OverlayNG::DIFFERENCE, 0, 2), 0);
  EXPECT_EQ(OverlayUtil::resultDimension(OverlayNG::DIFFERENCE, 1, -1), 1);

  EXPECT_EQ(OverlayUtil::resultDimension(OverlayNG::SYMDIFFERENCE, 0, 2), 2);
  EXPECT_EQ(OverlayUtil::resultDimension(OverlayNG::SYMDIFFERENCE, 1, 0), 1);
  EXPECT_EQ(OverlayUtil::resultDimension(OverlayNG::SYMDIFFERENCE, -1, 2), 2);

  EXPECT_EQ(OverlayUtil::resultDimension(0, 2, 2), -1);
}

TEST_F(OverlayUtilTest, ResultCoordinateDimension) {
  EXPECT_EQ(OverlayUtil::resultCoordinateDimension(2, 2), 2);
  EXPECT_EQ(OverlayUtil::resultCoordinateDimension(2, 3), 2);
  EXPECT_EQ(OverlayUtil::resultCoordinateDimension(3, 2), 2);
  EXPECT_EQ(OverlayUtil::resultCoordinateDimension(2, 4), 2);
  EXPECT_EQ(OverlayUtil::resultCoordinateDimension(4, 2), 2);
  EXPECT_EQ(OverlayUtil::resultCoordinateDimension(3, 3), 3);
  EXPECT_EQ(OverlayUtil::resultCoordinateDimension(3, 4), 3);
  EXPECT_EQ(OverlayUtil::resultCoordinateDimension(4, 3), 3);
  EXPECT_EQ(OverlayUtil::resultCoordinateDimension(4, 4), 4);
  // Single-geometry / unknown dimension (< 2) cases:
  EXPECT_EQ(OverlayUtil::resultCoordinateDimension(0, 2), 2);
  EXPECT_EQ(OverlayUtil::resultCoordinateDimension(0, 3), 3);
  EXPECT_EQ(OverlayUtil::resultCoordinateDimension(3, 0), 3);
  EXPECT_EQ(OverlayUtil::resultCoordinateDimension(0, 4), 4);
  EXPECT_EQ(OverlayUtil::resultCoordinateDimension(0, 0), 4);
  EXPECT_EQ(OverlayUtil::resultCoordinateDimension(1, 3), 3);
  EXPECT_EQ(OverlayUtil::resultCoordinateDimension(2, 1), 2);
  EXPECT_EQ(OverlayUtil::resultCoordinateDimension(1, 1), 4);
  EXPECT_EQ(OverlayUtil::resultCoordinateDimension(5, 5), 4);
}

TEST_F(OverlayUtilTest, CreateResultGeometryEmptyHomogeneousAndMixed) {
  // All empty -> empty GeometryCollection.
  {
    std::vector<std::unique_ptr<Polygon>> polys;
    std::vector<std::unique_ptr<LineString>> lines;
    std::vector<std::unique_ptr<Point>> pts;
    auto res = OverlayUtil::createResultGeometry(polys, lines, pts, gf_.get());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
    EXPECT_TRUE(res->isEmpty());
  }

  // Single polygon -> Polygon.
  {
    std::vector<std::unique_ptr<Polygon>> polys;
    std::vector<std::unique_ptr<LineString>> lines;
    std::vector<std::unique_ptr<Point>> pts;
    polys.push_back(
        reader_.read<Polygon>("POLYGON ((0 0, 0 2, 2 2, 2 0, 0 0))"));
    auto res = OverlayUtil::createResultGeometry(polys, lines, pts, gf_.get());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_DOUBLE_EQ(res->getArea(), 4.0);
  }

  // Multiple polygons -> MultiPolygon.
  {
    std::vector<std::unique_ptr<Polygon>> polys;
    std::vector<std::unique_ptr<LineString>> lines;
    std::vector<std::unique_ptr<Point>> pts;
    polys.push_back(
        reader_.read<Polygon>("POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0))"));
    polys.push_back(
        reader_.read<Polygon>("POLYGON ((2 2, 2 3, 3 3, 3 2, 2 2))"));
    auto res = OverlayUtil::createResultGeometry(polys, lines, pts, gf_.get());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
    EXPECT_EQ(res->getNumGeometries(), 2u);
  }

  // Single line -> LineString.
  {
    std::vector<std::unique_ptr<Polygon>> polys;
    std::vector<std::unique_ptr<LineString>> lines;
    std::vector<std::unique_ptr<Point>> pts;
    lines.push_back(reader_.read<LineString>("LINESTRING (0 0, 1 0)"));
    auto res = OverlayUtil::createResultGeometry(polys, lines, pts, gf_.get());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  }

  // Multiple lines -> MultiLineString.
  {
    std::vector<std::unique_ptr<Polygon>> polys;
    std::vector<std::unique_ptr<LineString>> lines;
    std::vector<std::unique_ptr<Point>> pts;
    lines.push_back(reader_.read<LineString>("LINESTRING (0 0, 1 0)"));
    lines.push_back(reader_.read<LineString>("LINESTRING (0 1, 1 1)"));
    auto res = OverlayUtil::createResultGeometry(polys, lines, pts, gf_.get());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
    EXPECT_EQ(res->getNumGeometries(), 2u);
  }

  // Single point -> Point.
  {
    std::vector<std::unique_ptr<Polygon>> polys;
    std::vector<std::unique_ptr<LineString>> lines;
    std::vector<std::unique_ptr<Point>> pts;
    pts.push_back(reader_.read<Point>("POINT (3 4)"));
    auto res = OverlayUtil::createResultGeometry(polys, lines, pts, gf_.get());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POINT);
  }

  // Multiple points -> MultiPoint.
  {
    std::vector<std::unique_ptr<Polygon>> polys;
    std::vector<std::unique_ptr<LineString>> lines;
    std::vector<std::unique_ptr<Point>> pts;
    pts.push_back(reader_.read<Point>("POINT (3 4)"));
    pts.push_back(reader_.read<Point>("POINT (5 6)"));
    auto res = OverlayUtil::createResultGeometry(polys, lines, pts, gf_.get());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
    EXPECT_EQ(res->getNumGeometries(), 2u);
  }

  // Mixed A, L, P -> GeometryCollection ordered A, L, P.
  {
    std::vector<std::unique_ptr<Polygon>> polys;
    std::vector<std::unique_ptr<LineString>> lines;
    std::vector<std::unique_ptr<Point>> pts;
    polys.push_back(
        reader_.read<Polygon>("POLYGON ((0 0, 0 2, 2 2, 2 0, 0 0))"));
    lines.push_back(reader_.read<LineString>("LINESTRING (3 0, 4 0)"));
    pts.push_back(reader_.read<Point>("POINT (5 5)"));
    auto res = OverlayUtil::createResultGeometry(polys, lines, pts, gf_.get());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
    ASSERT_EQ(res->getNumGeometries(), 3u);
    EXPECT_EQ(res->getGeometryN(0)->getGeometryTypeId(),
              geos::geom::GEOS_POLYGON);
    EXPECT_EQ(res->getGeometryN(1)->getGeometryTypeId(),
              geos::geom::GEOS_LINESTRING);
    EXPECT_EQ(res->getGeometryN(2)->getGeometryTypeId(),
              geos::geom::GEOS_POINT);
  }
}

TEST_F(OverlayUtilTest, ToLinesEmptyAndResultAreaFiltering) {
  OverlayGraph empty_graph;
  auto empty_res = OverlayUtil::toLines(&empty_graph, true, gf_.get());
  EXPECT_EQ(empty_res->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_TRUE(empty_res->isEmpty());

  OverlayGraph graph;
  EdgeSourceInfo info0(0, 1, false);
  auto seq1 = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{{0, 0}, {10, 0}});
  Edge edge1(std::move(seq1), &info0);
  OverlayEdge* oe1 = graph.addEdge(&edge1);

  auto seq2 = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{{10, 0}, {10, 10}});
  Edge edge2(std::move(seq2), &info0);
  OverlayEdge* oe2 = graph.addEdge(&edge2);

  // Neither edge marked in result area.
  auto none_res = OverlayUtil::toLines(&graph, false, gf_.get());
  EXPECT_EQ(none_res->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_TRUE(none_res->isEmpty());

  // Mark only reverse half-edge of edge1 in result area -> single LineString
  // with reversed orientation (10,0) -> (0,0).
  oe1->symOE()->markInResultArea();
  auto single_res = OverlayUtil::toLines(&graph, false, gf_.get());
  ASSERT_EQ(single_res->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  auto single_coords = single_res->getCoordinates();
  ASSERT_EQ(single_coords->size(), 2u);
  EXPECT_DOUBLE_EQ(single_coords->getX(0), 10.0);
  EXPECT_DOUBLE_EQ(single_coords->getY(0), 0.0);
  EXPECT_DOUBLE_EQ(single_coords->getX(1), 0.0);
  EXPECT_DOUBLE_EQ(single_coords->getY(1), 0.0);

  // Mark forward edge2 as well -> MultiLineString with 2 LineStrings.
  oe2->markInResultArea();
  auto two_res = OverlayUtil::toLines(&graph, false, gf_.get());
  EXPECT_EQ(two_res->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(two_res->getNumGeometries(), 2u);

  // isOutputEdges = true outputs all half-edges (4 total: 2 forward + 2 sym).
  auto all_res = OverlayUtil::toLines(&graph, true, gf_.get());
  EXPECT_EQ(all_res->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(all_res->getNumGeometries(), 4u);

  // Multi-vertex edge reversed in result area preserves all intermediate
  // points.
  OverlayGraph multi_pt_graph;
  auto seq3 = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{{0, 0}, {5, 3}, {10, 0}});
  Edge edge3(std::move(seq3), &info0);
  OverlayEdge* oe3 = multi_pt_graph.addEdge(&edge3);
  oe3->symOE()->markInResultArea();
  auto rev_multi = OverlayUtil::toLines(&multi_pt_graph, false, gf_.get());
  ASSERT_EQ(rev_multi->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  auto rev_coords = rev_multi->getCoordinates();
  ASSERT_EQ(rev_coords->size(), 3u);
  EXPECT_DOUBLE_EQ(rev_coords->getX(0), 10.0);
  EXPECT_DOUBLE_EQ(rev_coords->getX(1), 5.0);
  EXPECT_DOUBLE_EQ(rev_coords->getY(1), 3.0);
  EXPECT_DOUBLE_EQ(rev_coords->getX(2), 0.0);
}

TEST_F(OverlayUtilTest, IsResultAreaConsistentAllOperations) {
  auto rect_100 = ReadWkt("POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))");
  auto rect_25 = ReadWkt("POLYGON ((0 0, 0 5, 5 5, 5 0, 0 0))");
  auto rect_75 = ReadWkt("POLYGON ((0 0, 0 7.5, 10 7.5, 10 0, 0 0))");
  auto rect_125 = ReadWkt("POLYGON ((0 0, 0 12.5, 10 12.5, 10 0, 0 0))");
  auto rect_200 = ReadWkt("POLYGON ((0 0, 0 20, 10 20, 10 0, 0 0))");

  // Null input returns true.
  EXPECT_TRUE(OverlayUtil::isResultAreaConsistent(
      nullptr, rect_100.get(), OverlayNG::INTERSECTION, rect_200.get()));
  EXPECT_TRUE(OverlayUtil::isResultAreaConsistent(
      rect_100.get(), nullptr, OverlayNG::INTERSECTION, rect_200.get()));

  // INTERSECTION: result <= min(A, B) * 1.1
  EXPECT_TRUE(OverlayUtil::isResultAreaConsistent(
      rect_100.get(), rect_25.get(), OverlayNG::INTERSECTION, rect_25.get()));
  EXPECT_FALSE(OverlayUtil::isResultAreaConsistent(
      rect_25.get(), rect_100.get(), OverlayNG::INTERSECTION, rect_75.get()));
  EXPECT_FALSE(OverlayUtil::isResultAreaConsistent(
      rect_100.get(), rect_25.get(), OverlayNG::INTERSECTION, rect_75.get()));
  EXPECT_FALSE(OverlayUtil::isResultAreaConsistent(
      rect_100.get(), rect_100.get(), OverlayNG::INTERSECTION, rect_125.get()));

  // DIFFERENCE: 0.9 * (A - B) <= result <= 1.1 * A
  EXPECT_TRUE(OverlayUtil::isResultAreaConsistent(
      rect_100.get(), rect_25.get(), OverlayNG::DIFFERENCE, rect_75.get()));
  EXPECT_FALSE(OverlayUtil::isResultAreaConsistent(
      rect_100.get(), rect_25.get(), OverlayNG::DIFFERENCE, rect_125.get()));
  EXPECT_FALSE(OverlayUtil::isResultAreaConsistent(
      rect_100.get(), rect_25.get(), OverlayNG::DIFFERENCE, rect_25.get()));

  // SYMDIFFERENCE: result <= 1.1 * (A + B)
  EXPECT_TRUE(OverlayUtil::isResultAreaConsistent(
      rect_100.get(), rect_25.get(), OverlayNG::SYMDIFFERENCE, rect_125.get()));
  EXPECT_FALSE(OverlayUtil::isResultAreaConsistent(
      rect_100.get(), rect_25.get(), OverlayNG::SYMDIFFERENCE, rect_200.get()));

  // UNION: A <= 1.1 * result && B <= 1.1 * result && result >= 0.9 * (A - B)
  EXPECT_TRUE(OverlayUtil::isResultAreaConsistent(
      rect_100.get(), rect_25.get(), OverlayNG::UNION, rect_100.get()));
  EXPECT_FALSE(OverlayUtil::isResultAreaConsistent(
      rect_100.get(), rect_25.get(), OverlayNG::UNION, rect_75.get()));
  EXPECT_FALSE(OverlayUtil::isResultAreaConsistent(
      rect_25.get(), rect_100.get(), OverlayNG::UNION, rect_75.get()));

  // Unknown opCode returns true.
  EXPECT_TRUE(OverlayUtil::isResultAreaConsistent(rect_100.get(), rect_25.get(),
                                                  0, rect_200.get()));
}

TEST_F(OverlayUtilTest, RoundPointEmptyFloatingAndFixed) {
  auto empty_pt = gf_->createPoint();
  Coordinate out(42.0, 43.0, 44.0);
  PrecisionModel fixed(10.0);
  EXPECT_FALSE(OverlayUtil::round(empty_pt.get(), &fixed, out));
  EXPECT_DOUBLE_EQ(out.x, 42.0);
  EXPECT_DOUBLE_EQ(out.y, 43.0);

  auto pt_3d = gf_->createPoint(Coordinate(1.26, 3.74, 9.5));
  PrecisionModel floating;
  EXPECT_TRUE(OverlayUtil::round(pt_3d.get(), nullptr, out));
  EXPECT_DOUBLE_EQ(out.x, 1.26);
  EXPECT_DOUBLE_EQ(out.y, 3.74);
  EXPECT_DOUBLE_EQ(out.z, 9.5);

  EXPECT_TRUE(OverlayUtil::round(pt_3d.get(), &floating, out));
  EXPECT_DOUBLE_EQ(out.x, 1.26);
  EXPECT_DOUBLE_EQ(out.y, 3.74);

  EXPECT_TRUE(OverlayUtil::round(pt_3d.get(), &fixed, out));
  EXPECT_DOUBLE_EQ(out.x, 1.3);
  EXPECT_DOUBLE_EQ(out.y, 3.7);
  EXPECT_DOUBLE_EQ(out.z, 9.5);

  auto pt_neg = gf_->createPoint(Coordinate(-1.6, 2.4));
  PrecisionModel fixed_int(1.0);
  EXPECT_TRUE(OverlayUtil::round(pt_neg.get(), &fixed_int, out));
  EXPECT_DOUBLE_EQ(out.x, -2.0);
  EXPECT_DOUBLE_EQ(out.y, 2.0);
  EXPECT_TRUE(std::isnan(out.z));

  auto pt_xy = gf_->createPoint(CoordinateXY(4.49, -3.51));
  EXPECT_TRUE(OverlayUtil::round(pt_xy.get(), &fixed_int, out));
  EXPECT_DOUBLE_EQ(out.x, 4.0);
  EXPECT_DOUBLE_EQ(out.y, -4.0);
  EXPECT_TRUE(std::isnan(out.z));
}

}  // namespace
}  // namespace geos_google
