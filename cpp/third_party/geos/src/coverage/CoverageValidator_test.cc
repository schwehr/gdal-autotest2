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

// Unit tests for third_party/geos/src/coverage/CoverageValidator.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for
//   UnsupportedOperationException assertions.
// - Allocates isolated GeometryFactory instances via GeometryFactory::create().
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/coverage/CoverageValidator.cpp

#include "third_party/geos/include/geos/coverage/CoverageValidator.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::coverage::CoverageValidator;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::io::WKTReader;
using geos::util::UnsupportedOperationException;

std::vector<std::unique_ptr<Geometry>> ReadAllWkt(
    const WKTReader& reader, const std::vector<std::string_view>& wkts) {
  std::vector<std::unique_ptr<Geometry>> geoms;
  geoms.reserve(wkts.size());
  for (std::string_view wkt : wkts) {
    geoms.push_back(reader.read(std::string(wkt)));
  }
  return geoms;
}

std::vector<const Geometry*> AsRawPtrs(
    const std::vector<std::unique_ptr<Geometry>>& geoms) {
  std::vector<const Geometry*> ptrs;
  ptrs.reserve(geoms.size());
  for (const auto& g : geoms) {
    ptrs.push_back(g.get());
  }
  return ptrs;
}

void ExpectNormalizedExact(const Geometry* actual, const Geometry* expected,
                           double tolerance = 1e-12) {
  ASSERT_NE(actual, nullptr);
  ASSERT_NE(expected, nullptr);
  std::unique_ptr<Geometry> norm_actual = actual->clone();
  std::unique_ptr<Geometry> norm_expected = expected->clone();
  norm_actual->normalize();
  norm_expected->normalize();
  EXPECT_TRUE(norm_actual->equalsExact(norm_expected.get(), tolerance))
      << "actual: " << norm_actual->toText()
      << "\nexpected: " << norm_expected->toText();
}

TEST(CoverageValidatorTest, EmptyAndNonPolygonalNonCurvedInputs) {
  std::vector<const Geometry*> empty_cov;
  EXPECT_TRUE(CoverageValidator::isValid(empty_cov));
  EXPECT_TRUE(CoverageValidator::isValid(empty_cov, 1.0));
  EXPECT_TRUE(CoverageValidator::validate(empty_cov).empty());
  EXPECT_TRUE(CoverageValidator::validate(empty_cov, 1.0).empty());

  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // POLYGON EMPTY has a null envelope so it is skipped by the STRtree index
  // and produces a nullptr validation entry.
  std::vector<std::unique_ptr<Geometry>> with_empty =
      ReadAllWkt(reader, {
                             "POLYGON EMPTY",
                             "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))",
                         });
  std::vector<const Geometry*> with_empty_ptrs = AsRawPtrs(with_empty);
  EXPECT_TRUE(CoverageValidator::isValid(with_empty_ptrs));
  std::vector<std::unique_ptr<Geometry>> with_empty_res =
      CoverageValidator::validate(with_empty_ptrs);
  ASSERT_EQ(with_empty_res.size(), 2u);
  EXPECT_EQ(with_empty_res[0], nullptr);
  EXPECT_EQ(with_empty_res[1], nullptr);
  EXPECT_FALSE(CoverageValidator::hasInvalidResult(with_empty_res));

  // Non-polygonal, non-curved geometries yield zero extracted polygon rings.
  std::vector<std::unique_ptr<Geometry>> non_poly =
      ReadAllWkt(reader, {
                             "POINT (0 0)",
                             "LINESTRING (0 0, 2 2)",
                         });
  std::vector<const Geometry*> non_poly_ptrs = AsRawPtrs(non_poly);
  EXPECT_TRUE(CoverageValidator::isValid(non_poly_ptrs));
}

TEST(CoverageValidatorTest, HasInvalidResultDetectsNullAndNonNullEntries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  std::vector<std::unique_ptr<Geometry>> empty_results;
  EXPECT_FALSE(CoverageValidator::hasInvalidResult(empty_results));

  std::vector<std::unique_ptr<Geometry>> all_null(2);
  EXPECT_FALSE(CoverageValidator::hasInvalidResult(all_null));

  std::vector<std::unique_ptr<Geometry>> second_invalid;
  second_invalid.push_back(nullptr);
  second_invalid.push_back(reader.read("LINESTRING (0 0, 1 0)"));
  EXPECT_TRUE(CoverageValidator::hasInvalidResult(second_invalid));

  std::vector<std::unique_ptr<Geometry>> first_invalid;
  first_invalid.push_back(reader.read("LINESTRING (0 0, 0 1)"));
  first_invalid.push_back(nullptr);
  EXPECT_TRUE(CoverageValidator::hasInvalidResult(first_invalid));
}

