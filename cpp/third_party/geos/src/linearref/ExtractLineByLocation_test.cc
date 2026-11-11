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

// Unit tests for third_party/geos/src/linearref/ExtractLineByLocation.cpp and
// third_party/geos/include/geos/linearref/ExtractLineByLocation.h.

#include "third_party/geos/include/geos/linearref/ExtractLineByLocation.h"

#include <cmath>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/linearref/LinearLocation.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GEOS_LINESTRING;
using geos::geom::GEOS_MULTILINESTRING;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::io::WKTReader;
using geos::linearref::ExtractLineByLocation;
using geos::linearref::LinearLocation;
using geos::util::IllegalArgumentException;

// ExtractLineByLocation::computeLine is private and unused by extract() (which
// delegates to computeLinear()). Explicit template instantiation grants
// standard C++ access to test computeLine() directly.
template <typename Tag, typename Tag::type M>
struct AccessPrivate {
  friend typename Tag::type GetPrivate(Tag) { return M; }
};

struct ComputeLineTag {
  using type = std::unique_ptr<LineString> (ExtractLineByLocation::*)(
      const LinearLocation&, const LinearLocation&);
  friend type GetPrivate(ComputeLineTag);
};

template struct AccessPrivate<ComputeLineTag,
                              &ExtractLineByLocation::computeLine>;

class ExtractLineByLocationTest : public ::testing::Test {
 protected:
  void ExpectExtractWkt(const Geometry& line, const LinearLocation& start,
                        const LinearLocation& end,
                        const std::string& expected_wkt,
                        double tolerance = 1e-9) const {
    std::unique_ptr<Geometry> expected = reader_.read(expected_wkt);
    ASSERT_NE(expected, nullptr);

    std::unique_ptr<Geometry> actual_static =
        ExtractLineByLocation::extract(&line, start, end);
    ASSERT_NE(actual_static, nullptr);
    EXPECT_TRUE(actual_static->equalsExact(expected.get(), tolerance))
        << "actual=" << actual_static->toText()
        << " expected=" << expected->toText();

    ExtractLineByLocation extractor(&line);
    std::unique_ptr<Geometry> actual_instance = extractor.extract(start, end);
    ASSERT_NE(actual_instance, nullptr);
    EXPECT_TRUE(actual_instance->equalsExact(expected.get(), tolerance))
        << "actual=" << actual_instance->toText()
        << " expected=" << expected->toText();
  }

  void ExpectComputeLineWkt(const Geometry& line, const LinearLocation& start,
                            const LinearLocation& end,
                            const std::string& expected_wkt,
                            double tolerance = 1e-9) const {
    std::unique_ptr<Geometry> expected = reader_.read(expected_wkt);
    ASSERT_NE(expected, nullptr);

    ExtractLineByLocation extractor(&line);
    std::unique_ptr<LineString> actual =
        (extractor.*GetPrivate(ComputeLineTag{}))(start, end);
    ASSERT_NE(actual, nullptr);
    EXPECT_TRUE(actual->equalsExact(expected.get(), tolerance))
        << "actual=" << actual->toText() << " expected=" << expected->toText();
  }

  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};
};

TEST_F(ExtractLineByLocationTest, DirectConstructorAndRepeatedExtracts) {
  std::unique_ptr<Geometry> line =
      reader_.read("LINESTRING (0 0, 10 0, 10 10, 20 10)");
  ExtractLineByLocation extractor(line.get());

  std::unique_ptr<Geometry> forward =
      extractor.extract(LinearLocation(0, 0, 0.5), LinearLocation(0, 2, 0.5));
  ASSERT_NE(forward, nullptr);
  EXPECT_EQ(forward->getGeometryTypeId(), GEOS_LINESTRING);
  std::unique_ptr<Geometry> expected_forward =
      reader_.read("LINESTRING (5 0, 10 0, 10 10, 15 10)");
  EXPECT_TRUE(forward->equalsExact(expected_forward.get(), 1e-9));

  std::unique_ptr<Geometry> backward =
      extractor.extract(LinearLocation(0, 2, 0.5), LinearLocation(0, 0, 0.5));
  ASSERT_NE(backward, nullptr);
  EXPECT_EQ(backward->getGeometryTypeId(), GEOS_LINESTRING);
  std::unique_ptr<Geometry> expected_backward =
      reader_.read("LINESTRING (15 10, 10 10, 10 0, 5 0)");
  EXPECT_TRUE(backward->equalsExact(expected_backward.get(), 1e-9));
}

