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

#include "third_party/geos/include/geos/simplify/ComponentJumpChecker.h"

#include <initializer_list>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineSegment.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/simplify/TaggedLineSegment.h"
#include "third_party/geos/include/geos/simplify/TaggedLineString.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::GeometryFactory;
using geos::geom::LineSegment;
using geos::geom::LineString;
using geos::simplify::ComponentJumpChecker;
using geos::simplify::TaggedLineSegment;
using geos::simplify::TaggedLineString;

class ComponentJumpCheckerTest : public ::testing::Test {
 protected:
  std::unique_ptr<LineString> MakeLineString(
      std::initializer_list<CoordinateXY> coords) const {
    auto seq = std::make_unique<CoordinateSequence>(0, false, false);
    seq->reserve(coords.size());
    for (const CoordinateXY& coord : coords) {
      seq->add(coord);
    }
    return factory_->createLineString(std::move(seq));
  }

  GeometryFactory::Ptr factory_ = GeometryFactory::create();
};

TEST_F(ComponentJumpCheckerTest, EmptyComponentsNeverJump) {
  auto line = MakeLineString({{0.0, 0.0}, {5.0, 10.0}, {10.0, 0.0}});
  TaggedLineString tls(line.get(), 2, false);
  std::vector<TaggedLineString*> components;
  ComponentJumpChecker checker(components);

  LineSegment flat_seg(0.0, 0.0, 10.0, 0.0);
  LineSegment seg1(0.0, 0.0, 5.0, 10.0);
  LineSegment seg2(5.0, 10.0, 10.0, 0.0);

  EXPECT_FALSE(checker.hasJump(&tls, 0, 2, flat_seg));
  EXPECT_FALSE(checker.hasJump(&tls, &seg1, &seg2, flat_seg));
}

TEST_F(ComponentJumpCheckerTest, SelfComponentIsSkipped) {
  auto line =
      MakeLineString({{0.0, 0.0}, {5.0, 5.0}, {10.0, 10.0}, {10.0, 0.0}});
  TaggedLineString tls(line.get(), 2, false);
  std::vector<TaggedLineString*> components = {&tls};
  ComponentJumpChecker checker(components);

  LineSegment flat_seg(0.0, 0.0, 10.0, 0.0);
  EXPECT_FALSE(checker.hasJump(&tls, 0, 3, flat_seg));

  LineSegment seg1(0.0, 0.0, 5.0, 5.0);
  LineSegment seg2(5.0, 5.0, 10.0, 0.0);
  EXPECT_FALSE(checker.hasJump(&tls, &seg1, &seg2, flat_seg));
}

TEST_F(ComponentJumpCheckerTest, SectionHasJumpWhenComponentIsInsideTriangle) {
  auto line = MakeLineString({{0.0, 0.0}, {5.0, 10.0}, {10.0, 0.0}});
  // getComponentPoint() uses vertex index 1: (5.0, 4.0) inside the triangle.
  auto inside_comp = MakeLineString({{0.0, 0.0}, {5.0, 4.0}, {6.0, 4.0}});

  TaggedLineString line_tls(line.get(), 2, false);
  TaggedLineString comp_tls(inside_comp.get(), 2, false);
  std::vector<TaggedLineString*> components = {&line_tls, &comp_tls};
  ComponentJumpChecker checker(components);

  LineSegment flat_seg(0.0, 0.0, 10.0, 0.0);
  EXPECT_TRUE(checker.hasJump(&line_tls, 0, 2, flat_seg));
}

TEST_F(ComponentJumpCheckerTest, SectionNoJumpWhenComponentOutsideEnvelope) {
  auto line = MakeLineString({{0.0, 0.0}, {5.0, 10.0}, {10.0, 0.0}});
  auto outside_comp = MakeLineString({{19.0, 19.0}, {20.0, 20.0}});

  TaggedLineString line_tls(line.get(), 2, false);
  TaggedLineString comp_tls(outside_comp.get(), 2, false);
  std::vector<TaggedLineString*> components = {&line_tls, &comp_tls};
  ComponentJumpChecker checker(components);

  LineSegment flat_seg(0.0, 0.0, 10.0, 0.0);
  EXPECT_FALSE(checker.hasJump(&line_tls, 0, 2, flat_seg));
}

