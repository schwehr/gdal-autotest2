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
// third_party/geos/include/geos/operation/relateng/RelateMatrixPredicate.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/operation/relateng/RelateMatrixPredicate.h

#include "third_party/geos/include/geos/operation/relateng/RelateMatrixPredicate.h"

#include <array>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/BoundaryNodeRule.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/IntersectionMatrix.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/relateng/IMPredicate.h"
#include "third_party/geos/include/geos/operation/relateng/RelateNG.h"
#include "third_party/geos/include/geos/operation/relateng/TopologyPredicate.h"

namespace geos_google {
namespace {

using geos::algorithm::BoundaryNodeRule;
using geos::geom::Dimension;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::IntersectionMatrix;
using geos::geom::Location;
using geos::io::WKTReader;
using geos::operation::relateng::IMPredicate;
using geos::operation::relateng::RelateMatrixPredicate;
using geos::operation::relateng::RelateNG;
using geos::operation::relateng::TopologyPredicate;

static_assert(std::is_base_of_v<IMPredicate, RelateMatrixPredicate>);
static_assert(std::is_base_of_v<TopologyPredicate, RelateMatrixPredicate>);

TEST(RelateMatrixPredicateTest, DefaultStateAndCapabilities) {
  RelateMatrixPredicate pred;

  EXPECT_EQ(pred.name(), "relateMatrix");
  EXPECT_FALSE(pred.requireInteraction());
  EXPECT_FALSE(pred.isDetermined());
  EXPECT_FALSE(pred.valueIM());
  EXPECT_FALSE(pred.isKnown());
  EXPECT_FALSE(pred.value());
  EXPECT_EQ(pred.toString(), "relateMatrix: FFFFFFFF2");

  // Verify inherited TopologyPredicate capabilities through the base interface.
  TopologyPredicate& base = pred;
  EXPECT_EQ(base.name(), "relateMatrix");
  EXPECT_TRUE(base.requireSelfNoding());
  EXPECT_FALSE(base.requireInteraction());
  EXPECT_FALSE(base.requireCovers(true));
  EXPECT_FALSE(base.requireCovers(false));
  EXPECT_TRUE(base.requireExteriorCheck(true));
  EXPECT_TRUE(base.requireExteriorCheck(false));

  std::unique_ptr<IntersectionMatrix> im = pred.getIM();
  ASSERT_NE(im, nullptr);
  EXPECT_EQ(im->toString(), "FFFFFFFF2");
  EXPECT_EQ(im->get(Location::EXTERIOR, Location::EXTERIOR), Dimension::A);

  std::ostringstream tp_stream;
  tp_stream << base;
  EXPECT_EQ(tp_stream.str(), "relateMatrix");

  std::ostringstream imp_stream;
  imp_stream << static_cast<const IMPredicate&>(pred);
  EXPECT_EQ(imp_stream.str(), "relateMatrix: FFFFFFFF2 FFFFFFFF2");
}

TEST(RelateMatrixPredicateTest, NeverShortCircuitsAndFinalizesToFalse) {
  RelateMatrixPredicate pred;
  pred.init(Dimension::A, Dimension::A);
  EXPECT_FALSE(pred.isKnown());
  EXPECT_FALSE(pred.isDetermined());

  constexpr auto kLocs = std::to_array<Location>({
      Location::INTERIOR,
      Location::BOUNDARY,
      Location::EXTERIOR,
  });
  for (Location loc_a : kLocs) {
    for (Location loc_b : kLocs) {
      pred.updateDimension(loc_a, loc_b, Dimension::A);
      EXPECT_FALSE(pred.isDetermined());
      EXPECT_FALSE(pred.isKnown());
    }
  }

  std::unique_ptr<IntersectionMatrix> before_finish = pred.getIM();
  ASSERT_NE(before_finish, nullptr);
  EXPECT_EQ(before_finish->toString(), "222222222");
  EXPECT_FALSE(pred.valueIM());

  pred.finish();
  EXPECT_TRUE(pred.isKnown());
  EXPECT_FALSE(pred.value());
  EXPECT_FALSE(pred.isDetermined());
  EXPECT_FALSE(pred.valueIM());

  std::unique_ptr<IntersectionMatrix> after_finish = pred.getIM();
  ASSERT_NE(after_finish, nullptr);
  EXPECT_EQ(after_finish->toString(), "222222222");
}

TEST(RelateMatrixPredicateTest, GetIMReturnsIndependentSnapshotCopies) {
  RelateMatrixPredicate pred;

  pred.updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::L);
  pred.updateDimension(Location::BOUNDARY, Location::INTERIOR, Dimension::P);
  // Decreasing dimensions must not overwrite higher recorded dimensions.
  pred.updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::P);
  pred.updateDimension(Location::INTERIOR, Location::INTERIOR,
                       Dimension::False);

  std::unique_ptr<IntersectionMatrix> im1 = pred.getIM();
  ASSERT_NE(im1, nullptr);
  EXPECT_EQ(im1->toString(), "1FF0FFFF2");

  // Mutating the returned snapshot does not alter the predicate's matrix.
  im1->setAll(Dimension::A);
  EXPECT_EQ(im1->toString(), "222222222");

