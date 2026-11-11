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

#include "third_party/geos/include/geos/operation/overlayng/IndexedPointOnLineLocator.h"

#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/locate/PointOnGeometryLocator.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::algorithm::locate::PointOnGeometryLocator;
using geos::geom::Coordinate;
using geos::geom::CoordinateXY;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Location;
using geos::geom::MultiLineString;
using geos::io::WKTReader;
using geos::operation::overlayng::IndexedPointOnLineLocator;

class IndexedPointOnLineLocatorTest : public ::testing::Test {
 protected:
  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};
};

TEST_F(IndexedPointOnLineLocatorTest, OpenLineStringEndpointsInteriorExterior) {
  auto line = reader_.read<LineString>("LINESTRING (0 0, 10 0, 10 10)");
  IndexedPointOnLineLocator locator(*line);

  CoordinateXY start(0.0, 0.0);
  CoordinateXY end(10.0, 10.0);
  CoordinateXY vertex(10.0, 0.0);
  CoordinateXY mid1(5.0, 0.0);
  CoordinateXY mid2(10.0, 5.0);
  CoordinateXY off_line(5.0, 5.0);
  Coordinate with_z(5.0, 0.0, 99.0);

  EXPECT_EQ(locator.locate(&start), Location::BOUNDARY);
  EXPECT_EQ(locator.locate(&end), Location::BOUNDARY);
  EXPECT_EQ(locator.locate(&vertex), Location::INTERIOR);
  EXPECT_EQ(locator.locate(&mid1), Location::INTERIOR);
  EXPECT_EQ(locator.locate(&mid2), Location::INTERIOR);
  EXPECT_EQ(locator.locate(&off_line), Location::EXTERIOR);
  EXPECT_EQ(locator.locate(&with_z), Location::INTERIOR);
}

TEST_F(IndexedPointOnLineLocatorTest, ClosedLineStringAndLinearRing) {
  auto closed_line =
      reader_.read<LineString>("LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  IndexedPointOnLineLocator line_locator(*closed_line);

  CoordinateXY start_end(0.0, 0.0);
  CoordinateXY on_edge(5.0, 0.0);
  CoordinateXY inside_region(5.0, 5.0);
  CoordinateXY outside_region(20.0, 20.0);

  // Closed linear geometries have no boundary points.
  EXPECT_EQ(line_locator.locate(&start_end), Location::INTERIOR);
  EXPECT_EQ(line_locator.locate(&on_edge), Location::INTERIOR);
  // Linear geometries do not enclose area.
  EXPECT_EQ(line_locator.locate(&inside_region), Location::EXTERIOR);
  EXPECT_EQ(line_locator.locate(&outside_region), Location::EXTERIOR);

  auto ring =
      reader_.read<LinearRing>("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  IndexedPointOnLineLocator ring_locator(*ring);
  EXPECT_EQ(ring_locator.locate(&start_end), Location::INTERIOR);
  EXPECT_EQ(ring_locator.locate(&on_edge), Location::INTERIOR);
  EXPECT_EQ(ring_locator.locate(&inside_region), Location::EXTERIOR);
}

TEST_F(IndexedPointOnLineLocatorTest, MultiLineStringMod2BoundaryRule) {
  // Node (10, 0) has degree 3 (odd) -> BOUNDARY.
  auto odd_node = reader_.read<MultiLineString>(
      "MULTILINESTRING ((0 0, 10 0), (10 0, 20 0), (10 0, 10 10))");
  IndexedPointOnLineLocator odd_locator(*odd_node);

  CoordinateXY left_end(0.0, 0.0);
  CoordinateXY right_end(20.0, 0.0);
  CoordinateXY top_end(10.0, 10.0);
  CoordinateXY junction(10.0, 0.0);
  CoordinateXY on_seg(10.0, 5.0);
  CoordinateXY exterior(5.0, 5.0);

  EXPECT_EQ(odd_locator.locate(&left_end), Location::BOUNDARY);
  EXPECT_EQ(odd_locator.locate(&right_end), Location::BOUNDARY);
  EXPECT_EQ(odd_locator.locate(&top_end), Location::BOUNDARY);
  EXPECT_EQ(odd_locator.locate(&junction), Location::BOUNDARY);
  EXPECT_EQ(odd_locator.locate(&on_seg), Location::INTERIOR);
  EXPECT_EQ(odd_locator.locate(&exterior), Location::EXTERIOR);

  // Node (10, 0) has degree 2 (even) -> INTERIOR.
  auto even_node = reader_.read<MultiLineString>(
      "MULTILINESTRING ((0 0, 10 0), (10 0, 20 0))");
  IndexedPointOnLineLocator even_locator(*even_node);
  EXPECT_EQ(even_locator.locate(&junction), Location::INTERIOR);
  EXPECT_EQ(even_locator.locate(&left_end), Location::BOUNDARY);
  EXPECT_EQ(even_locator.locate(&right_end), Location::BOUNDARY);
}

TEST_F(IndexedPointOnLineLocatorTest, EmptyLineGeometriesReturnExterior) {
  auto empty_line = reader_.read("LINESTRING EMPTY");
  auto empty_multiline = reader_.read("MULTILINESTRING EMPTY");

  IndexedPointOnLineLocator line_locator(*empty_line);
  IndexedPointOnLineLocator multiline_locator(*empty_multiline);
  CoordinateXY pt(0.0, 0.0);

  EXPECT_EQ(line_locator.locate(&pt), Location::EXTERIOR);
  EXPECT_EQ(multiline_locator.locate(&pt), Location::EXTERIOR);
}

TEST_F(IndexedPointOnLineLocatorTest, PolymorphicBaseLocatorInterface) {
  auto line = reader_.read("LINESTRING (1 1, 5 1)");
  std::unique_ptr<PointOnGeometryLocator> locator =
      std::make_unique<IndexedPointOnLineLocator>(*line);

  CoordinateXY boundary(1.0, 1.0);
  CoordinateXY interior(3.0, 1.0);
  CoordinateXY exterior(3.0, 2.0);

  EXPECT_EQ(locator->locate(&boundary), Location::BOUNDARY);
  EXPECT_EQ(locator->locate(&interior), Location::INTERIOR);
  EXPECT_EQ(locator->locate(&exterior), Location::EXTERIOR);
}

}  // namespace
}  // namespace geos_google
