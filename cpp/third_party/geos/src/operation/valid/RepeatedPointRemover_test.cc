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

// Unit tests for third_party/geos/src/operation/valid/RepeatedPointRemover.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - With GEOS_COORDSEQ_PADZ, XYM sequences have stride 4; read M values via
//   2-arg CoordinateSequence::getAt(i, coord_xym) or getOrdinate(..., M).
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/valid/RepeatedPointRemover.cpp

#include "third_party/geos/include/geos/operation/valid/RepeatedPointRemover.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/CompoundCurve.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/SimpleCurve.h"
#include "third_party/geos/include/geos/util.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::DoubleInfinity;
using geos::DoubleNegInfinity;
using geos::DoubleNotANumber;
using geos::detail::down_cast;
using geos::geom::CircularString;
using geos::geom::CompoundCurve;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::MultiLineString;
using geos::geom::MultiPoint;
using geos::geom::MultiPolygon;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::SimpleCurve;
using geos::operation::valid::RepeatedPointRemover;
using geos::util::IllegalArgumentException;
using geos::util::UnsupportedOperationException;

void ExpectCoordsXY(const CoordinateSequence& actual,
                    std::initializer_list<CoordinateXY> expected) {
  ASSERT_EQ(actual.size(), expected.size());
  std::size_t idx = 0;
  for (const CoordinateXY& exp : expected) {
    EXPECT_DOUBLE_EQ(actual.getX(idx), exp.x) << "index=" << idx;
    EXPECT_DOUBLE_EQ(actual.getY(idx), exp.y) << "index=" << idx;
    ++idx;
  }
}

void ExpectCoordsXYZ(const CoordinateSequence& actual,
                     std::initializer_list<Coordinate> expected) {
  EXPECT_TRUE(actual.hasZ());
  EXPECT_FALSE(actual.hasM());
  ASSERT_EQ(actual.size(), expected.size());
  std::size_t idx = 0;
  for (const Coordinate& exp : expected) {
    const Coordinate& got = actual.getAt<Coordinate>(idx);
    EXPECT_DOUBLE_EQ(got.x, exp.x) << "index=" << idx;
    EXPECT_DOUBLE_EQ(got.y, exp.y) << "index=" << idx;
    if (std::isnan(exp.z)) {
      EXPECT_TRUE(std::isnan(got.z)) << "index=" << idx;
    } else {
      EXPECT_DOUBLE_EQ(got.z, exp.z) << "index=" << idx;
    }
    ++idx;
  }
}

void ExpectCoordsXYM(const CoordinateSequence& actual,
                     std::initializer_list<CoordinateXYM> expected) {
  EXPECT_FALSE(actual.hasZ());
  EXPECT_TRUE(actual.hasM());
  ASSERT_EQ(actual.size(), expected.size());
  std::size_t idx = 0;
  for (const CoordinateXYM& exp : expected) {
    CoordinateXYM got;
    actual.getAt(idx, got);
    EXPECT_DOUBLE_EQ(got.x, exp.x) << "index=" << idx;
    EXPECT_DOUBLE_EQ(got.y, exp.y) << "index=" << idx;
    EXPECT_DOUBLE_EQ(got.m, exp.m) << "index=" << idx;
    EXPECT_DOUBLE_EQ(actual.getOrdinate(idx, CoordinateSequence::M), exp.m)
        << "index=" << idx;
    EXPECT_TRUE(std::isnan(actual.getOrdinate(idx, CoordinateSequence::Z)))
        << "index=" << idx;
    ++idx;
  }
}

void ExpectCoordsXYZM(const CoordinateSequence& actual,
                      std::initializer_list<CoordinateXYZM> expected) {
  EXPECT_TRUE(actual.hasZ());
  EXPECT_TRUE(actual.hasM());
  ASSERT_EQ(actual.size(), expected.size());
  std::size_t idx = 0;
  for (const CoordinateXYZM& exp : expected) {
    const CoordinateXYZM& got = actual.getAt<CoordinateXYZM>(idx);
    EXPECT_DOUBLE_EQ(got.x, exp.x) << "index=" << idx;
    EXPECT_DOUBLE_EQ(got.y, exp.y) << "index=" << idx;
    EXPECT_DOUBLE_EQ(got.z, exp.z) << "index=" << idx;
    EXPECT_DOUBLE_EQ(got.m, exp.m) << "index=" << idx;
    ++idx;
  }
}