  std::unique_ptr<IntersectionMatrix> im2 = pred.getIM();
  ASSERT_NE(im2, nullptr);
  EXPECT_NE(im1.get(), im2.get());
  EXPECT_EQ(im2->toString(), "1FF0FFFF2");

  pred.updateDimension(Location::INTERIOR, Location::EXTERIOR, Dimension::A);
  std::unique_ptr<IntersectionMatrix> im3 = pred.getIM();
  ASSERT_NE(im3, nullptr);
  EXPECT_EQ(im3->toString(), "1F20FFFF2");
  EXPECT_EQ(im2->toString(), "1FF0FFFF2");
}

TEST(RelateMatrixPredicateTest, PolymorphicDestructionThroughBasePointer) {
  std::unique_ptr<TopologyPredicate> pred =
      std::make_unique<RelateMatrixPredicate>();
  EXPECT_EQ(pred->name(), "relateMatrix");
  EXPECT_FALSE(pred->requireInteraction());

  pred->updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::P);
  EXPECT_FALSE(pred->isKnown());
  pred->finish();
  EXPECT_TRUE(pred->isKnown());
  EXPECT_FALSE(pred->value());
}

TEST(RelateMatrixPredicateTest, EvaluatesFullMatrixViaRelateNG) {
  struct MatrixCase {
    std::string_view label;
    const char* wkt_a;
    const char* wkt_b;
    std::string_view expected_im;
  };

  constexpr auto kCases = std::to_array<MatrixCase>({
      {
          .label = "disjoint_polygons",
          .wkt_a = "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
          .wkt_b = "POLYGON ((5 5, 7 5, 7 7, 5 7, 5 5))",
          .expected_im = "FF2FF1212",
      },
      {
          .label = "overlapping_polygons",
          .wkt_a = "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))",
          .wkt_b = "POLYGON ((2 2, 6 2, 6 6, 2 6, 2 2))",
          .expected_im = "212101212",
      },
      {
          .label = "line_crossing_polygon",
          .wkt_a = "LINESTRING (-1 2, 5 2)",
          .wkt_b = "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))",
          .expected_im = "101FF0212",
      },
      {
          .label = "point_inside_polygon",
          .wkt_a = "POINT (2 2)",
          .wkt_b = "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))",
          .expected_im = "0FFFFF212",
      },
      {
          .label = "empty_point_vs_point",
          .wkt_a = "POINT EMPTY",
          .wkt_b = "POINT (1 1)",
          .expected_im = "FFFFFF0F2",
      },
      {
          .label = "empty_vs_empty",
          .wkt_a = "POLYGON EMPTY",
          .wkt_b = "LINESTRING EMPTY",
          .expected_im = "FFFFFFFF2",
      },
  });

  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.label);
    std::unique_ptr<Geometry> geom_a = reader.read(tc.wkt_a);
    std::unique_ptr<Geometry> geom_b = reader.read(tc.wkt_b);

    RelateMatrixPredicate pred;
    EXPECT_FALSE(RelateNG::relate(geom_a.get(), geom_b.get(), pred));
    EXPECT_TRUE(pred.isKnown());
    EXPECT_FALSE(pred.value());

    std::unique_ptr<IntersectionMatrix> im = pred.getIM();
    ASSERT_NE(im, nullptr);
    EXPECT_EQ(im->toString(), tc.expected_im);

    std::unique_ptr<IntersectionMatrix> expected =
        RelateNG::relate(geom_a.get(), geom_b.get());
    ASSERT_NE(expected, nullptr);
    EXPECT_EQ(im->toString(), expected->toString());

    RelateMatrixPredicate prepared_pred;
    std::unique_ptr<RelateNG> prepared = RelateNG::prepare(geom_a.get());
    EXPECT_FALSE(prepared->evaluate(geom_b.get(), prepared_pred));
    EXPECT_TRUE(prepared_pred.isKnown());
    EXPECT_FALSE(prepared_pred.value());
    EXPECT_EQ(prepared_pred.getIM()->toString(), tc.expected_im);
  }
}

TEST(RelateMatrixPredicateTest, EvaluatesWithBoundaryNodeRuleViaRelateNG) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  std::unique_ptr<Geometry> closed_line =
      reader.read("LINESTRING (0 0, 4 0, 4 4, 0 4, 0 0)");
  std::unique_ptr<Geometry> seam_point = reader.read("POINT (0 0)");

  RelateMatrixPredicate mod2_pred;
  EXPECT_FALSE(RelateNG::relate(closed_line.get(), seam_point.get(), mod2_pred,
                                BoundaryNodeRule::getBoundaryRuleMod2()));
  EXPECT_EQ(mod2_pred.getIM()->toString(), "0F1FFFFF2");

  RelateMatrixPredicate endpoint_pred;
  EXPECT_FALSE(RelateNG::relate(closed_line.get(), seam_point.get(),
                                endpoint_pred,
                                BoundaryNodeRule::getBoundaryEndPoint()));
  EXPECT_EQ(endpoint_pred.getIM()->toString(), "FF10FFFF2");
}

}  // namespace
}  // namespace geos_google
