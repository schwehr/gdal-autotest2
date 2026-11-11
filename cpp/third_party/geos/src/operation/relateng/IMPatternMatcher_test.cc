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

// Unit tests for third_party/geos/src/operation/relateng/IMPatternMatcher.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/relateng/IMPatternMatcher.cpp

#include "third_party/geos/include/geos/operation/relateng/IMPatternMatcher.h"

#include <array>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Dimension.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/relateng/RelateNG.h"
#include "third_party/geos/include/geos/operation/relateng/RelatePredicate.h"
#include "third_party/geos/include/geos/operation/relateng/TopologyPredicate.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Dimension;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::io::WKTReader;
using geos::operation::relateng::IMPatternMatcher;
using geos::operation::relateng::RelateNG;
using geos::operation::relateng::RelatePredicate;
using geos::operation::relateng::TopologyPredicate;
using geos::util::IllegalArgumentException;

TEST(IMPatternMatcherTest, NameToStringAndStreamInsertion) {
  IMPatternMatcher matcher("T*F**FFF*");
  EXPECT_EQ(matcher.name(), "IMPattern");
  EXPECT_EQ(matcher.toString(), "IMPattern(T*F**FFF*)");

  std::ostringstream oss;
  oss << matcher;
  EXPECT_EQ(oss.str(), "IMPattern(T*F**FFF*)");
}

TEST(IMPatternMatcherTest, InvalidPatternThrowsIllegalArgumentException) {
  EXPECT_THROW(IMPatternMatcher("T*X******"), IllegalArgumentException);

  // Wrong-length patterns construct, then fail when matching against intMatrix.
  IMPatternMatcher short_matcher("T*F");
  EXPECT_THROW(short_matcher.valueIM(), IllegalArgumentException);

  IMPatternMatcher long_matcher("TTTTTTTTTT");
  EXPECT_THROW(long_matcher.finish(), IllegalArgumentException);
}

TEST(IMPatternMatcherTest, RequireInteractionChecksInnerFourEntries) {
  struct PatternInteractionCase {
    std::string_view pattern;
    bool expected_require_interaction;
  };
  constexpr auto kCases = std::to_array<PatternInteractionCase>({
      // Interior/Interior
      {.pattern = "T********", .expected_require_interaction = true},
      {.pattern = "0********", .expected_require_interaction = true},
      {.pattern = "1********", .expected_require_interaction = true},
      {.pattern = "2********", .expected_require_interaction = true},
      // Interior/Boundary
      {.pattern = "FT*******", .expected_require_interaction = true},
      {.pattern = "F0*******", .expected_require_interaction = true},
      {.pattern = "F1*******", .expected_require_interaction = true},
      {.pattern = "F2*******", .expected_require_interaction = true},
      // Boundary/Interior
      {.pattern = "FF*T*****", .expected_require_interaction = true},
      {.pattern = "FF*0*****", .expected_require_interaction = true},
      // Boundary/Boundary
      {.pattern = "FF*FT****", .expected_require_interaction = true},
      {.pattern = "FF*F2****", .expected_require_interaction = true},
      // No non-E/non-E interaction
      {.pattern = "FF*FF****", .expected_require_interaction = false},
      {.pattern = "*********", .expected_require_interaction = false},
      {.pattern = "**F******", .expected_require_interaction = false},
      {.pattern = "**T**T***", .expected_require_interaction = false},
      {.pattern = "FF*FF*210", .expected_require_interaction = false},
      {.pattern = "******T**", .expected_require_interaction = false},
      {.pattern = "*******T*", .expected_require_interaction = false},
      {.pattern = "********T", .expected_require_interaction = false},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.pattern);
    IMPatternMatcher matcher{std::string(tc.pattern)};
    EXPECT_EQ(matcher.requireInteraction(), tc.expected_require_interaction);
  }
}

