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

#include "third_party/geos/include/geos/algorithm/construct/MaximumInscribedCircle.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <numbers>
#include <string>
#include <utility>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/GEOSException.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/Interrupt.h"

namespace geos_google {
namespace {

using geos::algorithm::construct::MaximumInscribedCircle;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::util::CurrentThreadInterrupt;
using geos::util::GEOSException;
using geos::util::IllegalArgumentException;

int AlwaysInterruptCallback(void* /*data*/) { return 1; }

class MaximumInscribedCircleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    factory_ = GeometryFactory::create();
    reader_ = std::make_unique<WKTReader>(*factory_);
  }

  std::unique_ptr<Geometry> Read(const std::string& wkt) const {
    return reader_->read(wkt);
  }

  std::unique_ptr<Polygon> MakeRegularPolygon(double cx, double cy,
                                              double radius,
                                              std::size_t sides) const {
    CoordinateSequence seq = CoordinateSequence::XY(sides + 1);
    for (std::size_t i = 0; i < sides; ++i) {
      double angle = (2.0 * std::numbers::pi * static_cast<double>(i)) /
                     static_cast<double>(sides);
      seq.setAt(CoordinateXY(cx + radius * std::cos(angle),
                             cy + radius * std::sin(angle)),
                i);
    }
    seq.setAt(seq.getAt<CoordinateXY>(0), sides);
    return factory_->createPolygon(std::move(seq));
  }

  void VerifyInscribedCircle(const Geometry* geom, const Point* center,
                             const Point* radius_pt,
                             const LineString* radius_line,
                             double dist_tol = 1e-9) const {
    ASSERT_NE(center, nullptr);
    ASSERT_NE(radius_pt, nullptr);
    ASSERT_NE(radius_line, nullptr);
    EXPECT_FALSE(center->isEmpty());
    EXPECT_FALSE(radius_pt->isEmpty());
    EXPECT_EQ(radius_line->getNumPoints(), 2u);

    EXPECT_DOUBLE_EQ(radius_line->getCoordinateN(0).x, center->getX());
    EXPECT_DOUBLE_EQ(radius_line->getCoordinateN(0).y, center->getY());
    EXPECT_DOUBLE_EQ(radius_line->getCoordinateN(1).x, radius_pt->getX());
    EXPECT_DOUBLE_EQ(radius_line->getCoordinateN(1).y, radius_pt->getY());

    EXPECT_TRUE(geom->covers(center));

    auto boundary = geom->getBoundary();
    EXPECT_NEAR(boundary->distance(radius_pt), 0.0, dist_tol);
    double radius = center->distance(radius_pt);
    EXPECT_NEAR(radius_line->getLength(), radius, dist_tol);
    EXPECT_NEAR(boundary->distance(center), radius, dist_tol);
  }

  GeometryFactory::Ptr factory_;
  std::unique_ptr<WKTReader> reader_;
};

TEST_F(MaximumInscribedCircleTest, RejectsNonPolygonalInputs) {
  auto pt = Read("POINT (1 2)");
  auto line = Read("LINESTRING (0 0, 10 10)");
  auto ring = factory_->createLinearRing(CoordinateSequence{
      CoordinateXY{0.0, 0.0},
      CoordinateXY{10.0, 0.0},
      CoordinateXY{10.0, 10.0},
      CoordinateXY{0.0, 0.0},
  });
  auto mpt = Read("MULTIPOINT ((0 0), (5 5))");
  auto mline = Read("MULTILINESTRING ((0 0, 1 1), (2 2, 3 3))");
  auto gc = Read("GEOMETRYCOLLECTION (POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)))");

  for (const Geometry* g : {static_cast<const Geometry*>(pt.get()),
                            static_cast<const Geometry*>(line.get()),
                            static_cast<const Geometry*>(ring.get()),
                            static_cast<const Geometry*>(mpt.get()),
                            static_cast<const Geometry*>(mline.get()),
                            static_cast<const Geometry*>(gc.get())}) {
    EXPECT_THROW(MaximumInscribedCircle(g, 0.1), IllegalArgumentException);
    EXPECT_THROW(MaximumInscribedCircle::getCenter(g, 0.1),
                 IllegalArgumentException);
    EXPECT_THROW(MaximumInscribedCircle::getRadiusLine(g, 0.1),
                 IllegalArgumentException);
    EXPECT_THROW(MaximumInscribedCircle::isRadiusWithin(g, 1.0),
                 IllegalArgumentException);
  }
}

