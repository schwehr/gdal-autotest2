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

// Unit tests for third_party/geos/src/operation/overlayng/OverlayNG.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/overlayng/OverlayNG.cpp

#include "third_party/geos/include/geos/operation/overlayng/OverlayNG.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
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
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/noding/Noder.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/noding/snap/SnappingNoder.h"
#include "third_party/geos/include/geos/noding/snapround/SnapRoundingNoder.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayLabel.h"
#include "third_party/geos/include/geos/util/GEOSException.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/Interrupt.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Dimension;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GEOS_GEOMETRYCOLLECTION;
using geos::geom::GEOS_LINESTRING;
using geos::geom::GEOS_MULTILINESTRING;
using geos::geom::GEOS_MULTIPOINT;
using geos::geom::GEOS_MULTIPOLYGON;
using geos::geom::GEOS_POINT;
using geos::geom::GEOS_POLYGON;
using geos::geom::Location;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::noding::Noder;
using geos::noding::SegmentString;
using geos::noding::snap::SnappingNoder;
using geos::noding::snapround::SnapRoundingNoder;
using geos::operation::overlayng::OverlayLabel;
using geos::operation::overlayng::OverlayNG;
using geos::util::CurrentThreadInterrupt;
using geos::util::GEOSException;
using geos::util::IllegalArgumentException;
using geos::util::TopologyException;
using ::testing::HasSubstr;

constexpr int kOpIntersection = OverlayNG::INTERSECTION;
constexpr int kOpUnion = OverlayNG::UNION;
constexpr int kOpDifference = OverlayNG::DIFFERENCE;
constexpr int kOpSymDifference = OverlayNG::SYMDIFFERENCE;

class RecordingNoder final : public Noder {
 public:
  explicit RecordingNoder(Noder* delegate) : delegate_(delegate) {}

  void computeNodes(std::vector<SegmentString*>* segStrings) override {
    ++compute_calls_;
    delegate_->computeNodes(segStrings);
  }

  std::vector<SegmentString*>* getNodedSubstrings() const override {
    ++substring_calls_;
    return delegate_->getNodedSubstrings();
  }

  int compute_calls() const { return compute_calls_; }
  int substring_calls() const { return substring_calls_; }

 private:
  Noder* delegate_;
  int compute_calls_ = 0;
  mutable int substring_calls_ = 0;
};

struct OpLocationCase {
  int op_code;
  Location loc0;
  Location loc1;
  bool expected;
};

constexpr auto kIsResultOfOpCases = std::to_array<OpLocationCase>({
    {kOpIntersection, Location::INTERIOR, Location::INTERIOR, true},
    {kOpIntersection, Location::BOUNDARY, Location::INTERIOR, true},
    {kOpIntersection, Location::INTERIOR, Location::BOUNDARY, true},
    {kOpIntersection, Location::BOUNDARY, Location::BOUNDARY, true},
    {kOpIntersection, Location::INTERIOR, Location::EXTERIOR, false},
    {kOpIntersection, Location::EXTERIOR, Location::INTERIOR, false},
    {kOpIntersection, Location::INTERIOR, Location::NONE, false},
    {kOpIntersection, Location::NONE, Location::INTERIOR, false},

    {kOpUnion, Location::INTERIOR, Location::INTERIOR, true},
    {kOpUnion, Location::BOUNDARY, Location::EXTERIOR, true},
    {kOpUnion, Location::EXTERIOR, Location::BOUNDARY, true},
    {kOpUnion, Location::INTERIOR, Location::NONE, true},
    {kOpUnion, Location::NONE, Location::INTERIOR, true},
    {kOpUnion, Location::EXTERIOR, Location::EXTERIOR, false},
    {kOpUnion, Location::EXTERIOR, Location::NONE, false},
    {kOpUnion, Location::NONE, Location::NONE, false},

    {kOpDifference, Location::INTERIOR, Location::EXTERIOR, true},
    {kOpDifference, Location::BOUNDARY, Location::EXTERIOR, true},
    {kOpDifference, Location::INTERIOR, Location::NONE, true},
    {kOpDifference, Location::INTERIOR, Location::INTERIOR, false},
    {kOpDifference, Location::INTERIOR, Location::BOUNDARY, false},
    {kOpDifference, Location::EXTERIOR, Location::EXTERIOR, false},
    {kOpDifference, Location::EXTERIOR, Location::INTERIOR, false},
    {kOpDifference, Location::NONE, Location::EXTERIOR, false},

    {kOpSymDifference, Location::INTERIOR, Location::EXTERIOR, true},
    {kOpSymDifference, Location::BOUNDARY, Location::EXTERIOR, true},
    {kOpSymDifference, Location::EXTERIOR, Location::BOUNDARY, true},
    {kOpSymDifference, Location::NONE, Location::INTERIOR, true},
    {kOpSymDifference, Location::INTERIOR, Location::INTERIOR, false},
    {kOpSymDifference, Location::BOUNDARY, Location::BOUNDARY, false},
    {kOpSymDifference, Location::EXTERIOR, Location::EXTERIOR, false},
    {kOpSymDifference, Location::NONE, Location::NONE, false},

    {0, Location::INTERIOR, Location::INTERIOR, false},
    {-1, Location::INTERIOR, Location::EXTERIOR, false},
    {99, Location::BOUNDARY, Location::NONE, false},
});

struct OverlayWktCase {
  std::string_view name;
  int op_code;
  std::string_view wkt0;
  std::string_view wkt1;
  std::string_view expected_wkt;
  double expected_area;
  double expected_length;
  bool expect_exact;
};

constexpr auto kPolygonOverlayCases = std::to_array<OverlayWktCase>({
    {
        .name = "intersection",
        .op_code = kOpIntersection,
        .wkt0 = "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
        .wkt1 = "POLYGON ((1 0, 3 0, 3 2, 1 2, 1 0))",
        .expected_wkt = "POLYGON ((1 0, 2 0, 2 2, 1 2, 1 0))",
        .expected_area = 2.0,
        .expected_length = 6.0,
        .expect_exact = true,
    },
    {
        .name = "union",
        .op_code = kOpUnion,
        .wkt0 = "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
        .wkt1 = "POLYGON ((1 0, 3 0, 3 2, 1 2, 1 0))",
        .expected_wkt = "POLYGON ((0 0, 3 0, 3 2, 0 2, 0 0))",
        .expected_area = 6.0,
        .expected_length = 10.0,
        .expect_exact = false,
    },
    {
        .name = "difference",
        .op_code = kOpDifference,
        .wkt0 = "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
        .wkt1 = "POLYGON ((1 0, 3 0, 3 2, 1 2, 1 0))",
        .expected_wkt = "POLYGON ((0 0, 1 0, 1 2, 0 2, 0 0))",
        .expected_area = 2.0,
        .expected_length = 6.0,
        .expect_exact = true,
    },
    {
        .name = "symdifference",
        .op_code = kOpSymDifference,
        .wkt0 = "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
        .wkt1 = "POLYGON ((1 0, 3 0, 3 2, 1 2, 1 0))",
        .expected_wkt = "MULTIPOLYGON (((0 0, 1 0, 1 2, 0 2, 0 0)), "
                        "((2 0, 3 0, 3 2, 2 2, 2 0)))",
        .expected_area = 4.0,
        .expected_length = 12.0,
        .expect_exact = true,
    },
});

struct InterruptCountState {
  int remaining_before_interrupt = 0;
  int calls = 0;
};

