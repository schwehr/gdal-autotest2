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

// Unit tests for third_party/geos/src/index/VertexSequencePackedRtree.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/VertexSequencePackedRtree.cpp

#include "third_party/geos/include/geos/index/VertexSequencePackedRtree.h"

#include <cstddef>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Envelope;
using geos::index::VertexSequencePackedRtree;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

std::vector<std::size_t> BruteForceQuery(const CoordinateSequence& pts,
                                         const std::vector<bool>& removed,
                                         const Envelope& env) {
  std::vector<std::size_t> expected;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    if (!removed[i] && env.contains(pts[i])) {
      expected.push_back(i);
    }
  }
  return expected;
}

CoordinateSequence MakeClusteredSequence(std::size_t count) {
  CoordinateSequence pts = CoordinateSequence::XYZ(0);
  pts.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t leaf = i / 16;
    const std::size_t offset = i % 16;
    const double x =
        static_cast<double>(leaf) * 100.0 + static_cast<double>(offset) * 2.0;
    const double y = static_cast<double>(leaf) * 50.0 +
                     static_cast<double>(offset % 4) * 3.0;
    pts.add(Coordinate(x, y, static_cast<double>(i)));
  }
  return pts;
}

TEST(VertexSequencePackedRtreeTest, EmptySequenceProducesSingleNullBound) {
  const CoordinateSequence pts = CoordinateSequence::XYZ(0);
  VertexSequencePackedRtree tree(pts);

  const std::vector<Envelope> bounds = tree.getBounds();
  ASSERT_EQ(bounds.size(), 1u);
  EXPECT_TRUE(bounds[0].isNull());

  // Existing elements in result must be preserved when query finds nothing.
  std::vector<std::size_t> result = {99u};
  tree.query(Envelope(-100.0, 100.0, -100.0, 100.0), result);
  EXPECT_THAT(result, ElementsAre(99u));
}

TEST(VertexSequencePackedRtreeTest, SinglePointBoundsQueryAndRemove) {
  const CoordinateSequence pts{Coordinate(3.0, 4.0, 9.0)};
  VertexSequencePackedRtree tree(pts);

  std::vector<Envelope> bounds = tree.getBounds();
  ASSERT_EQ(bounds.size(), 2u);
  EXPECT_EQ(bounds[0], Envelope(3.0, 3.0, 4.0, 4.0));
  EXPECT_EQ(bounds[1], Envelope(3.0, 3.0, 4.0, 4.0));

  std::vector<std::size_t> result;
  tree.query(Envelope(3.0, 3.0, 4.0, 4.0), result);
  EXPECT_THAT(result, ElementsAre(0u));

  // Boundary contact counts as containment.
  result.clear();
  tree.query(Envelope(3.0, 5.0, 4.0, 6.0), result);
  EXPECT_THAT(result, ElementsAre(0u));

  result.clear();
  tree.query(Envelope(3.1, 5.0, 4.0, 6.0), result);
  EXPECT_THAT(result, IsEmpty());

  result.clear();
  tree.query(Envelope(), result);
  EXPECT_THAT(result, IsEmpty());

  tree.remove(0);
  bounds = tree.getBounds();
  ASSERT_EQ(bounds.size(), 2u);
  EXPECT_TRUE(bounds[0].isNull());

  result.clear();
  tree.query(Envelope(0.0, 10.0, 0.0, 10.0), result);
  EXPECT_THAT(result, IsEmpty());
}