class RepeatedPointRemoverTest : public ::testing::Test {
 protected:
  RepeatedPointRemoverTest() : factory_(GeometryFactory::create()) {}

  std::unique_ptr<LineString> MakeLine(
      std::initializer_list<CoordinateXY> coords) const {
    return factory_->createLineString(CoordinateSequence(coords));
  }

  std::unique_ptr<LinearRing> MakeRing(
      std::initializer_list<CoordinateXY> coords) const {
    return factory_->createLinearRing(CoordinateSequence(coords));
  }

  std::unique_ptr<Polygon> MakePolygon(
      std::initializer_list<CoordinateXY> shell,
      std::initializer_list<std::initializer_list<CoordinateXY>> holes = {})
      const {
    std::vector<std::unique_ptr<LinearRing>> hole_rings;
    hole_rings.reserve(holes.size());
    for (const auto& hole : holes) {
      hole_rings.push_back(MakeRing(hole));
    }
    return factory_->createPolygon(MakeRing(shell), std::move(hole_rings));
  }

  GeometryFactory::Ptr factory_;
};

TEST_F(RepeatedPointRemoverTest, RemoveRepeatedPointsEmptySequences) {
  for (double tol : {0.0, 1.0}) {
    const CoordinateSequence empty_xy = CoordinateSequence::XY(0);
    auto res_xy = RepeatedPointRemover::removeRepeatedPoints(&empty_xy, tol);
    EXPECT_TRUE(res_xy->isEmpty());
    EXPECT_FALSE(res_xy->hasZ());
    EXPECT_FALSE(res_xy->hasM());

    const CoordinateSequence empty_xyz = CoordinateSequence::XYZ(0);
    auto res_xyz = RepeatedPointRemover::removeRepeatedPoints(&empty_xyz, tol);
    EXPECT_TRUE(res_xyz->isEmpty());
    EXPECT_TRUE(res_xyz->hasZ());
    EXPECT_FALSE(res_xyz->hasM());

    const CoordinateSequence empty_xym = CoordinateSequence::XYM(0);
    auto res_xym = RepeatedPointRemover::removeRepeatedPoints(&empty_xym, tol);
    EXPECT_TRUE(res_xym->isEmpty());
    EXPECT_FALSE(res_xym->hasZ());
    EXPECT_TRUE(res_xym->hasM());

    const CoordinateSequence empty_xyzm = CoordinateSequence::XYZM(0);
    auto res_xyzm =
        RepeatedPointRemover::removeRepeatedPoints(&empty_xyzm, tol);
    EXPECT_TRUE(res_xyzm->isEmpty());
    EXPECT_TRUE(res_xyzm->hasZ());
    EXPECT_TRUE(res_xyzm->hasM());
  }
}

TEST_F(RepeatedPointRemoverTest, RemoveRepeatedPointsZeroToleranceAllDims) {
  // XY: consecutive repeats collapsed, non-consecutive (ring closure) kept.
  const CoordinateSequence seq_xy{
      CoordinateXY{0.0, 0.0}, CoordinateXY{0.0, 0.0}, CoordinateXY{1.0, 2.0},
      CoordinateXY{1.0, 2.0}, CoordinateXY{1.0, 2.0}, CoordinateXY{3.0, 4.0},
      CoordinateXY{0.0, 0.0}, CoordinateXY{0.0, 0.0},
  };
  auto out_xy = RepeatedPointRemover::removeRepeatedPoints(&seq_xy);
  EXPECT_FALSE(out_xy->hasZ());
  EXPECT_FALSE(out_xy->hasM());
  ExpectCoordsXY(*out_xy, {{0.0, 0.0}, {1.0, 2.0}, {3.0, 4.0}, {0.0, 0.0}});

  // XYZ: 2D duplicate keeps first Z value.
  const CoordinateSequence seq_xyz{
      Coordinate{1.0, 2.0, 10.0},
      Coordinate{1.0, 2.0, 20.0},
      Coordinate{3.0, 4.0, 30.0},
  };
  auto out_xyz = RepeatedPointRemover::removeRepeatedPoints(&seq_xyz);
  ExpectCoordsXYZ(*out_xyz, {{1.0, 2.0, 10.0}, {3.0, 4.0, 30.0}});

  // XYM: 2D duplicate keeps first M value.
  const CoordinateSequence seq_xym{
      CoordinateXYM{1.0, 2.0, 5.5},
      CoordinateXYM{1.0, 2.0, 9.5},
      CoordinateXYM{4.0, 5.0, 12.5},
  };
  auto out_xym = RepeatedPointRemover::removeRepeatedPoints(&seq_xym);
  ExpectCoordsXYM(*out_xym, {{1.0, 2.0, 5.5}, {4.0, 5.0, 12.5}});

  // XYZM: 2D duplicate keeps first Z and M values.
  const CoordinateSequence seq_xyzm{
      CoordinateXYZM{0.0, 0.0, 1.0, 2.0},
      CoordinateXYZM{0.0, 0.0, 9.0, 8.0},
      CoordinateXYZM{2.0, 3.0, 4.0, 5.0},
  };
  auto out_xyzm = RepeatedPointRemover::removeRepeatedPoints(&seq_xyzm);
  ExpectCoordsXYZM(*out_xyzm, {{0.0, 0.0, 1.0, 2.0}, {2.0, 3.0, 4.0, 5.0}});
}

