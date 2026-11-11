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

// Unit tests for third_party/geos/src/geom/SimpleCurve.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - Tests SimpleCurve through its concrete subclasses (LineString, LinearRing,
//   and CircularString) using isolated GeometryFactory::create() instances.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/SimpleCurve.cpp

#include "third_party/geos/include/geos/geom/SimpleCurve.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateFilter.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/CoordinateSequenceFilter.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::DoubleNotANumber;
using geos::geom::CircularString;
using geos::geom::Coordinate;
using geos::geom::CoordinateInspector;
using geos::geom::CoordinateMutator;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateSequenceFilter;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Dimension;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GEOS_MULTIPOINT;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Point;
using geos::geom::SimpleCurve;
using geos::util::UnsupportedOperationException;

class InspectFilter final : public CoordinateInspector<InspectFilter> {
 public:
  explicit InspectFilter(
      std::size_t stop_after = std::numeric_limits<std::size_t>::max())
      : stop_after_(stop_after) {}

  bool isDone() const override { return visited_.size() >= stop_after_; }

  void filter(const CoordinateXY* c) {
    visited_.emplace_back(c->x, c->y, DoubleNotANumber, DoubleNotANumber);
  }

  void filter(const Coordinate* c) {
    visited_.emplace_back(c->x, c->y, c->z, DoubleNotANumber);
  }

  void filter(const CoordinateXYM* c) {
    visited_.emplace_back(c->x, c->y, DoubleNotANumber, c->m);
  }

  void filter(const CoordinateXYZM* c) { visited_.push_back(*c); }

  const std::vector<CoordinateXYZM>& visited() const { return visited_; }

 private:
  std::size_t stop_after_;
  std::vector<CoordinateXYZM> visited_;
};

class ShiftMutator final : public CoordinateMutator<ShiftMutator> {
 public:
  ShiftMutator(double dx, double dy,
               std::size_t stop_after = std::numeric_limits<std::size_t>::max())
      : dx_(dx), dy_(dy), stop_after_(stop_after) {}

  bool isDone() const override { return mutated_count_ >= stop_after_; }

  template <typename CoordT>
  void filter(CoordT* c) const {
    c->x += dx_;
    c->y += dy_;
    ++mutated_count_;
  }

  std::size_t mutated_count() const { return mutated_count_; }

 private:
  double dx_;
  double dy_;
  std::size_t stop_after_;
  mutable std::size_t mutated_count_ = 0;
};

class SeqInspectFilter final : public CoordinateSequenceFilter {
 public:
  explicit SeqInspectFilter(
      std::size_t stop_after = std::numeric_limits<std::size_t>::max())
      : stop_after_(stop_after) {}

  void filter_ro(const CoordinateSequence& seq, std::size_t i) override {
    visited_indices_.push_back(i);
    visited_coords_.emplace_back(seq.getX(i), seq.getY(i));
  }

  bool isDone() const override {
    return visited_indices_.size() >= stop_after_;
  }

  bool isGeometryChanged() const override { return false; }

  const std::vector<std::size_t>& visited_indices() const {
    return visited_indices_;
  }

  const std::vector<CoordinateXY>& visited_coords() const {
    return visited_coords_;
  }

 private:
  std::size_t stop_after_;
  std::vector<std::size_t> visited_indices_;
  std::vector<CoordinateXY> visited_coords_;
};

class SeqScaleFilter final : public CoordinateSequenceFilter {
 public:
  SeqScaleFilter(
      double scale, bool report_changed,
      std::size_t stop_after = std::numeric_limits<std::size_t>::max())
      : scale_(scale),
        report_changed_(report_changed),
        stop_after_(stop_after) {}

  void filter_rw(CoordinateSequence& seq, std::size_t i) override {
    seq.setOrdinate(i, CoordinateSequence::X, seq.getX(i) * scale_);
    seq.setOrdinate(i, CoordinateSequence::Y, seq.getY(i) * scale_);
    visited_indices_.push_back(i);
  }

  bool isDone() const override {
    return visited_indices_.size() >= stop_after_;
  }

  bool isGeometryChanged() const override { return report_changed_; }

  const std::vector<std::size_t>& visited_indices() const {
    return visited_indices_;
  }

 private:
  double scale_;
  bool report_changed_;
  std::size_t stop_after_;
  std::vector<std::size_t> visited_indices_;
};

class SimpleCurveTest : public ::testing::Test {
 protected:
  GeometryFactory::Ptr factory_ = GeometryFactory::create();

  std::unique_ptr<LineString> MakeLine(
      std::initializer_list<CoordinateXY> coords) const {
    return factory_->createLineString(
        std::make_unique<CoordinateSequence>(coords));
  }

  std::unique_ptr<LineString> MakeLineXYZ(
      std::initializer_list<Coordinate> coords) const {
    return factory_->createLineString(
        std::make_unique<CoordinateSequence>(coords));
  }

  std::unique_ptr<LineString> MakeLineXYM(
      std::initializer_list<CoordinateXYM> coords) const {
    return factory_->createLineString(
        std::make_unique<CoordinateSequence>(coords));
  }

  std::unique_ptr<LineString> MakeLineXYZM(
      std::initializer_list<CoordinateXYZM> coords) const {
    return factory_->createLineString(
        std::make_unique<CoordinateSequence>(coords));
  }

  std::unique_ptr<LinearRing> MakeRing(
      std::initializer_list<CoordinateXY> coords) const {
    return factory_->createLinearRing(
        std::make_unique<CoordinateSequence>(coords));
  }

  std::unique_ptr<CircularString> MakeCircular(
      std::initializer_list<CoordinateXY> coords) const {
    return factory_->createCircularString(
        std::make_unique<CoordinateSequence>(coords));
  }
};

