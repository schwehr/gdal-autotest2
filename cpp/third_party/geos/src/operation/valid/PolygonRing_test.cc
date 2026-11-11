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

// Unit tests for third_party/geos/src/operation/valid/PolygonRing.cpp.

#include "third_party/geos/include/geos/operation/valid/PolygonRing.h"

#include <initializer_list>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/valid/IsValidOp.h"
#include "third_party/geos/include/geos/operation/valid/PolygonTopologyAnalyzer.h"
#include "third_party/geos/include/geos/operation/valid/TopologyValidationError.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::operation::valid::IsValidOp;
using geos::operation::valid::PolygonRing;
using geos::operation::valid::PolygonTopologyAnalyzer;
using geos::operation::valid::TopologyValidationError;

class PolygonRingTest : public ::testing::Test {
 protected:
  // Declared first so the factory outlives any rings/geometries built from it.
  GeometryFactory::Ptr factory_ = GeometryFactory::create();

  std::unique_ptr<LinearRing> MakeRing(
      std::initializer_list<CoordinateXY> coords) const {
    return factory_->createLinearRing(
        std::make_unique<CoordinateSequence>(coords));
  }

  std::unique_ptr<LinearRing> MakeSquareRing(double min_x, double min_y,
                                             double max_x, double max_y) const {
    return MakeRing({{min_x, min_y},
                     {min_x, max_y},
                     {max_x, max_y},
                     {max_x, min_y},
                     {min_x, min_y}});
  }
};

TEST_F(PolygonRingTest, ShellAndHoleIdentityAndSamePolygon) {
  auto shell0_lr = MakeSquareRing(0.0, 0.0, 20.0, 20.0);
  auto hole0_lr = MakeSquareRing(2.0, 2.0, 6.0, 6.0);
  auto hole1_lr = MakeSquareRing(10.0, 10.0, 14.0, 14.0);
  auto shell1_lr = MakeSquareRing(30.0, 0.0, 50.0, 20.0);
  auto hole2_lr = MakeSquareRing(32.0, 2.0, 36.0, 6.0);

  PolygonRing shell0(shell0_lr.get());
  PolygonRing hole0(hole0_lr.get(), 0, &shell0);
  PolygonRing hole1(hole1_lr.get(), 1, &shell0);
  PolygonRing shell1(shell1_lr.get());
  PolygonRing hole2(hole2_lr.get(), 0, &shell1);

  EXPECT_TRUE(PolygonRing::isShell(nullptr));
  EXPECT_TRUE(PolygonRing::isShell(&shell0));
  EXPECT_FALSE(PolygonRing::isShell(&hole0));
  EXPECT_FALSE(PolygonRing::isShell(&hole1));

  EXPECT_TRUE(shell0.isShell());
  EXPECT_FALSE(hole0.isShell());
  EXPECT_FALSE(hole1.isShell());

  EXPECT_TRUE(shell0.isSamePolygon(&shell0));
  EXPECT_TRUE(shell0.isSamePolygon(&hole0));
  EXPECT_TRUE(hole0.isSamePolygon(&shell0));
  EXPECT_TRUE(hole0.isSamePolygon(&hole1));

  EXPECT_FALSE(shell0.isSamePolygon(&shell1));
  EXPECT_FALSE(shell0.isSamePolygon(&hole2));
  EXPECT_FALSE(hole0.isSamePolygon(&shell1));
  EXPECT_FALSE(hole0.isSamePolygon(&hole2));
}

