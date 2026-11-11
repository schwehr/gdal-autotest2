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

// Unit tests for
// third_party/geos/include/geos/noding/SegmentSetMutualIntersector.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/noding/SegmentSetMutualIntersector.h

#include "third_party/geos/include/geos/noding/SegmentSetMutualIntersector.h"

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
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/noding/BasicSegmentString.h"
#include "third_party/geos/include/geos/noding/FastSegmentSetIntersectionFinder.h"
#include "third_party/geos/include/geos/noding/MCIndexSegmentSetMutualIntersector.h"
#include "third_party/geos/include/geos/noding/NodingIntersectionFinder.h"
#include "third_party/geos/include/geos/noding/SegmentIntersectionDetector.h"
#include "third_party/geos/include/geos/noding/SegmentIntersector.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"

namespace geos_google {
namespace {

using geos::algorithm::LineIntersector;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Envelope;
using geos::noding::BasicSegmentString;
using geos::noding::FastSegmentSetIntersectionFinder;
using geos::noding::MCIndexSegmentSetMutualIntersector;
using geos::noding::NodingIntersectionFinder;
using geos::noding::SegmentIntersectionDetector;
using geos::noding::SegmentIntersector;
using geos::noding::SegmentSetMutualIntersector;
using geos::noding::SegmentString;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::SizeIs;

static_assert(std::is_abstract_v<SegmentSetMutualIntersector>);
static_assert(std::is_polymorphic_v<SegmentSetMutualIntersector>);
static_assert(std::has_virtual_destructor_v<SegmentSetMutualIntersector>);
static_assert(!std::is_default_constructible_v<SegmentSetMutualIntersector>);
static_assert(!std::is_copy_constructible_v<SegmentSetMutualIntersector>);
static_assert(std::is_base_of_v<SegmentSetMutualIntersector,
                                MCIndexSegmentSetMutualIntersector>);
static_assert(
    std::is_same_v<decltype(std::declval<SegmentSetMutualIntersector&>()
                                .setSegmentIntersector(
                                    std::declval<SegmentIntersector*>())),
                   void>);
static_assert(
    std::is_same_v<
        decltype(std::declval<SegmentSetMutualIntersector&>().setBaseSegments(
            std::declval<SegmentString::ConstVect*>())),
        void>);
static_assert(std::is_same_v<
              decltype(std::declval<SegmentSetMutualIntersector&>().process(
                  std::declval<SegmentString::ConstVect*>())),
              void>);

class OwnedSegmentSet {
 public:
  const SegmentString* Add(std::initializer_list<Coordinate> coords) {
    auto seq = std::make_unique<CoordinateSequence>(coords);
    auto seg = std::make_unique<BasicSegmentString>(seq.get(), nullptr);
    const SegmentString* raw = seg.get();
    sequences_.push_back(std::move(seq));
    strings_.push_back(std::move(seg));
    view_.push_back(raw);
    return raw;
  }

  SegmentString::ConstVect* view() { return &view_; }

 private:
  std::vector<std::unique_ptr<CoordinateSequence>> sequences_;
  std::vector<std::unique_ptr<BasicSegmentString>> strings_;
  SegmentString::ConstVect view_;
};

struct RecordedPair {
  const SegmentString* e0 = nullptr;
  std::size_t seg_index0 = 0;
  const SegmentString* e1 = nullptr;
  std::size_t seg_index1 = 0;
};

class RecordingSegmentIntersector final : public SegmentIntersector {
 public:
  explicit RecordingSegmentIntersector(
      std::size_t stop_after = std::numeric_limits<std::size_t>::max())
      : stop_after_(stop_after) {}

  void processIntersections(SegmentString* e0, std::size_t segIndex0,
                            SegmentString* e1, std::size_t segIndex1) override {
    calls_.push_back({e0, segIndex0, e1, segIndex1});
  }

  bool isDone() const override { return calls_.size() >= stop_after_; }

  const std::vector<RecordedPair>& calls() const { return calls_; }

 private:
  std::size_t stop_after_;
  std::vector<RecordedPair> calls_;
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

class BruteForceMutualIntersector final : public SegmentSetMutualIntersector {
 public:
  explicit BruteForceMutualIntersector(bool* destroyed = nullptr)
      : destroyed_(destroyed) {}

  ~BruteForceMutualIntersector() override {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
  }

  void setBaseSegments(SegmentString::ConstVect* segStrings) override {
    base_segments_.clear();
    if (segStrings != nullptr) {
      base_segments_ = *segStrings;
    }
  }

