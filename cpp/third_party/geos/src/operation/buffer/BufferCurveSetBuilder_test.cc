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

// Unit tests for
// third_party/geos/src/operation/buffer/BufferCurveSetBuilder.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - All inputs are constructed in memory via isolated GeometryFactory
// instances.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/buffer/BufferCurveSetBuilder.cpp

#include "third_party/geos/include/geos/operation/buffer/BufferCurveSetBuilder.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/Orientation.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Curve.h"
#include "third_party/geos/include/geos/geom/CurvePolygon.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/MultiPoint.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/geomgraph/Label.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"
#include "third_party/geos/include/geos/noding/SegmentNodeList.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/operation/buffer/BufferParameters.h"
#include "third_party/geos/include/geos/util/GEOSException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::algorithm::Orientation;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::Curve;
using geos::geom::CurvePolygon;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Location;
using geos::geom::MultiLineString;
using geos::geom::MultiPoint;
using geos::geom::MultiPolygon;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::geom::Position;
using geos::geom::PrecisionModel;
using geos::geomgraph::Label;
using geos::noding::NodedSegmentString;
using geos::noding::SegmentString;
using geos::operation::buffer::BufferCurveSetBuilder;
using geos::operation::buffer::BufferParameters;
using geos::util::GEOSException;
using geos::util::UnsupportedOperationException;

std::unique_ptr<CoordinateSequence> MakeSeqXY(
    std::initializer_list<CoordinateXY> coords) {
  return std::make_unique<CoordinateSequence>(coords);
}

std::unique_ptr<LineString> MakeLine(
    const GeometryFactory& factory,
    std::initializer_list<CoordinateXY> coords) {
  return factory.createLineString(MakeSeqXY(coords));
}

std::unique_ptr<LinearRing> MakeRing(
    const GeometryFactory& factory,
    std::initializer_list<CoordinateXY> coords) {
  return factory.createLinearRing(MakeSeqXY(coords));
}

std::unique_ptr<Polygon> MakePolygon(
    const GeometryFactory& factory, std::initializer_list<CoordinateXY> shell) {
  return factory.createPolygon(MakeRing(factory, shell));
}

void ExpectCurveLabel(const SegmentString* ss, Location expected_left,
                      Location expected_right) {
  ASSERT_NE(ss, nullptr);
  const auto* label = static_cast<const Label*>(ss->getData());
  ASSERT_NE(label, nullptr);
  EXPECT_EQ(label->getLocation(0, Position::ON), Location::BOUNDARY);
  EXPECT_EQ(label->getLocation(0, Position::LEFT), expected_left);
  EXPECT_EQ(label->getLocation(0, Position::RIGHT), expected_right);
  EXPECT_TRUE(label->isNull(1));
}

void ExpectEnvelopeNear(const SegmentString* ss, double min_x, double max_x,
                        double min_y, double max_y, double tol) {
  ASSERT_NE(ss, nullptr);
  const Envelope env = ss->getCoordinates()->getEnvelope();
  ASSERT_FALSE(env.isNull());
  EXPECT_NEAR(env.getMinX(), min_x, tol);
  EXPECT_NEAR(env.getMaxX(), max_x, tol);
  EXPECT_NEAR(env.getMinY(), min_y, tol);
  EXPECT_NEAR(env.getMaxY(), max_y, tol);
}

TEST(BufferCurveSetBuilderTest, EmptyGeometriesProduceNoCurves) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::vector<std::unique_ptr<Geometry>> empty_geoms;
  empty_geoms.push_back(factory->createPoint());
  empty_geoms.push_back(factory->createLineString());
  empty_geoms.push_back(factory->createLinearRing());
  empty_geoms.push_back(factory->createPolygon());
  empty_geoms.push_back(factory->createMultiPoint());
  empty_geoms.push_back(factory->createMultiLineString());
  empty_geoms.push_back(factory->createMultiPolygon());
  empty_geoms.push_back(factory->createGeometryCollection());

  constexpr std::array<double, 3> kDistances = {-2.0, 0.0, 2.0};
  const BufferParameters params;

  for (const auto& geom : empty_geoms) {
    SCOPED_TRACE(geom->getGeometryType());
    for (double dist : kDistances) {
      SCOPED_TRACE(dist);
      BufferCurveSetBuilder builder(*geom, dist, factory->getPrecisionModel(),
                                    params);
      EXPECT_TRUE(builder.getCurves().empty());
    }
  }
}

