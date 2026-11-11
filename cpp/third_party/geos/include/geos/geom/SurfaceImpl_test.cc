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

#include "third_party/geos/include/geos/geom/SurfaceImpl.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <numbers>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/CompoundCurve.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Curve.h"
#include "third_party/geos/include/geos/geom/CurvePolygon.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryComponentFilter.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/SimpleCurve.h"
#include "third_party/geos/include/geos/geom/Surface.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;

using geos::geom::CircularString;
using geos::geom::CompoundCurve;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Curve;
using geos::geom::CurvePolygon;
using geos::geom::Dimension;
using geos::geom::Geometry;
using geos::geom::GeometryComponentFilter;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::Polygon;
using geos::geom::SimpleCurve;
using geos::geom::Surface;
using geos::util::IllegalArgumentException;

std::unique_ptr<CoordinateSequence> MakeXYSeq(
    std::initializer_list<CoordinateXY> coords) {
  return std::make_unique<CoordinateSequence>(coords);
}

std::unique_ptr<LinearRing> MakeRectRing(const GeometryFactory& factory,
                                         double min_x, double min_y,
                                         double max_x, double max_y) {
  return factory.createLinearRing(MakeXYSeq({
      {min_x, min_y},
      {max_x, min_y},
      {max_x, max_y},
      {min_x, max_y},
      {min_x, min_y},
  }));
}

std::unique_ptr<CircularString> MakeCircleCurve(const GeometryFactory& factory,
                                                double cx, double cy,
                                                double radius) {
  return factory.createCircularString(MakeXYSeq({
      {cx - radius, cy},
      {cx, cy + radius},
      {cx + radius, cy},
      {cx, cy - radius},
      {cx - radius, cy},
  }));
}

std::unique_ptr<CompoundCurve> MakeDomeShell(const GeometryFactory& factory) {
  std::vector<std::unique_ptr<SimpleCurve>> sections;
  sections.push_back(factory.createCircularString(
      MakeXYSeq({{0.0, 0.0}, {10.0, 10.0}, {20.0, 0.0}})));
  sections.push_back(factory.createLineString(
      MakeXYSeq({{20.0, 0.0}, {20.0, -10.0}, {0.0, -10.0}, {0.0, 0.0}})));
  return factory.createCompoundCurve(std::move(sections));
}

std::unique_ptr<CompoundCurve> MakeRectCompoundHole(
    const GeometryFactory& factory) {
  std::vector<std::unique_ptr<SimpleCurve>> sections;
  sections.push_back(factory.createLineString(
      MakeXYSeq({{4.0, -8.0}, {16.0, -8.0}, {16.0, -2.0}})));
  sections.push_back(factory.createLineString(
      MakeXYSeq({{16.0, -2.0}, {4.0, -2.0}, {4.0, -8.0}})));
  return factory.createCompoundCurve(std::move(sections));
}

template <typename Fn>
void ExpectIllegalArg(Fn&& fn, std::string_view expected_substr) {
  try {
    fn();
    FAIL() << "Expected IllegalArgumentException containing: "
           << expected_substr;
  } catch (const IllegalArgumentException& e) {
    EXPECT_THAT(e.what(), HasSubstr(expected_substr));
  }
}

class RingSridMutatingComponentFilter : public GeometryComponentFilter {
 public:
  explicit RingSridMutatingComponentFilter(int srid_step)
      : next_srid_(srid_step) {}

  void filter_rw(Geometry* geom) override {
    visited_.push_back(geom);
    geom->setSRID(next_srid_++);
  }

  const std::vector<Geometry*>& visited() const { return visited_; }

 private:
  int next_srid_;
  std::vector<Geometry*> visited_;
};

class SurfaceImplTest : public ::testing::Test {
 protected:
  GeometryFactory::Ptr factory_ = GeometryFactory::create();
};

