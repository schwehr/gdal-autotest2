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

// Unit tests for third_party/geos/src/coverage/CoverageCleaner.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/coverage/CoverageCleaner.cpp

#include "third_party/geos/include/geos/coverage/CoverageCleaner.h"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/coverage/CoverageGapFinder.h"
#include "third_party/geos/include/geos/coverage/CoverageValidator.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/noding/BasicSegmentString.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::coverage::CoverageCleaner;
using geos::coverage::CoverageGapFinder;
using geos::coverage::CoverageValidator;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::noding::BasicSegmentString;
using geos::noding::SegmentString;
using geos::util::IllegalArgumentException;

static_assert(CoverageCleaner::MERGE_LONGEST_BORDER == 0);
static_assert(CoverageCleaner::MERGE_MAX_AREA == 1);
static_assert(CoverageCleaner::MERGE_MIN_AREA == 2);
static_assert(CoverageCleaner::MERGE_MIN_INDEX == 3);
static_assert(!std::is_copy_constructible_v<CoverageCleaner>);
static_assert(!std::is_copy_assignable_v<CoverageCleaner>);

std::vector<std::unique_ptr<Geometry>> ParseWkts(
    const GeometryFactory* factory,
    std::initializer_list<std::string_view> wkts) {
  WKTReader reader(*factory);
  std::vector<std::unique_ptr<Geometry>> geoms;
  geoms.reserve(wkts.size());
  for (std::string_view wkt : wkts) {
    geoms.push_back(reader.read(std::string(wkt)));
  }
  return geoms;
}

std::vector<const Geometry*> BorrowPtrs(
    const std::vector<std::unique_ptr<Geometry>>& geoms) {
  std::vector<const Geometry*> ptrs;
  ptrs.reserve(geoms.size());
  for (const auto& geom : geoms) {
    ptrs.push_back(geom.get());
  }
  return ptrs;
}

std::unique_ptr<Geometry> MakeBox(const GeometryFactory* factory, double minx,
                                  double miny, double maxx, double maxy) {
  CoordinateSequence seq({
      CoordinateXY{minx, miny},
      CoordinateXY{maxx, miny},
      CoordinateXY{maxx, maxy},
      CoordinateXY{minx, maxy},
      CoordinateXY{minx, miny},
  });
  return factory->createPolygon(std::move(seq));
}

double TotalArea(const std::vector<std::unique_ptr<Geometry>>& geoms) {
  double area = 0.0;
  for (const auto& geom : geoms) {
    area += geom->getArea();
  }
  return area;
}

void ExpectValidCoverage(const std::vector<std::unique_ptr<Geometry>>& geoms,
                         double gap_width = 0.0) {
  for (std::size_t i = 0; i < geoms.size(); ++i) {
    SCOPED_TRACE(i);
    ASSERT_NE(geoms[i], nullptr);
    EXPECT_TRUE(geoms[i]->isValid());
  }
  std::vector<const Geometry*> ptrs = BorrowPtrs(geoms);
  if (gap_width > 0.0) {
    EXPECT_TRUE(CoverageValidator::isValid(ptrs, gap_width));
  } else {
    EXPECT_TRUE(CoverageValidator::isValid(ptrs));
  }
}

TEST(CoverageCleanerTest, MergeStrategyConstants) {
  EXPECT_EQ(+CoverageCleaner::MERGE_LONGEST_BORDER, 0);
  EXPECT_EQ(+CoverageCleaner::MERGE_MAX_AREA, 1);
  EXPECT_EQ(+CoverageCleaner::MERGE_MIN_AREA, 2);
  EXPECT_EQ(+CoverageCleaner::MERGE_MIN_INDEX, 3);
}

TEST(CoverageCleanerTest, EmptyCoverageVectorConstructionAndSetters) {
  std::vector<const Geometry*> empty_cov;
  CoverageCleaner cleaner(empty_cov);
  cleaner.setSnappingDistance(-1.0);
  cleaner.setGapMaximumWidth(-1.0);
  cleaner.setOverlapMergeStrategy(CoverageCleaner::MERGE_MAX_AREA);
  EXPECT_TRUE(cleaner.getOverlaps().empty());
  EXPECT_TRUE(cleaner.getMergedGaps().empty());
}

TEST(CoverageCleanerTest, InvalidOverlapMergeStrategyThrows) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto geoms =
      ParseWkts(factory.get(), {
                                   "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))",
                               });
  std::vector<const Geometry*> ptrs = BorrowPtrs(geoms);

  CoverageCleaner cleaner(ptrs);
  EXPECT_THROW(cleaner.setOverlapMergeStrategy(-1), IllegalArgumentException);
  EXPECT_THROW(cleaner.setOverlapMergeStrategy(4), IllegalArgumentException);
  EXPECT_THROW(CoverageCleaner::clean(ptrs, 0.0, -1, 0.0),
               IllegalArgumentException);
  EXPECT_THROW(CoverageCleaner::cleanOverlapGap(ptrs, 99, 0.0),
               IllegalArgumentException);
}