TEST_F(ExtractLineByLocationTest, LineStringForwardSublines) {
  std::unique_ptr<Geometry> line =
      reader_.read("LINESTRING (0 0, 10 0, 10 10, 20 10)");

  // Full line from start location to getEndLocation.
  ExpectExtractWkt(*line, LinearLocation(0, 0, 0.0),
                   LinearLocation::getEndLocation(line.get()),
                   "LINESTRING (0 0, 10 0, 10 10, 20 10)");

  // Within a single segment, both endpoints non-vertex.
  ExpectExtractWkt(*line, LinearLocation(0, 0, 0.25),
                   LinearLocation(0, 0, 0.75), "LINESTRING (2.5 0, 7.5 0)");

  // Non-vertex start to exact vertex end.
  ExpectExtractWkt(*line, LinearLocation(0, 0, 0.5), LinearLocation(0, 2, 0.0),
                   "LINESTRING (5 0, 10 0, 10 10)");

  // Exact vertex start to non-vertex end.
  ExpectExtractWkt(*line, LinearLocation(0, 1, 0.0), LinearLocation(0, 2, 0.4),
                   "LINESTRING (10 0, 10 10, 14 10)");

  // 3-arg constructor normalizes fraction 1.0 to next segment index.
  ExpectExtractWkt(*line, LinearLocation(0, 0, 0.0), LinearLocation(0, 1, 1.0),
                   "LINESTRING (0 0, 10 0, 10 10)");

  // 2-arg constructor preserves unnormalized fraction 1.0 (isVertex() == true),
  // stopping before vertex index 2.
  ExpectExtractWkt(*line, LinearLocation(0, 0.0), LinearLocation(1, 1.0),
                   "LINESTRING (0 0, 10 0)");
}

TEST_F(ExtractLineByLocationTest, EqualStartAndEndProducesTwoPointLine) {
  std::unique_ptr<Geometry> line =
      reader_.read("LINESTRING (0 0, 10 0, 10 10)");

  // Equal at start vertex.
  ExpectExtractWkt(*line, LinearLocation(0, 0, 0.0), LinearLocation(0, 0, 0.0),
                   "LINESTRING (0 0, 0 0)");

  // Equal at interior vertex.
  ExpectExtractWkt(*line, LinearLocation(0, 1, 0.0), LinearLocation(0, 1, 0.0),
                   "LINESTRING (10 0, 10 0)");

  // Equal at end vertex (normalized vertex location).
  ExpectExtractWkt(*line, LinearLocation(0, 2, 0.0), LinearLocation(0, 2, 0.0),
                   "LINESTRING (10 10, 10 10)");

  // Equal at LinearLocation::getEndLocation (segmentFraction == 1.0 advances
  // LinearIterator past the last vertex, producing an empty geometry).
  LinearLocation end = LinearLocation::getEndLocation(line.get());
  std::unique_ptr<Geometry> at_end =
      ExtractLineByLocation::extract(line.get(), end, end);
  ASSERT_NE(at_end, nullptr);
  EXPECT_TRUE(at_end->isEmpty());

  // Equal at mid-segment non-vertex point.
  ExpectExtractWkt(*line, LinearLocation(0, 0, 0.5), LinearLocation(0, 0, 0.5),
                   "LINESTRING (5 0, 5 0)");
}

TEST_F(ExtractLineByLocationTest, LineStringReversedSublines) {
  std::unique_ptr<Geometry> line =
      reader_.read("LINESTRING (0 0, 10 0, 10 10, 20 10)");

  // Reversed within a single segment.
  ExpectExtractWkt(*line, LinearLocation(0, 0, 0.8), LinearLocation(0, 0, 0.2),
                   "LINESTRING (8 0, 2 0)");

  // Reversed across full line.
  ExpectExtractWkt(*line, LinearLocation::getEndLocation(line.get()),
                   LinearLocation(0, 0, 0.0),
                   "LINESTRING (20 10, 10 10, 10 0, 0 0)");

  // Reversed across vertex boundary.
  ExpectExtractWkt(*line, LinearLocation(0, 2, 0.0), LinearLocation(0, 0, 0.5),
                   "LINESTRING (10 10, 10 0, 5 0)");
}

