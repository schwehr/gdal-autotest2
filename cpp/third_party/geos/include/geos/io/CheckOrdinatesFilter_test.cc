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

// Unit tests for third_party/geos/include/geos/io/CheckOrdinatesFilter.h.
//
// google3 notes:
// - Allocates isolated GeometryFactory instances via GeometryFactory::create().
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/io/CheckOrdinatesFilter.h

#include "third_party/geos/include/geos/io/CheckOrdinatesFilter.h"

#include <cstddef>
#include <initializer_list>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/io/OrdinateSet.h"

namespace geos_google {
namespace {

using geos::DoubleNotANumber;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::geom::Point;
using geos::io::CheckOrdinatesFilter;
using geos::io::OrdinateSet;

TEST(CheckOrdinatesFilterTest, InitialStateMatchesCheckFlags) {
  CheckOrdinatesFilter xy_filter(OrdinateSet::createXY());
  EXPECT_EQ(xy_filter.getFoundOrdinates(), OrdinateSet::createXY());
  EXPECT_FALSE(xy_filter.isGeometryChanged());
  EXPECT_TRUE(xy_filter.isDone());

  CheckOrdinatesFilter xyz_filter(OrdinateSet::createXYZ());
  EXPECT_EQ(xyz_filter.getFoundOrdinates(), OrdinateSet::createXY());
  EXPECT_FALSE(xyz_filter.isGeometryChanged());
  EXPECT_FALSE(xyz_filter.isDone());

  CheckOrdinatesFilter xym_filter(OrdinateSet::createXYM());
  EXPECT_EQ(xym_filter.getFoundOrdinates(), OrdinateSet::createXY());
  EXPECT_FALSE(xym_filter.isGeometryChanged());
  EXPECT_FALSE(xym_filter.isDone());

  CheckOrdinatesFilter xyzm_filter(OrdinateSet::createXYZM());
  EXPECT_EQ(xyzm_filter.getFoundOrdinates(), OrdinateSet::createXY());
  EXPECT_FALSE(xyzm_filter.isGeometryChanged());
  EXPECT_FALSE(xyzm_filter.isDone());
}

TEST(CheckOrdinatesFilterTest, SkipsInspectionWhenNoOrdinatesNeedChecking) {
  const CoordinateSequence seq{
      CoordinateXYZM(1.0, 2.0, 3.0, 4.0),
      CoordinateXYZM(5.0, 6.0, 7.0, 8.0),
  };

  // Checking only XY skips reading coordinates completely.
  CheckOrdinatesFilter xy_filter(OrdinateSet::createXY());
  xy_filter.filter_ro(seq, 0);
  EXPECT_EQ(xy_filter.getFoundOrdinates(), OrdinateSet::createXY());
  EXPECT_TRUE(xy_filter.isDone());

  // Once Z is found for an XYZ filter, subsequent calls skip reading M too.
  CheckOrdinatesFilter xyz_filter(OrdinateSet::createXYZ());
  xyz_filter.filter_ro(seq, 0);
  EXPECT_EQ(xyz_filter.getFoundOrdinates(), OrdinateSet::createXYZ());
  EXPECT_TRUE(xyz_filter.isDone());

  xyz_filter.filter_ro(seq, 1);
  EXPECT_EQ(xyz_filter.getFoundOrdinates(), OrdinateSet::createXYZ());
  EXPECT_TRUE(xyz_filter.isDone());
  EXPECT_FALSE(xyz_filter.isGeometryChanged());
}

TEST(CheckOrdinatesFilterTest, DetectsOrdinatesAcrossSequenceDimensions) {
  const CoordinateSequence xy_seq{CoordinateXY(1.0, 2.0)};
  const CoordinateSequence xyz_seq{Coordinate(1.0, 2.0, 3.0)};
  const CoordinateSequence xym_seq{CoordinateXYM(1.0, 2.0, 4.0)};
  const CoordinateSequence xyzm_seq{CoordinateXYZM(1.0, 2.0, 3.0, 4.0)};

  {
    CheckOrdinatesFilter filter(OrdinateSet::createXYZM());
    filter.filter_ro(xy_seq, 0);
    EXPECT_EQ(filter.getFoundOrdinates(), OrdinateSet::createXY());
    EXPECT_FALSE(filter.isDone());
  }
  {
    CheckOrdinatesFilter filter(OrdinateSet::createXYZM());
    filter.filter_ro(xyz_seq, 0);
    EXPECT_EQ(filter.getFoundOrdinates(), OrdinateSet::createXYZ());
    EXPECT_FALSE(filter.isDone());
  }
  {
    CheckOrdinatesFilter filter(OrdinateSet::createXYZM());
    filter.filter_ro(xym_seq, 0);
    EXPECT_EQ(filter.getFoundOrdinates(), OrdinateSet::createXYM());
    EXPECT_FALSE(filter.isDone());
  }
  {
    CheckOrdinatesFilter filter(OrdinateSet::createXYZM());
    filter.filter_ro(xyzm_seq, 0);
    EXPECT_EQ(filter.getFoundOrdinates(), OrdinateSet::createXYZM());
    EXPECT_TRUE(filter.isDone());
  }
}

TEST(CheckOrdinatesFilterTest, RespectsCheckFlagsMaskOnXYZMSequence) {
  const CoordinateSequence seq{CoordinateXYZM(1.0, 2.0, 10.0, 20.0)};

  CheckOrdinatesFilter xyz_only(OrdinateSet::createXYZ());
  xyz_only.filter_ro(seq, 0);
  EXPECT_EQ(xyz_only.getFoundOrdinates(), OrdinateSet::createXYZ());
  EXPECT_TRUE(xyz_only.isDone());

  CheckOrdinatesFilter xym_only(OrdinateSet::createXYM());
  xym_only.filter_ro(seq, 0);
  EXPECT_EQ(xym_only.getFoundOrdinates(), OrdinateSet::createXYM());
  EXPECT_TRUE(xym_only.isDone());
}

TEST(CheckOrdinatesFilterTest, IgnoresNaNAndAccumulatesAcrossCoordinates) {
  const CoordinateSequence seq{
      CoordinateXYZM(0.0, 0.0, DoubleNotANumber, DoubleNotANumber),
      CoordinateXYZM(1.0, 1.0, 10.0, DoubleNotANumber),
      CoordinateXYZM(2.0, 2.0, 20.0, DoubleNotANumber),
      CoordinateXYZM(3.0, 3.0, DoubleNotANumber, 40.0),
  };

  CheckOrdinatesFilter filter(OrdinateSet::createXYZM());

  filter.filter_ro(seq, 0);
  EXPECT_EQ(filter.getFoundOrdinates(), OrdinateSet::createXY());
  EXPECT_FALSE(filter.isDone());

  filter.filter_ro(seq, 1);
  EXPECT_EQ(filter.getFoundOrdinates(), OrdinateSet::createXYZ());
  EXPECT_FALSE(filter.isDone());

  // Z is already found, M is still NaN at index 2.
  filter.filter_ro(seq, 2);
  EXPECT_EQ(filter.getFoundOrdinates(), OrdinateSet::createXYZ());
  EXPECT_FALSE(filter.isDone());

  // M becomes present at index 3 while Z is NaN.
  filter.filter_ro(seq, 3);
  EXPECT_EQ(filter.getFoundOrdinates(), OrdinateSet::createXYZM());
  EXPECT_TRUE(filter.isDone());
}

TEST(CheckOrdinatesFilterTest, AccumulatesMFirstThenZAndCountsZeroAsPresent) {
  const CoordinateSequence seq{
      CoordinateXYZM(0.0, 0.0, DoubleNotANumber, 0.0),
      CoordinateXYZM(1.0, 1.0, 0.0, DoubleNotANumber),
  };

  CheckOrdinatesFilter filter(OrdinateSet::createXYZM());

  filter.filter_ro(seq, 0);
  EXPECT_EQ(filter.getFoundOrdinates(), OrdinateSet::createXYM());
  EXPECT_FALSE(filter.isDone());

  filter.filter_ro(seq, 1);
  EXPECT_EQ(filter.getFoundOrdinates(), OrdinateSet::createXYZM());
  EXPECT_TRUE(filter.isDone());
}

TEST(CheckOrdinatesFilterTest, UnpopulatedXYZandXYZMSequencesReportOnlyXY) {
  CoordinateSequence xyz_nan = CoordinateSequence::XYZ(2);
  CoordinateSequence xyzm_nan = CoordinateSequence::XYZM(2);

  CheckOrdinatesFilter filter_xyz(OrdinateSet::createXYZM());
  filter_xyz.filter_ro(xyz_nan, 0);
  filter_xyz.filter_ro(xyz_nan, 1);
  EXPECT_EQ(filter_xyz.getFoundOrdinates(), OrdinateSet::createXY());
  EXPECT_FALSE(filter_xyz.isDone());

  CheckOrdinatesFilter filter_xyzm(OrdinateSet::createXYZM());
  filter_xyzm.filter_ro(xyzm_nan, 0);
  filter_xyzm.filter_ro(xyzm_nan, 1);
  EXPECT_EQ(filter_xyzm.getFoundOrdinates(), OrdinateSet::createXY());
  EXPECT_FALSE(filter_xyzm.isDone());

  // Infinite values are non-NaN and count as present ordinates.
  const double inf = std::numeric_limits<double>::infinity();
  const CoordinateSequence inf_seq{CoordinateXYZM(1.0, 2.0, inf, -inf)};
  CheckOrdinatesFilter inf_filter(OrdinateSet::createXYZM());
  inf_filter.filter_ro(inf_seq, 0);
  EXPECT_EQ(inf_filter.getFoundOrdinates(), OrdinateSet::createXYZM());
  EXPECT_TRUE(inf_filter.isDone());
}

TEST(CheckOrdinatesFilterTest, WorksWithGeometryApplyRoAndShortCircuits) {
  const GeometryFactory::Ptr factory = GeometryFactory::create();

  // Empty XYZM point yields no coordinates visited.
  const std::unique_ptr<Point> empty_point = factory->createPoint(true, true);
  CheckOrdinatesFilter empty_filter(OrdinateSet::createXYZM());
  empty_point->apply_ro(empty_filter);
  EXPECT_EQ(empty_filter.getFoundOrdinates(), OrdinateSet::createXY());
  EXPECT_FALSE(empty_filter.isDone());
  EXPECT_FALSE(empty_filter.isGeometryChanged());

  // LineString with NaN in first point, Z in second, M in third.
  auto line_seq = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXYZM>{
          CoordinateXYZM(0.0, 0.0, DoubleNotANumber, DoubleNotANumber),
          CoordinateXYZM(1.0, 1.0, 5.0, DoubleNotANumber),
          CoordinateXYZM(2.0, 2.0, DoubleNotANumber, 9.0),
      });
  std::unique_ptr<LineString> line =
      factory->createLineString(std::move(line_seq));