TEST_F(PolygonRingTest, AddTouchIgnoresNullAndCrossPolygonRings) {
  auto shell0_lr = MakeSquareRing(0.0, 0.0, 20.0, 20.0);
  auto hole0_lr = MakeSquareRing(2.0, 0.0, 6.0, 4.0);
  auto shell1_lr = MakeSquareRing(20.0, 0.0, 40.0, 20.0);
  auto hole1_lr = MakeSquareRing(22.0, 0.0, 26.0, 4.0);

  PolygonRing shell0(shell0_lr.get());
  PolygonRing hole0(hole0_lr.get(), 0, &shell0);
  PolygonRing shell1(shell1_lr.get());
  PolygonRing hole1(hole1_lr.get(), 0, &shell1);

  const CoordinateXY p0(20.0, 5.0);
  const CoordinateXY p1(20.0, 10.0);

  EXPECT_FALSE(PolygonRing::addTouch(nullptr, nullptr, p0));
  EXPECT_FALSE(PolygonRing::addTouch(nullptr, &shell0, p0));
  EXPECT_FALSE(PolygonRing::addTouch(&shell0, nullptr, p0));

  // Cross-polygon touches (even at multiple points) must be ignored.
  EXPECT_FALSE(PolygonRing::addTouch(&shell0, &shell1, p0));
  EXPECT_FALSE(PolygonRing::addTouch(&shell0, &shell1, p1));
  EXPECT_FALSE(PolygonRing::addTouch(&shell0, &hole1, p0));
  EXPECT_FALSE(PolygonRing::addTouch(&hole0, &hole1, p0));

  // Touch state within poly 0 remains clean.
  EXPECT_FALSE(PolygonRing::addTouch(&shell0, &hole0, CoordinateXY(4.0, 0.0)));
  EXPECT_EQ(
      PolygonRing::findHoleCycleLocation({&shell0, &hole0, &shell1, &hole1}),
      nullptr);
}

TEST_F(PolygonRingTest, AddTouchSingleDuplicateAndDoubleTouch) {
  auto shell_lr = MakeSquareRing(0.0, 0.0, 20.0, 20.0);
  auto hole0_lr = MakeSquareRing(2.0, 0.0, 6.0, 4.0);
  auto hole1_lr = MakeSquareRing(10.0, 0.0, 14.0, 4.0);
  auto hole2_lr = MakeSquareRing(6.0, 4.0, 10.0, 8.0);

  PolygonRing shell(shell_lr.get());
  PolygonRing hole0(hole0_lr.get(), 0, &shell);
  PolygonRing hole1(hole1_lr.get(), 1, &shell);
  PolygonRing hole2(hole2_lr.get(), 2, &shell);

  const CoordinateXY pt0(4.0, 0.0);
  const CoordinateXY pt1(12.0, 0.0);
  const CoordinateXY pt2(6.0, 4.0);
  const CoordinateXY pt3(10.0, 4.0);

  // First touch between shell and hole0.
  EXPECT_FALSE(PolygonRing::addTouch(&shell, &hole0, pt0));
  // Adding a touch with a different ring when touches map is non-empty.
  EXPECT_FALSE(PolygonRing::addTouch(&shell, &hole1, pt1));
  // Re-recording the exact same touch point (both arg orders) is not a double
  // touch and does not create duplicate map entries.
  EXPECT_FALSE(PolygonRing::addTouch(&shell, &hole0, pt0));
  EXPECT_FALSE(PolygonRing::addTouch(&hole0, &shell, pt0));

  // Touching the same two rings at a different location flags double touch.
  EXPECT_TRUE(PolygonRing::addTouch(&shell, &hole0, pt1));
  EXPECT_TRUE(PolygonRing::addTouch(&hole0, &shell, pt2));

  // Double touch between two holes.
  EXPECT_FALSE(PolygonRing::addTouch(&hole0, &hole2, pt2));
  EXPECT_TRUE(PolygonRing::addTouch(&hole0, &hole2, pt3));
}

