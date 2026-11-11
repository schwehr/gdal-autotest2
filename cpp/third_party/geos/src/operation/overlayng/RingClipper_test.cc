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

// Unit tests for third_party/geos/src/operation/overlayng/RingClipper.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/overlayng/RingClipper.cpp

#include "third_party/geos/include/geos/operation/overlayng/RingClipper.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::operation::overlayng::RingClipper;

void ExpectClipped2D(const CoordinateSequence* actual,
                     const std::vector<CoordinateXY>& expected) {
  ASSERT_NE(actual, nullptr);
  EXPECT_FALSE(actual->hasRepeatedPoints());
  ASSERT_EQ(actual->size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_DOUBLE_EQ(actual->getX(i), expected[i].x) << "at index " << i;
    EXPECT_DOUBLE_EQ(actual->getY(i), expected[i].y) << "at index " << i;
  }
}

TEST(RingClipperTest, NullEnvelopeReturnsEmptySequence) {
  Envelope null_env;
  RingClipper clipper(&null_env);
  CoordinateSequence ring({CoordinateXY{1.0, 1.0}, CoordinateXY{5.0, 1.0},
                           CoordinateXY{5.0, 5.0}, CoordinateXY{1.0, 1.0}});

  ExpectClipped2D(clipper.clip(&ring).get(), {});
}

TEST(RingClipperTest, OutsideEachBoxEdgeReturnsEmptyEarly) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  RingClipper clipper(&env);

  // Completely on or below bottom edge (y <= minY) -> returns at edgeIndex 0.
  CoordinateSequence below({CoordinateXY{1.0, -5.0}, CoordinateXY{5.0, -5.0},
                            CoordinateXY{5.0, 0.0}, CoordinateXY{1.0, -5.0}});
  ExpectClipped2D(clipper.clip(&below).get(), {});

  // Survives bottom (y in (0, 10)), completely on or right of maxX ->
  // edgeIndex 1.
  CoordinateSequence right({CoordinateXY{10.0, 2.0}, CoordinateXY{15.0, 2.0},
                            CoordinateXY{15.0, 8.0}, CoordinateXY{10.0, 2.0}});
  ExpectClipped2D(clipper.clip(&right).get(), {});

  // Survives bottom and right, completely on or above maxY -> edgeIndex 2.
  CoordinateSequence above({CoordinateXY{2.0, 10.0}, CoordinateXY{8.0, 10.0},
                            CoordinateXY{5.0, 15.0}, CoordinateXY{2.0, 10.0}});
  ExpectClipped2D(clipper.clip(&above).get(), {});

  // Survives bottom, right, and top, completely on or left of minX ->
  // edgeIndex 3.
  CoordinateSequence left({CoordinateXY{-5.0, 2.0}, CoordinateXY{0.0, 2.0},
                           CoordinateXY{0.0, 8.0}, CoordinateXY{-5.0, 2.0}});
  ExpectClipped2D(clipper.clip(&left).get(), {});
}

TEST(RingClipperTest, RingCoincidentWithOrSharingEnvelopeBoundaryIsPreserved) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  RingClipper clipper(&env);

  // Each box edge clips against one open half-plane, so a full ring whose
  // vertices lie on the envelope corners crosses into each half-plane and is
  // retained along the envelope boundary.
  CoordinateSequence on_boundary(
      {CoordinateXY{0.0, 0.0}, CoordinateXY{10.0, 0.0},
       CoordinateXY{10.0, 10.0}, CoordinateXY{0.0, 10.0},
       CoordinateXY{0.0, 0.0}});
  ExpectClipped2D(
      clipper.clip(&on_boundary).get(),
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});

  // Ring sharing bottom and left boundary edges while extending past maxX.
  CoordinateSequence partial_boundary(
      {CoordinateXY{0.0, 0.0}, CoordinateXY{12.0, 0.0}, CoordinateXY{12.0, 6.0},
       CoordinateXY{0.0, 6.0}, CoordinateXY{0.0, 0.0}});
  ExpectClipped2D(
      clipper.clip(&partial_boundary).get(),
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 6.0}, {0.0, 6.0}, {0.0, 0.0}});
}

