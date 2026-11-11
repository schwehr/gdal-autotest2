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

#include "third_party/geos/include/geos/operation/buffer/OffsetSegmentGenerator.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <numbers>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/Orientation.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/LineSegment.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/operation/buffer/BufferParameters.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using ::geos::algorithm::Orientation;
using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::geom::LineSegment;
using ::geos::geom::Position;
using ::geos::geom::PrecisionModel;
using ::geos::operation::buffer::BufferParameters;
using ::geos::operation::buffer::OffsetSegmentGenerator;

void ExpectCoordNear(const CoordinateXY& actual, double expected_x,
                     double expected_y, double tol = 1e-9) {
  EXPECT_NEAR(actual.x, expected_x, tol);
  EXPECT_NEAR(actual.y, expected_y, tol);
}

std::unique_ptr<CoordinateSequence> RunTwoSegmentCorner(
    const PrecisionModel& pm, const BufferParameters& params, double distance,
    const Coordinate& p0, const Coordinate& p1, const Coordinate& p2, int side,
    bool add_start_point) {
  OffsetSegmentGenerator gen(&pm, params, distance);
  gen.initSideSegments(p0, p1, side);
  gen.addFirstSegment();
  gen.addNextSegment(p2, add_start_point);
  gen.addLastSegment();
  return gen.getCoordinates();
}

TEST(OffsetSegmentGeneratorTest, ComputeOffsetSegmentLeftRightAndNegative) {
  LineSegment horizontal(0.0, 0.0, 10.0, 0.0);
  LineSegment offset;

  OffsetSegmentGenerator::computeOffsetSegment(horizontal, Position::LEFT, 2.5,
                                               offset);
  ExpectCoordNear(offset.p0, 0.0, 2.5);
  ExpectCoordNear(offset.p1, 10.0, 2.5);

  OffsetSegmentGenerator::computeOffsetSegment(horizontal, Position::RIGHT, 2.5,
                                               offset);
  ExpectCoordNear(offset.p0, 0.0, -2.5);
  ExpectCoordNear(offset.p1, 10.0, -2.5);

  // Negative offset flips the effective side.
  OffsetSegmentGenerator::computeOffsetSegment(horizontal, Position::LEFT, -2.0,
                                               offset);
  ExpectCoordNear(offset.p0, 0.0, -2.0);
  ExpectCoordNear(offset.p1, 10.0, -2.0);

  LineSegment vertical(0.0, 0.0, 0.0, 10.0);
  OffsetSegmentGenerator::computeOffsetSegment(vertical, Position::LEFT, 3.0,
                                               offset);
  ExpectCoordNear(offset.p0, -3.0, 0.0);
  ExpectCoordNear(offset.p1, -3.0, 10.0);

  OffsetSegmentGenerator::computeOffsetSegment(vertical, Position::RIGHT, 3.0,
                                               offset);
  ExpectCoordNear(offset.p0, 3.0, 0.0);
  ExpectCoordNear(offset.p1, 3.0, 10.0);

  // 3-4-5 diagonal segment has unit vector (0.6, 0.8), left normal (-0.8, 0.6).
  LineSegment diagonal(0.0, 0.0, 3.0, 4.0);
  OffsetSegmentGenerator::computeOffsetSegment(diagonal, Position::LEFT, 5.0,
                                               offset);
  ExpectCoordNear(offset.p0, -4.0, 3.0);
  ExpectCoordNear(offset.p1, -1.0, 7.0);

  OffsetSegmentGenerator::computeOffsetSegment(diagonal, Position::RIGHT, 5.0,
                                               offset);
  ExpectCoordNear(offset.p0, 4.0, -3.0);
  ExpectCoordNear(offset.p1, 7.0, 1.0);
}

TEST(OffsetSegmentGeneratorTest, EmptyGeneratorAndVectorOverload) {
  PrecisionModel pm(PrecisionModel::FLOATING);
  BufferParameters params;
  OffsetSegmentGenerator gen(&pm, params, 1.0);
  EXPECT_FALSE(gen.hasNarrowConcaveAngle());

  gen.closeRing();
  std::unique_ptr<CoordinateSequence> empty_seq = gen.getCoordinates();
  ASSERT_NE(empty_seq, nullptr);
  EXPECT_EQ(empty_seq->size(), 0U);

  // Exercise the vector<CoordinateSequence*>& ownership transfer overload.
  OffsetSegmentGenerator gen_vec(&pm, params, 2.0);
  gen_vec.createSquare(Coordinate(1.0, 2.0), 2.0);
  std::vector<CoordinateSequence*> out;
  gen_vec.getCoordinates(out);
  ASSERT_EQ(out.size(), 1U);
  std::unique_ptr<CoordinateSequence> owned(out[0]);
  ASSERT_NE(owned, nullptr);
  EXPECT_EQ(owned->size(), 5U);
  EXPECT_TRUE(owned->isRing());
}