int CountDownInterruptCallback(void* raw_state) {
  auto* state = static_cast<InterruptCountState*>(raw_state);
  ++state->calls;
  if (state->calls >= state->remaining_before_interrupt) {
    return 1;
  }
  return 0;
}

class OverlayNGTest : public ::testing::Test {
 protected:
  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};

  void ExpectNormalizedExact(const Geometry* actual,
                             std::string_view expected_wkt,
                             double tolerance = 1e-9) const {
    ASSERT_NE(actual, nullptr);
    Geometry::Ptr actual_norm = actual->clone();
    Geometry::Ptr expected = reader_.read(std::string(expected_wkt));
    ASSERT_NE(actual_norm, nullptr);
    ASSERT_NE(expected, nullptr);
    actual_norm->normalize();
    expected->normalize();
    EXPECT_TRUE(actual_norm->equalsExact(expected.get(), tolerance))
        << "actual=" << actual_norm->toString()
        << " expected=" << expected->toString();
  }

  void ExpectTopoEqual(const Geometry* actual, std::string_view expected_wkt,
                       double expected_area, double expected_length) const {
    ASSERT_NE(actual, nullptr);
    EXPECT_TRUE(actual->isValid());
    EXPECT_NEAR(actual->getArea(), expected_area, 1e-9);
    EXPECT_NEAR(actual->getLength(), expected_length, 1e-9);
    Geometry::Ptr expected = reader_.read(std::string(expected_wkt));
    ASSERT_NE(expected, nullptr);
    EXPECT_EQ(actual->getDimension(), expected->getDimension());
    EXPECT_TRUE(actual->equals(expected.get()))
        << "actual=" << actual->toString()
        << " expected=" << expected->toString();
  }
};

TEST_F(OverlayNGTest, ConstantsMatchExpectedValues) {
  static_assert(OverlayNG::STRICT_MODE_DEFAULT == false);
  static_assert(OverlayNG::INTERSECTION == 1);
  static_assert(OverlayNG::UNION == 2);
  static_assert(OverlayNG::DIFFERENCE == 3);
  static_assert(OverlayNG::SYMDIFFERENCE == 4);

  EXPECT_FALSE(+OverlayNG::STRICT_MODE_DEFAULT);
  EXPECT_EQ(+OverlayNG::INTERSECTION, 1);
  EXPECT_EQ(+OverlayNG::UNION, 2);
  EXPECT_EQ(+OverlayNG::DIFFERENCE, 3);
  EXPECT_EQ(+OverlayNG::SYMDIFFERENCE, 4);
}

TEST_F(OverlayNGTest, IsResultOfOpCoversAllCodesAndLocations) {
  for (const OpLocationCase& tc : kIsResultOfOpCases) {
    SCOPED_TRACE(tc.op_code);
    EXPECT_EQ(OverlayNG::isResultOfOp(tc.op_code, tc.loc0, tc.loc1),
              tc.expected);
  }
}

TEST_F(OverlayNGTest, IsResultOfOpPointInspectsOverlayLabelLocations) {
  OverlayLabel line_label;
  line_label.initLine(0);
  line_label.setLocationLine(0, Location::INTERIOR);
  line_label.initLine(1);
  line_label.setLocationLine(1, Location::EXTERIOR);

  EXPECT_FALSE(OverlayNG::isResultOfOpPoint(&line_label, kOpIntersection));
  EXPECT_TRUE(OverlayNG::isResultOfOpPoint(&line_label, kOpUnion));
  EXPECT_TRUE(OverlayNG::isResultOfOpPoint(&line_label, kOpDifference));
  EXPECT_TRUE(OverlayNG::isResultOfOpPoint(&line_label, kOpSymDifference));

  OverlayLabel boundary_label(0, Location::INTERIOR, Location::EXTERIOR, false);
  boundary_label.initBoundary(1, Location::INTERIOR, Location::EXTERIOR, false);
  EXPECT_TRUE(OverlayNG::isResultOfOpPoint(&boundary_label, kOpIntersection));
  EXPECT_TRUE(OverlayNG::isResultOfOpPoint(&boundary_label, kOpUnion));
  EXPECT_FALSE(OverlayNG::isResultOfOpPoint(&boundary_label, kOpDifference));
  EXPECT_FALSE(OverlayNG::isResultOfOpPoint(&boundary_label, kOpSymDifference));

  // Non-hole collapse sets EXTERIOR; hole collapse sets INTERIOR.
  OverlayLabel collapse_label;
  collapse_label.initCollapse(0, false);
  collapse_label.setLocationCollapse(0);
  collapse_label.initCollapse(1, true);
  collapse_label.setLocationCollapse(1);
  EXPECT_FALSE(OverlayNG::isResultOfOpPoint(&collapse_label, kOpIntersection));
  EXPECT_TRUE(OverlayNG::isResultOfOpPoint(&collapse_label, kOpUnion));
  EXPECT_FALSE(OverlayNG::isResultOfOpPoint(&collapse_label, kOpDifference));
  EXPECT_TRUE(OverlayNG::isResultOfOpPoint(&collapse_label, kOpSymDifference));

  // Default OverlayLabel has Location::NONE on both sides.
  OverlayLabel not_part_label;
  EXPECT_FALSE(OverlayNG::isResultOfOpPoint(&not_part_label, kOpUnion));
  EXPECT_FALSE(OverlayNG::isResultOfOpPoint(&not_part_label, kOpIntersection));
}

TEST_F(OverlayNGTest, PolygonBinaryOverlayAllOps) {
  for (const OverlayWktCase& tc : kPolygonOverlayCases) {
    SCOPED_TRACE(tc.name);
    Geometry::Ptr g0 = reader_.read(std::string(tc.wkt0));
    Geometry::Ptr g1 = reader_.read(std::string(tc.wkt1));

    Geometry::Ptr result = OverlayNG::overlay(g0.get(), g1.get(), tc.op_code);
    ASSERT_NE(result, nullptr);
    ExpectTopoEqual(result.get(), tc.expected_wkt, tc.expected_area,
                    tc.expected_length);
    if (tc.expect_exact) {
      ExpectNormalizedExact(result.get(), tc.expected_wkt);
    }
  }
}

TEST_F(OverlayNGTest, LineStringBinaryOverlayAllOps) {
  Geometry::Ptr l0 = reader_.read("LINESTRING (0 0, 3 0)");
  Geometry::Ptr l1 = reader_.read("LINESTRING (1 0, 4 0)");

  Geometry::Ptr inter = OverlayNG::overlay(l0.get(), l1.get(), kOpIntersection);
  EXPECT_EQ(inter->getGeometryTypeId(), GEOS_LINESTRING);
  ExpectNormalizedExact(inter.get(), "LINESTRING (1 0, 3 0)");

  Geometry::Ptr uni = OverlayNG::overlay(l0.get(), l1.get(), kOpUnion);
  EXPECT_EQ(uni->getDimension(), Dimension::L);
  EXPECT_NEAR(uni->getLength(), 4.0, 1e-9);
  Geometry::Ptr expected_union = reader_.read("LINESTRING (0 0, 4 0)");
  EXPECT_TRUE(uni->equals(expected_union.get()));

  Geometry::Ptr diff = OverlayNG::overlay(l0.get(), l1.get(), kOpDifference);
  EXPECT_EQ(diff->getGeometryTypeId(), GEOS_LINESTRING);
  ExpectNormalizedExact(diff.get(), "LINESTRING (0 0, 1 0)");

  Geometry::Ptr symdiff =
      OverlayNG::overlay(l0.get(), l1.get(), kOpSymDifference);
  EXPECT_EQ(symdiff->getGeometryTypeId(), GEOS_MULTILINESTRING);
  ExpectNormalizedExact(symdiff.get(),
                        "MULTILINESTRING ((0 0, 1 0), (3 0, 4 0))");

  // Crossing lines produce a Point intersection via IntersectionPointBuilder.
  Geometry::Ptr h = reader_.read("LINESTRING (0 1, 2 1)");
  Geometry::Ptr v = reader_.read("LINESTRING (1 0, 1 2)");
  Geometry::Ptr cross = OverlayNG::overlay(h.get(), v.get(), kOpIntersection);
  EXPECT_EQ(cross->getGeometryTypeId(), GEOS_POINT);
  ExpectNormalizedExact(cross.get(), "POINT (1 1)");
}

