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

#include "third_party/geos/include/geos/coverage/CoverageGapFinder.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::coverage::CoverageGapFinder;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::io::WKTReader;

class CoverageGapFinderTest : public ::testing::Test {
 protected:
  CoverageGapFinderTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> Read(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(CoverageGapFinderTest, NoGapsInAdjacentPolygons) {
  auto left = Read("POLYGON ((0 0, 5 0, 5 10, 0 10, 0 0))");
  auto right = Read("POLYGON ((5 0, 10 0, 10 10, 5 10, 5 0))");
  std::vector<const Geometry*> coverage = {left.get(), right.get()};

  std::unique_ptr<Geometry> static_gaps =
      CoverageGapFinder::findGaps(coverage, 1.0);
  ASSERT_NE(static_gaps, nullptr);
  EXPECT_TRUE(static_gaps->isEmpty());

  CoverageGapFinder finder(coverage);
  std::unique_ptr<Geometry> instance_gaps = finder.findGaps(1.0);
  ASSERT_NE(instance_gaps, nullptr);
  EXPECT_TRUE(instance_gaps->isEmpty());
}

TEST_F(CoverageGapFinderTest, FindsSingleNarrowGapBetweenFourPolygons) {
  // Four vector-clean polygons enclosing gap [4.5, 5.5] x [2, 8] (width 1.0).
  auto bottom = Read("POLYGON ((0 0, 10 0, 10 2, 5.5 2, 4.5 2, 0 2, 0 0))");
  auto top = Read("POLYGON ((0 8, 4.5 8, 5.5 8, 10 8, 10 10, 0 10, 0 8))");
  auto left = Read("POLYGON ((0 2, 4.5 2, 4.5 8, 0 8, 0 2))");
  auto right = Read("POLYGON ((5.5 2, 10 2, 10 8, 5.5 8, 5.5 2))");
  std::vector<const Geometry*> coverage = {bottom.get(), top.get(), left.get(),
                                           right.get()};

  std::unique_ptr<Geometry> gaps = CoverageGapFinder::findGaps(coverage, 1.5);
  ASSERT_NE(gaps, nullptr);
  EXPECT_FALSE(gaps->isEmpty());
  EXPECT_EQ(gaps->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_NEAR(gaps->getArea(), 6.0, 1e-9);

  auto expected_gap = Read("POLYGON ((4.5 2, 5.5 2, 5.5 8, 4.5 8, 4.5 2))");
  EXPECT_TRUE(gaps->equals(expected_gap.get()));

  CoverageGapFinder finder(coverage);
  std::unique_ptr<Geometry> too_narrow_threshold = finder.findGaps(0.5);
  ASSERT_NE(too_narrow_threshold, nullptr);
  EXPECT_TRUE(too_narrow_threshold->isEmpty());
}

TEST_F(CoverageGapFinderTest, ZeroAndNegativeGapWidthReturnsEmpty) {
  auto bottom = Read("POLYGON ((0 0, 10 0, 10 2, 5.5 2, 4.5 2, 0 2, 0 0))");
  auto top = Read("POLYGON ((0 8, 4.5 8, 5.5 8, 10 8, 10 10, 0 10, 0 8))");
  auto left = Read("POLYGON ((0 2, 4.5 2, 4.5 8, 0 8, 0 2))");
  auto right = Read("POLYGON ((5.5 2, 10 2, 10 8, 5.5 8, 5.5 2))");
  std::vector<const Geometry*> coverage = {bottom.get(), top.get(), left.get(),
                                           right.get()};

  std::unique_ptr<Geometry> zero_width =
      CoverageGapFinder::findGaps(coverage, 0.0);
  ASSERT_NE(zero_width, nullptr);
  EXPECT_TRUE(zero_width->isEmpty());

  std::unique_ptr<Geometry> neg_width =
      CoverageGapFinder::findGaps(coverage, -2.5);
  ASSERT_NE(neg_width, nullptr);
  EXPECT_TRUE(neg_width->isEmpty());
}

TEST_F(CoverageGapFinderTest, FindsMultipleGapsAndFiltersByWidth) {
  // Encloses two holes:
  //   Hole 1: [2, 2.5] x [2, 6] (width 0.5, area 2.0)
  //   Hole 2: [6, 8]   x [2, 6] (width 2.0, area 8.0)
  auto bottom =
      Read("POLYGON ((0 0, 10 0, 10 2, 8 2, 6 2, 2.5 2, 2 2, 0 2, 0 0))");
  auto top =
      Read("POLYGON ((0 6, 2 6, 2.5 6, 6 6, 8 6, 10 6, 10 8, 0 8, 0 6))");
  auto left = Read("POLYGON ((0 2, 2 2, 2 6, 0 6, 0 2))");
  auto middle = Read("POLYGON ((2.5 2, 6 2, 6 6, 2.5 6, 2.5 2))");
  auto right = Read("POLYGON ((8 2, 10 2, 10 6, 8 6, 8 2))");
  std::vector<const Geometry*> coverage = {bottom.get(), top.get(), left.get(),
                                           middle.get(), right.get()};

  std::unique_ptr<Geometry> narrow_only =
      CoverageGapFinder::findGaps(coverage, 1.0);
  ASSERT_NE(narrow_only, nullptr);
  EXPECT_EQ(narrow_only->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_NEAR(narrow_only->getArea(), 2.0, 1e-9);
  auto expected_narrow = Read("POLYGON ((2 2, 2.5 2, 2.5 6, 2 6, 2 2))");
  EXPECT_TRUE(narrow_only->equals(expected_narrow.get()));

  std::unique_ptr<Geometry> both_gaps =
      CoverageGapFinder::findGaps(coverage, 2.5);
  ASSERT_NE(both_gaps, nullptr);
  EXPECT_EQ(both_gaps->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(both_gaps->getNumGeometries(), 2U);
  EXPECT_NEAR(both_gaps->getArea(), 10.0, 1e-9);
  auto expected_both = Read(
      "MULTIPOLYGON (((2 2, 2.5 2, 2.5 6, 2 6, 2 2)), "
      "((6 2, 8 2, 8 6, 6 6, 6 2)))");
  EXPECT_TRUE(both_gaps->equals(expected_both.get()));
}

TEST_F(CoverageGapFinderTest, SinglePolygonHoleAndOpenGaps) {
  auto holed = Read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2.4 2, 2.4 8, 2 8, 2 2))");
  std::vector<const Geometry*> single_coverage = {holed.get()};
  std::unique_ptr<Geometry> single_hole_gap =
      CoverageGapFinder::findGaps(single_coverage, 0.6);
  ASSERT_NE(single_hole_gap, nullptr);
  EXPECT_FALSE(single_hole_gap->isEmpty());
  EXPECT_NEAR(single_hole_gap->getArea(), 2.4, 1e-9);

  // Open channel between disjoint polygons is not a closed gap.
  auto p1 = Read("POLYGON ((0 0, 4 0, 4 10, 0 10, 0 0))");
  auto p2 = Read("POLYGON ((4.5 0, 10 0, 10 10, 4.5 10, 4.5 0))");
  std::vector<const Geometry*> disjoint_coverage = {p1.get(), p2.get()};
  std::unique_ptr<Geometry> disjoint_gaps =
      CoverageGapFinder::findGaps(disjoint_coverage, 10.0);
  ASSERT_NE(disjoint_gaps, nullptr);
  EXPECT_TRUE(disjoint_gaps->isEmpty());

  // Open gore (indentation open on top) is not detected as a gap.
  auto bottom = Read("POLYGON ((0 0, 10 0, 10 2, 5.5 2, 4.5 2, 0 2, 0 0))");
  auto left = Read("POLYGON ((0 2, 4.5 2, 4.5 10, 0 10, 0 2))");
  auto right = Read("POLYGON ((5.5 2, 10 2, 10 10, 5.5 10, 5.5 2))");
  std::vector<const Geometry*> gore_coverage = {bottom.get(), left.get(),
                                                right.get()};
  std::unique_ptr<Geometry> gore_gaps =
      CoverageGapFinder::findGaps(gore_coverage, 5.0);
  ASSERT_NE(gore_gaps, nullptr);
  EXPECT_TRUE(gore_gaps->isEmpty());
}

TEST_F(CoverageGapFinderTest, EmptyPolygonInCoverageReturnsEmpty) {
  auto empty_poly = Read("POLYGON EMPTY");
  std::vector<const Geometry*> coverage = {empty_poly.get()};

  std::unique_ptr<Geometry> gaps = CoverageGapFinder::findGaps(coverage, 1.0);
  ASSERT_NE(gaps, nullptr);
  EXPECT_TRUE(gaps->isEmpty());
}

TEST_F(CoverageGapFinderTest, MultiIslandAndMultiPolygonCoverage) {
  // Island 1: [0, 10] x [0, 10] enclosing gap [4.5, 5.5] x [2, 8] (width 1.0).
  auto a_bottom = Read("POLYGON ((0 0, 10 0, 10 2, 5.5 2, 4.5 2, 0 2, 0 0))");
  auto a_top = Read("POLYGON ((0 8, 4.5 8, 5.5 8, 10 8, 10 10, 0 10, 0 8))");
  auto a_left = Read("POLYGON ((0 2, 4.5 2, 4.5 8, 0 8, 0 2))");
  auto a_right = Read("POLYGON ((5.5 2, 10 2, 10 8, 5.5 8, 5.5 2))");

  // Island 2: [20, 30] x [0, 10] enclosing gap [24, 26] x [2, 8] (width 2.0),
  // using a MultiPolygon as one of the coverage elements.
  auto b_bottom = Read("POLYGON ((20 0, 30 0, 30 2, 26 2, 24 2, 20 2, 20 0))");
  auto b_top = Read("POLYGON ((20 8, 24 8, 26 8, 30 8, 30 10, 20 10, 20 8))");
  auto b_sides = Read(
      "MULTIPOLYGON (((20 2, 24 2, 24 8, 20 8, 20 2)), "
      "((26 2, 30 2, 30 8, 26 8, 26 2)))");

  std::vector<const Geometry*> coverage = {
      a_bottom.get(), a_top.get(), a_left.get(), a_right.get(),
      b_bottom.get(), b_top.get(), b_sides.get()};

  std::unique_ptr<Geometry> narrow_gap =
      CoverageGapFinder::findGaps(coverage, 1.5);
  ASSERT_NE(narrow_gap, nullptr);
  EXPECT_EQ(narrow_gap->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_NEAR(narrow_gap->getArea(), 6.0, 1e-9);
  auto expected_a = Read("POLYGON ((4.5 2, 5.5 2, 5.5 8, 4.5 8, 4.5 2))");
  EXPECT_TRUE(narrow_gap->equals(expected_a.get()));

  std::unique_ptr<Geometry> both_gaps =
      CoverageGapFinder::findGaps(coverage, 2.5);
  ASSERT_NE(both_gaps, nullptr);
  EXPECT_EQ(both_gaps->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(both_gaps->getNumGeometries(), 2U);
  EXPECT_NEAR(both_gaps->getArea(), 18.0, 1e-9);
  auto expected_both = Read(
      "MULTIPOLYGON (((4.5 2, 5.5 2, 5.5 8, 4.5 8, 4.5 2)), "
      "((24 2, 26 2, 26 8, 24 8, 24 2)))");
  EXPECT_TRUE(both_gaps->equals(expected_both.get()));
}

TEST_F(CoverageGapFinderTest, NonConvexLShapedGapAndReusableFinder) {
  // Encloses a 6-vertex non-convex L-shaped hole (area 7.0, MIC width ~1.172),
  // exercising MaximumInscribedCircle approximation with tolerance = gapWidth /
  // 100.
  auto bottom = Read("POLYGON ((0 0, 8 0, 8 2, 6 2, 2 2, 0 2, 0 0))");
  auto left = Read("POLYGON ((0 2, 2 2, 2 6, 2 8, 0 8, 0 2))");
  auto right = Read("POLYGON ((6 2, 8 2, 8 8, 6 8, 6 3, 6 2))");
  auto inner_top = Read("POLYGON ((2 6, 3 6, 3 3, 6 3, 6 8, 2 8, 2 6))");
  std::vector<const Geometry*> coverage = {bottom.get(), left.get(),
                                           right.get(), inner_top.get()};

  CoverageGapFinder finder(coverage);

  std::unique_ptr<Geometry> zero_gap = finder.findGaps(0.0);
  ASSERT_NE(zero_gap, nullptr);
  EXPECT_TRUE(zero_gap->isEmpty());

  std::unique_ptr<Geometry> too_narrow = finder.findGaps(1.0);
  ASSERT_NE(too_narrow, nullptr);
  EXPECT_TRUE(too_narrow->isEmpty());

  std::unique_ptr<Geometry> detected = finder.findGaps(1.5);
  ASSERT_NE(detected, nullptr);
  EXPECT_EQ(detected->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_NEAR(detected->getArea(), 7.0, 1e-9);
  auto expected_l = Read("POLYGON ((2 2, 6 2, 6 3, 3 3, 3 6, 2 6, 2 2))");
  EXPECT_TRUE(detected->equals(expected_l.get()));

  std::unique_ptr<Geometry> narrow_again = finder.findGaps(0.5);
  ASSERT_NE(narrow_again, nullptr);
  EXPECT_TRUE(narrow_again->isEmpty());
}

TEST_F(CoverageGapFinderTest, PreservesFactorySridAndBoundaryWidth) {
  GeometryFactory::Ptr srid_factory =
      GeometryFactory::create(factory_->getPrecisionModel(), 4326);
  WKTReader srid_reader(*srid_factory);

  auto bottom =
      srid_reader.read("POLYGON ((0 0, 10 0, 10 2, 5.5 2, 4.5 2, 0 2, 0 0))");
  auto top =
      srid_reader.read("POLYGON ((0 8, 4.5 8, 5.5 8, 10 8, 10 10, 0 10, 0 8))");
  auto left = srid_reader.read("POLYGON ((0 2, 4.5 2, 4.5 8, 0 8, 0 2))");
  auto right = srid_reader.read("POLYGON ((5.5 2, 10 2, 10 8, 5.5 8, 5.5 2))");
  std::vector<const Geometry*> coverage = {bottom.get(), top.get(), left.get(),
                                           right.get()};

  std::unique_ptr<Geometry> just_above =
      CoverageGapFinder::findGaps(coverage, 1.0001);
  ASSERT_NE(just_above, nullptr);
  EXPECT_FALSE(just_above->isEmpty());
  EXPECT_EQ(just_above->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_EQ(just_above->getSRID(), 4326);

  std::unique_ptr<Geometry> just_below =
      CoverageGapFinder::findGaps(coverage, 0.9999);
  ASSERT_NE(just_below, nullptr);
  EXPECT_TRUE(just_below->isEmpty());
  EXPECT_EQ(just_below->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(just_below->getSRID(), 4326);
}

TEST_F(CoverageGapFinderTest, OverlappingPolygonsThrowTopologyException) {
  // Identical overlapping polygons cancel all boundary segments during union.
  auto p1 = Read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto p2 = Read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  std::vector<const Geometry*> duplicate_overlap = {p1.get(), p2.get()};

  EXPECT_THROW(CoverageGapFinder::findGaps(duplicate_overlap, 1.0),
               geos::util::TopologyException);

  // Vector-clean partially overlapping polygons sharing boundary edges trigger
  // a topology conflict in CoverageUnion.
  auto v1 = Read("POLYGON ((0 0, 4 0, 6 0, 6 10, 4 10, 0 10, 0 0))");
  auto v2 = Read("POLYGON ((4 0, 6 0, 10 0, 10 10, 6 10, 4 10, 4 0))");
  std::vector<const Geometry*> partial_overlap = {v1.get(), v2.get()};

  EXPECT_THROW(CoverageGapFinder::findGaps(partial_overlap, 1.0),
               geos::util::TopologyException);
}

}  // namespace
}  // namespace geos_google
