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

// Unit tests for third_party/geos/src/noding/SimpleNoder.cpp
// (and third_party/geos/include/geos/noding/SimpleNoder.h).
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/noding/SimpleNoder.cpp

#include "third_party/geos/include/geos/noding/SimpleNoder.h"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/noding/BasicSegmentString.h"
#include "third_party/geos/include/geos/noding/FastNodingValidator.h"
#include "third_party/geos/include/geos/noding/IntersectionAdder.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"
#include "third_party/geos/include/geos/noding/SegmentIntersector.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/util/GEOSException.h"

namespace geos_google {
namespace {

using geos::algorithm::LineIntersector;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYZM;
using geos::noding::BasicSegmentString;
using geos::noding::FastNodingValidator;
using geos::noding::IntersectionAdder;
using geos::noding::NodedSegmentString;
using geos::noding::SegmentIntersector;
using geos::noding::SegmentString;
using geos::noding::SimpleNoder;
using geos::util::GEOSException;
using ::testing::ElementsAre;
using ::testing::UnorderedElementsAre;

// Owns the heap-allocated vector and SegmentString elements returned by
// Noder::getNodedSubstrings().
class ScopedNodedSubstrings {
 public:
  explicit ScopedNodedSubstrings(SegmentString::NonConstVect* ptr)
      : ptr_(ptr) {}

  ScopedNodedSubstrings(const ScopedNodedSubstrings&) = delete;
  ScopedNodedSubstrings& operator=(const ScopedNodedSubstrings&) = delete;

  ~ScopedNodedSubstrings() {
    if (ptr_ != nullptr) {
      for (SegmentString* ss : *ptr_) {
        delete ss;
      }
      delete ptr_;
    }
  }

  SegmentString::NonConstVect& vec() { return *ptr_; }
  const SegmentString::NonConstVect& vec() const { return *ptr_; }
  bool empty() const { return ptr_->empty(); }
  std::size_t size() const { return ptr_->size(); }
  const SegmentString* operator[](std::size_t i) const { return (*ptr_)[i]; }

 private:
  SegmentString::NonConstVect* ptr_;
};

std::unique_ptr<NodedSegmentString> MakeNodedSS(
    std::initializer_list<CoordinateXY> pts, const void* context = nullptr) {
  auto seq = std::make_unique<CoordinateSequence>(pts);
  return std::make_unique<NodedSegmentString>(seq.release(), false, false,
                                              context);
}

std::unique_ptr<NodedSegmentString> MakeNodedSSXYZM(
    std::initializer_list<CoordinateXYZM> pts, const void* context = nullptr) {
  auto seq = std::make_unique<CoordinateSequence>(pts);
  return std::make_unique<NodedSegmentString>(seq.release(), true, true,
                                              context);
}

SegmentString::NonConstVect ToRawPtrs(
    const std::vector<std::unique_ptr<NodedSegmentString>>& owned) {
  SegmentString::NonConstVect raw;
  raw.reserve(owned.size());
  for (const auto& item : owned) {
    raw.push_back(item.get());
  }
  return raw;
}

std::vector<CoordinateXY> ExtractCoords2D(const SegmentString* ss) {
  std::vector<CoordinateXY> out;
  out.reserve(ss->size());
  for (std::size_t i = 0; i < ss->size(); ++i) {
    out.push_back(ss->getCoordinate<CoordinateXY>(i));
  }
  return out;
}

std::vector<std::vector<CoordinateXY>> CollectAllCoords2D(
    const SegmentString::NonConstVect& substrings) {
  std::vector<std::vector<CoordinateXY>> result;
  result.reserve(substrings.size());
  for (const SegmentString* ss : substrings) {
    result.push_back(ExtractCoords2D(ss));
  }
  return result;
}

void ExpectValidNoding(SegmentString::NonConstVect& substrings) {
  FastNodingValidator validator(substrings);
  EXPECT_NO_THROW(validator.checkValid());
  EXPECT_TRUE(validator.isValid());
}

struct IntersectionCall {
  const SegmentString* e0;
  std::size_t i0;
  const SegmentString* e1;
  std::size_t i1;

