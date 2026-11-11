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

#include "third_party/geos/include/geos/operation/buffer/OffsetCurveBuilder.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/operation/buffer/BufferParameters.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Envelope;
using geos::geom::Position;
using geos::geom::PrecisionModel;
using geos::operation::buffer::BufferParameters;
using geos::operation::buffer::OffsetCurveBuilder;
using geos::util::IllegalArgumentException;

void FreeCurves(std::vector<CoordinateSequence*>& curves) {
  for (CoordinateSequence* seq : curves) {
    delete seq;
  }
  curves.clear();
}

bool ContainsPointNear(const CoordinateSequence& seq, double x, double y,
                       double tol = 1e-9) {
  for (std::size_t i = 0; i < seq.size(); ++i) {
    if (std::abs(seq.getX(i) - x) <= tol && std::abs(seq.getY(i) - y) <= tol) {
      return true;
    }
  }
  return false;
}

void ExpectEnvelopeNear(const CoordinateSequence& seq, double min_x,
                        double max_x, double min_y, double max_y,
                        double tol = 1e-9) {
  ASSERT_FALSE(seq.isEmpty());
  Envelope env = seq.getEnvelope();
  EXPECT_NEAR(env.getMinX(), min_x, tol);
  EXPECT_NEAR(env.getMaxX(), max_x, tol);
  EXPECT_NEAR(env.getMinY(), min_y, tol);
  EXPECT_NEAR(env.getMaxY(), max_y, tol);
}

TEST(OffsetCurveBuilderTest, ConstructorAndGetBufferParameters) {
  PrecisionModel pm;
  BufferParameters params(12, BufferParameters::CAP_SQUARE,
                          BufferParameters::JOIN_MITRE, 3.5);
  params.setSingleSided(true);

  OffsetCurveBuilder builder(&pm, params);
  const BufferParameters& stored = builder.getBufferParameters();

  EXPECT_EQ(&stored, &params);
  EXPECT_EQ(stored.getQuadrantSegments(), 12);
  EXPECT_EQ(stored.getEndCapStyle(), BufferParameters::CAP_SQUARE);
  EXPECT_EQ(stored.getJoinStyle(), BufferParameters::JOIN_MITRE);
  EXPECT_DOUBLE_EQ(stored.getMitreLimit(), 3.5);
  EXPECT_TRUE(stored.isSingleSided());
}

TEST(OffsetCurveBuilderTest, IsLineOffsetEmptyTwoSidedAndSingleSided) {
  PrecisionModel pm;

  BufferParameters two_sided_params;
  two_sided_params.setSingleSided(false);
  OffsetCurveBuilder two_sided(&pm, two_sided_params);
  EXPECT_TRUE(two_sided.isLineOffsetEmpty(0.0));
  EXPECT_TRUE(two_sided.isLineOffsetEmpty(-0.0));
  EXPECT_TRUE(two_sided.isLineOffsetEmpty(-2.5));
  EXPECT_FALSE(two_sided.isLineOffsetEmpty(0.01));
  EXPECT_FALSE(two_sided.isLineOffsetEmpty(2.5));

  BufferParameters single_sided_params;
  single_sided_params.setSingleSided(true);
  OffsetCurveBuilder single_sided(&pm, single_sided_params);
  EXPECT_TRUE(single_sided.isLineOffsetEmpty(0.0));
  EXPECT_FALSE(single_sided.isLineOffsetEmpty(-2.5));
  EXPECT_FALSE(single_sided.isLineOffsetEmpty(2.5));
}

TEST(OffsetCurveBuilderTest, GetLineCurveEmptyDistancesReturnEmpty) {
  PrecisionModel pm;
  CoordinateSequence line{Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)};

  BufferParameters two_sided_params;
  OffsetCurveBuilder two_sided(&pm, two_sided_params);

  for (double dist : {0.0, -3.0}) {
    std::vector<CoordinateSequence*> list;
    absl::Cleanup cleanup = [&] { FreeCurves(list); };
    two_sided.getLineCurve(&line, dist, list);
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(two_sided.getLineCurve(&line, dist), nullptr);
  }

  BufferParameters single_sided_params;
  single_sided_params.setSingleSided(true);
  OffsetCurveBuilder single_sided(&pm, single_sided_params);

  std::vector<CoordinateSequence*> list;
  absl::Cleanup cleanup = [&] { FreeCurves(list); };
  single_sided.getLineCurve(&line, 0.0, list);
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(single_sided.getLineCurve(&line, 0.0), nullptr);
}