  CheckOrdinatesFilter line_filter(OrdinateSet::createXYZM());
  line->apply_ro(line_filter);
  EXPECT_EQ(line_filter.getFoundOrdinates(), OrdinateSet::createXYZM());
  EXPECT_TRUE(line_filter.isDone());

  // GeometryCollection stops iterating once filter.isDone() is true.
  std::vector<std::unique_ptr<Geometry>> parts;
  parts.push_back(factory->createPoint(CoordinateXY(0.0, 0.0)));
  parts.push_back(factory->createPoint(Coordinate(1.0, 1.0, 2.0)));
  parts.push_back(factory->createPoint(CoordinateXYM(2.0, 2.0, 3.0)));
  const std::unique_ptr<GeometryCollection> collection =
      factory->createGeometryCollection(std::move(parts));

  CheckOrdinatesFilter xyz_collection_filter(OrdinateSet::createXYZ());
  collection->apply_ro(xyz_collection_filter);
  EXPECT_EQ(xyz_collection_filter.getFoundOrdinates(),
            OrdinateSet::createXYZ());
  EXPECT_TRUE(xyz_collection_filter.isDone());

  CheckOrdinatesFilter xyzm_collection_filter(OrdinateSet::createXYZM());
  collection->apply_ro(xyzm_collection_filter);
  EXPECT_EQ(xyzm_collection_filter.getFoundOrdinates(),
            OrdinateSet::createXYZM());
  EXPECT_TRUE(xyzm_collection_filter.isDone());
}

}  // namespace
}  // namespace geos_google
