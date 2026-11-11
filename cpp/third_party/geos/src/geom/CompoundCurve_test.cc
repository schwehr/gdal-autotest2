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

// Unit tests for third_party/geos/src/geom/CompoundCurve.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/CompoundCurve.cpp

#include "third_party/geos/include/geos/geom/CompoundCurve.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateFilter.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/CoordinateSequenceFilter.h"
#include "third_party/geos/include/geos/geom/Curve.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/MultiCurve.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/SimpleCurve.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::CircularString;
using geos::geom::CompoundCurve;
using geos::geom::Coordinate;
using geos::geom::CoordinateInspector;
using geos::geom::CoordinateMutator;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateSequenceFilter;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Curve;
using geos::geom::Dimension;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::geom::MultiCurve;
using geos::geom::Point;
using geos::geom::SimpleCurve;
using geos::io::WKTReader;
using geos::util::IllegalArgumentException;
using geos::util::UnsupportedOperationException;
using ::testing::HasSubstr;

std::unique_ptr<CoordinateSequence> MakeSeqXY(
    std::initializer_list<CoordinateXY> coords) {
  return std::make_unique<CoordinateSequence>(coords);
}

std::unique_ptr<CoordinateSequence> MakeSeqXYZ(
    std::initializer_list<Coordinate> coords) {
  return std::make_unique<CoordinateSequence>(coords);
}

std::unique_ptr<CoordinateSequence> MakeSeqXYM(
    std::initializer_list<CoordinateXYM> coords) {
  return std::make_unique<CoordinateSequence>(coords);
}

std::unique_ptr<CoordinateSequence> MakeSeqXYZM(
    std::initializer_list<CoordinateXYZM> coords) {
  return std::make_unique<CoordinateSequence>(coords);
}

class RecordingCoordInspector final
    : public CoordinateInspector<RecordingCoordInspector> {
 public:
  template <typename CoordT>
  void filter(const CoordT* coord) {
    coords.emplace_back(coord->x, coord->y);
  }

  std::vector<CoordinateXY> coords;
};

class ShiftCoordMutator final : public CoordinateMutator<ShiftCoordMutator> {
 public:
  ShiftCoordMutator(double dx, double dy) : dx_(dx), dy_(dy) {}

  template <typename CoordT>
  void filter(CoordT* coord) const {
    coord->x += dx_;
    coord->y += dy_;
  }

 private:
  double dx_;
  double dy_;
};

class RecordingSeqFilter final : public CoordinateSequenceFilter {
 public:
  explicit RecordingSeqFilter(int stop_after = -1) : stop_after_(stop_after) {}

  void filter_ro(const CoordinateSequence& seq, std::size_t i) override {
    coords.push_back(seq.getAt<CoordinateXY>(i));
  }

  void filter_rw(CoordinateSequence&, std::size_t) override {}

  bool isDone() const override {
    return stop_after_ >= 0 &&
           coords.size() >= static_cast<std::size_t>(stop_after_);
  }

  bool isGeometryChanged() const override { return false; }

  std::vector<CoordinateXY> coords;

 private:
  int stop_after_;
};

class SubclassCompoundCurve final : public CompoundCurve {
 public:
  SubclassCompoundCurve(std::vector<std::unique_ptr<SimpleCurve>>&& curves,
                        const GeometryFactory& gf)
      : CompoundCurve(std::move(curves), gf) {}

  SubclassCompoundCurve(const CompoundCurve& other) : CompoundCurve(other) {}

  void AssignFrom(const CompoundCurve& other) {
    CompoundCurve::operator=(other);
  }
};

class CompoundCurveTest : public ::testing::Test {
 protected:
  CompoundCurveTest() : factory_(GeometryFactory::create()) {}

  std::unique_ptr<LineString> MakeLineXY(
      std::initializer_list<CoordinateXY> coords) const {
    return factory_->createLineString(MakeSeqXY(coords));
  }

  std::unique_ptr<CircularString> MakeArcXY(
      std::initializer_list<CoordinateXY> coords) const {
    return factory_->createCircularString(MakeSeqXY(coords));
  }

  std::unique_ptr<CompoundCurve> MakeOpenLineAndArc() const {
    std::vector<std::unique_ptr<SimpleCurve>> curves;
    curves.push_back(MakeLineXY({{0.0, 0.0}, {1.0, 0.0}}));
    curves.push_back(MakeArcXY({{1.0, 0.0}, {2.0, 1.0}, {3.0, 0.0}}));
    return factory_->createCompoundCurve(std::move(curves));
  }

  GeometryFactory::Ptr factory_;
};

TEST_F(CompoundCurveTest, EmptyCreationAndProperties) {
  std::unique_ptr<CompoundCurve> cc = factory_->createCompoundCurve();
  ASSERT_NE(cc, nullptr);
  EXPECT_TRUE(cc->isEmpty());
  EXPECT_FALSE(cc->isClosed());
  EXPECT_FALSE(cc->hasCurvedComponents());
  EXPECT_FALSE(cc->hasZ());
  EXPECT_FALSE(cc->hasM());
  EXPECT_EQ(cc->getCoordinateDimension(), 2);
  EXPECT_EQ(cc->getNumCurves(), 0u);
  EXPECT_EQ(cc->getNumPoints(), 0u);
  EXPECT_EQ(cc->getLength(), 0.0);
  EXPECT_EQ(cc->getCoordinate(), nullptr);
  EXPECT_EQ(cc->getGeometryType(), "CompoundCurve");
  EXPECT_EQ(cc->getGeometryTypeId(), geos::geom::GEOS_COMPOUNDCURVE);
  EXPECT_EQ(cc->getDimension(), Dimension::L);
  EXPECT_EQ(cc->getBoundaryDimension(), 0);
  EXPECT_TRUE(cc->getEnvelopeInternal()->isNull());

  std::unique_ptr<CoordinateSequence> coords = cc->getCoordinates();
  ASSERT_NE(coords, nullptr);
  EXPECT_TRUE(coords->isEmpty());
  EXPECT_FALSE(coords->hasZ());
  EXPECT_FALSE(coords->hasM());
}

