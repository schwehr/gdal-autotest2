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

#include "third_party/geos/include/geos/coverage/CoveragePolygon.h"

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::coverage::CoveragePolygon;
using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::geom::GeometryFactory;
using geos::geom::Polygon;
using geos::io::WKTReader;

class CoveragePolygonTest : public ::testing::Test {
 protected:
  CoveragePolygonTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Polygon> ReadPolygon(const std::string& wkt) const {
    return reader_.read<Polygon>(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(CoveragePolygonTest, IntersectsEnvelope) {
  auto poly = ReadPolygon("POLYGON ((0 0, 10 0, 10 4, 4 4, 4 10, 0 10, 0 0))");
  CoveragePolygon cov_poly(poly.get());

  EXPECT_FALSE(cov_poly.intersectsEnv(Envelope(11, 15, 0, 5)));
  EXPECT_FALSE(cov_poly.intersectsEnv(Envelope(2, 8, -5, -1)));
  EXPECT_FALSE(cov_poly.intersectsEnv(Envelope()));

  EXPECT_TRUE(cov_poly.intersectsEnv(Envelope(1, 2, 1, 2)));
  EXPECT_TRUE(cov_poly.intersectsEnv(Envelope(5, 15, 5, 15)));
  EXPECT_TRUE(cov_poly.intersectsEnv(Envelope(10, 12, 10, 12)));
  EXPECT_TRUE(cov_poly.intersectsEnv(Envelope(-5, 15, -5, 15)));
}

TEST_F(CoveragePolygonTest, IntersectsCoordinateEnvelope) {
  auto poly = ReadPolygon("POLYGON ((0 0, 10 0, 10 4, 4 4, 4 10, 0 10, 0 0))");
  CoveragePolygon cov_poly(poly.get());

  EXPECT_FALSE(cov_poly.intersectsEnv(CoordinateXY(-1, 5)));
  EXPECT_FALSE(cov_poly.intersectsEnv(CoordinateXY(5, 11)));

  EXPECT_TRUE(cov_poly.intersectsEnv(CoordinateXY(2, 2)));
  EXPECT_TRUE(cov_poly.intersectsEnv(CoordinateXY(10, 10)));
  // Inside bounding box, though outside the L-shaped polygon itself.
  EXPECT_TRUE(cov_poly.intersectsEnv(CoordinateXY(7, 7)));
}

TEST_F(CoveragePolygonTest, ContainsInteriorBoundaryExteriorAndCachedLocator) {
  auto poly = ReadPolygon("POLYGON ((0 0, 10 0, 10 4, 4 4, 4 10, 0 10, 0 0))");
  CoveragePolygon cov_poly(poly.get());

  // Short-circuits on envelope check before creating locator.
  EXPECT_FALSE(cov_poly.contains(CoordinateXY(15, 5)));

  // First call initializes lazy locator; subsequent calls reuse it.
  EXPECT_TRUE(cov_poly.contains(CoordinateXY(2, 2)));
  EXPECT_TRUE(cov_poly.contains(CoordinateXY(2, 8)));
  EXPECT_TRUE(cov_poly.contains(CoordinateXY(8, 2)));

  // Inside envelope, outside polygon in L-notch concavity.
  EXPECT_FALSE(cov_poly.contains(CoordinateXY(7, 7)));

  // Boundary vertices and edges are not strictly contained
  // (Location::BOUNDARY).
  EXPECT_FALSE(cov_poly.contains(CoordinateXY(0, 0)));
  EXPECT_FALSE(cov_poly.contains(CoordinateXY(5, 0)));
  EXPECT_FALSE(cov_poly.contains(CoordinateXY(4, 4)));
}

TEST_F(CoveragePolygonTest, ContainsRespectsInteriorHoles) {
  auto poly = ReadPolygon(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (3 3, 7 3, 7 7, 3 7, 3 3))");
  CoveragePolygon cov_poly(poly.get());

  EXPECT_TRUE(cov_poly.contains(CoordinateXY(1, 5)));
  EXPECT_TRUE(cov_poly.contains(CoordinateXY(9, 9)));
  EXPECT_FALSE(cov_poly.contains(CoordinateXY(5, 5)));
  EXPECT_FALSE(cov_poly.contains(CoordinateXY(3, 5)));
  EXPECT_FALSE(cov_poly.contains(CoordinateXY(0, 5)));
}

TEST_F(CoveragePolygonTest, EmptyPolygon) {
  auto poly = ReadPolygon("POLYGON EMPTY");
  CoveragePolygon cov_poly(poly.get());

  EXPECT_FALSE(cov_poly.intersectsEnv(Envelope(0, 10, 0, 10)));
  EXPECT_FALSE(cov_poly.intersectsEnv(CoordinateXY(0, 0)));
  EXPECT_FALSE(cov_poly.contains(CoordinateXY(0, 0)));
}

}  // namespace
}  // namespace geos_google
