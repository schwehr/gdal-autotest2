// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "third_party/geos/include/geos/operation/distance/FacetSequence.h"

#include <cmath>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/operation/distance/GeometryLocation.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::geom::Envelope;
using ::geos::geom::GeometryFactory;
using ::geos::geom::LineString;
using ::geos::geom::Point;
using ::geos::operation::distance::FacetSequence;
using ::geos::operation::distance::GeometryLocation;

TEST(FacetSequenceTest, SubrangeAccessorsAndEnvelopeExcludeOutOfRangePoints) {
  CoordinateSequence seq{Coordinate(100.0, 100.0), Coordinate(0.0, 0.0),
                         Coordinate(3.0, 1.0), Coordinate(1.0, 4.0),
                         Coordinate(-50.0, -50.0)};

  FacetSequence line_fs(&seq, 1, 4);
  EXPECT_EQ(line_fs.size(), 3u);
  EXPECT_FALSE(line_fs.isPoint());
  EXPECT_EQ(line_fs.getCoordinate(0), &seq.getAt(1));
  EXPECT_EQ(line_fs.getCoordinate(1), &seq.getAt(2));
  EXPECT_EQ(line_fs.getCoordinate(2), &seq.getAt(3));
  EXPECT_EQ(*line_fs.getEnvelope(), Envelope(0.0, 3.0, 0.0, 4.0));

  FacetSequence point_fs(&seq, 2, 3);
  EXPECT_EQ(point_fs.size(), 1u);
  EXPECT_TRUE(point_fs.isPoint());
  EXPECT_EQ(point_fs.getCoordinate(0), &seq.getAt(2));
  EXPECT_EQ(*point_fs.getEnvelope(), Envelope(3.0, 3.0, 1.0, 1.0));
}

TEST(FacetSequenceTest, EmptySubrangeHasZeroSizeAndNullEnvelope) {
  CoordinateSequence seq{Coordinate(1.0, 2.0), Coordinate(3.0, 4.0)};
  FacetSequence empty_fs(&seq, 1, 1);

  EXPECT_EQ(empty_fs.size(), 0u);
  EXPECT_FALSE(empty_fs.isPoint());
  EXPECT_TRUE(empty_fs.getEnvelope()->isNull());
}

TEST(FacetSequenceTest, PointToPointDistanceAndNearestLocations) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> pt_a = factory->createPoint(CoordinateXY(1.0, 2.0));
  std::unique_ptr<Point> pt_b = factory->createPoint(CoordinateXY(4.0, 6.0));

  CoordinateSequence seq_a{Coordinate(99.0, 99.0), Coordinate(1.0, 2.0),
                           Coordinate(88.0, 88.0)};
  CoordinateSequence seq_b{Coordinate(0.0, 0.0), Coordinate(-5.0, -5.0),
                           Coordinate(4.0, 6.0)};

  FacetSequence fs_a(pt_a.get(), &seq_a, 1, 2);
  FacetSequence fs_b(pt_b.get(), &seq_b, 2, 3);

  EXPECT_DOUBLE_EQ(fs_a.distance(fs_b), 5.0);

  std::vector<GeometryLocation> locs = fs_a.nearestLocations(fs_b);
  ASSERT_EQ(locs.size(), 2u);
  EXPECT_EQ(locs[0].getGeometryComponent(), pt_a.get());
  EXPECT_EQ(locs[0].getSegmentIndex(), 1u);
  EXPECT_FALSE(locs[0].isInsideArea());
  EXPECT_DOUBLE_EQ(locs[0].getCoordinate().x, 1.0);
  EXPECT_DOUBLE_EQ(locs[0].getCoordinate().y, 2.0);

  EXPECT_EQ(locs[1].getGeometryComponent(), pt_b.get());
  EXPECT_EQ(locs[1].getSegmentIndex(), 2u);
  EXPECT_FALSE(locs[1].isInsideArea());
  EXPECT_DOUBLE_EQ(locs[1].getCoordinate().x, 4.0);
  EXPECT_DOUBLE_EQ(locs[1].getCoordinate().y, 6.0);

  // Three-argument constructor leaves geometry component pointers null.
  CoordinateSequence seq_coincident{Coordinate(4.0, 6.0)};
  FacetSequence fs_null_a(&seq_b, 2, 3);
  FacetSequence fs_null_b(&seq_coincident, 0, 1);
  EXPECT_DOUBLE_EQ(fs_null_a.distance(fs_null_b), 0.0);

  std::vector<GeometryLocation> null_locs =
      fs_null_a.nearestLocations(fs_null_b);
  ASSERT_EQ(null_locs.size(), 2u);
  EXPECT_EQ(null_locs[0].getGeometryComponent(), nullptr);
  EXPECT_EQ(null_locs[1].getGeometryComponent(), nullptr);
  EXPECT_DOUBLE_EQ(null_locs[0].getCoordinate().x, 4.0);
  EXPECT_DOUBLE_EQ(null_locs[1].getCoordinate().y, 6.0);
}