  bool operator==(const IntersectionCall&) const = default;
};

class RecordingIntersector final : public SegmentIntersector {
 public:
  explicit RecordingIntersector(bool done = false) : done_(done) {}

  void processIntersections(SegmentString* e0, std::size_t seg_index0,
                            SegmentString* e1,
                            std::size_t seg_index1) override {
    calls_.push_back({e0, seg_index0, e1, seg_index1});
  }

  bool isDone() const override { return done_; }

  const std::vector<IntersectionCall>& calls() const { return calls_; }

 private:
  bool done_;
  std::vector<IntersectionCall> calls_;
};

class ThrowingIntersector final : public SegmentIntersector {
 public:
  void processIntersections(SegmentString*, std::size_t, SegmentString*,
                            std::size_t) override {
    throw GEOSException("intersector failure");
  }
};

TEST(SimpleNoderTest, EmptyInputProducesNoSubstringsWithDefaultIntersector) {
  SimpleNoder noder;
  SegmentString::NonConstVect empty_inputs;

  noder.computeNodes(&empty_inputs);

  ScopedNodedSubstrings out(noder.getNodedSubstrings());
  EXPECT_TRUE(out.empty());
}

TEST(SimpleNoderTest, SetSegmentIntersectorReplacesNullAndReusesNoder) {
  SimpleNoder noder;

  LineIntersector li;
  IntersectionAdder adder(li);
  noder.setSegmentIntersector(&adder);

  std::vector<std::unique_ptr<NodedSegmentString>> owned;
  owned.push_back(MakeNodedSS({{0.0, 0.0}, {10.0, 10.0}}));
  owned.push_back(MakeNodedSS({{0.0, 10.0}, {10.0, 0.0}}));
  SegmentString::NonConstVect inputs = ToRawPtrs(owned);

  noder.computeNodes(&inputs);
  ScopedNodedSubstrings first_out(noder.getNodedSubstrings());
  EXPECT_EQ(first_out.size(), 4u);
  ExpectValidNoding(first_out.vec());

  // Switch intersector and re-run computeNodes on a new input vector.
  RecordingIntersector recorder;
  noder.setSegmentIntersector(&recorder);

  std::vector<std::unique_ptr<NodedSegmentString>> second_owned;
  second_owned.push_back(MakeNodedSS({{0.0, 0.0}, {1.0, 0.0}}));
  SegmentString::NonConstVect second_inputs = ToRawPtrs(second_owned);

  noder.computeNodes(&second_inputs);
  EXPECT_EQ(recorder.calls().size(), 1u);
  ScopedNodedSubstrings second_out(noder.getNodedSubstrings());
  EXPECT_EQ(second_out.size(), 1u);
}

TEST(SimpleNoderTest, BruteForceVisitsEveryOrderedSegmentPairIgnoringIsDone) {
  // BasicSegmentString does not own its CoordinateSequence*, so sequences are
  // kept alive alongside the strings and only computeNodes() is called.
  CoordinateSequence seq_a(
      {CoordinateXY{0.0, 0.0}, CoordinateXY{1.0, 0.0}, CoordinateXY{2.0, 0.0}});
  CoordinateSequence seq_b(
      {CoordinateXY{10.0, 10.0}, CoordinateXY{11.0, 10.0}});
  BasicSegmentString str_a(&seq_a, nullptr);
  BasicSegmentString str_b(&seq_b, nullptr);

  SegmentString::NonConstVect inputs = {&str_a, &str_b};

  RecordingIntersector recorder(/*done=*/true);
  SimpleNoder noder(&recorder);
  noder.computeNodes(&inputs);

  // 3 total segments => 3 x 3 = 9 ordered segment pair calls, even with
  // disjoint coordinates and isDone() == true.
  EXPECT_THAT(recorder.calls(),
              ElementsAre(IntersectionCall{&str_a, 0, &str_a, 0},
                          IntersectionCall{&str_a, 0, &str_a, 1},
                          IntersectionCall{&str_a, 1, &str_a, 0},
                          IntersectionCall{&str_a, 1, &str_a, 1},
                          IntersectionCall{&str_a, 0, &str_b, 0},
                          IntersectionCall{&str_a, 1, &str_b, 0},
                          IntersectionCall{&str_b, 0, &str_a, 0},
                          IntersectionCall{&str_b, 0, &str_a, 1},
                          IntersectionCall{&str_b, 0, &str_b, 0}));
}

TEST(SimpleNoderTest, SinglePointSegmentStringHasZeroSegments) {
  RecordingIntersector recorder;
  SimpleNoder noder(&recorder);

  std::vector<std::unique_ptr<NodedSegmentString>> owned;
  owned.push_back(MakeNodedSS({{3.0, 4.0}}));
  SegmentString::NonConstVect inputs = ToRawPtrs(owned);

  noder.computeNodes(&inputs);
  EXPECT_TRUE(recorder.calls().empty());

  ScopedNodedSubstrings out(noder.getNodedSubstrings());
  EXPECT_TRUE(out.empty());
}

TEST(SimpleNoderTest, DisjointSegmentStringsRemainUnsplitAndPreserveContext) {
  const int tag0 = 101;
  const int tag1 = 202;

  std::vector<std::unique_ptr<NodedSegmentString>> owned;
  owned.push_back(MakeNodedSS({{0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}}, &tag0));
  owned.push_back(
      MakeNodedSS({{10.0, 10.0}, {11.0, 11.0}, {12.0, 10.0}}, &tag1));
  SegmentString::NonConstVect inputs = ToRawPtrs(owned);

  LineIntersector li;
  IntersectionAdder adder(li);
  SimpleNoder noder(&adder);
  noder.computeNodes(&inputs);

  ScopedNodedSubstrings out(noder.getNodedSubstrings());
  ASSERT_EQ(out.size(), 2u);
  EXPECT_FALSE(adder.hasIntersection());
  EXPECT_EQ(out[0]->getData(), static_cast<const void*>(&tag0));
  EXPECT_EQ(out[1]->getData(), static_cast<const void*>(&tag1));
  EXPECT_THAT(ExtractCoords2D(out[0]),
              ElementsAre(CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0),
                          CoordinateXY(2.0, 0.0)));
  EXPECT_THAT(ExtractCoords2D(out[1]),
              ElementsAre(CoordinateXY(10.0, 10.0), CoordinateXY(11.0, 11.0),
                          CoordinateXY(12.0, 10.0)));
  ExpectValidNoding(out.vec());
}

TEST(SimpleNoderTest, CrossingSegmentsSplitAtIntersectionAndKeepContext) {
  const std::string tag0 = "diag_up";
  const std::string tag1 = "diag_down";

  std::vector<std::unique_ptr<NodedSegmentString>> owned;
  owned.push_back(MakeNodedSS({{0.0, 0.0}, {10.0, 10.0}}, &tag0));
  owned.push_back(MakeNodedSS({{0.0, 10.0}, {10.0, 0.0}}, &tag1));
  SegmentString::NonConstVect inputs = ToRawPtrs(owned);

  LineIntersector li;
  IntersectionAdder adder(li);
  SimpleNoder noder(&adder);
  noder.computeNodes(&inputs);

  ScopedNodedSubstrings out(noder.getNodedSubstrings());
  ASSERT_EQ(out.size(), 4u);
  EXPECT_TRUE(adder.hasProperIntersection());
  ExpectValidNoding(out.vec());

  EXPECT_THAT(
      CollectAllCoords2D(out.vec()),
      UnorderedElementsAre(
          ElementsAre(CoordinateXY(0.0, 0.0), CoordinateXY(5.0, 5.0)),
          ElementsAre(CoordinateXY(5.0, 5.0), CoordinateXY(10.0, 10.0)),
          ElementsAre(CoordinateXY(0.0, 10.0), CoordinateXY(5.0, 5.0)),
          ElementsAre(CoordinateXY(5.0, 5.0), CoordinateXY(10.0, 0.0))));

  EXPECT_EQ(out[0]->getData(), static_cast<const void*>(&tag0));
  EXPECT_EQ(out[1]->getData(), static_cast<const void*>(&tag0));
  EXPECT_EQ(out[2]->getData(), static_cast<const void*>(&tag1));
  EXPECT_EQ(out[3]->getData(), static_cast<const void*>(&tag1));
}

TEST(SimpleNoderTest, MultiSegmentStringsWithMultipleIntersections) {
  std::vector<std::unique_ptr<NodedSegmentString>> owned;
  owned.push_back(
      MakeNodedSS({{0.0, 0.0}, {4.0, 10.0}, {8.0, 0.0}, {12.0, 10.0}}));
  owned.push_back(MakeNodedSS({{0.0, 5.0}, {12.0, 5.0}}));
  SegmentString::NonConstVect inputs = ToRawPtrs(owned);

  LineIntersector li;
  IntersectionAdder adder(li);
  SimpleNoder noder(&adder);
  noder.computeNodes(&inputs);

  ScopedNodedSubstrings out(noder.getNodedSubstrings());
  ASSERT_EQ(out.size(), 8u);
  ExpectValidNoding(out.vec());

  EXPECT_THAT(
      CollectAllCoords2D(out.vec()),
      UnorderedElementsAre(
          ElementsAre(CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 5.0)),
          ElementsAre(CoordinateXY(2.0, 5.0), CoordinateXY(4.0, 10.0),
                      CoordinateXY(6.0, 5.0)),
          ElementsAre(CoordinateXY(6.0, 5.0), CoordinateXY(8.0, 0.0),
                      CoordinateXY(10.0, 5.0)),
          ElementsAre(CoordinateXY(10.0, 5.0), CoordinateXY(12.0, 10.0)),
          ElementsAre(CoordinateXY(0.0, 5.0), CoordinateXY(2.0, 5.0)),
          ElementsAre(CoordinateXY(2.0, 5.0), CoordinateXY(6.0, 5.0)),
          ElementsAre(CoordinateXY(6.0, 5.0), CoordinateXY(10.0, 5.0)),
          ElementsAre(CoordinateXY(10.0, 5.0), CoordinateXY(12.0, 5.0))));
}

