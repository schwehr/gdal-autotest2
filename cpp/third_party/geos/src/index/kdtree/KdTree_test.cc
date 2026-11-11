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

#include "third_party/geos/include/geos/index/kdtree/KdTree.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/index/kdtree/KdNode.h"
#include "third_party/geos/include/geos/index/kdtree/KdNodeVisitor.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::Envelope;
using ::geos::index::kdtree::KdNode;
using ::geos::index::kdtree::KdNodeVisitor;
using ::geos::index::kdtree::KdTree;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;
using ::testing::UnorderedElementsAreArray;

class RecordingVisitor final : public KdNodeVisitor {
 public:
  void visit(KdNode* node) override { visited.push_back(node); }

  std::vector<KdNode*> visited;
};

std::vector<Coordinate> ExtractCoordinates(const std::vector<KdNode*>& nodes) {
  std::vector<Coordinate> coords;
  coords.reserve(nodes.size());
  for (KdNode* node : nodes) {
    coords.push_back(node->getCoordinate());
  }
  return coords;
}

TEST(KdTreeTest, EmptyTreeDefaultsAndQueries) {
  KdTree default_tree;
  KdTree tolerant_tree(1.5);

  EXPECT_TRUE(default_tree.isEmpty());
  EXPECT_TRUE(tolerant_tree.isEmpty());

  EXPECT_EQ(default_tree.query(Coordinate(0.0, 0.0)), nullptr);
  EXPECT_EQ(tolerant_tree.query(Coordinate(0.0, 0.0)), nullptr);

  const Envelope search_env(-10.0, 10.0, -10.0, 10.0);
  std::unique_ptr<std::vector<KdNode*>> owned_result =
      default_tree.query(search_env);
  ASSERT_NE(owned_result, nullptr);
  EXPECT_THAT(*owned_result, IsEmpty());

  std::vector<KdNode*> out_result;
  default_tree.query(search_env, out_result);
  EXPECT_THAT(out_result, IsEmpty());

  // Querying an empty tree with a null Envelope exits before reading extents.
  default_tree.query(Envelope(), out_result);
  EXPECT_THAT(out_result, IsEmpty());

  RecordingVisitor visitor;
  tolerant_tree.query(search_env, visitor);
  EXPECT_THAT(visitor.visited, IsEmpty());
}

TEST(KdTreeTest, InsertSinglePointDefaultAndWithData) {
  KdTree tree;
  const Coordinate p1(1.5, 2.5, 3.5);
  KdNode* n1 = tree.insert(p1);
  ASSERT_NE(n1, nullptr);
  EXPECT_FALSE(tree.isEmpty());
  EXPECT_TRUE(n1->getCoordinate().equals3D(p1));
  EXPECT_EQ(n1->getData(), nullptr);
  EXPECT_EQ(n1->getCount(), std::size_t{1});
  EXPECT_FALSE(n1->isRepeated());

  int payload = 7;
  const Coordinate p2(-4.0, 8.0);
  KdNode* n2 = tree.insert(p2, &payload);
  ASSERT_NE(n2, nullptr);
  EXPECT_NE(n2, n1);
  EXPECT_EQ(n2->getData(), &payload);
  EXPECT_EQ(n2->getCount(), std::size_t{1});
}