TEST(CoverageCleanerTest, PreservesValidSinglePolygonAndAdjacentPair) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Single polygon exercises the 1-line return branch in toGeometry().
  {
    auto geoms =
        ParseWkts(factory.get(), {
                                     "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
                                 });
    std::vector<const Geometry*> ptrs = BorrowPtrs(geoms);
    CoverageCleaner cleaner(ptrs);
    cleaner.clean();
    auto res = cleaner.getResult();
    ASSERT_EQ(res.size(), 1u);
    EXPECT_TRUE(res[0]->equals(geoms[0].get()));
    EXPECT_DOUBLE_EQ(res[0]->getArea(), 100.0);
    EXPECT_TRUE(cleaner.getOverlaps().empty());
    EXPECT_TRUE(cleaner.getMergedGaps().empty());
    ExpectValidCoverage(res);
  }

  // Two adjacent squares sharing an exact edge remain unchanged.
  {
    auto geoms =
        ParseWkts(factory.get(), {
                                     "POLYGON ((0 0, 5 0, 5 10, 0 10, 0 0))",
                                     "POLYGON ((5 0, 10 0, 10 10, 5 10, 5 0))",
                                 });
    std::vector<const Geometry*> ptrs = BorrowPtrs(geoms);
    auto res = CoverageCleaner::clean(ptrs, 0.0, 0.0);
    ASSERT_EQ(res.size(), 2u);
    EXPECT_DOUBLE_EQ(res[0]->getArea(), 50.0);
    EXPECT_DOUBLE_EQ(res[1]->getArea(), 50.0);
    ExpectValidCoverage(res);
  }
}

TEST(CoverageCleanerTest, NonPolygonalAndEmptyInputsBecomeEmptyPolygons) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Coverage containing only empty and non-polygonal entries.
  {
    auto geoms = ParseWkts(
        factory.get(),
        {
            "POLYGON EMPTY",
            "MULTIPOLYGON EMPTY",
            "POINT (1 1)",
            "LINESTRING (0 0, 2 2)",
            "GEOMETRYCOLLECTION (POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)))",
        });
    std::vector<const Geometry*> ptrs = BorrowPtrs(geoms);
    auto res = CoverageCleaner::clean(ptrs, 0.0, 0.0);
    ASSERT_EQ(res.size(), 5u);
    for (std::size_t i = 0; i < res.size(); ++i) {
      SCOPED_TRACE(i);
      ASSERT_NE(res[i], nullptr);
      EXPECT_TRUE(res[i]->isEmpty());
      EXPECT_EQ(res[i]->getDimension(), 2);
    }
  }

  // Mixed non-polygonal/empty entries alongside adjacent polygons.
  {
    auto geoms =
        ParseWkts(factory.get(), {
                                     "POINT (0 0)",
                                     "POLYGON EMPTY",
                                     "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))",
                                     "LINESTRING (0 0, 4 4)",
                                     "POLYGON ((4 0, 8 0, 8 4, 4 4, 4 0))",
                                 });
    std::vector<const Geometry*> ptrs = BorrowPtrs(geoms);
    auto res = CoverageCleaner::clean(ptrs, 0.0, 0.0);
    ASSERT_EQ(res.size(), 5u);
    EXPECT_TRUE(res[0]->isEmpty());
    EXPECT_TRUE(res[1]->isEmpty());
    EXPECT_DOUBLE_EQ(res[2]->getArea(), 16.0);
    EXPECT_TRUE(res[3]->isEmpty());
    EXPECT_DOUBLE_EQ(res[4]->getArea(), 16.0);
    ExpectValidCoverage(res);
  }
}

TEST(CoverageCleanerTest, MultiPolygonInputsAreCleanedAndValid) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto geoms =
      ParseWkts(factory.get(), {
                                   "MULTIPOLYGON (((0 0, 5 0, 5 4, 0 4, 0 0)), "
                                   "((8 0, 12 0, 12 4, 8 4, 8 0)))",
                                   "POLYGON ((4 0, 9 0, 9 4, 4 4, 4 0))",
                               });
  std::vector<const Geometry*> ptrs = BorrowPtrs(geoms);

  CoverageCleaner cleaner(ptrs);
  cleaner.setSnappingDistance(0.0);
  cleaner.setOverlapMergeStrategy(CoverageCleaner::MERGE_MIN_INDEX);
  cleaner.clean();

  std::vector<const Polygon*> overlaps = cleaner.getOverlaps();
  ASSERT_EQ(overlaps.size(), 2u);
  double overlap_area = 0.0;
  for (const Polygon* poly : overlaps) {
    ASSERT_NE(poly, nullptr);
    overlap_area += poly->getArea();
  }
  EXPECT_DOUBLE_EQ(overlap_area, 8.0);

  auto res = cleaner.getResult();
  ASSERT_EQ(res.size(), 2u);
  EXPECT_DOUBLE_EQ(res[0]->getArea(), 36.0);
  EXPECT_DOUBLE_EQ(res[1]->getArea(), 12.0);
  ExpectValidCoverage(res);
}

