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

// Unit tests for third_party/geos/src/operation/BoundaryOp.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - Exercises BoundaryOp constructors, static helpers, BoundaryNodeRule
//   variants, Z/M coordinate preservation, and curved/collection exceptions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/BoundaryOp.cpp

#include "third_party/geos/include/geos/operation/BoundaryOp.h"

#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/BoundaryNodeRule.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::algorithm::BoundaryNodeRule;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GeometryTypeId;
using geos::geom::LineString;
using geos::geom::MultiLineString;
using geos::geom::Point;
using geos::io::WKTReader;
using geos::operation::BoundaryOp;
using geos::util::IllegalArgumentException;
using geos::util::UnsupportedOperationException;

using RuleFn = const BoundaryNodeRule& (*)();

struct BoundaryCase {
  std::string_view name;
  std::string_view input_wkt;
  RuleFn rule_fn;
  std::string_view expected_wkt;
  GeometryTypeId expected_type;
};

struct HasBoundaryCase {
  std::string_view name;
  std::string_view input_wkt;
  RuleFn rule_fn;
  bool expected;
};

void CheckBoundaryResult(const Geometry& actual, const Geometry& expected,
                         GeometryTypeId expected_type) {
  EXPECT_EQ(actual.getGeometryTypeId(), expected_type);
  EXPECT_EQ(actual.isEmpty(), expected.isEmpty());
  EXPECT_TRUE(actual.equalsIdentical(&expected))
      << "actual: " << actual.toText() << "\nexpected: " << expected.toText();
}

void RunBoundaryCases(const auto& cases) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const auto& tc : cases) {
    SCOPED_TRACE(std::string(tc.name));
    std::unique_ptr<Geometry> geom = reader.read(std::string(tc.input_wkt));
    std::unique_ptr<Geometry> expected =
        reader.read(std::string(tc.expected_wkt));
    ASSERT_NE(geom, nullptr);
    ASSERT_NE(expected, nullptr);

    const BoundaryNodeRule& rule = tc.rule_fn();

    std::unique_ptr<Geometry> static_res = BoundaryOp::getBoundary(*geom, rule);
    ASSERT_NE(static_res, nullptr);
    CheckBoundaryResult(*static_res, *expected, tc.expected_type);

    BoundaryOp op(*geom, rule);
    std::unique_ptr<Geometry> instance_res = op.getBoundary();
    ASSERT_NE(instance_res, nullptr);
    CheckBoundaryResult(*instance_res, *expected, tc.expected_type);
  }
}

void ExpectPointValues(const Geometry& geom, double x, double y, bool has_z,
                       double z, bool has_m, double m) {
  ASSERT_EQ(geom.getGeometryTypeId(), geos::geom::GEOS_POINT);
  const auto* pt = dynamic_cast<const Point*>(&geom);
  ASSERT_NE(pt, nullptr);
  EXPECT_DOUBLE_EQ(pt->getX(), x);
  EXPECT_DOUBLE_EQ(pt->getY(), y);
  EXPECT_EQ(pt->hasZ(), has_z);
  EXPECT_EQ(pt->hasM(), has_m);
  if (has_z) {
    EXPECT_DOUBLE_EQ(pt->getZ(), z);
  } else {
    EXPECT_TRUE(std::isnan(pt->getZ()));
  }
  if (has_m) {
    EXPECT_DOUBLE_EQ(pt->getM(), m);
  } else {
    EXPECT_TRUE(std::isnan(pt->getM()));
  }
}

