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

#include "third_party/geos/include/geos/algorithm/MinimumDiameter.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using ::geos::algorithm::MinimumDiameter;
using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::geom::Geometry;
using ::geos::geom::GeometryFactory;
using ::geos::geom::GeometryTypeId;
using ::geos::geom::LinearRing;
using ::geos::geom::LineString;
using ::geos::geom::PrecisionModel;
using ::geos::io::WKTReader;

class MinimumDiameterTest : public ::testing::Test {
 protected:
  MinimumDiameterTest()
      : factory_(GeometryFactory::create()), reader_(factory_.get()) {}

  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

void ExpectRectangleMatchesDimensions(const Geometry* rect,
                                      const Geometry* input,
                                      double expected_width,
                                      double expected_length,
                                      double tol = 1e-9) {
  ASSERT_NE(rect, nullptr);
  EXPECT_EQ(rect->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
  EXPECT_FALSE(rect->isEmpty());
  EXPECT_EQ(rect->getNumPoints(), 5u);
  EXPECT_NEAR(rect->getArea(), expected_width * expected_length, tol);

  std::unique_ptr<CoordinateSequence> coords = rect->getCoordinates();
  ASSERT_EQ(coords->getSize(), 5u);
  EXPECT_TRUE(coords->getAt<CoordinateXY>(0).equals2D(
      coords->getAt<CoordinateXY>(4), tol));

  const CoordinateXY& p0 = coords->getAt<CoordinateXY>(0);
  const CoordinateXY& p1 = coords->getAt<CoordinateXY>(1);
  const CoordinateXY& p2 = coords->getAt<CoordinateXY>(2);
  const CoordinateXY& p3 = coords->getAt<CoordinateXY>(3);

  double d01 = p0.distance(p1);
  double d12 = p1.distance(p2);
  double d23 = p2.distance(p3);
  double d30 = p3.distance(p0);
  EXPECT_NEAR(d01, d23, tol);
  EXPECT_NEAR(d12, d30, tol);
  EXPECT_NEAR(std::min(d01, d12), expected_width, tol);
  EXPECT_NEAR(std::max(d01, d12), expected_length, tol);

  // Adjacent edges must be orthogonal.
  double dot = (p1.x - p0.x) * (p2.x - p1.x) + (p1.y - p0.y) * (p2.y - p1.y);
  EXPECT_NEAR(dot, 0.0, tol);

  // The enclosing rectangle must cover the input geometry (within FP epsilon).
  std::unique_ptr<Geometry> buffered = rect->buffer(tol);
  EXPECT_TRUE(buffered->covers(input));
}

TEST_F(MinimumDiameterTest, EmptyGeometriesReturnEmptyResults) {
  const std::vector<std::string> empty_wkts = {
      "POINT EMPTY",      "LINESTRING EMPTY",         "POLYGON EMPTY",
      "MULTIPOINT EMPTY", "GEOMETRYCOLLECTION EMPTY",
  };

  for (const std::string& wkt : empty_wkts) {
    SCOPED_TRACE(wkt);
    std::unique_ptr<Geometry> geom = ReadWkt(wkt);
    MinimumDiameter md(geom.get());

    EXPECT_DOUBLE_EQ(md.getLength(), 0.0);
    EXPECT_TRUE(md.getWidthCoordinate().isNull());

    std::unique_ptr<LineString> diam = md.getDiameter();
    ASSERT_NE(diam, nullptr);
    EXPECT_TRUE(diam->isEmpty());
    EXPECT_EQ(diam->getGeometryTypeId(), GeometryTypeId::GEOS_LINESTRING);

    std::unique_ptr<Geometry> rect = md.getMinimumRectangle();
    ASSERT_NE(rect, nullptr);
    EXPECT_TRUE(rect->isEmpty());
    EXPECT_EQ(rect->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);

    std::unique_ptr<LineString> support = md.getSupportingSegment();
    ASSERT_NE(support, nullptr);
    EXPECT_DOUBLE_EQ(support->getLength(), 0.0);
  }
}

TEST_F(MinimumDiameterTest, EmptyGeometriesWithConvexHint) {
  std::unique_ptr<Geometry> empty_poly = ReadWkt("POLYGON EMPTY");
  MinimumDiameter md_poly(empty_poly.get(), true);
  EXPECT_DOUBLE_EQ(md_poly.getLength(), 0.0);
  EXPECT_TRUE(md_poly.getWidthCoordinate().isNull());
  EXPECT_TRUE(md_poly.getDiameter()->isEmpty());
  EXPECT_TRUE(md_poly.getMinimumRectangle()->isEmpty());

  std::unique_ptr<Geometry> empty_pt = ReadWkt("POINT EMPTY");
  MinimumDiameter md_pt(empty_pt.get(), true);
  EXPECT_DOUBLE_EQ(md_pt.getLength(), 0.0);
  EXPECT_TRUE(md_pt.getWidthCoordinate().isNull());
  EXPECT_TRUE(md_pt.getDiameter()->isEmpty());
  EXPECT_TRUE(md_pt.getMinimumRectangle()->isEmpty());
}

TEST_F(MinimumDiameterTest, SinglePointAndDuplicateMultiPoint) {
  std::unique_ptr<Geometry> pt = ReadWkt("POINT (3 4)");
  MinimumDiameter md(pt.get());

  EXPECT_DOUBLE_EQ(md.getLength(), 0.0);
  const Coordinate& width_coord = md.getWidthCoordinate();
  EXPECT_FALSE(width_coord.isNull());
  EXPECT_DOUBLE_EQ(width_coord.x, 3.0);
  EXPECT_DOUBLE_EQ(width_coord.y, 4.0);

  std::unique_ptr<LineString> support = md.getSupportingSegment();
  ASSERT_NE(support, nullptr);
  EXPECT_DOUBLE_EQ(support->getLength(), 0.0);
  EXPECT_DOUBLE_EQ(support->getCoordinateN(0).x, 3.0);
  EXPECT_DOUBLE_EQ(support->getCoordinateN(0).y, 4.0);

  std::unique_ptr<LineString> diam = md.getDiameter();
  ASSERT_NE(diam, nullptr);
  EXPECT_EQ(diam->getNumPoints(), 2u);
  EXPECT_DOUBLE_EQ(diam->getLength(), 0.0);

  std::unique_ptr<Geometry> rect = md.getMinimumRectangle();
  ASSERT_NE(rect, nullptr);
  EXPECT_EQ(rect->getGeometryTypeId(), GeometryTypeId::GEOS_POINT);
  EXPECT_TRUE(rect->equals(pt.get()));

  // Also test MultiPoint with duplicate points and convex hint on Point.
  std::unique_ptr<Geometry> mp = ReadWkt("MULTIPOINT ((3 4), (3 4), (3 4))");
  MinimumDiameter md_mp(mp.get());
  EXPECT_DOUBLE_EQ(md_mp.getLength(), 0.0);
  EXPECT_TRUE(md_mp.getMinimumRectangle()->equals(pt.get()));

  MinimumDiameter md_pt_convex(pt.get(), true);
  EXPECT_DOUBLE_EQ(md_pt_convex.getLength(), 0.0);
  EXPECT_TRUE(md_pt_convex.getMinimumRectangle()->equals(pt.get()));
}

TEST_F(MinimumDiameterTest, LineStringDegenerateCases) {
  // Horizontal left-to-right 2-point LineString.
  std::unique_ptr<Geometry> horiz = ReadWkt("LINESTRING (0 2, 10 2)");
  MinimumDiameter md_horiz(horiz.get());
  EXPECT_DOUBLE_EQ(md_horiz.getLength(), 0.0);
  std::unique_ptr<Geometry> horiz_rect = md_horiz.getMinimumRectangle();
  EXPECT_EQ(horiz_rect->getGeometryTypeId(), GeometryTypeId::GEOS_LINESTRING);
  EXPECT_TRUE(horiz_rect->equals(horiz.get()));
  EXPECT_DOUBLE_EQ(md_horiz.getSupportingSegment()->getLength(), 10.0);
  EXPECT_DOUBLE_EQ(md_horiz.getDiameter()->getLength(), 0.0);

  // Horizontal right-to-left (exercises ptMinX update in computeMaximumLine).
  std::unique_ptr<Geometry> horiz_rev = ReadWkt("LINESTRING (10 2, 0 2)");
  std::unique_ptr<Geometry> horiz_rev_rect =
      MinimumDiameter::getMinimumRectangle(horiz_rev.get());
  EXPECT_TRUE(horiz_rev_rect->equals(horiz.get()));

  // Vertical bottom-to-top (exercises p0.x == p1.x in computeMaximumLine).
  std::unique_ptr<Geometry> vert = ReadWkt("LINESTRING (5 1, 5 11)");
  MinimumDiameter md_vert(vert.get(), true);
  EXPECT_DOUBLE_EQ(md_vert.getLength(), 0.0);
  std::unique_ptr<Geometry> vert_rect = md_vert.getMinimumRectangle();
  EXPECT_EQ(vert_rect->getGeometryTypeId(), GeometryTypeId::GEOS_LINESTRING);
  EXPECT_TRUE(vert_rect->equals(vert.get()));

  // Vertical top-to-bottom (exercises ptMinY update in computeMaximumLine).
  std::unique_ptr<Geometry> vert_rev = ReadWkt("LINESTRING (5 11, 5 1)");
  std::unique_ptr<Geometry> vert_rev_rect =
      MinimumDiameter::getMinimumRectangle(vert_rev.get());
  EXPECT_TRUE(vert_rev_rect->equals(vert.get()));
}

TEST_F(MinimumDiameterTest, ThreeAndFourPointConvexHintDegenerateCases) {
  // 3-point collinear LineString with convex=true exercises switch case 3.
  std::unique_ptr<Geometry> line3 = ReadWkt("LINESTRING (5 5, 0 0, 10 10)");
  MinimumDiameter md3(line3.get(), true);
  EXPECT_DOUBLE_EQ(md3.getLength(), 0.0);
  EXPECT_DOUBLE_EQ(md3.getWidthCoordinate().x, 5.0);
  EXPECT_DOUBLE_EQ(md3.getWidthCoordinate().y, 5.0);

  std::unique_ptr<Geometry> rect3 = md3.getMinimumRectangle();
  ASSERT_EQ(rect3->getGeometryTypeId(), GeometryTypeId::GEOS_LINESTRING);
  std::unique_ptr<Geometry> expected3 = ReadWkt("LINESTRING (0 0, 10 10)");
  EXPECT_TRUE(rect3->equals(expected3.get()));

  // 3-point vertical LineString with convex=true exercises case 3 + vertical
  // line.
  std::unique_ptr<Geometry> vline3 = ReadWkt("LINESTRING (2 5, 2 0, 2 12)");
  MinimumDiameter md_v3(vline3.get(), true);
  EXPECT_DOUBLE_EQ(md_v3.getLength(), 0.0);
  std::unique_ptr<Geometry> vrect3 = md_v3.getMinimumRectangle();
  std::unique_ptr<Geometry> vexpected3 = ReadWkt("LINESTRING (2 0, 2 12)");
  EXPECT_TRUE(vrect3->equals(vexpected3.get()));

  // 3-point LineString whose first two vertices coincide -> minBaseSeg is
  // point.
  std::unique_ptr<Geometry> dup_start = ReadWkt("LINESTRING (2 3, 2 3, 6 7)");
  MinimumDiameter md_dup(dup_start.get(), true);
  EXPECT_DOUBLE_EQ(md_dup.getLength(), 0.0);
  std::unique_ptr<Geometry> rect_dup = md_dup.getMinimumRectangle();
  EXPECT_EQ(rect_dup->getGeometryTypeId(), GeometryTypeId::GEOS_POINT);
  EXPECT_TRUE(rect_dup->equals(ReadWkt("POINT (2 3)").get()));

  // 4-point collinear LineString with convex=true enters default ring search
  // where perpendicular distance is identically 0, hitting nextIndex ==
  // startIndex.
  std::unique_ptr<Geometry> line4 = ReadWkt("LINESTRING (0 0, 1 0, 2 0, 3 0)");
  MinimumDiameter md4(line4.get(), true);
  EXPECT_DOUBLE_EQ(md4.getLength(), 0.0);
  std::unique_ptr<Geometry> rect4 = md4.getMinimumRectangle();
  EXPECT_EQ(rect4->getGeometryTypeId(), GeometryTypeId::GEOS_LINESTRING);
  EXPECT_TRUE(rect4->equals(ReadWkt("LINESTRING (0 0, 3 0)").get()));
}

TEST_F(MinimumDiameterTest, AxisAlignedRectanglesWideAndTall) {
  std::unique_ptr<Geometry> wide =
      ReadWkt("POLYGON ((0 0, 10 0, 10 4, 0 4, 0 0))");
  MinimumDiameter md_wide(wide.get());
  EXPECT_DOUBLE_EQ(md_wide.getLength(), 4.0);
  EXPECT_DOUBLE_EQ(md_wide.getDiameter()->getLength(), 4.0);
  EXPECT_DOUBLE_EQ(md_wide.getSupportingSegment()->getLength(), 10.0);
  std::unique_ptr<Geometry> wide_rect = md_wide.getMinimumRectangle();
  ExpectRectangleMatchesDimensions(wide_rect.get(), wide.get(), 4.0, 10.0);

  std::unique_ptr<Geometry> tall =
      ReadWkt("POLYGON ((0 0, 3 0, 3 10, 0 10, 0 0))");
  MinimumDiameter md_tall(tall.get());
  EXPECT_DOUBLE_EQ(md_tall.getLength(), 3.0);
  EXPECT_DOUBLE_EQ(md_tall.getDiameter()->getLength(), 3.0);
  EXPECT_DOUBLE_EQ(md_tall.getSupportingSegment()->getLength(), 10.0);
  std::unique_ptr<Geometry> tall_rect = md_tall.getMinimumRectangle();
  ExpectRectangleMatchesDimensions(tall_rect.get(), tall.get(), 3.0, 10.0);
}

TEST_F(MinimumDiameterTest, RotatedRectanglesShallowSteepAnd45Deg) {
  // 3-4-5 rotated rectangle: long edge (8, 6) has |dx| > |dy| > 0, length 10,
  // width 5.
  std::unique_ptr<Geometry> shallow =
      ReadWkt("POLYGON ((0 0, 8 6, 5 10, -3 4, 0 0))");
  MinimumDiameter md_shallow(shallow.get());
  EXPECT_NEAR(md_shallow.getLength(), 5.0, 1e-12);
  EXPECT_NEAR(md_shallow.getDiameter()->getLength(), 5.0, 1e-12);
  EXPECT_NEAR(md_shallow.getSupportingSegment()->getLength(), 10.0, 1e-12);
  std::unique_ptr<Geometry> shallow_rect = md_shallow.getMinimumRectangle();
  ExpectRectangleMatchesDimensions(shallow_rect.get(), shallow.get(), 5.0,
                                   10.0);

  // Steep 3-4-5 rotated rectangle: long edge (6, 8) has |dy| > |dx| > 0, length
  // 10, width 5.
  std::unique_ptr<Geometry> steep =
      ReadWkt("POLYGON ((0 0, 6 8, 2 11, -4 3, 0 0))");
  MinimumDiameter md_steep(steep.get());
  EXPECT_NEAR(md_steep.getLength(), 5.0, 1e-12);
  EXPECT_NEAR(md_steep.getDiameter()->getLength(), 5.0, 1e-12);
  EXPECT_NEAR(md_steep.getSupportingSegment()->getLength(), 10.0, 1e-12);
  std::unique_ptr<Geometry> steep_rect = md_steep.getMinimumRectangle();
  ExpectRectangleMatchesDimensions(steep_rect.get(), steep.get(), 5.0, 10.0);

  // 45-degree diamond (|dx| == |dy| on every edge).
  std::unique_ptr<Geometry> diamond =
      ReadWkt("POLYGON ((0 0, 2 2, 0 4, -2 2, 0 0))");
  MinimumDiameter md_diamond(diamond.get());
  const double side = 2.0 * std::sqrt(2.0);
  EXPECT_NEAR(md_diamond.getLength(), side, 1e-12);
  std::unique_ptr<Geometry> diamond_rect = md_diamond.getMinimumRectangle();
  ExpectRectangleMatchesDimensions(diamond_rect.get(), diamond.get(), side,
                                   side);
}

TEST_F(MinimumDiameterTest, TrianglesRightAndObtuse) {
  // 3-4-5 right triangle: smallest altitude is 12/5 = 2.4 onto hypotenuse of
  // length 5.
  std::unique_ptr<Geometry> right_tri =
      ReadWkt("POLYGON ((0 0, 4 0, 0 3, 0 0))");
  MinimumDiameter md_right(right_tri.get());
  EXPECT_NEAR(md_right.getLength(), 2.4, 1e-12);
  const Coordinate& wc_right = md_right.getWidthCoordinate();
  EXPECT_DOUBLE_EQ(wc_right.x, 0.0);
  EXPECT_DOUBLE_EQ(wc_right.y, 0.0);
  EXPECT_NEAR(md_right.getSupportingSegment()->getLength(), 5.0, 1e-12);
  EXPECT_NEAR(md_right.getDiameter()->getLength(), 2.4, 1e-12);
  ExpectRectangleMatchesDimensions(md_right.getMinimumRectangle().get(),
                                   right_tri.get(), 2.4, 5.0);

  // Obtuse triangle: base (0,0)-(10,0), apex (5,1), min diameter = 1.0.
  std::unique_ptr<Geometry> obtuse_tri =
      ReadWkt("POLYGON ((0 0, 10 0, 5 1, 0 0))");
  MinimumDiameter md_obtuse(obtuse_tri.get());
  EXPECT_NEAR(md_obtuse.getLength(), 1.0, 1e-12);
  EXPECT_DOUBLE_EQ(md_obtuse.getWidthCoordinate().x, 5.0);
  EXPECT_DOUBLE_EQ(md_obtuse.getWidthCoordinate().y, 1.0);
  std::unique_ptr<LineString> obtuse_support = md_obtuse.getSupportingSegment();
  EXPECT_NEAR(obtuse_support->getLength(), 10.0, 1e-12);
  std::unique_ptr<LineString> obtuse_diam = md_obtuse.getDiameter();
  ASSERT_EQ(obtuse_diam->getNumPoints(), 2u);
  EXPECT_NEAR(obtuse_diam->getCoordinateN(0).x, 5.0, 1e-12);
  EXPECT_NEAR(obtuse_diam->getCoordinateN(0).y, 0.0, 1e-12);
  EXPECT_NEAR(obtuse_diam->getCoordinateN(1).x, 5.0, 1e-12);
  EXPECT_NEAR(obtuse_diam->getCoordinateN(1).y, 1.0, 1e-12);
  ExpectRectangleMatchesDimensions(md_obtuse.getMinimumRectangle().get(),
                                   obtuse_tri.get(), 1.0, 10.0);
}

TEST_F(MinimumDiameterTest,
       ChamferedOctagonRotatingCalipersSelectsDiagonalBand) {
  // Axis-aligned extent is 6x6, but diagonal band across chamfered edges
  // has width 4*sqrt(2) (~5.65685) < 6.
  std::unique_ptr<Geometry> octagon =
      ReadWkt("POLYGON ((2 0, 4 0, 6 2, 6 4, 4 6, 2 6, 0 4, 0 2, 2 0))");
  MinimumDiameter md(octagon.get());
  const double expected = 4.0 * std::sqrt(2.0);
  EXPECT_NEAR(md.getLength(), expected, 1e-12);
  ExpectRectangleMatchesDimensions(md.getMinimumRectangle().get(),
                                   octagon.get(), expected, expected);
}

TEST_F(MinimumDiameterTest, NonConvexPolygonUsesConvexHull) {
  // L-shape convex hull is the pentagon (0,0)-(10,0)-(10,2)-(2,10)-(0,10).
  // Supporting edge is (10,2)-(2,10) (line x + y = 12) and furthest vertex is
  // (0,0) at distance 12/sqrt(2) = 6*sqrt(2), with span 10*sqrt(2) along
  // (1,-1).
  std::unique_ptr<Geometry> l_shape =
      ReadWkt("POLYGON ((0 0, 10 0, 10 2, 2 2, 2 10, 0 10, 0 0))");
  const double l_expected_width = 6.0 * std::sqrt(2.0);
  const double l_expected_len = 10.0 * std::sqrt(2.0);

  MinimumDiameter md_default(l_shape.get());
  EXPECT_NEAR(md_default.getLength(), l_expected_width, 1e-12);
  EXPECT_DOUBLE_EQ(md_default.getWidthCoordinate().x, 0.0);
  EXPECT_DOUBLE_EQ(md_default.getWidthCoordinate().y, 0.0);
  EXPECT_NEAR(md_default.getSupportingSegment()->getLength(),
              8.0 * std::sqrt(2.0), 1e-12);
  EXPECT_NEAR(md_default.getDiameter()->getLength(), l_expected_width, 1e-12);
  ExpectRectangleMatchesDimensions(md_default.getMinimumRectangle().get(),
                                   l_shape.get(), l_expected_width,
                                   l_expected_len);

  // Explicit isConvex=false behaves identically.
  MinimumDiameter md_nonconvex(l_shape.get(), false);
  EXPECT_NEAR(md_nonconvex.getLength(), l_expected_width, 1e-12);
  ExpectRectangleMatchesDimensions(md_nonconvex.getMinimumRectangle().get(),
                                   l_shape.get(), l_expected_width,
                                   l_expected_len);

  // Concave dart whose convex hull is the right triangle (0,0), (10,0), (0,10).
  // Minimum diameter is the altitude to the hypotenuse: 5*sqrt(2).
  std::unique_ptr<Geometry> dart =
      ReadWkt("POLYGON ((0 0, 10 0, 2 2, 0 10, 0 0))");
  const double dart_expected_width = 5.0 * std::sqrt(2.0);
  const double dart_expected_len = 10.0 * std::sqrt(2.0);
  MinimumDiameter md_dart(dart.get());
  EXPECT_NEAR(md_dart.getLength(), dart_expected_width, 1e-12);
  EXPECT_DOUBLE_EQ(md_dart.getWidthCoordinate().x, 0.0);
  EXPECT_DOUBLE_EQ(md_dart.getWidthCoordinate().y, 0.0);
  EXPECT_NEAR(md_dart.getSupportingSegment()->getLength(), dart_expected_len,
              1e-12);
  ExpectRectangleMatchesDimensions(md_dart.getMinimumRectangle().get(),
                                   dart.get(), dart_expected_width,
                                   dart_expected_len);
}

TEST_F(MinimumDiameterTest,
       SkewedParallelogramRepeatedVertexAndNegativeCoords) {
  // Skewed parallelogram of area 4: base 4 has width 1, slanted side of
  // vector (6,1) (length sqrt(37)) has width 4/sqrt(37) < 1 and length
  // 61/sqrt(37). Diameter projects orthogonally onto the infinite supporting
  // line even outside the supporting edge endpoints.
  std::unique_ptr<Geometry> parallelogram =
      ReadWkt("POLYGON ((0 0, 4 0, 10 1, 6 1, 0 0))");
  const double expected_width = 4.0 / std::sqrt(37.0);
  const double expected_length = 61.0 / std::sqrt(37.0);

  MinimumDiameter md(parallelogram.get());
  EXPECT_NEAR(md.getLength(), expected_width, 1e-12);
  std::unique_ptr<LineString> support = md.getSupportingSegment();
  std::unique_ptr<LineString> diam = md.getDiameter();
  ASSERT_EQ(support->getNumPoints(), 2u);
  ASSERT_EQ(diam->getNumPoints(), 2u);
  EXPECT_NEAR(support->getLength(), std::sqrt(37.0), 1e-12);
  EXPECT_NEAR(diam->getLength(), expected_width, 1e-12);
  const double sdx =
      support->getCoordinateN(1).x - support->getCoordinateN(0).x;
  const double sdy =
      support->getCoordinateN(1).y - support->getCoordinateN(0).y;
  const double ddx = diam->getCoordinateN(1).x - diam->getCoordinateN(0).x;
  const double ddy = diam->getCoordinateN(1).y - diam->getCoordinateN(0).y;
  EXPECT_NEAR(sdx * ddx + sdy * ddy, 0.0, 1e-12);
  ExpectRectangleMatchesDimensions(md.getMinimumRectangle().get(),
                                   parallelogram.get(), expected_width,
                                   expected_length);

  MinimumDiameter md_convex(parallelogram.get(), true);
  EXPECT_NEAR(md_convex.getLength(), expected_width, 1e-12);
  ExpectRectangleMatchesDimensions(md_convex.getMinimumRectangle().get(),
                                   parallelogram.get(), expected_width,
                                   expected_length);

  // Convex polygon with a repeated vertex exercises zero-length segment
  // handling in distancePerpendicular during ring traversal.
  std::unique_ptr<Geometry> dup_vertex_poly =
      ReadWkt("POLYGON ((0 0, 0 0, 6 0, 6 2, 0 2, 0 0))");
  MinimumDiameter md_dup(dup_vertex_poly.get(), true);
  EXPECT_DOUBLE_EQ(md_dup.getLength(), 2.0);
  EXPECT_DOUBLE_EQ(md_dup.getSupportingSegment()->getLength(), 6.0);
  ExpectRectangleMatchesDimensions(md_dup.getMinimumRectangle().get(),
                                   dup_vertex_poly.get(), 2.0, 6.0);

  // Negative-coordinate rectangle exercises negative line offsets in computeC.
  std::unique_ptr<Geometry> neg_rect =
      ReadWkt("POLYGON ((-10 -20, -2 -20, -2 -17, -10 -17, -10 -20))");
  MinimumDiameter md_neg(neg_rect.get());
  EXPECT_DOUBLE_EQ(md_neg.getLength(), 3.0);
  ExpectRectangleMatchesDimensions(md_neg.getMinimumRectangle().get(),
                                   neg_rect.get(), 3.0, 8.0);
}

TEST_F(MinimumDiameterTest, PolygonWithHoleAndConvexLinearRing) {
  std::unique_ptr<Geometry> with_hole = ReadWkt(
      "POLYGON ((0 0, 10 0, 10 5, 0 5, 0 0), (2 1, 8 1, 8 4, 2 4, 2 1))");

  MinimumDiameter md_hull(with_hole.get());
  EXPECT_DOUBLE_EQ(md_hull.getLength(), 5.0);
  ExpectRectangleMatchesDimensions(md_hull.getMinimumRectangle().get(),
                                   with_hole.get(), 5.0, 10.0);

  // Exterior ring is convex, so isConvex=true extracts exterior ring directly.
  MinimumDiameter md_convex_poly(with_hole.get(), true);
  EXPECT_DOUBLE_EQ(md_convex_poly.getLength(), 5.0);
  ExpectRectangleMatchesDimensions(md_convex_poly.getMinimumRectangle().get(),
                                   with_hole.get(), 5.0, 10.0);

  // LinearRing is not a Polygon instance, testing the non-Polygon branch of
  // computeWidthConvex with >= 4 coordinates.
  auto ring_coords =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {8.0, 0.0}, {8.0, 2.0}, {0.0, 2.0}, {0.0, 0.0}});
  std::unique_ptr<LinearRing> ring =
      factory_->createLinearRing(std::move(ring_coords));
  MinimumDiameter md_ring(ring.get(), true);
  EXPECT_DOUBLE_EQ(md_ring.getLength(), 2.0);
  ExpectRectangleMatchesDimensions(md_ring.getMinimumRectangle().get(),
                                   ring.get(), 2.0, 8.0);
}

