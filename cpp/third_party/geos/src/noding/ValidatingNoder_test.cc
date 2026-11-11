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

// Unit tests for third_party/geos/src/noding/ValidatingNoder.cpp
// (and third_party/geos/include/geos/noding/ValidatingNoder.h).
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/noding/ValidatingNoder.cpp

#include "third_party/geos/include/geos/noding/ValidatingNoder.h"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/noding/IntersectionAdder.h"
#include "third_party/geos/include/geos/noding/MCIndexNoder.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"
#include "third_party/geos/include/geos/noding/Noder.h"
#include "third_party/geos/include/geos/noding/SegmentIntersector.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/noding/SimpleNoder.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::algorithm::LineIntersector;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::noding::IntersectionAdder;
using geos::noding::MCIndexNoder;
using geos::noding::NodedSegmentString;
using geos::noding::Noder;
using geos::noding::SegmentIntersector;
using geos::noding::SegmentString;
using geos::noding::SimpleNoder;
using geos::noding::ValidatingNoder;
using geos::util::TopologyException;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
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

  const SegmentString::NonConstVect* raw() const { return ptr_; }
  bool empty() const { return ptr_->empty(); }
  std::size_t size() const { return ptr_->size(); }
  const SegmentString* operator[](std::size_t i) const { return (*ptr_)[i]; }
  const SegmentString::NonConstVect& vec() const { return *ptr_; }

 private:
  SegmentString::NonConstVect* ptr_;
};

std::unique_ptr<NodedSegmentString> MakeNodedSS(
    std::initializer_list<CoordinateXY> pts, const void* context = nullptr) {
  auto seq = std::make_unique<CoordinateSequence>(pts);
  return std::make_unique<NodedSegmentString>(seq.release(), false, false,
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

// SegmentIntersector that intentionally records no nodes, leaving crossing
// SegmentStrings un-split when used with SimpleNoder/MCIndexNoder.
class NoOpSegmentIntersector final : public SegmentIntersector {
 public:
  void processIntersections(SegmentString*, std::size_t, SegmentString*,
                            std::size_t) override {}
};

// Fake Noder that forwards inputs unchanged as newly allocated
// NodedSegmentStrings so ValidatingNoder's delegation and error cleanup paths
// can be tested directly.
class PassthroughFakeNoder final : public Noder {
 public:
  explicit PassthroughFakeNoder(bool throw_on_compute = false)
      : throw_on_compute_(throw_on_compute) {}

  void computeNodes(SegmentString::NonConstVect* seg_strings) override {
    last_input_ = seg_strings;
    ++compute_calls_;
    if (throw_on_compute_) {
      throw std::runtime_error("delegate computeNodes failed");
    }
    inputs_ = *seg_strings;
  }

  SegmentString::NonConstVect* getNodedSubstrings() const override {
    ++get_substrings_calls_;
    auto* out = new SegmentString::NonConstVect();
    out->reserve(inputs_.size());
    for (const SegmentString* ss : inputs_) {
      out->push_back(
          new NodedSegmentString(ss->getCoordinates()->clone().release(), false,
                                 false, ss->getData()));
    }
    return out;
  }

  const SegmentString::NonConstVect* last_input() const { return last_input_; }
  int compute_calls() const { return compute_calls_; }
  int get_substrings_calls() const { return get_substrings_calls_; }

 private:
  bool throw_on_compute_;
  SegmentString::NonConstVect* last_input_ = nullptr;
  SegmentString::NonConstVect inputs_;
  int compute_calls_ = 0;
  mutable int get_substrings_calls_ = 0;
};

TEST(ValidatingNoderTest, ValidEmptyInput) {
  LineIntersector li;
  IntersectionAdder adder(li);
  SimpleNoder inner(&adder);
  ValidatingNoder validating(inner);

  SegmentString::NonConstVect empty_inputs;
  validating.computeNodes(&empty_inputs);

  ScopedNodedSubstrings out(validating.getNodedSubstrings());
  ASSERT_NE(out.raw(), nullptr);
  EXPECT_TRUE(out.empty());
}

TEST(ValidatingNoderTest, DelegatesToInnerNoderAndValidatesDisjointInput) {
  const int tag0 = 10;
  const int tag1 = 20;

  std::vector<std::unique_ptr<NodedSegmentString>> owned;
  owned.push_back(MakeNodedSS({{0.0, 0.0}, {1.0, 1.0}}, &tag0));
  owned.push_back(MakeNodedSS({{5.0, 5.0}, {6.0, 6.0}}, &tag1));
  SegmentString::NonConstVect inputs = ToRawPtrs(owned);

  PassthroughFakeNoder fake;
  ValidatingNoder validating(fake);
  validating.computeNodes(&inputs);

  EXPECT_EQ(fake.last_input(), &inputs);
  EXPECT_EQ(fake.compute_calls(), 1);
  EXPECT_EQ(fake.get_substrings_calls(), 1);

  ScopedNodedSubstrings out(validating.getNodedSubstrings());
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0]->getData(), static_cast<const void*>(&tag0));
  EXPECT_EQ(out[1]->getData(), static_cast<const void*>(&tag1));
}

