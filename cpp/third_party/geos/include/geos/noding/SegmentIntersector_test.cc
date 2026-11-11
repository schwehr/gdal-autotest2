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

// Unit tests for third_party/geos/include/geos/noding/SegmentIntersector.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/noding/SegmentIntersector.h

#include "third_party/geos/include/geos/noding/SegmentIntersector.h"

#include <cstddef>
#include <initializer_list>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/noding/BasicSegmentString.h"
#include "third_party/geos/include/geos/noding/IntersectionAdder.h"
#include "third_party/geos/include/geos/noding/IntersectionFinderAdder.h"
#include "third_party/geos/include/geos/noding/MCIndexNoder.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"
#include "third_party/geos/include/geos/noding/NodingIntersectionFinder.h"
#include "third_party/geos/include/geos/noding/SegmentIntersectionDetector.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/noding/SimpleNoder.h"

namespace geos_google {
namespace {

using geos::algorithm::LineIntersector;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::noding::BasicSegmentString;
using geos::noding::IntersectionAdder;
using geos::noding::IntersectionFinderAdder;
using geos::noding::MCIndexNoder;
using geos::noding::NodedSegmentString;
using geos::noding::NodingIntersectionFinder;
using geos::noding::nodingSegmentIntersector;
using geos::noding::SegmentIntersectionDetector;
using geos::noding::SegmentIntersector;
using geos::noding::SegmentString;
using geos::noding::SimpleNoder;
using ::testing::IsEmpty;
using ::testing::Lt;
using ::testing::SizeIs;

static_assert(std::is_abstract_v<SegmentIntersector>);
static_assert(std::is_polymorphic_v<SegmentIntersector>);
static_assert(std::has_virtual_destructor_v<SegmentIntersector>);
static_assert(!std::is_default_constructible_v<SegmentIntersector>);
static_assert(!std::is_copy_constructible_v<SegmentIntersector>);
static_assert(std::is_same_v<nodingSegmentIntersector, SegmentIntersector>);
static_assert(std::is_same_v<
              decltype(std::declval<SegmentIntersector&>().processIntersections(
                  std::declval<SegmentString*>(), std::declval<std::size_t>(),
                  std::declval<SegmentString*>(), std::declval<std::size_t>())),
              void>);
static_assert(
    std::is_same_v<decltype(std::declval<const SegmentIntersector&>().isDone()),
                   bool>);
static_assert(std::is_base_of_v<SegmentIntersector, IntersectionAdder>);
static_assert(std::is_base_of_v<SegmentIntersector, IntersectionFinderAdder>);
static_assert(std::is_base_of_v<SegmentIntersector, NodingIntersectionFinder>);
static_assert(
    std::is_base_of_v<SegmentIntersector, SegmentIntersectionDetector>);

struct ProcessCall {
  const SegmentString* e0 = nullptr;
  std::size_t seg_index0 = 0;
  const SegmentString* e1 = nullptr;
  std::size_t seg_index1 = 0;
};

class DestructionFlag {
 public:
  explicit DestructionFlag(bool* destroyed) : destroyed_(destroyed) {}
  ~DestructionFlag() {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
  }

 private:
  bool* destroyed_;
};

// Relies on base SegmentIntersector::isDone() returning false and an implicitly
// declared virtual destructor to exercise base-class chaining.
class DefaultDoneSegmentIntersector final : public SegmentIntersector {
 public:
  explicit DefaultDoneSegmentIntersector(bool* destroyed = nullptr)
      : flag_(destroyed) {}

  void processIntersections(SegmentString* e0, std::size_t segIndex0,
                            SegmentString* e1, std::size_t segIndex1) override {
    calls_.push_back({e0, segIndex0, e1, segIndex1});
  }

  const std::vector<ProcessCall>& calls() const { return calls_; }

 private:
  std::vector<ProcessCall> calls_;
  DestructionFlag flag_;
};

class ConfigurableSegmentIntersector final : public SegmentIntersector {
 public:
  explicit ConfigurableSegmentIntersector(
      std::size_t stop_after = std::numeric_limits<std::size_t>::max(),
      bool* destroyed = nullptr)
      : stop_after_(stop_after), destroyed_(destroyed) {}

  ~ConfigurableSegmentIntersector() override {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
  }

