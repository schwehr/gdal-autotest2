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

#include "third_party/geos/include/geos/operation/overlayng/EdgeNodingBuilder.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"
#include "third_party/geos/include/geos/noding/Noder.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/noding/snapround/SnapRoundingNoder.h"
#include "third_party/geos/include/geos/operation/overlayng/Edge.h"
#include "third_party/geos/include/geos/operation/overlayng/EdgeSourceInfo.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayLabel.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::Location;
using geos::geom::Position;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::noding::NodedSegmentString;
using geos::noding::Noder;
using geos::noding::SegmentString;
using geos::noding::snapround::SnapRoundingNoder;
using geos::operation::overlayng::Edge;
using geos::operation::overlayng::EdgeNodingBuilder;
using geos::operation::overlayng::EdgeSourceInfo;
using geos::operation::overlayng::OverlayLabel;
using geos::util::IllegalArgumentException;

// Custom Noder that keeps geom 0 segment strings and replaces geom 1 with
// collapsed segment strings to exercise Edge::isCollapsed filtering.
class CollapsedSubstringsNoder : public Noder {
 public:
  void computeNodes(std::vector<SegmentString*>* seg_strings) override {
    seg_strings_ = seg_strings;
  }

  std::vector<SegmentString*>* getNodedSubstrings() const override {
    auto* result = new std::vector<SegmentString*>();
    const EdgeSourceInfo* geom1_info = nullptr;
    for (SegmentString* ss : *seg_strings_) {
      const auto* info = static_cast<const EdgeSourceInfo*>(ss->getData());
      if (info->getIndex() == 0) {
        result->push_back(new NodedSegmentString(
            ss->getCoordinates()->clone().release(), false, false, info));
      } else {
        geom1_info = info;
      }
    }
    if (geom1_info != nullptr) {
      auto two_equal = std::make_unique<CoordinateSequence>(
          std::initializer_list<CoordinateXY>{{1.0, 1.0}, {1.0, 1.0}});
      result->push_back(new NodedSegmentString(two_equal.release(), false,
                                               false, geom1_info));

      auto single_pt = std::make_unique<CoordinateSequence>(
          std::initializer_list<CoordinateXY>{{2.0, 2.0}});
      result->push_back(new NodedSegmentString(single_pt.release(), false,
                                               false, geom1_info));

      auto tail_equal = std::make_unique<CoordinateSequence>(
          std::initializer_list<CoordinateXY>{
              {0.0, 0.0}, {5.0, 0.0}, {5.0, 0.0}});
      result->push_back(new NodedSegmentString(tail_equal.release(), false,
                                               false, geom1_info));
    }
    return result;
  }

 private:
  const std::vector<SegmentString*>* seg_strings_ = nullptr;
};

TEST(EdgeNodingBuilderTest, FloatingPrecisionNodesCrossingLinesAndNullPm) {
  auto gf = GeometryFactory::create();
  WKTReader wkt(*gf);

  auto line0 = wkt.read("LINESTRING (0 0, 10 10)");
  auto line1 = wkt.read("LINESTRING (0 10, 10 0)");

  const PrecisionModel pm(PrecisionModel::FLOATING);
  EdgeNodingBuilder builder(&pm, nullptr);
  const std::vector<Edge*> edges = builder.build(line0.get(), line1.get());

  EXPECT_EQ(edges.size(), 4u);
  EXPECT_TRUE(builder.hasEdgesFor(0));
  EXPECT_TRUE(builder.hasEdgesFor(1));

  int geom0_count = 0;
  int geom1_count = 0;
  for (const Edge* edge : edges) {
    ASSERT_EQ(edge->size(), 2u);
    const bool touches_center = edge->getCoordinate(0).equals2D({5.0, 5.0}) ||
                                edge->getCoordinate(1).equals2D({5.0, 5.0});
    EXPECT_TRUE(touches_center);

    OverlayLabel label;
    edge->populateLabel(label);
    if (label.isLine(0)) {
      ++geom0_count;
      EXPECT_TRUE(label.isNotPart(1));
    } else if (label.isLine(1)) {
      ++geom1_count;
      EXPECT_TRUE(label.isNotPart(0));
    }
  }
  EXPECT_EQ(geom0_count, 2);
  EXPECT_EQ(geom1_count, 2);

  // Also test nullptr PrecisionModel and nullptr geom1.
  EdgeNodingBuilder null_pm_builder(nullptr, nullptr);
  const std::vector<Edge*> single_edges =
      null_pm_builder.build(line0.get(), nullptr);
  EXPECT_EQ(single_edges.size(), 1u);
  EXPECT_TRUE(null_pm_builder.hasEdgesFor(0));
  EXPECT_FALSE(null_pm_builder.hasEdgesFor(1));
}

