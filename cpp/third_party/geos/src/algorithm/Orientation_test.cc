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

#include "third_party/geos/include/geos/algorithm/Orientation.h"

#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"

namespace geos_google {
namespace {

using ::geos::algorithm::Orientation;
using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::geom::CoordinateXYM;
using ::geos::geom::CoordinateXYZM;

std::unique_ptr<CoordinateSequence> MakeReversedCopy(
    const CoordinateSequence& seq) {
  std::unique_ptr<CoordinateSequence> copy = seq.clone();
  copy->reverse();
  return copy;
}

TEST(OrientationTest, EnumConstantsMatchTurnAndSideAliases) {
  EXPECT_EQ(+Orientation::CLOCKWISE, -1);
  EXPECT_EQ(+Orientation::RIGHT, -1);
  EXPECT_EQ(+Orientation::COLLINEAR, 0);
  EXPECT_EQ(+Orientation::STRAIGHT, 0);
  EXPECT_EQ(+Orientation::COUNTERCLOCKWISE, 1);
  EXPECT_EQ(+Orientation::LEFT, 1);
}

TEST(OrientationTest, IndexBasicTurnsAndCollinearPositions) {
  // Horizontal reference segment (0,0) -> (10,0).
  const CoordinateXY p1(0.0, 0.0);
  const CoordinateXY p2(10.0, 0.0);

  EXPECT_EQ(Orientation::index(p1, p2, CoordinateXY(5.0, 3.0)),
            +Orientation::COUNTERCLOCKWISE);
  EXPECT_EQ(Orientation::index(p1, p2, CoordinateXY(5.0, -3.0)),
            +Orientation::CLOCKWISE);

  // Collinear before, at endpoints, between, and after.
  EXPECT_EQ(Orientation::index(p1, p2, CoordinateXY(-5.0, 0.0)),
            +Orientation::COLLINEAR);
  EXPECT_EQ(Orientation::index(p1, p2, CoordinateXY(0.0, 0.0)),
            +Orientation::COLLINEAR);
  EXPECT_EQ(Orientation::index(p1, p2, CoordinateXY(5.0, 0.0)),
            +Orientation::COLLINEAR);
  EXPECT_EQ(Orientation::index(p1, p2, CoordinateXY(10.0, 0.0)),
            +Orientation::COLLINEAR);
  EXPECT_EQ(Orientation::index(p1, p2, CoordinateXY(25.0, 0.0)),
            +Orientation::COLLINEAR);

  // Degenerate zero-length direction vector.
  EXPECT_EQ(Orientation::index(p1, p1, CoordinateXY(4.0, 7.0)),
            +Orientation::COLLINEAR);
  EXPECT_EQ(Orientation::index(p1, p1, p1), +Orientation::COLLINEAR);

  // Diagonal reference vector.
  EXPECT_EQ(Orientation::index(CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 2.0),
                               CoordinateXY(1.0, 2.0)),
            +Orientation::LEFT);
  EXPECT_EQ(Orientation::index(CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 2.0),
                               CoordinateXY(2.0, 1.0)),
            +Orientation::RIGHT);
  EXPECT_EQ(Orientation::index(CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 2.0),
                               CoordinateXY(-3.0, -3.0)),
            +Orientation::STRAIGHT);
}

TEST(OrientationTest, IndexExtendedPrecisionNearlyCollinear) {
  // Coordinates whose cross product exceeds 53-bit IEEE-754 double precision.
  const CoordinateXY a(0.0, 0.0);
  const CoordinateXY b(1.0e8, 1.0e8 + 1.0);

  EXPECT_EQ(Orientation::index(a, b, CoordinateXY(2.0e8, 2.0e8 + 2.0)),
            +Orientation::COLLINEAR);
  EXPECT_EQ(Orientation::index(a, b, CoordinateXY(2.0e8, 2.0e8 + 3.0)),
            +Orientation::COUNTERCLOCKWISE);
  EXPECT_EQ(Orientation::index(a, b, CoordinateXY(2.0e8, 2.0e8 + 1.0)),
            +Orientation::CLOCKWISE);
}

