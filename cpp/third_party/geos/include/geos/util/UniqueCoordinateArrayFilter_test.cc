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

// Unit tests for
// third_party/geos/include/geos/util/UniqueCoordinateArrayFilter.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/util/UniqueCoordinateArrayFilter.h

#include "third_party/geos/include/geos/util/UniqueCoordinateArrayFilter.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
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

using geos::geom::Coordinate;
using geos::geom::CoordinateFilter;
using geos::geom::CoordinateInspector;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::util::UniqueCoordinateArrayFilter;

static_assert(std::is_base_of_v<CoordinateFilter, UniqueCoordinateArrayFilter>);
static_assert(
    std::is_base_of_v<CoordinateInspector<UniqueCoordinateArrayFilter>,
                      UniqueCoordinateArrayFilter>);
static_assert(std::has_virtual_destructor_v<UniqueCoordinateArrayFilter>);
static_assert(std::is_constructible_v<UniqueCoordinateArrayFilter,
                                      std::vector<const Coordinate*>&>);
static_assert(
    std::is_constructible_v<UniqueCoordinateArrayFilter,
                            std::vector<const Coordinate*>&, std::size_t>);
static_assert(!std::is_copy_constructible_v<UniqueCoordinateArrayFilter>);
static_assert(!std::is_copy_assignable_v<UniqueCoordinateArrayFilter>);
static_assert(!std::is_move_constructible_v<UniqueCoordinateArrayFilter>);
static_assert(!std::is_move_assignable_v<UniqueCoordinateArrayFilter>);

TEST(UniqueCoordinateArrayFilterTest,
     CollectsUniqueCoordinatesInInsertionOrder) {
  const Coordinate c0(1.0, 2.0, 10.0);
  const Coordinate c1(3.0, 4.0, 20.0);
  const Coordinate c2_dup_diff_z(1.0, 2.0, 99.0);
  const Coordinate c3(-0.0, 0.0, geos::DoubleNotANumber);
  const Coordinate c4_neg_zero_dup(0.0, -0.0, 5.0);
  const Coordinate c5(5.0, 6.0);

  std::vector<const Coordinate*> pts;
  UniqueCoordinateArrayFilter filter(pts);
  EXPECT_FALSE(filter.isDone());

  filter.filter(&c0);
  filter.filter(&c1);
  filter.filter(&c2_dup_diff_z);
  filter.filter(&c3);
  filter.filter(&c4_neg_zero_dup);
  filter.filter(&c5);

  EXPECT_FALSE(filter.isDone());
  ASSERT_EQ(pts.size(), 4u);
  EXPECT_EQ(pts[0], &c0);
  EXPECT_EQ(pts[1], &c1);
  EXPECT_EQ(pts[2], &c3);
  EXPECT_EQ(pts[3], &c5);
  EXPECT_DOUBLE_EQ(pts[0]->z, 10.0);
  EXPECT_TRUE(std::isnan(pts[2]->z));
  EXPECT_TRUE(std::isnan(pts[3]->z));
}

TEST(UniqueCoordinateArrayFilterTest, PreservesPreexistingTargetElements) {
  const Coordinate preexisting(9.0, 9.0, 1.0);
  const Coordinate added(9.0, 9.0, 2.0);
  const Coordinate added_dup(9.0, 9.0, 3.0);
  const Coordinate distinct(1.0, 1.0, 4.0);

  std::vector<const Coordinate*> pts = {&preexisting};
  UniqueCoordinateArrayFilter filter(pts);

  filter.filter(&added);
  filter.filter(&added_dup);
  filter.filter(&distinct);

  ASSERT_EQ(pts.size(), 3u);
  EXPECT_EQ(pts[0], &preexisting);
  EXPECT_EQ(pts[1], &added);
  EXPECT_EQ(pts[2], &distinct);
}

TEST(UniqueCoordinateArrayFilterTest,
     SupportsCoordinateXyzmAndPolymorphicBaseUsage) {
  const CoordinateXYZM m0(1.0, 2.0, 3.0, 4.0);
  const CoordinateXYZM m1_dup_xy(1.0, 2.0, 30.0, 40.0);
  const CoordinateXYZM m2(7.0, 8.0, 9.0, 10.0);
  const Coordinate xyz3(11.0, 12.0, 13.0);

  std::vector<const Coordinate*> pts;
  auto filter = std::make_unique<UniqueCoordinateArrayFilter>(pts);
  CoordinateFilter* base = filter.get();
  EXPECT_FALSE(base->isDone());

  base->filter_ro(&m0);
  base->filter_ro(&m1_dup_xy);
  base->filter_ro(&m2);
  base->filter_ro(&xyz3);

  EXPECT_FALSE(base->isDone());
  ASSERT_EQ(pts.size(), 3u);
  EXPECT_EQ(pts[0], static_cast<const Coordinate*>(&m0));
  EXPECT_EQ(pts[1], static_cast<const Coordinate*>(&m2));
  EXPECT_EQ(pts[2], &xyz3);
  EXPECT_DOUBLE_EQ(pts[0]->z, 3.0);
  EXPECT_DOUBLE_EQ(pts[1]->z, 9.0);

  // Exercise virtual destruction through CoordinateFilter base pointer.
  std::unique_ptr<CoordinateFilter> poly_filter = std::move(filter);
  poly_filter.reset();
}