TEST(OffsetCurveBuilderTest, GetLineCurveSinglePointAllCapStyles) {
  PrecisionModel pm;
  CoordinateSequence pt{Coordinate(10.0, 20.0)};

  {
    BufferParameters params(4, BufferParameters::CAP_ROUND);
    OffsetCurveBuilder builder(&pm, params);
    std::vector<CoordinateSequence*> list;
    absl::Cleanup cleanup = [&] { FreeCurves(list); };

    builder.getLineCurve(&pt, 5.0, list);
    ASSERT_EQ(list.size(), 1u);
    std::unique_ptr<CoordinateSequence> curve = builder.getLineCurve(&pt, 5.0);
    ASSERT_NE(curve, nullptr);
    EXPECT_TRUE(CoordinateSequence::equals(list[0], curve.get()));
    EXPECT_TRUE(curve->isRing());
    EXPECT_GE(curve->size(), 16u);
    ExpectEnvelopeNear(*curve, 5.0, 15.0, 15.0, 25.0);
    for (std::size_t i = 0; i < curve->size(); ++i) {
      double radius = std::hypot(curve->getX(i) - 10.0, curve->getY(i) - 20.0);
      EXPECT_NEAR(radius, 5.0, 1e-9);
    }
  }

  {
    BufferParameters params(4, BufferParameters::CAP_SQUARE);
    OffsetCurveBuilder builder(&pm, params);
    std::vector<CoordinateSequence*> list;
    absl::Cleanup cleanup = [&] { FreeCurves(list); };

    builder.getLineCurve(&pt, 5.0, list);
    ASSERT_EQ(list.size(), 1u);
    std::unique_ptr<CoordinateSequence> curve = builder.getLineCurve(&pt, 5.0);
    ASSERT_NE(curve, nullptr);
    EXPECT_TRUE(CoordinateSequence::equals(list[0], curve.get()));

    CoordinateSequence expected{Coordinate(15.0, 25.0), Coordinate(15.0, 15.0),
                                Coordinate(5.0, 15.0), Coordinate(5.0, 25.0),
                                Coordinate(15.0, 25.0)};
    EXPECT_TRUE(CoordinateSequence::equals(curve.get(), &expected));
  }

  {
    BufferParameters params(4, BufferParameters::CAP_FLAT);
    OffsetCurveBuilder builder(&pm, params);
    std::vector<CoordinateSequence*> list;
    absl::Cleanup cleanup = [&] { FreeCurves(list); };

    builder.getLineCurve(&pt, 5.0, list);
    ASSERT_EQ(list.size(), 1u);
    EXPECT_TRUE(list[0]->isEmpty());

    std::unique_ptr<CoordinateSequence> curve = builder.getLineCurve(&pt, 5.0);
    ASSERT_NE(curve, nullptr);
    EXPECT_TRUE(curve->isEmpty());
  }

  {
    BufferParameters params(4, BufferParameters::CAP_SQUARE);
    params.setSingleSided(true);
    OffsetCurveBuilder builder(&pm, params);
    std::vector<CoordinateSequence*> list;
    absl::Cleanup cleanup = [&] { FreeCurves(list); };

    builder.getLineCurve(&pt, -5.0, list);
    ASSERT_EQ(list.size(), 1u);
    std::unique_ptr<CoordinateSequence> curve = builder.getLineCurve(&pt, -5.0);
    ASSERT_NE(curve, nullptr);
    EXPECT_TRUE(CoordinateSequence::equals(list[0], curve.get()));
    EXPECT_EQ(curve->size(), 5u);
    EXPECT_TRUE(curve->isRing());
    ExpectEnvelopeNear(*curve, 5.0, 15.0, 15.0, 25.0);
  }
}

TEST(OffsetCurveBuilderTest, GetLineCurveTwoPointSegmentAllCapStyles) {
  PrecisionModel pm;
  CoordinateSequence line{Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)};

  {
    BufferParameters params(8, BufferParameters::CAP_FLAT);
    OffsetCurveBuilder builder(&pm, params);
    std::vector<CoordinateSequence*> list;
    absl::Cleanup cleanup = [&] { FreeCurves(list); };

    builder.getLineCurve(&line, 2.0, list);
    ASSERT_EQ(list.size(), 1u);
    std::unique_ptr<CoordinateSequence> curve =
        builder.getLineCurve(&line, 2.0);
    ASSERT_NE(curve, nullptr);
    EXPECT_TRUE(CoordinateSequence::equals(list[0], curve.get()));

    CoordinateSequence expected{Coordinate(10.0, 2.0), Coordinate(10.0, -2.0),
                                Coordinate(0.0, -2.0), Coordinate(0.0, 2.0),
                                Coordinate(10.0, 2.0)};
    EXPECT_TRUE(CoordinateSequence::equals(curve.get(), &expected));
  }

  {
    BufferParameters params(8, BufferParameters::CAP_SQUARE);
    OffsetCurveBuilder builder(&pm, params);
    std::vector<CoordinateSequence*> list;
    absl::Cleanup cleanup = [&] { FreeCurves(list); };

    builder.getLineCurve(&line, 2.0, list);
    ASSERT_EQ(list.size(), 1u);
    std::unique_ptr<CoordinateSequence> curve =
        builder.getLineCurve(&line, 2.0);
    ASSERT_NE(curve, nullptr);
    EXPECT_TRUE(CoordinateSequence::equals(list[0], curve.get()));
    EXPECT_TRUE(curve->isRing());
    ExpectEnvelopeNear(*curve, -2.0, 12.0, -2.0, 2.0);
  }

  {
    BufferParameters params(4, BufferParameters::CAP_ROUND);
    OffsetCurveBuilder builder(&pm, params);
    std::vector<CoordinateSequence*> list;
    absl::Cleanup cleanup = [&] { FreeCurves(list); };

    builder.getLineCurve(&line, 2.0, list);
    ASSERT_EQ(list.size(), 1u);
    std::unique_ptr<CoordinateSequence> curve =
        builder.getLineCurve(&line, 2.0);
    ASSERT_NE(curve, nullptr);
    EXPECT_TRUE(CoordinateSequence::equals(list[0], curve.get()));
    EXPECT_TRUE(curve->isRing());
    EXPECT_GT(curve->size(), 8u);
    ExpectEnvelopeNear(*curve, -2.0, 12.0, -2.0, 2.0);
  }
}