TEST(BoundaryOpTest, DefaultRuleMatchesMod2AndOgcSfs) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  constexpr auto kDefaultInputs = std::to_array<std::string_view>({
      "LINESTRING EMPTY",
      "LINESTRING (0 0, 5 0)",
      "LINESTRING (0 0, 1 0, 1 1, 0 0)",
      "MULTILINESTRING ((0 0, 1 0), (1 0, 2 0))",
      "MULTILINESTRING ((0 0, 1 0), (1 0, 2 0), (1 0, 1 1))",
      "POINT (3 4)",
      "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
  });

  for (std::string_view wkt : kDefaultInputs) {
    SCOPED_TRACE(std::string(wkt));
    std::unique_ptr<Geometry> geom = reader.read(std::string(wkt));
    ASSERT_NE(geom, nullptr);

    std::unique_ptr<Geometry> default_static = BoundaryOp::getBoundary(*geom);
    BoundaryOp default_op(*geom);
    std::unique_ptr<Geometry> default_inst = default_op.getBoundary();
    std::unique_ptr<Geometry> mod2_res =
        BoundaryOp::getBoundary(*geom, BoundaryNodeRule::getBoundaryRuleMod2());
    std::unique_ptr<Geometry> ogc_res =
        BoundaryOp::getBoundary(*geom, BoundaryNodeRule::getBoundaryOGCSFS());

    ASSERT_NE(default_static, nullptr);
    ASSERT_NE(default_inst, nullptr);
    ASSERT_NE(mod2_res, nullptr);
    ASSERT_NE(ogc_res, nullptr);

    EXPECT_TRUE(default_static->equalsIdentical(default_inst.get()));
    EXPECT_TRUE(default_static->equalsIdentical(mod2_res.get()));
    EXPECT_TRUE(default_static->equalsIdentical(ogc_res.get()));
  }
}

TEST(BoundaryOpTest, LineStringCasesAcrossRules) {
  constexpr auto kCases = std::to_array<BoundaryCase>({
      {
          .name = "empty_linestring_mod2",
          .input_wkt = "LINESTRING EMPTY",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTIPOINT EMPTY",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "empty_linestring_endpoint",
          .input_wkt = "LINESTRING EMPTY",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected_wkt = "MULTIPOINT EMPTY",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "empty_linestring_z",
          .input_wkt = "LINESTRING Z EMPTY",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTIPOINT EMPTY",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "empty_linestring_m",
          .input_wkt = "LINESTRING M EMPTY",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTIPOINT EMPTY",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "empty_linestring_zm",
          .input_wkt = "LINESTRING ZM EMPTY",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTIPOINT EMPTY",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "open_linestring_mod2",
          .input_wkt = "LINESTRING (10 0, 5 5, 0 0)",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTIPOINT ((10 0), (0 0))",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "open_linestring_endpoint",
          .input_wkt = "LINESTRING (0 0, 5 0)",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected_wkt = "MULTIPOINT ((0 0), (5 0))",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "open_linestring_multivalent",
          .input_wkt = "LINESTRING (0 0, 5 0)",
          .rule_fn = &BoundaryNodeRule::getBoundaryMultivalentEndPoint,
          .expected_wkt = "MULTIPOINT ((0 0), (5 0))",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "open_linestring_monovalent",
          .input_wkt = "LINESTRING (0 0, 5 0)",
          .rule_fn = &BoundaryNodeRule::getBoundaryMonovalentEndPoint,
          .expected_wkt = "MULTIPOINT ((0 0), (5 0))",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "open_linestring_z",
          .input_wkt = "LINESTRING Z (0 0 1, 2 3 4)",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTIPOINT Z ((0 0 1), (2 3 4))",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "open_linestring_m",
          .input_wkt = "LINESTRING M (0 0 10, 2 3 20)",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTIPOINT M ((0 0 10), (2 3 20))",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "open_linestring_zm",
          .input_wkt = "LINESTRING ZM (0 0 1 10, 2 3 4 20)",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTIPOINT ZM ((0 0 1 10), (2 3 4 20))",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "closed_linestring_mod2",
          .input_wkt = "LINESTRING (0 0, 1 0, 1 1, 0 0)",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTIPOINT EMPTY",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "closed_linestring_ogcsfs",
          .input_wkt = "LINESTRING (0 0, 1 0, 1 1, 0 0)",
          .rule_fn = &BoundaryNodeRule::getBoundaryOGCSFS,
          .expected_wkt = "MULTIPOINT EMPTY",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "closed_linestring_monovalent",
          .input_wkt = "LINESTRING (0 0, 1 0, 1 1, 0 0)",
          .rule_fn = &BoundaryNodeRule::getBoundaryMonovalentEndPoint,
          .expected_wkt = "MULTIPOINT EMPTY",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "closed_linestring_endpoint",
          .input_wkt = "LINESTRING (0 0, 1 0, 1 1, 0 0)",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected_wkt = "POINT (0 0)",
          .expected_type = geos::geom::GEOS_POINT,
      },
      {
          .name = "closed_linestring_multivalent",
          .input_wkt = "LINESTRING (0 0, 1 0, 1 1, 0 0)",
          .rule_fn = &BoundaryNodeRule::getBoundaryMultivalentEndPoint,
          .expected_wkt = "POINT (0 0)",
          .expected_type = geos::geom::GEOS_POINT,
      },
      {
          .name = "closed_linestring_z_endpoint",
          .input_wkt = "LINESTRING Z (1 2 3, 4 2 5, 4 6 7, 1 2 3)",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected_wkt = "POINT Z (1 2 3)",
          .expected_type = geos::geom::GEOS_POINT,
      },
      {
          .name = "closed_linestring_m_endpoint",
          .input_wkt = "LINESTRING M (1 2 30, 4 2 50, 4 6 70, 1 2 30)",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected_wkt = "POINT M (1 2 30)",
          .expected_type = geos::geom::GEOS_POINT,
      },
      {
          .name = "closed_linestring_zm_endpoint",
          .input_wkt = "LINESTRING ZM (1 2 3 30, 4 2 5 50, 4 6 7 70, 1 2 3 30)",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected_wkt = "POINT ZM (1 2 3 30)",
          .expected_type = geos::geom::GEOS_POINT,
      },
      {
          .name = "empty_linearring_mod2",
          .input_wkt = "LINEARRING EMPTY",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTIPOINT EMPTY",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "empty_linearring_endpoint",
          .input_wkt = "LINEARRING EMPTY",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected_wkt = "MULTIPOINT EMPTY",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "linearring_mod2",
          .input_wkt = "LINEARRING (0 0, 2 0, 2 2, 0 2, 0 0)",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTIPOINT EMPTY",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "linearring_endpoint",
          .input_wkt = "LINEARRING (0 0, 2 0, 2 2, 0 2, 0 0)",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected_wkt = "POINT (0 0)",
          .expected_type = geos::geom::GEOS_POINT,
      },
  });

  RunBoundaryCases(kCases);
}