TEST_F(PolygonRingTest, AddTouchDetectsAsymmetricSecondRingMismatch) {
  auto shell_lr = MakeSquareRing(0.0, 0.0, 20.0, 20.0);
  auto hole0_lr = MakeSquareRing(2.0, 0.0, 6.0, 4.0);
  auto hole_dup_lr = MakeSquareRing(12.0, 0.0, 16.0, 4.0);

  PolygonRing shell(shell_lr.get());
  PolygonRing hole0(hole0_lr.get(), 0, &shell);
  // Shares id=0 and parent shell, but has an empty touches map so
  // ring0->isOnlyTouch(ring1, pt) is true while ring1->isOnlyTouch(ring0, pt)
  // detects the conflicting id=0 entry on shell.
  PolygonRing hole0_same_id(hole_dup_lr.get(), 0, &shell);

  EXPECT_FALSE(PolygonRing::addTouch(&shell, &hole0, CoordinateXY(4.0, 0.0)));
  EXPECT_TRUE(
      PolygonRing::addTouch(&hole0_same_id, &shell, CoordinateXY(14.0, 0.0)));
}

TEST_F(PolygonRingTest, FindHoleCycleLocationEmptyAndUntouchedRings) {
  EXPECT_EQ(PolygonRing::findHoleCycleLocation({}), nullptr);

  auto shell_lr = MakeSquareRing(0.0, 0.0, 20.0, 20.0);
  auto hole0_lr = MakeSquareRing(2.0, 2.0, 6.0, 6.0);
  auto hole1_lr = MakeSquareRing(10.0, 10.0, 14.0, 14.0);

  PolygonRing shell(shell_lr.get());
  PolygonRing hole0(hole0_lr.get(), 0, &shell);
  PolygonRing hole1(hole1_lr.get(), 1, &shell);

  EXPECT_EQ(PolygonRing::findHoleCycleLocation({&shell}), nullptr);
  EXPECT_EQ(PolygonRing::findHoleCycleLocation({&shell, &hole0, &hole1}),
            nullptr);
}

TEST_F(PolygonRingTest, FindHoleCycleLocationTreesAndSharedPinwheelAreValid) {
  auto shell_lr = MakeSquareRing(0.0, 0.0, 20.0, 20.0);
  auto hole0_lr = MakeSquareRing(2.0, 0.0, 6.0, 4.0);
  auto hole1_lr = MakeSquareRing(6.0, 4.0, 10.0, 8.0);
  auto hole2_lr = MakeSquareRing(10.0, 8.0, 14.0, 12.0);
  auto hole3_lr = MakeSquareRing(15.0, 2.0, 18.0, 5.0);

  // Case 1: Linear chain (tree) + disconnected touch component.
  {
    PolygonRing shell(shell_lr.get());
    PolygonRing hole0(hole0_lr.get(), 0, &shell);
    PolygonRing hole1(hole1_lr.get(), 1, &shell);
    PolygonRing hole2(hole2_lr.get(), 2, &shell);
    PolygonRing hole3(hole3_lr.get(), 3, &shell);

    EXPECT_FALSE(PolygonRing::addTouch(&shell, &hole0, CoordinateXY(4.0, 0.0)));
    EXPECT_FALSE(PolygonRing::addTouch(&hole0, &hole1, CoordinateXY(6.0, 4.0)));
    EXPECT_FALSE(
        PolygonRing::addTouch(&hole1, &hole2, CoordinateXY(10.0, 8.0)));
    // Remaining ring hole3 has no touches; already-marked rings in the chain
    // are skipped in the outer loop via !isInTouchSet().
    EXPECT_EQ(PolygonRing::findHoleCycleLocation(
                  {&shell, &hole0, &hole1, &hole2, &hole3}),
              nullptr);
  }

  // Case 2: Pinwheel / star where shell, hole0, hole1, hole2 all mutually touch
  // at the exact same vertex. Same-point back/sibling edges must be skipped.
  {
    PolygonRing shell(shell_lr.get());
    PolygonRing hole0(hole0_lr.get(), 0, &shell);
    PolygonRing hole1(hole1_lr.get(), 1, &shell);
    PolygonRing hole2(hole2_lr.get(), 2, &shell);

    const CoordinateXY hub(10.0, 0.0);
    EXPECT_FALSE(PolygonRing::addTouch(&shell, &hole0, hub));
    EXPECT_FALSE(PolygonRing::addTouch(&shell, &hole1, hub));
    EXPECT_FALSE(PolygonRing::addTouch(&shell, &hole2, hub));
    EXPECT_FALSE(PolygonRing::addTouch(&hole0, &hole1, hub));
    EXPECT_FALSE(PolygonRing::addTouch(&hole1, &hole2, hub));
    EXPECT_FALSE(PolygonRing::addTouch(&hole0, &hole2, hub));

    EXPECT_EQ(
        PolygonRing::findHoleCycleLocation({&shell, &hole0, &hole1, &hole2}),
        nullptr);
  }

  // Case 3: Two separate multi-ring trees in the same polygon.
  {
    PolygonRing shell(shell_lr.get());
    PolygonRing hole0(hole0_lr.get(), 0, &shell);
    PolygonRing hole1(hole1_lr.get(), 1, &shell);
    PolygonRing hole2(hole2_lr.get(), 2, &shell);
    PolygonRing hole3(hole3_lr.get(), 3, &shell);

    EXPECT_FALSE(PolygonRing::addTouch(&shell, &hole0, CoordinateXY(4.0, 0.0)));
    EXPECT_FALSE(
        PolygonRing::addTouch(&hole1, &hole2, CoordinateXY(10.0, 8.0)));
    EXPECT_FALSE(
        PolygonRing::addTouch(&hole2, &hole3, CoordinateXY(14.0, 10.0)));

    EXPECT_EQ(PolygonRing::findHoleCycleLocation(
                  {&shell, &hole0, &hole1, &hole2, &hole3}),
              nullptr);
  }
}

