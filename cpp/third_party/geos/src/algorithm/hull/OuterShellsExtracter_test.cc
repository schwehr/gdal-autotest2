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

// Unit tests for third_party/geos/src/algorithm/hull/OuterShellsExtracter.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/algorithm/hull/OuterShellsExtracter.cpp

#include "third_party/geos/include/geos/algorithm/hull/OuterShellsExtracter.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::algorithm::hull::OuterShellsExtracter;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::MultiPolygon;
using geos::geom::Polygon;
using geos::io::WKTReader;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

TEST(OuterShellsExtracterTest, EmptyMultiPolygonYieldsNoShells) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  const std::string wkt = "MULTIPOLYGON EMPTY";
  std::unique_ptr<Geometry> geom = reader.read(wkt);
  ASSERT_NE(geom, nullptr);

  std::vector<const LinearRing*> shells;
  OuterShellsExtracter::extractShells(geom.get(), shells);
  EXPECT_THAT(shells, IsEmpty());
}

TEST(OuterShellsExtracterTest, EmptyGeometryCollectionYieldsNoShells) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  std::unique_ptr<Geometry> geom = reader.read("GEOMETRYCOLLECTION EMPTY");
  ASSERT_NE(geom, nullptr);

  std::vector<const LinearRing*> shells;
  OuterShellsExtracter::extractShells(geom.get(), shells);
  EXPECT_THAT(shells, IsEmpty());
}

TEST(OuterShellsExtracterTest, SinglePolygonWithHolesExtractsExteriorRingOnly) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  std::unique_ptr<Geometry> geom = reader.read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 8, 8 8, 8 2, 2 2))");
  ASSERT_NE(geom, nullptr);
  const auto* poly = dynamic_cast<const Polygon*>(geom.get());
  ASSERT_NE(poly, nullptr);

  std::vector<const LinearRing*> shells;
  OuterShellsExtracter::extractShells(poly, shells);
  EXPECT_THAT(shells, ElementsAre(poly->getExteriorRing()));
  EXPECT_NE(shells[0], poly->getInteriorRingN(0));
}

TEST(OuterShellsExtracterTest, SingleEmptyPolygonExtractsEmptyExteriorRing) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  std::unique_ptr<Geometry> geom = reader.read("POLYGON EMPTY");
  ASSERT_NE(geom, nullptr);
  const auto* poly = dynamic_cast<const Polygon*>(geom.get());
  ASSERT_NE(poly, nullptr);

  std::vector<const LinearRing*> shells;
  OuterShellsExtracter::extractShells(poly, shells);
  ASSERT_EQ(shells.size(), 1u);
  EXPECT_EQ(shells[0], poly->getExteriorRing());
  EXPECT_TRUE(shells[0]->isEmpty());
}

TEST(OuterShellsExtracterTest,
     DisjointPolygonsOrderedByDecreasingEnvelopeArea) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  // Areas: index 0 -> 1, index 1 -> 16, index 2 -> 4.
  std::unique_ptr<Geometry> geom = reader.read(
      "MULTIPOLYGON ("
      "((0 0, 1 0, 1 1, 0 1, 0 0)),"
      "((10 0, 14 0, 14 4, 10 4, 10 0)),"
      "((20 0, 22 0, 22 2, 20 2, 20 0)))");
  ASSERT_NE(geom, nullptr);
  const auto* mp = dynamic_cast<const MultiPolygon*>(geom.get());
  ASSERT_NE(mp, nullptr);

  std::vector<const LinearRing*> shells;
  OuterShellsExtracter::extractShells(mp, shells);
  EXPECT_THAT(shells, ElementsAre(mp->getGeometryN(1)->getExteriorRing(),
                                  mp->getGeometryN(2)->getExteriorRing(),
                                  mp->getGeometryN(0)->getExteriorRing()));
}

TEST(OuterShellsExtracterTest, EqualAreaDisjointPolygonsBothExtracted) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  std::unique_ptr<Geometry> geom = reader.read(
      "MULTIPOLYGON ("
      "((0 0, 3 0, 3 3, 0 3, 0 0)),"
      "((10 0, 13 0, 13 3, 10 3, 10 0)),"
      "((20 0, 23 0, 23 3, 20 3, 20 0)))");
  ASSERT_NE(geom, nullptr);
  const auto* mp = dynamic_cast<const MultiPolygon*>(geom.get());
  ASSERT_NE(mp, nullptr);

  std::vector<const LinearRing*> shells;
  OuterShellsExtracter::extractShells(mp, shells);
  EXPECT_THAT(shells,
              UnorderedElementsAre(mp->getGeometryN(0)->getExteriorRing(),
                                   mp->getGeometryN(1)->getExteriorRing(),
                                   mp->getGeometryN(2)->getExteriorRing()));
}