TEST_F(MaximumInscribedCircleTest, RejectsEmptyPolygonalInputs) {
  auto empty_poly = Read("POLYGON EMPTY");
  auto empty_mpoly = Read("MULTIPOLYGON EMPTY");

  EXPECT_THROW(MaximumInscribedCircle(empty_poly.get(), 0.1),
               IllegalArgumentException);
  EXPECT_THROW(MaximumInscribedCircle(empty_mpoly.get(), 0.1),
               IllegalArgumentException);
  EXPECT_THROW(MaximumInscribedCircle::getCenter(empty_poly.get(), 0.1),
               IllegalArgumentException);
  EXPECT_THROW(MaximumInscribedCircle::getRadiusLine(empty_mpoly.get(), 0.1),
               IllegalArgumentException);
}

TEST_F(MaximumInscribedCircleTest,
       RejectsNonPositiveToleranceOnApproximatedPolygon) {
  auto pentagon = Read("POLYGON ((0 0, 10 0, 12 6, 5 10, -2 6, 0 0))");

  MaximumInscribedCircle mic_zero(pentagon.get(), 0.0);
  EXPECT_THROW(mic_zero.getCenter(), IllegalArgumentException);
  EXPECT_THROW(mic_zero.getRadiusPoint(), IllegalArgumentException);
  EXPECT_THROW(mic_zero.getRadiusLine(), IllegalArgumentException);

  MaximumInscribedCircle mic_neg(pentagon.get(), -0.5);
  EXPECT_THROW(mic_neg.getCenter(), IllegalArgumentException);

  EXPECT_THROW(MaximumInscribedCircle::getCenter(pentagon.get(), 0.0),
               IllegalArgumentException);
  EXPECT_THROW(MaximumInscribedCircle::getRadiusLine(pentagon.get(), -1.0),
               IllegalArgumentException);
}

TEST_F(MaximumInscribedCircleTest, ExactSquareAndRectangle) {
  auto square = Read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  MaximumInscribedCircle square_mic(square.get(), 0.01);
  auto sq_center = square_mic.getCenter();
  auto sq_radius_pt = square_mic.getRadiusPoint();
  auto sq_line = square_mic.getRadiusLine();

  VerifyInscribedCircle(square.get(), sq_center.get(), sq_radius_pt.get(),
                        sq_line.get());
  EXPECT_NEAR(sq_center->getX(), 5.0, 1e-12);
  EXPECT_NEAR(sq_center->getY(), 5.0, 1e-12);
  EXPECT_NEAR(sq_line->getLength(), 5.0, 1e-12);

  auto static_center = MaximumInscribedCircle::getCenter(square.get(), 0.01);
  auto static_line = MaximumInscribedCircle::getRadiusLine(square.get(), 0.01);
  EXPECT_TRUE(static_center->equalsExact(sq_center.get(), 1e-12));
  EXPECT_TRUE(static_line->equalsExact(sq_line.get(), 1e-12));

  auto rect = Read("POLYGON ((0 0, 20 0, 20 6, 0 6, 0 0))");
  MaximumInscribedCircle rect_mic(rect.get(), 0.01);
  auto rect_center = rect_mic.getCenter();
  auto rect_radius_pt = rect_mic.getRadiusPoint();
  auto rect_line = rect_mic.getRadiusLine();

  VerifyInscribedCircle(rect.get(), rect_center.get(), rect_radius_pt.get(),
                        rect_line.get());
  EXPECT_NEAR(rect_center->getY(), 3.0, 1e-12);
  EXPECT_GE(rect_center->getX(), 3.0 - 1e-12);
  EXPECT_LE(rect_center->getX(), 17.0 + 1e-12);
  EXPECT_NEAR(rect_line->getLength(), 3.0, 1e-12);
}