TEST_F(SurfaceImplTest, PolygonTwoArgConstructorValidEmptyAndNullShell) {
  auto shell = MakeRectRing(*factory_, 0.0, 0.0, 20.0, 20.0);
  const LinearRing* shell_ptr = shell.get();
  auto poly = factory_->createPolygon(std::move(shell));

  EXPECT_FALSE(poly->isEmpty());
  EXPECT_EQ(poly->getDimension(), Dimension::A);
  EXPECT_EQ(poly->getBoundaryDimension(), 1);
  EXPECT_EQ(poly->getNumInteriorRing(), 0u);
  EXPECT_EQ(poly->getExteriorRing(), shell_ptr);
  EXPECT_DOUBLE_EQ(poly->getArea(), 400.0);
  EXPECT_DOUBLE_EQ(poly->getLength(), 80.0);

  auto empty_poly = factory_->createPolygon(factory_->createLinearRing());
  EXPECT_TRUE(empty_poly->isEmpty());
  EXPECT_EQ(empty_poly->getNumInteriorRing(), 0u);
  ASSERT_NE(empty_poly->getExteriorRing(), nullptr);
  EXPECT_TRUE(empty_poly->getExteriorRing()->isEmpty());
  EXPECT_DOUBLE_EQ(empty_poly->getArea(), 0.0);

  std::unique_ptr<LinearRing> null_shell;
  auto null_poly = factory_->createPolygon(std::move(null_shell));
  EXPECT_TRUE(null_poly->isEmpty());
  EXPECT_EQ(null_poly->getNumInteriorRing(), 0u);
  ASSERT_NE(null_poly->getExteriorRing(), nullptr);
  EXPECT_TRUE(null_poly->getExteriorRing()->isEmpty());
  EXPECT_EQ(null_poly->getExteriorRing()->getGeometryTypeId(),
            geos::geom::GEOS_LINEARRING);
  EXPECT_DOUBLE_EQ(null_poly->getArea(), 0.0);
}

TEST_F(SurfaceImplTest, CurvePolygonTwoArgConstructorValidEmptyAndNullShell) {
  auto shell = MakeCircleCurve(*factory_, 10.0, 0.0, 10.0);
  const Curve* shell_ptr = shell.get();
  auto cp = factory_->createCurvePolygon(std::move(shell));

  EXPECT_FALSE(cp->isEmpty());
  EXPECT_TRUE(cp->hasCurvedComponents());
  EXPECT_EQ(cp->getDimension(), Dimension::A);
  EXPECT_EQ(cp->getBoundaryDimension(), 1);
  EXPECT_EQ(cp->getNumInteriorRing(), 0u);
  EXPECT_EQ(cp->getExteriorRing(), shell_ptr);
  EXPECT_EQ(cp->getExteriorRing()->getGeometryTypeId(),
            geos::geom::GEOS_CIRCULARSTRING);
  EXPECT_NEAR(cp->getArea(), 100.0 * std::numbers::pi, 1e-9);
  EXPECT_NEAR(cp->getLength(), 20.0 * std::numbers::pi, 1e-9);

  auto empty_cp = factory_->createCurvePolygon(
      factory_->createCircularString(false, false));
  EXPECT_TRUE(empty_cp->isEmpty());
  EXPECT_EQ(empty_cp->getNumInteriorRing(), 0u);
  ASSERT_NE(empty_cp->getExteriorRing(), nullptr);
  EXPECT_TRUE(empty_cp->getExteriorRing()->isEmpty());
  EXPECT_EQ(empty_cp->getExteriorRing()->getGeometryTypeId(),
            geos::geom::GEOS_CIRCULARSTRING);

  std::unique_ptr<Curve> null_shell;
  auto null_cp = factory_->createCurvePolygon(std::move(null_shell));
  EXPECT_TRUE(null_cp->isEmpty());
  EXPECT_EQ(null_cp->getNumInteriorRing(), 0u);
  ASSERT_NE(null_cp->getExteriorRing(), nullptr);
  EXPECT_TRUE(null_cp->getExteriorRing()->isEmpty());
  EXPECT_EQ(null_cp->getExteriorRing()->getGeometryTypeId(),
            geos::geom::GEOS_LINEARRING);
  EXPECT_DOUBLE_EQ(null_cp->getArea(), 0.0);
}

