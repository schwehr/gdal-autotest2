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

// Unit tests for third_party/geos/src/index/strtree/SimpleSTRtree.cpp.
//
// google3 notes:
// - SimpleSTRtree stores raw pointers to caller-owned Envelope and item
//   objects; inserted envelopes and items must outlive the SimpleSTRtree.
// - SimpleSTRtree::remove() dereferences root without a null check, so tests
//   invoke remove() only on non-empty trees.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/strtree/SimpleSTRtree.cpp

#include "third_party/geos/include/geos/index/strtree/SimpleSTRtree.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/index/ItemVisitor.h"
#include "third_party/geos/include/geos/index/SpatialIndex.h"
#include "third_party/geos/include/geos/index/strtree/GeometryItemDistance.h"
#include "third_party/geos/include/geos/index/strtree/ItemBoundable.h"
#include "third_party/geos/include/geos/index/strtree/ItemDistance.h"
#include "third_party/geos/include/geos/index/strtree/SimpleSTRnode.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::geom::Point;
using geos::index::ItemVisitor;
using geos::index::SpatialIndex;
using geos::index::strtree::GeometryItemDistance;
using geos::index::strtree::ItemBoundable;
using geos::index::strtree::ItemDistance;
using geos::index::strtree::SimpleSTRnode;
using geos::index::strtree::SimpleSTRtree;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAreArray;

struct BoxSpec {
  double min_x;
  double max_x;
  double min_y;
  double max_y;
  int id;
};

struct TaggedBox {
  Envelope env;
  int id;
};

class CollectingVisitor final : public ItemVisitor {
 public:
  void visitItem(void* item) override { items_.push_back(item); }

  const std::vector<void*>& items() const { return items_; }

 private:
  std::vector<void*> items_;
};

class EnvelopeItemDistance final : public ItemDistance {
 public:
  explicit EnvelopeItemDistance(bool exclude_same_item = false)
      : exclude_same_item_(exclude_same_item) {}

  double distance(const ItemBoundable* item1,
                  const ItemBoundable* item2) override {
    if (exclude_same_item_ && item1->getItem() == item2->getItem()) {
      return geos::DoubleMax;
    }
    const auto* e1 = static_cast<const Envelope*>(item1->getBounds());
    const auto* e2 = static_cast<const Envelope*>(item2->getBounds());
    return e1->distance(*e2);
  }

 private:
  bool exclude_same_item_;
};

std::vector<TaggedBox> MakeBoxes(std::span<const BoxSpec> specs) {
  std::vector<TaggedBox> boxes;
  boxes.reserve(specs.size());
  for (const BoxSpec& spec : specs) {
    boxes.push_back(
        {Envelope(spec.min_x, spec.max_x, spec.min_y, spec.max_y), spec.id});
  }
  return boxes;
}

std::vector<int> ExtractIds(const std::vector<void*>& ptrs) {
  std::vector<int> ids;
  ids.reserve(ptrs.size());
  for (void* p : ptrs) {
    ids.push_back(static_cast<const TaggedBox*>(p)->id);
  }
  return ids;
}

std::vector<int> ExpectedIntersectingIds(std::span<const TaggedBox> boxes,
                                         const Envelope& search_env) {
  std::vector<int> ids;
  for (const TaggedBox& box : boxes) {
    if (box.env.intersects(search_env)) {
      ids.push_back(box.id);
    }
  }
  return ids;
}

