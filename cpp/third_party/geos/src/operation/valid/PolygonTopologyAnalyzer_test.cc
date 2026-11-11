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

#include "third_party/geos/include/geos/operation/valid/PolygonTopologyAnalyzer.h"

#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/valid/TopologyValidationError.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/IllegalStateException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::operation::valid::PolygonTopologyAnalyzer;
using geos::operation::valid::TopologyValidationError;

class PolygonTopologyAnalyzerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    factory_ = GeometryFactory::create();
    reader_ = std::make_unique<WKTReader>(*factory_);
  }

  void TearDown() override {
    reader_.reset();
    factory_.reset();
  }

  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_->read(wkt);
  }

  std::unique_ptr<LinearRing> ReadLinearRing(const std::string& wkt) const {
    return reader_->read<LinearRing>(wkt);
  }

  std::unique_ptr<Polygon> ReadPolygon(const std::string& wkt) const {
    return reader_->read<Polygon>(wkt);
  }

  std::unique_ptr<LinearRing> MakeRing(
      std::initializer_list<CoordinateXY> coords) const {
    auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
    seq->reserve(coords.size());
    for (const auto& pt : coords) {
      seq->add(pt);
    }
    return factory_->createLinearRing(std::move(seq));
  }

  void ExpectValidAndConnected(const Geometry* geom,
                               bool is_inverted_ring_valid) const {
    PolygonTopologyAnalyzer analyzer(geom, is_inverted_ring_valid);
    EXPECT_FALSE(analyzer.hasInvalidIntersection());
    EXPECT_EQ(analyzer.getInvalidCode(),
              +TopologyValidationError::oNoInvalidIntersection);
    EXPECT_TRUE(analyzer.getInvalidLocation().isNull());
    EXPECT_FALSE(analyzer.isInteriorDisconnected());
    EXPECT_TRUE(analyzer.getDisconnectionLocation().isNull());
  }

  GeometryFactory::Ptr factory_;
  std::unique_ptr<WKTReader> reader_;
};

TEST_F(PolygonTopologyAnalyzerTest, EmptyGeometriesReturnEarlyAndAreConnected) {
  const std::vector<std::string> empty_wkts = {
      "POLYGON EMPTY", "MULTIPOLYGON EMPTY", "LINEARRING EMPTY",
      "POINT EMPTY",   "LINESTRING EMPTY",   "GEOMETRYCOLLECTION EMPTY",
  };

  for (const std::string& wkt : empty_wkts) {
    auto geom = ReadWkt(wkt);
    ExpectValidAndConnected(geom.get(), /*is_inverted_ring_valid=*/false);
    ExpectValidAndConnected(geom.get(), /*is_inverted_ring_valid=*/true);

    PolygonTopologyAnalyzer analyzer(geom.get(),
                                     /*p_isInvertedRingValid=*/true);
    analyzer.checkInteriorDisconnectedBySelfTouch();
    analyzer.checkInteriorDisconnectedByHoleCycle();
    EXPECT_TRUE(analyzer.getDisconnectionLocation().isNull());
  }
}

TEST_F(PolygonTopologyAnalyzerTest, NonEmptyNonPolygonalInputsThrow) {
  const std::vector<std::string> invalid_type_wkts = {
      "POINT (1 2)",
      "LINESTRING (0 0, 1 1)",
      "MULTIPOINT ((0 0), (1 1))",
      "MULTILINESTRING ((0 0, 1 1))",
      "GEOMETRYCOLLECTION (POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0)))",
  };

  for (const std::string& wkt : invalid_type_wkts) {
    auto geom = ReadWkt(wkt);
    EXPECT_THROW(PolygonTopologyAnalyzer(geom.get(), false),
                 geos::util::IllegalArgumentException)
        << wkt;
    EXPECT_THROW(PolygonTopologyAnalyzer(geom.get(), true),
                 geos::util::IllegalArgumentException)
        << wkt;
  }
}