TEST_F(CompoundCurveTest, MultiCurvePropertiesLengthEnvelopeAndClosure) {
  std::unique_ptr<CompoundCurve> open = MakeOpenLineAndArc();
  ASSERT_NE(open, nullptr);
  open->validateConstruction();

  EXPECT_FALSE(open->isEmpty());
  EXPECT_FALSE(open->isClosed());
  EXPECT_TRUE(open->hasCurvedComponents());
  EXPECT_FALSE(open->hasZ());
  EXPECT_FALSE(open->hasM());
  EXPECT_EQ(open->getCoordinateDimension(), 2);
  EXPECT_EQ(open->getNumCurves(), 2u);
  EXPECT_EQ(open->getNumPoints(), 5u);
  EXPECT_EQ(open->getGeometryType(), "CompoundCurve");
  EXPECT_EQ(open->getGeometryTypeId(), geos::geom::GEOS_COMPOUNDCURVE);
  EXPECT_EQ(open->getBoundaryDimension(), 0);
  EXPECT_EQ(open->getCurveN(0)->getGeometryTypeId(),
            geos::geom::GEOS_LINESTRING);
  EXPECT_EQ(open->getCurveN(1)->getGeometryTypeId(),
            geos::geom::GEOS_CIRCULARSTRING);

  const CoordinateXY* first_coord = open->getCoordinate();
  ASSERT_NE(first_coord, nullptr);
  EXPECT_EQ(*first_coord, (CoordinateXY{0.0, 0.0}));

  EXPECT_NEAR(open->getLength(), 1.0 + std::numbers::pi, 1e-12);
  const Envelope* env = open->getEnvelopeInternal();
  EXPECT_NEAR(env->getMinX(), 0.0, 1e-12);
  EXPECT_NEAR(env->getMaxX(), 3.0, 1e-12);
  EXPECT_NEAR(env->getMinY(), 0.0, 1e-12);
  EXPECT_NEAR(env->getMaxY(), 1.0, 1e-12);

  std::unique_ptr<CoordinateSequence> all_coords = open->getCoordinates();
  ASSERT_EQ(all_coords->size(), 5u);
  EXPECT_EQ(all_coords->getAt<CoordinateXY>(0), (CoordinateXY{0.0, 0.0}));
  EXPECT_EQ(all_coords->getAt<CoordinateXY>(1), (CoordinateXY{1.0, 0.0}));
  EXPECT_EQ(all_coords->getAt<CoordinateXY>(2), (CoordinateXY{1.0, 0.0}));
  EXPECT_EQ(all_coords->getAt<CoordinateXY>(3), (CoordinateXY{2.0, 1.0}));
  EXPECT_EQ(all_coords->getAt<CoordinateXY>(4), (CoordinateXY{3.0, 0.0}));

  // Closed CompoundCurve: linear segment + semicircle closing back to (0, 0).
  std::vector<std::unique_ptr<SimpleCurve>> closed_curves;
  closed_curves.push_back(MakeLineXY({{0.0, 0.0}, {2.0, 0.0}}));
  closed_curves.push_back(MakeArcXY({{2.0, 0.0}, {1.0, 1.0}, {0.0, 0.0}}));
  std::unique_ptr<CompoundCurve> closed =
      factory_->createCompoundCurve(std::move(closed_curves));
  EXPECT_TRUE(closed->isClosed());
  EXPECT_EQ(closed->getBoundaryDimension(), Dimension::False);
  EXPECT_NEAR(closed->getLength(), 2.0 + std::numbers::pi, 1e-12);

  // Single-curve closed and open CompoundCurves.
  std::vector<std::unique_ptr<SimpleCurve>> single_closed_curves;
  single_closed_curves.push_back(
      MakeLineXY({{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}, {0.0, 0.0}}));
  std::unique_ptr<CompoundCurve> single_closed =
      factory_->createCompoundCurve(std::move(single_closed_curves));
  EXPECT_TRUE(single_closed->isClosed());
  EXPECT_EQ(single_closed->getBoundaryDimension(), Dimension::False);

  std::vector<std::unique_ptr<SimpleCurve>> single_open_curves;
  single_open_curves.push_back(MakeLineXY({{0.0, 0.0}, {1.0, 0.0}}));
  std::unique_ptr<CompoundCurve> single_open =
      factory_->createCompoundCurve(std::move(single_open_curves));
  EXPECT_FALSE(single_open->isClosed());
  EXPECT_EQ(single_open->getBoundaryDimension(), 0);

  // Three-section CompoundCurve (LineString + CircularString + LineString).
  std::vector<std::unique_ptr<SimpleCurve>> three_curves;
  three_curves.push_back(MakeLineXY({{0.0, 0.0}, {1.0, 0.0}}));
  three_curves.push_back(MakeArcXY({{1.0, 0.0}, {2.0, 1.0}, {3.0, 0.0}}));
  three_curves.push_back(MakeLineXY({{3.0, 0.0}, {3.0, -2.0}}));
  std::unique_ptr<CompoundCurve> three_cc =
      factory_->createCompoundCurve(std::move(three_curves));
  EXPECT_EQ(three_cc->getNumCurves(), 3u);
  EXPECT_EQ(three_cc->getNumPoints(), 7u);
  EXPECT_TRUE(three_cc->hasCurvedComponents());
  EXPECT_NEAR(three_cc->getLength(), 3.0 + std::numbers::pi, 1e-12);
  EXPECT_EQ(three_cc->getCurveN(2)->getGeometryTypeId(),
            geos::geom::GEOS_LINESTRING);
  EXPECT_NEAR(three_cc->getEnvelopeInternal()->getMinY(), -2.0, 1e-12);
  EXPECT_NEAR(three_cc->getEnvelopeInternal()->getMaxY(), 1.0, 1e-12);

  // Closed 3D CompoundCurve whose start/end XY match while Z differs.
  std::vector<std::unique_ptr<SimpleCurve>> closed_3d_curves;
  closed_3d_curves.push_back(factory_->createLineString(
      MakeSeqXYZ({{0.0, 0.0, 1.0}, {2.0, 0.0, 2.0}})));
  closed_3d_curves.push_back(factory_->createLineString(
      MakeSeqXYZ({{2.0, 0.0, 2.0}, {1.0, 1.0, 3.0}, {0.0, 0.0, 9.0}})));
  std::unique_ptr<CompoundCurve> closed_3d =
      factory_->createCompoundCurve(std::move(closed_3d_curves));
  EXPECT_TRUE(closed_3d->isClosed());
}