TEST_F(PolygonRingTest, FindHoleCycleLocationDetectsCycles) {
  auto shell_lr = MakeSquareRing(0.0, 0.0, 20.0, 20.0);
  auto hole0_lr = MakeSquareRing(2.0, 0.0, 6.0, 5.0);
  auto hole1_lr = MakeSquareRing(6.0, 0.0, 10.0, 5.0);
  auto hole2_lr = MakeSquareRing(5.0, 5.0, 8.0, 9.0);

  // Case 1: Shell + 2 holes forming a 3-cycle at distinct points.
  {
    PolygonRing shell(shell_lr.get());
    PolygonRing hole0(hole0_lr.get(), 0, &shell);
    PolygonRing hole1(hole1_lr.get(), 1, &shell);

    const CoordinateXY p0(2.0, 0.0);
    const CoordinateXY p1(6.0, 5.0);
    const CoordinateXY p2(10.0, 0.0);

    EXPECT_FALSE(PolygonRing::addTouch(&shell, &hole0, p0));
    EXPECT_FALSE(PolygonRing::addTouch(&hole0, &hole1, p1));
    EXPECT_FALSE(PolygonRing::addTouch(&hole1, &shell, p2));

    const CoordinateXY* loc =
        PolygonRing::findHoleCycleLocation({&shell, &hole0, &hole1});
    ASSERT_NE(loc, nullptr);
    EXPECT_TRUE(loc->equals2D(p0) || loc->equals2D(p1) || loc->equals2D(p2));
  }

  // Case 2: Pure hole-only cycle when shell has no touches.
  {
    PolygonRing shell(shell_lr.get());
    PolygonRing hole0(hole0_lr.get(), 0, &shell);
    PolygonRing hole1(hole1_lr.get(), 1, &shell);
    PolygonRing hole2(hole2_lr.get(), 2, &shell);

    const CoordinateXY q0(6.0, 3.0);
    const CoordinateXY q1(8.0, 5.0);
    const CoordinateXY q2(5.0, 5.0);

    EXPECT_FALSE(PolygonRing::addTouch(&hole0, &hole1, q0));
    EXPECT_FALSE(PolygonRing::addTouch(&hole1, &hole2, q1));
    EXPECT_FALSE(PolygonRing::addTouch(&hole2, &hole0, q2));

    const CoordinateXY* loc =
        PolygonRing::findHoleCycleLocation({&shell, &hole0, &hole1, &hole2});
    ASSERT_NE(loc, nullptr);
    EXPECT_TRUE(loc->equals2D(q0) || loc->equals2D(q1) || loc->equals2D(q2));
  }

  // Case 3: 4-ring cycle where two branches share an initial touch point at
  // shell and reconverge at hole2.
  {
    PolygonRing shell(shell_lr.get());
    PolygonRing hole0(hole0_lr.get(), 0, &shell);
    PolygonRing hole1(hole1_lr.get(), 1, &shell);
    PolygonRing hole2(hole2_lr.get(), 2, &shell);

    const CoordinateXY shared_p(6.0, 0.0);
    const CoordinateXY p_left(5.0, 5.0);
    const CoordinateXY p_right(8.0, 5.0);

    EXPECT_FALSE(PolygonRing::addTouch(&shell, &hole0, shared_p));
    EXPECT_FALSE(PolygonRing::addTouch(&shell, &hole1, shared_p));
    EXPECT_FALSE(PolygonRing::addTouch(&hole0, &hole2, p_left));
    EXPECT_FALSE(PolygonRing::addTouch(&hole1, &hole2, p_right));

    const CoordinateXY* loc =
        PolygonRing::findHoleCycleLocation({&shell, &hole0, &hole1, &hole2});
    ASSERT_NE(loc, nullptr);
    EXPECT_TRUE(loc->equals2D(p_left) || loc->equals2D(p_right));
  }
}

