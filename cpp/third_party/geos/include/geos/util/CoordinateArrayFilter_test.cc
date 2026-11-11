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

// Unit tests for third_party/geos/include/geos/util/CoordinateArrayFilter.h.
//
// google3 notes:
// - Uses isolated GeometryFactory::create() instances rather than
//   getDefaultInstance().
// - CoordinateXY and CoordinateXYM overloads contain assert(0); verified via
//   EXPECT_DEBUG_DEATH so tests pass in both debug/fastbuild and -c opt.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/util/CoordinateArrayFilter.h

#include "third_party/geos/include/geos/util/CoordinateArrayFilter.h"

#include <cmath>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateFilter.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateFilter;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::geom::CoordinateXYM;
using ::geos::geom::CoordinateXYZM;
using ::geos::geom::Geometry;
using ::geos::geom::GeometryCollection;
using ::geos::geom::GeometryFactory;
using ::geos::geom::LinearRing;
using ::geos::geom::LineString;
using ::geos::geom::Point;
using ::geos::geom::Polygon;
using ::geos::util::CoordinateArrayFilter;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

static_assert(std::is_base_of_v<CoordinateFilter, CoordinateArrayFilter>);
static_assert(!std::is_copy_constructible_v<CoordinateArrayFilter>);
static_assert(!std::is_copy_assignable_v<CoordinateArrayFilter>);
static_assert(!std::is_move_constructible_v<CoordinateArrayFilter>);
static_assert(!std::is_move_assignable_v<CoordinateArrayFilter>);

TEST(CoordinateArrayFilterTest, EmptyGeometriesYieldNoCoordinates) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> empty_pt = factory->createPoint();
  std::unique_ptr<LineString> empty_line = factory->createLineString();
  std::unique_ptr<Polygon> empty_poly = factory->createPolygon();
  std::unique_ptr<GeometryCollection> empty_coll =
      factory->createGeometryCollection();

  std::vector<const Coordinate*> pts;
  CoordinateArrayFilter filter(pts);
  EXPECT_FALSE(filter.isDone());

  empty_pt->apply_ro(&filter);
  empty_line->apply_ro(&filter);
  empty_poly->apply_ro(&filter);
  empty_coll->apply_ro(&filter);

  EXPECT_THAT(pts, IsEmpty());
}

TEST(CoordinateArrayFilterTest, CollectsPointCoordinatesAndPointerIdentity) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> pt_xy = factory->createPoint(CoordinateXY(1.5, -2.5));
  std::unique_ptr<Point> pt_xyz =
      factory->createPoint(Coordinate(3.0, 4.0, 5.0));
  std::unique_ptr<Point> pt_xym =
      factory->createPoint(CoordinateXYM(6.0, 7.0, 8.0));
  std::unique_ptr<Point> pt_xyzm =
      factory->createPoint(CoordinateXYZM(9.0, 10.0, 11.0, 12.0));

  std::vector<const Coordinate*> pts;
  CoordinateArrayFilter filter(pts);
  pt_xy->apply_ro(&filter);
  pt_xyz->apply_ro(&filter);
  pt_xym->apply_ro(&filter);
  pt_xyzm->apply_ro(&filter);

  ASSERT_EQ(pts.size(), 4u);
  EXPECT_EQ(pts[0], &pt_xy->getCoordinatesRO()->getAt(0));
  EXPECT_DOUBLE_EQ(pts[0]->x, 1.5);
  EXPECT_DOUBLE_EQ(pts[0]->y, -2.5);
  EXPECT_TRUE(std::isnan(pts[0]->z));

  EXPECT_EQ(pts[1], &pt_xyz->getCoordinatesRO()->getAt(0));
  EXPECT_DOUBLE_EQ(pts[1]->x, 3.0);
  EXPECT_DOUBLE_EQ(pts[1]->y, 4.0);
  EXPECT_DOUBLE_EQ(pts[1]->z, 5.0);

  // With GEOS_COORDSEQ_PADZ, XYM sequences are padded to XYZM (stride 4) and
  // dispatched as const CoordinateXYZM*, upcasting cleanly to const
  // Coordinate*.
  EXPECT_DOUBLE_EQ(pts[2]->x, 6.0);
  EXPECT_DOUBLE_EQ(pts[2]->y, 7.0);
  EXPECT_TRUE(std::isnan(pts[2]->z));

  EXPECT_DOUBLE_EQ(pts[3]->x, 9.0);
  EXPECT_DOUBLE_EQ(pts[3]->y, 10.0);
  EXPECT_DOUBLE_EQ(pts[3]->z, 11.0);
}

TEST(CoordinateArrayFilterTest, PreservesOrderAndDuplicatesInLineString) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  const CoordinateSequence seq{
      Coordinate(0.0, 0.0),
      Coordinate(1.0, 1.0),
      Coordinate(0.0, 0.0),
      Coordinate(2.0, 2.0),
  };
  std::unique_ptr<LineString> line = factory->createLineString(seq);

  std::vector<const Coordinate*> pts;
  CoordinateArrayFilter filter(pts);
  line->apply_ro(&filter);

  ASSERT_EQ(pts.size(), 4u);
  EXPECT_EQ(*pts[0], Coordinate(0.0, 0.0));
  EXPECT_EQ(*pts[1], Coordinate(1.0, 1.0));
  EXPECT_EQ(*pts[2], Coordinate(0.0, 0.0));
  EXPECT_EQ(*pts[3], Coordinate(2.0, 2.0));
  EXPECT_EQ(pts[0], &line->getCoordinatesRO()->getAt(0));
  EXPECT_EQ(pts[2], &line->getCoordinatesRO()->getAt(2));
}