TEST(CoverageValidatorTest, ValidCoveragesIncludingHolesAndMultiPolygons) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Adjacent squares with opposite ring orientation (still coverage-valid as
  // long as vertices match) and a ring enclosing a coverage hole, plus a
  // MultiPolygon and an island inside a donut hole.
  std::vector<std::unique_ptr<Geometry>> valid_cov =
      ReadAllWkt(reader, {
                             "POLYGON ((0 0, 3 0, 3 1, 2 1, 1 1, 0 1, 0 0))",
                             "POLYGON ((0 2, 0 3, 3 3, 3 2, 2 2, 1 2, 0 2))",
                             "POLYGON ((0 1, 1 1, 1 2, 0 2, 0 1))",
                             "POLYGON ((2 1, 3 1, 3 2, 2 2, 2 1))",
                             "MULTIPOLYGON (((10 0, 14 0, 14 4, 10 4, 10 0), "
                             "(11 1, 13 1, 13 3, 11 3, 11 1)), "
                             "((20 0, 22 0, 22 2, 20 2, 20 0)))",
                             "POLYGON ((11 1, 13 1, 13 3, 11 3, 11 1))",
                         });
  std::vector<const Geometry*> valid_ptrs = AsRawPtrs(valid_cov);

  EXPECT_TRUE(CoverageValidator::isValid(valid_ptrs));
  std::vector<std::unique_ptr<Geometry>> res =
      CoverageValidator::validate(valid_ptrs);
  ASSERT_EQ(res.size(), valid_ptrs.size());
  EXPECT_FALSE(CoverageValidator::hasInvalidResult(res));
  for (const auto& entry : res) {
    EXPECT_EQ(entry, nullptr);
  }
}

TEST(CoverageValidatorTest, DetectsMismatchedTNodeAndExactInvalidSegments) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Polygon 0 omits vertex (2, 1) along shared edge x = 2.
  std::vector<std::unique_ptr<Geometry>> mismatched =
      ReadAllWkt(reader, {
                             "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
                             "POLYGON ((2 0, 4 0, 4 1, 2 1, 2 0))",
                             "POLYGON ((2 1, 4 1, 4 2, 2 2, 2 1))",
                         });
  std::vector<const Geometry*> mismatched_ptrs = AsRawPtrs(mismatched);

  EXPECT_FALSE(CoverageValidator::isValid(mismatched_ptrs));
  std::vector<std::unique_ptr<Geometry>> res =
      CoverageValidator::validate(mismatched_ptrs);
  ASSERT_EQ(res.size(), 3u);
  EXPECT_TRUE(CoverageValidator::hasInvalidResult(res));

  std::unique_ptr<Geometry> exp0 = reader.read("LINESTRING (2 0, 2 2)");
  std::unique_ptr<Geometry> exp1 = reader.read("LINESTRING (2 0, 2 1)");
  std::unique_ptr<Geometry> exp2 = reader.read("LINESTRING (2 1, 2 2)");
  ExpectNormalizedExact(res[0].get(), exp0.get());
  ExpectNormalizedExact(res[1].get(), exp1.get());
  ExpectNormalizedExact(res[2].get(), exp2.get());

  // Adding (2, 1) to Polygon 0 repairs the coverage.
  std::unique_ptr<Geometry> repaired0 =
      reader.read("POLYGON ((0 0, 2 0, 2 1, 2 2, 0 2, 0 0))");
  mismatched_ptrs[0] = repaired0.get();
  EXPECT_TRUE(CoverageValidator::isValid(mismatched_ptrs));
}

TEST(CoverageValidatorTest,
     DetectsDuplicatesOverlapsContainedAndPartialErrors) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Duplicate polygons invalidate all segments of both rings.
  std::vector<std::unique_ptr<Geometry>> duplicates =
      ReadAllWkt(reader, {
                             "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
                             "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
                         });
  std::vector<const Geometry*> dup_ptrs = AsRawPtrs(duplicates);
  EXPECT_FALSE(CoverageValidator::isValid(dup_ptrs));
  std::vector<std::unique_ptr<Geometry>> dup_res =
      CoverageValidator::validate(dup_ptrs);
  ASSERT_EQ(dup_res.size(), 2u);
  ASSERT_NE(dup_res[0], nullptr);
  ASSERT_NE(dup_res[1], nullptr);
  EXPECT_EQ(dup_res[0]->getDimension(), 1);
  EXPECT_DOUBLE_EQ(dup_res[0]->getLength(), 8.0);
  EXPECT_DOUBLE_EQ(dup_res[1]->getLength(), 8.0);

  // Overlapping pair plus a valid third disjoint polygon.
  std::vector<std::unique_ptr<Geometry>> partial =
      ReadAllWkt(reader, {
                             "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
                             "POLYGON ((1 0, 3 0, 3 2, 1 2, 1 0))",
                             "POLYGON ((10 0, 12 0, 12 2, 10 2, 10 0))",
                         });
  std::vector<const Geometry*> partial_ptrs = AsRawPtrs(partial);
  EXPECT_FALSE(CoverageValidator::isValid(partial_ptrs));
  std::vector<std::unique_ptr<Geometry>> partial_res =
      CoverageValidator::validate(partial_ptrs);
  ASSERT_EQ(partial_res.size(), 3u);
  ASSERT_NE(partial_res[0], nullptr);
  ASSERT_NE(partial_res[1], nullptr);
  EXPECT_EQ(partial_res[2], nullptr);
  EXPECT_GT(partial_res[0]->getLength(), 0.0);
  EXPECT_GT(partial_res[1]->getLength(), 0.0);

  // Small polygon completely inside a larger polygon's interior.
  std::vector<std::unique_ptr<Geometry>> contained =
      ReadAllWkt(reader, {
                             "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
                             "POLYGON ((2 2, 4 2, 4 4, 2 4, 2 2))",
                         });
  std::vector<const Geometry*> contained_ptrs = AsRawPtrs(contained);
  EXPECT_FALSE(CoverageValidator::isValid(contained_ptrs));
  std::vector<std::unique_ptr<Geometry>> contained_res =
      CoverageValidator::validate(contained_ptrs);
  ASSERT_EQ(contained_res.size(), 2u);
  EXPECT_EQ(contained_res[0], nullptr);
  ASSERT_NE(contained_res[1], nullptr);
  EXPECT_DOUBLE_EQ(contained_res[1]->getLength(), 8.0);
}