TEST_F(MinimumDiameterTest, MultiGeometriesAndCollections) {
  std::unique_ptr<Geometry> mp_collinear =
      ReadWkt("MULTIPOINT ((0 0), (3 4), (6 8))");
  MinimumDiameter md_mp(mp_collinear.get());
  EXPECT_DOUBLE_EQ(md_mp.getLength(), 0.0);
  std::unique_ptr<Geometry> mp_rect = md_mp.getMinimumRectangle();
  EXPECT_EQ(mp_rect->getGeometryTypeId(), GeometryTypeId::GEOS_LINESTRING);
  EXPECT_TRUE(mp_rect->equals(ReadWkt("LINESTRING (0 0, 6 8)").get()));

  std::unique_ptr<Geometry> mls =
      ReadWkt("MULTILINESTRING ((0 0, 10 0), (0 4, 10 4))");
  MinimumDiameter md_mls(mls.get());
  EXPECT_DOUBLE_EQ(md_mls.getLength(), 4.0);
  ExpectRectangleMatchesDimensions(md_mls.getMinimumRectangle().get(),
                                   mls.get(), 4.0, 10.0);

  std::unique_ptr<Geometry> mpoly = ReadWkt(
      "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "((8 0, 10 0, 10 2, 8 2, 8 0)))");
  MinimumDiameter md_mpoly(mpoly.get());
  EXPECT_DOUBLE_EQ(md_mpoly.getLength(), 2.0);
  ExpectRectangleMatchesDimensions(md_mpoly.getMinimumRectangle().get(),
                                   mpoly.get(), 2.0, 10.0);

  std::unique_ptr<Geometry> gc = ReadWkt(
      "GEOMETRYCOLLECTION (POINT (0 0), LINESTRING (0 6, 8 6), "
      "POLYGON ((4 0, 8 0, 8 3, 4 3, 4 0)))");
  MinimumDiameter md_gc(gc.get());
  EXPECT_DOUBLE_EQ(md_gc.getLength(), 6.0);
  ExpectRectangleMatchesDimensions(md_gc.getMinimumRectangle().get(), gc.get(),
                                   6.0, 8.0);
}

