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

// Unit tests for third_party/geos/src/operation/overlayng/OverlayNGRobust.cpp.

#include "third_party/geos/include/geos/operation/overlayng/OverlayNGRobust.h"

#include <array>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/MultiPolygon.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayNG.h"
#include "third_party/geos/include/geos/operation/union/UnionStrategy.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/TopologyException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GeometryTypeId;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::operation::geounion::UnionStrategy;
using geos::operation::overlayng::OverlayNG;
using geos::operation::overlayng::OverlayNGRobust;
using geos::util::IllegalArgumentException;
using geos::util::TopologyException;
using geos::util::UnsupportedOperationException;

struct BinaryPolygonCase {
  std::string_view name;
  int op_code;
  std::string_view expected_wkt;
  double expected_area;
};

constexpr auto kPolygonOverlayCases = std::to_array<BinaryPolygonCase>({
    {
        .name = "intersection",
        .op_code = OverlayNG::INTERSECTION,
        .expected_wkt = "POLYGON ((1 1, 2 1, 2 2, 1 2, 1 1))",
        .expected_area = 1.0,
    },
    {
        .name = "union",
        .op_code = OverlayNG::UNION,
        .expected_wkt =
            "POLYGON ((0 0, 2 0, 2 1, 3 1, 3 3, 1 3, 1 2, 0 2, 0 0))",
        .expected_area = 7.0,
    },
    {
        .name = "difference",
        .op_code = OverlayNG::DIFFERENCE,
        .expected_wkt = "POLYGON ((0 0, 2 0, 2 1, 1 1, 1 2, 0 2, 0 0))",
        .expected_area = 3.0,
    },
    {
        .name = "symdifference",
        .op_code = OverlayNG::SYMDIFFERENCE,
        .expected_wkt = "MULTIPOLYGON ("
                        "((0 0, 2 0, 2 1, 1 1, 1 2, 0 2, 0 0)), "
                        "((1 2, 2 2, 2 1, 3 1, 3 3, 1 3, 1 2)))",
        .expected_area = 6.0,
    },
});

std::unique_ptr<CoordinateSequence> MakeXYSeq(
    std::initializer_list<CoordinateXY> coords) {
  return std::make_unique<CoordinateSequence>(coords);
}

std::unique_ptr<Polygon> MakeXYPolygon(
    const GeometryFactory* factory,
    std::initializer_list<CoordinateXY> coords) {
  return factory->createPolygon(factory->createLinearRing(MakeXYSeq(coords)));
}

TEST(OverlayNGRobustTest, ComputesAllBinaryOperationsOnPolygons) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr g0 = reader.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  Geometry::Ptr g1 = reader.read("POLYGON ((1 1, 3 1, 3 3, 1 3, 1 1))");

  auto check_result = [&](const Geometry* result, const BinaryPolygonCase& tc) {
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->isValid());
    EXPECT_NEAR(result->getArea(), tc.expected_area, 1e-9);
    Geometry::Ptr expected = reader.read(std::string(tc.expected_wkt));
    Geometry::Ptr norm_result = result->clone();
    norm_result->normalize();
    expected->normalize();
    EXPECT_TRUE(norm_result->equalsExact(expected.get(), 1e-9));
  };

  for (const BinaryPolygonCase& tc : kPolygonOverlayCases) {
    SCOPED_TRACE(tc.name);
    Geometry::Ptr overlay_res =
        OverlayNGRobust::Overlay(g0.get(), g1.get(), tc.op_code);
    check_result(overlay_res.get(), tc);
  }

  check_result(OverlayNGRobust::Intersection(g0.get(), g1.get()).get(),
               kPolygonOverlayCases[0]);
  check_result(OverlayNGRobust::Union(g0.get(), g1.get()).get(),
               kPolygonOverlayCases[1]);
  check_result(OverlayNGRobust::Difference(g0.get(), g1.get()).get(),
               kPolygonOverlayCases[2]);
  check_result(OverlayNGRobust::SymDifference(g0.get(), g1.get()).get(),
               kPolygonOverlayCases[3]);
}

