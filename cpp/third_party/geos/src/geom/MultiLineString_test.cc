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

// Unit tests for third_party/geos/src/geom/MultiLineString.cpp and
// third_party/geos/include/geos/geom/MultiLineString.h.

#include "third_party/geos/include/geos/geom/MultiLineString.h"

#include <initializer_list>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiCurve.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Dimension;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GEOS_MULTILINESTRING;
using geos::geom::GEOS_MULTIPOINT;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::MultiCurve;
using geos::geom::MultiLineString;
using geos::geom::MultiPoint;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::util::IllegalArgumentException;

std::unique_ptr<LineString> MakeLineString(
    const GeometryFactory& factory,
    std::initializer_list<CoordinateXY> coords) {
  return factory.createLineString(std::make_unique<CoordinateSequence>(coords));
}

TEST(MultiLineStringTest, EmptyCreationAndBasicMetadata) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Default createMultiLineString() uses vector<unique_ptr<Geometry>> ctor.
  std::unique_ptr<MultiLineString> empty_geom =
      factory->createMultiLineString();
  ASSERT_NE(empty_geom, nullptr);
  EXPECT_TRUE(empty_geom->isEmpty());
  EXPECT_EQ(empty_geom->getNumGeometries(), 0u);
  EXPECT_EQ(empty_geom->getNumPoints(), 0u);
  EXPECT_EQ(empty_geom->getCoordinate(), nullptr);
  EXPECT_EQ(empty_geom->getGeometryType(), "MultiLineString");
  EXPECT_EQ(empty_geom->getGeometryTypeId(), GEOS_MULTILINESTRING);
  EXPECT_EQ(empty_geom->getDimension(), Dimension::L);
  EXPECT_TRUE(empty_geom->hasDimension(Dimension::L));
  EXPECT_FALSE(empty_geom->hasDimension(Dimension::P));
  EXPECT_FALSE(empty_geom->hasDimension(Dimension::A));
  EXPECT_FALSE(empty_geom->hasDimension(Dimension::False));
  EXPECT_TRUE(empty_geom->isDimensionStrict(Dimension::L));
  EXPECT_FALSE(empty_geom->isDimensionStrict(Dimension::P));
  EXPECT_FALSE(empty_geom->isDimensionStrict(Dimension::A));
  EXPECT_FALSE(empty_geom->isDimensionStrict(Dimension::False));
  EXPECT_TRUE(empty_geom->isLineal());
  EXPECT_FALSE(empty_geom->isPuntal());
  EXPECT_FALSE(empty_geom->isPolygonal());
  EXPECT_TRUE(empty_geom->isCollection());
  EXPECT_FALSE(empty_geom->isClosed());
  EXPECT_EQ(empty_geom->getBoundaryDimension(), 0);
  EXPECT_DOUBLE_EQ(empty_geom->getLength(), 0.0);
  EXPECT_DOUBLE_EQ(empty_geom->getArea(), 0.0);
  EXPECT_TRUE(empty_geom->getEnvelopeInternal()->isNull());

  // Polymorphic calls on Geometry* (hasCurvedComponents is protected on
  // MultiLineString).
  const Geometry* geom_ptr = empty_geom.get();
  EXPECT_FALSE(geom_ptr->hasCurvedComponents());
  EXPECT_EQ(geom_ptr->getDimension(), Dimension::L);
  EXPECT_EQ(geom_ptr->getGeometryType(), "MultiLineString");
  EXPECT_EQ(geom_ptr->getGeometryTypeId(), GEOS_MULTILINESTRING);

  // vector<unique_ptr<LineString>> constructor with empty vector.
  std::unique_ptr<MultiLineString> empty_ls = factory->createMultiLineString(
      std::vector<std::unique_ptr<LineString>>{});
  EXPECT_TRUE(empty_ls->isEmpty());
  EXPECT_EQ(empty_ls->getGeometryTypeId(), GEOS_MULTILINESTRING);
  EXPECT_TRUE(empty_geom->equalsExact(empty_ls.get()));
}