TEST(FacetSequenceTest, PointToLineDistanceAndNearestLocations) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  CoordinateSequence line_coords{
      Coordinate(-100.0, 0.0), Coordinate(0.0, 10.0),
      Coordinate(10.0, 10.0),  Coordinate(10.0, 0.0),
      Coordinate(20.0, 10.0),  Coordinate(100.0, 100.0)};
  std::unique_ptr<LineString> line = factory->createLineString(line_coords);
  FacetSequence line_fs(line.get(), &line_coords, 1, 5);

  // Off-line point: seg 1 is dist 6, seg 2 is dist 3 (best), seg 3 is dist
  // ~4.95.
  CoordinateSequence pt_coords{Coordinate(7.0, 4.0)};
  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(7.0, 4.0));
  FacetSequence pt_fs(pt.get(), &pt_coords, 0, 1);

  EXPECT_DOUBLE_EQ(pt_fs.distance(line_fs), 3.0);

  std::vector<GeometryLocation> locs = pt_fs.nearestLocations(line_fs);
  ASSERT_EQ(locs.size(), 2u);
  EXPECT_EQ(locs[0].getGeometryComponent(), pt.get());
  EXPECT_EQ(locs[0].getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(locs[0].getCoordinate().x, 7.0);
  EXPECT_DOUBLE_EQ(locs[0].getCoordinate().y, 4.0);

  EXPECT_EQ(locs[1].getGeometryComponent(), line.get());
  EXPECT_EQ(locs[1].getSegmentIndex(), 2u);
  EXPECT_DOUBLE_EQ(locs[1].getCoordinate().x, 10.0);
  EXPECT_DOUBLE_EQ(locs[1].getCoordinate().y, 4.0);

  // Point lying directly on seg 1 triggers early exit when minDistance <= 0.
  CoordinateSequence on_line_coords{Coordinate(5.0, 10.0)};
  std::unique_ptr<Point> on_line_pt =
      factory->createPoint(CoordinateXY(5.0, 10.0));
  FacetSequence on_line_fs(on_line_pt.get(), &on_line_coords, 0, 1);

  EXPECT_DOUBLE_EQ(on_line_fs.distance(line_fs), 0.0);
  std::vector<GeometryLocation> on_locs = on_line_fs.nearestLocations(line_fs);
  ASSERT_EQ(on_locs.size(), 2u);
  EXPECT_EQ(on_locs[0].getGeometryComponent(), on_line_pt.get());
  EXPECT_EQ(on_locs[1].getGeometryComponent(), line.get());
  EXPECT_EQ(on_locs[1].getSegmentIndex(), 1u);
  EXPECT_DOUBLE_EQ(on_locs[1].getCoordinate().x, 5.0);
  EXPECT_DOUBLE_EQ(on_locs[1].getCoordinate().y, 10.0);

  // Point lying on the last segment (seg 3) improves distance across earlier
  // segments before hitting the minDistance <= 0 early exit.
  CoordinateSequence on_last_coords{Coordinate(15.0, 5.0)};
  std::unique_ptr<Point> on_last_pt =
      factory->createPoint(CoordinateXY(15.0, 5.0));
  FacetSequence on_last_fs(on_last_pt.get(), &on_last_coords, 0, 1);

  EXPECT_DOUBLE_EQ(on_last_fs.distance(line_fs), 0.0);
  std::vector<GeometryLocation> last_locs =
      on_last_fs.nearestLocations(line_fs);
  ASSERT_EQ(last_locs.size(), 2u);
  EXPECT_EQ(last_locs[0].getGeometryComponent(), on_last_pt.get());
  EXPECT_EQ(last_locs[1].getGeometryComponent(), line.get());
  EXPECT_EQ(last_locs[1].getSegmentIndex(), 3u);
  EXPECT_DOUBLE_EQ(last_locs[1].getCoordinate().x, 15.0);
  EXPECT_DOUBLE_EQ(last_locs[1].getCoordinate().y, 5.0);
}