TEST_F(PolygonTopologyAnalyzerTest, ValidSimplePolygonBothModes) {
  auto poly = ReadPolygon("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  ExpectValidAndConnected(poly.get(), /*is_inverted_ring_valid=*/false);
  ExpectValidAndConnected(poly.get(), /*is_inverted_ring_valid=*/true);

  PolygonTopologyAnalyzer analyzer(poly.get(), /*p_isInvertedRingValid=*/false);
  analyzer.checkInteriorDisconnectedBySelfTouch();
  analyzer.checkInteriorDisconnectedByHoleCycle();
  EXPECT_TRUE(analyzer.getDisconnectionLocation().isNull());
}

TEST_F(PolygonTopologyAnalyzerTest, ValidPolygonWithHolesAndEmptyHole) {
  auto with_hole = ReadPolygon(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 4 2, 4 4, 2 4, 2 2))");
  ExpectValidAndConnected(with_hole.get(), /*is_inverted_ring_valid=*/false);
  ExpectValidAndConnected(with_hole.get(), /*is_inverted_ring_valid=*/true);

  auto shell = MakeRing({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}});
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(factory_->createLinearRing());
  holes.push_back(MakeRing({{2, 2}, {4, 2}, {4, 4}, {2, 4}, {2, 2}}));
  auto poly_with_empty_hole =
      factory_->createPolygon(std::move(shell), std::move(holes));
  ExpectValidAndConnected(poly_with_empty_hole.get(),
                          /*is_inverted_ring_valid=*/false);
  ExpectValidAndConnected(poly_with_empty_hole.get(),
                          /*is_inverted_ring_valid=*/true);

  auto shell_only_empty_hole =
      MakeRing({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}});
  std::vector<std::unique_ptr<LinearRing>> only_empty_hole;
  only_empty_hole.push_back(factory_->createLinearRing());
  auto poly_only_empty_hole = factory_->createPolygon(
      std::move(shell_only_empty_hole), std::move(only_empty_hole));
  ExpectValidAndConnected(poly_only_empty_hole.get(),
                          /*is_inverted_ring_valid=*/false);
}

