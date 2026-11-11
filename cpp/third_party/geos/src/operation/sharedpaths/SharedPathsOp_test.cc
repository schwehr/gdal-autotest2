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

#include "third_party/geos/include/geos/operation/sharedpaths/SharedPathsOp.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::geom::MultiLineString;
using geos::io::WKTReader;
using geos::operation::sharedpaths::SharedPathsOp;
using geos::util::IllegalArgumentException;

class SharedPathsOpTest : public ::testing::Test {
 protected:
  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};

  std::unique_ptr<Geometry> Read(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  std::unique_ptr<MultiLineString> ToMultiLineString(
      const SharedPathsOp::PathList& paths) const {
    std::vector<std::unique_ptr<LineString>> lines;
    lines.reserve(paths.size());
    for (const LineString* path : paths) {
      lines.push_back(path->clone());
    }
    return factory_->createMultiLineString(std::move(lines));
  }

  double TotalLength(const SharedPathsOp::PathList& paths) const {
    double total = 0.0;
    for (const LineString* path : paths) {
      total += path->getLength();
    }
    return total;
  }
};

TEST_F(SharedPathsOpTest, ThrowsOnNonLinealFirstGeometry) {
  auto valid_line = Read("LINESTRING (0 0, 10 0)");
  auto pt = Read("POINT (0 0)");
  auto multi_pt = Read("MULTIPOINT ((0 0), (1 1))");
  auto poly = Read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto multi_poly = Read("MULTIPOLYGON (((0 0, 5 0, 5 5, 0 5, 0 0)))");
  auto gc = Read("GEOMETRYCOLLECTION (LINESTRING (0 0, 10 0))");

  SharedPathsOp::PathList same;
  SharedPathsOp::PathList opposite;
  absl::Cleanup cleanup = [&]() {
    SharedPathsOp::clearEdges(same);
    SharedPathsOp::clearEdges(opposite);
  };

  EXPECT_THROW(SharedPathsOp::sharedPathsOp(*pt, *valid_line, same, opposite),
               IllegalArgumentException);
  EXPECT_THROW(
      SharedPathsOp::sharedPathsOp(*multi_pt, *valid_line, same, opposite),
      IllegalArgumentException);
  EXPECT_THROW(SharedPathsOp::sharedPathsOp(*poly, *valid_line, same, opposite),
               IllegalArgumentException);
  EXPECT_THROW(
      SharedPathsOp::sharedPathsOp(*multi_poly, *valid_line, same, opposite),
      IllegalArgumentException);
  EXPECT_THROW(SharedPathsOp::sharedPathsOp(*gc, *valid_line, same, opposite),
               IllegalArgumentException);
  EXPECT_TRUE(same.empty());
  EXPECT_TRUE(opposite.empty());
}

TEST_F(SharedPathsOpTest, ThrowsOnNonLinealSecondGeometry) {
  auto valid_line = Read("LINESTRING (0 0, 10 0)");
  auto pt = Read("POINT (5 0)");
  auto poly = Read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto gc = Read("GEOMETRYCOLLECTION (LINESTRING (0 0, 10 0))");

  EXPECT_THROW(SharedPathsOp(*valid_line, *pt), IllegalArgumentException);
  EXPECT_THROW(SharedPathsOp(*valid_line, *poly), IllegalArgumentException);
  EXPECT_THROW(SharedPathsOp(*valid_line, *gc), IllegalArgumentException);
}