TEST(OrientationTest, IsCCWRejectsShortAndFlatRings) {
  const CoordinateSequence empty = CoordinateSequence::XY(0);
  EXPECT_FALSE(Orientation::isCCW(&empty));
  EXPECT_FALSE(Orientation::isCCWArea(&empty));

  const CoordinateSequence one({CoordinateXY(0.0, 0.0)});
  EXPECT_FALSE(Orientation::isCCW(&one));
  EXPECT_FALSE(Orientation::isCCWArea(&one));

  const CoordinateSequence two(
      {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0)});
  EXPECT_FALSE(Orientation::isCCW(&two));
  EXPECT_FALSE(Orientation::isCCWArea(&two));

  // 3-point closed degenerate ring has < 4 coordinates.
  const CoordinateSequence three(
      {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0), CoordinateXY(0.0, 0.0)});
  EXPECT_FALSE(Orientation::isCCW(&three));
  EXPECT_FALSE(Orientation::isCCWArea(&three));

  // Horizontal flat ring with 4 points: no rising segment (iUpHi == 0).
  const CoordinateSequence flat(
      {CoordinateXY(0.0, 5.0), CoordinateXY(10.0, 5.0), CoordinateXY(5.0, 5.0),
       CoordinateXY(0.0, 5.0)});
  EXPECT_FALSE(Orientation::isCCW(&flat));
  EXPECT_FALSE(Orientation::isCCWArea(&flat));
}

TEST(OrientationTest, IsCCWPointedCapTrianglesAndWrapAround) {
  // Pointed cap where high point is at index 2 (nPts - 1), so iDownLow wraps to
  // 0.
  const CoordinateSequence tri_ccw(
      {CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0), CoordinateXY(5.0, 10.0),
       CoordinateXY(0.0, 0.0)});
  EXPECT_TRUE(Orientation::isCCW(&tri_ccw));
  EXPECT_TRUE(Orientation::isCCWArea(&tri_ccw));

  const CoordinateSequence tri_cw_base(
      {CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0), CoordinateXY(5.0, 10.0),
       CoordinateXY(0.0, 0.0)});
  std::unique_ptr<CoordinateSequence> tri_cw = MakeReversedCopy(tri_cw_base);
  EXPECT_FALSE(Orientation::isCCW(tri_cw.get()));
  EXPECT_FALSE(Orientation::isCCWArea(tri_cw.get()));

  // Ring starts and ends at the highest point (iUpHi == nPts).
  const CoordinateSequence start_top_ccw(
      {CoordinateXY(5.0, 10.0), CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0),
       CoordinateXY(5.0, 10.0)});
  EXPECT_TRUE(Orientation::isCCW(&start_top_ccw));
  EXPECT_TRUE(Orientation::isCCWArea(&start_top_ccw));

  const CoordinateSequence start_top_cw(
      {CoordinateXY(5.0, 10.0), CoordinateXY(10.0, 0.0), CoordinateXY(0.0, 0.0),
       CoordinateXY(5.0, 10.0)});
  EXPECT_FALSE(Orientation::isCCW(&start_top_cw));
  EXPECT_FALSE(Orientation::isCCWArea(&start_top_cw));
}