TEST(IMPatternMatcherTest,
     InitEnvelopeShortCircuitsDisjointInteractivePatterns) {
  const Envelope env_a(0.0, 2.0, 0.0, 2.0);
  const Envelope env_b_disjoint(5.0, 6.0, 5.0, 6.0);
  const Envelope env_b_touching(2.0, 4.0, 0.0, 2.0);
  const Envelope env_b_overlapping(1.0, 3.0, 1.0, 3.0);
  const Envelope env_null;

  {
    IMPatternMatcher matcher("T********");
    matcher.init(Dimension::A, Dimension::A);
    matcher.init(env_a, env_b_disjoint);
    EXPECT_TRUE(matcher.isKnown());
    EXPECT_FALSE(matcher.value());
  }
  {
    IMPatternMatcher matcher("FF*FT****");
    matcher.init(Dimension::A, Dimension::A);
    matcher.init(env_a, env_null);
    EXPECT_TRUE(matcher.isKnown());
    EXPECT_FALSE(matcher.value());
  }
  {
    IMPatternMatcher matcher("T********");
    matcher.init(Dimension::A, Dimension::A);
    matcher.init(env_a, env_b_touching);
    EXPECT_FALSE(matcher.isKnown());
  }
  {
    IMPatternMatcher matcher("T********");
    matcher.init(Dimension::A, Dimension::A);
    matcher.init(env_a, env_b_overlapping);
    EXPECT_FALSE(matcher.isKnown());
  }
  {
    IMPatternMatcher matcher("FF*FF****");
    matcher.init(Dimension::A, Dimension::A);
    matcher.init(env_a, env_b_disjoint);
    EXPECT_FALSE(matcher.isKnown());
  }
  {
    IMPatternMatcher matcher("FF*FF****");
    matcher.init(Dimension::A, Dimension::A);
    matcher.init(env_a, env_b_overlapping);
    EXPECT_FALSE(matcher.isKnown());
  }
}

