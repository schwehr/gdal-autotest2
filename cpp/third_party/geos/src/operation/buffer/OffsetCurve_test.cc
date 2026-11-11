// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "third_party/geos/include/geos/operation/buffer/OffsetCurve.h"

#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/buffer/BufferParameters.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Dimension;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::operation::buffer::BufferParameters;
using geos::operation::buffer::OffsetCurve;
using geos::util::IllegalArgumentException;

TEST(OffsetCurveTest, ConstantsHaveExpectedValues) {
  EXPECT_EQ(+OffsetCurve::MATCH_DISTANCE_FACTOR, 10000);
  EXPECT_EQ(+OffsetCurve::MIN_QUADRANT_SEGMENTS, 8);
}

TEST(OffsetCurveTest, ConstructorRejectsNonFiniteDistances) {
  GeometryFactory::Ptr gf = GeometryFactory::create();
  WKTReader reader(gf.get());
  std::unique_ptr<Geometry> line = reader.read("LINESTRING (0 0, 10 0)");
  BufferParameters bp(12);

  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  const double kInf = std::numeric_limits<double>::infinity();
  const double kNegInf = -std::numeric_limits<double>::infinity();

  for (double bad_dist : {kNaN, kInf, kNegInf}) {
    EXPECT_THROW(OffsetCurve(*line, bad_dist), IllegalArgumentException);
    EXPECT_THROW(OffsetCurve(*line, bad_dist, bp), IllegalArgumentException);
    EXPECT_THROW(OffsetCurve::getCurve(*line, bad_dist),
                 IllegalArgumentException);
  }
}

TEST(OffsetCurveTest, PointsEmptyInputsAndZeroDistance) {
  GeometryFactory::Ptr gf = GeometryFactory::create();
  WKTReader reader(gf.get());

  for (const char* wkt :
       {"POINT (3 4)", "POINT EMPTY", "MULTIPOINT ((0 0), (5 5))",
        "MULTIPOINT EMPTY", "LINESTRING EMPTY", "MULTILINESTRING EMPTY",
        "POLYGON EMPTY", "MULTIPOLYGON EMPTY", "GEOMETRYCOLLECTION EMPTY"}) {
    std::unique_ptr<Geometry> geom = reader.read(wkt);
    std::unique_ptr<Geometry> curve = OffsetCurve::getCurve(*geom, 2.0);
    ASSERT_NE(curve, nullptr) << wkt;
    EXPECT_TRUE(curve->isEmpty()) << wkt;
    EXPECT_EQ(curve->getDimension(), Dimension::L) << wkt;
  }

  std::unique_ptr<Geometry> zero_len = reader.read("LINESTRING (2 2, 2 2)");
  std::unique_ptr<Geometry> zero_len_curve =
      OffsetCurve::getCurve(*zero_len, 2.0);
  ASSERT_NE(zero_len_curve, nullptr);
  EXPECT_TRUE(zero_len_curve->isEmpty());
  EXPECT_EQ(zero_len_curve->getDimension(), Dimension::L);

  std::unique_ptr<Geometry> line = reader.read("LINESTRING (0 0, 3 4, 8 4)");
  for (double zero_dist : {0.0, -0.0}) {
    std::unique_ptr<Geometry> same = OffsetCurve::getCurve(*line, zero_dist);
    ASSERT_NE(same, nullptr);
    EXPECT_TRUE(same->equalsExact(line.get()));
  }
}

