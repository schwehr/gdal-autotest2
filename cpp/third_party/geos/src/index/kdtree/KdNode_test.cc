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

#include "third_party/geos/include/geos/index/kdtree/KdNode.h"

#include <cmath>
#include <cstddef>
#include <limits>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::index::kdtree::KdNode;

TEST(KdNodeTest, ConstructFromXYAndData) {
  int payload = 42;
  KdNode node(3.5, -2.25, &payload);

  EXPECT_EQ(node.getX(), 3.5);
  EXPECT_EQ(node.getY(), -2.25);
  EXPECT_EQ(node.getCoordinate().x, 3.5);
  EXPECT_EQ(node.getCoordinate().y, -2.25);
  EXPECT_TRUE(std::isnan(node.getCoordinate().z));
  EXPECT_EQ(node.getData(), &payload);
  EXPECT_EQ(node.getLeft(), nullptr);
  EXPECT_EQ(node.getRight(), nullptr);
  EXPECT_EQ(node.getCount(), std::size_t{1});
  EXPECT_FALSE(node.isRepeated());
}

TEST(KdNodeTest, ConstructFromXYWithNullDataAndSpecialOrdinates) {
  KdNode zero_node(0.0, 0.0, nullptr);
  EXPECT_EQ(zero_node.getX(), 0.0);
  EXPECT_EQ(zero_node.getY(), 0.0);
  EXPECT_EQ(zero_node.getData(), nullptr);
  EXPECT_EQ(zero_node.getCount(), std::size_t{1});
  EXPECT_FALSE(zero_node.isRepeated());

  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  KdNode special_node(nan, inf, nullptr);
  EXPECT_TRUE(std::isnan(special_node.getX()));
  EXPECT_TRUE(std::isinf(special_node.getY()));
  EXPECT_EQ(special_node.getData(), nullptr);
}

TEST(KdNodeTest, ConstructFromCoordinateCopiesValueAndPreservesZ) {
  Coordinate coord(1.25, 2.5, 3.75);
  int payload = 99;
  KdNode node(coord, &payload);

  // Mutate the source coordinate to verify KdNode stores its own copy.
  coord.x = 100.0;
  coord.y = 200.0;
  coord.z = 300.0;

  EXPECT_EQ(node.getX(), 1.25);
  EXPECT_EQ(node.getY(), 2.5);
  EXPECT_TRUE(node.getCoordinate().equals3D(Coordinate(1.25, 2.5, 3.75)));
  EXPECT_EQ(node.getData(), &payload);
  EXPECT_EQ(node.getLeft(), nullptr);
  EXPECT_EQ(node.getRight(), nullptr);
  EXPECT_EQ(node.getCount(), std::size_t{1});
  EXPECT_FALSE(node.isRepeated());
}

TEST(KdNodeTest, IncrementTracksCountAndRepeatedState) {
  KdNode node(Coordinate(5.0, 6.0), nullptr);

  EXPECT_EQ(node.getCount(), std::size_t{1});
  EXPECT_FALSE(node.isRepeated());

  node.increment();
  EXPECT_EQ(node.getCount(), std::size_t{2});
  EXPECT_TRUE(node.isRepeated());

  node.increment();
  node.increment();
  EXPECT_EQ(node.getCount(), std::size_t{4});
  EXPECT_TRUE(node.isRepeated());
}

TEST(KdNodeTest, SetAndGetLeftAndRightChildren) {
  KdNode root(10.0, 10.0, nullptr);
  KdNode left_child(5.0, 12.0, nullptr);
  KdNode right_child(15.0, 8.0, nullptr);
  KdNode replacement_left(3.0, 14.0, nullptr);

  root.setLeft(&left_child);
  root.setRight(&right_child);
  EXPECT_EQ(root.getLeft(), &left_child);
  EXPECT_EQ(root.getRight(), &right_child);

  root.setLeft(&replacement_left);
  root.setRight(nullptr);
  EXPECT_EQ(root.getLeft(), &replacement_left);
  EXPECT_EQ(root.getRight(), nullptr);
}

}  // namespace
}  // namespace geos_google
