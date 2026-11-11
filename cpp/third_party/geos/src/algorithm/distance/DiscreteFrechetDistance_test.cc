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
// third_party/geos/src/algorithm/distance/DiscreteFrechetDistance.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/algorithm/distance/DiscreteFrechetDistance.cpp

#include "third_party/geos/include/geos/algorithm/distance/DiscreteFrechetDistance.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/GEOSException.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/IllegalStateException.h"
#include "third_party/geos/include/geos/util/Interrupt.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::algorithm::distance::DiscreteFrechetDistance;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::geom::Point;
using geos::io::WKTReader;
using geos::util::CurrentThreadInterrupt;
using geos::util::GEOSException;
using geos::util::IllegalArgumentException;
using geos::util::IllegalStateException;
using geos::util::UnsupportedOperationException;
using ::testing::HasSubstr;

struct WktDistanceCase {
  std::string_view name;
  std::string_view wkt0;
  std::string_view wkt1;
  double expected_distance;
};

constexpr auto kWktDistanceCases = std::to_array<WktDistanceCase>({
    {
        .name = "identical_points",
        .wkt0 = "POINT (3 4)",
        .wkt1 = "POINT (3 4)",
        .expected_distance = 0.0,
    },
    {
        .name = "distinct_points",
        .wkt0 = "POINT (0 0)",
        .wkt1 = "POINT (3 4)",
        .expected_distance = 5.0,
    },
    {
        .name = "identical_linestrings",
        .wkt0 = "LINESTRING (0 0, 1 1, 2 0)",
        .wkt1 = "LINESTRING (0 0, 1 1, 2 0)",
        .expected_distance = 0.0,
    },
    {
        .name = "parallel_equal_vertex_count",
        .wkt0 = "LINESTRING (0 0, 5 0, 10 0)",
        .wkt1 = "LINESTRING (0 2, 5 2, 10 2)",
        .expected_distance = 2.0,
    },
    {
        .name = "opposite_direction_lines",
        .wkt0 = "LINESTRING (0 0, 10 0)",
        .wkt1 = "LINESTRING (10 0, 0 0)",
        .expected_distance = 10.0,
    },
    {
        .name = "point_vs_linestring",
        .wkt0 = "POINT (0 0)",
        .wkt1 = "LINESTRING (0 1, 3 4, 0 2)",
        .expected_distance = 5.0,
    },
    {
        .name = "linestring_vs_point",
        .wkt0 = "LINESTRING (0 1, 3 4, 0 2)",
        .wkt1 = "POINT (0 0)",
        .expected_distance = 5.0,
    },
    {
        .name = "equal_3x3_diagonal",
        .wkt0 = "LINESTRING (0 0, 5 0, 10 0)",
        .wkt1 = "LINESTRING (0 3, 5 4, 10 3)",
        .expected_distance = 4.0,
    },
    {
        .name = "shifted_polygons",
        .wkt0 = "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
        .wkt1 = "POLYGON ((3 0, 5 0, 5 2, 3 2, 3 0))",
        .expected_distance = 3.0,
    },
    {
        .name = "polygons_with_holes",
        .wkt0 = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
                "(2 2, 8 2, 8 8, 2 8, 2 2))",
        .wkt1 = "POLYGON ((0 1, 10 1, 10 11, 0 11, 0 1), "
                "(2 3, 8 3, 8 9, 2 9, 2 3))",
        .expected_distance = 1.0,
    },
    {
        .name = "multipoints",
        .wkt0 = "MULTIPOINT ((0 0), (3 4))",
        .wkt1 = "MULTIPOINT ((0 0), (0 0))",
        .expected_distance = 5.0,
    },
    {
        .name = "multilinestring_vs_linestring",
        .wkt0 = "MULTILINESTRING ((0 0, 2 0), (4 0, 6 0))",
        .wkt1 = "LINESTRING (0 1, 2 1, 4 1, 6 1)",
        .expected_distance = 1.0,
    },
    {
        .name = "geometry_collection_vs_linestring",
        .wkt0 = "GEOMETRYCOLLECTION (POINT (0 0), LINESTRING (1 0, 2 0))",
        .wkt1 = "LINESTRING (0 2, 1 2, 2 2)",
        .expected_distance = 2.0,
    },
    {
        .name = "point_z_ignores_z_ordinate",
        .wkt0 = "POINT Z (0 0 100)",
        .wkt1 = "POINT Z (3 4 0)",
        .expected_distance = 5.0,
    },
    {
        .name = "linestring_zm_ignores_z_and_m",
        .wkt0 = "LINESTRING ZM (0 0 50 1, 4 0 80 2)",
        .wkt1 = "LINESTRING ZM (0 3 -10 9, 4 3 99 8)",
        .expected_distance = 3.0,
    },
});