TEST_F(SimpleCurveTest, ConstructionFromCoordinatesAndNullPointer) {
  // createLinearRing(nullptr) forwards a null CoordinateSequence::Ptr directly
  // into SimpleCurve::SimpleCurve, exercising its null-coalescing branch.
  std::unique_ptr<LinearRing> null_ring =
      factory_->createLinearRing(CoordinateSequence::Ptr{nullptr});
  ASSERT_NE(null_ring, nullptr);
  const SimpleCurve& ring_curve = *null_ring;
  EXPECT_TRUE(ring_curve.isEmpty());
  EXPECT_EQ(ring_curve.getNumPoints(), 0u);
  EXPECT_NE(ring_curve.getCoordinatesRO(), nullptr);
  ASSERT_NE(ring_curve.getEnvelopeInternal(), nullptr);
  EXPECT_TRUE(ring_curve.getEnvelopeInternal()->isNull());

  std::unique_ptr<LineString> null_line =
      factory_->createLineString(CoordinateSequence::Ptr{nullptr});
  ASSERT_NE(null_line, nullptr);
  EXPECT_TRUE(null_line->isEmpty());
  EXPECT_EQ(null_line->getNumPoints(), 0u);

  std::unique_ptr<CircularString> null_circ =
      factory_->createCircularString(CoordinateSequence::Ptr{nullptr});
  ASSERT_NE(null_circ, nullptr);
  EXPECT_TRUE(null_circ->isEmpty());
  EXPECT_EQ(null_circ->getNumPoints(), 0u);
}

TEST_F(SimpleCurveTest, CopyConstructionAndCloneAreDeepCopies) {
  std::unique_ptr<LineString> line =
      MakeLineXYZM({{1.0, 2.0, 3.0, 4.0}, {5.0, 6.0, 7.0, 8.0}});
  std::unique_ptr<LineString> line_copy = factory_->createLineString(*line);
  std::unique_ptr<LineString> line_clone = line->clone();

  EXPECT_NE(line_copy->getCoordinatesRO(), line->getCoordinatesRO());
  EXPECT_NE(line_clone->getCoordinatesRO(), line->getCoordinatesRO());
  EXPECT_TRUE(line->equalsIdentical(line_copy.get()));
  EXPECT_TRUE(line->equalsIdentical(line_clone.get()));
  EXPECT_EQ(*line_clone->getEnvelopeInternal(), *line->getEnvelopeInternal());

  ShiftMutator shift(10.0, 20.0);
  line->apply_rw(&shift);
  line->geometryChanged();
  EXPECT_FALSE(line->equalsIdentical(line_clone.get()));
  EXPECT_DOUBLE_EQ(line_clone->getCoordinateN(0).x, 1.0);
  EXPECT_DOUBLE_EQ(line_clone->getCoordinateN(0).y, 2.0);

  std::unique_ptr<CircularString> circ =
      MakeCircular({{0.0, 0.0}, {1.0, 2.0}, {2.0, 0.0}});
  std::unique_ptr<CircularString> circ_copy =
      factory_->createCircularString(*circ);
  EXPECT_NE(circ_copy->getCoordinatesRO(), circ->getCoordinatesRO());
  EXPECT_TRUE(circ->equalsIdentical(circ_copy.get()));
  EXPECT_EQ(*circ_copy->getEnvelopeInternal(), *circ->getEnvelopeInternal());

  std::unique_ptr<LineString> empty_line = factory_->createLineString();
  std::unique_ptr<LineString> empty_clone = empty_line->clone();
  EXPECT_NE(empty_clone->getCoordinatesRO(), empty_line->getCoordinatesRO());
  EXPECT_TRUE(empty_clone->isEmpty());
  EXPECT_TRUE(empty_clone->getEnvelopeInternal()->isNull());
}

TEST_F(SimpleCurveTest, EmptyCurveAccessors) {
  std::unique_ptr<LineString> empty_line = factory_->createLineString();
  const SimpleCurve* curve = empty_line.get();

  EXPECT_TRUE(curve->isEmpty());
  EXPECT_FALSE(curve->isCurved());
  EXPECT_FALSE(curve->isClosed());
  EXPECT_EQ(curve->getNumPoints(), 0u);
  EXPECT_EQ(curve->getNumCurves(), 1u);
  EXPECT_EQ(curve->getCurveN(0), curve);
  EXPECT_EQ(curve->getCurveN(42), curve);
  EXPECT_EQ(curve->getCoordinate(), nullptr);
  EXPECT_EQ(curve->getStartPoint(), nullptr);
  EXPECT_EQ(curve->getEndPoint(), nullptr);
  EXPECT_TRUE(curve->getEnvelopeInternal()->isNull());

  std::unique_ptr<CoordinateSequence> coords = curve->getCoordinates();
  ASSERT_NE(coords, nullptr);
  EXPECT_NE(coords.get(), curve->getCoordinatesRO());
  EXPECT_TRUE(coords->isEmpty());

  std::unique_ptr<CircularString> empty_circ =
      factory_->createCircularString(false, false);
  const SimpleCurve* circ_curve = empty_circ.get();
  EXPECT_TRUE(circ_curve->isEmpty());
  EXPECT_TRUE(circ_curve->isCurved());
  EXPECT_FALSE(circ_curve->isClosed());
  EXPECT_EQ(circ_curve->getNumCurves(), 1u);
  EXPECT_EQ(circ_curve->getCurveN(0), circ_curve);
  EXPECT_EQ(circ_curve->getCoordinate(), nullptr);
  EXPECT_EQ(circ_curve->getStartPoint(), nullptr);
  EXPECT_EQ(circ_curve->getEndPoint(), nullptr);
}

TEST_F(SimpleCurveTest, NonEmptyAccessorsAndCoordinateOwnership) {
  std::unique_ptr<LineString> line =
      MakeLine({{1.0, 2.0}, {3.0, 4.0}, {7.0, -1.0}});
  const SimpleCurve* curve = line.get();

  EXPECT_FALSE(curve->isEmpty());
  EXPECT_FALSE(curve->isCurved());
  EXPECT_EQ(curve->getNumPoints(), 3u);
  EXPECT_EQ(curve->getNumCurves(), 1u);
  EXPECT_EQ(curve->getCurveN(0), curve);

  const CoordinateXY* first_coord = curve->getCoordinate();
  ASSERT_NE(first_coord, nullptr);
  EXPECT_DOUBLE_EQ(first_coord->x, 1.0);
  EXPECT_DOUBLE_EQ(first_coord->y, 2.0);
  EXPECT_EQ(first_coord, &curve->getCoordinatesRO()->getAt<CoordinateXY>(0));

  EXPECT_DOUBLE_EQ(curve->getCoordinateN(0).x, 1.0);
  EXPECT_DOUBLE_EQ(curve->getCoordinateN(0).y, 2.0);
  EXPECT_DOUBLE_EQ(curve->getCoordinateN(1).x, 3.0);
  EXPECT_DOUBLE_EQ(curve->getCoordinateN(1).y, 4.0);
  EXPECT_DOUBLE_EQ(curve->getCoordinateN(2).x, 7.0);
  EXPECT_DOUBLE_EQ(curve->getCoordinateN(2).y, -1.0);

  std::unique_ptr<Point> start = curve->getStartPoint();
  std::unique_ptr<Point> mid = curve->getPointN(1);
  std::unique_ptr<Point> end = curve->getEndPoint();
  ASSERT_NE(start, nullptr);
  ASSERT_NE(mid, nullptr);
  ASSERT_NE(end, nullptr);
  EXPECT_DOUBLE_EQ(start->getX(), 1.0);
  EXPECT_DOUBLE_EQ(start->getY(), 2.0);
  EXPECT_DOUBLE_EQ(mid->getX(), 3.0);
  EXPECT_DOUBLE_EQ(mid->getY(), 4.0);
  EXPECT_DOUBLE_EQ(end->getX(), 7.0);
  EXPECT_DOUBLE_EQ(end->getY(), -1.0);

  std::unique_ptr<CoordinateSequence> copied = curve->getCoordinates();
  ASSERT_NE(copied, nullptr);
  EXPECT_NE(copied.get(), curve->getCoordinatesRO());
  copied->setOrdinate(0, CoordinateSequence::X, 999.0);
  EXPECT_DOUBLE_EQ(curve->getCoordinateN(0).x, 1.0);
}

