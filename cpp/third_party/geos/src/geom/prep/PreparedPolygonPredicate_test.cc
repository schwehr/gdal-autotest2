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

// Unit tests for third_party/geos/src/geom/prep/PreparedPolygonPredicate.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/prep/PreparedPolygonPredicate.cpp

#include "third_party/geos/include/geos/geom/prep/PreparedPolygonPredicate.h"

#include <initializer_list>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/prep/PreparedPolygon.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Location;
using geos::geom::MultiPoint;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::prep::PreparedPolygon;
using geos::geom::prep::PreparedPolygonPredicate;

static_assert(!std::is_copy_constructible_v<PreparedPolygonPredicate>);
static_assert(!std::is_copy_assignable_v<PreparedPolygonPredicate>);

class TestablePreparedPolygonPredicate : public PreparedPolygonPredicate {
 public:
  using PreparedPolygonPredicate::PreparedPolygonPredicate;

  Location OutermostTestComponentLocation(const Geometry* test_geom) const {
    return getOutermostTestComponentLocation(test_geom);
  }

  bool AllTestComponentsInTargetInterior(const Geometry* test_geom) const {
    return isAllTestComponentsInTargetInterior(test_geom);
  }

  bool AnyTestComponentInTarget(const Geometry* test_geom) const {
    return isAnyTestComponentInTarget(test_geom);
  }

  bool AnyTestComponentInTargetInterior(const Geometry* test_geom) const {
    return isAnyTestComponentInTargetInterior(test_geom);
  }

  bool AnyTargetComponentInAreaTest(
      const Geometry* test_geom,
      const std::vector<const CoordinateXY*>* target_rep_pts) const {
    return isAnyTargetComponentInAreaTest(test_geom, target_rep_pts);
  }
};

std::unique_ptr<Point> MakePoint(const GeometryFactory& factory, double x,
                                 double y) {
  return factory.createPoint(CoordinateXY(x, y));
}

std::unique_ptr<LineString> MakeLineString(
    const GeometryFactory& factory,
    std::initializer_list<CoordinateXY> coords) {
  auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
  for (const auto& coord : coords) {
    seq->add(coord);
  }
  return factory.createLineString(std::move(seq));
}

std::unique_ptr<LinearRing> MakeRing(
    const GeometryFactory& factory,
    std::initializer_list<CoordinateXY> coords) {
  auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
  for (const auto& coord : coords) {
    seq->add(coord);
  }
  return factory.createLinearRing(std::move(seq));
}

std::unique_ptr<Polygon> MakeBoxPolygon(const GeometryFactory& factory,
                                        double min_x, double min_y,
                                        double max_x, double max_y) {
  auto shell = MakeRing(factory, {
                                     {min_x, min_y},
                                     {max_x, min_y},
                                     {max_x, max_y},
                                     {min_x, max_y},
                                     {min_x, min_y},
                                 });
  return factory.createPolygon(std::move(shell));
}

std::unique_ptr<Polygon> MakeSquareWithHole(const GeometryFactory& factory) {
  // Outer shell [0, 10] x [0, 10], hole [3, 7] x [3, 7].
  auto shell = MakeRing(factory, {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}});
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(MakeRing(factory, {{3, 3}, {7, 3}, {7, 7}, {3, 7}, {3, 3}}));
  return factory.createPolygon(std::move(shell), std::move(holes));
}

std::unique_ptr<MultiPoint> MakeMultiPoint(
    const GeometryFactory& factory,
    std::initializer_list<CoordinateXY> coords) {
  std::vector<std::unique_ptr<Point>> pts;
  pts.reserve(coords.size());
  for (const auto& coord : coords) {
    pts.push_back(factory.createPoint(coord));
  }
  return factory.createMultiPoint(std::move(pts));
}

TEST(PreparedPolygonPredicateTest, SupportsPolymorphicDestruction) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto target = MakeSquareWithHole(*factory);
  PreparedPolygon prep(target.get());

  std::unique_ptr<PreparedPolygonPredicate> base =
      std::make_unique<TestablePreparedPolygonPredicate>(&prep);
  EXPECT_NE(base.get(), nullptr);
  base.reset();
}