TEST_F(OverlayNGTest, LineAndPolygonMixedOverlayAllOps) {
  Geometry::Ptr poly = reader_.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  Geometry::Ptr line = reader_.read("LINESTRING (-1 1, 3 1)");

  Geometry::Ptr inter =
      OverlayNG::overlay(poly.get(), line.get(), kOpIntersection);
  EXPECT_EQ(inter->getGeometryTypeId(), GEOS_LINESTRING);
  ExpectNormalizedExact(inter.get(), "LINESTRING (0 1, 2 1)");

  Geometry::Ptr line_minus_poly =
      OverlayNG::overlay(line.get(), poly.get(), kOpDifference);
  EXPECT_EQ(line_minus_poly->getGeometryTypeId(), GEOS_MULTILINESTRING);
  ExpectNormalizedExact(line_minus_poly.get(),
                        "MULTILINESTRING ((-1 1, 0 1), (2 1, 3 1))");

  Geometry::Ptr poly_minus_line =
      OverlayNG::overlay(poly.get(), line.get(), kOpDifference);
  EXPECT_EQ(poly_minus_line->getGeometryTypeId(), GEOS_POLYGON);
  ExpectTopoEqual(poly_minus_line.get(), "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
                  4.0, 8.0);

  // Both UNION and SYMDIFFERENCE keep exterior line segments with the polygon.
  for (int op : {kOpUnion, kOpSymDifference}) {
    SCOPED_TRACE(op);
    Geometry::Ptr res = OverlayNG::overlay(poly.get(), line.get(), op);
    EXPECT_EQ(res->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION);
    EXPECT_EQ(res->getNumGeometries(), 3u);
    EXPECT_NEAR(res->getArea(), 4.0, 1e-9);
    EXPECT_NEAR(res->getLength(), 10.0, 1e-9);
  }
}

TEST_F(OverlayNGTest, PointPointOverlayRoutesToOverlayPoints) {
  Geometry::Ptr mp0 = reader_.read("MULTIPOINT ((0 0), (1 1))");
  Geometry::Ptr mp1 = reader_.read("MULTIPOINT ((1 1), (2 2))");

  ExpectNormalizedExact(
      OverlayNG::overlay(mp0.get(), mp1.get(), kOpIntersection).get(),
      "POINT (1 1)");
  ExpectNormalizedExact(
      OverlayNG::overlay(mp0.get(), mp1.get(), kOpUnion).get(),
      "MULTIPOINT ((0 0), (1 1), (2 2))");
  ExpectNormalizedExact(
      OverlayNG::overlay(mp0.get(), mp1.get(), kOpDifference).get(),
      "POINT (0 0)");
  ExpectNormalizedExact(
      OverlayNG::overlay(mp0.get(), mp1.get(), kOpSymDifference).get(),
      "MULTIPOINT ((0 0), (2 2))");

  // Disjoint points with same coordinates under fixed precision merge.
  Geometry::Ptr p0 = reader_.read("POINT (0.1 0.1)");
  Geometry::Ptr p1 = reader_.read("POINT (0.2 0.2)");
  PrecisionModel fixed_pm(1.0);
  ExpectNormalizedExact(
      OverlayNG::overlay(p0.get(), p1.get(), kOpIntersection, &fixed_pm).get(),
      "POINT (0 0)");

  // Disjoint points under floating precision yield empty point.
  Geometry::Ptr empty_pt =
      OverlayNG::overlay(p0.get(), p1.get(), kOpIntersection);
  EXPECT_TRUE(empty_pt->isEmpty());
  EXPECT_EQ(empty_pt->getGeometryTypeId(), GEOS_POINT);
}

TEST_F(OverlayNGTest, PointNonPointOverlayRoutesToOverlayMixedPoints) {
  Geometry::Ptr mp = reader_.read("MULTIPOINT ((0.5 0.5), (1 1), (5 5))");
  Geometry::Ptr poly = reader_.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");

  ExpectNormalizedExact(
      OverlayNG::overlay(mp.get(), poly.get(), kOpIntersection).get(),
      "MULTIPOINT ((0.5 0.5), (1 1))");
  ExpectNormalizedExact(
      OverlayNG::overlay(poly.get(), mp.get(), kOpIntersection).get(),
      "MULTIPOINT ((0.5 0.5), (1 1))");
  ExpectNormalizedExact(
      OverlayNG::overlay(mp.get(), poly.get(), kOpDifference).get(),
      "POINT (5 5)");
  ExpectNormalizedExact(
      OverlayNG::overlay(poly.get(), mp.get(), kOpDifference).get(),
      "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");

  for (int op : {kOpUnion, kOpSymDifference}) {
    SCOPED_TRACE(op);
    Geometry::Ptr gc = OverlayNG::overlay(mp.get(), poly.get(), op);
    EXPECT_EQ(gc->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION);
    EXPECT_EQ(gc->getNumGeometries(), 2u);
    EXPECT_NEAR(gc->getArea(), 4.0, 1e-9);
  }

  Geometry::Ptr line = reader_.read("LINESTRING (0 0, 2 0)");
  Geometry::Ptr pts_on_off = reader_.read("MULTIPOINT ((1 0), (1 2))");
  ExpectNormalizedExact(
      OverlayNG::overlay(pts_on_off.get(), line.get(), kOpIntersection).get(),
      "POINT (1 0)");
  ExpectNormalizedExact(
      OverlayNG::overlay(pts_on_off.get(), line.get(), kOpDifference).get(),
      "POINT (1 2)");
}