TEST_F(MinimumDiameterTest, CachedResultsAndIndependentAccessorEntrypoints) {
  std::unique_ptr<Geometry> poly =
      ReadWkt("POLYGON ((0 0, 12 0, 12 5, 0 5, 0 0))");

  // Single instance queried repeatedly hits cached minWidthPt branch.
  MinimumDiameter md_cached(poly.get());
  EXPECT_DOUBLE_EQ(md_cached.getLength(), 5.0);
  EXPECT_FALSE(md_cached.getWidthCoordinate().isNull());
  EXPECT_DOUBLE_EQ(md_cached.getSupportingSegment()->getLength(), 12.0);
  EXPECT_DOUBLE_EQ(md_cached.getDiameter()->getLength(), 5.0);
  ExpectRectangleMatchesDimensions(md_cached.getMinimumRectangle().get(),
                                   poly.get(), 5.0, 12.0);

  // Separate instances calling each accessor first.
  MinimumDiameter md1(poly.get());
  EXPECT_FALSE(md1.getWidthCoordinate().isNull());

  MinimumDiameter md2(poly.get());
  EXPECT_DOUBLE_EQ(md2.getSupportingSegment()->getLength(), 12.0);

  MinimumDiameter md3(poly.get());
  EXPECT_DOUBLE_EQ(md3.getDiameter()->getLength(), 5.0);

  MinimumDiameter md4(poly.get());
  ExpectRectangleMatchesDimensions(md4.getMinimumRectangle().get(), poly.get(),
                                   5.0, 12.0);
}