TEST(PreparedPolygonPredicateTest,
     GetOutermostTestComponentLocationHandlesEmptyAndSinglePoints) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto target = MakeSquareWithHole(*factory);
  PreparedPolygon prep(target.get());
  TestablePreparedPolygonPredicate pred(&prep);

  auto empty_pt = factory->createPoint();
  auto empty_line = factory->createLineString();
  auto empty_poly = factory->createPolygon();
  auto empty_gc = factory->createGeometryCollection();

  EXPECT_EQ(pred.OutermostTestComponentLocation(empty_pt.get()),
            Location::NONE);
  EXPECT_EQ(pred.OutermostTestComponentLocation(empty_line.get()),
            Location::NONE);
  EXPECT_EQ(pred.OutermostTestComponentLocation(empty_poly.get()),
            Location::NONE);
  EXPECT_EQ(pred.OutermostTestComponentLocation(empty_gc.get()),
            Location::NONE);

  auto interior_pt = MakePoint(*factory, 1.0, 1.0);
  auto shell_boundary_pt = MakePoint(*factory, 0.0, 5.0);
  auto hole_boundary_pt = MakePoint(*factory, 5.0, 3.0);
  auto outside_pt = MakePoint(*factory, 12.0, 12.0);
  auto inside_hole_pt = MakePoint(*factory, 5.0, 5.0);

  EXPECT_EQ(pred.OutermostTestComponentLocation(interior_pt.get()),
            Location::INTERIOR);
  EXPECT_EQ(pred.OutermostTestComponentLocation(shell_boundary_pt.get()),
            Location::BOUNDARY);
  EXPECT_EQ(pred.OutermostTestComponentLocation(hole_boundary_pt.get()),
            Location::BOUNDARY);
  EXPECT_EQ(pred.OutermostTestComponentLocation(outside_pt.get()),
            Location::EXTERIOR);
  EXPECT_EQ(pred.OutermostTestComponentLocation(inside_hole_pt.get()),
            Location::EXTERIOR);
}

TEST(PreparedPolygonPredicateTest,
     GetOutermostTestComponentLocationAggregatesAndShortCircuits) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto target = MakeSquareWithHole(*factory);
  PreparedPolygon prep(target.get());
  TestablePreparedPolygonPredicate pred(&prep);

  // NONE -> INTERIOR -> BOUNDARY, and subsequent INTERIOR does not downgrade.
  auto int_bnd_int = MakeMultiPoint(*factory, {{1, 1}, {5, 0}, {8, 8}});
  EXPECT_EQ(pred.OutermostTestComponentLocation(int_bnd_int.get()),
            Location::BOUNDARY);

  // BOUNDARY followed by EXTERIOR triggers else-if (done = true) and stops
  // before trailing INTERIOR point.
  auto bnd_ext_int = MakeMultiPoint(*factory, {{0, 0}, {12, 12}, {1, 1}});
  EXPECT_EQ(pred.OutermostTestComponentLocation(bnd_ext_int.get()),
            Location::EXTERIOR);

  // INTERIOR followed by EXTERIOR upgrades via first branch; trailing BOUNDARY
  // keeps EXTERIOR.
  auto int_ext_bnd = MakeMultiPoint(*factory, {{1, 1}, {5, 5}, {0, 5}});
  EXPECT_EQ(pred.OutermostTestComponentLocation(int_ext_bnd.get()),
            Location::EXTERIOR);

  // First component EXTERIOR stays EXTERIOR even with INTERIOR/BOUNDARY later.
  auto ext_int_bnd = MakeMultiPoint(*factory, {{-1, -1}, {1, 1}, {5, 0}});
  EXPECT_EQ(pred.OutermostTestComponentLocation(ext_int_bnd.get()),
            Location::EXTERIOR);

  // Collection containing an empty component mid-traversal exercises empty
  // skip.
  std::vector<std::unique_ptr<Geometry>> geoms;
  geoms.push_back(MakePoint(*factory, 1.0, 1.0));
  geoms.push_back(factory->createPoint());
  geoms.push_back(MakePoint(*factory, 5.0, 3.0));
  auto mixed_gc = factory->createGeometryCollection(std::move(geoms));
  EXPECT_EQ(pred.OutermostTestComponentLocation(mixed_gc.get()),
            Location::BOUNDARY);

  // Polygon whose shell starts on target boundary and hole starts in target
  // hole interior (target EXTERIOR).
  auto shell_bnd_hole_ext = factory->createPolygon(
      MakeRing(*factory, {{0, 0}, {9, 0}, {9, 9}, {0, 9}, {0, 0}}), [&]() {
        std::vector<std::unique_ptr<LinearRing>> holes;
        holes.push_back(
            MakeRing(*factory, {{5, 5}, {6, 5}, {6, 6}, {5, 6}, {5, 5}}));
        return holes;
      }());
  EXPECT_EQ(pred.OutermostTestComponentLocation(shell_bnd_hole_ext.get()),
            Location::EXTERIOR);
}

