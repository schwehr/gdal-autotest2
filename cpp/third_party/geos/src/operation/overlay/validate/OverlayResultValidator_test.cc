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
// third_party/geos/src/operation/overlay/validate/OverlayResultValidator.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/overlay/validate/OverlayResultValidator.cpp

#include "third_party/geos/include/geos/operation/overlay/validate/OverlayResultValidator.h"

#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayNG.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::io::WKTReader;
using geos::operation::overlay::validate::OverlayResultValidator;
using geos::operation::overlayng::OverlayNG;

static_assert(!std::is_copy_constructible_v<OverlayResultValidator>);
static_assert(!std::is_copy_assignable_v<OverlayResultValidator>);

constexpr int kIntersection = OverlayNG::INTERSECTION;
constexpr int kUnion = OverlayNG::UNION;
constexpr int kDifference = OverlayNG::DIFFERENCE;
constexpr int kSymDifference = OverlayNG::SYMDIFFERENCE;

class OverlayResultValidatorTest : public ::testing::Test {
 protected:
  OverlayResultValidatorTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  Geometry::Ptr ReadWkt(std::string_view wkt) const {
    return reader_.read(std::string(wkt));
  }

  Geometry::Ptr ComputeOverlay(const Geometry& g0, const Geometry& g1,
                               int op_code) const {
    switch (op_code) {
      case kIntersection:
        return g0.intersection(&g1);
      case kUnion:
        return g0.Union(&g1);
      case kDifference:
        return g0.difference(&g1);
      case kSymDifference:
        return g0.symDifference(&g1);
      default:
        return factory_->createPolygon();
    }
  }

