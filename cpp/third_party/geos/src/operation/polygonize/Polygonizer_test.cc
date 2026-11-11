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

#include "third_party/geos/include/geos/operation/polygonize/Polygonizer.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/IntersectionMatrix.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/GEOSException.h"
#include "third_party/geos/include/geos/util/Interrupt.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::operation::polygonize::Polygonizer;

std::vector<double> SortedAreas(
    const std::vector<std::unique_ptr<Polygon>>& polys) {
  std::vector<double> areas;
  areas.reserve(polys.size());
  for (const auto& poly : polys) {
    areas.push_back(poly->getArea());
  }
  std::sort(areas.begin(), areas.end());
  return areas;
}

TEST(PolygonizerTest, EmptyPolygonizerReturnsEmptyResultsAndNoErrors) {
  Polygonizer polygonizer;

  EXPECT_TRUE(polygonizer.getPolygons().empty());
  EXPECT_FALSE(polygonizer.hasDangles());
  EXPECT_TRUE(polygonizer.getDangles().empty());
  EXPECT_FALSE(polygonizer.hasCutEdges());
  EXPECT_TRUE(polygonizer.getCutEdges().empty());
  EXPECT_FALSE(polygonizer.hasInvalidRingLines());
  EXPECT_TRUE(polygonizer.getInvalidRingLines().empty());
  EXPECT_TRUE(polygonizer.allInputsFormPolygons());
}

TEST(PolygonizerTest, NonLinearAndEmptyGeometriesProduceNoPolygons) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  auto pt = reader.read("POINT (1 2)");
  auto mpt = reader.read("MULTIPOINT ((3 4), (5 6))");
  auto empty_gc = reader.read("GEOMETRYCOLLECTION EMPTY");

  Polygonizer no_graph_poly;
  no_graph_poly.add(pt.get());
  no_graph_poly.add(mpt.get());
  no_graph_poly.add(empty_gc.get());

  EXPECT_TRUE(no_graph_poly.getPolygons().empty());
  EXPECT_FALSE(no_graph_poly.hasDangles());
  EXPECT_FALSE(no_graph_poly.hasCutEdges());
  EXPECT_FALSE(no_graph_poly.hasInvalidRingLines());
  EXPECT_TRUE(no_graph_poly.allInputsFormPolygons());

  // Adding an empty LineString creates the graph without adding edges.
  auto empty_ls = reader.read("LINESTRING EMPTY");
  Polygonizer empty_line_poly;
  empty_line_poly.add(empty_ls.get());
  EXPECT_TRUE(empty_line_poly.getPolygons().empty());
  EXPECT_TRUE(empty_line_poly.allInputsFormPolygons());
}

TEST(PolygonizerTest, SinglePolygonFromSeparateUnclosedLineStrings) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  auto e1 = reader.read("LINESTRING (0 0, 10 0)");
  auto e2 = reader.read("LINESTRING (10 0, 10 10)");
  auto e3 = reader.read("LINESTRING (0 10, 10 10)");  // reversed direction
  auto e4 = reader.read("LINESTRING (0 10, 0 0)");
  auto expected =
      reader.read<Polygon>("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");

  Polygonizer polygonizer;
  polygonizer.add(e1.get());
  polygonizer.add(e2.get());
  polygonizer.add(e3.get());
  polygonizer.add(e4.get());

  EXPECT_TRUE(polygonizer.allInputsFormPolygons());
  EXPECT_FALSE(polygonizer.hasDangles());
  EXPECT_FALSE(polygonizer.hasCutEdges());
  EXPECT_FALSE(polygonizer.hasInvalidRingLines());

  auto polys = polygonizer.getPolygons();
  ASSERT_EQ(polys.size(), 1u);
  EXPECT_TRUE(polys[0]->isValid());
  EXPECT_EQ(polys[0]->getNumInteriorRing(), 0u);
  EXPECT_DOUBLE_EQ(polys[0]->getArea(), 100.0);
  EXPECT_TRUE(polys[0]->equals(expected.get()));

  // Subsequent calls hit the already-computed path; polygon ownership was
  // moved.
  EXPECT_TRUE(polygonizer.getPolygons().empty());
  EXPECT_TRUE(polygonizer.allInputsFormPolygons());
  EXPECT_TRUE(polygonizer.getDangles().empty());
  EXPECT_TRUE(polygonizer.getCutEdges().empty());
  EXPECT_TRUE(polygonizer.getInvalidRingLines().empty());
}

