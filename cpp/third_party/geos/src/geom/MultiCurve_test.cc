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

// Unit tests for third_party/geos/src/geom/MultiCurve.cpp and
// third_party/geos/include/geos/geom/MultiCurve.h.

#include "third_party/geos/include/geos/geom/MultiCurve.h"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <numbers>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/CompoundCurve.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Curve.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/MultiSurface.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/geom/SimpleCurve.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::CircularString;
using geos::geom::CompoundCurve;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYZM;
using geos::geom::Curve;
using geos::geom::Dimension;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GEOS_MULTICURVE;
using geos::geom::LineString;
using geos::geom::MultiCurve;
using geos::geom::MultiLineString;
using geos::geom::MultiSurface;
using geos::geom::PrecisionModel;
using geos::geom::SimpleCurve;
using geos::util::IllegalArgumentException;
using geos::util::UnsupportedOperationException;

std::unique_ptr<LineString> MakeLineString(
    const GeometryFactory& factory,
    std::initializer_list<CoordinateXY> coords) {
  return factory.createLineString(std::make_unique<CoordinateSequence>(coords));
}

std::unique_ptr<CircularString> MakeCircularString(
    const GeometryFactory& factory,
    std::initializer_list<CoordinateXY> coords) {
  return factory.createCircularString(
      std::make_unique<CoordinateSequence>(coords));
}

std::unique_ptr<CompoundCurve> MakeOpenCompoundCurve(
    const GeometryFactory& factory) {
  std::vector<std::unique_ptr<SimpleCurve>> sections;
  sections.push_back(MakeLineString(
      factory, {CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 0.0)}));
  sections.push_back(MakeCircularString(
      factory, {CoordinateXY(2.0, 0.0), CoordinateXY(3.0, 1.0),
                CoordinateXY(4.0, 0.0)}));
  return factory.createCompoundCurve(std::move(sections));
}

std::unique_ptr<CompoundCurve> MakeClosedCompoundCurve(
    const GeometryFactory& factory) {
  std::vector<std::unique_ptr<SimpleCurve>> sections;
  sections.push_back(MakeLineString(
      factory, {CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 0.0)}));
  sections.push_back(MakeCircularString(
      factory, {CoordinateXY(2.0, 0.0), CoordinateXY(1.0, 1.0),
                CoordinateXY(0.0, 0.0)}));
  return factory.createCompoundCurve(std::move(sections));
}

TEST(MultiCurveTest, EmptyCreationAndBasicMetadata) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::unique_ptr<MultiCurve> empty_default = factory->createMultiCurve();
  ASSERT_NE(empty_default, nullptr);
  EXPECT_TRUE(empty_default->isEmpty());
  EXPECT_EQ(empty_default->getNumGeometries(), 0u);
  EXPECT_EQ(empty_default->getNumPoints(), 0u);
  EXPECT_EQ(empty_default->getCoordinate(), nullptr);
  EXPECT_EQ(empty_default->getGeometryType(), "MultiCurve");
  EXPECT_EQ(empty_default->getGeometryTypeId(), GEOS_MULTICURVE);
  EXPECT_EQ(empty_default->getDimension(), Dimension::L);
  EXPECT_TRUE(empty_default->hasDimension(Dimension::L));
  EXPECT_FALSE(empty_default->hasDimension(Dimension::P));
  EXPECT_FALSE(empty_default->hasDimension(Dimension::A));
  EXPECT_FALSE(empty_default->hasDimension(Dimension::False));
  EXPECT_TRUE(empty_default->isDimensionStrict(Dimension::L));
  EXPECT_FALSE(empty_default->isDimensionStrict(Dimension::P));
  EXPECT_FALSE(empty_default->isDimensionStrict(Dimension::A));
  EXPECT_FALSE(empty_default->isDimensionStrict(Dimension::False));
  EXPECT_TRUE(empty_default->isLineal());
  EXPECT_FALSE(empty_default->isPuntal());
  EXPECT_FALSE(empty_default->isPolygonal());
  EXPECT_FALSE(empty_default->isClosed());
  EXPECT_EQ(empty_default->getBoundaryDimension(), 0);
  const Geometry* empty_default_geom = empty_default.get();
  EXPECT_FALSE(empty_default_geom->hasCurvedComponents());
  EXPECT_TRUE(empty_default_geom->hasDimension(Dimension::L));
  EXPECT_TRUE(empty_default_geom->isDimensionStrict(Dimension::L));
  EXPECT_EQ(empty_default_geom->getDimension(), Dimension::L);
  EXPECT_EQ(empty_default_geom->getBoundaryDimension(), 0);
  EXPECT_EQ(empty_default_geom->getGeometryType(), "MultiCurve");
  EXPECT_EQ(empty_default_geom->getGeometryTypeId(), GEOS_MULTICURVE);
  EXPECT_DOUBLE_EQ(empty_default->getLength(), 0.0);
  EXPECT_DOUBLE_EQ(empty_default->getArea(), 0.0);
  EXPECT_TRUE(empty_default->getEnvelopeInternal()->isNull());

  // Empty vector passed to the Geometry overload of createMultiCurve.
  std::unique_ptr<MultiCurve> empty_from_geoms =
      factory->createMultiCurve(std::vector<std::unique_ptr<Geometry>>{});
  EXPECT_TRUE(empty_from_geoms->isEmpty());
  EXPECT_EQ(empty_from_geoms->getGeometryTypeId(), GEOS_MULTICURVE);
  EXPECT_TRUE(empty_default->equalsExact(empty_from_geoms.get()));

  // createEmptyGeometry(GEOS_MULTICURVE) produces an empty MultiCurve.
  std::unique_ptr<Geometry> empty_generic =
      factory->createEmptyGeometry(GEOS_MULTICURVE);
  EXPECT_EQ(empty_generic->getGeometryTypeId(), GEOS_MULTICURVE);
  EXPECT_EQ(empty_generic->getGeometryType(), "MultiCurve");
  EXPECT_TRUE(empty_generic->isEmpty());
}