TEST_F(CompoundCurveTest, HasCurvedComponentsLinearOnlyAndCurved) {
  std::vector<std::unique_ptr<SimpleCurve>> linear_only;
  linear_only.push_back(MakeLineXY({{0.0, 0.0}, {1.0, 0.0}}));
  linear_only.push_back(MakeLineXY({{1.0, 0.0}, {1.0, 2.0}}));
  std::unique_ptr<CompoundCurve> linear_cc =
      factory_->createCompoundCurve(std::move(linear_only));
  EXPECT_FALSE(linear_cc->hasCurvedComponents());
  EXPECT_DOUBLE_EQ(linear_cc->getLength(), 3.0);

  std::vector<std::unique_ptr<SimpleCurve>> curved_first;
  curved_first.push_back(MakeArcXY({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}));
  curved_first.push_back(MakeLineXY({{2.0, 0.0}, {4.0, 0.0}}));
  std::unique_ptr<CompoundCurve> curved_cc =
      factory_->createCompoundCurve(std::move(curved_first));
  EXPECT_TRUE(curved_cc->hasCurvedComponents());
  EXPECT_NEAR(curved_cc->getLength(), std::numbers::pi + 2.0, 1e-12);
}

TEST_F(CompoundCurveTest, ValidateConstructionRejectsEmptyOrNonContiguous) {
  // Empty section at index 0.
  {
    std::vector<std::unique_ptr<SimpleCurve>> curves;
    curves.push_back(factory_->createLineString());
    try {
      factory_->createCompoundCurve(std::move(curves));
      FAIL() << "Expected IllegalArgumentException for empty section 0";
    } catch (const IllegalArgumentException& e) {
      EXPECT_THAT(e.what(), HasSubstr("Section 0 of CompoundCurve is empty"));
    }
  }

  // Empty section at index 1 after a valid section.
  {
    std::vector<std::unique_ptr<SimpleCurve>> curves;
    curves.push_back(MakeLineXY({{0.0, 0.0}, {1.0, 0.0}}));
    curves.push_back(factory_->createCircularString(false, false));
    try {
      factory_->createCompoundCurve(std::move(curves));
      FAIL() << "Expected IllegalArgumentException for empty section 1";
    } catch (const IllegalArgumentException& e) {
      EXPECT_THAT(e.what(), HasSubstr("Section 1 of CompoundCurve is empty"));
    }
  }

  // Non-contiguous sections at index 1.
  {
    std::vector<std::unique_ptr<SimpleCurve>> curves;
    curves.push_back(MakeLineXY({{0.0, 0.0}, {1.0, 0.0}}));
    curves.push_back(MakeArcXY({{2.0, 0.0}, {3.0, 1.0}, {4.0, 0.0}}));
    try {
      factory_->createCompoundCurve(std::move(curves));
      FAIL() << "Expected IllegalArgumentException for non-contiguous curves";
    } catch (const IllegalArgumentException& e) {
      EXPECT_THAT(e.what(), HasSubstr("not contiguous"));
      EXPECT_THAT(e.what(), HasSubstr("curve 0 ends at"));
      EXPECT_THAT(e.what(), HasSubstr("curve 1 begins at"));
    }
  }

  // Non-contiguous sections at index 2 after contiguous 0 -> 1.
  {
    std::vector<std::unique_ptr<SimpleCurve>> curves;
    curves.push_back(MakeLineXY({{0.0, 0.0}, {1.0, 0.0}}));
    curves.push_back(MakeArcXY({{1.0, 0.0}, {2.0, 1.0}, {3.0, 0.0}}));
    curves.push_back(MakeLineXY({{4.0, 0.0}, {5.0, 0.0}}));
    try {
      factory_->createCompoundCurve(std::move(curves));
      FAIL() << "Expected IllegalArgumentException for non-contiguous curve 2";
    } catch (const IllegalArgumentException& e) {
      EXPECT_THAT(e.what(), HasSubstr("curve 1 ends at"));
      EXPECT_THAT(e.what(), HasSubstr("curve 2 begins at"));
    }
  }

  // Contiguous XY with differing Z coordinates is allowed.
  {
    std::vector<std::unique_ptr<SimpleCurve>> curves;
    curves.push_back(factory_->createLineString(
        MakeSeqXYZ({{0.0, 0.0, 1.0}, {1.0, 0.0, 2.0}})));
    curves.push_back(factory_->createLineString(
        MakeSeqXYZ({{1.0, 0.0, 99.0}, {2.0, 0.0, 3.0}})));
    std::unique_ptr<CompoundCurve> cc =
        factory_->createCompoundCurve(std::move(curves));
    ASSERT_NE(cc, nullptr);
    EXPECT_EQ(cc->getNumCurves(), 2u);
    std::unique_ptr<CoordinateSequence> coords = cc->getCoordinates();
    ASSERT_EQ(coords->size(), 4u);
    EXPECT_DOUBLE_EQ(coords->getAt<Coordinate>(1).z, 2.0);
    EXPECT_DOUBLE_EQ(coords->getAt<Coordinate>(2).z, 99.0);
  }
}