TEST(BufferCurveSetBuilderTest, PointBufferDistancesAndEndCapStyles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(2.0, 3.0));

  struct EmptyPointCase {
    std::string_view name;
    double distance;
    BufferParameters::EndCapStyle cap_style;
    bool single_sided;
  };

  constexpr auto kEmptyCases = std::to_array<EmptyPointCase>({
      {
          .name = "zero_distance",
          .distance = 0.0,
          .cap_style = BufferParameters::CAP_ROUND,
          .single_sided = false,
      },
      {
          .name = "negative_distance",
          .distance = -1.5,
          .cap_style = BufferParameters::CAP_ROUND,
          .single_sided = false,
      },
      {
          .name = "negative_distance_single_sided",
          .distance = -1.5,
          .cap_style = BufferParameters::CAP_ROUND,
          .single_sided = true,
      },
      {
          .name = "flat_cap_empty_curve",
          .distance = 2.0,
          .cap_style = BufferParameters::CAP_FLAT,
          .single_sided = false,
      },
  });

  for (const auto& tc : kEmptyCases) {
    SCOPED_TRACE(tc.name);
    BufferParameters params;
    params.setEndCapStyle(tc.cap_style);
    params.setSingleSided(tc.single_sided);
    BufferCurveSetBuilder builder(*pt, tc.distance,
                                  factory->getPrecisionModel(), params);
    EXPECT_TRUE(builder.getCurves().empty());
  }

  {
    BufferParameters round_params(8, BufferParameters::CAP_ROUND);
    BufferCurveSetBuilder builder(*pt, 2.0, factory->getPrecisionModel(),
                                  round_params);
    std::vector<SegmentString*>& curves = builder.getCurves();
    ASSERT_EQ(curves.size(), 1u);
    ExpectCurveLabel(curves[0], Location::EXTERIOR, Location::INTERIOR);
    EXPECT_TRUE(curves[0]->isClosed());
    EXPECT_FALSE(Orientation::isCCWArea(curves[0]->getCoordinates()));
    ExpectEnvelopeNear(curves[0], 0.0, 4.0, 1.0, 5.0, 1e-9);
    const CoordinateSequence* seq = curves[0]->getCoordinates();
    for (std::size_t i = 0; i < seq->size(); ++i) {
      EXPECT_NEAR(seq->getAt<CoordinateXY>(i).distance(CoordinateXY(2.0, 3.0)),
                  2.0, 1e-9);
    }
  }

  {
    BufferParameters square_params(8, BufferParameters::CAP_SQUARE);
    BufferCurveSetBuilder builder(*pt, 2.0, factory->getPrecisionModel(),
                                  square_params);
    std::vector<SegmentString*>& curves = builder.getCurves();
    ASSERT_EQ(curves.size(), 1u);
    ExpectCurveLabel(curves[0], Location::EXTERIOR, Location::INTERIOR);
    EXPECT_TRUE(curves[0]->isClosed());
    EXPECT_EQ(curves[0]->size(), 5u);
    EXPECT_FALSE(Orientation::isCCWArea(curves[0]->getCoordinates()));
    ExpectEnvelopeNear(curves[0], 0.0, 4.0, 1.0, 5.0, 1e-12);
  }
}

TEST(BufferCurveSetBuilderTest, PointWithInvalidCoordinatesProducesNoCurves) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  const BufferParameters params;

  std::unique_ptr<Point> nan_pt = factory->createPoint(
      CoordinateXY(std::numeric_limits<double>::quiet_NaN(), 1.0));
  ASSERT_FALSE(nan_pt->isEmpty());
  BufferCurveSetBuilder nan_builder(*nan_pt, 1.0, factory->getPrecisionModel(),
                                    params);
  EXPECT_TRUE(nan_builder.getCurves().empty());

  std::unique_ptr<Point> inf_pt = factory->createPoint(
      CoordinateXY(std::numeric_limits<double>::infinity(), 2.0));
  ASSERT_FALSE(inf_pt->isEmpty());
  BufferCurveSetBuilder inf_builder(*inf_pt, 1.0, factory->getPrecisionModel(),
                                    params);
  EXPECT_TRUE(inf_builder.getCurves().empty());
}

TEST(BufferCurveSetBuilderTest, LineStringDistancesAndSingleSided) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LineString> line =
      MakeLine(*factory, {{0.0, 0.0}, {10.0, 0.0}});

  {
    const BufferParameters params;
    BufferCurveSetBuilder zero_builder(*line, 0.0, factory->getPrecisionModel(),
                                       params);
    EXPECT_TRUE(zero_builder.getCurves().empty());

    BufferCurveSetBuilder neg_builder(*line, -2.0, factory->getPrecisionModel(),
                                      params);
    EXPECT_TRUE(neg_builder.getCurves().empty());

    BufferCurveSetBuilder pos_builder(*line, 2.0, factory->getPrecisionModel(),
                                      params);
    std::vector<SegmentString*>& curves = pos_builder.getCurves();
    ASSERT_EQ(curves.size(), 1u);
    ExpectCurveLabel(curves[0], Location::EXTERIOR, Location::INTERIOR);
    EXPECT_TRUE(curves[0]->isClosed());
    ExpectEnvelopeNear(curves[0], -2.0, 12.0, -2.0, 2.0, 1e-9);
  }

  {
    BufferParameters ss_params;
    ss_params.setSingleSided(true);

    BufferCurveSetBuilder left_builder(*line, 2.0, factory->getPrecisionModel(),
                                       ss_params);
    std::vector<SegmentString*>& left_curves = left_builder.getCurves();
    ASSERT_EQ(left_curves.size(), 1u);
    ExpectCurveLabel(left_curves[0], Location::EXTERIOR, Location::INTERIOR);
    ExpectEnvelopeNear(left_curves[0], 0.0, 10.0, 0.0, 2.0, 1e-9);

    BufferCurveSetBuilder right_builder(
        *line, -2.0, factory->getPrecisionModel(), ss_params);
    std::vector<SegmentString*>& right_curves = right_builder.getCurves();
    ASSERT_EQ(right_curves.size(), 1u);
    ExpectCurveLabel(right_curves[0], Location::EXTERIOR, Location::INTERIOR);
    ExpectEnvelopeNear(right_curves[0], 0.0, 10.0, -2.0, 0.0, 1e-9);
  }
}