TEST(OffsetCurveBuilderTest, GetLineCurvePolylineJoinStylesAndSimplification) {
  PrecisionModel pm;
  // Shallow concavities (0.01 < distTol = 0.02) on both sides plus exact
  // 90-degree left and right turns.
  CoordinateSequence polyline{Coordinate(0.0, 0.0),   Coordinate(10.0, 0.0),
                              Coordinate(15.0, 0.01), Coordinate(20.0, 0.0),
                              Coordinate(30.0, 0.0),  Coordinate(30.0, 10.0),
                              Coordinate(40.0, 10.0), Coordinate(45.0, 9.99),
                              Coordinate(50.0, 10.0), Coordinate(60.0, 10.0)};

  std::size_t bevel_size = 0;
  {
    BufferParameters params(8, BufferParameters::CAP_FLAT,
                            BufferParameters::JOIN_MITRE, 5.0);
    OffsetCurveBuilder builder(&pm, params);
    std::vector<CoordinateSequence*> list;
    absl::Cleanup cleanup = [&] { FreeCurves(list); };

    builder.getLineCurve(&polyline, 2.0, list);
    ASSERT_EQ(list.size(), 1u);
    std::unique_ptr<CoordinateSequence> curve =
        builder.getLineCurve(&polyline, 2.0);
    ASSERT_NE(curve, nullptr);
    EXPECT_TRUE(CoordinateSequence::equals(list[0], curve.get()));
    EXPECT_TRUE(curve->isRing());
    ExpectEnvelopeNear(*curve, 0.0, 60.0, -2.0, 12.0);
    EXPECT_TRUE(ContainsPointNear(*curve, 28.0, 2.0));
    EXPECT_TRUE(ContainsPointNear(*curve, 28.0, 12.0));
    EXPECT_TRUE(ContainsPointNear(*curve, 32.0, -2.0));
    EXPECT_TRUE(ContainsPointNear(*curve, 32.0, 8.0));
  }

  {
    BufferParameters params(8, BufferParameters::CAP_FLAT,
                            BufferParameters::JOIN_BEVEL, 5.0);
    OffsetCurveBuilder builder(&pm, params);
    std::vector<CoordinateSequence*> list;
    absl::Cleanup cleanup = [&] { FreeCurves(list); };

    builder.getLineCurve(&polyline, 2.0, list);
    ASSERT_EQ(list.size(), 1u);
    std::unique_ptr<CoordinateSequence> curve =
        builder.getLineCurve(&polyline, 2.0);
    ASSERT_NE(curve, nullptr);
    EXPECT_TRUE(CoordinateSequence::equals(list[0], curve.get()));
    EXPECT_TRUE(curve->isRing());
    ExpectEnvelopeNear(*curve, 0.0, 60.0, -2.0, 12.0);
    EXPECT_FALSE(ContainsPointNear(*curve, 32.0, -2.0));
    EXPECT_FALSE(ContainsPointNear(*curve, 28.0, 12.0));
    EXPECT_TRUE(ContainsPointNear(*curve, 30.0, -2.0));
    EXPECT_TRUE(ContainsPointNear(*curve, 32.0, 0.0));
    bevel_size = curve->size();
  }

  {
    BufferParameters params(4, BufferParameters::CAP_FLAT,
                            BufferParameters::JOIN_ROUND, 5.0);
    OffsetCurveBuilder builder(&pm, params);
    std::vector<CoordinateSequence*> list;
    absl::Cleanup cleanup = [&] { FreeCurves(list); };

    builder.getLineCurve(&polyline, 2.0, list);
    ASSERT_EQ(list.size(), 1u);
    std::unique_ptr<CoordinateSequence> curve =
        builder.getLineCurve(&polyline, 2.0);
    ASSERT_NE(curve, nullptr);
    EXPECT_TRUE(CoordinateSequence::equals(list[0], curve.get()));
    EXPECT_TRUE(curve->isRing());
    ExpectEnvelopeNear(*curve, 0.0, 60.0, -2.0, 12.0);
    EXPECT_GT(curve->size(), bevel_size);
  }
}