TEST_F(CompoundCurveTest, CloneAndProtectedCopyAssignment) {
  std::unique_ptr<CompoundCurve> cc = MakeOpenLineAndArc();
  cc->setSRID(3857);

  std::unique_ptr<CompoundCurve> cloned = cc->clone();
  ASSERT_NE(cloned, nullptr);
  EXPECT_NE(cloned.get(), cc.get());
  EXPECT_EQ(cloned->getSRID(), 3857);
  EXPECT_TRUE(cloned->equalsExact(cc.get()));
  EXPECT_TRUE(cloned->equalsIdentical(cc.get()));

  const Geometry* base = cc.get();
  std::unique_ptr<Geometry> base_cloned = base->clone();
  ASSERT_NE(base_cloned, nullptr);
  EXPECT_EQ(base_cloned->getGeometryTypeId(), geos::geom::GEOS_COMPOUNDCURVE);
  EXPECT_TRUE(base_cloned->equalsIdentical(cc.get()));

  std::unique_ptr<CompoundCurve> empty_cc = factory_->createCompoundCurve();
  std::unique_ptr<CompoundCurve> empty_cloned = empty_cc->clone();
  EXPECT_TRUE(empty_cloned->isEmpty());
  EXPECT_TRUE(empty_cloned->equalsIdentical(empty_cc.get()));

  // Exercise protected CompoundCurve copy constructor and operator= via
  // subclass. Geometry::isEquivalentClass checks typeid(*this) ==
  // typeid(*other), so a subclass is not equivalent-class to CompoundCurve
  // itself, while clone() returns a CompoundCurve and two
  // SubclassCompoundCurves share typeid.
  SubclassCompoundCurve copy_constructed(*cc);
  EXPECT_FALSE(copy_constructed.equalsIdentical(cc.get()));
  EXPECT_EQ(copy_constructed.getSRID(), 3857);
  EXPECT_EQ(copy_constructed.getNumCurves(), 2u);
  EXPECT_EQ(copy_constructed.getNumPoints(), 5u);
  EXPECT_TRUE(copy_constructed.getCurveN(0)->equalsIdentical(cc->getCurveN(0)));
  EXPECT_TRUE(copy_constructed.getCurveN(1)->equalsIdentical(cc->getCurveN(1)));
  EXPECT_TRUE(copy_constructed.getEnvelopeInternal()->equals(
      cc->getEnvelopeInternal()));
  EXPECT_TRUE(copy_constructed.clone()->equalsIdentical(cc.get()));

  std::vector<std::unique_ptr<SimpleCurve>> single_curve;
  single_curve.push_back(MakeLineXY({{0.0, 0.0}, {5.0, 0.0}}));
  std::unique_ptr<CompoundCurve> single =
      factory_->createCompoundCurve(std::move(single_curve));

  SubclassCompoundCurve target(std::vector<std::unique_ptr<SimpleCurve>>{},
                               *factory_);
  EXPECT_TRUE(target.isEmpty());

  target.AssignFrom(*single);
  EXPECT_EQ(target.getNumCurves(), 1u);
  EXPECT_DOUBLE_EQ(target.getLength(), 5.0);
  EXPECT_TRUE(target.clone()->equalsIdentical(single.get()));

  target.AssignFrom(*cc);
  EXPECT_EQ(target.getNumCurves(), 2u);
  EXPECT_EQ(target.getNumPoints(), 5u);
  EXPECT_NEAR(target.getLength(), cc->getLength(), 1e-12);
  EXPECT_TRUE(target.getEnvelopeInternal()->equals(cc->getEnvelopeInternal()));
  EXPECT_TRUE(target.getCurveN(0)->equalsExact(cc->getCurveN(0)));
  EXPECT_TRUE(target.getCurveN(1)->equalsExact(cc->getCurveN(1)));
  EXPECT_TRUE(target.equalsIdentical(&copy_constructed));
  EXPECT_TRUE(target.clone()->equalsIdentical(cc.get()));

  // Self-assignment preserves curves and envelope.
  target.AssignFrom(target);
  EXPECT_TRUE(target.equalsIdentical(&copy_constructed));
  EXPECT_TRUE(target.clone()->equalsIdentical(cc.get()));
  EXPECT_EQ(target.getNumCurves(), 2u);
  EXPECT_TRUE(target.getCurveN(0)->equalsIdentical(cc->getCurveN(0)));
  EXPECT_TRUE(target.getCurveN(1)->equalsIdentical(cc->getCurveN(1)));

  // Reassign with fewer curves (empty) to verify vector shrinking.
  target.AssignFrom(*empty_cc);
  EXPECT_TRUE(target.isEmpty());
  EXPECT_EQ(target.getNumCurves(), 0u);
  EXPECT_TRUE(target.getEnvelopeInternal()->isNull());
  EXPECT_TRUE(target.clone()->equalsIdentical(empty_cc.get()));

  // Mutating original does not alter deep copies.
  ShiftCoordMutator mutate_orig(100.0, 100.0);
  cc->apply_rw(&mutate_orig);
  EXPECT_EQ(cloned->getCurveN(0)->getCoordinateN(0), (CoordinateXY{0.0, 0.0}));
  EXPECT_EQ(copy_constructed.getCurveN(0)->getCoordinateN(0),
            (CoordinateXY{0.0, 0.0}));
}