TEST(OverlayNGRobustTest, HandlesLinesPointsMixedAndEmptyInputs) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr line0 = reader.read("LINESTRING (0 0, 4 4)");
  Geometry::Ptr line1 = reader.read("LINESTRING (0 4, 4 0)");
  Geometry::Ptr line_inter =
      OverlayNGRobust::Intersection(line0.get(), line1.get());
  ASSERT_NE(line_inter, nullptr);
  Geometry::Ptr expected_pt = reader.read("POINT (2 2)");
  EXPECT_TRUE(line_inter->equalsExact(expected_pt.get(), 1e-9));

  Geometry::Ptr line_union = OverlayNGRobust::Union(line0.get(), line1.get());
  ASSERT_NE(line_union, nullptr);
  EXPECT_EQ(line_union->getNumGeometries(), 4u);
  EXPECT_NEAR(line_union->getLength(), 8.0 * std::sqrt(2.0), 1e-9);

  Geometry::Ptr poly = reader.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  Geometry::Ptr poly_line_diff =
      OverlayNGRobust::Difference(poly.get(), line0.get());
  ASSERT_NE(poly_line_diff, nullptr);
  EXPECT_NEAR(poly_line_diff->getArea(), 4.0, 1e-9);

  Geometry::Ptr poly_empty = reader.read("POLYGON EMPTY");
  Geometry::Ptr union_with_empty =
      OverlayNGRobust::Union(poly.get(), poly_empty.get());
  ASSERT_NE(union_with_empty, nullptr);
  EXPECT_NEAR(union_with_empty->getArea(), 4.0, 1e-9);

  Geometry::Ptr inter_with_empty =
      OverlayNGRobust::Intersection(poly.get(), poly_empty.get());
  ASSERT_NE(inter_with_empty, nullptr);
  EXPECT_TRUE(inter_with_empty->isEmpty());
}

TEST(OverlayNGRobustTest, UnaryUnionAndSRUnionStrategy) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr mp = reader.read(
      "MULTIPOLYGON ("
      "((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "((1 0, 3 0, 3 2, 1 2, 1 0)), "
      "((2 1, 4 1, 4 3, 2 3, 2 1)))");
  Geometry::Ptr mp_union = OverlayNGRobust::Union(mp.get());
  ASSERT_NE(mp_union, nullptr);
  EXPECT_TRUE(mp_union->isValid());
  EXPECT_NEAR(mp_union->getArea(), 9.0, 1e-9);

  Geometry::Ptr mls =
      reader.read("MULTILINESTRING ((0 0, 2 0), (1 -1, 1 1), (0 0, 1 0))");
  Geometry::Ptr mls_union = OverlayNGRobust::Union(mls.get());
  ASSERT_NE(mls_union, nullptr);
  EXPECT_NEAR(mls_union->getLength(), 4.0, 1e-9);

  Geometry::Ptr gc = reader.read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "LINESTRING (0.5 0.5, 2 0.5), "
      "POINT (0.5 0.5), "
      "POINT (5 5))");
  Geometry::Ptr gc_union = OverlayNGRobust::Union(gc.get());
  ASSERT_NE(gc_union, nullptr);
  EXPECT_TRUE(gc_union->isValid());
  EXPECT_NEAR(gc_union->getArea(), 1.0, 1e-9);
  EXPECT_NEAR(gc_union->getLength(), 5.0, 1e-9);

  Geometry::Ptr empty_gc = reader.read("GEOMETRYCOLLECTION EMPTY");
  Geometry::Ptr empty_res = OverlayNGRobust::Union(empty_gc.get());
  ASSERT_NE(empty_res, nullptr);
  EXPECT_TRUE(empty_res->isEmpty());

  OverlayNGRobust::SRUnionStrategy strategy;
  UnionStrategy* strategy_base = &strategy;
  EXPECT_TRUE(strategy_base->isFloatingPrecision());

  Geometry::Ptr sq0 = reader.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  Geometry::Ptr sq1 = reader.read("POLYGON ((1 0, 3 0, 3 2, 1 2, 1 0))");
  Geometry::Ptr strat_res = strategy_base->Union(sq0.get(), sq1.get());
  ASSERT_NE(strat_res, nullptr);
  EXPECT_TRUE(strat_res->isValid());
  EXPECT_NEAR(strat_res->getArea(), 6.0, 1e-9);
}

TEST(OverlayNGRobustTest, RejectsCurvedGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr arc = reader.read("CIRCULARSTRING (0 0, 1 1, 2 0)");
  Geometry::Ptr compound =
      reader.read("COMPOUNDCURVE (CIRCULARSTRING (0 0, 1 1, 2 0))");
  Geometry::Ptr curve_poly =
      reader.read("CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0))");
  Geometry::Ptr line = reader.read("LINESTRING (0 0, 2 0)");

  EXPECT_THROW(
      static_cast<void>(OverlayNGRobust::Intersection(arc.get(), line.get())),
      UnsupportedOperationException);
  EXPECT_THROW(
      static_cast<void>(OverlayNGRobust::Union(line.get(), compound.get())),
      UnsupportedOperationException);
  EXPECT_THROW(static_cast<void>(
                   OverlayNGRobust::Difference(curve_poly.get(), line.get())),
               UnsupportedOperationException);
}