TEST(CoverageCleanerTest, OverlapMergeStrategiesSelectExpectedTargets) {
  struct StrategyCase {
    std::string_view name;
    std::string_view wkt0;
    std::string_view wkt1;
    int strategy;
    double expected_area0;
    double expected_area1;
  };

  constexpr auto kCases = std::to_array<StrategyCase>({
      // Setup 1: F0 = [6,20]x[2,8] (area 84, border 6, index 0),
      //          F1 = [0,6]x[0,10] \ [4,6]x[2,8] (area 48, border 10, index 1).
      {
          .name = "setup1_longest_border_picks_index1",
          .wkt0 = "POLYGON ((4 2, 20 2, 20 8, 4 8, 4 2))",
          .wkt1 = "POLYGON ((0 0, 6 0, 6 10, 0 10, 0 0))",
          .strategy = CoverageCleaner::MERGE_LONGEST_BORDER,
          .expected_area0 = 84.0,
          .expected_area1 = 60.0,
      },
      {
          .name = "setup1_max_area_picks_index0",
          .wkt0 = "POLYGON ((4 2, 20 2, 20 8, 4 8, 4 2))",
          .wkt1 = "POLYGON ((0 0, 6 0, 6 10, 0 10, 0 0))",
          .strategy = CoverageCleaner::MERGE_MAX_AREA,
          .expected_area0 = 96.0,
          .expected_area1 = 48.0,
      },
      {
          .name = "setup1_min_area_picks_index1",
          .wkt0 = "POLYGON ((4 2, 20 2, 20 8, 4 8, 4 2))",
          .wkt1 = "POLYGON ((0 0, 6 0, 6 10, 0 10, 0 0))",
          .strategy = CoverageCleaner::MERGE_MIN_AREA,
          .expected_area0 = 84.0,
          .expected_area1 = 60.0,
      },
      {
          .name = "setup1_min_index_picks_index0",
          .wkt0 = "POLYGON ((4 2, 20 2, 20 8, 4 8, 4 2))",
          .wkt1 = "POLYGON ((0 0, 6 0, 6 10, 0 10, 0 0))",
          .strategy = CoverageCleaner::MERGE_MIN_INDEX,
          .expected_area0 = 96.0,
          .expected_area1 = 48.0,
      },
      // Setup 2: distinguishes MERGE_LONGEST_BORDER vs MERGE_MIN_AREA.
      // F0 (area 188, border 10), F1 (area 12, border 6).
      {
          .name = "setup2_longest_border_picks_index0",
          .wkt0 = "POLYGON ((0 0, 20 0, 20 10, 0 10, 0 0))",
          .wkt1 = "POLYGON ((18 2, 22 2, 22 8, 18 8, 18 2))",
          .strategy = CoverageCleaner::MERGE_LONGEST_BORDER,
          .expected_area0 = 200.0,
          .expected_area1 = 12.0,
      },
      {
          .name = "setup2_min_area_picks_index1",
          .wkt0 = "POLYGON ((0 0, 20 0, 20 10, 0 10, 0 0))",
          .wkt1 = "POLYGON ((18 2, 22 2, 22 8, 18 8, 18 2))",
          .strategy = CoverageCleaner::MERGE_MIN_AREA,
          .expected_area0 = 188.0,
          .expected_area1 = 24.0,
      },
      // Setup 3: distinguishes MERGE_MAX_AREA vs MERGE_MIN_INDEX.
      // F0 (area 48, index 0), F1 (area 84, index 1).
      {
          .name = "setup3_max_area_picks_index1",
          .wkt0 = "POLYGON ((0 0, 6 0, 6 10, 0 10, 0 0))",
          .wkt1 = "POLYGON ((4 2, 20 2, 20 8, 4 8, 4 2))",
          .strategy = CoverageCleaner::MERGE_MAX_AREA,
          .expected_area0 = 48.0,
          .expected_area1 = 96.0,
      },
      {
          .name = "setup3_min_index_picks_index0",
          .wkt0 = "POLYGON ((0 0, 6 0, 6 10, 0 10, 0 0))",
          .wkt1 = "POLYGON ((4 2, 20 2, 20 8, 4 8, 4 2))",
          .strategy = CoverageCleaner::MERGE_MIN_INDEX,
          .expected_area0 = 60.0,
          .expected_area1 = 84.0,
      },
  });

  GeometryFactory::Ptr factory = GeometryFactory::create();
  for (const StrategyCase& tc : kCases) {
    SCOPED_TRACE(std::string(tc.name));
    auto geoms = ParseWkts(factory.get(), {tc.wkt0, tc.wkt1});
    std::vector<const Geometry*> ptrs = BorrowPtrs(geoms);
    auto res = CoverageCleaner::clean(ptrs, 0.0, tc.strategy, 0.0);
    ASSERT_EQ(res.size(), 2u);
    EXPECT_DOUBLE_EQ(res[0]->getArea(), tc.expected_area0);
    EXPECT_DOUBLE_EQ(res[1]->getArea(), tc.expected_area1);
    ExpectValidCoverage(res);
  }
}

