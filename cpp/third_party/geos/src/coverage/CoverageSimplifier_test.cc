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

// Unit tests for third_party/geos/src/coverage/CoverageSimplifier.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for IllegalArgumentException
//   assertions.
// - Allocates isolated GeometryFactory instances via GeometryFactory::create().
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/coverage/CoverageSimplifier.cpp

#include "third_party/geos/include/geos/coverage/CoverageSimplifier.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/coverage/CoverageUnion.h"
#include "third_party/geos/include/geos/coverage/CoverageValidator.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::coverage::CoverageSimplifier;
using geos::coverage::CoverageUnion;
using geos::coverage::CoverageValidator;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::GeometryTypeId;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::util::IllegalArgumentException;

struct NonPolygonalCase {
  std::string_view name;
  std::string_view wkt;
};

constexpr auto kNonPolygonalCases = std::to_array<NonPolygonalCase>({
    {.name = "point", .wkt = "POINT (1 2)"},
    {.name = "linestring", .wkt = "LINESTRING (0 0, 1 1)"},
    {.name = "linearring", .wkt = "LINEARRING (0 0, 1 0, 1 1, 0 0)"},
    {.name = "multipoint", .wkt = "MULTIPOINT ((0 0), (1 1))"},
    {.name = "multilinestring",
     .wkt = "MULTILINESTRING ((0 0, 1 0), (1 0, 1 1))"},
    {.name = "geometrycollection_with_polygons",
     .wkt = "GEOMETRYCOLLECTION (POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)))"},
    {.name = "curvepolygon",
     .wkt = "CURVEPOLYGON (CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0))"},
});

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

TEST(CoverageSimplifierTest,
     ConstructorAcceptsEmptyVectorAndRejectsNonPolygonal) {
  std::vector<const Geometry*> empty_cov;
  EXPECT_NO_THROW(static_cast<void>(CoverageSimplifier(empty_cov)));

  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  for (const NonPolygonalCase& tc : kNonPolygonalCases) {
    SCOPED_TRACE(std::string(tc.name));
    std::unique_ptr<Geometry> invalid = reader.read(std::string(tc.wkt));
    std::vector<const Geometry*> cov = {invalid.get()};
    EXPECT_THROW(static_cast<void>(CoverageSimplifier(cov)),
                 IllegalArgumentException);
    EXPECT_THROW(CoverageSimplifier::simplify(cov, 1.0),
                 IllegalArgumentException);
    EXPECT_THROW(CoverageSimplifier::simplifyInner(cov, 1.0),
                 IllegalArgumentException);
  }

  // Verify validation continues past valid polygonal leading elements.
  std::unique_ptr<Geometry> poly =
      reader.read("POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  std::unique_ptr<Geometry> line = reader.read("LINESTRING (1 0, 2 0)");
  std::vector<const Geometry*> mixed = {poly.get(), line.get()};
  EXPECT_THROW(CoverageSimplifier::simplify(mixed, 1.0),
               IllegalArgumentException);
}

TEST(CoverageSimplifierTest, SinglePolygonZeroAndNonZeroTolerance) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Includes a collinear vertex (2, 0) with triangle area 0.0 and a small spike
  // (5, 0.5) whose triangle (4,0)-(5,0.5)-(6,0) has area 0.5.
  std::vector<std::unique_ptr<Geometry>> input = ReadAllWkt(
      reader,
      {"POLYGON ((0 0, 2 0, 4 0, 5 0.5, 6 0, 10 0, 10 10, 0 10, 0 0))"});
  std::vector<const Geometry*> input_ptrs = AsRawPtrs(input);

  // Zero tolerance removes only collinear vertices, preserving the 0.5 spike.
  std::vector<std::unique_ptr<Geometry>> zero_tol =
      CoverageSimplifier::simplify(input_ptrs, 0.0);
  ASSERT_EQ(zero_tol.size(), 1u);
  std::unique_ptr<Geometry> expected_zero =
      reader.read("POLYGON ((0 0, 4 0, 5 0.5, 6 0, 10 0, 10 10, 0 10, 0 0))");
  ExpectNormalizedExact(zero_tol[0].get(), expected_zero.get());

  // Tolerance 0.5 (areaTolerance 0.25) preserves the area-0.5 spike.
  std::vector<std::unique_ptr<Geometry>> small_tol =
      CoverageSimplifier::simplify(input_ptrs, 0.5);
  ASSERT_EQ(small_tol.size(), 1u);
  ExpectNormalizedExact(small_tol[0].get(), expected_zero.get());

  // Tolerance 0.8 (areaTolerance 0.64) removes the spike and its now-collinear
  // base vertices, collapsing to a 4-corner square.
  std::vector<std::unique_ptr<Geometry>> large_tol =
      CoverageSimplifier::simplify(input_ptrs, 0.8);
  ASSERT_EQ(large_tol.size(), 1u);
  std::unique_ptr<Geometry> expected_square =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  ExpectNormalizedExact(large_tol[0].get(), expected_square.get());

  // simplifyInner on a single polygon has no inner edges and preserves all
  // outer edges verbatim (up to canonical ring reconstruction).
  std::vector<std::unique_ptr<Geometry>> inner_only =
      CoverageSimplifier::simplifyInner(input_ptrs, 5.0);
  ASSERT_EQ(inner_only.size(), 1u);
  ExpectNormalizedExact(inner_only[0].get(), input[0].get());
}