TEST(OffsetCurveBuilderTest, GetLineCurveSingleSidedMode) {
  PrecisionModel pm;
  BufferParameters params(8, BufferParameters::CAP_FLAT,
                          BufferParameters::JOIN_MITRE, 5.0);
  params.setSingleSided(true);
  OffsetCurveBuilder builder(&pm, params);

  CoordinateSequence seg{Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)};
  {
    std::vector<CoordinateSequence*> list;
    absl::Cleanup cleanup = [&] { FreeCurves(list); };
    builder.getLineCurve(&seg, 2.0, list);
    ASSERT_EQ(list.size(), 1u);
    std::unique_ptr<CoordinateSequence> curve = builder.getLineCurve(&seg, 2.0);
    ASSERT_NE(curve, nullptr);
    EXPECT_TRUE(CoordinateSequence::equals(list[0], curve.get()));

    CoordinateSequence expected{Coordinate(10.0, 0.0), Coordinate(0.0, 0.0),
                                Coordinate(0.0, 2.0), Coordinate(10.0, 2.0),
                                Coordinate(10.0, 0.0)};
    EXPECT_TRUE(CoordinateSequence::equals(curve.get(), &expected));
  }

  {
    std::vector<CoordinateSequence*> list;
    absl::Cleanup cleanup = [&] { FreeCurves(list); };
    builder.getLineCurve(&seg, -2.0, list);
    ASSERT_EQ(list.size(), 1u);
    std::unique_ptr<CoordinateSequence> curve =
        builder.getLineCurve(&seg, -2.0);
    ASSERT_NE(curve, nullptr);
    EXPECT_TRUE(CoordinateSequence::equals(list[0], curve.get()));

    CoordinateSequence expected{Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                                Coordinate(10.0, -2.0), Coordinate(0.0, -2.0),
                                Coordinate(0.0, 0.0)};
    EXPECT_TRUE(CoordinateSequence::equals(curve.get(), &expected));
  }

  CoordinateSequence l_shape{Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                             Coordinate(10.0, 10.0)};
  {
    std::vector<CoordinateSequence*> list;
    absl::Cleanup cleanup = [&] { FreeCurves(list); };
    builder.getLineCurve(&l_shape, 2.0, list);
    ASSERT_EQ(list.size(), 1u);
    std::unique_ptr<CoordinateSequence> left =
        builder.getLineCurve(&l_shape, 2.0);
    ASSERT_NE(left, nullptr);
    EXPECT_TRUE(CoordinateSequence::equals(list[0], left.get()));
    EXPECT_TRUE(left->isRing());
    ExpectEnvelopeNear(*left, 0.0, 10.0, 0.0, 10.0);
    EXPECT_TRUE(ContainsPointNear(*left, 8.0, 2.0));
  }

  {
    std::vector<CoordinateSequence*> list;
    absl::Cleanup cleanup = [&] { FreeCurves(list); };
    builder.getLineCurve(&l_shape, -2.0, list);
    ASSERT_EQ(list.size(), 1u);
    std::unique_ptr<CoordinateSequence> right =
        builder.getLineCurve(&l_shape, -2.0);
    ASSERT_NE(right, nullptr);
    EXPECT_TRUE(CoordinateSequence::equals(list[0], right.get()));
    EXPECT_TRUE(right->isRing());
    ExpectEnvelopeNear(*right, 0.0, 12.0, -2.0, 10.0);
    EXPECT_TRUE(ContainsPointNear(*right, 12.0, -2.0));
  }
}

TEST(OffsetCurveBuilderTest, GetOffsetCurveVectorOverload) {
  PrecisionModel pm;
  BufferParameters params(8, BufferParameters::CAP_SQUARE,
                          BufferParameters::JOIN_MITRE, 5.0);
  OffsetCurveBuilder builder(&pm, params);

  CoordinateSequence seg{Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)};
  {
    std::vector<CoordinateSequence*> list;
    absl::Cleanup cleanup = [&] { FreeCurves(list); };
    builder.getOffsetCurve(&seg, 0.0, list);
    EXPECT_TRUE(list.empty());
  }

  CoordinateSequence pt{Coordinate(5.0, 5.0)};
  {
    std::vector<CoordinateSequence*> pos_list;
    std::vector<CoordinateSequence*> neg_list;
    absl::Cleanup cleanup = [&] {
      FreeCurves(pos_list);
      FreeCurves(neg_list);
    };
    builder.getOffsetCurve(&pt, 3.0, pos_list);
    builder.getOffsetCurve(&pt, -3.0, neg_list);
    ASSERT_EQ(pos_list.size(), 1u);
    ASSERT_EQ(neg_list.size(), 1u);
    EXPECT_EQ(pos_list[0]->size(), 5u);
    EXPECT_EQ(neg_list[0]->size(), 5u);
    ExpectEnvelopeNear(*pos_list[0], 2.0, 8.0, 2.0, 8.0);
    ExpectEnvelopeNear(*neg_list[0], 2.0, 8.0, 2.0, 8.0);
    EXPECT_FALSE(CoordinateSequence::equals(pos_list[0], neg_list[0]));
  }

  {
    std::vector<CoordinateSequence*> pos_list;
    std::vector<CoordinateSequence*> neg_list;
    absl::Cleanup cleanup = [&] {
      FreeCurves(pos_list);
      FreeCurves(neg_list);
    };
    builder.getOffsetCurve(&seg, 2.0, pos_list);
    builder.getOffsetCurve(&seg, -2.0, neg_list);
    ASSERT_EQ(pos_list.size(), 1u);
    ASSERT_EQ(neg_list.size(), 1u);

    CoordinateSequence expected_pos{Coordinate(10.0, 0.0), Coordinate(0.0, 0.0),
                                    Coordinate(0.0, 2.0), Coordinate(10.0, 2.0),
                                    Coordinate(10.0, 0.0)};
    // Right side is traversed in reverse order and reversed back.
    CoordinateSequence expected_neg{
        Coordinate(0.0, 0.0), Coordinate(0.0, -2.0), Coordinate(10.0, -2.0),
        Coordinate(10.0, 0.0), Coordinate(0.0, 0.0)};
    EXPECT_TRUE(CoordinateSequence::equals(pos_list[0], &expected_pos));
    EXPECT_TRUE(CoordinateSequence::equals(neg_list[0], &expected_neg));
  }

  CoordinateSequence l_shape{Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                             Coordinate(10.0, 10.0)};
  {
    std::vector<CoordinateSequence*> pos_list;
    std::vector<CoordinateSequence*> neg_list;
    absl::Cleanup cleanup = [&] {
      FreeCurves(pos_list);
      FreeCurves(neg_list);
    };
    builder.getOffsetCurve(&l_shape, 2.0, pos_list);
    builder.getOffsetCurve(&l_shape, -2.0, neg_list);
    ASSERT_EQ(pos_list.size(), 1u);
    ASSERT_EQ(neg_list.size(), 1u);
    ExpectEnvelopeNear(*pos_list[0], 0.0, 10.0, 0.0, 10.0);
    ExpectEnvelopeNear(*neg_list[0], 0.0, 12.0, -2.0, 10.0);
    EXPECT_TRUE(ContainsPointNear(*pos_list[0], 8.0, 2.0));
    EXPECT_TRUE(ContainsPointNear(*neg_list[0], 12.0, -2.0));
  }
}