TEST_F(ExtractLineByLocationTest, MultiLineStringForwardAndReversed) {
  std::unique_ptr<Geometry> mls = reader_.read(
      "MULTILINESTRING ((0 0, 10 0), (20 0, 30 0, 30 10), (40 0, 50 0))");

  // Full multi-linestring extraction.
  ExpectExtractWkt(
      *mls, LinearLocation(0, 0, 0.0),
      LinearLocation::getEndLocation(mls.get()),
      "MULTILINESTRING ((0 0, 10 0), (20 0, 30 0, 30 10), (40 0, 50 0))");

  // Forward spanning all three components.
  ExpectExtractWkt(
      *mls, LinearLocation(0, 0, 0.5), LinearLocation(2, 0, 0.5),
      "MULTILINESTRING ((5 0, 10 0), (20 0, 30 0, 30 10), (40 0, 45 0))");

  // Reversed spanning multiple components exercises MultiLineString::reverse(),
  // which reverses each component LineString in place without reordering the
  // components.
  ExpectExtractWkt(
      *mls, LinearLocation(2, 0, 0.5), LinearLocation(0, 0, 0.5),
      "MULTILINESTRING ((10 0, 5 0), (30 10, 30 0, 20 0), (45 0, 40 0))");

  // Reversed full multi-linestring extraction.
  ExpectExtractWkt(
      *mls, LinearLocation::getEndLocation(mls.get()),
      LinearLocation(0, 0, 0.0),
      "MULTILINESTRING ((10 0, 0 0), (30 10, 30 0, 20 0), (50 0, 40 0))");

  // Subline within a single component yields a LineString (forward & reversed).
  std::unique_ptr<Geometry> single_comp = ExtractLineByLocation::extract(
      mls.get(), LinearLocation(1, 0, 0.5), LinearLocation(1, 1, 0.5));
  EXPECT_EQ(single_comp->getGeometryTypeId(), GEOS_LINESTRING);
  ExpectExtractWkt(*mls, LinearLocation(1, 0, 0.5), LinearLocation(1, 1, 0.5),
                   "LINESTRING (25 0, 30 0, 30 5)");

  std::unique_ptr<Geometry> single_comp_rev = ExtractLineByLocation::extract(
      mls.get(), LinearLocation(1, 1, 0.5), LinearLocation(1, 0, 0.5));
  EXPECT_EQ(single_comp_rev->getGeometryTypeId(), GEOS_LINESTRING);
  ExpectExtractWkt(*mls, LinearLocation(1, 1, 0.5), LinearLocation(1, 0, 0.5),
                   "LINESTRING (30 5, 30 0, 25 0)");
}

TEST_F(ExtractLineByLocationTest, MultiLineStringComponentBoundaries) {
  std::unique_ptr<Geometry> mls =
      reader_.read("MULTILINESTRING ((0 0, 10 0), (20 0, 30 0))");

  // From end of component 0 to start of component 1.
  std::unique_ptr<Geometry> boundary = ExtractLineByLocation::extract(
      mls.get(), LinearLocation(0, 1, 0.0), LinearLocation(1, 0, 0.0));
  ASSERT_NE(boundary, nullptr);
  EXPECT_EQ(boundary->getGeometryTypeId(), GEOS_MULTILINESTRING);
  ExpectExtractWkt(*mls, LinearLocation(0, 1, 0.0), LinearLocation(1, 0, 0.0),
                   "MULTILINESTRING ((10 0, 10 0), (20 0, 20 0))");

  // Reversed across component boundary reverses each fixed 2-point component
  // in place without reordering components.
  ExpectExtractWkt(*mls, LinearLocation(1, 0, 0.0), LinearLocation(0, 1, 0.0),
                   "MULTILINESTRING ((10 0, 10 0), (20 0, 20 0))");

  // Mid-segment on component 0 to start vertex of component 1.
  ExpectExtractWkt(*mls, LinearLocation(0, 0, 0.5), LinearLocation(1, 0, 0.0),
                   "MULTILINESTRING ((5 0, 10 0), (20 0, 20 0))");

  // Reversed from start vertex of component 1 to mid-segment on component 0.
  ExpectExtractWkt(*mls, LinearLocation(1, 0, 0.0), LinearLocation(0, 0, 0.5),
                   "MULTILINESTRING ((10 0, 5 0), (20 0, 20 0))");
}