TEST(BufferCurveSetBuilderTest, LineStringWithRepeatedAndInvalidPoints) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  std::unique_ptr<LineString> dirty_line = MakeLine(
      *factory, {{0.0, 0.0}, {0.0, 0.0}, {nan, 5.0}, {10.0, 0.0}, {10.0, 0.0}});

  const BufferParameters round_params;
  BufferCurveSetBuilder builder(*dirty_line, 1.0, factory->getPrecisionModel(),
                                round_params);
  std::vector<SegmentString*>& curves = builder.getCurves();
  ASSERT_EQ(curves.size(), 1u);
  ExpectCurveLabel(curves[0], Location::EXTERIOR, Location::INTERIOR);
  ExpectEnvelopeNear(curves[0], -1.0, 11.0, -1.0, 1.0, 1e-9);

  // Degenerate line collapsing to a single coordinate after duplicate removal.
  std::unique_ptr<LineString> collapsed =
      MakeLine(*factory, {{3.0, 4.0}, {3.0, 4.0}});
  BufferCurveSetBuilder pt_builder(*collapsed, 1.5,
                                   factory->getPrecisionModel(), round_params);
  std::vector<SegmentString*>& pt_curves = pt_builder.getCurves();
  ASSERT_EQ(pt_curves.size(), 1u);
  ExpectEnvelopeNear(pt_curves[0], 1.5, 4.5, 2.5, 5.5, 1e-9);

  BufferParameters flat_params(8, BufferParameters::CAP_FLAT);
  BufferCurveSetBuilder flat_builder(*collapsed, 1.5,
                                     factory->getPrecisionModel(), flat_params);
  EXPECT_TRUE(flat_builder.getCurves().empty());
}

TEST(BufferCurveSetBuilderTest, ClosedLineStringAndLinearRingSides) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  const BufferParameters params;

  // CW square ring [0, 10] x [0, 10] as a LineString and a LinearRing.
  std::unique_ptr<LineString> cw_line = MakeLine(
      *factory,
      {{0.0, 0.0}, {0.0, 10.0}, {10.0, 10.0}, {10.0, 0.0}, {0.0, 0.0}});
  std::unique_ptr<LinearRing> cw_ring = MakeRing(
      *factory,
      {{0.0, 0.0}, {0.0, 10.0}, {10.0, 10.0}, {10.0, 0.0}, {0.0, 0.0}});

  for (const Geometry* geom : {static_cast<const Geometry*>(cw_line.get()),
                               static_cast<const Geometry*>(cw_ring.get())}) {
    SCOPED_TRACE(geom->getGeometryType());
    // Small distance preserves both outer shell side and inner hole side.
    BufferCurveSetBuilder small_builder(*geom, 1.0,
                                        factory->getPrecisionModel(), params);
    std::vector<SegmentString*>& small_curves = small_builder.getCurves();
    ASSERT_EQ(small_curves.size(), 2u);
    ExpectCurveLabel(small_curves[0], Location::EXTERIOR, Location::INTERIOR);
    ExpectEnvelopeNear(small_curves[0], -1.0, 11.0, -1.0, 11.0, 1e-9);
    ExpectCurveLabel(small_curves[1], Location::INTERIOR, Location::EXTERIOR);
    ExpectEnvelopeNear(small_curves[1], 1.0, 9.0, 1.0, 9.0, 1e-9);

    // Large distance erodes the hole side completely; only the shell remains.
    BufferCurveSetBuilder large_builder(*geom, 6.0,
                                        factory->getPrecisionModel(), params);
    std::vector<SegmentString*>& large_curves = large_builder.getCurves();
    ASSERT_EQ(large_curves.size(), 1u);
    ExpectCurveLabel(large_curves[0], Location::EXTERIOR, Location::INTERIOR);
    ExpectEnvelopeNear(large_curves[0], -6.0, 16.0, -6.0, 16.0, 1e-9);
  }

  // CCW square ring [0, 10] x [0, 10]: hole is on LEFT, shell is on RIGHT.
  std::unique_ptr<LineString> ccw_line = MakeLine(
      *factory,
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});

  {
    BufferCurveSetBuilder small_ccw(*ccw_line, 1.0,
                                    factory->getPrecisionModel(), params);
    std::vector<SegmentString*>& curves = small_ccw.getCurves();
    ASSERT_EQ(curves.size(), 2u);
    ExpectCurveLabel(curves[0], Location::EXTERIOR, Location::INTERIOR);
    ExpectEnvelopeNear(curves[0], 1.0, 9.0, 1.0, 9.0, 1e-9);
    ExpectCurveLabel(curves[1], Location::INTERIOR, Location::EXTERIOR);
    ExpectEnvelopeNear(curves[1], -1.0, 11.0, -1.0, 11.0, 1e-9);
  }

  {
    BufferCurveSetBuilder large_ccw(*ccw_line, 6.0,
                                    factory->getPrecisionModel(), params);
    std::vector<SegmentString*>& curves = large_ccw.getCurves();
    ASSERT_EQ(curves.size(), 1u);
    ExpectCurveLabel(curves[0], Location::INTERIOR, Location::EXTERIOR);
    ExpectEnvelopeNear(curves[0], -6.0, 16.0, -6.0, 16.0, 1e-9);
  }

  // Single-sided buffer on a closed ring takes the non-ring line curve path.
  BufferParameters ss_params;
  ss_params.setSingleSided(true);
  BufferCurveSetBuilder ss_builder(*ccw_line, 1.0, factory->getPrecisionModel(),
                                   ss_params);
  std::vector<SegmentString*>& ss_curves = ss_builder.getCurves();
  ASSERT_EQ(ss_curves.size(), 1u);
  ExpectCurveLabel(ss_curves[0], Location::EXTERIOR, Location::INTERIOR);
  ExpectEnvelopeNear(ss_curves[0], 0.0, 10.0, 0.0, 10.0, 1e-9);
}