TEST(OffsetCurveBuilderTest, GetOffsetCurveUniquePtrOverloadAndErrors) {
  PrecisionModel pm;
  BufferParameters params(8, BufferParameters::CAP_SQUARE,
                          BufferParameters::JOIN_MITRE, 5.0);
  OffsetCurveBuilder builder(&pm, params);

  CoordinateSequence seg{Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)};
  EXPECT_EQ(builder.getOffsetCurve(&seg, 0.0), nullptr);

  CoordinateSequence pt{Coordinate(5.0, 5.0)};
  {
    std::unique_ptr<CoordinateSequence> pos_pt =
        builder.getOffsetCurve(&pt, 3.0);
    std::unique_ptr<CoordinateSequence> neg_pt =
        builder.getOffsetCurve(&pt, -3.0);
    ASSERT_NE(pos_pt, nullptr);
    ASSERT_NE(neg_pt, nullptr);
    EXPECT_EQ(pos_pt->size(), 5u);
    EXPECT_EQ(neg_pt->size(), 5u);
    ExpectEnvelopeNear(*pos_pt, 2.0, 8.0, 2.0, 8.0);
    ExpectEnvelopeNear(*neg_pt, 2.0, 8.0, 2.0, 8.0);
  }

  {
    std::unique_ptr<CoordinateSequence> pos_seg =
        builder.getOffsetCurve(&seg, 2.0);
    std::unique_ptr<CoordinateSequence> neg_seg =
        builder.getOffsetCurve(&seg, -2.0);
    ASSERT_NE(pos_seg, nullptr);
    ASSERT_NE(neg_seg, nullptr);

    CoordinateSequence expected_pos{Coordinate(0.0, 2.0), Coordinate(10.0, 2.0),
                                    Coordinate(0.0, 2.0)};
    CoordinateSequence expected_neg{
        Coordinate(10.0, -2.0), Coordinate(0.0, -2.0), Coordinate(10.0, -2.0)};
    EXPECT_TRUE(CoordinateSequence::equals(pos_seg.get(), &expected_pos));
    EXPECT_TRUE(CoordinateSequence::equals(neg_seg.get(), &expected_neg));
  }

  CoordinateSequence l_shape{Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                             Coordinate(10.0, 10.0)};
  {
    std::unique_ptr<CoordinateSequence> left =
        builder.getOffsetCurve(&l_shape, 2.0);
    std::unique_ptr<CoordinateSequence> right =
        builder.getOffsetCurve(&l_shape, -2.0);
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);

    CoordinateSequence expected_left{Coordinate(0.0, 2.0), Coordinate(8.0, 2.0),
                                     Coordinate(8.0, 10.0),
                                     Coordinate(0.0, 2.0)};
    CoordinateSequence expected_right{
        Coordinate(12.0, 10.0), Coordinate(0.0, -2.0), Coordinate(12.0, -2.0),
        Coordinate(12.0, 10.0)};
    EXPECT_TRUE(CoordinateSequence::equals(left.get(), &expected_left));
    EXPECT_TRUE(CoordinateSequence::equals(right.get(), &expected_right));
  }

  // Repeated coordinates collapse to a single vertex in
  // BufferInputLineSimplifier and throw IllegalArgumentException on both sides.
  CoordinateSequence dup_line{Coordinate(3.0, 4.0), Coordinate(3.0, 4.0)};
  EXPECT_THROW(builder.getOffsetCurve(&dup_line, 2.0),
               IllegalArgumentException);
  EXPECT_THROW(builder.getOffsetCurve(&dup_line, -2.0),
               IllegalArgumentException);
}

