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

#include "third_party/geos/include/geos/simplify/TaggedLineString.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/simplify/TaggedLineSegment.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::simplify::TaggedLineSegment;
using geos::simplify::TaggedLineString;

void ExpectCoords2DEqual(const CoordinateSequence& seq,
                         const std::vector<Coordinate>& expected) {
  ASSERT_EQ(seq.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_DOUBLE_EQ(seq.getX(i), expected[i].x) << "at index " << i;
    EXPECT_DOUBLE_EQ(seq.getY(i), expected[i].y) << "at index " << i;
  }
}

class TaggedLineStringTest : public ::testing::Test {
 protected:
  TaggedLineStringTest() : factory_(GeometryFactory::create()) {}

  std::unique_ptr<LineString> MakeLine(
      std::initializer_list<Coordinate> coords) {
    auto seq = std::make_unique<CoordinateSequence>(coords);
    return factory_->createLineString(std::move(seq));
  }

  std::unique_ptr<LinearRing> MakeRing(
      std::initializer_list<Coordinate> coords) {
    auto seq = std::make_unique<CoordinateSequence>(coords);
    return factory_->createLinearRing(std::move(seq));
  }

  GeometryFactory::Ptr factory_;
};

TEST_F(TaggedLineStringTest, ConstructsFromEmptyLineString) {
  std::unique_ptr<LineString> empty_line = factory_->createLineString();
  TaggedLineString tls(empty_line.get(), 2, false);
  const TaggedLineString& const_tls = tls;

  EXPECT_EQ(tls.getMinimumSize(), 2u);
  EXPECT_FALSE(tls.isRing());
  EXPECT_EQ(tls.getParent(), empty_line.get());
  ASSERT_NE(tls.getParentCoordinates(), nullptr);
  EXPECT_TRUE(tls.getParentCoordinates()->isEmpty());
  EXPECT_EQ(tls.size(), 0u);

  EXPECT_TRUE(tls.getSegments().empty());
  EXPECT_TRUE(const_tls.getSegments().empty());
  EXPECT_TRUE(tls.getResultSegments().empty());
  EXPECT_EQ(tls.getResultSize(), 0u);

  CoordinateSequence::Ptr result_coords = tls.getResultCoordinates();
  ASSERT_NE(result_coords, nullptr);
  EXPECT_TRUE(result_coords->isEmpty());

  std::unique_ptr<Geometry> as_line = tls.asLineString();
  ASSERT_NE(as_line, nullptr);
  EXPECT_EQ(as_line->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(as_line->isEmpty());

  std::unique_ptr<Geometry> as_ring = tls.asLinearRing();
  ASSERT_NE(as_ring, nullptr);
  EXPECT_EQ(as_ring->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  EXPECT_TRUE(as_ring->isEmpty());
}

TEST_F(TaggedLineStringTest, ConstructsSegmentsAndAccessorsFromOpenLine) {
  std::unique_ptr<LineString> line =
      MakeLine({{0.0, 0.0}, {1.0, 2.0}, {3.0, 1.0}, {4.0, 4.0}});
  TaggedLineString tls(line.get(), 2, false);
  const TaggedLineString& const_tls = tls;

  EXPECT_EQ(tls.getMinimumSize(), 2u);
  EXPECT_FALSE(tls.isRing());
  EXPECT_EQ(tls.getParent(), line.get());
  EXPECT_EQ(tls.getParentCoordinates(), line->getCoordinatesRO());
  EXPECT_EQ(tls.size(), 4u);

  EXPECT_TRUE(tls.getCoordinate(0).equals2D(Coordinate(0.0, 0.0)));
  EXPECT_TRUE(tls.getCoordinate(1).equals2D(Coordinate(1.0, 2.0)));
  EXPECT_TRUE(tls.getCoordinate(2).equals2D(Coordinate(3.0, 1.0)));
  EXPECT_TRUE(tls.getCoordinate(3).equals2D(Coordinate(4.0, 4.0)));

  // Before result segments are added, getComponentPoint returns coordinate 1.
  EXPECT_TRUE(tls.getComponentPoint().equals2D(Coordinate(1.0, 2.0)));
  EXPECT_EQ(tls.getResultSize(), 0u);

  ASSERT_EQ(tls.getSegments().size(), 3u);
  ASSERT_EQ(const_tls.getSegments().size(), 3u);

  const std::vector<Coordinate> expected_pts = {
      {0.0, 0.0}, {1.0, 2.0}, {3.0, 1.0}, {4.0, 4.0}};
  for (std::size_t i = 0; i < 3; ++i) {
    TaggedLineSegment* seg = tls.getSegment(i);
    const TaggedLineSegment* const_seg = const_tls.getSegment(i);
    ASSERT_NE(seg, nullptr);
    EXPECT_EQ(seg, const_seg);
    EXPECT_TRUE(seg->p0.equals2D(expected_pts[i]));
    EXPECT_TRUE(seg->p1.equals2D(expected_pts[i + 1]));
    EXPECT_EQ(seg->getParent(), line.get());
    EXPECT_EQ(seg->getIndex(), i);
  }
}

TEST_F(TaggedLineStringTest, PreservesZOrMissingZInSegmentsAndResult) {
  std::unique_ptr<LineString> line_3d =
      MakeLine({{1.0, 2.0, 10.0}, {3.0, 4.0, 20.0}, {5.0, 6.0, 30.0}});
  TaggedLineString tls_3d(line_3d.get(), 2, false);

  ASSERT_EQ(tls_3d.getSegments().size(), 2u);
  EXPECT_DOUBLE_EQ(tls_3d.getSegment(0)->p0.z, 10.0);
  EXPECT_DOUBLE_EQ(tls_3d.getSegment(0)->p1.z, 20.0);
  EXPECT_DOUBLE_EQ(tls_3d.getSegment(1)->p1.z, 30.0);

  tls_3d.addToResult(std::make_unique<TaggedLineSegment>(
      Coordinate(1.0, 2.0, 10.0), Coordinate(5.0, 6.0, 30.0)));
  CoordinateSequence::Ptr coords_3d = tls_3d.getResultCoordinates();
  ASSERT_EQ(coords_3d->size(), 2u);
  EXPECT_DOUBLE_EQ(coords_3d->getAt(0).z, 10.0);
  EXPECT_DOUBLE_EQ(coords_3d->getAt(1).z, 30.0);

  std::unique_ptr<LineString> line_2d = MakeLine({{0.0, 0.0}, {10.0, 0.0}});
  TaggedLineString tls_2d(line_2d.get(), 2, false);
  EXPECT_TRUE(std::isnan(tls_2d.getSegment(0)->p0.z));
  EXPECT_TRUE(std::isnan(tls_2d.getSegment(0)->p1.z));
  tls_2d.addToResult(
      std::make_unique<TaggedLineSegment>(*tls_2d.getSegment(0)));
  CoordinateSequence::Ptr coords_2d = tls_2d.getResultCoordinates();
  ASSERT_EQ(coords_2d->size(), 2u);
  EXPECT_TRUE(std::isnan(coords_2d->getAt(0).z));
  EXPECT_TRUE(std::isnan(coords_2d->getAt(1).z));
}

TEST_F(TaggedLineStringTest, AddToResultUpdatesComponentPointAndAsLineString) {
  std::unique_ptr<LineString> line =
      MakeLine({{0.0, 0.0}, {1.0, 1.0}, {2.0, -1.0}, {3.0, 0.0}});
  TaggedLineString tls(line.get(), 2, false);

  tls.addToResult(std::make_unique<TaggedLineSegment>(Coordinate(0.0, 0.0),
                                                      Coordinate(2.0, -1.0)));
  tls.addToResult(std::make_unique<TaggedLineSegment>(Coordinate(2.0, -1.0),
                                                      Coordinate(3.0, 0.0)));

  EXPECT_EQ(tls.getResultSegments().size(), 2u);
  EXPECT_EQ(tls.getResultSize(), 3u);

  // With result segments populated, getComponentPoint returns
  // resultSegs[0]->p0.
  EXPECT_TRUE(tls.getComponentPoint().equals2D(Coordinate(0.0, 0.0)));

  CoordinateSequence::Ptr coords = tls.getResultCoordinates();
  ExpectCoords2DEqual(*coords, {{0.0, 0.0}, {2.0, -1.0}, {3.0, 0.0}});

  std::unique_ptr<Geometry> geom = tls.asLineString();
  ASSERT_NE(geom, nullptr);
  EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  const auto* result_line = dynamic_cast<const LineString*>(geom.get());
  ASSERT_NE(result_line, nullptr);
  ExpectCoords2DEqual(*result_line->getCoordinatesRO(),
                      {{0.0, 0.0}, {2.0, -1.0}, {3.0, 0.0}});
}

TEST_F(TaggedLineStringTest, AsLinearRingValidatesAndCreatesRing) {
  std::unique_ptr<LinearRing> ring = MakeRing(
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});
  TaggedLineString tls_invalid(ring.get(), 4, true);
  EXPECT_TRUE(tls_invalid.isRing());
  EXPECT_EQ(tls_invalid.getMinimumSize(), 4u);

  tls_invalid.addToResult(std::make_unique<TaggedLineSegment>(
      Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)));
  EXPECT_THROW((void)tls_invalid.asLinearRing(),
               geos::util::IllegalArgumentException);

  TaggedLineString tls_valid(ring.get(), 4, true);
  tls_valid.addToResult(std::make_unique<TaggedLineSegment>(
      Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)));
  tls_valid.addToResult(std::make_unique<TaggedLineSegment>(
      Coordinate(10.0, 0.0), Coordinate(5.0, 10.0)));
  tls_valid.addToResult(std::make_unique<TaggedLineSegment>(
      Coordinate(5.0, 10.0), Coordinate(0.0, 0.0)));

  std::unique_ptr<Geometry> geom = tls_valid.asLinearRing();
  ASSERT_NE(geom, nullptr);
  EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  const auto* result_ring = dynamic_cast<const LinearRing*>(geom.get());
  ASSERT_NE(result_ring, nullptr);
  EXPECT_TRUE(result_ring->isClosed());
  ExpectCoords2DEqual(*result_ring->getCoordinatesRO(),
                      {{0.0, 0.0}, {10.0, 0.0}, {5.0, 10.0}, {0.0, 0.0}});
}