TEST_F(SurfaceImplTest, PolygonThreeArgConstructorValidAndEdgeCases) {
  auto shell = MakeRectRing(*factory_, 0.0, 0.0, 20.0, 20.0);
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(MakeRectRing(*factory_, 2.0, 2.0, 6.0, 6.0));
  holes.push_back(MakeRectRing(*factory_, 10.0, 10.0, 14.0, 14.0));
  const LinearRing* hole0 = holes[0].get();
  const LinearRing* hole1 = holes[1].get();

  auto poly = factory_->createPolygon(std::move(shell), std::move(holes));
  ASSERT_EQ(poly->getNumInteriorRing(), 2u);
  EXPECT_EQ(poly->getInteriorRingN(0), hole0);
  EXPECT_EQ(poly->getInteriorRingN(1), hole1);
  EXPECT_DOUBLE_EQ(poly->getArea(), 368.0);
  EXPECT_DOUBLE_EQ(poly->getLength(), 112.0);

  // Valid shell with empty holes vector.
  auto poly_no_holes =
      factory_->createPolygon(MakeRectRing(*factory_, 0.0, 0.0, 5.0, 5.0),
                              std::vector<std::unique_ptr<LinearRing>>{});
  EXPECT_EQ(poly_no_holes->getNumInteriorRing(), 0u);
  EXPECT_DOUBLE_EQ(poly_no_holes->getArea(), 25.0);

  // Valid shell with an empty hole ring.
  std::vector<std::unique_ptr<LinearRing>> empty_hole_vec;
  empty_hole_vec.push_back(factory_->createLinearRing());
  auto poly_empty_hole = factory_->createPolygon(
      MakeRectRing(*factory_, 0.0, 0.0, 5.0, 5.0), std::move(empty_hole_vec));
  ASSERT_EQ(poly_empty_hole->getNumInteriorRing(), 1u);
  EXPECT_TRUE(poly_empty_hole->getInteriorRingN(0)->isEmpty());
  EXPECT_DOUBLE_EQ(poly_empty_hole->getArea(), 25.0);

  // Null shell with empty holes vector.
  std::unique_ptr<LinearRing> null_shell;
  auto null_poly = factory_->createPolygon(
      std::move(null_shell), std::vector<std::unique_ptr<LinearRing>>{});
  EXPECT_TRUE(null_poly->isEmpty());
  EXPECT_EQ(null_poly->getNumInteriorRing(), 0u);
  EXPECT_TRUE(null_poly->getExteriorRing()->isEmpty());

  // Null shell with an empty hole ring is permitted because no hole is
  // non-empty and no hole is null.
  std::vector<std::unique_ptr<LinearRing>> empty_holes_for_null_shell;
  empty_holes_for_null_shell.push_back(factory_->createLinearRing());
  auto null_poly_empty_hole = factory_->createPolygon(
      std::unique_ptr<LinearRing>{}, std::move(empty_holes_for_null_shell));
  EXPECT_TRUE(null_poly_empty_hole->isEmpty());
  ASSERT_EQ(null_poly_empty_hole->getNumInteriorRing(), 1u);
  EXPECT_TRUE(null_poly_empty_hole->getInteriorRingN(0)->isEmpty());
}

TEST_F(SurfaceImplTest, PolygonThreeArgConstructorThrowsOnInvalidInputs) {
  ExpectIllegalArg(
      [&] {
        std::vector<std::unique_ptr<LinearRing>> holes;
        holes.push_back(MakeRectRing(*factory_, 1.0, 1.0, 2.0, 2.0));
        factory_->createPolygon(factory_->createLinearRing(), std::move(holes));
      },
      "shell is empty but holes are not");

  ExpectIllegalArg(
      [&] {
        std::vector<std::unique_ptr<LinearRing>> holes;
        holes.push_back(MakeRectRing(*factory_, 1.0, 1.0, 2.0, 2.0));
        factory_->createPolygon(std::unique_ptr<LinearRing>{},
                                std::move(holes));
      },
      "shell is empty but holes are not");

  ExpectIllegalArg(
      [&] {
        std::vector<std::unique_ptr<LinearRing>> holes;
        holes.push_back(nullptr);
        factory_->createPolygon(MakeRectRing(*factory_, 0.0, 0.0, 10.0, 10.0),
                                std::move(holes));
      },
      "holes must not contain null elements");

  ExpectIllegalArg(
      [&] {
        std::vector<std::unique_ptr<LinearRing>> holes;
        holes.push_back(MakeRectRing(*factory_, 1.0, 1.0, 2.0, 2.0));
        holes.push_back(nullptr);
        factory_->createPolygon(MakeRectRing(*factory_, 0.0, 0.0, 10.0, 10.0),
                                std::move(holes));
      },
      "holes must not contain null elements");
}