TEST(MultiCurveTest, ConstructFromCurveVectorAndAccessors) {
  PrecisionModel pm;
  GeometryFactory::Ptr factory = GeometryFactory::create(&pm, 3857);

  std::vector<std::unique_ptr<Curve>> curves;
  curves.push_back(MakeLineString(
      *factory, {CoordinateXY(0.0, 0.0), CoordinateXY(3.0, 4.0)}));
  curves.push_back(MakeCircularString(
      *factory, {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0),
                 CoordinateXY(2.0, 0.0)}));
  curves.push_back(MakeOpenCompoundCurve(*factory));

  std::unique_ptr<MultiCurve> mc = factory->createMultiCurve(std::move(curves));
  ASSERT_NE(mc, nullptr);
  EXPECT_FALSE(mc->isEmpty());
  EXPECT_EQ(mc->getSRID(), 3857);
  EXPECT_EQ(mc->getNumGeometries(), 3u);
  const Geometry* geom = mc.get();
  EXPECT_TRUE(geom->hasCurvedComponents());

  static_assert(std::is_same_v<decltype(mc->getGeometryN(0)), const Curve*>);
  const Curve* c0 = mc->getGeometryN(0);
  const Curve* c1 = mc->getGeometryN(1);
  const Curve* c2 = mc->getGeometryN(2);
  ASSERT_NE(c0, nullptr);
  ASSERT_NE(c1, nullptr);
  ASSERT_NE(c2, nullptr);
  EXPECT_EQ(c0->getGeometryType(), "LineString");
  EXPECT_EQ(c1->getGeometryType(), "CircularString");
  EXPECT_EQ(c2->getGeometryType(), "CompoundCurve");
  EXPECT_EQ(c0->getSRID(), 3857);
  EXPECT_EQ(c1->getSRID(), 3857);
  EXPECT_EQ(c2->getSRID(), 3857);

  // Polymorphic Geometry* access returns the same subgeometries.
  EXPECT_EQ(geom->getGeometryN(0), c0);
  EXPECT_EQ(geom->getGeometryN(1), c1);
  EXPECT_EQ(geom->getGeometryN(2), c2);

  // Total length: 5 + pi + (2 + pi) = 7 + 2*pi.
  EXPECT_NEAR(mc->getLength(), 7.0 + 2.0 * std::numbers::pi, 1e-12);
  EXPECT_DOUBLE_EQ(mc->getArea(), 0.0);
  EXPECT_EQ(*mc->getEnvelopeInternal(), Envelope(0.0, 4.0, 0.0, 4.0));
}