TEST(FacetSequenceTest, PointToLineInfiniteDistancePopulatesLocations) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  CoordinateSequence line_coords{Coordinate(0.0, 0.0), Coordinate(1.0, 0.0)};
  CoordinateSequence huge_pt_coords{Coordinate(0.0, 1e200)};
  std::unique_ptr<LineString> line = factory->createLineString(line_coords);
  std::unique_ptr<Point> huge_pt =
      factory->createPoint(CoordinateXY(0.0, 1e200));

  FacetSequence line_fs(line.get(), &line_coords, 0, 2);
  FacetSequence pt_fs(huge_pt.get(), &huge_pt_coords, 0, 1);

  EXPECT_TRUE(std::isinf(pt_fs.distance(line_fs)));
  EXPECT_TRUE(std::isinf(line_fs.distance(pt_fs)));

  std::vector<GeometryLocation> locs = pt_fs.nearestLocations(line_fs);
  ASSERT_EQ(locs.size(), 2u);
  EXPECT_EQ(locs[0].getGeometryComponent(), huge_pt.get());
  EXPECT_EQ(locs[1].getGeometryComponent(), line.get());

  std::vector<GeometryLocation> rev_locs = line_fs.nearestLocations(pt_fs);
  ASSERT_EQ(rev_locs.size(), 2u);
  EXPECT_EQ(rev_locs[0].getGeometryComponent(), line.get());
  EXPECT_DOUBLE_EQ(rev_locs[0].getCoordinate().x, 1.0);
  EXPECT_DOUBLE_EQ(rev_locs[0].getCoordinate().y, 0.0);
  EXPECT_DOUBLE_EQ(rev_locs[1].getCoordinate().x, 0.0);
  EXPECT_DOUBLE_EQ(rev_locs[1].getCoordinate().y, 1e200);
}

TEST(FacetSequenceTest, LineToPointDistanceAndNearestLocations) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  CoordinateSequence line_coords{Coordinate(-100.0, 0.0), Coordinate(0.0, 10.0),
                                 Coordinate(10.0, 10.0), Coordinate(10.0, 0.0),
                                 Coordinate(20.0, 10.0)};
  std::unique_ptr<LineString> line = factory->createLineString(line_coords);
  FacetSequence line_fs(line.get(), &line_coords, 1, 5);

  CoordinateSequence pt_coords{Coordinate(7.0, 4.0)};
  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(7.0, 4.0));
  FacetSequence pt_fs(pt.get(), &pt_coords, 0, 1);

  EXPECT_DOUBLE_EQ(line_fs.distance(pt_fs), 3.0);

  std::vector<GeometryLocation> locs = line_fs.nearestLocations(pt_fs);
  ASSERT_EQ(locs.size(), 2u);
  EXPECT_EQ(locs[0].getGeometryComponent(), line.get());
  EXPECT_EQ(locs[0].getSegmentIndex(), 2u);
  EXPECT_DOUBLE_EQ(locs[0].getCoordinate().x, 10.0);
  EXPECT_DOUBLE_EQ(locs[0].getCoordinate().y, 4.0);
  EXPECT_DOUBLE_EQ(locs[1].getCoordinate().x, 7.0);
  EXPECT_DOUBLE_EQ(locs[1].getCoordinate().y, 4.0);

  // Coincident point on seg 1.
  CoordinateSequence on_coords{Coordinate(5.0, 10.0)};
  std::unique_ptr<Point> on_pt = factory->createPoint(CoordinateXY(5.0, 10.0));
  FacetSequence on_fs(on_pt.get(), &on_coords, 0, 1);

  EXPECT_DOUBLE_EQ(line_fs.distance(on_fs), 0.0);
  std::vector<GeometryLocation> on_locs = line_fs.nearestLocations(on_fs);
  ASSERT_EQ(on_locs.size(), 2u);
  EXPECT_EQ(on_locs[0].getGeometryComponent(), line.get());
  EXPECT_EQ(on_locs[0].getSegmentIndex(), 1u);
  EXPECT_DOUBLE_EQ(on_locs[0].getCoordinate().x, 5.0);
  EXPECT_DOUBLE_EQ(on_locs[0].getCoordinate().y, 10.0);
  EXPECT_DOUBLE_EQ(on_locs[1].getCoordinate().x, 5.0);
  EXPECT_DOUBLE_EQ(on_locs[1].getCoordinate().y, 10.0);
}