TEST_F(SurfaceImplTest, CurvePolygonThreeArgConstructorValidAndEdgeCases) {
  auto shell = MakeCircleCurve(*factory_, 10.0, 0.0, 10.0);
  std::vector<std::unique_ptr<Curve>> holes;
  holes.push_back(MakeCircleCurve(*factory_, 6.0, 0.0, 2.0));
  holes.push_back(MakeRectRing(*factory_, 12.0, -2.0, 16.0, 2.0));
  const Curve* hole0 = holes[0].get();
  const Curve* hole1 = holes[1].get();

  auto cp = factory_->createCurvePolygon(std::move(shell), std::move(holes));
  ASSERT_EQ(cp->getNumInteriorRing(), 2u);
  EXPECT_EQ(cp->getInteriorRingN(0), hole0);
  EXPECT_EQ(cp->getInteriorRingN(1), hole1);
  EXPECT_TRUE(cp->hasCurvedComponents());
  EXPECT_NEAR(cp->getArea(), 96.0 * std::numbers::pi - 16.0, 1e-9);
  EXPECT_NEAR(cp->getLength(), 24.0 * std::numbers::pi + 16.0, 1e-9);

  // CompoundCurve shell and CompoundCurve hole.
  std::vector<std::unique_ptr<Curve>> compound_holes;
  compound_holes.push_back(MakeRectCompoundHole(*factory_));
  auto dome_cp = factory_->createCurvePolygon(MakeDomeShell(*factory_),
                                              std::move(compound_holes));
  ASSERT_EQ(dome_cp->getNumInteriorRing(), 1u);
  EXPECT_EQ(dome_cp->getExteriorRing()->getGeometryTypeId(),
            geos::geom::GEOS_COMPOUNDCURVE);
  EXPECT_EQ(dome_cp->getInteriorRingN(0)->getGeometryTypeId(),
            geos::geom::GEOS_COMPOUNDCURVE);
  EXPECT_NEAR(dome_cp->getArea(), 50.0 * std::numbers::pi + 128.0, 1e-9);
  EXPECT_NEAR(dome_cp->getLength(), 10.0 * std::numbers::pi + 76.0, 1e-9);

  // Null shell with empty holes vector.
  auto null_cp = factory_->createCurvePolygon(
      std::unique_ptr<Curve>{}, std::vector<std::unique_ptr<Curve>>{});
  EXPECT_TRUE(null_cp->isEmpty());
  EXPECT_EQ(null_cp->getNumInteriorRing(), 0u);
  EXPECT_EQ(null_cp->getExteriorRing()->getGeometryTypeId(),
            geos::geom::GEOS_LINEARRING);

  // Null shell with an empty CircularString hole.
  std::vector<std::unique_ptr<Curve>> empty_curve_holes;
  empty_curve_holes.push_back(factory_->createCircularString(false, false));
  auto null_cp_empty_hole = factory_->createCurvePolygon(
      std::unique_ptr<Curve>{}, std::move(empty_curve_holes));
  EXPECT_TRUE(null_cp_empty_hole->isEmpty());
  ASSERT_EQ(null_cp_empty_hole->getNumInteriorRing(), 1u);
  EXPECT_TRUE(null_cp_empty_hole->getInteriorRingN(0)->isEmpty());
}

TEST_F(SurfaceImplTest, CurvePolygonThreeArgConstructorThrowsOnInvalidInputs) {
  ExpectIllegalArg(
      [&] {
        std::vector<std::unique_ptr<Curve>> holes;
        holes.push_back(MakeCircleCurve(*factory_, 0.0, 0.0, 1.0));
        factory_->createCurvePolygon(
            factory_->createCircularString(false, false), std::move(holes));
      },
      "shell is empty but holes are not");

  ExpectIllegalArg(
      [&] {
        std::vector<std::unique_ptr<Curve>> holes;
        holes.push_back(MakeCircleCurve(*factory_, 0.0, 0.0, 1.0));
        factory_->createCurvePolygon(std::unique_ptr<Curve>{},
                                     std::move(holes));
      },
      "shell is empty but holes are not");

  ExpectIllegalArg(
      [&] {
        std::vector<std::unique_ptr<Curve>> holes;
        holes.push_back(nullptr);
        factory_->createCurvePolygon(MakeCircleCurve(*factory_, 0.0, 0.0, 5.0),
                                     std::move(holes));
      },
      "holes must not contain null elements");

  ExpectIllegalArg(
      [&] {
        std::vector<std::unique_ptr<Curve>> holes;
        holes.push_back(MakeCircleCurve(*factory_, 0.0, 0.0, 1.0));
        holes.push_back(nullptr);
        factory_->createCurvePolygon(MakeCircleCurve(*factory_, 0.0, 0.0, 5.0),
                                     std::move(holes));
      },
      "holes must not contain null elements");
}