TEST_F(PolygonRingTest, FindInteriorSelfNodeEmptyAndAllRingOrientations) {
  EXPECT_EQ(PolygonRing::findInteriorSelfNode({}), nullptr);

  // CW shell without self-touches -> nullptr.
  auto plain_shell_lr = MakeSquareRing(0.0, 0.0, 20.0, 20.0);
  PolygonRing plain_shell(plain_shell_lr.get());
  EXPECT_EQ(plain_shell.findInteriorSelfNode(), nullptr);

  // 1. Inverted shell touching itself at (5, 0) to enclose an internal hole.
  // Both CW (interior on right) and CCW (interior on left) orientations treat
  // this self-touch as exterior (valid).
  auto cw_inverted_shell_lr = MakeRing({{0.0, 0.0},
                                        {0.0, 10.0},
                                        {10.0, 10.0},
                                        {10.0, 0.0},
                                        {5.0, 0.0},
                                        {8.0, 5.0},
                                        {2.0, 5.0},
                                        {5.0, 0.0},
                                        {0.0, 0.0}});
  {
    const CoordinateSequence* seq = cw_inverted_shell_lr->getCoordinatesRO();
    PolygonRing cw_shell(cw_inverted_shell_lr.get());
    cw_shell.addSelfTouch(
        seq->getAt<CoordinateXY>(4), &seq->getAt<CoordinateXY>(3),
        &seq->getAt<CoordinateXY>(5), &seq->getAt<CoordinateXY>(6),
        &seq->getAt<CoordinateXY>(8));
    EXPECT_EQ(cw_shell.findInteriorSelfNode(), nullptr);
  }

  auto ccw_inverted_shell_lr = cw_inverted_shell_lr->reverse();
  {
    const CoordinateSequence* seq = ccw_inverted_shell_lr->getCoordinatesRO();
    PolygonRing ccw_shell(ccw_inverted_shell_lr.get());
    ccw_shell.addSelfTouch(
        seq->getAt<CoordinateXY>(1), &seq->getAt<CoordinateXY>(0),
        &seq->getAt<CoordinateXY>(2), &seq->getAt<CoordinateXY>(3),
        &seq->getAt<CoordinateXY>(5));
    EXPECT_EQ(ccw_shell.findInteriorSelfNode(), nullptr);
  }

  // 2. Bow-tie ring touching at (5, 5):
  // - As a shell (CW or CCW): interior self-touch (invalid!).
  // - As a hole (CW or CCW): exterior self-touch splitting hole (valid!).
  auto cw_bowtie_lr = MakeRing({{0.0, 0.0},
                                {0.0, 10.0},
                                {5.0, 5.0},
                                {10.0, 10.0},
                                {10.0, 0.0},
                                {5.0, 5.0},
                                {0.0, 0.0}});
  {
    const CoordinateSequence* seq = cw_bowtie_lr->getCoordinatesRO();
    PolygonRing cw_bowtie_shell(cw_bowtie_lr.get());
    cw_bowtie_shell.addSelfTouch(
        seq->getAt<CoordinateXY>(2), &seq->getAt<CoordinateXY>(1),
        &seq->getAt<CoordinateXY>(3), &seq->getAt<CoordinateXY>(4),
        &seq->getAt<CoordinateXY>(0));
    const CoordinateXY* node = cw_bowtie_shell.findInteriorSelfNode();
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(node->equals2D(CoordinateXY(5.0, 5.0)));

    // Same CW bow-tie ring acting as a CW hole (isShell=false, isCCW=false).
    PolygonRing cw_bowtie_hole(cw_bowtie_lr.get(), 0, &plain_shell);
    cw_bowtie_hole.addSelfTouch(
        seq->getAt<CoordinateXY>(2), &seq->getAt<CoordinateXY>(1),
        &seq->getAt<CoordinateXY>(3), &seq->getAt<CoordinateXY>(4),
        &seq->getAt<CoordinateXY>(0));
    EXPECT_EQ(cw_bowtie_hole.findInteriorSelfNode(), nullptr);
  }

  auto ccw_bowtie_lr = cw_bowtie_lr->reverse();
  {
    const CoordinateSequence* seq = ccw_bowtie_lr->getCoordinatesRO();
    // CCW bow-tie as shell (isShell=true, isCCW=true) -> invalid!
    PolygonRing ccw_bowtie_shell(ccw_bowtie_lr.get());
    ccw_bowtie_shell.addSelfTouch(
        seq->getAt<CoordinateXY>(1), &seq->getAt<CoordinateXY>(0),
        &seq->getAt<CoordinateXY>(2), &seq->getAt<CoordinateXY>(3),
        &seq->getAt<CoordinateXY>(5));
    const CoordinateXY* node = ccw_bowtie_shell.findInteriorSelfNode();
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(node->equals2D(CoordinateXY(5.0, 5.0)));

    // CCW bow-tie as hole (isShell=false, isCCW=true) -> valid!
    PolygonRing ccw_bowtie_hole(ccw_bowtie_lr.get(), 0, &plain_shell);
    ccw_bowtie_hole.addSelfTouch(
        seq->getAt<CoordinateXY>(1), &seq->getAt<CoordinateXY>(0),
        &seq->getAt<CoordinateXY>(2), &seq->getAt<CoordinateXY>(3),
        &seq->getAt<CoordinateXY>(5));
    EXPECT_EQ(ccw_bowtie_hole.findInteriorSelfNode(), nullptr);
  }

  // 3. C-shaped hole pinching at (6, 10) to isolate an island inside the hole:
  // Invalid for both CCW and CW hole orientations.
  auto ccw_c_hole_lr = MakeRing({{2.0, 2.0},
                                 {18.0, 2.0},
                                 {18.0, 18.0},
                                 {2.0, 18.0},
                                 {2.0, 12.0},
                                 {6.0, 10.0},
                                 {6.0, 15.0},
                                 {14.0, 15.0},
                                 {14.0, 5.0},
                                 {6.0, 5.0},
                                 {6.0, 10.0},
                                 {2.0, 8.0},
                                 {2.0, 2.0}});
  {
    const CoordinateSequence* seq = ccw_c_hole_lr->getCoordinatesRO();
    PolygonRing ccw_hole(ccw_c_hole_lr.get(), 0, &plain_shell);
    ccw_hole.addSelfTouch(
        seq->getAt<CoordinateXY>(5), &seq->getAt<CoordinateXY>(4),
        &seq->getAt<CoordinateXY>(6), &seq->getAt<CoordinateXY>(9),
        &seq->getAt<CoordinateXY>(11));
    const CoordinateXY* node = ccw_hole.findInteriorSelfNode();
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(node->equals2D(CoordinateXY(6.0, 10.0)));
  }

  auto cw_c_hole_lr = ccw_c_hole_lr->reverse();
  {
    const CoordinateSequence* seq = cw_c_hole_lr->getCoordinatesRO();
    PolygonRing cw_hole(cw_c_hole_lr.get(), 1, &plain_shell);
    cw_hole.addSelfTouch(
        seq->getAt<CoordinateXY>(2), &seq->getAt<CoordinateXY>(1),
        &seq->getAt<CoordinateXY>(3), &seq->getAt<CoordinateXY>(6),
        &seq->getAt<CoordinateXY>(8));
    const CoordinateXY* node = cw_hole.findInteriorSelfNode();
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(node->equals2D(CoordinateXY(6.0, 10.0)));
  }
}

