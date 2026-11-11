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

// Unit tests for third_party/geos/src/precision/GeometryPrecisionReducer.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - Tests static convenience helpers, instance configuration setters,
//   collection-level preservation, pointwise vs topological reduction,
//   collapsed component handling, and PrecisionModel/factory replacement.
// - GeometryPrecisionReducer::fixPolygonalTopology is a private helper with no
//   in-tree callers and cannot be invoked through the public API.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/precision/GeometryPrecisionReducer.cpp

#include "third_party/geos/include/geos/precision/GeometryPrecisionReducer.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::CircularString;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYZM;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::MultiLineString;
using geos::geom::MultiPoint;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::precision::GeometryPrecisionReducer;
using geos::util::IllegalArgumentException;

class GeometryPrecisionReducerTest : public ::testing::Test {
 protected:
  GeometryPrecisionReducerTest()
      : factory_(GeometryFactory::create()), reader_(factory_.get()) {}

  Geometry::Ptr ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(GeometryPrecisionReducerTest, StaticReducePointLineAndPolygon) {
  PrecisionModel integer_pm(1.0);

  Geometry::Ptr point = ReadWkt("POINT (1.24 3.76)");
  Geometry::Ptr reduced_point =
      GeometryPrecisionReducer::reduce(*point, integer_pm);
  ASSERT_NE(reduced_point, nullptr);
  EXPECT_EQ(reduced_point->getGeometryTypeId(), geos::geom::GEOS_POINT);
  const CoordinateXY* coord = reduced_point->getCoordinate();
  ASSERT_NE(coord, nullptr);
  EXPECT_DOUBLE_EQ(coord->x, 1.0);
  EXPECT_DOUBLE_EQ(coord->y, 4.0);

  Geometry::Ptr line = ReadWkt("LINESTRING (0.1 0.2, 2.8 0.1, 3.2 4.9)");
  Geometry::Ptr reduced_line =
      GeometryPrecisionReducer::reduce(*line, integer_pm);
  ASSERT_NE(reduced_line, nullptr);
  EXPECT_EQ(reduced_line->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  Geometry::Ptr expected_line = ReadWkt("LINESTRING (0 0, 3 0, 3 5)");
  EXPECT_TRUE(reduced_line->equalsExact(expected_line.get()));

  PrecisionModel tenth_pm(10.0);
  Geometry::Ptr poly = ReadWkt(
      "POLYGON ((0.04 0.04, 5.04 0.04, 5.04 5.04, 0.04 5.04, 0.04 0.04))");
  Geometry::Ptr reduced_poly =
      GeometryPrecisionReducer::reduce(*poly, tenth_pm);
  ASSERT_NE(reduced_poly, nullptr);
  EXPECT_TRUE(reduced_poly->isValid());
  EXPECT_DOUBLE_EQ(reduced_poly->getArea(), 25.0);
  // Default precision reducer keeps the input geometry's precision model.
  EXPECT_TRUE(reduced_poly->getPrecisionModel()->isFloating());
}

TEST_F(GeometryPrecisionReducerTest, CoarseGridAndFloatingSinglePrecision) {
  Geometry::Ptr square = ReadWkt("POLYGON ((4 4, 24 4, 24 24, 4 24, 4 4))");

  PrecisionModel tens_pm(0.1);
  Geometry::Ptr coarse = GeometryPrecisionReducer::reduce(*square, tens_pm);
  ASSERT_NE(coarse, nullptr);
  EXPECT_TRUE(coarse->isValid());
  // Vertices at 4 round to 0 and 24 round to 20 -> [0, 20] x [0, 20].
  EXPECT_DOUBLE_EQ(coarse->getArea(), 400.0);

  PrecisionModel float_single_pm(PrecisionModel::FLOATING_SINGLE);
  Geometry::Ptr pt = ReadWkt("POINT (1.1234567890123 2.9876543210987)");
  Geometry::Ptr reduced_single =
      GeometryPrecisionReducer::reduce(*pt, float_single_pm);
  ASSERT_NE(reduced_single, nullptr);
  const CoordinateXY* orig_c = pt->getCoordinate();
  const CoordinateXY* c = reduced_single->getCoordinate();
  ASSERT_NE(orig_c, nullptr);
  ASSERT_NE(c, nullptr);
  EXPECT_DOUBLE_EQ(c->x, static_cast<double>(static_cast<float>(orig_c->x)));
  EXPECT_DOUBLE_EQ(c->y, static_cast<double>(static_cast<float>(orig_c->y)));
}

TEST_F(GeometryPrecisionReducerTest, PreservesTopologicalValidityVsPointwise) {
  PrecisionModel integer_pm(1.0);
  // Vertex (2, 0.4) is strictly above the bottom edge (0,0)-(4,0), so input is
  // valid. Rounding to grid 1.0 snaps (2, 0.4) onto (2, 0), creating a pinch
  // point between two triangles of area 4.0 each.
  Geometry::Ptr pinch_poly =
      ReadWkt("POLYGON ((0 0, 4 0, 4 4, 2 0.4, 0 4, 0 0))");
  ASSERT_TRUE(pinch_poly->isValid());

  Geometry::Ptr topo_reduced =
      GeometryPrecisionReducer::reduce(*pinch_poly, integer_pm);
  ASSERT_NE(topo_reduced, nullptr);
  EXPECT_TRUE(topo_reduced->isValid());
  EXPECT_EQ(topo_reduced->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(topo_reduced->getNumGeometries(), 2U);
  EXPECT_DOUBLE_EQ(topo_reduced->getArea(), 8.0);

  Geometry::Ptr pointwise_reduced =
      GeometryPrecisionReducer::reducePointwise(*pinch_poly, integer_pm);
  ASSERT_NE(pointwise_reduced, nullptr);
  EXPECT_EQ(pointwise_reduced->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_EQ(pointwise_reduced->getNumPoints(), 6U);
  EXPECT_FALSE(pointwise_reduced->isValid());
}

TEST_F(GeometryPrecisionReducerTest, PointwisePreservesCollapsedPolygon) {
  PrecisionModel integer_pm(1.0);
  Geometry::Ptr tiny_poly =
      ReadWkt("POLYGON ((0.1 0.1, 0.4 0.1, 0.4 0.4, 0.1 0.4, 0.1 0.1))");

  Geometry::Ptr topo_reduced =
      GeometryPrecisionReducer::reduce(*tiny_poly, integer_pm);
  ASSERT_NE(topo_reduced, nullptr);
  EXPECT_TRUE(topo_reduced->isEmpty());

  Geometry::Ptr pointwise_reduced =
      GeometryPrecisionReducer::reducePointwise(*tiny_poly, integer_pm);
  ASSERT_NE(pointwise_reduced, nullptr);
  EXPECT_FALSE(pointwise_reduced->isEmpty());
  EXPECT_EQ(pointwise_reduced->getNumPoints(), 5U);
  EXPECT_DOUBLE_EQ(pointwise_reduced->getArea(), 0.0);
  EXPECT_FALSE(pointwise_reduced->isValid());

  // Verify toggling setPointwise on a reducer instance.
  GeometryPrecisionReducer reducer(integer_pm);
  reducer.setPointwise(true);
  Geometry::Ptr instance_pointwise = reducer.reduce(*tiny_poly);
  EXPECT_TRUE(instance_pointwise->equalsExact(pointwise_reduced.get()));

  reducer.setPointwise(false);
  Geometry::Ptr instance_topo = reducer.reduce(*tiny_poly);
  EXPECT_TRUE(instance_topo->isEmpty());
}

TEST_F(GeometryPrecisionReducerTest, CollapsedLineStringAndLinearRingHandling) {
  PrecisionModel integer_pm(1.0);
  Geometry::Ptr short_line = ReadWkt("LINESTRING (0.1 0.1, 0.2 0.2)");

  Geometry::Ptr removed =
      GeometryPrecisionReducer::reduce(*short_line, integer_pm);
  ASSERT_NE(removed, nullptr);
  EXPECT_EQ(removed->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(removed->isEmpty());
  EXPECT_EQ(removed->getNumPoints(), 0U);

  Geometry::Ptr kept =
      GeometryPrecisionReducer::reduceKeepCollapsed(*short_line, integer_pm);
  ASSERT_NE(kept, nullptr);
  EXPECT_EQ(kept->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_FALSE(kept->isEmpty());
  EXPECT_EQ(kept->getNumPoints(), 2U);
  EXPECT_DOUBLE_EQ(kept->getLength(), 0.0);

  // Also test collapsing a LinearRing via instance setter.
  auto ring_coords =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.1, 0.1}, {0.3, 0.1}, {0.3, 0.3}, {0.1, 0.1}});
  std::unique_ptr<LinearRing> ring =
      factory_->createLinearRing(std::move(ring_coords));

  GeometryPrecisionReducer reducer(integer_pm);
  reducer.setRemoveCollapsedComponents(true);
  Geometry::Ptr removed_ring = reducer.reduce(*ring);
  ASSERT_NE(removed_ring, nullptr);
  EXPECT_TRUE(removed_ring->isEmpty());

  reducer.setRemoveCollapsedComponents(false);
  Geometry::Ptr kept_ring = reducer.reduce(*ring);
  ASSERT_NE(kept_ring, nullptr);
  EXPECT_FALSE(kept_ring->isEmpty());
  // PrecisionReducerTransformer extends collapsed LinearRing to
  // MINIMUM_VALID_SIZE (3), which GeometryTransformer converts to a LineString.
  EXPECT_EQ(kept_ring->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_EQ(kept_ring->getNumPoints(),
            static_cast<std::size_t>(+LinearRing::MINIMUM_VALID_SIZE));
  EXPECT_DOUBLE_EQ(kept_ring->getLength(), 0.0);

  // Under pointwise reduction, all 4 repeated vertices are retained as
  // LinearRing.
  Geometry::Ptr pointwise_ring =
      GeometryPrecisionReducer::reducePointwise(*ring, integer_pm);
  ASSERT_NE(pointwise_ring, nullptr);
  EXPECT_EQ(pointwise_ring->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  EXPECT_EQ(pointwise_ring->getNumPoints(), 4U);
  EXPECT_DOUBLE_EQ(pointwise_ring->getLength(), 0.0);
}

TEST_F(GeometryPrecisionReducerTest,
       CollectionLevelPreservedWhenCollapsedToSingle) {
  PrecisionModel integer_pm(1.0);

  // Touching MultiPolygon components merge into a single polygonal face under
  // snap-rounding; GeometryPrecisionReducer wraps it back into a MultiPolygon.
  Geometry::Ptr touching_mp = ReadWkt(
      "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "((2.1 0, 4 0, 4 2, 2.1 2, 2.1 0)))");
  Geometry::Ptr reduced_mp =
      GeometryPrecisionReducer::reduce(*touching_mp, integer_pm);
  ASSERT_NE(reduced_mp, nullptr);
  EXPECT_EQ(reduced_mp->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(reduced_mp->getNumGeometries(), 1U);
  EXPECT_TRUE(reduced_mp->isValid());
  EXPECT_DOUBLE_EQ(reduced_mp->getArea(), 8.0);

  // One LineString collapses and is removed, leaving one LineString;
  // GeometryPrecisionReducer wraps the single LineString back to
  // MultiLineString.
  Geometry::Ptr partial_mls =
      ReadWkt("MULTILINESTRING ((0 0, 10 0), (0.1 0.1, 0.2 0.2))");
  Geometry::Ptr reduced_mls =
      GeometryPrecisionReducer::reduce(*partial_mls, integer_pm);
  ASSERT_NE(reduced_mls, nullptr);
  EXPECT_EQ(reduced_mls->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(reduced_mls->getNumGeometries(), 1U);
  EXPECT_DOUBLE_EQ(reduced_mls->getLength(), 10.0);

  // Single-element MultiPoint stays MultiPoint.
  Geometry::Ptr single_mpt = ReadWkt("MULTIPOINT ((0.2 0.8))");
  Geometry::Ptr reduced_mpt =
      GeometryPrecisionReducer::reduce(*single_mpt, integer_pm);
  ASSERT_NE(reduced_mpt, nullptr);
  EXPECT_EQ(reduced_mpt->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(reduced_mpt->getNumGeometries(), 1U);

  // Collection remaining multi-component takes the already-collection branch.
  Geometry::Ptr two_lines = ReadWkt("MULTILINESTRING ((0 0, 3 0), (0 5, 3 5))");
  Geometry::Ptr reduced_two_lines =
      GeometryPrecisionReducer::reduce(*two_lines, integer_pm);
  ASSERT_NE(reduced_two_lines, nullptr);
  EXPECT_EQ(reduced_two_lines->getGeometryTypeId(),
            geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(reduced_two_lines->getNumGeometries(), 2U);
}

TEST_F(GeometryPrecisionReducerTest, ChangePrecisionModelFactoryAndSrid) {
  PrecisionModel float_pm;
  constexpr int kExpectedSrid = 4326;
  GeometryFactory::Ptr src_factory =
      GeometryFactory::create(&float_pm, kExpectedSrid);
  WKTReader srid_reader(src_factory.get());

  PrecisionModel fixed_pm(10.0);
  GeometryFactory::Ptr target_factory =
      GeometryFactory::create(&fixed_pm, kExpectedSrid);

  // Polygon with an interior ring exercises changePM across Polygon, exterior
  // shell LinearRing, and interior hole LinearRing.
  Geometry::Ptr poly_with_hole = srid_reader.read(
      "POLYGON ((0.04 0.04, 10.04 0.04, 10.04 10.04, 0.04 10.04, 0.04 0.04), "
      "(2.04 2.04, 4.04 2.04, 4.04 4.04, 2.04 4.04, 2.04 2.04))");
  Geometry::Ptr multipolygon = srid_reader.read(
      "MULTIPOLYGON (((0.04 0.04, 2.04 0.04, 2.04 2.04, 0.04 2.04, 0.04 "
      "0.04)), "
      "((5.04 0.04, 7.04 0.04, 7.04 2.04, 5.04 2.04, 5.04 0.04)))");
  Geometry::Ptr linestring =
      srid_reader.read("LINESTRING (0.04 0.04, 3.04 0.04)");
  Geometry::Ptr point = srid_reader.read("POINT (1.14 2.16)");

  // Even when constructed from GeometryFactory, changePrecisionModel defaults
  // to false until explicitly enabled.
  GeometryPrecisionReducer default_flag_reducer(*target_factory);
  default_flag_reducer.setUseAreaReducer(true);
  Geometry::Ptr unchanged_pm = default_flag_reducer.reduce(*point);
  ASSERT_NE(unchanged_pm, nullptr);
  EXPECT_TRUE(unchanged_pm->getPrecisionModel()->isFloating());

  // Enable setChangePrecisionModel(true) in an inner scope to verify returned
  // geometries own a valid ref-counted GeometryFactory beyond reducer lifetime.
  Geometry::Ptr changed_poly;
  Geometry::Ptr changed_mp;
  Geometry::Ptr changed_line;
  Geometry::Ptr changed_point;
  {
    GeometryPrecisionReducer reducer(*target_factory);
    reducer.setUseAreaReducer(false);
    reducer.setChangePrecisionModel(true);
    changed_poly = reducer.reduce(*poly_with_hole);
    changed_mp = reducer.reduce(*multipolygon);
    changed_line = reducer.reduce(*linestring);
    changed_point = reducer.reduce(*point);
  }

  for (const Geometry* g : {changed_poly.get(), changed_mp.get(),
                            changed_line.get(), changed_point.get()}) {
    ASSERT_NE(g, nullptr);
    EXPECT_FALSE(g->getPrecisionModel()->isFloating());
    EXPECT_EQ(g->getPrecisionModel()->getType(), PrecisionModel::FIXED);
    EXPECT_DOUBLE_EQ(g->getPrecisionModel()->getScale(), 10.0);
    EXPECT_EQ(g->getSRID(), kExpectedSrid);
  }

  EXPECT_TRUE(changed_poly->isValid());
  EXPECT_DOUBLE_EQ(changed_poly->getArea(), 96.0);
  EXPECT_EQ(changed_mp->getNumGeometries(), 2U);
  EXPECT_DOUBLE_EQ(changed_mp->getArea(), 8.0);
  EXPECT_DOUBLE_EQ(changed_line->getLength(), 3.0);
  const CoordinateXY* pt_coord = changed_point->getCoordinate();
  ASSERT_NE(pt_coord, nullptr);
  EXPECT_DOUBLE_EQ(pt_coord->x, 1.1);
  EXPECT_DOUBLE_EQ(pt_coord->y, 2.2);
}

TEST_F(GeometryPrecisionReducerTest,
       ChangePrecisionModelShortCircuitsSamePmPtr) {
  PrecisionModel fixed_pm(100.0);
  constexpr int kSrid = 3857;
  GeometryFactory::Ptr fixed_factory =
      GeometryFactory::create(&fixed_pm, kSrid);
  WKTReader fixed_reader(fixed_factory.get());

  Geometry::Ptr line =
      fixed_reader.read("LINESTRING (1.234 5.678, 9.876 5.432)");
  // Pass the exact GeometryFactory owning &fixed_factory->getPrecisionModel()
  // so &targetPM == geom.getFactory()->getPrecisionModel() short-circuits.
  GeometryPrecisionReducer reducer(*fixed_factory);
  reducer.setChangePrecisionModel(true);

  Geometry::Ptr reduced = reducer.reduce(*line);
  ASSERT_NE(reduced, nullptr);
  EXPECT_EQ(reduced->getFactory(), fixed_factory.get());
  EXPECT_EQ(reduced->getSRID(), kSrid);
  Geometry::Ptr expected =
      fixed_reader.read("LINESTRING (1.23 5.68, 9.88 5.43)");
  EXPECT_TRUE(reduced->equalsExact(expected.get()));
}

TEST_F(GeometryPrecisionReducerTest,
       EmptyCollectionsAndThreeDimensionalCoords) {
  PrecisionModel integer_pm(1.0);
  GeometryPrecisionReducer reducer(integer_pm);
  reducer.setChangePrecisionModel(true);

  for (const std::string& empty_wkt :
       {"POINT EMPTY", "LINESTRING EMPTY", "POLYGON EMPTY",
        "GEOMETRYCOLLECTION EMPTY"}) {
    SCOPED_TRACE(empty_wkt);
    Geometry::Ptr geom = ReadWkt(empty_wkt);
    Geometry::Ptr reduced = reducer.reduce(*geom);
    ASSERT_NE(reduced, nullptr);
    EXPECT_TRUE(reduced->isEmpty());
    EXPECT_FALSE(reduced->getPrecisionModel()->isFloating());
    EXPECT_EQ(reduced->getGeometryTypeId(), geom->getGeometryTypeId());
  }

  // Heterogeneous GeometryCollection preserves surviving components.
  Geometry::Ptr gc = ReadWkt(
      "GEOMETRYCOLLECTION (POINT (1.2 3.7), "
      "LINESTRING (0.1 0.1, 4.9 0.1), "
      "POLYGON ((0 0, 2.4 0, 2.4 2.4, 0 2.4, 0 0)))");
  Geometry::Ptr reduced_gc = reducer.reduce(*gc);
  ASSERT_NE(reduced_gc, nullptr);
  EXPECT_EQ(reduced_gc->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(reduced_gc->getNumGeometries(), 3U);

  // Z coordinates pass through unchanged under both modes.
  Geometry::Ptr line_z = ReadWkt("LINESTRING Z (0.4 1.6 9.25, 3.6 4.4 12.75)");
  Geometry::Ptr reduced_z =
      GeometryPrecisionReducer::reduce(*line_z, integer_pm);
  Geometry::Ptr reduced_z_pw =
      GeometryPrecisionReducer::reducePointwise(*line_z, integer_pm);
  for (const Geometry* g : {reduced_z.get(), reduced_z_pw.get()}) {
    ASSERT_NE(g, nullptr);
    EXPECT_TRUE(g->hasZ());
    std::unique_ptr<CoordinateSequence> seq = g->getCoordinates();
    ASSERT_EQ(seq->size(), 2U);
    EXPECT_DOUBLE_EQ(seq->getAt<Coordinate>(0).x, 0.0);
    EXPECT_DOUBLE_EQ(seq->getAt<Coordinate>(0).y, 2.0);
    EXPECT_DOUBLE_EQ(seq->getAt<Coordinate>(0).z, 9.25);
    EXPECT_DOUBLE_EQ(seq->getAt<Coordinate>(1).x, 4.0);
    EXPECT_DOUBLE_EQ(seq->getAt<Coordinate>(1).y, 4.0);
    EXPECT_DOUBLE_EQ(seq->getAt<Coordinate>(1).z, 12.75);
  }
}

TEST_F(GeometryPrecisionReducerTest,
       ReduceKeepCollapsedCollectionsAndPolygons) {
  PrecisionModel integer_pm(1.0);

  // Mixed MultiLineString retains the collapsed component with length 0.
  Geometry::Ptr partial_mls =
      ReadWkt("MULTILINESTRING ((0 0, 10 0), (0.1 0.1, 0.2 0.2))");
  Geometry::Ptr kept_mls =
      GeometryPrecisionReducer::reduceKeepCollapsed(*partial_mls, integer_pm);
  ASSERT_NE(kept_mls, nullptr);
  EXPECT_EQ(kept_mls->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  ASSERT_EQ(kept_mls->getNumGeometries(), 2U);
  EXPECT_DOUBLE_EQ(kept_mls->getGeometryN(0)->getLength(), 10.0);
  EXPECT_EQ(kept_mls->getGeometryN(1)->getNumPoints(), 2U);
  EXPECT_DOUBLE_EQ(kept_mls->getGeometryN(1)->getLength(), 0.0);

  // When every LineString collapses, default removes all while keep-collapsed
  // retains them.
  Geometry::Ptr all_collapse_mls =
      ReadWkt("MULTILINESTRING ((0.1 0.1, 0.2 0.2), (1.1 1.1, 1.2 1.2))");
  Geometry::Ptr removed_all_mls =
      GeometryPrecisionReducer::reduce(*all_collapse_mls, integer_pm);
  ASSERT_NE(removed_all_mls, nullptr);
  EXPECT_EQ(removed_all_mls->getGeometryTypeId(),
            geos::geom::GEOS_MULTILINESTRING);
  EXPECT_TRUE(removed_all_mls->isEmpty());

  Geometry::Ptr kept_all_mls = GeometryPrecisionReducer::reduceKeepCollapsed(
      *all_collapse_mls, integer_pm);
  ASSERT_NE(kept_all_mls, nullptr);
  EXPECT_EQ(kept_all_mls->getGeometryTypeId(),
            geos::geom::GEOS_MULTILINESTRING);
  EXPECT_FALSE(kept_all_mls->isEmpty());
  EXPECT_EQ(kept_all_mls->getNumGeometries(), 2U);
  EXPECT_DOUBLE_EQ(kept_all_mls->getLength(), 0.0);

  // Polygonal area reduction discards collapsed polygonal faces even with
  // keep-collapsed.
  Geometry::Ptr tiny_poly =
      ReadWkt("POLYGON ((0.1 0.1, 0.4 0.1, 0.4 0.4, 0.1 0.4, 0.1 0.1))");
  Geometry::Ptr kept_poly =
      GeometryPrecisionReducer::reduceKeepCollapsed(*tiny_poly, integer_pm);
  ASSERT_NE(kept_poly, nullptr);
  EXPECT_EQ(kept_poly->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(kept_poly->isEmpty());
}

TEST_F(GeometryPrecisionReducerTest,
       CollapsedMultiPolygonAndEmptyMultiCollections) {
  PrecisionModel integer_pm(1.0);

  // All-collapsed MultiPolygon returns POLYGON EMPTY from area reducer,
  // which GeometryPrecisionReducer wraps back to MULTIPOLYGON EMPTY.
  Geometry::Ptr tiny_mp =
      ReadWkt("MULTIPOLYGON (((0.1 0.1, 0.4 0.1, 0.4 0.4, 0.1 0.4, 0.1 0.1)))");
  Geometry::Ptr reduced_mp =
      GeometryPrecisionReducer::reduce(*tiny_mp, integer_pm);
  ASSERT_NE(reduced_mp, nullptr);
  EXPECT_EQ(reduced_mp->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_TRUE(reduced_mp->isEmpty());

  GeometryPrecisionReducer reducer(integer_pm);
  reducer.setChangePrecisionModel(true);
  Geometry::Ptr reduced_mp_pm = reducer.reduce(*tiny_mp);
  ASSERT_NE(reduced_mp_pm, nullptr);
  EXPECT_EQ(reduced_mp_pm->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_TRUE(reduced_mp_pm->isEmpty());
  EXPECT_FALSE(reduced_mp_pm->getPrecisionModel()->isFloating());

  for (const std::string& empty_wkt :
       {"MULTIPOINT EMPTY", "MULTILINESTRING EMPTY", "MULTIPOLYGON EMPTY"}) {
    SCOPED_TRACE(empty_wkt);
    Geometry::Ptr geom = ReadWkt(empty_wkt);
    Geometry::Ptr reduced = reducer.reduce(*geom);
    ASSERT_NE(reduced, nullptr);
    EXPECT_TRUE(reduced->isEmpty());
    EXPECT_EQ(reduced->getGeometryTypeId(), geom->getGeometryTypeId());
    EXPECT_FALSE(reduced->getPrecisionModel()->isFloating());
    EXPECT_DOUBLE_EQ(reduced->getPrecisionModel()->getScale(), 1.0);
  }

  // Pointwise reduction on a single-polygon MultiPolygon preserves MultiPolygon
  // type.
  Geometry::Ptr single_mp =
      ReadWkt("MULTIPOLYGON (((0.1 0.1, 2.1 0.1, 2.1 2.1, 0.1 2.1, 0.1 0.1)))");
  Geometry::Ptr pw_mp =
      GeometryPrecisionReducer::reducePointwise(*single_mp, integer_pm);
  ASSERT_NE(pw_mp, nullptr);
  EXPECT_EQ(pw_mp->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(pw_mp->getNumGeometries(), 1U);
  EXPECT_DOUBLE_EQ(pw_mp->getArea(), 4.0);
}

TEST_F(GeometryPrecisionReducerTest,
       CollectionDimensionMismatchSkipsMultiWrap) {
  PrecisionModel integer_pm(1.0);

  // Mixed-dimension MultiLineString: 3D short component collapses and is
  // removed, leaving only the 2D component (dimension 2 != input dimension 3),
  // so collection-level re-wrapping is skipped.
  auto short_3d_seq = std::make_unique<CoordinateSequence>(
      std::initializer_list<Coordinate>{{0.1, 0.1, 5.0}, {0.2, 0.2, 5.0}});
  auto long_2d_seq = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{{0.0, 0.0}, {10.0, 0.0}});
  std::vector<std::unique_ptr<LineString>> lines;
  lines.push_back(factory_->createLineString(std::move(short_3d_seq)));
  lines.push_back(factory_->createLineString(std::move(long_2d_seq)));
  std::unique_ptr<MultiLineString> mixed_dim_mls =
      factory_->createMultiLineString(std::move(lines));
  ASSERT_EQ(mixed_dim_mls->getCoordinateDimension(), 3U);

  Geometry::Ptr reduced_line =
      GeometryPrecisionReducer::reduce(*mixed_dim_mls, integer_pm);
  ASSERT_NE(reduced_line, nullptr);
  EXPECT_EQ(reduced_line->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_EQ(reduced_line->getCoordinateDimension(), 2U);
  EXPECT_DOUBLE_EQ(reduced_line->getLength(), 10.0);

  // Pointwise reduction drops M ordinates, reducing a single-point XYZM
  // MultiPoint (dim 4) to a Point Z (dim 3), skipping MultiPoint wrapping.
  std::vector<std::unique_ptr<Geometry>> pts;
  pts.push_back(factory_->createPoint(CoordinateXYZM(1.2, 3.4, 5.6, 7.8)));
  std::unique_ptr<MultiPoint> mpt_zm =
      factory_->createMultiPoint(std::move(pts));
  ASSERT_EQ(mpt_zm->getCoordinateDimension(), 4U);

  Geometry::Ptr reduced_pt =
      GeometryPrecisionReducer::reducePointwise(*mpt_zm, integer_pm);
  ASSERT_NE(reduced_pt, nullptr);
  EXPECT_EQ(reduced_pt->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_EQ(reduced_pt->getCoordinateDimension(), 3U);
  EXPECT_TRUE(reduced_pt->hasZ());
  EXPECT_FALSE(reduced_pt->hasM());
  std::unique_ptr<CoordinateSequence> pt_seq = reduced_pt->getCoordinates();
  ASSERT_EQ(pt_seq->size(), 1U);
  EXPECT_DOUBLE_EQ(pt_seq->getAt<Coordinate>(0).x, 1.0);
  EXPECT_DOUBLE_EQ(pt_seq->getAt<Coordinate>(0).y, 3.0);
  EXPECT_DOUBLE_EQ(pt_seq->getAt<Coordinate>(0).z, 5.6);
}

TEST_F(GeometryPrecisionReducerTest,
       ChangePrecisionModelAdditionalTypesAndTransitions) {
  PrecisionModel integer_pm(1.0);
  constexpr int kCustomSrid = 32610;
  GeometryFactory::Ptr srid_float_factory =
      GeometryFactory::create(nullptr, kCustomSrid);
  WKTReader srid_reader(srid_float_factory.get());

  // LinearRing (valid non-collapsing and pointwise collapsing) with changePM.
  auto square_ring_coords =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.1, 0.1}, {4.1, 0.1}, {4.1, 4.1}, {0.1, 4.1}, {0.1, 0.1}});
  std::unique_ptr<LinearRing> square_ring =
      srid_float_factory->createLinearRing(std::move(square_ring_coords));

  GeometryPrecisionReducer change_reducer(integer_pm);
  change_reducer.setChangePrecisionModel(true);
  Geometry::Ptr changed_ring = change_reducer.reduce(*square_ring);
  ASSERT_NE(changed_ring, nullptr);
  EXPECT_EQ(changed_ring->getGeometryTypeId(), geos::geom::GEOS_LINEARRING);
  EXPECT_EQ(changed_ring->getSRID(), kCustomSrid);
  EXPECT_FALSE(changed_ring->getPrecisionModel()->isFloating());
  EXPECT_DOUBLE_EQ(changed_ring->getLength(), 16.0);

  Geometry::Ptr mpt = srid_reader.read("MULTIPOINT ((1.2 2.8), (3.6 4.4))");
  Geometry::Ptr mls = srid_reader.read(
      "MULTILINESTRING ((0.1 0.1, 2.9 0.1), (0.1 5.1, 2.9 5.1))");
  Geometry::Ptr changed_mpt = change_reducer.reduce(*mpt);
  Geometry::Ptr changed_mls = change_reducer.reduce(*mls);
  ASSERT_NE(changed_mpt, nullptr);
  ASSERT_NE(changed_mls, nullptr);
  EXPECT_EQ(changed_mpt->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(changed_mls->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_EQ(changed_mpt->getSRID(), kCustomSrid);
  EXPECT_EQ(changed_mls->getSRID(), kCustomSrid);
  EXPECT_FALSE(changed_mpt->getPrecisionModel()->isFloating());
  EXPECT_FALSE(changed_mls->getPrecisionModel()->isFloating());

  // Transition from FIXED precision model back to FLOATING precision model.
  PrecisionModel fixed_src_pm(100.0);
  GeometryFactory::Ptr fixed_src_factory =
      GeometryFactory::create(&fixed_src_pm, kCustomSrid);
  WKTReader fixed_reader(fixed_src_factory.get());
  Geometry::Ptr fixed_pt = fixed_reader.read("POINT (1.25 2.75)");

  PrecisionModel floating_pm(PrecisionModel::FLOATING);
  GeometryPrecisionReducer to_float_reducer(floating_pm);
  to_float_reducer.setChangePrecisionModel(true);
  Geometry::Ptr became_floating = to_float_reducer.reduce(*fixed_pt);
  ASSERT_NE(became_floating, nullptr);
  EXPECT_TRUE(became_floating->getPrecisionModel()->isFloating());
  EXPECT_EQ(became_floating->getPrecisionModel()->getType(),
            PrecisionModel::FLOATING);
  EXPECT_EQ(became_floating->getSRID(), kCustomSrid);

  // Distinct PrecisionModel instance at another address triggers changePM even
  // when its scale matches the source factory's PrecisionModel.
  PrecisionModel duplicate_scale_pm(100.0);
  GeometryPrecisionReducer dup_reducer(duplicate_scale_pm);
  dup_reducer.setChangePrecisionModel(true);
  Geometry::Ptr changed_dup = dup_reducer.reduce(*fixed_pt);
  ASSERT_NE(changed_dup, nullptr);
  EXPECT_NE(changed_dup->getFactory(), fixed_src_factory.get());
  EXPECT_EQ(changed_dup->getSRID(), kCustomSrid);
  EXPECT_DOUBLE_EQ(changed_dup->getPrecisionModel()->getScale(), 100.0);

  // Negative scale specifies grid size directly (gridSize = 10.0, scale = 0.1).
  PrecisionModel neg_grid_pm(-10.0);
  Geometry::Ptr pt_to_snap = ReadWkt("POINT (14.2 16.8)");
  Geometry::Ptr snapped =
      GeometryPrecisionReducer::reduce(*pt_to_snap, neg_grid_pm);
  ASSERT_NE(snapped, nullptr);
  const CoordinateXY* c = snapped->getCoordinate();
  ASSERT_NE(c, nullptr);
  EXPECT_DOUBLE_EQ(c->x, 10.0);
  EXPECT_DOUBLE_EQ(c->y, 20.0);
}

TEST_F(GeometryPrecisionReducerTest,
       NanCoordinatesAndUnsupportedCurvedGeometry) {
  PrecisionModel integer_pm(1.0);

  // NaN ordinate stays NaN while finite ordinates are rounded.
  Geometry::Ptr nan_pt =
      factory_->createPoint(CoordinateXY(geos::DoubleNotANumber, 1.6));
  Geometry::Ptr reduced_nan =
      GeometryPrecisionReducer::reduce(*nan_pt, integer_pm);
  Geometry::Ptr reduced_nan_pw =
      GeometryPrecisionReducer::reducePointwise(*nan_pt, integer_pm);
  for (const Geometry* g : {reduced_nan.get(), reduced_nan_pw.get()}) {
    ASSERT_NE(g, nullptr);
    const CoordinateXY* c = g->getCoordinate();
    ASSERT_NE(c, nullptr);
    EXPECT_TRUE(std::isnan(c->x));
    EXPECT_DOUBLE_EQ(c->y, 2.0);
  }

  // Curved geometry subtypes are unsupported by GeometryTransformer.
  auto arc_seq = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}});
  std::unique_ptr<CircularString> arc =
      factory_->createCircularString(std::move(arc_seq));
  EXPECT_THROW(GeometryPrecisionReducer::reduce(*arc, integer_pm),
               IllegalArgumentException);
  EXPECT_THROW(GeometryPrecisionReducer::reducePointwise(*arc, integer_pm),
               IllegalArgumentException);
}

}  // namespace
}  // namespace geos_google