TEST(OffsetCurveTest, SingleSegmentPositiveNegativeDiagonalAndJoined) {
  GeometryFactory::Ptr gf = GeometryFactory::create();
  WKTReader reader(gf.get());

  std::unique_ptr<Geometry> horiz = reader.read("LINESTRING (0 0, 10 0)");

  // Positive distance offsets to the left (+Y).
  std::unique_ptr<Geometry> left = OffsetCurve::getCurve(*horiz, 2.5);
  ASSERT_NE(left, nullptr);
  EXPECT_EQ(left->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_DOUBLE_EQ(left->getLength(), 10.0);
  const Envelope* left_env = left->getEnvelopeInternal();
  EXPECT_DOUBLE_EQ(left_env->getMinX(), 0.0);
  EXPECT_DOUBLE_EQ(left_env->getMaxX(), 10.0);
  EXPECT_DOUBLE_EQ(left_env->getMinY(), 2.5);
  EXPECT_DOUBLE_EQ(left_env->getMaxY(), 2.5);

  // Negative distance offsets to the right (-Y).
  std::unique_ptr<Geometry> right = OffsetCurve::getCurve(*horiz, -2.5);
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(right->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_DOUBLE_EQ(right->getLength(), 10.0);
  const Envelope* right_env = right->getEnvelopeInternal();
  EXPECT_DOUBLE_EQ(right_env->getMinX(), 0.0);
  EXPECT_DOUBLE_EQ(right_env->getMaxX(), 10.0);
  EXPECT_DOUBLE_EQ(right_env->getMinY(), -2.5);
  EXPECT_DOUBLE_EQ(right_env->getMaxY(), -2.5);

  // Joined mode on a single section produces the same single segment.
  OffsetCurve oc(*horiz, 2.5);
  oc.setJoined(true);
  std::unique_ptr<Geometry> joined = oc.getCurve();
  ASSERT_NE(joined, nullptr);
  EXPECT_EQ(joined->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_DOUBLE_EQ(joined->getLength(), 10.0);

  // Diagonal (0,0)->(6,8) has length 10 and unit normal (-0.8, 0.6).
  std::unique_ptr<Geometry> diag = reader.read("LINESTRING (0 0, 6 8)");
  std::unique_ptr<Geometry> diag_left = OffsetCurve::getCurve(*diag, 5.0);
  const Envelope* dl_env = diag_left->getEnvelopeInternal();
  EXPECT_NEAR(dl_env->getMinX(), -4.0, 1e-9);
  EXPECT_NEAR(dl_env->getMaxX(), 2.0, 1e-9);
  EXPECT_NEAR(dl_env->getMinY(), 3.0, 1e-9);
  EXPECT_NEAR(dl_env->getMaxY(), 11.0, 1e-9);
  EXPECT_NEAR(diag_left->getLength(), 10.0, 1e-9);

  std::unique_ptr<Geometry> diag_right = OffsetCurve::getCurve(*diag, -5.0);
  const Envelope* dr_env = diag_right->getEnvelopeInternal();
  EXPECT_NEAR(dr_env->getMinX(), 4.0, 1e-9);
  EXPECT_NEAR(dr_env->getMaxX(), 10.0, 1e-9);
  EXPECT_NEAR(dr_env->getMinY(), -3.0, 1e-9);
  EXPECT_NEAR(dr_env->getMaxY(), 5.0, 1e-9);
  EXPECT_NEAR(diag_right->getLength(), 10.0, 1e-9);
}

TEST(OffsetCurveTest, JoinStylesQuadSegmentsClampingAndMitreLimit) {
  GeometryFactory::Ptr gf = GeometryFactory::create();
  WKTReader reader(gf.get());

  std::unique_ptr<Geometry> corner =
      reader.read("LINESTRING (0 0, 10 0, 10 10)");

  // Outer right corner offset with mitre join extends sharply to (12, -2).
  std::unique_ptr<Geometry> mitre = OffsetCurve::getCurve(
      *corner, -2.0, 8, BufferParameters::JOIN_MITRE, 5.0);
  ASSERT_NE(mitre, nullptr);
  EXPECT_EQ(mitre->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_GE(mitre->getNumPoints(), 3u);
  EXPECT_NEAR(mitre->getEnvelopeInternal()->getMaxX(), 12.0, 1e-9);
  EXPECT_NEAR(mitre->getEnvelopeInternal()->getMinY(), -2.0, 1e-9);
  EXPECT_NEAR(mitre->getLength(), 24.0, 1e-9);

  // Bevel join cuts diagonally across the corner, yielding shorter length.
  std::unique_ptr<Geometry> bevel = OffsetCurve::getCurve(
      *corner, -2.0, 8, BufferParameters::JOIN_BEVEL, 5.0);
  ASSERT_NE(bevel, nullptr);
  EXPECT_EQ(bevel->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_GE(bevel->getNumPoints(), 4u);
  EXPECT_LT(bevel->getLength(), mitre->getLength());
  EXPECT_NEAR(bevel->getEnvelopeInternal()->getMaxX(), 12.0, 1e-9);
  EXPECT_NEAR(bevel->getEnvelopeInternal()->getMinY(), -2.0, 1e-9);

  // Small quadSegs (< MIN_QUADRANT_SEGMENTS = 8) are clamped up to 8; larger
  // quadSegs (16) produce finer discretization around the round join.
  std::unique_ptr<Geometry> round_clamped = OffsetCurve::getCurve(
      *corner, -2.0, 2, BufferParameters::JOIN_ROUND, -1.0);
  std::unique_ptr<Geometry> round_default = OffsetCurve::getCurve(
      *corner, -2.0, -1, BufferParameters::JOIN_ROUND, -1.0);
  std::unique_ptr<Geometry> round_fine = OffsetCurve::getCurve(
      *corner, -2.0, 16, BufferParameters::JOIN_ROUND, 5.0);
  EXPECT_EQ(round_clamped->getNumPoints(), round_default->getNumPoints());
  EXPECT_GE(round_clamped->getNumPoints(), 10u);
  EXPECT_GT(round_fine->getNumPoints(), round_default->getNumPoints());

  // Constructor taking BufferParameters also clamps quadSegs < 8 to 8, while
  // preserving larger quadSegs, join style, and mitre limit.
  BufferParameters small_bp(3, BufferParameters::CAP_ROUND,
                            BufferParameters::JOIN_ROUND, 5.0);
  OffsetCurve oc_clamped(*corner, -2.0, small_bp);
  std::unique_ptr<Geometry> from_bp_clamped = oc_clamped.getCurve();
  EXPECT_EQ(from_bp_clamped->getNumPoints(), round_default->getNumPoints());

  BufferParameters custom_bp(16, BufferParameters::CAP_FLAT,
                             BufferParameters::JOIN_MITRE, 3.0);
  OffsetCurve oc_custom(*corner, -2.0, custom_bp);
  std::unique_ptr<Geometry> from_bp_custom = oc_custom.getCurve();
  std::unique_ptr<Geometry> via_static = OffsetCurve::getCurve(
      *corner, -2.0, 16, BufferParameters::JOIN_MITRE, 3.0);
  EXPECT_TRUE(from_bp_custom->equalsExact(via_static.get()));
  EXPECT_NEAR(from_bp_custom->getLength(), 24.0, 1e-9);

  // Acute turn tests mitre limit clipping (mitreLimit = 1.5 vs 20.0).
  std::unique_ptr<Geometry> acute = reader.read("LINESTRING (0 0, 10 0, 0 2)");
  std::unique_ptr<Geometry> acute_clipped =
      OffsetCurve::getCurve(*acute, -2.0, 8, BufferParameters::JOIN_MITRE, 1.5);
  std::unique_ptr<Geometry> acute_extended = OffsetCurve::getCurve(
      *acute, -2.0, 8, BufferParameters::JOIN_MITRE, 20.0);
  EXPECT_FALSE(acute_clipped->isEmpty());
  EXPECT_FALSE(acute_extended->isEmpty());
  EXPECT_LT(acute_clipped->getEnvelopeInternal()->getMaxX(),
            acute_extended->getEnvelopeInternal()->getMaxX());

  // Negative mitreLimit skips setMitreLimit and keeps BufferParameters default.
  std::unique_ptr<Geometry> corner_default_mitre = OffsetCurve::getCurve(
      *corner, -2.0, 8, BufferParameters::JOIN_MITRE, -1.0);
  EXPECT_NEAR(corner_default_mitre->getLength(), 24.0, 1e-9);
}

TEST(OffsetCurveTest, DisjointPocketPinchAndJoinedCurve) {
  GeometryFactory::Ptr gf = GeometryFactory::create();
  WKTReader reader(gf.get());

  // Pinched bay: (0,0)->(20,0) merges with (6,3)->(14,3) at dist = 2.0,
  // splitting the left offset curve into one section on the buffer shell
  // and a second section on the interior hole boundary.
  std::unique_ptr<Geometry> pinched =
      reader.read("LINESTRING (0 0, 20 0, 20 15, 6 15, 6 3, 14 3, 14 10)");

  std::unique_ptr<Geometry> unjoined = OffsetCurve::getCurve(*pinched, 2.0);
  ASSERT_NE(unjoined, nullptr);
  EXPECT_EQ(unjoined->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(unjoined->getNumGeometries(), 2u);
  EXPECT_TRUE(unjoined->isValid());

  OffsetCurve oc(*pinched, 2.0);
  oc.setJoined(true);
  std::unique_ptr<Geometry> joined_member = oc.getCurve();
  std::unique_ptr<Geometry> joined_static =
      OffsetCurve::getCurveJoined(*pinched, 2.0);
  ASSERT_NE(joined_member, nullptr);
  ASSERT_NE(joined_static, nullptr);
  EXPECT_EQ(joined_member->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(joined_member->equalsExact(joined_static.get()));
  EXPECT_GT(joined_member->getLength(), unjoined->getLength());

  // Flipping back to unjoined on the same instance separates sections again.
  oc.setJoined(false);
  std::unique_ptr<Geometry> toggled_back = oc.getCurve();
  EXPECT_EQ(toggled_back->getGeometryTypeId(),
            geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(toggled_back->getNumGeometries(), 2u);

  // Right-side offset of pinched bay also produces 2 disjoint sections
  // (outer perimeter + inner finger).
  std::unique_ptr<Geometry> pinched_right =
      OffsetCurve::getCurve(*pinched, -2.0);
  ASSERT_NE(pinched_right, nullptr);
  EXPECT_EQ(pinched_right->getGeometryTypeId(),
            geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(pinched_right->getNumGeometries(), 2u);

  // Unpinched U-bend on the right (outside) yields a single connected curve.
  std::unique_ptr<Geometry> wide_u =
      reader.read("LINESTRING (0 10, 0 0, 20 0, 20 10)");
  std::unique_ptr<Geometry> outer = OffsetCurve::getCurve(*wide_u, -2.5);
  ASSERT_NE(outer, nullptr);
  EXPECT_EQ(outer->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_GT(outer->getLength(), 40.0);
}

TEST(OffsetCurveTest, SelfIntersectingCrossingAndFigureEight) {
  GeometryFactory::Ptr gf = GeometryFactory::create();
  WKTReader reader(gf.get());

  std::unique_ptr<Geometry> alpha =
      reader.read("LINESTRING (0 5, 10 5, 5 0, 5 10)");
  // Left offset on crossing line splits into 2 sections on the buffer shell
  // in unjoined mode and joins across raw-curve index gaps in joined mode.
  std::unique_ptr<Geometry> alpha_unjoined = OffsetCurve::getCurve(*alpha, 0.5);
  ASSERT_NE(alpha_unjoined, nullptr);
  EXPECT_EQ(alpha_unjoined->getGeometryTypeId(),
            geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(alpha_unjoined->getNumGeometries(), 2u);
  EXPECT_TRUE(alpha_unjoined->isValid());

  std::unique_ptr<Geometry> alpha_joined =
      OffsetCurve::getCurveJoined(*alpha, 0.5);
  ASSERT_NE(alpha_joined, nullptr);
  EXPECT_EQ(alpha_joined->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(alpha_joined->isValid());

  for (double dist : {0.5, -0.5}) {
    std::unique_ptr<Geometry> unjoined = OffsetCurve::getCurve(*alpha, dist);
    std::unique_ptr<Geometry> joined =
        OffsetCurve::getCurveJoined(*alpha, dist);
    ASSERT_NE(unjoined, nullptr);
    ASSERT_NE(joined, nullptr);
    EXPECT_FALSE(unjoined->isEmpty());
    EXPECT_FALSE(joined->isEmpty());
    EXPECT_EQ(unjoined->getDimension(), Dimension::L);
    EXPECT_EQ(joined->getDimension(), Dimension::L);
    EXPECT_TRUE(unjoined->isValid());
    EXPECT_TRUE(joined->isValid());
  }

  std::unique_ptr<Geometry> bowtie =
      reader.read("LINESTRING (0 0, 10 10, 10 0, 0 10, 0 0)");
  for (double dist : {0.5, -0.5}) {
    std::unique_ptr<Geometry> unjoined = OffsetCurve::getCurve(*bowtie, dist);
    std::unique_ptr<Geometry> joined =
        OffsetCurve::getCurveJoined(*bowtie, dist);
    ASSERT_NE(unjoined, nullptr);
    ASSERT_NE(joined, nullptr);
    EXPECT_FALSE(unjoined->isEmpty());
    EXPECT_FALSE(joined->isEmpty());
    EXPECT_TRUE(unjoined->isValid());
    EXPECT_TRUE(joined->isValid());
  }

  // Collinear 3+ vertex line and hairpin reversal exercise computeSections
  // (bypassing the 2-point offsetSegment shortcut).
  std::unique_ptr<Geometry> collinear =
      reader.read("LINESTRING (0 0, 5 0, 10 0)");
  std::unique_ptr<Geometry> collinear_left =
      OffsetCurve::getCurve(*collinear, 2.0);
  EXPECT_EQ(collinear_left->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_NEAR(collinear_left->getLength(), 10.0, 1e-9);
  EXPECT_NEAR(collinear_left->getEnvelopeInternal()->getMinY(), 2.0, 1e-9);

  std::unique_ptr<Geometry> spike = reader.read("LINESTRING (0 0, 10 0, 2 0)");
  EXPECT_FALSE(OffsetCurve::getCurve(*spike, 1.0)->isEmpty());
  EXPECT_FALSE(OffsetCurve::getCurveJoined(*spike, 1.0)->isEmpty());
  EXPECT_FALSE(OffsetCurve::getCurve(*spike, -1.0)->isEmpty());
}

TEST(OffsetCurveTest, ClosedLineStringsAndLinearRingHolesAndWraparound) {
  GeometryFactory::Ptr gf = GeometryFactory::create();
  WKTReader reader(gf.get());

  // CCW closed ring: left (+1) traces the interior hole [1,9]x[1,9], while
  // right (-1) traces the expanded outer boundary.
  std::unique_ptr<Geometry> ccw_square =
      reader.read("LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)");

  std::unique_ptr<Geometry> inner = OffsetCurve::getCurve(*ccw_square, 1.0);
  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(inner->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_NEAR(inner->getLength(), 32.0, 1e-6);
  const Envelope* inner_env = inner->getEnvelopeInternal();
  EXPECT_NEAR(inner_env->getMinX(), 1.0, 1e-9);
  EXPECT_NEAR(inner_env->getMaxX(), 9.0, 1e-9);
  EXPECT_NEAR(inner_env->getMinY(), 1.0, 1e-9);
  EXPECT_NEAR(inner_env->getMaxY(), 9.0, 1e-9);

  std::unique_ptr<Geometry> inner_joined =
      OffsetCurve::getCurveJoined(*ccw_square, 1.0);
  EXPECT_NEAR(inner_joined->getLength(), 32.0, 1e-6);

  std::unique_ptr<Geometry> outer = OffsetCurve::getCurve(*ccw_square, -1.0);
  ASSERT_NE(outer, nullptr);
  EXPECT_EQ(outer->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  const Envelope* outer_env = outer->getEnvelopeInternal();
  EXPECT_NEAR(outer_env->getMinX(), -1.0, 1e-9);
  EXPECT_NEAR(outer_env->getMaxX(), 11.0, 1e-9);
  EXPECT_NEAR(outer_env->getMinY(), -1.0, 1e-9);
  EXPECT_NEAR(outer_env->getMaxY(), 11.0, 1e-9);

  std::unique_ptr<Geometry> outer_joined =
      OffsetCurve::getCurveJoined(*ccw_square, -1.0);
  EXPECT_FALSE(outer_joined->isEmpty());

  // When inner hole erodes completely (dist = 6.0 > half-width 5.0), no
  // section matches and getCurve returns an empty LineString.
  std::unique_ptr<Geometry> eroded = OffsetCurve::getCurve(*ccw_square, 6.0);
  ASSERT_NE(eroded, nullptr);
  EXPECT_TRUE(eroded->isEmpty());
  EXPECT_EQ(eroded->getDimension(), Dimension::L);

  // CW closed ring flips left/right relative to CCW.
  std::unique_ptr<Geometry> cw_square =
      reader.read("LINESTRING (0 0, 0 10, 10 10, 10 0, 0 0)");
  std::unique_ptr<Geometry> cw_left = OffsetCurve::getCurve(*cw_square, 1.0);
  std::unique_ptr<Geometry> cw_right = OffsetCurve::getCurve(*cw_square, -1.0);
  EXPECT_NEAR(cw_left->getEnvelopeInternal()->getMinX(), -1.0, 1e-9);
  EXPECT_NEAR(cw_right->getEnvelopeInternal()->getMinX(), 1.0, 1e-9);

  // LinearRing input extracted from a Polygon's exterior ring.
  std::unique_ptr<Geometry> poly_geom =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  const auto* poly = dynamic_cast<const Polygon*>(poly_geom.get());
  ASSERT_NE(poly, nullptr);
  const LinearRing* ring = poly->getExteriorRing();
  ASSERT_NE(ring, nullptr);
  EXPECT_FALSE(OffsetCurve::getCurve(*ring, 1.0)->isEmpty());
  EXPECT_FALSE(OffsetCurve::getCurve(*ring, -1.0)->isEmpty());
  std::unique_ptr<Geometry> ring_zero = OffsetCurve::getCurve(*ring, 0.0);
  EXPECT_TRUE(ring_zero->equalsExact(ring));

  // Pinched C-shape: shell has no raw-curve matches at dist = 2.0 while the
  // interior hole has one contiguous section.
  std::unique_ptr<Geometry> c_loop =
      reader.read("LINESTRING (4 10, 0 10, 0 0, 10 0, 10 10, 6 10)");
  std::unique_ptr<Geometry> c_inner = OffsetCurve::getCurve(*c_loop, 2.0);
  ASSERT_NE(c_inner, nullptr);
  EXPECT_EQ(c_inner->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_FALSE(c_inner->isEmpty());
  std::unique_ptr<Geometry> c_outer = OffsetCurve::getCurve(*c_loop, -2.0);
  ASSERT_NE(c_outer, nullptr);
  EXPECT_EQ(c_outer->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);

  // Closed pinched-waist ring: +1.5 creates two interior holes in the buffer
  // (left and right lobes), while -1.5 traces the full pinched outer shell.
  std::unique_ptr<Geometry> pinched_ring = reader.read(
      "LINESTRING (0 0, 10 0, 10 10, 6 10, 6 2, 4 2, 4 10, 0 10, 0 0)");
  std::unique_ptr<Geometry> lobes_unjoined =
      OffsetCurve::getCurve(*pinched_ring, 1.5);
  ASSERT_NE(lobes_unjoined, nullptr);
  EXPECT_EQ(lobes_unjoined->getGeometryTypeId(),
            geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(lobes_unjoined->getNumGeometries(), 2u);
  EXPECT_TRUE(lobes_unjoined->isValid());

  std::unique_ptr<Geometry> lobes_joined =
      OffsetCurve::getCurveJoined(*pinched_ring, 1.5);
  ASSERT_NE(lobes_joined, nullptr);
  EXPECT_EQ(lobes_joined->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_FALSE(lobes_joined->isEmpty());

  std::unique_ptr<Geometry> outer_pinched =
      OffsetCurve::getCurve(*pinched_ring, -1.5);
  std::unique_ptr<Geometry> outer_pinched_joined =
      OffsetCurve::getCurveJoined(*pinched_ring, -1.5);
  ASSERT_NE(outer_pinched, nullptr);
  ASSERT_NE(outer_pinched_joined, nullptr);
  EXPECT_EQ(outer_pinched->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_EQ(outer_pinched_joined->getGeometryTypeId(),
            geos::geom::GEOS_LINESTRING);
}

TEST(OffsetCurveTest, PolygonsAndMultiPolygons) {
  GeometryFactory::Ptr gf = GeometryFactory::create();
  WKTReader reader(gf.get());

  std::unique_ptr<Geometry> square =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");

  std::unique_ptr<Geometry> expanded = OffsetCurve::getCurve(*square, 2.0);
  EXPECT_EQ(expanded->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_NEAR(expanded->getEnvelopeInternal()->getMinX(), -2.0, 1e-9);
  EXPECT_NEAR(expanded->getEnvelopeInternal()->getMaxX(), 12.0, 1e-9);

  std::unique_ptr<Geometry> expanded_mitre =
      OffsetCurve::getCurve(*square, 2.0, 8, BufferParameters::JOIN_MITRE, 5.0);
  EXPECT_EQ(expanded_mitre->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_NEAR(expanded_mitre->getLength(), 56.0, 1e-9);

  std::unique_ptr<Geometry> expanded_bevel =
      OffsetCurve::getCurve(*square, 2.0, 8, BufferParameters::JOIN_BEVEL, 5.0);
  EXPECT_EQ(expanded_bevel->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_LT(expanded_bevel->getLength(), 56.0);

  std::unique_ptr<Geometry> shrunk = OffsetCurve::getCurve(*square, -2.0);
  EXPECT_EQ(shrunk->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_NEAR(shrunk->getLength(), 24.0, 1e-9);

  std::unique_ptr<Geometry> collapsed = OffsetCurve::getCurve(*square, -6.0);
  EXPECT_TRUE(collapsed->isEmpty());
  EXPECT_EQ(collapsed->getDimension(), Dimension::L);

  std::unique_ptr<Geometry> zero_poly = OffsetCurve::getCurve(*square, 0.0);
  EXPECT_EQ(zero_poly->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_NEAR(zero_poly->getLength(), 40.0, 1e-9);

  // Polygon with an interior hole yields MultiLineString boundary.
  std::unique_ptr<Geometry> donut = reader.read(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(5 5, 5 15, 15 15, 15 5, 5 5))");
  for (double d : {-1.0, 0.0, 1.0}) {
    std::unique_ptr<Geometry> bnd = OffsetCurve::getCurve(*donut, d);
    EXPECT_EQ(bnd->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
    EXPECT_EQ(bnd->getNumGeometries(), 2u);
  }

  // Negative buffer splits dumbbell Polygon across narrow width-1 bridge.
  std::unique_ptr<Geometry> dumbbell = reader.read(
      "POLYGON ((0 0, 4 0, 4 1.5, 8 1.5, 8 0, 12 0, "
      "12 4, 8 4, 8 2.5, 4 2.5, 4 4, 0 4, 0 0))");
  std::unique_ptr<Geometry> split_bnd = OffsetCurve::getCurve(*dumbbell, -0.8);
  EXPECT_EQ(split_bnd->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(split_bnd->getNumGeometries(), 2u);

  // OffsetCurve maps each Polygon element individually via flatMap, so a
  // 2-polygon MultiPolygon produces a 2-component MultiLineString even when
  // expanded curves overlap.
  std::unique_ptr<Geometry> mp = reader.read(
      "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "((10 0, 14 0, 14 4, 10 4, 10 0)))");
  std::unique_ptr<Geometry> mp_neg = OffsetCurve::getCurve(*mp, -1.0);
  EXPECT_EQ(mp_neg->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(mp_neg->getNumGeometries(), 2u);
  EXPECT_NEAR(mp_neg->getLength(), 16.0, 1e-9);

  std::unique_ptr<Geometry> mp_pos = OffsetCurve::getCurve(*mp, 1.0);
  EXPECT_EQ(mp_pos->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(mp_pos->getNumGeometries(), 2u);

  std::unique_ptr<Geometry> mp_expanded = OffsetCurve::getCurve(*mp, 4.0);
  EXPECT_EQ(mp_expanded->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(mp_expanded->getNumGeometries(), 2u);

  std::unique_ptr<Geometry> mp_zero = OffsetCurve::getCurve(*mp, 0.0);
  EXPECT_EQ(mp_zero->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(mp_zero->getNumGeometries(), 2u);

  // When one polygon collapses on negative offset, flatMap unwraps the single
  // surviving curve to a LineString.
  std::unique_ptr<Geometry> mp_uneven = reader.read(
      "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), "
      "((10 0, 20 0, 20 10, 10 10, 10 0)))");
  std::unique_ptr<Geometry> mp_one = OffsetCurve::getCurve(*mp_uneven, -2.5);
  EXPECT_EQ(mp_one->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_NEAR(mp_one->getLength(), 20.0, 1e-9);
}

TEST(OffsetCurveTest, MultiLineStringsAndNestedGeometryCollections) {
  GeometryFactory::Ptr gf = GeometryFactory::create();
  WKTReader reader(gf.get());

  std::unique_ptr<Geometry> mls =
      reader.read("MULTILINESTRING ((0 0, 10 0), (0 10, 10 10))");
  std::unique_ptr<Geometry> mls_left = OffsetCurve::getCurve(*mls, 2.0);
  EXPECT_EQ(mls_left->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(mls_left->getNumGeometries(), 2u);
  EXPECT_NEAR(mls_left->getLength(), 20.0, 1e-9);

  std::unique_ptr<Geometry> mls_right = OffsetCurve::getCurve(*mls, -2.0);
  EXPECT_EQ(mls_right->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(mls_right->getNumGeometries(), 2u);

  std::unique_ptr<Geometry> mls_joined = OffsetCurve::getCurveJoined(*mls, 2.0);
  EXPECT_EQ(mls_joined->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(mls_joined->getNumGeometries(), 2u);

  std::unique_ptr<Geometry> hetero = reader.read(
      "GEOMETRYCOLLECTION (POINT (1 1), LINESTRING (0 0, 10 0), "
      "POLYGON ((20 0, 24 0, 24 4, 20 4, 20 0)))");
  std::unique_ptr<Geometry> hetero_curve = OffsetCurve::getCurve(*hetero, 1.0);
  EXPECT_EQ(hetero_curve->getGeometryTypeId(),
            geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(hetero_curve->getNumGeometries(), 2u);

  // Nested GeometryCollection is flattened via GeometryMapper::flatMap.
  std::vector<std::unique_ptr<Geometry>> inner_vec;
  inner_vec.push_back(reader.read("LINESTRING (0 0, 10 0)"));
  std::unique_ptr<GeometryCollection> inner_gc =
      gf->createGeometryCollection(std::move(inner_vec));

  std::vector<std::unique_ptr<Geometry>> outer_vec;
  outer_vec.push_back(std::move(inner_gc));
  outer_vec.push_back(reader.read("MULTIPOINT ((5 5), (6 6))"));
  outer_vec.push_back(reader.read("LINESTRING EMPTY"));
  outer_vec.push_back(reader.read("LINESTRING (0 20, 10 20)"));
  outer_vec.push_back(reader.read("POLYGON ((30 0, 34 0, 34 4, 30 4, 30 0))"));
  std::unique_ptr<GeometryCollection> nested_gc =
      gf->createGeometryCollection(std::move(outer_vec));

  std::unique_ptr<Geometry> nested_curve =
      OffsetCurve::getCurve(*nested_gc, 1.0);
  EXPECT_EQ(nested_curve->getGeometryTypeId(),
            geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(nested_curve->getNumGeometries(), 3u);
}

TEST(OffsetCurveTest, FixedPrecisionMultiPolygonBufferSelection) {
  // PrecisionModel must outlive fixed_gf and geometries created from it.
  PrecisionModel pm(1.0);
  GeometryFactory::Ptr fixed_gf = GeometryFactory::create(&pm);
  WKTReader reader(fixed_gf.get());

  // Under unit grid snapping with dist = 0.6, the vertical spans snap to
  // width-2 polygons while the non-collinear diagonal bridge (4,9)-(8,14)
  // collapses onto a single canceled edge on both sides, producing a
  // MultiPolygon buffer so extractMaxAreaPolygon selects the larger leg.
  std::unique_ptr<Geometry> two_span =
      reader.read("LINESTRING (0 0, 0 5, 4 9, 8 14, 12 18, 12 28)");

  for (double dist : {0.6, -0.6}) {
    std::unique_ptr<Geometry> curve = OffsetCurve::getCurve(*two_span, dist);
    ASSERT_NE(curve, nullptr);
    ASSERT_FALSE(curve->isEmpty());
    EXPECT_EQ(curve->getDimension(), Dimension::L);
    EXPECT_TRUE(curve->isValid());
    EXPECT_GE(curve->getEnvelopeInternal()->getMinX(), 7.0);
    EXPECT_GE(curve->getEnvelopeInternal()->getMinY(), 13.0);
    EXPECT_GT(curve->getLength(), 10.0);
  }

  // Three vertical spans of lengths 2, 10, 5 separated by collapsed bridges
  // yield three polygons ordered [small, large, medium], exercising both true
  // and false branches of `area > maxArea` in extractMaxAreaPolygon and
  // selecting the middle span.
  std::unique_ptr<Geometry> three_span = reader.read(
      "LINESTRING (0 0, 0 2, 4 6, 8 11, 12 15, 12 25, "
      "16 29, 20 34, 24 38, 24 43)");
  for (double dist : {0.6, -0.6}) {
    std::unique_ptr<Geometry> curve = OffsetCurve::getCurve(*three_span, dist);
    ASSERT_NE(curve, nullptr);
    ASSERT_FALSE(curve->isEmpty());
    EXPECT_EQ(curve->getDimension(), Dimension::L);
    EXPECT_TRUE(curve->isValid());
    const Envelope* env = curve->getEnvelopeInternal();
    EXPECT_GE(env->getMinX(), 7.0);
    EXPECT_LE(env->getMaxX(), 17.0);
    EXPECT_GE(env->getMinY(), 10.0);
    EXPECT_LE(env->getMaxY(), 30.0);
    EXPECT_GT(curve->getLength(), 10.0);
  }
}

TEST(OffsetCurveTest, RawOffsetAndRawOffsetCurve) {
  GeometryFactory::Ptr gf = GeometryFactory::create();
  WKTReader reader(gf.get());

  std::unique_ptr<Geometry> line_geom =
      reader.read("LINESTRING (0 0, 0 0, 10 0, 10 0, 10 5)");
  const auto* line = dynamic_cast<const LineString*>(line_geom.get());
  ASSERT_NE(line, nullptr);

  std::unique_ptr<CoordinateSequence> raw_left =
      OffsetCurve::rawOffset(*line, 2.0);
  ASSERT_NE(raw_left, nullptr);
  EXPECT_GE(raw_left->size(), 2u);
  Coordinate first_left;
  raw_left->getAt(0, first_left);
  EXPECT_NEAR(first_left.x, 0.0, 1e-9);
  EXPECT_NEAR(first_left.y, 2.0, 1e-9);

  std::unique_ptr<CoordinateSequence> raw_right =
      OffsetCurve::rawOffset(*line, -2.0);
  ASSERT_NE(raw_right, nullptr);
  EXPECT_GE(raw_right->size(), 2u);

  std::unique_ptr<CoordinateSequence> raw_zero =
      OffsetCurve::rawOffset(*line, 0.0);
  EXPECT_EQ(raw_zero, nullptr);

  BufferParameters zero_bp(8, BufferParameters::CAP_ROUND,
                           BufferParameters::JOIN_ROUND, 3.0);
  EXPECT_EQ(OffsetCurve::rawOffsetCurve(*line, 0.0, zero_bp), nullptr);

  for (BufferParameters::JoinStyle style :
       {BufferParameters::JOIN_ROUND, BufferParameters::JOIN_MITRE,
        BufferParameters::JOIN_BEVEL}) {
    BufferParameters bp(8, BufferParameters::CAP_ROUND, style, 3.0);
    std::unique_ptr<CoordinateSequence> seq_pos =
        OffsetCurve::rawOffsetCurve(*line, 2.0, bp);
    std::unique_ptr<CoordinateSequence> seq_neg =
        OffsetCurve::rawOffsetCurve(*line, -2.0, bp);
    ASSERT_NE(seq_pos, nullptr);
    ASSERT_NE(seq_neg, nullptr);
    EXPECT_FALSE(seq_pos->isEmpty());
    EXPECT_FALSE(seq_neg->isEmpty());
  }
}

}  // namespace
}  // namespace geos_google