TEST(PreparedPolygonPredicateTest, IsAllTestComponentsInTargetInterior) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto target = MakeSquareWithHole(*factory);
  PreparedPolygon prep(target.get());
  TestablePreparedPolygonPredicate pred(&prep);

  // Empty geometries have no components outside interior -> vacuously true.
  auto empty_pt = factory->createPoint();
  auto empty_gc = factory->createGeometryCollection();
  EXPECT_TRUE(pred.AllTestComponentsInTargetInterior(empty_pt.get()));
  EXPECT_TRUE(pred.AllTestComponentsInTargetInterior(empty_gc.get()));

  // All representative points in interior -> true.
  auto int_pt = MakePoint(*factory, 1.0, 1.0);
  auto int_mp = MakeMultiPoint(*factory, {{1, 1}, {2, 8}, {9, 9}});
  auto int_line = MakeLineString(*factory, {{1, 1}, {15, 15}});
  auto int_poly = MakeBoxPolygon(*factory, 1.0, 1.0, 2.0, 2.0);
  EXPECT_TRUE(pred.AllTestComponentsInTargetInterior(int_pt.get()));
  EXPECT_TRUE(pred.AllTestComponentsInTargetInterior(int_mp.get()));
  EXPECT_TRUE(pred.AllTestComponentsInTargetInterior(int_line.get()));
  EXPECT_TRUE(pred.AllTestComponentsInTargetInterior(int_poly.get()));

  // Boundary and exterior components -> false.
  auto bnd_pt = MakePoint(*factory, 5.0, 0.0);
  auto ext_pt = MakePoint(*factory, 5.0, 5.0);
  EXPECT_FALSE(pred.AllTestComponentsInTargetInterior(bnd_pt.get()));
  EXPECT_FALSE(pred.AllTestComponentsInTargetInterior(ext_pt.get()));

  // First two match INTERIOR (isDone=false), third is BOUNDARY (isDone=true
  // stops before trailing INTERIOR point).
  auto mixed_mp = MakeMultiPoint(*factory, {{1, 1}, {2, 2}, {0, 5}, {8, 8}});
  EXPECT_FALSE(pred.AllTestComponentsInTargetInterior(mixed_mp.get()));

  // Empty element inside a collection is skipped without falsifying result.
  std::vector<std::unique_ptr<Geometry>> all_int_with_empty;
  all_int_with_empty.push_back(MakePoint(*factory, 1.0, 1.0));
  all_int_with_empty.push_back(factory->createPoint());
  all_int_with_empty.push_back(MakePoint(*factory, 8.0, 8.0));
  auto gc_true =
      factory->createGeometryCollection(std::move(all_int_with_empty));
  EXPECT_TRUE(pred.AllTestComponentsInTargetInterior(gc_true.get()));

  std::vector<std::unique_ptr<Geometry>> int_empty_ext;
  int_empty_ext.push_back(MakePoint(*factory, 1.0, 1.0));
  int_empty_ext.push_back(factory->createPoint());
  int_empty_ext.push_back(MakePoint(*factory, 5.0, 5.0));
  auto gc_false = factory->createGeometryCollection(std::move(int_empty_ext));
  EXPECT_FALSE(pred.AllTestComponentsInTargetInterior(gc_false.get()));
}

TEST(PreparedPolygonPredicateTest, IsAnyTestComponentInTarget) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto target = MakeSquareWithHole(*factory);
  PreparedPolygon prep(target.get());
  TestablePreparedPolygonPredicate pred(&prep);

  auto empty_pt = factory->createPoint();
  auto empty_gc = factory->createGeometryCollection();
  EXPECT_FALSE(pred.AnyTestComponentInTarget(empty_pt.get()));
  EXPECT_FALSE(pred.AnyTestComponentInTarget(empty_gc.get()));

  // All exterior (both outside outer shell and inside hole) -> false.
  auto all_ext = MakeMultiPoint(*factory, {{-1, -1}, {5, 5}, {12, 0}});
  EXPECT_FALSE(pred.AnyTestComponentInTarget(all_ext.get()));

  // Boundary counts as in target (not EXTERIOR).
  auto hole_bnd_pt = MakePoint(*factory, 3.0, 5.0);
  EXPECT_TRUE(pred.AnyTestComponentInTarget(hole_bnd_pt.get()));

  // Exterior components first (isDone=false), then boundary (isDone=true skips
  // trailing interior component).
  auto ext_then_bnd =
      MakeMultiPoint(*factory, {{-2, -2}, {5, 5}, {5, 0}, {1, 1}});
  EXPECT_TRUE(pred.AnyTestComponentInTarget(ext_then_bnd.get()));

  // Empty component skipped mid-traversal before interior hit.
  std::vector<std::unique_ptr<Geometry>> ext_empty_int;
  ext_empty_int.push_back(MakePoint(*factory, -1.0, -1.0));
  ext_empty_int.push_back(factory->createPoint());
  ext_empty_int.push_back(MakePoint(*factory, 2.0, 2.0));
  auto gc = factory->createGeometryCollection(std::move(ext_empty_int));
  EXPECT_TRUE(pred.AnyTestComponentInTarget(gc.get()));
}