TEST(SimpleNoderTest, TJunctionEndpointOnInteriorSplitsTargetEdgeOnly) {
  const int horiz_tag = 1;
  const int stem_tag = 2;

  std::vector<std::unique_ptr<NodedSegmentString>> owned;
  owned.push_back(MakeNodedSS({{0.0, 5.0}, {10.0, 5.0}}, &horiz_tag));
  owned.push_back(MakeNodedSS({{5.0, 0.0}, {5.0, 5.0}}, &stem_tag));
  SegmentString::NonConstVect inputs = ToRawPtrs(owned);

  LineIntersector li;
  IntersectionAdder adder(li);
  SimpleNoder noder(&adder);
  noder.computeNodes(&inputs);

  ScopedNodedSubstrings out(noder.getNodedSubstrings());
  ASSERT_EQ(out.size(), 3u);
  EXPECT_TRUE(adder.hasInteriorIntersection());
  EXPECT_FALSE(adder.hasProperIntersection());
  ExpectValidNoding(out.vec());

  EXPECT_THAT(CollectAllCoords2D(out.vec()),
              UnorderedElementsAre(
                  ElementsAre(CoordinateXY(0.0, 5.0), CoordinateXY(5.0, 5.0)),
                  ElementsAre(CoordinateXY(5.0, 5.0), CoordinateXY(10.0, 5.0)),
                  ElementsAre(CoordinateXY(5.0, 0.0), CoordinateXY(5.0, 5.0))));
  EXPECT_EQ(out[0]->getData(), static_cast<const void*>(&horiz_tag));
  EXPECT_EQ(out[1]->getData(), static_cast<const void*>(&horiz_tag));
  EXPECT_EQ(out[2]->getData(), static_cast<const void*>(&stem_tag));
}