TEST_F(RepeatedPointRemoverTest, RemoveRepeatedPointsNonFiniteBehavior) {
  // removeRepeatedPoints preserves NaNs (NaN != NaN) and deduplicates exact
  // Inf pairs.
  const CoordinateSequence seq{
      CoordinateXY{DoubleNotANumber, 0.0},
      CoordinateXY{DoubleNotANumber, 0.0},
      CoordinateXY{DoubleInfinity, 1.0},
      CoordinateXY{DoubleInfinity, 1.0},
      CoordinateXY{5.0, 6.0},
  };
  for (double tol : {0.0, 1.0}) {
    SCOPED_TRACE(tol);
    auto out = RepeatedPointRemover::removeRepeatedPoints(&seq, tol);
    ASSERT_EQ(out->size(), 4u);
    EXPECT_TRUE(std::isnan(out->getX(0)));
    EXPECT_DOUBLE_EQ(out->getY(0), 0.0);
    EXPECT_TRUE(std::isnan(out->getX(1)));
    EXPECT_DOUBLE_EQ(out->getY(1), 0.0);
    EXPECT_EQ(out->getX(2), DoubleInfinity);
    EXPECT_DOUBLE_EQ(out->getY(2), 1.0);
    EXPECT_DOUBLE_EQ(out->getX(3), 5.0);
    EXPECT_DOUBLE_EQ(out->getY(3), 6.0);
  }
}

TEST_F(RepeatedPointRemoverTest, RemoveRepeatedPointsWithTolerance) {
  // Boundary: distance == tolerance is removed; distance > tolerance is kept;
  // greedy comparison uses the last retained point, not the skipped point.
  const CoordinateSequence seq_xy{
      CoordinateXY{0.0, 0.0},
      CoordinateXY{0.6, 0.0},    // dist 0.6 <= 1.0 -> drop
      CoordinateXY{1.0, 0.0},    // dist 1.0 <= 1.0 from (0,0) -> drop
      CoordinateXY{1.001, 0.0},  // dist 1.001 > 1.0 -> keep (new prev)
      CoordinateXY{2.001, 0.0},  // dist 1.0 <= 1.0 from (1.001,0) -> drop
      CoordinateXY{2.5, 0.0},    // dist 1.499 > 1.0 -> keep
  };
  auto out_xy = RepeatedPointRemover::removeRepeatedPoints(&seq_xy, 1.0);
  ExpectCoordsXY(*out_xy, {{0.0, 0.0}, {1.001, 0.0}, {2.5, 0.0}});

  const CoordinateSequence seq_xyz{
      Coordinate{0.0, 0.0, 10.0},
      Coordinate{0.3, 0.4, 20.0},  // dist 0.5 <= 0.5 -> drop
      Coordinate{1.0, 0.0, 30.0},  // dist 1.0 > 0.5 -> keep
  };
  auto out_xyz = RepeatedPointRemover::removeRepeatedPoints(&seq_xyz, 0.5);
  ExpectCoordsXYZ(*out_xyz, {{0.0, 0.0, 10.0}, {1.0, 0.0, 30.0}});

  const CoordinateSequence seq_xym{
      CoordinateXYM{0.0, 0.0, 1.0},
      CoordinateXYM{0.1, 0.1, 2.0},  // within 0.5 -> drop
      CoordinateXYM{2.0, 0.0, 3.0},  // keep
  };
  auto out_xym = RepeatedPointRemover::removeRepeatedPoints(&seq_xym, 0.5);
  ExpectCoordsXYM(*out_xym, {{0.0, 0.0, 1.0}, {2.0, 0.0, 3.0}});

  const CoordinateSequence seq_xyzm{
      CoordinateXYZM{0.0, 0.0, 1.0, 10.0},
      CoordinateXYZM{0.2, 0.2, 2.0, 20.0},  // within 0.5 -> drop
      CoordinateXYZM{3.0, 4.0, 3.0, 30.0},  // keep
  };
  auto out_xyzm = RepeatedPointRemover::removeRepeatedPoints(&seq_xyzm, 0.5);
  ExpectCoordsXYZM(*out_xyzm, {{0.0, 0.0, 1.0, 10.0}, {3.0, 4.0, 3.0, 30.0}});
}