TEST(OverlayNGRobustTest, UsesFixedPrecisionBranchWhenInputIsFixedPrecision) {
  PrecisionModel pm_fixed(1.0);
  GeometryFactory::Ptr fixed_factory = GeometryFactory::create(&pm_fixed);
  WKTReader fixed_reader(fixed_factory.get());

  GeometryFactory::Ptr float_factory = GeometryFactory::create();
  WKTReader float_reader(float_factory.get());

  Geometry::Ptr g0 = fixed_reader.read(
      "POLYGON ((0.1 0.1, 2.1 0.1, 2.1 2.1, 0.1 2.1, 0.1 0.1))");
  Geometry::Ptr g1 = float_reader.read(
      "POLYGON ((1.2 1.2, 3.2 1.2, 3.2 3.2, 1.2 3.2, 1.2 1.2))");

  Geometry::Ptr inter = OverlayNGRobust::Intersection(g0.get(), g1.get());
  ASSERT_NE(inter, nullptr);
  EXPECT_TRUE(inter->isValid());
  EXPECT_NEAR(inter->getArea(), 1.0, 1e-9);
  Geometry::Ptr expected =
      fixed_reader.read("POLYGON ((1 1, 2 1, 2 2, 1 2, 1 1))");
  inter->normalize();
  expected->normalize();
  EXPECT_TRUE(inter->equalsExact(expected.get(), 1e-9));
}

TEST(OverlayNGRobustTest, ComputesSnapToleranceFromInputMagnitudes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  EXPECT_DOUBLE_EQ(OverlayNGRobust::snapTolerance(nullptr, nullptr), 0.0);

  Geometry::Ptr empty_poly = reader.read("POLYGON EMPTY");
  Geometry::Ptr empty_pt = reader.read("POINT EMPTY");
  EXPECT_DOUBLE_EQ(
      OverlayNGRobust::snapTolerance(empty_poly.get(), empty_pt.get()), 0.0);

  Geometry::Ptr box10 = reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  EXPECT_DOUBLE_EQ(OverlayNGRobust::snapTolerance(box10.get(), nullptr),
                   10.0 / 1e12);
  EXPECT_DOUBLE_EQ(OverlayNGRobust::snapTolerance(nullptr, box10.get()),
                   10.0 / 1e12);

  Geometry::Ptr neg_pt = reader.read("POINT (-500 -1000)");
  Geometry::Ptr pos_line = reader.read("LINESTRING (0 0, 300 40)");
  EXPECT_DOUBLE_EQ(OverlayNGRobust::snapTolerance(neg_pt.get(), pos_line.get()),
                   1000.0 / 1e12);
  EXPECT_DOUBLE_EQ(OverlayNGRobust::snapTolerance(pos_line.get(), neg_pt.get()),
                   1000.0 / 1e12);
}

TEST(OverlayNGRobustTest, PropagatesNonTopologyRuntimeException) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr mixed_gc = reader.read(
      "GEOMETRYCOLLECTION ("
      "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "LINESTRING (2 0, 3 0))");
  Geometry::Ptr box = reader.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");

  EXPECT_THROW(
      static_cast<void>(OverlayNGRobust::Union(mixed_gc.get(), box.get())),
      IllegalArgumentException);
}