TEST(CoverageCleanerTest, ThreeWayOverlapAndCoveredDuplicates) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Three polygons sharing a common 2x2 core [4,6]x[4,6].
  {
    auto geoms =
        ParseWkts(factory.get(), {
                                     "POLYGON ((0 0, 6 0, 6 6, 0 6, 0 0))",
                                     "POLYGON ((4 0, 10 0, 10 6, 4 6, 4 0))",
                                     "POLYGON ((2 4, 8 4, 8 10, 2 10, 2 4))",
                                 });
    std::vector<const Geometry*> ptrs = BorrowPtrs(geoms);
    CoverageCleaner cleaner(ptrs);
    cleaner.setSnappingDistance(0.0);
    cleaner.setOverlapMergeStrategy(CoverageCleaner::MERGE_MIN_INDEX);
    cleaner.clean();

    EXPECT_EQ(cleaner.getOverlaps().size(), 4u);
    auto res = cleaner.getResult();
    ASSERT_EQ(res.size(), 3u);
    EXPECT_DOUBLE_EQ(TotalArea(res), 84.0);
    ExpectValidCoverage(res);

    for (int strategy :
         {CoverageCleaner::MERGE_LONGEST_BORDER,
          CoverageCleaner::MERGE_MAX_AREA, CoverageCleaner::MERGE_MIN_AREA}) {
      SCOPED_TRACE(strategy);
      auto res_strategy = CoverageCleaner::clean(ptrs, 0.0, strategy, 0.0);
      ASSERT_EQ(res_strategy.size(), 3u);
      EXPECT_DOUBLE_EQ(TotalArea(res_strategy), 84.0);
      ExpectValidCoverage(res_strategy);
    }
  }

  // Exact duplicate polygons: first keeps area, duplicate becomes empty.
  {
    auto geoms =
        ParseWkts(factory.get(), {
                                     "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))",
                                     "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))",
                                 });
    std::vector<const Geometry*> ptrs = BorrowPtrs(geoms);
    auto res = CoverageCleaner::clean(ptrs, 0.0, 0.0);
    ASSERT_EQ(res.size(), 2u);
    EXPECT_DOUBLE_EQ(res[0]->getArea(), 16.0);
    EXPECT_TRUE(res[1]->isEmpty());
    EXPECT_EQ(res[1]->getDimension(), 2);
    ExpectValidCoverage(res);
  }

  // Contained polygon: longest border / max area merge into outer polygon,
  // min index / min area keep inner polygon.
  {
    auto geoms =
        ParseWkts(factory.get(), {
                                     "POLYGON ((2 2, 4 2, 4 4, 2 4, 2 2))",
                                     "POLYGON ((0 0, 6 0, 6 6, 0 6, 0 0))",
                                 });
    std::vector<const Geometry*> ptrs = BorrowPtrs(geoms);

    auto res_border = CoverageCleaner::clean(
        ptrs, 0.0, CoverageCleaner::MERGE_LONGEST_BORDER, 0.0);
    ASSERT_EQ(res_border.size(), 2u);
    EXPECT_TRUE(res_border[0]->isEmpty());
    EXPECT_DOUBLE_EQ(res_border[1]->getArea(), 36.0);
    ExpectValidCoverage(res_border);

    auto res_max_area =
        CoverageCleaner::clean(ptrs, 0.0, CoverageCleaner::MERGE_MAX_AREA, 0.0);
    ASSERT_EQ(res_max_area.size(), 2u);
    EXPECT_TRUE(res_max_area[0]->isEmpty());
    EXPECT_DOUBLE_EQ(res_max_area[1]->getArea(), 36.0);
    ExpectValidCoverage(res_max_area);

    auto res_min_area =
        CoverageCleaner::clean(ptrs, 0.0, CoverageCleaner::MERGE_MIN_AREA, 0.0);
    ASSERT_EQ(res_min_area.size(), 2u);
    EXPECT_DOUBLE_EQ(res_min_area[0]->getArea(), 4.0);
    EXPECT_DOUBLE_EQ(res_min_area[1]->getArea(), 32.0);
    ExpectValidCoverage(res_min_area);

    auto res_index = CoverageCleaner::clean(
        ptrs, 0.0, CoverageCleaner::MERGE_MIN_INDEX, 0.0);
    ASSERT_EQ(res_index.size(), 2u);
    EXPECT_DOUBLE_EQ(res_index[0]->getArea(), 4.0);
    EXPECT_DOUBLE_EQ(res_index[1]->getArea(), 32.0);
    ExpectValidCoverage(res_index);
  }
}