TEST(OrientationTest, IsCCWFlatCapAndCollinearTopSegments) {
  const CoordinateSequence square_ccw(
      {CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0),
       CoordinateXY(10.0, 10.0), CoordinateXY(0.0, 10.0),
       CoordinateXY(0.0, 0.0)});
  EXPECT_TRUE(Orientation::isCCW(&square_ccw));
  EXPECT_TRUE(Orientation::isCCWArea(&square_ccw));

  std::unique_ptr<CoordinateSequence> square_cw = MakeReversedCopy(square_ccw);
  EXPECT_FALSE(Orientation::isCCW(square_cw.get()));
  EXPECT_FALSE(Orientation::isCCWArea(square_cw.get()));

  // Multiple collinear vertices along the flat top edge.
  const CoordinateSequence multi_top_ccw(
      {CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0),
       CoordinateXY(10.0, 10.0), CoordinateXY(7.0, 10.0),
       CoordinateXY(3.0, 10.0), CoordinateXY(0.0, 10.0),
       CoordinateXY(0.0, 0.0)});
  EXPECT_TRUE(Orientation::isCCW(&multi_top_ccw));
  EXPECT_TRUE(Orientation::isCCWArea(&multi_top_ccw));

  std::unique_ptr<CoordinateSequence> multi_top_cw =
      MakeReversedCopy(multi_top_ccw);
  EXPECT_FALSE(Orientation::isCCW(multi_top_cw.get()));
  EXPECT_FALSE(Orientation::isCCWArea(multi_top_cw.get()));

  // Flat top wraps across index 0.
  const CoordinateSequence wrap_top_ccw(
      {CoordinateXY(5.0, 10.0), CoordinateXY(0.0, 10.0), CoordinateXY(0.0, 0.0),
       CoordinateXY(10.0, 0.0), CoordinateXY(10.0, 10.0),
       CoordinateXY(5.0, 10.0)});
  EXPECT_TRUE(Orientation::isCCW(&wrap_top_ccw));
  EXPECT_TRUE(Orientation::isCCWArea(&wrap_top_ccw));

  // Mildly invalid ring with collapsed flat segment along top edge.
  const CoordinateSequence collapsed_top_ccw(
      {CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0),
       CoordinateXY(10.0, 10.0), CoordinateXY(5.0, 10.0),
       CoordinateXY(10.0, 10.0), CoordinateXY(0.0, 10.0),
       CoordinateXY(0.0, 0.0)});
  EXPECT_TRUE(Orientation::isCCW(&collapsed_top_ccw));

  const CoordinateSequence collapsed_top_cw(
      {CoordinateXY(0.0, 0.0), CoordinateXY(0.0, 10.0),
       CoordinateXY(10.0, 10.0), CoordinateXY(5.0, 10.0),
       CoordinateXY(10.0, 10.0), CoordinateXY(10.0, 0.0),
       CoordinateXY(0.0, 0.0)});
  EXPECT_FALSE(Orientation::isCCW(&collapsed_top_cw));
}

TEST(OrientationTest, IsCCWMultiplePeaksAndRepeatedPoints) {
  // First peak lower than second peak (exercises updating iUpHi multiple
  // times).
  const CoordinateSequence rising_ccw(
      {CoordinateXY(0.0, 0.0), CoordinateXY(4.0, 0.0), CoordinateXY(4.0, 3.0),
       CoordinateXY(6.0, 1.0), CoordinateXY(8.0, 10.0),
       CoordinateXY(0.0, 0.0)});
  std::unique_ptr<CoordinateSequence> rising_cw = MakeReversedCopy(rising_ccw);
  EXPECT_TRUE(Orientation::isCCW(&rising_ccw));
  EXPECT_TRUE(Orientation::isCCWArea(&rising_ccw));
  EXPECT_FALSE(Orientation::isCCW(rising_cw.get()));
  EXPECT_FALSE(Orientation::isCCWArea(rising_cw.get()));

  // First peak higher than second peak (second peak does not replace upHiPt).
  const CoordinateSequence falling_ccw(
      {CoordinateXY(0.0, 0.0), CoordinateXY(8.0, 0.0), CoordinateXY(6.0, 10.0),
       CoordinateXY(4.0, 1.0), CoordinateXY(2.0, 5.0), CoordinateXY(0.0, 0.0)});
  std::unique_ptr<CoordinateSequence> falling_cw =
      MakeReversedCopy(falling_ccw);
  EXPECT_TRUE(Orientation::isCCW(&falling_ccw));
  EXPECT_TRUE(Orientation::isCCWArea(&falling_ccw));
  EXPECT_FALSE(Orientation::isCCW(falling_cw.get()));
  EXPECT_FALSE(Orientation::isCCWArea(falling_cw.get()));

  // Equal-height peaks separated by a dip.
  const CoordinateSequence equal_ccw(
      {CoordinateXY(0.0, 0.0), CoordinateXY(8.0, 0.0), CoordinateXY(6.0, 10.0),
       CoordinateXY(4.0, 2.0), CoordinateXY(2.0, 10.0),
       CoordinateXY(0.0, 0.0)});
  std::unique_ptr<CoordinateSequence> equal_cw = MakeReversedCopy(equal_ccw);
  EXPECT_TRUE(Orientation::isCCW(&equal_ccw));
  EXPECT_TRUE(Orientation::isCCWArea(&equal_ccw));
  EXPECT_FALSE(Orientation::isCCW(equal_cw.get()));
  EXPECT_FALSE(Orientation::isCCWArea(equal_cw.get()));

  // Repeated coordinates on top cap.
  const CoordinateSequence repeated_ccw(
      {CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0), CoordinateXY(5.0, 10.0),
       CoordinateXY(5.0, 10.0), CoordinateXY(0.0, 0.0)});
  EXPECT_TRUE(Orientation::isCCW(&repeated_ccw));
}