TEST_F(MaximumInscribedCircleTest, ExactRightTriangleAndRotatedDiamond) {
  auto triangle = Read("POLYGON ((0 0, 3 0, 0 4, 0 0))");
  MaximumInscribedCircle tri_mic(triangle.get(), 0.01);
  auto tri_center = tri_mic.getCenter();
  auto tri_radius_pt = tri_mic.getRadiusPoint();
  auto tri_line = tri_mic.getRadiusLine();

  VerifyInscribedCircle(triangle.get(), tri_center.get(), tri_radius_pt.get(),
                        tri_line.get());
  EXPECT_NEAR(tri_center->getX(), 1.0, 1e-12);
  EXPECT_NEAR(tri_center->getY(), 1.0, 1e-12);
  EXPECT_NEAR(tri_line->getLength(), 1.0, 1e-12);

  auto diamond = Read("POLYGON ((5 0, 10 5, 5 10, 0 5, 5 0))");
  MaximumInscribedCircle dia_mic(diamond.get(), 0.01);
  auto dia_center = dia_mic.getCenter();
  auto dia_radius_pt = dia_mic.getRadiusPoint();
  auto dia_line = dia_mic.getRadiusLine();

  VerifyInscribedCircle(diamond.get(), dia_center.get(), dia_radius_pt.get(),
                        dia_line.get());
  EXPECT_NEAR(dia_center->getX(), 5.0, 1e-12);
  EXPECT_NEAR(dia_center->getY(), 5.0, 1e-12);
  EXPECT_NEAR(dia_line->getLength(), 5.0 / std::numbers::sqrt2, 1e-12);
}

TEST_F(MaximumInscribedCircleTest, RegularPolygonApproximationAndCachedReuse) {
  constexpr double kCx = 10.0;
  constexpr double kCy = 20.0;
  constexpr double kCircumradius = 10.0;
  constexpr std::size_t kSides = 16;
  constexpr double kTol = 0.001;
  const double expected_inradius =
      kCircumradius * std::cos(std::numbers::pi / kSides);

  auto poly = MakeRegularPolygon(kCx, kCy, kCircumradius, kSides);
  MaximumInscribedCircle mic(poly.get(), kTol);

  auto center1 = mic.getCenter();
  auto radius_pt = mic.getRadiusPoint();
  auto radius_line = mic.getRadiusLine();
  // Calling getters again hits the cached `done` branch in compute().
  auto center2 = mic.getCenter();

  VerifyInscribedCircle(poly.get(), center1.get(), radius_pt.get(),
                        radius_line.get());
  EXPECT_TRUE(center2->equalsExact(center1.get()));
  EXPECT_NEAR(center1->getX(), kCx, 0.02);
  EXPECT_NEAR(center1->getY(), kCy, 0.02);
  EXPECT_GE(radius_line->getLength(), expected_inradius - kTol);
  EXPECT_LE(radius_line->getLength(), expected_inradius + 1e-9);

  auto static_center = MaximumInscribedCircle::getCenter(poly.get(), kTol);
  auto static_line = MaximumInscribedCircle::getRadiusLine(poly.get(), kTol);
  EXPECT_TRUE(static_center->equalsExact(center1.get()));
  EXPECT_TRUE(static_line->equalsExact(radius_line.get()));
}