  void processIntersections(SegmentString* e0, std::size_t segIndex0,
                            SegmentString* e1, std::size_t segIndex1) override {
    calls_.push_back({e0, segIndex0, e1, segIndex1});
  }

  bool isDone() const override { return calls_.size() >= stop_after_; }

  const std::vector<ProcessCall>& calls() const { return calls_; }

 private:
  std::size_t stop_after_;
  std::vector<ProcessCall> calls_;
  bool* destroyed_;
};

class OwnedBasicSegmentStrings {
 public:
  SegmentString* Add(std::initializer_list<Coordinate> coords) {
    auto seq = std::make_unique<CoordinateSequence>(coords);
    auto seg = std::make_unique<BasicSegmentString>(seq.get(), nullptr);
    SegmentString* raw = seg.get();
    sequences_.push_back(std::move(seq));
    strings_.push_back(std::move(seg));
    view_.push_back(raw);
    return raw;
  }

  SegmentString::NonConstVect* view() { return &view_; }

 private:
  std::vector<std::unique_ptr<CoordinateSequence>> sequences_;
  std::vector<std::unique_ptr<BasicSegmentString>> strings_;
  SegmentString::NonConstVect view_;
};

class OwnedNodedSegmentStrings {
 public:
  NodedSegmentString* Add(std::initializer_list<Coordinate> coords) {
    auto seq = std::make_unique<CoordinateSequence>(coords);
    auto seg = std::make_unique<NodedSegmentString>(seq.release(), false, false,
                                                    nullptr);
    NodedSegmentString* raw = seg.get();
    strings_.push_back(std::move(seg));
    view_.push_back(raw);
    return raw;
  }

  SegmentString::NonConstVect* view() { return &view_; }