// 15 unsorted boxes spanning negative coordinates, degenerate point/line
// boxes, and shared midpoints to exercise X/Y sorting and multi-level STR
// tree construction.
constexpr auto kGridBoxSpecs = std::to_array<BoxSpec>({
    {.min_x = 30.0, .max_x = 32.0, .min_y = 20.0, .max_y = 22.0, .id = 1},
    {.min_x = -12.0, .max_x = -10.0, .min_y = -8.0, .max_y = -6.0, .id = 2},
    {.min_x = 5.0, .max_x = 5.0, .min_y = 5.0, .max_y = 5.0, .id = 3},
    {.min_x = 12.0, .max_x = 14.0, .min_y = 2.0, .max_y = 6.0, .id = 4},
    {.min_x = -4.0, .max_x = -2.0, .min_y = 14.0, .max_y = 18.0, .id = 5},
    {.min_x = 20.0, .max_x = 24.0, .min_y = -5.0, .max_y = -1.0, .id = 6},
    {.min_x = 0.0, .max_x = 2.0, .min_y = 0.0, .max_y = 2.0, .id = 7},
    {.min_x = 12.0, .max_x = 14.0, .min_y = 16.0, .max_y = 20.0, .id = 8},
    {.min_x = -10.0, .max_x = -8.0, .min_y = 7.0, .max_y = 9.0, .id = 9},
    {.min_x = 25.0, .max_x = 27.0, .min_y = 10.0, .max_y = 10.0, .id = 10},
    {.min_x = 7.0, .max_x = 9.0, .min_y = -12.0, .max_y = -10.0, .id = 11},
    {.min_x = 16.0, .max_x = 18.0, .min_y = 8.0, .max_y = 12.0, .id = 12},
    {.min_x = -6.0, .max_x = -4.0, .min_y = -2.0, .max_y = 2.0, .id = 13},
    {.min_x = 21.0, .max_x = 23.0, .min_y = 24.0, .max_y = 26.0, .id = 14},
    {.min_x = 1.0, .max_x = 3.0, .min_y = 22.0, .max_y = 24.0, .id = 15},
});

TEST(SimpleSTRtreeTest, DefaultAndCustomCapacityAndEmptyTree) {
  SimpleSTRtree default_tree;
  EXPECT_EQ(default_tree.getNodeCapacity(), 10U);
  EXPECT_FALSE(default_tree.getBuilt());
  EXPECT_EQ(default_tree.getNumLeafNodes(), 0U);
  EXPECT_EQ(default_tree.root, nullptr);

  SimpleSTRtree tree(3);
  EXPECT_EQ(tree.getNodeCapacity(), 3U);
  EXPECT_FALSE(tree.getBuilt());

  // Building an empty tree sets built=true while leaving root null.
  EXPECT_EQ(tree.getRoot(), nullptr);
  EXPECT_TRUE(tree.getBuilt());
  EXPECT_EQ(tree.getNumLeafNodes(), 0U);

  // Subsequent getRoot() is an idempotent no-op.
  EXPECT_EQ(tree.getRoot(), nullptr);

  CollectingVisitor iter_visitor;
  tree.iterate(iter_visitor);
  EXPECT_THAT(iter_visitor.items(), IsEmpty());

  const Envelope search(-50.0, 50.0, -50.0, 50.0);
  std::vector<void*> matches;
  tree.query(&search, matches);
  EXPECT_THAT(matches, IsEmpty());

  CollectingVisitor query_visitor;
  tree.query(&search, query_visitor);
  EXPECT_THAT(query_visitor.items(), IsEmpty());

  EnvelopeItemDistance dist;
  EXPECT_EQ(tree.nearestNeighbour(&dist),
            (std::pair<const void*, const void*>{nullptr, nullptr}));
  int dummy_item = 1;
  EXPECT_EQ(tree.nearestNeighbour(&search, &dummy_item, &dist), nullptr);

  SimpleSTRtree other_empty(3);
  EXPECT_EQ(tree.nearestNeighbour(other_empty, &dist),
            (std::pair<const void*, const void*>{nullptr, nullptr}));
  EXPECT_FALSE(tree.isWithinDistance(other_empty, &dist, 100.0));
}

TEST(SimpleSTRtreeTest, InsertIgnoresNullEnvelopeAndEmptyGeometry) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> empty_point = factory->createPoint();
  std::unique_ptr<Point> valid_point =
      factory->createPoint(CoordinateXY(3.0, 4.0));

  SimpleSTRtree tree(4);
  Envelope null_env;
  ASSERT_TRUE(null_env.isNull());
  int ignored_item = -1;
  tree.insert(&null_env, &ignored_item);
  tree.insert(empty_point.get());

  TaggedBox box{Envelope(10.0, 12.0, 10.0, 12.0), 77};
  tree.insert(&box.env, &box);
  tree.insert(valid_point.get());

  EXPECT_FALSE(tree.getBuilt());
  EXPECT_EQ(tree.getNumLeafNodes(), 0U);

  SimpleSTRnode* root = tree.getRoot();
  ASSERT_NE(root, nullptr);
  EXPECT_TRUE(tree.getBuilt());
  EXPECT_EQ(tree.getNumLeafNodes(), 2U);

  const Envelope all(-100.0, 100.0, -100.0, 100.0);
  std::vector<void*> matches;
  tree.query(&all, matches);
  EXPECT_THAT(matches, UnorderedElementsAreArray(std::vector<void*>{
                           &box, static_cast<void*>(valid_point.get())}));
}