TEST(EdgeNodingBuilderTest, FixedPrecisionSnapRoundingNodesAndCollapses) {
  auto gf = GeometryFactory::create();
  WKTReader wkt(*gf);
  const PrecisionModel pm(1.0);

  // Crossing segments snap onto integer grid coordinates.
  {
    auto line0 = wkt.read("LINESTRING (0.1 0.1, 4.1 4.1)");
    auto line1 = wkt.read("LINESTRING (0.1 4.1, 4.1 0.1)");
    EdgeNodingBuilder builder(&pm, nullptr);
    const std::vector<Edge*> edges = builder.build(line0.get(), line1.get());

    EXPECT_EQ(edges.size(), 4u);
    EXPECT_TRUE(builder.hasEdgesFor(0));
    EXPECT_TRUE(builder.hasEdgesFor(1));
    for (const Edge* edge : edges) {
      for (std::size_t i = 0; i < edge->size(); ++i) {
        const auto& c = edge->getCoordinate(i);
        EXPECT_DOUBLE_EQ(c.x, std::round(c.x));
        EXPECT_DOUBLE_EQ(c.y, std::round(c.y));
      }
    }
  }

  // Tiny geometry below grid scale collapses completely.
  {
    auto tiny_line = wkt.read("LINESTRING (0.1 0.1, 0.2 0.2)");
    EdgeNodingBuilder builder(&pm, nullptr);
    const std::vector<Edge*> edges = builder.build(tiny_line.get(), nullptr);
    EXPECT_TRUE(edges.empty());
    EXPECT_FALSE(builder.hasEdgesFor(0));
    EXPECT_FALSE(builder.hasEdgesFor(1));
  }

  // Thin spike polygon collapses into a single merged area collapse edge.
  {
    auto thin_poly = wkt.read("POLYGON ((0 0, 10 0, 10 0.1, 0 0))");
    EdgeNodingBuilder builder(&pm, nullptr);
    const std::vector<Edge*> edges = builder.build(thin_poly.get(), nullptr);
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_TRUE(builder.hasEdgesFor(0));
    OverlayLabel label;
    edges[0]->populateLabel(label);
    EXPECT_TRUE(label.isCollapse(0));
  }
}

TEST(EdgeNodingBuilderTest, CustomNoderOverridesPrecisionModel) {
  auto gf = GeometryFactory::create();
  WKTReader wkt(*gf);

  const PrecisionModel snap_pm(1.0);
  SnapRoundingNoder custom_noder(&snap_pm);

  auto line = wkt.read("LINESTRING (0.2 0.3, 4.8 4.7)");
  EdgeNodingBuilder builder(nullptr, &custom_noder);
  const std::vector<Edge*> edges = builder.build(line.get(), nullptr);

  ASSERT_EQ(edges.size(), 1u);
  EXPECT_TRUE(builder.hasEdgesFor(0));
  EXPECT_FALSE(builder.hasEdgesFor(1));
  EXPECT_TRUE(edges[0]->getCoordinate(0).equals2D({0.0, 0.0}));
  EXPECT_TRUE(edges[0]->getCoordinate(1).equals2D({5.0, 5.0}));
}