TEST(OrientationTest, IsCCWDegenerateCapsReturnFalse) {
  // A-B-A spike at the top: upLowPt == downLowPt.
  const CoordinateSequence aba_spike(
      {CoordinateXY(0.0, 0.0), CoordinateXY(5.0, 10.0), CoordinateXY(0.0, 0.0),
       CoordinateXY(10.0, 0.0), CoordinateXY(0.0, 0.0)});
  EXPECT_FALSE(Orientation::isCCW(&aba_spike));

  // Collinear non-equal top segments: orientation index is COLLINEAR.
  const CoordinateSequence collinear_spike(
      {CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 2.0), CoordinateXY(1.0, 1.0),
       CoordinateXY(3.0, 0.0), CoordinateXY(0.0, 0.0)});
  EXPECT_FALSE(Orientation::isCCW(&collinear_spike));
}

TEST(OrientationTest, IsCCWAreaSelfIntersectingBowtie) {
  // Larger CCW triangle (area 8) + smaller CW triangle (area 0.5) -> net CCW.
  const CoordinateSequence bowtie_ccw_dominant(
      {CoordinateXY(0.0, 0.0), CoordinateXY(4.0, 0.0), CoordinateXY(4.0, 4.0),
       CoordinateXY(-1.0, -1.0), CoordinateXY(-1.0, 0.0),
       CoordinateXY(0.0, 0.0)});
  EXPECT_TRUE(Orientation::isCCWArea(&bowtie_ccw_dominant));

  // Reversed bowtie: larger CW triangle + smaller CCW triangle -> net CW.
  std::unique_ptr<CoordinateSequence> bowtie_cw_dominant =
      MakeReversedCopy(bowtie_ccw_dominant);
  EXPECT_FALSE(Orientation::isCCWArea(bowtie_cw_dominant.get()));

  // Equal-area bowtie has signed area 0 -> false.
  const CoordinateSequence bowtie_balanced(
      {CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 0.0), CoordinateXY(2.0, 2.0),
       CoordinateXY(-2.0, -2.0), CoordinateXY(-2.0, 0.0),
       CoordinateXY(0.0, 0.0)});
  EXPECT_FALSE(Orientation::isCCWArea(&bowtie_balanced));
}

TEST(OrientationTest, WorksWithXYZ_XYM_And_XYZMSequences) {
  const CoordinateSequence xyz_ccw(
      {Coordinate(0.0, 0.0, 1.0), Coordinate(4.0, 0.0, 2.0),
       Coordinate(2.0, 3.0, 3.0), Coordinate(0.0, 0.0, 1.0)});
  EXPECT_TRUE(Orientation::isCCW(&xyz_ccw));
  EXPECT_TRUE(Orientation::isCCWArea(&xyz_ccw));

  const CoordinateSequence xym_cw(
      {CoordinateXYM(0.0, 0.0, 10.0), CoordinateXYM(2.0, 3.0, 20.0),
       CoordinateXYM(4.0, 0.0, 30.0), CoordinateXYM(0.0, 0.0, 10.0)});
  EXPECT_FALSE(Orientation::isCCW(&xym_cw));
  EXPECT_FALSE(Orientation::isCCWArea(&xym_cw));

  const CoordinateSequence xyzm_ccw(
      {CoordinateXYZM(0.0, 0.0, 1.0, 10.0), CoordinateXYZM(4.0, 0.0, 2.0, 20.0),
       CoordinateXYZM(4.0, 4.0, 3.0, 30.0), CoordinateXYZM(0.0, 4.0, 4.0, 40.0),
       CoordinateXYZM(0.0, 0.0, 1.0, 10.0)});
  EXPECT_TRUE(Orientation::isCCW(&xyzm_ccw));
  EXPECT_TRUE(Orientation::isCCWArea(&xyzm_ccw));
}

}  // namespace
}  // namespace geos_google