TEST(PolygonizerTest, AddVectorOverloadsSupportMutableAndConstGeometryVectors) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  auto t1_a = reader.read("LINESTRING (0 0, 4 0, 2 3)");
  auto t1_b = reader.read("LINESTRING (2 3, 0 0)");
  auto t2_a = reader.read("LINESTRING (10 0, 16 0)");
  auto t2_b = reader.read("LINESTRING (16 0, 13 4, 10 0)");

  Polygonizer polygonizer;

  std::vector<Geometry*> empty_mut;
  polygonizer.add(&empty_mut);

  std::vector<Geometry*> mut_geoms = {t1_a.get(), t1_b.get()};
  polygonizer.add(&mut_geoms);

  std::vector<const Geometry*> empty_const;
  polygonizer.add(&empty_const);

  std::vector<const Geometry*> const_geoms = {t2_a.get(), t2_b.get()};
  polygonizer.add(&const_geoms);

  EXPECT_TRUE(polygonizer.allInputsFormPolygons());
  auto polys = polygonizer.getPolygons();
  ASSERT_EQ(polys.size(), 2u);
  EXPECT_THAT(SortedAreas(polys), testing::ElementsAre(6.0, 12.0));
}

TEST(PolygonizerTest, MixedGeometryTypesExtractLineworkAndIgnorePoints) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  auto mls =
      reader.read("MULTILINESTRING ((0 0, 10 0, 10 10), (10 10, 0 10, 0 0))");
  auto poly = reader.read(
      "POLYGON ((20 0, 30 0, 30 10, 20 10, 20 0), "
      "(22 2, 28 2, 28 8, 22 8, 22 2))");
  auto gc = reader.read(
      "GEOMETRYCOLLECTION (POINT (99 99), LINESTRING (40 0, 50 0, 50 10))");
  auto gc_close = reader.read("LINESTRING (50 10, 40 10, 40 0)");
  CoordinateSequence ring_coords{CoordinateXY{60, 0}, CoordinateXY{64, 0},
                                 CoordinateXY{64, 5}, CoordinateXY{60, 5},
                                 CoordinateXY{60, 0}};
  auto ring = factory->createLinearRing(ring_coords);

  Polygonizer polygonizer;
  polygonizer.add(mls.get());
  polygonizer.add(poly.get());
  polygonizer.add(gc.get());
  polygonizer.add(gc_close.get());
  // Polygonizer::add(const LineString*) is private; dispatch via Geometry*.
  polygonizer.add(static_cast<const Geometry*>(ring.get()));

  EXPECT_TRUE(polygonizer.allInputsFormPolygons());
  auto polys = polygonizer.getPolygons();
  // [0,10]^2 (100), [20,30]x[0,10] with hole (64), inner hole filled (36),
  // [40,50]x[0,10] (100), [60,64]x[0,5] (20).
  ASSERT_EQ(polys.size(), 5u);
  EXPECT_THAT(SortedAreas(polys),
              testing::ElementsAre(20.0, 36.0, 64.0, 100.0, 100.0));
}