TEST(OffsetSegmentGeneratorTest, NegativeQuadrantSegmentsClampsToOne) {
  PrecisionModel pm(PrecisionModel::FLOATING);
  // Negative quadrantSegments exercises the quadSegs < 1 clamp without zero
  // div.
  BufferParameters params(-2);
  OffsetSegmentGenerator gen(&pm, params, 3.0);
  gen.createCircle(Coordinate(1.0, 2.0), 3.0);
  std::unique_ptr<CoordinateSequence> seq = gen.getCoordinates();
  ASSERT_EQ(seq->size(), 5U);
  EXPECT_TRUE(seq->isRing());
  ExpectCoordNear(seq->getAt(0), 4.0, 2.0);
  ExpectCoordNear(seq->getAt(1), 1.0, -1.0);
  ExpectCoordNear(seq->getAt(2), -2.0, 2.0);
  ExpectCoordNear(seq->getAt(3), 1.0, 5.0);
  ExpectCoordNear(seq->getAt(4), 4.0, 2.0);
}

TEST(OffsetSegmentGeneratorTest, CreateCircleAndCreateSquare) {
  PrecisionModel pm(PrecisionModel::FLOATING);
  BufferParameters params(8);

  {
    OffsetSegmentGenerator gen(&pm, params, 5.0);
    gen.createCircle(Coordinate(10.0, 20.0), 5.0);
    std::unique_ptr<CoordinateSequence> circle = gen.getCoordinates();
    ASSERT_EQ(circle->size(), 33U);
    EXPECT_TRUE(circle->isRing());
    EXPECT_FALSE(Orientation::isCCW(circle.get()));

    const CoordinateXY center(10.0, 20.0);
    for (std::size_t i = 0; i < circle->size(); ++i) {
      EXPECT_NEAR(circle->getAt(i).distance(center), 5.0, 1e-9);
    }
    ExpectCoordNear(circle->getAt(0), 15.0, 20.0);
    ExpectCoordNear(circle->getAt(8), 10.0, 15.0);
    ExpectCoordNear(circle->getAt(16), 5.0, 20.0);
    ExpectCoordNear(circle->getAt(24), 10.0, 25.0);
  }

  {
    OffsetSegmentGenerator gen(&pm, params, 4.0);
    gen.createSquare(Coordinate(10.0, 20.0), 4.0);
    std::unique_ptr<CoordinateSequence> square = gen.getCoordinates();
    ASSERT_EQ(square->size(), 5U);
    EXPECT_TRUE(square->isRing());
    ExpectCoordNear(square->getAt(0), 14.0, 24.0);
    ExpectCoordNear(square->getAt(1), 14.0, 16.0);
    ExpectCoordNear(square->getAt(2), 6.0, 16.0);
    ExpectCoordNear(square->getAt(3), 6.0, 24.0);
    ExpectCoordNear(square->getAt(4), 14.0, 24.0);
  }
}

TEST(OffsetSegmentGeneratorTest, AddLineEndCapAllStyles) {
  PrecisionModel pm(PrecisionModel::FLOATING);

  {
    BufferParameters params(4, BufferParameters::CAP_FLAT);
    OffsetSegmentGenerator gen(&pm, params, 2.0);
    gen.addLineEndCap(Coordinate(0.0, 0.0), Coordinate(10.0, 0.0));
    std::unique_ptr<CoordinateSequence> seq = gen.getCoordinates();
    ASSERT_EQ(seq->size(), 3U);
    ExpectCoordNear(seq->getAt(0), 10.0, 2.0);
    ExpectCoordNear(seq->getAt(1), 10.0, -2.0);
    ExpectCoordNear(seq->getAt(2), 10.0, 2.0);
  }

  {
    BufferParameters params(4, BufferParameters::CAP_SQUARE);
    OffsetSegmentGenerator gen(&pm, params, 2.0);
    gen.addLineEndCap(Coordinate(0.0, 0.0), Coordinate(10.0, 0.0));
    std::unique_ptr<CoordinateSequence> seq = gen.getCoordinates();
    ASSERT_EQ(seq->size(), 3U);
    ExpectCoordNear(seq->getAt(0), 12.0, 2.0);
    ExpectCoordNear(seq->getAt(1), 12.0, -2.0);
    ExpectCoordNear(seq->getAt(2), 12.0, 2.0);
  }

  {
    BufferParameters params(4, BufferParameters::CAP_SQUARE);
    OffsetSegmentGenerator gen(&pm, params, 2.0);
    gen.addLineEndCap(Coordinate(0.0, 0.0), Coordinate(0.0, 10.0));
    std::unique_ptr<CoordinateSequence> seq = gen.getCoordinates();
    ASSERT_EQ(seq->size(), 3U);
    ExpectCoordNear(seq->getAt(0), -2.0, 12.0);
    ExpectCoordNear(seq->getAt(1), 2.0, 12.0);
  }

  {
    BufferParameters params(4, BufferParameters::CAP_ROUND);
    OffsetSegmentGenerator gen(&pm, params, 2.0);
    gen.addLineEndCap(Coordinate(0.0, 0.0), Coordinate(10.0, 0.0));
    std::unique_ptr<CoordinateSequence> seq = gen.getCoordinates();
    ASSERT_EQ(seq->size(), 10U);
    EXPECT_TRUE(seq->isRing());
    const CoordinateXY tip(10.0, 0.0);
    for (std::size_t i = 0; i < seq->size(); ++i) {
      EXPECT_NEAR(seq->getAt(i).distance(tip), 2.0, 1e-9);
    }
    ExpectCoordNear(seq->getAt(0), 10.0, 2.0);
    ExpectCoordNear(seq->getAt(4), 12.0, 0.0);
    ExpectCoordNear(seq->getAt(8), 10.0, -2.0);
  }

  // Diagonal 3-4-5 segment with CAP_SQUARE verifies trig rotation of cap.
  {
    BufferParameters params(4, BufferParameters::CAP_SQUARE);
    OffsetSegmentGenerator gen(&pm, params, 5.0);
    gen.addLineEndCap(Coordinate(0.0, 0.0), Coordinate(3.0, 4.0));
    std::unique_ptr<CoordinateSequence> seq = gen.getCoordinates();
    ASSERT_EQ(seq->size(), 3U);
    ExpectCoordNear(seq->getAt(0), 2.0, 11.0);
    ExpectCoordNear(seq->getAt(1), 10.0, 5.0);
    ExpectCoordNear(seq->getAt(2), 2.0, 11.0);
  }
}