  void process(SegmentString::ConstVect* segStrings) override {
    if (segInt == nullptr || segStrings == nullptr) {
      return;
    }
    for (const SegmentString* query_css : *segStrings) {
      if (query_css == nullptr || query_css->size() < 2) {
        continue;
      }
      auto* query_ss = const_cast<SegmentString*>(query_css);
      for (std::size_t qi = 0; qi + 1 < query_ss->size(); ++qi) {
        for (const SegmentString* base_css : base_segments_) {
          if (base_css == nullptr || base_css->size() < 2) {
            continue;
          }
          auto* base_ss = const_cast<SegmentString*>(base_css);
          for (std::size_t bi = 0; bi + 1 < base_ss->size(); ++bi) {
            segInt->processIntersections(query_ss, qi, base_ss, bi);
            if (segInt->isDone()) {
              return;
            }
          }
        }
      }
    }
  }

  SegmentIntersector* currentSegmentIntersector() const { return segInt; }
  const SegmentString::ConstVect& baseSegments() const {
    return base_segments_;
  }

 private:
  SegmentString::ConstVect base_segments_;
  bool* destroyed_;
};

// Relies on an implicitly declared virtual destructor to exercise base-class
// destructor chaining without an explicit ~Derived() override.
class ImplicitDtorMutualIntersector final : public SegmentSetMutualIntersector {
 public:
  explicit ImplicitDtorMutualIntersector(bool* destroyed) : flag_(destroyed) {}

  void setBaseSegments(SegmentString::ConstVect* segStrings) override {
    base_calls_.push_back(segStrings);
  }

  void process(SegmentString::ConstVect* segStrings) override {
    process_calls_.push_back(segStrings);
  }

  SegmentIntersector* currentSegmentIntersector() const { return segInt; }
  const std::vector<SegmentString::ConstVect*>& baseCalls() const {
    return base_calls_;
  }
  const std::vector<SegmentString::ConstVect*>& processCalls() const {
    return process_calls_;
  }