TEST(RingClipperTest,
     DegenerateBoundaryRingsAndDegenerateEnvelopesReturnEmpty) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  RingClipper clipper(&env);

  // A degenerate ring lying entirely on a single boundary edge has no vertex
  // strictly inside that edge's half-plane and clips to empty.
  CoordinateSequence on_bottom({CoordinateXY{2.0, 0.0}, CoordinateXY{8.0, 0.0},
                                CoordinateXY{5.0, 0.0},
                                CoordinateXY{2.0, 0.0}});
  ExpectClipped2D(clipper.clip(&on_bottom).get(), {});

  CoordinateSequence on_right({CoordinateXY{10.0, 2.0}, CoordinateXY{10.0, 8.0},
                               CoordinateXY{10.0, 5.0},
                               CoordinateXY{10.0, 2.0}});
  ExpectClipped2D(clipper.clip(&on_right).get(), {});

  CoordinateSequence on_top({CoordinateXY{2.0, 10.0}, CoordinateXY{8.0, 10.0},
                             CoordinateXY{5.0, 10.0}, CoordinateXY{2.0, 10.0}});
  ExpectClipped2D(clipper.clip(&on_top).get(), {});

  CoordinateSequence on_left({CoordinateXY{0.0, 2.0}, CoordinateXY{0.0, 8.0},
                              CoordinateXY{0.0, 5.0}, CoordinateXY{0.0, 2.0}});
  ExpectClipped2D(clipper.clip(&on_left).get(), {});

  // Zero-width, zero-height, and point envelopes cannot have interior points.
  CoordinateSequence crossing({CoordinateXY{0.0, 2.0}, CoordinateXY{10.0, 2.0},
                               CoordinateXY{10.0, 8.0}, CoordinateXY{0.0, 8.0},
                               CoordinateXY{0.0, 2.0}});

  Envelope vertical_line_env(5.0, 5.0, 0.0, 10.0);
  RingClipper vertical_clipper(&vertical_line_env);
  ExpectClipped2D(vertical_clipper.clip(&crossing).get(), {});

  Envelope horizontal_line_env(0.0, 10.0, 5.0, 5.0);
  RingClipper horizontal_clipper(&horizontal_line_env);
  ExpectClipped2D(horizontal_clipper.clip(&crossing).get(), {});

  Envelope point_env(CoordinateXY{5.0, 5.0});
  RingClipper point_clipper(&point_env);
  ExpectClipped2D(point_clipper.clip(&crossing).get(), {});
}

TEST(RingClipperTest, CompletelyInsideRingIsPreservedAndUnclosedGetsClosed) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  RingClipper clipper(&env);

  CoordinateSequence inside({CoordinateXY{2.0, 2.0}, CoordinateXY{8.0, 2.0},
                             CoordinateXY{8.0, 8.0}, CoordinateXY{2.0, 8.0},
                             CoordinateXY{2.0, 2.0}});
  ExpectClipped2D(clipper.clip(&inside).get(),
                  {{2.0, 2.0}, {8.0, 2.0}, {8.0, 8.0}, {2.0, 8.0}, {2.0, 2.0}});

  CoordinateSequence unclosed(
      {CoordinateXY{2.0, 2.0}, CoordinateXY{8.0, 2.0}, CoordinateXY{5.0, 8.0}});
  ExpectClipped2D(clipper.clip(&unclosed).get(),
                  {{2.0, 2.0}, {8.0, 2.0}, {5.0, 8.0}, {2.0, 2.0}});
}

TEST(RingClipperTest, ClipsAcrossLeftEdgeWithStartOutsideAndStartInside) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  RingClipper clipper(&env);

  // Starts outside minX so closeRing must append the closing coordinate.
  CoordinateSequence start_outside(
      {CoordinateXY{-5.0, 2.0}, CoordinateXY{5.0, 2.0}, CoordinateXY{5.0, 8.0},
       CoordinateXY{-5.0, 8.0}, CoordinateXY{-5.0, 2.0}});
  ExpectClipped2D(clipper.clip(&start_outside).get(),
                  {{0.0, 2.0}, {5.0, 2.0}, {5.0, 8.0}, {0.0, 8.0}, {0.0, 2.0}});

  // Starts inside minX so final output already ends at start.
  CoordinateSequence start_inside(
      {CoordinateXY{5.0, 2.0}, CoordinateXY{5.0, 8.0}, CoordinateXY{-5.0, 8.0},
       CoordinateXY{-5.0, 2.0}, CoordinateXY{5.0, 2.0}});
  ExpectClipped2D(clipper.clip(&start_inside).get(),
                  {{5.0, 2.0}, {5.0, 8.0}, {0.0, 8.0}, {0.0, 2.0}, {5.0, 2.0}});
}