TEST_F(ExtractLineByLocationTest, PreservesAndInterpolatesZOrdinates) {
  std::unique_ptr<Geometry> line =
      reader_.read("LINESTRING Z (0 0 10, 10 0 20, 20 0 40)");

  std::unique_ptr<Geometry> sub = ExtractLineByLocation::extract(
      line.get(), LinearLocation(0, 0, 0.5), LinearLocation(0, 1, 0.25));
  ASSERT_NE(sub, nullptr);
  std::unique_ptr<CoordinateSequence> coords = sub->getCoordinates();
  ASSERT_EQ(coords->size(), 3u);

  Coordinate c;
  coords->getAt(0, c);
  EXPECT_DOUBLE_EQ(c.x, 5.0);
  EXPECT_DOUBLE_EQ(c.y, 0.0);
  EXPECT_DOUBLE_EQ(c.z, 15.0);

  coords->getAt(1, c);
  EXPECT_DOUBLE_EQ(c.x, 10.0);
  EXPECT_DOUBLE_EQ(c.y, 0.0);
  EXPECT_DOUBLE_EQ(c.z, 20.0);

  coords->getAt(2, c);
  EXPECT_DOUBLE_EQ(c.x, 12.5);
  EXPECT_DOUBLE_EQ(c.y, 0.0);
  EXPECT_DOUBLE_EQ(c.z, 25.0);

  std::unique_ptr<Geometry> rev = ExtractLineByLocation::extract(
      line.get(), LinearLocation(0, 1, 0.25), LinearLocation(0, 0, 0.5));
  ASSERT_NE(rev, nullptr);
  std::unique_ptr<CoordinateSequence> rev_coords = rev->getCoordinates();
  ASSERT_EQ(rev_coords->size(), 3u);
  rev_coords->getAt(0, c);
  EXPECT_DOUBLE_EQ(c.x, 12.5);
  EXPECT_DOUBLE_EQ(c.z, 25.0);
  rev_coords->getAt(2, c);
  EXPECT_DOUBLE_EQ(c.x, 5.0);
  EXPECT_DOUBLE_EQ(c.z, 15.0);

  // Extracting from a 2D line leaves Z as NaN.
  std::unique_ptr<Geometry> line2d = reader_.read("LINESTRING (0 0, 10 0)");
  std::unique_ptr<Geometry> sub2d = ExtractLineByLocation::extract(
      line2d.get(), LinearLocation(0, 0, 0.25), LinearLocation(0, 0, 0.75));
  ASSERT_NE(sub2d, nullptr);
  std::unique_ptr<CoordinateSequence> coords2d = sub2d->getCoordinates();
  ASSERT_EQ(coords2d->size(), 2u);
  coords2d->getAt(0, c);
  EXPECT_DOUBLE_EQ(c.x, 2.5);
  EXPECT_DOUBLE_EQ(c.y, 0.0);
  EXPECT_TRUE(std::isnan(c.z));
}

TEST_F(ExtractLineByLocationTest, ClosedRingAndRepeatedVertexLine) {
  std::unique_ptr<Geometry> ring =
      reader_.read("LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  ExpectExtractWkt(*ring, LinearLocation(0, 1, 0.5), LinearLocation(0, 3, 0.5),
                   "LINESTRING (10 5, 10 10, 0 10, 0 5)");

  CoordinateSequence ring_coords{CoordinateXY(0, 0), CoordinateXY(10, 0),
                                 CoordinateXY(10, 10), CoordinateXY(0, 10),
                                 CoordinateXY(0, 0)};
  std::unique_ptr<LinearRing> linear_ring =
      factory_->createLinearRing(ring_coords);
  ExpectExtractWkt(*linear_ring, LinearLocation(0, 1, 0.5),
                   LinearLocation(0, 3, 0.5),
                   "LINESTRING (10 5, 10 10, 0 10, 0 5)");
  ExpectExtractWkt(*linear_ring, LinearLocation(0, 3, 0.5),
                   LinearLocation(0, 1, 0.5),
                   "LINESTRING (0 5, 0 10, 10 10, 10 5)");

  std::unique_ptr<Geometry> zero_seg =
      reader_.read("LINESTRING (0 0, 0 0, 10 0)");
  ExpectExtractWkt(*zero_seg, LinearLocation(0, 0, 0.0),
                   LinearLocation(0, 1, 0.5), "LINESTRING (0 0, 0 0, 5 0)");
}