TEST(ValidatingNoderTest,
     ValidatesProperlyNodedCrossingSegmentsWithSimpleNoder) {
  const int tag0 = 1;
  const int tag1 = 2;

  std::vector<std::unique_ptr<NodedSegmentString>> owned;
  owned.push_back(MakeNodedSS({{0.0, 0.0}, {10.0, 10.0}}, &tag0));
  owned.push_back(MakeNodedSS({{0.0, 10.0}, {10.0, 0.0}}, &tag1));
  SegmentString::NonConstVect inputs = ToRawPtrs(owned);

  LineIntersector li;
  IntersectionAdder adder(li);
  SimpleNoder inner(&adder);
  ValidatingNoder validating(inner);

  validating.computeNodes(&inputs);

  ScopedNodedSubstrings out(validating.getNodedSubstrings());
  ASSERT_EQ(out.size(), 4u);
  EXPECT_THAT(
      CollectAllCoords2D(out.vec()),
      UnorderedElementsAre(
          ElementsAre(CoordinateXY(0.0, 0.0), CoordinateXY(5.0, 5.0)),
          ElementsAre(CoordinateXY(5.0, 5.0), CoordinateXY(10.0, 10.0)),
          ElementsAre(CoordinateXY(0.0, 10.0), CoordinateXY(5.0, 5.0)),
          ElementsAre(CoordinateXY(5.0, 5.0), CoordinateXY(10.0, 0.0))));
}

TEST(ValidatingNoderTest, ValidatesComplexArrangementWithMCIndexNoder) {
  std::vector<std::unique_ptr<NodedSegmentString>> owned;
  owned.push_back(MakeNodedSS({{0.0, 0.0}, {10.0, 0.0}}));
  owned.push_back(MakeNodedSS({{4.0, 0.0}, {14.0, 0.0}}));
  owned.push_back(MakeNodedSS({{5.0, -5.0}, {5.0, 5.0}}));
  SegmentString::NonConstVect inputs = ToRawPtrs(owned);

  LineIntersector li;
  IntersectionAdder adder(li);
  MCIndexNoder inner(&adder);
  ValidatingNoder validating(inner);

  validating.computeNodes(&inputs);

  ScopedNodedSubstrings out(validating.getNodedSubstrings());
  ASSERT_EQ(out.size(), 8u);
}

TEST(ValidatingNoderTest, AcceptsEndpointSharedAndCoincidentSegments) {
  std::vector<std::unique_ptr<NodedSegmentString>> owned;
  owned.push_back(MakeNodedSS({{0.0, 0.0}, {5.0, 0.0}}));
  owned.push_back(MakeNodedSS({{5.0, 0.0}, {10.0, 0.0}}));
  owned.push_back(MakeNodedSS({{0.0, 0.0}, {5.0, 0.0}}));
  SegmentString::NonConstVect inputs = ToRawPtrs(owned);

  PassthroughFakeNoder fake;
  ValidatingNoder validating(fake);
  validating.computeNodes(&inputs);

  // Explicit validate() call remains valid and keeps noded substrings intact.
  EXPECT_NO_THROW(validating.validate());
  ScopedNodedSubstrings out(validating.getNodedSubstrings());
  ASSERT_EQ(out.size(), 3u);
}

TEST(ValidatingNoderTest,
     ThrowsAndCleansUpOnProperIntersectionWithSimpleNoder) {
  std::vector<std::unique_ptr<NodedSegmentString>> owned;
  owned.push_back(MakeNodedSS({{0.0, 0.0}, {10.0, 10.0}}));
  owned.push_back(MakeNodedSS({{0.0, 10.0}, {10.0, 0.0}}));
  owned.push_back(MakeNodedSS({{100.0, 100.0}, {105.0, 105.0}}));
  SegmentString::NonConstVect inputs = ToRawPtrs(owned);

  NoOpSegmentIntersector noop;
  SimpleNoder inner(&noop);
  ValidatingNoder validating(inner);

  try {
    validating.computeNodes(&inputs);
    FAIL() << "Expected TopologyException for un-noded crossing segments";
  } catch (TopologyException& ex) {
    EXPECT_THAT(ex.what(), HasSubstr("non-noded intersection"));
    EXPECT_TRUE(ex.getCoordinate().equals2D(CoordinateXY(5.0, 5.0)));
  }

  EXPECT_EQ(validating.getNodedSubstrings(), nullptr);
}

