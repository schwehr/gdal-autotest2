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

// Unit tests for third_party/geos/src/index/strtree/SimpleSTRnode.cpp.

#include "third_party/geos/include/geos/index/strtree/SimpleSTRnode.h"

#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Envelope.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::index::strtree::SimpleSTRnode;
using ::testing::ElementsAre;
using ::testing::EndsWith;
using ::testing::IsEmpty;
using ::testing::StartsWith;

TEST(SimpleSTRnodeTest, ConstructorsAndBasicAccessors) {
  SimpleSTRnode empty_composite(2);
  EXPECT_EQ(empty_composite.getLevel(), 2U);
  EXPECT_EQ(empty_composite.getItem(), nullptr);
  EXPECT_TRUE(empty_composite.getEnvelope().isNull());
  EXPECT_EQ(empty_composite.getBounds(), &empty_composite.getEnvelope());
  EXPECT_FALSE(empty_composite.isLeaf());
  EXPECT_TRUE(empty_composite.isComposite());
  EXPECT_EQ(empty_composite.size(), 0U);
  EXPECT_DOUBLE_EQ(empty_composite.area(), 0.0);
  EXPECT_THAT(empty_composite.getChildNodes(), IsEmpty());

  int item_value = 77;
  const Envelope leaf_env(1.0, 5.0, -2.0, 4.0);
  SimpleSTRnode leaf(0, &leaf_env, &item_value, 4);
  EXPECT_EQ(leaf.getLevel(), 0U);
  EXPECT_EQ(leaf.getItem(), &item_value);
  EXPECT_EQ(leaf.getEnvelope(), leaf_env);
  EXPECT_EQ(leaf.getBounds(), &leaf.getEnvelope());
  EXPECT_TRUE(leaf.isLeaf());
  EXPECT_FALSE(leaf.isComposite());
  EXPECT_EQ(leaf.size(), 0U);
  EXPECT_DOUBLE_EQ(leaf.area(), 24.0);
  EXPECT_THAT(leaf.getChildNodes(), IsEmpty());

  const Envelope point_env(CoordinateXY(3.0, -1.0));
  SimpleSTRnode point_leaf(0, &point_env, &item_value);
  EXPECT_DOUBLE_EQ(point_leaf.area(), 0.0);
  EXPECT_EQ(point_leaf.getEnvelope(), point_env);
}

TEST(SimpleSTRnodeTest, AddChildNodeInitializesAndExpandsBounds) {
  const Envelope e1(1.0, 3.0, 2.0, 5.0);
  const Envelope e2(-2.0, 2.0, 4.0, 8.0);
  const Envelope e_inside(0.0, 1.0, 3.0, 4.0);
  int item1 = 1;
  int item2 = 2;
  int item3 = 3;

  SimpleSTRnode leaf1(0, &e1, &item1);
  SimpleSTRnode leaf2(0, &e2, &item2);
  SimpleSTRnode leaf_inside(0, &e_inside, &item3);

  SimpleSTRnode parent(1);
  ASSERT_TRUE(parent.getEnvelope().isNull());

  // First child initializes bounds directly when parent bounds are null.
  parent.addChildNode(&leaf1);
  EXPECT_EQ(parent.size(), 1U);
  EXPECT_EQ(parent.getEnvelope(), e1);
  EXPECT_DOUBLE_EQ(parent.area(), 6.0);

  // Second child expands bounds to union [-2, 3] x [2, 8].
  parent.addChildNode(&leaf2);
  EXPECT_EQ(parent.size(), 2U);
  EXPECT_EQ(parent.getEnvelope(), Envelope(-2.0, 3.0, 2.0, 8.0));
  EXPECT_DOUBLE_EQ(parent.area(), 30.0);

  // Contained child leaves bounds unchanged.
  parent.addChildNode(&leaf_inside);
  EXPECT_EQ(parent.size(), 3U);
  EXPECT_EQ(parent.getEnvelope(), Envelope(-2.0, 3.0, 2.0, 8.0));
  EXPECT_THAT(parent.getChildNodes(),
              ElementsAre(&leaf1, &leaf2, &leaf_inside));

  // Composite constructed with explicit non-null envelope expands on 1st child.
  const Envelope seed_env(0.0, 1.0, 0.0, 1.0);
  SimpleSTRnode prebounded(1, &seed_env, nullptr);
  prebounded.addChildNode(&leaf1);
  EXPECT_EQ(prebounded.getEnvelope(), Envelope(0.0, 3.0, 0.0, 5.0));
}

TEST(SimpleSTRnodeTest, GetNumNodesAndGetNumLeafNodes) {
  int v1 = 1;
  int v2 = 2;
  int v3 = 3;
  const Envelope e1(0.0, 1.0, 0.0, 1.0);
  const Envelope e2(2.0, 3.0, 0.0, 1.0);
  const Envelope e3(4.0, 5.0, 0.0, 1.0);

  SimpleSTRnode leaf1(0, &e1, &v1);
  SimpleSTRnode leaf2(0, &e2, &v2);
  SimpleSTRnode leaf3(0, &e3, &v3);

  // Single leaf.
  EXPECT_EQ(leaf1.getNumNodes(), 1U);
  EXPECT_EQ(leaf1.getNumLeafNodes(), 1U);

  // Empty composite node has 1 node (itself) and 0 leaf nodes.
  SimpleSTRnode empty_mid(1);
  EXPECT_EQ(empty_mid.getNumNodes(), 1U);
  EXPECT_EQ(empty_mid.getNumLeafNodes(), 0U);

  SimpleSTRnode mid1(1);
  mid1.addChildNode(&leaf1);
  mid1.addChildNode(&leaf2);
  EXPECT_EQ(mid1.getNumNodes(), 3U);
  EXPECT_EQ(mid1.getNumLeafNodes(), 2U);

  SimpleSTRnode mid2(1);
  mid2.addChildNode(&leaf3);
  EXPECT_EQ(mid2.getNumNodes(), 2U);
  EXPECT_EQ(mid2.getNumLeafNodes(), 1U);

  SimpleSTRnode root(2);
  root.addChildNode(&mid1);
  root.addChildNode(&mid2);
  root.addChildNode(&empty_mid);

  // 1 root + 3 level-1 composites + 3 level-0 leaves = 7 nodes; 3 leaves.
  EXPECT_EQ(root.getNumNodes(), 7U);
  EXPECT_EQ(root.getNumLeafNodes(), 3U);
}

