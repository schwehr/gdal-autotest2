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

// Unit tests for geos::noding::SinglePassNoder.
// See also:
// https://github.com/libgeos/geos/blob/main/include/geos/noding/SinglePassNoder.h

#include "third_party/geos/include/geos/noding/SinglePassNoder.h"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/noding/BasicSegmentString.h"
#include "third_party/geos/include/geos/noding/IntersectionAdder.h"
#include "third_party/geos/include/geos/noding/MCIndexNoder.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"
#include "third_party/geos/include/geos/noding/Noder.h"
#include "third_party/geos/include/geos/noding/SegmentIntersector.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/noding/SimpleNoder.h"

namespace geos_google {
namespace {

static_assert(std::is_abstract_v<geos::noding::SinglePassNoder>);
static_assert(std::is_polymorphic_v<geos::noding::SinglePassNoder>);
static_assert(std::has_virtual_destructor_v<geos::noding::SinglePassNoder>);
static_assert(
    std::is_base_of_v<geos::noding::Noder, geos::noding::SinglePassNoder>);
static_assert(std::is_base_of_v<geos::noding::SinglePassNoder,
                                geos::noding::SimpleNoder>);
static_assert(std::is_base_of_v<geos::noding::SinglePassNoder,
                                geos::noding::MCIndexNoder>);
static_assert(!std::is_default_constructible_v<geos::noding::SinglePassNoder>);
static_assert(!std::is_copy_constructible_v<geos::noding::SinglePassNoder>);
static_assert(std::is_same_v<
              decltype(&geos::noding::SinglePassNoder::setSegmentIntersector),
              void (geos::noding::SinglePassNoder::*)(
                  geos::noding::SegmentIntersector*)>);

struct NodedSubstringsDeleter {
  void operator()(std::vector<geos::noding::SegmentString*>* strings) const {
    if (strings != nullptr) {
      for (geos::noding::SegmentString* ss : *strings) {
        delete ss;
      }
      delete strings;
    }
  }
};

using ScopedNodedSubstrings =
    std::unique_ptr<std::vector<geos::noding::SegmentString*>,
                    NodedSubstringsDeleter>;

std::unique_ptr<geos::noding::NodedSegmentString> MakeNodedString(
    std::initializer_list<geos::geom::Coordinate> pts,
    const void* context = nullptr) {
  auto seq = std::make_unique<geos::geom::CoordinateSequence>();
  seq->reserve(pts.size());
  for (const auto& pt : pts) {
    seq->add(pt);
  }
  return std::make_unique<geos::noding::NodedSegmentString>(
      seq.release(), /*constructZ=*/false, /*constructM=*/false, context);
}

class RecordingSegmentIntersector final
    : public geos::noding::SegmentIntersector {
 public:
  struct Call {
    geos::noding::SegmentString* e0;
    std::size_t seg_index0;
    geos::noding::SegmentString* e1;
    std::size_t seg_index1;
  };

  RecordingSegmentIntersector() = default;
  explicit RecordingSegmentIntersector(std::size_t stop_after)
      : stop_after_(stop_after), has_limit_(true) {}

  void processIntersections(geos::noding::SegmentString* e0,
                            std::size_t seg_index0,
                            geos::noding::SegmentString* e1,
                            std::size_t seg_index1) override {
    calls_.push_back({e0, seg_index0, e1, seg_index1});
  }

  bool isDone() const override {
    return has_limit_ && calls_.size() >= stop_after_;
  }

  const std::vector<Call>& calls() const { return calls_; }

 private:
  std::vector<Call> calls_;
  std::size_t stop_after_ = 0;
  bool has_limit_ = false;
};

class RecordingSinglePassNoder : public geos::noding::SinglePassNoder {
 public:
  using geos::noding::SinglePassNoder::SinglePassNoder;

  RecordingSinglePassNoder(geos::noding::SegmentIntersector* intersector,
                           bool* destroyed_flag)
      : SinglePassNoder(intersector), destroyed_flag_(destroyed_flag) {}