TEST(MultiCurveTest, ConstructFromGeometryVectorValidAndInvalid) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Valid Curve subclasses in a std::vector<std::unique_ptr<Geometry>>:
  // LineString, LinearRing, CircularString, CompoundCurve.
  std::vector<std::unique_ptr<Geometry>> valid_geoms;
  valid_geoms.push_back(MakeLineString(
      *factory, {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0)}));
  valid_geoms.push_back(factory->createLinearRing(
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0),
          CoordinateXY(1.0, 1.0), CoordinateXY(0.0, 0.0)})));
  valid_geoms.push_back(MakeCircularString(
      *factory, {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0),
                 CoordinateXY(2.0, 0.0)}));
  valid_geoms.push_back(MakeClosedCompoundCurve(*factory));

  std::unique_ptr<MultiCurve> mc =
      factory->createMultiCurve(std::move(valid_geoms));
  EXPECT_EQ(mc->getNumGeometries(), 4u);
  EXPECT_FALSE(mc->hasDimension(Dimension::P));
  EXPECT_TRUE(mc->hasDimension(Dimension::L));
  EXPECT_TRUE(static_cast<const Geometry*>(mc.get())->hasCurvedComponents());

  // Non-Curve elements must throw IllegalArgumentException.
  {
    std::vector<std::unique_ptr<Geometry>> with_point;
    with_point.push_back(MakeLineString(
        *factory, {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0)}));
    with_point.push_back(factory->createPoint(CoordinateXY(2.0, 3.0)));
    EXPECT_THROW(factory->createMultiCurve(std::move(with_point)),
                 IllegalArgumentException);
  }
  {
    std::vector<std::unique_ptr<Geometry>> with_polygon;
    with_polygon.push_back(factory->createPolygon(2));
    EXPECT_THROW(factory->createMultiCurve(std::move(with_polygon)),
                 IllegalArgumentException);
  }
  {
    std::vector<std::unique_ptr<Geometry>> with_multilinestring;
    with_multilinestring.push_back(factory->createMultiLineString());
    EXPECT_THROW(factory->createMultiCurve(std::move(with_multilinestring)),
                 IllegalArgumentException);
  }
}

TEST(MultiCurveTest, IsClosedAndGetBoundaryDimension) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Empty MultiCurve is not closed -> boundary dimension 0.
  std::unique_ptr<MultiCurve> empty_mc = factory->createMultiCurve();
  EXPECT_FALSE(empty_mc->isClosed());
  EXPECT_EQ(empty_mc->getBoundaryDimension(), 0);

  // MultiCurve containing only empty curves is empty -> not closed.
  {
    std::vector<std::unique_ptr<Curve>> only_empty;
    only_empty.push_back(factory->createLineString());
    only_empty.push_back(factory->createCircularString(false, false));
    std::unique_ptr<MultiCurve> mc =
        factory->createMultiCurve(std::move(only_empty));
    EXPECT_TRUE(mc->isEmpty());
    EXPECT_FALSE(mc->isClosed());
    EXPECT_EQ(mc->getBoundaryDimension(), 0);
  }

  // All component curves closed -> isClosed() is true, boundary dimension
  // False.
  {
    std::vector<std::unique_ptr<Curve>> closed_curves;
    closed_curves.push_back(MakeLineString(
        *factory, {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0),
                   CoordinateXY(0.0, 1.0), CoordinateXY(0.0, 0.0)}));
    closed_curves.push_back(MakeCircularString(
        *factory,
        {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0), CoordinateXY(2.0, 0.0),
         CoordinateXY(1.0, -1.0), CoordinateXY(0.0, 0.0)}));
    closed_curves.push_back(MakeClosedCompoundCurve(*factory));
    std::unique_ptr<MultiCurve> mc =
        factory->createMultiCurve(std::move(closed_curves));
    EXPECT_TRUE(mc->isClosed());
    EXPECT_EQ(mc->getBoundaryDimension(), Dimension::False);
  }

  // First closed, second open -> isClosed() is false, boundary dimension 0.
  {
    std::vector<std::unique_ptr<Curve>> mixed;
    mixed.push_back(MakeClosedCompoundCurve(*factory));
    mixed.push_back(MakeLineString(
        *factory, {CoordinateXY(0.0, 0.0), CoordinateXY(5.0, 0.0)}));
    std::unique_ptr<MultiCurve> mc =
        factory->createMultiCurve(std::move(mixed));
    EXPECT_FALSE(mc->isClosed());
    EXPECT_EQ(mc->getBoundaryDimension(), 0);
  }

  // First open, second closed -> isClosed() is false, boundary dimension 0.
  {
    std::vector<std::unique_ptr<Curve>> mixed;
    mixed.push_back(MakeOpenCompoundCurve(*factory));
    mixed.push_back(MakeCircularString(
        *factory,
        {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0), CoordinateXY(2.0, 0.0),
         CoordinateXY(1.0, -1.0), CoordinateXY(0.0, 0.0)}));
    std::unique_ptr<MultiCurve> mc =
        factory->createMultiCurve(std::move(mixed));
    EXPECT_FALSE(mc->isClosed());
    EXPECT_EQ(mc->getBoundaryDimension(), 0);
  }

  // Closed curve plus an empty curve -> MultiCurve is non-empty, not closed.
  {
    std::vector<std::unique_ptr<Curve>> closed_and_empty;
    closed_and_empty.push_back(MakeClosedCompoundCurve(*factory));
    closed_and_empty.push_back(factory->createLineString());
    std::unique_ptr<MultiCurve> mc =
        factory->createMultiCurve(std::move(closed_and_empty));
    EXPECT_FALSE(mc->isEmpty());
    EXPECT_FALSE(mc->isClosed());
    EXPECT_EQ(mc->getBoundaryDimension(), 0);
  }
}