TEST_F(MinimumDiameterTest, StaticConvenienceMethodsAndSridPreservation) {
  PrecisionModel pm;
  GeometryFactory::Ptr srid_factory = GeometryFactory::create(&pm, 4326);
  WKTReader srid_reader(srid_factory.get());

  std::unique_ptr<Geometry> poly =
      srid_reader.read("POLYGON ((1 1, 7 1, 7 4, 1 4, 1 1))");

  std::unique_ptr<Geometry> diam =
      MinimumDiameter::getMinimumDiameter(poly.get());
  ASSERT_NE(diam, nullptr);
  EXPECT_EQ(diam->getGeometryTypeId(), GeometryTypeId::GEOS_LINESTRING);
  EXPECT_DOUBLE_EQ(diam->getLength(), 3.0);
  EXPECT_EQ(diam->getSRID(), 4326);

  std::unique_ptr<Geometry> rect =
      MinimumDiameter::getMinimumRectangle(poly.get());
  ASSERT_NE(rect, nullptr);
  ExpectRectangleMatchesDimensions(rect.get(), poly.get(), 3.0, 6.0);
  EXPECT_EQ(rect->getSRID(), 4326);

  std::unique_ptr<Geometry> empty = ReadWkt("POLYGON EMPTY");
  EXPECT_TRUE(MinimumDiameter::getMinimumDiameter(empty.get())->isEmpty());
  EXPECT_TRUE(MinimumDiameter::getMinimumRectangle(empty.get())->isEmpty());
}

TEST_F(MinimumDiameterTest, Handles3DInputPolygon) {
  std::unique_ptr<Geometry> poly_z =
      ReadWkt("POLYGON Z ((0 0 10, 6 0 20, 6 2 30, 0 2 40, 0 0 10))");
  MinimumDiameter md(poly_z.get());
  EXPECT_DOUBLE_EQ(md.getLength(), 2.0);
  ExpectRectangleMatchesDimensions(md.getMinimumRectangle().get(), poly_z.get(),
                                   2.0, 6.0);
}

}  // namespace
}  // namespace geos_google
