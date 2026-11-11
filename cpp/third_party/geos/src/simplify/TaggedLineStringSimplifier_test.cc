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

#include "third_party/geos/include/geos/simplify/TaggedLineStringSimplifier.h"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineSegment.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/simplify/ComponentJumpChecker.h"
#include "third_party/geos/include/geos/simplify/LineSegmentIndex.h"
#include "third_party/geos/include/geos/simplify/TaggedLineSegment.h"
#include "third_party/geos/include/geos/simplify/TaggedLineString.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineSegment;
using geos::geom::LineString;
using geos::simplify::ComponentJumpChecker;
using geos::simplify::LineSegmentIndex;
using geos::simplify::TaggedLineSegment;
using geos::simplify::TaggedLineString;
using geos::simplify::TaggedLineStringSimplifier;

void ExpectResultCoords(const TaggedLineString& tls,
                        const std::vector<Coordinate>& expected) {
  CoordinateSequence::Ptr coords = tls.getResultCoordinates();
  ASSERT_NE(coords, nullptr);
  ASSERT_EQ(coords->size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_DOUBLE_EQ(coords->getX(i), expected[i].x) << "at index " << i;
    EXPECT_DOUBLE_EQ(coords->getY(i), expected[i].y) << "at index " << i;
  }
}

void SimplifySingle(TaggedLineString& tls, double tolerance) {
  LineSegmentIndex input_index;
  LineSegmentIndex output_index;
  input_index.add(tls);
  std::vector<TaggedLineString*> components{&tls};
  ComponentJumpChecker jump_checker(components);
  TaggedLineStringSimplifier simplifier(&input_index, &output_index,
                                        &jump_checker);
  simplifier.simplify(&tls, tolerance);
}

void SimplifyMulti(std::vector<TaggedLineString*>& components,
                   double tolerance) {
  LineSegmentIndex input_index;
  LineSegmentIndex output_index;
  for (TaggedLineString* tls : components) {
    input_index.add(*tls);
  }
  ComponentJumpChecker jump_checker(components);
  for (TaggedLineString* tls : components) {
    TaggedLineStringSimplifier simplifier(&input_index, &output_index,
                                          &jump_checker);
    simplifier.simplify(tls, tolerance);
  }
}

class TaggedLineStringSimplifierTest : public ::testing::Test {
 protected:
  TaggedLineStringSimplifierTest() : factory_(GeometryFactory::create()) {}

  std::unique_ptr<LineString> MakeLine(
      std::initializer_list<Coordinate> coords) {
    auto seq = std::make_unique<CoordinateSequence>(coords);
    return factory_->createLineString(std::move(seq));
  }

  std::unique_ptr<LinearRing> MakeRing(
      std::initializer_list<Coordinate> coords) {
    auto seq = std::make_unique<CoordinateSequence>(coords);
    return factory_->createLinearRing(std::move(seq));
  }

  GeometryFactory::Ptr factory_;
};

TEST_F(TaggedLineStringSimplifierTest, EmptyLineStringProducesEmptyResult) {
  std::unique_ptr<LineString> empty_line = factory_->createLineString();
  TaggedLineString tls(empty_line.get(), 2, false);

  SimplifySingle(tls, 10.0);

  EXPECT_EQ(tls.getResultSize(), 0u);
  EXPECT_TRUE(tls.getResultSegments().empty());

  std::unique_ptr<LinearRing> empty_ring = factory_->createLinearRing();
  TaggedLineString tls_ring(empty_ring.get(), 4, true);

  SimplifySingle(tls_ring, 10.0);

  EXPECT_EQ(tls_ring.getResultSize(), 0u);
  EXPECT_TRUE(tls_ring.getResultSegments().empty());
}

TEST_F(TaggedLineStringSimplifierTest, SingleSegmentLineStringIsPreserved) {
  std::unique_ptr<LineString> line = MakeLine({{0.0, 0.0}, {10.0, 0.0}});
  TaggedLineString tls(line.get(), 2, false);

  LineSegmentIndex input_index;
  LineSegmentIndex output_index;
  input_index.add(tls);
  std::vector<TaggedLineString*> components{&tls};
  ComponentJumpChecker jump_checker(components);
  TaggedLineStringSimplifier simplifier(&input_index, &output_index,
                                        &jump_checker);
  simplifier.simplify(&tls, 100.0);

  ExpectResultCoords(tls, {{0.0, 0.0}, {10.0, 0.0}});
  ASSERT_EQ(tls.getResultSegments().size(), 1u);
  EXPECT_EQ(tls.getResultSegments()[0]->getParent(), line.get());
  EXPECT_EQ(tls.getResultSegments()[0]->getIndex(), 0u);

  LineSegment probe(0.0, 0.0, 10.0, 0.0);
  EXPECT_EQ(input_index.query(&probe).size(), 1u);
  EXPECT_TRUE(output_index.query(&probe).empty());
}

