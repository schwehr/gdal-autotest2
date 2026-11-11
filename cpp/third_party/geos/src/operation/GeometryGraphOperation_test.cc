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

// Unit tests for third_party/geos/src/operation/GeometryGraphOperation.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - Tests all three GeometryGraphOperation constructors, computation precision
//   model selection (including LineIntersector rounding), BoundaryNodeRule
//   forwarding, getArgGeometry(), setComputationPrecision(), and virtual
//   destruction.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/GeometryGraphOperation.cpp

#include "third_party/geos/include/geos/operation/GeometryGraphOperation.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/BoundaryNodeRule.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/geomgraph/GeometryGraph.h"
#include "third_party/geos/include/geos/geomgraph/Node.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::algorithm::BoundaryNodeRule;
using geos::algorithm::LineIntersector;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYZM;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::PrecisionModel;
using geos::geomgraph::GeometryGraph;
using geos::geomgraph::Node;
using geos::io::WKTReader;
using geos::operation::GeometryGraphOperation;
using geos::util::UnsupportedOperationException;

static_assert(!std::is_copy_constructible_v<GeometryGraphOperation>);
static_assert(!std::is_copy_assignable_v<GeometryGraphOperation>);

class TestGeometryGraphOperation final : public GeometryGraphOperation {
 public:
  using GeometryGraphOperation::GeometryGraphOperation;

  const PrecisionModel* precision_model() const { return resultPrecisionModel; }

  std::size_t arg_count() const { return arg.size(); }

  GeometryGraph* graph(std::size_t i) const { return arg.at(i).get(); }

  LineIntersector& line_intersector() { return li; }

  void update_computation_precision(const PrecisionModel* pm) {
    setComputationPrecision(pm);
  }
};

// Segments (0,0)-(3,1) and (1,1)-(2,-1) intersect at (9/7, 3/7).
void ExpectIntersectionRoundedTo(LineIntersector& li, double expected_x,
                                 double expected_y, double tolerance) {
  CoordinateXY p0(0.0, 0.0);
  CoordinateXY p1(3.0, 1.0);
  CoordinateXY q0(1.0, 1.0);
  CoordinateXY q1(2.0, -1.0);
  li.computeIntersection(p0, p1, q0, q1);
  ASSERT_EQ(li.getIntersectionNum(), 1u);
  const CoordinateXYZM& pt = li.getIntersection(0);
  EXPECT_NEAR(pt.x, expected_x, tolerance);
  EXPECT_NEAR(pt.y, expected_y, tolerance);
}

TEST(GeometryGraphOperationTest, DirectPublicConstructionAndDestruction) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  std::unique_ptr<Geometry> g0 = reader.read("LINESTRING (0 0, 2 2)");
  std::unique_ptr<Geometry> g1 =
      reader.read("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  ASSERT_NE(g0, nullptr);
  ASSERT_NE(g1, nullptr);

  {
    GeometryGraphOperation two_arg_stack(g0.get(), g1.get());
    EXPECT_EQ(two_arg_stack.getArgGeometry(0), g0.get());
    EXPECT_EQ(two_arg_stack.getArgGeometry(1), g1.get());

    GeometryGraphOperation three_arg_stack(
        g0.get(), g1.get(), BoundaryNodeRule::getBoundaryEndPoint());
    EXPECT_EQ(three_arg_stack.getArgGeometry(0), g0.get());
    EXPECT_EQ(three_arg_stack.getArgGeometry(1), g1.get());

    GeometryGraphOperation one_arg_stack(g0.get());
    EXPECT_EQ(one_arg_stack.getArgGeometry(0), g0.get());
  }

  // Heap-allocated instances exercise the deleting virtual destructor.
  auto two_arg_heap =
      std::make_unique<GeometryGraphOperation>(g0.get(), g1.get());
  EXPECT_EQ(two_arg_heap->getArgGeometry(0), g0.get());
  EXPECT_EQ(two_arg_heap->getArgGeometry(1), g1.get());

  auto three_arg_heap = std::make_unique<GeometryGraphOperation>(
      g0.get(), g1.get(), BoundaryNodeRule::getBoundaryRuleMod2());
  EXPECT_EQ(three_arg_heap->getArgGeometry(0), g0.get());
  EXPECT_EQ(three_arg_heap->getArgGeometry(1), g1.get());

  auto one_arg_heap = std::make_unique<GeometryGraphOperation>(g0.get());
  EXPECT_EQ(one_arg_heap->getArgGeometry(0), g0.get());

  std::unique_ptr<GeometryGraphOperation> poly_derived =
      std::make_unique<TestGeometryGraphOperation>(g1.get());
  EXPECT_EQ(poly_derived->getArgGeometry(0), g1.get());
}