TEST(EdgeNodingBuilderTest, CreateEdgesSkipsCollapsedSubstringsFromNoder) {
  auto gf = GeometryFactory::create();
  WKTReader wkt(*gf);

  auto line0 = wkt.read("LINESTRING (0 0, 10 0)");
  auto line1 = wkt.read("LINESTRING (0 5, 10 5)");

  CollapsedSubstringsNoder custom_noder;
  EdgeNodingBuilder builder(nullptr, &custom_noder);
  const std::vector<Edge*> edges = builder.build(line0.get(), line1.get());

  ASSERT_EQ(edges.size(), 1u);
  EXPECT_TRUE(builder.hasEdgesFor(0));
  EXPECT_FALSE(builder.hasEdgesFor(1));

  OverlayLabel label;
  edges[0]->populateLabel(label);
  EXPECT_TRUE(label.isLine(0));
  EXPECT_TRUE(label.isNotPart(1));
}

TEST(EdgeNodingBuilderTest, EmptyPointsAndUnsupportedTypesProduceNoEdges) {
  auto gf = GeometryFactory::create();
  WKTReader wkt(*gf);
  const PrecisionModel pm(PrecisionModel::FLOATING);

  auto empty_line = wkt.read("LINESTRING EMPTY");
  auto empty_poly = wkt.read("POLYGON EMPTY");
  auto empty_gc = wkt.read("GEOMETRYCOLLECTION EMPTY");
  auto point = wkt.read("POINT (1 2)");
  auto multipoint = wkt.read("MULTIPOINT ((1 2), (3 4))");
  auto cs = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}});
  auto circular = gf->createCircularString(std::move(cs));

  const std::vector<const Geometry*> no_edge_geoms = {
      empty_line.get(), empty_poly.get(), empty_gc.get(),
      point.get(),      multipoint.get(), circular.get(),
  };

  for (const Geometry* g : no_edge_geoms) {
    EdgeNodingBuilder builder(&pm, nullptr);
    const std::vector<Edge*> edges = builder.build(g, nullptr);
    EXPECT_TRUE(edges.empty());
    EXPECT_FALSE(builder.hasEdgesFor(0));
    EXPECT_FALSE(builder.hasEdgesFor(1));
  }
}

TEST(EdgeNodingBuilderTest, LinearRingMultiLineStringAndMultiPolygonAreAdded) {
  auto gf = GeometryFactory::create();
  WKTReader wkt(*gf);
  const PrecisionModel pm(PrecisionModel::FLOATING);

  auto ring = wkt.read<LinearRing>("LINEARRING (0 0, 4 0, 4 4, 0 4, 0 0)");
  auto mls = wkt.read("MULTILINESTRING ((2 -1, 2 5), EMPTY, (10 10, 12 10))");

  {
    EdgeNodingBuilder builder(&pm, nullptr);
    const std::vector<Edge*> edges = builder.build(ring.get(), mls.get());
    EXPECT_FALSE(edges.empty());
    EXPECT_TRUE(builder.hasEdgesFor(0));
    EXPECT_TRUE(builder.hasEdgesFor(1));

    // Direct LinearRing is routed through addLine -> linear dimension.
    bool saw_geom0_line = false;
    for (const Edge* edge : edges) {
      OverlayLabel label;
      edge->populateLabel(label);
      if (label.isLine(0)) {
        saw_geom0_line = true;
      }
    }
    EXPECT_TRUE(saw_geom0_line);
  }

  auto mpoly = wkt.read(
      "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "((10 10, 12 10, 12 12, 10 12, 10 10)))");
  {
    EdgeNodingBuilder builder(&pm, nullptr);
    const std::vector<Edge*> edges = builder.build(mpoly.get(), nullptr);
    EXPECT_EQ(edges.size(), 2u);
    EXPECT_TRUE(builder.hasEdgesFor(0));
    EXPECT_FALSE(builder.hasEdgesFor(1));
    for (const Edge* edge : edges) {
      OverlayLabel label;
      edge->populateLabel(label);
      EXPECT_TRUE(label.isBoundary(0));
    }
  }
}

