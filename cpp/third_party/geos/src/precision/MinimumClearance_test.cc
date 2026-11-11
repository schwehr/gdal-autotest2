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

// Unit tests for third_party/geos/src/precision/MinimumClearance.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - Covers empty/degenerate geometries, lazy result caching in both method
//   orderings, point-point vs vertex-segment vs zero-distance underflow cases,
//   multi-facet polygons/polylines, and the unsupported-component error path.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/precision/MinimumClearance.cpp

#include "third_party/geos/include/geos/precision/MinimumClearance.h"

#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/GEOSException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::io::WKTReader;
using geos::precision::MinimumClearance;
using geos::util::GEOSException;

class MinimumClearanceTest : public ::testing::Test {
 protected:
  MinimumClearanceTest()
      : factory_(GeometryFactory::create()), reader_(factory_.get()) {}

  Geometry::Ptr ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(MinimumClearanceTest, EmptyAndSinglePointReturnInfinityAndEmptyLine) {
  for (const std::string& wkt :
       {"POINT EMPTY", "LINESTRING EMPTY", "POLYGON EMPTY",
        "GEOMETRYCOLLECTION EMPTY", "POINT (3 4)", "MULTIPOINT ((3 4), (3 4))",
        "LINESTRING (2 2, 2 2)"}) {
    SCOPED_TRACE(wkt);
    Geometry::Ptr geom = ReadWkt(wkt);

    MinimumClearance mc_dist_first(geom.get());
    EXPECT_EQ(mc_dist_first.getDistance(), geos::DoubleInfinity);
    std::unique_ptr<LineString> line_after_dist = mc_dist_first.getLine();
    ASSERT_NE(line_after_dist, nullptr);
    EXPECT_TRUE(line_after_dist->isEmpty());
    EXPECT_EQ(line_after_dist->getFactory(), geom->getFactory());

    MinimumClearance mc_line_first(geom.get());
    std::unique_ptr<LineString> line_first = mc_line_first.getLine();
    ASSERT_NE(line_first, nullptr);
    EXPECT_TRUE(line_first->isEmpty());
    EXPECT_EQ(mc_line_first.getDistance(), geos::DoubleInfinity);
  }
}

TEST_F(MinimumClearanceTest, MultiPointReturnsNearestVertexPair) {
  Geometry::Ptr mpt = ReadWkt("MULTIPOINT ((0 0), (3 4), (10 10))");
  MinimumClearance mc(mpt.get());

  EXPECT_DOUBLE_EQ(mc.getDistance(), 5.0);
  // Repeated call exercises cached minClearancePts path.
  EXPECT_DOUBLE_EQ(mc.getDistance(), 5.0);

  std::unique_ptr<LineString> line = mc.getLine();
  ASSERT_NE(line, nullptr);
  ASSERT_EQ(line->getNumPoints(), 2U);
  EXPECT_DOUBLE_EQ(line->getLength(), 5.0);

  const CoordinateSequence* seq = line->getCoordinatesRO();
  CoordinateXY p0 = seq->getAt<CoordinateXY>(0);
  CoordinateXY p1 = seq->getAt<CoordinateXY>(1);
  const bool forward = p0.equals2D({0.0, 0.0}) && p1.equals2D({3.0, 4.0});
  const bool reverse = p0.equals2D({3.0, 4.0}) && p1.equals2D({0.0, 0.0});
  EXPECT_TRUE(forward || reverse);

  // Calling getLine() again produces an independent cloned LineString.
  std::unique_ptr<LineString> line_copy = mc.getLine();
  EXPECT_NE(line_copy.get(), line.get());
  EXPECT_TRUE(line_copy->equalsExact(line.get()));
}

TEST_F(MinimumClearanceTest, VertexToSegmentProjection) {
  Geometry::Ptr gc =
      ReadWkt("GEOMETRYCOLLECTION (POINT (5 1), LINESTRING (0 0, 10 0))");
  MinimumClearance mc(gc.get());

  EXPECT_DOUBLE_EQ(mc.getDistance(), 1.0);
  std::unique_ptr<LineString> line = mc.getLine();
  ASSERT_NE(line, nullptr);
  ASSERT_EQ(line->getNumPoints(), 2U);
  const CoordinateSequence* seq = line->getCoordinatesRO();
  EXPECT_DOUBLE_EQ(seq->getAt<CoordinateXY>(0).x, 5.0);
  EXPECT_DOUBLE_EQ(seq->getAt<CoordinateXY>(0).y, 1.0);
  EXPECT_DOUBLE_EQ(seq->getAt<CoordinateXY>(1).x, 5.0);
  EXPECT_DOUBLE_EQ(seq->getAt<CoordinateXY>(1).y, 0.0);

  // Reverse input component order as well.
  Geometry::Ptr gc_rev =
      ReadWkt("GEOMETRYCOLLECTION (LINESTRING (0 0, 10 0), POINT (5 1))");
  MinimumClearance mc_rev(gc_rev.get());
  EXPECT_DOUBLE_EQ(mc_rev.getDistance(), 1.0);
}

TEST_F(MinimumClearanceTest, VertexDistanceUnderflowsToZeroReturnsEarly) {
  // Coordinates differ (equals2D is false), but squared delta (1e-200)^2
  // underflows IEEE-754 double to 0.0 in distance(), hitting early return in
  // both vertexDistance() and distance() when facet size > 1.
  auto coords =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {1e-200, 0.0}, {10.0, 0.0}});
  std::unique_ptr<LineString> line =
      factory_->createLineString(std::move(coords));

  MinimumClearance mc(line.get());
  EXPECT_DOUBLE_EQ(mc.getDistance(), 0.0);
  std::unique_ptr<LineString> min_line = mc.getLine();
  ASSERT_NE(min_line, nullptr);
  ASSERT_EQ(min_line->getNumPoints(), 2U);
  EXPECT_DOUBLE_EQ(min_line->getLength(), 0.0);
}

