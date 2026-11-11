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

#include "third_party/geos/include/geos/simplify/TaggedLineSegment.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineSegment.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/Point.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::GeometryFactory;
using geos::geom::LineSegment;
using geos::geom::LineString;
using geos::geom::Point;
using geos::simplify::TaggedLineSegment;

TEST(TaggedLineSegmentTest, ConstructsWithParentAndIndex) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  CoordinateSequence seq;
  seq.add(Coordinate(1.0, 2.0, 3.0));
  seq.add(Coordinate(4.0, 6.0, 9.0));
  seq.add(Coordinate(10.0, 6.0, 12.0));
  std::unique_ptr<LineString> parent = factory->createLineString(seq);

  const Coordinate p0(1.0, 2.0, 3.0);
  const Coordinate p1(4.0, 6.0, 9.0);
  TaggedLineSegment seg(p0, p1, parent.get(), 5);

  EXPECT_EQ(seg.getParent(), parent.get());
  EXPECT_EQ(seg.getIndex(), 5u);
  EXPECT_TRUE(seg.p0.equals3D(p0));
  EXPECT_TRUE(seg.p1.equals3D(p1));
  EXPECT_DOUBLE_EQ(seg.getLength(), 5.0);

  std::unique_ptr<LineString> geom = seg.toGeometry(*factory);
  ASSERT_NE(geom, nullptr);
  EXPECT_EQ(geom->getNumPoints(), 2u);
  EXPECT_TRUE(geom->getCoordinateN(0).equals2D(p0));
  EXPECT_TRUE(geom->getCoordinateN(1).equals2D(p1));
}

TEST(TaggedLineSegmentTest, ConstructsWithoutParentDefaultsToNullAndZero) {
  const Coordinate p0(0.0, 0.0);
  const Coordinate p1(3.0, 4.0);
  TaggedLineSegment seg(p0, p1);

  EXPECT_EQ(seg.getParent(), nullptr);
  EXPECT_EQ(seg.getIndex(), 0u);
  EXPECT_EQ(seg.p0, p0);
  EXPECT_EQ(seg.p1, p1);
  EXPECT_TRUE(std::isnan(seg.p0.z));
  EXPECT_TRUE(std::isnan(seg.p1.z));
  EXPECT_DOUBLE_EQ(seg.getLength(), 5.0);
}

TEST(TaggedLineSegmentTest, ConstructsWithNullParentAndExplicitIndex) {
  const Coordinate p0(-2.5, 1.5);
  const Coordinate p1(2.5, 1.5);
  const std::size_t max_index = std::numeric_limits<std::size_t>::max();
  TaggedLineSegment seg(p0, p1, nullptr, max_index);

  EXPECT_EQ(seg.getParent(), nullptr);
  EXPECT_EQ(seg.getIndex(), max_index);
  EXPECT_TRUE(seg.isHorizontal());
  EXPECT_FALSE(seg.isVertical());
  EXPECT_DOUBLE_EQ(seg.getLength(), 5.0);
}

TEST(TaggedLineSegmentTest, CopyConstructorPreservesTagAndIsolatesCoordinates) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> parent = factory->createPoint(Coordinate(7.0, 8.0));

  TaggedLineSegment original(Coordinate(1.0, 2.0, 10.0),
                             Coordinate(5.0, 2.0, 20.0), parent.get(), 7);
  TaggedLineSegment copy(original);

  EXPECT_EQ(copy.getParent(), parent.get());
  EXPECT_EQ(copy.getIndex(), 7u);
  EXPECT_TRUE(copy.p0.equals3D(original.p0));
  EXPECT_TRUE(copy.p1.equals3D(original.p1));

  // Mutating the copy's inherited coordinates leaves the original untouched.
  copy.p0.x = 99.0;
  copy.p1.z = -1.0;
  EXPECT_DOUBLE_EQ(original.p0.x, 1.0);
  EXPECT_DOUBLE_EQ(original.p1.z, 20.0);
  EXPECT_EQ(copy.getParent(), parent.get());
  EXPECT_EQ(copy.getIndex(), 7u);
}

TEST(TaggedLineSegmentTest, SupportsLineSegmentOperationsAndVectorStorage) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  CoordinateSequence seq;
  seq.add(Coordinate(0.0, 0.0));
  seq.add(Coordinate(0.0, 10.0));
  std::unique_ptr<LineString> parent = factory->createLineString(seq);

  std::vector<TaggedLineSegment> segments;
  segments.emplace_back(Coordinate(0.0, 0.0), Coordinate(0.0, 10.0),
                        parent.get(), 0);
  segments.emplace_back(Coordinate(0.0, 10.0), Coordinate(5.0, 10.0),
                        parent.get(), 1);

  ASSERT_EQ(segments.size(), 2u);
  EXPECT_EQ(segments[0].getParent(), parent.get());
  EXPECT_EQ(segments[0].getIndex(), 0u);
  EXPECT_TRUE(segments[0].isVertical());
  EXPECT_EQ(segments[1].getParent(), parent.get());
  EXPECT_EQ(segments[1].getIndex(), 1u);
  EXPECT_TRUE(segments[1].isHorizontal());

  TaggedLineSegment reversed(segments[0]);
  reversed.reverse();
  const LineSegment& as_base = segments[0];
  EXPECT_TRUE(reversed.equalsTopo(as_base));
  EXPECT_EQ(reversed.getParent(), parent.get());
  EXPECT_EQ(reversed.getIndex(), 0u);
}

TEST(TaggedLineSegmentTest, PreservesNullCoordinates) {
  const Coordinate null_coord = Coordinate::getNull();
  TaggedLineSegment seg(null_coord, null_coord, nullptr, 3);

  EXPECT_TRUE(seg.p0.isNull());
  EXPECT_TRUE(seg.p1.isNull());
  EXPECT_EQ(seg.getParent(), nullptr);
  EXPECT_EQ(seg.getIndex(), 3u);
}

}  // namespace
}  // namespace geos_google
