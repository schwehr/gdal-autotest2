// Copyright 2026 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests for GDAL's quad tree implementation (cpl_quad_tree.cpp).

#include "third_party/gdal/port/cpl_quad_tree.h"

#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_conv.h"

namespace autotest2 {
namespace {

struct Feature {
  int id;
  CPLRectObj bounds;
};

void GetFeatureBounds(const void* hFeature, CPLRectObj* pBounds) {
  const auto* f = static_cast<const Feature*>(hFeature);
  *pBounds = f->bounds;
}

void GetFeatureBoundsEx(const void* hFeature, void* pUserData,
                        CPLRectObj* pBounds) {
  int* counter = static_cast<int*>(pUserData);
  if (counter != nullptr) {
    (*counter)++;
  }
  const auto* f = static_cast<const Feature*>(hFeature);
  *pBounds = f->bounds;
}

int ForeachCollectAll(void* pElt, void* pUserData) {
  auto* out = static_cast<std::vector<Feature*>*>(pUserData);
  out->push_back(static_cast<Feature*>(pElt));
  return TRUE;
}

int ForeachStopEarly(void* pElt, void* pUserData) {
  auto* out = static_cast<std::vector<Feature*>*>(pUserData);
  out->push_back(static_cast<Feature*>(pElt));
  return out->size() >= 2 ? FALSE : TRUE;
}

void CustomDumpFeature(const void* hFeature, int nIndentLevel,
                       void* pUserData) {
  (void)hFeature;
  (void)nIndentLevel;
  int* dump_calls = static_cast<int*>(pUserData);
  (*dump_calls)++;
}

TEST(CplQuadTreeTest, AdvisedMaxDepth) {
  EXPECT_GT(CPLQuadTreeGetAdvisedMaxDepth(10), 0);
  EXPECT_GT(CPLQuadTreeGetAdvisedMaxDepth(1000), 0);
  // Large feature count caps at MAX_DEFAULT_TREE_DEPTH (12).
  EXPECT_EQ(CPLQuadTreeGetAdvisedMaxDepth(100000000), 12);
}

TEST(CplQuadTreeTest, Alg1WithGetBoundsAndForceSubNodesAndDumpStats) {
  // Global bounds 200x100.  With the default split ratio (0.55) the root is
  // split along X into two overlapping halves {0..110} and {90..200}; each
  // half is 110x100 so it is split along X again, giving the root quads
  //   quad1 = {0, 0, 60.5, 100}      quad2 = {49.5, 0, 110, 100}
  //   quad3 = {90, 0, 150.5, 100}    quad4 = {139.5, 0, 200, 100}
  // Each 60.5x100 quad in turn splits along Y and then X.
  const CPLRectObj global_bounds = {0.0, 0.0, 200.0, 100.0};
  CPLQuadTree* tree = CPLQuadTreeCreate(&global_bounds, GetFeatureBounds);
  ASSERT_NE(tree, nullptr);

  CPLQuadTreeSetBucketCapacity(tree, 0);  // Ignored (<= 0).
  CPLQuadTreeSetBucketCapacity(tree, 2);

  std::vector<Feature> features;
  features.reserve(40);
  // 12 small 2x2 features on a 4x3 grid (x in {5, 50, 95, 140},
  // y in {5, 35, 65}).  Each one fits in a root quad and in a sub-quad, so
  // quad1, quad2 and quad3 all get split; quad4 stays empty.
  for (int i = 0; i < 12; ++i) {
    double x = (i % 4) * 45.0 + 5.0;
    double y = (i / 4) * 30.0 + 5.0;
    features.push_back({i, {x, y, x + 2.0, y + 2.0}});
    CPLQuadTreeInsert(tree, &features.back());
  }

  // Six identical features fully contained in quad2 {49.5, 0, 110, 100} but
  // straddling all four of its sub-quads.  Without ForceUseOfSubNodes they
  // accumulate in quad2's own list, exceeding the bucket capacity and
  // triggering the pahFeatures reallocation path.
  for (int i = 12; i < 18; ++i) {
    features.push_back({i, {90.0, 40.0, 110.0, 60.0}});
    CPLQuadTreeInsert(tree, &features.back());
  }

  // Now enable ForceUseOfSubNodes and insert:
  //  - 100: contained in quad2's sub-quad {76.225, 0, 110, 55}, which is
  //    already at capacity.  ForceUseOfSubNodes lets that node split (depth 3)
  //    even though no further sub-quad fully contains the two existing
  //    features.
  //  - 101: overlaps all 4 root quads, so it is kept in the root node.
  //  - 102: overlaps only quad1 and quad2, so it is split: the quad1 part is
  //    split again across two of quad1's sub-quads, the quad2 part lands in
  //    one of quad2's sub-quads.  The feature is therefore stored in 3 leaves.
  CPLQuadTreeForceUseOfSubNodes(tree);
  features.push_back({100, {95.0, 5.0, 105.0, 15.0}});
  CPLQuadTreeInsert(tree, &features.back());
  features.push_back({101, {10.0, 10.0, 190.0, 90.0}});
  CPLQuadTreeInsert(tree, &features.back());
  features.push_back({102, {5.0, 80.0, 70.0, 90.0}});
  CPLQuadTreeInsert(tree, &features.back());

  constexpr int kNumInserted = 21;
  // 21 inserts, with feature 102 present in 3 leaves.
  constexpr int kNumLeafEntries = kNumInserted + 2;
  // root + 4 root quads + 4 sub-quads under each of quad1, quad2, quad3 +
  // 4 sub-sub-quads under quad2's second sub-quad.
  constexpr int kNumNodes = 1 + 4 + 3 * 4 + 4;

  // Stats with nullptr and non-nullptr outputs.
  CPLQuadTreeGetStats(tree, nullptr, nullptr, nullptr, nullptr);
  int feat_count = 0, node_count = 0, max_depth = 0, max_bucket = 0;
  CPLQuadTreeGetStats(tree, &feat_count, &node_count, &max_depth, &max_bucket);
  EXPECT_EQ(feat_count, kNumInserted);
  EXPECT_EQ(node_count, kNumNodes);
  EXPECT_EQ(max_depth, 3);
  EXPECT_EQ(max_bucket, 6);  // quad2 holds features 12..17.

  // Dump visits every leaf entry, so the split feature is reported 3 times.
  int dump_calls = 0;
  CPLQuadTreeDump(tree, CustomDumpFeature, &dump_calls);
  EXPECT_EQ(dump_calls, kNumLeafEntries);

  // Search with pnFeatureCount == nullptr and != nullptr, plus non-overlapping
  // AOIs in all 4 directions (left, right, below, above).
  {
    void** res = CPLQuadTreeSearch(tree, &global_bounds, nullptr);
    CPLFree(res);
  }
  {
    int count = 0;
    void** res = CPLQuadTreeSearch(tree, &global_bounds, &count);
    EXPECT_EQ(count, kNumLeafEntries);
    CPLFree(res);
  }
  const CPLRectObj kOutsideRects[] = {
      {-20.0, 10.0, -5.0, 20.0},
      {210.0, 10.0, 220.0, 20.0},
      {10.0, -20.0, 20.0, -5.0},
      {10.0, 110.0, 20.0, 120.0},
  };
  for (const auto& rect : kOutsideRects) {
    int count = -1;
    void** res = CPLQuadTreeSearch(tree, &rect, &count);
    EXPECT_EQ(count, 0);
    CPLFree(res);
    EXPECT_FALSE(CPLQuadTreeHasMatch(tree, &rect));
  }

  // HasMatch on root feature (101), subnode feature (0), and empty area
  // inside bounds.
  const CPLRectObj root_match = {99.0, 49.0, 101.0, 51.0};
  EXPECT_TRUE(CPLQuadTreeHasMatch(tree, &root_match));
  const CPLRectObj subnode_match = {5.5, 5.5, 6.5, 6.5};
  EXPECT_TRUE(CPLQuadTreeHasMatch(tree, &subnode_match));
  const CPLRectObj empty_inside = {198.0, 98.0, 199.0, 99.0};
  EXPECT_FALSE(CPLQuadTreeHasMatch(tree, &empty_inside));

  // Foreach: full and early stop.
  std::vector<Feature*> visited;
  CPLQuadTreeForeach(tree, ForeachCollectAll, &visited);
  EXPECT_EQ(static_cast<int>(visited.size()), kNumLeafEntries);

  visited.clear();
  CPLQuadTreeForeach(tree, ForeachStopEarly, &visited);
  EXPECT_EQ(visited.size(), 2u);

  // Remove features (including non-last and last elements in nodes and
  // subnodes, and the feature that was split across 3 leaves).
  for (auto& f : features) {
    CPLQuadTreeRemove(tree, &f, nullptr);
  }
  CPLQuadTreeGetStats(tree, &feat_count, nullptr, nullptr, &max_bucket);
  EXPECT_EQ(feat_count, 0);
  EXPECT_EQ(max_bucket, 0);
  EXPECT_FALSE(CPLQuadTreeHasMatch(tree, &global_bounds));

  CPLQuadTreeDestroy(tree);
}

TEST(CplQuadTreeTest, CreateExAndNullCallbacksAndAlg2) {
  WithQuietHandler quiet_handler;
  const CPLRectObj global_bounds = {0.0, 0.0, 100.0, 200.0};

  // 1) Tree created with CPLQuadTreeCreateEx.
  int ex_calls = 0;
  CPLQuadTree* tree_ex =
      CPLQuadTreeCreateEx(&global_bounds, GetFeatureBoundsEx, &ex_calls);
  CPLQuadTreeSetBucketCapacity(tree_ex, 2);

  std::vector<Feature> features;
  features.reserve(10);
  for (int i = 0; i < 6; ++i) {
    double x = (i % 2) * 60.0 + 5.0;
    double y = (i / 2) * 60.0 + 5.0;
    features.push_back({i, {x, y, x + 2.0, y + 2.0}});
    CPLQuadTreeInsert(tree_ex, &features.back());
  }
  EXPECT_GT(ex_calls, 0);

  const CPLRectObj aoi = {0.0, 0.0, 100.0, 200.0};
  EXPECT_TRUE(CPLQuadTreeHasMatch(tree_ex, &aoi));
  int count = 0;
  void** res = CPLQuadTreeSearch(tree_ex, &aoi, &count);
  EXPECT_EQ(count, 6);
  CPLFree(res);

  for (auto& f : features) {
    CPLQuadTreeRemove(tree_ex, &f, nullptr);
  }
  CPLQuadTreeDestroy(tree_ex);

  // 2) Tree with nullptr callback (stores pasBounds in nodes), testing both
  // Alg1 and Alg2 (when CPLQuadTreeSetMaxDepth is explicitly called).
  for (int explicit_depth : {0, 3}) {
    CPLQuadTree* tree_null = CPLQuadTreeCreate(&global_bounds, nullptr);
    CPLQuadTreeSetBucketCapacity(tree_null, 2);
    if (explicit_depth > 0) {
      CPLQuadTreeSetMaxDepth(tree_null, explicit_depth);
    }

    // Calling CPLQuadTreeInsert or CPLQuadTreeRemove(..., nullptr) without
    // bounds callback logs an error and returns safely.
    CPLQuadTreeInsert(tree_null, &features[0]);
    CPLQuadTreeRemove(tree_null, &features[0], nullptr);

    for (auto& f : features) {
      CPLQuadTreeInsertWithBounds(tree_null, &f, &f.bounds);
    }
    // Also insert center-spanning features to exercise pasBounds reallocations.
    Feature center1 = {10, {10.0, 10.0, 90.0, 190.0}};
    Feature center2 = {11, {15.0, 15.0, 85.0, 185.0}};
    Feature center3 = {12, {20.0, 20.0, 80.0, 180.0}};
    CPLQuadTreeInsertWithBounds(tree_null, &center1, &center1.bounds);
    CPLQuadTreeInsertWithBounds(tree_null, &center2, &center2.bounds);
    CPLQuadTreeInsertWithBounds(tree_null, &center3, &center3.bounds);

    EXPECT_TRUE(CPLQuadTreeHasMatch(tree_null, &aoi));
    res = CPLQuadTreeSearch(tree_null, &aoi, &count);
    EXPECT_EQ(count, 9);
    CPLFree(res);

    CPLQuadTreeRemove(tree_null, &center1, &center1.bounds);
    CPLQuadTreeRemove(tree_null, &center2, &center2.bounds);
    CPLQuadTreeRemove(tree_null, &center3, &center3.bounds);
    for (auto& f : features) {
      CPLQuadTreeRemove(tree_null, &f, &f.bounds);
    }
    CPLQuadTreeDestroy(tree_null);
  }
}

}  // namespace
}  // namespace autotest2