TEST(OffsetSegmentGeneratorTest, AddSegmentsForwardReverseAndFixedPrecision) {
  PrecisionModel floating_pm(PrecisionModel::FLOATING);
  BufferParameters params;
  CoordinateSequence input(
      {Coordinate(0.0, 0.0), Coordinate(3.0, 0.0), Coordinate(3.0, 4.0)});

  {
    OffsetSegmentGenerator gen(&floating_pm, params, 1.0);
    gen.addSegments(input, true);
    gen.closeRing();
    std::unique_ptr<CoordinateSequence> seq = gen.getCoordinates();
    ASSERT_EQ(seq->size(), 4U);
    ExpectCoordNear(seq->getAt(0), 0.0, 0.0);
    ExpectCoordNear(seq->getAt(1), 3.0, 0.0);
    ExpectCoordNear(seq->getAt(2), 3.0, 4.0);
    ExpectCoordNear(seq->getAt(3), 0.0, 0.0);
  }

  {
    OffsetSegmentGenerator gen(&floating_pm, params, 1.0);
    gen.addSegments(input, false);
    std::unique_ptr<CoordinateSequence> seq = gen.getCoordinates();
    ASSERT_EQ(seq->size(), 4U);
    ExpectCoordNear(seq->getAt(0), 3.0, 4.0);
    ExpectCoordNear(seq->getAt(1), 3.0, 0.0);
    ExpectCoordNear(seq->getAt(2), 0.0, 0.0);
    ExpectCoordNear(seq->getAt(3), 3.0, 4.0);
  }

  // Fixed precision model rounds added points and snaps near-duplicates.
  PrecisionModel fixed_pm(10.0);
  CoordinateSequence noisy({Coordinate(1.23, 4.56),
                            Coordinate(1.23001, 4.56001),
                            Coordinate(5.67, 8.91)});
  OffsetSegmentGenerator gen(&fixed_pm, params, 1.0);
  gen.addSegments(noisy, true);
  std::unique_ptr<CoordinateSequence> seq = gen.getCoordinates();
  ASSERT_EQ(seq->size(), 3U);
  ExpectCoordNear(seq->getAt(0), 1.2, 4.6);
  ExpectCoordNear(seq->getAt(1), 5.7, 8.9);
  ExpectCoordNear(seq->getAt(2), 1.2, 4.6);
}

TEST(OffsetSegmentGeneratorTest, AddNextSegmentDuplicateAndNonFiniteInput) {
  PrecisionModel pm(PrecisionModel::FLOATING);
  BufferParameters params;

  {
    OffsetSegmentGenerator gen(&pm, params, 1.0);
    gen.initSideSegments(Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                         Position::LEFT);
    gen.addFirstSegment();
    gen.addNextSegment(Coordinate(10.0, 0.0), true);  // Duplicate s2: no-op.
    gen.addNextSegment(Coordinate(20.0, 0.0), true);
    gen.addLastSegment();
    std::unique_ptr<CoordinateSequence> seq = gen.getCoordinates();
    ASSERT_EQ(seq->size(), 3U);
    ExpectCoordNear(seq->getAt(0), 0.0, 1.0);
    ExpectCoordNear(seq->getAt(1), 20.0, 1.0);
    ExpectCoordNear(seq->getAt(2), 0.0, 1.0);
  }

  {
    OffsetSegmentGenerator gen(&pm, params, 1.0);
    gen.initSideSegments(Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                         Position::LEFT);
    EXPECT_THROW(
        gen.addNextSegment(Coordinate(::geos::DoubleNotANumber, 0.0), true),
        ::geos::util::IllegalArgumentException);
  }

  {
    OffsetSegmentGenerator gen(&pm, params, 1.0);
    gen.initSideSegments(Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                         Position::LEFT);
    EXPECT_THROW(
        gen.addNextSegment(Coordinate(0.0, ::geos::DoubleInfinity), true),
        ::geos::util::IllegalArgumentException);
  }
}

