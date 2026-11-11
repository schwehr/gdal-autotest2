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

#include "third_party/geos/include/geos/operation/buffer/BufferBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/noding/IntersectionAdder.h"
#include "third_party/geos/include/geos/noding/MCIndexNoder.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"
#include "third_party/geos/include/geos/noding/Noder.h"
#include "third_party/geos/include/geos/noding/ScaledNoder.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/noding/ValidatingNoder.h"
#include "third_party/geos/include/geos/noding/snapround/SnapRoundingNoder.h"
#include "third_party/geos/include/geos/operation/buffer/BufferParameters.h"
#include "third_party/geos/include/geos/util/GEOSException.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/Interrupt.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::algorithm::LineIntersector;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::geomgraph::Label;
using geos::io::WKTReader;
using geos::noding::IntersectionAdder;
using geos::noding::MCIndexNoder;
using geos::noding::NodedSegmentString;
using geos::noding::Noder;
using geos::noding::ScaledNoder;
using geos::noding::SegmentString;
using geos::noding::ValidatingNoder;
using geos::noding::snapround::SnapRoundingNoder;
using geos::operation::buffer::BufferBuilder;
using geos::operation::buffer::BufferParameters;
using geos::util::CurrentThreadInterrupt;
using geos::util::GEOSException;
using geos::util::IllegalArgumentException;
using geos::util::TopologyException;

// Custom noder that injects a degenerate 2-point collapsed segment string
// alongside normally noded edges so RepeatedPointRemover collapses it to < 2
// points.
class InjectCollapsedEdgeNoder final : public Noder {
 public:
  explicit InjectCollapsedEdgeNoder(const PrecisionModel* pm)
      : li_(pm), adder_(li_), inner_(&adder_) {}

  void computeNodes(std::vector<SegmentString*>* seg_strings) override {
    inner_.computeNodes(seg_strings);
  }

  std::vector<SegmentString*>* getNodedSubstrings() const override {
    auto* out = inner_.getNodedSubstrings();
    auto* collapsed_seq = new CoordinateSequence(
        {CoordinateXY{5.0, 5.0}, CoordinateXY{5.0, 5.0}});
    labels_.push_back(std::make_unique<Label>(
        0, Location::BOUNDARY, Location::EXTERIOR, Location::INTERIOR));
    out->push_back(new NodedSegmentString(collapsed_seq, false, false,
                                          labels_.back().get()));
    return out;
  }

 private:
  LineIntersector li_;
  IntersectionAdder adder_;
  MCIndexNoder inner_;
  mutable std::vector<std::unique_ptr<Label>> labels_;
};

// Custom noder that replaces edge labels with EXTERIOR/EXTERIOR so
// BufferBuilder::depthDelta returns 0 and no result edges are emitted.
class ZeroDepthDeltaNoder final : public Noder {
 public:
  explicit ZeroDepthDeltaNoder(const PrecisionModel* pm)
      : li_(pm), adder_(li_), inner_(&adder_) {}

  void computeNodes(std::vector<SegmentString*>* seg_strings) override {
    inner_.computeNodes(seg_strings);
  }

  std::vector<SegmentString*>* getNodedSubstrings() const override {
    auto* out = inner_.getNodedSubstrings();
    labels_.push_back(std::make_unique<Label>(
        0, Location::BOUNDARY, Location::EXTERIOR, Location::EXTERIOR));
    for (SegmentString* ss : *out) {
      ss->setData(labels_.back().get());
    }
    return out;
  }

 private:
  LineIntersector li_;
  IntersectionAdder adder_;
  MCIndexNoder inner_;
  mutable std::vector<std::unique_ptr<Label>> labels_;
};

// Custom noder that returns a single unclosed vertical edge with a non-zero
// depthDelta, triggering a TopologyException during subgraph depth calculation.
class OpenEdgeNoder final : public Noder {
 public:
  void computeNodes(std::vector<SegmentString*>* /*seg_strings*/) override {}

  std::vector<SegmentString*>* getNodedSubstrings() const override {
    auto* out = new std::vector<SegmentString*>();
    auto* seq = new CoordinateSequence(
        {CoordinateXY{1.0, 0.0}, CoordinateXY{1.0, 2.0}});
    labels_.push_back(std::make_unique<Label>(
        0, Location::BOUNDARY, Location::EXTERIOR, Location::INTERIOR));
    out->push_back(
        new NodedSegmentString(seq, false, false, labels_.back().get()));
    return out;
  }

 private:
  mutable std::vector<std::unique_ptr<Label>> labels_;
};

// Custom noder that emits a single short 2-point stub interior to the buffer
// so intersection with the two-sided buffer boundary produces no lines.
class ShortStubNoder final : public Noder {
 public:
  void computeNodes(std::vector<SegmentString*>* /*seg_strings*/) override {}

  std::vector<SegmentString*>* getNodedSubstrings() const override {
    auto* out = new std::vector<SegmentString*>();
    auto* seq = new CoordinateSequence(
        {CoordinateXY{0.0, 1.0}, CoordinateXY{0.2, 1.0}});
    out->push_back(new NodedSegmentString(seq, false, false, nullptr));
    return out;
  }
};

// Custom noder that emits predetermined coordinate sequences as noded
// substrings for deterministic trimming and break tests.
class StaticEdgesNoder final : public Noder {
 public:
  explicit StaticEdgesNoder(std::vector<std::vector<CoordinateXY>> edges)
      : edges_(std::move(edges)) {}

  void computeNodes(std::vector<SegmentString*>* /*seg_strings*/) override {}

  std::vector<SegmentString*>* getNodedSubstrings() const override {
    auto* out = new std::vector<SegmentString*>();
    for (const auto& pts : edges_) {
      auto* seq = new CoordinateSequence(0u, false, false);
      for (const CoordinateXY& pt : pts) {
        seq->add(pt);
      }
      out->push_back(new NodedSegmentString(seq, false, false, nullptr));
    }
    return out;
  }