TEST(BufferCurveSetBuilderTest, PolygonShellOrientationsAndDistances) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  const BufferParameters params;

  std::unique_ptr<Polygon> cw_poly = MakePolygon(
      *factory,
      {{0.0, 0.0}, {0.0, 10.0}, {10.0, 10.0}, {10.0, 0.0}, {0.0, 0.0}});
  std::unique_ptr<Polygon> ccw_poly = MakePolygon(
      *factory,
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});

  struct PolyDistanceCase {
    double distance;
    double min_xy;
    double max_xy;
  };
  constexpr auto kDistCases = std::to_array<PolyDistanceCase>({
      {.distance = 1.0, .min_xy = -1.0, .max_xy = 11.0},
      {.distance = 0.0, .min_xy = 0.0, .max_xy = 10.0},
      {.distance = -1.0, .min_xy = 1.0, .max_xy = 9.0},
  });

  for (const auto& tc : kDistCases) {
    SCOPED_TRACE(tc.distance);

    BufferCurveSetBuilder cw_builder(*cw_poly, tc.distance,
                                     factory->getPrecisionModel(), params);
    std::vector<SegmentString*>& cw_curves = cw_builder.getCurves();
    ASSERT_EQ(cw_curves.size(), 1u);
    ExpectCurveLabel(cw_curves[0], Location::EXTERIOR, Location::INTERIOR);
    ExpectEnvelopeNear(cw_curves[0], tc.min_xy, tc.max_xy, tc.min_xy, tc.max_xy,
                       1e-9);

    BufferCurveSetBuilder ccw_builder(*ccw_poly, tc.distance,
                                      factory->getPrecisionModel(), params);
    std::vector<SegmentString*>& ccw_curves = ccw_builder.getCurves();
    ASSERT_EQ(ccw_curves.size(), 1u);
    ExpectCurveLabel(ccw_curves[0], Location::INTERIOR, Location::EXTERIOR);
    ExpectEnvelopeNear(ccw_curves[0], tc.min_xy, tc.max_xy, tc.min_xy,
                       tc.max_xy, 1e-9);
  }
}

TEST(BufferCurveSetBuilderTest, PolygonWithHolesAndHoleErosion) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  const BufferParameters params;

  auto shell = MakeRing(
      *factory,
      {{0.0, 0.0}, {20.0, 0.0}, {20.0, 20.0}, {0.0, 20.0}, {0.0, 0.0}});
  std::vector<std::unique_ptr<LinearRing>> holes;
  // Small CW hole [2, 4] x [2, 4] (min dimension 2.0).
  holes.push_back(MakeRing(
      *factory, {{2.0, 2.0}, {2.0, 4.0}, {4.0, 4.0}, {4.0, 2.0}, {2.0, 2.0}}));
  // Large CW hole [8, 18] x [8, 18] (min dimension 10.0).
  holes.push_back(MakeRing(
      *factory,
      {{8.0, 8.0}, {8.0, 18.0}, {18.0, 18.0}, {18.0, 8.0}, {8.0, 8.0}}));
  std::unique_ptr<Polygon> poly =
      factory->createPolygon(std::move(shell), std::move(holes));

  // Positive buffer of 2.0 erodes the small hole (2*2 > 2) and shrinks the
  // large hole to [10, 16] x [10, 16].
  {
    BufferCurveSetBuilder builder(*poly, 2.0, factory->getPrecisionModel(),
                                  params);
    std::vector<SegmentString*>& curves = builder.getCurves();
    ASSERT_EQ(curves.size(), 2u);
    ExpectCurveLabel(curves[0], Location::INTERIOR, Location::EXTERIOR);
    ExpectEnvelopeNear(curves[0], -2.0, 22.0, -2.0, 22.0, 1e-9);
    ExpectCurveLabel(curves[1], Location::INTERIOR, Location::EXTERIOR);
    ExpectEnvelopeNear(curves[1], 10.0, 16.0, 10.0, 16.0, 1e-9);
  }

  // Zero distance emits shell and both holes.
  {
    BufferCurveSetBuilder builder(*poly, 0.0, factory->getPrecisionModel(),
                                  params);
    EXPECT_EQ(builder.getCurves().size(), 3u);
  }

  // Negative buffer expands both holes and shrinks shell.
  {
    BufferCurveSetBuilder builder(*poly, -0.5, factory->getPrecisionModel(),
                                  params);
    std::vector<SegmentString*>& curves = builder.getCurves();
    ASSERT_EQ(curves.size(), 3u);
    ExpectEnvelopeNear(curves[0], 0.5, 19.5, 0.5, 19.5, 1e-9);
    ExpectEnvelopeNear(curves[1], 1.5, 4.5, 1.5, 4.5, 1e-9);
    ExpectEnvelopeNear(curves[2], 7.5, 18.5, 7.5, 18.5, 1e-9);
  }

  // CCW hole has its label flipped by addPolygonRingSide.
  auto shell2 = MakeRing(
      *factory,
      {{0.0, 0.0}, {20.0, 0.0}, {20.0, 20.0}, {0.0, 20.0}, {0.0, 0.0}});
  std::vector<std::unique_ptr<LinearRing>> ccw_holes;
  ccw_holes.push_back(MakeRing(
      *factory,
      {{8.0, 8.0}, {18.0, 8.0}, {18.0, 18.0}, {8.0, 18.0}, {8.0, 8.0}}));
  std::unique_ptr<Polygon> ccw_hole_poly =
      factory->createPolygon(std::move(shell2), std::move(ccw_holes));
  BufferCurveSetBuilder ccw_hole_builder(*ccw_hole_poly, 1.0,
                                         factory->getPrecisionModel(), params);
  std::vector<SegmentString*>& ccw_hole_curves = ccw_hole_builder.getCurves();
  ASSERT_EQ(ccw_hole_curves.size(), 2u);
  ExpectCurveLabel(ccw_hole_curves[0], Location::INTERIOR, Location::EXTERIOR);
  ExpectCurveLabel(ccw_hole_curves[1], Location::EXTERIOR, Location::INTERIOR);
  ExpectEnvelopeNear(ccw_hole_curves[1], 9.0, 17.0, 9.0, 17.0, 1e-9);
}

