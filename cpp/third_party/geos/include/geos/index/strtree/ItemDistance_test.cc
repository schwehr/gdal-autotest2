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

#include "third_party/geos/include/geos/index/strtree/ItemDistance.h"

#include <memory>
#include <utility>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/index/strtree/GeometryItemDistance.h"
#include "third_party/geos/include/geos/index/strtree/ItemBoundable.h"
#include "third_party/geos/include/geos/index/strtree/STRtree.h"
#include "third_party/geos/include/geos/index/strtree/SimpleSTRtree.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

class CoordinateItemDistance : public geos::index::strtree::ItemDistance {
 public:
  explicit CoordinateItemDistance(bool* destroyed = nullptr)
      : destroyed_(destroyed) {}

  ~CoordinateItemDistance() override {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
  }

  double distance(const geos::index::strtree::ItemBoundable* item1,
                  const geos::index::strtree::ItemBoundable* item2) override {
    if (item1 == nullptr || item2 == nullptr || item1->getItem() == nullptr ||
        item2->getItem() == nullptr) {
      throw geos::util::IllegalArgumentException(
          "ItemBoundable and underlying item must not be null");
    }
    const auto* c1 =
        static_cast<const geos::geom::CoordinateXY*>(item1->getItem());
    const auto* c2 =
        static_cast<const geos::geom::CoordinateXY*>(item2->getItem());
    return c1->distance(*c2);
  }

 private:
  bool* destroyed_;
};

class DistinctCoordinateItemDistance : public CoordinateItemDistance {
 public:
  double distance(const geos::index::strtree::ItemBoundable* item1,
                  const geos::index::strtree::ItemBoundable* item2) override {
    if (item1 != nullptr && item2 != nullptr &&
        item1->getItem() == item2->getItem()) {
      return geos::DoubleMax;
    }
    return CoordinateItemDistance::distance(item1, item2);
  }
};

TEST(ItemDistanceTest, PolymorphicDestructionAndDistance) {
  bool destroyed = false;
  std::unique_ptr<geos::index::strtree::ItemDistance> dist =
      std::make_unique<CoordinateItemDistance>(&destroyed);

  geos::geom::CoordinateXY c1(0.0, 0.0);
  geos::geom::CoordinateXY c2(3.0, 4.0);
  geos::geom::Envelope e1(c1);
  geos::geom::Envelope e2(c2);
  geos::index::strtree::ItemBoundable b1(&e1, &c1);
  geos::index::strtree::ItemBoundable b2(&e2, &c2);

  EXPECT_DOUBLE_EQ(dist->distance(&b1, &b2), 5.0);
  EXPECT_DOUBLE_EQ(dist->distance(&b1, &b1), 0.0);
  EXPECT_FALSE(destroyed);

  dist.reset();
  EXPECT_TRUE(destroyed);
}

TEST(ItemDistanceTest, StackDestructionAndIllegalArgumentException) {
  bool destroyed = false;
  geos::geom::CoordinateXY c1(1.0, 2.0);
  geos::geom::Envelope e1(c1);
  geos::index::strtree::ItemBoundable valid_boundable(&e1, &c1);
  geos::index::strtree::ItemBoundable null_item_boundable(&e1, nullptr);

  {
    CoordinateItemDistance dist(&destroyed);
    geos::index::strtree::ItemDistance& base_ref = dist;

    EXPECT_DOUBLE_EQ(base_ref.distance(&valid_boundable, &valid_boundable),
                     0.0);
    EXPECT_THROW(base_ref.distance(nullptr, &valid_boundable),
                 geos::util::IllegalArgumentException);
    EXPECT_THROW(base_ref.distance(&valid_boundable, nullptr),
                 geos::util::IllegalArgumentException);
    EXPECT_THROW(base_ref.distance(&valid_boundable, &null_item_boundable),
                 geos::util::IllegalArgumentException);
    EXPECT_FALSE(destroyed);
  }

  EXPECT_TRUE(destroyed);
}