TEST_F(MaximumInscribedCircleTest, NonConvexQuadrilateralUsesApproximation) {
  auto chevron = Read("POLYGON ((0 0, 10 0, 3 3, 0 10, 0 0))");
  constexpr double kTol = 0.001;
  const double expected_radius = 3.0 * (2.0 - std::numbers::sqrt2);

  MaximumInscribedCircle mic(chevron.get(), kTol);
  auto center = mic.getCenter();
  auto radius_pt = mic.getRadiusPoint();
  auto radius_line = mic.getRadiusLine();

  VerifyInscribedCircle(chevron.get(), center.get(), radius_pt.get(),
                        radius_line.get());
  EXPECT_NEAR(center->getX(), expected_radius, 0.01);
  EXPECT_NEAR(center->getY(), expected_radius, 0.01);
  EXPECT_GE(radius_line->getLength(), expected_radius - kTol);
  EXPECT_LE(radius_line->getLength(), expected_radius + 1e-9);
}

TEST_F(MaximumInscribedCircleTest,
       ConcaveChimneyPolygonRefinesBeyondInteriorPoint) {
  // getInteriorPoint() lands in the width-2 chimney around y=20 (radius 1.0),
  // so branch-and-bound must update farthestCell to reach the 10x10 base.
  auto chimney = Read("POLYGON ((0 0, 10 0, 10 10, 2 10, 2 30, 0 30, 0 0))");
  constexpr double kTol = 0.001;

  MaximumInscribedCircle mic(chimney.get(), kTol);
  auto center = mic.getCenter();
  auto radius_pt = mic.getRadiusPoint();
  auto radius_line = mic.getRadiusLine();

  VerifyInscribedCircle(chimney.get(), center.get(), radius_pt.get(),
                        radius_line.get());
  EXPECT_NEAR(center->getX(), 5.0, 0.01);
  EXPECT_NEAR(center->getY(), 5.0, 0.01);
  EXPECT_GE(radius_line->getLength(), 5.0 - kTol);
  EXPECT_LE(radius_line->getLength(), 5.0 + 1e-9);
}

TEST_F(MaximumInscribedCircleTest, LShapedPolygonCoarseVsFineTolerance) {
  auto l_shape = Read("POLYGON ((0 0, 10 0, 10 2, 2 2, 2 10, 0 10, 0 0))");
  const double expected_radius = 4.0 - 2.0 * std::numbers::sqrt2;

  MaximumInscribedCircle coarse_mic(l_shape.get(), 0.5);
  auto coarse_center = coarse_mic.getCenter();
  auto coarse_radius_pt = coarse_mic.getRadiusPoint();
  auto coarse_line = coarse_mic.getRadiusLine();
  VerifyInscribedCircle(l_shape.get(), coarse_center.get(),
                        coarse_radius_pt.get(), coarse_line.get());
  EXPECT_GE(coarse_line->getLength(), expected_radius - 0.5);
  EXPECT_LE(coarse_line->getLength(), expected_radius + 1e-9);

  constexpr double kFineTol = 0.0005;
  MaximumInscribedCircle fine_mic(l_shape.get(), kFineTol);
  auto fine_center = fine_mic.getCenter();
  auto fine_radius_pt = fine_mic.getRadiusPoint();
  auto fine_line = fine_mic.getRadiusLine();
  VerifyInscribedCircle(l_shape.get(), fine_center.get(), fine_radius_pt.get(),
                        fine_line.get());
  EXPECT_NEAR(fine_center->getX(), expected_radius, 0.01);
  EXPECT_NEAR(fine_center->getY(), expected_radius, 0.01);
  EXPECT_GE(fine_line->getLength(), expected_radius - kFineTol);
  EXPECT_LE(fine_line->getLength(), expected_radius + 1e-9);
}

