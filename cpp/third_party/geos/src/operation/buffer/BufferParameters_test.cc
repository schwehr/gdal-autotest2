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

#include "third_party/geos/include/geos/operation/buffer/BufferParameters.h"

#include <array>
#include <cmath>
#include <initializer_list>
#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/Angle.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/operation/buffer/BufferOp.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::operation::buffer::BufferOp;
using geos::operation::buffer::BufferParameters;

TEST(BufferParametersTest, DefaultConstructorSetsDefaults) {
  BufferParameters params;

  const int default_quadrant_segments =
      BufferParameters::DEFAULT_QUADRANT_SEGMENTS;
  const double default_mitre_limit = BufferParameters::DEFAULT_MITRE_LIMIT;

  EXPECT_EQ(default_quadrant_segments, 8);
  EXPECT_DOUBLE_EQ(default_mitre_limit, 5.0);
  EXPECT_EQ(params.getQuadrantSegments(), default_quadrant_segments);
  EXPECT_EQ(params.getEndCapStyle(), BufferParameters::CAP_ROUND);
  EXPECT_EQ(params.getJoinStyle(), BufferParameters::JOIN_ROUND);
  EXPECT_DOUBLE_EQ(params.getMitreLimit(), default_mitre_limit);
  EXPECT_FALSE(params.isSingleSided());
}

TEST(BufferParametersTest, SingleArgConstructorSetsQuadrantSegments) {
  BufferParameters params_positive(16);
  EXPECT_EQ(params_positive.getQuadrantSegments(), 16);
  EXPECT_EQ(params_positive.getEndCapStyle(), BufferParameters::CAP_ROUND);
  EXPECT_EQ(params_positive.getJoinStyle(), BufferParameters::JOIN_ROUND);
  EXPECT_DOUBLE_EQ(params_positive.getMitreLimit(),
                   BufferParameters::DEFAULT_MITRE_LIMIT);
  EXPECT_FALSE(params_positive.isSingleSided());

  BufferParameters params_zero(0);
  EXPECT_EQ(params_zero.getQuadrantSegments(), 0);

  BufferParameters params_negative(-4);
  EXPECT_EQ(params_negative.getQuadrantSegments(), -4);
}

TEST(BufferParametersTest, TwoArgConstructorSetsQuadrantSegmentsAndCapStyle) {
  BufferParameters flat_params(4, BufferParameters::CAP_FLAT);
  EXPECT_EQ(flat_params.getQuadrantSegments(), 4);
  EXPECT_EQ(flat_params.getEndCapStyle(), BufferParameters::CAP_FLAT);
  EXPECT_EQ(flat_params.getJoinStyle(), BufferParameters::JOIN_ROUND);
  EXPECT_DOUBLE_EQ(flat_params.getMitreLimit(),
                   BufferParameters::DEFAULT_MITRE_LIMIT);
  EXPECT_FALSE(flat_params.isSingleSided());

  BufferParameters square_params(12, BufferParameters::CAP_SQUARE);
  EXPECT_EQ(square_params.getQuadrantSegments(), 12);
  EXPECT_EQ(square_params.getEndCapStyle(), BufferParameters::CAP_SQUARE);

  BufferParameters round_params(1, BufferParameters::CAP_ROUND);
  EXPECT_EQ(round_params.getQuadrantSegments(), 1);
  EXPECT_EQ(round_params.getEndCapStyle(), BufferParameters::CAP_ROUND);
}

TEST(BufferParametersTest, FourArgConstructorSetsAllParameters) {
  BufferParameters params(10, BufferParameters::CAP_SQUARE,
                          BufferParameters::JOIN_MITRE, 2.5);
  EXPECT_EQ(params.getQuadrantSegments(), 10);
  EXPECT_EQ(params.getEndCapStyle(), BufferParameters::CAP_SQUARE);
  EXPECT_EQ(params.getJoinStyle(), BufferParameters::JOIN_MITRE);
  EXPECT_DOUBLE_EQ(params.getMitreLimit(), 2.5);
  EXPECT_FALSE(params.isSingleSided());

  BufferParameters bevel_params(6, BufferParameters::CAP_FLAT,
                                BufferParameters::JOIN_BEVEL, 12.25);
  EXPECT_EQ(bevel_params.getQuadrantSegments(), 6);
  EXPECT_EQ(bevel_params.getEndCapStyle(), BufferParameters::CAP_FLAT);
  EXPECT_EQ(bevel_params.getJoinStyle(), BufferParameters::JOIN_BEVEL);
  EXPECT_DOUBLE_EQ(bevel_params.getMitreLimit(), 12.25);
  EXPECT_FALSE(bevel_params.isSingleSided());
}