TEST_F(CompoundCurveTest, ApplyCoordinateFiltersReadOnlyAndReadWrite) {
  std::unique_ptr<CompoundCurve> empty_cc = factory_->createCompoundCurve();
  RecordingCoordInspector empty_inspector;
  empty_cc->apply_ro(&empty_inspector);
  EXPECT_TRUE(empty_inspector.coords.empty());

  RecordingSeqFilter empty_seq_filter;
  empty_cc->apply_ro(empty_seq_filter);
  EXPECT_TRUE(empty_seq_filter.coords.empty());

  ShiftCoordMutator empty_shifter(1.0, 2.0);
  empty_cc->apply_rw(&empty_shifter);
  empty_cc->geometryChanged();
  EXPECT_TRUE(empty_cc->isEmpty());
  EXPECT_TRUE(empty_cc->getEnvelopeInternal()->isNull());

  std::unique_ptr<CompoundCurve> cc = MakeOpenLineAndArc();
  RecordingCoordInspector inspector;
  cc->apply_ro(&inspector);
  ASSERT_EQ(inspector.coords.size(), 5u);
  EXPECT_EQ(inspector.coords[0], (CoordinateXY{0.0, 0.0}));
  EXPECT_EQ(inspector.coords[4], (CoordinateXY{3.0, 0.0}));

  RecordingSeqFilter full_seq_filter;
  cc->apply_ro(full_seq_filter);
  ASSERT_EQ(full_seq_filter.coords.size(), 5u);
  EXPECT_EQ(full_seq_filter.coords[3], (CoordinateXY{2.0, 1.0}));

  RecordingSeqFilter done_immediately_filter(/*stop_after=*/0);
  cc->apply_ro(done_immediately_filter);
  EXPECT_TRUE(done_immediately_filter.coords.empty());

  RecordingSeqFilter early_stop_filter(/*stop_after=*/2);
  cc->apply_ro(early_stop_filter);
  ASSERT_EQ(early_stop_filter.coords.size(), 2u);
  EXPECT_EQ(early_stop_filter.coords[0], (CoordinateXY{0.0, 0.0}));
  EXPECT_EQ(early_stop_filter.coords[1], (CoordinateXY{1.0, 0.0}));

  RecordingSeqFilter mid_second_stop(/*stop_after=*/3);
  cc->apply_ro(mid_second_stop);
  ASSERT_EQ(mid_second_stop.coords.size(), 3u);
  EXPECT_EQ(mid_second_stop.coords[2], (CoordinateXY{1.0, 0.0}));

  // Virtual dispatch through Geometry*.
  const Geometry* base_const = cc.get();
  RecordingCoordInspector base_inspector;
  base_const->apply_ro(&base_inspector);
  EXPECT_EQ(base_inspector.coords.size(), 5u);
  RecordingSeqFilter base_seq_filter;
  base_const->apply_ro(base_seq_filter);
  EXPECT_EQ(base_seq_filter.coords.size(), 5u);

  // Mutate via CoordinateFilter and trigger geometryChangedAction().
  std::unique_ptr<CompoundCurve> before_mutation = cc->clone();
  ShiftCoordMutator shifter(10.0, -5.0);
  Geometry* base_mut = cc.get();
  base_mut->apply_rw(&shifter);
  base_mut->geometryChanged();
  EXPECT_FALSE(cc->getEnvelopeInternal()->isNull());

  std::unique_ptr<CoordinateSequence> shifted = cc->getCoordinates();
  ASSERT_EQ(shifted->size(), 5u);
  EXPECT_EQ(shifted->getAt<CoordinateXY>(0), (CoordinateXY{10.0, -5.0}));
  EXPECT_EQ(shifted->getAt<CoordinateXY>(3), (CoordinateXY{12.0, -4.0}));
  EXPECT_EQ(shifted->getAt<CoordinateXY>(4), (CoordinateXY{13.0, -5.0}));
  EXPECT_TRUE(before_mutation->getCurveN(0)->getCoordinateN(0) ==
              (CoordinateXY{0.0, 0.0}));

  // CoordinateSequenceFilter read-write is unsupported on CompoundCurve.
  EXPECT_THROW(cc->apply_rw(full_seq_filter), UnsupportedOperationException);
  EXPECT_THROW(empty_cc->apply_rw(empty_seq_filter),
               UnsupportedOperationException);
}

TEST_F(CompoundCurveTest, EqualsExactAndEqualsIdentical) {
  std::unique_ptr<CompoundCurve> empty_a = factory_->createCompoundCurve();
  std::unique_ptr<CompoundCurve> empty_b = factory_->createCompoundCurve();
  EXPECT_TRUE(empty_a->equalsExact(empty_b.get()));
  EXPECT_TRUE(empty_a->equalsIdentical(empty_b.get()));

  std::unique_ptr<CompoundCurve> cc = MakeOpenLineAndArc();
  std::unique_ptr<CompoundCurve> same = MakeOpenLineAndArc();
  std::unique_ptr<LineString> line =
      MakeLineXY({{0.0, 0.0}, {1.0, 0.0}, {1.0, 0.0}, {2.0, 1.0}, {3.0, 0.0}});

  EXPECT_FALSE(empty_a->equalsExact(cc.get()));
  EXPECT_FALSE(empty_a->equalsIdentical(cc.get()));

  // Different geometry class.
  EXPECT_FALSE(cc->equalsExact(line.get()));
  EXPECT_FALSE(cc->equalsIdentical(line.get()));

  // Different number of sections.
  std::vector<std::unique_ptr<SimpleCurve>> one_curve;
  one_curve.push_back(MakeLineXY({{0.0, 0.0}, {1.0, 0.0}}));
  std::unique_ptr<CompoundCurve> single =
      factory_->createCompoundCurve(std::move(one_curve));
  EXPECT_FALSE(cc->equalsExact(single.get()));
  EXPECT_FALSE(cc->equalsIdentical(single.get()));

  // Same number of sections, first section differs.
  std::vector<std::unique_ptr<SimpleCurve>> first_differs;
  first_differs.push_back(MakeLineXY({{0.0, 1.0}, {1.0, 0.0}}));
  first_differs.push_back(MakeArcXY({{1.0, 0.0}, {2.0, 1.0}, {3.0, 0.0}}));
  std::unique_ptr<CompoundCurve> first_diff_cc =
      factory_->createCompoundCurve(std::move(first_differs));
  EXPECT_FALSE(cc->equalsExact(first_diff_cc.get()));
  EXPECT_FALSE(cc->equalsIdentical(first_diff_cc.get()));

  // Same number of sections, first matches, second section differs.
  std::vector<std::unique_ptr<SimpleCurve>> second_differs;
  second_differs.push_back(MakeLineXY({{0.0, 0.0}, {1.0, 0.0}}));
  second_differs.push_back(MakeArcXY({{1.0, 0.0}, {2.0, 2.0}, {3.0, 0.0}}));
  std::unique_ptr<CompoundCurve> second_diff_cc =
      factory_->createCompoundCurve(std::move(second_differs));
  EXPECT_FALSE(cc->equalsExact(second_diff_cc.get()));
  EXPECT_FALSE(cc->equalsIdentical(second_diff_cc.get()));

  // Same number of sections, different subcurve type (LineString vs
  // CircularString).
  std::vector<std::unique_ptr<SimpleCurve>> two_lines;
  two_lines.push_back(MakeLineXY({{0.0, 0.0}, {1.0, 0.0}}));
  two_lines.push_back(MakeLineXY({{1.0, 0.0}, {2.0, 1.0}, {3.0, 0.0}}));
  std::unique_ptr<CompoundCurve> two_lines_cc =
      factory_->createCompoundCurve(std::move(two_lines));
  EXPECT_FALSE(cc->equalsExact(two_lines_cc.get()));
  EXPECT_FALSE(cc->equalsIdentical(two_lines_cc.get()));

  // Exact match and tolerance match.
  EXPECT_TRUE(cc->equalsExact(same.get()));
  EXPECT_TRUE(cc->equalsIdentical(same.get()));

  std::vector<std::unique_ptr<SimpleCurve>> perturbed;
  perturbed.push_back(MakeLineXY({{0.0, 0.0}, {1.0, 0.0625}}));
  perturbed.push_back(MakeArcXY({{1.0, 0.0625}, {2.0, 1.0}, {3.0, 0.0}}));
  std::unique_ptr<CompoundCurve> cc_perturbed =
      factory_->createCompoundCurve(std::move(perturbed));
  EXPECT_FALSE(cc->equalsExact(cc_perturbed.get(), 0.0));
  EXPECT_FALSE(cc->equalsExact(cc_perturbed.get(), 0.06));
  EXPECT_TRUE(cc->equalsExact(cc_perturbed.get(), 0.0625));
  EXPECT_TRUE(cc->equalsExact(cc_perturbed.get(), 0.1));
  EXPECT_FALSE(cc->equalsIdentical(cc_perturbed.get()));

  // Identical XY with differing Z or M values.
  std::vector<std::unique_ptr<SimpleCurve>> z_a_curves;
  z_a_curves.push_back(factory_->createLineString(
      MakeSeqXYZ({{0.0, 0.0, 1.0}, {1.0, 0.0, 2.0}})));
  std::vector<std::unique_ptr<SimpleCurve>> z_b_curves;
  z_b_curves.push_back(factory_->createLineString(
      MakeSeqXYZ({{0.0, 0.0, 1.0}, {1.0, 0.0, 9.0}})));
  std::unique_ptr<CompoundCurve> cc_z_a =
      factory_->createCompoundCurve(std::move(z_a_curves));
  std::unique_ptr<CompoundCurve> cc_z_b =
      factory_->createCompoundCurve(std::move(z_b_curves));
  EXPECT_TRUE(cc_z_a->equalsExact(cc_z_b.get()));
  EXPECT_FALSE(cc_z_a->equalsIdentical(cc_z_b.get()));

  std::vector<std::unique_ptr<SimpleCurve>> m_a_curves;
  m_a_curves.push_back(factory_->createLineString(
      MakeSeqXYM({{0.0, 0.0, 1.0}, {1.0, 0.0, 2.0}})));
  std::vector<std::unique_ptr<SimpleCurve>> m_b_curves;
  m_b_curves.push_back(factory_->createLineString(
      MakeSeqXYM({{0.0, 0.0, 1.0}, {1.0, 0.0, 7.0}})));
  std::unique_ptr<CompoundCurve> cc_m_a =
      factory_->createCompoundCurve(std::move(m_a_curves));
  std::unique_ptr<CompoundCurve> cc_m_b =
      factory_->createCompoundCurve(std::move(m_b_curves));
  EXPECT_TRUE(cc_m_a->equalsExact(cc_m_b.get()));
  EXPECT_FALSE(cc_m_a->equalsIdentical(cc_m_b.get()));
}

