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

// Unit tests for
// third_party/geos/src/operation/predicate/SegmentIntersectionTester.cpp.

#include "third_party/geos/include/geos/operation/predicate/SegmentIntersectionTester.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::io::WKTReader;
using geos::operation::predicate::SegmentIntersectionTester;

class SegmentIntersectionTesterTest : public ::testing::Test {
 protected:
  SegmentIntersectionTesterTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<LineString> ReadLineString(const std::string& wkt) const {
    return reader_.read<LineString>(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(SegmentIntersectionTesterTest, HasIntersectionBasicSegmentRelations) {
  {
    auto a = ReadLineString("LINESTRING (0 0, 10 10)");
    auto b = ReadLineString("LINESTRING (0 10, 10 0)");
    SegmentIntersectionTester tester;
    EXPECT_TRUE(tester.hasIntersection(*a, *b));
  }
  {
    // Endpoint touches interior (T-junction).
    auto a = ReadLineString("LINESTRING (0 0, 10 0)");
    auto b = ReadLineString("LINESTRING (5 0, 5 5)");
    SegmentIntersectionTester tester;
    EXPECT_TRUE(tester.hasIntersection(*a, *b));
  }
  {
    // Shared endpoint.
    auto a = ReadLineString("LINESTRING (0 0, 5 5)");
    auto b = ReadLineString("LINESTRING (5 5, 10 0)");
    SegmentIntersectionTester tester;
    EXPECT_TRUE(tester.hasIntersection(*a, *b));
  }
  {
    // Collinear overlap.
    auto a = ReadLineString("LINESTRING (0 0, 10 0)");
    auto b = ReadLineString("LINESTRING (4 0, 14 0)");
    SegmentIntersectionTester tester;
    EXPECT_TRUE(tester.hasIntersection(*a, *b));
  }
  {
    // Collinear touching at single endpoint.
    auto a = ReadLineString("LINESTRING (0 0, 5 0)");
    auto b = ReadLineString("LINESTRING (5 0, 10 0)");
    SegmentIntersectionTester tester;
    EXPECT_TRUE(tester.hasIntersection(*a, *b));
  }
  {
    // Collinear disjoint.
    auto a = ReadLineString("LINESTRING (0 0, 4 0)");
    auto b = ReadLineString("LINESTRING (6 0, 10 0)");
    SegmentIntersectionTester tester;
    EXPECT_FALSE(tester.hasIntersection(*a, *b));
  }
  {
    // Parallel disjoint.
    auto a = ReadLineString("LINESTRING (0 0, 10 0)");
    auto b = ReadLineString("LINESTRING (0 2, 10 2)");
    SegmentIntersectionTester tester;
    EXPECT_FALSE(tester.hasIntersection(*a, *b));
  }
  {
    // Disjoint segments with overlapping bounding boxes.
    auto a = ReadLineString("LINESTRING (0 0, 10 10)");
    auto b = ReadLineString("LINESTRING (0 8, 2 10)");
    SegmentIntersectionTester tester;
    EXPECT_FALSE(tester.hasIntersection(*a, *b));
  }
}

TEST_F(SegmentIntersectionTesterTest,
       HasIntersectionMultiSegmentAndShortCircuit) {
  {
    // Intersection only on final segments (segment index 2 in both lines).
    auto a = ReadLineString("LINESTRING (0 0, 2 0, 4 0, 8 4)");
    auto b = ReadLineString("LINESTRING (10 10, 8 10, 5 2, 7 2)");
    SegmentIntersectionTester tester;
    EXPECT_TRUE(tester.hasIntersection(*a, *b));
  }
  {
    // Intersection on the very first segment pair short-circuits remaining
    // work.
    auto a = ReadLineString("LINESTRING (0 0, 10 10, 20 20)");
    auto b = ReadLineString("LINESTRING (0 10, 10 0, 20 10)");
    SegmentIntersectionTester tester;
    EXPECT_TRUE(tester.hasIntersection(*a, *b));
  }
  {
    // Multi-segment chains with no intersecting segment pairs.
    auto a = ReadLineString("LINESTRING (0 0, 4 0, 4 4, 0 4)");
    auto b = ReadLineString("LINESTRING (1 1, 3 1, 3 3, 1 3)");
    SegmentIntersectionTester tester;
    EXPECT_FALSE(tester.hasIntersection(*a, *b));
  }
}

TEST_F(SegmentIntersectionTesterTest, HasIntersectionEmptyLineStrings) {
  auto empty = ReadLineString("LINESTRING EMPTY");
  auto line = ReadLineString("LINESTRING (0 0, 10 10)");

  {
    SegmentIntersectionTester tester;
    EXPECT_FALSE(tester.hasIntersection(*empty, *line));
  }
  {
    SegmentIntersectionTester tester;
    EXPECT_FALSE(tester.hasIntersection(*line, *empty));
  }
  {
    SegmentIntersectionTester tester;
    EXPECT_FALSE(tester.hasIntersection(*empty, *empty));
  }
}

TEST_F(SegmentIntersectionTesterTest, HasIntersectionWithEnvelopeFilter) {
  auto empty = ReadLineString("LINESTRING EMPTY");
  auto diag = ReadLineString("LINESTRING (0 0, 5 5, 10 10)");

  {
    SegmentIntersectionTester tester;
    EXPECT_FALSE(tester.hasIntersectionWithEnvelopeFilter(*empty, *diag));
  }
  {
    SegmentIntersectionTester tester;
    EXPECT_FALSE(tester.hasIntersectionWithEnvelopeFilter(*diag, *empty));
  }
  {
    // Segments outside diag's envelope are skipped; later segment intersects.
    auto test_line =
        ReadLineString("LINESTRING (-10 -10, -5 -5, -2 2, 0 10, 10 0)");
    SegmentIntersectionTester tester;
    EXPECT_TRUE(tester.hasIntersectionWithEnvelopeFilter(*diag, *test_line));
  }
  {
    // Mix of disjoint-envelope segments and an overlapping-envelope segment
    // that misses all segments of diag.
    auto miss_line =
        ReadLineString("LINESTRING (-10 0, -5 0, 0 8, 2 10, 20 20)");
    SegmentIntersectionTester tester;
    EXPECT_FALSE(tester.hasIntersectionWithEnvelopeFilter(*diag, *miss_line));
  }
  {
    // First segment of test_line intersects immediately; short-circuits
    // remaining segments.
    auto early_hit = ReadLineString("LINESTRING (0 4, 4 0, 50 50, 60 60)");
    SegmentIntersectionTester tester;
    EXPECT_TRUE(tester.hasIntersectionWithEnvelopeFilter(*diag, *early_hit));
  }
}

TEST_F(SegmentIntersectionTesterTest,
       HasIntersectionWithLineStringsAndStateReset) {
  auto query = ReadLineString("LINESTRING (0 0, 10 10)");
  auto miss1 = ReadLineString("LINESTRING (20 20, 30 30)");
  auto miss2 = ReadLineString("LINESTRING (0 8, 2 10)");
  auto hit1 = ReadLineString("LINESTRING (0 10, 10 0)");
  auto hit2 = ReadLineString("LINESTRING (5 0, 5 10)");

  SegmentIntersectionTester tester;

  // Empty vector returns false.
  LineString::ConstVect empty_lines;
  EXPECT_FALSE(tester.hasIntersectionWithLineStrings(*query, empty_lines));

  // Vector with no intersecting lines returns false.
  LineString::ConstVect non_intersecting{miss1.get(), miss2.get()};
  EXPECT_FALSE(tester.hasIntersectionWithLineStrings(*query, non_intersecting));

  // Second line intersects -> returns true.
  LineString::ConstVect hit_second{miss1.get(), hit1.get(), miss2.get()};
  EXPECT_TRUE(tester.hasIntersectionWithLineStrings(*query, hit_second));

  // Reusing tester with non-intersecting lines resets hasIntersectionVar.
  EXPECT_FALSE(tester.hasIntersectionWithLineStrings(*query, non_intersecting));

  // First line intersects -> short-circuits immediately and returns true.
  LineString::ConstVect hit_first{hit1.get(), hit2.get(), miss1.get()};
  EXPECT_TRUE(tester.hasIntersectionWithLineStrings(*query, hit_first));

  // Calling hasIntersectionWithLineStrings after hasIntersection sets true
  // still resets internal state.
  SegmentIntersectionTester reused;
  EXPECT_TRUE(reused.hasIntersection(*query, *hit1));
  EXPECT_FALSE(reused.hasIntersectionWithLineStrings(*query, non_intersecting));
}

TEST_F(SegmentIntersectionTesterTest, SupportsHigherDimensionCoordinates) {
  CoordinateSequence seq_z = CoordinateSequence::XYZ(0);
  seq_z.add(Coordinate{0.0, 0.0, 1.0});
  seq_z.add(Coordinate{10.0, 10.0, 2.0});

  CoordinateSequence seq_xym = CoordinateSequence::XYM(0);
  seq_xym.add(CoordinateXYM{0.0, 10.0, 10.0});
  seq_xym.add(CoordinateXYM{10.0, 0.0, 20.0});

  CoordinateSequence seq_xyzm = CoordinateSequence::XYZM(0);
  seq_xyzm.add(CoordinateXYZM{0.0, 10.0, 3.0, 100.0});
  seq_xyzm.add(CoordinateXYZM{10.0, 0.0, 4.0, 200.0});

  CoordinateSequence seq_disjoint = CoordinateSequence::XYZM(0);
  seq_disjoint.add(CoordinateXYZM{20.0, 20.0, 1.0, 1.0});
  seq_disjoint.add(CoordinateXYZM{30.0, 30.0, 2.0, 2.0});

  auto line_z = factory_->createLineString(seq_z);
  auto line_xym = factory_->createLineString(seq_xym);
  auto line_xyzm = factory_->createLineString(seq_xyzm);
  auto line_disjoint = factory_->createLineString(seq_disjoint);

  {
    SegmentIntersectionTester tester;
    EXPECT_TRUE(tester.hasIntersection(*line_z, *line_xyzm));
  }
  {
    SegmentIntersectionTester tester;
    EXPECT_TRUE(tester.hasIntersection(*line_xym, *line_z));
  }
  {
    SegmentIntersectionTester tester;
    EXPECT_TRUE(tester.hasIntersectionWithEnvelopeFilter(*line_z, *line_xyzm));
  }
  {
    SegmentIntersectionTester tester;
    LineString::ConstVect lines{line_disjoint.get(), line_xym.get(),
                                line_xyzm.get()};
    EXPECT_TRUE(tester.hasIntersectionWithLineStrings(*line_z, lines));
  }
}

}  // namespace
}  // namespace geos_google