TEST(OffsetSegmentGeneratorTest, CollinearSameDirectionAndReversingAllJoins) {
  PrecisionModel pm(PrecisionModel::FLOATING);

  // Same-direction collinear: no midpoint emitted at (5, 1).
  {
    BufferParameters params(4, BufferParameters::CAP_ROUND,
                            BufferParameters::JOIN_ROUND, 5.0);
    std::unique_ptr<CoordinateSequence> seq = RunTwoSegmentCorner(
        pm, params, 1.0, Coordinate(0.0, 0.0), Coordinate(5.0, 0.0),
        Coordinate(10.0, 0.0), Position::LEFT, true);
    ASSERT_EQ(seq->size(), 3U);
    ExpectCoordNear(seq->getAt(0), 0.0, 1.0);
    ExpectCoordNear(seq->getAt(1), 10.0, 1.0);
    ExpectCoordNear(seq->getAt(2), 0.0, 1.0);
  }

  // Reversing collinear with JOIN_BEVEL and JOIN_MITRE for addStartPoint
  // true and false.
  for (BufferParameters::JoinStyle style :
       {BufferParameters::JOIN_BEVEL, BufferParameters::JOIN_MITRE}) {
    BufferParameters params(4, BufferParameters::CAP_FLAT, style, 5.0);
    std::unique_ptr<CoordinateSequence> seq_with_start = RunTwoSegmentCorner(
        pm, params, 1.0, Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
        Coordinate(0.0, 0.0), Position::LEFT, true);
    ASSERT_EQ(seq_with_start->size(), 5U);
    ExpectCoordNear(seq_with_start->getAt(0), 0.0, 1.0);
    ExpectCoordNear(seq_with_start->getAt(1), 10.0, 1.0);
    ExpectCoordNear(seq_with_start->getAt(2), 10.0, -1.0);
    ExpectCoordNear(seq_with_start->getAt(3), 0.0, -1.0);
    ExpectCoordNear(seq_with_start->getAt(4), 0.0, 1.0);

    std::unique_ptr<CoordinateSequence> seq_without_start = RunTwoSegmentCorner(
        pm, params, 1.0, Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
        Coordinate(5.0, 0.0), Position::LEFT, false);
    ASSERT_EQ(seq_without_start->size(), 4U);
    ExpectCoordNear(seq_without_start->getAt(0), 0.0, 1.0);
    ExpectCoordNear(seq_without_start->getAt(1), 10.0, -1.0);
    ExpectCoordNear(seq_without_start->getAt(2), 5.0, -1.0);
    ExpectCoordNear(seq_without_start->getAt(3), 0.0, 1.0);
  }

  // Reversing collinear with JOIN_ROUND: startAngle > endAngle (+PI/2 to
  // -PI/2).
  {
    BufferParameters params(4, BufferParameters::CAP_ROUND,
                            BufferParameters::JOIN_ROUND, 5.0);
    std::unique_ptr<CoordinateSequence> seq = RunTwoSegmentCorner(
        pm, params, 1.0, Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
        Coordinate(0.0, 0.0), Position::LEFT, true);
    ASSERT_EQ(seq->size(), 12U);
    ExpectCoordNear(seq->getAt(0), 0.0, 1.0);
    ExpectCoordNear(seq->getAt(1), 10.0, 1.0);
    ExpectCoordNear(seq->getAt(5), 11.0, 0.0);
    ExpectCoordNear(seq->getAt(9), 10.0, -1.0);
    ExpectCoordNear(seq->getAt(10), 0.0, -1.0);
  }

  // Reversing collinear going left->right->left wraps startAngle <= endAngle
  // (-PI/2 to +PI/2) in CW addDirectedFillet.
  {
    BufferParameters params(4, BufferParameters::CAP_ROUND,
                            BufferParameters::JOIN_ROUND, 5.0);
    std::unique_ptr<CoordinateSequence> seq = RunTwoSegmentCorner(
        pm, params, 1.0, Coordinate(10.0, 0.0), Coordinate(0.0, 0.0),
        Coordinate(10.0, 0.0), Position::LEFT, true);
    ASSERT_EQ(seq->size(), 12U);
    ExpectCoordNear(seq->getAt(0), 10.0, -1.0);
    ExpectCoordNear(seq->getAt(1), 0.0, -1.0);
    ExpectCoordNear(seq->getAt(5), -1.0, 0.0);
    ExpectCoordNear(seq->getAt(9), 0.0, 1.0);
    ExpectCoordNear(seq->getAt(10), 10.0, 1.0);
  }
}