TEST_F(SimpleCurveTest, DimensionsHasZHasMAndPointFactoryPropagation) {
  struct DimensionCase {
    std::string_view name;
    bool has_z;
    bool has_m;
    int expected_dim;
  };
  constexpr auto kEmptyCases = std::to_array<DimensionCase>({
      {.name = "xy", .has_z = false, .has_m = false, .expected_dim = 2},
      {.name = "xyz", .has_z = true, .has_m = false, .expected_dim = 3},
      {.name = "xym", .has_z = false, .has_m = true, .expected_dim = 3},
      {.name = "xyzm", .has_z = true, .has_m = true, .expected_dim = 4},
  });

  for (const auto& tc : kEmptyCases) {
    SCOPED_TRACE(tc.name);
    std::unique_ptr<LineString> empty_ls =
        factory_->createLineString(tc.has_z, tc.has_m);
    EXPECT_EQ(empty_ls->hasZ(), tc.has_z);
    EXPECT_EQ(empty_ls->hasM(), tc.has_m);
    EXPECT_EQ(static_cast<int>(empty_ls->getCoordinateDimension()),
              tc.expected_dim);

    std::unique_ptr<CircularString> empty_cs =
        factory_->createCircularString(tc.has_z, tc.has_m);
    EXPECT_EQ(empty_cs->hasZ(), tc.has_z);
    EXPECT_EQ(empty_cs->hasM(), tc.has_m);
    EXPECT_EQ(static_cast<int>(empty_cs->getCoordinateDimension()),
              tc.expected_dim);
  }

  GeometryFactory::Ptr srid_factory = GeometryFactory::create(nullptr, 4326);
  std::unique_ptr<LineString> ls_xy = MakeLine({{1.0, 2.0}, {3.0, 4.0}});
  EXPECT_FALSE(ls_xy->hasZ());
  EXPECT_FALSE(ls_xy->hasM());
  EXPECT_EQ(static_cast<int>(ls_xy->getCoordinateDimension()), 2);
  EXPECT_TRUE(std::isnan(ls_xy->getCoordinateN(0).z));
  std::unique_ptr<Point> pt_xy = ls_xy->getPointN(0);
  EXPECT_FALSE(pt_xy->hasZ());
  EXPECT_FALSE(pt_xy->hasM());
  EXPECT_TRUE(std::isnan(pt_xy->getZ()));
  EXPECT_TRUE(std::isnan(pt_xy->getM()));

  std::unique_ptr<LineString> ls_xyz =
      MakeLineXYZ({{1.0, 2.0, 10.0}, {3.0, 4.0, 20.0}});
  EXPECT_TRUE(ls_xyz->hasZ());
  EXPECT_FALSE(ls_xyz->hasM());
  EXPECT_EQ(static_cast<int>(ls_xyz->getCoordinateDimension()), 3);
  std::unique_ptr<Point> pt_xyz = ls_xyz->getPointN(1);
  EXPECT_TRUE(pt_xyz->hasZ());
  EXPECT_FALSE(pt_xyz->hasM());
  EXPECT_DOUBLE_EQ(pt_xyz->getZ(), 20.0);
  EXPECT_TRUE(std::isnan(pt_xyz->getM()));

  std::unique_ptr<LineString> ls_xym =
      MakeLineXYM({{1.0, 2.0, 100.0}, {3.0, 4.0, 200.0}});
  EXPECT_FALSE(ls_xym->hasZ());
  EXPECT_TRUE(ls_xym->hasM());
  EXPECT_EQ(static_cast<int>(ls_xym->getCoordinateDimension()), 3);
  std::unique_ptr<Point> pt_xym = ls_xym->getPointN(1);
  EXPECT_FALSE(pt_xym->hasZ());
  EXPECT_TRUE(pt_xym->hasM());
  EXPECT_TRUE(std::isnan(pt_xym->getZ()));
  EXPECT_DOUBLE_EQ(pt_xym->getM(), 200.0);

  auto seq_xyzm = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXYZM>{{1.0, 2.0, 10.0, 100.0},
                                            {3.0, 4.0, 20.0, 200.0}});
  std::unique_ptr<LineString> ls_xyzm =
      srid_factory->createLineString(std::move(seq_xyzm));
  EXPECT_TRUE(ls_xyzm->hasZ());
  EXPECT_TRUE(ls_xyzm->hasM());
  EXPECT_EQ(static_cast<int>(ls_xyzm->getCoordinateDimension()), 4);
  std::unique_ptr<Point> pt_xyzm = ls_xyzm->getPointN(0);
  EXPECT_EQ(pt_xyzm->getSRID(), 4326);
  EXPECT_TRUE(pt_xyzm->hasZ());
  EXPECT_TRUE(pt_xyzm->hasM());
  EXPECT_DOUBLE_EQ(pt_xyzm->getZ(), 10.0);
  EXPECT_DOUBLE_EQ(pt_xyzm->getM(), 100.0);
}