TEST(BufferCurveSetBuilderTest, PolygonShellErosionRectangularAndTriangular) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  const BufferParameters params;

  // Height-limited rectangle [0, 20] x [0, 4].
  std::unique_ptr<Polygon> wide_rect = MakePolygon(
      *factory, {{0.0, 0.0}, {20.0, 0.0}, {20.0, 4.0}, {0.0, 4.0}, {0.0, 0.0}});
  {
    BufferCurveSetBuilder eroded(*wide_rect, -2.5, factory->getPrecisionModel(),
                                 params);
    EXPECT_TRUE(eroded.getCurves().empty());

    BufferCurveSetBuilder surviving(*wide_rect, -1.5,
                                    factory->getPrecisionModel(), params);
    std::vector<SegmentString*>& curves = surviving.getCurves();
    ASSERT_EQ(curves.size(), 1u);
    ExpectEnvelopeNear(curves[0], 1.5, 18.5, 1.5, 2.5, 1e-9);
  }

  // Width-limited rectangle [0, 4] x [0, 20].
  std::unique_ptr<Polygon> tall_rect = MakePolygon(
      *factory, {{0.0, 0.0}, {4.0, 0.0}, {4.0, 20.0}, {0.0, 20.0}, {0.0, 0.0}});
  {
    BufferCurveSetBuilder eroded(*tall_rect, -2.5, factory->getPrecisionModel(),
                                 params);
    EXPECT_TRUE(eroded.getCurves().empty());
  }

  // 3-4-5 right triangle (6 x 8 x 10): exact inradius = 2.0.
  std::unique_ptr<Polygon> tri_poly =
      MakePolygon(*factory, {{0.0, 0.0}, {6.0, 0.0}, {0.0, 8.0}, {0.0, 0.0}});
  {
    BufferCurveSetBuilder eroded(*tri_poly, -2.1, factory->getPrecisionModel(),
                                 params);
    EXPECT_TRUE(eroded.getCurves().empty());

    BufferCurveSetBuilder surviving(*tri_poly, -1.5,
                                    factory->getPrecisionModel(), params);
    ASSERT_EQ(surviving.getCurves().size(), 1u);
  }

  // Triangular hole with exact inradius = 2.0 inside a large shell.
  auto big_shell = MakeRing(
      *factory,
      {{0.0, 0.0}, {20.0, 0.0}, {20.0, 20.0}, {0.0, 20.0}, {0.0, 0.0}});
  std::vector<std::unique_ptr<LinearRing>> tri_holes;
  tri_holes.push_back(
      MakeRing(*factory, {{4.0, 4.0}, {4.0, 12.0}, {10.0, 4.0}, {4.0, 4.0}}));
  std::unique_ptr<Polygon> poly_with_tri_hole =
      factory->createPolygon(std::move(big_shell), std::move(tri_holes));
  {
    BufferCurveSetBuilder hole_eroded(*poly_with_tri_hole, 2.1,
                                      factory->getPrecisionModel(), params);
    EXPECT_EQ(hole_eroded.getCurves().size(), 1u);

    BufferCurveSetBuilder hole_surviving(*poly_with_tri_hole, 1.5,
                                         factory->getPrecisionModel(), params);
    EXPECT_EQ(hole_surviving.getCurves().size(), 2u);
  }

  // Closed triangular LineString exercises isTriangleErodedCompletely via
  // addLinearRingSides.
  std::unique_ptr<LineString> tri_line =
      MakeLine(*factory, {{0.0, 0.0}, {6.0, 0.0}, {0.0, 8.0}, {0.0, 0.0}});
  {
    BufferCurveSetBuilder hole_eroded(*tri_line, 2.1,
                                      factory->getPrecisionModel(), params);
    EXPECT_EQ(hole_eroded.getCurves().size(), 1u);

    BufferCurveSetBuilder hole_surviving(*tri_line, 1.5,
                                         factory->getPrecisionModel(), params);
    EXPECT_EQ(hole_surviving.getCurves().size(), 2u);
  }
}

