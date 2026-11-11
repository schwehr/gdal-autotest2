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
// third_party/geos/src/geom/util/ShortCircuitedGeometryVisitor.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/geom/util/ShortCircuitedGeometryVisitor.cpp

#include "third_party/geos/include/geos/geom/util/ShortCircuitedGeometryVisitor.h"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"

namespace geos_google {
namespace {

using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::geom::Geometry;
using ::geos::geom::GeometryCollection;
using ::geos::geom::GeometryFactory;
using ::geos::geom::LineString;
using ::geos::geom::MultiPoint;
using ::geos::geom::MultiPolygon;
using ::geos::geom::Point;
using ::geos::geom::Polygon;
using ::geos::geom::util::ShortCircuitedGeometryVisitor;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

class RecordingVisitor final : public ShortCircuitedGeometryVisitor {
 public:
  explicit RecordingVisitor(const Geometry* stop_target = nullptr)
      : stop_target_(stop_target) {}

  const std::vector<const Geometry*>& visited() const { return visited_; }
  int is_done_calls() const { return is_done_calls_; }

 protected:
  void visit(const Geometry& element) override {
    visited_.push_back(&element);
    if (&element == stop_target_) {
      reached_stop_ = true;
    }
  }

  bool isDone() override {
    ++is_done_calls_;
    return reached_stop_;
  }

 private:
  const Geometry* stop_target_;
  std::vector<const Geometry*> visited_;
  int is_done_calls_ = 0;
  bool reached_stop_ = false;
};

// GeometryCollection subclass returning caller-provided pointers (including
// nullptr) from getGeometryN() without passing null elements to the
// GeometryCollection constructor.
class NullElementCollection final : public GeometryCollection {
 public:
  NullElementCollection(const GeometryFactory& factory,
                        std::vector<const Geometry*> elements)
      : GeometryCollection(std::vector<std::unique_ptr<Geometry>>{}, factory),
        elements_(std::move(elements)) {}

  ~NullElementCollection() override = default;

  std::size_t getNumGeometries() const override { return elements_.size(); }

  const Geometry* getGeometryN(std::size_t n) const override {
    return elements_[n];
  }

 private:
  std::vector<const Geometry*> elements_;
};

std::unique_ptr<LineString> MakeLine(const GeometryFactory& factory, double x0,
                                     double y0, double x1, double y1) {
  CoordinateSequence seq{CoordinateXY(x0, y0), CoordinateXY(x1, y1)};
  return factory.createLineString(seq);
}

std::unique_ptr<Polygon> MakeSquare(const GeometryFactory& factory, double x,
                                    double y, double size) {
  CoordinateSequence ring{
      CoordinateXY(x, y),
      CoordinateXY(x + size, y),
      CoordinateXY(x + size, y + size),
      CoordinateXY(x, y + size),
      CoordinateXY(x, y),
  };
  return factory.createPolygon(std::move(ring));
}

TEST(ShortCircuitedGeometryVisitorTest, VisitsSingleAtomicGeometry) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> point = factory->createPoint(CoordinateXY(1.0, 2.0));
  std::unique_ptr<LineString> line = MakeLine(*factory, 0.0, 0.0, 3.0, 4.0);
  std::unique_ptr<Polygon> poly = MakeSquare(*factory, 0.0, 0.0, 2.0);

  {
    RecordingVisitor visitor;
    visitor.applyTo(*point);
    EXPECT_THAT(visitor.visited(), ElementsAre(point.get()));
    EXPECT_EQ(visitor.is_done_calls(), 1);
  }
  {
    RecordingVisitor visitor;
    visitor.applyTo(*line);
    EXPECT_THAT(visitor.visited(), ElementsAre(line.get()));
    EXPECT_EQ(visitor.is_done_calls(), 1);
  }
  {
    RecordingVisitor visitor(poly.get());
    visitor.applyTo(*poly);
    EXPECT_THAT(visitor.visited(), ElementsAre(poly.get()));
    EXPECT_EQ(visitor.is_done_calls(), 1);
  }
}

TEST(ShortCircuitedGeometryVisitorTest, VisitsEmptyAtomicGeometry) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> empty_point = factory->createPoint();

  RecordingVisitor visitor;
  visitor.applyTo(*empty_point);
  EXPECT_THAT(visitor.visited(), ElementsAre(empty_point.get()));
  EXPECT_EQ(visitor.is_done_calls(), 1);
}