TEST_F(MaximumInscribedCircleTest, PolygonWithHoles) {
  auto frame = Read(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(4 4, 16 4, 16 16, 4 16, 4 4))");
  constexpr double kTol = 0.001;
  // In a square frame of side width 4, the corner regions admit a larger
  // inscribed circle touching two outer edges and the inner hole corner:
  // r = 4 * (2 - sqrt(2)) ~= 2.34314575 > 2.0.
  const double expected_frame_radius = 4.0 * (2.0 - std::numbers::sqrt2);

  MaximumInscribedCircle frame_mic(frame.get(), kTol);
  auto frame_center = frame_mic.getCenter();
  auto frame_radius_pt = frame_mic.getRadiusPoint();
  auto frame_line = frame_mic.getRadiusLine();
  VerifyInscribedCircle(frame.get(), frame_center.get(), frame_radius_pt.get(),
                        frame_line.get());
  EXPECT_NEAR(std::min(frame_center->getX(), 20.0 - frame_center->getX()),
              expected_frame_radius, 0.01);
  EXPECT_NEAR(std::min(frame_center->getY(), 20.0 - frame_center->getY()),
              expected_frame_radius, 0.01);
  EXPECT_GE(frame_line->getLength(), expected_frame_radius - kTol);
  EXPECT_LE(frame_line->getLength(), expected_frame_radius + 1e-9);

  auto asymmetric = Read(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(2 2, 6 2, 6 6, 2 6, 2 2))");
  const double expected_center = 14.0 * std::numbers::sqrt2 - 8.0;
  const double expected_radius = 20.0 - expected_center;

  MaximumInscribedCircle asym_mic(asymmetric.get(), kTol);
  auto asym_center = asym_mic.getCenter();
  auto asym_radius_pt = asym_mic.getRadiusPoint();
  auto asym_line = asym_mic.getRadiusLine();
  VerifyInscribedCircle(asymmetric.get(), asym_center.get(),
                        asym_radius_pt.get(), asym_line.get());
  EXPECT_NEAR(asym_center->getX(), expected_center, 0.01);
  EXPECT_NEAR(asym_center->getY(), expected_center, 0.01);
  EXPECT_GE(asym_line->getLength(), expected_radius - kTol);
  EXPECT_LE(asym_line->getLength(), expected_radius + 1e-9);

  auto multi_hole = Read(
      "POLYGON ((0 0, 30 0, 30 10, 0 10, 0 0), "
      "(2 2, 8 2, 8 8, 2 8, 2 2), "
      "(12 2, 18 2, 18 8, 12 8, 12 2))");
  MaximumInscribedCircle mh_mic(multi_hole.get(), kTol);
  auto mh_center = mh_mic.getCenter();
  auto mh_radius_pt = mh_mic.getRadiusPoint();
  auto mh_line = mh_mic.getRadiusLine();
  VerifyInscribedCircle(multi_hole.get(), mh_center.get(), mh_radius_pt.get(),
                        mh_line.get());
  EXPECT_GE(mh_center->getX(), 23.0 - 0.01);
  EXPECT_LE(mh_center->getX(), 25.0 + 0.01);
  EXPECT_NEAR(mh_center->getY(), 5.0, 0.01);
  EXPECT_GE(mh_line->getLength(), 5.0 - kTol);
  EXPECT_LE(mh_line->getLength(), 5.0 + 1e-9);
}

TEST_F(MaximumInscribedCircleTest, MultiPolygonDisjointComponents) {
  auto mpoly = Read(
      "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "((20 0, 30 0, 30 10, 20 10, 20 0)))");
  constexpr double kTol = 0.001;

  MaximumInscribedCircle mic(mpoly.get(), kTol);
  auto center = mic.getCenter();
  auto radius_pt = mic.getRadiusPoint();
  auto radius_line = mic.getRadiusLine();

  VerifyInscribedCircle(mpoly.get(), center.get(), radius_pt.get(),
                        radius_line.get());
  EXPECT_NEAR(center->getX(), 25.0, 0.01);
  EXPECT_NEAR(center->getY(), 5.0, 0.01);
  EXPECT_GE(radius_line->getLength(), 5.0 - kTol);
  EXPECT_LE(radius_line->getLength(), 5.0 + 1e-9);
}