TEST(BoundaryOpTest, MultiLineStringCasesAcrossRules) {
  constexpr auto kCases = std::to_array<BoundaryCase>({
      {
          .name = "empty_multilinestring_mod2",
          .input_wkt = "MULTILINESTRING EMPTY",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTIPOINT EMPTY",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "empty_multilinestring_endpoint",
          .input_wkt = "MULTILINESTRING EMPTY",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected_wkt = "MULTIPOINT EMPTY",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "all_empty_components",
          .input_wkt = "MULTILINESTRING (EMPTY, EMPTY)",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected_wkt = "MULTIPOINT EMPTY",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "skips_empty_component_and_sorts_endpoints",
          .input_wkt = "MULTILINESTRING (EMPTY, (10 0, 0 0))",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTIPOINT ((0 0), (10 0))",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "skips_empty_component_single_boundary_point",
          .input_wkt = "MULTILINESTRING (EMPTY, (0 0, 1 0, 1 1, 0 0))",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected_wkt = "POINT (0 0)",
          .expected_type = geos::geom::GEOS_POINT,
      },
      {
          .name = "single_closed_component_mod2",
          .input_wkt = "MULTILINESTRING ((0 0, 1 0, 1 1, 0 0))",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTIPOINT EMPTY",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "single_closed_component_monovalent",
          .input_wkt = "MULTILINESTRING ((0 0, 1 0, 1 1, 0 0))",
          .rule_fn = &BoundaryNodeRule::getBoundaryMonovalentEndPoint,
          .expected_wkt = "MULTIPOINT EMPTY",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "single_closed_component_endpoint",
          .input_wkt = "MULTILINESTRING ((0 0, 1 0, 1 1, 0 0))",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected_wkt = "POINT (0 0)",
          .expected_type = geos::geom::GEOS_POINT,
      },
      {
          .name = "single_open_component_multivalent",
          .input_wkt = "MULTILINESTRING ((0 0, 1 0))",
          .rule_fn = &BoundaryNodeRule::getBoundaryMultivalentEndPoint,
          .expected_wkt = "MULTIPOINT EMPTY",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "connected_chain_mod2",
          .input_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 2 0))",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTIPOINT ((0 0), (2 0))",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "connected_chain_endpoint",
          .input_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 2 0))",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected_wkt = "MULTIPOINT ((0 0), (1 0), (2 0))",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "connected_chain_multivalent",
          .input_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 2 0))",
          .rule_fn = &BoundaryNodeRule::getBoundaryMultivalentEndPoint,
          .expected_wkt = "POINT (1 0)",
          .expected_type = geos::geom::GEOS_POINT,
      },
      {
          .name = "connected_chain_monovalent",
          .input_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 2 0))",
          .rule_fn = &BoundaryNodeRule::getBoundaryMonovalentEndPoint,
          .expected_wkt = "MULTIPOINT ((0 0), (2 0))",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "closed_loop_two_parts_mod2",
          .input_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 1 1, 0 0))",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTIPOINT EMPTY",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "closed_loop_two_parts_endpoint",
          .input_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 1 1, 0 0))",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected_wkt = "MULTIPOINT ((0 0), (1 0))",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "t_junction_valence3_mod2",
          .input_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 2 0), (1 0, 1 1))",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTIPOINT ((0 0), (1 0), (1 1), (2 0))",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "t_junction_valence3_monovalent",
          .input_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 2 0), (1 0, 1 1))",
          .rule_fn = &BoundaryNodeRule::getBoundaryMonovalentEndPoint,
          .expected_wkt = "MULTIPOINT ((0 0), (1 1), (2 0))",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "t_junction_valence3_multivalent",
          .input_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 2 0), (1 0, 1 1))",
          .rule_fn = &BoundaryNodeRule::getBoundaryMultivalentEndPoint,
          .expected_wkt = "POINT (1 0)",
          .expected_type = geos::geom::GEOS_POINT,
      },
      {
          .name = "lollipop_single_monovalent_endpoint",
          .input_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 2 0, 2 1, 1 0))",
          .rule_fn = &BoundaryNodeRule::getBoundaryMonovalentEndPoint,
          .expected_wkt = "POINT (0 0)",
          .expected_type = geos::geom::GEOS_POINT,
      },
      {
          .name = "four_rays_valence4_mod2",
          .input_wkt = "MULTILINESTRING ((0 0, 1 0), (0 0, -1 0), (0 0, 0 1), "
                       "(0 0, 0 -1))",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTIPOINT ((-1 0), (0 -1), (0 1), (1 0))",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "four_rays_valence4_multivalent",
          .input_wkt = "MULTILINESTRING ((0 0, 1 0), (0 0, -1 0), (0 0, 0 1), "
                       "(0 0, 0 -1))",
          .rule_fn = &BoundaryNodeRule::getBoundaryMultivalentEndPoint,
          .expected_wkt = "POINT (0 0)",
          .expected_type = geos::geom::GEOS_POINT,
      },
      {
          .name = "multilinestring_z_mod2",
          .input_wkt = "MULTILINESTRING Z ((0 0 10, 1 0 20), (1 0 20, 2 0 30))",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTIPOINT Z ((0 0 10), (2 0 30))",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "multilinestring_m_mod2",
          .input_wkt =
              "MULTILINESTRING M ((0 0 100, 1 0 200), (1 0 200, 2 0 300))",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTIPOINT M ((0 0 100), (2 0 300))",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
      {
          .name = "multilinestring_zm_mod2",
          .input_wkt = "MULTILINESTRING ZM ((0 0 10 100, 1 0 20 200), (1 0 20 "
                       "200, 2 0 30 300))",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTIPOINT ZM ((0 0 10 100), (2 0 30 300))",
          .expected_type = geos::geom::GEOS_MULTIPOINT,
      },
  });

  RunBoundaryCases(kCases);
}