TEST(UniqueCoordinateArrayFilterTest, MaxUniqueSetsDoneOnlyWhenExceeded) {
  const Coordinate p0(0.0, 0.0);
  const Coordinate p1(1.0, 0.0);
  const Coordinate p2(2.0, 0.0);
  const Coordinate p3(3.0, 0.0);

  {
    std::vector<const Coordinate*> pts;
    UniqueCoordinateArrayFilter zero_limit(pts, 0);
    EXPECT_FALSE(zero_limit.isDone());
    zero_limit.filter(&p0);
    EXPECT_TRUE(zero_limit.isDone());
    ASSERT_EQ(pts.size(), 1u);
    EXPECT_EQ(pts[0], &p0);
  }

  {
    std::vector<const Coordinate*> pts;
    UniqueCoordinateArrayFilter two_limit(pts, 2);
    two_limit.filter(&p0);
    two_limit.filter(&p0);
    EXPECT_FALSE(two_limit.isDone());
    two_limit.filter(&p1);
    two_limit.filter(&p1);
    EXPECT_FALSE(two_limit.isDone());
    EXPECT_EQ(pts.size(), 2u);

    // Exceeding maxUnique (size == 3 > 2) sets isDone() to true.
    two_limit.filter(&p2);
    EXPECT_TRUE(two_limit.isDone());
    ASSERT_EQ(pts.size(), 3u);
    EXPECT_EQ(pts[2], &p2);
  }

  {
    std::vector<const Coordinate*> pts;
    UniqueCoordinateArrayFilter unlimited(pts, geos::NO_COORD_INDEX);
    unlimited.filter(&p0);
    unlimited.filter(&p1);
    unlimited.filter(&p2);
    unlimited.filter(&p3);
    EXPECT_FALSE(unlimited.isDone());
    EXPECT_EQ(pts.size(), 4u);
  }
}

TEST(UniqueCoordinateArrayFilterTest, FiltersCoordinateSequencesWithEarlyStop) {
  const CoordinateSequence xyz_seq({
      Coordinate(0.0, 0.0, 1.0),
      Coordinate(1.0, 0.0, 2.0),
      Coordinate(0.0, 0.0, 3.0),
      Coordinate(2.0, 2.0, 4.0),
      Coordinate(3.0, 3.0, 5.0),
  });

  std::vector<const Coordinate*> all_pts;
  UniqueCoordinateArrayFilter all_filter(all_pts);
  xyz_seq.apply_ro(&all_filter);
  EXPECT_FALSE(all_filter.isDone());
  ASSERT_EQ(all_pts.size(), 4u);
  EXPECT_TRUE(all_pts[0]->equals3D(Coordinate(0.0, 0.0, 1.0)));
  EXPECT_TRUE(all_pts[1]->equals3D(Coordinate(1.0, 0.0, 2.0)));
  EXPECT_TRUE(all_pts[2]->equals3D(Coordinate(2.0, 2.0, 4.0)));
  EXPECT_TRUE(all_pts[3]->equals3D(Coordinate(3.0, 3.0, 5.0)));

  std::vector<const Coordinate*> capped_pts;
  UniqueCoordinateArrayFilter capped_filter(capped_pts, 2);
  xyz_seq.apply_ro(&capped_filter);
  EXPECT_TRUE(capped_filter.isDone());
  ASSERT_EQ(capped_pts.size(), 3u);
  EXPECT_TRUE(capped_pts[2]->equals2D(CoordinateXY(2.0, 2.0)));

  const CoordinateSequence xy_seq({
      CoordinateXY(2.0, 3.0),
      CoordinateXY(4.0, 5.0),
      CoordinateXY(2.0, 3.0),
  });
  std::vector<const Coordinate*> xy_pts;
  UniqueCoordinateArrayFilter xy_filter(xy_pts);
  xy_seq.apply_ro(&xy_filter);
  EXPECT_FALSE(xy_filter.isDone());
  ASSERT_EQ(xy_pts.size(), 2u);
  EXPECT_TRUE(xy_pts[0]->equals2D(CoordinateXY(2.0, 3.0)));
  EXPECT_TRUE(xy_pts[1]->equals2D(CoordinateXY(4.0, 5.0)));

  const CoordinateSequence xyzm_seq({
      CoordinateXYZM(1.0, 1.0, 2.0, 10.0),
      CoordinateXYZM(1.0, 1.0, 9.0, 90.0),
      CoordinateXYZM(4.0, 5.0, 6.0, 20.0),
  });
  std::vector<const Coordinate*> xyzm_pts;
  UniqueCoordinateArrayFilter xyzm_filter(xyzm_pts);
  xyzm_seq.apply_ro(&xyzm_filter);
  EXPECT_FALSE(xyzm_filter.isDone());
  ASSERT_EQ(xyzm_pts.size(), 2u);
  EXPECT_TRUE(xyzm_pts[0]->equals3D(Coordinate(1.0, 1.0, 2.0)));
  EXPECT_TRUE(xyzm_pts[1]->equals3D(Coordinate(4.0, 5.0, 6.0)));
}