TEST(OuterShellsExtracterTest,
     NestedIslandInsideHoleIsExcludedRegardlessOfOrder) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // Outer polygon first, nested island second.
  std::unique_ptr<Geometry> outer_first = reader.read(
      "MULTIPOLYGON ("
      "((0 0, 20 0, 20 20, 0 20, 0 0), (2 2, 2 18, 18 18, 18 2, 2 2)),"
      "((5 5, 15 5, 15 15, 5 15, 5 5)))");
  ASSERT_NE(outer_first, nullptr);
  const auto* mp1 = dynamic_cast<const MultiPolygon*>(outer_first.get());
  ASSERT_NE(mp1, nullptr);

  std::vector<const LinearRing*> shells1;
  OuterShellsExtracter::extractShells(mp1, shells1);
  EXPECT_THAT(shells1, ElementsAre(mp1->getGeometryN(0)->getExteriorRing()));

  // Nested island first, outer polygon second.
  std::unique_ptr<Geometry> island_first = reader.read(
      "MULTIPOLYGON ("
      "((5 5, 15 5, 15 15, 5 15, 5 5)),"
      "((0 0, 20 0, 20 20, 0 20, 0 0), (2 2, 2 18, 18 18, 18 2, 2 2)))");
  ASSERT_NE(island_first, nullptr);
  const auto* mp2 = dynamic_cast<const MultiPolygon*>(island_first.get());
  ASSERT_NE(mp2, nullptr);

  std::vector<const LinearRing*> shells2;
  OuterShellsExtracter::extractShells(mp2, shells2);
  EXPECT_THAT(shells2, ElementsAre(mp2->getGeometryN(1)->getExteriorRing()));
}

TEST(OuterShellsExtracterTest, DeeplyNestedIslandsInsideHolesAreExcluded) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  std::unique_ptr<Geometry> geom = reader.read(
      "MULTIPOLYGON ("
      "((8 8, 22 8, 22 22, 8 22, 8 8)),"
      "((4 4, 26 4, 26 26, 4 26, 4 4), (6 6, 6 24, 24 24, 24 6, 6 6)),"
      "((0 0, 30 0, 30 30, 0 30, 0 0), (2 2, 2 28, 28 28, 28 2, 2 2)))");
  ASSERT_NE(geom, nullptr);
  const auto* mp = dynamic_cast<const MultiPolygon*>(geom.get());
  ASSERT_NE(mp, nullptr);

  std::vector<const LinearRing*> shells;
  OuterShellsExtracter::extractShells(mp, shells);
  EXPECT_THAT(shells, ElementsAre(mp->getGeometryN(2)->getExteriorRing()));
}

TEST(OuterShellsExtracterTest, PolygonInsideConcaveExteriorEnvelopeIsRetained) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  // Polygon 0 is L-shaped with envelope [0..10] x [0..10] (area 100).
  // Polygon 1 sits in the concave notch [4..8] x [4..8] (area 16), so its
  // envelope is covered by polygon 0's envelope, yet its point is outside ring
  // 0.
  std::unique_ptr<Geometry> geom = reader.read(
      "MULTIPOLYGON ("
      "((0 0, 10 0, 10 2, 2 2, 2 10, 0 10, 0 0)),"
      "((4 4, 8 4, 8 8, 4 8, 4 4)))");
  ASSERT_NE(geom, nullptr);
  const auto* mp = dynamic_cast<const MultiPolygon*>(geom.get());
  ASSERT_NE(mp, nullptr);

  std::vector<const LinearRing*> shells;
  OuterShellsExtracter::extractShells(mp, shells);
  EXPECT_THAT(shells, ElementsAre(mp->getGeometryN(0)->getExteriorRing(),
                                  mp->getGeometryN(1)->getExteriorRing()));
}