TEST(BoundaryOpTest, MultiLineStringSinglePointBoundaryPreservesZAndM) {
  struct SinglePointCase {
    std::string_view name;
    std::string_view input_wkt;
    double x;
    double y;
    bool has_z;
    double z;
    bool has_m;
    double m;
  };

  constexpr auto kCases = std::to_array<SinglePointCase>({
      {
          .name = "xy",
          .input_wkt = "MULTILINESTRING ((1 2, 3 2, 3 4, 1 2))",
          .x = 1.0,
          .y = 2.0,
          .has_z = false,
          .z = 0.0,
          .has_m = false,
          .m = 0.0,
      },
      {
          .name = "xyz",
          .input_wkt = "MULTILINESTRING Z ((1 2 5, 3 2 6, 3 4 7, 1 2 5))",
          .x = 1.0,
          .y = 2.0,
          .has_z = true,
          .z = 5.0,
          .has_m = false,
          .m = 0.0,
      },
      {
          .name = "xym",
          .input_wkt = "MULTILINESTRING M ((1 2 50, 3 2 60, 3 4 70, 1 2 50))",
          .x = 1.0,
          .y = 2.0,
          .has_z = false,
          .z = 0.0,
          .has_m = true,
          .m = 50.0,
      },
      {
          .name = "xyzm",
          .input_wkt =
              "MULTILINESTRING ZM ((1 2 5 50, 3 2 6 60, 3 4 7 70, 1 2 5 50))",
          .x = 1.0,
          .y = 2.0,
          .has_z = true,
          .z = 5.0,
          .has_m = true,
          .m = 50.0,
      },
  });

  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const auto& tc : kCases) {
    SCOPED_TRACE(std::string(tc.name));
    std::unique_ptr<Geometry> geom = reader.read(std::string(tc.input_wkt));
    ASSERT_NE(geom, nullptr);

    std::unique_ptr<Geometry> boundary =
        BoundaryOp::getBoundary(*geom, BoundaryNodeRule::getBoundaryEndPoint());
    ASSERT_NE(boundary, nullptr);
    ExpectPointValues(*boundary, tc.x, tc.y, tc.has_z, tc.z, tc.has_m, tc.m);
  }
}