TEST(SimpleNoderTest, SharedEndpointDoesNotSplitSegments) {
  std::vector<std::unique_ptr<NodedSegmentString>> owned;
  owned.push_back(MakeNodedSS({{0.0, 0.0}, {5.0, 5.0}}));
  owned.push_back(MakeNodedSS({{5.0, 5.0}, {10.0, 0.0}}));
  SegmentString::NonConstVect inputs = ToRawPtrs(owned);

  LineIntersector li;
  IntersectionAdder adder(li);
  SimpleNoder noder(&adder);
  noder.computeNodes(&inputs);

  ScopedNodedSubstrings out(noder.getNodedSubstrings());
  ASSERT_EQ(out.size(), 2u);
  EXPECT_FALSE(adder.hasInteriorIntersection());
  ExpectValidNoding(out.vec());
}

TEST(SimpleNoderTest, CollinearOverlappingSegmentsSplitAtOverlapEndpoints) {
  std::vector<std::unique_ptr<NodedSegmentString>> owned;
  owned.push_back(MakeNodedSS({{0.0, 0.0}, {10.0, 0.0}}));
  owned.push_back(MakeNodedSS({{4.0, 0.0}, {14.0, 0.0}}));
  SegmentString::NonConstVect inputs = ToRawPtrs(owned);

  LineIntersector li;
  IntersectionAdder adder(li);
  SimpleNoder noder(&adder);
  noder.computeNodes(&inputs);

  ScopedNodedSubstrings out(noder.getNodedSubstrings());
  ASSERT_EQ(out.size(), 4u);
  ExpectValidNoding(out.vec());

  EXPECT_THAT(
      CollectAllCoords2D(out.vec()),
      UnorderedElementsAre(
          ElementsAre(CoordinateXY(0.0, 0.0), CoordinateXY(4.0, 0.0)),
          ElementsAre(CoordinateXY(4.0, 0.0), CoordinateXY(10.0, 0.0)),
          ElementsAre(CoordinateXY(4.0, 0.0), CoordinateXY(10.0, 0.0)),
          ElementsAre(CoordinateXY(10.0, 0.0), CoordinateXY(14.0, 0.0))));
}