TEST(EdgeNodingBuilderTest, GeometryCollectionHomogeneousAndMixedDimension) {
  auto gf = GeometryFactory::create();
  WKTReader wkt(*gf);
  const PrecisionModel pm(PrecisionModel::FLOATING);

  // Homogeneous collection of two crossing linestrings.
  auto homo_gc = wkt.read(
      "GEOMETRYCOLLECTION (LINESTRING (0 0, 4 0), LINESTRING (2 -2, 2 2))");
  {
    EdgeNodingBuilder builder(&pm, nullptr);
    const std::vector<Edge*> edges = builder.build(homo_gc.get(), nullptr);
    EXPECT_EQ(edges.size(), 4u);
    EXPECT_TRUE(builder.hasEdgesFor(0));
    EXPECT_FALSE(builder.hasEdgesFor(1));
  }

  // Mixed point + linestring throws IllegalArgumentException.
  auto mixed_pl =
      wkt.read("GEOMETRYCOLLECTION (POINT (0 0), LINESTRING (0 0, 1 1))");
  {
    EdgeNodingBuilder builder(&pm, nullptr);
    EXPECT_THROW(builder.build(mixed_pl.get(), nullptr),
                 IllegalArgumentException);
  }

  // Mixed linestring + polygon throws IllegalArgumentException.
  auto mixed_la = wkt.read(
      "GEOMETRYCOLLECTION (LINESTRING (0 0, 1 1), "
      "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)))");
  {
    EdgeNodingBuilder builder(&pm, nullptr);
    EXPECT_THROW(builder.build(mixed_la.get(), nullptr),
                 IllegalArgumentException);
  }
}

TEST(EdgeNodingBuilderTest, PolygonRingOrientationsComputeDepthDelta) {
  auto gf = GeometryFactory::create();
  WKTReader wkt(*gf);
  const PrecisionModel pm(PrecisionModel::FLOATING);

  // Polygon 0: CW shell (canonical, depthDelta=1) and CCW hole (canonical,
  // depthDelta=1).
  auto poly0 = wkt.read(
      "POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0), "
      "(2 2, 8 2, 8 8, 2 8, 2 2))");
  // Polygon 1: CCW shell (non-canonical, depthDelta=-1) and CW hole
  // (non-canonical, depthDelta=-1).
  auto poly1 = wkt.read(
      "POLYGON ((20 0, 30 0, 30 10, 20 10, 20 0), "
      "(22 2, 22 8, 28 8, 28 2, 22 2))");

  EdgeNodingBuilder builder(&pm, nullptr);
  const std::vector<Edge*> edges = builder.build(poly0.get(), poly1.get());
  ASSERT_EQ(edges.size(), 4u);

  for (const Edge* edge : edges) {
    OverlayLabel label;
    edge->populateLabel(label);
    if (!label.isNotPart(0)) {
      EXPECT_TRUE(label.isBoundary(0));
      // Both CW shell and CCW hole produce canonical depthDelta = 1.
      EXPECT_EQ(label.getLocation(0, +Position::LEFT, true),
                Location::EXTERIOR);
      EXPECT_EQ(label.getLocation(0, +Position::RIGHT, true),
                Location::INTERIOR);
    } else {
      EXPECT_TRUE(label.isBoundary(1));
      // Both CCW shell and CW hole produce reversed depthDelta = -1.
      EXPECT_EQ(label.getLocation(1, +Position::LEFT, true),
                Location::INTERIOR);
      EXPECT_EQ(label.getLocation(1, +Position::RIGHT, true),
                Location::EXTERIOR);
    }
  }
}