TEST(OffsetSegmentGeneratorTest, OutsideTurnSmallSeparationAndSmallFillet) {
  PrecisionModel pm(PrecisionModel::FLOATING);

  // Near-parallel outside turn (< OFFSET_SEGMENT_SEPARATION_FACTOR * distance).
  for (BufferParameters::JoinStyle style :
       {BufferParameters::JOIN_ROUND, BufferParameters::JOIN_BEVEL,
        BufferParameters::JOIN_MITRE}) {
    BufferParameters params(4, BufferParameters::CAP_ROUND, style, 5.0);
    std::unique_ptr<CoordinateSequence> seq = RunTwoSegmentCorner(
        pm, params, 1.0, Coordinate(0.0, 0.0), Coordinate(10000.0, 0.0),
        Coordinate(20000.0, -1.0), Position::LEFT, true);
    ASSERT_EQ(seq->size(), 4U);
    ExpectCoordNear(seq->getAt(0), 0.0, 1.0);
    ExpectCoordNear(seq->getAt(1), 10000.0, 1.0);
  }

  // Outside turn wider than separation snap, but smaller than 0.5 *
  // filletAngleQuantum: triggers nSegs < 1 early return in addDirectedFillet.
  {
    BufferParameters params(4, BufferParameters::CAP_ROUND,
                            BufferParameters::JOIN_ROUND, 5.0);
    std::unique_ptr<CoordinateSequence> seq = RunTwoSegmentCorner(
        pm, params, 1.0, Coordinate(0.0, 0.0), Coordinate(100.0, 0.0),
        Coordinate(200.0, -10.0), Position::LEFT, false);
    ASSERT_EQ(seq->size(), 5U);
    ExpectCoordNear(seq->getAt(0), 0.0, 1.0);
    ExpectCoordNear(seq->getAt(1), 100.0, 1.0);
    ExpectCoordNear(seq->getAt(2), 100.0 + 10.0 / std::sqrt(10100.0),
                    100.0 / std::sqrt(10100.0));
  }
}