TEST(BufferParametersTest, SettersMutateFieldsAndCopyPreservesState) {
  BufferParameters params;

  params.setQuadrantSegments(24);
  EXPECT_EQ(params.getQuadrantSegments(), 24);
  params.setQuadrantSegments(0);
  EXPECT_EQ(params.getQuadrantSegments(), 0);
  params.setQuadrantSegments(-2);
  EXPECT_EQ(params.getQuadrantSegments(), -2);
  params.setQuadrantSegments(14);

  params.setEndCapStyle(BufferParameters::CAP_FLAT);
  EXPECT_EQ(params.getEndCapStyle(), BufferParameters::CAP_FLAT);
  params.setEndCapStyle(BufferParameters::CAP_SQUARE);
  EXPECT_EQ(params.getEndCapStyle(), BufferParameters::CAP_SQUARE);
  params.setEndCapStyle(BufferParameters::CAP_ROUND);
  EXPECT_EQ(params.getEndCapStyle(), BufferParameters::CAP_ROUND);
  params.setEndCapStyle(BufferParameters::CAP_FLAT);

  params.setJoinStyle(BufferParameters::JOIN_MITRE);
  EXPECT_EQ(params.getJoinStyle(), BufferParameters::JOIN_MITRE);
  params.setJoinStyle(BufferParameters::JOIN_BEVEL);
  EXPECT_EQ(params.getJoinStyle(), BufferParameters::JOIN_BEVEL);
  params.setJoinStyle(BufferParameters::JOIN_ROUND);
  EXPECT_EQ(params.getJoinStyle(), BufferParameters::JOIN_ROUND);
  params.setJoinStyle(BufferParameters::JOIN_MITRE);

  params.setMitreLimit(0.0);
  EXPECT_DOUBLE_EQ(params.getMitreLimit(), 0.0);
  params.setMitreLimit(-1.5);
  EXPECT_DOUBLE_EQ(params.getMitreLimit(), -1.5);
  params.setMitreLimit(3.75);
  EXPECT_DOUBLE_EQ(params.getMitreLimit(), 3.75);

  params.setSingleSided(true);
  EXPECT_TRUE(params.isSingleSided());
  params.setSingleSided(false);
  EXPECT_FALSE(params.isSingleSided());
  params.setSingleSided(true);

  BufferParameters copied(params);
  EXPECT_EQ(copied.getQuadrantSegments(), 14);
  EXPECT_EQ(copied.getEndCapStyle(), BufferParameters::CAP_FLAT);
  EXPECT_EQ(copied.getJoinStyle(), BufferParameters::JOIN_MITRE);
  EXPECT_DOUBLE_EQ(copied.getMitreLimit(), 3.75);
  EXPECT_TRUE(copied.isSingleSided());

  BufferParameters assigned;
  assigned = copied;
  assigned.setQuadrantSegments(3);
  EXPECT_EQ(assigned.getQuadrantSegments(), 3);
  EXPECT_EQ(copied.getQuadrantSegments(), 14);
  EXPECT_EQ(assigned.getEndCapStyle(), BufferParameters::CAP_FLAT);
  EXPECT_EQ(assigned.getJoinStyle(), BufferParameters::JOIN_MITRE);
  EXPECT_DOUBLE_EQ(assigned.getMitreLimit(), 3.75);
  EXPECT_TRUE(assigned.isSingleSided());
}

TEST(BufferParametersTest, BufferDistanceErrorFormulaAndMonotonicity) {
  const double expected_qs1 =
      1.0 - std::cos(geos::algorithm::Angle::PI_OVER_2 / 2.0);
  EXPECT_DOUBLE_EQ(BufferParameters::bufferDistanceError(1), expected_qs1);

  const double expected_qs2 =
      1.0 - std::cos(geos::algorithm::Angle::PI_OVER_2 / 4.0);
  EXPECT_DOUBLE_EQ(BufferParameters::bufferDistanceError(2), expected_qs2);

  const double err8 = BufferParameters::bufferDistanceError(8);
  const double err12 = BufferParameters::bufferDistanceError(12);
  const double err18 = BufferParameters::bufferDistanceError(18);
  EXPECT_LT(err8, 0.02);
  EXPECT_LT(err12, 0.01);
  EXPECT_LT(err18, 0.001);

  // Cosine is an even function, so negative quadrant segments match positive.
  EXPECT_DOUBLE_EQ(BufferParameters::bufferDistanceError(-8), err8);

  const std::array<int, 7> kQuadSegs = {1, 2, 4, 8, 12, 18, 36};
  double prev_err = 1.0;
  for (int qs : kQuadSegs) {
    const double err = BufferParameters::bufferDistanceError(qs);
    EXPECT_GT(err, 0.0);
    EXPECT_LT(err, prev_err);
    prev_err = err;
  }
}