TEST(CoverageSimplifierTest, SimplifyVsSimplifyInnerOnAdjacentPolygons) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Outer dent (5, -0.2) has area 1.0; shared inner kink (10.2, 5) has
  // area 1.0.
  std::vector<std::unique_ptr<Geometry>> input = ReadAllWkt(
      reader, {
                  "POLYGON ((0 0, 5 -0.2, 10 0, 10.2 5, 10 10, 0 10, 0 0))",
                  "POLYGON ((10 0, 20 0, 20 10, 10 10, 10.2 5, 10 0))",
              });
  std::vector<const Geometry*> input_ptrs = AsRawPtrs(input);
  ASSERT_TRUE(CoverageValidator::isValid(input_ptrs));

  // Full simplification removes both outer dent and inner kink at tolerance 1.5
  // (areaTolerance 2.25) while keeping nodes (10, 0) and (10, 10) fixed.
  std::vector<std::unique_ptr<Geometry>> full_simp =
      CoverageSimplifier::simplify(input_ptrs, 1.5);
  ASSERT_EQ(full_simp.size(), 2u);
  std::vector<const Geometry*> full_ptrs = AsRawPtrs(full_simp);
  EXPECT_TRUE(CoverageValidator::isValid(full_ptrs));

  std::unique_ptr<Geometry> expected_left_full =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  std::unique_ptr<Geometry> expected_right =
      reader.read("POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0))");
  ExpectNormalizedExact(full_simp[0].get(), expected_left_full.get());
  ExpectNormalizedExact(full_simp[1].get(), expected_right.get());

  // Inner simplification removes the shared kink (10.2, 5) while leaving the
  // outer dent (5, -0.2) untouched.
  std::vector<std::unique_ptr<Geometry>> inner_simp =
      CoverageSimplifier::simplifyInner(input_ptrs, 1.5);
  ASSERT_EQ(inner_simp.size(), 2u);
  std::vector<const Geometry*> inner_ptrs = AsRawPtrs(inner_simp);
  EXPECT_TRUE(CoverageValidator::isValid(inner_ptrs));

  std::unique_ptr<Geometry> expected_left_inner =
      reader.read("POLYGON ((0 0, 5 -0.2, 10 0, 10 10, 0 10, 0 0))");
  ExpectNormalizedExact(inner_simp[0].get(), expected_left_inner.get());
  ExpectNormalizedExact(inner_simp[1].get(), expected_right.get());

  // Original union must match inner-simplified union since outer edges did not
  // change.
  std::unique_ptr<Geometry> orig_union = CoverageUnion::Union(input_ptrs);
  std::unique_ptr<Geometry> inner_union = CoverageUnion::Union(inner_ptrs);
  ExpectNormalizedExact(inner_union.get(), orig_union.get());
}