TEST(PolygonizerTest, CurvedGeometryThrowsUnsupportedOperationException) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  auto circular = reader.read("CIRCULARSTRING (0 0, 1 1, 2 0)");
  auto curve_poly = reader.read(
      "CURVEPOLYGON (COMPOUNDCURVE (CIRCULARSTRING (0 0, 1 1, 2 0), (2 0, 0 "
      "0)))");

  Polygonizer polygonizer;
  EXPECT_THROW(polygonizer.add(circular.get()),
               geos::util::UnsupportedOperationException);
  EXPECT_THROW(polygonizer.add(curve_poly.get()),
               geos::util::UnsupportedOperationException);
}

TEST(PolygonizerTest, NestedRingsDefaultModeExtractsAllShellsAndHoles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  auto outer = reader.read("LINESTRING (0 0, 100 0, 100 100, 0 100, 0 0)");
  auto hole1 = reader.read("LINESTRING (10 10, 40 10, 40 40, 10 40, 10 10)");
  auto hole2 = reader.read("LINESTRING (50 10, 90 10, 90 40, 50 40, 50 10)");
  auto island = reader.read("LINESTRING (20 20, 30 20, 30 30, 20 30, 20 20)");
  auto inner_hole =
      reader.read("LINESTRING (23 23, 27 23, 27 27, 23 27, 23 23)");

  Polygonizer polygonizer(/*onlyPolygonal=*/false);
  polygonizer.add(outer.get());
  polygonizer.add(hole1.get());
  polygonizer.add(hole2.get());
  polygonizer.add(island.get());
  polygonizer.add(inner_hole.get());

  EXPECT_TRUE(polygonizer.allInputsFormPolygons());
  auto polys = polygonizer.getPolygons();
  ASSERT_EQ(polys.size(), 5u);
  for (const auto& poly : polys) {
    EXPECT_TRUE(poly->isValid());
  }
  // Areas: inner_hole=16, island=100-16=84, hole1=900-100=800,
  // hole2=1200, outer=10000-900-1200=7900.
  EXPECT_THAT(SortedAreas(polys),
              testing::ElementsAre(16.0, 84.0, 800.0, 1200.0, 7900.0));
}

TEST(PolygonizerTest, OnlyPolygonalExtractsNonOverlappingNestedPolygons) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  auto outer = reader.read("LINESTRING (0 0, 100 0, 100 100, 0 100, 0 0)");
  auto hole1 = reader.read("LINESTRING (10 10, 40 10, 40 40, 10 40, 10 10)");
  auto hole2 = reader.read("LINESTRING (50 10, 90 10, 90 40, 50 40, 50 10)");
  auto island = reader.read("LINESTRING (20 20, 30 20, 30 30, 20 30, 20 20)");
  auto inner_hole =
      reader.read("LINESTRING (23 23, 27 23, 27 27, 23 27, 23 23)");

  Polygonizer polygonizer(/*onlyPolygonal=*/true);
  polygonizer.add(outer.get());
  polygonizer.add(hole1.get());
  polygonizer.add(hole2.get());
  polygonizer.add(island.get());
  polygonizer.add(inner_hole.get());

  EXPECT_TRUE(polygonizer.allInputsFormPolygons());
  auto polys = polygonizer.getPolygons();
  ASSERT_EQ(polys.size(), 2u);
  for (const auto& poly : polys) {
    EXPECT_TRUE(poly->isValid());
  }
  EXPECT_THAT(SortedAreas(polys), testing::ElementsAre(84.0, 7900.0));
  EXPECT_TRUE(polys[0]->disjoint(polys[1].get()));
}