TEST(FacetSequenceTest, LineToLineCrossingTouchingAndDisjointSegments) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Crossing segments intersect at (2, 2).
  CoordinateSequence cross_a_coords{Coordinate(0.0, 0.0), Coordinate(4.0, 4.0)};
  CoordinateSequence cross_b_coords{Coordinate(0.0, 4.0), Coordinate(4.0, 0.0)};
  std::unique_ptr<LineString> cross_a =
      factory->createLineString(cross_a_coords);
  std::unique_ptr<LineString> cross_b =
      factory->createLineString(cross_b_coords);
  FacetSequence cross_fs_a(cross_a.get(), &cross_a_coords, 0, 2);
  FacetSequence cross_fs_b(cross_b.get(), &cross_b_coords, 0, 2);

  EXPECT_DOUBLE_EQ(cross_fs_a.distance(cross_fs_b), 0.0);
  std::vector<GeometryLocation> cross_locs =
      cross_fs_a.nearestLocations(cross_fs_b);
  ASSERT_EQ(cross_locs.size(), 2u);
  EXPECT_EQ(cross_locs[0].getGeometryComponent(), cross_a.get());
  EXPECT_EQ(cross_locs[1].getGeometryComponent(), cross_b.get());
  EXPECT_DOUBLE_EQ(cross_locs[0].getCoordinate().x, 2.0);
  EXPECT_DOUBLE_EQ(cross_locs[0].getCoordinate().y, 2.0);
  EXPECT_DOUBLE_EQ(cross_locs[1].getCoordinate().x, 2.0);
  EXPECT_DOUBLE_EQ(cross_locs[1].getCoordinate().y, 2.0);

  // T-junction touching at (2, 0).
  CoordinateSequence tee_a_coords{Coordinate(0.0, 0.0), Coordinate(4.0, 0.0)};
  CoordinateSequence tee_b_coords{Coordinate(2.0, 0.0), Coordinate(2.0, 5.0)};
  FacetSequence tee_fs_a(&tee_a_coords, 0, 2);
  FacetSequence tee_fs_b(&tee_b_coords, 0, 2);
  EXPECT_DOUBLE_EQ(tee_fs_a.distance(tee_fs_b), 0.0);
  std::vector<GeometryLocation> tee_locs = tee_fs_a.nearestLocations(tee_fs_b);
  ASSERT_EQ(tee_locs.size(), 2u);
  EXPECT_DOUBLE_EQ(tee_locs[0].getCoordinate().x, 2.0);
  EXPECT_DOUBLE_EQ(tee_locs[0].getCoordinate().y, 0.0);
  EXPECT_DOUBLE_EQ(tee_locs[1].getCoordinate().x, 2.0);
  EXPECT_DOUBLE_EQ(tee_locs[1].getCoordinate().y, 0.0);

  // Collinear disjoint segments separated by 3.0.
  CoordinateSequence col_a_coords{Coordinate(0.0, 0.0), Coordinate(2.0, 0.0)};
  CoordinateSequence col_b_coords{Coordinate(5.0, 0.0), Coordinate(7.0, 0.0)};
  FacetSequence col_fs_a(&col_a_coords, 0, 2);
  FacetSequence col_fs_b(&col_b_coords, 0, 2);
  EXPECT_DOUBLE_EQ(col_fs_a.distance(col_fs_b), 3.0);
  std::vector<GeometryLocation> col_locs = col_fs_a.nearestLocations(col_fs_b);
  ASSERT_EQ(col_locs.size(), 2u);
  EXPECT_DOUBLE_EQ(col_locs[0].getCoordinate().x, 2.0);
  EXPECT_DOUBLE_EQ(col_locs[0].getCoordinate().y, 0.0);
  EXPECT_DOUBLE_EQ(col_locs[1].getCoordinate().x, 5.0);
  EXPECT_DOUBLE_EQ(col_locs[1].getCoordinate().y, 0.0);

  // Collinear overlapping segments intersect with distance 0.0.
  CoordinateSequence ov_a_coords{Coordinate(0.0, 0.0), Coordinate(5.0, 0.0)};
  CoordinateSequence ov_b_coords{Coordinate(2.0, 0.0), Coordinate(8.0, 0.0)};
  FacetSequence ov_fs_a(&ov_a_coords, 0, 2);
  FacetSequence ov_fs_b(&ov_b_coords, 0, 2);
  EXPECT_DOUBLE_EQ(ov_fs_a.distance(ov_fs_b), 0.0);
  std::vector<GeometryLocation> ov_locs = ov_fs_a.nearestLocations(ov_fs_b);
  ASSERT_EQ(ov_locs.size(), 2u);
  EXPECT_DOUBLE_EQ(ov_locs[0].getCoordinate().y, 0.0);
  EXPECT_EQ(ov_locs[0].getCoordinate(), ov_locs[1].getCoordinate());
  EXPECT_GE(ov_locs[0].getCoordinate().x, 2.0);
  EXPECT_LE(ov_locs[0].getCoordinate().x, 5.0);

  // Skew sub-ranges with non-zero start indices.
  CoordinateSequence skew_a_coords{Coordinate(-1.0, -1.0), Coordinate(0.0, 0.0),
                                   Coordinate(10.0, 0.0)};
  CoordinateSequence skew_b_coords{Coordinate(99.0, 99.0), Coordinate(4.0, 3.0),
                                   Coordinate(8.0, 7.0)};
  std::unique_ptr<LineString> skew_a = factory->createLineString(skew_a_coords);
  std::unique_ptr<LineString> skew_b = factory->createLineString(skew_b_coords);
  FacetSequence skew_fs_a(skew_a.get(), &skew_a_coords, 1, 3);
  FacetSequence skew_fs_b(skew_b.get(), &skew_b_coords, 1, 3);

  EXPECT_DOUBLE_EQ(skew_fs_a.distance(skew_fs_b), 3.0);
  std::vector<GeometryLocation> skew_locs =
      skew_fs_a.nearestLocations(skew_fs_b);
  ASSERT_EQ(skew_locs.size(), 2u);
  EXPECT_EQ(skew_locs[0].getGeometryComponent(), skew_a.get());
  EXPECT_EQ(skew_locs[0].getSegmentIndex(), 1u);
  EXPECT_DOUBLE_EQ(skew_locs[0].getCoordinate().x, 4.0);
  EXPECT_DOUBLE_EQ(skew_locs[0].getCoordinate().y, 0.0);
  EXPECT_EQ(skew_locs[1].getGeometryComponent(), skew_b.get());
  EXPECT_EQ(skew_locs[1].getSegmentIndex(), 1u);
  EXPECT_DOUBLE_EQ(skew_locs[1].getCoordinate().x, 4.0);
  EXPECT_DOUBLE_EQ(skew_locs[1].getCoordinate().y, 3.0);
}