TEST(SimpleNoderTest, SelfIntersectingSegmentStringIsNoded) {
  std::vector<std::unique_ptr<NodedSegmentString>> owned;
  owned.push_back(
      MakeNodedSS({{0.0, 0.0}, {10.0, 10.0}, {10.0, 0.0}, {0.0, 10.0}}));
  SegmentString::NonConstVect inputs = ToRawPtrs(owned);

  LineIntersector li;
  IntersectionAdder adder(li);
  SimpleNoder noder(&adder);
  noder.computeNodes(&inputs);

  ScopedNodedSubstrings out(noder.getNodedSubstrings());
  ASSERT_EQ(out.size(), 3u);
  ExpectValidNoding(out.vec());

  EXPECT_THAT(
      CollectAllCoords2D(out.vec()),
      UnorderedElementsAre(
          ElementsAre(CoordinateXY(0.0, 0.0), CoordinateXY(5.0, 5.0)),
          ElementsAre(CoordinateXY(5.0, 5.0), CoordinateXY(10.0, 10.0),
                      CoordinateXY(10.0, 0.0), CoordinateXY(5.0, 5.0)),
          ElementsAre(CoordinateXY(5.0, 5.0), CoordinateXY(0.0, 10.0))));
}

TEST(SimpleNoderTest, ClosedRingIntersectedByLine) {
  std::vector<std::unique_ptr<NodedSegmentString>> owned;
  owned.push_back(MakeNodedSS(
      {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}}));
  owned.push_back(MakeNodedSS({{5.0, -2.0}, {5.0, 12.0}}));
  SegmentString::NonConstVect inputs = ToRawPtrs(owned);

  LineIntersector li;
  IntersectionAdder adder(li);
  SimpleNoder noder(&adder);
  noder.computeNodes(&inputs);

  ScopedNodedSubstrings out(noder.getNodedSubstrings());
  ASSERT_EQ(out.size(), 6u);
  ExpectValidNoding(out.vec());

  EXPECT_THAT(
      CollectAllCoords2D(out.vec()),
      UnorderedElementsAre(
          ElementsAre(CoordinateXY(0.0, 0.0), CoordinateXY(5.0, 0.0)),
          ElementsAre(CoordinateXY(5.0, 0.0), CoordinateXY(10.0, 0.0),
                      CoordinateXY(10.0, 10.0), CoordinateXY(5.0, 10.0)),
          ElementsAre(CoordinateXY(5.0, 10.0), CoordinateXY(0.0, 10.0),
                      CoordinateXY(0.0, 0.0)),
          ElementsAre(CoordinateXY(5.0, -2.0), CoordinateXY(5.0, 0.0)),
          ElementsAre(CoordinateXY(5.0, 0.0), CoordinateXY(5.0, 10.0)),
          ElementsAre(CoordinateXY(5.0, 10.0), CoordinateXY(5.0, 12.0))));
}