TEST(CoverageCleanerTest, NonConvexBoundingBoxTriggersCoversFalseBranch) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // P0 is L-shaped with bbox [0,20]x[0,20] and arm thickness 4.0 (> 2.5);
  // P1..P4 and the enclosed gap lie inside P0's bounding box without being
  // covered by P0's area.
  auto geoms = ParseWkts(
      factory.get(), {
                         "POLYGON ((0 0, 20 0, 20 4, 4 4, 4 20, 0 20, 0 0))",
                         "POLYGON ((4 4, 20 4, 20 8, 4 8, 4 4))",
                         "POLYGON ((4 8, 8 8, 8 20, 4 20, 4 8))",
                         "POLYGON ((10 8, 20 8, 20 20, 10 20, 10 8))",
                         "POLYGON ((4 16, 20 16, 20 20, 4 20, 4 16))",
                     });
  std::vector<const Geometry*> ptrs = BorrowPtrs(geoms);
  // Overlaps exist between P4 and P2/P3; enclosed gap [8,10]x[8,16] has
  // width 2.
  auto res = CoverageCleaner::cleanOverlapGap(
      ptrs, CoverageCleaner::MERGE_MIN_INDEX, 2.5);
  ASSERT_EQ(res.size(), 5u);
  EXPECT_DOUBLE_EQ(res[0]->getArea(), 144.0);
  EXPECT_DOUBLE_EQ(TotalArea(res), 400.0);
  ExpectValidCoverage(res, 2.5);
  std::vector<const Geometry*> res_ptrs = BorrowPtrs(res);
  auto gaps = CoverageGapFinder::findGaps(res_ptrs, 2.5);
  ASSERT_NE(gaps, nullptr);
  EXPECT_TRUE(gaps->isEmpty());
}

TEST(CoverageCleanerTest, SnappingDistanceControlsAlignmentAndSliverCollapse) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Explicit snapping vs disabled snapping (0.0) across a 0.02 gap.
  {
    std::vector<std::unique_ptr<Geometry>> geoms;
    geoms.push_back(MakeBox(factory.get(), 0.0, 0.0, 10.0, 10.0));
    geoms.push_back(MakeBox(factory.get(), 10.02, 0.0, 20.0, 10.0));
    std::vector<const Geometry*> ptrs = BorrowPtrs(geoms);

    auto res_no_snap = CoverageCleaner::clean(ptrs, 0.0, 0.0);
    std::vector<const Geometry*> no_snap_ptrs = BorrowPtrs(res_no_snap);
    EXPECT_FALSE(CoverageValidator::isValid(no_snap_ptrs, 0.05));
    EXPECT_NEAR(TotalArea(res_no_snap), 199.8, 1e-9);

    auto res_snapped = CoverageCleaner::clean(ptrs, 0.05, 0.0);
    ExpectValidCoverage(res_snapped, 0.05);
    EXPECT_NEAR(TotalArea(res_snapped), 200.0, 1e-9);

    // Passing a negative distance after a positive one preserves the positive
    // one.
    CoverageCleaner cleaner(ptrs);
    cleaner.setSnappingDistance(0.05);
    cleaner.setSnappingDistance(-1.0);
    cleaner.clean();
    auto res_kept = cleaner.getResult();
    ExpectValidCoverage(res_kept, 0.05);
    EXPECT_NEAR(TotalArea(res_kept), 200.0, 1e-9);
  }

  // Passing a negative distance from construction keeps default snapping
  // (~diam/1e8).
  {
    std::vector<std::unique_ptr<Geometry>> geoms;
    geoms.push_back(MakeBox(factory.get(), 0.0, 0.0, 100.0, 100.0));
    geoms.push_back(MakeBox(factory.get(), 100.0 + 1e-8, 0.0, 200.0, 100.0));
    std::vector<const Geometry*> ptrs = BorrowPtrs(geoms);

    CoverageCleaner default_cleaner(ptrs);
    default_cleaner.setSnappingDistance(-5.0);
    default_cleaner.clean();
    auto res_default = default_cleaner.getResult();
    ExpectValidCoverage(res_default, 1e-5);

    CoverageCleaner zero_cleaner(ptrs);
    zero_cleaner.setSnappingDistance(0.0);
    zero_cleaner.clean();
    auto res_zero = zero_cleaner.getResult();
    std::vector<const Geometry*> zero_ptrs = BorrowPtrs(res_zero);
    EXPECT_FALSE(CoverageValidator::isValid(zero_ptrs, 1e-5));
  }

  // Tiny polygon smaller than snapDistance collapses to an empty polygon.
  {
    std::vector<std::unique_ptr<Geometry>> geoms;
    geoms.push_back(MakeBox(factory.get(), 0.0, 0.0, 10.0, 10.0));
    geoms.push_back(MakeBox(factory.get(), 20.0, 0.0, 20.01, 0.01));
    std::vector<const Geometry*> ptrs = BorrowPtrs(geoms);

    auto res = CoverageCleaner::clean(ptrs, 0.05, 0.0);
    ASSERT_EQ(res.size(), 2u);
    EXPECT_DOUBLE_EQ(res[0]->getArea(), 100.0);
    EXPECT_TRUE(res[1]->isEmpty());
    EXPECT_EQ(res[1]->getDimension(), 2);
    ExpectValidCoverage(res);
  }
}