TEST(PreparedPolygonPredicateTest, IsAnyTestComponentInTargetInterior) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto target = MakeSquareWithHole(*factory);
  PreparedPolygon prep(target.get());
  TestablePreparedPolygonPredicate pred(&prep);

  auto empty_pt = factory->createPoint();
  auto empty_line = factory->createLineString();
  auto empty_gc = factory->createGeometryCollection();
  EXPECT_FALSE(pred.AnyTestComponentInTargetInterior(empty_pt.get()));
  EXPECT_FALSE(pred.AnyTestComponentInTargetInterior(empty_line.get()));
  EXPECT_FALSE(pred.AnyTestComponentInTargetInterior(empty_gc.get()));

  // Boundary and exterior components only -> false.
  auto bnd_and_ext =
      MakeMultiPoint(*factory, {{0, 0}, {-1, -1}, {3, 3}, {5, 5}});
  EXPECT_FALSE(pred.AnyTestComponentInTargetInterior(bnd_and_ext.get()));

  // Boundary and exterior first (isDone=false), then interior (isDone=true
  // stops before trailing boundary point).
  auto bnd_ext_int_bnd =
      MakeMultiPoint(*factory, {{0, 0}, {5, 5}, {1, 1}, {10, 10}});
  EXPECT_TRUE(pred.AnyTestComponentInTargetInterior(bnd_ext_int_bnd.get()));

  // Collection with empty element before interior hit.
  std::vector<std::unique_ptr<Geometry>> bnd_empty_int;
  bnd_empty_int.push_back(MakePoint(*factory, 0.0, 0.0));
  bnd_empty_int.push_back(factory->createPoint());
  bnd_empty_int.push_back(MakePoint(*factory, 8.0, 8.0));
  auto gc = factory->createGeometryCollection(std::move(bnd_empty_int));
  EXPECT_TRUE(pred.AnyTestComponentInTargetInterior(gc.get()));
}

TEST(PreparedPolygonPredicateTest, IsAnyTargetComponentInAreaTest) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Target with two components (shell + hole) -> 2 representative points:
  // (0, 0) on shell and (3, 3) on hole.
  auto target = MakeSquareWithHole(*factory);
  PreparedPolygon prep(target.get());
  TestablePreparedPolygonPredicate pred(&prep);

  // Empty representative points vector -> false.
  auto test_area = MakeBoxPolygon(*factory, 2.0, 2.0, 8.0, 8.0);
  const std::vector<const CoordinateXY*> empty_rep_pts;
  EXPECT_FALSE(
      pred.AnyTargetComponentInAreaTest(test_area.get(), &empty_rep_pts));

  // First rep point (0, 0) is outside [2, 8]^2, second rep point (3, 3) is
  // inside [2, 8]^2 -> iterates past first and returns true on second.
  EXPECT_TRUE(pred.AnyTargetComponentInAreaTest(
      test_area.get(), prep.getRepresentativePoints()));

  // Rep point on boundary of test_area -> BOUNDARY != EXTERIOR -> true.
  auto boundary_area = MakeBoxPolygon(*factory, 3.0, 3.0, 4.0, 4.0);
  EXPECT_TRUE(pred.AnyTargetComponentInAreaTest(
      boundary_area.get(), prep.getRepresentativePoints()));

  // Disjoint test area -> all rep points in EXTERIOR -> false.
  auto disjoint_area = MakeBoxPolygon(*factory, 20.0, 20.0, 30.0, 30.0);
  EXPECT_FALSE(pred.AnyTargetComponentInAreaTest(
      disjoint_area.get(), prep.getRepresentativePoints()));

  // Test area with a hole containing target's representative points:
  // target rep points (0, 0) and (3, 3) lie inside test_area's hole [-5, 5]^2.
  auto ring_shell = MakeRing(
      *factory, {{-10, -10}, {10, -10}, {10, 10}, {-10, 10}, {-10, -10}});
  std::vector<std::unique_ptr<LinearRing>> ring_holes;
  ring_holes.push_back(
      MakeRing(*factory, {{-5, -5}, {5, -5}, {5, 5}, {-5, 5}, {-5, -5}}));
  auto annulus =
      factory->createPolygon(std::move(ring_shell), std::move(ring_holes));
  EXPECT_FALSE(pred.AnyTargetComponentInAreaTest(
      annulus.get(), prep.getRepresentativePoints()));

  // MultiPolygon test geometry where second sub-polygon contains target rep pt.
  std::vector<std::unique_ptr<Polygon>> test_polys;
  test_polys.push_back(MakeBoxPolygon(*factory, 50.0, 50.0, 60.0, 60.0));
  test_polys.push_back(MakeBoxPolygon(*factory, 2.5, 2.5, 4.0, 4.0));
  auto multi_test = factory->createMultiPolygon(std::move(test_polys));
  EXPECT_TRUE(pred.AnyTargetComponentInAreaTest(
      multi_test.get(), prep.getRepresentativePoints()));
}