TEST_F(SimpleCurveTest, EnvelopeComputationLinearVsCircular) {
  std::unique_ptr<LineString> line =
      MakeLine({{1.0, 5.0}, {-2.0, 3.0}, {4.0, -1.0}});
  const Envelope* line_env = line->getEnvelopeInternal();
  ASSERT_NE(line_env, nullptr);
  EXPECT_FALSE(line_env->isNull());
  EXPECT_DOUBLE_EQ(line_env->getMinX(), -2.0);
  EXPECT_DOUBLE_EQ(line_env->getMaxX(), 4.0);
  EXPECT_DOUBLE_EQ(line_env->getMinY(), -1.0);
  EXPECT_DOUBLE_EQ(line_env->getMaxY(), 5.0);

  // Arc through (0, 0), (1, 2), (2, 0) lies on circle centered at (1, 0.75)
  // with radius 1.25, so X extends beyond the vertex span [0, 2] to
  // [-0.25, 2.25].
  std::unique_ptr<CircularString> arc =
      MakeCircular({{0.0, 0.0}, {1.0, 2.0}, {2.0, 0.0}});
  const Envelope* arc_env = arc->getEnvelopeInternal();
  ASSERT_NE(arc_env, nullptr);
  EXPECT_NEAR(arc_env->getMinX(), -0.25, 1e-12);
  EXPECT_NEAR(arc_env->getMaxX(), 2.25, 1e-12);
  EXPECT_NEAR(arc_env->getMinY(), 0.0, 1e-12);
  EXPECT_NEAR(arc_env->getMaxY(), 2.0, 1e-12);

  // Full circle defined by (0, 0), (2, 0), (0, 0) has vertices only on y = 0
  // but circular envelope spans y in [-1, 1].
  std::unique_ptr<CircularString> full_circle =
      MakeCircular({{0.0, 0.0}, {2.0, 0.0}, {0.0, 0.0}});
  const Envelope* circle_env = full_circle->getEnvelopeInternal();
  EXPECT_NEAR(circle_env->getMinX(), 0.0, 1e-12);
  EXPECT_NEAR(circle_env->getMaxX(), 2.0, 1e-12);
  EXPECT_NEAR(circle_env->getMinY(), -1.0, 1e-12);
  EXPECT_NEAR(circle_env->getMaxY(), 1.0, 1e-12);

  // 5-point CircularString on the unit circle centered at (1, 0) exercises
  // multiple iterations of computeEnvelopeInternal(false).
  std::unique_ptr<CircularString> two_arcs = MakeCircular(
      {{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}, {1.0, -1.0}, {0.0, 0.0}});
  const Envelope* two_arcs_env = two_arcs->getEnvelopeInternal();
  EXPECT_NEAR(two_arcs_env->getMinX(), 0.0, 1e-12);
  EXPECT_NEAR(two_arcs_env->getMaxX(), 2.0, 1e-12);
  EXPECT_NEAR(two_arcs_env->getMinY(), -1.0, 1e-12);
  EXPECT_NEAR(two_arcs_env->getMaxY(), 1.0, 1e-12);
}

TEST_F(SimpleCurveTest, IsClosedAndIsCoordinate) {
  std::unique_ptr<LineString> open_line =
      MakeLine({{0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}});
  EXPECT_FALSE(open_line->isClosed());

  std::unique_ptr<LineString> closed_line =
      MakeLine({{0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}, {0.0, 0.0}});
  EXPECT_TRUE(closed_line->isClosed());

  // SimpleCurve::isClosed compares endpoints in 2D even when Z values differ.
  std::unique_ptr<LineString> closed_diff_z =
      MakeLineXYZ({{0.0, 0.0, 1.0}, {2.0, 0.0, 2.0}, {0.0, 0.0, 9.0}});
  EXPECT_TRUE(closed_diff_z->isClosed());

  std::unique_ptr<CircularString> open_circ =
      MakeCircular({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}});
  std::unique_ptr<CircularString> closed_circ =
      MakeCircular({{0.0, 0.0}, {2.0, 0.0}, {0.0, 0.0}});
  EXPECT_FALSE(open_circ->isClosed());
  EXPECT_TRUE(closed_circ->isClosed());

  CoordinateXY start_pt(0.0, 0.0);
  CoordinateXY mid_vertex(2.0, 0.0);
  CoordinateXY end_pt(2.0, 2.0);
  CoordinateXY on_segment_not_vertex(1.0, 0.0);
  CoordinateXY off_curve(5.0, 5.0);

  EXPECT_TRUE(open_line->isCoordinate(start_pt));
  EXPECT_TRUE(open_line->isCoordinate(mid_vertex));
  EXPECT_TRUE(open_line->isCoordinate(end_pt));
  EXPECT_FALSE(open_line->isCoordinate(on_segment_not_vertex));
  EXPECT_FALSE(open_line->isCoordinate(off_curve));

  std::unique_ptr<LineString> empty_line = factory_->createLineString();
  EXPECT_FALSE(empty_line->isCoordinate(start_pt));

  CoordinateXY circ_mid(1.0, 1.0);
  EXPECT_TRUE(open_circ->isCoordinate(circ_mid));
  EXPECT_FALSE(open_circ->isCoordinate(off_curve));
}

TEST_F(SimpleCurveTest, GetBoundaryBehavior) {
  std::unique_ptr<LineString> empty_line = factory_->createLineString();
  std::unique_ptr<Geometry> empty_boundary = empty_line->getBoundary();
  ASSERT_NE(empty_boundary, nullptr);
  EXPECT_EQ(empty_boundary->getGeometryTypeId(), GEOS_MULTIPOINT);
  EXPECT_TRUE(empty_boundary->isEmpty());

  std::unique_ptr<LineString> open_line =
      MakeLine({{0.0, 0.0}, {5.0, 5.0}, {10.0, 0.0}});
  std::unique_ptr<Geometry> open_boundary = open_line->getBoundary();
  ASSERT_NE(open_boundary, nullptr);
  EXPECT_EQ(open_boundary->getGeometryTypeId(), GEOS_MULTIPOINT);
  EXPECT_EQ(open_boundary->getDimension(), Dimension::P);
  ASSERT_EQ(open_boundary->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(open_boundary->getGeometryN(0)->getCoordinate()->x, 0.0);
  EXPECT_DOUBLE_EQ(open_boundary->getGeometryN(0)->getCoordinate()->y, 0.0);
  EXPECT_DOUBLE_EQ(open_boundary->getGeometryN(1)->getCoordinate()->x, 10.0);
  EXPECT_DOUBLE_EQ(open_boundary->getGeometryN(1)->getCoordinate()->y, 0.0);

  std::unique_ptr<LineString> closed_line =
      MakeLine({{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 0.0}});
  std::unique_ptr<Geometry> closed_boundary = closed_line->getBoundary();
  ASSERT_NE(closed_boundary, nullptr);
  EXPECT_EQ(closed_boundary->getGeometryTypeId(), GEOS_MULTIPOINT);
  EXPECT_TRUE(closed_boundary->isEmpty());

  std::unique_ptr<LinearRing> ring =
      MakeRing({{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}, {0.0, 0.0}});
  std::unique_ptr<Geometry> ring_boundary = ring->getBoundary();
  ASSERT_NE(ring_boundary, nullptr);
  EXPECT_EQ(ring_boundary->getGeometryTypeId(), GEOS_MULTIPOINT);
  EXPECT_TRUE(ring_boundary->isEmpty());

  std::unique_ptr<CircularString> circ =
      MakeCircular({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}});
  EXPECT_THROW(circ->getBoundary(), UnsupportedOperationException);
}