TEST(OuterShellsExtracterTest, MixedOuterNestedAndNotchPolygons) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  // 0: nested island inside 3's hole
  // 1: small square inside 4's concave notch
  // 2: standalone 5x5 outer square
  // 3: 20x20 outer polygon with hole
  // 4: 12x12 U-shaped outer polygon
  std::unique_ptr<Geometry> geom = reader.read(
      "MULTIPOLYGON ("
      "((5 5, 15 5, 15 15, 5 15, 5 5)),"
      "((34 4, 37 4, 37 7, 34 7, 34 4)),"
      "((60 0, 65 0, 65 5, 60 5, 60 0)),"
      "((0 0, 20 0, 20 20, 0 20, 0 0), (2 2, 2 18, 18 18, 18 2, 2 2)),"
      "((30 0, 42 0, 42 12, 39 12, 39 2, 32 2, 32 12, 30 12, 30 0)))");
  ASSERT_NE(geom, nullptr);
  const auto* mp = dynamic_cast<const MultiPolygon*>(geom.get());
  ASSERT_NE(mp, nullptr);

  std::vector<const LinearRing*> shells;
  OuterShellsExtracter::extractShells(mp, shells);
  EXPECT_THAT(shells, ElementsAre(mp->getGeometryN(3)->getExteriorRing(),
                                  mp->getGeometryN(4)->getExteriorRing(),
                                  mp->getGeometryN(2)->getExteriorRing(),
                                  mp->getGeometryN(1)->getExteriorRing()));
}

TEST(OuterShellsExtracterTest,
     GeometryCollectionOfPolygonsExtractsOuterShells) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  std::unique_ptr<Geometry> geom = reader.read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), (2 2, 2 18, 18 18, 18 2, 2 2)),"
      "POLYGON ((5 5, 15 5, 15 15, 5 15, 5 5)),"
      "POLYGON ((30 0, 34 0, 34 4, 30 4, 30 0)))");
  ASSERT_NE(geom, nullptr);
  const auto* gc = dynamic_cast<const GeometryCollection*>(geom.get());
  ASSERT_NE(gc, nullptr);
  const auto* poly0 = dynamic_cast<const Polygon*>(gc->getGeometryN(0));
  const auto* poly2 = dynamic_cast<const Polygon*>(gc->getGeometryN(2));
  ASSERT_NE(poly0, nullptr);
  ASSERT_NE(poly2, nullptr);

  std::vector<const LinearRing*> shells;
  OuterShellsExtracter::extractShells(gc, shells);
  EXPECT_THAT(shells,
              ElementsAre(poly0->getExteriorRing(), poly2->getExteriorRing()));
}

TEST(OuterShellsExtracterTest,
     MultiPolygonContainingEmptyPolygonHandlesNullEnvelope) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  std::unique_ptr<Geometry> geom =
      reader.read("MULTIPOLYGON (EMPTY, ((0 0, 10 0, 10 10, 0 10, 0 0)))");
  ASSERT_NE(geom, nullptr);
  const auto* mp = dynamic_cast<const MultiPolygon*>(geom.get());
  ASSERT_NE(mp, nullptr);

  std::vector<const LinearRing*> shells;
  OuterShellsExtracter::extractShells(mp, shells);
  EXPECT_THAT(shells, ElementsAre(mp->getGeometryN(1)->getExteriorRing(),
                                  mp->getGeometryN(0)->getExteriorRing()));
}

TEST(OuterShellsExtracterTest, PolygonsWithZMCoordinates) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  std::unique_ptr<Geometry> geom = reader.read(
      "MULTIPOLYGON ZM ("
      "((0 0 1 10, 20 0 1 10, 20 20 1 10, 0 20 1 10, 0 0 1 10),"
      " (2 2 2 20, 2 18 2 20, 18 18 2 20, 18 2 2 20, 2 2 2 20)),"
      "((6 6 3 30, 14 6 3 30, 14 14 3 30, 6 14 3 30, 6 6 3 30)))");
  ASSERT_NE(geom, nullptr);
  const auto* mp = dynamic_cast<const MultiPolygon*>(geom.get());
  ASSERT_NE(mp, nullptr);

  std::vector<const LinearRing*> shells;
  OuterShellsExtracter::extractShells(mp, shells);
  EXPECT_THAT(shells, ElementsAre(mp->getGeometryN(0)->getExteriorRing()));
}

}  // namespace
}  // namespace geos_google
