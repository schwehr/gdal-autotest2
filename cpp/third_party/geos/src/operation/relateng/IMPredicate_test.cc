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

// Unit tests for third_party/geos/src/operation/relateng/IMPredicate.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/relateng/IMPredicate.cpp

#include "third_party/geos/include/geos/operation/relateng/IMPredicate.h"

#include <array>
#include <memory>
#include <sstream>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/relateng/RelateNG.h"
#include "third_party/geos/include/geos/operation/relateng/RelatePredicate.h"

namespace geos_google {
namespace {

using geos::geom::Dimension;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::io::WKTReader;
using geos::operation::relateng::IMPredicate;
using geos::operation::relateng::RelateNG;
using geos::operation::relateng::RelatePredicate;

class StubIMPredicate final : public IMPredicate {
 public:
  explicit StubIMPredicate(bool determined = false, bool im_value = true)
      : determined_(determined), im_value_(im_value) {}

  std::string name() const override { return "StubIM"; }

  void setDetermined(bool determined) { determined_ = determined; }
  void setValueIM(bool val) { im_value_ = val; }

  int getDimA() const { return dimA; }
  int getDimB() const { return dimB; }
  static int getDimUnknown() { return +DIM_UNKNOWN; }

  void setMatrixAll(int dim) { intMatrix.setAll(dim); }

  bool testIntersectsExteriorOf(bool isA) const {
    return intersectsExteriorOf(isA);
  }

  bool testIsIntersects(Location locA, Location locB) const {
    return isIntersects(locA, locB);
  }

 protected:
  bool valueIM() override { return im_value_; }
  bool isDetermined() const override { return determined_; }

 private:
  bool determined_;
  bool im_value_;
};

TEST(IMPredicateTest, DefaultConstructorInitializesMatrix) {
  StubIMPredicate pred;
  EXPECT_FALSE(pred.isKnown());
  EXPECT_EQ(pred.getDimension(Location::EXTERIOR, Location::EXTERIOR),
            Dimension::A);
  EXPECT_TRUE(
      pred.isDimension(Location::EXTERIOR, Location::EXTERIOR, Dimension::A));

  constexpr auto kLocs = std::to_array<Location>({
      Location::INTERIOR,
      Location::BOUNDARY,
      Location::EXTERIOR,
  });
  for (Location locA : kLocs) {
    for (Location locB : kLocs) {
      if (locA == Location::EXTERIOR && locB == Location::EXTERIOR) {
        continue;
      }
      EXPECT_EQ(pred.getDimension(locA, locB), Dimension::False);
      EXPECT_TRUE(pred.isDimension(locA, locB, Dimension::False));
      // Default entries are Dimension::False (-1), distinct from DIM_UNKNOWN
      // (-3).
      EXPECT_TRUE(pred.isKnown(locA, locB));
    }
  }
  EXPECT_EQ(pred.toString(), "StubIM: FFFFFFFF2");
}

TEST(IMPredicateTest, IsDimsCompatibleWithCovers) {
  struct DimPairCase {
    int dim0;
    int dim1;
    bool expected;
  };
  constexpr auto kCases = std::to_array<DimPairCase>({
      // Zero-length lines are treated as points, so P can cover L.
      {.dim0 = Dimension::P, .dim1 = Dimension::L, .expected = true},
      {.dim0 = Dimension::False, .dim1 = Dimension::False, .expected = true},
      {.dim0 = Dimension::P, .dim1 = Dimension::False, .expected = true},
      {.dim0 = Dimension::P, .dim1 = Dimension::P, .expected = true},
      {.dim0 = Dimension::L, .dim1 = Dimension::P, .expected = true},
      {.dim0 = Dimension::L, .dim1 = Dimension::L, .expected = true},
      {.dim0 = Dimension::A, .dim1 = Dimension::False, .expected = true},
      {.dim0 = Dimension::A, .dim1 = Dimension::P, .expected = true},
      {.dim0 = Dimension::A, .dim1 = Dimension::L, .expected = true},
      {.dim0 = Dimension::A, .dim1 = Dimension::A, .expected = true},
      {.dim0 = Dimension::False, .dim1 = Dimension::P, .expected = false},
      {.dim0 = Dimension::False, .dim1 = Dimension::L, .expected = false},
      {.dim0 = Dimension::False, .dim1 = Dimension::A, .expected = false},
      {.dim0 = Dimension::P, .dim1 = Dimension::A, .expected = false},
      {.dim0 = Dimension::L, .dim1 = Dimension::A, .expected = false},
  });

  for (const auto& tc : kCases) {
    EXPECT_EQ(IMPredicate::isDimsCompatibleWithCovers(tc.dim0, tc.dim1),
              tc.expected)
        << "dim0=" << tc.dim0 << ", dim1=" << tc.dim1;
  }
}

TEST(IMPredicateTest, InitSetsDimensions) {
  StubIMPredicate pred;
  pred.init(Dimension::P, Dimension::A);
  EXPECT_EQ(pred.getDimA(), Dimension::P);
  EXPECT_EQ(pred.getDimB(), Dimension::A);

  pred.init(Dimension::False, Dimension::L);
  EXPECT_EQ(pred.getDimA(), Dimension::False);
  EXPECT_EQ(pred.getDimB(), Dimension::L);
}

TEST(IMPredicateTest, IsDimChangedAndUpdateDimensionMonotonic) {
  StubIMPredicate pred(/*determined=*/true, /*im_value=*/false);

  // Initial (I, I) is Dimension::False (-1); non-increasing values do not
  // change it.
  EXPECT_FALSE(pred.isDimChanged(Location::INTERIOR, Location::INTERIOR,
                                 Dimension::DONTCARE));
  EXPECT_FALSE(pred.isDimChanged(Location::INTERIOR, Location::INTERIOR,
                                 Dimension::True));
  EXPECT_FALSE(pred.isDimChanged(Location::INTERIOR, Location::INTERIOR,
                                 Dimension::False));
  EXPECT_TRUE(
      pred.isDimChanged(Location::INTERIOR, Location::INTERIOR, Dimension::P));

  // Updating with a non-increasing value must not trigger
  // isDetermined()/setValue().
  pred.updateDimension(Location::INTERIOR, Location::INTERIOR,
                       Dimension::False);
  EXPECT_FALSE(pred.isKnown());
  EXPECT_EQ(pred.getDimension(Location::INTERIOR, Location::INTERIOR),
            Dimension::False);

  // Updating while isDetermined() is false updates the matrix without
  // finalizing.
  pred.setDetermined(false);
  pred.updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::L);
  EXPECT_FALSE(pred.isKnown());
  EXPECT_EQ(pred.getDimension(Location::INTERIOR, Location::INTERIOR),
            Dimension::L);
  EXPECT_TRUE(
      pred.isDimension(Location::INTERIOR, Location::INTERIOR, Dimension::L));
  EXPECT_FALSE(
      pred.isDimension(Location::INTERIOR, Location::INTERIOR, Dimension::P));