TEST(SimpleNoderTest, InterpolatesZAndMOnIntersection) {
  std::vector<std::unique_ptr<NodedSegmentString>> owned;
  owned.push_back(
      MakeNodedSSXYZM({{0.0, 0.0, 0.0, 10.0}, {10.0, 10.0, 20.0, 30.0}}));
  owned.push_back(
      MakeNodedSSXYZM({{0.0, 10.0, 100.0, 200.0}, {10.0, 0.0, 100.0, 200.0}}));
  SegmentString::NonConstVect inputs = ToRawPtrs(owned);

  LineIntersector li;
  IntersectionAdder adder(li);
  SimpleNoder noder(&adder);
  noder.computeNodes(&inputs);

  ScopedNodedSubstrings out(noder.getNodedSubstrings());
  ASSERT_EQ(out.size(), 4u);
  ExpectValidNoding(out.vec());

  // At midpoint (5, 5): line 0 interpolates z=10, m=20; line 1 has z=100,
  // m=200; LineIntersector averages both segments to z=55, m=110.
  const CoordinateSequence* seq = out[0]->getCoordinates();
  ASSERT_EQ(seq->size(), 2u);
  EXPECT_TRUE(seq->hasZ());
  EXPECT_TRUE(seq->hasM());
  CoordinateXYZM mid;
  seq->getAt(1, mid);
  EXPECT_DOUBLE_EQ(mid.x, 5.0);
  EXPECT_DOUBLE_EQ(mid.y, 5.0);
  EXPECT_DOUBLE_EQ(mid.z, 55.0);
  EXPECT_DOUBLE_EQ(mid.m, 110.0);
}

TEST(SimpleNoderTest, PropagatesExceptionFromSegmentIntersector) {
  ThrowingIntersector throwing_intersector;
  SimpleNoder noder(&throwing_intersector);

  std::vector<std::unique_ptr<NodedSegmentString>> owned;
  owned.push_back(MakeNodedSS({{0.0, 0.0}, {1.0, 1.0}}));
  SegmentString::NonConstVect inputs = ToRawPtrs(owned);

  EXPECT_THROW(noder.computeNodes(&inputs), GEOSException);
}

}  // namespace
}  // namespace geos_google