TEST_F(PolygonRingTest, FindInteriorSelfNodeMultipleNodesAndRingVector) {
  auto shell_lr = MakeSquareRing(0.0, 0.0, 20.0, 20.0);
  auto ext_self_lr = MakeRing({{0.0, 0.0},
                               {0.0, 10.0},
                               {10.0, 10.0},
                               {10.0, 0.0},
                               {5.0, 0.0},
                               {8.0, 5.0},
                               {2.0, 5.0},
                               {5.0, 0.0},
                               {0.0, 0.0}});
  auto bowtie_lr = MakeRing({{0.0, 0.0},
                             {0.0, 10.0},
                             {5.0, 5.0},
                             {10.0, 10.0},
                             {10.0, 0.0},
                             {5.0, 5.0},
                             {0.0, 0.0}});

  PolygonRing shell_no_self(shell_lr.get());

  // Ring with an exterior self-node only.
  const CoordinateSequence* ext_seq = ext_self_lr->getCoordinatesRO();
  PolygonRing ext_only_ring(ext_self_lr.get());
  ext_only_ring.addSelfTouch(
      ext_seq->getAt<CoordinateXY>(4), &ext_seq->getAt<CoordinateXY>(3),
      &ext_seq->getAt<CoordinateXY>(5), &ext_seq->getAt<CoordinateXY>(6),
      &ext_seq->getAt<CoordinateXY>(8));
  EXPECT_EQ(PolygonRing::findInteriorSelfNode({&shell_no_self, &ext_only_ring}),
            nullptr);

  // Ring with both an exterior self-node and an interior self-node: must skip
  // the exterior one and return the interior one.
  const CoordinateSequence* bow_seq = bowtie_lr->getCoordinatesRO();
  PolygonRing mixed_ring(bowtie_lr.get());
  mixed_ring.addSelfTouch(
      ext_seq->getAt<CoordinateXY>(4), &ext_seq->getAt<CoordinateXY>(3),
      &ext_seq->getAt<CoordinateXY>(5), &ext_seq->getAt<CoordinateXY>(6),
      &ext_seq->getAt<CoordinateXY>(8));
  mixed_ring.addSelfTouch(
      bow_seq->getAt<CoordinateXY>(2), &bow_seq->getAt<CoordinateXY>(1),
      &bow_seq->getAt<CoordinateXY>(3), &bow_seq->getAt<CoordinateXY>(4),
      &bow_seq->getAt<CoordinateXY>(0));

  const CoordinateXY* found = PolygonRing::findInteriorSelfNode(
      {&shell_no_self, &ext_only_ring, &mixed_ring});
  ASSERT_NE(found, nullptr);
  EXPECT_TRUE(found->equals2D(CoordinateXY(5.0, 5.0)));
}