  // Decreasing or equal updates do not regress the entry.
  pred.setDetermined(true);
  pred.updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::P);
  pred.updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::L);
  EXPECT_EQ(pred.getDimension(Location::INTERIOR, Location::INTERIOR),
            Dimension::L);
  EXPECT_FALSE(pred.isKnown());

  // Increasing when isDetermined() is true updates the matrix and sets value.
  pred.setValueIM(true);
  pred.updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::A);
  EXPECT_EQ(pred.getDimension(Location::INTERIOR, Location::INTERIOR),
            Dimension::A);
  EXPECT_TRUE(pred.isKnown());
  EXPECT_TRUE(pred.value());
}

TEST(IMPredicateTest, IsKnownTwoArgChecksDimUnknown) {
  StubIMPredicate pred;
  const int dim_unknown = StubIMPredicate::getDimUnknown();
  EXPECT_EQ(dim_unknown, Dimension::DONTCARE);

  pred.setMatrixAll(dim_unknown);
  EXPECT_FALSE(pred.isKnown(Location::INTERIOR, Location::INTERIOR));
  EXPECT_FALSE(pred.isKnown(Location::BOUNDARY, Location::EXTERIOR));

  pred.updateDimension(Location::INTERIOR, Location::INTERIOR,
                       Dimension::False);
  EXPECT_TRUE(pred.isKnown(Location::INTERIOR, Location::INTERIOR));
  EXPECT_FALSE(pred.isKnown(Location::BOUNDARY, Location::EXTERIOR));

  pred.updateDimension(Location::BOUNDARY, Location::EXTERIOR, Dimension::P);
  EXPECT_TRUE(pred.isKnown(Location::BOUNDARY, Location::EXTERIOR));
}

TEST(IMPredicateTest, IsIntersectsChecksDimensionAtLeastPoint) {
  StubIMPredicate pred;
  EXPECT_TRUE(pred.testIsIntersects(Location::EXTERIOR, Location::EXTERIOR));
  EXPECT_FALSE(pred.testIsIntersects(Location::INTERIOR, Location::INTERIOR));

  pred.setMatrixAll(Dimension::DONTCARE);
  EXPECT_FALSE(pred.testIsIntersects(Location::INTERIOR, Location::INTERIOR));

  pred.updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::True);
  EXPECT_FALSE(pred.testIsIntersects(Location::INTERIOR, Location::INTERIOR));

  pred.updateDimension(Location::INTERIOR, Location::INTERIOR,
                       Dimension::False);
  EXPECT_FALSE(pred.testIsIntersects(Location::INTERIOR, Location::INTERIOR));

  pred.updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::P);
  EXPECT_TRUE(pred.testIsIntersects(Location::INTERIOR, Location::INTERIOR));

  pred.updateDimension(Location::BOUNDARY, Location::BOUNDARY, Dimension::L);
  EXPECT_TRUE(pred.testIsIntersects(Location::BOUNDARY, Location::BOUNDARY));

  pred.updateDimension(Location::EXTERIOR, Location::EXTERIOR, Dimension::A);
  EXPECT_TRUE(pred.testIsIntersects(Location::EXTERIOR, Location::EXTERIOR));
}