TEST_F(SurfaceImplTest, CopyConstructorDeepCopiesPolygonAndCurvePolygon) {
  auto srid_factory = GeometryFactory::create(nullptr, 4326);

  // Empty Polygon clone.
  auto empty_poly = srid_factory->createPolygon();
  auto empty_poly_clone = empty_poly->clone();
  EXPECT_TRUE(empty_poly_clone->isEmpty());
  EXPECT_NE(empty_poly_clone->getExteriorRing(), empty_poly->getExteriorRing());
  EXPECT_EQ(empty_poly_clone->getSRID(), 4326);

  // Polygon with holes clone.
  std::vector<std::unique_ptr<LinearRing>> poly_holes;
  poly_holes.push_back(MakeRectRing(*srid_factory, 2.0, 2.0, 5.0, 5.0));
  poly_holes.push_back(MakeRectRing(*srid_factory, 10.0, 10.0, 12.0, 12.0));
  auto poly = srid_factory->createPolygon(
      MakeRectRing(*srid_factory, 0.0, 0.0, 20.0, 20.0), std::move(poly_holes));

  auto poly_clone = poly->clone();
  ASSERT_EQ(poly_clone->getNumInteriorRing(), 2u);
  EXPECT_EQ(poly_clone->getSRID(), 4326);
  EXPECT_NE(poly_clone->getExteriorRing(), poly->getExteriorRing());
  EXPECT_NE(poly_clone->getInteriorRingN(0), poly->getInteriorRingN(0));
  EXPECT_NE(poly_clone->getInteriorRingN(1), poly->getInteriorRingN(1));
  EXPECT_TRUE(poly_clone->equalsIdentical(poly.get()));

  // Mutating clone's ring does not affect original.
  poly_clone->getExteriorRing()->setSRID(999);
  EXPECT_EQ(poly->getExteriorRing()->getSRID(), 4326);

  // Empty CurvePolygon clone.
  auto empty_cp = srid_factory->createCurvePolygon(false, false);
  auto empty_cp_clone_geom = empty_cp->clone();
  auto* empty_cp_clone = dynamic_cast<CurvePolygon*>(empty_cp_clone_geom.get());
  ASSERT_NE(empty_cp_clone, nullptr);
  EXPECT_TRUE(empty_cp_clone->isEmpty());
  EXPECT_NE(empty_cp_clone->getExteriorRing(), empty_cp->getExteriorRing());

  // CurvePolygon with holes clone.
  std::vector<std::unique_ptr<Curve>> cp_holes;
  cp_holes.push_back(MakeCircleCurve(*srid_factory, 6.0, 0.0, 2.0));
  cp_holes.push_back(MakeRectRing(*srid_factory, 12.0, -2.0, 16.0, 2.0));
  auto cp = srid_factory->createCurvePolygon(
      MakeCircleCurve(*srid_factory, 10.0, 0.0, 10.0), std::move(cp_holes));

  auto cp_clone_geom = cp->clone();
  auto* cp_clone = dynamic_cast<CurvePolygon*>(cp_clone_geom.get());
  ASSERT_NE(cp_clone, nullptr);
  ASSERT_EQ(cp_clone->getNumInteriorRing(), 2u);
  EXPECT_EQ(cp_clone->getSRID(), 4326);
  EXPECT_NE(cp_clone->getExteriorRing(), cp->getExteriorRing());
  EXPECT_NE(cp_clone->getInteriorRingN(0), cp->getInteriorRingN(0));
  EXPECT_NE(cp_clone->getInteriorRingN(1), cp->getInteriorRingN(1));
  EXPECT_TRUE(cp_clone->equalsIdentical(cp.get()));

  cp_clone->getInteriorRingN(0)->setSRID(777);
  EXPECT_EQ(cp->getInteriorRingN(0)->getSRID(), 4326);
}