TEST_F(MinimumClearanceTest, SegmentDistanceZeroReturnsEarlyRegardlessOfOrder) {
  // Mutual T-junction: (2, 0) lies interior to segment (0,0)-(4,0) and (4, 0)
  // lies interior to segment (2,0)-(6,0). Whichever order the STRtree visits
  // the two components, segmentDistance(fs1, fs2) returns 0.0 and triggers the
  // second early return in distance().
  Geometry::Ptr mls = ReadWkt("MULTILINESTRING ((0 0, 4 0), (2 -2, 2 0, 6 0))");
  MinimumClearance mc(mls.get());

  EXPECT_DOUBLE_EQ(mc.getDistance(), 0.0);
  std::unique_ptr<LineString> min_line = mc.getLine();
  ASSERT_NE(min_line, nullptr);
  ASSERT_EQ(min_line->getNumPoints(), 2U);
  EXPECT_DOUBLE_EQ(min_line->getLength(), 0.0);
}

TEST_F(MinimumClearanceTest, PolygonWithHoleAndLongMultiFacetPolyline) {
  // Outer square [0, 10]x[0, 10] with diamond hole whose bottom vertex (5, 1)
  // sits at distance 1.0 from bottom edge y = 0.
  Geometry::Ptr poly_with_hole = ReadWkt(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(5 1, 8 5, 5 9, 2 5, 5 1))");
  MinimumClearance mc_poly(poly_with_hole.get());
  EXPECT_DOUBLE_EQ(mc_poly.getDistance(), 1.0);
  std::unique_ptr<LineString> poly_line = mc_poly.getLine();
  ASSERT_NE(poly_line, nullptr);
  EXPECT_DOUBLE_EQ(poly_line->getLength(), 1.0);

  // Single LineString with > 6 vertices spans multiple FacetSequences in
  // FacetSequenceTreeBuilder.
  Geometry::Ptr long_line = ReadWkt(
      "LINESTRING (0 0, 10 0, 20 0, 20 10, 10 10, 0 10, 0 4, 8 4, 8 0.5)");
  MinimumClearance mc_long(long_line.get());
  EXPECT_DOUBLE_EQ(mc_long.getDistance(), 0.5);
  EXPECT_DOUBLE_EQ(mc_long.getLine()->getLength(), 0.5);

  // Distance computation is 2D even when Z ordinates are present.
  Geometry::Ptr mpt_z = ReadWkt("MULTIPOINT Z ((0 0 0), (3 4 100))");
  MinimumClearance mc_z(mpt_z.get());
  EXPECT_DOUBLE_EQ(mc_z.getDistance(), 5.0);
}

TEST_F(MinimumClearanceTest, ThrowsOnNonEmptyUnsupportedCurvedGeometry) {
  // FacetSequenceTreeBuilder ignores non-linear CircularStrings, yielding an
  // empty STRtree on a non-empty geometry and throwing GEOSException.
  Geometry::Ptr curve1 = ReadWkt("CIRCULARSTRING (0 0, 1 1, 2 0)");
  ASSERT_FALSE(curve1->isEmpty());
  MinimumClearance mc1(curve1.get());
  EXPECT_THROW(mc1.getDistance(), GEOSException);

  Geometry::Ptr curve2 = ReadWkt("CIRCULARSTRING (0 0, 1 1, 2 0)");
  MinimumClearance mc2(curve2.get());
  EXPECT_THROW(mc2.getLine(), GEOSException);
}

}  // namespace
}  // namespace geos_google