TEST_F(TaggedLineStringSimplifierTest,
       CollinearVerticesSimplifyAtZeroTolerance) {
  std::unique_ptr<LineString> line =
      MakeLine({{0.0, 0.0}, {2.0, 0.0}, {5.0, 0.0}, {8.0, 0.0}, {10.0, 0.0}});
  TaggedLineString tls(line.get(), 2, false);

  SimplifySingle(tls, 0.0);

  ExpectResultCoords(tls, {{0.0, 0.0}, {10.0, 0.0}});
}

TEST_F(TaggedLineStringSimplifierTest,
       RespectsDistanceToleranceBoundaryAndNegativeTolerance) {
  std::unique_ptr<LineString> line =
      MakeLine({{0.0, 0.0}, {5.0, 1.0}, {10.0, 0.0}});

  {
    TaggedLineString tls(line.get(), 2, false);
    SimplifySingle(tls, 1.0);
    ExpectResultCoords(tls, {{0.0, 0.0}, {10.0, 0.0}});
  }
  {
    TaggedLineString tls(line.get(), 2, false);
    SimplifySingle(tls, 0.99);
    ExpectResultCoords(tls, {{0.0, 0.0}, {5.0, 1.0}, {10.0, 0.0}});
  }
  {
    TaggedLineString tls(line.get(), 2, false);
    SimplifySingle(tls, -1.0);
    ExpectResultCoords(tls, {{0.0, 0.0}, {5.0, 1.0}, {10.0, 0.0}});
  }
}

TEST_F(TaggedLineStringSimplifierTest,
       RecursiveDouglasPeuckerSelectsFurthestVertex) {
  std::unique_ptr<LineString> line =
      MakeLine({{0.0, 0.0}, {3.0, 1.0}, {5.0, 5.0}, {8.0, 0.5}, {10.0, 0.0}});
  TaggedLineString tls(line.get(), 2, false);

  SimplifySingle(tls, 2.0);

  ExpectResultCoords(tls, {{0.0, 0.0}, {5.0, 5.0}, {10.0, 0.0}});
}

TEST_F(TaggedLineStringSimplifierTest,
       MinimumSizePreventsOverSimplificationAndClearsOnceMet) {
  std::unique_ptr<LineString> flat_line =
      MakeLine({{0.0, 0.0}, {2.0, 0.1}, {5.0, 0.2}, {8.0, 0.1}, {10.0, 0.0}});
  TaggedLineString tls_min4(flat_line.get(), 4, false);

  SimplifySingle(tls_min4, 10.0);

  EXPECT_GE(tls_min4.getResultSize(), 4u);
  ExpectResultCoords(
      tls_min4, {{0.0, 0.0}, {2.0, 0.1}, {5.0, 0.2}, {8.0, 0.1}, {10.0, 0.0}});

  // Once the left branch produces >= minimumSize vertices, the right branch
  // can flatten at shallow recursion depth.
  std::unique_ptr<LineString> asymmetric_line = MakeLine({{0.0, 0.0},
                                                          {1.0, 4.0},
                                                          {3.0, 4.0},
                                                          {5.0, 5.0},
                                                          {7.5, 2.55},
                                                          {10.0, 0.0}});
  TaggedLineString tls_min3(asymmetric_line.get(), 3, false);

  SimplifySingle(tls_min3, 1.0);

  ExpectResultCoords(tls_min3,
                     {{0.0, 0.0}, {1.0, 4.0}, {5.0, 5.0}, {10.0, 0.0}});
}