TEST(KdTreeTest, InsertZeroToleranceDeduplicatesExact2DCoordinates) {
  KdTree tree;
  int d1 = 1;
  int d2 = 2;
  int d3 = 3;
  int d4 = 4;

  KdNode* root = tree.insert(Coordinate(10.0, 10.0, 1.0), &d1);
  KdNode* left = tree.insert(Coordinate(5.0, 5.0), &d2);
  KdNode* right = tree.insert(Coordinate(15.0, 15.0), &d3);

  // Re-inserting an identical 2D point (even with different Z and data)
  // increments the existing node's count and preserves its original data.
  KdNode* dup_root = tree.insert(Coordinate(10.0, 10.0, 99.0), &d4);
  EXPECT_EQ(dup_root, root);
  EXPECT_EQ(root->getCount(), std::size_t{2});
  EXPECT_TRUE(root->isRepeated());
  EXPECT_EQ(root->getData(), &d1);

  EXPECT_EQ(tree.insert(Coordinate(5.0, 5.0), &d4), left);
  EXPECT_EQ(tree.insert(Coordinate(15.0, 15.0), &d4), right);
  EXPECT_EQ(left->getCount(), std::size_t{2});
  EXPECT_EQ(right->getCount(), std::size_t{2});

  std::unique_ptr<std::vector<KdNode*>> all =
      tree.query(Envelope(0.0, 20.0, 0.0, 20.0));
  ASSERT_NE(all, nullptr);
  EXPECT_THAT(*all, UnorderedElementsAre(root, left, right));
}

TEST(KdTreeTest, InsertNegativeToleranceCreatesDistinctNodesForDuplicates) {
  KdTree tree(-1.0);
  int d1 = 1;
  int d2 = 2;

  KdNode* first = tree.insert(Coordinate(3.0, 4.0), &d1);
  KdNode* second = tree.insert(Coordinate(3.0, 4.0), &d2);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(first, second);
  EXPECT_EQ(first->getCount(), std::size_t{1});
  EXPECT_EQ(second->getCount(), std::size_t{1});
  EXPECT_EQ(first->getData(), &d1);
  EXPECT_EQ(second->getData(), &d2);

  std::unique_ptr<std::vector<KdNode*>> found =
      tree.query(Envelope(0.0, 5.0, 0.0, 5.0));
  ASSERT_NE(found, nullptr);
  EXPECT_THAT(*found, UnorderedElementsAre(first, second));
  EXPECT_EQ(tree.query(Coordinate(3.0, 4.0)), first);
}

TEST(KdTreeTest, InsertWithToleranceSnapsWithinRadiusAndRejectsCorners) {
  KdTree tree(1.0);
  int d1 = 10;
  int d2 = 20;
  int d3 = 30;

  KdNode* origin = tree.insert(Coordinate(0.0, 0.0), &d1);

  // (1, 1) has query envelope [0, 2] x [0, 2], which contains (0, 0), so
  // BestMatchVisitor visits (0, 0) but rejects it because sqrt(2) > 1.0.
  KdNode* corner = tree.insert(Coordinate(1.0, 1.0), &d2);
  EXPECT_NE(corner, origin);
  EXPECT_EQ(origin->getCount(), std::size_t{1});
  EXPECT_EQ(corner->getCount(), std::size_t{1});

  // (0, -1) is at exact boundary distance 1.0 from (0, 0) and sqrt(5) from
  // (1, 1), so it snaps to origin.
  KdNode* snapped = tree.insert(Coordinate(0.0, -1.0), &d3);
  EXPECT_EQ(snapped, origin);
  EXPECT_EQ(origin->getCount(), std::size_t{2});
  EXPECT_EQ(origin->getData(), &d1);
}

TEST(KdTreeTest, InsertWithTolerancePicksClosestRegardlessOfVisitOrder) {
  // Place three points at pairwise distance > 2.5 so none snap to each other.
  KdTree tree(2.5);
  KdNode* a = tree.insert(Coordinate(0.0, 2.0));    // root (X split)
  KdNode* b = tree.insert(Coordinate(-1.7, -1.0));  // left of root
  KdNode* c = tree.insert(Coordinate(1.7, -1.0));   // right of root
  ASSERT_NE(a, b);
  ASSERT_NE(a, c);
  ASSERT_NE(b, c);

  // Query envelope for (0, 0.2) covers all three; in-order visits B (d~2.08),
  // then A (d=1.80 < 2.08 -> updates), then C (d~2.08 > 1.80 -> keeps A).
  KdNode* snap_to_a = tree.insert(Coordinate(0.0, 0.2));
  EXPECT_EQ(snap_to_a, a);
  EXPECT_EQ(a->getCount(), std::size_t{2});

  // Query (0.2, -0.8): A is outside tol (d~2.81 > 2.5); in-order visits B
  // (d~1.91), skips A, then visits C (d~1.51 < 1.91 -> updates to C).
  KdNode* snap_to_c = tree.insert(Coordinate(0.2, -0.8));
  EXPECT_EQ(snap_to_c, c);
  EXPECT_EQ(c->getCount(), std::size_t{2});
}