TEST(DiscreteFrechetDistanceTest, StandardGeometryPairs) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const auto& tc : kWktDistanceCases) {
    SCOPED_TRACE(tc.name);
    Geometry::Ptr g0 = reader.read(std::string(tc.wkt0));
    Geometry::Ptr g1 = reader.read(std::string(tc.wkt1));
    ASSERT_NE(g0, nullptr);
    ASSERT_NE(g1, nullptr);
    EXPECT_NEAR(DiscreteFrechetDistance::distance(*g0, *g1),
                tc.expected_distance, 1e-12);
  }
}

TEST(DiscreteFrechetDistanceTest, AsymmetricVertexCountsBothDirections) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // 5 vertices vs 3 vertices exercises bresenhamDiagonal for both
  // numCols > numRows and numCols <= numRows with err > 0 true and false.
  Geometry::Ptr line5 = reader.read("LINESTRING (0 0, 2 0, 4 0, 6 0, 8 0)");
  Geometry::Ptr line3 = reader.read("LINESTRING (0 1, 4 1, 8 1)");
  ASSERT_NE(line5, nullptr);
  ASSERT_NE(line3, nullptr);

  const double expected = std::sqrt(5.0);
  EXPECT_NEAR(DiscreteFrechetDistance::distance(*line5, *line3), expected,
              1e-12);
  EXPECT_NEAR(DiscreteFrechetDistance::distance(*line3, *line5), expected,
              1e-12);
}

TEST(DiscreteFrechetDistanceTest, InstanceGetCoordinatesCachesResult) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr g0 = reader.read("LINESTRING (0 0, 5 10, 10 0)");
  Geometry::Ptr g1 = reader.read("LINESTRING (0 0, 5 2, 10 0)");
  ASSERT_NE(g0, nullptr);
  ASSERT_NE(g1, nullptr);

  DiscreteFrechetDistance dfd(*g0, *g1);
  // First call initializes ptDist; second call returns the cached pair.
  std::array<CoordinateXY, 2> first_coords = dfd.getCoordinates();
  std::array<CoordinateXY, 2> second_coords = dfd.getCoordinates();

  EXPECT_EQ(first_coords[0], second_coords[0]);
  EXPECT_EQ(first_coords[1], second_coords[1]);
  EXPECT_EQ(first_coords[0], CoordinateXY(5.0, 10.0));
  EXPECT_EQ(first_coords[1], CoordinateXY(5.0, 2.0));
  EXPECT_DOUBLE_EQ(first_coords[0].distance(first_coords[1]), 8.0);
}