TEST_F(SurfaceImplTest, ConstAndNonConstAccessorsAndCovariantReturnTypes) {
  static_assert(
      std::is_same_v<decltype(std::declval<const Polygon&>().getExteriorRing()),
                     const LinearRing*>);
  static_assert(
      std::is_same_v<decltype(std::declval<Polygon&>().getExteriorRing()),
                     LinearRing*>);
  static_assert(std::is_same_v<
                decltype(std::declval<const Polygon&>().getInteriorRingN(0)),
                const LinearRing*>);
  static_assert(
      std::is_same_v<decltype(std::declval<Polygon&>().getInteriorRingN(0)),
                     LinearRing*>);

  static_assert(std::is_same_v<
                decltype(std::declval<const CurvePolygon&>().getExteriorRing()),
                const Curve*>);
  static_assert(
      std::is_same_v<decltype(std::declval<CurvePolygon&>().getExteriorRing()),
                     Curve*>);
  static_assert(std::is_same_v<decltype(std::declval<const CurvePolygon&>()
                                            .getInteriorRingN(0)),
                               const Curve*>);
  static_assert(
      std::is_same_v<
          decltype(std::declval<CurvePolygon&>().getInteriorRingN(0)), Curve*>);
  static_assert(
      std::is_same_v<decltype(std::declval<const Surface&>().getExteriorRing()),
                     const Curve*>);

  std::vector<std::unique_ptr<LinearRing>> poly_holes;
  poly_holes.push_back(MakeRectRing(*factory_, 2.0, 2.0, 4.0, 4.0));
  auto poly = factory_->createPolygon(
      MakeRectRing(*factory_, 0.0, 0.0, 10.0, 10.0), std::move(poly_holes));

  Polygon& mut_poly = *poly;
  const Polygon& const_poly = *poly;
  EXPECT_EQ(mut_poly.getExteriorRing(), const_poly.getExteriorRing());
  EXPECT_EQ(mut_poly.getInteriorRingN(0), const_poly.getInteriorRingN(0));
  EXPECT_EQ(const_poly.getNumInteriorRing(), 1u);

  mut_poly.getExteriorRing()->setSRID(11);
  mut_poly.getInteriorRingN(0)->setSRID(22);
  EXPECT_EQ(const_poly.getExteriorRing()->getSRID(), 11);
  EXPECT_EQ(const_poly.getInteriorRingN(0)->getSRID(), 22);

  const Surface& const_surface = *poly;
  EXPECT_EQ(const_surface.getExteriorRing(), const_poly.getExteriorRing());
  EXPECT_EQ(const_surface.getInteriorRingN(0), const_poly.getInteriorRingN(0));
  EXPECT_EQ(const_surface.getNumInteriorRing(), 1u);

  // Surface::apply_rw dispatches through protected virtual getExteriorRing()
  // and getInteriorRingN(i) on Surface*.
  RingSridMutatingComponentFilter comp_filter(500);
  static_cast<Surface*>(poly.get())->apply_rw(&comp_filter);
  EXPECT_THAT(comp_filter.visited(),
              ElementsAre(poly.get(), mut_poly.getExteriorRing(),
                          mut_poly.getInteriorRingN(0)));
  EXPECT_EQ(const_poly.getExteriorRing()->getSRID(), 501);
  EXPECT_EQ(const_poly.getInteriorRingN(0)->getSRID(), 502);

  std::vector<std::unique_ptr<Curve>> cp_holes;
  cp_holes.push_back(MakeCircleCurve(*factory_, 0.0, 0.0, 2.0));
  auto cp = factory_->createCurvePolygon(
      MakeCircleCurve(*factory_, 0.0, 0.0, 6.0), std::move(cp_holes));

  CurvePolygon& mut_cp = *cp;
  const CurvePolygon& const_cp = *cp;
  EXPECT_EQ(mut_cp.getExteriorRing(), const_cp.getExteriorRing());
  EXPECT_EQ(mut_cp.getInteriorRingN(0), const_cp.getInteriorRingN(0));
  EXPECT_EQ(const_cp.getNumInteriorRing(), 1u);

  mut_cp.getExteriorRing()->setSRID(33);
  mut_cp.getInteriorRingN(0)->setSRID(44);
  EXPECT_EQ(const_cp.getExteriorRing()->getSRID(), 33);
  EXPECT_EQ(const_cp.getInteriorRingN(0)->getSRID(), 44);
}