TEST(MultiLineStringTest, ConstructorsAndGetGeometryN) {
  PrecisionModel pm;
  GeometryFactory::Ptr factory = GeometryFactory::create(&pm, 32610);

  // Construct from vector<unique_ptr<LineString>>.
  std::vector<std::unique_ptr<LineString>> lines;
  lines.push_back(MakeLineString(
      *factory, {CoordinateXY(0.0, 0.0), CoordinateXY(3.0, 4.0)}));
  lines.push_back(MakeLineString(
      *factory, {CoordinateXY(10.0, 0.0), CoordinateXY(10.0, 2.0)}));
  std::unique_ptr<MultiLineString> mls =
      factory->createMultiLineString(std::move(lines));

  ASSERT_NE(mls, nullptr);
  EXPECT_FALSE(mls->isEmpty());
  EXPECT_EQ(mls->getSRID(), 32610);
  EXPECT_EQ(mls->getNumGeometries(), 2u);
  EXPECT_EQ(mls->getNumPoints(), 4u);
  EXPECT_DOUBLE_EQ(mls->getLength(), 7.0);
  EXPECT_DOUBLE_EQ(mls->getArea(), 0.0);
  EXPECT_EQ(*mls->getEnvelopeInternal(), Envelope(0.0, 10.0, 0.0, 4.0));
  EXPECT_EQ(mls->getDimension(), Dimension::L);
  EXPECT_EQ(mls->getGeometryType(), "MultiLineString");
  EXPECT_EQ(mls->getGeometryTypeId(), GEOS_MULTILINESTRING);
  EXPECT_TRUE(mls->hasDimension(Dimension::L));
  EXPECT_FALSE(mls->hasDimension(Dimension::P));
  EXPECT_TRUE(mls->isDimensionStrict(Dimension::L));
  EXPECT_FALSE(mls->isDimensionStrict(Dimension::A));

  static_assert(
      std::is_same_v<decltype(mls->getGeometryN(0)), const LineString*>);
  const LineString* ls0 = mls->getGeometryN(0);
  const LineString* ls1 = mls->getGeometryN(1);
  ASSERT_NE(ls0, nullptr);
  ASSERT_NE(ls1, nullptr);
  EXPECT_EQ(ls0->getNumPoints(), 2u);
  EXPECT_EQ(ls1->getNumPoints(), 2u);
  EXPECT_DOUBLE_EQ(ls0->getLength(), 5.0);
  EXPECT_DOUBLE_EQ(ls1->getLength(), 2.0);
  EXPECT_EQ(ls0->getSRID(), 32610);
  EXPECT_EQ(ls1->getSRID(), 32610);

  // Polymorphic Geometry* access returns the same subgeometries and checks
  // curved components.
  const Geometry* base = mls.get();
  EXPECT_EQ(base->getGeometryN(0), ls0);
  EXPECT_EQ(base->getGeometryN(1), ls1);
  EXPECT_FALSE(base->hasCurvedComponents());
  EXPECT_EQ(base->getBoundaryDimension(), 0);

  // Construct from vector<unique_ptr<Geometry>> overload with valid
  // LineStrings.
  std::vector<std::unique_ptr<Geometry>> geom_lines;
  geom_lines.push_back(MakeLineString(
      *factory, {CoordinateXY(0.0, 0.0), CoordinateXY(3.0, 4.0)}));
  geom_lines.push_back(MakeLineString(
      *factory, {CoordinateXY(10.0, 0.0), CoordinateXY(10.0, 2.0)}));
  std::unique_ptr<MultiLineString> from_geom_vec =
      factory->createMultiLineString(std::move(geom_lines));
  ASSERT_NE(from_geom_vec, nullptr);
  EXPECT_TRUE(from_geom_vec->equalsExact(mls.get()));
  EXPECT_EQ(from_geom_vec->getSRID(), 32610);

  // Construct from vector<const Geometry*> deep-copy overload.
  std::vector<const Geometry*> raw_ptrs = {ls0, ls1};
  std::unique_ptr<MultiLineString> copied =
      factory->createMultiLineString(raw_ptrs);
  EXPECT_TRUE(copied->equalsExact(mls.get()));
  EXPECT_NE(copied->getGeometryN(0), ls0);

  // Passing a non-LineString or nullptr in vector<const Geometry*> throws.
  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(1.0, 1.0));
  std::vector<const Geometry*> bad_ptrs = {ls0, pt.get()};
  EXPECT_THROW(factory->createMultiLineString(bad_ptrs),
               IllegalArgumentException);
  std::vector<const Geometry*> raw_with_null = {ls0, nullptr};
  EXPECT_THROW(factory->createMultiLineString(raw_with_null),
               IllegalArgumentException);
}