TEST(PolygonizerTest,
     OnlyPolygonalWithEdgeSharingSquaresSelectsDisjointShells) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Two adjacent squares sharing edge (10,0)-(10,10).
  auto left_loop = reader.read("LINESTRING (10 10, 0 10, 0 0, 10 0)");
  auto shared_edge = reader.read("LINESTRING (10 0, 10 10)");
  auto right_loop = reader.read("LINESTRING (10 0, 20 0, 20 10, 10 10)");

  Polygonizer default_poly(/*onlyPolygonal=*/false);
  default_poly.add(left_loop.get());
  default_poly.add(shared_edge.get());
  default_poly.add(right_loop.get());
  EXPECT_EQ(default_poly.getPolygons().size(), 2u);

  Polygonizer only_poly(/*onlyPolygonal=*/true);
  only_poly.add(left_loop.get());
  only_poly.add(shared_edge.get());
  only_poly.add(right_loop.get());
  EXPECT_TRUE(only_poly.allInputsFormPolygons());
  auto polys = only_poly.getPolygons();
  ASSERT_EQ(polys.size(), 1u);
  EXPECT_DOUBLE_EQ(polys[0]->getArea(), 100.0);

  // Three adjacent squares in a row exercise multi-step recursive inclusion.
  auto s1 = reader.read("LINESTRING (10 10, 0 10, 0 0, 10 0)");
  auto e12 = reader.read("LINESTRING (10 0, 10 10)");
  auto s2_b = reader.read("LINESTRING (10 0, 20 0)");
  auto s2_t = reader.read("LINESTRING (20 10, 10 10)");
  auto e23 = reader.read("LINESTRING (20 0, 20 10)");
  auto s3 = reader.read("LINESTRING (20 0, 30 0, 30 10, 20 10)");

  Polygonizer chain_poly(/*onlyPolygonal=*/true);
  chain_poly.add(s1.get());
  chain_poly.add(e12.get());
  chain_poly.add(s2_b.get());
  chain_poly.add(s2_t.get());
  chain_poly.add(e23.get());
  chain_poly.add(s3.get());
  auto chain_result = chain_poly.getPolygons();
  EXPECT_GE(chain_result.size(), 1u);
  EXPECT_LE(chain_result.size(), 2u);
  for (std::size_t i = 0; i < chain_result.size(); ++i) {
    EXPECT_DOUBLE_EQ(chain_result[i]->getArea(), 100.0);
    for (std::size_t j = i + 1; j < chain_result.size(); ++j) {
      auto im = chain_result[i]->relate(chain_result[j].get());
      EXPECT_NE(im->get(geos::geom::Location::BOUNDARY,
                        geos::geom::Location::BOUNDARY),
                static_cast<int>(geos::geom::Dimension::L));
    }
  }
}

TEST(PolygonizerTest, DanglesAreDetectedAndExcludedFromPolygons) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  auto sq_a = reader.read("LINESTRING (0 0, 10 0, 10 5)");
  auto sq_b = reader.read("LINESTRING (10 5, 10 10, 0 10, 0 0)");
  auto dangle_stem = reader.read<LineString>("LINESTRING (10 5, 15 5)");
  auto dangle_branch1 = reader.read<LineString>("LINESTRING (15 5, 18 8)");
  auto dangle_branch2 = reader.read<LineString>("LINESTRING (15 5, 18 2)");
  auto isolated_dangle = reader.read<LineString>("LINESTRING (25 0, 30 0)");

  Polygonizer polygonizer;
  polygonizer.add(sq_a.get());
  polygonizer.add(sq_b.get());
  polygonizer.add(static_cast<const Geometry*>(dangle_stem.get()));
  polygonizer.add(static_cast<const Geometry*>(dangle_branch1.get()));
  polygonizer.add(static_cast<const Geometry*>(dangle_branch2.get()));
  polygonizer.add(static_cast<const Geometry*>(isolated_dangle.get()));

  EXPECT_TRUE(polygonizer.hasDangles());
  EXPECT_FALSE(polygonizer.hasCutEdges());
  EXPECT_FALSE(polygonizer.hasInvalidRingLines());
  EXPECT_FALSE(polygonizer.allInputsFormPolygons());

  const auto& dangles = polygonizer.getDangles();
  ASSERT_EQ(dangles.size(), 4u);
  EXPECT_THAT(dangles, testing::UnorderedElementsAre(
                           dangle_stem.get(), dangle_branch1.get(),
                           dangle_branch2.get(), isolated_dangle.get()));

  auto polys = polygonizer.getPolygons();
  ASSERT_EQ(polys.size(), 1u);
  EXPECT_DOUBLE_EQ(polys[0]->getArea(), 100.0);
}