TEST(KdTreeTest, InsertWithToleranceBreaksDistanceTiesToLowestCoordinate) {
  // Case 1: lesser coordinate visited before greater coordinate.
  {
    KdTree tree(1.5);
    KdNode* root = tree.insert(Coordinate(2.0, 5.0));
    KdNode* left = tree.insert(Coordinate(1.0, 0.0));
    KdNode* right = tree.insert(Coordinate(3.0, 0.0));
    ASSERT_NE(left, right);
    ASSERT_NE(left, root);

    // (2, 1) is equidistant (sqrt(2) <= 1.5) from (1, 0) and (3, 0).
    // In-order visits (1, 0) then (3, 0) and keeps the lesser (1, 0).
    KdNode* snapped = tree.insert(Coordinate(2.0, 1.0));
    EXPECT_EQ(snapped, left);
    EXPECT_EQ(left->getCount(), std::size_t{2});
    EXPECT_EQ(right->getCount(), std::size_t{1});
  }

  // Case 2: greater X coordinate visited before lesser X coordinate.
  {
    KdTree tree(1.5);
    tree.insert(Coordinate(5.0, 10.0));
    KdNode* greater = tree.insert(Coordinate(3.0, 0.0));  // left of root (Y)
    KdNode* lesser = tree.insert(Coordinate(1.0, 0.0));   // right of (3, 0)
    ASSERT_NE(greater, lesser);

    // In-order visits (3, 0) first, then (1, 0), and updates to (1, 0).
    KdNode* snapped = tree.insert(Coordinate(2.0, 1.0));
    EXPECT_EQ(snapped, lesser);
    EXPECT_EQ(lesser->getCount(), std::size_t{2});
    EXPECT_EQ(greater->getCount(), std::size_t{1});
  }

  // Case 3: equal X tie broken by Y when greater Y is visited before lesser Y.
  {
    KdTree tree(1.5);
    tree.insert(Coordinate(10.0, 10.0));                    // depth 1 (X)
    tree.insert(Coordinate(-5.0, 5.0));                     // depth 2 (Y)
    KdNode* greater_y = tree.insert(Coordinate(0.0, 2.0));  // depth 3 (X)
    KdNode* lesser_y = tree.insert(Coordinate(0.0, 0.0));   // right of (0, 2)
    ASSERT_NE(greater_y, lesser_y);

    // For (0, 1), in-order visits (0, 2) then (0, 0) (both dist 1.0) and
    // updates matchNode to (0, 0).
    KdNode* snapped = tree.insert(Coordinate(0.0, 1.0));
    EXPECT_EQ(snapped, lesser_y);
    EXPECT_EQ(lesser_y->getCount(), std::size_t{2});
    EXPECT_EQ(greater_y->getCount(), std::size_t{1});
  }
}