TEST(OffsetSegmentGeneratorTest, OutsideTurnRoundCwAndCcwBothSides) {
  PrecisionModel pm(PrecisionModel::FLOATING);

  // CW outside turn (Position::LEFT), addStartPoint=true and false.
  for (bool add_start : {true, false}) {
    BufferParameters params(2, BufferParameters::CAP_ROUND,
                            BufferParameters::JOIN_ROUND, 5.0);
    std::unique_ptr<CoordinateSequence> seq = RunTwoSegmentCorner(
        pm, params, 2.0, Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
        Coordinate(10.0, -10.0), Position::LEFT, add_start);
    ASSERT_EQ(seq->size(), 6U);
    ExpectCoordNear(seq->getAt(0), 0.0, 2.0);
    ExpectCoordNear(seq->getAt(1), 10.0, 2.0);
    ExpectCoordNear(seq->getAt(2), 10.0 + std::numbers::sqrt2,
                    std::numbers::sqrt2);
    ExpectCoordNear(seq->getAt(3), 12.0, 0.0);
    ExpectCoordNear(seq->getAt(4), 12.0, -10.0);
    ExpectCoordNear(seq->getAt(5), 0.0, 2.0);
  }

  // CCW outside turn (Position::RIGHT) with startAngle < endAngle (-PI/2 to 0),
  // addStartPoint=true and false.
  for (bool add_start : {true, false}) {
    BufferParameters params(2, BufferParameters::CAP_ROUND,
                            BufferParameters::JOIN_ROUND, 5.0);
    std::unique_ptr<CoordinateSequence> seq = RunTwoSegmentCorner(
        pm, params, 2.0, Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
        Coordinate(10.0, 10.0), Position::RIGHT, add_start);
    ASSERT_EQ(seq->size(), 6U);
    ExpectCoordNear(seq->getAt(0), 0.0, -2.0);
    ExpectCoordNear(seq->getAt(1), 10.0, -2.0);
    ExpectCoordNear(seq->getAt(2), 10.0 + std::numbers::sqrt2,
                    -std::numbers::sqrt2);
    ExpectCoordNear(seq->getAt(3), 12.0, 0.0);
    ExpectCoordNear(seq->getAt(4), 12.0, 10.0);
    ExpectCoordNear(seq->getAt(5), 0.0, -2.0);
  }

  // CCW outside turn (Position::RIGHT) wrapping startAngle >= endAngle
  // (+3*PI/4 to -3*PI/4) across +/-PI with an exact midpoint at (-2, 0).
  {
    BufferParameters params(4, BufferParameters::CAP_ROUND,
                            BufferParameters::JOIN_ROUND, 5.0);
    std::unique_ptr<CoordinateSequence> seq = RunTwoSegmentCorner(
        pm, params, 2.0, Coordinate(1.0, 1.0), Coordinate(0.0, 0.0),
        Coordinate(1.0, -1.0), Position::RIGHT, true);
    ASSERT_EQ(seq->size(), 8U);
    const double pi_over_8 = std::numbers::pi / 8.0;
    ExpectCoordNear(seq->getAt(0), 1.0 - std::numbers::sqrt2,
                    1.0 + std::numbers::sqrt2);
    ExpectCoordNear(seq->getAt(1), -std::numbers::sqrt2, std::numbers::sqrt2);
    ExpectCoordNear(seq->getAt(2), -2.0 * std::cos(pi_over_8),
                    2.0 * std::sin(pi_over_8));
    ExpectCoordNear(seq->getAt(3), -2.0, 0.0);
    ExpectCoordNear(seq->getAt(4), -2.0 * std::cos(pi_over_8),
                    -2.0 * std::sin(pi_over_8));
    ExpectCoordNear(seq->getAt(5), -std::numbers::sqrt2, -std::numbers::sqrt2);
    ExpectCoordNear(seq->getAt(6), 1.0 - std::numbers::sqrt2,
                    -1.0 - std::numbers::sqrt2);
    const CoordinateXY corner(0.0, 0.0);
    for (std::size_t i = 1; i + 2 < seq->size(); ++i) {
      EXPECT_NEAR(seq->getAt(i).distance(corner), 2.0, 1e-9);
    }
  }

  // Acute CCW outside turn (Position::RIGHT) wrapping startAngle >= endAngle
  // (approx +1.67 rad to -1.67 rad) across +/-PI with 7 fillet segments.
  {
    BufferParameters params(4, BufferParameters::CAP_ROUND,
                            BufferParameters::JOIN_ROUND, 5.0);
    std::unique_ptr<CoordinateSequence> seq = RunTwoSegmentCorner(
        pm, params, 2.0, Coordinate(10.0, 1.0), Coordinate(0.0, 0.0),
        Coordinate(10.0, -1.0), Position::RIGHT, true);
    ASSERT_EQ(seq->size(), 11U);
    ExpectCoordNear(seq->getAt(4), -1.9559946, 0.4172349, 1e-6);
    ExpectCoordNear(seq->getAt(5), -1.9559946, -0.4172349, 1e-6);
    const CoordinateXY corner(0.0, 0.0);
    for (std::size_t i = 1; i + 2 < seq->size(); ++i) {
      EXPECT_NEAR(seq->getAt(i).distance(corner), 2.0, 1e-9);
    }
  }
}