TEST(CoverageSimplifierTest, UniquePtrStaticOverloadsAndInstanceReuse) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  const std::vector<std::unique_ptr<Geometry>> input = ReadAllWkt(
      reader, {
                  "POLYGON ((0 0, 5 -0.1, 10 0, 10.1 5, 10 10, 0 10, 0 0))",
                  "POLYGON ((10 0, 20 0, 20 10, 10 10, 10.1 5, 10 0))",
              });

  std::vector<std::unique_ptr<Geometry>> static_full =
      CoverageSimplifier::simplify(input, 1.0);
  ASSERT_EQ(static_full.size(), 2u);
  std::vector<const Geometry*> static_full_ptrs = AsRawPtrs(static_full);
  EXPECT_TRUE(CoverageValidator::isValid(static_full_ptrs));
  EXPECT_EQ(static_full[0]->getNumPoints(), 5u);
  EXPECT_EQ(static_full[1]->getNumPoints(), 5u);

  std::vector<std::unique_ptr<Geometry>> static_inner =
      CoverageSimplifier::simplifyInner(input, 1.0);
  ASSERT_EQ(static_inner.size(), 2u);
  std::vector<const Geometry*> static_inner_ptrs = AsRawPtrs(static_inner);
  EXPECT_TRUE(CoverageValidator::isValid(static_inner_ptrs));
  EXPECT_EQ(static_inner[0]->getNumPoints(), 6u);
  EXPECT_EQ(static_inner[1]->getNumPoints(), 5u);

  // Reuse a CoverageSimplifier instance across calls with different tolerances.
  std::vector<const Geometry*> input_ptrs = AsRawPtrs(input);
  CoverageSimplifier simplifier(input_ptrs);

  std::vector<std::unique_ptr<Geometry>> res_tiny = simplifier.simplify(0.1);
  ASSERT_EQ(res_tiny.size(), 2u);
  EXPECT_EQ(res_tiny[0]->getNumPoints(), 7u);

  std::vector<std::unique_ptr<Geometry>> res_inner =
      simplifier.simplifyInner(1.0);
  ASSERT_EQ(res_inner.size(), 2u);
  EXPECT_EQ(res_inner[0]->getNumPoints(), 6u);
  EXPECT_EQ(res_inner[1]->getNumPoints(), 5u);

  std::vector<std::unique_ptr<Geometry>> res_large = simplifier.simplify(1.0);
  ASSERT_EQ(res_large.size(), 2u);
  EXPECT_EQ(res_large[0]->getNumPoints(), 5u);
  EXPECT_EQ(res_large[1]->getNumPoints(), 5u);
}