TEST(CoverageCleanerTest, GapMergingWidthThresholdAndNegativeMaxWidth) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  // Frame enclosing two distinct gaps:
  //   Gap 1: [4,5]x[8,14]   (width 1.0, area 6.0, longest border 6.0 on poly 2)
  //   Gap 2: [10,20]x[8,14] (width 6.0, length 10.0, area 60.0)
  // Base polygon total area: 192 + 192 + 24 + 15 + 15 + 24 = 462.0.
  auto geoms =
      ParseWkts(factory.get(), {
                                   "POLYGON ((0 14, 24 14, 24 22, 0 22, 0 14))",
                                   "POLYGON ((0 0, 24 0, 24 8, 0 8, 0 0))",
                                   "POLYGON ((0 8, 4 8, 4 14, 0 14, 0 8))",
                                   "POLYGON ((5 8, 10 8, 10 11, 5 11, 5 8))",
                                   "POLYGON ((5 11, 10 11, 10 14, 5 14, 5 11))",
                                   "POLYGON ((20 8, 24 8, 24 14, 20 14, 20 8))",
                               });
  std::vector<const Geometry*> ptrs = BorrowPtrs(geoms);

  // maxWidth = 0.0 disables gap merging even though gaps exist.
  {
    CoverageCleaner cleaner(ptrs);
    cleaner.setSnappingDistance(0.0);
    cleaner.setGapMaximumWidth(0.0);
    cleaner.clean();
    EXPECT_TRUE(cleaner.getMergedGaps().empty());
    auto res = cleaner.getResult();
    EXPECT_DOUBLE_EQ(TotalArea(res), 462.0);
    ExpectValidCoverage(res);
  }

  // Negative maxWidth from default leaves 0.0 (no gaps merged).
  {
    CoverageCleaner cleaner(ptrs);
    cleaner.setGapMaximumWidth(-1.0);
    cleaner.clean();
    EXPECT_TRUE(cleaner.getMergedGaps().empty());
    auto res = cleaner.getResult();
    EXPECT_DOUBLE_EQ(TotalArea(res), 462.0);
  }

  // maxWidth = 2.0 merges Gap 1 (width 1.0) into poly 2, leaves Gap 2
  // (width 6.0).
  {
    CoverageCleaner cleaner(ptrs);
    cleaner.setSnappingDistance(0.0);
    cleaner.setGapMaximumWidth(2.0);
    cleaner.setGapMaximumWidth(-3.0);  // Ignored; retains 2.0.
    cleaner.clean();

    std::vector<const Polygon*> merged_gaps = cleaner.getMergedGaps();
    ASSERT_EQ(merged_gaps.size(), 1u);
    EXPECT_DOUBLE_EQ(merged_gaps[0]->getArea(), 6.0);

    auto res = cleaner.getResult();
    ASSERT_EQ(res.size(), 6u);
    EXPECT_DOUBLE_EQ(res[2]->getArea(), 30.0);
    EXPECT_DOUBLE_EQ(TotalArea(res), 468.0);
    ExpectValidCoverage(res, 2.0);
    std::vector<const Geometry*> res_ptrs = BorrowPtrs(res);
    EXPECT_FALSE(CoverageValidator::isValid(res_ptrs, 7.0));
    auto remaining_gaps = CoverageGapFinder::findGaps(res_ptrs, 7.0);
    ASSERT_NE(remaining_gaps, nullptr);
    EXPECT_FALSE(remaining_gaps->isEmpty());
  }

  // cleanGapWidth with maxWidth = 7.0 merges both Gap 1 and Gap 2.
  {
    auto res = CoverageCleaner::cleanGapWidth(ptrs, 7.0);
    EXPECT_DOUBLE_EQ(TotalArea(res), 528.0);
    ExpectValidCoverage(res, 7.0);
    std::vector<const Geometry*> res_ptrs = BorrowPtrs(res);
    auto remaining_gaps = CoverageGapFinder::findGaps(res_ptrs, 7.0);
    ASSERT_NE(remaining_gaps, nullptr);
    EXPECT_TRUE(remaining_gaps->isEmpty());
  }
}