TEST_F(PolygonTopologyAnalyzerTest, ValidLinearRingAndRepeatedPoints) {
  auto ring = ReadLinearRing("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  ExpectValidAndConnected(ring.get(), /*is_inverted_ring_valid=*/false);
  ExpectValidAndConnected(ring.get(), /*is_inverted_ring_valid=*/true);

  auto dedup_poly = ReadPolygon(
      "POLYGON ((0 0, 10 0, 10 0, 10 10, 0 10, 0 10, 0 0), "
      "(2 2, 4 2, 4 2, 4 4, 2 4, 2 2))");
  ExpectValidAndConnected(dedup_poly.get(), /*is_inverted_ring_valid=*/false);
  ExpectValidAndConnected(dedup_poly.get(), /*is_inverted_ring_valid=*/true);

  auto dedup_ring =
      ReadLinearRing("LINEARRING (0 0, 0 0, 10 0, 10 10, 0 10, 0 0)");
  ExpectValidAndConnected(dedup_ring.get(), /*is_inverted_ring_valid=*/false);
}

TEST_F(PolygonTopologyAnalyzerTest, CrossingAndSpikeSelfIntersections) {
  auto bowtie = ReadPolygon("POLYGON ((0 0, 10 10, 10 0, 0 10, 0 0))");
  for (bool inverted_valid : {false, true}) {
    PolygonTopologyAnalyzer analyzer(bowtie.get(), inverted_valid);
    EXPECT_TRUE(analyzer.hasInvalidIntersection());
    EXPECT_EQ(analyzer.getInvalidCode(),
              +TopologyValidationError::eSelfIntersection);
    EXPECT_TRUE(analyzer.getInvalidLocation().equals2D(CoordinateXY(5, 5)));
  }

  auto spike =
      ReadPolygon("POLYGON ((0 0, 10 0, 10 10, 5 10, 5 2, 5 10, 0 10, 0 0))");
  {
    // With inverted rings disallowed, the vertex touch at the spike base (5,
    // 10) is reported as a ring self-intersection.
    PolygonTopologyAnalyzer spike_analyzer(spike.get(), false);
    EXPECT_TRUE(spike_analyzer.hasInvalidIntersection());
    EXPECT_EQ(spike_analyzer.getInvalidCode(),
              +TopologyValidationError::eRingSelfIntersection);
    EXPECT_TRUE(
        spike_analyzer.getInvalidLocation().equals2D(CoordinateXY(5, 10)));
  }
  {
    // Allowing self-touching rings skips the vertex touch and catches the
    // collinear zero-width spike overlap as eSelfIntersection.
    PolygonTopologyAnalyzer spike_analyzer(spike.get(), true);
    EXPECT_TRUE(spike_analyzer.hasInvalidIntersection());
    EXPECT_EQ(spike_analyzer.getInvalidCode(),
              +TopologyValidationError::eSelfIntersection);
    EXPECT_FALSE(spike_analyzer.getInvalidLocation().isNull());
  }

  auto collapsed_poly = ReadPolygon("POLYGON ((0 0, 10 0, 5 0, 0 0))");
  for (bool inverted_valid : {false, true}) {
    PolygonTopologyAnalyzer collapsed_poly_analyzer(collapsed_poly.get(),
                                                    inverted_valid);
    EXPECT_TRUE(collapsed_poly_analyzer.hasInvalidIntersection());
    EXPECT_EQ(collapsed_poly_analyzer.getInvalidCode(),
              +TopologyValidationError::eSelfIntersection);
    EXPECT_FALSE(collapsed_poly_analyzer.getInvalidLocation().isNull());
  }

  auto collapsed = ReadLinearRing("LINEARRING (0 0, 0 0, 10 0, 0 0)");
  PolygonTopologyAnalyzer collapsed_analyzer(collapsed.get(), false);
  EXPECT_TRUE(collapsed_analyzer.hasInvalidIntersection());
  EXPECT_EQ(collapsed_analyzer.getInvalidCode(),
            +TopologyValidationError::eSelfIntersection);
  EXPECT_FALSE(collapsed_analyzer.getInvalidLocation().isNull());
}

TEST_F(PolygonTopologyAnalyzerTest, RingSelfTouchRespectsInvertedRingFlag) {
  auto inverted_shell = ReadPolygon(
      "POLYGON ((0 0, 5 0, 3 5, 7 5, 5 0, 10 0, 10 10, 0 10, 0 0))");

  {
    PolygonTopologyAnalyzer analyzer(inverted_shell.get(), false);
    EXPECT_TRUE(analyzer.hasInvalidIntersection());
    EXPECT_EQ(analyzer.getInvalidCode(),
              +TopologyValidationError::eRingSelfIntersection);
    EXPECT_TRUE(analyzer.getInvalidLocation().equals2D(CoordinateXY(5, 0)));
  }
  {
    ExpectValidAndConnected(inverted_shell.get(),
                            /*is_inverted_ring_valid=*/true);
  }

  auto cw_inverted_shell = ReadPolygon(
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 5 0, 7 5, 3 5, 5 0, 0 0))");
  ExpectValidAndConnected(cw_inverted_shell.get(),
                          /*is_inverted_ring_valid=*/true);

  auto self_touch_ring = ReadLinearRing(
      "LINEARRING (0 0, 5 0, 3 5, 7 5, 5 0, 10 0, 10 10, 0 10, 0 0)");
  {
    PolygonTopologyAnalyzer analyzer(self_touch_ring.get(), false);
    EXPECT_TRUE(analyzer.hasInvalidIntersection());
    EXPECT_EQ(analyzer.getInvalidCode(),
              +TopologyValidationError::eRingSelfIntersection);
    EXPECT_TRUE(analyzer.getInvalidLocation().equals2D(CoordinateXY(5, 0)));
  }
  EXPECT_THROW(PolygonTopologyAnalyzer(self_touch_ring.get(), true),
               geos::util::IllegalStateException);
}

TEST_F(PolygonTopologyAnalyzerTest, HoleCrossingShellAndDuplicateRings) {
  auto crossing_hole = ReadPolygon(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (5 5, 15 5, 15 8, 5 8, 5 5))");
  PolygonTopologyAnalyzer crossing_analyzer(crossing_hole.get(), false);
  EXPECT_TRUE(crossing_analyzer.hasInvalidIntersection());
  EXPECT_EQ(crossing_analyzer.getInvalidCode(),
            +TopologyValidationError::eSelfIntersection);
  EXPECT_FALSE(crossing_analyzer.getInvalidLocation().isNull());

  auto dup_rings = ReadPolygon(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(0 0, 10 0, 10 10, 0 10, 0 0))");
  PolygonTopologyAnalyzer dup_analyzer(dup_rings.get(), false);
  EXPECT_TRUE(dup_analyzer.hasInvalidIntersection());
  EXPECT_EQ(dup_analyzer.getInvalidCode(),
            +TopologyValidationError::eSelfIntersection);
}