TEST(VertexSequencePackedRtreeTest,
     ExactSingleNodeCapacityPartialAndFullRemoval) {
  constexpr std::size_t kCount = 16;
  const CoordinateSequence pts = MakeClusteredSequence(kCount);
  VertexSequencePackedRtree tree(pts);

  std::vector<Envelope> bounds = tree.getBounds();
  ASSERT_EQ(bounds.size(), 2u);
  EXPECT_EQ(bounds[0], Envelope(0.0, 30.0, 0.0, 9.0));
  EXPECT_EQ(bounds[1], bounds[0]);

  std::vector<bool> removed(kCount, false);
  const Envelope partial_env(4.0, 16.0, 0.0, 6.0);
  std::vector<std::size_t> result;
  tree.query(partial_env, result);
  EXPECT_EQ(result, BruteForceQuery(pts, removed, partial_env));
  EXPECT_FALSE(result.empty());

  // Removing 15 of 16 items must keep the leaf node bound non-null.
  for (std::size_t i = 0; i < kCount - 1; ++i) {
    tree.remove(i);
    removed[i] = true;
  }
  bounds = tree.getBounds();
  EXPECT_FALSE(bounds[0].isNull());

  const Envelope full_env(0.0, 40.0, 0.0, 20.0);
  result.clear();
  tree.query(full_env, result);
  EXPECT_THAT(result, ElementsAre(15u));

  // Removing the final item clears the leaf bound.
  tree.remove(15);
  bounds = tree.getBounds();
  EXPECT_TRUE(bounds[0].isNull());

  result.clear();
  tree.query(full_env, result);
  EXPECT_THAT(result, IsEmpty());
}

TEST(VertexSequencePackedRtreeTest, TwoLevelTreeSplitsAndPrunesEmptyLeaves) {
  constexpr std::size_t kCount = 17;
  const CoordinateSequence pts = MakeClusteredSequence(kCount);
  VertexSequencePackedRtree tree(pts);

  // 17 items -> 2 leaf nodes, 1 level-1 parent, 1 root slot => 4 bounds.
  std::vector<Envelope> bounds = tree.getBounds();
  ASSERT_EQ(bounds.size(), 4u);
  EXPECT_EQ(bounds[0], Envelope(0.0, 30.0, 0.0, 9.0));
  EXPECT_EQ(bounds[1], Envelope(100.0, 100.0, 50.0, 50.0));
  EXPECT_EQ(bounds[2], Envelope(0.0, 100.0, 0.0, 50.0));
  EXPECT_EQ(bounds[3], bounds[2]);

  std::vector<bool> removed(kCount, false);
  const std::vector<Envelope> queries = {
      Envelope(0.0, 10.0, 0.0, 10.0),        // leaf 0 subset
      Envelope(95.0, 105.0, 45.0, 55.0),     // leaf 1 only
      Envelope(20.0, 100.0, 0.0, 50.0),      // both leaves
      Envelope(50.0, 80.0, 20.0, 30.0),      // inside root bbox, outside leaves
      Envelope(500.0, 600.0, 500.0, 600.0),  // disjoint from root
  };

  for (const auto& q : queries) {
    std::vector<std::size_t> actual;
    tree.query(q, actual);
    EXPECT_EQ(actual, BruteForceQuery(pts, removed, q));
  }

  // Emptying leaf 1 nulls bounds[1], while parent bounds[2] remains non-null
  // because leaf 0 is still populated.
  tree.remove(16);
  removed[16] = true;
  bounds = tree.getBounds();
  EXPECT_TRUE(bounds[1].isNull());
  EXPECT_FALSE(bounds[2].isNull());

  std::vector<std::size_t> leaf1_query;
  tree.query(Envelope(90.0, 110.0, 40.0, 60.0), leaf1_query);
  EXPECT_THAT(leaf1_query, IsEmpty());

  // Emptying leaf 0 nulls bounds[0] and also propagates to parent bounds[2].
  for (std::size_t i = 0; i < 16; ++i) {
    tree.remove(i);
    removed[i] = true;
  }
  bounds = tree.getBounds();
  EXPECT_TRUE(bounds[0].isNull());
  EXPECT_TRUE(bounds[2].isNull());

  std::vector<std::size_t> all_query;
  tree.query(Envelope(-10.0, 200.0, -10.0, 100.0), all_query);
  EXPECT_THAT(all_query, IsEmpty());
}

TEST(VertexSequencePackedRtreeTest, ExactMultipleLeavesAndSquaredCapacity) {
  for (const std::size_t count : {std::size_t{32}, std::size_t{256}}) {
    SCOPED_TRACE(count);
    const CoordinateSequence pts = MakeClusteredSequence(count);
    VertexSequencePackedRtree tree(pts);

    const std::vector<bool> removed(count, false);
    const Envelope query_env(100.0, 350.0, 50.0, 200.0);
    std::vector<std::size_t> actual;
    tree.query(query_env, actual);
    EXPECT_EQ(actual, BruteForceQuery(pts, removed, query_env));
    EXPECT_FALSE(actual.empty());
  }
}