TEST(MultiLineStringTest, IsClosedAndGetBoundaryDimension) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Empty MultiLineString is not closed -> boundary dimension 0.
  std::unique_ptr<MultiLineString> empty_mls = factory->createMultiLineString();
  EXPECT_FALSE(empty_mls->isClosed());
  EXPECT_EQ(empty_mls->getBoundaryDimension(), 0);

  // Collection of only empty LineStrings -> isEmpty() is true -> not closed.
  {
    std::vector<std::unique_ptr<LineString>> empty_lines;
    empty_lines.push_back(factory->createLineString());
    empty_lines.push_back(factory->createLineString());
    std::unique_ptr<MultiLineString> mls =
        factory->createMultiLineString(std::move(empty_lines));
    EXPECT_TRUE(mls->isEmpty());
    EXPECT_FALSE(mls->isClosed());
    EXPECT_EQ(mls->getBoundaryDimension(), 0);
  }

  // All non-empty LineStrings closed (including LinearRing) -> isClosed() true.
  {
    std::vector<std::unique_ptr<LineString>> closed_lines;
    closed_lines.push_back(MakeLineString(
        *factory, {CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 0.0),
                   CoordinateXY(2.0, 2.0), CoordinateXY(0.0, 0.0)}));
    closed_lines.push_back(
        factory->createLinearRing(std::make_unique<CoordinateSequence>(
            std::initializer_list<CoordinateXY>{
                CoordinateXY(5.0, 5.0), CoordinateXY(6.0, 5.0),
                CoordinateXY(6.0, 6.0), CoordinateXY(5.0, 5.0)})));
    std::unique_ptr<MultiLineString> mls =
        factory->createMultiLineString(std::move(closed_lines));
    EXPECT_TRUE(mls->isClosed());
    EXPECT_EQ(mls->getBoundaryDimension(), Dimension::False);
  }

  // Closed followed by open -> isClosed() false, boundary dimension 0.
  {
    std::vector<std::unique_ptr<LineString>> mixed;
    mixed.push_back(MakeLineString(
        *factory, {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0),
                   CoordinateXY(0.0, 1.0), CoordinateXY(0.0, 0.0)}));
    mixed.push_back(MakeLineString(
        *factory, {CoordinateXY(2.0, 0.0), CoordinateXY(3.0, 0.0)}));
    std::unique_ptr<MultiLineString> mls =
        factory->createMultiLineString(std::move(mixed));
    EXPECT_FALSE(mls->isClosed());
    EXPECT_EQ(mls->getBoundaryDimension(), 0);
  }

  // Open followed by closed -> isClosed() false, boundary dimension 0.
  {
    std::vector<std::unique_ptr<LineString>> mixed;
    mixed.push_back(MakeLineString(
        *factory, {CoordinateXY(2.0, 0.0), CoordinateXY(3.0, 0.0)}));
    mixed.push_back(MakeLineString(
        *factory, {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0),
                   CoordinateXY(0.0, 1.0), CoordinateXY(0.0, 0.0)}));
    std::unique_ptr<MultiLineString> mls =
        factory->createMultiLineString(std::move(mixed));
    EXPECT_FALSE(mls->isClosed());
    EXPECT_EQ(mls->getBoundaryDimension(), 0);
  }

  // Closed line plus an empty line -> non-empty, not closed.
  {
    std::vector<std::unique_ptr<LineString>> closed_and_empty;
    closed_and_empty.push_back(MakeLineString(
        *factory, {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0),
                   CoordinateXY(0.0, 1.0), CoordinateXY(0.0, 0.0)}));
    closed_and_empty.push_back(factory->createLineString());
    std::unique_ptr<MultiLineString> mls =
        factory->createMultiLineString(std::move(closed_and_empty));
    EXPECT_FALSE(mls->isEmpty());
    EXPECT_FALSE(mls->isClosed());
    EXPECT_EQ(mls->getBoundaryDimension(), 0);
  }
}