TEST(BufferParametersTest, BufferOpAppliesCapJoinMitreQuadrantsAndSingleSided) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto segment = factory->createLineString(
      std::make_unique<CoordinateSequence>(std::initializer_list<Coordinate>{
          Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)}));

  BufferParameters flat_params(8, BufferParameters::CAP_FLAT);
  std::unique_ptr<Geometry> flat_buf =
      BufferOp::bufferOp(segment.get(), 1.0, flat_params);
  EXPECT_NEAR(flat_buf->getArea(), 20.0, 1e-9);

  BufferParameters square_params(8, BufferParameters::CAP_SQUARE);
  std::unique_ptr<Geometry> square_buf =
      BufferOp::bufferOp(segment.get(), 1.0, square_params);
  EXPECT_NEAR(square_buf->getArea(), 24.0, 1e-9);

  BufferParameters coarse_round(2, BufferParameters::CAP_ROUND);
  BufferParameters fine_round(16, BufferParameters::CAP_ROUND);
  std::unique_ptr<Geometry> coarse_buf =
      BufferOp::bufferOp(segment.get(), 1.0, coarse_round);
  std::unique_ptr<Geometry> fine_buf =
      BufferOp::bufferOp(segment.get(), 1.0, fine_round);
  EXPECT_GT(fine_buf->getNumPoints(), coarse_buf->getNumPoints());
  EXPECT_GT(fine_buf->getArea(), coarse_buf->getArea());
  EXPECT_LT(fine_buf->getArea(), 24.0);

  // Sharp L-corner tests JOIN_MITRE vs JOIN_BEVEL and mitreLimit cutoff.
  auto corner = factory->createLineString(
      std::make_unique<CoordinateSequence>(std::initializer_list<Coordinate>{
          Coordinate(0.0, 0.0), Coordinate(10.0, 0.0),
          Coordinate(10.0, 10.0)}));
  BufferParameters mitre_params(8, BufferParameters::CAP_FLAT,
                                BufferParameters::JOIN_MITRE, 5.0);
  BufferParameters bevel_params(8, BufferParameters::CAP_FLAT,
                                BufferParameters::JOIN_BEVEL, 5.0);
  BufferParameters limited_mitre_params(8, BufferParameters::CAP_FLAT,
                                        BufferParameters::JOIN_MITRE, 1.0);
  BufferParameters bevel_fallback_mitre_params(
      8, BufferParameters::CAP_FLAT, BufferParameters::JOIN_MITRE, 0.5);
  std::unique_ptr<Geometry> mitre_buf =
      BufferOp::bufferOp(corner.get(), 1.0, mitre_params);
  std::unique_ptr<Geometry> bevel_buf =
      BufferOp::bufferOp(corner.get(), 1.0, bevel_params);
  std::unique_ptr<Geometry> limited_mitre_buf =
      BufferOp::bufferOp(corner.get(), 1.0, limited_mitre_params);
  std::unique_ptr<Geometry> bevel_fallback_mitre_buf =
      BufferOp::bufferOp(corner.get(), 1.0, bevel_fallback_mitre_params);
  EXPECT_NEAR(mitre_buf->getArea(), 40.0, 1e-9);
  EXPECT_NEAR(bevel_buf->getArea(), 39.5, 1e-9);
  EXPECT_GT(mitre_buf->getArea(), limited_mitre_buf->getArea());
  EXPECT_GT(limited_mitre_buf->getArea(), bevel_buf->getArea());
  // For a 90-degree corner at distance 1, bevelDist = sqrt(2)/2 < 1.0 <
  // sqrt(2), so mitreLimit = 1.0 builds a limited-mitre bevel rather than a
  // plain bevel.
  EXPECT_NEAR(limited_mitre_buf->getArea(), 37.0 + 2.0 * std::sqrt(2.0), 1e-9);
  // When mitreLimit <= bevelDist, JOIN_MITRE falls back to a plain bevel join.
  EXPECT_NEAR(bevel_fallback_mitre_buf->getArea(), bevel_buf->getArea(), 1e-9);

  // Single-sided buffer builds only on left (positive) or right (negative),
  // and forces flat end caps even when endCapStyle is CAP_ROUND.
  BufferParameters single_sided;
  single_sided.setSingleSided(true);
  std::unique_ptr<Geometry> left_buf =
      BufferOp::bufferOp(segment.get(), 2.0, single_sided);
  std::unique_ptr<Geometry> right_buf =
      BufferOp::bufferOp(segment.get(), -2.0, single_sided);
  const Envelope* left_env = left_buf->getEnvelopeInternal();
  const Envelope* right_env = right_buf->getEnvelopeInternal();
  EXPECT_NEAR(left_buf->getArea(), 20.0, 1e-6);
  EXPECT_NEAR(right_buf->getArea(), 20.0, 1e-6);
  EXPECT_NEAR(left_env->getMinX(), 0.0, 1e-6);
  EXPECT_NEAR(left_env->getMaxX(), 10.0, 1e-6);
  EXPECT_GE(left_env->getMinY(), -1e-9);
  EXPECT_NEAR(left_env->getMaxY(), 2.0, 1e-6);
  EXPECT_NEAR(right_env->getMinX(), 0.0, 1e-6);
  EXPECT_NEAR(right_env->getMaxX(), 10.0, 1e-6);
  EXPECT_NEAR(right_env->getMinY(), -2.0, 1e-6);
  EXPECT_LE(right_env->getMaxY(), 1e-9);
}

}  // namespace
}  // namespace geos_google