TEST_F(RepeatedPointRemoverTest, RemoveRepeatedAndInvalidPointsEmptySequences) {
  const CoordinateSequence empty_xy = CoordinateSequence::XY(0);
  auto out_xy =
      RepeatedPointRemover::removeRepeatedAndInvalidPoints(&empty_xy, 0.0);
  EXPECT_TRUE(out_xy->isEmpty());
  EXPECT_EQ(out_xy->getDimension(), 2u);

  const CoordinateSequence empty_xyz = CoordinateSequence::XYZ(0);
  auto out_xyz =
      RepeatedPointRemover::removeRepeatedAndInvalidPoints(&empty_xyz, 1.0);
  EXPECT_TRUE(out_xyz->isEmpty());
  EXPECT_EQ(out_xyz->getDimension(), 3u);

  const CoordinateSequence empty_xym = CoordinateSequence::XYM(0);
  auto out_xym =
      RepeatedPointRemover::removeRepeatedAndInvalidPoints(&empty_xym, 1.0);
  EXPECT_TRUE(out_xym->isEmpty());
  EXPECT_EQ(out_xym->getDimension(), 3u);

  const CoordinateSequence empty_xyzm = CoordinateSequence::XYZM(0);
  auto out_xyzm =
      RepeatedPointRemover::removeRepeatedAndInvalidPoints(&empty_xyzm, 0.0);
  EXPECT_TRUE(out_xyzm->isEmpty());
  EXPECT_EQ(out_xyzm->getDimension(), 4u);
}

TEST_F(RepeatedPointRemoverTest,
       RemoveRepeatedAndInvalidPointsFiltersAllCases) {
  // Leading invalid, interior invalid separating identical valid points,
  // and trailing invalid points are all dropped.
  const CoordinateSequence mixed_xy{
      CoordinateXY{DoubleNotANumber, 0.0},
      CoordinateXY{DoubleInfinity, 1.0},
      CoordinateXY{0.0, DoubleNegInfinity},
      CoordinateXY{1.0, 2.0},
      CoordinateXY{DoubleNotANumber, 9.0},
      CoordinateXY{1.0, 2.0},
      CoordinateXY{3.0, 4.0},
      CoordinateXY{3.0, DoubleInfinity},
  };
  auto out_mixed =
      RepeatedPointRemover::removeRepeatedAndInvalidPoints(&mixed_xy);
  ExpectCoordsXY(*out_mixed, {{1.0, 2.0}, {3.0, 4.0}});

  // All-invalid input collapses to empty sequence.
  const CoordinateSequence all_invalid{
      CoordinateXY{DoubleNotANumber, DoubleNotANumber},
      CoordinateXY{DoubleInfinity, 0.0},
      CoordinateXY{0.0, DoubleNegInfinity},
  };
  auto out_empty =
      RepeatedPointRemover::removeRepeatedAndInvalidPoints(&all_invalid);
  EXPECT_TRUE(out_empty->isEmpty());

  // Valid XY with NaN Z keeps the coordinate in XYZ.
  const CoordinateSequence xyz_seq{
      Coordinate{DoubleNotANumber, 0.0, 1.0},
      Coordinate{1.0, 2.0, DoubleNotANumber},
      Coordinate{1.1, 2.1, 5.0},  // dist ~0.141 <= 0.5 -> dropped
      Coordinate{4.0, 5.0, 9.0},
  };
  auto out_xyz =
      RepeatedPointRemover::removeRepeatedAndInvalidPoints(&xyz_seq, 0.5);
  ExpectCoordsXYZ(*out_xyz, {{1.0, 2.0, DoubleNotANumber}, {4.0, 5.0, 9.0}});

  // XYM and XYZM with invalid + tolerance filtering.
  const CoordinateSequence xym_seq{
      CoordinateXYM{0.0, DoubleNotANumber, 1.0},
      CoordinateXYM{0.0, 0.0, 2.0},
      CoordinateXYM{0.2, 0.0, 3.0},  // within 0.5 -> drop
      CoordinateXYM{2.0, 0.0, 4.0},
  };
  auto out_xym =
      RepeatedPointRemover::removeRepeatedAndInvalidPoints(&xym_seq, 0.5);
  ExpectCoordsXYM(*out_xym, {{0.0, 0.0, 2.0}, {2.0, 0.0, 4.0}});

  const CoordinateSequence xyzm_seq{
      CoordinateXYZM{DoubleInfinity, 0.0, 1.0, 2.0},
      CoordinateXYZM{0.0, 0.0, 3.0, 4.0},
      CoordinateXYZM{0.0, 0.3, 5.0, 6.0},  // within 0.5 -> drop
      CoordinateXYZM{0.0, 2.0, 7.0, 8.0},
  };
  auto out_xyzm =
      RepeatedPointRemover::removeRepeatedAndInvalidPoints(&xyzm_seq, 0.5);
  ExpectCoordsXYZM(*out_xyzm, {{0.0, 0.0, 3.0, 4.0}, {0.0, 2.0, 7.0, 8.0}});
}