TEST_F(CompoundCurveTest, MixedZAndMDimensionsAndEqualsIdentical) {
  std::vector<std::unique_ptr<SimpleCurve>> mixed_z;
  mixed_z.push_back(MakeLineXY({{0.0, 0.0}, {1.0, 0.0}}));
  mixed_z.push_back(factory_->createCircularString(
      MakeSeqXYZ({{1.0, 0.0, 10.0}, {2.0, 1.0, 20.0}, {3.0, 0.0, 30.0}})));
  std::unique_ptr<CompoundCurve> cc_z =
      factory_->createCompoundCurve(std::move(mixed_z));

  EXPECT_TRUE(cc_z->hasZ());
  EXPECT_FALSE(cc_z->hasM());
  EXPECT_EQ(cc_z->getCoordinateDimension(), 3);

  std::unique_ptr<CoordinateSequence> z_coords = cc_z->getCoordinates();
  ASSERT_EQ(z_coords->size(), 5u);
  EXPECT_TRUE(z_coords->hasZ());
  EXPECT_FALSE(z_coords->hasM());
  EXPECT_TRUE(std::isnan(z_coords->getAt<Coordinate>(0).z));
  EXPECT_DOUBLE_EQ(z_coords->getAt<Coordinate>(3).z, 20.0);

  // equalsExact ignores Z, while equalsIdentical checks Z.
  std::unique_ptr<CompoundCurve> cc_2d = MakeOpenLineAndArc();
  EXPECT_TRUE(cc_z->equalsExact(cc_2d.get()));
  EXPECT_FALSE(cc_z->equalsIdentical(cc_2d.get()));

  // Mixed Z and M across curves produces 4D coordinates.
  std::vector<std::unique_ptr<SimpleCurve>> mixed_zm;
  mixed_zm.push_back(factory_->createLineString(
      MakeSeqXYZ({{0.0, 0.0, 5.0}, {1.0, 0.0, 6.0}})));
  mixed_zm.push_back(factory_->createCircularString(
      MakeSeqXYM({{1.0, 0.0, 1.5}, {2.0, 1.0, 2.5}, {3.0, 0.0, 3.5}})));
  std::unique_ptr<CompoundCurve> cc_zm =
      factory_->createCompoundCurve(std::move(mixed_zm));

  EXPECT_TRUE(cc_zm->hasZ());
  EXPECT_TRUE(cc_zm->hasM());
  EXPECT_EQ(cc_zm->getCoordinateDimension(), 4);

  std::unique_ptr<CoordinateSequence> zm_coords = cc_zm->getCoordinates();
  ASSERT_EQ(zm_coords->size(), 5u);
  EXPECT_DOUBLE_EQ(zm_coords->getAt<CoordinateXYZM>(0).z, 5.0);
  EXPECT_TRUE(std::isnan(zm_coords->getAt<CoordinateXYZM>(0).m));
  EXPECT_TRUE(std::isnan(zm_coords->getAt<CoordinateXYZM>(3).z));
  EXPECT_DOUBLE_EQ(zm_coords->getAt<CoordinateXYZM>(3).m, 2.5);

  std::unique_ptr<CompoundCurve> cc_zm_clone = cc_zm->clone();
  EXPECT_TRUE(cc_zm->equalsIdentical(cc_zm_clone.get()));

  std::vector<std::unique_ptr<SimpleCurve>> full_xyzm;
  full_xyzm.push_back(factory_->createLineString(
      MakeSeqXYZM({{0.0, 0.0, 5.0, 1.0}, {1.0, 0.0, 6.0, 2.0}})));
  std::unique_ptr<CompoundCurve> cc_xyzm =
      factory_->createCompoundCurve(std::move(full_xyzm));
  EXPECT_TRUE(cc_xyzm->hasZ());
  EXPECT_TRUE(cc_xyzm->hasM());
  EXPECT_EQ(cc_xyzm->getCoordinateDimension(), 4);

  // XYM-only CompoundCurve (hasM true, hasZ false, dim = 3).
  std::vector<std::unique_ptr<SimpleCurve>> only_m;
  only_m.push_back(factory_->createLineString(
      MakeSeqXYM({{0.0, 0.0, 1.0}, {1.0, 0.0, 2.0}})));
  std::unique_ptr<CompoundCurve> cc_m =
      factory_->createCompoundCurve(std::move(only_m));
  EXPECT_FALSE(cc_m->hasZ());
  EXPECT_TRUE(cc_m->hasM());
  EXPECT_EQ(cc_m->getCoordinateDimension(), 3);

  std::vector<std::unique_ptr<SimpleCurve>> only_xy;
  only_xy.push_back(MakeLineXY({{0.0, 0.0}, {1.0, 0.0}}));
  std::unique_ptr<CompoundCurve> cc_xy =
      factory_->createCompoundCurve(std::move(only_xy));
  EXPECT_TRUE(cc_m->equalsExact(cc_xy.get()));
  EXPECT_FALSE(cc_m->equalsIdentical(cc_xy.get()));

  // XYZ first curve + XY second curve, and XY first curve + XYM second curve.
  std::vector<std::unique_ptr<SimpleCurve>> z_first;
  z_first.push_back(factory_->createLineString(
      MakeSeqXYZ({{0.0, 0.0, 3.0}, {1.0, 0.0, 4.0}})));
  z_first.push_back(MakeLineXY({{1.0, 0.0}, {2.0, 0.0}}));
  std::unique_ptr<CompoundCurve> cc_z_first =
      factory_->createCompoundCurve(std::move(z_first));
  EXPECT_TRUE(cc_z_first->hasZ());
  EXPECT_FALSE(cc_z_first->hasM());
  EXPECT_EQ(cc_z_first->getCoordinateDimension(), 3);

  std::vector<std::unique_ptr<SimpleCurve>> m_second;
  m_second.push_back(MakeLineXY({{0.0, 0.0}, {1.0, 0.0}}));
  m_second.push_back(factory_->createLineString(
      MakeSeqXYM({{1.0, 0.0, 7.0}, {2.0, 0.0, 8.0}})));
  std::unique_ptr<CompoundCurve> cc_m_second =
      factory_->createCompoundCurve(std::move(m_second));
  EXPECT_FALSE(cc_m_second->hasZ());
  EXPECT_TRUE(cc_m_second->hasM());
  EXPECT_EQ(cc_m_second->getCoordinateDimension(), 3);
  std::unique_ptr<CoordinateSequence> m_second_coords =
      cc_m_second->getCoordinates();
  CoordinateXYM first_m;
  CoordinateXYM last_m;
  m_second_coords->getAt(0, first_m);
  m_second_coords->getAt(3, last_m);
  EXPECT_TRUE(std::isnan(first_m.m));
  EXPECT_DOUBLE_EQ(last_m.m, 8.0);
  EXPECT_DOUBLE_EQ(m_second_coords->getOrdinate(3, CoordinateSequence::M), 8.0);

  // Reversing mixed Z/M CompoundCurve preserves Z and M dimensions.
  std::unique_ptr<CompoundCurve> cc_zm_rev = cc_zm->reverse();
  ASSERT_NE(cc_zm_rev, nullptr);
  EXPECT_TRUE(cc_zm_rev->hasZ());
  EXPECT_TRUE(cc_zm_rev->hasM());
  EXPECT_EQ(cc_zm_rev->getCoordinateDimension(), 4);
  EXPECT_EQ(cc_zm_rev->getCurveN(0)->getGeometryTypeId(),
            geos::geom::GEOS_CIRCULARSTRING);
  EXPECT_EQ(cc_zm_rev->getCurveN(1)->getGeometryTypeId(),
            geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(cc_zm_rev->reverse()->equalsIdentical(cc_zm.get()));
}

TEST_F(CompoundCurveTest, CompareToAndSortIndex) {
  std::unique_ptr<CompoundCurve> empty_a = factory_->createCompoundCurve();
  std::unique_ptr<CompoundCurve> empty_b = factory_->createCompoundCurve();
  std::unique_ptr<CompoundCurve> cc_a = MakeOpenLineAndArc();
  std::unique_ptr<CompoundCurve> cc_same = MakeOpenLineAndArc();

  std::vector<std::unique_ptr<SimpleCurve>> higher_curves;
  higher_curves.push_back(MakeLineXY({{0.0, 0.0}, {2.0, 0.0}}));
  higher_curves.push_back(MakeArcXY({{2.0, 0.0}, {3.0, 1.0}, {4.0, 0.0}}));
  std::unique_ptr<CompoundCurve> cc_b =
      factory_->createCompoundCurve(std::move(higher_curves));

  std::vector<std::unique_ptr<SimpleCurve>> second_higher_curves;
  second_higher_curves.push_back(MakeLineXY({{0.0, 0.0}, {1.0, 0.0}}));
  second_higher_curves.push_back(
      MakeArcXY({{1.0, 0.0}, {2.0, 2.0}, {3.0, 0.0}}));
  std::unique_ptr<CompoundCurve> cc_second_higher =
      factory_->createCompoundCurve(std::move(second_higher_curves));

  std::vector<std::unique_ptr<SimpleCurve>> prefix_curves;
  prefix_curves.push_back(MakeLineXY({{0.0, 0.0}, {1.0, 0.0}}));
  std::unique_ptr<CompoundCurve> cc_prefix =
      factory_->createCompoundCurve(std::move(prefix_curves));

  EXPECT_EQ(empty_a->compareTo(empty_b.get()), 0);
  EXPECT_LT(empty_a->compareTo(cc_a.get()), 0);
  EXPECT_GT(cc_a->compareTo(empty_a.get()), 0);
  EXPECT_EQ(cc_a->compareTo(cc_a.get()), 0);
  EXPECT_EQ(cc_a->compareTo(cc_same.get()), 0);
  EXPECT_LT(cc_a->compareTo(cc_b.get()), 0);
  EXPECT_GT(cc_b->compareTo(cc_a.get()), 0);
  EXPECT_LT(cc_a->compareTo(cc_second_higher.get()), 0);
  EXPECT_GT(cc_second_higher->compareTo(cc_a.get()), 0);
  EXPECT_LT(cc_prefix->compareTo(cc_a.get()), 0);
  EXPECT_GT(cc_a->compareTo(cc_prefix.get()), 0);

  // CompoundCurve sorts after Point, LineString, CircularString, and before
  // MultiCurve (SORTINDEX_COMPOUNDCURVE = 9).
  std::unique_ptr<Point> pt = factory_->createPoint(CoordinateXY{0.0, 0.0});
  std::unique_ptr<LineString> ls = MakeLineXY({{0.0, 0.0}, {1.0, 0.0}});
  std::unique_ptr<CircularString> arc =
      MakeArcXY({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}});
  std::vector<std::unique_ptr<Curve>> mc_curves;
  mc_curves.push_back(MakeLineXY({{0.0, 0.0}, {1.0, 0.0}}));
  std::unique_ptr<MultiCurve> mc =
      factory_->createMultiCurve(std::move(mc_curves));
  EXPECT_GT(cc_a->compareTo(pt.get()), 0);
  EXPECT_GT(cc_a->compareTo(ls.get()), 0);
  EXPECT_LT(ls->compareTo(cc_a.get()), 0);
  EXPECT_GT(cc_a->compareTo(arc.get()), 0);
  EXPECT_LT(arc->compareTo(cc_a.get()), 0);
  EXPECT_LT(cc_a->compareTo(mc.get()), 0);
  EXPECT_GT(mc->compareTo(cc_a.get()), 0);
}