TEST_F(TaggedLineStringSimplifierTest,
       NonRingFlagOrNonRingCoordsSkipRingEndpointSimplification) {
  std::unique_ptr<LineString> closed_line = MakeLine({{5.0, 0.0},
                                                      {10.0, 0.0},
                                                      {10.0, 10.0},
                                                      {0.0, 10.0},
                                                      {0.0, 0.0},
                                                      {5.0, 0.0}});
  TaggedLineString tls_non_ring(closed_line.get(), 4, false);
  SimplifySingle(tls_non_ring, 1.0);
  ExpectResultCoords(tls_non_ring, {{5.0, 0.0},
                                    {10.0, 0.0},
                                    {10.0, 10.0},
                                    {0.0, 10.0},
                                    {0.0, 0.0},
                                    {5.0, 0.0}});

  // Fewer than 4 coordinates: CoordinateSequence::isRing() is false even when
  // TaggedLineString::isRing() is true.
  std::unique_ptr<LineString> short_closed =
      MakeLine({{0.0, 0.0}, {10.0, 0.0}, {0.0, 0.0}});
  TaggedLineString tls_short(short_closed.get(), 2, true);
  SimplifySingle(tls_short, 1.0);
  ExpectResultCoords(tls_short, {{0.0, 0.0}, {10.0, 0.0}, {0.0, 0.0}});

  std::unique_ptr<LineString> open_line =
      MakeLine({{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}});
  TaggedLineString tls_open(open_line.get(), 4, true);
  SimplifySingle(tls_open, 1.0);
  ExpectResultCoords(tls_open,
                     {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}});
}

TEST_F(TaggedLineStringSimplifierTest,
       LargeMagnitudeCoordinatesFallbackPreservesSegments) {
  std::unique_ptr<LineString> huge_line =
      MakeLine({{0.0, 0.0}, {2e199, 5.0}, {5e199, 10.0}, {1e200, 0.0}});
  TaggedLineString tls(huge_line.get(), 2, false);

  SimplifySingle(tls, 1e100);

  ExpectResultCoords(tls,
                     {{0.0, 0.0}, {2e199, 5.0}, {5e199, 10.0}, {1e200, 0.0}});
}

TEST_F(TaggedLineStringSimplifierTest,
       InputIntersectionIgnoresCollapsedSectionAndBlocksOtherSegments) {
  // Crossing an input segment inside the section being collapsed is allowed.
  std::unique_ptr<LineString> zigzag =
      MakeLine({{0.0, 0.0}, {7.0, 0.5}, {3.0, -0.5}, {10.0, 0.0}});
  TaggedLineString tls_zigzag(zigzag.get(), 2, false);
  SimplifySingle(tls_zigzag, 1.0);
  ExpectResultCoords(tls_zigzag, {{0.0, 0.0}, {10.0, 0.0}});

  // Crossing another uncollapsed segment of the same line blocks flattening:
  // both when the blocking segment follows the section (segIndex > excludeEnd)
  // and when it precedes the section (segIndex < excludeStart).
  std::unique_ptr<LineString> self_crossing_candidate = MakeLine({{0.0, 0.0},
                                                                  {0.0, 1.0},
                                                                  {10.0, 1.0},
                                                                  {12.0, 0.0},
                                                                  {5.0, -20.0},
                                                                  {5.0, 10.0}});
  TaggedLineString tls_self(self_crossing_candidate.get(), 2, false);
  SimplifySingle(tls_self, 2.0);
  ExpectResultCoords(tls_self, {{0.0, 0.0},
                                {0.0, 1.0},
                                {10.0, 1.0},
                                {12.0, 0.0},
                                {5.0, -20.0},
                                {5.0, 10.0}});

  std::unique_ptr<LineString> earlier_self_crossing = MakeLine({{5.0, -20.0},
                                                                {5.0, 10.0},
                                                                {12.0, 0.0},
                                                                {10.0, 1.0},
                                                                {0.0, 1.0},
                                                                {0.0, 0.0}});
  TaggedLineString tls_earlier_self(earlier_self_crossing.get(), 2, false);
  SimplifySingle(tls_earlier_self, 2.0);
  ExpectResultCoords(tls_earlier_self, {{5.0, -20.0},
                                        {5.0, 10.0},
                                        {12.0, 0.0},
                                        {10.0, 1.0},
                                        {0.0, 1.0},
                                        {0.0, 0.0}});

  // Exact excludeEnd boundary: segment at index == excludeEnd overlaps flatSeg
  // collinearly and is not excluded (isInLineSection uses segIndex <
  // excludeEnd).
  std::unique_ptr<LineString> foldback_at_end =
      MakeLine({{0.0, 0.0}, {5.0, 0.2}, {10.0, 0.0}, {2.0, 0.0}, {2.0, 10.0}});
  TaggedLineString tls_foldback(foldback_at_end.get(), 2, false);
  SimplifySingle(tls_foldback, 1.0);
  ExpectResultCoords(
      tls_foldback,
      {{0.0, 0.0}, {5.0, 0.2}, {10.0, 0.0}, {2.0, 0.0}, {2.0, 10.0}});

  // Crossing a segment of another line in inputIndex blocks flattening.
  std::unique_ptr<LineString> line_a =
      MakeLine({{0.0, 0.0}, {5.0, 0.5}, {10.0, 0.0}});
  std::unique_ptr<LineString> line_b = MakeLine({{5.0, -2.0}, {5.0, 2.0}});
  TaggedLineString tls_a(line_a.get(), 2, false);
  TaggedLineString tls_b(line_b.get(), 2, false);

  LineSegmentIndex input_index;
  LineSegmentIndex output_index;
  input_index.add(tls_a);
  input_index.add(tls_b);
  std::vector<TaggedLineString*> components{&tls_a, &tls_b};
  ComponentJumpChecker jump_checker(components);
  TaggedLineStringSimplifier simplifier(&input_index, &output_index,
                                        &jump_checker);
  simplifier.simplify(&tls_a, 1.0);

  ExpectResultCoords(tls_a, {{0.0, 0.0}, {5.0, 0.5}, {10.0, 0.0}});
}