TEST(OffsetCurveBuilderTest, GetSingleSidedLineCurveAllBranchesAndErrors) {
  PrecisionModel pm;
  BufferParameters params(8, BufferParameters::CAP_FLAT,
                          BufferParameters::JOIN_MITRE, 5.0);
  OffsetCurveBuilder builder(&pm, params);

  CoordinateSequence empty_pts;
  CoordinateSequence single_pt{Coordinate(1.0, 1.0)};
  CoordinateSequence seg{Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)};

  {
    std::vector<CoordinateSequence*> list;
    absl::Cleanup cleanup = [&] { FreeCurves(list); };
    builder.getSingleSidedLineCurve(&seg, 0.0, list, true, true);
    builder.getSingleSidedLineCurve(&seg, -2.0, list, true, true);
    builder.getSingleSidedLineCurve(&empty_pts, 2.0, list, true, true);
    builder.getSingleSidedLineCurve(&single_pt, 2.0, list, true, true);
    EXPECT_TRUE(list.empty());
  }

  {
    std::vector<CoordinateSequence*> list;
    absl::Cleanup cleanup = [&] { FreeCurves(list); };
    builder.getSingleSidedLineCurve(&seg, 2.0, list, false, false);
    ASSERT_EQ(list.size(), 1u);
    EXPECT_TRUE(list[0]->isEmpty());
  }

  {
    std::vector<CoordinateSequence*> left_list;
    std::vector<CoordinateSequence*> right_list;
    std::vector<CoordinateSequence*> both_list;
    absl::Cleanup cleanup = [&] {
      FreeCurves(left_list);
      FreeCurves(right_list);
      FreeCurves(both_list);
    };

    builder.getSingleSidedLineCurve(&seg, 2.0, left_list, true, false);
    builder.getSingleSidedLineCurve(&seg, 2.0, right_list, false, true);
    builder.getSingleSidedLineCurve(&seg, 2.0, both_list, true, true);

    ASSERT_EQ(left_list.size(), 1u);
    ASSERT_EQ(right_list.size(), 1u);
    ASSERT_EQ(both_list.size(), 1u);

    CoordinateSequence expected_left{
        Coordinate(0.0, 2.0), Coordinate(10.0, 2.0), Coordinate(0.0, 2.0)};
    CoordinateSequence expected_right{
        Coordinate(10.0, -2.0), Coordinate(0.0, -2.0), Coordinate(10.0, -2.0)};
    EXPECT_TRUE(CoordinateSequence::equals(left_list[0], &expected_left));
    EXPECT_TRUE(CoordinateSequence::equals(right_list[0], &expected_right));
    EXPECT_TRUE(both_list[0]->isRing());
    ExpectEnvelopeNear(*both_list[0], 0.0, 10.0, -2.0, 2.0);
  }

  CoordinateSequence l_shape{Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                             Coordinate(10.0, 10.0)};
  {
    std::vector<CoordinateSequence*> left_list;
    std::vector<CoordinateSequence*> right_list;
    std::vector<CoordinateSequence*> both_list;
    absl::Cleanup cleanup = [&] {
      FreeCurves(left_list);
      FreeCurves(right_list);
      FreeCurves(both_list);
    };

    builder.getSingleSidedLineCurve(&l_shape, 2.0, left_list, true, false);
    builder.getSingleSidedLineCurve(&l_shape, 2.0, right_list, false, true);
    builder.getSingleSidedLineCurve(&l_shape, 2.0, both_list, true, true);

    ASSERT_EQ(left_list.size(), 1u);
    ASSERT_EQ(right_list.size(), 1u);
    ASSERT_EQ(both_list.size(), 1u);
    ExpectEnvelopeNear(*left_list[0], 0.0, 8.0, 2.0, 10.0);
    ExpectEnvelopeNear(*right_list[0], 0.0, 12.0, -2.0, 10.0);
    ExpectEnvelopeNear(*both_list[0], 0.0, 12.0, -2.0, 10.0);
    EXPECT_TRUE(ContainsPointNear(*both_list[0], 8.0, 2.0));
    EXPECT_TRUE(ContainsPointNear(*both_list[0], 12.0, -2.0));
  }

  CoordinateSequence dup_line{Coordinate(1.0, 2.0), Coordinate(1.0, 2.0)};
  std::vector<CoordinateSequence*> err_list;
  absl::Cleanup cleanup = [&] { FreeCurves(err_list); };
  EXPECT_THROW(
      builder.getSingleSidedLineCurve(&dup_line, 2.0, err_list, true, false),
      IllegalArgumentException);
  EXPECT_THROW(
      builder.getSingleSidedLineCurve(&dup_line, 2.0, err_list, false, true),
      IllegalArgumentException);
  EXPECT_TRUE(err_list.empty());
}