TEST_F(PolygonTopologyAnalyzerTest, MultiPolygonValidAndEmptyElements) {
  auto touching_mp = ReadWkt(
      "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "((10 5, 15 0, 15 10, 10 5)))");
  ExpectValidAndConnected(touching_mp.get(), /*is_inverted_ring_valid=*/false);
  ExpectValidAndConnected(touching_mp.get(), /*is_inverted_ring_valid=*/true);

  auto mp_with_empty =
      ReadWkt("MULTIPOLYGON (EMPTY, ((0 0, 10 0, 10 10, 0 10, 0 0)))");
  ExpectValidAndConnected(mp_with_empty.get(),
                          /*is_inverted_ring_valid=*/false);

  auto mp_with_holes = ReadWkt(
      "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0), (5 0, 7 4, 3 4, 5 0)), "
      "((20 0, 30 0, 30 10, 20 10, 20 0), (25 0, 27 4, 23 4, 25 0)))");
  ExpectValidAndConnected(mp_with_holes.get(),
                          /*is_inverted_ring_valid=*/false);
}

TEST_F(PolygonTopologyAnalyzerTest, MultiPolygonInvalidIntersections) {
  auto overlapping_mp = ReadWkt(
      "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "((5 5, 15 5, 15 15, 5 15, 5 5)))");
  PolygonTopologyAnalyzer overlap_analyzer(overlapping_mp.get(), false);
  EXPECT_TRUE(overlap_analyzer.hasInvalidIntersection());
  EXPECT_EQ(overlap_analyzer.getInvalidCode(),
            +TopologyValidationError::eSelfIntersection);

  auto shared_edge_mp = ReadWkt(
      "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "((10 0, 20 0, 20 10, 10 10, 10 0)))");
  PolygonTopologyAnalyzer edge_analyzer(shared_edge_mp.get(), false);
  EXPECT_TRUE(edge_analyzer.hasInvalidIntersection());
  EXPECT_EQ(edge_analyzer.getInvalidCode(),
            +TopologyValidationError::eSelfIntersection);

  auto vertex_cross_mp = ReadWkt(
      "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "((0 0, 5 5, 5 -5, 0 0)))");
  PolygonTopologyAnalyzer cross_analyzer(vertex_cross_mp.get(), false);
  EXPECT_TRUE(cross_analyzer.hasInvalidIntersection());
  EXPECT_EQ(cross_analyzer.getInvalidCode(),
            +TopologyValidationError::eSelfIntersection);
  const CoordinateXY& cross_loc = cross_analyzer.getInvalidLocation();
  EXPECT_TRUE(cross_loc.equals2D(CoordinateXY(0, 0)) ||
              cross_loc.equals2D(CoordinateXY(5, 0)));

  auto pure_vertex_cross_mp = ReadWkt(
      "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "((0 0, 5 5, 10 10, -5 15, 0 0)))");
  PolygonTopologyAnalyzer pure_cross_analyzer(pure_vertex_cross_mp.get(),
                                              false);
  EXPECT_TRUE(pure_cross_analyzer.hasInvalidIntersection());
  EXPECT_EQ(pure_cross_analyzer.getInvalidCode(),
            +TopologyValidationError::eSelfIntersection);
  const CoordinateXY& pure_loc = pure_cross_analyzer.getInvalidLocation();
  EXPECT_TRUE(pure_loc.equals2D(CoordinateXY(0, 0)) ||
              pure_loc.equals2D(CoordinateXY(10, 10)));
}