TEST_F(ExtractLineByLocationTest, EmptyAndNonLinearGeometries) {
  std::unique_ptr<Geometry> empty_line = reader_.read("LINESTRING EMPTY");
  std::unique_ptr<Geometry> res_empty_line = ExtractLineByLocation::extract(
      empty_line.get(), LinearLocation(0, 0, 0.0), LinearLocation(0, 0, 0.0));
  ASSERT_NE(res_empty_line, nullptr);
  EXPECT_TRUE(res_empty_line->isEmpty());

  std::unique_ptr<Geometry> empty_mls = reader_.read("MULTILINESTRING EMPTY");
  std::unique_ptr<Geometry> res_empty_mls = ExtractLineByLocation::extract(
      empty_mls.get(), LinearLocation(0, 0, 0.0), LinearLocation(0, 0, 0.0));
  ASSERT_NE(res_empty_mls, nullptr);
  EXPECT_TRUE(res_empty_mls->isEmpty());

  // A GeometryCollection containing only LineStrings is iterable
  // component-wise.
  std::unique_ptr<Geometry> linear_gc = reader_.read(
      "GEOMETRYCOLLECTION (LINESTRING (0 0, 10 0), LINESTRING (20 0, 30 0))");
  ExpectExtractWkt(*linear_gc, LinearLocation(0, 0, 0.5),
                   LinearLocation(1, 0, 0.5),
                   "MULTILINESTRING ((5 0, 10 0), (20 0, 25 0))");
  ExpectExtractWkt(*linear_gc, LinearLocation(1, 0, 0.5),
                   LinearLocation(0, 0, 0.5),
                   "MULTILINESTRING ((10 0, 5 0), (25 0, 20 0))");

  std::unique_ptr<Geometry> mixed_gc =
      reader_.read("GEOMETRYCOLLECTION (LINESTRING (0 0, 10 0), POINT (5 5))");
  EXPECT_THROW(
      ExtractLineByLocation::extract(mixed_gc.get(), LinearLocation(0, 0, 0.0),
                                     LinearLocation(1, 0, 0.0)),
      IllegalArgumentException);

  std::unique_ptr<Geometry> pt = reader_.read("POINT (1 2)");
  EXPECT_THROW(
      ExtractLineByLocation::extract(pt.get(), LinearLocation(0, 0, 0.0),
                                     LinearLocation(0, 0, 0.0)),
      IllegalArgumentException);

  std::unique_ptr<Geometry> poly =
      reader_.read("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  EXPECT_THROW(
      ExtractLineByLocation::extract(poly.get(), LinearLocation(0, 0, 0.0),
                                     LinearLocation(0, 1, 0.0)),
      IllegalArgumentException);
}

TEST_F(ExtractLineByLocationTest, ComputeLineForwardSublines) {
  std::unique_ptr<Geometry> line =
      reader_.read("LINESTRING (0 0, 10 0, 10 10, 20 10)");

  // Full line with LinearLocation::getEndLocation clamps lastSegmentIndex to
  // coordinates->size() - 1.
  ExpectComputeLineWkt(*line, LinearLocation(0, 0, 0.0),
                       LinearLocation::getEndLocation(line.get()),
                       "LINESTRING (0 0, 10 0, 10 10, 20 10)");

  // Within a single segment, both endpoints non-vertex (loop runs 0 times).
  ExpectComputeLineWkt(*line, LinearLocation(0, 0, 0.25),
                       LinearLocation(0, 0, 0.75), "LINESTRING (2.5 0, 7.5 0)");

  // Non-vertex start to exact vertex end.
  ExpectComputeLineWkt(*line, LinearLocation(0, 0, 0.5),
                       LinearLocation(0, 2, 0.0),
                       "LINESTRING (5 0, 10 0, 10 10)");

  // Exact vertex start to non-vertex end.
  ExpectComputeLineWkt(*line, LinearLocation(0, 1, 0.0),
                       LinearLocation(0, 2, 0.4),
                       "LINESTRING (10 0, 10 10, 14 10)");

  // Unnormalized 2-arg end with segmentFraction == 1.0 advances
  // lastSegmentIndex by 1 without exceeding coordinates->size().
  ExpectComputeLineWkt(*line, LinearLocation(0, 0.0), LinearLocation(1, 1.0),
                       "LINESTRING (0 0, 10 0, 10 10)");

  // Unnormalized 2-arg start with segmentFraction == 1.0 advances
  // startSegmentIndex by 1 while still satisfying isVertex().
  ExpectComputeLineWkt(*line, LinearLocation(0, 1.0), LinearLocation(2, 0.0),
                       "LINESTRING (10 0, 10 10)");

  // End past the final vertex index is clamped to the last coordinate.
  ExpectComputeLineWkt(*line, LinearLocation(0, 1, 0.0),
                       LinearLocation(0, 10, 0.0),
                       "LINESTRING (10 0, 10 10, 20 10)");
}