TEST(OffsetCurveBuilderTest, GetRingCurveDegenerateAndZeroDistance) {
  PrecisionModel pm;
  BufferParameters params(8, BufferParameters::CAP_SQUARE,
                          BufferParameters::JOIN_MITRE, 5.0);
  OffsetCurveBuilder builder(&pm, params);

  CoordinateSequence empty_pts;
  {
    std::vector<CoordinateSequence*> list;
    absl::Cleanup cleanup = [&] { FreeCurves(list); };
    builder.getRingCurve(&empty_pts, +Position::LEFT, 0.0, list);
    ASSERT_EQ(list.size(), 1u);
    EXPECT_TRUE(list[0]->isEmpty());
    EXPECT_EQ(builder.getRingCurve(&empty_pts, +Position::LEFT, 0.0), nullptr);
  }

  CoordinateSequence single_pt{Coordinate(5.0, 5.0)};
  {
    std::vector<CoordinateSequence*> zero_list;
    std::vector<CoordinateSequence*> pos_list;
    absl::Cleanup cleanup = [&] {
      FreeCurves(zero_list);
      FreeCurves(pos_list);
    };
    builder.getRingCurve(&single_pt, +Position::LEFT, 0.0, zero_list);
    ASSERT_EQ(zero_list.size(), 1u);
    EXPECT_TRUE(zero_list[0]->equalsIdentical(single_pt));
    EXPECT_EQ(builder.getRingCurve(&single_pt, +Position::LEFT, 0.0), nullptr);

    builder.getRingCurve(&single_pt, +Position::LEFT, 3.0, pos_list);
    ASSERT_EQ(pos_list.size(), 1u);
    std::unique_ptr<CoordinateSequence> curve =
        builder.getRingCurve(&single_pt, +Position::LEFT, 3.0);
    ASSERT_NE(curve, nullptr);
    EXPECT_TRUE(CoordinateSequence::equals(pos_list[0], curve.get()));
    ExpectEnvelopeNear(*curve, 2.0, 8.0, 2.0, 8.0);
  }

  CoordinateSequence two_pts{Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)};
  {
    std::vector<CoordinateSequence*> zero_list;
    std::vector<CoordinateSequence*> pos_list;
    absl::Cleanup cleanup = [&] {
      FreeCurves(zero_list);
      FreeCurves(pos_list);
    };
    builder.getRingCurve(&two_pts, +Position::LEFT, 0.0, zero_list);
    ASSERT_EQ(zero_list.size(), 1u);
    EXPECT_TRUE(zero_list[0]->equalsIdentical(two_pts));
    EXPECT_EQ(builder.getRingCurve(&two_pts, +Position::LEFT, 0.0), nullptr);

    builder.getRingCurve(&two_pts, +Position::LEFT, 2.0, pos_list);
    ASSERT_EQ(pos_list.size(), 1u);
    std::unique_ptr<CoordinateSequence> curve =
        builder.getRingCurve(&two_pts, +Position::LEFT, 2.0);
    ASSERT_NE(curve, nullptr);
    EXPECT_TRUE(CoordinateSequence::equals(pos_list[0], curve.get()));
    ExpectEnvelopeNear(*curve, -2.0, 12.0, -2.0, 2.0);
  }

  CoordinateSequence square_ring{Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                                 Coordinate(10.0, 10.0), Coordinate(0.0, 10.0),
                                 Coordinate(0.0, 0.0)};
  {
    std::vector<CoordinateSequence*> zero_list;
    absl::Cleanup cleanup = [&] { FreeCurves(zero_list); };
    builder.getRingCurve(&square_ring, +Position::LEFT, 0.0, zero_list);
    ASSERT_EQ(zero_list.size(), 1u);
    EXPECT_TRUE(zero_list[0]->equalsIdentical(square_ring));

    std::unique_ptr<CoordinateSequence> curve =
        builder.getRingCurve(&square_ring, +Position::LEFT, 0.0);
    ASSERT_NE(curve, nullptr);
    EXPECT_TRUE(curve->equalsIdentical(square_ring));
  }
}