TEST_F(PolygonTopologyAnalyzerTest, DoubleTouchDisconnectsInterior) {
  auto hole_double_touch_shell = ReadPolygon(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (5 0, 8 5, 5 10, 2 5, 5 0))");
  PolygonTopologyAnalyzer analyzer(hole_double_touch_shell.get(), false);
  EXPECT_FALSE(analyzer.hasInvalidIntersection());
  // Double-touch is detected during construction before
  // isInteriorDisconnected().
  EXPECT_FALSE(analyzer.getDisconnectionLocation().isNull());
  EXPECT_TRUE(analyzer.isInteriorDisconnected());
  const CoordinateXY& loc = analyzer.getDisconnectionLocation();
  EXPECT_TRUE(loc.equals2D(CoordinateXY(5, 0)) ||
              loc.equals2D(CoordinateXY(5, 10)));

  auto holes_double_touch = ReadPolygon(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 5, 5 4, 8 5, 5 2, 2 5), "
      "(2 5, 5 8, 8 5, 5 6, 2 5))");
  PolygonTopologyAnalyzer holes_analyzer(holes_double_touch.get(), false);
  EXPECT_FALSE(holes_analyzer.hasInvalidIntersection());
  EXPECT_TRUE(holes_analyzer.isInteriorDisconnected());
  const CoordinateXY& h_loc = holes_analyzer.getDisconnectionLocation();
  EXPECT_TRUE(h_loc.equals2D(CoordinateXY(2, 5)) ||
              h_loc.equals2D(CoordinateXY(8, 5)));
}

TEST_F(PolygonTopologyAnalyzerTest, SelfTouchInteriorDisconnection) {
  auto pinched_shell =
      ReadPolygon("POLYGON ((0 0, 5 5, 10 0, 10 10, 5 5, 0 10, 0 0))");
  {
    PolygonTopologyAnalyzer analyzer(pinched_shell.get(),
                                     /*p_isInvertedRingValid=*/true);
    EXPECT_FALSE(analyzer.hasInvalidIntersection());
    EXPECT_TRUE(analyzer.getDisconnectionLocation().isNull());
    EXPECT_TRUE(analyzer.isInteriorDisconnected());
    EXPECT_TRUE(
        analyzer.getDisconnectionLocation().equals2D(CoordinateXY(5, 5)));
  }
  {
    PolygonTopologyAnalyzer direct(pinched_shell.get(),
                                   /*p_isInvertedRingValid=*/true);
    direct.checkInteriorDisconnectedBySelfTouch();
    EXPECT_TRUE(direct.getDisconnectionLocation().equals2D(CoordinateXY(5, 5)));
  }

  auto c_shaped_hole = ReadPolygon(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 8 2, 8 8, 2 8, 5 5, 7 6, 7 4, 5 5, 2 2))");
  PolygonTopologyAnalyzer hole_analyzer(c_shaped_hole.get(),
                                        /*p_isInvertedRingValid=*/true);
  EXPECT_FALSE(hole_analyzer.hasInvalidIntersection());
  EXPECT_TRUE(hole_analyzer.isInteriorDisconnected());
  EXPECT_TRUE(
      hole_analyzer.getDisconnectionLocation().equals2D(CoordinateXY(5, 5)));

  auto valid_pinched_hole = ReadPolygon(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(2 2, 5 5, 8 2, 8 8, 5 5, 2 8, 2 2))");
  ExpectValidAndConnected(valid_pinched_hole.get(),
                          /*is_inverted_ring_valid=*/true);

  auto mp_pinched = ReadWkt(
      "MULTIPOLYGON (((20 0, 30 0, 30 10, 20 10, 20 0)), "
      "((0 0, 5 5, 10 0, 10 10, 5 5, 0 10, 0 0)))");
  PolygonTopologyAnalyzer mp_pinched_analyzer(mp_pinched.get(),
                                              /*p_isInvertedRingValid=*/true);
  EXPECT_FALSE(mp_pinched_analyzer.hasInvalidIntersection());
  EXPECT_TRUE(mp_pinched_analyzer.isInteriorDisconnected());
  EXPECT_TRUE(mp_pinched_analyzer.isInteriorDisconnected());
  EXPECT_TRUE(mp_pinched_analyzer.getDisconnectionLocation().equals2D(
      CoordinateXY(5, 5)));
}