TEST_F(TaggedLineStringSimplifierTest,
       OutputIntersectionBlocksInvalidFlatteningAndAllowsSharedEndpoint) {
  // Within the same line, flattening the right section across the already-
  // flattened left section in outputIndex is rejected (even though the
  // flattened left section is disjoint from the original input segments).
  std::unique_ptr<LineString> line = MakeLine({{5.0, 1.0},
                                               {5.5, 0.0},
                                               {5.0, -1.0},
                                               {12.0, -5.0},
                                               {2.0, -3.0},
                                               {2.0, 0.0},
                                               {5.0, 1.5},
                                               {5.8, 0.0}});
  TaggedLineString tls(line.get(), 2, false);
  SimplifySingle(tls, 1.5);
  ExpectResultCoords(tls, {{5.0, 1.0},
                           {5.0, -1.0},
                           {12.0, -5.0},
                           {2.0, -3.0},
                           {2.0, 0.0},
                           {5.0, 1.5},
                           {5.8, 0.0}});

  // Across multiple lines sharing input/output indexes: interior crossings,
  // collinear partial overlaps, and topologically equal segments (same or
  // reversed orientation) are blocked.
  std::unique_ptr<LineString> base_line =
      MakeLine({{0.0, 0.0}, {5.0, 0.2}, {10.0, 0.0}});
  std::unique_ptr<LineString> cross_line =
      MakeLine({{1.0, 0.8}, {-0.2, 0.0}, {1.0, -0.8}});
  std::unique_ptr<LineString> reversed_dup =
      MakeLine({{10.0, 0.0}, {5.0, -0.2}, {0.0, 0.0}});
  std::unique_ptr<LineString> forward_dup =
      MakeLine({{0.0, 0.0}, {5.0, -0.3}, {10.0, 0.0}});
  std::unique_ptr<LineString> collinear_overlap =
      MakeLine({{-2.0, 0.0}, {10.0, 0.4}, {22.0, 0.0}});

  TaggedLineString tls_base(base_line.get(), 2, false);
  TaggedLineString tls_cross(cross_line.get(), 2, false);
  TaggedLineString tls_rev(reversed_dup.get(), 2, false);
  TaggedLineString tls_fwd(forward_dup.get(), 2, false);
  TaggedLineString tls_overlap(collinear_overlap.get(), 2, false);

  std::vector<TaggedLineString*> components{&tls_base, &tls_cross, &tls_rev,
                                            &tls_fwd, &tls_overlap};
  SimplifyMulti(components, 2.0);

  ExpectResultCoords(tls_base, {{0.0, 0.0}, {10.0, 0.0}});
  ExpectResultCoords(tls_cross, {{1.0, 0.8}, {-0.2, 0.0}, {1.0, -0.8}});
  ExpectResultCoords(tls_rev, {{10.0, 0.0}, {5.0, -0.2}, {0.0, 0.0}});
  ExpectResultCoords(tls_fwd, {{0.0, 0.0}, {5.0, -0.3}, {10.0, 0.0}});
  ExpectResultCoords(tls_overlap, {{-2.0, 0.0}, {10.0, 0.4}, {22.0, 0.0}});

  // Endpoint of candidate segment landing on the interior of an output segment
  // (T-junction) is an interior intersection and is blocked.
  {
    std::unique_ptr<LineString> trunk =
        MakeLine({{0.0, 0.0}, {5.0, 0.2}, {10.0, 0.0}});
    TaggedLineString tls_trunk(trunk.get(), 2, false);
    LineSegmentIndex input_index;
    LineSegmentIndex output_index;
    input_index.add(tls_trunk);
    std::vector<TaggedLineString*> comps{&tls_trunk};
    ComponentJumpChecker jump_checker(comps);
    TaggedLineStringSimplifier trunk_simplifier(&input_index, &output_index,
                                                &jump_checker);
    trunk_simplifier.simplify(&tls_trunk, 2.0);
    ExpectResultCoords(tls_trunk, {{0.0, 0.0}, {10.0, 0.0}});

    std::unique_ptr<LineString> branch =
        MakeLine({{5.0, 0.0}, {5.2, 1.0}, {5.0, 2.0}});
    TaggedLineString tls_branch(branch.get(), 2, false);
    input_index.add(tls_branch);
    comps.push_back(&tls_branch);
    TaggedLineStringSimplifier branch_simplifier(&input_index, &output_index,
                                                 &jump_checker);
    branch_simplifier.simplify(&tls_branch, 2.0);
    ExpectResultCoords(tls_branch, {{5.0, 0.0}, {5.2, 1.0}, {5.0, 2.0}});
  }

  // Touching an existing output segment solely at a shared endpoint is allowed.
  {
    std::unique_ptr<LineString> seg_a =
        MakeLine({{0.0, 0.0}, {5.0, 0.2}, {10.0, 0.0}});
    std::unique_ptr<LineString> seg_b =
        MakeLine({{10.0, 0.0}, {15.0, 0.2}, {20.0, 0.0}});
    TaggedLineString tls_a(seg_a.get(), 2, false);
    TaggedLineString tls_b(seg_b.get(), 2, false);
    std::vector<TaggedLineString*> pair{&tls_a, &tls_b};
    SimplifyMulti(pair, 2.0);
    ExpectResultCoords(tls_a, {{0.0, 0.0}, {10.0, 0.0}});
    ExpectResultCoords(tls_b, {{10.0, 0.0}, {20.0, 0.0}});
  }
}