TEST(GeometryGraphOperationTest, TwoArgSelectsMostPrecisePrecisionModel) {
  struct PrecisionPairCase {
    std::string_view name;
    PrecisionModel pm0;
    PrecisionModel pm1;
    bool expect_pm0;
    double expected_int_x;
    double expected_int_y;
    double tolerance;
  };

  const std::vector<PrecisionPairCase> cases = {
      {
          .name = "both_floating_picks_pm0",
          .pm0 = PrecisionModel(PrecisionModel::FLOATING),
          .pm1 = PrecisionModel(PrecisionModel::FLOATING),
          .expect_pm0 = true,
          .expected_int_x = 9.0 / 7.0,
          .expected_int_y = 3.0 / 7.0,
          .tolerance = 1e-14,
      },
      {
          .name = "equal_fixed_picks_pm0",
          .pm0 = PrecisionModel(100.0),
          .pm1 = PrecisionModel(100.0),
          .expect_pm0 = true,
          .expected_int_x = 1.29,
          .expected_int_y = 0.43,
          .tolerance = 1e-12,
      },
      {
          .name = "floating_over_fixed_picks_pm0",
          .pm0 = PrecisionModel(PrecisionModel::FLOATING),
          .pm1 = PrecisionModel(10.0),
          .expect_pm0 = true,
          .expected_int_x = 9.0 / 7.0,
          .expected_int_y = 3.0 / 7.0,
          .tolerance = 1e-14,
      },
      {
          .name = "fixed_1000_over_fixed_10_picks_pm0",
          .pm0 = PrecisionModel(1000.0),
          .pm1 = PrecisionModel(10.0),
          .expect_pm0 = true,
          .expected_int_x = 1.286,
          .expected_int_y = 0.429,
          .tolerance = 1e-12,
      },
      {
          .name = "floating_single_over_fixed_100_picks_pm0",
          .pm0 = PrecisionModel(PrecisionModel::FLOATING_SINGLE),
          .pm1 = PrecisionModel(100.0),
          .expect_pm0 = true,
          .expected_int_x = static_cast<double>(static_cast<float>(9.0 / 7.0)),
          .expected_int_y = static_cast<double>(static_cast<float>(3.0 / 7.0)),
          .tolerance = 1e-14,
      },
      {
          .name = "fixed_over_floating_picks_pm1",
          .pm0 = PrecisionModel(10.0),
          .pm1 = PrecisionModel(PrecisionModel::FLOATING),
          .expect_pm0 = false,
          .expected_int_x = 9.0 / 7.0,
          .expected_int_y = 3.0 / 7.0,
          .tolerance = 1e-14,
      },
      {
          .name = "fixed_10_under_fixed_1000_picks_pm1",
          .pm0 = PrecisionModel(10.0),
          .pm1 = PrecisionModel(1000.0),
          .expect_pm0 = false,
          .expected_int_x = 1.286,
          .expected_int_y = 0.429,
          .tolerance = 1e-12,
      },
      {
          .name = "floating_single_under_floating_picks_pm1",
          .pm0 = PrecisionModel(PrecisionModel::FLOATING_SINGLE),
          .pm1 = PrecisionModel(PrecisionModel::FLOATING),
          .expect_pm0 = false,
          .expected_int_x = 9.0 / 7.0,
          .expected_int_y = 3.0 / 7.0,
          .tolerance = 1e-14,
      },
  };

  for (const auto& tc : cases) {
    SCOPED_TRACE(std::string(tc.name));
    GeometryFactory::Ptr f0 = GeometryFactory::create(&tc.pm0);
    GeometryFactory::Ptr f1 = GeometryFactory::create(&tc.pm1);
    WKTReader r0(f0.get());
    WKTReader r1(f1.get());

    std::unique_ptr<Geometry> g0 = r0.read("LINESTRING (0 0, 3 1)");
    std::unique_ptr<Geometry> g1 = r1.read("LINESTRING (1 1, 2 -1)");
    ASSERT_NE(g0, nullptr);
    ASSERT_NE(g1, nullptr);

    const PrecisionModel* expected_pm =
        tc.expect_pm0 ? g0->getPrecisionModel() : g1->getPrecisionModel();

    TestGeometryGraphOperation op_two_arg(g0.get(), g1.get());
    EXPECT_EQ(op_two_arg.arg_count(), 2u);
    EXPECT_EQ(op_two_arg.getArgGeometry(0), g0.get());
    EXPECT_EQ(op_two_arg.getArgGeometry(1), g1.get());
    EXPECT_EQ(op_two_arg.precision_model(), expected_pm);
    EXPECT_EQ(&op_two_arg.graph(0)->getBoundaryNodeRule(),
              &BoundaryNodeRule::getBoundaryOGCSFS());
    EXPECT_EQ(&op_two_arg.graph(1)->getBoundaryNodeRule(),
              &BoundaryNodeRule::getBoundaryOGCSFS());
    ExpectIntersectionRoundedTo(op_two_arg.line_intersector(),
                                tc.expected_int_x, tc.expected_int_y,
                                tc.tolerance);

    const BoundaryNodeRule& ep_rule = BoundaryNodeRule::getBoundaryEndPoint();
    TestGeometryGraphOperation op_three_arg(g0.get(), g1.get(), ep_rule);
    EXPECT_EQ(op_three_arg.arg_count(), 2u);
    EXPECT_EQ(op_three_arg.getArgGeometry(0), g0.get());
    EXPECT_EQ(op_three_arg.getArgGeometry(1), g1.get());
    EXPECT_EQ(op_three_arg.precision_model(), expected_pm);
    EXPECT_EQ(&op_three_arg.graph(0)->getBoundaryNodeRule(), &ep_rule);
    EXPECT_EQ(&op_three_arg.graph(1)->getBoundaryNodeRule(), &ep_rule);
    ExpectIntersectionRoundedTo(op_three_arg.line_intersector(),
                                tc.expected_int_x, tc.expected_int_y,
                                tc.tolerance);
  }
}