TEST(EdgeNodingBuilderTest, EmptyHoleAndDegenerateRingsAndLinesAreSkipped) {
  auto gf = GeometryFactory::create();
  const PrecisionModel pm(PrecisionModel::FLOATING);

  // Polygon with valid shell and an empty hole ring.
  auto shell_cs =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});
  auto shell = gf->createLinearRing(std::move(shell_cs));
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(gf->createLinearRing());
  auto poly_with_empty_hole =
      gf->createPolygon(std::move(shell), std::move(holes));

  // Degenerate polygon whose shell has all identical coordinates.
  auto degen_cs =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {5.0, 5.0}, {5.0, 5.0}, {5.0, 5.0}, {5.0, 5.0}});
  auto degen_ring = gf->createLinearRing(std::move(degen_cs));
  auto degen_poly = gf->createPolygon(std::move(degen_ring));

  {
    EdgeNodingBuilder builder(&pm, nullptr);
    const std::vector<Edge*> edges =
        builder.build(poly_with_empty_hole.get(), degen_poly.get());
    EXPECT_EQ(edges.size(), 1u);
    EXPECT_TRUE(builder.hasEdgesFor(0));
    EXPECT_FALSE(builder.hasEdgesFor(1));
  }

  // Degenerate LineString with repeated points collapses to < 2 points.
  WKTReader wkt(*gf);
  auto degen_line = wkt.read("LINESTRING (5 5, 5 5, 5 5)");
  {
    EdgeNodingBuilder builder(&pm, nullptr);
    const std::vector<Edge*> edges = builder.build(degen_line.get(), nullptr);
    EXPECT_TRUE(edges.empty());
    EXPECT_FALSE(builder.hasEdgesFor(0));
  }
}

TEST(EdgeNodingBuilderTest, ClipEnvelopeClipsPolygonsHolesAndTouches) {
  auto gf = GeometryFactory::create();
  WKTReader wkt(*gf);
  const PrecisionModel pm(PrecisionModel::FLOATING);
  const Envelope clip_env(0.0, 10.0, 0.0, 10.0);

  // Completely disjoint geometry is skipped before adding edges.
  {
    auto outside_line = wkt.read("LINESTRING (20 20, 30 30)");
    auto outside_poly =
        wkt.read("POLYGON ((20 20, 30 20, 30 30, 20 30, 20 20))");
    EdgeNodingBuilder builder(&pm, nullptr);
    builder.setClipEnvelope(&clip_env);
    const std::vector<Edge*> edges =
        builder.build(outside_line.get(), outside_poly.get());
    EXPECT_TRUE(edges.empty());
    EXPECT_FALSE(builder.hasEdgesFor(0));
    EXPECT_FALSE(builder.hasEdgesFor(1));
  }

  // Polygon covered completely by clipEnv takes the no-clip repeated-point
  // path; polygon extending outside clipEnv clips shell and drops disjoint
  // hole.
  {
    auto covered_poly = wkt.read("POLYGON ((1 1, 9 1, 9 9, 1 9, 1 1))");
    auto large_poly = wkt.read(
        "POLYGON ((-5 -5, 20 -5, 20 20, -5 20, -5 -5), "
        "(2 2, 2 4, 4 4, 4 2, 2 2), "
        "(12 12, 12 15, 15 15, 15 12, 12 12))");
    EdgeNodingBuilder builder(&pm, nullptr);
    builder.setClipEnvelope(&clip_env);
    const std::vector<Edge*> edges =
        builder.build(covered_poly.get(), large_poly.get());

    // Expected: 1 edge for covered_poly, 1 clipped shell edge + 1 inner hole
    // edge for large_poly (disjoint hole (12..15) is dropped).
    EXPECT_EQ(edges.size(), 3u);
    EXPECT_TRUE(builder.hasEdgesFor(0));
    EXPECT_TRUE(builder.hasEdgesFor(1));
    for (const Edge* edge : edges) {
      for (std::size_t i = 0; i < edge->size(); ++i) {
        EXPECT_TRUE(clip_env.covers(&edge->getCoordinate(i)));
      }
    }
  }

  // Polygon touching clipEnv only at corner (0, 0) clips to empty sequence.
  {
    auto corner_touch =
        wkt.read("POLYGON ((-10 -10, 0 -10, 0 0, -10 0, -10 -10))");
    EdgeNodingBuilder builder(&pm, nullptr);
    builder.setClipEnvelope(&clip_env);
    const std::vector<Edge*> edges = builder.build(corner_touch.get(), nullptr);
    EXPECT_TRUE(edges.empty());
    EXPECT_FALSE(builder.hasEdgesFor(0));
  }
}