TEST(IMPatternMatcherTest, IsDeterminedHandlesDontCareTrueAndNumericBounds) {
  // All DONTCARE ('*'): never short-circuits.
  {
    IMPatternMatcher matcher("*********");
    EXPECT_FALSE(matcher.isDetermined());
    matcher.updateDimension(Location::INTERIOR, Location::INTERIOR,
                            Dimension::A);
    EXPECT_FALSE(matcher.isDetermined());
    EXPECT_FALSE(matcher.isKnown());
    matcher.finish();
    EXPECT_TRUE(matcher.isKnown());
    EXPECT_TRUE(matcher.value());
  }

  // Mask 'T' with unresolved entry (< 0) returns false even if a later entry
  // exceeds mask.
  {
    IMPatternMatcher matcher("T*F******");
    EXPECT_FALSE(matcher.isDetermined());
    matcher.updateDimension(Location::INTERIOR, Location::EXTERIOR,
                            Dimension::P);
    EXPECT_FALSE(matcher.isDetermined());
    EXPECT_FALSE(matcher.isKnown());

    // Once (I, I) becomes >= 0, (I, E) > False causes isDetermined() to become
    // true.
    matcher.updateDimension(Location::INTERIOR, Location::INTERIOR,
                            Dimension::P);
    EXPECT_TRUE(matcher.isDetermined());
    EXPECT_TRUE(matcher.isKnown());
    EXPECT_FALSE(matcher.value());
  }

  // Mask 'T' satisfied without any exceeded entry stays undetermined until
  // finish().
  {
    IMPatternMatcher matcher("T*F******");
    matcher.updateDimension(Location::INTERIOR, Location::INTERIOR,
                            Dimension::L);
    EXPECT_FALSE(matcher.isDetermined());
    EXPECT_FALSE(matcher.isKnown());
    matcher.finish();
    EXPECT_TRUE(matcher.isKnown());
    EXPECT_TRUE(matcher.value());
  }

  // Numeric mask entries short-circuit once exceeded (matrixVal >
  // patternEntry).
  {
    IMPatternMatcher matcher("0********");
    matcher.updateDimension(Location::INTERIOR, Location::INTERIOR,
                            Dimension::P);
    EXPECT_FALSE(matcher.isDetermined());
    EXPECT_FALSE(matcher.isKnown());

    matcher.updateDimension(Location::INTERIOR, Location::INTERIOR,
                            Dimension::L);
    EXPECT_TRUE(matcher.isDetermined());
    EXPECT_TRUE(matcher.isKnown());
    EXPECT_FALSE(matcher.value());
  }
  {
    IMPatternMatcher matcher("1********");
    matcher.updateDimension(Location::INTERIOR, Location::INTERIOR,
                            Dimension::L);
    EXPECT_FALSE(matcher.isDetermined());

    matcher.updateDimension(Location::INTERIOR, Location::INTERIOR,
                            Dimension::A);
    EXPECT_TRUE(matcher.isDetermined());
    EXPECT_TRUE(matcher.isKnown());
    EXPECT_FALSE(matcher.value());
  }
  {
    IMPatternMatcher matcher("2********");
    matcher.updateDimension(Location::INTERIOR, Location::INTERIOR,
                            Dimension::A);
    EXPECT_FALSE(matcher.isDetermined());
    EXPECT_FALSE(matcher.isKnown());
  }
  {
    IMPatternMatcher matcher("FF*FF****");
    matcher.updateDimension(Location::BOUNDARY, Location::BOUNDARY,
                            Dimension::P);
    EXPECT_TRUE(matcher.isDetermined());
    EXPECT_TRUE(matcher.isKnown());
    EXPECT_FALSE(matcher.value());
  }

  // E/E starts at Dimension::A (2), so patterns disallowing E/E=2 are
  // immediately determined.
  {
    IMPatternMatcher matcher_false_ee("********F");
    EXPECT_TRUE(matcher_false_ee.isDetermined());
    EXPECT_FALSE(matcher_false_ee.valueIM());
  }
  {
    IMPatternMatcher matcher_line_ee("********1");
    EXPECT_TRUE(matcher_line_ee.isDetermined());
    EXPECT_FALSE(matcher_line_ee.valueIM());
  }
  {
    IMPatternMatcher matcher_area_ee("********2");
    EXPECT_FALSE(matcher_area_ee.isDetermined());
    EXPECT_TRUE(matcher_area_ee.valueIM());
  }
  {
    IMPatternMatcher matcher_true_ee("********T");
    EXPECT_FALSE(matcher_true_ee.isDetermined());
    EXPECT_TRUE(matcher_true_ee.valueIM());
  }
}

TEST(IMPatternMatcherTest, ValueIMMatchesFullMatrix) {
  IMPatternMatcher matcher("0FF1FFFF2");
  matcher.updateDimension(Location::INTERIOR, Location::INTERIOR, Dimension::P);
  EXPECT_FALSE(matcher.valueIM());

  matcher.updateDimension(Location::BOUNDARY, Location::INTERIOR, Dimension::L);
  EXPECT_TRUE(matcher.valueIM());
  matcher.finish();
  EXPECT_TRUE(matcher.isKnown());
  EXPECT_TRUE(matcher.value());
}