TEST(RingClipperTest, ClipsAcrossBottomRightAndTopEdges) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  RingClipper clipper(&env);

  CoordinateSequence cross_bottom(
      {CoordinateXY{2.0, -4.0}, CoordinateXY{8.0, -4.0}, CoordinateXY{8.0, 6.0},
       CoordinateXY{2.0, 6.0}, CoordinateXY{2.0, -4.0}});
  ExpectClipped2D(clipper.clip(&cross_bottom).get(),
                  {{8.0, 0.0}, {8.0, 6.0}, {2.0, 6.0}, {2.0, 0.0}, {8.0, 0.0}});

  CoordinateSequence cross_right(
      {CoordinateXY{4.0, 2.0}, CoordinateXY{14.0, 2.0}, CoordinateXY{14.0, 8.0},
       CoordinateXY{4.0, 8.0}, CoordinateXY{4.0, 2.0}});
  ExpectClipped2D(
      clipper.clip(&cross_right).get(),
      {{4.0, 2.0}, {10.0, 2.0}, {10.0, 8.0}, {4.0, 8.0}, {4.0, 2.0}});

  CoordinateSequence cross_top(
      {CoordinateXY{2.0, 4.0}, CoordinateXY{8.0, 4.0}, CoordinateXY{8.0, 14.0},
       CoordinateXY{2.0, 14.0}, CoordinateXY{2.0, 4.0}});
  ExpectClipped2D(
      clipper.clip(&cross_top).get(),
      {{2.0, 4.0}, {8.0, 4.0}, {8.0, 10.0}, {2.0, 10.0}, {2.0, 4.0}});
}

TEST(RingClipperTest, OversizedRectangleAndRotatedDiamondClipAllFourEdges) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  RingClipper clipper(&env);

  CoordinateSequence oversized(
      {CoordinateXY{-5.0, -5.0}, CoordinateXY{15.0, -5.0},
       CoordinateXY{15.0, 15.0}, CoordinateXY{-5.0, 15.0},
       CoordinateXY{-5.0, -5.0}});
  ExpectClipped2D(
      clipper.clip(&oversized).get(),
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});

  CoordinateSequence diamond({CoordinateXY{-3.0, 5.0}, CoordinateXY{5.0, -3.0},
                              CoordinateXY{13.0, 5.0}, CoordinateXY{5.0, 13.0},
                              CoordinateXY{-3.0, 5.0}});
  ExpectClipped2D(clipper.clip(&diamond).get(), {{0.0, 2.0},
                                                 {2.0, 0.0},
                                                 {8.0, 0.0},
                                                 {10.0, 2.0},
                                                 {10.0, 8.0},
                                                 {8.0, 10.0},
                                                 {2.0, 10.0},
                                                 {0.0, 8.0},
                                                 {0.0, 2.0}});
}

TEST(RingClipperTest, ConcaveRingAndBoundaryTouchAvoidDuplicatePoints) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  RingClipper clipper(&env);

  CoordinateSequence u_shape({CoordinateXY{-5.0, 1.0}, CoordinateXY{6.0, 1.0},
                              CoordinateXY{6.0, 3.0}, CoordinateXY{-5.0, 3.0},
                              CoordinateXY{-5.0, 7.0}, CoordinateXY{6.0, 7.0},
                              CoordinateXY{6.0, 9.0}, CoordinateXY{-5.0, 9.0},
                              CoordinateXY{-5.0, 1.0}});
  ExpectClipped2D(clipper.clip(&u_shape).get(), {{0.0, 1.0},
                                                 {6.0, 1.0},
                                                 {6.0, 3.0},
                                                 {0.0, 3.0},
                                                 {0.0, 7.0},
                                                 {6.0, 7.0},
                                                 {6.0, 9.0},
                                                 {0.0, 9.0},
                                                 {0.0, 1.0}});

  CoordinateSequence touch_left({CoordinateXY{5.0, 2.0}, CoordinateXY{0.0, 5.0},
                                 CoordinateXY{5.0, 8.0},
                                 CoordinateXY{5.0, 2.0}});
  ExpectClipped2D(clipper.clip(&touch_left).get(),
                  {{5.0, 2.0}, {0.0, 5.0}, {5.0, 8.0}, {5.0, 2.0}});
}

TEST(RingClipperTest, SinglePointInsideAndOutside) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  RingClipper clipper(&env);

  CoordinateSequence inside({CoordinateXY{5.0, 5.0}});
  ExpectClipped2D(clipper.clip(&inside).get(), {{5.0, 5.0}});

  CoordinateSequence outside({CoordinateXY{15.0, 5.0}});
  ExpectClipped2D(clipper.clip(&outside).get(), {});
}