TEST(BufferCurveSetBuilderTest, PolygonDegenerateAndInvalidShellsAndHoles) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  const BufferParameters params;

  // Non-empty shell whose points are all non-finite triggers an exception
  // after RepeatedPointRemover removes all invalid coordinates.
  const double inf = std::numeric_limits<double>::infinity();
  std::unique_ptr<Polygon> inf_shell_poly =
      MakePolygon(*factory, {{inf, 0.0}, {inf, 1.0}, {inf, 2.0}, {inf, 0.0}});
  ASSERT_FALSE(inf_shell_poly->isEmpty());
  BufferCurveSetBuilder inf_builder(*inf_shell_poly, 1.0,
                                    factory->getPrecisionModel(), params);
  EXPECT_THROW(inf_builder.getCurves(), GEOSException);

  // Collapsed 3-identical-point shell (filters down to size 1).
  std::unique_ptr<Polygon> point_shell_poly =
      MakePolygon(*factory, {{5.0, 5.0}, {5.0, 5.0}, {5.0, 5.0}});
  {
    BufferCurveSetBuilder zero_builder(*point_shell_poly, 0.0,
                                       factory->getPrecisionModel(), params);
    EXPECT_TRUE(zero_builder.getCurves().empty());

    BufferCurveSetBuilder neg_builder(*point_shell_poly, -1.0,
                                      factory->getPrecisionModel(), params);
    EXPECT_TRUE(neg_builder.getCurves().empty());

    BufferCurveSetBuilder pos_builder(*point_shell_poly, 1.0,
                                      factory->getPrecisionModel(), params);
    std::vector<SegmentString*>& curves = pos_builder.getCurves();
    ASSERT_EQ(curves.size(), 1u);
    ExpectEnvelopeNear(curves[0], 4.0, 6.0, 4.0, 6.0, 1e-9);
  }

  // Collapsed 3-point back-and-forth shell (size 3 <= 3 in
  // isRingCurveInverted).
  std::unique_ptr<Polygon> seg_shell_poly =
      MakePolygon(*factory, {{0.0, 0.0}, {4.0, 0.0}, {0.0, 0.0}});
  {
    BufferCurveSetBuilder neg_builder(*seg_shell_poly, -1.0,
                                      factory->getPrecisionModel(), params);
    EXPECT_TRUE(neg_builder.getCurves().empty());

    BufferCurveSetBuilder pos_builder(*seg_shell_poly, 1.0,
                                      factory->getPrecisionModel(), params);
    ASSERT_EQ(pos_builder.getCurves().size(), 1u);
    ExpectEnvelopeNear(pos_builder.getCurves()[0], -1.0, 5.0, -1.0, 1.0, 1e-9);
  }

  // Valid shell with a collapsed 3-point hole hits the zero-distance flat
  // hole skip inside addPolygonRingSide and the size < 4 hole check at > 0.
  auto shell = MakeRing(
      *factory,
      {{0.0, 0.0}, {20.0, 0.0}, {20.0, 20.0}, {0.0, 20.0}, {0.0, 0.0}});
  std::vector<std::unique_ptr<LinearRing>> flat_holes;
  flat_holes.push_back(
      MakeRing(*factory, {{5.0, 5.0}, {5.0, 5.0}, {5.0, 5.0}}));
  std::unique_ptr<Polygon> poly_with_flat_hole =
      factory->createPolygon(std::move(shell), std::move(flat_holes));
  {
    BufferCurveSetBuilder zero_builder(*poly_with_flat_hole, 0.0,
                                       factory->getPrecisionModel(), params);
    EXPECT_EQ(zero_builder.getCurves().size(), 1u);

    BufferCurveSetBuilder pos_builder(*poly_with_flat_hole, 1.0,
                                      factory->getPrecisionModel(), params);
    EXPECT_EQ(pos_builder.getCurves().size(), 1u);
  }
}

TEST(BufferCurveSetBuilderTest, InvertedRingCurveDetectionAndThresholds) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  const BufferParameters params;

  // Thin diagonal rectangle (5 coords < MAX_INVERTED_RING_SIZE):
  // Envelope width and height are both 10.2 (> 2 * 0.20), so
  // isRingFullyEroded returns false, yet its true width is ~0.283. At
  // distance = -0.20, all vertices and midpoints of the inward offset curve lie
  // within ~0.141 < 0.99 * 0.20, so the curve is detected as inverted and
  // discarded.
  std::unique_ptr<Polygon> thin_quad = MakePolygon(
      *factory,
      {{0.0, 0.0}, {10.0, 10.0}, {10.2, 9.8}, {0.2, -0.2}, {0.0, 0.0}});
  {
    BufferCurveSetBuilder builder(*thin_quad, -0.20,
                                  factory->getPrecisionModel(), params);
    EXPECT_TRUE(builder.getCurves().empty());
  }

  // Same thin diagonal shape represented with 9 coordinates
  // (>= MAX_INVERTED_RING_SIZE) bypasses the inversion heuristic.
  std::unique_ptr<Polygon> thin_nine_pts = MakePolygon(*factory, {{0.0, 0.0},
                                                                  {2.5, 2.52},
                                                                  {5.0, 5.03},
                                                                  {7.5, 7.52},
                                                                  {10.0, 10.0},
                                                                  {10.2, 9.8},
                                                                  {5.2, 4.78},
                                                                  {0.2, -0.2},
                                                                  {0.0, 0.0}});
  {
    BufferCurveSetBuilder builder(*thin_nine_pts, -0.25,
                                  factory->getPrecisionModel(), params);
    EXPECT_EQ(builder.getCurves().size(), 1u);
  }

  // Mitre-joined outward square (5 input pts, 5 curve pts <= 4 * 5) reaches
  // hasPointOnBuffer and passes via vertex distance.
  BufferParameters mitre_params(4, BufferParameters::CAP_ROUND,
                                BufferParameters::JOIN_MITRE, 5.0);
  std::unique_ptr<Polygon> square = MakePolygon(
      *factory,
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});
  {
    BufferCurveSetBuilder builder(*square, 1.0, factory->getPrecisionModel(),
                                  mitre_params);
    std::vector<SegmentString*>& curves = builder.getCurves();
    ASSERT_EQ(curves.size(), 1u);
    ExpectEnvelopeNear(curves[0], -1.0, 11.0, -1.0, 11.0, 1e-9);
  }

  // Tapered trapezoid with narrow left end (height 0.5) and wide right end
  // (height 4.0) at distance = -1.0: the first inside-turn vertices near the
  // narrow end lie within distTol = 0.99, while the midpoint along the long
  // offset segment lies >= 1.0 > 0.99 from all edges.
  std::unique_ptr<Polygon> tapered = MakePolygon(
      *factory,
      {{0.0, 0.0}, {10.0, -2.0}, {10.0, 2.0}, {0.0, 0.5}, {0.0, 0.0}});
  {
    BufferCurveSetBuilder builder(*tapered, -1.0, factory->getPrecisionModel(),
                                  params);
    std::vector<SegmentString*>& curves = builder.getCurves();
    ASSERT_EQ(curves.size(), 1u);
    ExpectCurveLabel(curves[0], Location::INTERIOR, Location::EXTERIOR);
  }
}