TEST(OffsetSegmentGeneratorTest, OutsideTurnBevelAndMitreVariants) {
  PrecisionModel pm(PrecisionModel::FLOATING);

  // Plain bevel join (both LEFT/CW and RIGHT/CCW).
  {
    BufferParameters params(4, BufferParameters::CAP_ROUND,
                            BufferParameters::JOIN_BEVEL, 5.0);
    std::unique_ptr<CoordinateSequence> seq_left = RunTwoSegmentCorner(
        pm, params, 2.0, Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
        Coordinate(10.0, -10.0), Position::LEFT, true);
    ASSERT_EQ(seq_left->size(), 5U);
    ExpectCoordNear(seq_left->getAt(0), 0.0, 2.0);
    ExpectCoordNear(seq_left->getAt(1), 10.0, 2.0);
    ExpectCoordNear(seq_left->getAt(2), 12.0, 0.0);
    ExpectCoordNear(seq_left->getAt(3), 12.0, -10.0);

    std::unique_ptr<CoordinateSequence> seq_right = RunTwoSegmentCorner(
        pm, params, 2.0, Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
        Coordinate(10.0, 10.0), Position::RIGHT, true);
    ASSERT_EQ(seq_right->size(), 5U);
    ExpectCoordNear(seq_right->getAt(0), 0.0, -2.0);
    ExpectCoordNear(seq_right->getAt(1), 10.0, -2.0);
    ExpectCoordNear(seq_right->getAt(2), 12.0, 0.0);
    ExpectCoordNear(seq_right->getAt(3), 12.0, 10.0);
  }

  // Sharp mitre join within mitre limit (both LEFT/CW and RIGHT/CCW).
  {
    BufferParameters params(4, BufferParameters::CAP_ROUND,
                            BufferParameters::JOIN_MITRE, 5.0);
    std::unique_ptr<CoordinateSequence> seq_left = RunTwoSegmentCorner(
        pm, params, 2.0, Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
        Coordinate(10.0, -10.0), Position::LEFT, true);
    ASSERT_EQ(seq_left->size(), 4U);
    ExpectCoordNear(seq_left->getAt(0), 0.0, 2.0);
    ExpectCoordNear(seq_left->getAt(1), 12.0, 2.0);
    ExpectCoordNear(seq_left->getAt(2), 12.0, -10.0);

    std::unique_ptr<CoordinateSequence> seq_right = RunTwoSegmentCorner(
        pm, params, 2.0, Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
        Coordinate(10.0, 10.0), Position::RIGHT, true);
    ASSERT_EQ(seq_right->size(), 4U);
    ExpectCoordNear(seq_right->getAt(0), 0.0, -2.0);
    ExpectCoordNear(seq_right->getAt(1), 12.0, -2.0);
    ExpectCoordNear(seq_right->getAt(2), 12.0, 10.0);
  }

  // Mitre limit smaller than bevelDist falls back directly to plain bevel join.
  {
    BufferParameters params(4, BufferParameters::CAP_ROUND,
                            BufferParameters::JOIN_MITRE, 0.5);
    std::unique_ptr<CoordinateSequence> seq = RunTwoSegmentCorner(
        pm, params, 2.0, Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
        Coordinate(10.0, -10.0), Position::LEFT, true);
    ASSERT_EQ(seq->size(), 5U);
    ExpectCoordNear(seq->getAt(0), 0.0, 2.0);
    ExpectCoordNear(seq->getAt(1), 10.0, 2.0);
    ExpectCoordNear(seq->getAt(2), 12.0, 0.0);
    ExpectCoordNear(seq->getAt(3), 12.0, -10.0);
  }

  // Limited mitre join (bevelDist < mitreLimitDistance < mitre tip distance)
  // for both LEFT/CW and RIGHT/CCW turns.
  {
    BufferParameters params(4, BufferParameters::CAP_ROUND,
                            BufferParameters::JOIN_MITRE, 1.0);
    std::unique_ptr<CoordinateSequence> seq_left = RunTwoSegmentCorner(
        pm, params, 2.0, Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
        Coordinate(10.0, -10.0), Position::LEFT, true);
    ASSERT_EQ(seq_left->size(), 5U);
    ExpectCoordNear(seq_left->getAt(0), 0.0, 2.0);
    ExpectCoordNear(seq_left->getAt(1), 8.0 + 2.0 * std::numbers::sqrt2, 2.0);
    ExpectCoordNear(seq_left->getAt(2), 12.0, 2.0 * std::numbers::sqrt2 - 2.0);
    ExpectCoordNear(seq_left->getAt(3), 12.0, -10.0);

    std::unique_ptr<CoordinateSequence> seq_right = RunTwoSegmentCorner(
        pm, params, 2.0, Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
        Coordinate(10.0, 10.0), Position::RIGHT, true);
    ASSERT_EQ(seq_right->size(), 5U);
    ExpectCoordNear(seq_right->getAt(0), 0.0, -2.0);
    ExpectCoordNear(seq_right->getAt(1), 8.0 + 2.0 * std::numbers::sqrt2, -2.0);
    ExpectCoordNear(seq_right->getAt(2), 12.0, 2.0 - 2.0 * std::numbers::sqrt2);
    ExpectCoordNear(seq_right->getAt(3), 12.0, 10.0);
  }

  // Acute hairpin limited mitre where CGAlgorithmsDD::intersection returns null
  // due to offset rounding while orientationIndex remains clockwise.
  {
    BufferParameters params(4, BufferParameters::CAP_ROUND,
                            BufferParameters::JOIN_MITRE, 3.0);
    std::unique_ptr<CoordinateSequence> seq = RunTwoSegmentCorner(
        pm, params, 1.0, Coordinate(1.0, 0.0), Coordinate(2.0, 0.0),
        Coordinate(1.0, -1e-18), Position::LEFT, true);
    ASSERT_EQ(seq->size(), 5U);
    ExpectCoordNear(seq->getAt(0), 1.0, 1.0);
    ExpectCoordNear(seq->getAt(1), 5.0, 1.0, 1e-6);
    ExpectCoordNear(seq->getAt(2), 5.0, -1.0, 1e-6);
    ExpectCoordNear(seq->getAt(3), 1.0, -1.0);
  }

  // Limited mitre bevel fails to intersect one offset segment due to floating
  // point rounding at large coordinates (ULP = 1/64 at 1e14) and falls back
  // to addBevelJoin.
  {
    const double base = 1e14;
    BufferParameters params(4, BufferParameters::CAP_ROUND,
                            BufferParameters::JOIN_MITRE, 100.0);
    std::unique_ptr<CoordinateSequence> seq = RunTwoSegmentCorner(
        pm, params, 1.0, Coordinate(base - 10000.0, base + 1.0),
        Coordinate(base, base), Coordinate(base - 10000.0, base + 0.984375),
        Position::LEFT, true);
    ASSERT_EQ(seq->size(), 5U);
    ExpectCoordNear(seq->getAt(1), base, base + 1.0, 1e-3);
    ExpectCoordNear(seq->getAt(2), base, base - 1.0, 1e-3);
  }
}