TEST(RingClipperTest, PreservesZOnInteriorVerticesAndLeavesClippedZNaN) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  RingClipper clipper(&env);

  CoordinateSequence xyz_ring(
      {Coordinate{-5.0, 5.0, 10.0}, Coordinate{5.0, 2.0, 20.0},
       Coordinate{5.0, 8.0, 30.0}, Coordinate{-5.0, 5.0, 10.0}});
  std::unique_ptr<CoordinateSequence> clipped = clipper.clip(&xyz_ring);

  ExpectClipped2D(clipped.get(),
                  {{0.0, 3.5}, {5.0, 2.0}, {5.0, 8.0}, {0.0, 6.5}, {0.0, 3.5}});
  ASSERT_EQ(clipped->size(), 5u);
  EXPECT_TRUE(std::isnan(clipped->getAt(0).z));
  EXPECT_DOUBLE_EQ(clipped->getAt(1).z, 20.0);
  EXPECT_DOUBLE_EQ(clipped->getAt(2).z, 30.0);
  EXPECT_TRUE(std::isnan(clipped->getAt(3).z));
  EXPECT_TRUE(std::isnan(clipped->getAt(4).z));
}

TEST(RingClipperTest, SlopedEdgesAcrossNegativeEnvelope) {
  // Unsorted envelope coordinates are normalized to [-10, -2] x [-8, 0].
  Envelope env(-2.0, -10.0, 0.0, -8.0);
  RingClipper clipper(&env);

  // Diamond-like quad with slopes +-0.5 and +-2.0 crossing all four edges.
  CoordinateSequence quad({CoordinateXY{-12.0, -6.0}, CoordinateXY{-4.0, -10.0},
                           CoordinateXY{0.0, -2.0}, CoordinateXY{-8.0, 2.0},
                           CoordinateXY{-12.0, -6.0}});
  ExpectClipped2D(clipper.clip(&quad).get(), {{-10.0, -7.0},
                                              {-8.0, -8.0},
                                              {-3.0, -8.0},
                                              {-2.0, -6.0},
                                              {-2.0, -1.0},
                                              {-4.0, 0.0},
                                              {-9.0, 0.0},
                                              {-10.0, -2.0},
                                              {-10.0, -7.0}});
}

TEST(RingClipperTest, RepeatedInputVerticesAreDeduplicated) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  RingClipper clipper(&env);

  CoordinateSequence with_repeats(
      {CoordinateXY{2.0, 2.0}, CoordinateXY{2.0, 2.0}, CoordinateXY{8.0, 2.0},
       CoordinateXY{8.0, 8.0}, CoordinateXY{8.0, 8.0}, CoordinateXY{2.0, 8.0},
       CoordinateXY{2.0, 2.0}});
  ExpectClipped2D(clipper.clip(&with_repeats).get(),
                  {{2.0, 2.0}, {8.0, 2.0}, {8.0, 8.0}, {2.0, 8.0}, {2.0, 2.0}});
}

TEST(RingClipperTest, CornerCrossingAndSingleCornerTouch) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  RingClipper clipper(&env);

  // Diagonal edge passes directly through the top-left corner (0, 10).
  CoordinateSequence through_corner(
      {CoordinateXY{-5.0, 5.0}, CoordinateXY{5.0, 5.0}, CoordinateXY{5.0, 15.0},
       CoordinateXY{-5.0, 5.0}});
  ExpectClipped2D(
      clipper.clip(&through_corner).get(),
      {{0.0, 5.0}, {5.0, 5.0}, {5.0, 10.0}, {0.0, 10.0}, {0.0, 5.0}});

  // Triangle grazing only the single corner (0, 10) from the outside clips
  // empty.
  CoordinateSequence outside_corner(
      {CoordinateXY{-5.0, 5.0}, CoordinateXY{5.0, 15.0},
       CoordinateXY{-5.0, 15.0}, CoordinateXY{-5.0, 5.0}});
  ExpectClipped2D(clipper.clip(&outside_corner).get(), {});
}

TEST(RingClipperTest, ClockwiseOversizedRingPreservesOrientationAndCloses) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  RingClipper clipper(&env);

  CoordinateSequence cw_oversized(
      {CoordinateXY{-5.0, -5.0}, CoordinateXY{-5.0, 15.0},
       CoordinateXY{15.0, 15.0}, CoordinateXY{15.0, -5.0},
       CoordinateXY{-5.0, -5.0}});
  ExpectClipped2D(
      clipper.clip(&cw_oversized).get(),
      {{0.0, 10.0}, {10.0, 10.0}, {10.0, 0.0}, {0.0, 0.0}, {0.0, 10.0}});
}

}  // namespace
}  // namespace geos_google