TEST(DiscreteFrechetDistanceTest, BottleneckPropagationRowAndColumn) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // 4x3 and 3x4 shapes where the middle spike forces dist > distances.get(...)
  // in both the i-loop and j-loop of computeFrechet, and breaks on unset cells.
  Geometry::Ptr spike4 = reader.read("LINESTRING (0 0, 0 5, 0 10, 0 0)");
  Geometry::Ptr bump3 = reader.read("LINESTRING (0 0, 0 1, 0 0)");
  ASSERT_NE(spike4, nullptr);
  ASSERT_NE(bump3, nullptr);

  DiscreteFrechetDistance dfd_4x3(*spike4, *bump3);
  std::array<CoordinateXY, 2> coords_4x3 = dfd_4x3.getCoordinates();
  EXPECT_DOUBLE_EQ(coords_4x3[0].distance(coords_4x3[1]), 9.0);
  EXPECT_EQ(coords_4x3[0], CoordinateXY(0.0, 10.0));
  EXPECT_EQ(coords_4x3[1], CoordinateXY(0.0, 1.0));

  DiscreteFrechetDistance dfd_3x4(*bump3, *spike4);
  std::array<CoordinateXY, 2> coords_3x4 = dfd_3x4.getCoordinates();
  EXPECT_DOUBLE_EQ(coords_3x4[0].distance(coords_3x4[1]), 9.0);
  EXPECT_EQ(coords_3x4[0], CoordinateXY(0.0, 1.0));
  EXPECT_EQ(coords_3x4[1], CoordinateXY(0.0, 10.0));
}

TEST(DiscreteFrechetDistanceTest, OffDiagonalIminAndJminFallback) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // At diagonal entry (3, 3) the distance is 100.0 (setting maxDistOnDiag),
  // so k=0 advances imin/jmin to 4. At k=2 (1, 1), dist((-50,0),(80,0)) = 130
  // exceeds maxDistOnDiag, yet i < imin and j < jmin keep expanding.
  Geometry::Ptr g0 = reader.read("LINESTRING (0 0, -50 0, 80 0, 0 0)");
  Geometry::Ptr g1 = reader.read("LINESTRING (0 0, -50 0, 80 0, 100 0)");
  ASSERT_NE(g0, nullptr);
  ASSERT_NE(g1, nullptr);

  EXPECT_DOUBLE_EQ(DiscreteFrechetDistance::distance(*g0, *g1), 100.0);
  EXPECT_DOUBLE_EQ(DiscreteFrechetDistance::distance(*g1, *g0), 100.0);
}

TEST(DiscreteFrechetDistanceTest, DensificationReducesVertexDiscrepancy) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr baseline = reader.read("LINESTRING (0 0, 100 0)");
  Geometry::Ptr peak = reader.read("LINESTRING (0 0, 50 10, 100 0)");
  Geometry::Ptr offset_peak = reader.read("LINESTRING (0 0, 50.5 10, 100 0)");
  ASSERT_NE(baseline, nullptr);
  ASSERT_NE(peak, nullptr);
  ASSERT_NE(offset_peak, nullptr);

  // Without densification, (50, 10) must match one of the endpoints at x=0/100.
  EXPECT_NEAR(DiscreteFrechetDistance::distance(*baseline, *peak),
              std::hypot(50.0, 10.0), 1e-12);

  // With fraction 1.0, each edge has 1 subsegment (vertices only).
  EXPECT_NEAR(DiscreteFrechetDistance::distance(*baseline, *peak, 1.0),
              std::hypot(50.0, 10.0), 1e-12);

  // With fraction 0.5, midpoint (50, 0) matches (50, 10), while (25, 5)
  // matches (0, 0) or (50, 0) at distance hypot(25, 5).
  EXPECT_NEAR(DiscreteFrechetDistance::distance(*baseline, *peak, 0.5),
              std::hypot(25.0, 5.0), 1e-12);

  // With fine densification (0.01), baseline has a vertex at (50, 0) directly
  // below the peak (50, 10), yielding the exact height 10.0.
  DiscreteFrechetDistance dfd(*baseline, *peak);
  dfd.setDensifyFraction(0.01);
  std::array<CoordinateXY, 2> coords = dfd.getCoordinates();
  EXPECT_DOUBLE_EQ(coords[0].distance(coords[1]), 10.0);
  EXPECT_EQ(coords[0], CoordinateXY(50.0, 0.0));
  EXPECT_EQ(coords[1], CoordinateXY(50.0, 10.0));

  // When the peak x=50.5 falls halfway between densified baseline vertices at
  // x=50 and x=51, the discrepancy is hypot(0.5, 10.0).
  EXPECT_NEAR(DiscreteFrechetDistance::distance(*baseline, *offset_peak, 0.01),
              std::hypot(0.5, 10.0), 1e-12);

  // Also test near the lower valid bound (> 0.001).
  EXPECT_NEAR(DiscreteFrechetDistance::distance(*baseline, *peak, 0.002), 10.0,
              1e-12);
}