TEST(MultiCurveTest, GetBoundaryThrowsForCurvedComponents) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // MultiCurve::getBoundary() delegates to BoundaryOp::getBoundary(), which
  // rejects curved geometries via ensureNoCurvedComponents() before
  // dispatching. (Calling getBoundary() on a non-curved or empty MultiCurve is
  // avoided because BoundaryOp::getBoundary() falls through to
  // m_geom.getBoundary().)
  std::vector<std::unique_ptr<Curve>> with_arc;
  with_arc.push_back(MakeCircularString(
      *factory, {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0),
                 CoordinateXY(2.0, 0.0)}));
  std::unique_ptr<MultiCurve> mc_arc =
      factory->createMultiCurve(std::move(with_arc));
  const Geometry* mc_arc_geom = mc_arc.get();
  EXPECT_TRUE(mc_arc_geom->hasCurvedComponents());
  EXPECT_THROW(mc_arc->getBoundary(), UnsupportedOperationException);
  EXPECT_THROW(mc_arc_geom->getBoundary(), UnsupportedOperationException);

  std::vector<std::unique_ptr<Curve>> with_compound;
  with_compound.push_back(MakeOpenCompoundCurve(*factory));
  std::unique_ptr<MultiCurve> mc_compound =
      factory->createMultiCurve(std::move(with_compound));
  EXPECT_TRUE(
      static_cast<const Geometry*>(mc_compound.get())->hasCurvedComponents());
  EXPECT_THROW(mc_compound->getBoundary(), UnsupportedOperationException);
}