TEST_F(PolygonTopologyAnalyzerTest, HoleCycleDisconnectsInterior) {
  auto shell_two_hole_cycle = ReadPolygon(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(5 0, 8 2, 5 5, 2 2, 5 0), "
      "(5 5, 8 8, 5 10, 2 8, 5 5))");
  {
    PolygonTopologyAnalyzer analyzer(shell_two_hole_cycle.get(), false);
    EXPECT_FALSE(analyzer.hasInvalidIntersection());
    EXPECT_TRUE(analyzer.getDisconnectionLocation().isNull());
    EXPECT_TRUE(analyzer.isInteriorDisconnected());
    EXPECT_FALSE(analyzer.getDisconnectionLocation().isNull());
  }
  {
    PolygonTopologyAnalyzer direct(shell_two_hole_cycle.get(), false);
    direct.checkInteriorDisconnectedByHoleCycle();
    EXPECT_FALSE(direct.getDisconnectionLocation().isNull());
  }

  auto three_hole_cycle = ReadPolygon(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(4 4, 6 4, 5 2, 4 4), "
      "(6 4, 5 6, 8 6, 6 4), "
      "(5 6, 4 4, 2 6, 5 6))");
  PolygonTopologyAnalyzer three_analyzer(three_hole_cycle.get(), false);
  EXPECT_FALSE(three_analyzer.hasInvalidIntersection());
  EXPECT_TRUE(three_analyzer.isInteriorDisconnected());
  const CoordinateXY& t_loc = three_analyzer.getDisconnectionLocation();
  EXPECT_TRUE(t_loc.equals2D(CoordinateXY(4, 4)) ||
              t_loc.equals2D(CoordinateXY(6, 4)) ||
              t_loc.equals2D(CoordinateXY(5, 6)));

  auto inverted_plus_hole_cycle = ReadPolygon(
      "POLYGON ((0 0, 5 0, 4 2, 6 2, 5 0, 10 0, 10 10, 0 10, 0 0), "
      "(0 5, 3 4, 5 5, 3 6, 0 5), "
      "(5 5, 7 4, 10 5, 7 6, 5 5))");
  PolygonTopologyAnalyzer combo_analyzer(inverted_plus_hole_cycle.get(),
                                         /*p_isInvertedRingValid=*/true);
  EXPECT_FALSE(combo_analyzer.hasInvalidIntersection());
  EXPECT_TRUE(combo_analyzer.isInteriorDisconnected());
  EXPECT_FALSE(combo_analyzer.getDisconnectionLocation().isNull());

  auto mp_cycle = ReadWkt(
      "MULTIPOLYGON (((20 0, 30 0, 30 10, 20 10, 20 0)), "
      "((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(5 0, 8 2, 5 5, 2 2, 5 0), "
      "(5 5, 8 8, 5 10, 2 8, 5 5)))");
  PolygonTopologyAnalyzer mp_analyzer(mp_cycle.get(), false);
  EXPECT_FALSE(mp_analyzer.hasInvalidIntersection());
  EXPECT_TRUE(mp_analyzer.isInteriorDisconnected());
}

TEST_F(PolygonTopologyAnalyzerTest, TouchingHolesTreeRemainsConnected) {
  auto single_touch = ReadPolygon(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (5 0, 7 4, 3 4, 5 0))");
  ExpectValidAndConnected(single_touch.get(), /*is_inverted_ring_valid=*/false);

  auto two_hole_tree = ReadPolygon(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
      "(5 0, 7 2, 5 4, 3 2, 5 0), "
      "(5 4, 7 6, 5 8, 3 6, 5 4))");
  ExpectValidAndConnected(two_hole_tree.get(),
                          /*is_inverted_ring_valid=*/false);
}