 private:
  std::vector<std::vector<CoordinateXY>> edges_;
};

struct InterruptCounter {
  int checks = 0;
  int trigger_on = 0;
};

int InterruptOnNthCheck(void* data) {
  auto* counter = static_cast<InterruptCounter*>(data);
  ++counter->checks;
  return counter->checks >= counter->trigger_on ? 1 : 0;
}

class BufferBuilderTest : public ::testing::Test {
 protected:
  BufferBuilderTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(BufferBuilderTest, EmptyGeometriesReturnEmptyPolygon) {
  const std::vector<std::string> empty_wkts = {
      "POINT EMPTY",
      "LINESTRING EMPTY",
      "POLYGON EMPTY",
      "MULTIPOINT EMPTY",
      "MULTILINESTRING EMPTY",
      "MULTIPOLYGON EMPTY",
      "GEOMETRYCOLLECTION EMPTY",
  };
  BufferParameters params;
  for (const std::string& wkt : empty_wkts) {
    auto geom = ReadWkt(wkt);
    BufferBuilder builder(params);
    auto result = builder.buffer(geom.get(), 2.0);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->isEmpty()) << wkt;
    EXPECT_TRUE(result->isValid()) << wkt;
    EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_POLYGON) << wkt;
  }
}

TEST_F(BufferBuilderTest,
       PointAndLineWithNonPositiveDistanceReturnEmptyPolygon) {
  BufferParameters params;
  auto point = ReadWkt("POINT (3 4)");
  auto line = ReadWkt("LINESTRING (0 0, 10 0)");

  for (double dist : {0.0, -1.5}) {
    BufferBuilder point_builder(params);
    auto point_buf = point_builder.buffer(point.get(), dist);
    EXPECT_TRUE(point_buf->isEmpty());
    EXPECT_EQ(point_buf->getGeometryTypeId(), geos::geom::GEOS_POLYGON);

    BufferBuilder line_builder(params);
    auto line_buf = line_builder.buffer(line.get(), dist);
    EXPECT_TRUE(line_buf->isEmpty());
    EXPECT_EQ(line_buf->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  }
}

TEST_F(BufferBuilderTest, NegativeBufferCompletelyErodesPolygons) {
  BufferParameters params;

  // Triangle eroded beyond its incircle radius short-circuits in curve builder.
  auto tri = ReadWkt("POLYGON ((0 0, 4 0, 2 3, 0 0))");
  {
    BufferBuilder builder(params);
    auto res = builder.buffer(tri.get(), -2.0);
    EXPECT_TRUE(res->isEmpty());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  }

  // Narrow diagonal polygon with >= 9 vertices passes the curve-builder
  // envelope/triangle heuristics and yields an empty PolygonBuilder result.
  auto narrow_strip = ReadWkt(
      "POLYGON ((0 0, 2 2, 4 4, 6 6, 10 10, "
      "10.2 10, 6.2 6, 4.2 4, 2.2 2, 0.2 0, 0 0))");
  ASSERT_TRUE(narrow_strip->isValid());
  {
    BufferBuilder builder(params);
    auto res = builder.buffer(narrow_strip.get(), -1.0);
    EXPECT_TRUE(res->isEmpty());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  }
}

TEST_F(BufferBuilderTest, BuffersPointRoundAndSquareCaps) {
  auto point = ReadWkt("POINT (2 3)");

  BufferParameters round_params(16, BufferParameters::CAP_ROUND);
  BufferBuilder round_builder(round_params);
  auto round_buf = round_builder.buffer(point.get(), 2.0);
  ASSERT_FALSE(round_buf->isEmpty());
  EXPECT_TRUE(round_buf->isValid());
  EXPECT_EQ(round_buf->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(round_buf->covers(point.get()));
  EXPECT_NEAR(round_buf->getArea(), std::numbers::pi * 4.0, 0.05);

  BufferParameters square_params(8, BufferParameters::CAP_SQUARE);
  BufferBuilder square_builder(square_params);
  auto square_buf = square_builder.buffer(point.get(), 2.0);
  ASSERT_FALSE(square_buf->isEmpty());
  EXPECT_TRUE(square_buf->isValid());
  EXPECT_NEAR(square_buf->getArea(), 16.0, 1e-6);
  const Envelope* env = square_buf->getEnvelopeInternal();
  EXPECT_NEAR(env->getMinX(), 0.0, 1e-6);
  EXPECT_NEAR(env->getMaxX(), 4.0, 1e-6);
  EXPECT_NEAR(env->getMinY(), 1.0, 1e-6);
  EXPECT_NEAR(env->getMaxY(), 5.0, 1e-6);
}

TEST_F(BufferBuilderTest, BuffersLineStringWithCapAndJoinStyles) {
  auto straight = ReadWkt("LINESTRING (0 0, 10 0)");

  {
    BufferParameters flat_params(8, BufferParameters::CAP_FLAT);
    BufferBuilder builder(flat_params);
    auto res = builder.buffer(straight.get(), 2.0);
    EXPECT_TRUE(res->isValid());
    EXPECT_NEAR(res->getArea(), 40.0, 1e-6);
  }
  {
    BufferParameters square_params(8, BufferParameters::CAP_SQUARE);
    BufferBuilder builder(square_params);
    auto res = builder.buffer(straight.get(), 2.0);
    EXPECT_TRUE(res->isValid());
    EXPECT_NEAR(res->getArea(), 56.0, 1e-6);
  }
  {
    BufferParameters round_params(16, BufferParameters::CAP_ROUND);
    BufferBuilder builder(round_params);
    auto res = builder.buffer(straight.get(), 2.0);
    EXPECT_TRUE(res->isValid());
    EXPECT_NEAR(res->getArea(), 40.0 + std::numbers::pi * 4.0, 0.05);
  }

  auto corner = ReadWkt("LINESTRING (0 0, 10 0, 10 10)");
  BufferParameters mitre_params(8, BufferParameters::CAP_FLAT,
                                BufferParameters::JOIN_MITRE, 5.0);
  BufferParameters round_params(8, BufferParameters::CAP_FLAT,
                                BufferParameters::JOIN_ROUND, 5.0);
  BufferParameters bevel_params(8, BufferParameters::CAP_FLAT,
                                BufferParameters::JOIN_BEVEL, 5.0);

  BufferBuilder mitre_builder(mitre_params);
  BufferBuilder round_builder(round_params);
  BufferBuilder bevel_builder(bevel_params);

  auto mitre_res = mitre_builder.buffer(corner.get(), 2.0);
  auto round_res = round_builder.buffer(corner.get(), 2.0);
  auto bevel_res = bevel_builder.buffer(corner.get(), 2.0);

  EXPECT_TRUE(mitre_res->isValid());
  EXPECT_TRUE(round_res->isValid());
  EXPECT_TRUE(bevel_res->isValid());
  EXPECT_GT(mitre_res->getArea(), round_res->getArea());
  EXPECT_GT(round_res->getArea(), bevel_res->getArea());
}

TEST_F(BufferBuilderTest, BuffersClosedLineStringRingWithHole) {
  auto ring = ReadWkt("LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  BufferParameters params(8, BufferParameters::CAP_ROUND);
  BufferBuilder builder(params);

  auto res = builder.buffer(ring.get(), 1.0);
  ASSERT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(res->isValid());

  const auto* poly = dynamic_cast<const Polygon*>(res.get());
  ASSERT_NE(poly, nullptr);
  EXPECT_EQ(poly->getNumInteriorRing(), 1u);
  // Outer 12x12 rounded square (~144 - (4 - pi)) minus 8x8 hole (64).
  EXPECT_NEAR(poly->getArea(), (144.0 - (4.0 - std::numbers::pi)) - 64.0, 0.1);
}

TEST_F(BufferBuilderTest, BuffersPolygonsCcwAndCwWithPositiveZeroNegative) {
  auto ccw_square = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  auto cw_square = ccw_square->reverse();
  BufferParameters params(12);

  for (const Geometry* poly : {ccw_square.get(), cw_square.get()}) {
    {
      BufferBuilder builder(params);
      auto expanded = builder.buffer(poly, 2.0);
      EXPECT_TRUE(expanded->isValid());
      EXPECT_NEAR(expanded->getArea(), 100.0 + 80.0 + std::numbers::pi * 4.0,
                  0.1);
    }
    {
      BufferBuilder builder(params);
      auto zero = builder.buffer(poly, 0.0);
      EXPECT_TRUE(zero->isValid());
      EXPECT_NEAR(zero->getArea(), 100.0, 1e-6);
    }
    {
      BufferBuilder builder(params);
      auto eroded = builder.buffer(poly, -2.0);
      EXPECT_TRUE(eroded->isValid());
      EXPECT_NEAR(eroded->getArea(), 36.0, 1e-6);
    }
  }
}

TEST_F(BufferBuilderTest, BuffersNestedShellHoleAndIslandSubgraphs) {
  auto concentric = ReadWkt(
      "MULTILINESTRING ("
      "(0 0, 20 0, 20 20, 0 20, 0 0),"
      "(8 8, 12 8, 12 12, 8 12, 8 8))");
  BufferParameters params;
  BufferBuilder builder(params);

  auto res = builder.buffer(concentric.get(), 1.0);
  ASSERT_TRUE(res->isValid());
  ASSERT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(res->getNumGeometries(), 2u);
  for (std::size_t i = 0; i < res->getNumGeometries(); ++i) {
    const auto* poly = dynamic_cast<const Polygon*>(res->getGeometryN(i));
    ASSERT_NE(poly, nullptr);
    EXPECT_EQ(poly->getNumInteriorRing(), 1u);
  }
}

TEST_F(BufferBuilderTest, BuffersMultiPointSortsMultipleSubgraphs) {
  auto multipoint = ReadWkt("MULTIPOINT ((20 0), (0 0), (15 0), (5 0))");
  BufferParameters params;
  BufferBuilder builder(params);

  auto res = builder.buffer(multipoint.get(), 1.0);
  ASSERT_TRUE(res->isValid());
  EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(res->getNumGeometries(), 4u);
  EXPECT_NEAR(res->getArea(), 4.0 * std::numbers::pi, 0.15);
}

TEST_F(BufferBuilderTest, BuffersGeometryCollection) {
  auto collection = ReadWkt(
      "GEOMETRYCOLLECTION ("
      "POINT (0 0),"
      "LINESTRING (10 0, 20 0),"
      "POLYGON ((30 0, 40 0, 40 10, 30 10, 30 0)))");
  BufferParameters params;
  BufferBuilder builder(params);

  auto res = builder.buffer(collection.get(), 1.0);
  ASSERT_TRUE(res->isValid());
  EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(res->getNumGeometries(), 3u);
}

TEST_F(BufferBuilderTest, SetInvertOrientationSelectsSmallerSelfCrossingLobe) {
  // Asymmetric figure-8: large CCW lobe (area 50) + small CW lobe (area 2).
  auto figure_eight =
      ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 0, -2 -2, -2 0, 0 0))");
  BufferParameters params;

  BufferBuilder normal_builder(params);
  auto normal_res = normal_builder.buffer(figure_eight.get(), 0.0);
  EXPECT_TRUE(normal_res->isValid());
  EXPECT_NEAR(normal_res->getArea(), 50.0, 1e-6);

  BufferBuilder inverted_builder(params);
  inverted_builder.setInvertOrientation(true);
  auto inverted_res = inverted_builder.buffer(figure_eight.get(), 0.0);
  EXPECT_TRUE(inverted_res->isValid());
  EXPECT_NEAR(inverted_res->getArea(), 2.0, 1e-6);
}

TEST_F(BufferBuilderTest, MergesIdenticalSameDirectionEdges) {
  auto dup_lines = ReadWkt("MULTILINESTRING ((0 0, 10 0), (0 0, 10 0))");
  BufferParameters params(8, BufferParameters::CAP_FLAT);
  BufferBuilder line_builder(params);

  auto line_res = line_builder.buffer(dup_lines.get(), 1.0);
  EXPECT_TRUE(line_res->isValid());
  EXPECT_EQ(line_res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_NEAR(line_res->getArea(), 20.0, 1e-6);

  auto dup_polys = ReadWkt(
      "MULTIPOLYGON ("
      "((0 0, 10 0, 10 10, 0 10, 0 0)),"
      "((0 0, 10 0, 10 10, 0 10, 0 0)))");
  BufferBuilder poly_builder(params);
  auto poly_res = poly_builder.buffer(dup_polys.get(), 0.0);
  EXPECT_TRUE(poly_res->isValid());
  EXPECT_EQ(poly_res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_NEAR(poly_res->getArea(), 100.0, 1e-6);
}

TEST_F(BufferBuilderTest, MergesCoincidentOppositeDirectionEdges) {
  auto adjacent_polys = ReadWkt(
      "MULTIPOLYGON ("
      "((0 0, 10 0, 10 10, 0 10, 0 0)),"
      "((10 0, 20 0, 20 10, 10 10, 10 0)))");
  BufferParameters params;
  BufferBuilder poly_builder(params);

  auto poly_res = poly_builder.buffer(adjacent_polys.get(), 0.0);
  EXPECT_TRUE(poly_res->isValid());
  EXPECT_EQ(poly_res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_NEAR(poly_res->getArea(), 200.0, 1e-6);

  // Two parallel lines distance 2 apart buffered by 1 share an opposite-sense
  // edge.
  auto parallel_lines = ReadWkt("MULTILINESTRING ((0 0, 10 0), (0 2, 10 2))");
  BufferParameters flat_params(8, BufferParameters::CAP_FLAT);
  BufferBuilder line_builder(flat_params);

  auto line_res = line_builder.buffer(parallel_lines.get(), 1.0);
  EXPECT_TRUE(line_res->isValid());
  EXPECT_EQ(line_res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_NEAR(line_res->getArea(), 40.0, 1e-6);
}

TEST_F(BufferBuilderTest, SetWorkingPrecisionModelRoundsOutput) {
  auto line = ReadWkt("LINESTRING (0.12 0.34, 10.12 0.34)");
  PrecisionModel integer_pm(1.0);
  BufferParameters params(4, BufferParameters::CAP_SQUARE);

  BufferBuilder builder(params);
  builder.setWorkingPrecisionModel(&integer_pm);
  auto res = builder.buffer(line.get(), 1.0);
  ASSERT_TRUE(res->isValid());

  auto coords = res->getCoordinates();
  ASSERT_GE(coords->size(), 4u);
  for (std::size_t i = 0; i < coords->size(); ++i) {
    const double x = coords->getX(i);
    const double y = coords->getY(i);
    EXPECT_DOUBLE_EQ(x, std::round(x));
    EXPECT_DOUBLE_EQ(y, std::round(y));
  }

  // Clearing workingPrecisionModel falls back to geometry's precision model.
  BufferBuilder reset_builder(params);
  reset_builder.setWorkingPrecisionModel(&integer_pm);
  reset_builder.setWorkingPrecisionModel(nullptr);
  auto float_res = reset_builder.buffer(line.get(), 1.0);
  EXPECT_TRUE(float_res->isValid());
  EXPECT_NEAR(float_res->getEnvelopeInternal()->getMinY(), -0.66, 1e-6);
}

TEST_F(BufferBuilderTest, SetNoderUsesExternalNoderWithoutOwningIt) {
  auto poly = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  BufferParameters params;

  // Test with ScaledNoder + SnapRoundingNoder (matching BufferOp fallback).
  PrecisionModel unit_pm(1.0);
  PrecisionModel work_pm(100.0);
  SnapRoundingNoder snap_noder(&unit_pm);
  ScaledNoder scaled_noder(snap_noder, work_pm.getScale());
  {
    BufferBuilder builder(params);
    builder.setWorkingPrecisionModel(&work_pm);
    builder.setNoder(&scaled_noder);
    auto res = builder.buffer(poly.get(), 1.0);
    EXPECT_TRUE(res->isValid());
    EXPECT_NEAR(res->getArea(), 140.0 + std::numbers::pi, 0.2);
  }

  // Test with ValidatingNoder wrapping MCIndexNoder.
  const PrecisionModel* pm = poly->getPrecisionModel();
  LineIntersector li(pm);
  IntersectionAdder adder(li);
  MCIndexNoder inner_noder(&adder);
  ValidatingNoder validating_noder(inner_noder);
  {
    BufferBuilder builder(params);
    builder.setNoder(&validating_noder);
    auto res = builder.buffer(poly.get(), 1.0);
    EXPECT_TRUE(res->isValid());
    EXPECT_NEAR(res->getArea(), 140.0 + std::numbers::pi, 0.2);
  }
}

TEST_F(BufferBuilderTest, CustomNoderHandlesCollapsedEdgesAndZeroDepthDelta) {
  auto point = ReadWkt("POINT (0 0)");
  BufferParameters params;

  {
    InjectCollapsedEdgeNoder noder(point->getPrecisionModel());
    BufferBuilder builder(params);
    builder.setNoder(&noder);
    auto res = builder.buffer(point.get(), 1.0);
    EXPECT_TRUE(res->isValid());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_NEAR(res->getArea(), std::numbers::pi, 0.1);
  }

  {
    ZeroDepthDeltaNoder noder(point->getPrecisionModel());
    BufferBuilder builder(params);
    builder.setNoder(&noder);
    auto res = builder.buffer(point.get(), 1.0);
    EXPECT_TRUE(res->isEmpty());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  }
}

TEST_F(BufferBuilderTest,
       SingleSidedBufferParametersOnLineStringAndMultiGeometry) {
  BufferParameters params;
  params.setSingleSided(true);

  auto line = ReadWkt("LINESTRING (0 0, 10 0)");
  {
    BufferBuilder left_builder(params);
    auto left = left_builder.buffer(line.get(), 2.0);
    EXPECT_TRUE(left->isValid());
    EXPECT_NEAR(left->getArea(), 20.0, 1e-6);
    EXPECT_NEAR(left->getEnvelopeInternal()->getMinY(), 0.0, 1e-6);
    EXPECT_NEAR(left->getEnvelopeInternal()->getMaxY(), 2.0, 1e-6);
  }
  {
    BufferBuilder right_builder(params);
    auto right = right_builder.buffer(line.get(), -2.0);
    EXPECT_TRUE(right->isValid());
    EXPECT_NEAR(right->getArea(), 20.0, 1e-6);
    EXPECT_NEAR(right->getEnvelopeInternal()->getMinY(), -2.0, 1e-6);
    EXPECT_NEAR(right->getEnvelopeInternal()->getMaxY(), 0.0, 1e-6);
  }

  // Multi-geometry (> 1 element) buffers each component and unions them.
  auto multi_line = ReadWkt("MULTILINESTRING ((0 0, 10 0), (5 -5, 5 5))");
  {
    BufferBuilder multi_builder(params);
    auto multi_res = multi_builder.buffer(multi_line.get(), 1.0);
    EXPECT_TRUE(multi_res->isValid());
    // [0, 10]x[0, 1] (area 10) union [4, 5]x[-5, 5] (area 10) minus overlap
    // [4, 5]x[0, 1] (area 1) = 19.0.
    EXPECT_NEAR(multi_res->getArea(), 19.0, 1e-4);
  }

  // Single-element MultiLineString (getNumGeometries() == 1) takes non-multi
  // path.
  auto single_multi = ReadWkt("MULTILINESTRING ((0 0, 10 0))");
  {
    BufferBuilder builder(params);
    auto res = builder.buffer(single_multi.get(), 2.0);
    EXPECT_TRUE(res->isValid());
    EXPECT_NEAR(res->getArea(), 20.0, 1e-6);
  }
}

TEST_F(BufferBuilderTest, SingleSidedBufferCleanupSelectsLargestPolygon) {
  BufferParameters params;
  params.setSingleSided(true);

  // Dimension > 1 (Polygon) enters boundary-noding cleanup and picks largest
  // polygon.
  auto square = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  {
    BufferBuilder builder(params);
    auto res = builder.buffer(square.get(), 5.0);
    EXPECT_TRUE(res->isValid());
    EXPECT_FALSE(res->isEmpty());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_GT(res->getArea(), 100.0);
  }
  // Smaller distance (1.0) produces outer band (~43.14) smaller than inner
  // square (100.0), so Polygonizer picks the inner square and exercises the
  // non-updating branch of max-area comparison regardless of ring order.
  {
    BufferBuilder builder(params);
    auto res = builder.buffer(square.get(), 1.0);
    EXPECT_TRUE(res->isValid());
    EXPECT_FALSE(res->isEmpty());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_NEAR(res->getArea(), 100.0, 1e-6);
  }

  // Point with single-sided flag produces a single polygon in cleanup path.
  auto point = ReadWkt("POINT (0 0)");
  {
    BufferBuilder builder(params);
    auto res = builder.buffer(point.get(), 2.0);
    EXPECT_TRUE(res->isValid());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_NEAR(res->getArea(), 4.0 * std::numbers::pi, 0.1);
  }

  // Closed LineString with single-sided negative distance produces an outer
  // annulus + inner square in Polygonizer and returns the larger polygon.
  auto closed_ring = ReadWkt("LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)");
  {
    BufferBuilder builder(params);
    auto res = builder.buffer(closed_ring.get(), -5.0);
    EXPECT_TRUE(res->isValid());
    EXPECT_FALSE(res->isEmpty());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_GT(res->getArea(), 100.0);
  }
}

TEST_F(BufferBuilderTest, BufferLineSingleSidedRejectsNonLineString) {
  BufferParameters params;
  const std::vector<std::string> invalid_wkts = {
      "POINT (0 0)",
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))",
      "MULTILINESTRING ((0 0, 1 0))",
      "GEOMETRYCOLLECTION (LINESTRING (0 0, 1 0))",
  };
  for (const std::string& wkt : invalid_wkts) {
    auto geom = ReadWkt(wkt);
    BufferBuilder builder(params);
    EXPECT_THROW(builder.bufferLineSingleSided(geom.get(), 1.0, true),
                 IllegalArgumentException)
        << wkt;
  }
}

TEST_F(BufferBuilderTest, BufferLineSingleSidedZeroAndNegativeDistance) {
  BufferParameters params;
  auto line = ReadWkt("LINESTRING (0 0, 10 0)");
  auto empty_line = ReadWkt("LINESTRING EMPTY");

  {
    BufferBuilder builder(params);
    auto res = builder.bufferLineSingleSided(line.get(), 0.0, true);
    EXPECT_TRUE(res->equalsExact(line.get()));
  }
  {
    BufferBuilder builder(params);
    auto res = builder.bufferLineSingleSided(empty_line.get(), 0.0, false);
    EXPECT_TRUE(res->isEmpty());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  }
  {
    BufferBuilder builder(params);
    auto res = builder.bufferLineSingleSided(line.get(), -2.0, true);
    EXPECT_TRUE(res->isEmpty());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  }
}

TEST_F(BufferBuilderTest, BufferLineSingleSidedOffsetsAndReusesNoder) {
  BufferParameters params;
  auto line = ReadWkt("LINESTRING (0 0, 10 0)");

  BufferBuilder builder(params);
  auto left = builder.bufferLineSingleSided(line.get(), 2.0, true);
  auto expected_left = ReadWkt("LINESTRING (0 2, 10 2)");
  EXPECT_TRUE(left->equals(expected_left.get()));

  // Reusing the same BufferBuilder after bufferLineSingleSided hits the
  // existing LineIntersector reuse path in getNoder without edgeList issues.
  auto right = builder.bufferLineSingleSided(line.get(), 3.0, false);
  auto expected_right = ReadWkt("LINESTRING (0 -3, 10 -3)");
  EXPECT_TRUE(right->equals(expected_right.get()));

  // Can also follow with buffer() using the same reused LineIntersector.
  auto poly = ReadWkt("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  auto poly_buf = builder.buffer(poly.get(), 1.0);
  EXPECT_TRUE(poly_buf->isValid());
  EXPECT_NEAR(poly_buf->getArea(), 32.0 + std::numbers::pi, 0.2);
}

TEST_F(BufferBuilderTest,
       BufferLineSingleSidedWithWorkingPrecisionAndExternalNoder) {
  auto line = ReadWkt("LINESTRING (0.1 0.1, 10.1 0.1)");
  PrecisionModel fixed_pm(10.0);
  LineIntersector li(&fixed_pm);
  IntersectionAdder adder(li);
  MCIndexNoder external_noder(&adder);

  BufferParameters params;
  BufferBuilder builder(params);
  builder.setWorkingPrecisionModel(&fixed_pm);
  builder.setNoder(&external_noder);

  auto offset = builder.bufferLineSingleSided(line.get(), 2.0, true);
  EXPECT_FALSE(offset->isEmpty());
  EXPECT_TRUE(offset->isValid());
  EXPECT_NEAR(offset->getLength(), 10.0, 1e-6);
  EXPECT_NEAR(offset->getEnvelopeInternal()->getMinY(), 2.1, 1e-6);
}

TEST_F(BufferBuilderTest,
       BufferLineSingleSidedReturnsMultiLineStringWhenSplit) {
  auto crossing = ReadWkt("LINESTRING (0 0, 10 0, 10 5, 5 5, 5 -5)");
  BufferParameters params;
  BufferBuilder builder(params);

  auto offset = builder.bufferLineSingleSided(crossing.get(), 1.0, true);
  EXPECT_FALSE(offset->isEmpty());
  EXPECT_TRUE(offset->isValid());
  EXPECT_EQ(offset->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  ASSERT_EQ(offset->getNumGeometries(), 3u);
  // Raw offset curve length is 24.0, minus two 2.0-unit interior segments
  // inside the crossing corridor ([4, 6] x [-1, 1]), yielding 20.0 total.
  EXPECT_NEAR(offset->getLength(), 20.0, 1e-6);

  std::vector<double> part_lengths;
  part_lengths.reserve(offset->getNumGeometries());
  for (std::size_t i = 0; i < offset->getNumGeometries(); ++i) {
    part_lengths.push_back(offset->getGeometryN(i)->getLength());
  }
  std::sort(part_lengths.begin(), part_lengths.end());
  EXPECT_NEAR(part_lengths[0], 4.0, 1e-6);
  EXPECT_NEAR(part_lengths[1], 4.0, 1e-6);
  EXPECT_NEAR(part_lengths[2], 12.0, 1e-6);

  const Envelope* env = offset->getEnvelopeInternal();
  EXPECT_NEAR(env->getMinX(), 0.0, 1e-6);
  EXPECT_NEAR(env->getMaxX(), 9.0, 1e-6);
  EXPECT_NEAR(env->getMinY(), -5.0, 1e-6);
  EXPECT_NEAR(env->getMaxY(), 4.0, 1e-6);
}

TEST_F(BufferBuilderTest,
       BufferLineSingleSidedTrimsPointsNearStartAndEndPoints) {
  BufferParameters params;
  constexpr double kDist = 2.0;

  // Front near startPoint: trims short tip and stops at long segment.
  {
    auto trim_line =
        ReadWkt("LINESTRING (0 0, 10 0, 10 10, -3.5 10, -3.55 1.5, -3.5 0.5)");
    BufferBuilder b1(params);
    auto trimmed = b1.bufferLineSingleSided(trim_line.get(), kDist, true);
    ASSERT_FALSE(trimmed->isEmpty());
    EXPECT_TRUE(trimmed->isValid());
    EXPECT_GT(trimmed->getEnvelopeInternal()->getMinY(), 1.0);

    auto break_line =
        ReadWkt("LINESTRING (0 0, 10 0, 10 10, -3.5 10, -3.5 0.5)");
    BufferBuilder b2(params);
    auto unbroken = b2.bufferLineSingleSided(break_line.get(), kDist, true);
    ASSERT_FALSE(unbroken->isEmpty());
    EXPECT_LT(unbroken->getEnvelopeInternal()->getMinY(), 1.0);
  }

  // Back near startPoint (reflection across X = 0 with leftSide = false).
  {
    auto trim_line =
        ReadWkt("LINESTRING (0 0, -10 0, -10 10, 3.5 10, 3.55 1.5, 3.5 0.5)");
    BufferBuilder b1(params);
    auto trimmed = b1.bufferLineSingleSided(trim_line.get(), kDist, false);
    ASSERT_FALSE(trimmed->isEmpty());
    EXPECT_TRUE(trimmed->isValid());
    EXPECT_GT(trimmed->getEnvelopeInternal()->getMinY(), 1.0);

    auto break_line =
        ReadWkt("LINESTRING (0 0, -10 0, -10 10, 3.5 10, 3.5 0.5)");
    BufferBuilder b2(params);
    auto unbroken = b2.bufferLineSingleSided(break_line.get(), kDist, false);
    ASSERT_FALSE(unbroken->isEmpty());
    EXPECT_LT(unbroken->getEnvelopeInternal()->getMinY(), 1.0);
  }

  // Front near endPoint (leftSide = true puts smaller X tip at front).
  {
    auto trim_line =
        ReadWkt("LINESTRING (0 0, 1.0 0.2, 10 0, 10 10, 0 10, 0 2.5)");
    BufferBuilder b1(params);
    auto trimmed = b1.bufferLineSingleSided(trim_line.get(), kDist, true);
    ASSERT_FALSE(trimmed->isEmpty());
    EXPECT_TRUE(trimmed->isValid());
    EXPECT_GT(trimmed->getEnvelopeInternal()->getMinX(), 0.5);

    auto break_line = ReadWkt("LINESTRING (0 0, 10 0, 10 10, 0 10, 0 2.5)");
    BufferBuilder b2(params);
    auto unbroken = b2.bufferLineSingleSided(break_line.get(), kDist, true);
    ASSERT_FALSE(unbroken->isEmpty());
    EXPECT_LT(unbroken->getEnvelopeInternal()->getMinX(), 0.5);
    EXPECT_LT(trimmed->getLength(), unbroken->getLength());
  }

  // Back near endPoint (leftSide = false puts larger X tip at back).
  {
    auto trim_line =
        ReadWkt("LINESTRING (0 0, -1.0 0.2, -10 0, -10 10, 0 10, 0 2.5)");
    BufferBuilder b1(params);
    auto trimmed = b1.bufferLineSingleSided(trim_line.get(), kDist, false);
    ASSERT_FALSE(trimmed->isEmpty());
    EXPECT_TRUE(trimmed->isValid());
    EXPECT_LT(trimmed->getEnvelopeInternal()->getMaxX(), -0.5);

    auto break_line = ReadWkt("LINESTRING (0 0, -10 0, -10 10, 0 10, 0 2.5)");
    BufferBuilder b2(params);
    auto unbroken = b2.bufferLineSingleSided(break_line.get(), kDist, false);
    ASSERT_FALSE(unbroken->isEmpty());
    EXPECT_GT(unbroken->getEnvelopeInternal()->getMaxX(), -0.5);
    EXPECT_LT(trimmed->getLength(), unbroken->getLength());
  }

  // Short endpoint stubs within ptDistAllowance shrink to sz == 1 and are
  // discarded, leaving only the single closed loop LineString.
  {
    auto stub_line = ReadWkt("LINESTRING (1.2 0, 10 0, 10 10, 0 10, 0 1.2)");
    BufferBuilder builder(params);
    auto res = builder.bufferLineSingleSided(stub_line.get(), kDist, true);
    ASSERT_FALSE(res->isEmpty());
    EXPECT_TRUE(res->isValid());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
    EXPECT_NEAR(res->getLength(), 24.0, 1e-6);
  }
}

TEST_F(BufferBuilderTest,
       BufferLineSingleSidedReturnsEmptyWhenAllLinesTrimmed) {
  auto line = ReadWkt("LINESTRING (0 0, 10 0)");
  BufferParameters params;

  // Interior stub does not intersect the flat-cap buffer boundary.
  {
    ShortStubNoder stub_noder;
    BufferBuilder builder(params);
    builder.setNoder(&stub_noder);

    auto res = builder.bufferLineSingleSided(line.get(), 2.0, true);
    ASSERT_NE(res, nullptr);
    EXPECT_TRUE(res->isEmpty());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  }

  // Stub lying on the left flat cap trims down to sz == 1 and is discarded.
  {
    StaticEdgesNoder start_stub_noder({{{0.0, 1.5}, {0.0, 0.5}}});
    BufferBuilder builder(params);
    builder.setNoder(&start_stub_noder);

    auto res = builder.bufferLineSingleSided(line.get(), 2.0, true);
    ASSERT_NE(res, nullptr);
    EXPECT_TRUE(res->isEmpty());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  }

  // Stub lying on the right flat cap trims down to sz == 1 near endPoint.
  {
    StaticEdgesNoder end_stub_noder({{{10.0, 1.5}, {10.0, 0.5}}});
    BufferBuilder builder(params);
    builder.setNoder(&end_stub_noder);

    auto res = builder.bufferLineSingleSided(line.get(), 2.0, true);
    ASSERT_NE(res, nullptr);
    EXPECT_TRUE(res->isEmpty());
    EXPECT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  }
}

TEST_F(BufferBuilderTest,
       BufferLineSingleSidedStaticNoderCoversAllTrimAndBreakBranches) {
  auto line = ReadWkt("LINESTRING (0 0, 10 0)");
  auto expected_top = ReadWkt("LINESTRING (0 2, 10 2)");
  BufferParameters params;

  // Forward and reversed boundary lines trim both front and back ends near
  // startPoint and endPoint, leaving LINESTRING (0 2, 10 2).
  {
    StaticEdgesNoder fwd_noder(
        {{{0.0, 1.5}, {0.0, 2.0}, {10.0, 2.0}, {10.0, 1.5}}});
    BufferBuilder builder(params);
    builder.setNoder(&fwd_noder);
    auto res = builder.bufferLineSingleSided(line.get(), 2.0, true);
    EXPECT_TRUE(res->equals(expected_top.get()));
  }
  {
    StaticEdgesNoder rev_noder(
        {{{10.0, 1.5}, {10.0, 2.0}, {0.0, 2.0}, {0.0, 1.5}}});
    BufferBuilder builder(params);
    builder.setNoder(&rev_noder);
    auto res = builder.bufferLineSingleSided(line.get(), 2.0, true);
    EXPECT_TRUE(res->equals(expected_top.get()));
  }

  // Long segments along the flat caps trigger segLength > segLengthAllowance
  // breaks in all four front/back start/end loops without trimming.
  for (const auto& edge : std::vector<std::vector<CoordinateXY>>{
           {{0.0, -1.5}, {0.0, 2.0}},
           {{0.0, -2.0}, {0.0, 1.5}},
           {{10.0, -1.5}, {10.0, 2.0}},
           {{10.0, -2.0}, {10.0, 1.5}},
       }) {
    StaticEdgesNoder break_noder({edge});
    BufferBuilder builder(params);
    builder.setNoder(&break_noder);
    auto res = builder.bufferLineSingleSided(line.get(), 2.0, true);
    EXPECT_FALSE(res->isEmpty());
    EXPECT_NEAR(res->getLength(), 3.5, 1e-6);
  }
}

TEST_F(BufferBuilderTest,
       BufferLineSingleSidedShortLineAndIgnoredSingleSidedParam) {
  // Short line exercises distance - length * 0.1 > distance * 0.98 in
  // ptDistAllowance without over-trimming the offset at distance 2.0.
  auto short_line = ReadWkt("LINESTRING (0 0, 0.1 0)");
  BufferParameters params(8, BufferParameters::CAP_SQUARE);
  params.setSingleSided(true);

  BufferBuilder builder(params);
  auto res = builder.bufferLineSingleSided(short_line.get(), 2.0, true);
  auto expected = ReadWkt("LINESTRING (0 2, 0.1 2)");
  EXPECT_TRUE(res->equals(expected.get()));
}

TEST_F(BufferBuilderTest, BuffersDeeplyNestedIslandsAndFactoryPrecisionModel) {
  auto nested = ReadWkt(
      "MULTILINESTRING ("
      "(0 0, 40 0, 40 40, 0 40, 0 0),"
      "(10 10, 30 10, 30 30, 10 30, 10 10),"
      "(18 18, 22 18, 22 22, 18 22, 18 18))");
  BufferParameters params;
  {
    BufferBuilder builder(params);
    auto res = builder.buffer(nested.get(), 1.0);
    ASSERT_TRUE(res->isValid());
    ASSERT_EQ(res->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
    EXPECT_EQ(res->getNumGeometries(), 3u);
    for (std::size_t i = 0; i < res->getNumGeometries(); ++i) {
      const auto* poly = dynamic_cast<const Polygon*>(res->getGeometryN(i));
      ASSERT_NE(poly, nullptr);
      EXPECT_EQ(poly->getNumInteriorRing(), 1u);
    }
  }

  // Geometry with FIXED PrecisionModel uses factory precision when
  // workingPrecisionModel is not explicitly set.
  PrecisionModel fixed_pm(10.0);
  auto fixed_factory = GeometryFactory::create(&fixed_pm);
  WKTReader fixed_reader(*fixed_factory);
  auto fixed_line = fixed_reader.read("LINESTRING (0.1 0.1, 10.1 0.1)");

  BufferBuilder line_builder(params);
  auto offset = line_builder.bufferLineSingleSided(fixed_line.get(), 2.0, true);
  auto expected_offset = fixed_reader.read("LINESTRING (0.1 2.1, 10.1 2.1)");
  EXPECT_TRUE(offset->equals(expected_offset.get()));

  BufferBuilder poly_builder(params);
  auto poly = fixed_reader.read("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  auto poly_buf = poly_builder.buffer(poly.get(), 1.0);
  ASSERT_TRUE(poly_buf->isValid());
  auto coords = poly_buf->getCoordinates();
  for (std::size_t i = 0; i < coords->size(); ++i) {
    EXPECT_NEAR(coords->getX(i) * 10.0, std::round(coords->getX(i) * 10.0),
                1e-9);
    EXPECT_NEAR(coords->getY(i) * 10.0, std::round(coords->getY(i) * 10.0),
                1e-9);
  }
}

TEST_F(BufferBuilderTest, CleansUpSubgraphsOnInterruptAndTopologyException) {
  auto poly = ReadWkt(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0),"
      "(5 5, 15 5, 15 15, 5 15, 5 5))");
  BufferParameters params;

  // Interrupt before noding (check #1).
  {
    InterruptCounter counter{.checks = 0, .trigger_on = 1};
    auto* prev = CurrentThreadInterrupt::registerCallback(&InterruptOnNthCheck,
                                                          &counter);
    absl::Cleanup restore = [prev]() {
      CurrentThreadInterrupt::registerCallback(prev, nullptr);
    };
    BufferBuilder builder(params);
    EXPECT_THROW(builder.buffer(poly.get(), 1.0), GEOSException);
  }

  // Interrupt after createSubgraphs (check #4) inside try block so subgraphList
  // cleanup executes in catch(const util::GEOSException&).
  {
    InterruptCounter counter{.checks = 0, .trigger_on = 4};
    auto* prev = CurrentThreadInterrupt::registerCallback(&InterruptOnNthCheck,
                                                          &counter);
    absl::Cleanup restore = [prev]() {
      CurrentThreadInterrupt::registerCallback(prev, nullptr);
    };
    BufferBuilder builder(params);
    EXPECT_THROW(builder.buffer(poly.get(), 1.0), GEOSException);
    EXPECT_EQ(counter.checks, 4);
  }

  // Open edge from custom noder causes TopologyException in buildSubgraphs.
  {
    OpenEdgeNoder open_noder;
    BufferBuilder builder(params);
    builder.setNoder(&open_noder);
    EXPECT_THROW(builder.buffer(poly.get(), 1.0), TopologyException);
  }
}

}  // namespace
}  // namespace geos_google