TEST(EdgeNodingBuilderTest, ClipEnvelopeLineLimiterThresholdAndSections) {
  auto gf = GeometryFactory::create();
  const PrecisionModel pm(PrecisionModel::FLOATING);
  const Envelope clip_env(0.0, 10.0, 0.0, 10.0);

  // Line with exactly MIN_LIMIT_PTS (20) points extending outside clipEnv is
  // NOT limited.
  auto cs_20 = std::make_unique<CoordinateSequence>(0u, false, false);
  for (int i = 0; i < 20; ++i) {
    cs_20->add(-5.0 + 1.0 * i, 5.0);
  }
  auto line_20 = gf->createLineString(std::move(cs_20));

  // Line with 22 points completely inside clipEnv is NOT limited.
  auto cs_inside = std::make_unique<CoordinateSequence>(0u, false, false);
  for (int i = 0; i < 22; ++i) {
    cs_inside->add(1.0 + 0.25 * i, 3.0);
  }
  auto line_inside = gf->createLineString(std::move(cs_inside));

  {
    EdgeNodingBuilder builder(&pm, nullptr);
    builder.setClipEnvelope(&clip_env);
    const std::vector<Edge*> edges =
        builder.build(line_20.get(), line_inside.get());
    ASSERT_EQ(edges.size(), 2u);
    EXPECT_TRUE(builder.hasEdgesFor(0));
    EXPECT_TRUE(builder.hasEdgesFor(1));
    EXPECT_EQ(edges[0]->size(), 20u);
    EXPECT_EQ(edges[1]->size(), 22u);
    OverlayLabel label0;
    OverlayLabel label1;
    edges[0]->populateLabel(label0);
    edges[1]->populateLabel(label1);
    EXPECT_TRUE(label0.isLine(0));
    EXPECT_TRUE(label1.isLine(1));
  }

  // Line with > 20 points passing through clipEnv twice is split into 2
  // limited sections excluding distant outside vertices.
  auto cs_multi_section =
      std::make_unique<CoordinateSequence>(0u, false, false);
  for (int x = -12; x <= 14; x += 2) {
    cs_multi_section->add(static_cast<double>(x), 2.0);
  }
  cs_multi_section->add(16.0, 4.0);
  cs_multi_section->add(16.0, 6.0);
  for (int x = 14; x >= -12; x -= 2) {
    cs_multi_section->add(static_cast<double>(x), 8.0);
  }
  ASSERT_GT(cs_multi_section->size(), 20u);
  auto line_multi = gf->createLineString(std::move(cs_multi_section));

  {
    EdgeNodingBuilder builder(&pm, nullptr);
    builder.setClipEnvelope(&clip_env);
    const std::vector<Edge*> edges = builder.build(line_multi.get(), nullptr);
    EXPECT_EQ(edges.size(), 2u);
    EXPECT_TRUE(builder.hasEdgesFor(0));
    for (const Edge* edge : edges) {
      for (std::size_t i = 0; i < edge->size(); ++i) {
        EXPECT_GE(edge->getCoordinate(i).x, -2.0);
        EXPECT_LE(edge->getCoordinate(i).x, 12.0);
      }
    }
  }

  // L-shaped line with > 20 points whose envelope intersects clipEnv while no
  // segment intersects clipEnv yields 0 sections.
  auto cs_l_outside = std::make_unique<CoordinateSequence>(0u, false, false);
  for (int x = -5; x <= 15; x += 2) {
    cs_l_outside->add(static_cast<double>(x), -5.0);
  }
  for (int y = -3; y <= 17; y += 2) {
    cs_l_outside->add(15.0, static_cast<double>(y));
  }
  ASSERT_GT(cs_l_outside->size(), 20u);
  auto line_l_outside = gf->createLineString(std::move(cs_l_outside));

  {
    EdgeNodingBuilder builder(&pm, nullptr);
    builder.setClipEnvelope(&clip_env);
    const std::vector<Edge*> edges =
        builder.build(line_l_outside.get(), nullptr);
    EXPECT_TRUE(edges.empty());
    EXPECT_FALSE(builder.hasEdgesFor(0));
  }
}