 private:
  std::vector<SegmentString::ConstVect*> base_calls_;
  std::vector<SegmentString::ConstVect*> process_calls_;
  DestructionFlag flag_;
};

TEST(SegmentSetMutualIntersectorTest,
     ConstructorInitializesSegIntToNullAndVirtualDestructionWorks) {
  bool stack_destroyed = false;
  {
    BruteForceMutualIntersector stack_impl(&stack_destroyed);
    EXPECT_EQ(stack_impl.currentSegmentIntersector(), nullptr);

    RecordingSegmentIntersector recorder;
    SegmentSetMutualIntersector& base_ref = stack_impl;
    base_ref.setSegmentIntersector(&recorder);
    EXPECT_EQ(stack_impl.currentSegmentIntersector(), &recorder);

    base_ref.setSegmentIntersector(nullptr);
    EXPECT_EQ(stack_impl.currentSegmentIntersector(), nullptr);
  }
  EXPECT_TRUE(stack_destroyed);

  bool explicit_destroyed = false;
  bool implicit_destroyed = false;
  std::vector<std::unique_ptr<SegmentSetMutualIntersector>> intersectors;
  intersectors.push_back(
      std::make_unique<BruteForceMutualIntersector>(&explicit_destroyed));
  intersectors.push_back(
      std::make_unique<ImplicitDtorMutualIntersector>(&implicit_destroyed));
  intersectors.push_back(
      std::make_unique<MCIndexSegmentSetMutualIntersector>());

  EXPECT_FALSE(explicit_destroyed);
  EXPECT_FALSE(implicit_destroyed);
  intersectors.clear();
  EXPECT_TRUE(explicit_destroyed);
  EXPECT_TRUE(implicit_destroyed);
}

TEST(SegmentSetMutualIntersectorTest,
     CustomRedBlueIntersectorPolymorphicDispatchAndNullGuard) {
  OwnedSegmentSet base;
  const SegmentString* base0 = base.Add({{0.0, 0.0}, {10.0, 0.0}});
  base.Add({{5.0, 5.0}});  // Single vertex (0 segments).

  OwnedSegmentSet query;
  const SegmentString* query0 =
      query.Add({{2.0, -1.0}, {2.0, 1.0}, {8.0, 1.0}});

  BruteForceMutualIntersector impl;
  SegmentSetMutualIntersector* base_ptr = &impl;

  // Calling process() before setting a SegmentIntersector is a safe no-op.
  base_ptr->setBaseSegments(nullptr);
  EXPECT_THAT(impl.baseSegments(), IsEmpty());
  base_ptr->setBaseSegments(base.view());
  base_ptr->process(nullptr);
  base_ptr->process(query.view());

  // First SegmentIntersector receives all query-vs-base segment pairs.
  RecordingSegmentIntersector rec_all;
  base_ptr->setSegmentIntersector(&rec_all);
  base_ptr->process(query.view());
  ASSERT_THAT(rec_all.calls(), SizeIs(2));
  EXPECT_EQ(rec_all.calls()[0].e0, query0);
  EXPECT_EQ(rec_all.calls()[0].seg_index0, 0u);
  EXPECT_EQ(rec_all.calls()[0].e1, base0);
  EXPECT_EQ(rec_all.calls()[0].seg_index1, 0u);
  EXPECT_EQ(rec_all.calls()[1].seg_index0, 1u);

  // Swapping SegmentIntersector stops early when the new one reports isDone().
  RecordingSegmentIntersector rec_stop(/*stop_after=*/1);
  base_ptr->setSegmentIntersector(&rec_stop);
  base_ptr->process(query.view());
  EXPECT_THAT(rec_stop.calls(), SizeIs(1));
  EXPECT_THAT(rec_all.calls(), SizeIs(2));

  // ImplicitDtorMutualIntersector forwards exact vector pointers.
  bool implicit_destroyed = false;
  ImplicitDtorMutualIntersector forwarder(&implicit_destroyed);
  SegmentSetMutualIntersector* forward_ptr = &forwarder;
  EXPECT_EQ(forwarder.currentSegmentIntersector(), nullptr);
  forward_ptr->setBaseSegments(base.view());
  forward_ptr->process(query.view());
  EXPECT_THAT(forwarder.baseCalls(), ElementsAre(base.view()));
  EXPECT_THAT(forwarder.processCalls(), ElementsAre(query.view()));
}

TEST(SegmentSetMutualIntersectorTest,
     ConcreteMCIndexMutualIntersectorThroughBaseInterface) {
  OwnedSegmentSet base;
  base.Add({{0.0, 0.0}, {10.0, 0.0}});
  base.Add({{20.0, 0.0}, {24.0, 0.0}});

  std::unique_ptr<SegmentSetMutualIntersector> intersector =
      std::make_unique<MCIndexSegmentSetMutualIntersector>();
  intersector->setBaseSegments(base.view());

  // 1) Crossing segment detected with NodingIntersectionFinder.
  {
    OwnedSegmentSet query;
    query.Add({{5.0, -5.0}, {5.0, 5.0}});
    LineIntersector li;
    NodingIntersectionFinder finder(li);
    intersector->setSegmentIntersector(&finder);
    intersector->process(query.view());
    EXPECT_TRUE(finder.hasIntersection());
    EXPECT_TRUE(
        finder.getInteriorIntersection().equals2D(Coordinate(5.0, 0.0)));
  }

  // 2) Disjoint segment records no intersection.
  {
    OwnedSegmentSet query;
    query.Add({{50.0, 50.0}, {60.0, 60.0}});
    LineIntersector li;
    SegmentIntersectionDetector detector(&li);
    intersector->setSegmentIntersector(&detector);
    intersector->process(query.view());
    EXPECT_FALSE(detector.hasIntersection());
  }

  // 3) T-junction endpoint intersection detected as non-proper.
  {
    OwnedSegmentSet query;
    query.Add({{22.0, 0.0}, {22.0, 4.0}});
    LineIntersector li;
    SegmentIntersectionDetector detector(&li);
    intersector->setSegmentIntersector(&detector);
    intersector->process(query.view());
    EXPECT_TRUE(detector.hasIntersection());
    EXPECT_FALSE(detector.hasProperIntersection());
    EXPECT_TRUE(detector.hasNonProperIntersection());
    ASSERT_NE(detector.getIntersection(), nullptr);
    EXPECT_TRUE(detector.getIntersection()->equals2D(Coordinate(22.0, 0.0)));
  }

  // 4) Envelope-restricted MCIndexSegmentSetMutualIntersector through base API.
  {
    const Envelope roi(0.0, 12.0, -6.0, 6.0);
    std::unique_ptr<SegmentSetMutualIntersector> filtered =
        std::make_unique<MCIndexSegmentSetMutualIntersector>(&roi);
    filtered->setBaseSegments(base.view());

    OwnedSegmentSet outside_query;
    outside_query.Add({{22.0, -3.0}, {22.0, 3.0}});
    LineIntersector li;
    SegmentIntersectionDetector detector(&li);
    filtered->setSegmentIntersector(&detector);
    filtered->process(outside_query.view());
    EXPECT_FALSE(detector.hasIntersection());
  }

  // 5) Early termination when SegmentIntersector::isDone() returns true.
  {
    OwnedSegmentSet multi_base;
    for (int x = 1; x <= 4; ++x) {
      multi_base.Add(
          {{static_cast<double>(x), 0.0}, {static_cast<double>(x), 10.0}});
    }
    OwnedSegmentSet single_query;
    single_query.Add({{0.0, 5.0}, {5.0, 5.0}});

    std::unique_ptr<SegmentSetMutualIntersector> early_intersector =
        std::make_unique<MCIndexSegmentSetMutualIntersector>();
    early_intersector->setBaseSegments(multi_base.view());

    RecordingSegmentIntersector visit_all;
    early_intersector->setSegmentIntersector(&visit_all);
    early_intersector->process(single_query.view());
    EXPECT_THAT(visit_all.calls(), SizeIs(4));

    RecordingSegmentIntersector stop_early(/*stop_after=*/1);
    early_intersector->setSegmentIntersector(&stop_early);
    early_intersector->process(single_query.view());
    EXPECT_THAT(stop_early.calls(), SizeIs(1));
  }
}

TEST(SegmentSetMutualIntersectorTest,
     RedBlueInvariantIgnoresIntraSetCrossingsAndHandlesEmptySets) {
  // Empty base set produces zero calls even for valid query segments.
  {
    SegmentString::ConstVect empty_base;
    OwnedSegmentSet query;
    query.Add({{0.0, 0.0}, {4.0, 4.0}});

    std::unique_ptr<SegmentSetMutualIntersector> empty_intersector =
        std::make_unique<MCIndexSegmentSetMutualIntersector>();
    RecordingSegmentIntersector recorder;
    empty_intersector->setSegmentIntersector(&recorder);
    empty_intersector->setBaseSegments(&empty_base);
    empty_intersector->process(query.view());
    EXPECT_THAT(recorder.calls(), IsEmpty());
  }

  // Base set has an internal self-crossing at (2, 2).
  OwnedSegmentSet base;
  base.Add({{0.0, 0.0}, {4.0, 4.0}});
  base.Add({{0.0, 4.0}, {4.0, 0.0}});

  // Disjoint query set also has an internal self-crossing at (12, 2).
  OwnedSegmentSet disjoint_query;
  disjoint_query.Add({{10.0, 0.0}, {14.0, 4.0}});
  disjoint_query.Add({{10.0, 4.0}, {14.0, 0.0}});

  std::unique_ptr<SegmentSetMutualIntersector> intersector =
      std::make_unique<MCIndexSegmentSetMutualIntersector>();
  intersector->setBaseSegments(base.view());

  {
    LineIntersector li;
    SegmentIntersectionDetector detector(&li);
    intersector->setSegmentIntersector(&detector);
    intersector->process(disjoint_query.view());
    EXPECT_FALSE(detector.hasIntersection());
  }

  // Empty query set and single-vertex query string produce zero calls.
  {
    SegmentString::ConstVect empty_query;
    RecordingSegmentIntersector recorder;
    intersector->setSegmentIntersector(&recorder);
    intersector->process(&empty_query);
    EXPECT_THAT(recorder.calls(), IsEmpty());

    OwnedSegmentSet degenerate_query;
    degenerate_query.Add({});
    degenerate_query.Add({{2.0, 2.0}});
    intersector->process(degenerate_query.view());
    EXPECT_THAT(recorder.calls(), IsEmpty());
  }

  // Query intersecting base triggers red-blue detection.
  {
    OwnedSegmentSet crossing_query;
    crossing_query.Add({{2.0, -1.0}, {2.0, 5.0}});
    LineIntersector li;
    NodingIntersectionFinder finder(li);
    intersector->setSegmentIntersector(&finder);
    intersector->process(crossing_query.view());
    EXPECT_TRUE(finder.hasIntersection());
    EXPECT_TRUE(
        finder.getInteriorIntersection().equals2D(Coordinate(2.0, 2.0)));
  }
}

TEST(SegmentSetMutualIntersectorTest,
     FastSegmentSetIntersectionFinderExposesBaseInterface) {
  OwnedSegmentSet base;
  base.Add({{0.0, 0.0}, {10.0, 0.0}});

  FastSegmentSetIntersectionFinder finder(base.view());
  const SegmentSetMutualIntersector* base_intersector =
      finder.getSegmentSetIntersector();
  ASSERT_NE(base_intersector, nullptr);
  EXPECT_NE(
      dynamic_cast<const MCIndexSegmentSetMutualIntersector*>(base_intersector),
      nullptr);

  OwnedSegmentSet hit_query;
  hit_query.Add({{5.0, -1.0}, {5.0, 1.0}});
  EXPECT_TRUE(finder.intersects(hit_query.view()));

  OwnedSegmentSet miss_query;
  miss_query.Add({{5.0, 2.0}, {5.0, 4.0}});
  EXPECT_FALSE(finder.intersects(miss_query.view()));
}

}  // namespace
}  // namespace geos_google