TEST(MultiLineStringTest, GetBoundaryAppliesMod2BoundaryNodeRule) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Empty MultiLineString -> empty MultiPoint.
  {
    std::unique_ptr<MultiLineString> empty_mls =
        factory->createMultiLineString();
    std::unique_ptr<Geometry> boundary = empty_mls->getBoundary();
    ASSERT_NE(boundary, nullptr);
    EXPECT_EQ(boundary->getGeometryTypeId(), GEOS_MULTIPOINT);
    EXPECT_TRUE(boundary->isEmpty());
  }

  // Closed MultiLineString -> empty MultiPoint.
  {
    std::vector<std::unique_ptr<LineString>> closed;
    closed.push_back(MakeLineString(
        *factory, {CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 0.0),
                   CoordinateXY(1.0, 2.0), CoordinateXY(0.0, 0.0)}));
    std::unique_ptr<MultiLineString> mls =
        factory->createMultiLineString(std::move(closed));
    std::unique_ptr<Geometry> boundary = mls->getBoundary();
    EXPECT_EQ(boundary->getGeometryTypeId(), GEOS_MULTIPOINT);
    EXPECT_TRUE(boundary->isEmpty());
  }

  // Two open lines meeting at (1, 0): valence 2 at (1, 0) cancels under mod-2.
  {
    std::vector<std::unique_ptr<LineString>> chain;
    chain.push_back(MakeLineString(
        *factory, {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0)}));
    chain.push_back(MakeLineString(
        *factory, {CoordinateXY(1.0, 0.0), CoordinateXY(2.0, 1.0)}));
    std::unique_ptr<MultiLineString> mls =
        factory->createMultiLineString(std::move(chain));
    std::unique_ptr<Geometry> boundary = mls->getBoundary();
    ASSERT_EQ(boundary->getGeometryTypeId(), GEOS_MULTIPOINT);
    const auto* mp = dynamic_cast<const MultiPoint*>(boundary.get());
    ASSERT_NE(mp, nullptr);
    ASSERT_EQ(mp->getNumGeometries(), 2u);
    EXPECT_EQ(*mp->getGeometryN(0)->getCoordinate(), CoordinateXY(0.0, 0.0));
    EXPECT_EQ(*mp->getGeometryN(1)->getCoordinate(), CoordinateXY(2.0, 1.0));
  }

  // T-junction: three lines meeting at (1, 1) give valence 3 (odd -> in
  // boundary).
  {
    std::vector<std::unique_ptr<LineString>> tee;
    tee.push_back(MakeLineString(
        *factory, {CoordinateXY(0.0, 1.0), CoordinateXY(1.0, 1.0)}));
    tee.push_back(MakeLineString(
        *factory, {CoordinateXY(1.0, 1.0), CoordinateXY(2.0, 1.0)}));
    tee.push_back(MakeLineString(
        *factory, {CoordinateXY(1.0, 1.0), CoordinateXY(1.0, 0.0)}));
    std::unique_ptr<MultiLineString> mls =
        factory->createMultiLineString(std::move(tee));
    std::unique_ptr<Geometry> boundary = mls->getBoundary();
    EXPECT_EQ(boundary->getGeometryTypeId(), GEOS_MULTIPOINT);
    EXPECT_EQ(boundary->getNumGeometries(), 4u);
  }

  // Z and M ordinates are preserved in boundary points.
  {
    std::vector<std::unique_ptr<LineString>> lines_zm;
    lines_zm.push_back(
        factory->createLineString(std::make_unique<CoordinateSequence>(
            std::initializer_list<CoordinateXYZM>{
                CoordinateXYZM(1.0, 2.0, 3.0, 4.0),
                CoordinateXYZM(5.0, 6.0, 7.0, 8.0)})));
    std::unique_ptr<MultiLineString> mls_zm =
        factory->createMultiLineString(std::move(lines_zm));
    std::unique_ptr<Geometry> boundary = mls_zm->getBoundary();
    ASSERT_EQ(boundary->getNumGeometries(), 2u);
    EXPECT_TRUE(boundary->hasZ());
    EXPECT_TRUE(boundary->hasM());
    const auto* pt0 = dynamic_cast<const Point*>(boundary->getGeometryN(0));
    const auto* pt1 = dynamic_cast<const Point*>(boundary->getGeometryN(1));
    ASSERT_NE(pt0, nullptr);
    ASSERT_NE(pt1, nullptr);
    EXPECT_DOUBLE_EQ(pt0->getZ(), 3.0);
    EXPECT_DOUBLE_EQ(pt0->getM(), 4.0);
    EXPECT_DOUBLE_EQ(pt1->getZ(), 7.0);
    EXPECT_DOUBLE_EQ(pt1->getM(), 8.0);
  }
}