TEST(ValidatingNoderTest, ThrowsAndCleansUpOnInteriorVertexTJunction) {
  std::vector<std::unique_ptr<NodedSegmentString>> owned;
  owned.push_back(MakeNodedSS({{0.0, 5.0}, {10.0, 5.0}}));
  owned.push_back(MakeNodedSS({{5.0, 0.0}, {5.0, 5.0}}));
  SegmentString::NonConstVect inputs = ToRawPtrs(owned);

  PassthroughFakeNoder fake;
  ValidatingNoder validating(fake);

  try {
    validating.computeNodes(&inputs);
    FAIL() << "Expected TopologyException for un-noded T-junction";
  } catch (TopologyException& ex) {
    EXPECT_THAT(ex.what(), HasSubstr("non-noded intersection"));
    EXPECT_TRUE(ex.getCoordinate().equals2D(CoordinateXY(5.0, 5.0)));
  }

  EXPECT_EQ(validating.getNodedSubstrings(), nullptr);
}

TEST(ValidatingNoderTest, ThrowsAndCleansUpOnInteriorVertexToEndpointTouch) {
  std::vector<std::unique_ptr<NodedSegmentString>> owned;
  owned.push_back(MakeNodedSS({{0.0, 0.0}, {5.0, 5.0}, {10.0, 0.0}}));
  owned.push_back(MakeNodedSS({{5.0, 5.0}, {5.0, 10.0}}));
  SegmentString::NonConstVect inputs = ToRawPtrs(owned);

  PassthroughFakeNoder fake;
  ValidatingNoder validating(fake);

  EXPECT_THROW(validating.computeNodes(&inputs), TopologyException);
  EXPECT_EQ(validating.getNodedSubstrings(), nullptr);
}

TEST(ValidatingNoderTest, ThrowsAndCleansUpOnPartialCollinearOverlap) {
  std::vector<std::unique_ptr<NodedSegmentString>> owned;
  owned.push_back(MakeNodedSS({{0.0, 0.0}, {10.0, 0.0}}));
  owned.push_back(MakeNodedSS({{4.0, 0.0}, {14.0, 0.0}}));
  SegmentString::NonConstVect inputs = ToRawPtrs(owned);

  PassthroughFakeNoder fake;
  ValidatingNoder validating(fake);

  EXPECT_THROW(validating.computeNodes(&inputs), TopologyException);
  EXPECT_EQ(validating.getNodedSubstrings(), nullptr);
}

TEST(ValidatingNoderTest, PropagatesDelegateComputeNodesException) {
  std::vector<std::unique_ptr<NodedSegmentString>> owned;
  owned.push_back(MakeNodedSS({{0.0, 0.0}, {1.0, 1.0}}));
  SegmentString::NonConstVect inputs = ToRawPtrs(owned);

  PassthroughFakeNoder throwing_fake(/*throw_on_compute=*/true);
  ValidatingNoder validating(throwing_fake);

  EXPECT_THROW(validating.computeNodes(&inputs), std::runtime_error);
}

TEST(ValidatingNoderTest, CanReuseValidatingNoderAfterValidationFailure) {
  std::vector<std::unique_ptr<NodedSegmentString>> owned;
  owned.push_back(MakeNodedSS({{0.0, 0.0}, {10.0, 10.0}}));
  owned.push_back(MakeNodedSS({{0.0, 10.0}, {10.0, 0.0}}));
  SegmentString::NonConstVect inputs = ToRawPtrs(owned);

  NoOpSegmentIntersector noop;
  SimpleNoder inner(&noop);
  ValidatingNoder validating(inner);

  EXPECT_THROW(validating.computeNodes(&inputs), TopologyException);
  EXPECT_EQ(validating.getNodedSubstrings(), nullptr);

  LineIntersector li;
  IntersectionAdder adder(li);
  inner.setSegmentIntersector(&adder);

  validating.computeNodes(&inputs);
  ScopedNodedSubstrings out(validating.getNodedSubstrings());
  ASSERT_EQ(out.size(), 4u);
}

}  // namespace
}  // namespace geos_google
