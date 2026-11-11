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

// Unit tests for third_party/geos/src/coverage/CleanCoverage.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for TopologyException checks.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/coverage/CleanCoverage.cpp

#include "third_party/geos/include/geos/coverage/CleanCoverage.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/relateng/RelateNG.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::coverage::CleanCoverage;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::operation::relateng::RelateNG;
using geos::util::TopologyException;

std::unique_ptr<Polygon> ReadPolygon(const GeometryFactory* factory,
                                     const std::string& wkt) {
  WKTReader reader(factory);
  return reader.read<Polygon>(wkt);
}

void ExpectTopoEqual(const Geometry* actual, const Geometry* expected) {
  ASSERT_NE(actual, nullptr);
  ASSERT_NE(expected, nullptr);
  EXPECT_TRUE(actual->isValid()) << "actual=" << actual->toText();
  EXPECT_EQ(actual->getGeometryTypeId(), expected->getGeometryTypeId());
  EXPECT_DOUBLE_EQ(actual->getArea(), expected->getArea());
  EXPECT_TRUE(actual->equals(expected))
      << "actual=" << actual->toText() << " expected=" << expected->toText();
}

class FixedIndexMergeStrategy : public CleanCoverage::MergeStrategy {
 public:
  explicit FixedIndexMergeStrategy(std::size_t fixed_target)
      : fixed_target_(fixed_target) {}

  std::size_t getTarget() const override { return target_; }

  void checkMergeTarget(std::size_t area_index,
                        CleanCoverage::CleanArea* /*clean_area*/,
                        const Polygon* /*poly*/) override {
    if (area_index == fixed_target_) {
      target_ = area_index;
    }
  }

 private:
  std::size_t fixed_target_;
  std::size_t target_ = geos::INDEX_UNKNOWN;
};

TEST(CleanCoverageTest, ZeroSizeCoverageProducesEmptyResult) {
  CleanCoverage cov(0);
  std::vector<const Polygon*> gaps;
  cov.mergeGaps(gaps);

  std::vector<std::unique_ptr<Geometry>> result = cov.toCoverage(nullptr);
  EXPECT_TRUE(result.empty());
}

