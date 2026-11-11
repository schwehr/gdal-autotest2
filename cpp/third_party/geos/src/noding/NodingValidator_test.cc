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

#include "third_party/geos/include/geos/noding/NodingValidator.h"

#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/noding/BasicSegmentString.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::noding::BasicSegmentString;
using ::geos::noding::NodingValidator;
using ::geos::noding::SegmentString;
using ::geos::util::TopologyException;
using ::testing::HasSubstr;

// Owns backing CoordinateSequences and BasicSegmentStrings whose addresses stay
// stable while NodingValidator inspects the SegmentString pointer vector.
class SegmentStringCollection {
 public:
  void Add(std::initializer_list<Coordinate> pts) {
    auto seq = std::make_unique<CoordinateSequence>(pts);
    auto ss = std::make_unique<BasicSegmentString>(seq.get(), nullptr);
    ptrs_.push_back(ss.get());
    seqs_.push_back(std::move(seq));
    strings_.push_back(std::move(ss));
  }

  const SegmentString::NonConstVect& ptrs() const { return ptrs_; }

 private:
  std::vector<std::unique_ptr<CoordinateSequence>> seqs_;
  std::vector<std::unique_ptr<BasicSegmentString>> strings_;
  SegmentString::NonConstVect ptrs_;
};

void ExpectInvalidNoding(const SegmentString::NonConstVect& ptrs,
                         std::string_view expected_substr) {
  NodingValidator validator(ptrs);
  try {
    validator.checkValid();
    FAIL() << "Expected TopologyException containing \"" << expected_substr
           << "\"";
  } catch (const TopologyException& e) {
    EXPECT_THAT(e.what(), HasSubstr(std::string(expected_substr)));
  }
}

TEST(NodingValidatorTest, EmptyCollectionIsValid) {
  const SegmentString::NonConstVect empty;
  NodingValidator validator(empty);
  EXPECT_NO_THROW(validator.checkValid());
}

TEST(NodingValidatorTest, ValidSingleAndDisjointSegmentStrings) {
  SegmentStringCollection single_seg;
  single_seg.Add({Coordinate(0.0, 0.0), Coordinate(3.0, 4.0)});
  NodingValidator single_validator(single_seg.ptrs());
  EXPECT_NO_THROW(single_validator.checkValid());
  // Re-running validation on the same validator instance stays valid.
  EXPECT_NO_THROW(single_validator.checkValid());

  SegmentStringCollection disjoint;
  disjoint.Add(
      {Coordinate(0.0, 0.0), Coordinate(1.0, 0.0), Coordinate(2.0, 1.0)});
  disjoint.Add(
      {Coordinate(10.0, 10.0), Coordinate(11.0, 10.0), Coordinate(12.0, 11.0)});
  NodingValidator disjoint_validator(disjoint.ptrs());
  EXPECT_NO_THROW(disjoint_validator.checkValid());
}

TEST(NodingValidatorTest, ValidNodedJunctionsRingsAndCoincidentSegments) {
  // Properly noded three-way junction meeting only at endpoints.
  SegmentStringCollection junction;
  junction.Add({Coordinate(0.0, 0.0), Coordinate(2.0, 0.0)});
  junction.Add({Coordinate(2.0, 0.0), Coordinate(4.0, 0.0)});
  junction.Add({Coordinate(2.0, 0.0), Coordinate(2.0, 3.0)});
  NodingValidator junction_validator(junction.ptrs());
  EXPECT_NO_THROW(junction_validator.checkValid());

  // Closed non-self-intersecting ring in a single SegmentString.
  SegmentStringCollection ring;
  ring.Add({Coordinate(0.0, 0.0), Coordinate(2.0, 0.0), Coordinate(2.0, 2.0),
            Coordinate(0.0, 2.0), Coordinate(0.0, 0.0)});
  NodingValidator ring_validator(ring.ptrs());
  EXPECT_NO_THROW(ring_validator.checkValid());

  // Two-part triangle sharing endpoints while keeping interior vertices free.
  SegmentStringCollection split_triangle;
  split_triangle.Add(
      {Coordinate(0.0, 0.0), Coordinate(3.0, 0.0), Coordinate(3.0, 4.0)});
  split_triangle.Add({Coordinate(0.0, 0.0), Coordinate(3.0, 4.0)});
  NodingValidator triangle_validator(split_triangle.ptrs());
  EXPECT_NO_THROW(triangle_validator.checkValid());

  // Duplicate coincident segments intersect only at their endpoints.
  SegmentStringCollection duplicate;
  duplicate.Add({Coordinate(0.0, 0.0), Coordinate(2.0, 0.0)});
  duplicate.Add({Coordinate(2.0, 0.0), Coordinate(0.0, 0.0)});
  NodingValidator duplicate_validator(duplicate.ptrs());
  EXPECT_NO_THROW(duplicate_validator.checkValid());
}