TEST(GeometryGraphOperationTest,
     OneArgConstructorUsesSingleGeometryAndPrecision) {
  PrecisionModel fixed_pm(10.0);
  GeometryFactory::Ptr fixed_factory = GeometryFactory::create(&fixed_pm);
  GeometryFactory::Ptr float_factory = GeometryFactory::create();

  WKTReader fixed_reader(fixed_factory.get());
  WKTReader float_reader(float_factory.get());

  std::unique_ptr<Geometry> fixed_line =
      fixed_reader.read("LINESTRING (0 0, 3 1)");
  std::unique_ptr<Geometry> float_line =
      float_reader.read("LINESTRING (0 0, 3 1)");
  ASSERT_NE(fixed_line, nullptr);
  ASSERT_NE(float_line, nullptr);

  TestGeometryGraphOperation fixed_op(fixed_line.get());
  EXPECT_EQ(fixed_op.arg_count(), 1u);
  EXPECT_EQ(fixed_op.getArgGeometry(0), fixed_line.get());
  EXPECT_EQ(fixed_op.precision_model(), fixed_line->getPrecisionModel());
  EXPECT_EQ(&fixed_op.graph(0)->getBoundaryNodeRule(),
            &BoundaryNodeRule::getBoundaryOGCSFS());
  ExpectIntersectionRoundedTo(fixed_op.line_intersector(), 1.3, 0.4, 1e-12);

  TestGeometryGraphOperation float_op(float_line.get());
  EXPECT_EQ(float_op.arg_count(), 1u);
  EXPECT_EQ(float_op.getArgGeometry(0), float_line.get());
  EXPECT_EQ(float_op.precision_model(), float_line->getPrecisionModel());
  ExpectIntersectionRoundedTo(float_op.line_intersector(), 9.0 / 7.0, 3.0 / 7.0,
                              1e-14);
}