TEST_F(SurfaceImplTest, ReleaseExteriorAndInteriorRingsPolygon) {
  auto expected_shell = MakeRectRing(*factory_, 0.0, 0.0, 20.0, 20.0);
  auto expected_hole0 = MakeRectRing(*factory_, 2.0, 2.0, 5.0, 5.0);
  auto expected_hole1 = MakeRectRing(*factory_, 10.0, 10.0, 15.0, 15.0);

  // Release exterior first, then interior.
  {
    std::vector<std::unique_ptr<LinearRing>> holes;
    holes.push_back(expected_hole0->clone());
    holes.push_back(expected_hole1->clone());
    auto poly =
        factory_->createPolygon(expected_shell->clone(), std::move(holes));

    std::unique_ptr<LinearRing> ext = poly->releaseExteriorRing();
    ASSERT_NE(ext, nullptr);
    EXPECT_TRUE(ext->equalsIdentical(expected_shell.get()));
    EXPECT_EQ(poly->releaseExteriorRing(), nullptr);

    std::vector<std::unique_ptr<LinearRing>> ints =
        poly->releaseInteriorRings();
    ASSERT_EQ(ints.size(), 2u);
    EXPECT_TRUE(ints[0]->equalsIdentical(expected_hole0.get()));
    EXPECT_TRUE(ints[1]->equalsIdentical(expected_hole1.get()));
    EXPECT_THAT(poly->releaseInteriorRings(), IsEmpty());
  }

  // Release interior first, then exterior.
  {
    std::vector<std::unique_ptr<LinearRing>> holes;
    holes.push_back(expected_hole0->clone());
    auto poly =
        factory_->createPolygon(expected_shell->clone(), std::move(holes));

    auto ints = poly->releaseInteriorRings();
    ASSERT_EQ(ints.size(), 1u);
    EXPECT_TRUE(ints[0]->equalsIdentical(expected_hole0.get()));

    auto ext = poly->releaseExteriorRing();
    ASSERT_NE(ext, nullptr);
    EXPECT_TRUE(ext->equalsIdentical(expected_shell.get()));
  }

  // Partial releases: destructor cleanly destroys unreleased member(s).
  {
    std::vector<std::unique_ptr<LinearRing>> holes;
    holes.push_back(expected_hole0->clone());
    auto poly_ext_only =
        factory_->createPolygon(expected_shell->clone(), std::move(holes));
    auto ext = poly_ext_only->releaseExteriorRing();
    ASSERT_NE(ext, nullptr);
  }
  {
    std::vector<std::unique_ptr<LinearRing>> holes;
    holes.push_back(expected_hole0->clone());
    auto poly_int_only =
        factory_->createPolygon(expected_shell->clone(), std::move(holes));
    auto ints = poly_int_only->releaseInteriorRings();
    ASSERT_EQ(ints.size(), 1u);
  }

  // Empty Polygon release.
  {
    auto empty_poly = factory_->createPolygon();
    auto ext = empty_poly->releaseExteriorRing();
    ASSERT_NE(ext, nullptr);
    EXPECT_TRUE(ext->isEmpty());
    EXPECT_THAT(empty_poly->releaseInteriorRings(), IsEmpty());
  }
}

