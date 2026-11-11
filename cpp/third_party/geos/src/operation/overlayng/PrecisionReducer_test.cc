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

// Unit tests for third_party/geos/src/operation/overlayng/PrecisionReducer.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/overlayng/PrecisionReducer.cpp

#include "third_party/geos/include/geos/operation/overlayng/PrecisionReducer.h"

#include <cmath>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/io/WKTWriter.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::io::WKTWriter;
using geos::operation::overlayng::PrecisionReducer;

void ExpectNormalizedExact(const Geometry* actual,
                           const std::string& expected_wkt,
                           const GeometryFactory* factory) {
  ASSERT_NE(actual, nullptr);
  WKTReader reader(*factory);
  std::unique_ptr<Geometry> expected = reader.read(expected_wkt);
  std::unique_ptr<Geometry> actual_norm = actual->clone();
  actual_norm->normalize();
  expected->normalize();

  WKTWriter writer;
  EXPECT_TRUE(actual_norm->equalsExact(expected.get()))
      << "actual:   " << writer.write(actual_norm.get()) << "\n"
      << "expected: " << writer.write(expected.get());
}

TEST(PrecisionReducerTest, DefaultConstructorAndSinglePointReturnsEmpty) {
  PrecisionReducer reducer;
  (void)reducer;

  PrecisionModel floating_pm(PrecisionModel::FLOATING);
  GeometryFactory::Ptr gf = GeometryFactory::create(&floating_pm, 4326);
  WKTReader reader(*gf);
  std::unique_ptr<Geometry> pt = reader.read("POINT (1.24 2.76)");

  PrecisionModel pm(10.0);
  // Single-geometry OverlayNG(..., nullptr, ..., UNION) routes through
  // computeEdgeOverlay(), which ignores puntal geometries and yields an empty
  // 0-dimensional result.
  std::unique_ptr<Geometry> reduced_keep =
      PrecisionReducer::reducePrecision(pt.get(), &pm);
  ExpectNormalizedExact(reduced_keep.get(), "POINT EMPTY", gf.get());
  EXPECT_TRUE(reduced_keep->isEmpty());
  EXPECT_EQ(reduced_keep->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_EQ(reduced_keep->getDimension(), 0);
  EXPECT_EQ(reduced_keep->getSRID(), 4326);
  EXPECT_TRUE(reduced_keep->getPrecisionModel()->isFloating());
  EXPECT_EQ(reduced_keep->getFactory(), gf.get());

  std::unique_ptr<Geometry> reduced_replace =
      PrecisionReducer::reducePrecision(pt.get(), &pm, true);
  ExpectNormalizedExact(reduced_replace.get(), "POINT EMPTY",
                        reduced_replace->getFactory());
  EXPECT_TRUE(reduced_replace->isEmpty());
  EXPECT_EQ(reduced_replace->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_EQ(reduced_replace->getDimension(), 0);
  EXPECT_EQ(reduced_replace->getSRID(), 4326);
  EXPECT_NE(reduced_replace->getFactory(), gf.get());
  EXPECT_EQ(reduced_replace->getPrecisionModel()->getType(),
            PrecisionModel::FIXED);
  EXPECT_DOUBLE_EQ(reduced_replace->getPrecisionModel()->getScale(), 10.0);
}

TEST(PrecisionReducerTest, MultiPointReturnsPointEmpty) {
  GeometryFactory::Ptr gf = GeometryFactory::create();
  WKTReader reader(*gf);
  std::unique_ptr<Geometry> mpt =
      reader.read("MULTIPOINT ((1.24 2.76), (1.26 2.74), (5.01 6.49))");

  PrecisionModel pm_tenth(10.0);
  std::unique_ptr<Geometry> reduced_tenth =
      PrecisionReducer::reducePrecision(mpt.get(), &pm_tenth, false);
  ExpectNormalizedExact(reduced_tenth.get(), "POINT EMPTY", gf.get());
  EXPECT_TRUE(reduced_tenth->isEmpty());
  EXPECT_EQ(reduced_tenth->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_EQ(reduced_tenth->getDimension(), 0);
  EXPECT_TRUE(reduced_tenth->getPrecisionModel()->isFloating());

  PrecisionModel pm_int(1.0);
  std::unique_ptr<Geometry> reduced_int =
      PrecisionReducer::reducePrecision(mpt.get(), &pm_int, true);
  ExpectNormalizedExact(reduced_int.get(), "POINT EMPTY",
                        reduced_int->getFactory());
  EXPECT_TRUE(reduced_int->isEmpty());
  EXPECT_EQ(reduced_int->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_EQ(reduced_int->getPrecisionModel()->getType(), PrecisionModel::FIXED);
  EXPECT_DOUBLE_EQ(reduced_int->getPrecisionModel()->getScale(), 1.0);
}

TEST(PrecisionReducerTest, ReducesAndNodesCrossingAndSelfIntersectingLines) {
  GeometryFactory::Ptr gf = GeometryFactory::create();
  WKTReader reader(*gf);
  PrecisionModel pm(1.0);

  // Two crossing lines node at (5, 5) into four segments.
  std::unique_ptr<Geometry> crossing = reader.read(
      "MULTILINESTRING ((0.04 0.04, 10.04 10.04), (10.04 0.04, 0.04 10.04))");
  std::unique_ptr<Geometry> reduced_crossing =
      PrecisionReducer::reducePrecision(crossing.get(), &pm);

  ExpectNormalizedExact(
      reduced_crossing.get(),
      "MULTILINESTRING ((0 0, 5 5), (0 10, 5 5), (5 5, 10 0), (5 5, 10 10))",
      gf.get());
  EXPECT_TRUE(reduced_crossing->isValid());
  EXPECT_TRUE(reduced_crossing->getPrecisionModel()->isFloating());
  EXPECT_EQ(reduced_crossing->getFactory(), gf.get());

  // Single self-intersecting linestring is split at (5, 5) into 3 components:
  // (0 0, 5 5), the closed loop (5 5, 10 10, 10 0, 5 5), and (5 5, 0 10).
  std::unique_ptr<Geometry> self_xing = reader.read(
      "LINESTRING (0.04 0.04, 10.04 10.04, 10.04 0.04, 0.04 10.04)");
  std::unique_ptr<Geometry> reduced_self_xing =
      PrecisionReducer::reducePrecision(self_xing.get(), &pm);

  EXPECT_TRUE(reduced_self_xing->isValid());
  EXPECT_EQ(reduced_self_xing->getNumGeometries(), 3u);
  EXPECT_NEAR(reduced_self_xing->getLength(), 10.0 + 20.0 * std::sqrt(2.0),
              1e-12);

  // Nearly coincident lines merge into a single LineString.
  std::unique_ptr<Geometry> parallel =
      reader.read("MULTILINESTRING ((0 0, 10 0), (0 0.1, 10 0.1))");
  std::unique_ptr<Geometry> merged_lines =
      PrecisionReducer::reducePrecision(parallel.get(), &pm, false);
  ExpectNormalizedExact(merged_lines.get(), "LINESTRING (0 0, 10 0)", gf.get());
  EXPECT_EQ(merged_lines->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
}

TEST(PrecisionReducerTest, ReplacesPrecisionModelOnLineStringAndPreservesSrid) {
  PrecisionModel floating_pm(PrecisionModel::FLOATING);
  GeometryFactory::Ptr gf = GeometryFactory::create(&floating_pm, 4326);
  WKTReader reader(*gf);
  std::unique_ptr<Geometry> line =
      reader.read("LINESTRING (1.234 5.678, 3.456 7.891)");

  PrecisionModel fixed_pm(10.0);
  std::unique_ptr<Geometry> reduced =
      PrecisionReducer::reducePrecision(line.get(), &fixed_pm, true);

  ASSERT_NE(reduced, nullptr);
  EXPECT_EQ(reduced->getSRID(), 4326);
  EXPECT_NE(reduced->getFactory(), gf.get());
  EXPECT_EQ(reduced->getPrecisionModel()->getType(), PrecisionModel::FIXED);
  EXPECT_DOUBLE_EQ(reduced->getPrecisionModel()->getScale(), 10.0);
  ExpectNormalizedExact(reduced.get(), "LINESTRING (1.2 5.7, 3.5 7.9)",
                        reduced->getFactory());
}

TEST(PrecisionReducerTest, NegativeScaleGridSizeRoundsToCoarseGrid) {
  PrecisionModel floating_pm(PrecisionModel::FLOATING);
  GeometryFactory::Ptr gf = GeometryFactory::create(&floating_pm, 32610);
  WKTReader reader(*gf);
  std::unique_ptr<Geometry> line = reader.read("LINESTRING (4 6, 16 24)");

  // Negative scale specifies grid size 10.0 (scale = 0.1).
  PrecisionModel coarse_pm(-10.0);
  std::unique_ptr<Geometry> keep =
      PrecisionReducer::reducePrecision(line.get(), &coarse_pm, false);
  ExpectNormalizedExact(keep.get(), "LINESTRING (0 10, 20 20)", gf.get());
  EXPECT_TRUE(keep->getPrecisionModel()->isFloating());

  std::unique_ptr<Geometry> replaced =
      PrecisionReducer::reducePrecision(line.get(), &coarse_pm, true);
  ExpectNormalizedExact(replaced.get(), "LINESTRING (0 10, 20 20)",
                        replaced->getFactory());
  EXPECT_EQ(replaced->getSRID(), 32610);
  EXPECT_EQ(replaced->getPrecisionModel()->getType(), PrecisionModel::FIXED);
  EXPECT_DOUBLE_EQ(replaced->getPrecisionModel()->getGridSize(), 10.0);
  EXPECT_DOUBLE_EQ(replaced->getPrecisionModel()->getScale(), 0.1);
}

TEST(PrecisionReducerTest, PolygonDropsCollapseLinesWithoutReplacingModel) {
  GeometryFactory::Ptr gf = GeometryFactory::create();
  WKTReader reader(*gf);

  // Spike at y = 4.0..4.2 collapses to a line segment when rounded to scale 1.
  // The collapsed line is omitted because area-only mode is enabled for
  // dimension 2, leaving only the noded boundary vertex (10, 4).
  std::unique_ptr<Geometry> spiked = reader.read(
      "POLYGON ((0 0, 10 0, 10 4, 20 4.1, 10 4.2, 10 10, 0 10, 0 0))");
  PrecisionModel pm(1.0);
  std::unique_ptr<Geometry> reduced_spiked =
      PrecisionReducer::reducePrecision(spiked.get(), &pm, false);

  EXPECT_TRUE(reduced_spiked->isValid());
  EXPECT_EQ(reduced_spiked->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_EQ(reduced_spiked->getDimension(), 2);
  EXPECT_TRUE(reduced_spiked->getPrecisionModel()->isFloating());
  ExpectNormalizedExact(reduced_spiked.get(),
                        "POLYGON ((0 0, 10 0, 10 4, 10 10, 0 10, 0 0))",
                        gf.get());
  std::unique_ptr<Geometry> expected_topo =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  EXPECT_TRUE(reduced_spiked->equals(expected_topo.get()));

  // Thin sliver collapses completely to an empty polygon.
  std::unique_ptr<Geometry> sliver =
      reader.read("POLYGON ((0 0, 10 0, 10 0.2, 0 0.2, 0 0))");
  std::unique_ptr<Geometry> reduced_sliver =
      PrecisionReducer::reducePrecision(sliver.get(), &pm, false);

  ASSERT_NE(reduced_sliver, nullptr);
  EXPECT_TRUE(reduced_sliver->isEmpty());
  EXPECT_EQ(reduced_sliver->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_EQ(reduced_sliver->getDimension(), 2);
  EXPECT_TRUE(reduced_sliver->getPrecisionModel()->isFloating());

  // Narrow bridge collapses and splits polygon into two valid polygons.
  std::unique_ptr<Geometry> pinched = reader.read(
      "POLYGON ((0 0, 4 0, 4 4, 6 4, 6 0, 10 0, 10 10, 6 10, 6 4.2, 4 4.2, "
      "4 10, 0 10, 0 0))");
  std::unique_ptr<Geometry> split =
      PrecisionReducer::reducePrecision(pinched.get(), &pm, false);
  EXPECT_TRUE(split->isValid());
  EXPECT_EQ(split->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(split->getNumGeometries(), 2u);
  EXPECT_DOUBLE_EQ(split->getArea(), 80.0);
  ExpectNormalizedExact(split.get(),
                        "MULTIPOLYGON (((0 0, 4 0, 4 4, 4 10, 0 10, 0 0)), "
                        "((6 0, 10 0, 10 10, 6 10, 6 4, 6 0)))",
                        gf.get());
}

TEST(PrecisionReducerTest,
     PolygonDropsCollapseLinesAndReplacesPrecisionModelAndPreservesSrid) {
  PrecisionModel floating_pm(PrecisionModel::FLOATING);
  GeometryFactory::Ptr gf = GeometryFactory::create(&floating_pm, 3857);
  WKTReader reader(*gf);

  std::unique_ptr<Geometry> spiked = reader.read(
      "POLYGON ((0 0, 10 0, 10 4, 20 4.1, 10 4.2, 10 10, 0 10, 0 0))");
  PrecisionModel pm(1.0);
  std::unique_ptr<Geometry> reduced_spiked =
      PrecisionReducer::reducePrecision(spiked.get(), &pm, true);

  ASSERT_NE(reduced_spiked, nullptr);
  EXPECT_TRUE(reduced_spiked->isValid());
  EXPECT_EQ(reduced_spiked->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_EQ(reduced_spiked->getDimension(), 2);
  EXPECT_EQ(reduced_spiked->getSRID(), 3857);
  EXPECT_NE(reduced_spiked->getFactory(), gf.get());
  EXPECT_EQ(reduced_spiked->getPrecisionModel()->getType(),
            PrecisionModel::FIXED);
  EXPECT_DOUBLE_EQ(reduced_spiked->getPrecisionModel()->getScale(), 1.0);
  ExpectNormalizedExact(reduced_spiked.get(),
                        "POLYGON ((0 0, 10 0, 10 4, 10 10, 0 10, 0 0))",
                        reduced_spiked->getFactory());
  std::unique_ptr<Geometry> expected_topo =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  EXPECT_TRUE(reduced_spiked->equals(expected_topo.get()));

  // Total sliver collapse while replacing precision model and preserving SRID.
  std::unique_ptr<Geometry> sliver =
      reader.read("POLYGON ((0 0, 10 0, 10 0.1, 0 0.1, 0 0))");
  std::unique_ptr<Geometry> reduced_sliver =
      PrecisionReducer::reducePrecision(sliver.get(), &pm, true);

  ASSERT_NE(reduced_sliver, nullptr);
  EXPECT_TRUE(reduced_sliver->isEmpty());
  EXPECT_EQ(reduced_sliver->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_EQ(reduced_sliver->getDimension(), 2);
  EXPECT_EQ(reduced_sliver->getSRID(), 3857);
  EXPECT_EQ(reduced_sliver->getPrecisionModel()->getType(),
            PrecisionModel::FIXED);
  EXPECT_DOUBLE_EQ(reduced_sliver->getPrecisionModel()->getScale(), 1.0);
}

TEST(PrecisionReducerTest, MultiPolygonMergesTouchingComponentsAndDropsHoles) {
  GeometryFactory::Ptr gf = GeometryFactory::create();
  WKTReader reader(*gf);

  std::unique_ptr<Geometry> mp = reader.read(
      "MULTIPOLYGON (((0 0, 5 0, 5 5, 0 5, 0 0)), "
      "((5.2 0, 10 0, 10 5, 5.2 5, 5.2 0)))");
  PrecisionModel pm(1.0);
  std::unique_ptr<Geometry> merged =
      PrecisionReducer::reducePrecision(mp.get(), &pm, true);

  EXPECT_TRUE(merged->isValid());
  EXPECT_EQ(merged->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  ExpectNormalizedExact(merged.get(),
                        "POLYGON ((0 0, 5 0, 10 0, 10 5, 5 5, 0 5, 0 0))",
                        merged->getFactory());
  std::unique_ptr<Geometry> expected_rect =
      reader.read("POLYGON ((0 0, 10 0, 10 5, 0 5, 0 0))");
  EXPECT_TRUE(merged->equals(expected_rect.get()));

  std::unique_ptr<Geometry> holed = reader.read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(4 4, 6 4, 6 4.2, 4 4.2, 4 4))");
  std::unique_ptr<Geometry> filled =
      PrecisionReducer::reducePrecision(holed.get(), &pm, true);

  EXPECT_TRUE(filled->isValid());
  ExpectNormalizedExact(filled.get(), "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
                        filled->getFactory());
}

TEST(PrecisionReducerTest,
     HomogeneousGeometryCollectionsAndMixedDimensionThrows) {
  PrecisionModel floating_pm(PrecisionModel::FLOATING);
  GeometryFactory::Ptr gf = GeometryFactory::create(&floating_pm, 27700);
  WKTReader reader(*gf);
  PrecisionModel pm(1.0);

  // Homogeneous polygonal GeometryCollection activates area-only mode.
  std::unique_ptr<Geometry> poly_gc = reader.read(
      "GEOMETRYCOLLECTION (POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0)), "
      "POLYGON ((5.2 0, 10 0, 10 5, 5.2 5, 5.2 0)))");
  std::unique_ptr<Geometry> reduced_poly_gc =
      PrecisionReducer::reducePrecision(poly_gc.get(), &pm, false);
  EXPECT_TRUE(reduced_poly_gc->isValid());
  EXPECT_EQ(reduced_poly_gc->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  ExpectNormalizedExact(reduced_poly_gc.get(),
                        "POLYGON ((0 0, 5 0, 10 0, 10 5, 5 5, 0 5, 0 0))",
                        gf.get());

  // Homogeneous lineal GeometryCollection nodes lines and preserves SRID.
  std::unique_ptr<Geometry> line_gc = reader.read(
      "GEOMETRYCOLLECTION (LINESTRING (0.04 0.04, 10.04 10.04), "
      "LINESTRING (10.04 0.04, 0.04 10.04))");
  std::unique_ptr<Geometry> reduced_line_gc =
      PrecisionReducer::reducePrecision(line_gc.get(), &pm, true);
  EXPECT_TRUE(reduced_line_gc->isValid());
  EXPECT_EQ(reduced_line_gc->getSRID(), 27700);
  EXPECT_EQ(reduced_line_gc->getPrecisionModel()->getType(),
            PrecisionModel::FIXED);
  ExpectNormalizedExact(
      reduced_line_gc.get(),
      "MULTILINESTRING ((0 0, 5 5), (0 10, 5 5), (5 5, 10 0), (5 5, 10 10))",
      reduced_line_gc->getFactory());

  // Mixed-dimension GeometryCollections are rejected by OverlayNG.
  std::unique_ptr<Geometry> mixed_gc = reader.read(
      "GEOMETRYCOLLECTION (LINESTRING (0 0, 5 5), "
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)))");
  EXPECT_THROW(PrecisionReducer::reducePrecision(mixed_gc.get(), &pm, false),
               geos::util::IllegalArgumentException);
  EXPECT_THROW(PrecisionReducer::reducePrecision(mixed_gc.get(), &pm, true),
               geos::util::IllegalArgumentException);
}

TEST(PrecisionReducerTest,
     EmptyGeometriesReturnCanonicalTypeAndPreserveSridAndReplaceOption) {
  PrecisionModel floating_pm(PrecisionModel::FLOATING);
  GeometryFactory::Ptr gf = GeometryFactory::create(&floating_pm, 26910);
  WKTReader reader(*gf);
  PrecisionModel fixed_pm(100.0);

  struct EmptyCase {
    const char* wkt;
    geos::geom::GeometryTypeId expected_type;
  };
  const EmptyCase cases[] = {
      {"POINT EMPTY", geos::geom::GEOS_POINT},
      {"LINESTRING EMPTY", geos::geom::GEOS_LINESTRING},
      {"POLYGON EMPTY", geos::geom::GEOS_POLYGON},
      {"MULTIPOINT EMPTY", geos::geom::GEOS_POINT},
      {"MULTILINESTRING EMPTY", geos::geom::GEOS_LINESTRING},
      {"MULTIPOLYGON EMPTY", geos::geom::GEOS_POLYGON},
      {"GEOMETRYCOLLECTION EMPTY", geos::geom::GEOS_GEOMETRYCOLLECTION},
  };

  for (const auto& c : cases) {
    std::unique_ptr<Geometry> geom = reader.read(c.wkt);

    std::unique_ptr<Geometry> keep_pm =
        PrecisionReducer::reducePrecision(geom.get(), &fixed_pm, false);
    ASSERT_NE(keep_pm, nullptr);
    EXPECT_TRUE(keep_pm->isEmpty());
    EXPECT_EQ(keep_pm->getGeometryTypeId(), c.expected_type);
    EXPECT_EQ(keep_pm->getSRID(), 26910);
    EXPECT_TRUE(keep_pm->getPrecisionModel()->isFloating());

    std::unique_ptr<Geometry> replace_pm =
        PrecisionReducer::reducePrecision(geom.get(), &fixed_pm, true);
    ASSERT_NE(replace_pm, nullptr);
    EXPECT_TRUE(replace_pm->isEmpty());
    EXPECT_EQ(replace_pm->getGeometryTypeId(), c.expected_type);
    EXPECT_EQ(replace_pm->getSRID(), 26910);
    EXPECT_EQ(replace_pm->getPrecisionModel()->getType(),
              PrecisionModel::FIXED);
    EXPECT_DOUBLE_EQ(replace_pm->getPrecisionModel()->getScale(), 100.0);
  }
}

TEST(PrecisionReducerTest, FloatingAndNullPrecisionModelsDoNotRound) {
  PrecisionModel input_pm(10.0);
  GeometryFactory::Ptr gf = GeometryFactory::create(&input_pm, 1234);
  WKTReader reader(*gf);
  std::unique_ptr<Geometry> line = reader.read("LINESTRING (0.1 0.2, 5.3 5.4)");

  PrecisionModel target_floating(PrecisionModel::FLOATING);
  std::unique_ptr<Geometry> replaced =
      PrecisionReducer::reducePrecision(line.get(), &target_floating, true);
  EXPECT_EQ(replaced->getSRID(), 1234);
  EXPECT_TRUE(replaced->getPrecisionModel()->isFloating());
  ExpectNormalizedExact(replaced.get(), "LINESTRING (0.1 0.2, 5.3 5.4)",
                        replaced->getFactory());

  std::unique_ptr<Geometry> kept =
      PrecisionReducer::reducePrecision(line.get(), &target_floating, false);
  EXPECT_EQ(kept->getSRID(), 1234);
  EXPECT_EQ(kept->getPrecisionModel()->getType(), PrecisionModel::FIXED);
  EXPECT_DOUBLE_EQ(kept->getPrecisionModel()->getScale(), 10.0);
  ExpectNormalizedExact(kept.get(), "LINESTRING (0.1 0.2, 5.3 5.4)", gf.get());

  // Null PrecisionModel behaves as floating precision.
  std::unique_ptr<Geometry> null_kept =
      PrecisionReducer::reducePrecision(line.get(), nullptr, false);
  EXPECT_EQ(null_kept->getSRID(), 1234);
  EXPECT_EQ(null_kept->getPrecisionModel()->getType(), PrecisionModel::FIXED);
  ExpectNormalizedExact(null_kept.get(), "LINESTRING (0.1 0.2, 5.3 5.4)",
                        gf.get());

  std::unique_ptr<Geometry> null_replaced =
      PrecisionReducer::reducePrecision(line.get(), nullptr, true);
  EXPECT_EQ(null_replaced->getSRID(), 1234);
  EXPECT_TRUE(null_replaced->getPrecisionModel()->isFloating());
  ExpectNormalizedExact(null_replaced.get(), "LINESTRING (0.1 0.2, 5.3 5.4)",
                        null_replaced->getFactory());
}

TEST(PrecisionReducerTest, PreservesZCoordinatesOnLineString) {
  GeometryFactory::Ptr gf = GeometryFactory::create();
  WKTReader reader(*gf);
  std::unique_ptr<Geometry> line_z =
      reader.read("LINESTRING Z (0.1 0.2 5.5, 3.8 4.9 9.5)");

  PrecisionModel pm(1.0);
  std::unique_ptr<Geometry> reduced =
      PrecisionReducer::reducePrecision(line_z.get(), &pm, true);

  ASSERT_NE(reduced, nullptr);
  EXPECT_TRUE(reduced->hasZ());
  EXPECT_EQ(reduced->getCoordinateDimension(), 3);
  ExpectNormalizedExact(reduced.get(), "LINESTRING (0 0, 4 5)",
                        reduced->getFactory());

  std::unique_ptr<CoordinateSequence> coords = reduced->getCoordinates();
  ASSERT_EQ(coords->size(), 2u);
  EXPECT_DOUBLE_EQ(coords->getAt(0).z, 5.5);
  EXPECT_DOUBLE_EQ(coords->getAt(1).z, 9.5);
}

}  // namespace
}  // namespace geos_google