TEST(UniqueCoordinateArrayFilterTest, FiltersEmptyAndMultiPartGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto empty_line = factory->createLineString();
  std::vector<const Coordinate*> empty_pts;
  UniqueCoordinateArrayFilter empty_filter(empty_pts);
  empty_line->apply_ro(&empty_filter);
  EXPECT_TRUE(empty_pts.empty());
  EXPECT_FALSE(empty_filter.isDone());

  auto pt = factory->createPoint(Coordinate(0.0, 0.0, 1.0));
  auto line = factory->createLineString(
      std::make_unique<CoordinateSequence>(std::initializer_list<Coordinate>{
          Coordinate(0.0, 0.0, 2.0),
          Coordinate(10.0, 0.0, 2.0),
          Coordinate(10.0, 10.0, 2.0),
          Coordinate(0.0, 0.0, 2.0),
      }));

  auto shell_seq =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});
  auto hole_seq =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {2.0, 2.0}, {4.0, 2.0}, {4.0, 4.0}, {2.0, 4.0}, {2.0, 2.0}});
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(factory->createLinearRing(std::move(hole_seq)));
  auto poly = factory->createPolygon(
      factory->createLinearRing(std::move(shell_seq)), std::move(holes));

  std::vector<std::unique_ptr<Geometry>> geoms;
  geoms.push_back(std::move(pt));
  geoms.push_back(std::move(line));
  geoms.push_back(std::move(poly));
  auto collection = factory->createGeometryCollection(std::move(geoms));

  std::vector<const Coordinate*> geom_pts;
  UniqueCoordinateArrayFilter geom_filter(geom_pts);
  collection->apply_ro(&geom_filter);
  EXPECT_FALSE(geom_filter.isDone());
  ASSERT_EQ(geom_pts.size(), 8u);
  EXPECT_TRUE(geom_pts[0]->equals2D(CoordinateXY(0.0, 0.0)));
  EXPECT_TRUE(geom_pts[1]->equals2D(CoordinateXY(10.0, 0.0)));
  EXPECT_TRUE(geom_pts[2]->equals2D(CoordinateXY(10.0, 10.0)));
  EXPECT_TRUE(geom_pts[3]->equals2D(CoordinateXY(0.0, 10.0)));
  EXPECT_TRUE(geom_pts[4]->equals2D(CoordinateXY(2.0, 2.0)));
  EXPECT_TRUE(geom_pts[5]->equals2D(CoordinateXY(4.0, 2.0)));
  EXPECT_TRUE(geom_pts[6]->equals2D(CoordinateXY(4.0, 4.0)));
  EXPECT_TRUE(geom_pts[7]->equals2D(CoordinateXY(2.0, 4.0)));

  std::vector<const Coordinate*> capped_geom_pts;
  UniqueCoordinateArrayFilter capped_geom_filter(capped_geom_pts, 3);
  collection->apply_ro(&capped_geom_filter);
  EXPECT_TRUE(capped_geom_filter.isDone());
  EXPECT_EQ(capped_geom_pts.size(), 4u);
}

#ifdef NDEBUG
TEST(UniqueCoordinateArrayFilterTest,
     UnsupportedXyAndXymOverloadsAreNoOpsUnderNdDebug) {
  std::vector<const Coordinate*> pts;
  UniqueCoordinateArrayFilter filter(pts);
  CoordinateFilter* base = &filter;

  const CoordinateXY xy(1.0, 2.0);
  const CoordinateXYM xym(3.0, 4.0, 5.0);

  filter.filter(&xy);
  filter.filter(&xym);
  base->filter_ro(&xy);
  base->filter_ro(&xym);

  EXPECT_TRUE(pts.empty());
  EXPECT_FALSE(filter.isDone());
}
#elif defined(GTEST_HAS_DEATH_TEST)
TEST(UniqueCoordinateArrayFilterDeathTest,
     UnsupportedXyAndXymOverloadsAbortWithoutNdDebug) {
  std::vector<const Coordinate*> pts;
  UniqueCoordinateArrayFilter filter(pts);
  CoordinateFilter* base = &filter;

  const CoordinateXY xy(1.0, 2.0);
  const CoordinateXYM xym(3.0, 4.0, 5.0);

  EXPECT_DEATH(filter.filter(&xy), "");
  EXPECT_DEATH(filter.filter(&xym), "");
  EXPECT_DEATH(base->filter_ro(&xy), "");
  EXPECT_DEATH(base->filter_ro(&xym), "");
}
#endif

}  // namespace
}  // namespace geos_google