TEST_F(TaggedLineStringTest, RemoveRingEndpointMergesFirstAndLastSegment) {
  std::unique_ptr<LinearRing> ring = MakeRing({{5.0, 0.0},
                                               {10.0, 0.0},
                                               {10.0, 10.0},
                                               {0.0, 10.0},
                                               {0.0, 0.0},
                                               {5.0, 0.0}});
  TaggedLineString tls(ring.get(), 4, true);
  ASSERT_EQ(tls.getSegments().size(), 5u);
  for (const TaggedLineSegment* seg : tls.getSegments()) {
    tls.addToResult(std::make_unique<TaggedLineSegment>(*seg));
  }
  EXPECT_EQ(tls.getResultSegments().size(), 5u);
  EXPECT_EQ(tls.getResultSize(), 6u);

  const TaggedLineSegment* merged = tls.removeRingEndpoint();
  ASSERT_NE(merged, nullptr);
  EXPECT_EQ(merged, tls.getResultSegments().front());
  EXPECT_TRUE(merged->p0.equals2D(Coordinate(0.0, 0.0)));
  EXPECT_TRUE(merged->p1.equals2D(Coordinate(10.0, 0.0)));

  EXPECT_EQ(tls.getResultSegments().size(), 4u);
  EXPECT_EQ(tls.getResultSize(), 5u);

  CoordinateSequence::Ptr coords = tls.getResultCoordinates();
  ExpectCoords2DEqual(
      *coords,
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});

  std::unique_ptr<Geometry> geom = tls.asLinearRing();
  ASSERT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  const auto* result_ring = dynamic_cast<const LinearRing*>(geom.get());
  ASSERT_NE(result_ring, nullptr);
  EXPECT_TRUE(result_ring->isClosed());
  EXPECT_EQ(result_ring->getNumPoints(), 5u);
}

}  // namespace
}  // namespace geos_google