TEST(SimpleSTRtreeTest, IterateBeforeAndAfterBuild) {
  std::vector<TaggedBox> boxes = MakeBoxes(std::span(kGridBoxSpecs).first(4));
  SimpleSTRtree tree(2);
  tree.insert(&boxes[0].env, &boxes[0]);
  tree.insert(&boxes[1].env, &boxes[1]);

  CollectingVisitor pre_build_visitor;
  tree.iterate(pre_build_visitor);
  EXPECT_THAT(ExtractIds(pre_build_visitor.items()),
              UnorderedElementsAreArray({1, 2}));
  EXPECT_FALSE(tree.getBuilt());

  // Since iterate() does not build the tree, more inserts remain valid.
  tree.insert(&boxes[2].env, &boxes[2]);
  tree.insert(&boxes[3].env, &boxes[3]);

  ASSERT_NE(tree.getRoot(), nullptr);
  EXPECT_TRUE(tree.getBuilt());
  EXPECT_EQ(tree.getNumLeafNodes(), 4U);

  CollectingVisitor post_build_visitor;
  tree.iterate(post_build_visitor);
  EXPECT_THAT(ExtractIds(post_build_visitor.items()),
              UnorderedElementsAreArray({1, 2, 3, 4}));
}

TEST(SimpleSTRtreeTest, SingleItemAndExactCapacityLeafLayerQuery) {
  // Single-item tree: root is level 1 with 1 leaf child.
  SimpleSTRtree single_tree(4);
  TaggedBox single{Envelope(2.0, 4.0, 5.0, 7.0), 10};
  single_tree.insert(&single.env, &single);

  SimpleSTRnode* single_root = single_tree.getRoot();
  ASSERT_NE(single_root, nullptr);
  EXPECT_EQ(single_root->getLevel(), 1U);
  EXPECT_EQ(single_tree.getNumLeafNodes(), 1U);

  const Envelope touching(4.0, 6.0, 7.0, 9.0);
  std::vector<void*> matches;
  single_tree.query(&touching, matches);
  EXPECT_THAT(ExtractIds(matches), UnorderedElementsAreArray({10}));

  CollectingVisitor visitor;
  single_tree.query(&touching, visitor);
  EXPECT_THAT(ExtractIds(visitor.items()), UnorderedElementsAreArray({10}));

  const Envelope disjoint(10.0, 12.0, 10.0, 12.0);
  matches.clear();
  single_tree.query(&disjoint, matches);
  EXPECT_THAT(matches, IsEmpty());

  CollectingVisitor disjoint_visitor;
  single_tree.query(&disjoint, disjoint_visitor);
  EXPECT_THAT(disjoint_visitor.items(), IsEmpty());

  const Envelope null_search;
  matches.clear();
  single_tree.query(&null_search, matches);
  EXPECT_THAT(matches, IsEmpty());

  // 4 items with capacity 4 fills one parent exactly (no trailing partial
  // node).
  constexpr auto kFourSpecs = std::to_array<BoxSpec>({
      {.min_x = 0.0, .max_x = 1.0, .min_y = 0.0, .max_y = 1.0, .id = 1},
      {.min_x = 10.0, .max_x = 11.0, .min_y = 0.0, .max_y = 1.0, .id = 2},
      {.min_x = 0.0, .max_x = 1.0, .min_y = 10.0, .max_y = 11.0, .id = 3},
      {.min_x = 10.0, .max_x = 11.0, .min_y = 10.0, .max_y = 11.0, .id = 4},
  });
  std::vector<TaggedBox> four_boxes = MakeBoxes(kFourSpecs);
  SimpleSTRtree cap4_tree(4);
  for (TaggedBox& box : four_boxes) {
    cap4_tree.insert(&box.env, &box);
  }
  ASSERT_NE(cap4_tree.getRoot(), nullptr);
  EXPECT_EQ(cap4_tree.getRoot()->getLevel(), 1U);

  const Envelope left_half(-1.0, 2.0, -1.0, 12.0);
  matches.clear();
  cap4_tree.query(&left_half, matches);
  EXPECT_THAT(ExtractIds(matches), UnorderedElementsAreArray({1, 3}));

  CollectingVisitor left_visitor;
  cap4_tree.query(&left_half, left_visitor);
  EXPECT_THAT(ExtractIds(left_visitor.items()),
              UnorderedElementsAreArray({1, 3}));
}