TEST_F(MaximumInscribedCircleTest, FlatAndCollapsedZeroAreaGeometries) {
  auto collinear = Read("POLYGON ((1 2, 5 2, 3 2, 1 2))");
  MaximumInscribedCircle collinear_mic(collinear.get(), 0.0);
  auto col_center = collinear_mic.getCenter();
  auto col_radius_pt = collinear_mic.getRadiusPoint();
  auto col_line = collinear_mic.getRadiusLine();
  EXPECT_DOUBLE_EQ(col_center->getX(), 1.0);
  EXPECT_DOUBLE_EQ(col_center->getY(), 2.0);
  EXPECT_DOUBLE_EQ(col_radius_pt->getX(), 1.0);
  EXPECT_DOUBLE_EQ(col_radius_pt->getY(), 2.0);
  EXPECT_DOUBLE_EQ(col_line->getLength(), 0.0);

  auto collapsed = Read("POLYGON ((4 7, 4 7, 4 7, 4 7))");
  auto collapsed_center =
      MaximumInscribedCircle::getCenter(collapsed.get(), 0.1);
  auto collapsed_line =
      MaximumInscribedCircle::getRadiusLine(collapsed.get(), 0.1);
  EXPECT_DOUBLE_EQ(collapsed_center->getX(), 4.0);
  EXPECT_DOUBLE_EQ(collapsed_center->getY(), 7.0);
  EXPECT_DOUBLE_EQ(collapsed_line->getLength(), 0.0);

  auto flat_mpoly = Read(
      "MULTIPOLYGON (((2 3, 6 3, 4 3, 2 3)), "
      "((8 9, 10 9, 9 9, 8 9)))");
  MaximumInscribedCircle flat_mp_mic(flat_mpoly.get(), 0.1);
  auto mp_center = flat_mp_mic.getCenter();
  auto mp_radius_pt = flat_mp_mic.getRadiusPoint();
  EXPECT_DOUBLE_EQ(mp_center->getX(), 2.0);
  EXPECT_DOUBLE_EQ(mp_center->getY(), 3.0);
  EXPECT_DOUBLE_EQ(mp_radius_pt->getX(), 2.0);
  EXPECT_DOUBLE_EQ(mp_radius_pt->getY(), 3.0);
}

TEST_F(MaximumInscribedCircleTest,
       IsRadiusWithinValidationAndEnvelopeShortcuts) {
  auto square = Read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  EXPECT_THROW(MaximumInscribedCircle::isRadiusWithin(square.get(), -0.01),
               IllegalArgumentException);
  MaximumInscribedCircle sq_neg_mic(square.get(), -1.0);
  EXPECT_THROW(sq_neg_mic.isRadiusWithin(-1.0), IllegalArgumentException);

  EXPECT_FALSE(MaximumInscribedCircle::isRadiusWithin(square.get(), 0.0));
  auto flat = Read("POLYGON ((1 2, 5 2, 3 2, 1 2))");
  EXPECT_FALSE(MaximumInscribedCircle::isRadiusWithin(flat.get(), 0.0));
  EXPECT_TRUE(MaximumInscribedCircle::isRadiusWithin(flat.get(), 0.5));

  // Exact square radius is 5.0; test boundary without envelope shortcut.
  EXPECT_TRUE(MaximumInscribedCircle::isRadiusWithin(square.get(), 5.0));
  EXPECT_FALSE(MaximumInscribedCircle::isRadiusWithin(square.get(), 4.99));
  EXPECT_TRUE(MaximumInscribedCircle::isRadiusWithin(square.get(), 5.01));

  // Width < 2 * maxRadius shortcut.
  auto tall_pentagon = Read("POLYGON ((0 0, 2 0, 2 20, 1 21, 0 20, 0 0))");
  EXPECT_TRUE(MaximumInscribedCircle::isRadiusWithin(tall_pentagon.get(), 1.5));

  // Height < 2 * maxRadius shortcut.
  auto wide_pentagon = Read("POLYGON ((0 0, 20 0, 21 1, 20 2, 0 2, 0 0))");
  EXPECT_TRUE(MaximumInscribedCircle::isRadiusWithin(wide_pentagon.get(), 1.5));
}