TEST(CleanCoverageTest, NullSlotsProduceEmptyPolygonsInToCoverage) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto poly = ReadPolygon(factory.get(), "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");

  CleanCoverage cov(3);
  cov.add(1, poly.get());

  // Also exercise createIndex() with null and non-null coverage entries.
  std::vector<const Polygon*> no_gaps;
  cov.mergeGaps(no_gaps);

  std::vector<std::unique_ptr<Geometry>> result = cov.toCoverage(factory.get());
  ASSERT_EQ(result.size(), 3u);
  EXPECT_TRUE(result[0]->isEmpty());
  EXPECT_EQ(result[0]->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  ExpectTopoEqual(result[1].get(), poly.get());
  EXPECT_TRUE(result[2]->isEmpty());
  EXPECT_EQ(result[2]->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
}

TEST(CleanCoverageTest, AddMultiplePolygonsToSameSlotUnionsResult) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto left = ReadPolygon(factory.get(), "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  auto right =
      ReadPolygon(factory.get(), "POLYGON ((2 0, 5 0, 5 2, 2 2, 2 0))");
  auto expected =
      ReadPolygon(factory.get(), "POLYGON ((0 0, 5 0, 5 2, 0 2, 0 0))");

  CleanCoverage cov(1);
  cov.add(0, left.get());
  cov.add(0, right.get());

  std::vector<std::unique_ptr<Geometry>> result = cov.toCoverage(factory.get());
  ASSERT_EQ(result.size(), 1u);
  EXPECT_DOUBLE_EQ(result[0]->getArea(), 10.0);
  ExpectTopoEqual(result[0].get(), expected.get());
}

TEST(CleanCoverageTest, CleanAreaEmptyAndMultiPolygonOperations) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto left = ReadPolygon(factory.get(), "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  auto middle =
      ReadPolygon(factory.get(), "POLYGON ((2 0, 4 0, 4 2, 2 2, 2 0))");
  auto disjoint =
      ReadPolygon(factory.get(), "POLYGON ((6 0, 8 0, 8 2, 6 2, 6 0))");
  auto top_bar =
      ReadPolygon(factory.get(), "POLYGON ((0 2, 8 2, 8 4, 0 4, 0 2))");
  auto point_touch =
      ReadPolygon(factory.get(), "POLYGON ((-2 -2, 0 -2, 0 0, -2 0, -2 -2))");
  auto far_away = ReadPolygon(factory.get(),
                              "POLYGON ((20 20, 21 20, 21 21, 20 21, 20 20))");
  auto overlapping =
      ReadPolygon(factory.get(), "POLYGON ((1 1, 3 1, 3 3, 1 3, 1 1))");

  CleanCoverage::CleanArea empty_area;
  EXPECT_TRUE(empty_area.getEnvelope()->isNull());
  EXPECT_DOUBLE_EQ(empty_area.getArea(), 0.0);
  EXPECT_DOUBLE_EQ(empty_area.getBorderLength(left.get()), 0.0);
  auto rel_top = RelateNG::prepare(top_bar.get());
  EXPECT_FALSE(empty_area.isAdjacent(*rel_top));
  EXPECT_EQ(empty_area.getUnion(), nullptr);

  CleanCoverage::CleanArea disjoint_pair;
  disjoint_pair.add(left.get());
  disjoint_pair.add(disjoint.get());
  const Envelope* env = disjoint_pair.getEnvelope();
  EXPECT_DOUBLE_EQ(env->getMinX(), 0.0);
  EXPECT_DOUBLE_EQ(env->getMaxX(), 8.0);
  EXPECT_DOUBLE_EQ(env->getMinY(), 0.0);
  EXPECT_DOUBLE_EQ(env->getMaxY(), 2.0);
  EXPECT_DOUBLE_EQ(disjoint_pair.getArea(), 8.0);
  EXPECT_DOUBLE_EQ(disjoint_pair.getBorderLength(top_bar.get()), 4.0);
  EXPECT_DOUBLE_EQ(disjoint_pair.getBorderLength(point_touch.get()), 0.0);
  EXPECT_DOUBLE_EQ(disjoint_pair.getBorderLength(far_away.get()), 0.0);

  // First polygon (left) is not adjacent to an edge on disjoint's right,
  // while second polygon (disjoint) is adjacent.
  auto right_of_disjoint =
      ReadPolygon(factory.get(), "POLYGON ((8 0, 10 0, 10 2, 8 2, 8 0))");
  auto rel_right = RelateNG::prepare(right_of_disjoint.get());
  EXPECT_TRUE(disjoint_pair.isAdjacent(*rel_right));

  auto rel_point_touch = RelateNG::prepare(point_touch.get());
  EXPECT_FALSE(disjoint_pair.isAdjacent(*rel_point_touch));

  auto rel_overlap = RelateNG::prepare(overlapping.get());
  EXPECT_FALSE(disjoint_pair.isAdjacent(*rel_overlap));

  std::unique_ptr<Geometry> mp = disjoint_pair.getUnion();
  ASSERT_NE(mp, nullptr);
  EXPECT_EQ(mp->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_DOUBLE_EQ(mp->getArea(), 8.0);

  CleanCoverage::CleanArea adjacent_pair;
  adjacent_pair.add(left.get());
  adjacent_pair.add(middle.get());
  std::unique_ptr<Geometry> merged = adjacent_pair.getUnion();
  ASSERT_NE(merged, nullptr);
  EXPECT_EQ(merged->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(merged->getArea(), 8.0);

  // CoverageUnion uses BoundaryChainNoder, which expects valid non-overlapping
  // coverage polygons. Duplicate polygons cancel every boundary segment and
  // trigger the area-difference TopologyException in CoverageUnion.
  auto left_copy =
      ReadPolygon(factory.get(), "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  CleanCoverage::CleanArea duplicate_pair;
  duplicate_pair.add(left.get());
  duplicate_pair.add(left_copy.get());
  EXPECT_THROW(duplicate_pair.getUnion(), TopologyException);
}

TEST(CleanCoverageTest, MergeOverlapBorderStrategyPicksLongestSharedBorder) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Overlap shares border of length 1 with parent 0 (left) and length 3 with
  // parent 1 (bottom).
  auto parent0 =
      ReadPolygon(factory.get(), "POLYGON ((0 0, 2 0, 2 1, 0 1, 0 0))");
  auto parent1 =
      ReadPolygon(factory.get(), "POLYGON ((2 -2, 5 -2, 5 0, 2 0, 2 -2))");
  auto overlap =
      ReadPolygon(factory.get(), "POLYGON ((2 0, 5 0, 5 1, 2 1, 2 0))");
  auto expected_p1 =
      ReadPolygon(factory.get(), "POLYGON ((2 -2, 5 -2, 5 1, 2 1, 2 -2))");

  CleanCoverage cov(2);
  cov.add(0, parent0.get());
  cov.add(1, parent1.get());

  CleanCoverage::BorderMergeStrategy strategy;
  std::vector<std::size_t> parents = {0, 1};
  cov.mergeOverlap(overlap.get(), strategy, parents);
  EXPECT_EQ(strategy.getTarget(), 1u);

  std::vector<std::unique_ptr<Geometry>> result = cov.toCoverage(factory.get());
  ExpectTopoEqual(result[0].get(), parent0.get());
  ExpectTopoEqual(result[1].get(), expected_p1.get());
}

TEST(CleanCoverageTest, MergeOverlapBorderStrategyBreaksTiesToSmallestIndex) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Parent 0 (left) and parent 2 (right) each share a border of length 2.0
  // with overlap; parent 1 (top) shares length 1.0. Unsorted parentIndexes
  // must still resolve the tie to parent 0.
  auto parent0 =
      ReadPolygon(factory.get(), "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  auto parent1 =
      ReadPolygon(factory.get(), "POLYGON ((2 2, 3 2, 3 4, 2 4, 2 2))");
  auto parent2 =
      ReadPolygon(factory.get(), "POLYGON ((3 0, 5 0, 5 2, 3 2, 3 0))");
  auto overlap =
      ReadPolygon(factory.get(), "POLYGON ((2 0, 3 0, 3 2, 2 2, 2 0))");
  auto expected_p0 =
      ReadPolygon(factory.get(), "POLYGON ((0 0, 3 0, 3 2, 0 2, 0 0))");

  CleanCoverage cov(3);
  cov.add(0, parent0.get());
  cov.add(1, parent1.get());
  cov.add(2, parent2.get());

  CleanCoverage::BorderMergeStrategy strategy;
  std::vector<std::size_t> parents = {2, 1, 0};
  cov.mergeOverlap(overlap.get(), strategy, parents);
  EXPECT_EQ(strategy.getTarget(), 0u);

  std::vector<std::unique_ptr<Geometry>> result = cov.toCoverage(factory.get());
  ExpectTopoEqual(result[0].get(), expected_p0.get());
  ExpectTopoEqual(result[1].get(), parent1.get());
  ExpectTopoEqual(result[2].get(), parent2.get());
}

TEST(CleanCoverageTest,
     AreaMergeStrategySelectsMaxOrMinAreaIncludingNullSlots) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto large =
      ReadPolygon(factory.get(), "POLYGON ((0 0, 5 0, 5 2, 0 2, 0 0))");
  auto small =
      ReadPolygon(factory.get(), "POLYGON ((6 0, 8 0, 8 2, 6 2, 6 0))");
  auto overlap =
      ReadPolygon(factory.get(), "POLYGON ((5 0, 6 0, 6 2, 5 2, 5 0))");

  // Max area: large (area 10) beats small (area 4) and null slot 2 (area 0).
  {
    CleanCoverage cov(3);
    cov.add(0, small.get());
    cov.add(1, large.get());

    CleanCoverage::AreaMergeStrategy max_strategy(/*isMax=*/true);
    std::vector<std::size_t> parents = {2, 1, 0};
    cov.mergeOverlap(overlap.get(), max_strategy, parents);
    EXPECT_EQ(max_strategy.getTarget(), 1u);

    std::vector<std::unique_ptr<Geometry>> result =
        cov.toCoverage(factory.get());
    EXPECT_DOUBLE_EQ(result[0]->getArea(), 4.0);
    EXPECT_DOUBLE_EQ(result[1]->getArea(), 12.0);
    EXPECT_TRUE(result[2]->isEmpty());
  }

  // Min area among non-null slots: small (area 4) beats large (area 10).
  {
    CleanCoverage cov(2);
    cov.add(0, large.get());
    cov.add(1, small.get());

    CleanCoverage::AreaMergeStrategy min_strategy(/*isMax=*/false);
    std::vector<std::size_t> parents = {1, 0};
    cov.mergeOverlap(overlap.get(), min_strategy, parents);
    EXPECT_EQ(min_strategy.getTarget(), 1u);

    std::vector<std::unique_ptr<Geometry>> result =
        cov.toCoverage(factory.get());
    EXPECT_DOUBLE_EQ(result[0]->getArea(), 10.0);
    EXPECT_DOUBLE_EQ(result[1]->getArea(), 6.0);
  }

  // Min area with a null slot treats null area as 0.0.
  {
    CleanCoverage cov(2);
    cov.add(1, small.get());

    CleanCoverage::AreaMergeStrategy min_strategy(/*isMax=*/false);
    std::vector<std::size_t> parents = {1, 0};
    cov.mergeOverlap(overlap.get(), min_strategy, parents);
    EXPECT_EQ(min_strategy.getTarget(), 0u);
  }
}

TEST(CleanCoverageTest, IndexAndCustomStrategiesAndFindMergeTargetEdgeCases) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto poly0 =
      ReadPolygon(factory.get(), "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  auto poly1 =
      ReadPolygon(factory.get(), "POLYGON ((1 0, 2 0, 2 1, 1 1, 1 0))");
  auto poly2 =
      ReadPolygon(factory.get(), "POLYGON ((2 0, 3 0, 3 1, 2 1, 2 0))");
  auto overlap =
      ReadPolygon(factory.get(), "POLYGON ((1 1, 2 1, 2 2, 1 2, 1 1))");

  const std::size_t unknown_idx = geos::INDEX_UNKNOWN;

  // Empty parentIndexes yields INDEX_UNKNOWN without touching cov.
  {
    std::vector<std::unique_ptr<CleanCoverage::CleanArea>> cov(2);
    std::vector<std::size_t> empty_parents;
    CleanCoverage::BorderMergeStrategy border_strategy;
    EXPECT_EQ(CleanCoverage::findMergeTarget(overlap.get(), border_strategy,
                                             empty_parents, cov),
              unknown_idx);
  }

  // BorderMergeStrategy with all null parent areas selects smallest index.
  {
    std::vector<std::unique_ptr<CleanCoverage::CleanArea>> cov(4);
    std::vector<std::size_t> parents = {3, 1, 2};
    CleanCoverage::BorderMergeStrategy border_strategy;
    EXPECT_EQ(CleanCoverage::findMergeTarget(overlap.get(), border_strategy,
                                             parents, cov),
              1u);
  }

  // IndexMergeStrategy(true) leaves m_targetIndex at INDEX_UNKNOWN because
  // INDEX_UNKNOWN == SIZE_MAX cannot be exceeded.
  {
    std::vector<std::unique_ptr<CleanCoverage::CleanArea>> cov(3);
    cov[0] = std::make_unique<CleanCoverage::CleanArea>();
    cov[0]->add(poly0.get());
    std::vector<std::size_t> parents = {0, 1, 2};
    CleanCoverage::IndexMergeStrategy max_idx_strategy(/*isMax=*/true);
    EXPECT_EQ(CleanCoverage::findMergeTarget(overlap.get(), max_idx_strategy,
                                             parents, cov),
              unknown_idx);
  }

  // IndexMergeStrategy(false) merges into lowest parent index regardless of
  // geometry area or shared border.
  {
    CleanCoverage cov(3);
    cov.add(0, poly0.get());
    cov.add(1, poly1.get());
    cov.add(2, poly2.get());

    CleanCoverage::IndexMergeStrategy min_idx_strategy(/*isMax=*/false);
    std::vector<std::size_t> parents = {2, 1};
    cov.mergeOverlap(overlap.get(), min_idx_strategy, parents);
    EXPECT_EQ(min_idx_strategy.getTarget(), 1u);

    FixedIndexMergeStrategy custom_strategy(2);
    std::vector<std::size_t> custom_parents = {0, 2};
    cov.mergeOverlap(overlap.get(), custom_strategy, custom_parents);
    EXPECT_EQ(custom_strategy.getTarget(), 2u);

    std::vector<std::unique_ptr<Geometry>> result =
        cov.toCoverage(factory.get());
    EXPECT_DOUBLE_EQ(result[0]->getArea(), 1.0);
    EXPECT_DOUBLE_EQ(result[1]->getArea(), 2.0);
    EXPECT_DOUBLE_EQ(result[2]->getArea(), 2.0);
  }
}

TEST(CleanCoverageTest,
     MergeGapsMergesLongestBorderUpdatesIndexAndDiscardsInvalid) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Area 0: [0,2] x [0,4] (shares length 3 with gap1)
  auto area0 =
      ReadPolygon(factory.get(), "POLYGON ((0 0, 2 0, 2 4, 0 4, 0 0))");
  // Area 1: [2,6] x [0,1] (shares length 4 with gap1)
  auto area1 =
      ReadPolygon(factory.get(), "POLYGON ((2 0, 6 0, 6 1, 2 1, 2 0))");
  // Area 2: [0,2] x [4,5] (envelope touches gap1 at (2,4) only -> not adjacent)
  auto area2 =
      ReadPolygon(factory.get(), "POLYGON ((0 4, 2 4, 2 5, 0 5, 0 4))");
  // Area 3: [10,12] x [10,12] (disjoint)
  auto area3 = ReadPolygon(factory.get(),
                           "POLYGON ((10 10, 12 10, 12 12, 10 12, 10 10))");
  // Slot 4 intentionally left null.

  // gap1: [2,6] x [1,4] -> merges into area1 (border 4 > 3).
  auto gap1 = ReadPolygon(factory.get(), "POLYGON ((2 1, 6 1, 6 4, 2 4, 2 1))");
  // gap2: [6,8] x [1,4] -> only adjacent to gap1 (requires updated index!).
  auto gap2 = ReadPolygon(factory.get(), "POLYGON ((6 1, 8 1, 8 4, 6 4, 6 1))");
  // gap3: completely disjoint from all areas -> discarded.
  auto gap3_disjoint = ReadPolygon(
      factory.get(), "POLYGON ((20 20, 21 20, 21 21, 20 21, 20 20))");
  // gap4: touches area3 only at corner (12,12) -> discarded.
  auto gap4_point_touch = ReadPolygon(
      factory.get(), "POLYGON ((12 12, 13 12, 13 13, 12 13, 12 12))");
  // gap5: interior overlaps area3 -> not ADJACENT ("F***1****") -> discarded.
  auto gap5_overlap = ReadPolygon(
      factory.get(),
      "POLYGON ((10.5 10.5, 11.5 10.5, 11.5 11.5, 10.5 11.5, 10.5 10.5))");

  auto expected_area1 = ReadPolygon(
      factory.get(), "POLYGON ((2 0, 6 0, 6 1, 8 1, 8 4, 2 4, 2 0))");

  CleanCoverage cov(5);
  cov.add(0, area0.get());
  cov.add(1, area1.get());
  cov.add(2, area2.get());
  cov.add(3, area3.get());

  std::vector<const Polygon*> gaps = {
      gap1.get(),          gap2.get(),
      gap3_disjoint.get(), gap4_point_touch.get(),
      gap5_overlap.get(),
  };
  cov.mergeGaps(gaps);

  std::vector<std::unique_ptr<Geometry>> result = cov.toCoverage(factory.get());
  ASSERT_EQ(result.size(), 5u);
  ExpectTopoEqual(result[0].get(), area0.get());
  ExpectTopoEqual(result[1].get(), expected_area1.get());
  ExpectTopoEqual(result[2].get(), area2.get());
  ExpectTopoEqual(result[3].get(), area3.get());
  EXPECT_TRUE(result[4]->isEmpty());
}

TEST(CleanCoverageTest, MergeGapsSelectsLongestBorderRegardlessOfAreaOrder) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Area 0: left column [0,2] x [0,4], noded at (2,1) for CoverageUnion.
  auto area0 =
      ReadPolygon(factory.get(), "POLYGON ((0 0, 2 0, 2 1, 2 4, 0 4, 0 0))");
  // Area 1: bottom bar [2,7] x [0,1], noded at (3,1) for CoverageUnion.
  auto area1 =
      ReadPolygon(factory.get(), "POLYGON ((2 0, 7 0, 7 1, 3 1, 2 1, 2 0))");
  // Gap A: [2,3] x [1,4] -> shares 3 with area0, 1 with area1 => merges to 0.
  auto gap_a =
      ReadPolygon(factory.get(), "POLYGON ((2 1, 3 1, 3 2, 3 4, 2 4, 2 1))");
  // Gap B: [3,7] x [1,2] -> shares 4 with area1, 1 with area0+gap_a => merges
  // to 1.
  auto gap_b =
      ReadPolygon(factory.get(), "POLYGON ((3 1, 7 1, 7 2, 3 2, 3 1))");

  auto expected0 = ReadPolygon(factory.get(),
                               "POLYGON ((0 0, 2 0, 2 1, 3 1, 3 4, 0 4, 0 0))");
  auto expected1 = ReadPolygon(factory.get(),
                               "POLYGON ((2 0, 7 0, 7 2, 3 2, 3 1, 2 1, 2 0))");

  CleanCoverage cov(2);
  cov.add(0, area0.get());
  cov.add(1, area1.get());

  std::vector<const Polygon*> gaps = {gap_a.get(), gap_b.get()};
  cov.mergeGaps(gaps);

  std::vector<std::unique_ptr<Geometry>> result = cov.toCoverage(factory.get());
  ASSERT_EQ(result.size(), 2u);
  ExpectTopoEqual(result[0].get(), expected0.get());
  ExpectTopoEqual(result[1].get(), expected1.get());
}