TEST(VertexSequencePackedRtreeTest, ThreeLevelTreeMultiLevelQueryAndPruning) {
  // 275 items -> 18 leaves, 2 level-1 nodes, 1 level-2 node, 1 root slot = 22.
  constexpr std::size_t kCount = 275;
  const CoordinateSequence pts = MakeClusteredSequence(kCount);
  VertexSequencePackedRtree tree(pts);

  std::vector<Envelope> bounds = tree.getBounds();
  ASSERT_EQ(bounds.size(), 22u);

  std::vector<bool> removed(kCount, false);
  const std::vector<Envelope> queries = {
      // Intersects level-1 node 0 (leaves 0..15) and prunes level-1 node 1.
      Envelope(200.0, 650.0, 100.0, 350.0),
      // Prunes level-1 node 0, visits full leaf 16 and partial leaf 17.
      Envelope(1600.0, 1750.0, 800.0, 900.0),
      // Spans all 275 items.
      Envelope(-1.0, 2000.0, -1.0, 1000.0),
      // Completely disjoint.
      Envelope(-100.0, -10.0, -100.0, -10.0),
  };

  for (const auto& q : queries) {
    std::vector<std::size_t> actual;
    tree.query(q, actual);
    EXPECT_EQ(actual, BruteForceQuery(pts, removed, q));
  }

  // Remove leaf 0 (items 0..15): leaf 0 becomes null, level-1 node 0 stays
  // non-null because leaves 1..15 remain active.
  for (std::size_t i = 0; i < 16; ++i) {
    tree.remove(i);
    removed[i] = true;
  }
  bounds = tree.getBounds();
  EXPECT_TRUE(bounds[0].isNull());
  EXPECT_FALSE(bounds[18].isNull());

  // Remove leaves 1..15 (items 16..255): all 16 children of level-1 node 0 are
  // now null, so level-1 node 0 (bounds[18]) becomes null too.
  for (std::size_t i = 16; i < 256; ++i) {
    tree.remove(i);
    removed[i] = true;
  }
  bounds = tree.getBounds();
  EXPECT_TRUE(bounds[18].isNull());
  EXPECT_FALSE(bounds[19].isNull());

  // Items in level-1 node 1 (256..274) must still match queries.
  const Envelope full_env(-1.0, 2000.0, -1.0, 1000.0);
  std::vector<std::size_t> remaining;
  tree.query(full_env, remaining);
  EXPECT_EQ(remaining, BruteForceQuery(pts, removed, full_env));
  EXPECT_EQ(remaining.size(), kCount - 256);

  // Remove remaining items 256..274 (partial level-1 node 1).
  for (std::size_t i = 256; i < kCount; ++i) {
    tree.remove(i);
    removed[i] = true;
  }
  bounds = tree.getBounds();
  EXPECT_TRUE(bounds[19].isNull());

  remaining.clear();
  tree.query(full_env, remaining);
  EXPECT_THAT(remaining, IsEmpty());
}

TEST(VertexSequencePackedRtreeTest, DuplicatePointsIdempotentRemovalAndAppend) {
  const CoordinateSequence pts{
      Coordinate(1.0, 1.0), Coordinate(10.0, 10.0), Coordinate(1.0, 1.0),
      Coordinate(5.0, 5.0), Coordinate(1.0, 1.0),
  };
  VertexSequencePackedRtree tree(pts);

  std::vector<std::size_t> result = {100u};
  tree.query(Envelope(0.5, 1.5, 0.5, 1.5), result);
  EXPECT_THAT(result, ElementsAre(100u, 0u, 2u, 4u));

  // Removing the same index twice is safe and idempotent.
  tree.remove(2);
  tree.remove(2);

  result.clear();
  tree.query(Envelope(0.5, 1.5, 0.5, 1.5), result);
  EXPECT_THAT(result, ElementsAre(0u, 4u));

  tree.remove(0);
  tree.remove(4);
  result.clear();
  tree.query(Envelope(0.5, 1.5, 0.5, 1.5), result);
  EXPECT_THAT(result, IsEmpty());
}

}  // namespace
}  // namespace geos_google