TEST(CoverageSimplifierTest, PreservesThreeWayJunctionNodesAndTopology) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Three polygons sharing an interior 3-way junction node at (4, 4), plus
  // edge perturbations (5, 2.5) [area 2.5] and (5, 10.1) [area 0.5].
  std::vector<std::unique_ptr<Geometry>> input = ReadAllWkt(
      reader, {
                  "POLYGON ((0 0, 10 0, 5 2.5, 4 4, 0 0))",
                  "POLYGON ((0 0, 4 4, 0 10, 0 0))",
                  "POLYGON ((10 0, 10 10, 5 10.1, 0 10, 4 4, 5 2.5, 10 0))",
              });
  std::vector<const Geometry*> input_ptrs = AsRawPtrs(input);
  ASSERT_TRUE(CoverageValidator::isValid(input_ptrs));

  // Tolerance 3.0 (areaTolerance 9.0) removes (5, 2.5) and (5, 10.1) while
  // keeping corner (10, 10) [area 50.0] and 3-way junction node (4, 4).
  std::vector<std::unique_ptr<Geometry>> simplified =
      CoverageSimplifier::simplify(input_ptrs, 3.0);
  ASSERT_EQ(simplified.size(), 3u);
  std::vector<const Geometry*> simp_ptrs = AsRawPtrs(simplified);
  EXPECT_TRUE(CoverageValidator::isValid(simp_ptrs));

  std::unique_ptr<Geometry> expected0 =
      reader.read("POLYGON ((0 0, 10 0, 4 4, 0 0))");
  std::unique_ptr<Geometry> expected1 =
      reader.read("POLYGON ((0 0, 4 4, 0 10, 0 0))");
  std::unique_ptr<Geometry> expected2 =
      reader.read("POLYGON ((10 0, 10 10, 0 10, 4 4, 10 0))");
  ExpectNormalizedExact(simplified[0].get(), expected0.get());
  ExpectNormalizedExact(simplified[1].get(), expected1.get());
  ExpectNormalizedExact(simplified[2].get(), expected2.get());

  // Even a huge tolerance preserves the 3-way junction node (4, 4) and
  // boundary nodes (0, 0), (10, 0), (0, 10) while simplifying (10, 10).
  std::vector<std::unique_ptr<Geometry>> aggressive =
      CoverageSimplifier::simplify(input_ptrs, 100.0);
  ASSERT_EQ(aggressive.size(), 3u);
  std::vector<const Geometry*> aggr_ptrs = AsRawPtrs(aggressive);
  EXPECT_TRUE(CoverageValidator::isValid(aggr_ptrs));
  std::unique_ptr<Geometry> expected2_aggr =
      reader.read("POLYGON ((10 0, 0 10, 4 4, 10 0))");
  ExpectNormalizedExact(aggressive[0].get(), expected0.get());
  ExpectNormalizedExact(aggressive[1].get(), expected1.get());
  ExpectNormalizedExact(aggressive[2].get(), expected2_aggr.get());

  // Nearby disjoint polygon blocks removal of concave notch (5, 0.5)
  // [area 0.5 <= 0.81], while (4, 0) and (6, 0) [area 1.0 > 0.81] stay fixed.
  std::vector<std::unique_ptr<Geometry>> corridor = ReadAllWkt(
      reader, {
                  "POLYGON ((0 0, 4 0, 5 0.5, 6 0, 10 0, 10 10, 0 10, 0 0))",
                  "POLYGON ((4.8 0.1, 5.2 0.1, 5.2 0.3, 4.8 0.3, 4.8 0.1))",
              });
  std::vector<const Geometry*> corridor_ptrs = AsRawPtrs(corridor);
  ASSERT_TRUE(CoverageValidator::isValid(corridor_ptrs));

  std::vector<std::unique_ptr<Geometry>> corridor_simp =
      CoverageSimplifier::simplify(corridor_ptrs, 0.9);
  ASSERT_EQ(corridor_simp.size(), 2u);
  std::vector<const Geometry*> corridor_simp_ptrs = AsRawPtrs(corridor_simp);
  EXPECT_TRUE(CoverageValidator::isValid(corridor_simp_ptrs));
  EXPECT_TRUE(corridor_simp[0]->disjoint(corridor_simp[1].get()));
  ExpectNormalizedExact(corridor_simp[0].get(), corridor[0].get());
}

TEST(CoverageSimplifierTest, MultiPolygonHolesAndIslandInHole) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // MultiPolygon whose left component shares a kinked edge with a bridging
  // Polygon.
  std::vector<std::unique_ptr<Geometry>> multi_cov = ReadAllWkt(
      reader, {
                  "MULTIPOLYGON (((0 0, 5 0, 5.1 2.5, 5 5, 0 5, 0 0)), "
                  "((10 0, 15 0, 15 5, 10 5, 10 0)))",
                  "POLYGON ((5 0, 10 0, 10 5, 5 5, 5.1 2.5, 5 0))",
              });
  std::vector<const Geometry*> multi_ptrs = AsRawPtrs(multi_cov);
  ASSERT_TRUE(CoverageValidator::isValid(multi_ptrs));

  std::vector<std::unique_ptr<Geometry>> multi_simp =
      CoverageSimplifier::simplify(multi_ptrs, 1.5);
  ASSERT_EQ(multi_simp.size(), 2u);
  EXPECT_EQ(multi_simp[0]->getGeometryTypeId(),
            GeometryTypeId::GEOS_MULTIPOLYGON);
  EXPECT_EQ(multi_simp[1]->getGeometryTypeId(), GeometryTypeId::GEOS_POLYGON);
  std::vector<const Geometry*> multi_simp_ptrs = AsRawPtrs(multi_simp);
  EXPECT_TRUE(CoverageValidator::isValid(multi_simp_ptrs));

  std::unique_ptr<Geometry> expected_mp = reader.read(
      "MULTIPOLYGON (((0 0, 5 0, 5 5, 0 5, 0 0)), "
      "((10 0, 15 0, 15 5, 10 5, 10 0)))");
  std::unique_ptr<Geometry> expected_bridge =
      reader.read("POLYGON ((5 0, 10 0, 10 5, 5 5, 5 0))");
  ExpectNormalizedExact(multi_simp[0].get(), expected_mp.get());
  ExpectNormalizedExact(multi_simp[1].get(), expected_bridge.get());

  // Donut polygon whose hole is filled by a matching island polygon (shared
  // free ring with ringCount == 2).
  std::vector<std::unique_ptr<Geometry>> island_cov =
      ReadAllWkt(reader, {
                             "POLYGON ((0 0, 10 -0.2, 20 0, 20 20, 0 20, 0 0), "
                             "(4 4, 16 4, 16.1 10, 16 16, 4 16, 4 4))",
                             "POLYGON ((4 4, 16 4, 16.1 10, 16 16, 4 16, 4 4))",
                         });
  std::vector<const Geometry*> island_ptrs = AsRawPtrs(island_cov);
  ASSERT_TRUE(CoverageValidator::isValid(island_ptrs));

  // Inner simplification simplifies the shared hole/island free ring (area 0.6)
  // while keeping the outer boundary perturbation (10, -0.2).
  std::vector<std::unique_ptr<Geometry>> island_inner =
      CoverageSimplifier::simplifyInner(island_ptrs, 1.0);
  ASSERT_EQ(island_inner.size(), 2u);
  std::vector<const Geometry*> island_inner_ptrs = AsRawPtrs(island_inner);
  EXPECT_TRUE(CoverageValidator::isValid(island_inner_ptrs));

  std::unique_ptr<Geometry> expected_donut_inner = reader.read(
      "POLYGON ((0 0, 10 -0.2, 20 0, 20 20, 0 20, 0 0), "
      "(4 4, 16 4, 16 16, 4 16, 4 4))");
  std::unique_ptr<Geometry> expected_island =
      reader.read("POLYGON ((4 4, 16 4, 16 16, 4 16, 4 4))");
  ExpectNormalizedExact(island_inner[0].get(), expected_donut_inner.get());
  ExpectNormalizedExact(island_inner[1].get(), expected_island.get());
}