TEST(BoundaryOpTest, MultiLineStringMixedDimensionsAndEmptyComponent) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  CoordinateSequence xy_seq({CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 0.0)});
  CoordinateSequence xyz_seq(
      {Coordinate(1.0, 0.0, 7.0), Coordinate(2.0, 0.0, 9.0)});

  std::vector<std::unique_ptr<LineString>> lines;
  lines.push_back(factory->createLineString());
  lines.push_back(factory->createLineString(xy_seq));
  lines.push_back(factory->createLineString(xyz_seq));

  std::unique_ptr<MultiLineString> mline =
      factory->createMultiLineString(std::move(lines));
  ASSERT_NE(mline, nullptr);

  std::unique_ptr<Geometry> boundary = BoundaryOp::getBoundary(*mline);
  ASSERT_NE(boundary, nullptr);
  ASSERT_EQ(boundary->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  ASSERT_EQ(boundary->getNumGeometries(), 2u);

  ExpectPointValues(*boundary->getGeometryN(0), 0.0, 0.0, false, 0.0, false,
                    0.0);
  ExpectPointValues(*boundary->getGeometryN(1), 2.0, 0.0, true, 9.0, false,
                    0.0);
}

TEST(BoundaryOpTest, NonLinearGeometriesDelegateToGeometryGetBoundary) {
  constexpr auto kCases = std::to_array<BoundaryCase>({
      {
          .name = "empty_point",
          .input_wkt = "POINT EMPTY",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
          .expected_type = geos::geom::GEOS_GEOMETRYCOLLECTION,
      },
      {
          .name = "point",
          .input_wkt = "POINT (1 2)",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
          .expected_type = geos::geom::GEOS_GEOMETRYCOLLECTION,
      },
      {
          .name = "empty_multipoint",
          .input_wkt = "MULTIPOINT EMPTY",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
          .expected_type = geos::geom::GEOS_GEOMETRYCOLLECTION,
      },
      {
          .name = "multipoint",
          .input_wkt = "MULTIPOINT ((0 0), (1 1))",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected_wkt = "GEOMETRYCOLLECTION EMPTY",
          .expected_type = geos::geom::GEOS_GEOMETRYCOLLECTION,
      },
      {
          .name = "empty_polygon",
          .input_wkt = "POLYGON EMPTY",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTILINESTRING EMPTY",
          .expected_type = geos::geom::GEOS_MULTILINESTRING,
      },
      {
          .name = "polygon_without_holes",
          .input_wkt = "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "LINESTRING (0 0, 4 0, 4 4, 0 4, 0 0)",
          .expected_type = geos::geom::GEOS_LINESTRING,
      },
      {
          .name = "polygon_with_hole",
          .input_wkt = "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 4, 4 "
                       "4, 4 2, 2 2))",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected_wkt = "MULTILINESTRING ((0 0, 10 0, 10 10, 0 10, 0 0), (2 "
                          "2, 2 4, 4 4, 4 2, 2 2))",
          .expected_type = geos::geom::GEOS_MULTILINESTRING,
      },
      {
          .name = "empty_multipolygon",
          .input_wkt = "MULTIPOLYGON EMPTY",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTILINESTRING EMPTY",
          .expected_type = geos::geom::GEOS_MULTILINESTRING,
      },
      {
          .name = "multipolygon",
          .input_wkt = "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), ((10 0, 12 "
                       "0, 12 2, 10 2, 10 0)))",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_wkt = "MULTILINESTRING ((0 0, 2 0, 2 2, 0 2, 0 0), (10 0, "
                          "12 0, 12 2, 10 2, 10 0))",
          .expected_type = geos::geom::GEOS_MULTILINESTRING,
      },
  });

  RunBoundaryCases(kCases);
}