  void ExpectAllFourOperationsValid(const Geometry& g0,
                                    const Geometry& g1) const {
    constexpr std::array<int, 4> kOps = {
        kIntersection,
        kUnion,
        kDifference,
        kSymDifference,
    };
    for (int op : kOps) {
      SCOPED_TRACE(op);
      Geometry::Ptr res = ComputeOverlay(g0, g1, op);
      ASSERT_NE(res, nullptr);
      EXPECT_TRUE(OverlayResultValidator::isValid(g0, g1, op, *res));

      OverlayResultValidator validator(g0, g1, *res);
      EXPECT_TRUE(validator.isValid(op));
    }
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(OverlayResultValidatorTest,
       ValidatesAllFourOperationsForOverlappingPolygons) {
  Geometry::Ptr g0 = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  Geometry::Ptr g1 = ReadWkt("POLYGON ((4 2, 14 2, 14 12, 4 12, 4 2))");
  ASSERT_NE(g0, nullptr);
  ASSERT_NE(g1, nullptr);

  ExpectAllFourOperationsValid(*g0, *g1);
}

TEST_F(OverlayResultValidatorTest, DetectsInvalidResultAndRecordsLocation) {
  Geometry::Ptr g0 = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  Geometry::Ptr g1 = ReadWkt("POLYGON ((5 0, 15 0, 15 10, 5 10, 5 0))");
  Geometry::Ptr wrong_intersection = g0->Union(g1.get());
  Geometry::Ptr wrong_union = g0->intersection(g1.get());
  Geometry::Ptr wrong_difference = g1->difference(g0.get());
  Geometry::Ptr wrong_symdiff = g0->Union(g1.get());

  struct InvalidCase {
    std::string_view name;
    int op_code;
    const Geometry* bad_result;
  };
  const std::array<InvalidCase, 4> kCases = {{
      {.name = "intersection_given_union",
       .op_code = kIntersection,
       .bad_result = wrong_intersection.get()},
      {.name = "union_given_intersection",
       .op_code = kUnion,
       .bad_result = wrong_union.get()},
      {.name = "difference_given_swapped_difference",
       .op_code = kDifference,
       .bad_result = wrong_difference.get()},
      {.name = "symdiff_given_union",
       .op_code = kSymDifference,
       .bad_result = wrong_symdiff.get()},
  }};

  for (const auto& c : kCases) {
    SCOPED_TRACE(c.name);
    EXPECT_FALSE(
        OverlayResultValidator::isValid(*g0, *g1, c.op_code, *c.bad_result));

    OverlayResultValidator validator(*g0, *g1, *c.bad_result);
    const Coordinate initial_loc = validator.getInvalidLocation();
    EXPECT_DOUBLE_EQ(initial_loc.x, 0.0);
    EXPECT_DOUBLE_EQ(initial_loc.y, 0.0);
    EXPECT_TRUE(std::isnan(initial_loc.z));

    EXPECT_FALSE(validator.isValid(c.op_code));
    const Coordinate invalid_loc = validator.getInvalidLocation();
    EXPECT_TRUE(std::isfinite(invalid_loc.x));
    EXPECT_TRUE(std::isfinite(invalid_loc.y));
    EXPECT_FALSE(invalid_loc.equals2D(Coordinate(0.0, 0.0)));
  }

  // Specifically verify the witness point for g0 passed as INTERSECTION.
  OverlayResultValidator inter_validator(*g0, *g1, *g0);
  EXPECT_FALSE(inter_validator.isValid(kIntersection));
  Geometry::Ptr witness =
      factory_->createPoint(inter_validator.getInvalidLocation());
  EXPECT_TRUE(g0->contains(witness.get()));
  EXPECT_TRUE(g1->disjoint(witness.get()));
}

TEST_F(OverlayResultValidatorTest,
       InvalidOpCodeReturnsFalseWhenResultNonEmpty) {
  Geometry::Ptr g0 = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  Geometry::Ptr g1 = ReadWkt("POLYGON ((4 2, 14 2, 14 12, 4 12, 4 2))");
  Geometry::Ptr union_res = g0->Union(g1.get());

  OverlayResultValidator validator(*g0, *g1, *union_res);
  EXPECT_FALSE(validator.isValid(99));
  EXPECT_TRUE(std::isfinite(validator.getInvalidLocation().x));
  EXPECT_TRUE(std::isfinite(validator.getInvalidLocation().y));
}

TEST_F(OverlayResultValidatorTest, ValidatesPolygonsWithHolesAndContainment) {
  Geometry::Ptr g0 = ReadWkt(
      "POLYGON ((0 0, 20 0, 20 20, 0 20, 0 0), "
      "(2 2, 2 6, 6 6, 6 2, 2 2))");
  Geometry::Ptr g1 = ReadWkt("POLYGON ((10 10, 16 10, 16 16, 10 16, 10 10))");

  ExpectAllFourOperationsValid(*g0, *g1);

  // Passing g0 without punching out g1 fails DIFFERENCE validation inside g1.
  OverlayResultValidator validator(*g0, *g1, *g0);
  EXPECT_FALSE(validator.isValid(kDifference));
  Geometry::Ptr witness = factory_->createPoint(validator.getInvalidLocation());
  EXPECT_TRUE(g1->contains(witness.get()));
}

TEST_F(OverlayResultValidatorTest, ValidatesMultiPolygonsWithAsymmetricScale) {
  Geometry::Ptr g0 = ReadWkt(
      "MULTIPOLYGON ("
      "((0 0, 1000 0, 1000 1000, 0 1000, 0 0)), "
      "((2000 0, 3000 0, 3000 1000, 2000 1000, 2000 0)))");
  Geometry::Ptr g1 =
      ReadWkt("POLYGON ((500 500, 2500 500, 2500 510, 500 510, 500 500))");

  ExpectAllFourOperationsValid(*g0, *g1);

  // g1 spans the (1000, 2000) gap, so using g1 as INTERSECTION fails.
  EXPECT_FALSE(OverlayResultValidator::isValid(*g0, *g1, kIntersection, *g1));
}

TEST_F(OverlayResultValidatorTest, SkipsTestPointsWithinBoundaryTolerance) {
  // g0 has min dimension 1000 -> tolerance = 1e-6 -> offset = 5e-6.
  // g1 has min dimension 1001 and bottom edge at y = 5e-6, so the offset point
  // (500, 5e-6) from g0's bottom segment lands on g1's boundary and hits the
  // BOUNDARY early-exit path in testValid.
  Geometry::Ptr g0 = ReadWkt("POLYGON ((0 0, 1000 0, 1000 1000, 0 1000, 0 0))");
  Geometry::Ptr g1 = ReadWkt(
      "POLYGON ((200 0.000005, 1201 0.000005, 1201 1001.000005, "
      "200 1001.000005, 200 0.000005))");

  Geometry::Ptr union_res = g0->Union(g1.get());
  OverlayResultValidator valid_union(*g0, *g1, *union_res);
  EXPECT_TRUE(valid_union.isValid(kUnion));
  EXPECT_DOUBLE_EQ(valid_union.getInvalidLocation().x, 0.0);
  EXPECT_DOUBLE_EQ(valid_union.getInvalidLocation().y, 0.0);

  Geometry::Ptr inter_res = g0->intersection(g1.get());
  EXPECT_FALSE(OverlayResultValidator::isValid(*g0, *g1, kUnion, *inter_res));
}

TEST_F(OverlayResultValidatorTest, HandlesDisjointTouchingEmptyAndPointInputs) {
  Geometry::Ptr left = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  Geometry::Ptr far_right =
      ReadWkt("POLYGON ((20 0, 30 0, 30 10, 20 10, 20 0))");
  ExpectAllFourOperationsValid(*left, *far_right);

  Geometry::Ptr touching_right =
      ReadWkt("POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0))");
  ExpectAllFourOperationsValid(*left, *touching_right);

  Geometry::Ptr empty_poly = ReadWkt("POLYGON EMPTY");
  EXPECT_TRUE(OverlayResultValidator::isValid(*empty_poly, *empty_poly, kUnion,
                                              *empty_poly));
  EXPECT_TRUE(
      OverlayResultValidator::isValid(*left, *empty_poly, kUnion, *left));

  Geometry::Ptr line = ReadWkt("LINESTRING (0 0, 10 0)");
  EXPECT_TRUE(OverlayResultValidator::isValid(*line, *left, kUnion, *left));

  Geometry::Ptr pt = ReadWkt("POINT (1 2)");
  EXPECT_TRUE(OverlayResultValidator::isValid(*pt, *pt, kIntersection, *pt));
}

TEST_F(OverlayResultValidatorTest,
       DetectsInvalidDisjointResultsAndLocatesWitness) {
  Geometry::Ptr left = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  Geometry::Ptr right = ReadWkt("POLYGON ((20 0, 30 0, 30 10, 20 10, 20 0))");
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  Geometry::Ptr both = left->Union(right.get());
  ASSERT_NE(both, nullptr);

  OverlayResultValidator inter_validator(*left, *right, *left);
  EXPECT_FALSE(inter_validator.isValid(kIntersection));
  Geometry::Ptr inter_witness =
      factory_->createPoint(inter_validator.getInvalidLocation());
  EXPECT_TRUE(left->contains(inter_witness.get()));
  EXPECT_TRUE(right->disjoint(inter_witness.get()));

  OverlayResultValidator diff_validator(*left, *right, *both);
  EXPECT_FALSE(diff_validator.isValid(kDifference));
  Geometry::Ptr diff_witness =
      factory_->createPoint(diff_validator.getInvalidLocation());
  EXPECT_TRUE(right->contains(diff_witness.get()));
  EXPECT_TRUE(left->disjoint(diff_witness.get()));
}

TEST_F(OverlayResultValidatorTest,
       ValidatesIslandInsideHoleAndAsymmetricToleranceOrder) {
  Geometry::Ptr island_poly = ReadWkt(
      "MULTIPOLYGON ("
      "((0 0, 20 0, 20 20, 0 20, 0 0), (2 2, 2 18, 18 18, 18 2, 2 2)), "
      "((6 6, 14 6, 14 14, 6 14, 6 6)))");
  Geometry::Ptr stripe = ReadWkt("POLYGON ((4 0, 16 0, 16 20, 4 20, 4 0))");
  ASSERT_NE(island_poly, nullptr);
  ASSERT_NE(stripe, nullptr);

  ExpectAllFourOperationsValid(*island_poly, *stripe);
  EXPECT_FALSE(
      OverlayResultValidator::isValid(*island_poly, *stripe, kUnion, *stripe));

  Geometry::Ptr small = ReadWkt("POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  Geometry::Ptr large = ReadWkt(
      "POLYGON ((0.5 0.5, 1000.5 0.5, 1000.5 1000.5, 0.5 1000.5, 0.5 0.5))");
  ASSERT_NE(small, nullptr);
  ASSERT_NE(large, nullptr);
  ExpectAllFourOperationsValid(*small, *large);
  ExpectAllFourOperationsValid(*large, *small);
  EXPECT_FALSE(
      OverlayResultValidator::isValid(*small, *large, kIntersection, *large));
  EXPECT_FALSE(
      OverlayResultValidator::isValid(*large, *small, kIntersection, *large));
}

TEST_F(OverlayResultValidatorTest, ReusesValidatorInstanceAcrossOperations) {
  Geometry::Ptr g0 = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  Geometry::Ptr g1 = ReadWkt("POLYGON ((5 0, 15 0, 15 10, 5 10, 5 0))");
  ASSERT_NE(g0, nullptr);
  ASSERT_NE(g1, nullptr);
  Geometry::Ptr inter = g0->intersection(g1.get());
  ASSERT_NE(inter, nullptr);

  OverlayResultValidator validator(*g0, *g1, *inter);
  EXPECT_TRUE(validator.isValid(kIntersection));
  EXPECT_DOUBLE_EQ(validator.getInvalidLocation().x, 0.0);
  EXPECT_DOUBLE_EQ(validator.getInvalidLocation().y, 0.0);

  EXPECT_FALSE(validator.isValid(kUnion));
  Geometry::Ptr witness = factory_->createPoint(validator.getInvalidLocation());
  EXPECT_TRUE(g0->contains(witness.get()) || g1->contains(witness.get()));
  EXPECT_TRUE(inter->disjoint(witness.get()));
}

TEST_F(OverlayResultValidatorTest,
       DetectsSpuriousResultComponentAndMissingResult) {
  Geometry::Ptr g0 = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  Geometry::Ptr g1 = ReadWkt("POLYGON ((20 0, 30 0, 30 10, 20 10, 20 0))");
  Geometry::Ptr spurious =
      ReadWkt("POLYGON ((40 0, 50 0, 50 10, 40 10, 40 0))");
  Geometry::Ptr with_spurious = ReadWkt(
      "MULTIPOLYGON ("
      "((0 0, 10 0, 10 10, 0 10, 0 0)), "
      "((20 0, 30 0, 30 10, 20 10, 20 0)), "
      "((40 0, 50 0, 50 10, 40 10, 40 0)))");
  Geometry::Ptr empty_poly = ReadWkt("POLYGON EMPTY");
  ASSERT_NE(g0, nullptr);
  ASSERT_NE(g1, nullptr);
  ASSERT_NE(spurious, nullptr);
  ASSERT_NE(with_spurious, nullptr);
  ASSERT_NE(empty_poly, nullptr);

  // A spurious component exists only in the result, so addTestPts(gres) must
  // supply the failing witness coordinate.
  OverlayResultValidator spurious_validator(*g0, *g1, *with_spurious);
  EXPECT_FALSE(spurious_validator.isValid(kUnion));
  Geometry::Ptr spurious_witness =
      factory_->createPoint(spurious_validator.getInvalidLocation());
  EXPECT_TRUE(spurious->contains(spurious_witness.get()));
  EXPECT_TRUE(g0->disjoint(spurious_witness.get()));
  EXPECT_TRUE(g1->disjoint(spurious_witness.get()));

  // A missing union component is detected via test points from the omitted
  // polygon (empty results hit FuzzyPointLocator's empty-linework BOUNDARY
  // heuristic when boundaryDistanceTolerance > 0).
  OverlayResultValidator incomplete_union_validator(*g0, *g1, *g0);
  EXPECT_FALSE(incomplete_union_validator.isValid(kUnion));
  Geometry::Ptr missing_witness =
      factory_->createPoint(incomplete_union_validator.getInvalidLocation());
  EXPECT_TRUE(g1->contains(missing_witness.get()));
  EXPECT_TRUE(g0->disjoint(missing_witness.get()));

  EXPECT_TRUE(
      OverlayResultValidator::isValid(*g0, *g1, kIntersection, *empty_poly));
}

TEST_F(OverlayResultValidatorTest, ValidatesPolygonsWithZCoordinates) {
  Geometry::Ptr g0 =
      ReadWkt("POLYGON Z ((0 0 1, 10 0 2, 10 10 3, 0 10 4, 0 0 1))");
  Geometry::Ptr g1 =
      ReadWkt("POLYGON Z ((5 2 10, 15 2 20, 15 12 30, 5 12 40, 5 2 10))");
  ASSERT_NE(g0, nullptr);
  ASSERT_NE(g1, nullptr);

  ExpectAllFourOperationsValid(*g0, *g1);
  EXPECT_FALSE(OverlayResultValidator::isValid(*g0, *g1, kIntersection, *g0));
}

template <typename Tag, typename Tag::type M>
struct OverlayResultValidatorPrivateAccess {
  friend typename Tag::type get(Tag) { return M; }
};

struct AddVerticesTag {
  using type = void (OverlayResultValidator::*)(const Geometry&);
  friend type get(AddVerticesTag);
};

struct TestValidTag {
  using type = bool (OverlayResultValidator::*)(int);
  friend type get(TestValidTag);
};

template struct OverlayResultValidatorPrivateAccess<
    AddVerticesTag, &OverlayResultValidator::addVertices>;
template struct OverlayResultValidatorPrivateAccess<
    TestValidTag, &OverlayResultValidator::testValid>;

TEST_F(OverlayResultValidatorTest, AddVerticesAppendsCoordinatesForValidation) {
  Geometry::Ptr g0 = ReadWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  Geometry::Ptr g1 = ReadWkt("POLYGON ((20 0, 30 0, 30 10, 20 10, 20 0))");
  Geometry::Ptr witness_pt = ReadWkt("POINT (25 5)");
  ASSERT_NE(g0, nullptr);
  ASSERT_NE(g1, nullptr);
  ASSERT_NE(witness_pt, nullptr);

  // Validate incomplete union (*g0 missing *g1) using only addVertices() +
  // testValid() so the direct coordinate-extraction path is verified.
  OverlayResultValidator validator(*g0, *g1, *g0);
  (validator.*get(AddVerticesTag{}))(*witness_pt);
  EXPECT_FALSE((validator.*get(TestValidTag{}))(kUnion));
  EXPECT_DOUBLE_EQ(validator.getInvalidLocation().x, 25.0);
  EXPECT_DOUBLE_EQ(validator.getInvalidLocation().y, 5.0);
}

}  // namespace
}  // namespace geos_google