TEST(FacetSequenceTest, LineToLineSkipsZeroLengthSegments) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Repeated point in first sequence (seg 0 is zero-length).
  CoordinateSequence rep_a_coords{Coordinate(0.0, 0.0), Coordinate(0.0, 0.0),
                                  Coordinate(4.0, 0.0)};
  CoordinateSequence line_b_coords{Coordinate(2.0, 3.0), Coordinate(4.0, 3.0)};
  std::unique_ptr<LineString> rep_a = factory->createLineString(rep_a_coords);
  std::unique_ptr<LineString> line_b = factory->createLineString(line_b_coords);
  FacetSequence rep_fs_a(rep_a.get(), &rep_a_coords, 0, 3);
  FacetSequence fs_b(line_b.get(), &line_b_coords, 0, 2);

  EXPECT_DOUBLE_EQ(rep_fs_a.distance(fs_b), 3.0);
  std::vector<GeometryLocation> locs_a = rep_fs_a.nearestLocations(fs_b);
  ASSERT_EQ(locs_a.size(), 2u);
  EXPECT_EQ(locs_a[0].getSegmentIndex(), 1u);
  EXPECT_EQ(locs_a[1].getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(locs_a[0].getCoordinate().y, 0.0);
  EXPECT_DOUBLE_EQ(locs_a[1].getCoordinate().y, 3.0);

  // Repeated point in second sequence (seg 0 is zero-length).
  CoordinateSequence line_c_coords{Coordinate(0.0, 0.0), Coordinate(4.0, 0.0)};
  CoordinateSequence rep_d_coords{Coordinate(1.0, 5.0), Coordinate(1.0, 5.0),
                                  Coordinate(2.0, 2.0), Coordinate(4.0, 2.0)};
  FacetSequence fs_c(&line_c_coords, 0, 2);
  FacetSequence rep_fs_d(&rep_d_coords, 0, 4);

  EXPECT_DOUBLE_EQ(fs_c.distance(rep_fs_d), 2.0);
  std::vector<GeometryLocation> locs_d = fs_c.nearestLocations(rep_fs_d);
  ASSERT_EQ(locs_d.size(), 2u);
  EXPECT_EQ(locs_d[0].getSegmentIndex(), 0u);
  EXPECT_EQ(locs_d[1].getSegmentIndex(), 2u);
  EXPECT_DOUBLE_EQ(locs_d[0].getCoordinate().y, 0.0);
  EXPECT_DOUBLE_EQ(locs_d[1].getCoordinate().y, 2.0);

  // Degenerate sequence consisting only of zero-length segments returns Inf and
  // leaves nearestLocations empty.
  CoordinateSequence degenerate_coords{Coordinate(1.0, 1.0),
                                       Coordinate(1.0, 1.0)};
  FacetSequence degen_fs(&degenerate_coords, 0, 2);
  EXPECT_TRUE(std::isinf(degen_fs.distance(fs_c)));
  EXPECT_TRUE(std::isinf(fs_c.distance(degen_fs)));
  EXPECT_TRUE(degen_fs.nearestLocations(fs_c).empty());
  EXPECT_TRUE(fs_c.nearestLocations(degen_fs).empty());
}