TEST_F(ComponentJumpCheckerTest,
       SectionNoJumpWhenComponentInsideEnvelopeWithSameCrossingParity) {
  auto triangle = MakeLineString({{0.0, 0.0}, {5.0, 10.0}, {10.0, 0.0}});
  // (1.0, 5.0) is inside [0, 10] x [0, 10] to the left of the triangle:
  // rightward ray crosses both edges (2 crossings) and flat segment (0).
  auto left_comp = MakeLineString({{0.0, 5.0}, {1.0, 5.0}});
  // (9.0, 5.0) is inside [0, 10] x [0, 10] to the right of the triangle:
  // rightward ray crosses 0 edges and 0 flat segments.
  auto right_comp = MakeLineString({{8.0, 5.0}, {9.0, 5.0}});

  TaggedLineString tri_tls(triangle.get(), 2, false);
  TaggedLineString left_tls(left_comp.get(), 2, false);
  TaggedLineString right_tls(right_comp.get(), 2, false);
  std::vector<TaggedLineString*> components = {&tri_tls, &left_tls, &right_tls};
  ComponentJumpChecker checker(components);

  EXPECT_FALSE(
      checker.hasJump(&tri_tls, 0, 2, LineSegment(0.0, 0.0, 10.0, 0.0)));

  // Both section and flat segment cross rightward ray once (odd vs odd => no
  // jump).
  auto bent = MakeLineString({{0.0, 0.0}, {8.0, 5.0}, {10.0, 10.0}});
  TaggedLineString bent_tls(bent.get(), 2, false);
  std::vector<TaggedLineString*> odd_components = {&bent_tls, &left_tls};
  ComponentJumpChecker odd_checker(odd_components);
  EXPECT_FALSE(
      odd_checker.hasJump(&bent_tls, 0, 2, LineSegment(0.0, 0.0, 10.0, 10.0)));

  // Component between chord (5, 5) and bend (8, 5) is jumped across.
  auto between_comp = MakeLineString({{0.0, 0.0}, {6.0, 5.0}});
  TaggedLineString between_tls(between_comp.get(), 2, false);
  std::vector<TaggedLineString*> jump_components = {&bent_tls, &left_tls,
                                                    &between_tls};
  ComponentJumpChecker jump_checker(jump_components);
  EXPECT_TRUE(
      jump_checker.hasJump(&bent_tls, 0, 2, LineSegment(0.0, 0.0, 10.0, 10.0)));
}

TEST_F(ComponentJumpCheckerTest, SubrangeAndDegenerateStartEqualsEnd) {
  auto line = MakeLineString({
      {-10.0, 0.0},
      {0.0, 0.0},
      {4.0, 8.0},
      {8.0, 8.0},
      {10.0, 0.0},
      {20.0, 0.0},
  });
  // Outside subrange [1..4] envelope [0, 10] x [0, 8], though inside full line.
  auto outside_subrange = MakeLineString({{14.0, 0.0}, {15.0, 0.0}});
  // Inside trapezoid of subrange [1..4].
  auto inside_subrange = MakeLineString({{5.0, 0.0}, {5.0, 4.0}});

  TaggedLineString line_tls(line.get(), 2, false);
  TaggedLineString out_tls(outside_subrange.get(), 2, false);
  TaggedLineString in_tls(inside_subrange.get(), 2, false);

  std::vector<TaggedLineString*> only_out = {&line_tls, &out_tls};
  ComponentJumpChecker checker_out(only_out);
  LineSegment sub_flat(0.0, 0.0, 10.0, 0.0);
  EXPECT_FALSE(checker_out.hasJump(&line_tls, 1, 4, sub_flat));

  std::vector<TaggedLineString*> with_in = {&line_tls, &out_tls, &in_tls};
  ComponentJumpChecker checker_in(with_in);
  EXPECT_TRUE(checker_in.hasJump(&line_tls, 1, 4, sub_flat));

  // Degenerate start == end produces 0 crossings for both section and segment.
  auto at_vertex = MakeLineString({{1.0, 1.0}, {0.0, 0.0}});
  TaggedLineString at_vertex_tls(at_vertex.get(), 2, false);
  std::vector<TaggedLineString*> vertex_comps = {&line_tls, &at_vertex_tls};
  ComponentJumpChecker vertex_checker(vertex_comps);
  EXPECT_FALSE(
      vertex_checker.hasJump(&line_tls, 1, 1, LineSegment(0.0, 0.0, 0.0, 0.0)));
}