TEST(BoundaryOpTest, GeometryCollectionAndCurvedGeometriesThrow) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  constexpr auto kGeometryCollections = std::to_array<std::string_view>({
      "GEOMETRYCOLLECTION EMPTY",
      "GEOMETRYCOLLECTION (POINT (1 2), LINESTRING (0 0, 1 1))",
  });

  for (std::string_view wkt : kGeometryCollections) {
    SCOPED_TRACE(std::string(wkt));
    std::unique_ptr<Geometry> geom = reader.read(std::string(wkt));
    ASSERT_NE(geom, nullptr);
    EXPECT_THROW(BoundaryOp::getBoundary(*geom), IllegalArgumentException);
  }

  constexpr auto kCurvedGeoms = std::to_array<std::string_view>({
      "CIRCULARSTRING EMPTY",
      "CIRCULARSTRING (0 0, 1 1, 2 0)",
      "COMPOUNDCURVE ((0 0, 1 0), CIRCULARSTRING (1 0, 2 1, 3 0))",
      "CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0))",
      "GEOMETRYCOLLECTION (POINT (0 0), CIRCULARSTRING (0 0, 1 1, 2 0))",
  });

  for (std::string_view wkt : kCurvedGeoms) {
    SCOPED_TRACE(std::string(wkt));
    std::unique_ptr<Geometry> geom = reader.read(std::string(wkt));
    ASSERT_NE(geom, nullptr);
    EXPECT_THROW(BoundaryOp::getBoundary(*geom), UnsupportedOperationException);
  }
}