TEST(CoverageSimplifierTest, HandlesEmptyPolygonElementsAndPreservesFactory) {
  PrecisionModel pm(100.0);
  GeometryFactory::Ptr factory = GeometryFactory::create(&pm, 4326);
  WKTReader reader(*factory);

  // Coverage containing only empty geometries has zero edges.
  std::vector<std::unique_ptr<Geometry>> only_empty =
      ReadAllWkt(reader, {"POLYGON EMPTY", "MULTIPOLYGON EMPTY"});
  std::vector<const Geometry*> only_empty_ptrs = AsRawPtrs(only_empty);
  std::vector<std::unique_ptr<Geometry>> only_empty_simp =
      CoverageSimplifier::simplify(only_empty_ptrs, 1.0);
  ASSERT_EQ(only_empty_simp.size(), 2u);
  EXPECT_TRUE(only_empty_simp[0]->isEmpty());
  EXPECT_TRUE(only_empty_simp[1]->isEmpty());
  EXPECT_EQ(only_empty_simp[0]->getFactory(), factory.get());
  EXPECT_EQ(only_empty_simp[0]->getSRID(), 4326);

  std::vector<std::unique_ptr<Geometry>> only_empty_inner =
      CoverageSimplifier::simplifyInner(only_empty_ptrs, 1.0);
  ASSERT_EQ(only_empty_inner.size(), 2u);
  EXPECT_TRUE(only_empty_inner[0]->isEmpty());
  EXPECT_TRUE(only_empty_inner[1]->isEmpty());

  // Coverage mixing an empty polygon and a non-empty polygon.
  std::vector<std::unique_ptr<Geometry>> mixed_empty =
      ReadAllWkt(reader, {
                             "POLYGON EMPTY",
                             "POLYGON ((0 0, 5 0.1, 10 0, 10 10, 0 10, 0 0))",
                         });
  std::vector<const Geometry*> mixed_ptrs = AsRawPtrs(mixed_empty);

  std::vector<std::unique_ptr<Geometry>> mixed_simp =
      CoverageSimplifier::simplify(mixed_ptrs, 1.0);
  ASSERT_EQ(mixed_simp.size(), 2u);
  EXPECT_TRUE(mixed_simp[0]->isEmpty());
  EXPECT_EQ(mixed_simp[1]->getFactory(), factory.get());
  EXPECT_EQ(mixed_simp[1]->getSRID(), 4326);
  std::unique_ptr<Geometry> expected_square =
      reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  ExpectNormalizedExact(mixed_simp[1].get(), expected_square.get());
}

}  // namespace
}  // namespace geos_google