TEST(GeometryGraphOperationTest, BoundaryNodeRulePropagatesToBothGraphs) {
  using RuleFn = const BoundaryNodeRule& (*)();
  struct RuleCase {
    std::string_view name;
    RuleFn rule_fn;
    std::size_t expected_closed_bdy_nodes;
    std::size_t expected_open_bdy_nodes;
  };

  constexpr auto kRuleCases = std::to_array<RuleCase>({
      {
          .name = "mod2",
          .rule_fn = &BoundaryNodeRule::getBoundaryRuleMod2,
          .expected_closed_bdy_nodes = 0u,
          .expected_open_bdy_nodes = 2u,
      },
      {
          .name = "ogcsfs",
          .rule_fn = &BoundaryNodeRule::getBoundaryOGCSFS,
          .expected_closed_bdy_nodes = 0u,
          .expected_open_bdy_nodes = 2u,
      },
      {
          .name = "endpoint",
          .rule_fn = &BoundaryNodeRule::getBoundaryEndPoint,
          .expected_closed_bdy_nodes = 1u,
          .expected_open_bdy_nodes = 2u,
      },
      {
          .name = "monovalent",
          .rule_fn = &BoundaryNodeRule::getBoundaryMonovalentEndPoint,
          .expected_closed_bdy_nodes = 0u,
          .expected_open_bdy_nodes = 2u,
      },
      {
          .name = "multivalent",
          .rule_fn = &BoundaryNodeRule::getBoundaryMultivalentEndPoint,
          .expected_closed_bdy_nodes = 0u,
          .expected_open_bdy_nodes = 0u,
      },
  });

  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  std::unique_ptr<Geometry> closed_line =
      reader.read("LINESTRING (0 0, 1 0, 1 1, 0 0)");
  std::unique_ptr<Geometry> open_line = reader.read("LINESTRING (2 0, 4 0)");
  ASSERT_NE(closed_line, nullptr);
  ASSERT_NE(open_line, nullptr);

  for (const auto& tc : kRuleCases) {
    SCOPED_TRACE(std::string(tc.name));
    const BoundaryNodeRule& rule = tc.rule_fn();
    TestGeometryGraphOperation op(closed_line.get(), open_line.get(), rule);

    EXPECT_EQ(&op.graph(0)->getBoundaryNodeRule(), &rule);
    EXPECT_EQ(&op.graph(1)->getBoundaryNodeRule(), &rule);
    EXPECT_EQ(op.graph(0)->getGeometry(), closed_line.get());
    EXPECT_EQ(op.graph(1)->getGeometry(), open_line.get());

    std::vector<Node*>* bdy0 = op.graph(0)->getBoundaryNodes();
    std::vector<Node*>* bdy1 = op.graph(1)->getBoundaryNodes();
    ASSERT_NE(bdy0, nullptr);
    ASSERT_NE(bdy1, nullptr);
    EXPECT_EQ(bdy0->size(), tc.expected_closed_bdy_nodes);
    EXPECT_EQ(bdy1->size(), tc.expected_open_bdy_nodes);
    if (tc.expected_closed_bdy_nodes == 1u) {
      EXPECT_EQ((*bdy0)[0]->getCoordinate(), CoordinateXY(0.0, 0.0));
    }
  }
}

TEST(GeometryGraphOperationTest,
     SetComputationPrecisionUpdatesLineIntersector) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  std::unique_ptr<Geometry> pt = reader.read("POINT (0 0)");
  ASSERT_NE(pt, nullptr);

  TestGeometryGraphOperation op(pt.get());
  EXPECT_EQ(op.precision_model(), pt->getPrecisionModel());
  ExpectIntersectionRoundedTo(op.line_intersector(), 9.0 / 7.0, 3.0 / 7.0,
                              1e-14);

  PrecisionModel fixed_pm_10(10.0);
  op.update_computation_precision(&fixed_pm_10);
  EXPECT_EQ(op.precision_model(), &fixed_pm_10);
  ExpectIntersectionRoundedTo(op.line_intersector(), 1.3, 0.4, 1e-12);

  PrecisionModel fixed_pm_1(1.0);
  op.update_computation_precision(&fixed_pm_1);
  EXPECT_EQ(op.precision_model(), &fixed_pm_1);
  ExpectIntersectionRoundedTo(op.line_intersector(), 1.0, 0.0, 1e-12);
}

TEST(GeometryGraphOperationTest, EmptyVariedAndUnsupportedCurvedGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  constexpr auto kValidWkts = std::to_array<std::string_view>({
      "POINT EMPTY",
      "LINESTRING EMPTY",
      "POLYGON EMPTY",
      "GEOMETRYCOLLECTION EMPTY",
      "POINT (5 6)",
      "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)))",
      "GEOMETRYCOLLECTION (POINT (1 1), LINESTRING (0 0, 2 2))",
  });

  for (std::string_view wkt : kValidWkts) {
    SCOPED_TRACE(std::string(wkt));
    std::unique_ptr<Geometry> geom = reader.read(std::string(wkt));
    ASSERT_NE(geom, nullptr);

    TestGeometryGraphOperation one_op(geom.get());
    EXPECT_EQ(one_op.getArgGeometry(0), geom.get());

    TestGeometryGraphOperation two_op(geom.get(), geom.get());
    EXPECT_EQ(two_op.getArgGeometry(0), geom.get());
    EXPECT_EQ(two_op.getArgGeometry(1), geom.get());
  }

  std::unique_ptr<Geometry> curved =
      reader.read("CIRCULARSTRING (0 0, 1 1, 2 0)");
  std::unique_ptr<Geometry> pt = reader.read("POINT (0 0)");
  ASSERT_NE(curved, nullptr);
  ASSERT_NE(pt, nullptr);

  EXPECT_THROW(GeometryGraphOperation(curved.get()),
               UnsupportedOperationException);
  EXPECT_THROW(GeometryGraphOperation(curved.get(), pt.get()),
               UnsupportedOperationException);
  EXPECT_THROW(GeometryGraphOperation(pt.get(), curved.get()),
               UnsupportedOperationException);
  EXPECT_THROW(GeometryGraphOperation(pt.get(), curved.get(),
                                      BoundaryNodeRule::getBoundaryEndPoint()),
               UnsupportedOperationException);
}

}  // namespace
}  // namespace geos_google