TEST_F(TaggedLineStringSimplifierTest,
       ComponentJumpCheckerBlocksSectionFlatteningAcrossComponent) {
  std::unique_ptr<LineString> outer_u =
      MakeLine({{0.0, 0.0}, {0.0, 10.0}, {10.0, 10.0}, {10.0, 0.0}});
  std::unique_ptr<LineString> inner_line =
      MakeLine({{4.0, 3.0}, {5.0, 3.0}, {6.0, 3.0}});

  TaggedLineString tls_outer(outer_u.get(), 2, false);
  TaggedLineString tls_inner(inner_line.get(), 2, false);

  std::vector<TaggedLineString*> components{&tls_outer, &tls_inner};
  SimplifyMulti(components, 20.0);

  ExpectResultCoords(tls_outer, {{0.0, 0.0}, {0.0, 10.0}, {10.0, 0.0}});
  ExpectResultCoords(tls_inner, {{4.0, 3.0}, {6.0, 3.0}});
}

TEST_F(TaggedLineStringSimplifierTest,
       SimplifyRingEndpointCollinearNonCollinearExceedsTolAndMinSize) {
  // Collinear ring endpoint is removed via isCollinear fast path.
  std::unique_ptr<LinearRing> collinear_ring = MakeRing({{5.0, 0.0},
                                                         {10.0, 0.0},
                                                         {10.0, 10.0},
                                                         {0.0, 10.0},
                                                         {0.0, 0.0},
                                                         {5.0, 0.0}});
  TaggedLineString tls_collinear(collinear_ring.get(), 4, true);
  SimplifySingle(tls_collinear, 1.0);
  ExpectResultCoords(
      tls_collinear,
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});

  // Non-collinear ring endpoint within tolerance is removed after full checks.
  std::unique_ptr<LinearRing> offset_ring = MakeRing({{5.0, 0.2},
                                                      {10.0, 0.0},
                                                      {10.0, 10.0},
                                                      {0.0, 10.0},
                                                      {0.0, 0.0},
                                                      {5.0, 0.2}});
  TaggedLineString tls_offset_ok(offset_ring.get(), 4, true);
  SimplifySingle(tls_offset_ok, 1.0);
  ExpectResultCoords(
      tls_offset_ok,
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});

  // Non-collinear ring endpoint exactly at distance tolerance is removed.
  std::unique_ptr<LinearRing> exact_tol_ring = MakeRing({{5.0, 1.0},
                                                         {10.0, 0.0},
                                                         {10.0, 10.0},
                                                         {0.0, 10.0},
                                                         {0.0, 0.0},
                                                         {5.0, 1.0}});
  TaggedLineString tls_exact_tol(exact_tol_ring.get(), 4, true);
  SimplifySingle(tls_exact_tol, 1.0);
  ExpectResultCoords(
      tls_exact_tol,
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});

  // Non-collinear ring endpoint exceeding tolerance is retained.
  TaggedLineString tls_offset_keep(offset_ring.get(), 4, true);
  SimplifySingle(tls_offset_keep, 0.1);
  ExpectResultCoords(tls_offset_keep, {{5.0, 0.2},
                                       {10.0, 0.0},
                                       {10.0, 10.0},
                                       {0.0, 10.0},
                                       {0.0, 0.0},
                                       {5.0, 0.2}});

  // Triangle ring already at minimumSize (4) skips endpoint removal even with
  // large tolerance.
  std::unique_ptr<LinearRing> tri_ring =
      MakeRing({{0.0, 0.0}, {10.0, 0.0}, {5.0, 10.0}, {0.0, 0.0}});
  TaggedLineString tls_tri(tri_ring.get(), 4, true);
  SimplifySingle(tls_tri, 100.0);
  ExpectResultCoords(tls_tri,
                     {{0.0, 0.0}, {10.0, 0.0}, {5.0, 10.0}, {0.0, 0.0}});
}