TEST_F(SimpleCurveTest, ApplyReadOnlyCoordinateFilter) {
  std::unique_ptr<LineString> empty_line = factory_->createLineString();
  InspectFilter empty_filter;
  empty_line->apply_ro(&empty_filter);
  EXPECT_TRUE(empty_filter.visited().empty());

  std::unique_ptr<LineString> line_xy = MakeLine({{1.0, 2.0}, {3.0, 4.0}});
  InspectFilter xy_filter;
  line_xy->apply_ro(&xy_filter);
  ASSERT_EQ(xy_filter.visited().size(), 2u);
  EXPECT_DOUBLE_EQ(xy_filter.visited()[0].x, 1.0);
  EXPECT_DOUBLE_EQ(xy_filter.visited()[0].y, 2.0);
  EXPECT_TRUE(std::isnan(xy_filter.visited()[0].z));
  EXPECT_TRUE(std::isnan(xy_filter.visited()[0].m));

  std::unique_ptr<LineString> line_xyz =
      MakeLineXYZ({{1.0, 2.0, 5.0}, {3.0, 4.0, 6.0}});
  InspectFilter xyz_filter;
  line_xyz->apply_ro(&xyz_filter);
  ASSERT_EQ(xyz_filter.visited().size(), 2u);
  EXPECT_DOUBLE_EQ(xyz_filter.visited()[1].z, 6.0);
  EXPECT_TRUE(std::isnan(xyz_filter.visited()[1].m));

  std::unique_ptr<LineString> line_xym =
      MakeLineXYM({{1.0, 2.0, 50.0}, {3.0, 4.0, 60.0}});
  InspectFilter xym_filter;
  line_xym->apply_ro(&xym_filter);
  ASSERT_EQ(xym_filter.visited().size(), 2u);
  EXPECT_TRUE(std::isnan(xym_filter.visited()[0].z));
  EXPECT_DOUBLE_EQ(xym_filter.visited()[0].m, 50.0);

  std::unique_ptr<LineString> line_xyzm =
      MakeLineXYZM({{1.0, 2.0, 3.0, 4.0},
                    {5.0, 6.0, 7.0, 8.0},
                    {9.0, 10.0, 11.0, 12.0},
                    {13.0, 14.0, 15.0, 16.0}});
  InspectFilter all_filter;
  line_xyzm->apply_ro(&all_filter);
  ASSERT_EQ(all_filter.visited().size(), 4u);
  EXPECT_TRUE(all_filter.visited()[2].equals4D({9.0, 10.0, 11.0, 12.0}));

  InspectFilter early_stop_filter(/*stop_after=*/2);
  line_xyzm->apply_ro(&early_stop_filter);
  ASSERT_EQ(early_stop_filter.visited().size(), 2u);
  EXPECT_TRUE(early_stop_filter.visited()[0].equals4D({1.0, 2.0, 3.0, 4.0}));
  EXPECT_TRUE(early_stop_filter.visited()[1].equals4D({5.0, 6.0, 7.0, 8.0}));
}

TEST_F(SimpleCurveTest, ApplyReadOnlyCoordinateSequenceFilter) {
  std::unique_ptr<LineString> empty_line = factory_->createLineString();
  SeqInspectFilter empty_filter;
  empty_line->apply_ro(empty_filter);
  EXPECT_TRUE(empty_filter.visited_indices().empty());

  std::unique_ptr<LineString> line =
      MakeLine({{1.0, 2.0}, {3.0, 4.0}, {5.0, 6.0}, {7.0, 8.0}});
  SeqInspectFilter full_filter;
  line->apply_ro(full_filter);
  EXPECT_EQ(full_filter.visited_indices(),
            (std::vector<std::size_t>{0, 1, 2, 3}));
  ASSERT_EQ(full_filter.visited_coords().size(), 4u);
  EXPECT_EQ(full_filter.visited_coords()[3], CoordinateXY(7.0, 8.0));

  SeqInspectFilter stop_filter(/*stop_after=*/2);
  line->apply_ro(stop_filter);
  EXPECT_EQ(stop_filter.visited_indices(), (std::vector<std::size_t>{0, 1}));
  ASSERT_EQ(stop_filter.visited_coords().size(), 2u);
  EXPECT_EQ(stop_filter.visited_coords()[1], CoordinateXY(3.0, 4.0));
}

TEST_F(SimpleCurveTest, ApplyReadWriteCoordinateFilter) {
  std::unique_ptr<LineString> empty_line = factory_->createLineString();
  ShiftMutator empty_mutator(5.0, -5.0);
  empty_line->apply_rw(&empty_mutator);
  EXPECT_EQ(empty_mutator.mutated_count(), 0u);

  std::unique_ptr<LineString> line =
      MakeLine({{1.0, 2.0}, {3.0, 4.0}, {5.0, 6.0}});
  ShiftMutator partial_mutator(10.0, 20.0, /*stop_after=*/1);
  line->apply_rw(&partial_mutator);
  EXPECT_EQ(partial_mutator.mutated_count(), 1u);
  EXPECT_DOUBLE_EQ(line->getCoordinateN(0).x, 11.0);
  EXPECT_DOUBLE_EQ(line->getCoordinateN(0).y, 22.0);
  EXPECT_DOUBLE_EQ(line->getCoordinateN(1).x, 3.0);
  EXPECT_DOUBLE_EQ(line->getCoordinateN(2).x, 5.0);

  ShiftMutator full_mutator(1.0, -2.0);
  line->apply_rw(&full_mutator);
  line->geometryChanged();
  EXPECT_EQ(full_mutator.mutated_count(), 3u);
  EXPECT_DOUBLE_EQ(line->getEnvelopeInternal()->getMinX(), 4.0);
  EXPECT_DOUBLE_EQ(line->getEnvelopeInternal()->getMaxX(), 12.0);
  EXPECT_DOUBLE_EQ(line->getEnvelopeInternal()->getMinY(), 2.0);
  EXPECT_DOUBLE_EQ(line->getEnvelopeInternal()->getMaxY(), 20.0);
}