TEST(CleanCoverageTest, MergeGapsFillsInteriorHoleAndHandlesRepeatedCalls) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto donut = ReadPolygon(
      factory.get(),
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), (1 1, 1 3, 3 3, 3 1, 1 1))");
  auto hole_gap =
      ReadPolygon(factory.get(), "POLYGON ((1 1, 3 1, 3 3, 1 3, 1 1))");
  auto side_gap =
      ReadPolygon(factory.get(), "POLYGON ((4 0, 5 0, 5 4, 4 4, 4 0))");
  auto expected =
      ReadPolygon(factory.get(), "POLYGON ((0 0, 5 0, 5 4, 0 4, 0 0))");

  CleanCoverage cov(1);
  cov.add(0, donut.get());

  std::vector<const Polygon*> first_pass = {hole_gap.get()};
  cov.mergeGaps(first_pass);

  std::vector<const Polygon*> second_pass = {side_gap.get()};
  cov.mergeGaps(second_pass);

  std::vector<std::unique_ptr<Geometry>> result = cov.toCoverage(factory.get());
  ASSERT_EQ(result.size(), 1u);
  ExpectTopoEqual(result[0].get(), expected.get());
  const auto* poly_result = dynamic_cast<const Polygon*>(result[0].get());
  ASSERT_NE(poly_result, nullptr);
  EXPECT_EQ(poly_result->getNumInteriorRing(), 0u);
}

TEST(CleanCoverageTest, MergeGapsEqualBorderLengthTieSelectsValidTarget) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto left = ReadPolygon(factory.get(), "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  auto right =
      ReadPolygon(factory.get(), "POLYGON ((3 0, 5 0, 5 2, 3 2, 3 0))");
  auto gap = ReadPolygon(factory.get(), "POLYGON ((2 0, 3 0, 3 2, 2 2, 2 0))");

  CleanCoverage cov(2);
  cov.add(0, left.get());
  cov.add(1, right.get());

  std::vector<const Polygon*> gaps = {gap.get()};
  cov.mergeGaps(gaps);

  std::vector<std::unique_ptr<Geometry>> result = cov.toCoverage(factory.get());
  ASSERT_EQ(result.size(), 2u);
  EXPECT_TRUE(result[0]->isValid());
  EXPECT_TRUE(result[1]->isValid());
  EXPECT_EQ(result[0]->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_EQ(result[1]->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(result[0]->getArea() + result[1]->getArea(), 10.0);
  EXPECT_TRUE((result[0]->getArea() == 6.0 && result[1]->getArea() == 4.0) ||
              (result[0]->getArea() == 4.0 && result[1]->getArea() == 6.0));
}

}  // namespace
}  // namespace geos_google