TEST_F(TaggedLineStringSimplifierTest,
       SimplifyRingEndpointTopologyValidationBlocksInvalidRemoval) {
  std::unique_ptr<LinearRing> ring = MakeRing({{5.0, 0.2},
                                               {10.0, 0.0},
                                               {10.0, 10.0},
                                               {0.0, 10.0},
                                               {0.0, 0.0},
                                               {5.0, 0.2}});

  // Blocked by outputIndex intersection.
  {
    std::unique_ptr<LineString> blocker =
        MakeLine({{5.0, -2.0}, {5.2, -1.0}, {5.0, 0.0}});
    TaggedLineString tls_blocker(blocker.get(), 2, false);
    TaggedLineString tls_ring(ring.get(), 4, true);
    std::vector<TaggedLineString*> components{&tls_blocker, &tls_ring};
    SimplifyMulti(components, 1.0);
    ExpectResultCoords(tls_blocker, {{5.0, -2.0}, {5.0, 0.0}});
    EXPECT_EQ(tls_ring.getResultSize(), 6u);
  }

  // Blocked by inputIndex intersection (outputIndex has no crossing edge yet).
  {
    std::unique_ptr<LineString> blocker = MakeLine({{5.0, -2.0}, {5.0, 0.0}});
    TaggedLineString tls_ring(ring.get(), 4, true);
    TaggedLineString tls_blocker(blocker.get(), 2, false);
    std::vector<TaggedLineString*> components{&tls_ring, &tls_blocker};
    SimplifyMulti(components, 1.0);
    EXPECT_EQ(tls_ring.getResultSize(), 6u);
  }

  // Blocked by ComponentJumpChecker (no segment intersections).
  {
    std::unique_ptr<LinearRing> tent_ring = MakeRing({{5.0, 0.5},
                                                      {10.0, 0.0},
                                                      {10.0, 10.0},
                                                      {0.0, 10.0},
                                                      {0.0, 0.0},
                                                      {5.0, 0.5}});
    std::unique_ptr<LineString> inner_comp =
        MakeLine({{4.5, 0.1}, {5.0, 0.2}, {5.5, 0.1}});
    TaggedLineString tls_ring(tent_ring.get(), 4, true);
    TaggedLineString tls_inner(inner_comp.get(), 2, false);
    std::vector<TaggedLineString*> components{&tls_ring, &tls_inner};
    SimplifyMulti(components, 1.0);
    EXPECT_EQ(tls_ring.getResultSize(), 6u);
  }
}