TEST(OverlayNGRobustTest, FallsBackToOverlaySnappingWhenFloatingNodingFails) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // S1: (8192, 0) -> (8193, 2^20) intersects horizontal S2 at y2 = 1 + 7*2^-21.
  // Exact x* rounds up to x_p = 8192 + 2^-20 + 2^-38, placing split segment
  // S1b to the right of true S1 near y3 = 1 + 15*2^-22 so S3 crosses S1b
  // without intersecting unsplit S1, triggering ValidatingNoder failure in
  // floating mode while SnappingNoder resolves it on the first snap try.
  const double x_p = 8192.0 + std::ldexp(1.0, -20) + std::ldexp(1.0, -38);
  const double y_2 = 1.0 + 7.0 * std::ldexp(1.0, -21);
  const double y_3 = 1.0 + 15.0 * std::ldexp(1.0, -22);

  std::unique_ptr<LineString> g0 = factory->createLineString(
      MakeXYSeq({CoordinateXY(8192.0, 0.0), CoordinateXY(8193.0, 1048576.0)}));

  std::vector<std::unique_ptr<LineString>> lines;
  lines.push_back(factory->createLineString(
      MakeXYSeq({CoordinateXY(8191.0, y_2), CoordinateXY(8194.0, y_2)})));
  lines.push_back(factory->createLineString(
      MakeXYSeq({CoordinateXY(x_p, y_3), CoordinateXY(8194.0, y_3)})));
  auto g1 = factory->createMultiLineString(std::move(lines));

  EXPECT_THROW(static_cast<void>(
                   OverlayNG::overlay(g0.get(), g1.get(), OverlayNG::UNION)),
               TopologyException);

  Geometry::Ptr robust = OverlayNGRobust::Union(g0.get(), g1.get());
  ASSERT_NE(robust, nullptr);
  EXPECT_FALSE(robust->isEmpty());
  EXPECT_EQ(robust->getDimension(), 1);
  EXPECT_TRUE(robust->isValid());

  Geometry::Ptr direct_snap =
      OverlayNGRobust::overlaySnapTries(g0.get(), g1.get(), OverlayNG::UNION);
  ASSERT_NE(direct_snap, nullptr);
  EXPECT_FALSE(direct_snap->isEmpty());
  EXPECT_TRUE(direct_snap->equals(robust.get()));
}

TEST(OverlayNGRobustTest, FallsBackToOverlaySnapBothOnAreaInconsistency) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Duplicate identical holes give input area 100 - 36 - 36 = 28 while
  // merged overlay result has 1 hole (area 64). Floating mode and single-pass
  // overlaySnapping fail the result-area heuristic check, whereas snapSelf
  // repairs geom0 first so overlaySnapBoth succeeds.
  auto shell = factory->createLinearRing(MakeXYSeq({
      CoordinateXY(0.0, 0.0),
      CoordinateXY(10.0, 0.0),
      CoordinateXY(10.0, 10.0),
      CoordinateXY(0.0, 10.0),
      CoordinateXY(0.0, 0.0),
  }));
  auto make_hole = [&]() {
    return factory->createLinearRing(MakeXYSeq({
        CoordinateXY(2.0, 2.0),
        CoordinateXY(2.0, 8.0),
        CoordinateXY(8.0, 8.0),
        CoordinateXY(8.0, 2.0),
        CoordinateXY(2.0, 2.0),
    }));
  };
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(make_hole());
  holes.push_back(make_hole());
  std::unique_ptr<Polygon> g0 =
      factory->createPolygon(std::move(shell), std::move(holes));

  std::unique_ptr<Polygon> g1 =
      MakeXYPolygon(factory.get(), {
                                       CoordinateXY(0.0, 0.0),
                                       CoordinateXY(10.0, 0.0),
                                       CoordinateXY(10.0, 10.0),
                                       CoordinateXY(0.0, 10.0),
                                       CoordinateXY(0.0, 0.0),
                                   });

  EXPECT_THROW(static_cast<void>(OverlayNG::overlay(g0.get(), g1.get(),
                                                    OverlayNG::INTERSECTION)),
               TopologyException);

  Geometry::Ptr result = OverlayNGRobust::Intersection(g0.get(), g1.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_EQ(result->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
  EXPECT_NEAR(result->getArea(), 64.0, 1e-9);
}

TEST(OverlayNGRobustTest, IncreasesSnapToleranceAcrossRetriesToCollapseSpike) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Unit square with an external 5e-11 twisted bowtie spike inside [-1, 2]^2.
  // The outer triangle of the bowtie is an uncontained clockwise loop that
  // throws in floating mode and on the first two snap tries (snapTol = 2e-12
  // and 2e-11), then collapses cleanly on the third try (snapTol = 2e-10).
  const double d = 5.0e-11;
  std::unique_ptr<Polygon> spiked =
      MakeXYPolygon(factory.get(), {
                                       CoordinateXY(0.0, 0.0),
                                       CoordinateXY(1.0, 0.0),
                                       CoordinateXY(1.0, 0.5),
                                       CoordinateXY(1.2, 0.5 + d),
                                       CoordinateXY(1.2, 0.5),
                                       CoordinateXY(1.0, 0.5 + d),
                                       CoordinateXY(1.0, 1.0),
                                       CoordinateXY(0.0, 1.0),
                                       CoordinateXY(0.0, 0.0),
                                   });
  std::unique_ptr<Polygon> enclosing_box =
      MakeXYPolygon(factory.get(), {
                                       CoordinateXY(-1.0, -1.0),
                                       CoordinateXY(2.0, -1.0),
                                       CoordinateXY(2.0, 2.0),
                                       CoordinateXY(-1.0, 2.0),
                                       CoordinateXY(-1.0, -1.0),
                                   });

  EXPECT_THROW(static_cast<void>(OverlayNG::overlay(
                   spiked.get(), enclosing_box.get(), OverlayNG::INTERSECTION)),
               TopologyException);

  Geometry::Ptr result =
      OverlayNGRobust::Intersection(spiked.get(), enclosing_box.get());
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isValid());
  EXPECT_NEAR(result->getArea(), 1.0, 1e-9);
}