TEST_F(MaximumInscribedCircleTest,
       IsRadiusWithinApproximationShortCircuitBranches) {
  // L-shape has envelope 10x10 and exact MIC radius ~1.17157.
  auto l_shape = Read("POLYGON ((0 0, 10 0, 10 2, 2 2, 2 10, 0 10, 0 0))");
  EXPECT_TRUE(MaximumInscribedCircle::isRadiusWithin(l_shape.get(), 2.0));
  EXPECT_FALSE(MaximumInscribedCircle::isRadiusWithin(l_shape.get(), 1.0));

  // Chimney starts with interior point distance 1.0 < 2.5, then refines into
  // the 10x10 base where distance > 2.5 and provides a witness location.
  auto chimney = Read("POLYGON ((0 0, 10 0, 10 10, 2 10, 2 30, 0 30, 0 0))");
  MaximumInscribedCircle chimney_mic(chimney.get(), -1.0);
  EXPECT_FALSE(chimney_mic.isRadiusWithin(2.5));
  auto witness_center = chimney_mic.getCenter();
  auto witness_radius_pt = chimney_mic.getRadiusPoint();
  auto witness_line = chimney_mic.getRadiusLine();
  VerifyInscribedCircle(chimney.get(), witness_center.get(),
                        witness_radius_pt.get(), witness_line.get());
  EXPECT_GT(witness_line->getLength(), 2.5);

  // Regular 16-gon of radius 10 (inradius ~9.80785).
  auto poly16 = MakeRegularPolygon(0.0, 0.0, 10.0, 16);
  EXPECT_FALSE(MaximumInscribedCircle::isRadiusWithin(poly16.get(), 3.0));
  EXPECT_TRUE(MaximumInscribedCircle::isRadiusWithin(poly16.get(), 9.9));

  // Polygon with hole and MultiPolygon without envelope shortcut.
  auto frame = Read(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(4 4, 16 4, 16 16, 4 16, 4 4))");
  EXPECT_TRUE(MaximumInscribedCircle::isRadiusWithin(frame.get(), 2.5));
  EXPECT_FALSE(MaximumInscribedCircle::isRadiusWithin(frame.get(), 2.2));
  EXPECT_FALSE(MaximumInscribedCircle::isRadiusWithin(frame.get(), 1.5));

  auto mpoly = Read(
      "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "((20 0, 30 0, 30 10, 20 10, 20 0)))");
  EXPECT_TRUE(MaximumInscribedCircle::isRadiusWithin(mpoly.get(), 5.05));
  EXPECT_FALSE(MaximumInscribedCircle::isRadiusWithin(mpoly.get(), 4.9));
}

TEST_F(MaximumInscribedCircleTest, LargeToleranceDrainsQueueWithoutSplitting) {
  auto pentagon = MakeRegularPolygon(0.0, 0.0, 10.0, 5);
  MaximumInscribedCircle mic(pentagon.get(), 100.0);
  auto center = mic.getCenter();
  auto radius_pt = mic.getRadiusPoint();
  auto radius_line = mic.getRadiusLine();

  VerifyInscribedCircle(pentagon.get(), center.get(), radius_pt.get(),
                        radius_line.get());
  EXPECT_GT(radius_line->getLength(), 0.0);
}