TEST_F(TaggedLineStringSimplifierTest,
       SimplifyRingEndpointUpdatesIndexesWhenMergingFlattenedSegments) {
  std::unique_ptr<LinearRing> ring = MakeRing({{5.0, 0.1},
                                               {7.5, 0.08},
                                               {10.0, 0.0},
                                               {10.0, 10.0},
                                               {0.0, 10.0},
                                               {0.0, 0.0},
                                               {2.5, 0.08},
                                               {5.0, 0.1}});
  TaggedLineString tls_ring(ring.get(), 4, true);

  LineSegmentIndex input_index;
  LineSegmentIndex output_index;
  input_index.add(tls_ring);
  std::vector<TaggedLineString*> components{&tls_ring};
  ComponentJumpChecker jump_checker(components);
  TaggedLineStringSimplifier simplifier(&input_index, &output_index,
                                        &jump_checker);
  simplifier.simplify(&tls_ring, 1.0);

  ExpectResultCoords(
      tls_ring,
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});

  // Old flattened first/last segments were removed from output_index and the
  // merged segment (0,0)->(10,0) was added.
  LineSegment merged_probe(0.0, 0.0, 10.0, 0.0);
  std::vector<const LineSegment*> out_hits = output_index.query(&merged_probe);
  ASSERT_EQ(out_hits.size(), 1u);
  EXPECT_TRUE(out_hits[0]->equalsTopo(merged_probe));

  // A subsequent line crossing the merged segment is blocked by output_index.
  std::unique_ptr<LineString> crossing_line =
      MakeLine({{2.0, -2.0}, {2.1, -1.0}, {2.0, 0.0}});
  TaggedLineString tls_cross(crossing_line.get(), 2, false);
  input_index.add(tls_cross);
  components.push_back(&tls_cross);
  TaggedLineStringSimplifier cross_simplifier(&input_index, &output_index,
                                              &jump_checker);
  cross_simplifier.simplify(&tls_cross, 1.0);
  ExpectResultCoords(tls_cross, {{2.0, -2.0}, {2.1, -1.0}, {2.0, 0.0}});

  // A subsequent line crossing only the stale removed first/last segments' old
  // y-band succeeds because both segments were cleanly removed from indexes.
  std::unique_ptr<LineString> above_merged =
      MakeLine({{4.5, 0.095}, {5.0, 0.09}, {5.5, 0.095}});
  TaggedLineString tls_above(above_merged.get(), 2, false);
  input_index.add(tls_above);
  components.push_back(&tls_above);
  TaggedLineStringSimplifier above_simplifier(&input_index, &output_index,
                                              &jump_checker);
  above_simplifier.simplify(&tls_above, 1.0);
  ExpectResultCoords(tls_above, {{4.5, 0.095}, {5.5, 0.095}});
}