TEST_F(PolygonRingTest, EndToEndPolygonTopologyAndValidation) {
  WKTReader reader(*factory_);

  // Inverted shell forming a hole at (5, 0): valid only when
  // setSelfTouchingRingFormingHoleValid(true) is enabled.
  {
    auto poly = reader.read<Polygon>(
        "POLYGON ((0 0, 0 10, 10 10, 10 0, 5 0, 8 5, 2 5, 5 0, 0 0))");
    IsValidOp ogc_op(poly.get());
    EXPECT_FALSE(ogc_op.isValid());
    ASSERT_NE(ogc_op.getValidationError(), nullptr);
    EXPECT_EQ(ogc_op.getValidationError()->getErrorType(),
              TopologyValidationError::eRingSelfIntersection);

    IsValidOp esri_op(poly.get());
    esri_op.setSelfTouchingRingFormingHoleValid(true);
    EXPECT_TRUE(esri_op.isValid());
  }

  // Bow-tie shell self-touching at (5, 5) disconnects interior even when
  // self-touching rings forming holes are allowed.
  {
    auto poly = reader.read<Polygon>(
        "POLYGON ((0 0, 0 10, 5 5, 10 10, 10 0, 5 5, 0 0))");
    IsValidOp esri_op(poly.get());
    esri_op.setSelfTouchingRingFormingHoleValid(true);
    EXPECT_FALSE(esri_op.isValid());
    const TopologyValidationError* err = esri_op.getValidationError();
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->getErrorType(),
              TopologyValidationError::eDisconnectedInterior);
    EXPECT_TRUE(err->getCoordinate().equals2D(CoordinateXY(5.0, 5.0)));
  }

  // C-shaped hole self-touching at (6, 10) traps an island in the hole.
  {
    auto poly = reader.read<Polygon>(
        "POLYGON ((0 0, 0 20, 20 20, 20 0, 0 0), "
        "(2 2, 18 2, 18 18, 2 18, 2 12, 6 10, 6 15, 14 15, 14 5, 6 5, 6 10, "
        "2 8, 2 2))");
    IsValidOp esri_op(poly.get());
    esri_op.setSelfTouchingRingFormingHoleValid(true);
    EXPECT_FALSE(esri_op.isValid());
    const TopologyValidationError* err = esri_op.getValidationError();
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->getErrorType(),
              TopologyValidationError::eDisconnectedInterior);
    EXPECT_TRUE(err->getCoordinate().equals2D(CoordinateXY(6.0, 10.0)));
  }

  // Exverted double-hole self-touching at (10, 10) is valid under ESRI mode.
  {
    auto poly = reader.read<Polygon>(
        "POLYGON ((0 0, 0 20, 20 20, 20 0, 0 0), "
        "(4 4, 10 10, 16 4, 16 16, 10 10, 4 16, 4 4))");
    IsValidOp esri_op(poly.get());
    esri_op.setSelfTouchingRingFormingHoleValid(true);
    EXPECT_TRUE(esri_op.isValid());
  }

  // Hole cycle shell(2,0)-hole0(5,5)-hole1(8,0)-shell disconnects interior.
  {
    auto poly = reader.read<Polygon>(
        "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), "
        "(2 0, 5 5, 2 5, 2 0), "
        "(8 0, 8 5, 5 5, 8 0))");
    PolygonTopologyAnalyzer analyzer(poly.get(), false);
    EXPECT_FALSE(analyzer.hasInvalidIntersection());
    EXPECT_TRUE(analyzer.isInteriorDisconnected());
    const CoordinateXY& pt = analyzer.getDisconnectionLocation();
    EXPECT_TRUE(pt.equals2D(CoordinateXY(2.0, 0.0)) ||
                pt.equals2D(CoordinateXY(5.0, 5.0)) ||
                pt.equals2D(CoordinateXY(8.0, 0.0)));
  }

  // Two holes both touching the shell and each other at the single point (5, 0)
  // do not form a hole cycle.
  {
    auto poly = reader.read<Polygon>(
        "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), "
        "(5 0, 4 5, 2 5, 5 0), "
        "(5 0, 8 5, 6 5, 5 0))");
    PolygonTopologyAnalyzer analyzer(poly.get(), false);
    EXPECT_FALSE(analyzer.hasInvalidIntersection());
    EXPECT_FALSE(analyzer.isInteriorDisconnected());
  }
}

}  // namespace
}  // namespace geos_google
