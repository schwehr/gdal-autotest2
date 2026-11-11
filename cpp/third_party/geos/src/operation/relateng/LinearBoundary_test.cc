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

#include "third_party/geos/include/geos/operation/relateng/LinearBoundary.h"

#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/BoundaryNodeRule.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::algorithm::BoundaryNodeRule;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::io::WKTReader;
using geos::operation::relateng::LinearBoundary;

class LinearBoundaryTest : public ::testing::Test {
 protected:
  LinearBoundaryTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<LineString> ReadLine(const std::string& wkt) const {
    return reader_.read<LineString>(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(LinearBoundaryTest, EmptyLinesVectorHasNoBoundaryUnderAllRules) {
  std::vector<const LineString*> lines;
  const CoordinateXY probe(0.0, 0.0);

  const BoundaryNodeRule* rules[] = {
      &BoundaryNodeRule::getBoundaryRuleMod2(),
      &BoundaryNodeRule::getBoundaryOGCSFS(),
      &BoundaryNodeRule::getBoundaryEndPoint(),
      &BoundaryNodeRule::getBoundaryMultivalentEndPoint(),
      &BoundaryNodeRule::getBoundaryMonovalentEndPoint(),
  };

  for (const BoundaryNodeRule* rule : rules) {
    LinearBoundary lb(lines, *rule);
    EXPECT_FALSE(lb.hasBoundary()) << rule->toString();
    EXPECT_FALSE(lb.isBoundary(&probe)) << rule->toString();
  }
}

TEST_F(LinearBoundaryTest, EmptyLineStringsAreSkipped) {
  std::unique_ptr<LineString> empty1 = factory_->createLineString();
  std::unique_ptr<LineString> empty2 = ReadLine("LINESTRING EMPTY");
  std::vector<const LineString*> only_empty = {empty1.get(), empty2.get()};
  const CoordinateXY probe(0.0, 0.0);

  LinearBoundary empty_lb(only_empty, BoundaryNodeRule::getBoundaryEndPoint());
  EXPECT_FALSE(empty_lb.hasBoundary());
  EXPECT_FALSE(empty_lb.isBoundary(&probe));

  std::unique_ptr<LineString> line = ReadLine("LINESTRING (1 2, 5 2, 9 2)");
  std::vector<const LineString*> mixed = {empty1.get(), line.get(),
                                          empty2.get()};

  LinearBoundary mixed_lb(mixed, BoundaryNodeRule::getBoundaryRuleMod2());
  const CoordinateXY start(1.0, 2.0);
  const CoordinateXY mid(5.0, 2.0);
  const CoordinateXY end(9.0, 2.0);
  EXPECT_TRUE(mixed_lb.hasBoundary());
  EXPECT_TRUE(mixed_lb.isBoundary(&start));
  EXPECT_FALSE(mixed_lb.isBoundary(&mid));
  EXPECT_TRUE(mixed_lb.isBoundary(&end));
}

TEST_F(LinearBoundaryTest, SingleOpenLineStringAcrossBoundaryRules) {
  std::unique_ptr<LineString> line = ReadLine("LINESTRING (0 0, 5 5, 10 0)");
  std::vector<const LineString*> lines = {line.get()};

  const CoordinateXY start(0.0, 0.0);
  const CoordinateXY interior_vertex(5.0, 5.0);
  const CoordinateXY end(10.0, 0.0);
  const CoordinateXY off_line(2.0, 3.0);

  {
    LinearBoundary lb(lines, BoundaryNodeRule::getBoundaryRuleMod2());
    EXPECT_TRUE(lb.hasBoundary());
    EXPECT_TRUE(lb.isBoundary(&start));
    EXPECT_FALSE(lb.isBoundary(&interior_vertex));
    EXPECT_TRUE(lb.isBoundary(&end));
    EXPECT_FALSE(lb.isBoundary(&off_line));
  }
  {
    LinearBoundary lb(lines, BoundaryNodeRule::getBoundaryOGCSFS());
    EXPECT_TRUE(lb.hasBoundary());
    EXPECT_TRUE(lb.isBoundary(&start));
    EXPECT_TRUE(lb.isBoundary(&end));
    EXPECT_FALSE(lb.isBoundary(&interior_vertex));
  }
  {
    LinearBoundary lb(lines, BoundaryNodeRule::getBoundaryEndPoint());
    EXPECT_TRUE(lb.hasBoundary());
    EXPECT_TRUE(lb.isBoundary(&start));
    EXPECT_FALSE(lb.isBoundary(&interior_vertex));
    EXPECT_TRUE(lb.isBoundary(&end));
  }
  {
    LinearBoundary lb(lines, BoundaryNodeRule::getBoundaryMonovalentEndPoint());
    EXPECT_TRUE(lb.hasBoundary());
    EXPECT_TRUE(lb.isBoundary(&start));
    EXPECT_FALSE(lb.isBoundary(&interior_vertex));
    EXPECT_TRUE(lb.isBoundary(&end));
  }
  {
    // Degree-1 endpoints are not multivalent boundaries, so checkBoundary
    // visits every entry and returns false.
    LinearBoundary lb(lines,
                      BoundaryNodeRule::getBoundaryMultivalentEndPoint());
    EXPECT_FALSE(lb.hasBoundary());
    EXPECT_FALSE(lb.isBoundary(&start));
    EXPECT_FALSE(lb.isBoundary(&interior_vertex));
    EXPECT_FALSE(lb.isBoundary(&end));
  }
}

TEST_F(LinearBoundaryTest, ClosedLineStringHasDegreeTwoEndpoint) {
  std::unique_ptr<LineString> ring =
      ReadLine("LINESTRING (0 0, 10 0, 10 10, 0 0)");
  std::vector<const LineString*> lines = {ring.get()};

  const CoordinateXY closure(0.0, 0.0);
  const CoordinateXY corner(10.0, 0.0);

  {
    LinearBoundary lb(lines, BoundaryNodeRule::getBoundaryRuleMod2());
    EXPECT_FALSE(lb.hasBoundary());
    EXPECT_FALSE(lb.isBoundary(&closure));
    EXPECT_FALSE(lb.isBoundary(&corner));
  }
  {
    LinearBoundary lb(lines, BoundaryNodeRule::getBoundaryEndPoint());
    EXPECT_TRUE(lb.hasBoundary());
    EXPECT_TRUE(lb.isBoundary(&closure));
    EXPECT_FALSE(lb.isBoundary(&corner));
  }
  {
    LinearBoundary lb(lines,
                      BoundaryNodeRule::getBoundaryMultivalentEndPoint());
    EXPECT_TRUE(lb.hasBoundary());
    EXPECT_TRUE(lb.isBoundary(&closure));
    EXPECT_FALSE(lb.isBoundary(&corner));
  }
  {
    LinearBoundary lb(lines, BoundaryNodeRule::getBoundaryMonovalentEndPoint());
    EXPECT_FALSE(lb.hasBoundary());
    EXPECT_FALSE(lb.isBoundary(&closure));
  }
}

TEST_F(LinearBoundaryTest, SharedJunctionDegreesThreeAndFour) {
  std::unique_ptr<LineString> l1 = ReadLine("LINESTRING (0 5, 5 5)");
  std::unique_ptr<LineString> l2 = ReadLine("LINESTRING (5 5, 10 5)");
  std::unique_ptr<LineString> l3 = ReadLine("LINESTRING (5 5, 5 10)");
  std::unique_ptr<LineString> l4 = ReadLine("LINESTRING (5 5, 5 0)");

  const CoordinateXY center(5.0, 5.0);
  const CoordinateXY tip_west(0.0, 5.0);
  const CoordinateXY tip_east(10.0, 5.0);
  const CoordinateXY tip_north(5.0, 10.0);
  const CoordinateXY tip_south(5.0, 0.0);

  // Degree 3 at center, degree 1 at outer tips.
  std::vector<const LineString*> y_lines = {l1.get(), l2.get(), l3.get()};
  {
    LinearBoundary lb(y_lines, BoundaryNodeRule::getBoundaryRuleMod2());
    EXPECT_TRUE(lb.hasBoundary());
    EXPECT_TRUE(lb.isBoundary(&center));
    EXPECT_TRUE(lb.isBoundary(&tip_west));
    EXPECT_TRUE(lb.isBoundary(&tip_east));
    EXPECT_TRUE(lb.isBoundary(&tip_north));
  }
  {
    LinearBoundary lb(y_lines,
                      BoundaryNodeRule::getBoundaryMultivalentEndPoint());
    EXPECT_TRUE(lb.hasBoundary());
    EXPECT_TRUE(lb.isBoundary(&center));
    EXPECT_FALSE(lb.isBoundary(&tip_west));
    EXPECT_FALSE(lb.isBoundary(&tip_east));
    EXPECT_FALSE(lb.isBoundary(&tip_north));
  }
  {
    LinearBoundary lb(y_lines,
                      BoundaryNodeRule::getBoundaryMonovalentEndPoint());
    EXPECT_TRUE(lb.hasBoundary());
    EXPECT_FALSE(lb.isBoundary(&center));
    EXPECT_TRUE(lb.isBoundary(&tip_west));
    EXPECT_TRUE(lb.isBoundary(&tip_east));
    EXPECT_TRUE(lb.isBoundary(&tip_north));
  }

  // Degree 4 at center, degree 1 at all four outer tips.
  std::vector<const LineString*> cross_lines = {l1.get(), l2.get(), l3.get(),
                                                l4.get()};
  {
    LinearBoundary lb(cross_lines, BoundaryNodeRule::getBoundaryRuleMod2());
    EXPECT_TRUE(lb.hasBoundary());
    EXPECT_FALSE(lb.isBoundary(&center));
    EXPECT_TRUE(lb.isBoundary(&tip_south));
  }
  {
    LinearBoundary lb(cross_lines, BoundaryNodeRule::getBoundaryEndPoint());
    EXPECT_TRUE(lb.hasBoundary());
    EXPECT_TRUE(lb.isBoundary(&center));
    EXPECT_TRUE(lb.isBoundary(&tip_south));
  }
}

TEST_F(LinearBoundaryTest, MixedClosedAndOpenContinuesPastNonBoundaryEntry) {
  // Closed loop at (0, 0) sorts before (10, 0) in Coordinate::ConstIntMap;
  // checkBoundary must continue past (0, 0) (degree 2) to find (10, 0).
  std::unique_ptr<LineString> closed =
      ReadLine("LINESTRING (0 0, 4 0, 4 4, 0 0)");
  std::unique_ptr<LineString> open = ReadLine("LINESTRING (10 0, 20 0)");
  std::vector<const LineString*> lines = {closed.get(), open.get()};

  const CoordinateXY loop_pt(0.0, 0.0);
  const CoordinateXY open_start(10.0, 0.0);
  const CoordinateXY open_end(20.0, 0.0);

  LinearBoundary lb(lines, BoundaryNodeRule::getBoundaryRuleMod2());
  EXPECT_TRUE(lb.hasBoundary());
  EXPECT_FALSE(lb.isBoundary(&loop_pt));
  EXPECT_TRUE(lb.isBoundary(&open_start));
  EXPECT_TRUE(lb.isBoundary(&open_end));
}

TEST_F(LinearBoundaryTest, DegreeComparisonIgnoresZOrdinate) {
  auto cs1 =
      std::make_unique<CoordinateSequence>(std::initializer_list<Coordinate>{
          Coordinate(0.0, 0.0, 10.0), Coordinate(5.0, 0.0, 20.0)});
  auto cs2 =
      std::make_unique<CoordinateSequence>(std::initializer_list<Coordinate>{
          Coordinate(5.0, 0.0, 99.0), Coordinate(10.0, 0.0, 30.0)});

  std::unique_ptr<LineString> line1 =
      factory_->createLineString(std::move(cs1));
  std::unique_ptr<LineString> line2 =
      factory_->createLineString(std::move(cs2));
  std::vector<const LineString*> lines = {line1.get(), line2.get()};

  const CoordinateXY join_xy(5.0, 0.0);
  const CoordinateXY start_xy(0.0, 0.0);
  const CoordinateXY end_xy(10.0, 0.0);

  LinearBoundary lb(lines, BoundaryNodeRule::getBoundaryRuleMod2());
  EXPECT_TRUE(lb.hasBoundary());
  EXPECT_FALSE(lb.isBoundary(&join_xy));
  EXPECT_TRUE(lb.isBoundary(&start_xy));
  EXPECT_TRUE(lb.isBoundary(&end_xy));
}

}  // namespace
}  // namespace geos_google