TEST_F(PolygonTopologyAnalyzerTest, FindSelfIntersectionStaticMethod) {
  auto valid = ReadLinearRing("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  EXPECT_TRUE(
      PolygonTopologyAnalyzer::findSelfIntersection(valid.get()).isNull());

  auto empty = ReadLinearRing("LINEARRING EMPTY");
  EXPECT_TRUE(
      PolygonTopologyAnalyzer::findSelfIntersection(empty.get()).isNull());

  auto crossing = ReadLinearRing("LINEARRING (0 0, 10 10, 10 0, 0 10, 0 0)");
  EXPECT_TRUE(PolygonTopologyAnalyzer::findSelfIntersection(crossing.get())
                  .equals2D(CoordinateXY(5, 5)));

  auto self_touch = ReadLinearRing(
      "LINEARRING (0 0, 5 0, 3 5, 7 5, 5 0, 10 0, 10 10, 0 10, 0 0)");
  EXPECT_TRUE(PolygonTopologyAnalyzer::findSelfIntersection(self_touch.get())
                  .equals2D(CoordinateXY(5, 0)));

  auto dedup_crossing =
      ReadLinearRing("LINEARRING (0 0, 0 0, 10 10, 10 0, 0 10, 0 0)");
  EXPECT_TRUE(
      PolygonTopologyAnalyzer::findSelfIntersection(dedup_crossing.get())
          .equals2D(CoordinateXY(5, 5)));
}