TEST(IMPatternMatcherTest, RelateNGIntegrationMatchesPatterns) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  std::unique_ptr<Geometry> poly =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  std::unique_ptr<Geometry> l_poly =
      reader.read("POLYGON ((0 0, 10 0, 10 2, 2 2, 2 10, 0 10, 0 0))");
  std::unique_ptr<Geometry> pt_in = reader.read("POINT (5 5)");
  std::unique_ptr<Geometry> pt_boundary = reader.read("POINT (0 5)");
  std::unique_ptr<Geometry> pt_out = reader.read("POINT (20 20)");
  std::unique_ptr<Geometry> pt_notch = reader.read("POINT (8 8)");
  std::unique_ptr<Geometry> line_cross = reader.read("LINESTRING (-5 5, 15 5)");
  std::unique_ptr<Geometry> empty_pt = reader.read("POINT EMPTY");
  std::unique_ptr<Geometry> empty_poly = reader.read("POLYGON EMPTY");

  EXPECT_TRUE(RelateNG::relate(poly.get(), pt_in.get(), "0F2FF1FF2"));
  EXPECT_TRUE(RelateNG::relate(pt_in.get(), poly.get(), "0FFFFF212"));
  EXPECT_FALSE(RelateNG::relate(poly.get(), pt_in.get(), "0FFFFF212"));
  EXPECT_TRUE(RelateNG::relate(poly.get(), pt_in.get(), "T*****FF*"));
  EXPECT_FALSE(RelateNG::relate(poly.get(), pt_in.get(), "FF*FF****"));

  EXPECT_TRUE(RelateNG::relate(poly.get(), pt_boundary.get(), "FF20F1FF2"));
  EXPECT_TRUE(RelateNG::relate(poly.get(), pt_boundary.get(), "***0*****"));
  EXPECT_FALSE(RelateNG::relate(poly.get(), pt_boundary.get(), "T********"));

  EXPECT_TRUE(RelateNG::relate(poly.get(), pt_out.get(), "FF2FF10F2"));
  EXPECT_TRUE(RelateNG::relate(poly.get(), pt_out.get(), "FF*FF****"));
  EXPECT_FALSE(RelateNG::relate(poly.get(), pt_out.get(), "T********"));

  // Overlapping envelopes with disjoint geometries exercise init(envA, envB).
  EXPECT_TRUE(RelateNG::relate(l_poly.get(), pt_notch.get(), "FF2FF10F2"));
  EXPECT_TRUE(RelateNG::relate(l_poly.get(), pt_notch.get(), "FF*FF****"));
  EXPECT_FALSE(RelateNG::relate(l_poly.get(), pt_notch.get(), "T********"));

  EXPECT_TRUE(RelateNG::relate(line_cross.get(), poly.get(), "101FF0212"));
  EXPECT_TRUE(RelateNG::relate(poly.get(), line_cross.get(), "1F20F1102"));
  EXPECT_TRUE(RelateNG::relate(line_cross.get(), poly.get(), "T*T******"));

  EXPECT_TRUE(RelateNG::relate(poly.get(), poly.get(), "2FFF1FFF2"));
  EXPECT_TRUE(RelateNG::relate(poly.get(), poly.get(), "T*F**FFF*"));

  EXPECT_TRUE(RelateNG::relate(empty_pt.get(), empty_poly.get(), "FFFFFFFF2"));
  EXPECT_FALSE(RelateNG::relate(empty_pt.get(), empty_poly.get(), "T********"));
  EXPECT_FALSE(RelateNG::relate(poly.get(), pt_in.get(), "********F"));

  std::unique_ptr<TopologyPredicate> pred =
      RelatePredicate::matches("0F2FF1FF2");
  EXPECT_EQ(pred->name(), "IMPattern");
  EXPECT_TRUE(RelateNG::relate(poly.get(), pt_in.get(), *pred));
  EXPECT_TRUE(pred->isKnown());
  EXPECT_TRUE(pred->value());

  std::unique_ptr<TopologyPredicate> pred_mismatch =
      RelatePredicate::matches("0FFFFF212");
  EXPECT_FALSE(RelateNG::relate(poly.get(), pt_in.get(), *pred_mismatch));
  EXPECT_TRUE(pred_mismatch->isKnown());
  EXPECT_FALSE(pred_mismatch->value());

  std::unique_ptr<RelateNG> prepared = RelateNG::prepare(poly.get());
  EXPECT_TRUE(prepared->evaluate(pt_in.get(), "0F2FF1FF2"));
  EXPECT_TRUE(prepared->evaluate(pt_out.get(), "FF2FF10F2"));
  EXPECT_TRUE(prepared->relate(line_cross.get(), "1F20F1102"));

  EXPECT_THROW(RelateNG::relate(poly.get(), pt_in.get(), "T*X******"),
               IllegalArgumentException);
}

}  // namespace
}  // namespace geos_google
