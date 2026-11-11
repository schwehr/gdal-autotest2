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

#include "third_party/geos/include/geos/linearref/LinearGeometryBuilder.h"

#include <cmath>
#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::geom::MultiLineString;
using geos::linearref::LinearGeometryBuilder;
using geos::util::IllegalArgumentException;

class LinearGeometryBuilderTest : public ::testing::Test {
 protected:
  LinearGeometryBuilderTest() : factory_(GeometryFactory::create()) {}

  GeometryFactory::Ptr factory_;
};

TEST_F(LinearGeometryBuilderTest, EmptyBuilderProducesEmptyGeometryCollection) {
  LinearGeometryBuilder builder(factory_.get());

  const Coordinate initial = builder.getLastCoordinate();
  EXPECT_DOUBLE_EQ(initial.x, 0.0);
  EXPECT_DOUBLE_EQ(initial.y, 0.0);
  EXPECT_TRUE(std::isnan(initial.z));

  // Ending when no coordinates have been added is a safe no-op.
  builder.endLine();

  std::unique_ptr<Geometry> geom = builder.getGeometry();
  ASSERT_NE(geom, nullptr);
  EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_TRUE(geom->isEmpty());
  EXPECT_EQ(geom->getNumGeometries(), 0u);
}

TEST_F(LinearGeometryBuilderTest, SingleLineStringWithImplicitAndExplicitEnd) {
  // Implicit endLine() inside getGeometry().
  {
    LinearGeometryBuilder builder(factory_.get());
    builder.add(Coordinate(0.0, 0.0));
    EXPECT_TRUE(builder.getLastCoordinate().equals2D(Coordinate(0.0, 0.0)));
    builder.add(Coordinate(5.0, 0.0));
    EXPECT_TRUE(builder.getLastCoordinate().equals2D(Coordinate(5.0, 0.0)));
    builder.add(Coordinate(5.0, 10.0));
    EXPECT_TRUE(builder.getLastCoordinate().equals2D(Coordinate(5.0, 10.0)));

    std::unique_ptr<Geometry> geom = builder.getGeometry();
    ASSERT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
    const auto* line = dynamic_cast<const LineString*>(geom.get());
    ASSERT_NE(line, nullptr);
    EXPECT_EQ(line->getNumPoints(), 3u);
    EXPECT_DOUBLE_EQ(line->getLength(), 15.0);
    EXPECT_TRUE(line->getCoordinateN(0).equals2D(Coordinate(0.0, 0.0)));
    EXPECT_TRUE(line->getCoordinateN(1).equals2D(Coordinate(5.0, 0.0)));
    EXPECT_TRUE(line->getCoordinateN(2).equals2D(Coordinate(5.0, 10.0)));
  }

  // Explicit endLine() followed by getGeometry().
  {
    LinearGeometryBuilder builder(factory_.get());
    builder.add(Coordinate(1.0, 2.0));
    builder.add(Coordinate(4.0, 6.0));
    builder.endLine();
    builder.endLine();  // Second consecutive endLine() is a no-op.

    std::unique_ptr<Geometry> geom = builder.getGeometry();
    ASSERT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
    EXPECT_DOUBLE_EQ(geom->getLength(), 5.0);
  }
}