TEST(ItemDistanceTest, UsedWithSTRtreeNearestNeighbour) {
  geos::geom::CoordinateXY p0(0.0, 0.0);
  geos::geom::CoordinateXY p1(10.0, 10.0);
  geos::geom::CoordinateXY p2(1.0, 1.0);
  geos::geom::CoordinateXY p3(10.0, 11.0);
  geos::geom::Envelope e0(p0);
  geos::geom::Envelope e1(p1);
  geos::geom::Envelope e2(p2);
  geos::geom::Envelope e3(p3);

  geos::index::strtree::STRtree tree(2);
  tree.insert(&e0, &p0);
  tree.insert(&e1, &p1);
  tree.insert(&e2, &p2);
  tree.insert(&e3, &p3);
  // STRtree::nearestNeighbour(ItemDistance*) and nearestNeighbour(STRtree*,
  // ...) read getRoot() directly without calling build(), leaving root
  // uninitialized until build() is called explicitly.
  tree.build();

  DistinctCoordinateItemDistance distinct_dist;
  std::pair<const void*, const void*> self_pair =
      tree.nearestNeighbour(&distinct_dist);
  const bool matched_closest_pair =
      (self_pair.first == &p1 && self_pair.second == &p3) ||
      (self_pair.first == &p3 && self_pair.second == &p1);
  EXPECT_TRUE(matched_closest_pair);

  CoordinateItemDistance dist;
  geos::geom::CoordinateXY query_pt(0.9, 1.1);
  geos::geom::Envelope query_env(query_pt);
  EXPECT_EQ(tree.nearestNeighbour(&query_env, &query_pt, &dist), &p2);

  geos::geom::CoordinateXY q0(10.2, 10.5);
  geos::geom::CoordinateXY q1(20.0, 20.0);
  geos::geom::Envelope qe0(q0);
  geos::geom::Envelope qe1(q1);
  geos::index::strtree::STRtree other(2);
  other.insert(&qe0, &q0);
  other.insert(&qe1, &q1);
  other.build();

  std::pair<const void*, const void*> cross_pair =
      tree.nearestNeighbour(&other, &dist);
  EXPECT_TRUE(cross_pair.first == &p1 || cross_pair.first == &p3);
  EXPECT_EQ(cross_pair.second, &q0);
}

TEST(ItemDistanceTest, GeometryItemDistanceThroughBasePointer) {
  auto factory = geos::geom::GeometryFactory::create();
  std::unique_ptr<geos::geom::Point> pt1 =
      factory->createPoint(geos::geom::CoordinateXY(1.0, 2.0));
  std::unique_ptr<geos::geom::Point> pt2 =
      factory->createPoint(geos::geom::CoordinateXY(4.0, 6.0));
  std::unique_ptr<geos::geom::Point> pt3 =
      factory->createPoint(geos::geom::CoordinateXY(20.0, 20.0));

  geos::index::strtree::ItemBoundable b1(
      pt1->getEnvelopeInternal(),
      static_cast<geos::geom::Geometry*>(pt1.get()));
  geos::index::strtree::ItemBoundable b2(
      pt2->getEnvelopeInternal(),
      static_cast<geos::geom::Geometry*>(pt2.get()));

  std::unique_ptr<geos::index::strtree::ItemDistance> dist =
      std::make_unique<geos::index::strtree::GeometryItemDistance>();
  EXPECT_DOUBLE_EQ(dist->distance(&b1, &b2), 5.0);
  EXPECT_DOUBLE_EQ(dist->distance(&b1, &b1), 0.0);

  geos::index::strtree::GeometryItemDistance stack_dist;
  geos::index::strtree::ItemDistance* base_ptr = &stack_dist;
  geos::index::strtree::SimpleSTRtree simple_tree(2);
  simple_tree.insert(pt1.get());
  simple_tree.insert(pt2.get());
  simple_tree.insert(pt3.get());

  std::unique_ptr<geos::geom::Point> query_pt =
      factory->createPoint(geos::geom::CoordinateXY(3.9, 6.1));
  EXPECT_EQ(simple_tree.nearestNeighbour(query_pt->getEnvelopeInternal(),
                                         query_pt.get(), base_ptr),
            static_cast<const void*>(pt2.get()));
}

}  // namespace
}  // namespace geos_google