TEST_F(SharedPathsOpTest, EmptyLinealInputsYieldNoPaths) {
  auto empty_line = Read("LINESTRING EMPTY");
  auto empty_multiline = Read("MULTILINESTRING EMPTY");
  auto line = Read("LINESTRING (0 0, 10 0)");

  SharedPathsOp::PathList same;
  SharedPathsOp::PathList opposite;
  absl::Cleanup cleanup = [&]() {
    SharedPathsOp::clearEdges(same);
    SharedPathsOp::clearEdges(opposite);
  };

  SharedPathsOp::sharedPathsOp(*empty_line, *line, same, opposite);
  EXPECT_TRUE(same.empty());
  EXPECT_TRUE(opposite.empty());

  SharedPathsOp::sharedPathsOp(*line, *empty_line, same, opposite);
  EXPECT_TRUE(same.empty());
  EXPECT_TRUE(opposite.empty());

  SharedPathsOp::sharedPathsOp(*empty_line, *empty_line, same, opposite);
  EXPECT_TRUE(same.empty());
  EXPECT_TRUE(opposite.empty());

  SharedPathsOp::sharedPathsOp(*empty_multiline, *line, same, opposite);
  EXPECT_TRUE(same.empty());
  EXPECT_TRUE(opposite.empty());
}

TEST_F(SharedPathsOpTest, DisjointLinesYieldNoPaths) {
  auto g1 = Read("LINESTRING (0 0, 10 0)");
  auto g2 = Read("LINESTRING (0 5, 10 5)");

  SharedPathsOp::PathList same;
  SharedPathsOp::PathList opposite;
  absl::Cleanup cleanup = [&]() {
    SharedPathsOp::clearEdges(same);
    SharedPathsOp::clearEdges(opposite);
  };

  SharedPathsOp::sharedPathsOp(*g1, *g2, same, opposite);
  EXPECT_TRUE(same.empty());
  EXPECT_TRUE(opposite.empty());
}

TEST_F(SharedPathsOpTest, PointCrossingOnlyYieldsNoPaths) {
  auto g1 = Read("LINESTRING (0 5, 10 5)");
  auto g2 = Read("LINESTRING (5 0, 5 10)");

  SharedPathsOp::PathList same;
  SharedPathsOp::PathList opposite;
  absl::Cleanup cleanup = [&]() {
    SharedPathsOp::clearEdges(same);
    SharedPathsOp::clearEdges(opposite);
  };

  SharedPathsOp::sharedPathsOp(*g1, *g2, same, opposite);
  EXPECT_TRUE(same.empty());
  EXPECT_TRUE(opposite.empty());
}

TEST_F(SharedPathsOpTest, MixedPointCrossingAndSharedSegment) {
  // Crossing at (2, 2) and shared segment along y = 0 from x = 5 to x = 10.
  auto g1 = Read("LINESTRING (0 0, 2 4, 4 0, 10 0)");
  auto g2 = Read("LINESTRING (0 2, 4 2, 5 0, 10 0)");
  auto expected = Read("LINESTRING (5 0, 10 0)");

  SharedPathsOp::PathList same;
  SharedPathsOp::PathList opposite;
  absl::Cleanup cleanup = [&]() {
    SharedPathsOp::clearEdges(same);
    SharedPathsOp::clearEdges(opposite);
  };

  SharedPathsOp::sharedPathsOp(*g1, *g2, same, opposite);
  ASSERT_EQ(same.size(), 1);
  EXPECT_TRUE(opposite.empty());
  EXPECT_TRUE(same[0]->equals(expected.get()));
}