TEST_F(MaximumInscribedCircleTest, ComputeMaximumIterationsAndIterationCap) {
  auto square = Read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  EXPECT_EQ(
      MaximumInscribedCircle::computeMaximumIterations(square.get(), 100.0),
      4000u);

  const double diam = square->getEnvelopeInternal()->getDiameter();
  const int expected_factor = static_cast<int>(std::log(diam / 0.01));
  EXPECT_EQ(
      MaximumInscribedCircle::computeMaximumIterations(square.get(), 0.01),
      static_cast<std::size_t>(2000 + 2000 * expected_factor));

  // Very thin 5-vertex strip triggers the iteration limit before full
  // convergence across its length.
  auto thin_strip = factory_->createPolygon(CoordinateSequence{
      CoordinateXY{0.0, 0.0},
      CoordinateXY{100.0, 0.0},
      CoordinateXY{100.0, 1e-6},
      CoordinateXY{50.0, 1e-6},
      CoordinateXY{0.0, 1e-6},
      CoordinateXY{0.0, 0.0},
  });
  MaximumInscribedCircle mic(thin_strip.get(), 0.002);
  auto center = mic.getCenter();
  auto radius_pt = mic.getRadiusPoint();
  auto radius_line = mic.getRadiusLine();

  VerifyInscribedCircle(thin_strip.get(), center.get(), radius_pt.get(),
                        radius_line.get(), 1e-12);
  EXPECT_LE(radius_line->getLength(), 1e-6);
}

TEST_F(MaximumInscribedCircleTest, NonFiniteEnvelopeThrowsGEOSException) {
  auto huge_pentagon = factory_->createPolygon(CoordinateSequence{
      CoordinateXY{0.0, 0.0},
      CoordinateXY{1e160, 0.0},
      CoordinateXY{1e160, 1e160},
      CoordinateXY{5e159, 1e160},
      CoordinateXY{0.0, 1e160},
      CoordinateXY{0.0, 0.0},
  });

  EXPECT_THROW(MaximumInscribedCircle::getCenter(huge_pentagon.get(), 1.0),
               GEOSException);
}

TEST_F(MaximumInscribedCircleTest, RespectsInterruptRequest) {
  auto pentagon = MakeRegularPolygon(0.0, 0.0, 10.0, 5);
  auto* prev_cb = CurrentThreadInterrupt::registerCallback(
      &AlwaysInterruptCallback, nullptr);
  absl::Cleanup restore_cb([prev_cb]() {
    CurrentThreadInterrupt::registerCallback(prev_cb, nullptr);
  });

  EXPECT_THROW(MaximumInscribedCircle::getCenter(pentagon.get(), 0.01),
               GEOSException);
}

TEST_F(MaximumInscribedCircleTest,
       PreservesFactorySRIDAndProduces2DCoordinates) {
  PrecisionModel pm;
  GeometryFactory::Ptr srid_factory = GeometryFactory::create(&pm, 4326);
  WKTReader srid_reader(*srid_factory);
  auto poly_z = srid_reader.read(
      "POLYGON Z ((0 0 100, 10 0 100, 10 10 100, 5 12 100, 0 10 100, 0 0 "
      "100))");

  MaximumInscribedCircle mic(poly_z.get(), 0.01);
  auto center = mic.getCenter();
  auto radius_pt = mic.getRadiusPoint();
  auto radius_line = mic.getRadiusLine();

  EXPECT_EQ(center->getSRID(), 4326);
  EXPECT_EQ(radius_pt->getSRID(), 4326);
  EXPECT_EQ(radius_line->getSRID(), 4326);
  EXPECT_TRUE(std::isnan(center->getZ()));
  EXPECT_TRUE(std::isnan(radius_pt->getZ()));
  EXPECT_TRUE(std::isnan(radius_line->getCoordinateN(0).z));
  EXPECT_TRUE(std::isnan(radius_line->getCoordinateN(1).z));
  VerifyInscribedCircle(poly_z.get(), center.get(), radius_pt.get(),
                        radius_line.get());
}

}  // namespace
}  // namespace geos_google