TEST(ShortCircuitedGeometryVisitorTest, SkipsEmptyCollections) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<GeometryCollection> empty_gc =
      factory->createGeometryCollection();
  std::unique_ptr<MultiPoint> empty_mp = factory->createMultiPoint();
  std::unique_ptr<MultiPolygon> empty_mpoly = factory->createMultiPolygon();

  {
    RecordingVisitor visitor;
    visitor.applyTo(*empty_gc);
    EXPECT_THAT(visitor.visited(), IsEmpty());
    EXPECT_EQ(visitor.is_done_calls(), 0);
  }
  {
    RecordingVisitor visitor;
    visitor.applyTo(*empty_mp);
    EXPECT_THAT(visitor.visited(), IsEmpty());
    EXPECT_EQ(visitor.is_done_calls(), 0);
  }
  {
    RecordingVisitor visitor;
    visitor.applyTo(*empty_mpoly);
    EXPECT_THAT(visitor.visited(), IsEmpty());
    EXPECT_EQ(visitor.is_done_calls(), 0);
  }
}

TEST(ShortCircuitedGeometryVisitorTest, VisitsAllElementsInFlatCollection) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::vector<std::unique_ptr<Geometry>> geoms;
  geoms.push_back(factory->createPoint(CoordinateXY(0.0, 0.0)));
  geoms.push_back(MakeLine(*factory, 1.0, 1.0, 2.0, 2.0));
  geoms.push_back(MakeSquare(*factory, 3.0, 3.0, 1.0));
  const Geometry* g0 = geoms[0].get();
  const Geometry* g1 = geoms[1].get();
  const Geometry* g2 = geoms[2].get();

  std::unique_ptr<GeometryCollection> gc =
      factory->createGeometryCollection(std::move(geoms));

  RecordingVisitor visitor;
  visitor.applyTo(*gc);
  EXPECT_THAT(visitor.visited(), ElementsAre(g0, g1, g2));
  EXPECT_EQ(visitor.is_done_calls(), 3);
}

TEST(ShortCircuitedGeometryVisitorTest,
     ShortCircuitsAtFirstMiddleAndLastElement) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  std::vector<std::unique_ptr<Geometry>> points;
  points.push_back(factory->createPoint(CoordinateXY(1.0, 0.0)));
  points.push_back(factory->createPoint(CoordinateXY(2.0, 0.0)));
  points.push_back(factory->createPoint(CoordinateXY(3.0, 0.0)));
  const Geometry* p0 = points[0].get();
  const Geometry* p1 = points[1].get();
  const Geometry* p2 = points[2].get();

  std::unique_ptr<MultiPoint> mp = factory->createMultiPoint(std::move(points));

  {
    RecordingVisitor visitor(p0);
    visitor.applyTo(*mp);
    EXPECT_THAT(visitor.visited(), ElementsAre(p0));
    EXPECT_EQ(visitor.is_done_calls(), 1);
  }
  {
    RecordingVisitor visitor(p1);
    visitor.applyTo(*mp);
    EXPECT_THAT(visitor.visited(), ElementsAre(p0, p1));
    EXPECT_EQ(visitor.is_done_calls(), 2);
  }
  {
    RecordingVisitor visitor(p2);
    visitor.applyTo(*mp);
    EXPECT_THAT(visitor.visited(), ElementsAre(p0, p1, p2));
    EXPECT_EQ(visitor.is_done_calls(), 3);
  }
}

TEST(ShortCircuitedGeometryVisitorTest,
     RecursesIntoNestedCollectionsAndSkipsContainers) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto p0 = factory->createPoint(CoordinateXY(0.0, 0.0));
  auto line1 = MakeLine(*factory, 1.0, 0.0, 2.0, 0.0);
  auto poly2 = MakeSquare(*factory, 10.0, 0.0, 1.0);
  auto poly3 = MakeSquare(*factory, 20.0, 0.0, 1.0);
  auto p4 = factory->createPoint(CoordinateXY(30.0, 0.0));

  const Geometry* expected0 = p0.get();
  const Geometry* expected1 = line1.get();
  const Geometry* expected2 = poly2.get();
  const Geometry* expected3 = poly3.get();
  const Geometry* expected4 = p4.get();

  std::vector<std::unique_ptr<Polygon>> polys;
  polys.push_back(std::move(poly2));
  polys.push_back(std::move(poly3));
  auto multi_poly = factory->createMultiPolygon(std::move(polys));

  std::vector<std::unique_ptr<Geometry>> inner_geoms;
  inner_geoms.push_back(std::move(line1));
  inner_geoms.push_back(std::move(multi_poly));
  auto inner_gc = factory->createGeometryCollection(std::move(inner_geoms));

  std::vector<std::unique_ptr<Geometry>> outer_geoms;
  outer_geoms.push_back(std::move(p0));
  outer_geoms.push_back(factory->createGeometryCollection());
  outer_geoms.push_back(std::move(inner_gc));
  outer_geoms.push_back(std::move(p4));
  auto outer_gc = factory->createGeometryCollection(std::move(outer_geoms));

  RecordingVisitor visitor;
  visitor.applyTo(*outer_gc);
  EXPECT_THAT(visitor.visited(), ElementsAre(expected0, expected1, expected2,
                                             expected3, expected4));
  EXPECT_EQ(visitor.is_done_calls(), 5);
}