TEST(DiscreteFrechetDistanceTest, DensificationPreservesSinglePointGeometry) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // Exercises seq.size() == 1 branch in DensifiedCoordinatesFilter::filter_ro
  // as well as intermediate and final vertices of a multi-segment LineString.
  Geometry::Ptr pt = reader.read("POINT (5 3)");
  Geometry::Ptr line = reader.read("LINESTRING (0 0, 5 0, 10 0)");
  ASSERT_NE(pt, nullptr);
  ASSERT_NE(line, nullptr);

  const double expected = std::hypot(5.0, 3.0);
  EXPECT_NEAR(DiscreteFrechetDistance::distance(*pt, *line, 0.5), expected,
              1e-12);
  EXPECT_NEAR(DiscreteFrechetDistance::distance(*line, *pt, 0.5), expected,
              1e-12);
}

TEST(DiscreteFrechetDistanceTest, DensificationOnMultiAndPolygonalGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr mp0 = reader.read("MULTIPOINT ((0 0), (3 4))");
  Geometry::Ptr mp1 = reader.read("MULTIPOINT ((0 0), (0 0))");
  ASSERT_NE(mp0, nullptr);
  ASSERT_NE(mp1, nullptr);
  EXPECT_DOUBLE_EQ(DiscreteFrechetDistance::distance(*mp0, *mp1, 0.5), 5.0);

  Geometry::Ptr mls =
      reader.read("MULTILINESTRING ((0 0, 10 0), (10 0, 20 0))");
  Geometry::Ptr line = reader.read("LINESTRING (0 1, 20 1)");
  ASSERT_NE(mls, nullptr);
  ASSERT_NE(line, nullptr);
  EXPECT_NEAR(DiscreteFrechetDistance::distance(*mls, *line, 0.5),
              std::hypot(5.0, 1.0), 1e-12);

  Geometry::Ptr poly0 = reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  Geometry::Ptr poly1 = reader.read("POLYGON ((0 1, 10 1, 10 11, 0 11, 0 1))");
  ASSERT_NE(poly0, nullptr);
  ASSERT_NE(poly1, nullptr);
  EXPECT_DOUBLE_EQ(DiscreteFrechetDistance::distance(*poly0, *poly1, 0.5), 1.0);
}

TEST(DiscreteFrechetDistanceTest, EmptyGeometriesThrowIllegalArgument) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr empty_pt = reader.read("POINT EMPTY");
  Geometry::Ptr empty_line = reader.read("LINESTRING EMPTY");
  Geometry::Ptr empty_coll = reader.read("GEOMETRYCOLLECTION EMPTY");
  Geometry::Ptr pt = reader.read("POINT (1 2)");
  ASSERT_NE(empty_pt, nullptr);
  ASSERT_NE(empty_line, nullptr);
  ASSERT_NE(empty_coll, nullptr);
  ASSERT_NE(pt, nullptr);

  EXPECT_THROW(DiscreteFrechetDistance::distance(*empty_pt, *pt),
               IllegalArgumentException);
  EXPECT_THROW(DiscreteFrechetDistance::distance(*pt, *empty_line),
               IllegalArgumentException);
  EXPECT_THROW(DiscreteFrechetDistance::distance(*empty_coll, *empty_pt),
               IllegalArgumentException);
  DiscreteFrechetDistance dfd(*empty_pt, *pt);
  EXPECT_THROW(dfd.getCoordinates(), IllegalArgumentException);
}