TEST(CoordinateArrayFilterTest, CollectsPolygonShellAndHoles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  const CoordinateSequence shell_seq{
      Coordinate(0.0, 0.0),  Coordinate(10.0, 0.0), Coordinate(10.0, 10.0),
      Coordinate(0.0, 10.0), Coordinate(0.0, 0.0),
  };
  std::unique_ptr<LinearRing> shell = factory->createLinearRing(shell_seq);

  const CoordinateSequence hole_seq{
      Coordinate(2.0, 2.0), Coordinate(2.0, 4.0), Coordinate(4.0, 4.0),
      Coordinate(4.0, 2.0), Coordinate(2.0, 2.0),
  };
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(factory->createLinearRing(hole_seq));

  std::unique_ptr<Polygon> poly =
      factory->createPolygon(std::move(shell), std::move(holes));

  std::vector<const Coordinate*> pts;
  CoordinateArrayFilter filter(pts);
  poly->apply_ro(&filter);

  ASSERT_EQ(pts.size(), 10u);
  EXPECT_EQ(*pts[0], Coordinate(0.0, 0.0));
  EXPECT_EQ(*pts[2], Coordinate(10.0, 10.0));
  EXPECT_EQ(*pts[4], Coordinate(0.0, 0.0));
  EXPECT_EQ(*pts[5], Coordinate(2.0, 2.0));
  EXPECT_EQ(*pts[7], Coordinate(4.0, 4.0));
  EXPECT_EQ(*pts[9], Coordinate(2.0, 2.0));
}

TEST(CoordinateArrayFilterTest,
     AppendsAcrossGeometryCollectionAndExistingItems) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  const CoordinateSequence line_seq{
      Coordinate(3.0, 4.0),
      Coordinate(5.0, 6.0),
  };
  std::vector<std::unique_ptr<Geometry>> geoms;
  geoms.push_back(factory->createPoint(Coordinate(1.0, 2.0)));
  geoms.push_back(factory->createPoint());
  geoms.push_back(factory->createLineString(line_seq));
  std::unique_ptr<GeometryCollection> coll =
      factory->createGeometryCollection(std::move(geoms));

  const Coordinate initial(99.0, 99.0);
  std::vector<const Coordinate*> pts = {&initial};
  CoordinateArrayFilter filter(pts);
  coll->apply_ro(&filter);

  ASSERT_EQ(pts.size(), 4u);
  EXPECT_EQ(pts[0], &initial);
  EXPECT_EQ(*pts[1], Coordinate(1.0, 2.0));
  EXPECT_EQ(*pts[2], Coordinate(3.0, 4.0));
  EXPECT_EQ(*pts[3], Coordinate(5.0, 6.0));
}

TEST(CoordinateArrayFilterTest,
     WorksWithCoordinateSequenceAndDirectFilterCalls) {
  const CoordinateSequence seq_xyz{
      Coordinate(1.0, 2.0, 3.0),
      Coordinate(4.0, 5.0, 6.0),
  };
  const CoordinateSequence seq_xyzm{
      CoordinateXYZM(7.0, 8.0, 9.0, 10.0),
  };

  std::vector<const Coordinate*> pts;
  CoordinateArrayFilter filter(pts);
  seq_xyz.apply_ro(&filter);
  seq_xyzm.apply_ro(&filter);

  const Coordinate direct_xyz(11.0, 12.0, 13.0);
  const CoordinateXYZM direct_xyzm(14.0, 15.0, 16.0, 17.0);
  filter.filter(&direct_xyz);
  filter.filter(&direct_xyzm);

  EXPECT_THAT(
      pts,
      ElementsAre(
          &seq_xyz.getAt<Coordinate>(0), &seq_xyz.getAt<Coordinate>(1),
          static_cast<const Coordinate*>(&seq_xyzm.getAt<CoordinateXYZM>(0)),
          &direct_xyz, static_cast<const Coordinate*>(&direct_xyzm)));
}

TEST(CoordinateArrayFilterTest, PolymorphicDestructionViaBasePointer) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> pt = factory->createPoint(Coordinate(7.0, 8.0));

  std::vector<const Coordinate*> pts;
  std::unique_ptr<CoordinateFilter> base_filter =
      std::make_unique<CoordinateArrayFilter>(pts);
  pt->apply_ro(base_filter.get());
  ASSERT_EQ(pts.size(), 1u);
  EXPECT_EQ(*pts[0], Coordinate(7.0, 8.0));

  base_filter.reset();
}

TEST(CoordinateArrayFilterTest, RejectsCoordinateXYInDebug) {
  std::vector<const Coordinate*> pts;
  CoordinateArrayFilter filter(pts);
  CoordinateFilter* base_filter = &filter;
  const CoordinateXY xy(1.0, 2.0);
  EXPECT_DEBUG_DEATH(base_filter->filter_ro(&xy), "");
  EXPECT_THAT(pts, IsEmpty());
}

TEST(CoordinateArrayFilterTest, RejectsCoordinateXYMInDebug) {
  std::vector<const Coordinate*> pts;
  CoordinateArrayFilter filter(pts);
  CoordinateFilter* base_filter = &filter;
  const CoordinateXYM xym(1.0, 2.0, 3.0);
  EXPECT_DEBUG_DEATH(base_filter->filter_ro(&xym), "");
  EXPECT_THAT(pts, IsEmpty());
}

}  // namespace
}  // namespace geos_google