TEST(PolygonizerTest, CutEdgesBetweenPolygonsAndShellToHoleAreReported) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Bridge between two squares.
  auto left_a = reader.read("LINESTRING (0 0, 10 0, 10 5)");
  auto left_b = reader.read("LINESTRING (10 5, 10 10, 0 10, 0 0)");
  auto right_a =
      reader.read("LINESTRING (20 5, 20 0, 30 0, 30 10, 20 10, 20 5)");
  auto bridge = reader.read<LineString>("LINESTRING (10 5, 20 5)");

  Polygonizer p1;
  p1.add(left_a.get());
  p1.add(left_b.get());
  p1.add(right_a.get());
  p1.add(static_cast<const Geometry*>(bridge.get()));

  EXPECT_FALSE(p1.hasDangles());
  EXPECT_TRUE(p1.hasCutEdges());
  EXPECT_FALSE(p1.hasInvalidRingLines());
  EXPECT_FALSE(p1.allInputsFormPolygons());
  ASSERT_EQ(p1.getCutEdges().size(), 1u);
  EXPECT_EQ(p1.getCutEdges()[0], bridge.get());
  auto polys1 = p1.getPolygons();
  ASSERT_EQ(polys1.size(), 2u);
  EXPECT_THAT(SortedAreas(polys1), testing::ElementsAre(100.0, 100.0));

  // Cut edge connecting an outer shell to an interior hole ring.
  auto outer_a = reader.read("LINESTRING (0 10, 0 0, 20 0, 20 20, 0 20, 0 10)");
  auto inner_a = reader.read("LINESTRING (5 10, 5 5, 15 5, 15 15, 5 15, 5 10)");
  auto inner_bridge = reader.read<LineString>("LINESTRING (0 10, 5 10)");

  Polygonizer p2;
  p2.add(outer_a.get());
  p2.add(inner_a.get());
  p2.add(static_cast<const Geometry*>(inner_bridge.get()));

  EXPECT_FALSE(p2.hasDangles());
  EXPECT_TRUE(p2.hasCutEdges());
  EXPECT_FALSE(p2.hasInvalidRingLines());
  EXPECT_FALSE(p2.allInputsFormPolygons());
  ASSERT_EQ(p2.getCutEdges().size(), 1u);
  EXPECT_EQ(p2.getCutEdges()[0], inner_bridge.get());
  auto polys2 = p2.getPolygons();
  ASSERT_EQ(polys2.size(), 2u);
  EXPECT_THAT(SortedAreas(polys2), testing::ElementsAre(100.0, 300.0));
}