TEST(ShortCircuitedGeometryVisitorTest, ShortCircuitsOutOfNestedCollections) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto p0 = factory->createPoint(CoordinateXY(0.0, 0.0));
  auto line1 = MakeLine(*factory, 1.0, 0.0, 2.0, 0.0);
  auto poly2 = MakeSquare(*factory, 10.0, 0.0, 1.0);
  auto poly3 = MakeSquare(*factory, 20.0, 0.0, 1.0);
  auto p4 = factory->createPoint(CoordinateXY(30.0, 0.0));

  const Geometry* expected0 = p0.get();
  const Geometry* expected1 = line1.get();
  const Geometry* stop_target = poly2.get();

  std::vector<std::unique_ptr<Polygon>> polys;
  polys.push_back(std::move(poly2));
  polys.push_back(std::move(poly3));
  auto multi_poly = factory->createMultiPolygon(std::move(polys));

  std::vector<std::unique_ptr<Geometry>> inner_geoms;
  inner_geoms.push_back(std::move(line1));
  inner_geoms.push_back(std::move(multi_poly));
  auto inner_gc = factory->createGeometryCollection(std::move(inner_geoms));

  std::vector<std::unique_ptr<Geometry>> outer_geoms;
  outer_geoms.push_back(std::move(p0));
  outer_geoms.push_back(std::move(inner_gc));
  outer_geoms.push_back(std::move(p4));
  auto outer_gc = factory->createGeometryCollection(std::move(outer_geoms));

  RecordingVisitor visitor(stop_target);
  visitor.applyTo(*outer_gc);
  EXPECT_THAT(visitor.visited(),
              ElementsAre(expected0, expected1, stop_target));
  EXPECT_EQ(visitor.is_done_calls(), 3);
}

TEST(ShortCircuitedGeometryVisitorTest, SkipsNullElementsReturnedByCollection) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto p0 = factory->createPoint(CoordinateXY(1.0, 1.0));
  auto p1 = factory->createPoint(CoordinateXY(2.0, 2.0));

  {
    NullElementCollection only_nulls(*factory, {nullptr});
    RecordingVisitor visitor;
    visitor.applyTo(only_nulls);
    EXPECT_THAT(visitor.visited(), IsEmpty());
    EXPECT_EQ(visitor.is_done_calls(), 0);
  }
  {
    NullElementCollection mixed_nulls(
        *factory, {nullptr, p0.get(), nullptr, p1.get(), nullptr});
    RecordingVisitor visitor;
    visitor.applyTo(mixed_nulls);
    EXPECT_THAT(visitor.visited(), ElementsAre(p0.get(), p1.get()));
    EXPECT_EQ(visitor.is_done_calls(), 2);
  }
  {
    NullElementCollection short_circuit_after_null(
        *factory, {nullptr, p0.get(), p1.get()});
    RecordingVisitor visitor(p0.get());
    visitor.applyTo(short_circuit_after_null);
    EXPECT_THAT(visitor.visited(), ElementsAre(p0.get()));
    EXPECT_EQ(visitor.is_done_calls(), 1);
  }
}

TEST(ShortCircuitedGeometryVisitorTest,
     StickyDoneStopsImmediatelyAfterEmptyNestedCollection) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto p0 = factory->createPoint(CoordinateXY(0.0, 0.0));
  auto p1 = factory->createPoint(CoordinateXY(1.0, 1.0));
  auto p2 = factory->createPoint(CoordinateXY(2.0, 2.0));

  const Geometry* p0_ptr = p0.get();

  std::vector<std::unique_ptr<Geometry>> first_geoms;
  first_geoms.push_back(std::move(p0));
  first_geoms.push_back(std::move(p1));
  auto first_gc = factory->createGeometryCollection(std::move(first_geoms));

  std::vector<std::unique_ptr<Geometry>> second_geoms;
  second_geoms.push_back(factory->createGeometryCollection());
  second_geoms.push_back(std::move(p2));
  auto second_gc = factory->createGeometryCollection(std::move(second_geoms));

  RecordingVisitor visitor(p0_ptr);
  visitor.applyTo(*first_gc);
  ASSERT_THAT(visitor.visited(), ElementsAre(p0_ptr));

  // With done already true, recursing into an empty nested collection exits
  // at the outer done check before visiting subsequent elements.
  visitor.applyTo(*second_gc);
  EXPECT_THAT(visitor.visited(), ElementsAre(p0_ptr));
  EXPECT_EQ(visitor.is_done_calls(), 1);
}

}  // namespace
}  // namespace geos_google