TEST(SimpleSTRtreeTest, MultiLevelBuildExactSlicesAndQueries) {
  // 8 items with capacity 2 produces 2 slices of 4 items, each filling 2
  // full nodes of size 2 (leaving parent == nullptr at the end of each slice).
  std::vector<TaggedBox> eight_boxes =
      MakeBoxes(std::span(kGridBoxSpecs).first(8));
  SimpleSTRtree exact_tree(2);
  for (TaggedBox& box : eight_boxes) {
    exact_tree.insert(&box.env, &box);
  }
  SimpleSTRnode* exact_root = exact_tree.getRoot();
  ASSERT_NE(exact_root, nullptr);
  EXPECT_GE(exact_root->getLevel(), 2U);
  EXPECT_EQ(exact_tree.getNumLeafNodes(), 8U);

  // 15 items with capacity 2 produces partial slice parents and multiple
  // recursive createHigherLevels() invocations.
  std::vector<TaggedBox> boxes = MakeBoxes(kGridBoxSpecs);
  SimpleSTRtree tree(2);
  for (TaggedBox& box : boxes) {
    tree.insert(&box.env, &box);
  }

  SimpleSTRnode* root = tree.getRoot();
  ASSERT_NE(root, nullptr);
  EXPECT_GE(root->getLevel(), 3U);
  EXPECT_EQ(tree.getNumLeafNodes(), boxes.size());

  const std::array<Envelope, 7> queries = {
      Envelope(-100.0, 100.0, -100.0, 100.0),
      Envelope(0.0, 15.0, 0.0, 15.0),
      Envelope(-15.0, -3.0, -10.0, 10.0),
      Envelope(5.0, 5.0, 5.0, 5.0),
      Envelope(24.0, 35.0, 9.0, 27.0),
      Envelope(200.0, 210.0, 200.0, 210.0),
      Envelope(),
  };

  for (const Envelope& q : queries) {
    std::vector<int> expected = ExpectedIntersectingIds(boxes, q);

    std::vector<void*> matches;
    tree.query(&q, matches);
    EXPECT_THAT(ExtractIds(matches), UnorderedElementsAreArray(expected));

    CollectingVisitor visitor;
    tree.query(&q, visitor);
    EXPECT_THAT(ExtractIds(visitor.items()),
                UnorderedElementsAreArray(expected));
  }
}

TEST(SimpleSTRtreeTest, RemoveSingleLevelMultiLevelAndPrunesEmptySubtrees) {
  // Note: SimpleSTRtree::remove() dereferences `root` directly after build(),
  // so it must only be invoked on non-empty trees.

  // Single-level tree (root level 1).
  std::vector<TaggedBox> small_boxes =
      MakeBoxes(std::span(kGridBoxSpecs).first(3));
  SimpleSTRtree small_tree(4);
  for (TaggedBox& box : small_boxes) {
    small_tree.insert(&box.env, &box);
  }

  TaggedBox unregistered{Envelope(0.0, 1.0, 0.0, 1.0), 999};
  const Envelope disjoint_bounds(500.0, 510.0, 500.0, 510.0);
  EXPECT_FALSE(small_tree.remove(&disjoint_bounds, &small_boxes[0]));
  EXPECT_EQ(small_tree.getNumLeafNodes(), 3U);

  const Envelope all(-100.0, 100.0, -100.0, 100.0);
  EXPECT_FALSE(small_tree.remove(&all, &unregistered));
  EXPECT_EQ(small_tree.getNumLeafNodes(), 3U);

  EXPECT_TRUE(small_tree.remove(&small_boxes[1].env, &small_boxes[1]));
  EXPECT_EQ(small_tree.getNumLeafNodes(), 2U);
  EXPECT_FALSE(small_tree.remove(&small_boxes[1].env, &small_boxes[1]));

  std::vector<void*> matches;
  small_tree.query(&all, matches);
  EXPECT_THAT(ExtractIds(matches), UnorderedElementsAreArray({1, 3}));

  // Multi-level tree (capacity 2, 15 items): test wrong-bounds miss,
  // missing-item miss, and full drain pruning empty internal nodes.
  std::vector<TaggedBox> boxes = MakeBoxes(kGridBoxSpecs);
  SimpleSTRtree tree(2);
  for (TaggedBox& box : boxes) {
    tree.insert(&box.env, &box);
  }
  ASSERT_GE(tree.getRoot()->getLevel(), 3U);

  // Intersects root and some children, but disjoint from box[0] at
  // [30..32,20..22].
  const Envelope wrong_region(-15.0, 0.0, -10.0, 10.0);
  EXPECT_FALSE(tree.remove(&wrong_region, &boxes[0]));
  EXPECT_FALSE(tree.remove(&wrong_region, &unregistered));
  EXPECT_EQ(tree.getNumLeafNodes(), boxes.size());

  std::vector<int> remaining_ids;
  remaining_ids.reserve(boxes.size());
  for (const TaggedBox& box : boxes) {
    remaining_ids.push_back(box.id);
  }

  for (std::size_t i = 0; i < boxes.size(); ++i) {
    EXPECT_TRUE(tree.remove(&boxes[i].env, &boxes[i]));
    remaining_ids.erase(
        std::remove(remaining_ids.begin(), remaining_ids.end(), boxes[i].id),
        remaining_ids.end());

    EXPECT_EQ(tree.getNumLeafNodes(), remaining_ids.size());
    EXPECT_FALSE(tree.remove(&boxes[i].env, &boxes[i]));

    matches.clear();
    tree.query(&all, matches);
    EXPECT_THAT(ExtractIds(matches), UnorderedElementsAreArray(remaining_ids));
  }
  EXPECT_EQ(tree.getNumLeafNodes(), 0U);
}