TEST(PolygonizerTest, InvalidRingLinesAreReportedAndDeduplicated) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // A single self-intersecting closed bowtie forms two invalid rings sharing
  // the same linework; extractInvalidLines deduplicates them to one line.
  auto bowtie = reader.read("LINESTRING (0 0, 10 10, 10 0, 0 10, 0 0)");
  Polygonizer p_single;
  p_single.add(bowtie.get());

  EXPECT_FALSE(p_single.hasDangles());
  EXPECT_FALSE(p_single.hasCutEdges());
  EXPECT_TRUE(p_single.hasInvalidRingLines());
  EXPECT_FALSE(p_single.allInputsFormPolygons());
  ASSERT_EQ(p_single.getInvalidRingLines().size(), 1u);
  EXPECT_TRUE(p_single.getInvalidRingLines()[0]->isClosed());
  EXPECT_TRUE(p_single.getPolygons().empty());

  // Two bowties with distinct envelope areas exercise envelope-area sorting.
  auto small_bowtie = reader.read("LINESTRING (20 0, 22 2, 22 0, 20 2, 20 0)");
  Polygonizer p_two;
  p_two.add(bowtie.get());
  p_two.add(small_bowtie.get());
  EXPECT_TRUE(p_two.hasInvalidRingLines());
  EXPECT_EQ(p_two.getInvalidRingLines().size(), 2u);
  EXPECT_TRUE(p_two.getPolygons().empty());

  // Valid outer square plus crossing chord forming one valid ring and two
  // invalid rings; tests isIncludedInvalid with both valid and processed adj.
  auto outer_box =
      reader.read("LINESTRING (-5 -5, 15 -5, 15 15, -5 15, -5 -5)");
  auto sq_1 = reader.read("LINESTRING (0 0, 10 0, 10 10)");
  auto sq_2 = reader.read("LINESTRING (10 10, 0 10, 0 0)");
  auto crossing = reader.read("LINESTRING (0 0, 12 5, -2 5, 10 10)");

  Polygonizer p_mixed;
  p_mixed.add(outer_box.get());
  p_mixed.add(sq_1.get());
  p_mixed.add(sq_2.get());
  p_mixed.add(crossing.get());

  EXPECT_FALSE(p_mixed.hasDangles());
  EXPECT_FALSE(p_mixed.hasCutEdges());
  EXPECT_TRUE(p_mixed.hasInvalidRingLines());
  EXPECT_FALSE(p_mixed.allInputsFormPolygons());
  EXPECT_EQ(p_mixed.getInvalidRingLines().size(), 1u);

  auto mixed_polys = p_mixed.getPolygons();
  ASSERT_EQ(mixed_polys.size(), 1u);
  EXPECT_TRUE(mixed_polys[0]->isValid());
  EXPECT_EQ(mixed_polys[0]->getNumInteriorRing(), 1u);
  EXPECT_DOUBLE_EQ(mixed_polys[0]->getArea(), 300.0);
}

TEST(PolygonizerTest, InterruptDuringPolygonizeThrowsGEOSException) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  auto square = reader.read("LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  Polygonizer polygonizer;
  polygonizer.add(square.get());

  absl::Cleanup cancel_interrupt([] { geos::util::Interrupt::cancel(); });
  geos::util::Interrupt::request();

  EXPECT_THROW(polygonizer.getPolygons(), geos::util::GEOSException);
}

TEST(PolygonizerTest, EmptyOnlyPolygonalAndRepeatedPointLineStrings) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  Polygonizer only_poly_empty(/*onlyPolygonal=*/true);
  EXPECT_TRUE(only_poly_empty.getPolygons().empty());
  EXPECT_TRUE(only_poly_empty.allInputsFormPolygons());

  // Calling getPolygons() with no graph does not freeze `computed`.
  auto square = reader.read("LINESTRING (0 0, 5 0, 5 5, 0 5, 0 0)");
  only_poly_empty.add(square.get());
  auto polys = only_poly_empty.getPolygons();
  ASSERT_EQ(polys.size(), 1u);
  EXPECT_DOUBLE_EQ(polys[0]->getArea(), 25.0);

  auto repeated_only = reader.read("LINESTRING (1 2, 1 2, 1 2)");
  Polygonizer dup_poly;
  dup_poly.add(repeated_only.get());
  EXPECT_TRUE(dup_poly.getPolygons().empty());
  EXPECT_TRUE(dup_poly.allInputsFormPolygons());
  EXPECT_FALSE(dup_poly.hasDangles());
  EXPECT_FALSE(dup_poly.hasCutEdges());
  EXPECT_FALSE(dup_poly.hasInvalidRingLines());
}