TEST_F(ExtractLineByLocationTest, ComputeLineDegenerateAndEndpointCases) {
  std::unique_ptr<Geometry> line =
      reader_.read("LINESTRING (0 0, 10 0, 10 10)");

  // Equal at start, interior, and end vertices yield 1 vertex duplicated to 2.
  ExpectComputeLineWkt(*line, LinearLocation(0, 0, 0.0),
                       LinearLocation(0, 0, 0.0), "LINESTRING (0 0, 0 0)");
  ExpectComputeLineWkt(*line, LinearLocation(0, 1, 0.0),
                       LinearLocation(0, 1, 0.0), "LINESTRING (10 0, 10 0)");
  ExpectComputeLineWkt(*line, LinearLocation(0, 2, 0.0),
                       LinearLocation(0, 2, 0.0), "LINESTRING (10 10, 10 10)");

  // Equal at getEndLocation leaves newCoordinateArray empty after the vertex
  // loop, hitting the empty-array fallback and then duplicating to 2 points.
  LinearLocation end = LinearLocation::getEndLocation(line.get());
  ExpectComputeLineWkt(*line, end, end, "LINESTRING (10 10, 10 10)");

  // Equal at unnormalized 2-arg end fraction 1.0.
  ExpectComputeLineWkt(*line, LinearLocation(1, 1.0), LinearLocation(1, 1.0),
                       "LINESTRING (10 10, 10 10)");

  // Equal at mid-segment non-vertex point adds start and end directly.
  ExpectComputeLineWkt(*line, LinearLocation(0, 0, 0.5),
                       LinearLocation(0, 0, 0.5), "LINESTRING (5 0, 5 0)");
}

TEST_F(ExtractLineByLocationTest, ComputeLinePreservesZOrdinates) {
  std::unique_ptr<Geometry> line =
      reader_.read("LINESTRING Z (0 0 10, 10 0 20, 20 0 40)");
  ExtractLineByLocation extractor(line.get());
  std::unique_ptr<LineString> sub = (extractor.*GetPrivate(ComputeLineTag{}))(
      LinearLocation(0, 0, 0.5), LinearLocation(0, 1, 0.25));
  ASSERT_NE(sub, nullptr);
  std::unique_ptr<CoordinateSequence> coords = sub->getCoordinates();
  ASSERT_EQ(coords->size(), 3u);

  Coordinate c;
  coords->getAt(0, c);
  EXPECT_DOUBLE_EQ(c.x, 5.0);
  EXPECT_DOUBLE_EQ(c.y, 0.0);
  EXPECT_DOUBLE_EQ(c.z, 15.0);

  coords->getAt(1, c);
  EXPECT_DOUBLE_EQ(c.x, 10.0);
  EXPECT_DOUBLE_EQ(c.y, 0.0);
  EXPECT_DOUBLE_EQ(c.z, 20.0);

  coords->getAt(2, c);
  EXPECT_DOUBLE_EQ(c.x, 12.5);
  EXPECT_DOUBLE_EQ(c.y, 0.0);
  EXPECT_DOUBLE_EQ(c.z, 25.0);

  std::unique_ptr<Geometry> line2d = reader_.read("LINESTRING (0 0, 10 0)");
  ExtractLineByLocation extractor2d(line2d.get());
  std::unique_ptr<LineString> sub2d =
      (extractor2d.*GetPrivate(ComputeLineTag{}))(LinearLocation(0, 0, 0.25),
                                                  LinearLocation(0, 0, 0.75));
  ASSERT_NE(sub2d, nullptr);
  std::unique_ptr<CoordinateSequence> coords2d = sub2d->getCoordinates();
  ASSERT_EQ(coords2d->size(), 2u);
  coords2d->getAt(0, c);
  EXPECT_DOUBLE_EQ(c.x, 2.5);
  EXPECT_DOUBLE_EQ(c.y, 0.0);
  EXPECT_TRUE(std::isnan(c.z));
}

}  // namespace
}  // namespace geos_google