TEST(SimpleSTRtreeTest, StreamOutputFormatting) {
  SimpleSTRtree empty_tree(5);
  std::ostringstream unbuilt_empty_out;
  unbuilt_empty_out << empty_tree;
  EXPECT_THAT(unbuilt_empty_out.str(), HasSubstr("nodeCapacity: 5"));
  EXPECT_THAT(unbuilt_empty_out.str(), HasSubstr("nodes.size(): 0"));
  EXPECT_THAT(unbuilt_empty_out.str(), HasSubstr("built: 0"));
  EXPECT_THAT(unbuilt_empty_out.str(), HasSubstr("tree: empty"));

  EXPECT_EQ(empty_tree.getRoot(), nullptr);
  std::ostringstream built_empty_out;
  built_empty_out << empty_tree;
  EXPECT_THAT(built_empty_out.str(), HasSubstr("built: 1"));
  EXPECT_THAT(built_empty_out.str(), HasSubstr("tree: empty"));

  std::vector<TaggedBox> boxes = MakeBoxes(std::span(kGridBoxSpecs).first(5));
  SimpleSTRtree tree(2);
  for (TaggedBox& box : boxes) {
    tree.insert(&box.env, &box);
  }
  std::ostringstream unbuilt_nonempty_out;
  unbuilt_nonempty_out << tree;
  EXPECT_THAT(unbuilt_nonempty_out.str(), HasSubstr("built: 0"));
  EXPECT_THAT(unbuilt_nonempty_out.str(), HasSubstr("tree: empty"));

  ASSERT_NE(tree.getRoot(), nullptr);
  std::ostringstream built_nonempty_out;
  built_nonempty_out << tree;
  EXPECT_THAT(built_nonempty_out.str(), HasSubstr("nodeCapacity: 2"));
  EXPECT_THAT(built_nonempty_out.str(), HasSubstr("nodes.size(): 5"));
  EXPECT_THAT(built_nonempty_out.str(), HasSubstr("built: 1"));
  EXPECT_THAT(built_nonempty_out.str(), HasSubstr("tree: "));
  EXPECT_THAT(built_nonempty_out.str(), HasSubstr("[0]"));
  EXPECT_THAT(built_nonempty_out.str(), HasSubstr("[1]"));
}