TEST(KdTreeTest, QueryEnvelopeOverloadsBoundaryInclusionAndAppending) {
  KdTree tree;
  const std::vector<Coordinate> inside_pts = {
      Coordinate(5.0, 5.0),    // interior
      Coordinate(0.0, 0.0),    // min-min corner
      Coordinate(10.0, 10.0),  // max-max corner
      Coordinate(0.0, 5.0),    // left edge
      Coordinate(10.0, 5.0),   // right edge
      Coordinate(5.0, 0.0),    // bottom edge
      Coordinate(5.0, 10.0),   // top edge
  };
  const std::vector<Coordinate> outside_pts = {
      Coordinate(-0.01, 5.0),
      Coordinate(10.01, 5.0),
      Coordinate(5.0, -0.01),
      Coordinate(5.0, 10.01),
  };

  for (const Coordinate& pt : inside_pts) {
    tree.insert(pt);
  }
  for (const Coordinate& pt : outside_pts) {
    tree.insert(pt);
  }

  const Envelope query_env(0.0, 10.0, 0.0, 10.0);

  std::unique_ptr<std::vector<KdNode*>> owned = tree.query(query_env);
  ASSERT_NE(owned, nullptr);
  EXPECT_THAT(ExtractCoordinates(*owned),
              UnorderedElementsAreArray(inside_pts));

  // Verify vector overload appends to existing results.
  KdNode extra(100.0, 100.0, nullptr);
  std::vector<KdNode*> appended = {&extra};
  tree.query(query_env, appended);
  ASSERT_EQ(appended.size(), inside_pts.size() + 1);
  EXPECT_EQ(appended.front(), &extra);

  RecordingVisitor visitor;
  tree.query(query_env, visitor);
  EXPECT_THAT(ExtractCoordinates(visitor.visited),
              UnorderedElementsAreArray(inside_pts));

  // Non-empty tree with a disjoint query envelope returns nothing.
  std::unique_ptr<std::vector<KdNode*>> empty_match =
      tree.query(Envelope(50.0, 60.0, 50.0, 60.0));
  ASSERT_NE(empty_match, nullptr);
  EXPECT_THAT(*empty_match, IsEmpty());
}

TEST(KdTreeTest, QueryNodeHandlesEqualSplitOrdinatesAndPrunedSubtrees) {
  KdTree tree;
  // Build an explicit structure exercising equal-X and equal-Y routing:
  // Level 1 (X): (10, 10)
  //   Left  (Y): (8, 20)
  //     Left  (X): (6, 15)
  //     Right (X): (6, 20)   [20 >= 20 -> right]
  //       Right (Y): (7, 25) [7 >= 6 -> right]
  //   Right (Y): (10, 20)    [10 >= 10 -> right]
  //     Left  (X): (12, 12)  [12 < 20 -> left]
  //     Right (X): (14, 25)  [25 >= 20 -> right]
  tree.insert(Coordinate(10.0, 10.0));
  tree.insert(Coordinate(8.0, 20.0));
  tree.insert(Coordinate(6.0, 15.0));
  tree.insert(Coordinate(6.0, 20.0));
  tree.insert(Coordinate(7.0, 25.0));
  tree.insert(Coordinate(10.0, 20.0));
  tree.insert(Coordinate(12.0, 12.0));
  tree.insert(Coordinate(14.0, 25.0));

  // minX == maxX == 10 prunes left subtree of root (10 < 10 is false) while
  // searching right subtree (10 <= 10 is true) to find (10, 20).
  std::unique_ptr<std::vector<KdNode*>> on_x_split =
      tree.query(Envelope(10.0, 10.0, 10.0, 25.0));
  ASSERT_NE(on_x_split, nullptr);
  EXPECT_THAT(
      ExtractCoordinates(*on_x_split),
      UnorderedElementsAre(Coordinate(10.0, 10.0), Coordinate(10.0, 20.0)));

  // minY == maxY == 20 at level 2 node (8, 20) prunes (6, 15) (20 < 20 false)
  // while searching right child (6, 20) (20 <= 20 true).
  std::unique_ptr<std::vector<KdNode*>> on_y_split =
      tree.query(Envelope(5.0, 9.0, 20.0, 20.0));
  ASSERT_NE(on_y_split, nullptr);
  EXPECT_THAT(
      ExtractCoordinates(*on_y_split),
      UnorderedElementsAre(Coordinate(8.0, 20.0), Coordinate(6.0, 20.0)));

  // Root (10, 10) is excluded by Envelope(11, 13, 11, 13), yet right child
  // (10, 20) -> left child (12, 12) IS found.
  std::unique_ptr<std::vector<KdNode*>> child_only_right =
      tree.query(Envelope(11.0, 13.0, 11.0, 13.0));
  ASSERT_NE(child_only_right, nullptr);
  EXPECT_THAT(ExtractCoordinates(*child_only_right),
              UnorderedElementsAre(Coordinate(12.0, 12.0)));

  // Root (10, 10) excluded and right subtree pruned; left child subtree yields
  // (6, 15).
  std::unique_ptr<std::vector<KdNode*>> child_only_left =
      tree.query(Envelope(5.5, 6.5, 14.5, 15.5));
  ASSERT_NE(child_only_left, nullptr);
  EXPECT_THAT(ExtractCoordinates(*child_only_left),
              UnorderedElementsAre(Coordinate(6.0, 15.0)));

  // Prune both left and right subtrees when query falls below root's Y range
  // and searchRight stops before entering right branch.
  std::unique_ptr<std::vector<KdNode*>> right_pruned =
      tree.query(Envelope(6.5, 7.5, 24.0, 26.0));
  ASSERT_NE(right_pruned, nullptr);
  EXPECT_THAT(ExtractCoordinates(*right_pruned),
              UnorderedElementsAre(Coordinate(7.0, 25.0)));
}