TEST(MultiCurveTest, ReverseEmptyAndNonEmptyPreservesFactoryAndReversesCurves) {
  PrecisionModel pm;
  GeometryFactory::Ptr factory = GeometryFactory::create(&pm, 4326);

  // Reversing an empty MultiCurve clones it and preserves factory & SRID.
  std::unique_ptr<MultiCurve> empty_mc = factory->createMultiCurve();
  std::unique_ptr<MultiCurve> rev_empty = empty_mc->reverse();
  ASSERT_NE(rev_empty, nullptr);
  EXPECT_NE(rev_empty.get(), empty_mc.get());
  EXPECT_TRUE(rev_empty->isEmpty());
  EXPECT_EQ(rev_empty->getGeometryTypeId(), GEOS_MULTICURVE);
  EXPECT_EQ(rev_empty->getFactory(), factory.get());
  EXPECT_EQ(rev_empty->getSRID(), 4326);

  // MultiCurve containing only empty curves is empty -> reverse clones it.
  {
    std::vector<std::unique_ptr<Curve>> empty_children;
    empty_children.push_back(factory->createLineString());
    empty_children.push_back(factory->createCompoundCurve());
    std::unique_ptr<MultiCurve> mc_empty_children =
        factory->createMultiCurve(std::move(empty_children));
    std::unique_ptr<MultiCurve> rev_children = mc_empty_children->reverse();
    EXPECT_TRUE(rev_children->isEmpty());
    EXPECT_EQ(rev_children->getNumGeometries(), 2u);
    EXPECT_EQ(rev_children->getSRID(), 4326);
  }

  // Reversing a non-empty MultiCurve reverses each component curve in place.
  std::vector<std::unique_ptr<Curve>> curves;
  curves.push_back(
      MakeLineString(*factory, {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 2.0),
                                CoordinateXY(3.0, 4.0)}));
  curves.push_back(MakeCircularString(
      *factory, {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0),
                 CoordinateXY(2.0, 0.0)}));
  curves.push_back(MakeOpenCompoundCurve(*factory));

  std::unique_ptr<MultiCurve> mc = factory->createMultiCurve(std::move(curves));
  std::unique_ptr<MultiCurve> reversed = mc->reverse();
  ASSERT_NE(reversed, nullptr);
  EXPECT_EQ(reversed->getFactory(), factory.get());
  EXPECT_EQ(reversed->getSRID(), 4326);
  EXPECT_EQ(reversed->getGeometryTypeId(), GEOS_MULTICURVE);
  ASSERT_EQ(reversed->getNumGeometries(), 3u);

  for (std::size_t i = 0; i < mc->getNumGeometries(); ++i) {
    std::unique_ptr<Geometry> expected_component =
        mc->getGeometryN(i)->reverse();
    EXPECT_TRUE(
        reversed->getGeometryN(i)->equalsExact(expected_component.get()))
        << "Mismatch at component " << i;
  }

  // CompoundCurve reverses both section order and coordinate order.
  EXPECT_EQ(reversed->getGeometryN(2)->getNumCurves(), 2u);
  EXPECT_EQ(reversed->getGeometryN(2)->getCurveN(0)->getGeometryType(),
            "CircularString");
  EXPECT_EQ(reversed->getGeometryN(2)->getCurveN(1)->getGeometryType(),
            "LineString");

  // Reversing via base Geometry* calls MultiCurve::reverseImpl().
  const Geometry* base_geom = mc.get();
  std::unique_ptr<Geometry> base_reversed = base_geom->reverse();
  EXPECT_EQ(base_reversed->getGeometryTypeId(), GEOS_MULTICURVE);
  EXPECT_TRUE(base_reversed->equalsExact(reversed.get()));

  // Double reverse restores the original MultiCurve.
  std::unique_ptr<MultiCurve> double_reversed = reversed->reverse();
  EXPECT_TRUE(double_reversed->equalsExact(mc.get()));
}

TEST(MultiCurveTest, CloneEqualityDimensionFlagsAndSortOrder) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Linear-only MultiCurve has hasCurvedComponents() == false.
  std::vector<std::unique_ptr<Curve>> linear_only;
  linear_only.push_back(
      factory->createLineString(std::make_unique<CoordinateSequence>(
          std::initializer_list<CoordinateXYZM>{
              CoordinateXYZM(0.0, 0.0, 10.0, 1.0),
              CoordinateXYZM(2.0, 0.0, 20.0, 2.0)})));
  std::unique_ptr<MultiCurve> mc_linear =
      factory->createMultiCurve(std::move(linear_only));
  EXPECT_FALSE(
      static_cast<const Geometry*>(mc_linear.get())->hasCurvedComponents());
  EXPECT_TRUE(mc_linear->hasZ());
  EXPECT_TRUE(mc_linear->hasM());
  EXPECT_EQ(mc_linear->getCoordinateDimension(), 4u);
  EXPECT_EQ(mc_linear->getNumPoints(), 2u);

  // Clone creates an independent deep copy.
  std::unique_ptr<MultiCurve> cloned = mc_linear->clone();
  ASSERT_NE(cloned, nullptr);
  EXPECT_NE(cloned.get(), mc_linear.get());
  EXPECT_NE(cloned->getGeometryN(0), mc_linear->getGeometryN(0));
  EXPECT_TRUE(cloned->equalsExact(mc_linear.get()));
  EXPECT_TRUE(cloned->equalsIdentical(mc_linear.get()));

  // Sort index ordering: MultiLineString (4) < MultiCurve (11) < MultiSurface
  // (12).
  std::unique_ptr<MultiLineString> mls = factory->createMultiLineString();
  std::unique_ptr<MultiSurface> ms = factory->createMultiSurface();
  EXPECT_GT(mc_linear->compareTo(mls.get()), 0);
  EXPECT_LT(mls->compareTo(mc_linear.get()), 0);
  EXPECT_LT(mc_linear->compareTo(ms.get()), 0);
  EXPECT_GT(ms->compareTo(mc_linear.get()), 0);
  EXPECT_EQ(mc_linear->compareTo(cloned.get()), 0);
}

}  // namespace
}  // namespace geos_google