TEST(DiscreteFrechetDistanceTest, InvalidDensifyFractionsThrowIllegalArgument) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr g0 = reader.read("LINESTRING (0 0, 1 0)");
  Geometry::Ptr g1 = reader.read("LINESTRING (0 1, 1 1)");
  ASSERT_NE(g0, nullptr);
  ASSERT_NE(g1, nullptr);

  constexpr auto kBadFractions = std::to_array<double>({
      geos::DoubleNotANumber,
      1.0001,
      2.0,
      std::numeric_limits<double>::infinity(),
      0.001,
      0.0,
      -0.5,
      -std::numeric_limits<double>::infinity(),
  });

  for (double frac : kBadFractions) {
    SCOPED_TRACE(frac);
    EXPECT_THROW(DiscreteFrechetDistance::distance(*g0, *g1, frac),
                 IllegalArgumentException);
    DiscreteFrechetDistance dfd(*g0, *g1);
    EXPECT_THROW(dfd.setDensifyFraction(frac), IllegalArgumentException);
  }
}

TEST(DiscreteFrechetDistanceTest, CurvedGeometriesThrowUnsupported) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr arc = reader.read("CIRCULARSTRING (0 0, 1 1, 2 0)");
  Geometry::Ptr compound =
      reader.read("COMPOUNDCURVE ((0 0, 1 1), CIRCULARSTRING (1 1, 2 0, 3 1))");
  Geometry::Ptr curve_poly =
      reader.read("CURVEPOLYGON (CIRCULARSTRING (0 0, 2 2, 4 0, 2 -2, 0 0))");
  Geometry::Ptr line = reader.read("LINESTRING (0 0, 2 0)");
  ASSERT_NE(arc, nullptr);
  ASSERT_NE(compound, nullptr);
  ASSERT_NE(curve_poly, nullptr);
  ASSERT_NE(line, nullptr);

  EXPECT_THROW(DiscreteFrechetDistance::distance(*arc, *line),
               UnsupportedOperationException);
  EXPECT_THROW(DiscreteFrechetDistance::distance(*line, *arc),
               UnsupportedOperationException);
  EXPECT_THROW(DiscreteFrechetDistance::distance(*compound, *line),
               UnsupportedOperationException);
  EXPECT_THROW(DiscreteFrechetDistance::distance(*line, *curve_poly),
               UnsupportedOperationException);
}

TEST(DiscreteFrechetDistanceTest, NanCoordinateThrowsIllegalState) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Point> nan_pt =
      factory->createPoint(CoordinateXY(geos::DoubleNotANumber, 0.0));
  std::unique_ptr<Point> origin = factory->createPoint(CoordinateXY(0.0, 0.0));
  ASSERT_NE(nan_pt, nullptr);
  ASSERT_NE(origin, nullptr);

  EXPECT_THROW(DiscreteFrechetDistance::distance(*nan_pt, *origin),
               IllegalStateException);
  EXPECT_THROW(DiscreteFrechetDistance::distance(*origin, *nan_pt),
               IllegalStateException);
  EXPECT_THROW(DiscreteFrechetDistance::distance(*nan_pt, *origin, 0.5),
               IllegalStateException);
}