TEST(MultiLineStringTest,
     ReverseEmptyAndNonEmptyPreservesFactoryAndReversesLines) {
  PrecisionModel pm;
  GeometryFactory::Ptr factory = GeometryFactory::create(&pm, 4326);

  // Empty MultiLineString reverse clones it and preserves factory & SRID.
  std::unique_ptr<MultiLineString> empty_mls = factory->createMultiLineString();
  std::unique_ptr<MultiLineString> rev_empty = empty_mls->reverse();
  ASSERT_NE(rev_empty, nullptr);
  EXPECT_NE(rev_empty.get(), empty_mls.get());
  EXPECT_TRUE(rev_empty->isEmpty());
  EXPECT_EQ(rev_empty->getGeometryTypeId(), GEOS_MULTILINESTRING);
  EXPECT_EQ(rev_empty->getFactory(), factory.get());
  EXPECT_EQ(rev_empty->getSRID(), 4326);

  // MultiLineString with only empty LineStrings -> reverse clones it.
  {
    std::vector<std::unique_ptr<LineString>> empty_lines;
    empty_lines.push_back(factory->createLineString());
    empty_lines.push_back(factory->createLineString());
    std::unique_ptr<MultiLineString> mls_empty_children =
        factory->createMultiLineString(std::move(empty_lines));
    std::unique_ptr<MultiLineString> rev = mls_empty_children->reverse();
    EXPECT_TRUE(rev->isEmpty());
    EXPECT_EQ(rev->getNumGeometries(), 2u);
    EXPECT_EQ(rev->getSRID(), 4326);
  }

  // Non-empty MultiLineString reverses each LineString in-place in component
  // order.
  std::vector<std::unique_ptr<LineString>> lines;
  lines.push_back(
      MakeLineString(*factory, {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 2.0),
                                CoordinateXY(3.0, 4.0)}));
  lines.push_back(factory->createLineString(
      std::make_unique<CoordinateSequence>(std::initializer_list<Coordinate>{
          Coordinate(10.0, 0.0, 5.0), Coordinate(20.0, 0.0, 15.0)})));
  std::unique_ptr<MultiLineString> mls =
      factory->createMultiLineString(std::move(lines));

  std::unique_ptr<MultiLineString> reversed = mls->reverse();
  ASSERT_NE(reversed, nullptr);
  EXPECT_EQ(reversed->getFactory(), factory.get());
  EXPECT_EQ(reversed->getSRID(), 4326);
  EXPECT_EQ(reversed->getGeometryTypeId(), GEOS_MULTILINESTRING);
  ASSERT_EQ(reversed->getNumGeometries(), 2u);

  // Component 0 reversed: (3,4), (1,2), (0,0).
  const LineString* r0 = reversed->getGeometryN(0);
  ASSERT_EQ(r0->getNumPoints(), 3u);
  EXPECT_EQ(r0->getCoordinateN(0), CoordinateXY(3.0, 4.0));
  EXPECT_EQ(r0->getCoordinateN(1), CoordinateXY(1.0, 2.0));
  EXPECT_EQ(r0->getCoordinateN(2), CoordinateXY(0.0, 0.0));

  // Component 1 reversed with Z preserved: (20,0,15), (10,0,5).
  const LineString* r1 = reversed->getGeometryN(1);
  ASSERT_EQ(r1->getNumPoints(), 2u);
  EXPECT_TRUE(r1->getCoordinateN(0).equals3D(Coordinate(20.0, 0.0, 15.0)));
  EXPECT_TRUE(r1->getCoordinateN(1).equals3D(Coordinate(10.0, 0.0, 5.0)));

  // Polymorphic reverse() via Geometry*.
  const Geometry* base = mls.get();
  std::unique_ptr<Geometry> base_rev = base->reverse();
  EXPECT_EQ(base_rev->getGeometryTypeId(), GEOS_MULTILINESTRING);
  EXPECT_TRUE(base_rev->equalsExact(reversed.get()));

  // Double reverse restores the original coordinates.
  std::unique_ptr<MultiLineString> double_rev = reversed->reverse();
  EXPECT_TRUE(double_rev->equalsExact(mls.get()));
  EXPECT_TRUE(double_rev->equalsIdentical(mls.get()));

  // Mixed empty and non-empty LineString reverses cleanly.
  {
    std::vector<std::unique_ptr<LineString>> mixed;
    mixed.push_back(factory->createLineString());
    mixed.push_back(MakeLineString(
        *factory, {CoordinateXY(1.0, 2.0), CoordinateXY(3.0, 4.0)}));
    std::unique_ptr<MultiLineString> mls_mixed =
        factory->createMultiLineString(std::move(mixed));
    std::unique_ptr<MultiLineString> rev_mixed = mls_mixed->reverse();
    ASSERT_EQ(rev_mixed->getNumGeometries(), 2u);
    EXPECT_TRUE(rev_mixed->getGeometryN(0)->isEmpty());
    EXPECT_EQ(rev_mixed->getGeometryN(1)->getCoordinateN(0),
              CoordinateXY(3.0, 4.0));
    EXPECT_EQ(rev_mixed->getGeometryN(1)->getCoordinateN(1),
              CoordinateXY(1.0, 2.0));
  }
}