TEST(SimpleSTRtreeTest, InsertGeometriesAndGeometryItemDistance) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> pt_a = factory->createPoint(CoordinateXY(0.0, 0.0));
  std::unique_ptr<Point> pt_b = factory->createPoint(CoordinateXY(10.0, 0.0));
  std::unique_ptr<LineString> seg = factory->createLineString(
      CoordinateSequence{CoordinateXY(4.0, 2.0), CoordinateXY(4.0, 6.0)});
  Envelope poly_env(20.0, 24.0, 20.0, 24.0);
  std::unique_ptr<Geometry> poly = factory->toGeometry(&poly_env);
  std::unique_ptr<Point> empty_pt = factory->createPoint();

  SimpleSTRtree tree(2);
  tree.insert(pt_a.get());
  tree.insert(pt_b.get());
  tree.insert(seg.get());
  tree.insert(poly.get());
  tree.insert(empty_pt.get());

  // getNumLeafNodes() traverses `root`, so it remains 0 until build().
  EXPECT_FALSE(tree.getBuilt());
  EXPECT_EQ(tree.getNumLeafNodes(), 0U);

  const Envelope geom_search(3.0, 11.0, -1.0, 7.0);
  std::vector<void*> geom_matches;
  tree.query(&geom_search, geom_matches);
  EXPECT_TRUE(tree.getBuilt());
  EXPECT_EQ(tree.getNumLeafNodes(), 4U);
  EXPECT_THAT(geom_matches, UnorderedElementsAreArray(std::vector<void*>{
                                static_cast<void*>(pt_b.get()),
                                static_cast<void*>(seg.get())}));

  GeometryItemDistance geom_dist;
  std::unique_ptr<Point> query_pt =
      factory->createPoint(CoordinateXY(4.5, 3.0));
  const void* nearest = tree.nearestNeighbour(query_pt->getEnvelopeInternal(),
                                              query_pt.get(), &geom_dist);
  EXPECT_EQ(nearest, seg.get());

  SimpleSTRtree single_tree(2);
  single_tree.insert(pt_a.get());
  EXPECT_EQ(single_tree.nearestNeighbour(&geom_dist),
            (std::pair<const void*, const void*>{pt_a.get(), pt_a.get()}));

  SimpleSTRtree other_tree(2);
  std::unique_ptr<Point> other_near =
      factory->createPoint(CoordinateXY(10.5, 0.0));
  std::unique_ptr<Point> other_far =
      factory->createPoint(CoordinateXY(50.0, 50.0));
  other_tree.insert(other_near.get());
  other_tree.insert(other_far.get());

  const auto closest_pair = tree.nearestNeighbour(other_tree, &geom_dist);
  EXPECT_EQ(closest_pair.first, pt_b.get());
  EXPECT_EQ(closest_pair.second, other_near.get());

  EXPECT_FALSE(tree.isWithinDistance(other_tree, &geom_dist, 0.4));
  EXPECT_TRUE(tree.isWithinDistance(other_tree, &geom_dist, 0.5));
  EXPECT_TRUE(tree.isWithinDistance(other_tree, &geom_dist, 1.0));
}