 private:
  std::vector<std::unique_ptr<NodedSegmentString>> strings_;
  SegmentString::NonConstVect view_;
};

TEST(SegmentIntersectorTest, DefaultIsDoneAndPolymorphicLifecycle) {
  bool stack_destroyed = false;
  {
    DefaultDoneSegmentIntersector stack_si(&stack_destroyed);
    nodingSegmentIntersector& base_ref = stack_si;
    const SegmentIntersector& const_ref = base_ref;
    EXPECT_FALSE(const_ref.isDone());
    base_ref.processIntersections(nullptr, 0, nullptr, 0);
    EXPECT_FALSE(const_ref.isDone());
    EXPECT_THAT(stack_si.calls(), SizeIs(1));
  }
  EXPECT_TRUE(stack_destroyed);

  bool implicit_dtor_destroyed = false;
  bool explicit_dtor_destroyed = false;
  std::vector<std::unique_ptr<SegmentIntersector>> intersectors;
  intersectors.push_back(std::make_unique<DefaultDoneSegmentIntersector>(
      &implicit_dtor_destroyed));
  intersectors.push_back(std::make_unique<ConfigurableSegmentIntersector>(
      1, &explicit_dtor_destroyed));

  EXPECT_FALSE(intersectors[0]->isDone());
  EXPECT_FALSE(intersectors[1]->isDone());

  intersectors[0]->processIntersections(nullptr, 1, nullptr, 2);
  intersectors[1]->processIntersections(nullptr, 3, nullptr, 4);

  // Base implementation always returns false; derived override stops at 1.
  EXPECT_FALSE(intersectors[0]->isDone());
  EXPECT_TRUE(intersectors[1]->isDone());

  EXPECT_FALSE(implicit_dtor_destroyed);
  EXPECT_FALSE(explicit_dtor_destroyed);
  intersectors.clear();
  EXPECT_TRUE(implicit_dtor_destroyed);
  EXPECT_TRUE(explicit_dtor_destroyed);
}

TEST(SegmentIntersectorTest,
     PolymorphicProcessIntersectionsForwardsArgumentsAndStops) {
  OwnedBasicSegmentStrings strings;
  SegmentString* s0 = strings.Add({{0.0, 0.0}, {2.0, 0.0}, {4.0, 0.0}});
  SegmentString* s1 = strings.Add({{1.0, -1.0}, {1.0, 1.0}});

  ConfigurableSegmentIntersector impl(/*stop_after=*/2);
  SegmentIntersector* si = &impl;

  EXPECT_FALSE(si->isDone());
  si->processIntersections(s0, 0, s1, 0);
  EXPECT_FALSE(si->isDone());

  const std::size_t max_index = std::numeric_limits<std::size_t>::max();
  si->processIntersections(s0, 1, nullptr, max_index);
  EXPECT_TRUE(si->isDone());

  ASSERT_THAT(impl.calls(), SizeIs(2));
  EXPECT_EQ(impl.calls()[0].e0, s0);
  EXPECT_EQ(impl.calls()[0].seg_index0, 0u);
  EXPECT_EQ(impl.calls()[0].e1, s1);
  EXPECT_EQ(impl.calls()[0].seg_index1, 0u);

  EXPECT_EQ(impl.calls()[1].e0, s0);
  EXPECT_EQ(impl.calls()[1].seg_index0, 1u);
  EXPECT_EQ(impl.calls()[1].e1, nullptr);
  EXPECT_EQ(impl.calls()[1].seg_index1, max_index);
}

TEST(SegmentIntersectorTest, SimpleNoderDrivesSegmentIntersectorExhaustively) {
  // Empty input produces no calls.
  {
    DefaultDoneSegmentIntersector impl;
    SegmentString::NonConstVect empty_segs;
    SimpleNoder noder(&impl);
    noder.computeNodes(&empty_segs);
    EXPECT_THAT(impl.calls(), IsEmpty());
    EXPECT_FALSE(impl.isDone());
  }

  // Single polyline with 2 segments + one separate 1-segment string => 3 total
  // segments => SimpleNoder checks all 3 * 3 = 9 ordered pairs.
  {
    OwnedBasicSegmentStrings strings;
    SegmentString* s0 = strings.Add({{0.0, 0.0}, {2.0, 0.0}, {4.0, 0.0}});
    SegmentString* s1 = strings.Add({{1.0, -1.0}, {1.0, 1.0}});

    DefaultDoneSegmentIntersector impl;
    SegmentIntersector* base_si = &impl;
    SimpleNoder noder;
    noder.setSegmentIntersector(base_si);
    noder.computeNodes(strings.view());

    EXPECT_FALSE(base_si->isDone());
    ASSERT_THAT(impl.calls(), SizeIs(9));
    EXPECT_EQ(impl.calls()[0].e0, s0);
    EXPECT_EQ(impl.calls()[0].seg_index0, 0u);
    EXPECT_EQ(impl.calls()[0].e1, s0);
    EXPECT_EQ(impl.calls()[0].seg_index1, 0u);

    EXPECT_EQ(impl.calls()[4].e0, s0);
    EXPECT_EQ(impl.calls()[4].seg_index0, 0u);
    EXPECT_EQ(impl.calls()[4].e1, s1);
    EXPECT_EQ(impl.calls()[4].seg_index1, 0u);

    EXPECT_EQ(impl.calls()[8].e0, s1);
    EXPECT_EQ(impl.calls()[8].seg_index0, 0u);
    EXPECT_EQ(impl.calls()[8].e1, s1);
    EXPECT_EQ(impl.calls()[8].seg_index1, 0u);
  }
}

TEST(SegmentIntersectorTest, MCIndexNoderRespectsIsDoneEarlyTermination) {
  // 8 mutually overlapping segments produce 8 * 7 / 2 = 28 candidate pairs.
  OwnedBasicSegmentStrings strings;
  for (int i = 0; i < 8; ++i) {
    const double offset = static_cast<double>(i) * 0.1;
    strings.Add({{0.0, offset}, {10.0, 10.0 - offset}});
  }

  // Default isDone() returns false: visits all 28 overlapping candidate pairs.
  DefaultDoneSegmentIntersector exhaustive_impl;
  {
    MCIndexNoder exhaustive_noder(&exhaustive_impl);
    exhaustive_noder.computeNodes(strings.view());
    EXPECT_FALSE(exhaustive_impl.isDone());
    EXPECT_THAT(exhaustive_impl.calls(), SizeIs(28));
  }

  // Overridden isDone() aborts queryPairs sub-queries early after first call.
  ConfigurableSegmentIntersector early_stop_impl(/*stop_after=*/1);
  {
    MCIndexNoder early_noder(&early_stop_impl);
    early_noder.computeNodes(strings.view());
    EXPECT_TRUE(early_stop_impl.isDone());
    EXPECT_FALSE(early_stop_impl.calls().empty());
    EXPECT_THAT(early_stop_impl.calls(), SizeIs(Lt(std::size_t{28})));
  }
}

TEST(SegmentIntersectorTest, ConcreteGeosImplementationsThroughBaseInterface) {
  // SegmentIntersectionDetector via std::unique_ptr<SegmentIntersector>.
  {
    OwnedBasicSegmentStrings strings;
    SegmentString* s0 = strings.Add({{0.0, 0.0}, {4.0, 4.0}});
    SegmentString* s1 = strings.Add({{0.0, 4.0}, {4.0, 0.0}});

    LineIntersector li;
    auto detector = std::make_unique<SegmentIntersectionDetector>(&li);
    SegmentIntersectionDetector* detector_ptr = detector.get();
    std::unique_ptr<SegmentIntersector> base_si = std::move(detector);

    EXPECT_FALSE(base_si->isDone());
    base_si->processIntersections(s0, 0, s1, 0);
    EXPECT_TRUE(base_si->isDone());
    EXPECT_TRUE(detector_ptr->hasProperIntersection());
    ASSERT_NE(detector_ptr->getIntersection(), nullptr);
    EXPECT_TRUE(
        detector_ptr->getIntersection()->equals2D(CoordinateXY{2.0, 2.0}));
  }

  // NodingIntersectionFinder via std::unique_ptr<SegmentIntersector>.
  {
    OwnedBasicSegmentStrings strings;
    SegmentString* s0 = strings.Add({{0.0, 2.0}, {6.0, 2.0}});
    SegmentString* s1 = strings.Add({{3.0, 0.0}, {3.0, 5.0}});

    LineIntersector li;
    auto finder = std::make_unique<NodingIntersectionFinder>(li);
    NodingIntersectionFinder* finder_ptr = finder.get();
    std::unique_ptr<SegmentIntersector> base_si = std::move(finder);

    EXPECT_FALSE(base_si->isDone());
    base_si->processIntersections(s0, 0, s1, 0);
    EXPECT_TRUE(base_si->isDone());
    EXPECT_TRUE(finder_ptr->hasIntersection());
    EXPECT_TRUE(
        finder_ptr->getInteriorIntersection().equals2D(Coordinate{3.0, 2.0}));
  }

  // IntersectionAdder via SegmentIntersector* driven by MCIndexNoder.
  {
    OwnedNodedSegmentStrings strings;
    NodedSegmentString* n0 = strings.Add({{0.0, 0.0}, {4.0, 4.0}});
    NodedSegmentString* n1 = strings.Add({{0.0, 4.0}, {4.0, 0.0}});

    LineIntersector li;
    auto adder = std::make_unique<IntersectionAdder>(li);
    IntersectionAdder* adder_ptr = adder.get();
    std::unique_ptr<SegmentIntersector> base_si = std::move(adder);

    MCIndexNoder noder(base_si.get());
    noder.computeNodes(strings.view());
    EXPECT_FALSE(base_si->isDone());
    EXPECT_TRUE(adder_ptr->hasProperIntersection());
    EXPECT_TRUE(n0->hasNodes());
    EXPECT_TRUE(n1->hasNodes());
    auto noded_coords = n0->getNodedCoordinates();
    ASSERT_EQ(noded_coords->size(), 3u);
    EXPECT_TRUE(noded_coords->getAt<CoordinateXY>(1).equals2D({2.0, 2.0}));
  }

  // IntersectionFinderAdder via SegmentIntersector* driven by MCIndexNoder.
  {
    OwnedNodedSegmentStrings strings;
    strings.Add({{0.0, 1.0}, {5.0, 1.0}});
    strings.Add({{2.0, -2.0}, {2.0, 4.0}});

    LineIntersector li;
    std::vector<Coordinate> interior_intersections;
    auto finder_adder =
        std::make_unique<IntersectionFinderAdder>(li, interior_intersections);
    std::unique_ptr<SegmentIntersector> base_si = std::move(finder_adder);

    MCIndexNoder noder(base_si.get());
    noder.computeNodes(strings.view());
    EXPECT_FALSE(base_si->isDone());
    ASSERT_THAT(interior_intersections, SizeIs(1));
    EXPECT_TRUE(interior_intersections[0].equals2D(Coordinate{2.0, 1.0}));
  }
}

}  // namespace
}  // namespace geos_google