TEST(CoverageCleanerTest, CombinedOverlapAndHoleGapAndInletBehavior) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Polygon with an interior hole (width 4.0) overlapping an adjacent polygon.
  // Non-overlapping parts have area 84.0 (P0) and 60.0 (P1), so MERGE_MAX_AREA
  // assigns the [10,12]x[0,10] overlap to P0, leaving P1 width 6.0 (> 5.0).
  {
    auto geoms = ParseWkts(factory.get(),
                           {
                               "POLYGON ((0 0, 12 0, 12 10, 0 10, 0 0), (3 3, "
                               "7 3, 7 7, 3 7, 3 3))",
                               "POLYGON ((10 0, 18 0, 18 10, 10 10, 10 0))",
                           });
    std::vector<const Geometry*> ptrs = BorrowPtrs(geoms);

    // Hole preserved when maxGapWidth = 3.0 (< 4.0).
    auto res_hole_kept = CoverageCleaner::cleanOverlapGap(
        ptrs, CoverageCleaner::MERGE_MAX_AREA, 3.0);
    ASSERT_EQ(res_hole_kept.size(), 2u);
    const auto* poly_kept =
        dynamic_cast<const Polygon*>(res_hole_kept[0].get());
    ASSERT_NE(poly_kept, nullptr);
    EXPECT_EQ(poly_kept->getNumInteriorRing(), 1u);
    EXPECT_DOUBLE_EQ(res_hole_kept[0]->getArea(), 104.0);
    EXPECT_DOUBLE_EQ(res_hole_kept[1]->getArea(), 60.0);
    ExpectValidCoverage(res_hole_kept, 3.0);
    std::vector<const Geometry*> kept_ptrs = BorrowPtrs(res_hole_kept);
    auto kept_gaps_5 = CoverageGapFinder::findGaps(kept_ptrs, 5.0);
    ASSERT_NE(kept_gaps_5, nullptr);
    EXPECT_FALSE(kept_gaps_5->isEmpty());

    // Hole filled when maxGapWidth = 5.0 (>= 4.0).
    auto res_hole_filled = CoverageCleaner::cleanOverlapGap(
        ptrs, CoverageCleaner::MERGE_MAX_AREA, 5.0);
    ASSERT_EQ(res_hole_filled.size(), 2u);
    const auto* poly_filled =
        dynamic_cast<const Polygon*>(res_hole_filled[0].get());
    ASSERT_NE(poly_filled, nullptr);
    EXPECT_EQ(poly_filled->getNumInteriorRing(), 0u);
    EXPECT_DOUBLE_EQ(res_hole_filled[0]->getArea(), 120.0);
    EXPECT_DOUBLE_EQ(res_hole_filled[1]->getArea(), 60.0);
    ExpectValidCoverage(res_hole_filled, 5.0);
    std::vector<const Geometry*> filled_ptrs = BorrowPtrs(res_hole_filled);
    auto filled_gaps_5 = CoverageGapFinder::findGaps(filled_ptrs, 5.0);
    ASSERT_NE(filled_gaps_5, nullptr);
    EXPECT_TRUE(filled_gaps_5->isEmpty());
  }

  // Open inlet (not enclosed) is not filled even when narrower than maxWidth.
  {
    auto geoms =
        ParseWkts(factory.get(), {
                                     "POLYGON ((0 0, 10 0, 10 4.5, 8 4.5, 8 "
                                     "5.5, 10 5.5, 10 10, 0 10, 0 0))",
                                 });
    std::vector<const Geometry*> ptrs = BorrowPtrs(geoms);
    CoverageCleaner cleaner(ptrs);
    cleaner.setGapMaximumWidth(5.0);
    cleaner.clean();
    EXPECT_TRUE(cleaner.getMergedGaps().empty());
    auto res = cleaner.getResult();
    ASSERT_EQ(res.size(), 1u);
    EXPECT_DOUBLE_EQ(res[0]->getArea(), 98.0);
    ExpectValidCoverage(res);
  }
}