TEST(SimpleSTRtreeTest, NearestNeighbourAllOverloadsAndWithinDistance) {
  constexpr auto kLeftSpecs = std::to_array<BoxSpec>({
      {.min_x = 0.0, .max_x = 1.0, .min_y = 0.0, .max_y = 1.0, .id = 1},
      {.min_x = 4.0, .max_x = 4.7, .min_y = 0.0, .max_y = 1.0, .id = 2},
      {.min_x = 5.0, .max_x = 5.5, .min_y = 0.0, .max_y = 1.0, .id = 3},
      {.min_x = 20.0, .max_x = 21.0, .min_y = 20.0, .max_y = 21.0, .id = 4},
      {.min_x = -15.0, .max_x = -14.0, .min_y = -15.0, .max_y = -14.0, .id = 5},
  });
  constexpr auto kRightSpecs = std::to_array<BoxSpec>({
      {.min_x = 8.0, .max_x = 9.0, .min_y = 0.0, .max_y = 1.0, .id = 101},
      {.min_x = 30.0, .max_x = 31.0, .min_y = 30.0, .max_y = 31.0, .id = 102},
      {.min_x = -30.0,
       .max_x = -29.0,
       .min_y = -30.0,
       .max_y = -29.0,
       .id = 103},
  });

  std::vector<TaggedBox> left_boxes = MakeBoxes(kLeftSpecs);
  std::vector<TaggedBox> right_boxes = MakeBoxes(kRightSpecs);

  SimpleSTRtree left_tree(2);
  for (TaggedBox& box : left_boxes) {
    left_tree.insert(&box.env, &box);
  }
  SimpleSTRtree right_tree(2);
  for (TaggedBox& box : right_boxes) {
    right_tree.insert(&box.env, &box);
  }
  SimpleSTRtree empty_tree(2);

  EnvelopeItemDistance dist(/*exclude_same_item=*/false);
  EnvelopeItemDistance distinct_dist(/*exclude_same_item=*/true);

  // Empty partner cases on both sides of &&.
  EXPECT_EQ(empty_tree.nearestNeighbour(left_tree, &dist),
            (std::pair<const void*, const void*>{nullptr, nullptr}));
  EXPECT_EQ(left_tree.nearestNeighbour(empty_tree, &dist),
            (std::pair<const void*, const void*>{nullptr, nullptr}));
  EXPECT_FALSE(empty_tree.isWithinDistance(left_tree, &dist, 100.0));
  EXPECT_FALSE(left_tree.isWithinDistance(empty_tree, &dist, 100.0));

  // Self nearest-neighbour finds closest distinct pair: id 2 (max_x = 4.7) and
  // id 3 (min_x = 5.0), separated by distance 0.3.
  const auto self_pair = left_tree.nearestNeighbour(&distinct_dist);
  ASSERT_NE(self_pair.first, nullptr);
  ASSERT_NE(self_pair.second, nullptr);
  const int self_id1 = static_cast<const TaggedBox*>(self_pair.first)->id;
  const int self_id2 = static_cast<const TaggedBox*>(self_pair.second)->id;
  EXPECT_THAT((std::array<int, 2>{self_id1, self_id2}),
              UnorderedElementsAreArray({2, 3}));

  // Single-item nearestNeighbour query (both disjoint and overlapping).
  TaggedBox probe{Envelope(-0.5, 0.2, 0.2, 0.8), 900};
  const void* nearest_to_probe =
      left_tree.nearestNeighbour(&probe.env, &probe, &dist);
  ASSERT_NE(nearest_to_probe, nullptr);
  EXPECT_EQ(static_cast<const TaggedBox*>(nearest_to_probe)->id, 1);

  TaggedBox far_probe{Envelope(25.0, 26.0, 25.0, 26.0), 901};
  const void* nearest_to_far =
      left_tree.nearestNeighbour(&far_probe.env, &far_probe, &dist);
  ASSERT_NE(nearest_to_far, nullptr);
  EXPECT_EQ(static_cast<const TaggedBox*>(nearest_to_far)->id, 4);

  // Tree-vs-tree nearestNeighbour: id 3 (max_x = 5.5) to id 101 (min_x = 8.0),
  // exact distance = 2.5.
  const auto cross_pair = left_tree.nearestNeighbour(right_tree, &dist);
  ASSERT_NE(cross_pair.first, nullptr);
  ASSERT_NE(cross_pair.second, nullptr);
  EXPECT_EQ(static_cast<const TaggedBox*>(cross_pair.first)->id, 3);
  EXPECT_EQ(static_cast<const TaggedBox*>(cross_pair.second)->id, 101);

  EXPECT_FALSE(left_tree.isWithinDistance(right_tree, &dist, 2.49));
  EXPECT_TRUE(left_tree.isWithinDistance(right_tree, &dist, 2.5));
  EXPECT_TRUE(left_tree.isWithinDistance(right_tree, &dist, 500.0));
}