TEST(PreparedPolygonPredicateTest, EmptyTargetPolygonBehavesConsistently) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto empty_target = factory->createPolygon();
  PreparedPolygon prep(empty_target.get());
  TestablePreparedPolygonPredicate pred(&prep);

  auto pt = MakePoint(*factory, 1.0, 1.0);
  auto empty_pt = factory->createPoint();
  auto area = MakeBoxPolygon(*factory, 0.0, 0.0, 5.0, 5.0);

  EXPECT_EQ(pred.OutermostTestComponentLocation(pt.get()), Location::EXTERIOR);
  EXPECT_EQ(pred.OutermostTestComponentLocation(empty_pt.get()),
            Location::NONE);
  EXPECT_FALSE(pred.AllTestComponentsInTargetInterior(pt.get()));
  EXPECT_TRUE(pred.AllTestComponentsInTargetInterior(empty_pt.get()));
  EXPECT_FALSE(pred.AnyTestComponentInTarget(pt.get()));
  EXPECT_FALSE(pred.AnyTestComponentInTargetInterior(pt.get()));
  EXPECT_FALSE(pred.AnyTargetComponentInAreaTest(
      area.get(), prep.getRepresentativePoints()));
}

TEST(PreparedPolygonPredicateTest,
     PublicPreparedPolygonPredicatesExercisePredicateHelpers) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Non-rectangular polygon (with hole) bypasses Rectangle* shortcuts.
  auto target = MakeSquareWithHole(*factory);
  PreparedPolygon prep(target.get());

  auto bnd_only_mp = MakeMultiPoint(*factory, {{0, 0}, {5, 0}, {3, 3}});
  auto bnd_and_int_mp = MakeMultiPoint(*factory, {{0, 0}, {1, 1}, {5, 0}});
  auto interior_box = MakeBoxPolygon(*factory, 0.5, 0.5, 2.5, 2.5);
  auto surrounding_box = MakeBoxPolygon(*factory, -5.0, -5.0, 15.0, 15.0);
  auto disjoint_box = MakeBoxPolygon(*factory, 20.0, 20.0, 25.0, 25.0);

  // MultiPoint solely on boundary: covered, not contained, not properly
  // contained, intersects.
  EXPECT_FALSE(prep.contains(bnd_only_mp.get()));
  EXPECT_TRUE(prep.covers(bnd_only_mp.get()));
  EXPECT_FALSE(prep.containsProperly(bnd_only_mp.get()));
  EXPECT_TRUE(prep.intersects(bnd_only_mp.get()));

  // MultiPoint with boundary + interior: contained and covered, not properly
  // contained.
  EXPECT_TRUE(prep.contains(bnd_and_int_mp.get()));
  EXPECT_TRUE(prep.covers(bnd_and_int_mp.get()));
  EXPECT_FALSE(prep.containsProperly(bnd_and_int_mp.get()));

  // Strictly interior polygon.
  EXPECT_TRUE(prep.contains(interior_box.get()));
  EXPECT_TRUE(prep.containsProperly(interior_box.get()));
  EXPECT_TRUE(prep.intersects(interior_box.get()));

  // Surrounding polygon contains target's representative points.
  EXPECT_TRUE(prep.intersects(surrounding_box.get()));
  EXPECT_FALSE(prep.contains(surrounding_box.get()));
  EXPECT_FALSE(prep.intersects(disjoint_box.get()));
}

}  // namespace
}  // namespace geos_google