TEST_F(SimpleCurveTest, ApplyReadWriteCoordinateSequenceFilter) {
  std::unique_ptr<LineString> empty_line = factory_->createLineString();
  SeqScaleFilter empty_filter(2.0, /*report_changed=*/true);
  empty_line->apply_rw(empty_filter);
  EXPECT_TRUE(empty_filter.visited_indices().empty());

  // When isGeometryChanged() is true, envelope is automatically recomputed.
  std::unique_ptr<CircularString> arc =
      MakeCircular({{0.0, 0.0}, {1.0, 2.0}, {2.0, 0.0}});
  SeqScaleFilter scale_changed(2.0, /*report_changed=*/true);
  arc->apply_rw(scale_changed);
  EXPECT_EQ(scale_changed.visited_indices(),
            (std::vector<std::size_t>{0, 1, 2}));
  EXPECT_NEAR(arc->getEnvelopeInternal()->getMinX(), -0.5, 1e-12);
  EXPECT_NEAR(arc->getEnvelopeInternal()->getMaxX(), 4.5, 1e-12);
  EXPECT_NEAR(arc->getEnvelopeInternal()->getMaxY(), 4.0, 1e-12);

  // When isGeometryChanged() is false, envelope stays unrefreshed until
  // geometryChanged() is explicitly called.
  std::unique_ptr<LineString> line =
      MakeLine({{1.0, 1.0}, {2.0, 2.0}, {3.0, 3.0}, {4.0, 4.0}});
  SeqScaleFilter scale_unchanged(10.0, /*report_changed=*/false,
                                 /*stop_after=*/2);
  line->apply_rw(scale_unchanged);
  EXPECT_EQ(scale_unchanged.visited_indices(),
            (std::vector<std::size_t>{0, 1}));
  EXPECT_DOUBLE_EQ(line->getCoordinateN(0).x, 10.0);
  EXPECT_DOUBLE_EQ(line->getCoordinateN(1).x, 20.0);
  EXPECT_DOUBLE_EQ(line->getCoordinateN(2).x, 3.0);
  EXPECT_DOUBLE_EQ(line->getEnvelopeInternal()->getMaxX(), 4.0);

  line->geometryChanged();
  EXPECT_DOUBLE_EQ(line->getEnvelopeInternal()->getMinX(), 3.0);
  EXPECT_DOUBLE_EQ(line->getEnvelopeInternal()->getMaxX(), 20.0);
}

TEST_F(SimpleCurveTest, CompareToSameClassOrdering) {
  std::unique_ptr<LineString> a = MakeLine({{0.0, 0.0}, {2.0, 2.0}});
  std::unique_ptr<LineString> a_equal_xyz =
      MakeLineXYZ({{0.0, 0.0, 99.0}, {2.0, 2.0, -5.0}});
  std::unique_ptr<LineString> longer =
      MakeLine({{0.0, 0.0}, {1.0, 1.0}, {2.0, 2.0}});
  std::unique_ptr<LineString> larger_first_x =
      MakeLine({{1.0, 0.0}, {2.0, 2.0}});
  std::unique_ptr<LineString> larger_second_y =
      MakeLine({{0.0, 0.0}, {2.0, 5.0}});

  // Distinct instances with equal XY coordinates enter compareToSameClass and
  // return 0 (Z/M are ignored).
  EXPECT_EQ(a->compareTo(a_equal_xyz.get()), 0);
  EXPECT_EQ(longer->compareTo(a.get()), 1);
  EXPECT_EQ(a->compareTo(longer.get()), -1);
  EXPECT_EQ(a->compareTo(larger_first_x.get()), -1);
  EXPECT_EQ(larger_first_x->compareTo(a.get()), 1);
  EXPECT_EQ(a->compareTo(larger_second_y.get()), -1);
  EXPECT_EQ(larger_second_y->compareTo(a.get()), 1);

  // LineString and CircularString share SORTINDEX_LINESTRING, so compareTo
  // compares them via SimpleCurve::compareToSameClass.
  std::unique_ptr<CircularString> circ =
      MakeCircular({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}});
  EXPECT_EQ(circ->compareTo(a.get()), 1);
  EXPECT_EQ(a->compareTo(circ.get()), -1);

  std::unique_ptr<LinearRing> ring1 =
      MakeRing({{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}, {0.0, 0.0}});
  std::unique_ptr<LinearRing> ring2 =
      MakeRing({{0.0, 0.0}, {2.0, 0.0}, {0.0, 1.0}, {0.0, 0.0}});
  EXPECT_EQ(ring1->compareTo(ring2.get()), -1);
  EXPECT_EQ(ring2->compareTo(ring1.get()), 1);
}

TEST_F(SimpleCurveTest, EqualsExactAndTolerance) {
  std::unique_ptr<LineString> line = MakeLine({{0.0, 0.0}, {3.0, 4.0}});
  std::unique_ptr<LineString> same_xyz =
      MakeLineXYZ({{0.0, 0.0, 5.0}, {3.0, 4.0, 9.0}});
  // Offset (0.375, 0.5) has exact binary distance sqrt(9/64 + 16/64) = 0.625.
  std::unique_ptr<LineString> near_line = MakeLine({{0.375, 0.5}, {3.0, 4.0}});
  std::unique_ptr<LineString> three_pts =
      MakeLine({{0.0, 0.0}, {1.5, 2.0}, {3.0, 4.0}});
  std::unique_ptr<LineString> empty_line = factory_->createLineString();
  std::unique_ptr<LineString> empty_line2 = factory_->createLineString();

  EXPECT_TRUE(line->equalsExact(same_xyz.get()));
  EXPECT_TRUE(empty_line->equalsExact(empty_line2.get()));
  EXPECT_FALSE(line->equalsExact(empty_line.get()));
  EXPECT_FALSE(line->equalsExact(three_pts.get()));
  EXPECT_FALSE(line->equalsExact(near_line.get(), 0.0));
  EXPECT_FALSE(line->equalsExact(near_line.get(), 0.624));
  EXPECT_TRUE(line->equalsExact(near_line.get(), 0.625));

  // Class mismatch must return false even when coordinates match.
  std::unique_ptr<Point> pt = factory_->createPoint(CoordinateXY(0.0, 0.0));
  std::unique_ptr<LineString> closed_line =
      MakeLine({{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}, {0.0, 0.0}});
  std::unique_ptr<LinearRing> ring =
      MakeRing({{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}, {0.0, 0.0}});
  std::unique_ptr<CircularString> circ =
      MakeCircular({{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}});
  std::unique_ptr<LineString> three_pt_line =
      MakeLine({{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}});
  EXPECT_FALSE(line->equalsExact(pt.get()));
  EXPECT_FALSE(closed_line->equalsExact(ring.get()));
  EXPECT_FALSE(three_pt_line->equalsExact(circ.get()));
}