TEST(BufferCurveSetBuilderTest, SetInvertOrientationOnRingsAndBowtie) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  const BufferParameters params;

  std::unique_ptr<Polygon> ccw_square = MakePolygon(
      *factory,
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});

  {
    BufferCurveSetBuilder normal(*ccw_square, 0.0, factory->getPrecisionModel(),
                                 params);
    normal.setInvertOrientation(false);
    std::vector<SegmentString*>& curves = normal.getCurves();
    ASSERT_EQ(curves.size(), 1u);
    ExpectCurveLabel(curves[0], Location::INTERIOR, Location::EXTERIOR);
  }

  {
    BufferCurveSetBuilder inverted(*ccw_square, 0.0,
                                   factory->getPrecisionModel(), params);
    inverted.setInvertOrientation(true);
    std::vector<SegmentString*>& curves = inverted.getCurves();
    ASSERT_EQ(curves.size(), 1u);
    ExpectCurveLabel(curves[0], Location::EXTERIOR, Location::INTERIOR);
  }

  // Asymmetric figure-8 ring: large CW lobe on right (area 100) dominates small
  // CCW lobe on left (area 4). Inverting orientation switches sides/labels.
  std::unique_ptr<Polygon> bowtie = MakePolygon(*factory, {{0.0, 0.0},
                                                           {10.0, 10.0},
                                                           {10.0, -10.0},
                                                           {0.0, 0.0},
                                                           {-2.0, 2.0},
                                                           {-2.0, -2.0},
                                                           {0.0, 0.0}});
  {
    BufferCurveSetBuilder normal(*bowtie, 0.0, factory->getPrecisionModel(),
                                 params);
    ASSERT_EQ(normal.getCurves().size(), 1u);
    ExpectCurveLabel(normal.getCurves()[0], Location::EXTERIOR,
                     Location::INTERIOR);
  }
  {
    BufferCurveSetBuilder inverted(*bowtie, 0.0, factory->getPrecisionModel(),
                                   params);
    inverted.setInvertOrientation(true);
    ASSERT_EQ(inverted.getCurves().size(), 1u);
    ExpectCurveLabel(inverted.getCurves()[0], Location::INTERIOR,
                     Location::EXTERIOR);
  }
}

TEST(BufferCurveSetBuilderTest, CollectionsAndUnsupportedGeometryTypes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  const BufferParameters params;

  // MultiPoint with one empty point and two valid points.
  {
    std::vector<std::unique_ptr<Point>> pts;
    pts.push_back(factory->createPoint());
    pts.push_back(factory->createPoint(CoordinateXY(0.0, 0.0)));
    pts.push_back(factory->createPoint(CoordinateXY(10.0, 0.0)));
    std::unique_ptr<MultiPoint> mp = factory->createMultiPoint(std::move(pts));

    BufferCurveSetBuilder builder(*mp, 1.0, factory->getPrecisionModel(),
                                  params);
    std::vector<SegmentString*>& curves = builder.getCurves();
    ASSERT_EQ(curves.size(), 2u);
    ExpectEnvelopeNear(curves[0], -1.0, 1.0, -1.0, 1.0, 1e-9);
    ExpectEnvelopeNear(curves[1], 9.0, 11.0, -1.0, 1.0, 1e-9);
  }

  // MultiLineString with one open line and one closed CCW ring.
  {
    std::vector<std::unique_ptr<LineString>> lines;
    lines.push_back(MakeLine(*factory, {{0.0, 0.0}, {4.0, 0.0}}));
    lines.push_back(MakeLine(
        *factory,
        {{10.0, 0.0}, {20.0, 0.0}, {20.0, 10.0}, {10.0, 10.0}, {10.0, 0.0}}));
    std::unique_ptr<MultiLineString> mls =
        factory->createMultiLineString(std::move(lines));

    BufferCurveSetBuilder builder(*mls, 1.0, factory->getPrecisionModel(),
                                  params);
    EXPECT_EQ(builder.getCurves().size(), 3u);
  }

  // MultiPolygon where one polygon erodes completely at negative distance.
  {
    std::vector<std::unique_ptr<Polygon>> polys;
    polys.push_back(MakePolygon(
        *factory,
        {{0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}, {0.0, 2.0}, {0.0, 0.0}}));
    polys.push_back(MakePolygon(
        *factory,
        {{10.0, 0.0}, {20.0, 0.0}, {20.0, 10.0}, {10.0, 10.0}, {10.0, 0.0}}));
    std::unique_ptr<MultiPolygon> mpoly =
        factory->createMultiPolygon(std::move(polys));

    BufferCurveSetBuilder builder(*mpoly, -2.0, factory->getPrecisionModel(),
                                  params);
    std::vector<SegmentString*>& curves = builder.getCurves();
    ASSERT_EQ(curves.size(), 1u);
    ExpectEnvelopeNear(curves[0], 12.0, 18.0, 2.0, 8.0, 1e-9);
  }

  // Heterogeneous nested GeometryCollection.
  {
    std::vector<std::unique_ptr<Geometry>> inner;
    inner.push_back(factory->createPoint(CoordinateXY(-5.0, -5.0)));
    std::vector<std::unique_ptr<Geometry>> outer;
    outer.push_back(factory->createGeometryCollection(std::move(inner)));
    outer.push_back(MakeLine(*factory, {{0.0, 0.0}, {5.0, 0.0}}));
    outer.push_back(MakePolygon(
        *factory,
        {{10.0, 0.0}, {14.0, 0.0}, {14.0, 4.0}, {10.0, 4.0}, {10.0, 0.0}}));
    std::unique_ptr<GeometryCollection> gc =
        factory->createGeometryCollection(std::move(outer));

    BufferCurveSetBuilder builder(*gc, 1.0, factory->getPrecisionModel(),
                                  params);
    EXPECT_EQ(builder.getCurves().size(), 3u);
  }

  // Curved geometry types are not handled by BufferCurveSetBuilder directly.
  std::unique_ptr<Curve> arc = factory->createCircularString(
      MakeSeqXY({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}));
  BufferCurveSetBuilder arc_builder(*arc, 1.0, factory->getPrecisionModel(),
                                    params);
  EXPECT_THROW(arc_builder.getCurves(), UnsupportedOperationException);

  std::unique_ptr<Curve> closed_arc = factory->createCircularString(
      MakeSeqXY({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}, {1.0, -1.0}, {0.0, 0.0}}));
  std::unique_ptr<CurvePolygon> curve_poly =
      factory->createCurvePolygon(std::move(closed_arc));
  std::vector<std::unique_ptr<Geometry>> curved_items;
  curved_items.push_back(std::move(curve_poly));
  std::unique_ptr<GeometryCollection> curved_gc =
      factory->createGeometryCollection(std::move(curved_items));
  BufferCurveSetBuilder curved_gc_builder(*curved_gc, 1.0,
                                          factory->getPrecisionModel(), params);
  EXPECT_THROW(curved_gc_builder.getCurves(), UnsupportedOperationException);
}