TEST(NodingValidatorTest, EndpointTouchingInteriorVertexThrows) {
  // Start endpoint of second string equals interior vertex of first string.
  SegmentStringCollection start_hit;
  start_hit.Add(
      {Coordinate(0.0, 0.0), Coordinate(1.0, 0.0), Coordinate(2.0, 0.0)});
  start_hit.Add({Coordinate(1.0, 0.0), Coordinate(1.0, 2.0)});
  ExpectInvalidNoding(start_hit.ptrs(),
                      "found endpt/interior pt intersection at index 1");

  // End endpoint of second string equals interior vertex of first string.
  SegmentStringCollection end_hit;
  end_hit.Add({Coordinate(0.0, 0.0), Coordinate(1.0, 0.0), Coordinate(2.0, 0.0),
               Coordinate(3.0, 0.0)});
  end_hit.Add({Coordinate(2.0, 5.0), Coordinate(2.0, 0.0)});
  ExpectInvalidNoding(end_hit.ptrs(),
                      "found endpt/interior pt intersection at index 2");

  // Self-lasso where start endpoint matches an interior vertex.
  SegmentStringCollection self_start_hit;
  self_start_hit.Add({Coordinate(0.0, 0.0), Coordinate(1.0, 0.0),
                      Coordinate(1.0, 1.0), Coordinate(0.0, 0.0),
                      Coordinate(-1.0, 0.0)});
  ExpectInvalidNoding(self_start_hit.ptrs(),
                      "found endpt/interior pt intersection at index 3");

  // Self-lasso where end endpoint matches an interior vertex.
  SegmentStringCollection self_end_hit;
  self_end_hit.Add({Coordinate(-1.0, 0.0), Coordinate(0.0, 0.0),
                    Coordinate(1.0, 0.0), Coordinate(1.0, 1.0),
                    Coordinate(0.0, 0.0)});
  ExpectInvalidNoding(self_end_hit.ptrs(),
                      "found endpt/interior pt intersection at index 1");

  // Vertex check ignores Z coordinates (2D equality).
  SegmentStringCollection diff_z;
  diff_z.Add({Coordinate(0.0, 0.0, 1.0), Coordinate(1.0, 0.0, 2.0),
              Coordinate(2.0, 0.0, 3.0)});
  diff_z.Add({Coordinate(1.0, 0.0, 99.0), Coordinate(1.0, 2.0, 99.0)});
  ExpectInvalidNoding(diff_z.ptrs(), "found endpt/interior pt intersection");
}

TEST(NodingValidatorTest, ProperInteriorIntersectionThrows) {
  // Crossing two-segment strings.
  SegmentStringCollection cross;
  cross.Add({Coordinate(0.0, 0.0), Coordinate(2.0, 2.0)});
  cross.Add({Coordinate(0.0, 2.0), Coordinate(2.0, 0.0)});
  ExpectInvalidNoding(cross.ptrs(), "found non-noded intersection");

  // Self-intersecting bowtie inside a single SegmentString.
  SegmentStringCollection bowtie;
  bowtie.Add({Coordinate(0.0, 0.0), Coordinate(2.0, 2.0), Coordinate(0.0, 2.0),
              Coordinate(2.0, 0.0)});
  ExpectInvalidNoding(bowtie.ptrs(), "found non-noded intersection");

  // Crossing occurring at non-zero segment indices.
  SegmentStringCollection multi_seg_cross;
  multi_seg_cross.Add(
      {Coordinate(0.0, 0.0), Coordinate(1.0, 0.0), Coordinate(3.0, 2.0)});
  multi_seg_cross.Add(
      {Coordinate(0.0, 3.0), Coordinate(1.0, 2.0), Coordinate(3.0, 0.0)});
  ExpectInvalidNoding(multi_seg_cross.ptrs(), "found non-noded intersection");
}