TEST_F(CompoundCurveTest, ReverseEmptyAndMultiCurve) {
  std::unique_ptr<CompoundCurve> empty_cc = factory_->createCompoundCurve();
  std::unique_ptr<CompoundCurve> empty_rev = empty_cc->reverse();
  ASSERT_NE(empty_rev, nullptr);
  EXPECT_TRUE(empty_rev->isEmpty());
  EXPECT_EQ(empty_rev->getGeometryTypeId(), geos::geom::GEOS_COMPOUNDCURVE);

  std::unique_ptr<CompoundCurve> cc = MakeOpenLineAndArc();
  std::unique_ptr<CompoundCurve> rev = cc->reverse();
  ASSERT_NE(rev, nullptr);
  EXPECT_EQ(rev->getNumCurves(), 2u);
  EXPECT_EQ(rev->getCurveN(0)->getGeometryTypeId(),
            geos::geom::GEOS_CIRCULARSTRING);
  EXPECT_EQ(rev->getCurveN(1)->getGeometryTypeId(),
            geos::geom::GEOS_LINESTRING);
  EXPECT_NEAR(rev->getLength(), cc->getLength(), 1e-12);
  EXPECT_EQ(rev->getCurveN(0)->getCoordinateN(0), (CoordinateXY{3.0, 0.0}));
  EXPECT_EQ(rev->getCurveN(1)->getCoordinateN(1), (CoordinateXY{0.0, 0.0}));
  EXPECT_TRUE(rev->reverse()->equalsExact(cc.get()));

  const Geometry* base = cc.get();
  std::unique_ptr<Geometry> base_rev = base->reverse();
  EXPECT_EQ(base_rev->getGeometryTypeId(), geos::geom::GEOS_COMPOUNDCURVE);
  EXPECT_TRUE(base_rev->equalsExact(rev.get()));
}