TEST_F(OverlayNGTest, ConstructorsAndFactoryPrecisionOverloads) {
  Geometry::Ptr g0 = reader_.read(
      "POLYGON ((0.14 0.14, 2.14 0.14, 2.14 2.14, 0.14 2.14, 0.14 0.14))");
  Geometry::Ptr g1 = reader_.read(
      "POLYGON ((1.14 0.14, 3.14 0.14, 3.14 2.14, 1.14 2.14, 1.14 0.14))");

  // 4-arg GeometryFactory constructor uses custom factory & its precision
  // model.
  PrecisionModel fixed_pm(10.0);
  GeometryFactory::Ptr custom_factory =
      GeometryFactory::create(&fixed_pm, 4326);
  {
    OverlayNG ov(g0.get(), g1.get(), custom_factory.get(), kOpIntersection);
    Geometry::Ptr res = ov.getResult();
    EXPECT_EQ(res->getFactory(), custom_factory.get());
    EXPECT_EQ(res->getSRID(), 4326);
    ExpectNormalizedExact(
        res.get(), "POLYGON ((1.1 0.1, 2.1 0.1, 2.1 2.1, 1.1 2.1, 1.1 0.1))");
  }

  // PrecisionModel constructor keeps g0's factory while using supplied PM.
  PrecisionModel coarse_pm(1.0);
  {
    OverlayNG ov(g0.get(), g1.get(), &coarse_pm, kOpIntersection);
    Geometry::Ptr res = ov.getResult();
    EXPECT_EQ(res->getFactory(), g0->getFactory());
    ExpectNormalizedExact(res.get(), "POLYGON ((1 0, 2 0, 2 2, 1 2, 1 0))");
  }

  // 3-arg constructor & 3-arg static overlay use input factory precision model.
  GeometryFactory::Ptr fixed_factory = GeometryFactory::create(&coarse_pm);
  WKTReader fixed_reader(*fixed_factory);
  Geometry::Ptr fg0 = fixed_reader.read(
      "POLYGON ((0.1 0.1, 2.1 0.1, 2.1 2.1, 0.1 2.1, 0.1 0.1))");
  Geometry::Ptr fg1 = fixed_reader.read(
      "POLYGON ((1.1 0.1, 3.1 0.1, 3.1 2.1, 1.1 2.1, 1.1 0.1))");
  {
    OverlayNG ov(fg0.get(), fg1.get(), kOpIntersection);
    Geometry::Ptr res = ov.getResult();
    EXPECT_EQ(res->getFactory(), fixed_factory.get());
    ExpectNormalizedExact(res.get(), "POLYGON ((1 0, 2 0, 2 2, 1 2, 1 0))");
  }

  // Single-geometry constructor & OverlayNG::geomunion(geom, pm).
  {
    OverlayNG ov(g0.get(), &coarse_pm);
    ExpectNormalizedExact(ov.getResult().get(),
                          "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  }
  {
    Geometry::Ptr adjacent_mp = reader_.read(
        "MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)), "
        "((1 0, 2 0, 2 1, 1 1, 1 0)))");
    Geometry::Ptr merged = OverlayNG::geomunion(adjacent_mp.get(), &coarse_pm);
    EXPECT_EQ(merged->getGeometryTypeId(), GEOS_POLYGON);
    ExpectTopoEqual(merged.get(), "POLYGON ((0 0, 2 0, 2 1, 0 1, 0 0))", 2.0,
                    6.0);
  }
}

TEST_F(OverlayNGTest, StaticOverlayOverloadsWithCustomNoder) {
  Geometry::Ptr g0 = reader_.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  Geometry::Ptr g1 =
      reader_.read("POLYGON ((1.1 0, 3.1 0, 3.1 2, 1.1 2, 1.1 0))");

  // 5-arg overlay(g0, g1, op, pm, noder).
  PrecisionModel coarse_pm(1.0);
  SnapRoundingNoder sr_noder(&coarse_pm);
  RecordingNoder rec_noder(&sr_noder);
  Geometry::Ptr res_pm_noder =
      OverlayNG::overlay(g0.get(), g1.get(), kOpUnion, &coarse_pm, &rec_noder);
  EXPECT_EQ(rec_noder.compute_calls(), 1);
  EXPECT_EQ(rec_noder.substring_calls(), 1);
  EXPECT_EQ(res_pm_noder->getGeometryTypeId(), GEOS_POLYGON);
  ExpectTopoEqual(res_pm_noder.get(), "POLYGON ((0 0, 3 0, 3 2, 0 2, 0 0))",
                  6.0, 10.0);

  // 4-arg overlay(g0, g1, op, noder) passes pm = nullptr and snaps nearby
  // edges.
  Geometry::Ptr almost_aligned =
      reader_.read("POLYGON ((0.02 0, 3 0, 3 2, 0.02 2, 0.02 0))");
  SnappingNoder snap_noder(0.05);
  RecordingNoder rec_snap(&snap_noder);
  Geometry::Ptr res_noder_only = OverlayNG::overlay(
      g0.get(), almost_aligned.get(), kOpIntersection, &rec_snap);
  EXPECT_EQ(rec_snap.compute_calls(), 1);
  EXPECT_EQ(rec_snap.substring_calls(), 1);
  EXPECT_NEAR(res_noder_only->getArea(), 4.0, 1e-9);
  ExpectNormalizedExact(res_noder_only.get(),
                        "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");

  // 3-arg geomunion(geom, pm, noder) enables strict mode and calls custom
  // noder.
  Geometry::Ptr spike =
      reader_.read("POLYGON ((0 0, 2 0, 2 2, 1.1 2, 1 5, 0.9 2, 0 2, 0 0))");
  SnapRoundingNoder union_sr_noder(&coarse_pm);
  RecordingNoder rec_union(&union_sr_noder);
  Geometry::Ptr res_strict_union =
      OverlayNG::geomunion(spike.get(), &coarse_pm, &rec_union);
  EXPECT_EQ(rec_union.compute_calls(), 1);
  EXPECT_EQ(rec_union.substring_calls(), 1);
  EXPECT_EQ(res_strict_union->getGeometryTypeId(), GEOS_POLYGON);
  EXPECT_NEAR(res_strict_union->getArea(), 4.0, 1e-9);
}

TEST_F(OverlayNGTest, StrictModeControlsMixedIntersectionAndCollapseLines) {
  // C-shaped polygon overlapping B in [2, 4] x [0, 1] and touching along
  // segment (2 2, 4 2).
  Geometry::Ptr c_poly =
      reader_.read("POLYGON ((0 0, 4 0, 4 1, 1 1, 1 2, 4 2, 4 3, 0 3, 0 0))");
  Geometry::Ptr touch_line_poly =
      reader_.read("POLYGON ((2 -1, 4 -1, 4 2, 2 2, 2 -1))");

  {
    OverlayNG ov(c_poly.get(), touch_line_poly.get(), kOpIntersection);
    ov.setStrictMode(false);
    Geometry::Ptr non_strict = ov.getResult();
    EXPECT_EQ(non_strict->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION);
    EXPECT_EQ(non_strict->getNumGeometries(), 2u);
    EXPECT_NEAR(non_strict->getArea(), 2.0, 1e-9);
  }
  {
    OverlayNG ov(c_poly.get(), touch_line_poly.get(), kOpIntersection);
    ov.setStrictMode(true);
    Geometry::Ptr strict = ov.getResult();
    EXPECT_EQ(strict->getGeometryTypeId(), GEOS_POLYGON);
    EXPECT_NEAR(strict->getArea(), 2.0, 1e-9);
  }

  // C-shaped polygon overlapping B in [2, 4] x [0, 1] and touching at point (3,
  // 2).
  Geometry::Ptr touch_pt_poly =
      reader_.read("POLYGON ((2 -1, 4 -1, 4 1, 3 2, 2 1, 2 -1))");
  {
    OverlayNG ov(c_poly.get(), touch_pt_poly.get(), kOpIntersection);
    ov.setStrictMode(false);
    Geometry::Ptr non_strict = ov.getResult();
    EXPECT_EQ(non_strict->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION);
    EXPECT_EQ(non_strict->getNumGeometries(), 2u);
    EXPECT_NEAR(non_strict->getArea(), 2.0, 1e-9);
  }
  {
    OverlayNG ov(c_poly.get(), touch_pt_poly.get(), kOpIntersection);
    ov.setStrictMode(true);
    Geometry::Ptr strict = ov.getResult();
    EXPECT_EQ(strict->getGeometryTypeId(), GEOS_POLYGON);
    EXPECT_NEAR(strict->getArea(), 2.0, 1e-9);
  }

  // UNION and SYMDIFFERENCE still keep linear components in strict mode when
  // inputs have mixed dimensions.
  Geometry::Ptr sq = reader_.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  Geometry::Ptr line = reader_.read("LINESTRING (-1 1, 3 1)");
  for (int op : {kOpUnion, kOpSymDifference}) {
    SCOPED_TRACE(op);
    OverlayNG ov(sq.get(), line.get(), op);
    ov.setStrictMode(true);
    Geometry::Ptr res = ov.getResult();
    EXPECT_EQ(res->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION);
    EXPECT_EQ(res->getNumGeometries(), 3u);
  }

  // Collapse lines from fixed precision reduction appear only in non-strict
  // mode.
  Geometry::Ptr spike =
      reader_.read("POLYGON ((0 0, 2 0, 2 2, 1.1 2, 1 5, 0.9 2, 0 2, 0 0))");
  PrecisionModel coarse_pm(1.0);
  {
    OverlayNG ov(spike.get(), &coarse_pm);
    ov.setStrictMode(false);
    Geometry::Ptr non_strict = ov.getResult();
    EXPECT_EQ(non_strict->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION);
    EXPECT_EQ(non_strict->getNumGeometries(), 2u);
    EXPECT_NEAR(non_strict->getArea(), 4.0, 1e-9);
    EXPECT_NEAR(non_strict->getLength(), 11.0, 1e-9);
  }
  {
    OverlayNG ov(spike.get(), &coarse_pm);
    ov.setStrictMode(true);
    Geometry::Ptr strict = ov.getResult();
    EXPECT_EQ(strict->getGeometryTypeId(), GEOS_POLYGON);
    EXPECT_NEAR(strict->getArea(), 4.0, 1e-9);
    EXPECT_NEAR(strict->getLength(), 8.0, 1e-9);
  }

  // Line-line intersection with both overlapping segments and a crossing point
  // keeps the point only in non-strict mode (exercises
  // !resultLineList.empty()).
  Geometry::Ptr ml0 = reader_.read("MULTILINESTRING ((0 0, 2 0), (1 1, 1 3))");
  Geometry::Ptr ml1 = reader_.read("MULTILINESTRING ((0 0, 2 0), (0 2, 2 2))");
  {
    OverlayNG ov(ml0.get(), ml1.get(), kOpIntersection);
    ov.setStrictMode(false);
    Geometry::Ptr non_strict = ov.getResult();
    EXPECT_EQ(non_strict->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION);
    EXPECT_EQ(non_strict->getNumGeometries(), 2u);
    EXPECT_NEAR(non_strict->getLength(), 2.0, 1e-9);
  }
  {
    OverlayNG ov(ml0.get(), ml1.get(), kOpIntersection);
    ov.setStrictMode(true);
    Geometry::Ptr strict = ov.getResult();
    EXPECT_EQ(strict->getGeometryTypeId(), GEOS_LINESTRING);
    ExpectNormalizedExact(strict.get(), "LINESTRING (0 0, 2 0)");
  }

  // DIFFERENCE with area result and boundary collapse excludes collapse line
  // only when strict mode is enabled.
  Geometry::Ptr corner = reader_.read("POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  {
    OverlayNG ov(spike.get(), corner.get(), &coarse_pm, kOpDifference);
    ov.setStrictMode(false);
    Geometry::Ptr non_strict = ov.getResult();
    EXPECT_EQ(non_strict->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION);
    EXPECT_EQ(non_strict->getNumGeometries(), 2u);
    EXPECT_NEAR(non_strict->getArea(), 3.0, 1e-9);
    EXPECT_NEAR(non_strict->getLength(), 11.0, 1e-9);
  }
  {
    OverlayNG ov(spike.get(), corner.get(), &coarse_pm, kOpDifference);
    ov.setStrictMode(true);
    Geometry::Ptr strict = ov.getResult();
    EXPECT_EQ(strict->getGeometryTypeId(), GEOS_POLYGON);
    EXPECT_NEAR(strict->getArea(), 3.0, 1e-9);
    EXPECT_NEAR(strict->getLength(), 8.0, 1e-9);
  }
}

TEST_F(OverlayNGTest, AreaResultOnlyOmitsLineAndPointResults) {
  Geometry::Ptr c_poly =
      reader_.read("POLYGON ((0 0, 4 0, 4 1, 1 1, 1 2, 4 2, 4 3, 0 3, 0 0))");
  Geometry::Ptr touch_line_poly =
      reader_.read("POLYGON ((2 -1, 4 -1, 4 2, 2 2, 2 -1))");
  {
    OverlayNG ov(c_poly.get(), touch_line_poly.get(), kOpIntersection);
    ov.setStrictMode(false);
    ov.setAreaResultOnly(true);
    Geometry::Ptr res = ov.getResult();
    EXPECT_EQ(res->getGeometryTypeId(), GEOS_POLYGON);
    EXPECT_NEAR(res->getArea(), 2.0, 1e-9);
  }

  // Adjacent polygons sharing only an edge produce an empty polygon.
  Geometry::Ptr left_sq = reader_.read("POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  Geometry::Ptr right_sq = reader_.read("POLYGON ((1 0, 2 0, 2 1, 1 1, 1 0))");
  {
    OverlayNG ov(left_sq.get(), right_sq.get(), kOpIntersection);
    ov.setAreaResultOnly(true);
    Geometry::Ptr res = ov.getResult();
    EXPECT_TRUE(res->isEmpty());
    EXPECT_EQ(res->getGeometryTypeId(), GEOS_POLYGON);
  }

  // Crossing lines produce an empty line result when area_result_only is set.
  Geometry::Ptr l0 = reader_.read("LINESTRING (0 1, 2 1)");
  Geometry::Ptr l1 = reader_.read("LINESTRING (1 0, 1 2)");
  {
    OverlayNG ov(l0.get(), l1.get(), kOpIntersection);
    ov.setAreaResultOnly(true);
    Geometry::Ptr res = ov.getResult();
    EXPECT_TRUE(res->isEmpty());
    EXPECT_EQ(res->getGeometryTypeId(), GEOS_LINESTRING);
  }

  // UNION of polygon and crossing line omits linear component when
  // area_result_only is set.
  Geometry::Ptr sq = reader_.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  Geometry::Ptr crossing_line = reader_.read("LINESTRING (-1 1, 3 1)");
  {
    OverlayNG ov(sq.get(), crossing_line.get(), kOpUnion);
    ov.setAreaResultOnly(true);
    Geometry::Ptr res = ov.getResult();
    EXPECT_EQ(res->getGeometryTypeId(), GEOS_POLYGON);
    ExpectTopoEqual(res.get(), "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))", 4.0, 8.0);
  }
}

TEST_F(OverlayNGTest, SetOptimizedControlsClippingEnvelope) {
  // Concave polygon whose left arm [0, 4] x [4, 6] has no segments intersecting
  // the clipped overlap region [7.8, 11] x [3.8, 6.2].
  Geometry::Ptr large = reader_.read(
      "POLYGON ((0 4, 4 4, 4 0, 10 0, 10 10, 4 10, 4 6, 0 6, 0 4))");
  Geometry::Ptr small = reader_.read("POLYGON ((8 4, 12 4, 12 6, 8 6, 8 4))");

  // Unoptimized intersection and difference still match optimized outputs.
  {
    OverlayNG ov(large.get(), small.get(), kOpIntersection);
    ov.setOptimized(false);
    ExpectNormalizedExact(ov.getResult().get(),
                          "POLYGON ((8 4, 10 4, 10 6, 8 6, 8 4))");
  }
  {
    OverlayNG ov(large.get(), small.get(), kOpIntersection);
    ov.setOptimized(true);
    ExpectNormalizedExact(ov.getResult().get(),
                          "POLYGON ((8 4, 10 4, 10 6, 8 6, 8 4))");
  }
  {
    OverlayNG ov(large.get(), small.get(), kOpDifference);
    ov.setOptimized(false);
    Geometry::Ptr diff = ov.getResult();
    EXPECT_TRUE(diff->isValid());
    EXPECT_NEAR(diff->getArea(), 64.0, 1e-9);
  }

  // Inspecting noded edges shows clipping removes edges far outside overlap.
  OverlayNG ov_opt(large.get(), small.get(), kOpIntersection);
  ov_opt.setOptimized(true);
  ov_opt.setOutputNodedEdges(true);
  Geometry::Ptr opt_edges = ov_opt.getResult();
  EXPECT_GT(opt_edges->getEnvelopeInternal()->getMinX(), 5.0);

  OverlayNG ov_unopt(large.get(), small.get(), kOpIntersection);
  ov_unopt.setOptimized(false);
  ov_unopt.setOutputNodedEdges(true);
  Geometry::Ptr unopt_edges = ov_unopt.getResult();
  EXPECT_DOUBLE_EQ(unopt_edges->getEnvelopeInternal()->getMinX(), 0.0);
  EXPECT_DOUBLE_EQ(unopt_edges->getEnvelopeInternal()->getMaxX(), 12.0);
}

TEST_F(OverlayNGTest, OutputEdgeModesReturnExpectedLinework) {
  Geometry::Ptr g0 = reader_.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  Geometry::Ptr g1 = reader_.read("POLYGON ((1 0, 3 0, 3 2, 1 2, 1 0))");

  // EdgeNodingBuilder splits g0 and g1 only at intersection nodes and ring
  // endpoints, yielding 7 merged edges (total length 14).
  // toLines(..., isOutputEdges=true) emits both half-edges of each merged
  // edge (14 half-edges, total length 2 * 14 = 28).
  {
    OverlayNG ov(g0.get(), g1.get(), kOpIntersection);
    ov.setOutputNodedEdges(true);
    Geometry::Ptr noded_edges = ov.getResult();
    EXPECT_EQ(noded_edges->getDimension(), Dimension::L);
    EXPECT_EQ(noded_edges->getNumGeometries(), 14u);
    EXPECT_NEAR(noded_edges->getLength(), 28.0, 1e-9);
  }

  // setOutputNodedEdges(false) keeps isOutputEdges = true while still running
  // labelGraph (unlike setOutputNodedEdges(true), which returns before line
  // 259's interrupt check).
  {
    OverlayNG ov(g0.get(), g1.get(), kOpIntersection);
    ov.setOutputNodedEdges(false);
    Geometry::Ptr labelled_edges = ov.getResult();
    EXPECT_EQ(labelled_edges->getDimension(), Dimension::L);
    EXPECT_EQ(labelled_edges->getNumGeometries(), 14u);
    EXPECT_NEAR(labelled_edges->getLength(), 28.0, 1e-9);
  }
  {
    InterruptCountState state{.remaining_before_interrupt = 3};
    CurrentThreadInterrupt::ThreadCallback* prev =
        CurrentThreadInterrupt::registerCallback(&CountDownInterruptCallback,
                                                 &state);
    absl::Cleanup restore_cb(
        [prev]() { CurrentThreadInterrupt::registerCallback(prev, nullptr); });

    OverlayNG ov_early(g0.get(), g1.get(), kOpIntersection);
    ov_early.setOutputNodedEdges(true);
    EXPECT_NEAR(ov_early.getResult()->getLength(), 28.0, 1e-9);
    EXPECT_EQ(state.calls, 2);

    state.calls = 0;
    OverlayNG ov_labelled(g0.get(), g1.get(), kOpIntersection);
    ov_labelled.setOutputNodedEdges(false);
    EXPECT_THROW(ov_labelled.getResult(), GEOSException);
    EXPECT_EQ(state.calls, 3);
  }

  // setOutputEdges(true) outputs all labelled half-edges.
  {
    OverlayNG ov(g0.get(), g1.get(), kOpUnion);
    ov.setOutputEdges(true);
    Geometry::Ptr all_edges = ov.getResult();
    EXPECT_EQ(all_edges->getDimension(), Dimension::L);
    EXPECT_EQ(all_edges->getNumGeometries(), 14u);
    EXPECT_NEAR(all_edges->getLength(), 28.0, 1e-9);
  }

  // setOutputResultEdges(true) outputs only edges bounding the result area.
  {
    OverlayNG ov(g0.get(), g1.get(), kOpIntersection);
    ov.setOutputResultEdges(true);
    Geometry::Ptr res_edges = ov.getResult();
    EXPECT_EQ(res_edges->getDimension(), Dimension::L);
    EXPECT_EQ(res_edges->getNumGeometries(), 4u);
    EXPECT_NEAR(res_edges->getLength(), 6.0, 1e-9);
  }
  {
    OverlayNG ov(g0.get(), g1.get(), kOpUnion);
    ov.setOutputResultEdges(true);
    Geometry::Ptr res_edges = ov.getResult();
    EXPECT_EQ(res_edges->getDimension(), Dimension::L);
    EXPECT_EQ(res_edges->getNumGeometries(), 5u);
    EXPECT_NEAR(res_edges->getLength(), 10.0, 1e-9);
  }
  {
    OverlayNG ov(g0.get(), g1.get(), kOpDifference);
    ov.setOutputResultEdges(true);
    Geometry::Ptr res_edges = ov.getResult();
    EXPECT_EQ(res_edges->getDimension(), Dimension::L);
    EXPECT_EQ(res_edges->getNumGeometries(), 3u);
    EXPECT_NEAR(res_edges->getLength(), 6.0, 1e-9);
  }
  {
    OverlayNG ov(g0.get(), g1.get(), kOpSymDifference);
    ov.setOutputResultEdges(true);
    Geometry::Ptr res_edges = ov.getResult();
    EXPECT_EQ(res_edges->getDimension(), Dimension::L);
    EXPECT_EQ(res_edges->getNumGeometries(), 5u);
    EXPECT_NEAR(res_edges->getLength(), 12.0, 1e-9);
  }
  {
    OverlayNG ov(g0.get(), g1.get(), kOpIntersection);
    ov.setOutputEdges(true);
    ov.setOutputResultEdges(true);
    Geometry::Ptr both_flags = ov.getResult();
    EXPECT_EQ(both_flags->getDimension(), Dimension::L);
    EXPECT_EQ(both_flags->getNumGeometries(), 14u);
    EXPECT_NEAR(both_flags->getLength(), 28.0, 1e-9);
  }
}

TEST_F(OverlayNGTest, ElevationModelPopulatesZInBinaryAndUnaryOverlays) {
  // Binary overlay of 3D polygon and 2D polygon fills missing Z values.
  Geometry::Ptr poly_z =
      reader_.read("POLYGON Z ((0 0 10, 2 0 10, 2 2 10, 0 2 10, 0 0 10))");
  Geometry::Ptr poly_2d = reader_.read("POLYGON ((1 0, 3 0, 3 2, 1 2, 1 0))");
  Geometry::Ptr union_z =
      OverlayNG::overlay(poly_z.get(), poly_2d.get(), kOpUnion);
  ASSERT_NE(union_z, nullptr);
  EXPECT_TRUE(union_z->hasZ());
  std::unique_ptr<CoordinateSequence> union_coords = union_z->getCoordinates();
  for (std::size_t i = 0; i < union_coords->size(); ++i) {
    EXPECT_DOUBLE_EQ(union_coords->getAt<Coordinate>(i).z, 10.0);
  }

  // Binary line intersection interpolates / populates finite Z.
  Geometry::Ptr l0_z = reader_.read("LINESTRING Z (0 1 10, 2 1 10)");
  Geometry::Ptr l1_z = reader_.read("LINESTRING Z (1 0 20, 1 2 20)");
  Geometry::Ptr cross_z =
      OverlayNG::overlay(l0_z.get(), l1_z.get(), kOpIntersection);
  EXPECT_TRUE(cross_z->hasZ());
  EXPECT_DOUBLE_EQ(cross_z->getCoordinates()->getAt<Coordinate>(0).z, 15.0);

  // Unary geomunion (ig1 == nullptr) elevates via single-geometry
  // ElevationModel.
  Geometry::Ptr cross_ml_z =
      reader_.read("MULTILINESTRING Z ((0 1 12, 2 1 12), (1 0 12, 1 2 12))");
  PrecisionModel floating_pm;
  Geometry::Ptr noded_z = OverlayNG::geomunion(cross_ml_z.get(), &floating_pm);
  EXPECT_TRUE(noded_z->hasZ());
  std::unique_ptr<CoordinateSequence> noded_coords = noded_z->getCoordinates();
  for (std::size_t i = 0; i < noded_coords->size(); ++i) {
    EXPECT_DOUBLE_EQ(noded_coords->getAt<Coordinate>(i).z, 12.0);
  }

  // 2D-only inputs remain 2D.
  Geometry::Ptr poly_2d_a = reader_.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  Geometry::Ptr res_2d =
      OverlayNG::overlay(poly_2d_a.get(), poly_2d.get(), kOpUnion);
  EXPECT_FALSE(res_2d->hasZ());
}

TEST_F(OverlayNGTest, EarlyEmptyResultForEmptyAndDisjointInputs) {
  Geometry::Ptr poly = reader_.read("POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  Geometry::Ptr far_poly = reader_.read("POLYGON ((5 5, 6 5, 6 6, 5 6, 5 5))");
  Geometry::Ptr line = reader_.read("LINESTRING (0 0, 1 1)");
  Geometry::Ptr empty_poly = reader_.read("POLYGON EMPTY");
  Geometry::Ptr empty_line = reader_.read("LINESTRING EMPTY");
  Geometry::Ptr empty_gc = reader_.read("GEOMETRYCOLLECTION EMPTY");

  // Disjoint envelopes under INTERSECTION (floating and fixed PM).
  Geometry::Ptr inter_disjoint =
      OverlayNG::overlay(poly.get(), far_poly.get(), kOpIntersection);
  EXPECT_TRUE(inter_disjoint->isEmpty());
  EXPECT_EQ(inter_disjoint->getGeometryTypeId(), GEOS_POLYGON);

  PrecisionModel fixed_pm(1.0);
  Geometry::Ptr inter_fixed_disjoint = OverlayNG::overlay(
      poly.get(), far_poly.get(), kOpIntersection, &fixed_pm);
  EXPECT_TRUE(inter_fixed_disjoint->isEmpty());
  EXPECT_EQ(inter_fixed_disjoint->getGeometryTypeId(), GEOS_POLYGON);

  // INTERSECTION with empty or nullptr second geometry.
  Geometry::Ptr inter_empty =
      OverlayNG::overlay(poly.get(), empty_line.get(), kOpIntersection);
  EXPECT_TRUE(inter_empty->isEmpty());
  EXPECT_EQ(inter_empty->getGeometryTypeId(), GEOS_LINESTRING);

  OverlayNG inter_null_rhs(poly.get(), nullptr, kOpIntersection);
  Geometry::Ptr inter_null = inter_null_rhs.getResult();
  EXPECT_TRUE(inter_null->isEmpty());
  EXPECT_EQ(inter_null->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION);

  // DIFFERENCE with empty first geometry.
  Geometry::Ptr diff_empty_lhs =
      OverlayNG::overlay(empty_line.get(), poly.get(), kOpDifference);
  EXPECT_TRUE(diff_empty_lhs->isEmpty());
  EXPECT_EQ(diff_empty_lhs->getGeometryTypeId(), GEOS_LINESTRING);

  // UNION and SYMDIFFERENCE with both inputs empty.
  Geometry::Ptr union_both_empty =
      OverlayNG::overlay(empty_poly.get(), empty_line.get(), kOpUnion);
  EXPECT_TRUE(union_both_empty->isEmpty());
  EXPECT_EQ(union_both_empty->getGeometryTypeId(), GEOS_POLYGON);

  Geometry::Ptr sym_empty_gc =
      OverlayNG::overlay(empty_gc.get(), empty_gc.get(), kOpSymDifference);
  EXPECT_TRUE(sym_empty_gc->isEmpty());
  EXPECT_EQ(sym_empty_gc->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION);

  // Unary geomunion of empty geometry.
  Geometry::Ptr unary_empty = OverlayNG::geomunion(empty_poly.get(), &fixed_pm);
  EXPECT_TRUE(unary_empty->isEmpty());
  EXPECT_EQ(unary_empty->getGeometryTypeId(), GEOS_POLYGON);

  // Empty result preserves Z only when both operands have Z.
  Geometry::Ptr poly_z0 =
      reader_.read("POLYGON Z ((0 0 1, 1 0 1, 1 1 1, 0 1 1, 0 0 1))");
  Geometry::Ptr poly_z1 =
      reader_.read("POLYGON Z ((5 5 2, 6 5 2, 6 6 2, 5 6 2, 5 5 2))");
  Geometry::Ptr empty_z =
      OverlayNG::overlay(poly_z0.get(), poly_z1.get(), kOpIntersection);
  EXPECT_TRUE(empty_z->isEmpty());
  EXPECT_TRUE(empty_z->hasZ());

  Geometry::Ptr empty_mixed_z =
      OverlayNG::overlay(poly_z0.get(), far_poly.get(), kOpIntersection);
  EXPECT_TRUE(empty_mixed_z->isEmpty());
  EXPECT_FALSE(empty_mixed_z->hasZ());

  // Empty intersection of disjoint lines preserves line dimension.
  Geometry::Ptr line_far = reader_.read("LINESTRING (5 5, 6 6)");
  Geometry::Ptr line_inter_empty =
      OverlayNG::overlay(line.get(), line_far.get(), kOpIntersection);
  EXPECT_TRUE(line_inter_empty->isEmpty());
  EXPECT_EQ(line_inter_empty->getGeometryTypeId(), GEOS_LINESTRING);

  // One-sided empty operands for UNION, DIFFERENCE, SYMDIFFERENCE keep the
  // non-empty operand.
  ExpectNormalizedExact(
      OverlayNG::overlay(empty_poly.get(), poly.get(), kOpUnion).get(),
      "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  ExpectNormalizedExact(
      OverlayNG::overlay(poly.get(), empty_poly.get(), kOpDifference).get(),
      "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  ExpectNormalizedExact(
      OverlayNG::overlay(empty_poly.get(), poly.get(), kOpSymDifference).get(),
      "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  Geometry::Ptr empty_pt = reader_.read("POINT EMPTY");
  ExpectNormalizedExact(
      OverlayNG::overlay(empty_pt.get(), poly.get(), kOpUnion).get(),
      "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
}

TEST_F(OverlayNGTest, ExtractResultReturnsEmptyAndHandlesCollapsedInputs) {
  // Overlapping envelopes with disjoint geometries reach extractResult().
  Geometry::Ptr l_shape =
      reader_.read("POLYGON ((0 0, 4 0, 4 1, 1 1, 1 4, 0 4, 0 0))");
  Geometry::Ptr inside_concavity =
      reader_.read("POLYGON ((2 2, 3 2, 3 3, 2 3, 2 2))");
  Geometry::Ptr disjoint_overlap_env = OverlayNG::overlay(
      l_shape.get(), inside_concavity.get(), kOpIntersection);
  EXPECT_TRUE(disjoint_overlap_env->isEmpty());
  EXPECT_EQ(disjoint_overlap_env->getGeometryTypeId(), GEOS_POLYGON);

  // Identical geometries yield empty DIFFERENCE and SYMDIFFERENCE.
  EXPECT_TRUE(OverlayNG::overlay(l_shape.get(), l_shape.get(), kOpDifference)
                  ->isEmpty());
  EXPECT_TRUE(OverlayNG::overlay(l_shape.get(), l_shape.get(), kOpSymDifference)
                  ->isEmpty());

  // Complete collapse under fixed precision produces empty geometry.
  PrecisionModel coarse_pm(1.0);
  Geometry::Ptr tiny =
      reader_.read("POLYGON ((0 0, 0.2 0, 0.2 0.2, 0 0.2, 0 0))");
  Geometry::Ptr collapsed_unary = OverlayNG::geomunion(tiny.get(), &coarse_pm);
  EXPECT_TRUE(collapsed_unary->isEmpty());
  EXPECT_EQ(collapsed_unary->getGeometryTypeId(), GEOS_POLYGON);

  // Partial collapse marks inputGeom.setCollapsed(0/1) so surviving line/poly
  // locates against collapsed polygon as EXTERIOR. SnapRoundingNoder splits
  // big_line at tiny's (0, 0) hot pixel, so compare topologically.
  Geometry::Ptr big_line = reader_.read("LINESTRING (-1 0, 2 0)");
  Geometry::Ptr diff_collapsed =
      OverlayNG::overlay(big_line.get(), tiny.get(), kOpDifference, &coarse_pm);
  ExpectTopoEqual(diff_collapsed.get(), "LINESTRING (-1 0, 2 0)", 0.0, 3.0);
  ExpectNormalizedExact(OverlayNG::overlay(tiny.get(), inside_concavity.get(),
                                           kOpUnion, &coarse_pm)
                            .get(),
                        "POLYGON ((2 2, 3 2, 3 3, 2 3, 2 2))");
  ExpectNormalizedExact(OverlayNG::overlay(inside_concavity.get(), tiny.get(),
                                           kOpUnion, &coarse_pm)
                            .get(),
                        "POLYGON ((2 2, 3 2, 3 3, 2 3, 2 2))");
}

TEST_F(OverlayNGTest, MixedDimensionCollectionThrowsIllegalArgument) {
  Geometry::Ptr poly = reader_.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  Geometry::Ptr mixed_poly_pt = reader_.read(
      "GEOMETRYCOLLECTION (POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)), POINT (0.5 "
      "0.5))");
  EXPECT_THROW(
      OverlayNG::overlay(mixed_poly_pt.get(), poly.get(), kOpIntersection),
      IllegalArgumentException);

  Geometry::Ptr line = reader_.read("LINESTRING (0 0, 2 0)");
  Geometry::Ptr mixed_line_pt =
      reader_.read("GEOMETRYCOLLECTION (LINESTRING (0 0, 1 0), POINT (2 2))");
  EXPECT_THROW(OverlayNG::overlay(mixed_line_pt.get(), line.get(), kOpUnion),
               IllegalArgumentException);
}

TEST_F(OverlayNGTest, FloatingInconsistentAreaThrowsTopologyException) {
  // Using a coarse snap-rounding noder with floating PM (nullptr) causes the
  // rounded intersection area (4.0) to exceed geom0's area (1.96) beyond the
  // area consistency heuristic tolerance.
  Geometry::Ptr inner =
      reader_.read("POLYGON ((0.3 0.3, 1.7 0.3, 1.7 1.7, 0.3 1.7, 0.3 0.3))");
  Geometry::Ptr outer = reader_.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  PrecisionModel coarse_pm(1.0);
  SnapRoundingNoder sr_noder(&coarse_pm);

  EXPECT_THROW(
      OverlayNG::overlay(inner.get(), outer.get(), kOpIntersection, &sr_noder),
      TopologyException);
}

TEST_F(OverlayNGTest, SelfIntersectingInputThrowsTopologyException) {
  Geometry::Ptr bowtie = reader_.read("POLYGON ((0 0, 2 2, 2 0, 0 2, 0 0))");
  Geometry::Ptr box = reader_.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  EXPECT_THROW(OverlayNG::overlay(bowtie.get(), box.get(), kOpIntersection),
               TopologyException);
}

TEST_F(OverlayNGTest, CurrentThreadInterruptAbortsComputeEdgeOverlay) {
  Geometry::Ptr g0 = reader_.read("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  Geometry::Ptr g1 = reader_.read("POLYGON ((1 0, 3 0, 3 2, 1 2, 1 0))");

  for (int trigger_at : {1, 2, 3, 4, 5}) {
    SCOPED_TRACE(trigger_at);
    InterruptCountState state{.remaining_before_interrupt = trigger_at};
    CurrentThreadInterrupt::ThreadCallback* prev =
        CurrentThreadInterrupt::registerCallback(&CountDownInterruptCallback,
                                                 &state);
    absl::Cleanup restore_cb(
        [prev]() { CurrentThreadInterrupt::registerCallback(prev, nullptr); });

    EXPECT_THAT(
        [&]() {
          try {
            OverlayNG::overlay(g0.get(), g1.get(), kOpIntersection);
            return std::string("no_throw");
          } catch (const GEOSException& e) {
            return std::string(e.what());
          }
        }(),
        HasSubstr("Interrupted"));
  }

  // A high threshold completes without interrupting and hits all 5 checks.
  InterruptCountState state{.remaining_before_interrupt = 100};
  CurrentThreadInterrupt::ThreadCallback* prev =
      CurrentThreadInterrupt::registerCallback(&CountDownInterruptCallback,
                                               &state);
  absl::Cleanup restore_cb(
      [prev]() { CurrentThreadInterrupt::registerCallback(prev, nullptr); });
  ExpectNormalizedExact(
      OverlayNG::overlay(g0.get(), g1.get(), kOpIntersection).get(),
      "POLYGON ((1 0, 2 0, 2 2, 1 2, 1 0))");
  EXPECT_GE(state.calls, 5);
}

}  // namespace
}  // namespace geos_google