TEST(PolygonizerTest, OnlyPolygonalDisconnectedShellsAndGetPolygonsFirst) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  auto left_sq = reader.read("LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  auto right_a = reader.read("LINESTRING (20 0, 30 0, 30 5)");
  auto right_b = reader.read("LINESTRING (30 5, 30 10, 20 10, 20 0)");
  auto dangle = reader.read<LineString>("LINESTRING (30 5, 35 5)");

  Polygonizer polygonizer(/*onlyPolygonal=*/true);
  polygonizer.add(left_sq.get());
  polygonizer.add(right_a.get());
  polygonizer.add(right_b.get());
  polygonizer.add(static_cast<const Geometry*>(dangle.get()));

  // Call getPolygons() before inspecting error predicates.
  auto polys = polygonizer.getPolygons();
  ASSERT_EQ(polys.size(), 2u);
  EXPECT_TRUE(polys[0]->isValid());
  EXPECT_TRUE(polys[1]->isValid());
  EXPECT_TRUE(polys[0]->disjoint(polys[1].get()));
  EXPECT_THAT(SortedAreas(polys), testing::ElementsAre(100.0, 100.0));

  EXPECT_TRUE(polygonizer.hasDangles());
  EXPECT_THAT(polygonizer.getDangles(), testing::ElementsAre(dangle.get()));
  EXPECT_FALSE(polygonizer.hasCutEdges());
  EXPECT_FALSE(polygonizer.hasInvalidRingLines());
  EXPECT_FALSE(polygonizer.allInputsFormPolygons());
}

TEST(PolygonizerTest,
     InvalidRingSortingDiscardsOuterInvalidRingByEnvelopeArea) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // e1 + e2 form a self-crossing inner ring (envelope [0,10]x[0,10], area 100);
  // e2 + e3 form a valid trapezoid (area 75);
  // e3 + e1 form a larger outer invalid ring (envelope [-5,15]x[-5,10], area
  // 300).
  auto e1 = reader.read("LINESTRING (0 0, 10 10, 0 10, 10 0)");
  auto e2 = reader.read("LINESTRING (10 0, 0 0)");
  auto e3 = reader.read("LINESTRING (0 0, -5 -5, 15 -5, 10 0)");

  Polygonizer polygonizer;
  polygonizer.add(e1.get());
  polygonizer.add(e2.get());
  polygonizer.add(e3.get());

  EXPECT_FALSE(polygonizer.hasDangles());
  EXPECT_FALSE(polygonizer.hasCutEdges());
  EXPECT_TRUE(polygonizer.hasInvalidRingLines());
  EXPECT_FALSE(polygonizer.allInputsFormPolygons());

  const auto& invalid_lines = polygonizer.getInvalidRingLines();
  ASSERT_EQ(invalid_lines.size(), 1u);
  EXPECT_DOUBLE_EQ(invalid_lines[0]->getEnvelopeInternal()->getArea(), 100.0);

  auto polys = polygonizer.getPolygons();
  ASSERT_EQ(polys.size(), 1u);
  EXPECT_TRUE(polys[0]->isValid());
  EXPECT_DOUBLE_EQ(polys[0]->getArea(), 75.0);
}

TEST(PolygonizerTest,
     CurrentThreadInterruptDuringPolygonizeThrowsGEOSException) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  auto square = reader.read("LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  Polygonizer polygonizer;
  polygonizer.add(square.get());

  struct InterruptState {
    int remaining_checks = 3;
  } state;
  auto* prev_cb = geos::util::CurrentThreadInterrupt::registerCallback(
      [](void* data) -> int {
        auto* st = static_cast<InterruptState*>(data);
        return (--st->remaining_checks <= 0) ? 1 : 0;
      },
      &state);
  absl::Cleanup restore_cb([prev_cb] {
    geos::util::CurrentThreadInterrupt::registerCallback(prev_cb, nullptr);
  });

  EXPECT_THROW(polygonizer.getPolygons(), geos::util::GEOSException);
  EXPECT_LE(state.remaining_checks, 0);
}

}  // namespace
}  // namespace geos_google