TEST(BufferCurveSetBuilderTest, AddCurvesDirectAndCoordinateDimensions) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> empty_pt = factory->createPoint();
  const BufferParameters params;

  BufferCurveSetBuilder builder(*empty_pt, 1.0, factory->getPrecisionModel(),
                                params);

  // Empty lineList adds nothing.
  builder.addCurves({}, Location::EXTERIOR, Location::INTERIOR);
  EXPECT_TRUE(builder.getCurves().empty());

  // Sequences with size < 2 are deleted and skipped cleanly.
  auto seq_empty = std::make_unique<CoordinateSequence>(0u, false, false);
  auto seq_single = std::make_unique<CoordinateSequence>(1u, false, false);
  seq_single->setAt(CoordinateXY(1.0, 2.0), 0);

  // Sequences with size >= 2 preserve Z/M flags on NodedSegmentString.
  auto seq_xyz = std::make_unique<CoordinateSequence>(2u, true, false);
  seq_xyz->setAt(Coordinate(0.0, 0.0, 10.0), 0);
  seq_xyz->setAt(Coordinate(1.0, 0.0, 20.0), 1);

  auto seq_xym = std::make_unique<CoordinateSequence>(2u, false, true);
  seq_xym->setAt(CoordinateXYM(0.0, 0.0, 3.0), 0);
  seq_xym->setAt(CoordinateXYM(0.0, 1.0, 4.0), 1);

  auto seq_xyzm = std::make_unique<CoordinateSequence>(2u, true, true);
  seq_xyzm->setAt(CoordinateXYZM(1.0, 1.0, 5.0, 6.0), 0);
  seq_xyzm->setAt(CoordinateXYZM(2.0, 2.0, 7.0, 8.0), 1);

  std::vector<CoordinateSequence*> inputs;
  inputs.reserve(5);
  inputs.push_back(seq_empty.release());
  inputs.push_back(seq_single.release());
  inputs.push_back(seq_xyz.release());
  inputs.push_back(seq_xym.release());
  inputs.push_back(seq_xyzm.release());

  builder.addCurves(inputs, Location::INTERIOR, Location::EXTERIOR);
  std::vector<SegmentString*>& curves = builder.getCurves();
  ASSERT_EQ(curves.size(), 3u);

  struct ExpectedDim {
    bool has_z;
    bool has_m;
  };
  constexpr auto kExpectedDims = std::to_array<ExpectedDim>({
      {.has_z = true, .has_m = false},
      {.has_z = false, .has_m = true},
      {.has_z = true, .has_m = true},
  });

  for (std::size_t i = 0; i < kExpectedDims.size(); ++i) {
    SCOPED_TRACE(i);
    ExpectCurveLabel(curves[i], Location::INTERIOR, Location::EXTERIOR);
    const auto* nss = dynamic_cast<const NodedSegmentString*>(curves[i]);
    ASSERT_NE(nss, nullptr);
    EXPECT_EQ(nss->getNodeList().getConstructZ(), kExpectedDims[i].has_z);
    EXPECT_EQ(nss->getNodeList().getConstructM(), kExpectedDims[i].has_m);
  }
}

TEST(BufferCurveSetBuilderTest, FixedPrecisionModelRoundsCurveVertices) {
  PrecisionModel fixed_pm(1.0);
  GeometryFactory::Ptr factory = GeometryFactory::create(&fixed_pm);
  std::unique_ptr<Point> pt = factory->createPoint(CoordinateXY(0.3, 0.4));
  const BufferParameters params;

  BufferCurveSetBuilder builder(*pt, 2.4, &fixed_pm, params);
  std::vector<SegmentString*>& curves = builder.getCurves();
  ASSERT_EQ(curves.size(), 1u);

  const CoordinateSequence* seq = curves[0]->getCoordinates();
  ASSERT_GE(seq->size(), 4u);
  for (std::size_t i = 0; i < seq->size(); ++i) {
    const CoordinateXY& c = seq->getAt<CoordinateXY>(i);
    EXPECT_DOUBLE_EQ(c.x, std::round(c.x));
    EXPECT_DOUBLE_EQ(c.y, std::round(c.y));
  }
}

}  // namespace
}  // namespace geos_google