TEST(SimpleSTRtreeTest, DefaultCapacityMultiLevelAndSpatialIndexInterface) {
  std::vector<TaggedBox> boxes;
  boxes.reserve(25);
  for (int i = 24; i >= 0; --i) {
    const int x = ((i % 5) * 7) % 5;
    const int y = ((i / 5) * 3) % 5;
    boxes.push_back(
        {Envelope(x * 10.0, x * 10.0 + 4.0, y * 10.0, y * 10.0 + 4.0), i + 1});
  }

  auto owned_tree = std::make_unique<SimpleSTRtree>();
  SimpleSTRtree* raw_tree = owned_tree.get();
  std::unique_ptr<SpatialIndex> index = std::move(owned_tree);

  for (TaggedBox& box : boxes) {
    index->insert(&box.env, &box);
  }
  EXPECT_FALSE(raw_tree->getBuilt());

  const Envelope window(8.0, 25.0, 8.0, 25.0);
  const std::vector<int> expected = ExpectedIntersectingIds(boxes, window);
  ASSERT_FALSE(expected.empty());

  std::vector<void*> matches;
  index->query(&window, matches);
  EXPECT_TRUE(raw_tree->getBuilt());
  EXPECT_EQ(raw_tree->getNodeCapacity(), 10U);
  EXPECT_EQ(raw_tree->getNumLeafNodes(), 25U);
  ASSERT_NE(raw_tree->root, nullptr);
  EXPECT_GE(raw_tree->root->getLevel(), 2U);
  EXPECT_THAT(ExtractIds(matches), UnorderedElementsAreArray(expected));

  CollectingVisitor visitor;
  index->query(&window, visitor);
  EXPECT_THAT(ExtractIds(visitor.items()), UnorderedElementsAreArray(expected));

  // Remove one matching item and verify query reflects the removal.
  const int removed_id = expected.front();
  TaggedBox* removed_box = nullptr;
  for (TaggedBox& box : boxes) {
    if (box.id == removed_id) {
      removed_box = &box;
      break;
    }
  }
  ASSERT_NE(removed_box, nullptr);
  EXPECT_TRUE(index->remove(&removed_box->env, removed_box));
  EXPECT_EQ(raw_tree->getNumLeafNodes(), 24U);

  matches.clear();
  index->query(&window, matches);
  std::vector<int> remaining_expected = expected;
  remaining_expected.erase(std::remove(remaining_expected.begin(),
                                       remaining_expected.end(), removed_id),
                           remaining_expected.end());
  EXPECT_THAT(ExtractIds(matches),
              UnorderedElementsAreArray(remaining_expected));
}

TEST(SimpleSTRtreeTest, RemoveSharedEnvelopesAndQueryVisitorAfterDrain) {
  constexpr auto kSharedSpecs = std::to_array<BoxSpec>({
      {.min_x = 2.0, .max_x = 4.0, .min_y = 2.0, .max_y = 4.0, .id = 1},
      {.min_x = 2.0, .max_x = 4.0, .min_y = 2.0, .max_y = 4.0, .id = 2},
      {.min_x = 10.0, .max_x = 12.0, .min_y = 10.0, .max_y = 12.0, .id = 3},
      {.min_x = 14.0, .max_x = 16.0, .min_y = 14.0, .max_y = 16.0, .id = 4},
      {.min_x = 40.0, .max_x = 42.0, .min_y = 40.0, .max_y = 42.0, .id = 5},
      {.min_x = 50.0, .max_x = 52.0, .min_y = 50.0, .max_y = 52.0, .id = 6},
  });
  std::vector<TaggedBox> boxes = MakeBoxes(kSharedSpecs);

  SimpleSTRtree tree(2);
  for (TaggedBox& box : boxes) {
    tree.insert(&box.env, &box);
  }
  ASSERT_NE(tree.getRoot(), nullptr);
  EXPECT_GE(tree.getRoot()->getLevel(), 2U);

  // Removing by pointer identity removes only boxes[0], keeping boxes[1].
  const Envelope point_bounds(3.0, 3.0, 3.0, 3.0);
  EXPECT_TRUE(tree.remove(&point_bounds, &boxes[0]));
  EXPECT_FALSE(tree.remove(&point_bounds, &boxes[0]));
  EXPECT_EQ(tree.getNumLeafNodes(), 5U);

  const Envelope all(-10.0, 100.0, -10.0, 100.0);
  std::vector<void*> matches;
  tree.query(&all, matches);
  EXPECT_THAT(ExtractIds(matches), UnorderedElementsAreArray({2, 3, 4, 5, 6}));

  // Bounds intersect root and nearby subtrees, missing boxes[5]'s leaf parent.
  const Envelope near_only(0.0, 20.0, 0.0, 20.0);
  EXPECT_FALSE(tree.remove(&near_only, &boxes[5]));
  EXPECT_EQ(tree.getNumLeafNodes(), 5U);

  for (std::size_t i = 1; i < boxes.size(); ++i) {
    EXPECT_TRUE(tree.remove(&boxes[i].env, &boxes[i]));
  }
  EXPECT_EQ(tree.getNumLeafNodes(), 0U);

  matches.clear();
  tree.query(&all, matches);
  EXPECT_THAT(matches, IsEmpty());

  CollectingVisitor visitor;
  tree.query(&all, visitor);
  EXPECT_THAT(visitor.items(), IsEmpty());
}

}  // namespace
}  // namespace geos_google