TEST(FacetSequenceTest, LineToLineEnvelopePruningAndDistanceFiltering) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Outer envelope pruning: seg 0 achieves minDistance = 1.0 at interior point
  // (0, 0); seg 1 and seg 2 stay in x >= 10, y <= 0 so
  // pEnv.distanceSquared(*facetSeq.getEnvelope()) >= 101 > 1.0^2 prunes both.
  CoordinateSequence outer_a_coords{
      Coordinate(-1.0, 0.0), Coordinate(10.0, 0.0), Coordinate(20.0, -10.0),
      Coordinate(30.0, -10.0)};
  CoordinateSequence outer_b_coords{Coordinate(0.0, 1.0), Coordinate(0.0, 3.0)};
  FacetSequence outer_fs_a(&outer_a_coords, 0, 4);
  FacetSequence outer_fs_b(&outer_b_coords, 0, 2);
  EXPECT_DOUBLE_EQ(outer_fs_a.distance(outer_fs_b), 1.0);
  std::vector<GeometryLocation> outer_locs =
      outer_fs_a.nearestLocations(outer_fs_b);
  ASSERT_EQ(outer_locs.size(), 2u);
  EXPECT_EQ(outer_locs[0].getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(outer_locs[0].getCoordinate().x, 0.0);
  EXPECT_DOUBLE_EQ(outer_locs[0].getCoordinate().y, 0.0);
  EXPECT_EQ(outer_locs[1].getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(outer_locs[1].getCoordinate().x, 0.0);
  EXPECT_DOUBLE_EQ(outer_locs[1].getCoordinate().y, 1.0);

  // Inner envelope pruning: single segment in A, multiple in B where seg 2 has
  // pEnv.distanceSquared(qEnv) = 16 > 1.0^2.
  CoordinateSequence inner_a_coords{Coordinate(0.0, 0.0),
                                    Coordinate(10.0, 0.0)};
  CoordinateSequence inner_b_coords{Coordinate(1.0, 1.0), Coordinate(2.0, 1.0),
                                    Coordinate(2.0, 4.0), Coordinate(9.0, 4.0)};
  FacetSequence inner_fs_a(&inner_a_coords, 0, 2);
  FacetSequence inner_fs_b(&inner_b_coords, 0, 4);
  EXPECT_DOUBLE_EQ(inner_fs_a.distance(inner_fs_b), 1.0);
  std::vector<GeometryLocation> inner_locs =
      inner_fs_a.nearestLocations(inner_fs_b);
  ASSERT_EQ(inner_locs.size(), 2u);
  EXPECT_DOUBLE_EQ(inner_locs[0].getCoordinate().y, 0.0);
  EXPECT_DOUBLE_EQ(inner_locs[1].getCoordinate().y, 1.0);

  // Inner segment whose envelope passes distanceSquared <= minDistance^2
  // (3.25 <= 4.0) while actual segment distance sqrt(4.5) > 2.0.
  CoordinateSequence filter_a_coords{Coordinate(0.0, 0.0),
                                     Coordinate(0.0, 2.0)};
  CoordinateSequence filter_b_coords{Coordinate(2.0, 0.0), Coordinate(3.0, 3.0),
                                     Coordinate(1.5, 3.5)};
  std::unique_ptr<LineString> filter_a =
      factory->createLineString(filter_a_coords);
  std::unique_ptr<LineString> filter_b =
      factory->createLineString(filter_b_coords);
  FacetSequence filter_fs_a(filter_a.get(), &filter_a_coords, 0, 2);
  FacetSequence filter_fs_b(filter_b.get(), &filter_b_coords, 0, 3);

  EXPECT_DOUBLE_EQ(filter_fs_a.distance(filter_fs_b), 2.0);
  std::vector<GeometryLocation> filter_locs =
      filter_fs_a.nearestLocations(filter_fs_b);
  ASSERT_EQ(filter_locs.size(), 2u);
  EXPECT_EQ(filter_locs[0].getSegmentIndex(), 0u);
  EXPECT_EQ(filter_locs[1].getSegmentIndex(), 0u);
  EXPECT_DOUBLE_EQ(filter_locs[0].getCoordinate().x, 0.0);
  EXPECT_DOUBLE_EQ(filter_locs[0].getCoordinate().y, 0.0);
  EXPECT_DOUBLE_EQ(filter_locs[1].getCoordinate().x, 2.0);
  EXPECT_DOUBLE_EQ(filter_locs[1].getCoordinate().y, 0.0);

  // Improvement followed by equal-distance segment: `dist <= minDistance`
  // updates locations on equal distance.
  CoordinateSequence tie_b_coords{Coordinate(5.0, 0.0), Coordinate(5.0, 2.0),
                                  Coordinate(2.0, 2.0), Coordinate(2.0, 0.0)};
  FacetSequence tie_fs_b(filter_b.get(), &tie_b_coords, 0, 4);
  EXPECT_DOUBLE_EQ(filter_fs_a.distance(tie_fs_b), 2.0);
  std::vector<GeometryLocation> tie_locs =
      filter_fs_a.nearestLocations(tie_fs_b);
  ASSERT_EQ(tie_locs.size(), 2u);
  EXPECT_EQ(tie_locs[1].getSegmentIndex(), 2u);
  EXPECT_DOUBLE_EQ(tie_locs[1].getCoordinate().x, 2.0);
}

TEST(FacetSequenceTest,
     ThreeDimensionalCoordinatesCompute2DDistanceAndPreserveZ) {
  CoordinateSequence z_a_coords{Coordinate(1.0, 2.0, 100.0)};
  CoordinateSequence z_b_coords{Coordinate(4.0, 6.0, -50.0)};
  FacetSequence z_fs_a(&z_a_coords, 0, 1);
  FacetSequence z_fs_b(&z_b_coords, 0, 1);
  EXPECT_DOUBLE_EQ(z_fs_a.distance(z_fs_b), 5.0);
  EXPECT_DOUBLE_EQ(z_fs_a.getCoordinate(0)->z, 100.0);
  EXPECT_DOUBLE_EQ(z_fs_b.getCoordinate(0)->z, -50.0);

  std::vector<GeometryLocation> z_locs = z_fs_a.nearestLocations(z_fs_b);
  ASSERT_EQ(z_locs.size(), 2u);
  EXPECT_DOUBLE_EQ(z_locs[0].getCoordinate().x, 1.0);
  EXPECT_DOUBLE_EQ(z_locs[0].getCoordinate().y, 2.0);
  EXPECT_DOUBLE_EQ(z_locs[1].getCoordinate().x, 4.0);
  EXPECT_DOUBLE_EQ(z_locs[1].getCoordinate().y, 6.0);
}

}  // namespace
}  // namespace geos_google