TEST(IMPredicateTest, IntersectsExteriorOfChecksCorrectMatrixSlice) {
  {
    StubIMPredicate pred;
    EXPECT_FALSE(pred.testIntersectsExteriorOf(/*isA=*/true));
    EXPECT_FALSE(pred.testIntersectsExteriorOf(/*isA=*/false));
  }
  {
    StubIMPredicate pred;
    pred.updateDimension(Location::EXTERIOR, Location::INTERIOR, Dimension::P);
    EXPECT_TRUE(pred.testIntersectsExteriorOf(/*isA=*/true));
    EXPECT_FALSE(pred.testIntersectsExteriorOf(/*isA=*/false));
  }
  {
    StubIMPredicate pred;
    pred.updateDimension(Location::EXTERIOR, Location::BOUNDARY, Dimension::L);
    EXPECT_TRUE(pred.testIntersectsExteriorOf(/*isA=*/true));
    EXPECT_FALSE(pred.testIntersectsExteriorOf(/*isA=*/false));
  }
  {
    StubIMPredicate pred;
    pred.updateDimension(Location::INTERIOR, Location::EXTERIOR, Dimension::A);
    EXPECT_FALSE(pred.testIntersectsExteriorOf(/*isA=*/true));
    EXPECT_TRUE(pred.testIntersectsExteriorOf(/*isA=*/false));
  }
  {
    StubIMPredicate pred;
    pred.updateDimension(Location::BOUNDARY, Location::EXTERIOR, Dimension::P);
    EXPECT_FALSE(pred.testIntersectsExteriorOf(/*isA=*/true));
    EXPECT_TRUE(pred.testIntersectsExteriorOf(/*isA=*/false));
  }
}

TEST(IMPredicateTest, FinishSetsValueAndDoesNotOverwriteKnownValue) {
  {
    StubIMPredicate pred(/*determined=*/false, /*im_value=*/true);
    EXPECT_FALSE(pred.isKnown());
    pred.finish();
    EXPECT_TRUE(pred.isKnown());
    EXPECT_TRUE(pred.value());
  }
  {
    StubIMPredicate pred(/*determined=*/false, /*im_value=*/false);
    pred.finish();
    EXPECT_TRUE(pred.isKnown());
    EXPECT_FALSE(pred.value());
  }
  {
    StubIMPredicate pred(/*determined=*/true, /*im_value=*/false);
    pred.updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::P);
    ASSERT_TRUE(pred.isKnown());
    EXPECT_FALSE(pred.value());

    // Once known, finish() does not overwrite the short-circuited value.
    pred.setValueIM(true);
    pred.finish();
    EXPECT_FALSE(pred.value());
  }
}

TEST(IMPredicateTest, ToStringAndStreamInsertionFormatMatrix) {
  StubIMPredicate pred;
  pred.updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::P);
  pred.updateDimension(Location::BOUNDARY, Location::INTERIOR, Dimension::L);
  EXPECT_EQ(pred.toString(), "StubIM: 0FF1FFFF2");

  std::ostringstream oss;
  oss << pred;
  EXPECT_EQ(oss.str(), "StubIM: 0FF1FFFF2 0FF1FFFF2");
}

TEST(IMPredicateTest, ConcreteRelatePredicatesEvaluateViaRelateNG) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  std::unique_ptr<Geometry> poly =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  std::unique_ptr<Geometry> pt_in = reader.read("POINT (5 5)");
  std::unique_ptr<Geometry> pt_bdy = reader.read("POINT (0 5)");
  std::unique_ptr<Geometry> zero_len_line =
      reader.read("LINESTRING (1 1, 1 1)");
  std::unique_ptr<Geometry> pt_on_zero_line = reader.read("POINT (1 1)");

  auto contains_pred = RelatePredicate::contains();
  EXPECT_TRUE(RelateNG::relate(poly.get(), pt_in.get(), *contains_pred));
  EXPECT_EQ(contains_pred->getDimension(Location::INTERIOR, Location::INTERIOR),
            Dimension::P);

  auto covers_pred = RelatePredicate::covers();
  EXPECT_TRUE(RelateNG::relate(poly.get(), pt_bdy.get(), *covers_pred));

  auto covered_by_zero = RelatePredicate::coveredBy();
  EXPECT_TRUE(RelateNG::relate(zero_len_line.get(), pt_on_zero_line.get(),
                               *covered_by_zero));

  auto zero_covers_pt = RelatePredicate::covers();
  EXPECT_TRUE(RelateNG::relate(pt_on_zero_line.get(), zero_len_line.get(),
                               *zero_covers_pt));
}

}  // namespace
}  // namespace geos_google