TEST(BoundaryOpTest, HasBoundaryAcrossDimensionsAndRules) {
  constexpr auto kCases = std::to_array<HasBoundaryCase>({
      {
          .name = "empty_point",
          .input_wkt = "POINT EMPTY",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected = false,
      },
      {
          .name = "empty_linestring_endpoint",
          .input_wkt = "LINESTRING EMPTY",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected = false,
      },
      {
          .name = "empty_linearring",
          .input_wkt = "LINEARRING EMPTY",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected = false,
      },
      {
          .name = "empty_multilinestring",
          .input_wkt = "MULTILINESTRING EMPTY",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected = false,
      },
      {
          .name = "empty_polygon",
          .input_wkt = "POLYGON EMPTY",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected = false,
      },
      {
          .name = "empty_geometrycollection",
          .input_wkt = "GEOMETRYCOLLECTION EMPTY",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected = false,
      },
      {
          .name = "empty_circularstring",
          .input_wkt = "CIRCULARSTRING EMPTY",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected = false,
      },
      {
          .name = "nonempty_point",
          .input_wkt = "POINT (1 2)",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected = false,
      },
      {
          .name = "nonempty_multipoint",
          .input_wkt = "MULTIPOINT ((0 0), (1 1))",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected = false,
      },
      {
          .name = "collection_of_point",
          .input_wkt = "GEOMETRYCOLLECTION (POINT (0 0))",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected = false,
      },
      {
          .name = "open_linestring_mod2",
          .input_wkt = "LINESTRING (0 0, 1 1)",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected = true,
      },
      {
          .name = "open_linestring_multivalent",
          .input_wkt = "LINESTRING (0 0, 1 1)",
          .rule_fn = &BoundaryNodeRule::getBoundaryMultivalentEndPoint,
          .expected = true,
      },
      {
          .name = "closed_linestring_mod2",
          .input_wkt = "LINESTRING (0 0, 1 0, 1 1, 0 0)",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected = false,
      },
      {
          .name = "closed_linestring_monovalent",
          .input_wkt = "LINESTRING (0 0, 1 0, 1 1, 0 0)",
          .rule_fn = &BoundaryNodeRule::getBoundaryMonovalentEndPoint,
          .expected = false,
      },
      {
          .name = "closed_linestring_endpoint",
          .input_wkt = "LINESTRING (0 0, 1 0, 1 1, 0 0)",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected = true,
      },
      {
          .name = "closed_linestring_multivalent",
          .input_wkt = "LINESTRING (0 0, 1 0, 1 1, 0 0)",
          .rule_fn = &BoundaryNodeRule::getBoundaryMultivalentEndPoint,
          .expected = true,
      },
      {
          .name = "linearring_mod2",
          .input_wkt = "LINEARRING (0 0, 1 0, 1 1, 0 0)",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected = false,
      },
      {
          .name = "linearring_endpoint",
          .input_wkt = "LINEARRING (0 0, 1 0, 1 1, 0 0)",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected = true,
      },
      {
          .name = "open_multilinestring_multivalent",
          .input_wkt = "MULTILINESTRING ((0 0, 1 0))",
          .rule_fn = &BoundaryNodeRule::getBoundaryMultivalentEndPoint,
          .expected = false,
      },
      {
          .name = "closed_multilinestring_mod2",
          .input_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 1 1, 0 0))",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected = false,
      },
      {
          .name = "closed_multilinestring_endpoint",
          .input_wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 1 1, 0 0))",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected = true,
      },
      {
          .name = "nonempty_polygon",
          .input_wkt = "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected = true,
      },
      {
          .name = "nonempty_multipolygon",
          .input_wkt = "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)))",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected = true,
      },
      {
          .name = "collection_with_polygon",
          .input_wkt = "GEOMETRYCOLLECTION (POLYGON ((0 0, 1 0, 1 1, 0 0)), "
                       "LINESTRING (0 0, 5 5))",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected = true,
      },
  });

  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const auto& tc : kCases) {
    SCOPED_TRACE(std::string(tc.name));
    std::unique_ptr<Geometry> geom = reader.read(std::string(tc.input_wkt));
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(BoundaryOp::hasBoundary(*geom, tc.rule_fn()), tc.expected);
  }

  std::unique_ptr<Geometry> linear_gc =
      reader.read("GEOMETRYCOLLECTION (LINESTRING (0 0, 1 0))");
  ASSERT_NE(linear_gc, nullptr);
  EXPECT_THROW(BoundaryOp::hasBoundary(*linear_gc,
                                       BoundaryNodeRule::getBoundaryRuleMod2()),
               IllegalArgumentException);

  std::unique_ptr<Geometry> circular =
      reader.read("CIRCULARSTRING (0 0, 1 1, 2 0)");
  ASSERT_NE(circular, nullptr);
  EXPECT_THROW(BoundaryOp::hasBoundary(*circular,
                                       BoundaryNodeRule::getBoundaryRuleMod2()),
               UnsupportedOperationException);
}

}  // namespace
}  // namespace geos_google