TEST(NodingValidatorTest, ImproperTAndCollinearIntersectionsThrow) {
  // T-intersection where endpoint of one segment lies on interior of another.
  // Test both input orderings so both hasInteriorIntersection checks execute.
  SegmentStringCollection t_base_first;
  t_base_first.Add({Coordinate(0.0, 0.0), Coordinate(4.0, 0.0)});
  t_base_first.Add({Coordinate(2.0, 0.0), Coordinate(2.0, 3.0)});
  ExpectInvalidNoding(t_base_first.ptrs(), "found non-noded intersection");

  SegmentStringCollection t_stem_first;
  t_stem_first.Add({Coordinate(2.0, 0.0), Coordinate(2.0, 3.0)});
  t_stem_first.Add({Coordinate(0.0, 0.0), Coordinate(4.0, 0.0)});
  ExpectInvalidNoding(t_stem_first.ptrs(), "found non-noded intersection");

  // Collinear overlap sharing one endpoint (second intersection point interior
  // to the longer segment). Test both orderings.
  SegmentStringCollection collinear_short_first;
  collinear_short_first.Add({Coordinate(0.0, 0.0), Coordinate(2.0, 0.0)});
  collinear_short_first.Add({Coordinate(0.0, 0.0), Coordinate(5.0, 0.0)});
  ExpectInvalidNoding(collinear_short_first.ptrs(),
                      "found non-noded intersection");

  SegmentStringCollection collinear_long_first;
  collinear_long_first.Add({Coordinate(0.0, 0.0), Coordinate(5.0, 0.0)});
  collinear_long_first.Add({Coordinate(0.0, 0.0), Coordinate(2.0, 0.0)});
  ExpectInvalidNoding(collinear_long_first.ptrs(),
                      "found non-noded intersection");

  // Strictly interior collinear containment.
  SegmentStringCollection collinear_contained;
  collinear_contained.Add({Coordinate(0.0, 0.0), Coordinate(6.0, 0.0)});
  collinear_contained.Add({Coordinate(1.0, 0.0), Coordinate(4.0, 0.0)});
  ExpectInvalidNoding(collinear_contained.ptrs(),
                      "found non-noded intersection");
}

TEST(NodingValidatorTest, CollapseThrows) {
  // Simple a-b-a backtrack collapse.
  SegmentStringCollection simple_collapse;
  simple_collapse.Add(
      {Coordinate(0.0, 0.0), Coordinate(1.0, 1.0), Coordinate(0.0, 0.0)});
  ExpectInvalidNoding(simple_collapse.ptrs(), "found non-noded collapse at");

  // Collapse starting at an interior index (i > 0) without triggering
  // endpoint/vertex or interior-intersection checks first.
  SegmentStringCollection interior_collapse;
  interior_collapse.Add({Coordinate(0.0, 0.0), Coordinate(1.0, 0.0),
                         Coordinate(1.0, 2.0), Coordinate(1.0, 0.0),
                         Coordinate(2.0, 0.0)});
  ExpectInvalidNoding(interior_collapse.ptrs(), "found non-noded collapse at");

  // Collapse detection uses 2D equality and ignores Z ordinates.
  SegmentStringCollection collapse_diff_z;
  collapse_diff_z.Add({Coordinate(0.0, 0.0, 1.0), Coordinate(1.0, 0.0, 2.0),
                       Coordinate(0.0, 0.0, 5.0)});
  ExpectInvalidNoding(collapse_diff_z.ptrs(), "found non-noded collapse at");
}

}  // namespace
}  // namespace geos_google