TEST(EdgeNodingBuilderTest, PreservesHasZAndHasMFlagsFromEitherInput) {
  auto gf = GeometryFactory::create();
  WKTReader wkt(*gf);
  const PrecisionModel pm(PrecisionModel::FLOATING);

  auto line_xy = wkt.read("LINESTRING (0 0, 10 0)");
  auto line_z = wkt.read("LINESTRING Z (5 -5 10, 5 5 20)");
  auto line_m = wkt.read("LINESTRING M (0 0 1, 10 0 2)");
  auto line_m_cross = wkt.read("LINESTRING M (5 -5 7, 5 5 8)");

  // Both XY -> hasZ == false, hasM == false.
  {
    EdgeNodingBuilder builder(&pm, nullptr);
    const std::vector<Edge*> edges = builder.build(line_xy.get(), nullptr);
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_FALSE(edges[0]->getCoordinatesRO()->hasZ());
    EXPECT_FALSE(edges[0]->getCoordinatesRO()->hasM());
  }

  // geom0 XY, geom1 Z -> hasZ == true.
  {
    EdgeNodingBuilder builder(&pm, nullptr);
    const std::vector<Edge*> edges = builder.build(line_xy.get(), line_z.get());
    ASSERT_EQ(edges.size(), 4u);
    for (const Edge* edge : edges) {
      EXPECT_TRUE(edge->getCoordinatesRO()->hasZ());
      EXPECT_FALSE(edge->getCoordinatesRO()->hasM());
    }
  }

  // geom0 Z, geom1 XY -> hasZ == true.
  {
    EdgeNodingBuilder builder(&pm, nullptr);
    const std::vector<Edge*> edges = builder.build(line_z.get(), line_xy.get());
    ASSERT_EQ(edges.size(), 4u);
    for (const Edge* edge : edges) {
      EXPECT_TRUE(edge->getCoordinatesRO()->hasZ());
      EXPECT_FALSE(edge->getCoordinatesRO()->hasM());
    }
  }

  // geom0 M, geom1 nullptr -> hasM == true, M values preserved.
  {
    EdgeNodingBuilder builder(&pm, nullptr);
    const std::vector<Edge*> edges = builder.build(line_m.get(), nullptr);
    ASSERT_EQ(edges.size(), 1u);
    const CoordinateSequence* coords = edges[0]->getCoordinatesRO();
    EXPECT_FALSE(coords->hasZ());
    EXPECT_TRUE(coords->hasM());
    EXPECT_DOUBLE_EQ(coords->getOrdinate(0, CoordinateSequence::M), 1.0);
    EXPECT_DOUBLE_EQ(coords->getOrdinate(1, CoordinateSequence::M), 2.0);
  }

  // geom0 XY, geom1 M -> hasM == true.
  {
    EdgeNodingBuilder builder(&pm, nullptr);
    const std::vector<Edge*> edges =
        builder.build(line_xy.get(), line_m_cross.get());
    ASSERT_EQ(edges.size(), 4u);
    for (const Edge* edge : edges) {
      EXPECT_FALSE(edge->getCoordinatesRO()->hasZ());
      EXPECT_TRUE(edge->getCoordinatesRO()->hasM());
    }
  }

  // geom0 Z, geom1 M -> both hasZ and hasM == true.
  {
    EdgeNodingBuilder builder(&pm, nullptr);
    const std::vector<Edge*> edges = builder.build(line_z.get(), line_m.get());
    ASSERT_EQ(edges.size(), 4u);
    for (const Edge* edge : edges) {
      EXPECT_TRUE(edge->getCoordinatesRO()->hasZ());
      EXPECT_TRUE(edge->getCoordinatesRO()->hasM());
    }
  }
}

}  // namespace
}  // namespace geos_google