  ~RecordingSinglePassNoder() override {
    if (destroyed_flag_ != nullptr) {
      *destroyed_flag_ = true;
    }
  }

  geos::noding::SegmentIntersector* segment_intersector() const {
    return segInt;
  }

  void computeNodes(
      std::vector<geos::noding::SegmentString*>* seg_strings) override {
    last_input_ = seg_strings;
    ++compute_calls_;
    if (segInt == nullptr || seg_strings == nullptr) {
      return;
    }
    for (std::size_t i = 0; i < seg_strings->size(); ++i) {
      for (std::size_t j = i + 1; j < seg_strings->size(); ++j) {
        if (segInt->isDone()) {
          return;
        }
        segInt->processIntersections((*seg_strings)[i], 0, (*seg_strings)[j],
                                     0);
      }
    }
  }

  std::vector<geos::noding::SegmentString*>* getNodedSubstrings()
      const override {
    auto* result = new std::vector<geos::noding::SegmentString*>();
    if (last_input_ != nullptr) {
      for (geos::noding::SegmentString* ss : *last_input_) {
        result->push_back(new geos::noding::BasicSegmentString(
            const_cast<geos::geom::CoordinateSequence*>(ss->getCoordinates()),
            ss->getData()));
      }
    }
    return result;
  }

  int compute_calls() const { return compute_calls_; }

 private:
  std::vector<geos::noding::SegmentString*>* last_input_ = nullptr;
  int compute_calls_ = 0;
  bool* destroyed_flag_ = nullptr;
};

class OverrideSinglePassNoder final : public RecordingSinglePassNoder {
 public:
  OverrideSinglePassNoder() : RecordingSinglePassNoder(nullptr) {}

  void setSegmentIntersector(
      geos::noding::SegmentIntersector* new_seg_int) override {
    ++set_calls_;
    SinglePassNoder::setSegmentIntersector(new_seg_int);
  }

  int set_calls() const { return set_calls_; }

 private:
  int set_calls_ = 0;
};

class DestructionFlag {
 public:
  explicit DestructionFlag(bool* flag) : flag_(flag) {}
  DestructionFlag(const DestructionFlag&) = delete;
  DestructionFlag& operator=(const DestructionFlag&) = delete;

  ~DestructionFlag() {
    if (flag_ != nullptr) {
      *flag_ = true;
    }
  }

 private:
  bool* flag_;
};

// Direct subclass with an implicitly declared virtual destructor so the
// compiler synthesizes the base-destructor call directly in ~Derived().
class ImplicitDtorSinglePassNoder final : public geos::noding::SinglePassNoder {
 public:
  ImplicitDtorSinglePassNoder(geos::noding::SegmentIntersector* intersector,
                              bool* destroyed_flag)
      : SinglePassNoder(intersector), destroyed_(destroyed_flag) {}

  geos::noding::SegmentIntersector* segment_intersector() const {
    return segInt;
  }

  void computeNodes(
      std::vector<geos::noding::SegmentString*>* seg_strings) override {
    if (segInt != nullptr && seg_strings != nullptr &&
        seg_strings->size() >= 2 && !segInt->isDone()) {
      segInt->processIntersections((*seg_strings)[0], 0, (*seg_strings)[1], 0);
    }
  }

  std::vector<geos::noding::SegmentString*>* getNodedSubstrings()
      const override {
    return new std::vector<geos::noding::SegmentString*>();
  }

 private:
  DestructionFlag destroyed_;
};

// Memberless direct subclass used for normal destruction and qualified
// base-subobject destructor invocation without leaking member state.
class MinimalSinglePassNoder final : public geos::noding::SinglePassNoder {
 public:
  using geos::noding::SinglePassNoder::SinglePassNoder;

  geos::noding::SegmentIntersector* segment_intersector() const {
    return segInt;
  }

  void computeNodes(
      std::vector<geos::noding::SegmentString*>* /*seg_strings*/) override {}