TEST(CoverageValidatorTest, DetectsNarrowGapsAndGoresWithGapWidth) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Two squares separated by a narrow gap of width 0.2.
  std::vector<std::unique_ptr<Geometry>> narrow_gap =
      ReadAllWkt(reader, {
                             "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))",
                             "POLYGON ((10.2 0, 20 0, 20 10, 10.2 10, 10.2 0))",
                         });
  std::vector<const Geometry*> gap_ptrs = AsRawPtrs(narrow_gap);

  EXPECT_TRUE(CoverageValidator::isValid(gap_ptrs));
  EXPECT_TRUE(CoverageValidator::isValid(gap_ptrs, 0.1));
  EXPECT_FALSE(CoverageValidator::isValid(gap_ptrs, 0.25));

  std::vector<std::unique_ptr<Geometry>> gap_res =
      CoverageValidator::validate(gap_ptrs, 0.25);
  ASSERT_EQ(gap_res.size(), 2u);
  std::unique_ptr<Geometry> exp_left = reader.read("LINESTRING (10 0, 10 10)");
  std::unique_ptr<Geometry> exp_right =
      reader.read("LINESTRING (10.2 0, 10.2 10)");
  ExpectNormalizedExact(gap_res[0].get(), exp_left.get());
  ExpectNormalizedExact(gap_res[1].get(), exp_right.get());

  // Exercise instance method setGapWidth + validate reuse.
  CoverageValidator validator(gap_ptrs);
  EXPECT_FALSE(CoverageValidator::hasInvalidResult(validator.validate()));
  validator.setGapWidth(0.25);
  EXPECT_TRUE(CoverageValidator::hasInvalidResult(validator.validate()));
  validator.setGapWidth(0.1);
  EXPECT_FALSE(CoverageValidator::hasInvalidResult(validator.validate()));

  // Narrow gore (wedge touching at (0, 0) and opening to width 0.2 at y = 10).
  std::vector<std::unique_ptr<Geometry>> gore =
      ReadAllWkt(reader, {
                             "POLYGON ((-5 0, 0 0, 0 10, -5 10, -5 0))",
                             "POLYGON ((0 0, 5 0, 5 10, 0.2 10, 0 0))",
                         });
  std::vector<const Geometry*> gore_ptrs = AsRawPtrs(gore);
  EXPECT_TRUE(CoverageValidator::isValid(gore_ptrs));
  EXPECT_FALSE(CoverageValidator::isValid(gore_ptrs, 0.25));
}

TEST(CoverageValidatorTest, ThrowsOnCurvedComponents) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  std::unique_ptr<Geometry> curve_poly =
      reader.read("CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0))");
  std::vector<const Geometry*> curved_only = {curve_poly.get()};
  EXPECT_THROW(CoverageValidator::isValid(curved_only),
               UnsupportedOperationException);
  EXPECT_THROW(CoverageValidator::validate(curved_only),
               UnsupportedOperationException);

  std::unique_ptr<Geometry> poly =
      reader.read("POLYGON ((10 0, 12 0, 12 2, 10 2, 10 0))");
  std::unique_ptr<Geometry> multisurface = reader.read(
      "MULTISURFACE (CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 "
      "0)))");
  std::vector<const Geometry*> mixed = {poly.get(), multisurface.get()};
  EXPECT_THROW(CoverageValidator::isValid(mixed, 0.1),
               UnsupportedOperationException);
}

}  // namespace
}  // namespace geos_google