TEST(KdTreeTest, QueryPointSearchExactAlternatingSplitsAndMisses) {
  KdTree tree;
  const std::vector<Coordinate> points = {
      Coordinate(10.0, 10.0, 5.0),  // root
      Coordinate(5.0, 15.0),        // left (X < 10)
      Coordinate(7.0, 12.0),        // left-left (Y < 15)
      Coordinate(3.0, 18.0),        // left-right (Y >= 15)
      Coordinate(2.0, 20.0),        // left-right-left (X < 3)
      Coordinate(4.0, 16.0),        // left-right-right (X >= 3)
      Coordinate(15.0, 5.0),        // right (X >= 10)
      Coordinate(12.0, 3.0),        // right-left (Y < 5)
      Coordinate(18.0, 8.0),        // right-right (Y >= 5)
      Coordinate(10.0, 20.0),       // equal-X to root -> right path
      Coordinate(6.0, 15.0),        // equal-Y to (5, 15) -> right path
  };

  std::vector<KdNode*> nodes;
  nodes.reserve(points.size());
  for (const Coordinate& pt : points) {
    nodes.push_back(tree.insert(pt));
  }

  for (std::size_t i = 0; i < points.size(); ++i) {
    KdNode* found = tree.query(points[i]);
    EXPECT_EQ(found, nodes[i]) << "Failed at index " << i;
  }

  // Point lookup uses equals2D, so a differing Z coordinate still matches.
  EXPECT_EQ(tree.query(Coordinate(10.0, 10.0, 999.0)), nodes[0]);

  // Verify misses across left/right and odd/even null branches.
  EXPECT_EQ(tree.query(Coordinate(10.0, 9.0)), nullptr);
  EXPECT_EQ(tree.query(Coordinate(5.0, 16.0)), nullptr);
  EXPECT_EQ(tree.query(Coordinate(2.0, 19.0)), nullptr);
  EXPECT_EQ(tree.query(Coordinate(18.0, 9.0)), nullptr);
  EXPECT_EQ(tree.query(Coordinate(100.0, 100.0)), nullptr);
}