TEST(OffsetSegmentGeneratorTest, InsideTurnIntersectionSnapAndClosingSegments) {
  PrecisionModel pm(PrecisionModel::FLOATING);

  // Inside turn where offset segments intersect directly (CCW + LEFT, CW +
  // RIGHT).
  {
    BufferParameters params(4, BufferParameters::CAP_ROUND,
                            BufferParameters::JOIN_ROUND, 5.0);
    std::unique_ptr<CoordinateSequence> seq_left = RunTwoSegmentCorner(
        pm, params, 1.0, Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
        Coordinate(10.0, 10.0), Position::LEFT, true);
    ASSERT_EQ(seq_left->size(), 4U);
    ExpectCoordNear(seq_left->getAt(0), 0.0, 1.0);
    ExpectCoordNear(seq_left->getAt(1), 9.0, 1.0);
    ExpectCoordNear(seq_left->getAt(2), 9.0, 10.0);

    std::unique_ptr<CoordinateSequence> seq_right = RunTwoSegmentCorner(
        pm, params, 1.0, Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
        Coordinate(10.0, -10.0), Position::RIGHT, true);
    ASSERT_EQ(seq_right->size(), 4U);
    ExpectCoordNear(seq_right->getAt(0), 0.0, -1.0);
    ExpectCoordNear(seq_right->getAt(1), 9.0, -1.0);
    ExpectCoordNear(seq_right->getAt(2), 9.0, -10.0);
  }

  // Non-intersecting offset segments with closely spaced endpoints snap to
  // offset0.p1 (distance < distance * INSIDE_TURN_VERTEX_SNAP_DISTANCE_FACTOR).
  {
    BufferParameters params(4, BufferParameters::CAP_ROUND,
                            BufferParameters::JOIN_ROUND, 5.0);
    std::unique_ptr<CoordinateSequence> seq = RunTwoSegmentCorner(
        pm, params, 10000.0, Coordinate(0.0, 0.0), Coordinate(1.0, 0.0),
        Coordinate(2.0, 0.0005), Position::LEFT, true);
    ASSERT_EQ(seq->size(), 4U);
    ExpectCoordNear(seq->getAt(0), 0.0, 10000.0);
    ExpectCoordNear(seq->getAt(1), 1.0, 10000.0);
    ExpectCoordNear(seq->getAt(2), -3.0, 10000.0, 0.1);
  }

  // Non-intersecting inside turn with closingSegLengthFactor == 1 (JOIN_BEVEL
  // or quadSegs < 8).
  for (const BufferParameters& params : {
           BufferParameters(8, BufferParameters::CAP_ROUND,
                            BufferParameters::JOIN_BEVEL, 5.0),
           BufferParameters(4, BufferParameters::CAP_ROUND,
                            BufferParameters::JOIN_ROUND, 5.0),
       }) {
    std::unique_ptr<CoordinateSequence> seq = RunTwoSegmentCorner(
        pm, params, 5.0, Coordinate(0.0, 0.0), Coordinate(2.0, 0.0),
        Coordinate(2.0, 2.0), Position::LEFT, true);
    ASSERT_EQ(seq->size(), 7U);
    ExpectCoordNear(seq->getAt(0), 0.0, 5.0);
    ExpectCoordNear(seq->getAt(1), 2.0, 5.0);
    ExpectCoordNear(seq->getAt(2), 2.0, 2.5);
    ExpectCoordNear(seq->getAt(3), -0.5, 0.0);
    ExpectCoordNear(seq->getAt(4), -3.0, 0.0);
    ExpectCoordNear(seq->getAt(5), -3.0, 2.0);
  }

  // Non-intersecting inside turn with closingSegLengthFactor == 80 (quadSegs >=
  // 8 and JOIN_ROUND).
  {
    BufferParameters params(8, BufferParameters::CAP_ROUND,
                            BufferParameters::JOIN_ROUND, 5.0);
    std::unique_ptr<CoordinateSequence> seq = RunTwoSegmentCorner(
        pm, params, 5.0, Coordinate(0.0, 0.0), Coordinate(2.0, 0.0),
        Coordinate(2.0, 2.0), Position::LEFT, true);
    ASSERT_EQ(seq->size(), 7U);
    ExpectCoordNear(seq->getAt(0), 0.0, 5.0);
    ExpectCoordNear(seq->getAt(1), 2.0, 5.0);
    ExpectCoordNear(seq->getAt(2), 2.0, 400.0 / 81.0);
    ExpectCoordNear(seq->getAt(3), -238.0 / 81.0, 0.0);
    ExpectCoordNear(seq->getAt(4), -3.0, 0.0);
    ExpectCoordNear(seq->getAt(5), -3.0, 2.0);
  }
}

}  // namespace
}  // namespace geos_google