TEST(OverlayNGRobustTest, FallsBackToOverlaySRWhenAllSnapTriesFail) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  // Two identical [0, 9]^2 squares give g0 area 162, while g1 covers
  // [0, 9 - 5e-12] x [0, 9] (area 81 - 4.5e-11). Floating mode and all 5
  // snap tries (snapTol 9e-12..9e-8 > 5e-12) collapse g0 - g1 to empty and
  // fail the floating area-consistency heuristic even after snapSelf(g0),
  // whereas overlaySR uses fixed safeScale (grid 1e-13) and returns the strip.
  auto make_square9 = [&]() {
    return MakeXYPolygon(factory.get(), {
                                            CoordinateXY(0.0, 0.0),
                                            CoordinateXY(9.0, 0.0),
                                            CoordinateXY(9.0, 9.0),
                                            CoordinateXY(0.0, 9.0),
                                            CoordinateXY(0.0, 0.0),
                                        });
  };
  std::vector<std::unique_ptr<Polygon>> polys;
  polys.push_back(make_square9());
  polys.push_back(make_square9());
  auto g0 = factory->createMultiPolygon(std::move(polys));

  const double right_x = 9.0 - 5.0e-12;
  std::unique_ptr<Polygon> g1 =
      MakeXYPolygon(factory.get(), {
                                       CoordinateXY(0.0, 0.0),
                                       CoordinateXY(right_x, 0.0),
                                       CoordinateXY(right_x, 9.0),
                                       CoordinateXY(0.0, 9.0),
                                       CoordinateXY(0.0, 0.0),
                                   });

  EXPECT_THROW(static_cast<void>(OverlayNG::overlay(g0.get(), g1.get(),
                                                    OverlayNG::DIFFERENCE)),
               TopologyException);
  EXPECT_EQ(OverlayNGRobust::overlaySnapTries(g0.get(), g1.get(),
                                              OverlayNG::DIFFERENCE),
            nullptr);

  Geometry::Ptr result = OverlayNGRobust::Difference(g0.get(), g1.get());
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(result->isEmpty());
  EXPECT_TRUE(result->isValid());
  EXPECT_NEAR(result->getArea(), 4.5e-11, 1e-13);
}

TEST(OverlayNGRobustTest, ThrowsOriginalExceptionWhenAllStrategiesFail) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  // Large asymmetric bowtie unioned with a disjoint box keeps the bowtie
  // unmasked, so floating overlay, all 5 snap tries, and overlaySR all fail
  // and OverlayNGRobust::Overlay rethrows exOriginal (sliced to runtime_error).
  Geometry::Ptr bowtie = reader.read("POLYGON ((0 0, 4 2, 4 0, 0 1, 0 0))");
  Geometry::Ptr disjoint_box =
      reader.read("POLYGON ((10 0, 14 0, 14 2, 10 2, 10 0))");

  EXPECT_EQ(OverlayNGRobust::overlaySnapTries(bowtie.get(), disjoint_box.get(),
                                              OverlayNG::UNION),
            nullptr);

  try {
    static_cast<void>(OverlayNGRobust::Union(bowtie.get(), disjoint_box.get()));
    FAIL() << "Expected std::runtime_error when all overlay strategies fail";
  } catch (const std::runtime_error& ex) {
    const std::string_view msg(ex.what());
    EXPECT_NE(msg.find("TopologyException"), std::string_view::npos);
    EXPECT_NE(msg.find("side location conflict"), std::string_view::npos);
  }
}

}  // namespace
}  // namespace geos_google