TEST(KdTreeTest, ToCoordinatesHandlesCountsAndConsecutiveDuplicates) {
  std::vector<KdNode*> empty_nodes;
  EXPECT_THAT(*KdTree::toCoordinates(empty_nodes), IsEmpty());
  EXPECT_THAT(*KdTree::toCoordinates(empty_nodes, true), IsEmpty());

  KdTree tree(0.5);
  KdNode* n1 = tree.insert(Coordinate(1.0, 2.0));
  tree.insert(Coordinate(1.1, 2.0));  // snaps to n1 -> count == 2
  tree.insert(Coordinate(1.0, 2.1));  // snaps to n1 -> count == 3
  KdNode* n2 = tree.insert(Coordinate(5.0, 6.0));

  std::vector<KdNode*> nodes = {n1, n2};
  std::unique_ptr<std::vector<Coordinate>> unique_coords =
      KdTree::toCoordinates(nodes);
  EXPECT_THAT(*unique_coords,
              ElementsAre(Coordinate(1.0, 2.0), Coordinate(5.0, 6.0)));

  std::unique_ptr<std::vector<Coordinate>> no_repeat_coords =
      KdTree::toCoordinates(nodes, false);
  EXPECT_THAT(*no_repeat_coords,
              ElementsAre(Coordinate(1.0, 2.0), Coordinate(5.0, 6.0)));

  std::unique_ptr<std::vector<Coordinate>> repeated_coords =
      KdTree::toCoordinates(nodes, true);
  EXPECT_THAT(*repeated_coords,
              ElementsAre(Coordinate(1.0, 2.0), Coordinate(1.0, 2.0),
                          Coordinate(1.0, 2.0), Coordinate(5.0, 6.0)));

  // Distinct KdNode objects with identical 2D coordinates placed consecutively
  // are collapsed by std::unique when includeRepeated is false.
  KdNode dup_a(Coordinate(9.0, 8.0, 1.0), nullptr);
  KdNode dup_b(Coordinate(9.0, 8.0, 2.0), nullptr);
  dup_b.increment();
  std::vector<KdNode*> consecutive_dups = {&dup_a, &dup_b, n2};

  EXPECT_THAT(*KdTree::toCoordinates(consecutive_dups, false),
              ElementsAre(Coordinate(9.0, 8.0), Coordinate(5.0, 6.0)));
  EXPECT_THAT(*KdTree::toCoordinates(consecutive_dups, true),
              ElementsAre(Coordinate(9.0, 8.0), Coordinate(9.0, 8.0),
                          Coordinate(9.0, 8.0), Coordinate(5.0, 6.0)));
}

TEST(KdTreeTest, MatchesBruteForceOnMixedAndDeepTrees) {
  KdTree tree;
  std::vector<Coordinate> inserted;

  // First insert a monotone diagonal chain to create a deep unbalanced branch
  // that exercises the iterative stack traversal in queryNode.
  for (int i = 0; i < 250; ++i) {
    Coordinate pt(static_cast<double>(i) * 0.25, static_cast<double>(i) * 0.5);
    inserted.push_back(pt);
    tree.insert(pt);
  }

  // Insert deterministic pseudo-random points covering all four quadrants.
  std::uint32_t state = 0x12345678u;
  auto next_unit = [&state]() {
    state = state * 1664525u + 1013904223u;
    return static_cast<double>(state % 2001u) - 1000.0;
  };
  for (int i = 0; i < 500; ++i) {
    const Coordinate pt(next_unit() * 0.1, next_unit() * 0.1);
    if (tree.query(pt) == nullptr) {
      inserted.push_back(pt);
    }
    tree.insert(pt);
  }

  for (const Coordinate& pt : inserted) {
    KdNode* found = tree.query(pt);
    ASSERT_NE(found, nullptr);
    EXPECT_TRUE(found->getCoordinate().equals2D(pt));
  }

  const std::vector<Envelope> envelopes = {
      Envelope(-100.0, 100.0, -100.0, 100.0),
      Envelope(-25.0, 25.0, -10.0, 40.0),
      Envelope(0.0, 62.25, 0.0, 124.5),
      Envelope(-50.0, -10.0, 5.0, 80.0),
      Envelope(1000.0, 2000.0, 1000.0, 2000.0),
  };

  for (const Envelope& env : envelopes) {
    std::vector<Coordinate> expected;
    for (const Coordinate& pt : inserted) {
      if (env.contains(pt)) {
        expected.push_back(pt);
      }
    }
    std::unique_ptr<std::vector<KdNode*>> actual = tree.query(env);
    ASSERT_NE(actual, nullptr);
    EXPECT_THAT(ExtractCoordinates(*actual),
                UnorderedElementsAreArray(expected));
  }
}

}  // namespace
}  // namespace geos_google