TEST_F(CompoundCurveTest, NormalizeAndCurvedBoundaryThrow) {
  std::unique_ptr<CompoundCurve> cc = MakeOpenLineAndArc();
  EXPECT_THROW(cc->normalize(), UnsupportedOperationException);
  // Note: BoundaryOp::getBoundary() recursively calls m_geom.getBoundary() for
  // non-curved CompoundCurve/empty CompoundCurve in GEOS 3.14, so only invoke
  // getBoundary() on a CompoundCurve with curved components.
  EXPECT_THROW(cc->getBoundary(), UnsupportedOperationException);

  std::vector<std::unique_ptr<SimpleCurve>> single_arc_curves;
  single_arc_curves.push_back(MakeArcXY({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}));
  std::unique_ptr<CompoundCurve> single_arc_cc =
      factory_->createCompoundCurve(std::move(single_arc_curves));
  EXPECT_THROW(single_arc_cc->getBoundary(), UnsupportedOperationException);

  std::vector<std::unique_ptr<SimpleCurve>> linear_curves;
  linear_curves.push_back(MakeLineXY({{0.0, 0.0}, {1.0, 0.0}}));
  std::unique_ptr<CompoundCurve> linear_cc =
      factory_->createCompoundCurve(std::move(linear_curves));
  EXPECT_THROW(linear_cc->normalize(), UnsupportedOperationException);

  std::unique_ptr<CompoundCurve> empty_cc = factory_->createCompoundCurve();
  EXPECT_THROW(empty_cc->normalize(), UnsupportedOperationException);
}

TEST_F(CompoundCurveTest, WktRoundTrip) {
  WKTReader reader(*factory_);
  std::unique_ptr<CompoundCurve> parsed = reader.read<CompoundCurve>(
      "COMPOUNDCURVE ((0 0, 1 0), CIRCULARSTRING (1 0, 2 1, 3 0))");
  ASSERT_NE(parsed, nullptr);
  EXPECT_EQ(parsed->getGeometryTypeId(), geos::geom::GEOS_COMPOUNDCURVE);
  EXPECT_EQ(parsed->getNumCurves(), 2u);
  EXPECT_NEAR(parsed->getLength(), 1.0 + std::numbers::pi, 1e-12);
  EXPECT_EQ(parsed->toText(),
            "COMPOUNDCURVE ((0 0, 1 0), CIRCULARSTRING (1 0, 2 1, 3 0))");

  std::unique_ptr<CompoundCurve> parsed_empty =
      reader.read<CompoundCurve>("COMPOUNDCURVE EMPTY");
  ASSERT_NE(parsed_empty, nullptr);
  EXPECT_TRUE(parsed_empty->isEmpty());
  EXPECT_EQ(parsed_empty->toText(), "COMPOUNDCURVE EMPTY");
}

}  // namespace
}  // namespace geos_google