TEST_F(SharedPathsOpTest, IdenticalAndReversedLineStrings) {
  auto fwd1 = Read("LINESTRING (0 0, 10 0)");
  auto fwd2 = Read("LINESTRING (0 0, 10 0)");
  auto rev1 = Read("LINESTRING (10 0, 0 0)");
  auto rev2 = Read("LINESTRING (10 0, 0 0)");
  auto expected = Read("LINESTRING (0 0, 10 0)");

  {
    SharedPathsOp::PathList same;
    SharedPathsOp::PathList opposite;
    absl::Cleanup cleanup = [&]() {
      SharedPathsOp::clearEdges(same);
      SharedPathsOp::clearEdges(opposite);
    };

    SharedPathsOp::sharedPathsOp(*fwd1, *fwd2, same, opposite);
    ASSERT_EQ(same.size(), 1);
    EXPECT_TRUE(opposite.empty());
    EXPECT_TRUE(same[0]->equals(expected.get()));
  }

  {
    SharedPathsOp::PathList same;
    SharedPathsOp::PathList opposite;
    absl::Cleanup cleanup = [&]() {
      SharedPathsOp::clearEdges(same);
      SharedPathsOp::clearEdges(opposite);
    };

    SharedPathsOp::sharedPathsOp(*fwd1, *rev1, same, opposite);
    EXPECT_TRUE(same.empty());
    ASSERT_EQ(opposite.size(), 1);
    EXPECT_TRUE(opposite[0]->equals(expected.get()));
  }

  {
    // Both lines run opposite to canonical coordinate order: still same dir.
    SharedPathsOp::PathList same;
    SharedPathsOp::PathList opposite;
    absl::Cleanup cleanup = [&]() {
      SharedPathsOp::clearEdges(same);
      SharedPathsOp::clearEdges(opposite);
    };

    SharedPathsOp::sharedPathsOp(*rev1, *rev2, same, opposite);
    ASSERT_EQ(same.size(), 1);
    EXPECT_TRUE(opposite.empty());
    EXPECT_TRUE(same[0]->equals(expected.get()));
  }

  {
    SharedPathsOp::PathList same;
    SharedPathsOp::PathList opposite;
    absl::Cleanup cleanup = [&]() {
      SharedPathsOp::clearEdges(same);
      SharedPathsOp::clearEdges(opposite);
    };

    SharedPathsOp::sharedPathsOp(*rev1, *fwd1, same, opposite);
    EXPECT_TRUE(same.empty());
    ASSERT_EQ(opposite.size(), 1);
    EXPECT_TRUE(opposite[0]->equals(expected.get()));
  }
}

TEST_F(SharedPathsOpTest, PartialOverlapAndMultiSegmentBend) {
  auto g1 = Read("LINESTRING (0 0, 5 0, 5 5, 10 5)");
  auto g2_same = Read("LINESTRING (2 0, 5 0, 5 5, 8 5)");
  auto g2_opp = Read("LINESTRING (8 5, 5 5, 5 0, 2 0)");
  auto expected = Read("LINESTRING (2 0, 5 0, 5 5, 8 5)");

  {
    SharedPathsOp::PathList same;
    SharedPathsOp::PathList opposite;
    absl::Cleanup cleanup = [&]() {
      SharedPathsOp::clearEdges(same);
      SharedPathsOp::clearEdges(opposite);
    };

    SharedPathsOp::sharedPathsOp(*g1, *g2_same, same, opposite);
    EXPECT_TRUE(opposite.empty());
    EXPECT_DOUBLE_EQ(TotalLength(same), 11.0);
    auto merged = ToMultiLineString(same)->Union();
    EXPECT_TRUE(merged->equals(expected.get()));
  }

  {
    SharedPathsOp::PathList same;
    SharedPathsOp::PathList opposite;
    absl::Cleanup cleanup = [&]() {
      SharedPathsOp::clearEdges(same);
      SharedPathsOp::clearEdges(opposite);
    };

    SharedPathsOp::sharedPathsOp(*g1, *g2_opp, same, opposite);
    EXPECT_TRUE(same.empty());
    EXPECT_DOUBLE_EQ(TotalLength(opposite), 11.0);
    auto merged = ToMultiLineString(opposite)->Union();
    EXPECT_TRUE(merged->equals(expected.get()));
  }
}

TEST_F(SharedPathsOpTest, MultiLineStringBothDirectionsInSingleCall) {
  auto g1 = Read("MULTILINESTRING ((0 0, 10 0), (0 10, 10 10), (20 0, 30 0))");
  auto g2 = Read("MULTILINESTRING ((2 0, 6 0), (8 10, 3 10), (50 50, 60 50))");

  auto expected_same = Read("LINESTRING (2 0, 6 0)");
  auto expected_opp = Read("LINESTRING (3 10, 8 10)");

  SharedPathsOp::PathList same;
  SharedPathsOp::PathList opposite;
  absl::Cleanup cleanup = [&]() {
    SharedPathsOp::clearEdges(same);
    SharedPathsOp::clearEdges(opposite);
  };

  SharedPathsOp::sharedPathsOp(*g1, *g2, same, opposite);
  ASSERT_EQ(same.size(), 1);
  ASSERT_EQ(opposite.size(), 1);
  EXPECT_TRUE(same[0]->equals(expected_same.get()));
  EXPECT_TRUE(opposite[0]->equals(expected_opp.get()));
}

