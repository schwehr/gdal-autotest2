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

// Unit tests for third_party/geos/src/index/strtree/GeometryItemDistance.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/strtree/GeometryItemDistance.cpp

#include "third_party/geos/include/geos/index/strtree/GeometryItemDistance.h"

#include <memory>
#include <utility>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/index/strtree/ItemBoundable.h"
#include "third_party/geos/include/geos/index/strtree/ItemDistance.h"
#include "third_party/geos/include/geos/index/strtree/STRtree.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::index::strtree::GeometryItemDistance;
using geos::index::strtree::ItemBoundable;
using geos::index::strtree::ItemDistance;
using geos::index::strtree::STRtree;

ItemBoundable MakeBoundable(const Geometry* geom) {
  return ItemBoundable(geom->getEnvelopeInternal(),
                       const_cast<Geometry*>(geom));
}

std::unique_ptr<Polygon> MakeBox(const GeometryFactory& factory, double minx,
                                 double miny, double maxx, double maxy) {
  CoordinateSequence seq{
      CoordinateXY(minx, miny), CoordinateXY(maxx, miny),
      CoordinateXY(maxx, maxy), CoordinateXY(minx, maxy),
      CoordinateXY(minx, miny),
  };
  return factory.createPolygon(std::move(seq));
}

TEST(GeometryItemDistanceTest, ComputesPointToPointDistance) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> p1 = factory->createPoint(CoordinateXY(0.0, 0.0));
  std::unique_ptr<Point> p2 = factory->createPoint(CoordinateXY(3.0, 4.0));
  std::unique_ptr<Point> p_dup = factory->createPoint(CoordinateXY(3.0, 4.0));

  ItemBoundable b1 = MakeBoundable(p1.get());
  ItemBoundable b2 = MakeBoundable(p2.get());
  ItemBoundable b_dup = MakeBoundable(p_dup.get());

  GeometryItemDistance metric;
  EXPECT_DOUBLE_EQ(metric.distance(&b1, &b2), 5.0);
  EXPECT_DOUBLE_EQ(metric.distance(&b2, &b1), 5.0);
  EXPECT_DOUBLE_EQ(metric.distance(&b2, &b_dup), 0.0);
  EXPECT_DOUBLE_EQ(metric.distance(&b1, &b1), 0.0);
}

TEST(GeometryItemDistanceTest, SupportsPolymorphicItemDistanceCall) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> p1 = factory->createPoint(CoordinateXY(-1.0, -2.0));
  std::unique_ptr<Point> p2 = factory->createPoint(CoordinateXY(5.0, 6.0));

  ItemBoundable b1 = MakeBoundable(p1.get());
  ItemBoundable b2 = MakeBoundable(p2.get());

  std::unique_ptr<ItemDistance> metric =
      std::make_unique<GeometryItemDistance>();
  EXPECT_DOUBLE_EQ(metric->distance(&b1, &b2), 10.0);
}