TEST_F(SimpleCurveTest, EqualsIdentical) {
  std::unique_ptr<LineString> line_xy =
      MakeLine({{0.0, 0.0}, {1.0, 1.0}, {2.0, 2.0}});
  std::unique_ptr<LineString> line_xy_copy =
      MakeLine({{0.0, 0.0}, {1.0, 1.0}, {2.0, 2.0}});
  std::unique_ptr<LineString> diff_mid_same_env =
      MakeLine({{0.0, 0.0}, {0.5, 0.5}, {2.0, 2.0}});
  std::unique_ptr<LineString> diff_env =
      MakeLine({{0.0, 0.0}, {1.0, 1.0}, {3.0, 3.0}});
  std::unique_ptr<LineString> line_xyz =
      MakeLineXYZ({{0.0, 0.0, 1.0}, {1.0, 1.0, 2.0}, {2.0, 2.0, 3.0}});
  std::unique_ptr<LineString> line_xyz_diff_z =
      MakeLineXYZ({{0.0, 0.0, 1.0}, {1.0, 1.0, 99.0}, {2.0, 2.0, 3.0}});

  EXPECT_TRUE(line_xy->equalsIdentical(line_xy_copy.get()));
  EXPECT_FALSE(line_xy->equalsIdentical(diff_mid_same_env.get()));
  EXPECT_FALSE(line_xy->equalsIdentical(diff_env.get()));
  EXPECT_FALSE(line_xy->equalsIdentical(line_xyz.get()));
  EXPECT_FALSE(line_xyz->equalsIdentical(line_xyz_diff_z.get()));

  std::unique_ptr<LineString> line_xym =
      MakeLineXYM({{0.0, 0.0, 1.0}, {1.0, 1.0, 2.0}, {2.0, 2.0, 3.0}});
  std::unique_ptr<LineString> line_xyzm = MakeLineXYZM(
      {{0.0, 0.0, 0.0, 1.0}, {1.0, 1.0, 0.0, 2.0}, {2.0, 2.0, 0.0, 3.0}});
  EXPECT_FALSE(line_xy->equalsIdentical(line_xym.get()));
  EXPECT_FALSE(line_xym->equalsIdentical(line_xyzm.get()));

  std::unique_ptr<CircularString> circ1 =
      MakeCircular({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}});
  std::unique_ptr<CircularString> circ2 =
      MakeCircular({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}});
  std::unique_ptr<CircularString> circ_diff =
      MakeCircular({{0.0, 0.0}, {1.0, 2.0}, {2.0, 0.0}});
  EXPECT_TRUE(circ1->equalsIdentical(circ2.get()));
  EXPECT_FALSE(circ1->equalsIdentical(circ_diff.get()));
  EXPECT_FALSE(circ1->equalsIdentical(
      MakeLine({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}).get()));

  // NaN Z values match identically in equalsIdentical.
  auto seq_nan1 = std::make_unique<CoordinateSequence>(2u, true, false);
  seq_nan1->setAt(Coordinate(0.0, 0.0, 1.0), 0);
  seq_nan1->setAt(Coordinate(1.0, 1.0, DoubleNotANumber), 1);
  auto seq_nan2 = std::make_unique<CoordinateSequence>(2u, true, false);
  seq_nan2->setAt(Coordinate(0.0, 0.0, 1.0), 0);
  seq_nan2->setAt(Coordinate(1.0, 1.0, DoubleNotANumber), 1);
  EXPECT_TRUE(factory_->createLineString(std::move(seq_nan1))
                  ->equalsIdentical(
                      factory_->createLineString(std::move(seq_nan2)).get()));

  std::unique_ptr<LineString> empty_xy =
      factory_->createLineString(false, false);
  std::unique_ptr<LineString> empty_xy2 =
      factory_->createLineString(false, false);
  std::unique_ptr<LineString> empty_xyz =
      factory_->createLineString(true, false);
  EXPECT_TRUE(empty_xy->equalsIdentical(empty_xy2.get()));
  EXPECT_FALSE(empty_xy->equalsIdentical(empty_xyz.get()));
  EXPECT_FALSE(line_xy->equalsIdentical(empty_xy.get()));

  std::unique_ptr<Point> pt = factory_->createPoint(CoordinateXY(0.0, 0.0));
  EXPECT_FALSE(line_xy->equalsIdentical(pt.get()));
}