TEST_F(SurfaceImplTest, ReleaseExteriorAndInteriorRingsCurvePolygon) {
  auto expected_shell = MakeCircleCurve(*factory_, 10.0, 0.0, 10.0);
  auto expected_hole0 = MakeCircleCurve(*factory_, 6.0, 0.0, 2.0);
  auto expected_hole1 = MakeRectRing(*factory_, 12.0, -2.0, 16.0, 2.0);

  // Release exterior first, then interior.
  {
    std::vector<std::unique_ptr<Curve>> holes;
    holes.push_back(expected_hole0->clone());
    holes.push_back(expected_hole1->clone());
    auto cp =
        factory_->createCurvePolygon(expected_shell->clone(), std::move(holes));

    std::unique_ptr<Curve> ext = cp->releaseExteriorRing();
    ASSERT_NE(ext, nullptr);
    EXPECT_EQ(ext->getGeometryTypeId(), geos::geom::GEOS_CIRCULARSTRING);
    EXPECT_TRUE(ext->equalsIdentical(expected_shell.get()));
    EXPECT_EQ(cp->releaseExteriorRing(), nullptr);

    std::vector<std::unique_ptr<Curve>> ints = cp->releaseInteriorRings();
    ASSERT_EQ(ints.size(), 2u);
    EXPECT_EQ(ints[0]->getGeometryTypeId(), geos::geom::GEOS_CIRCULARSTRING);
    EXPECT_EQ(ints[1]->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
    EXPECT_TRUE(ints[0]->equalsIdentical(expected_hole0.get()));
    EXPECT_TRUE(ints[1]->equalsIdentical(expected_hole1.get()));
    EXPECT_THAT(cp->releaseInteriorRings(), IsEmpty());
  }

  // Release interior first, then exterior.
  {
    std::vector<std::unique_ptr<Curve>> holes;
    holes.push_back(expected_hole0->clone());
    auto cp =
        factory_->createCurvePolygon(expected_shell->clone(), std::move(holes));

    auto ints = cp->releaseInteriorRings();
    ASSERT_EQ(ints.size(), 1u);
    EXPECT_TRUE(ints[0]->equalsIdentical(expected_hole0.get()));

    auto ext = cp->releaseExteriorRing();
    ASSERT_NE(ext, nullptr);
    EXPECT_TRUE(ext->equalsIdentical(expected_shell.get()));
  }

  // Empty CurvePolygon release.
  {
    auto empty_cp = factory_->createCurvePolygon(false, false);
    auto ext = empty_cp->releaseExteriorRing();
    ASSERT_NE(ext, nullptr);
    EXPECT_TRUE(ext->isEmpty());
    EXPECT_THAT(empty_cp->releaseInteriorRings(), IsEmpty());
  }
}

TEST_F(SurfaceImplTest, PreservesXYZMAndXYMCoordinatesAcrossCloneAndRelease) {
  auto shell_seq = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXYZM>{
          {0.0, 0.0, 10.0, 100.0},
          {10.0, 0.0, 11.0, 101.0},
          {10.0, 10.0, 12.0, 102.0},
          {0.0, 10.0, 13.0, 103.0},
          {0.0, 0.0, 10.0, 100.0},
      });
  auto hole_seq = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXYZM>{
          {2.0, 2.0, 20.0, 200.0},
          {4.0, 2.0, 21.0, 201.0},
          {4.0, 4.0, 22.0, 202.0},
          {2.0, 4.0, 23.0, 203.0},
          {2.0, 2.0, 20.0, 200.0},
      });
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(factory_->createLinearRing(std::move(hole_seq)));

  auto poly = factory_->createPolygon(
      factory_->createLinearRing(std::move(shell_seq)), std::move(holes));
  EXPECT_TRUE(poly->hasZ());
  EXPECT_TRUE(poly->hasM());
  EXPECT_EQ(poly->getCoordinateDimension(), 4u);

  auto cloned = poly->clone();
  EXPECT_TRUE(cloned->hasZ());
  EXPECT_TRUE(cloned->hasM());
  EXPECT_EQ(cloned->getCoordinateDimension(), 4u);
  EXPECT_TRUE(cloned->equalsIdentical(poly.get()));

  auto released_shell = cloned->releaseExteriorRing();
  auto released_holes = cloned->releaseInteriorRings();
  ASSERT_EQ(released_holes.size(), 1u);

  CoordinateXYZM pt;
  released_shell->getCoordinatesRO()->getAt(2, pt);
  EXPECT_DOUBLE_EQ(pt.z, 12.0);
  EXPECT_DOUBLE_EQ(pt.m, 102.0);

  released_holes[0]->getCoordinatesRO()->getAt(1, pt);
  EXPECT_DOUBLE_EQ(pt.z, 21.0);
  EXPECT_DOUBLE_EQ(pt.m, 201.0);

  // XYM CurvePolygon verifies Z remains NaN and M is preserved.
  auto xym_shell_seq =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXYM>{
          {0.0, 0.0, 1.5},
          {5.0, 5.0, 2.5},
          {10.0, 0.0, 3.5},
          {5.0, -5.0, 4.5},
          {0.0, 0.0, 1.5},
      });
  auto xym_cp = factory_->createCurvePolygon(
      factory_->createCircularString(std::move(xym_shell_seq)));
  EXPECT_FALSE(xym_cp->hasZ());
  EXPECT_TRUE(xym_cp->hasM());
  EXPECT_EQ(xym_cp->getCoordinateDimension(), 3u);

  auto released_xym_curve = xym_cp->releaseExteriorRing();
  const auto* simple_xym =
      dynamic_cast<const SimpleCurve*>(released_xym_curve.get());
  ASSERT_NE(simple_xym, nullptr);
  const auto* xym_cs = simple_xym->getCoordinatesRO();
  CoordinateXYM xym_pt;
  xym_cs->getAt(1, xym_pt);
  EXPECT_DOUBLE_EQ(xym_pt.m, 2.5);
  EXPECT_DOUBLE_EQ(xym_cs->getOrdinate(1, CoordinateSequence::M), 2.5);
  EXPECT_TRUE(std::isnan(xym_cs->getOrdinate(1, CoordinateSequence::Z)));
}

}  // namespace
}  // namespace geos_google