TEST_F(ComponentJumpCheckerTest, TwoSegmentOverloadDetectsAndRejectsJumps) {
  auto line =
      MakeLineString({{0.0, 0.0}, {10.0, 10.0}, {10.0, 0.0}, {0.0, 0.0}});
  TaggedLineString line_tls(line.get(), 4, true);

  // Outside [0, 10] x [0, 10].
  auto outside = MakeLineString({{14.0, 14.0}, {15.0, 15.0}});
  // Inside [0, 10] x [0, 10], outside triangle (2 crossings vs 0 crossings).
  auto inside_no_jump = MakeLineString({{0.0, 8.0}, {1.0, 8.0}});
  // Inside triangle (0,0)-(10,10)-(10,0): (7, 4) has 1 crossing vs 0.
  auto inside_jump = MakeLineString({{7.0, 0.0}, {7.0, 4.0}});

  TaggedLineString out_tls(outside.get(), 2, false);
  TaggedLineString no_jump_tls(inside_no_jump.get(), 2, false);
  TaggedLineString jump_tls(inside_jump.get(), 2, false);

  LineSegment seg1(0.0, 0.0, 10.0, 10.0);
  LineSegment seg2(10.0, 10.0, 10.0, 0.0);
  LineSegment flat_seg(0.0, 0.0, 10.0, 0.0);

  std::vector<TaggedLineString*> no_jump_comps = {&line_tls, &out_tls,
                                                  &no_jump_tls};
  ComponentJumpChecker no_jump_checker(no_jump_comps);
  EXPECT_FALSE(no_jump_checker.hasJump(&line_tls, &seg1, &seg2, flat_seg));

  std::vector<TaggedLineString*> jump_comps = {&line_tls, &out_tls,
                                               &no_jump_tls, &jump_tls};
  ComponentJumpChecker jump_checker(jump_comps);
  EXPECT_TRUE(jump_checker.hasJump(&line_tls, &seg1, &seg2, flat_seg));
}

TEST_F(ComponentJumpCheckerTest, UsesComponentResultPointWhenSimplified) {
  auto line = MakeLineString({{0.0, 0.0}, {5.0, 10.0}, {10.0, 0.0}});
  // Initial vertex index 1 is outside, while simplified resultSegs[0]->p0 is
  // inside.
  auto moved_in = MakeLineString({{50.0, 50.0}, {60.0, 60.0}});
  TaggedLineString line_tls(line.get(), 2, false);
  TaggedLineString moved_in_tls(moved_in.get(), 2, false);
  moved_in_tls.addToResult(std::make_unique<TaggedLineSegment>(
      Coordinate(5.0, 4.0), Coordinate(6.0, 4.0)));

  std::vector<TaggedLineString*> components = {&line_tls, &moved_in_tls};
  ComponentJumpChecker checker(components);
  EXPECT_TRUE(
      checker.hasJump(&line_tls, 0, 2, LineSegment(0.0, 0.0, 10.0, 0.0)));

  // Conversely, when resultSegs[0]->p0 is outside even if parent point was
  // inside.
  auto moved_out = MakeLineString({{0.0, 0.0}, {5.0, 4.0}});
  TaggedLineString moved_out_tls(moved_out.get(), 2, false);
  moved_out_tls.addToResult(std::make_unique<TaggedLineSegment>(
      Coordinate(50.0, 50.0), Coordinate(60.0, 60.0)));
  std::vector<TaggedLineString*> out_components = {&line_tls, &moved_out_tls};
  ComponentJumpChecker out_checker(out_components);
  EXPECT_FALSE(
      out_checker.hasJump(&line_tls, 0, 2, LineSegment(0.0, 0.0, 10.0, 0.0)));
}

}  // namespace
}  // namespace geos_google