TEST(SimpleSTRnodeTest, RemoveItemRemovesMatchingChildAndHandlesMisses) {
  int item1 = 10;
  int item2 = 20;
  int item3 = 30;
  int missing = 40;
  const Envelope e1(0.0, 1.0, 0.0, 1.0);
  const Envelope e2(2.0, 3.0, 0.0, 1.0);
  const Envelope e3(4.0, 5.0, 0.0, 1.0);

  SimpleSTRnode parent(1);
  EXPECT_FALSE(parent.removeItem(&item1));

  SimpleSTRnode leaf1(0, &e1, &item1);
  SimpleSTRnode leaf2(0, &e2, &item2);
  SimpleSTRnode leaf3(0, &e3, &item3);
  parent.addChildNode(&leaf1);
  parent.addChildNode(&leaf2);
  parent.addChildNode(&leaf3);
  const Envelope original_bounds = parent.getEnvelope();

  EXPECT_FALSE(parent.removeItem(&missing));
  EXPECT_EQ(parent.size(), 3U);

  // Remove middle, then verify duplicate removal fails.
  EXPECT_TRUE(parent.removeItem(&item2));
  EXPECT_FALSE(parent.removeItem(&item2));
  EXPECT_THAT(parent.getChildNodes(), ElementsAre(&leaf1, &leaf3));

  // Bounds remain unchanged after removeItem.
  EXPECT_EQ(parent.getEnvelope(), original_bounds);

  // Remove first and last items.
  EXPECT_TRUE(parent.removeItem(&item1));
  EXPECT_TRUE(parent.removeItem(&item3));
  EXPECT_EQ(parent.size(), 0U);
  EXPECT_FALSE(parent.removeItem(&item1));

  // Composite child holds nullptr item; removing non-null item returns false.
  SimpleSTRnode child_comp(1);
  SimpleSTRnode root(2);
  root.addChildNode(&child_comp);
  EXPECT_FALSE(root.removeItem(&item1));
}

TEST(SimpleSTRnodeTest, RemoveChildRemovesMatchingNodeAndHandlesMisses) {
  int item1 = 1;
  int item2 = 2;
  int item3 = 3;
  const Envelope env(0.0, 1.0, 0.0, 1.0);
  SimpleSTRnode child1(0, &env, &item1);
  SimpleSTRnode child2(1);
  SimpleSTRnode child3(0, &env, &item2);
  SimpleSTRnode unrelated(0, &env, &item3);

  SimpleSTRnode parent(2);
  EXPECT_FALSE(parent.removeChild(&child1));

  parent.addChildNode(&child1);
  parent.addChildNode(&child2);
  parent.addChildNode(&child3);

  EXPECT_FALSE(parent.removeChild(&unrelated));
  EXPECT_EQ(parent.size(), 3U);

  EXPECT_TRUE(parent.removeChild(&child2));
  EXPECT_FALSE(parent.removeChild(&child2));
  EXPECT_THAT(parent.getChildNodes(), ElementsAre(&child1, &child3));

  EXPECT_TRUE(parent.removeChild(&child1));
  EXPECT_TRUE(parent.removeChild(&child3));
  EXPECT_THAT(parent.getChildNodes(), IsEmpty());
  EXPECT_FALSE(parent.removeChild(&child1));
}

TEST(SimpleSTRnodeTest, ToStringFormatsIndentationAndHierarchy) {
  int item1 = 1;
  int item2 = 2;
  const Envelope e1(0.0, 1.0, 0.0, 2.0);
  const Envelope e2(4.0, 5.0, 5.0, 6.0);
  SimpleSTRnode leaf1(0, &e1, &item1);
  SimpleSTRnode leaf2(0, &e2, &item2);

  {
    std::ostringstream oss;
    leaf1.toString(oss, 0);
    EXPECT_EQ(oss.str(), "Env[0:1,0:2] [0]\n");
  }
  {
    std::ostringstream oss;
    leaf1.toString(oss, 2);
    EXPECT_EQ(oss.str(), "    Env[0:1,0:2] [0]\n");
  }

  SimpleSTRnode mid(1);
  mid.addChildNode(&leaf1);
  mid.addChildNode(&leaf2);
  SimpleSTRnode root(2);
  root.addChildNode(&mid);

  std::ostringstream tree_oss;
  root.toString(tree_oss, 0);
  EXPECT_EQ(tree_oss.str(),
            "Env[0:5,0:6] [2]\n"
            "  Env[0:5,0:6] [1]\n"
            "    Env[0:1,0:2] [0]\n"
            "    Env[4:5,5:6] [0]\n");

  SimpleSTRnode empty_comp(1);
  std::ostringstream empty_oss;
  empty_comp.toString(empty_oss, 1);
  EXPECT_THAT(empty_oss.str(), StartsWith("  Env["));
  EXPECT_THAT(empty_oss.str(), EndsWith("] [1]\n"));
}

}  // namespace
}  // namespace geos_google