  std::vector<geos::noding::SegmentString*>* getNodedSubstrings()
      const override {
    return new std::vector<geos::noding::SegmentString*>();
  }
};

// Throws after SinglePassNoder base initialization to exercise exception-unwind
// destruction of SinglePassNoder.
class ThrowingDerivedSinglePassNoder final
    : public geos::noding::SinglePassNoder {
 public:
  ThrowingDerivedSinglePassNoder(geos::noding::SegmentIntersector* intersector,
                                 bool* destroyed_flag)
      : SinglePassNoder(intersector), destroyed_(destroyed_flag) {
    throw std::runtime_error("simulated derived construction failure");
  }

  void computeNodes(
      std::vector<geos::noding::SegmentString*>* /*seg_strings*/) override {}

  std::vector<geos::noding::SegmentString*>* getNodedSubstrings()
      const override {
    return new std::vector<geos::noding::SegmentString*>();
  }

 private:
  DestructionFlag destroyed_;
};

TEST(SinglePassNoderTest, DefaultConstructorInitializesNullSegmentIntersector) {
  RecordingSinglePassNoder noder;
  EXPECT_EQ(noder.segment_intersector(), nullptr);

  geos::geom::CoordinateSequence seq{geos::geom::Coordinate(0.0, 0.0),
                                     geos::geom::Coordinate(1.0, 1.0)};
  geos::noding::BasicSegmentString ss(&seq, nullptr);
  std::vector<geos::noding::SegmentString*> input{&ss};

  noder.computeNodes(&input);
  EXPECT_EQ(noder.compute_calls(), 1);

  ScopedNodedSubstrings substrings(noder.getNodedSubstrings());
  ASSERT_NE(substrings, nullptr);
  ASSERT_EQ(substrings->size(), 1u);
  EXPECT_EQ((*substrings)[0]->getCoordinates(), &seq);
}

TEST(SinglePassNoderTest, ConstructorStoresNonNullSegmentIntersector) {
  RecordingSegmentIntersector intersector;
  RecordingSinglePassNoder noder(&intersector);
  EXPECT_EQ(noder.segment_intersector(), &intersector);

  geos::geom::CoordinateSequence seq0{geos::geom::Coordinate(0.0, 0.0),
                                      geos::geom::Coordinate(2.0, 0.0)};
  geos::geom::CoordinateSequence seq1{geos::geom::Coordinate(1.0, -1.0),
                                      geos::geom::Coordinate(1.0, 1.0)};
  geos::noding::BasicSegmentString ss0(&seq0, nullptr);
  geos::noding::BasicSegmentString ss1(&seq1, nullptr);
  std::vector<geos::noding::SegmentString*> input{&ss0, &ss1};

  noder.computeNodes(&input);
  ASSERT_EQ(intersector.calls().size(), 1u);
  EXPECT_EQ(intersector.calls()[0].e0, &ss0);
  EXPECT_EQ(intersector.calls()[0].seg_index0, 0u);
  EXPECT_EQ(intersector.calls()[0].e1, &ss1);
  EXPECT_EQ(intersector.calls()[0].seg_index1, 0u);
}

TEST(SinglePassNoderTest, SetSegmentIntersectorReplacesAndClearsPointer) {
  RecordingSinglePassNoder noder;
  geos::noding::SinglePassNoder* base = &noder;

  RecordingSegmentIntersector first;
  RecordingSegmentIntersector second;

  geos::geom::CoordinateSequence seq0{geos::geom::Coordinate(0.0, 0.0),
                                      geos::geom::Coordinate(1.0, 0.0)};
  geos::geom::CoordinateSequence seq1{geos::geom::Coordinate(0.0, 1.0),
                                      geos::geom::Coordinate(1.0, 1.0)};
  geos::noding::BasicSegmentString ss0(&seq0, nullptr);
  geos::noding::BasicSegmentString ss1(&seq1, nullptr);
  std::vector<geos::noding::SegmentString*> input{&ss0, &ss1};

  base->setSegmentIntersector(&first);
  EXPECT_EQ(noder.segment_intersector(), &first);
  base->computeNodes(&input);
  EXPECT_EQ(first.calls().size(), 1u);
  EXPECT_EQ(second.calls().size(), 0u);

  base->setSegmentIntersector(&second);
  EXPECT_EQ(noder.segment_intersector(), &second);
  base->computeNodes(&input);
  EXPECT_EQ(first.calls().size(), 1u);
  EXPECT_EQ(second.calls().size(), 1u);

  base->setSegmentIntersector(nullptr);
  EXPECT_EQ(noder.segment_intersector(), nullptr);
  base->computeNodes(&input);
  EXPECT_EQ(first.calls().size(), 1u);
  EXPECT_EQ(second.calls().size(), 1u);
}

TEST(SinglePassNoderTest, VirtualSetSegmentIntersectorDispatchesToOverride) {
  OverrideSinglePassNoder noder;
  geos::noding::SinglePassNoder* base = &noder;
  RecordingSegmentIntersector intersector;

  base->setSegmentIntersector(&intersector);
  EXPECT_EQ(noder.set_calls(), 1);
  EXPECT_EQ(noder.segment_intersector(), &intersector);

  base->setSegmentIntersector(nullptr);
  EXPECT_EQ(noder.set_calls(), 2);
  EXPECT_EQ(noder.segment_intersector(), nullptr);
}

TEST(SinglePassNoderTest, PolymorphicDestructionThroughBasePointers) {
  bool destroyed_via_single_pass = false;
  {
    std::unique_ptr<geos::noding::SinglePassNoder> noder =
        std::make_unique<RecordingSinglePassNoder>(nullptr,
                                                   &destroyed_via_single_pass);
    EXPECT_FALSE(destroyed_via_single_pass);
  }
  EXPECT_TRUE(destroyed_via_single_pass);

  bool destroyed_via_noder = false;
  {
    std::unique_ptr<geos::noding::Noder> noder =
        std::make_unique<RecordingSinglePassNoder>(nullptr,
                                                   &destroyed_via_noder);
    EXPECT_FALSE(destroyed_via_noder);
  }
  EXPECT_TRUE(destroyed_via_noder);
}

TEST(SinglePassNoderTest, SimpleNoderNodesCrossingStringsViaBaseInterface) {
  geos::algorithm::LineIntersector li;
  geos::noding::IntersectionAdder adder(li);

  std::unique_ptr<geos::noding::SinglePassNoder> noder =
      std::make_unique<geos::noding::SimpleNoder>();
  noder->setSegmentIntersector(&adder);

  auto ss0 = MakeNodedString(
      {geos::geom::Coordinate(0.0, 0.0), geos::geom::Coordinate(10.0, 10.0)});
  auto ss1 = MakeNodedString(
      {geos::geom::Coordinate(0.0, 10.0), geos::geom::Coordinate(10.0, 0.0)});
  std::vector<geos::noding::SegmentString*> input{ss0.get(), ss1.get()};

  noder->computeNodes(&input);

  EXPECT_TRUE(adder.hasIntersection());
  EXPECT_TRUE(adder.hasProperIntersection());
  EXPECT_TRUE(adder.getProperIntersectionPoint().equals2D(
      geos::geom::CoordinateXY(5.0, 5.0)));

  ScopedNodedSubstrings substrings(noder->getNodedSubstrings());
  ASSERT_NE(substrings, nullptr);
  EXPECT_EQ(substrings->size(), 4u);
}

TEST(SinglePassNoderTest, MCIndexNoderNodesStringsWithSwappedIntersector) {
  geos::algorithm::LineIntersector unused_li;
  geos::noding::IntersectionAdder unused_adder(unused_li);
  geos::algorithm::LineIntersector active_li;
  geos::noding::IntersectionAdder active_adder(active_li);

  std::unique_ptr<geos::noding::SinglePassNoder> noder =
      std::make_unique<geos::noding::MCIndexNoder>(&unused_adder);
  noder->setSegmentIntersector(&active_adder);

  auto ss0 = MakeNodedString(
      {geos::geom::Coordinate(0.0, 2.0), geos::geom::Coordinate(4.0, 2.0)});
  auto ss1 = MakeNodedString(
      {geos::geom::Coordinate(2.0, 0.0), geos::geom::Coordinate(2.0, 4.0)});
  auto ss2 = MakeNodedString(
      {geos::geom::Coordinate(20.0, 20.0), geos::geom::Coordinate(30.0, 20.0)});
  std::vector<geos::noding::SegmentString*> input{ss0.get(), ss1.get(),
                                                  ss2.get()};

  noder->computeNodes(&input);

  EXPECT_FALSE(unused_adder.hasIntersection());
  EXPECT_TRUE(active_adder.hasIntersection());
  EXPECT_TRUE(active_adder.getProperIntersectionPoint().equals2D(
      geos::geom::CoordinateXY(2.0, 2.0)));

  ScopedNodedSubstrings substrings(noder->getNodedSubstrings());
  ASSERT_NE(substrings, nullptr);
  EXPECT_EQ(substrings->size(), 5u);
}

TEST(SinglePassNoderTest, ImplicitDtorAndDirectBaseSubobjectDestruction) {
  bool stack_destroyed = false;
  {
    ImplicitDtorSinglePassNoder noder(nullptr, &stack_destroyed);
    EXPECT_EQ(noder.segment_intersector(), nullptr);
    RecordingSegmentIntersector intersector;
    noder.setSegmentIntersector(&intersector);
    EXPECT_EQ(noder.segment_intersector(), &intersector);

    geos::geom::CoordinateSequence seq0{geos::geom::Coordinate(0.0, 0.0),
                                        geos::geom::Coordinate(1.0, 0.0)};
    geos::geom::CoordinateSequence seq1{geos::geom::Coordinate(0.0, 1.0),
                                        geos::geom::Coordinate(1.0, 1.0)};
    geos::noding::BasicSegmentString ss0(&seq0, nullptr);
    geos::noding::BasicSegmentString ss1(&seq1, nullptr);
    std::vector<geos::noding::SegmentString*> input{&ss0, &ss1};
    noder.computeNodes(&input);
    EXPECT_EQ(intersector.calls().size(), 1u);

    ScopedNodedSubstrings substrings(noder.getNodedSubstrings());
    ASSERT_NE(substrings, nullptr);
    EXPECT_TRUE(substrings->empty());
  }
  EXPECT_TRUE(stack_destroyed);

  bool single_pass_destroyed = false;
  {
    std::unique_ptr<geos::noding::SinglePassNoder> noder =
        std::make_unique<ImplicitDtorSinglePassNoder>(nullptr,
                                                      &single_pass_destroyed);
    EXPECT_FALSE(single_pass_destroyed);
  }
  EXPECT_TRUE(single_pass_destroyed);

  bool noder_destroyed = false;
  {
    std::unique_ptr<geos::noding::Noder> noder =
        std::make_unique<ImplicitDtorSinglePassNoder>(nullptr,
                                                      &noder_destroyed);
    EXPECT_FALSE(noder_destroyed);
  }
  EXPECT_TRUE(noder_destroyed);

  {
    MinimalSinglePassNoder stack_minimal;
    EXPECT_EQ(stack_minimal.segment_intersector(), nullptr);

    RecordingSegmentIntersector intersector;
    std::unique_ptr<geos::noding::SinglePassNoder> heap_minimal =
        std::make_unique<MinimalSinglePassNoder>(&intersector);
    heap_minimal->setSegmentIntersector(nullptr);
    ScopedNodedSubstrings substrings(heap_minimal->getNodedSubstrings());
    ASSERT_NE(substrings, nullptr);
    EXPECT_TRUE(substrings->empty());
  }

  // Invoke SinglePassNoder's complete destructor directly on a memberless
  // derived subobject placed in raw byte storage so neither duplicate nor
  // missing member destruction occurs at block exit.
  {
    using geos::noding::SinglePassNoder;
    alignas(MinimalSinglePassNoder)
        std::array<std::byte, sizeof(MinimalSinglePassNoder)>
            storage;
    RecordingSegmentIntersector intersector;
    MinimalSinglePassNoder* derived = ::new (static_cast<void*>(storage.data()))
        MinimalSinglePassNoder(&intersector);
    EXPECT_EQ(derived->segment_intersector(), &intersector);
    SinglePassNoder* base = derived;
    base->SinglePassNoder::~SinglePassNoder();
  }
}

TEST(SinglePassNoderTest, UnwindsBaseDestructorWhenDerivedConstructorThrows) {
  RecordingSegmentIntersector intersector;

  bool stack_unwound = false;
  EXPECT_THROW(static_cast<void>(ThrowingDerivedSinglePassNoder(
                   &intersector, &stack_unwound)),
               std::runtime_error);
  EXPECT_TRUE(stack_unwound);

  bool heap_unwound = false;
  EXPECT_THROW(
      static_cast<void>(std::make_unique<ThrowingDerivedSinglePassNoder>(
          &intersector, &heap_unwound)),
      std::runtime_error);
  EXPECT_TRUE(heap_unwound);
}

TEST(SinglePassNoderTest, MemberPointerSetSegmentIntersectorAndNullInput) {
  RecordingSinglePassNoder noder;
  geos::noding::SinglePassNoder* base = &noder;
  RecordingSegmentIntersector stop_after_one(/*stop_after=*/1);

  void (geos::noding::SinglePassNoder::*setter)(
      geos::noding::SegmentIntersector*) =
      &geos::noding::SinglePassNoder::setSegmentIntersector;
  (base->*setter)(&stop_after_one);
  EXPECT_EQ(noder.segment_intersector(), &stop_after_one);

  base->computeNodes(nullptr);
  EXPECT_EQ(noder.compute_calls(), 1);
  EXPECT_TRUE(stop_after_one.calls().empty());

  ScopedNodedSubstrings empty_from_null(base->getNodedSubstrings());
  ASSERT_NE(empty_from_null, nullptr);
  EXPECT_TRUE(empty_from_null->empty());

  geos::geom::CoordinateSequence seq0{geos::geom::Coordinate(0.0, 0.0),
                                      geos::geom::Coordinate(1.0, 0.0)};
  geos::geom::CoordinateSequence seq1{geos::geom::Coordinate(0.0, 1.0),
                                      geos::geom::Coordinate(1.0, 1.0)};
  geos::geom::CoordinateSequence seq2{geos::geom::Coordinate(0.0, 2.0),
                                      geos::geom::Coordinate(1.0, 2.0)};
  geos::noding::BasicSegmentString ss0(&seq0, nullptr);
  geos::noding::BasicSegmentString ss1(&seq1, nullptr);
  geos::noding::BasicSegmentString ss2(&seq2, nullptr);
  std::vector<geos::noding::SegmentString*> input{&ss0, &ss1, &ss2};

  base->computeNodes(&input);
  EXPECT_EQ(noder.compute_calls(), 2);
  EXPECT_TRUE(stop_after_one.isDone());
  EXPECT_EQ(stop_after_one.calls().size(), 1u);

  std::unique_ptr<geos::noding::SinglePassNoder> default_simple =
      std::make_unique<geos::noding::SimpleNoder>();
  std::vector<geos::noding::SegmentString*> empty_input;
  default_simple->computeNodes(&empty_input);
  ScopedNodedSubstrings simple_substrings(default_simple->getNodedSubstrings());
  ASSERT_NE(simple_substrings, nullptr);
  EXPECT_TRUE(simple_substrings->empty());
}

}  // namespace
}  // namespace geos_google