TEST_F(LinearGeometryBuilderTest, MultipleLinesProduceMultiLineString) {
  LinearGeometryBuilder builder(factory_.get());

  builder.add(Coordinate(0.0, 0.0));
  builder.add(Coordinate(10.0, 0.0));
  builder.endLine();

  builder.add(Coordinate(20.0, 0.0));
  builder.add(Coordinate(20.0, 5.0));
  builder.add(Coordinate(30.0, 5.0));
  // Omit final endLine() to verify getGeometry() closes the trailing line.

  std::unique_ptr<Geometry> geom = builder.getGeometry();
  ASSERT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  const auto* mls = dynamic_cast<const MultiLineString*>(geom.get());
  ASSERT_NE(mls, nullptr);
  ASSERT_EQ(mls->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(mls->getLength(), 25.0);
  EXPECT_EQ(mls->getGeometryN(0)->getNumPoints(), 2u);
  EXPECT_EQ(mls->getGeometryN(1)->getNumPoints(), 3u);
  EXPECT_TRUE(
      mls->getGeometryN(1)->getCoordinateN(2).equals2D(Coordinate(30.0, 5.0)));
}

TEST_F(LinearGeometryBuilderTest, AllowRepeatedPointsControlsDeduplication) {
  LinearGeometryBuilder builder(factory_.get());

  // allowRepeatedPoints = true keeps consecutive equal points.
  builder.add(Coordinate(1.0, 2.0), true);
  builder.add(Coordinate(1.0, 2.0), true);
  builder.endLine();

  // allowRepeatedPoints = false collapses consecutive equal points.
  builder.add(Coordinate(0.0, 0.0), false);
  builder.add(Coordinate(0.0, 0.0, 7.0), false);
  // lastPt updates to the passed coordinate even if deduplicated.
  const Coordinate last = builder.getLastCoordinate();
  EXPECT_DOUBLE_EQ(last.x, 0.0);
  EXPECT_DOUBLE_EQ(last.y, 0.0);
  EXPECT_DOUBLE_EQ(last.z, 7.0);

  builder.add(Coordinate(10.0, 0.0), false);
  builder.add(Coordinate(10.0, 0.0), false);
  builder.endLine();

  std::unique_ptr<Geometry> geom = builder.getGeometry();
  ASSERT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  const auto* mls = dynamic_cast<const MultiLineString*>(geom.get());
  ASSERT_NE(mls, nullptr);
  ASSERT_EQ(mls->getNumGeometries(), 2u);
  EXPECT_EQ(mls->getGeometryN(0)->getNumPoints(), 2u);
  EXPECT_DOUBLE_EQ(mls->getGeometryN(0)->getLength(), 0.0);
  EXPECT_EQ(mls->getGeometryN(1)->getNumPoints(), 2u);
  EXPECT_DOUBLE_EQ(mls->getGeometryN(1)->getLength(), 10.0);
}

TEST_F(LinearGeometryBuilderTest, SinglePointLineThrowsByDefault) {
  {
    LinearGeometryBuilder builder(factory_.get());
    builder.add(Coordinate(3.0, 4.0));
    EXPECT_THROW(builder.endLine(), IllegalArgumentException);
    // Builder remains empty and valid after failed endLine().
    EXPECT_TRUE(builder.getGeometry()->isEmpty());
  }

  {
    LinearGeometryBuilder builder(factory_.get());
    builder.add(Coordinate(5.0, 6.0), false);
    builder.add(Coordinate(5.0, 6.0), false);
    EXPECT_THROW(builder.getGeometry(), IllegalArgumentException);
  }
}

TEST_F(LinearGeometryBuilderTest, IgnoreInvalidLinesDiscardsSinglePointLines) {
  {
    LinearGeometryBuilder builder(factory_.get());
    builder.setIgnoreInvalidLines(true);

    builder.add(Coordinate(1.0, 1.0));
    builder.endLine();

    builder.add(Coordinate(0.0, 0.0));
    builder.add(Coordinate(4.0, 0.0));
    builder.endLine();

    // Trailing 1-point line collapsed from repeated points, closed by
    // getGeometry().
    builder.add(Coordinate(9.0, 9.0), false);
    builder.add(Coordinate(9.0, 9.0), false);

    std::unique_ptr<Geometry> geom = builder.getGeometry();
    ASSERT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
    EXPECT_DOUBLE_EQ(geom->getLength(), 4.0);
    EXPECT_EQ(geom->getNumPoints(), 2u);
  }

  // Ignoring all lines yields an empty GeometryCollection.
  {
    LinearGeometryBuilder builder(factory_.get());
    builder.setIgnoreInvalidLines(true);
    builder.add(Coordinate(2.0, 3.0));
    builder.endLine();

    std::unique_ptr<Geometry> geom = builder.getGeometry();
    EXPECT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
    EXPECT_TRUE(geom->isEmpty());
  }

  // Re-disabling ignoreInvalidLines restores throwing behavior.
  {
    LinearGeometryBuilder builder(factory_.get());
    builder.setIgnoreInvalidLines(true);
    builder.setIgnoreInvalidLines(false);
    builder.add(Coordinate(2.0, 3.0));
    EXPECT_THROW(builder.endLine(), IllegalArgumentException);
  }
}

TEST_F(LinearGeometryBuilderTest, FixInvalidLinesDuplicatesSinglePoint) {
  {
    LinearGeometryBuilder builder(factory_.get());
    builder.setFixInvalidLines(true);

    builder.add(Coordinate(7.0, 8.0));
    builder.endLine();

    builder.add(Coordinate(0.0, 0.0));
    builder.add(Coordinate(3.0, 4.0));
    builder.endLine();

    std::unique_ptr<Geometry> geom = builder.getGeometry();
    ASSERT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
    const auto* mls = dynamic_cast<const MultiLineString*>(geom.get());
    ASSERT_NE(mls, nullptr);
    ASSERT_EQ(mls->getNumGeometries(), 2u);

    const LineString* fixed = mls->getGeometryN(0);
    ASSERT_EQ(fixed->getNumPoints(), 2u);
    EXPECT_TRUE(fixed->getCoordinateN(0).equals2D(Coordinate(7.0, 8.0)));
    EXPECT_TRUE(fixed->getCoordinateN(1).equals2D(Coordinate(7.0, 8.0)));
    EXPECT_DOUBLE_EQ(fixed->getLength(), 0.0);

    EXPECT_DOUBLE_EQ(mls->getGeometryN(1)->getLength(), 5.0);
  }

  // ignoreInvalidLines takes precedence when both flags are enabled.
  {
    LinearGeometryBuilder builder(factory_.get());
    builder.setFixInvalidLines(true);
    builder.setIgnoreInvalidLines(true);
    builder.add(Coordinate(7.0, 8.0));
    std::unique_ptr<Geometry> geom = builder.getGeometry();
    EXPECT_TRUE(geom->isEmpty());
  }

  // Disabling fixInvalidLines restores throwing behavior.
  {
    LinearGeometryBuilder builder(factory_.get());
    builder.setFixInvalidLines(true);
    builder.setFixInvalidLines(false);
    builder.add(Coordinate(7.0, 8.0));
    EXPECT_THROW(builder.endLine(), IllegalArgumentException);
  }
}

TEST_F(LinearGeometryBuilderTest, PreservesZOrdinatesAndCleansUpOnDestruction) {
  {
    LinearGeometryBuilder builder(factory_.get());
    builder.add(Coordinate(1.0, 2.0, 10.0));
    builder.add(Coordinate(4.0, 6.0, 20.0));
    std::unique_ptr<Geometry> geom = builder.getGeometry();
    ASSERT_EQ(geom->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
    const auto* line = dynamic_cast<const LineString*>(geom.get());
    ASSERT_NE(line, nullptr);
    EXPECT_TRUE(line->hasZ());
    EXPECT_DOUBLE_EQ(line->getCoordinateN(0).z, 10.0);
    EXPECT_DOUBLE_EQ(line->getCoordinateN(1).z, 20.0);
  }

  // Destroy with both finalized lines and pending points without calling
  // getGeometry().
  {
    LinearGeometryBuilder builder(factory_.get());
    builder.add(Coordinate(0.0, 0.0));
    builder.add(Coordinate(1.0, 0.0));
    builder.endLine();
    builder.add(Coordinate(2.0, 0.0));
  }
}

}  // namespace
}  // namespace geos_google