TEST_F(SharedPathsOpTest, ClosedLineStringSharedAcrossClosureEndpoint) {
  auto closed = Read("LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  auto same_arc = Read("LINESTRING (0 10, 0 0, 10 0)");
  auto opp_arc = Read("LINESTRING (10 0, 0 0, 0 10)");

  {
    SharedPathsOp::PathList same;
    SharedPathsOp::PathList opposite;
    absl::Cleanup cleanup = [&]() {
      SharedPathsOp::clearEdges(same);
      SharedPathsOp::clearEdges(opposite);
    };

    SharedPathsOp::sharedPathsOp(*closed, *same_arc, same, opposite);
    EXPECT_FALSE(same.empty());
    EXPECT_TRUE(opposite.empty());
    EXPECT_DOUBLE_EQ(TotalLength(same), 20.0);
    auto merged = ToMultiLineString(same)->Union();
    EXPECT_TRUE(merged->equals(same_arc.get()));
  }

  {
    SharedPathsOp::PathList same;
    SharedPathsOp::PathList opposite;
    absl::Cleanup cleanup = [&]() {
      SharedPathsOp::clearEdges(same);
      SharedPathsOp::clearEdges(opposite);
    };

    SharedPathsOp::sharedPathsOp(*closed, *opp_arc, same, opposite);
    EXPECT_TRUE(same.empty());
    EXPECT_FALSE(opposite.empty());
    EXPECT_DOUBLE_EQ(TotalLength(opposite), 20.0);
    auto merged = ToMultiLineString(opposite)->Union();
    EXPECT_TRUE(merged->equals(opp_arc.get()));
  }
}

TEST_F(SharedPathsOpTest, LinearRingAcceptedAsLineal) {
  auto ring = Read("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  auto same_edge = Read("LINESTRING (0 0, 10 0)");
  auto opp_edge = Read("LINESTRING (10 0, 0 0)");

  SharedPathsOp::PathList same;
  SharedPathsOp::PathList opposite;
  absl::Cleanup cleanup = [&]() {
    SharedPathsOp::clearEdges(same);
    SharedPathsOp::clearEdges(opposite);
  };

  SharedPathsOp::sharedPathsOp(*ring, *same_edge, same, opposite);
  EXPECT_EQ(same.size(), 1);
  EXPECT_TRUE(opposite.empty());

  SharedPathsOp::sharedPathsOp(*ring, *opp_edge, same, opposite);
  EXPECT_EQ(same.size(), 1);
  EXPECT_EQ(opposite.size(), 1);
}

TEST_F(SharedPathsOpTest, InstanceGetSharedPathsAndClearEdges) {
  auto g1 = Read("LINESTRING (0 0, 10 0)");
  auto g2 = Read("LINESTRING (0 0, 5 0)");

  SharedPathsOp::PathList same;
  SharedPathsOp::PathList opposite;
  absl::Cleanup cleanup = [&]() {
    SharedPathsOp::clearEdges(same);
    SharedPathsOp::clearEdges(opposite);
  };

  // Clearing already-empty PathLists is a safe no-op.
  SharedPathsOp::clearEdges(same);
  EXPECT_TRUE(same.empty());

  SharedPathsOp op(*g1, *g2);
  op.getSharedPaths(same, opposite);
  EXPECT_EQ(same.size(), 1);
  EXPECT_TRUE(opposite.empty());

  // Calling getSharedPaths again appends new copies to the lists.
  op.getSharedPaths(same, opposite);
  EXPECT_EQ(same.size(), 2);
  EXPECT_TRUE(opposite.empty());

  SharedPathsOp::clearEdges(same);
  SharedPathsOp::clearEdges(opposite);
  EXPECT_TRUE(same.empty());
  EXPECT_TRUE(opposite.empty());
}

}  // namespace
}  // namespace geos_google