TEST_F(SimpleCurveTest, NormalizeOpenClosedAndCurved) {
  // Curved SimpleCurves throw UnsupportedOperationException.
  std::unique_ptr<CircularString> circ =
      MakeCircular({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}});
  std::unique_ptr<CircularString> empty_circ =
      factory_->createCircularString(false, false);
  EXPECT_THROW(circ->normalize(), UnsupportedOperationException);
  EXPECT_THROW(empty_circ->normalize(), UnsupportedOperationException);

  // Empty linear curves are no-ops.
  std::unique_ptr<LineString> empty_line = factory_->createLineString();
  empty_line->normalize();
  EXPECT_TRUE(empty_line->isEmpty());
  std::unique_ptr<LinearRing> empty_ring = factory_->createLinearRing();
  empty_ring->normalize();
  EXPECT_TRUE(empty_ring->isEmpty());

  // Open LineStrings: reverse only if start > end lexicographically.
  std::unique_ptr<LineString> open_forward =
      MakeLine({{0.0, 0.0}, {5.0, 5.0}, {2.0, 0.0}});
  open_forward->normalize();
  EXPECT_DOUBLE_EQ(open_forward->getCoordinateN(0).x, 0.0);
  EXPECT_DOUBLE_EQ(open_forward->getCoordinateN(2).x, 2.0);

  std::unique_ptr<LineString> open_reversed =
      MakeLine({{1.0, 5.0}, {2.0, 0.0}, {1.0, 1.0}});
  open_reversed->normalize();
  EXPECT_DOUBLE_EQ(open_reversed->getCoordinateN(0).x, 1.0);
  EXPECT_DOUBLE_EQ(open_reversed->getCoordinateN(0).y, 1.0);
  EXPECT_DOUBLE_EQ(open_reversed->getCoordinateN(2).x, 1.0);
  EXPECT_DOUBLE_EQ(open_reversed->getCoordinateN(2).y, 5.0);

  // Degenerate closed LineString with < 4 points scrolls to min coordinate
  // without running Orientation::isCCW.
  std::unique_ptr<LineString> degenerate_closed =
      MakeLine({{5.0, 5.0}, {1.0, 2.0}, {5.0, 5.0}});
  degenerate_closed->normalize();
  EXPECT_EQ(degenerate_closed->getNumPoints(), 3u);
  EXPECT_DOUBLE_EQ(degenerate_closed->getCoordinateN(0).x, 1.0);
  EXPECT_DOUBLE_EQ(degenerate_closed->getCoordinateN(0).y, 2.0);
  EXPECT_DOUBLE_EQ(degenerate_closed->getCoordinateN(1).x, 5.0);
  EXPECT_DOUBLE_EQ(degenerate_closed->getCoordinateN(2).x, 1.0);
  EXPECT_DOUBLE_EQ(degenerate_closed->getCoordinateN(2).y, 2.0);

  // Closed CCW square is scrolled to min coordinate (0, 0) and reversed to CW.
  std::unique_ptr<LineString> ccw_square =
      MakeLine({{1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}, {0.0, 0.0}, {1.0, 0.0}});
  ccw_square->normalize();
  std::unique_ptr<LineString> expected_cw_square =
      MakeLine({{0.0, 0.0}, {0.0, 1.0}, {1.0, 1.0}, {1.0, 0.0}, {0.0, 0.0}});
  EXPECT_TRUE(ccw_square->equalsExact(expected_cw_square.get()));

  // Closed CW square is scrolled to min coordinate (0, 0) and stays CW.
  std::unique_ptr<LineString> cw_square =
      MakeLine({{1.0, 1.0}, {1.0, 0.0}, {0.0, 0.0}, {0.0, 1.0}, {1.0, 1.0}});
  cw_square->normalize();
  EXPECT_TRUE(cw_square->equalsExact(expected_cw_square.get()));

  // LinearRing with XYZM coordinates preserves Z and M across scroll+reverse.
  auto ring_seq = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXYZM>{{1.0, 0.0, 10.0, 100.0},
                                            {1.0, 1.0, 20.0, 200.0},
                                            {0.0, 1.0, 30.0, 300.0},
                                            {0.0, 0.0, 40.0, 400.0},
                                            {1.0, 0.0, 10.0, 100.0}});
  std::unique_ptr<LinearRing> ring_xyzm =
      factory_->createLinearRing(std::move(ring_seq));
  ring_xyzm->normalize();
  auto expected_ring_seq = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXYZM>{{0.0, 0.0, 40.0, 400.0},
                                            {0.0, 1.0, 30.0, 300.0},
                                            {1.0, 1.0, 20.0, 200.0},
                                            {1.0, 0.0, 10.0, 100.0},
                                            {0.0, 0.0, 40.0, 400.0}});
  std::unique_ptr<LinearRing> expected_ring_xyzm =
      factory_->createLinearRing(std::move(expected_ring_seq));
  EXPECT_TRUE(ring_xyzm->equalsIdentical(expected_ring_xyzm.get()));
}

TEST_F(SimpleCurveTest, ReleaseCoordinatesResetsToEmptyWithDimensions) {
  std::unique_ptr<LineString> line =
      MakeLineXYZM({{1.0, 2.0, 3.0, 4.0}, {5.0, 6.0, 7.0, 8.0}});
  std::unique_ptr<CoordinateSequence> released = line->releaseCoordinates();
  ASSERT_NE(released, nullptr);
  EXPECT_EQ(released->size(), 2u);
  EXPECT_TRUE(released->hasZ());
  EXPECT_TRUE(released->hasM());
  EXPECT_TRUE(
      released->getAt<CoordinateXYZM>(1).equals4D({5.0, 6.0, 7.0, 8.0}));

  EXPECT_TRUE(line->isEmpty());
  EXPECT_EQ(line->getNumPoints(), 0u);
  EXPECT_TRUE(line->hasZ());
  EXPECT_TRUE(line->hasM());
  EXPECT_TRUE(line->getEnvelopeInternal()->isNull());
  EXPECT_EQ(line->getCoordinate(), nullptr);

  std::unique_ptr<CircularString> circ =
      MakeCircular({{0.0, 0.0}, {1.0, 2.0}, {2.0, 0.0}});
  std::unique_ptr<CoordinateSequence> released_circ =
      circ->releaseCoordinates();
  ASSERT_NE(released_circ, nullptr);
  EXPECT_EQ(released_circ->size(), 3u);
  EXPECT_TRUE(circ->isEmpty());
  EXPECT_TRUE(circ->getEnvelopeInternal()->isNull());
}

TEST_F(SimpleCurveTest, InheritedCurveProperties) {
  std::unique_ptr<LineString> open_line =
      MakeLine({{0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}});
  std::unique_ptr<LineString> closed_simple =
      MakeLine({{0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}, {0.0, 0.0}});
  std::unique_ptr<LineString> closed_self_intersecting =
      MakeLine({{0.0, 0.0}, {2.0, 2.0}, {2.0, 0.0}, {0.0, 2.0}, {0.0, 0.0}});

  const SimpleCurve& open_curve = *open_line;
  const SimpleCurve& closed_curve = *closed_simple;
  const SimpleCurve& bowtie_curve = *closed_self_intersecting;

  EXPECT_EQ(open_curve.getDimension(), Dimension::L);
  EXPECT_EQ(open_curve.getBoundaryDimension(), 0);
  EXPECT_FALSE(open_curve.isRing());

  EXPECT_EQ(closed_curve.getDimension(), Dimension::L);
  EXPECT_EQ(closed_curve.getBoundaryDimension(), Dimension::False);
  EXPECT_TRUE(closed_curve.isRing());

  EXPECT_TRUE(bowtie_curve.isClosed());
  EXPECT_FALSE(bowtie_curve.isRing());
}

}  // namespace
}  // namespace geos_google