TEST(DiscreteFrechetDistanceTest, LargeInputsUseCsrMatrixStorage) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // >= 1024 coordinates switches createMatrixStorage from RectMatrix to
  // CsrMatrix. Test exact threshold (1024) as well as asymmetric shapes.
  constexpr std::size_t kNumPts = 1024;
  auto seq0 = std::make_unique<CoordinateSequence>(0u, false, false);
  auto seq1 = std::make_unique<CoordinateSequence>(0u, false, false);
  seq0->reserve(kNumPts);
  seq1->reserve(kNumPts);
  for (std::size_t i = 0; i < kNumPts; ++i) {
    seq0->add(static_cast<double>(i), 0.0);
    seq1->add(static_cast<double>(i), 2.0);
  }
  std::unique_ptr<LineString> line0 =
      factory->createLineString(std::move(seq0));
  std::unique_ptr<LineString> line1 =
      factory->createLineString(std::move(seq1));
  ASSERT_NE(line0, nullptr);
  ASSERT_NE(line1, nullptr);

  EXPECT_DOUBLE_EQ(DiscreteFrechetDistance::distance(*line0, *line1), 2.0);

  // Asymmetric 1024 vs 3 and 2 vs 1024 exercise off-diagonal row/col insertions
  // in CsrMatrix when either rows >= 1024 or cols >= 1024.
  auto seq_short = std::make_unique<CoordinateSequence>(0u, false, false);
  seq_short->add(0.0, 1.0);
  seq_short->add(512.0, 1.0);
  seq_short->add(1023.0, 1.0);
  std::unique_ptr<LineString> line_short =
      factory->createLineString(std::move(seq_short));
  ASSERT_NE(line_short, nullptr);

  EXPECT_NEAR(DiscreteFrechetDistance::distance(*line0, *line_short),
              std::hypot(256.0, 1.0), 1e-12);

  auto seq_two = std::make_unique<CoordinateSequence>(0u, false, false);
  seq_two->add(0.0, 1.0);
  seq_two->add(1023.0, 1.0);
  std::unique_ptr<LineString> line_two =
      factory->createLineString(std::move(seq_two));
  ASSERT_NE(line_two, nullptr);

  EXPECT_NEAR(DiscreteFrechetDistance::distance(*line_two, *line0),
              std::hypot(511.0, 1.0), 1e-12);
}

struct InterruptState {
  int calls = 0;
  int trigger_on_call = 1;
};

int InterruptAfterNChecks(void* userdata) {
  auto* state = static_cast<InterruptState*>(userdata);
  ++state->calls;
  return state->calls >= state->trigger_on_call ? 1 : 0;
}

TEST(DiscreteFrechetDistanceTest, CurrentThreadInterruptAbortsComputation) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Geometry::Ptr g0 = reader.read("LINESTRING (0 0, 1 0, 2 0)");
  Geometry::Ptr g1 = reader.read("LINESTRING (0 1, 1 1, 2 1)");
  ASSERT_NE(g0, nullptr);
  ASSERT_NE(g1, nullptr);

  // Checks 1..4 occur in computeCoordinateDistances; checks 5+ in
  // computeFrechet.
  for (int trigger_at : {1, 2, 5, 6}) {
    SCOPED_TRACE(trigger_at);
    InterruptState state{.calls = 0, .trigger_on_call = trigger_at};
    CurrentThreadInterrupt::ThreadCallback* prev =
        CurrentThreadInterrupt::registerCallback(&InterruptAfterNChecks,
                                                 &state);
    absl::Cleanup restore = [prev]() {
      CurrentThreadInterrupt::registerCallback(prev, nullptr);
    };

    try {
      DiscreteFrechetDistance::distance(*g0, *g1);
      FAIL() << "Expected GEOSException on interrupt";
    } catch (const GEOSException& e) {
      EXPECT_THAT(e.what(), HasSubstr("Interrupted"));
    }
  }
}