TEST(OffsetCurveBuilderTest, GetRingCurveLeftRightAndSimplification) {
  PrecisionModel pm;
  CoordinateSequence ccw_square{Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                                Coordinate(10.0, 10.0), Coordinate(0.0, 10.0),
                                Coordinate(0.0, 0.0)};

  {
    BufferParameters params(8, BufferParameters::CAP_ROUND,
                            BufferParameters::JOIN_MITRE, 5.0);
    OffsetCurveBuilder builder(&pm, params);
    std::vector<CoordinateSequence*> left_list;
    std::vector<CoordinateSequence*> right_list;
    absl::Cleanup cleanup = [&] {
      FreeCurves(left_list);
      FreeCurves(right_list);
    };

    builder.getRingCurve(&ccw_square, +Position::LEFT, 2.0, left_list);
    builder.getRingCurve(&ccw_square, +Position::RIGHT, 2.0, right_list);
    ASSERT_EQ(left_list.size(), 1u);
    ASSERT_EQ(right_list.size(), 1u);

    std::unique_ptr<CoordinateSequence> left_curve =
        builder.getRingCurve(&ccw_square, +Position::LEFT, 2.0);
    std::unique_ptr<CoordinateSequence> right_curve =
        builder.getRingCurve(&ccw_square, +Position::RIGHT, 2.0);
    ASSERT_NE(left_curve, nullptr);
    ASSERT_NE(right_curve, nullptr);
    EXPECT_TRUE(CoordinateSequence::equals(left_list[0], left_curve.get()));
    EXPECT_TRUE(CoordinateSequence::equals(right_list[0], right_curve.get()));

    EXPECT_TRUE(left_curve->isRing());
    EXPECT_TRUE(right_curve->isRing());
    ExpectEnvelopeNear(*left_curve, 2.0, 8.0, 2.0, 8.0);
    ExpectEnvelopeNear(*right_curve, -2.0, 12.0, -2.0, 12.0);
  }

  // Non-convex ring with shallow concavities simplified differently per side.
  CoordinateSequence indented_ring{
      Coordinate(0.0, 0.0),   Coordinate(10.0, 0.0),  Coordinate(15.0, 0.01),
      Coordinate(20.0, 0.0),  Coordinate(20.0, 20.0), Coordinate(10.0, 20.0),
      Coordinate(10.0, 10.0), Coordinate(5.0, 10.01), Coordinate(0.0, 10.0),
      Coordinate(0.0, 0.0)};
  {
    BufferParameters params(4, BufferParameters::CAP_ROUND,
                            BufferParameters::JOIN_ROUND, 5.0);
    OffsetCurveBuilder builder(&pm, params);
    std::vector<CoordinateSequence*> left_list;
    std::vector<CoordinateSequence*> right_list;
    absl::Cleanup cleanup = [&] {
      FreeCurves(left_list);
      FreeCurves(right_list);
    };

    builder.getRingCurve(&indented_ring, +Position::LEFT, 2.0, left_list);
    builder.getRingCurve(&indented_ring, +Position::RIGHT, 2.0, right_list);
    ASSERT_EQ(left_list.size(), 1u);
    ASSERT_EQ(right_list.size(), 1u);

    std::unique_ptr<CoordinateSequence> left_curve =
        builder.getRingCurve(&indented_ring, +Position::LEFT, 2.0);
    std::unique_ptr<CoordinateSequence> right_curve =
        builder.getRingCurve(&indented_ring, +Position::RIGHT, 2.0);
    ASSERT_NE(left_curve, nullptr);
    ASSERT_NE(right_curve, nullptr);
    EXPECT_TRUE(CoordinateSequence::equals(left_list[0], left_curve.get()));
    EXPECT_TRUE(CoordinateSequence::equals(right_list[0], right_curve.get()));

    EXPECT_TRUE(left_curve->isRing());
    EXPECT_TRUE(right_curve->isRing());
    ExpectEnvelopeNear(*left_curve, 2.0, 18.0, 2.0, 18.0, 0.02);
    ExpectEnvelopeNear(*right_curve, -2.0, 22.0, -2.0, 22.0, 0.02);
  }
}

TEST(OffsetCurveBuilderTest, FixedPrecisionModelRoundsOutputVertices) {
  PrecisionModel fixed_pm(1.0);
  BufferParameters params(4, BufferParameters::CAP_ROUND,
                          BufferParameters::JOIN_ROUND, 5.0);
  OffsetCurveBuilder builder(&fixed_pm, params);

  CoordinateSequence line{Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)};
  std::unique_ptr<CoordinateSequence> curve = builder.getLineCurve(&line, 2.0);
  ASSERT_NE(curve, nullptr);
  EXPECT_TRUE(curve->isRing());

  for (std::size_t i = 0; i < curve->size(); ++i) {
    EXPECT_DOUBLE_EQ(curve->getX(i), std::round(curve->getX(i)));
    EXPECT_DOUBLE_EQ(curve->getY(i), std::round(curve->getY(i)));
  }
}

TEST(OffsetCurveBuilderTest, ReuseBuilderAcrossMultipleOperations) {
  PrecisionModel pm;
  BufferParameters params(8, BufferParameters::CAP_FLAT,
                          BufferParameters::JOIN_MITRE, 5.0);
  OffsetCurveBuilder builder(&pm, params);

  CoordinateSequence line{Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)};
  CoordinateSequence ring{Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
                          Coordinate(10.0, 10.0), Coordinate(0.0, 10.0),
                          Coordinate(0.0, 0.0)};

  std::unique_ptr<CoordinateSequence> line_buf =
      builder.getLineCurve(&line, 2.0);
  std::unique_ptr<CoordinateSequence> right_offset =
      builder.getOffsetCurve(&line, -3.0);
  std::unique_ptr<CoordinateSequence> outer_ring =
      builder.getRingCurve(&ring, +Position::RIGHT, 1.5);

  std::vector<CoordinateSequence*> single_list;
  absl::Cleanup cleanup = [&] { FreeCurves(single_list); };
  builder.getSingleSidedLineCurve(&line, 4.0, single_list, true, false);

  ASSERT_NE(line_buf, nullptr);
  ASSERT_NE(right_offset, nullptr);
  ASSERT_NE(outer_ring, nullptr);
  ASSERT_EQ(single_list.size(), 1u);

  ExpectEnvelopeNear(*line_buf, 0.0, 10.0, -2.0, 2.0);
  ExpectEnvelopeNear(*right_offset, 0.0, 10.0, -3.0, -3.0);
  ExpectEnvelopeNear(*outer_ring, -1.5, 11.5, -1.5, 11.5);
  ExpectEnvelopeNear(*single_list[0], 0.0, 10.0, 4.0, 4.0);
}

}  // namespace
}  // namespace geos_google