TEST_F(TaggedLineStringSimplifierTest,
       SimplifyRingEndpointUpdatesIndexesWhenMergingOriginalSegments) {
  std::unique_ptr<LinearRing> ring = MakeRing({{5.0, 0.2},
                                               {10.0, 0.0},
                                               {10.0, 10.0},
                                               {0.0, 10.0},
                                               {0.0, 0.0},
                                               {5.0, 0.2}});
  TaggedLineString tls_ring(ring.get(), 4, true);

  LineSegmentIndex input_index;
  LineSegmentIndex output_index;
  input_index.add(tls_ring);
  std::vector<TaggedLineString*> components{&tls_ring};
  ComponentJumpChecker jump_checker(components);
  TaggedLineStringSimplifier simplifier(&input_index, &output_index,
                                        &jump_checker);
  simplifier.simplify(&tls_ring, 1.0);

  ExpectResultCoords(
      tls_ring,
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});

  // Single-segment leaves original segment pointers in input_index (while
  // resultSegs holds distinct copies), so input_index still retains the two
  // original endpoint segments; meanwhile, the merged segment (0,0)->(10,0) is
  // added to output_index.
  LineSegment peak_probe(4.0, 0.1, 6.0, 0.2);
  EXPECT_EQ(input_index.query(&peak_probe).size(), 2u);

  LineSegment merged_probe(0.0, 0.0, 10.0, 0.0);
  std::vector<const LineSegment*> out_hits = output_index.query(&merged_probe);
  ASSERT_EQ(out_hits.size(), 1u);
  EXPECT_TRUE(out_hits[0]->equalsTopo(merged_probe));

  // A subsequent line crossing the merged segment (disjoint from the retained
  // original segments) is blocked by output_index.
  std::unique_ptr<LineString> crossing_merged =
      MakeLine({{2.0, -1.0}, {2.5, -0.5}, {2.0, 0.05}});
  TaggedLineString tls_cross(crossing_merged.get(), 2, false);
  input_index.add(tls_cross);
  components.push_back(&tls_cross);
  TaggedLineStringSimplifier cross_simplifier(&input_index, &output_index,
                                              &jump_checker);
  cross_simplifier.simplify(&tls_cross, 1.0);
  ExpectResultCoords(tls_cross, {{2.0, -1.0}, {2.5, -0.5}, {2.0, 0.05}});

  // A subsequent line crossing the retained original peak segments at y = 0.18
  // is still blocked by input_index.
  std::unique_ptr<LineString> above_peak =
      MakeLine({{4.0, 0.18}, {5.0, 0.10}, {6.0, 0.18}});
  TaggedLineString tls_above(above_peak.get(), 2, false);
  input_index.add(tls_above);
  components.push_back(&tls_above);
  TaggedLineStringSimplifier above_simplifier(&input_index, &output_index,
                                              &jump_checker);
  above_simplifier.simplify(&tls_above, 1.0);
  ExpectResultCoords(tls_above, {{4.0, 0.18}, {5.0, 0.10}, {6.0, 0.18}});
}

TEST_F(TaggedLineStringSimplifierTest,
       SimplifyRingEndpointMergesMixedFlattenedAndOriginalSegments) {
  std::unique_ptr<LinearRing> ring = MakeRing({{5.0, 0.1},
                                               {7.5, 0.08},
                                               {10.0, 0.0},
                                               {10.0, 10.0},
                                               {0.0, 10.0},
                                               {0.0, 0.0},
                                               {5.0, 0.1}});
  TaggedLineString tls_ring(ring.get(), 4, true);

  LineSegmentIndex input_index;
  LineSegmentIndex output_index;
  input_index.add(tls_ring);
  std::vector<TaggedLineString*> components{&tls_ring};
  ComponentJumpChecker jump_checker(components);
  TaggedLineStringSimplifier simplifier(&input_index, &output_index,
                                        &jump_checker);
  simplifier.simplify(&tls_ring, 1.0);

  ExpectResultCoords(
      tls_ring,
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});

  LineSegment merged_probe(0.0, 0.0, 10.0, 0.0);
  std::vector<const LineSegment*> out_hits = output_index.query(&merged_probe);
  ASSERT_EQ(out_hits.size(), 1u);
  EXPECT_TRUE(out_hits[0]->equalsTopo(merged_probe));

  // The flattened first segment (5,0.1)->(10,0) was removed from output_index
  // and its underlying original segments were already removed from input_index.
  LineSegment stale_first_probe(7.5, 0.05, 8.0, 0.06);
  EXPECT_TRUE(output_index.query(&stale_first_probe).empty());
  EXPECT_TRUE(input_index.query(&stale_first_probe).empty());

  std::unique_ptr<LineString> right_band =
      MakeLine({{6.0, 0.06}, {7.5, 0.03}, {9.0, 0.06}});
  TaggedLineString tls_right(right_band.get(), 2, false);
  input_index.add(tls_right);
  components.push_back(&tls_right);
  TaggedLineStringSimplifier right_simplifier(&input_index, &output_index,
                                              &jump_checker);
  right_simplifier.simplify(&tls_right, 1.0);
  ExpectResultCoords(tls_right, {{6.0, 0.06}, {9.0, 0.06}});
}

}  // namespace
}  // namespace geos_google