TEST(GeometryItemDistanceTest, ComputesDistancesBetweenMixedGeometryTypes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> pt_above =
      factory->createPoint(CoordinateXY(5.0, 3.0));
  std::unique_ptr<LineString> line_h1 = factory->createLineString(
      CoordinateSequence{CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0)});
  std::unique_ptr<LineString> line_h2 = factory->createLineString(
      CoordinateSequence{CoordinateXY(0.0, 5.0), CoordinateXY(10.0, 5.0)});
  std::unique_ptr<LineString> line_cross = factory->createLineString(
      CoordinateSequence{CoordinateXY(2.0, -2.0), CoordinateXY(2.0, 2.0)});

  std::unique_ptr<Polygon> box1 = MakeBox(*factory, 0.0, 0.0, 10.0, 10.0);
  std::unique_ptr<Polygon> box2 = MakeBox(*factory, 13.0, 14.0, 15.0, 16.0);
  std::unique_ptr<Point> pt_inside =
      factory->createPoint(CoordinateXY(5.0, 5.0));
  std::unique_ptr<Point> pt_boundary =
      factory->createPoint(CoordinateXY(0.0, 5.0));
  std::unique_ptr<Point> pt_outside =
      factory->createPoint(CoordinateXY(15.0, 5.0));

  ItemBoundable b_pt_above = MakeBoundable(pt_above.get());
  ItemBoundable b_line_h1 = MakeBoundable(line_h1.get());
  ItemBoundable b_line_h2 = MakeBoundable(line_h2.get());
  ItemBoundable b_line_cross = MakeBoundable(line_cross.get());
  ItemBoundable b_box1 = MakeBoundable(box1.get());
  ItemBoundable b_box2 = MakeBoundable(box2.get());
  ItemBoundable b_pt_inside = MakeBoundable(pt_inside.get());
  ItemBoundable b_pt_boundary = MakeBoundable(pt_boundary.get());
  ItemBoundable b_pt_outside = MakeBoundable(pt_outside.get());

  GeometryItemDistance metric;
  EXPECT_DOUBLE_EQ(metric.distance(&b_pt_above, &b_line_h1), 3.0);
  EXPECT_DOUBLE_EQ(metric.distance(&b_line_h1, &b_line_h2), 5.0);
  EXPECT_DOUBLE_EQ(metric.distance(&b_line_h1, &b_line_cross), 0.0);
  EXPECT_DOUBLE_EQ(metric.distance(&b_pt_inside, &b_box1), 0.0);
  EXPECT_DOUBLE_EQ(metric.distance(&b_pt_boundary, &b_box1), 0.0);
  EXPECT_DOUBLE_EQ(metric.distance(&b_pt_outside, &b_box1), 5.0);
  EXPECT_DOUBLE_EQ(metric.distance(&b_box1, &b_box2), 5.0);
}

TEST(GeometryItemDistanceTest, EmptyGeometriesReturnZeroDistance) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> empty_pt = factory->createPoint();
  std::unique_ptr<Polygon> empty_poly = factory->createPolygon();
  std::unique_ptr<Point> valid_pt =
      factory->createPoint(CoordinateXY(3.0, 4.0));

  ItemBoundable b_empty_pt = MakeBoundable(empty_pt.get());
  ItemBoundable b_empty_poly = MakeBoundable(empty_poly.get());
  ItemBoundable b_valid_pt = MakeBoundable(valid_pt.get());

  GeometryItemDistance metric;
  EXPECT_DOUBLE_EQ(metric.distance(&b_empty_pt, &b_valid_pt), 0.0);
  EXPECT_DOUBLE_EQ(metric.distance(&b_valid_pt, &b_empty_pt), 0.0);
  EXPECT_DOUBLE_EQ(metric.distance(&b_empty_pt, &b_empty_poly), 0.0);
}

TEST(GeometryItemDistanceTest, IntegratesWithStrTreeNearestNeighbour) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Polygon> box_far = MakeBox(*factory, 20.0, 20.0, 22.0, 22.0);
  std::unique_ptr<LineString> seg_near = factory->createLineString(
      CoordinateSequence{CoordinateXY(4.0, 0.0), CoordinateXY(4.0, 10.0)});
  std::unique_ptr<Point> pt_far =
      factory->createPoint(CoordinateXY(-10.0, -10.0));
  std::unique_ptr<Point> pt_query =
      factory->createPoint(CoordinateXY(1.0, 5.0));

  STRtree tree1(2);
  tree1.insert(box_far->getEnvelopeInternal(), box_far.get());
  tree1.insert(seg_near->getEnvelopeInternal(), seg_near.get());
  tree1.insert(pt_far->getEnvelopeInternal(), pt_far.get());
  tree1.build();

  GeometryItemDistance metric;
  const void* nearest_to_query = tree1.nearestNeighbour(
      pt_query->getEnvelopeInternal(), pt_query.get(), &metric);
  EXPECT_EQ(nearest_to_query, seg_near.get());

  std::unique_ptr<Point> pt_other =
      factory->createPoint(CoordinateXY(5.5, 5.0));
  std::unique_ptr<Point> pt_remote =
      factory->createPoint(CoordinateXY(100.0, 100.0));
  STRtree tree2(2);
  tree2.insert(pt_other->getEnvelopeInternal(), pt_other.get());
  tree2.insert(pt_remote->getEnvelopeInternal(), pt_remote.get());
  tree2.build();

  const std::pair<const void*, const void*> closest_pair =
      tree1.nearestNeighbour(&tree2, &metric);
  EXPECT_EQ(closest_pair.first, seg_near.get());
  EXPECT_EQ(closest_pair.second, pt_other.get());
}

}  // namespace
}  // namespace geos_google