TEST_F(PolygonTopologyAnalyzerTest, IsRingNestedInteriorExteriorAndEmpty) {
  auto target_ccw = ReadLinearRing("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  auto target_cw = ReadLinearRing("LINEARRING (0 0, 0 10, 10 10, 10 0, 0 0)");
  auto empty_target = ReadLinearRing("LINEARRING EMPTY");

  auto inside = ReadLinearRing("LINEARRING (2 2, 8 2, 8 8, 2 8, 2 2)");
  auto outside = ReadLinearRing("LINEARRING (12 2, 18 2, 18 8, 12 8, 12 2)");

  EXPECT_TRUE(
      PolygonTopologyAnalyzer::isRingNested(inside.get(), target_ccw.get()));
  EXPECT_TRUE(
      PolygonTopologyAnalyzer::isRingNested(inside.get(), target_cw.get()));
  EXPECT_FALSE(
      PolygonTopologyAnalyzer::isRingNested(outside.get(), target_ccw.get()));
  EXPECT_FALSE(
      PolygonTopologyAnalyzer::isRingNested(outside.get(), target_cw.get()));
  EXPECT_FALSE(
      PolygonTopologyAnalyzer::isRingNested(inside.get(), empty_target.get()));
}

TEST_F(PolygonTopologyAnalyzerTest, IsRingNestedBoundaryOnSegmentInterior) {
  auto target_ccw = ReadLinearRing("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  auto target_cw = ReadLinearRing("LINEARRING (0 0, 0 10, 10 10, 10 0, 0 0)");

  auto inside_at_mid = ReadLinearRing("LINEARRING (5 0, 7 4, 3 4, 5 0)");
  auto outside_at_mid = ReadLinearRing("LINEARRING (5 0, 3 -4, 7 -4, 5 0)");

  EXPECT_TRUE(PolygonTopologyAnalyzer::isRingNested(inside_at_mid.get(),
                                                    target_ccw.get()));
  EXPECT_TRUE(PolygonTopologyAnalyzer::isRingNested(inside_at_mid.get(),
                                                    target_cw.get()));
  EXPECT_FALSE(PolygonTopologyAnalyzer::isRingNested(outside_at_mid.get(),
                                                     target_ccw.get()));
  EXPECT_FALSE(PolygonTopologyAnalyzer::isRingNested(outside_at_mid.get(),
                                                     target_cw.get()));

  auto slanted_ccw = ReadLinearRing("LINEARRING (0 0, 10 0, 10 10, 0 0)");
  auto slanted_cw = ReadLinearRing("LINEARRING (0 0, 10 10, 10 0, 0 0)");
  auto slanted_inside = ReadLinearRing("LINEARRING (5 5, 8 4, 8 2, 5 5)");
  auto slanted_outside = ReadLinearRing("LINEARRING (5 5, 2 8, 4 8, 5 5)");
  EXPECT_TRUE(PolygonTopologyAnalyzer::isRingNested(slanted_inside.get(),
                                                    slanted_ccw.get()));
  EXPECT_TRUE(PolygonTopologyAnalyzer::isRingNested(slanted_inside.get(),
                                                    slanted_cw.get()));
  EXPECT_FALSE(PolygonTopologyAnalyzer::isRingNested(slanted_outside.get(),
                                                     slanted_ccw.get()));
  EXPECT_FALSE(PolygonTopologyAnalyzer::isRingNested(slanted_outside.get(),
                                                     slanted_cw.get()));
}

TEST_F(PolygonTopologyAnalyzerTest,
       IsRingNestedAtVerticesWithRepeatedTargetPoints) {
  auto target_ccw =
      ReadLinearRing("LINEARRING (0 0, 10 0, 10 0, 10 10, 0 10, 0 10, 0 0)");
  auto target_cw =
      ReadLinearRing("LINEARRING (0 0, 0 10, 0 10, 10 10, 10 0, 10 0, 0 0)");

  auto inside_v0 = ReadLinearRing("LINEARRING (0 0, 4 2, 2 4, 0 0)");
  auto outside_v0 = ReadLinearRing("LINEARRING (0 0, -4 -2, -2 -4, 0 0)");

  auto inside_v1 = ReadLinearRing("LINEARRING (10 0, 6 2, 8 4, 10 0)");
  auto outside_v1 = ReadLinearRing("LINEARRING (10 0, 14 2, 12 -4, 10 0)");

  auto inside_v_last = ReadLinearRing("LINEARRING (0 10, 2 6, 4 8, 0 10)");
  auto outside_v_last = ReadLinearRing("LINEARRING (0 10, -2 6, -4 8, 0 10)");

  for (const LinearRing* target : {target_ccw.get(), target_cw.get()}) {
    EXPECT_TRUE(PolygonTopologyAnalyzer::isRingNested(inside_v0.get(), target));
    EXPECT_FALSE(
        PolygonTopologyAnalyzer::isRingNested(outside_v0.get(), target));
    EXPECT_TRUE(PolygonTopologyAnalyzer::isRingNested(inside_v1.get(), target));
    EXPECT_FALSE(
        PolygonTopologyAnalyzer::isRingNested(outside_v1.get(), target));
    EXPECT_TRUE(
        PolygonTopologyAnalyzer::isRingNested(inside_v_last.get(), target));
    EXPECT_FALSE(
        PolygonTopologyAnalyzer::isRingNested(outside_v_last.get(), target));
  }

  auto target_repeated_start =
      ReadLinearRing("LINEARRING (0 0, 0 0, 10 0, 10 10, 0 10, 0 0)");
  EXPECT_TRUE(PolygonTopologyAnalyzer::isRingNested(
      inside_v0.get(), target_repeated_start.get()));
  EXPECT_FALSE(PolygonTopologyAnalyzer::isRingNested(
      outside_v0.get(), target_repeated_start.get()));
}

TEST_F(PolygonTopologyAnalyzerTest, IsRingNestedTestRingWithRepeatedVertices) {
  auto target_ccw = ReadLinearRing("LINEARRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  auto target_cw = ReadLinearRing("LINEARRING (0 0, 0 10, 10 10, 10 0, 0 0)");

  auto inside_repeated = ReadLinearRing("LINEARRING (0 0, 0 0, 4 2, 2 4, 0 0)");
  auto outside_repeated =
      ReadLinearRing("LINEARRING (0 0, 0 0, -4 -2, -2 -4, 0 0)");

  EXPECT_TRUE(PolygonTopologyAnalyzer::isRingNested(inside_repeated.get(),
                                                    target_ccw.get()));
  EXPECT_TRUE(PolygonTopologyAnalyzer::isRingNested(inside_repeated.get(),
                                                    target_cw.get()));
  EXPECT_FALSE(PolygonTopologyAnalyzer::isRingNested(outside_repeated.get(),
                                                     target_ccw.get()));
  EXPECT_FALSE(PolygonTopologyAnalyzer::isRingNested(outside_repeated.get(),
                                                     target_cw.get()));

  auto all_equal_on_boundary =
      ReadLinearRing("LINEARRING (0 0, 0 0, 0 0, 0 0)");
  EXPECT_THROW(PolygonTopologyAnalyzer::isRingNested(
                   all_equal_on_boundary.get(), target_ccw.get()),
               geos::util::IllegalArgumentException);
}

}  // namespace
}  // namespace geos_google