TEST(DiscreteFrechetDistanceTest, MatrixStorageImplementations) {
  constexpr double kInf = std::numeric_limits<double>::infinity();

  // RectMatrix via MatrixStorage base pointer (covers ~MatrixStorage).
  {
    std::unique_ptr<DiscreteFrechetDistance::MatrixStorage> rect =
        std::make_unique<DiscreteFrechetDistance::RectMatrix>(3, 4, kInf);
    EXPECT_FALSE(rect->isValueSet(1, 2));
    EXPECT_EQ(rect->get(1, 2), kInf);

    rect->set(1, 2, 7.5);
    EXPECT_TRUE(rect->isValueSet(1, 2));
    EXPECT_DOUBLE_EQ(rect->get(1, 2), 7.5);

    rect->set(1, 2, kInf);
    EXPECT_FALSE(rect->isValueSet(1, 2));
  }

  // HashMapMatrix via MatrixStorage base pointer.
  {
    std::unique_ptr<DiscreteFrechetDistance::MatrixStorage> hash =
        std::make_unique<DiscreteFrechetDistance::HashMapMatrix>(4, 4, -1.0);
    EXPECT_FALSE(hash->isValueSet(0, 0));
    EXPECT_DOUBLE_EQ(hash->get(0, 0), -1.0);

    hash->set(0, 0, 3.25);
    hash->set(2, 3, 9.5);
    EXPECT_TRUE(hash->isValueSet(0, 0));
    EXPECT_TRUE(hash->isValueSet(2, 3));
    EXPECT_DOUBLE_EQ(hash->get(0, 0), 3.25);
    EXPECT_DOUBLE_EQ(hash->get(2, 3), 9.5);

    hash->set(2, 3, 12.0);
    EXPECT_DOUBLE_EQ(hash->get(2, 3), 12.0);
  }

  // CsrMatrix: heuristic constructor, capacity growth, out-of-order insertion,
  // overwrite existing value, and cppBinarySearch edge cases.
  {
    DiscreteFrechetDistance::CsrMatrix default_csr(10, 10, kInf);
    EXPECT_EQ(
        DiscreteFrechetDistance::CsrMatrix::expectedValuesHeuristic(10, 5),
        10u);
    EXPECT_FALSE(default_csr.isValueSet(0, 0));

    DiscreteFrechetDistance::CsrMatrix csr(4, 5, -1.0, 2);
    EXPECT_FALSE(csr.isValueSet(2, 3));
    EXPECT_DOUBLE_EQ(csr.get(2, 3), -1.0);

    // Insert out of row and column order to trigger shifting and
    // ensureCapacity.
    csr.set(3, 2, 32.0);
    csr.set(1, 4, 14.0);
    csr.set(1, 0, 10.0);
    csr.set(1, 2, 12.0);
    csr.set(0, 1, 1.0);
    csr.set(2, 3, 23.0);

    // Overwrite an existing entry in-place.
    csr.set(1, 2, 120.0);

    EXPECT_TRUE(csr.isValueSet(0, 1));
    EXPECT_TRUE(csr.isValueSet(1, 0));
    EXPECT_TRUE(csr.isValueSet(1, 2));
    EXPECT_TRUE(csr.isValueSet(1, 4));
    EXPECT_TRUE(csr.isValueSet(2, 3));
    EXPECT_TRUE(csr.isValueSet(3, 2));
    EXPECT_FALSE(csr.isValueSet(0, 0));
    EXPECT_FALSE(csr.isValueSet(1, 3));

    EXPECT_DOUBLE_EQ(csr.get(0, 1), 1.0);
    EXPECT_DOUBLE_EQ(csr.get(1, 0), 10.0);
    EXPECT_DOUBLE_EQ(csr.get(1, 2), 120.0);
    EXPECT_DOUBLE_EQ(csr.get(1, 4), 14.0);
    EXPECT_DOUBLE_EQ(csr.get(2, 3), 23.0);
    EXPECT_DOUBLE_EQ(csr.get(3, 2), 32.0);
    EXPECT_DOUBLE_EQ(csr.get(1, 1), -1.0);

    const std::vector<std::size_t> sorted{2, 4, 8, 16};
    EXPECT_EQ(csr.cppBinarySearch(sorted, 3, 1, 4),
              (std::pair<bool, std::size_t>{false, 0}));
    EXPECT_EQ(csr.cppBinarySearch(sorted, 0, 5, 4),
              (std::pair<bool, std::size_t>{false, 0}));
    EXPECT_EQ(csr.cppBinarySearch(sorted, 2, 2, 4),
              (std::pair<bool, std::size_t>{false, 2}));
    EXPECT_EQ(csr.cppBinarySearch(sorted, 0, 4, 8),
              (std::pair<bool, std::size_t>{true, 2}));
    EXPECT_EQ(csr.cppBinarySearch(sorted, 0, 4, 5),
              (std::pair<bool, std::size_t>{false, 2}));
    EXPECT_EQ(csr.cppBinarySearch(sorted, 1, 3, 1),
              (std::pair<bool, std::size_t>{false, 1}));
    EXPECT_EQ(csr.cppBinarySearch(sorted, 1, 3, 10),
              (std::pair<bool, std::size_t>{false, 3}));
  }
}

}  // namespace
}  // namespace geos_google