TEST_F(RepeatedPointRemoverTest, RemoveRepeatedPointsPointAndMultiPoint) {
  const auto empty_point = factory_->createPoint();
  auto res_empty_pt =
      RepeatedPointRemover::removeRepeatedPoints(empty_point.get());
  EXPECT_TRUE(res_empty_pt->isEmpty());
  EXPECT_EQ(res_empty_pt->getGeometryTypeId(), geos::geom::GEOS_POINT);

  const auto pt_xyzm =
      factory_->createPoint(CoordinateXYZM(1.0, 2.0, 3.0, 4.0));
  auto res_pt_xyzm =
      RepeatedPointRemover::removeRepeatedPoints(pt_xyzm.get(), 10.0);
  ASSERT_EQ(res_pt_xyzm->getGeometryTypeId(), geos::geom::GEOS_POINT);
  ExpectCoordsXYZM(*res_pt_xyzm->getCoordinates(), {{1.0, 2.0, 3.0, 4.0}});

  // Point with NaN coordinate is returned directly via coordinates->clone().
  const auto nan_pt =
      factory_->createPoint(CoordinateXY(DoubleNotANumber, 1.0));
  auto res_nan_pt = RepeatedPointRemover::removeRepeatedPoints(nan_pt.get());
  EXPECT_FALSE(res_nan_pt->isEmpty());
  EXPECT_TRUE(std::isnan(res_nan_pt->getCoordinate()->x));

  const auto empty_mp = factory_->createMultiPoint();
  auto res_empty_mp =
      RepeatedPointRemover::removeRepeatedPoints(empty_mp.get());
  EXPECT_TRUE(res_empty_mp->isEmpty());
  EXPECT_EQ(res_empty_mp->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);

  // MultiPoint keeps non-empty points (even duplicates) and drops empty child.
  std::vector<std::unique_ptr<Point>> pts;
  pts.push_back(factory_->createPoint(CoordinateXY(1.0, 2.0)));
  pts.push_back(factory_->createPoint());
  pts.push_back(factory_->createPoint(CoordinateXY(1.0, 2.0)));
  const auto mp = factory_->createMultiPoint(std::move(pts));
  auto res_mp = RepeatedPointRemover::removeRepeatedPoints(mp.get());
  ASSERT_EQ(res_mp->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(res_mp->getNumGeometries(), 2u);
}

TEST_F(RepeatedPointRemoverTest, RemoveRepeatedPointsLineStringVariants) {
  const auto empty_line = factory_->createLineString();
  auto res_empty = RepeatedPointRemover::removeRepeatedPoints(empty_line.get());
  EXPECT_TRUE(res_empty->isEmpty());
  EXPECT_EQ(res_empty->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);

  // 2-point LineString is never shortened below minLength = 2.
  const auto two_identical = MakeLine({{1.0, 1.0}, {1.0, 1.0}});
  auto res_two_ident =
      RepeatedPointRemover::removeRepeatedPoints(two_identical.get());
  ExpectCoordsXY(*res_two_ident->getCoordinates(), {{1.0, 1.0}, {1.0, 1.0}});

  const auto two_within_tol = MakeLine({{0.0, 0.0}, {0.3, 0.0}});
  auto res_two_tol =
      RepeatedPointRemover::removeRepeatedPoints(two_within_tol.get(), 1.0);
  ExpectCoordsXY(*res_two_tol->getCoordinates(), {{0.0, 0.0}, {0.3, 0.0}});

  // 3-point LineString collapsing to 1 point is fluffed back up to 2 points
  // by appending origEndCoord.
  const auto three_identical = MakeLine({{2.0, 3.0}, {2.0, 3.0}, {2.0, 3.0}});
  auto res_fluffed_zero =
      RepeatedPointRemover::removeRepeatedPoints(three_identical.get());
  ExpectCoordsXY(*res_fluffed_zero->getCoordinates(), {{2.0, 3.0}, {2.0, 3.0}});

  const auto three_within_tol = MakeLine({{0.0, 0.0}, {0.2, 0.0}, {0.4, 0.0}});
  auto res_fluffed_tol =
      RepeatedPointRemover::removeRepeatedPoints(three_within_tol.get(), 1.0);
  ExpectCoordsXY(*res_fluffed_tol->getCoordinates(), {{0.0, 0.0}, {0.4, 0.0}});

  // Endpoint repair: (1.0, 0.0) is within tolerance of origEndCoord (1.2, 0.0),
  // so (1.0, 0.0) is popped and origEndCoord (1.2, 0.0) is restored.
  const auto repair_line = MakeLine({{0.0, 0.0}, {1.0, 0.0}, {1.2, 0.0}});
  auto res_repaired =
      RepeatedPointRemover::removeRepeatedPoints(repair_line.get(), 1.0);
  ExpectCoordsXY(*res_repaired->getCoordinates(), {{0.0, 0.0}, {1.2, 0.0}});

  // Leading and interior invalid/repeated points are removed.
  const auto dirty_line = MakeLine({
      {DoubleNotANumber, 0.0},
      {0.0, 0.0},
      {0.0, 0.0},
      {DoubleInfinity, 1.0},
      {2.0, 0.0},
      {2.0, 0.0},
      {5.0, 0.0},
  });
  auto res_clean = RepeatedPointRemover::removeRepeatedPoints(dirty_line.get());
  ExpectCoordsXY(*res_clean->getCoordinates(),
                 {{0.0, 0.0}, {2.0, 0.0}, {5.0, 0.0}});

  // All-invalid 3-point LineString collapses to empty LineString.
  const auto all_invalid_line = MakeLine({
      {DoubleInfinity, 0.0},
      {DoubleNotANumber, 1.0},
      {DoubleNegInfinity, 2.0},
  });
  auto res_invalid_empty =
      RepeatedPointRemover::removeRepeatedPoints(all_invalid_line.get());
  EXPECT_TRUE(res_invalid_empty->isEmpty());
  EXPECT_EQ(res_invalid_empty->getGeometryTypeId(),
            geos::geom::GEOS_LINESTRING);

  // XYZM LineString retains Z and M ordinates.
  const auto xyzm_line = factory_->createLineString(CoordinateSequence{
      CoordinateXYZM{0.0, 0.0, 1.0, 10.0},
      CoordinateXYZM{0.0, 0.0, 9.0, 90.0},
      CoordinateXYZM{5.0, 0.0, 2.0, 20.0},
  });
  auto res_xyzm_line =
      RepeatedPointRemover::removeRepeatedPoints(xyzm_line.get());
  ExpectCoordsXYZM(*res_xyzm_line->getCoordinates(),
                   {{0.0, 0.0, 1.0, 10.0}, {5.0, 0.0, 2.0, 20.0}});
}

TEST_F(RepeatedPointRemoverTest, RemoveRepeatedPointsLinearRingAndPolygon) {
  const auto empty_ring = factory_->createLinearRing();
  auto res_empty_ring =
      RepeatedPointRemover::removeRepeatedPoints(empty_ring.get());
  EXPECT_TRUE(res_empty_ring->isEmpty());
  EXPECT_EQ(res_empty_ring->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);

  const auto min_ring = MakeRing({{0.0, 0.0}, {1.0, 0.0}, {0.0, 0.0}});
  auto res_min_ring =
      RepeatedPointRemover::removeRepeatedPoints(min_ring.get(), 10.0);
  EXPECT_EQ(res_min_ring->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  ExpectCoordsXY(*res_min_ring->getCoordinates(),
                 {{0.0, 0.0}, {1.0, 0.0}, {0.0, 0.0}});

  const auto dirty_ring = MakeRing(
      {{0.0, 0.0}, {4.0, 0.0}, {4.0, 0.0}, {4.0, 4.0}, {0.0, 4.0}, {0.0, 0.0}});
  auto res_ring = RepeatedPointRemover::removeRepeatedPoints(dirty_ring.get());
  EXPECT_EQ(res_ring->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  ExpectCoordsXY(*res_ring->getCoordinates(),
                 {{0.0, 0.0}, {4.0, 0.0}, {4.0, 4.0}, {0.0, 4.0}, {0.0, 0.0}});

  const auto empty_poly = factory_->createPolygon();
  auto res_empty_poly =
      RepeatedPointRemover::removeRepeatedPoints(empty_poly.get());
  EXPECT_TRUE(res_empty_poly->isEmpty());
  EXPECT_EQ(res_empty_poly->getGeometryTypeId(), geos::geom::GEOS_POLYGON);

  // Polygon with repeated vertices in shell and hole.
  const auto poly_with_hole = MakePolygon({{0.0, 0.0},
                                           {10.0, 0.0},
                                           {10.0, 0.0},
                                           {10.0, 10.0},
                                           {0.0, 10.0},
                                           {0.0, 0.0}},
                                          {{{2.0, 2.0},
                                            {4.0, 2.0},
                                            {4.0, 4.0},
                                            {4.0, 4.0},
                                            {2.0, 4.0},
                                            {2.0, 2.0}}});
  auto res_poly =
      RepeatedPointRemover::removeRepeatedPoints(poly_with_hole.get());
  const auto* out_poly = down_cast<const Polygon*>(res_poly.get());
  ExpectCoordsXY(
      *out_poly->getExteriorRing()->getCoordinatesRO(),
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});
  ASSERT_EQ(out_poly->getNumInteriorRing(), 1u);
  ExpectCoordsXY(*out_poly->getInteriorRingN(0)->getCoordinatesRO(),
                 {{2.0, 2.0}, {4.0, 2.0}, {4.0, 4.0}, {2.0, 4.0}, {2.0, 2.0}});

  // Tolerance endpoint repair on polygon shell: (0.1, 0.1) is popped so the
  // ring closes back to origEndCoord (0, 0).
  const auto poly_endpoint_repair = MakePolygon(
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.1, 0.1}, {0.0, 0.0}});
  auto res_repaired_poly = RepeatedPointRemover::removeRepeatedPoints(
      poly_endpoint_repair.get(), 1.0);
  const auto* repaired_poly =
      down_cast<const Polygon*>(res_repaired_poly.get());
  ExpectCoordsXY(*repaired_poly->getExteriorRing()->getCoordinatesRO(),
                 {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 0.0}});

  // Polygon with all-invalid hole drops the hole while preserving shell.
  const auto poly_invalid_hole = MakePolygon(
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}},
      {{{DoubleInfinity, 0.0},
        {DoubleNotANumber, 1.0},
        {DoubleNegInfinity, 2.0},
        {DoubleInfinity, 0.0}}});
  auto res_dropped_hole =
      RepeatedPointRemover::removeRepeatedPoints(poly_invalid_hole.get());
  const auto* poly_no_hole = down_cast<const Polygon*>(res_dropped_hole.get());
  EXPECT_FALSE(poly_no_hole->isEmpty());
  EXPECT_EQ(poly_no_hole->getNumInteriorRing(), 0u);

  // Polygon with all-invalid shell becomes empty Polygon.
  const auto poly_invalid_shell = MakePolygon({{DoubleInfinity, 0.0},
                                               {DoubleNotANumber, 1.0},
                                               {DoubleNegInfinity, 2.0},
                                               {DoubleInfinity, 0.0}});
  auto res_empty_shell_poly =
      RepeatedPointRemover::removeRepeatedPoints(poly_invalid_shell.get());
  EXPECT_TRUE(res_empty_shell_poly->isEmpty());
  EXPECT_EQ(res_empty_shell_poly->getGeometryTypeId(),
            geos::geom::GEOS_POLYGON);

  // Ring collapsing to 1 point is fluffed to 2 points, which fails
  // LinearRing::MINIMUM_VALID_SIZE (3) validation.
  const auto collapsing_poly =
      MakePolygon({{0.0, 0.0}, {0.1, 0.0}, {0.1, 0.1}, {0.0, 0.0}});
  EXPECT_THROW(
      RepeatedPointRemover::removeRepeatedPoints(collapsing_poly.get(), 1.0),
      IllegalArgumentException);
}