TEST(MultiLineStringTest, CloneNormalizeAndSortOrder) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::vector<std::unique_ptr<LineString>> lines_a;
  lines_a.push_back(MakeLineString(
      *factory, {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0)}));
  lines_a.push_back(MakeLineString(
      *factory, {CoordinateXY(5.0, 0.0), CoordinateXY(4.0, 0.0)}));
  std::unique_ptr<MultiLineString> mls_a =
      factory->createMultiLineString(std::move(lines_a));

  std::vector<std::unique_ptr<LineString>> lines_b;
  lines_b.push_back(MakeLineString(
      *factory, {CoordinateXY(4.0, 0.0), CoordinateXY(5.0, 0.0)}));
  lines_b.push_back(MakeLineString(
      *factory, {CoordinateXY(1.0, 0.0), CoordinateXY(0.0, 0.0)}));
  std::unique_ptr<MultiLineString> mls_b =
      factory->createMultiLineString(std::move(lines_b));

  std::unique_ptr<MultiLineString> cloned = mls_a->clone();
  ASSERT_NE(cloned, nullptr);
  EXPECT_NE(cloned.get(), mls_a.get());
  EXPECT_NE(cloned->getGeometryN(0), mls_a->getGeometryN(0));
  EXPECT_TRUE(cloned->equalsExact(mls_a.get()));
  EXPECT_TRUE(cloned->equalsIdentical(mls_a.get()));

  const Geometry* base_a = mls_a.get();
  std::unique_ptr<Geometry> base_cloned = base_a->clone();
  EXPECT_EQ(base_cloned->getGeometryTypeId(), GEOS_MULTILINESTRING);
  EXPECT_TRUE(base_cloned->equalsExact(mls_a.get()));

  EXPECT_FALSE(mls_a->equalsExact(mls_b.get()));
  mls_a->normalize();
  mls_b->normalize();
  EXPECT_TRUE(mls_a->equalsExact(mls_b.get()));

  // Sort index ordering: LineString (2) < LinearRing (3) < MultiLineString (4)
  // < Polygon (5) < MultiCurve (11).
  std::unique_ptr<LineString> single_ls = MakeLineString(
      *factory, {CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0)});
  std::unique_ptr<LinearRing> ring = factory->createLinearRing();
  std::unique_ptr<Polygon> poly = factory->createPolygon(2);
  std::unique_ptr<MultiCurve> mc = factory->createMultiCurve();
  EXPECT_GT(mls_a->compareTo(single_ls.get()), 0);
  EXPECT_GT(mls_a->compareTo(ring.get()), 0);
  EXPECT_LT(mls_a->compareTo(poly.get()), 0);
  EXPECT_LT(mls_a->compareTo(mc.get()), 0);
  EXPECT_EQ(mls_a->compareTo(mls_b.get()), 0);

  // Coordinate dimensions for XYM sequence.
  std::vector<std::unique_ptr<LineString>> lines_m;
  lines_m.push_back(factory->createLineString(
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXYM>{
          CoordinateXYM(0.0, 0.0, 1.5), CoordinateXYM(1.0, 1.0, 2.5)})));
  std::unique_ptr<MultiLineString> mls_m =
      factory->createMultiLineString(std::move(lines_m));
  EXPECT_FALSE(mls_m->hasZ());
  EXPECT_TRUE(mls_m->hasM());
  EXPECT_EQ(mls_m->getCoordinateDimension(), 3u);
}

}  // namespace
}  // namespace geos_google