TEST(CoverageCleanerTest, NodeAndToGeometryPublicMethods) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto geoms =
      ParseWkts(factory.get(), {
                                   "POINT (0 0)",
                                   "POLYGON EMPTY",
                                   "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))",
                                   "POLYGON ((2 2, 6 2, 6 6, 2 6, 2 2))",
                               });
  std::vector<const Geometry*> ptrs = BorrowPtrs(geoms);
  CoverageCleaner cleaner(ptrs);

  std::unique_ptr<Geometry> noded = cleaner.node(ptrs, 0.0);
  ASSERT_NE(noded, nullptr);
  EXPECT_FALSE(noded->isEmpty());
  EXPECT_EQ(noded->getDimension(), 1);
  EXPECT_DOUBLE_EQ(noded->getLength(), 32.0);
  EXPECT_GT(noded->getNumGeometries(), 2u);

  // Only empty/non-polygonal inputs produce an empty MultiLineString.
  auto empty_only = ParseWkts(factory.get(), {"POINT (1 2)", "POLYGON EMPTY"});
  std::vector<const Geometry*> empty_ptrs = BorrowPtrs(empty_only);
  std::unique_ptr<Geometry> empty_noded = cleaner.node(empty_ptrs, 0.0);
  ASSERT_NE(empty_noded, nullptr);
  EXPECT_TRUE(empty_noded->isEmpty());
  EXPECT_EQ(empty_noded->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);

  // Direct toGeometry calls for 0, 1, and 2 segment strings.
  std::vector<SegmentString*> segs_zero;
  std::unique_ptr<Geometry> g0 = cleaner.toGeometry(segs_zero, factory.get());
  ASSERT_NE(g0, nullptr);
  EXPECT_TRUE(g0->isEmpty());
  EXPECT_EQ(g0->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);

  CoordinateSequence seq1({CoordinateXY{0.0, 0.0}, CoordinateXY{3.0, 4.0}});
  BasicSegmentString ss1(&seq1, nullptr);
  std::vector<SegmentString*> segs_one = {&ss1};
  std::unique_ptr<Geometry> g1 = cleaner.toGeometry(segs_one, factory.get());
  ASSERT_NE(g1, nullptr);
  EXPECT_EQ(g1->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_DOUBLE_EQ(g1->getLength(), 5.0);

  CoordinateSequence seq2({CoordinateXY{0.0, 0.0}, CoordinateXY{0.0, 2.0}});
  BasicSegmentString ss2(&seq2, nullptr);
  std::vector<SegmentString*> segs_two = {&ss1, &ss2};
  std::unique_ptr<Geometry> g2 = cleaner.toGeometry(segs_two, factory.get());
  ASSERT_NE(g2, nullptr);
  EXPECT_EQ(g2->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(g2->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(g2->getLength(), 7.0);
}

TEST(CoverageCleanerTest, AllEmptyPolygonsAndEmptyFirstElement) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Only empty polygons: extent is null (default snap distance 0.0), clean
  // succeeds.
  {
    auto geoms = ParseWkts(factory.get(), {"POLYGON EMPTY", "POLYGON EMPTY"});
    std::vector<const Geometry*> ptrs = BorrowPtrs(geoms);
    CoverageCleaner cleaner(ptrs);
    cleaner.clean();
    EXPECT_TRUE(cleaner.getOverlaps().empty());
    EXPECT_TRUE(cleaner.getMergedGaps().empty());
    auto res = cleaner.getResult();
    ASSERT_EQ(res.size(), 2u);
    for (const auto& geom : res) {
      ASSERT_NE(geom, nullptr);
      EXPECT_TRUE(geom->isEmpty());
      EXPECT_EQ(geom->getDimension(), 2);
    }
  }

  // Empty first polygon followed by a valid polygon uses non-empty extent for
  // default snap.
  {
    auto geoms = ParseWkts(factory.get(),
                           {
                               "POLYGON EMPTY",
                               "POLYGON ((0 0, 100 0, 100 100, 0 100, 0 0))",
                               "POLYGON ((100.00000001 0, 200 0, 200 100, "
                               "100.00000001 100, 100.00000001 0))",
                           });
    std::vector<const Geometry*> ptrs = BorrowPtrs(geoms);
    auto res = CoverageCleaner::cleanGapWidth(ptrs, 0.0);
    ASSERT_EQ(res.size(), 3u);
    EXPECT_TRUE(res[0]->isEmpty());
    EXPECT_NEAR(res[1]->getArea() + res[2]->getArea(), 20000.0, 1e-6);
    ExpectValidCoverage(res, 1e-5);
  }
}

TEST(CoverageCleanerTest, PreservesFactoryAndSridAcrossCleanAndNode) {
  PrecisionModel pm;
  GeometryFactory::Ptr factory = GeometryFactory::create(&pm, 4326);
  auto geoms =
      ParseWkts(factory.get(), {
                                   "POLYGON ((0 0, 6 0, 6 6, 0 6, 0 0))",
                                   "POLYGON ((4 0, 10 0, 10 6, 4 6, 4 0))",
                                   "POLYGON ((4 0, 10 0, 10 6, 4 6, 4 0))",
                               });
  std::vector<const Geometry*> ptrs = BorrowPtrs(geoms);
  CoverageCleaner cleaner(ptrs);
  cleaner.setSnappingDistance(0.0);
  cleaner.clean();

  std::unique_ptr<Geometry> noded = cleaner.node(ptrs, 0.05);
  ASSERT_NE(noded, nullptr);
  EXPECT_FALSE(noded->isEmpty());
  EXPECT_EQ(noded->getDimension(), 1);
  EXPECT_EQ(noded->getSRID(), 4326);

  auto res = cleaner.getResult();
  ASSERT_EQ(res.size(), 3u);
  for (std::size_t i = 0; i < res.size(); ++i) {
    SCOPED_TRACE(i);
    ASSERT_NE(res[i], nullptr);
    EXPECT_EQ(res[i]->getSRID(), 4326);
  }
  EXPECT_TRUE(res[2]->isEmpty());
  EXPECT_DOUBLE_EQ(TotalArea(res), 60.0);
  ExpectValidCoverage(res);
}

}  // namespace
}  // namespace geos_google