TEST_F(RepeatedPointRemoverTest, RemoveRepeatedPointsCollections) {
  EXPECT_TRUE(RepeatedPointRemover::removeRepeatedPoints(
                  factory_->createMultiLineString().get())
                  ->isEmpty());
  EXPECT_TRUE(RepeatedPointRemover::removeRepeatedPoints(
                  factory_->createMultiPolygon().get())
                  ->isEmpty());
  EXPECT_TRUE(RepeatedPointRemover::removeRepeatedPoints(
                  factory_->createGeometryCollection().get())
                  ->isEmpty());

  // MultiLineString cleans lines and drops components that become empty.
  std::vector<std::unique_ptr<LineString>> lines;
  lines.push_back(MakeLine({{0.0, 0.0}, {1.0, 0.0}, {1.0, 0.0}, {2.0, 0.0}}));
  lines.push_back(MakeLine({{DoubleInfinity, 0.0},
                            {DoubleNotANumber, 1.0},
                            {DoubleNegInfinity, 2.0}}));
  const auto mls = factory_->createMultiLineString(std::move(lines));
  auto res_mls = RepeatedPointRemover::removeRepeatedPoints(mls.get());
  ASSERT_EQ(res_mls->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  ASSERT_EQ(res_mls->getNumGeometries(), 1u);
  ExpectCoordsXY(*res_mls->getGeometryN(0)->getCoordinates(),
                 {{0.0, 0.0}, {1.0, 0.0}, {2.0, 0.0}});

  // MultiPolygon cleans polygons and drops all-invalid polygons.
  std::vector<std::unique_ptr<Polygon>> polys;
  polys.push_back(MakePolygon(
      {{0.0, 0.0}, {5.0, 0.0}, {5.0, 0.0}, {5.0, 5.0}, {0.0, 0.0}}));
  polys.push_back(MakePolygon({{DoubleInfinity, 0.0},
                               {DoubleNotANumber, 1.0},
                               {DoubleNegInfinity, 2.0},
                               {DoubleInfinity, 0.0}}));
  const auto mp = factory_->createMultiPolygon(std::move(polys));
  auto res_mp = RepeatedPointRemover::removeRepeatedPoints(mp.get());
  ASSERT_EQ(res_mp->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  ASSERT_EQ(res_mp->getNumGeometries(), 1u);
  ExpectCoordsXY(*res_mp->getGeometryN(0)->getCoordinates(),
                 {{0.0, 0.0}, {5.0, 0.0}, {5.0, 5.0}, {0.0, 0.0}});

  // Nested heterogeneous GeometryCollection.
  std::vector<std::unique_ptr<Geometry>> inner;
  inner.push_back(MakeLine({{0.0, 0.0}, {0.0, 0.0}, {3.0, 3.0}}));
  std::vector<std::unique_ptr<Geometry>> outer;
  outer.push_back(factory_->createPoint(CoordinateXY(1.0, 1.0)));
  outer.push_back(factory_->createGeometryCollection(std::move(inner)));
  const auto gc = factory_->createGeometryCollection(std::move(outer));
  auto res_gc = RepeatedPointRemover::removeRepeatedPoints(gc.get());
  ASSERT_EQ(res_gc->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(res_gc->getNumGeometries(), 2u);
  const Geometry* res_inner = res_gc->getGeometryN(1);
  ASSERT_EQ(res_inner->getNumGeometries(), 1u);
  ExpectCoordsXY(*res_inner->getGeometryN(0)->getCoordinates(),
                 {{0.0, 0.0}, {3.0, 3.0}});
}

TEST_F(RepeatedPointRemoverTest, RemoveRepeatedPointsThrowsOnCurvedGeometries) {
  const auto empty_circular = factory_->createCircularString(false, false);
  EXPECT_THROW(RepeatedPointRemover::removeRepeatedPoints(empty_circular.get()),
               UnsupportedOperationException);

  auto circular = factory_->createCircularString(CoordinateSequence{
      CoordinateXY{0.0, 0.0}, CoordinateXY{1.0, 1.0}, CoordinateXY{2.0, 0.0}});
  EXPECT_THROW(RepeatedPointRemover::removeRepeatedPoints(circular.get()),
               UnsupportedOperationException);

  std::vector<std::unique_ptr<SimpleCurve>> curves;
  curves.push_back(std::move(circular));
  const auto compound = factory_->createCompoundCurve(std::move(curves));
  EXPECT_THROW(RepeatedPointRemover::removeRepeatedPoints(compound.get()),
               UnsupportedOperationException);
}

}  // namespace
}  // namespace geos_google
