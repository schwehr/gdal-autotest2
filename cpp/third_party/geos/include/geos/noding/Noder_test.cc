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

#include "third_party/geos/include/geos/noding/Noder.h"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/noding/IntersectionAdder.h"
#include "third_party/geos/include/geos/noding/MCIndexNoder.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/noding/SimpleNoder.h"

namespace geos_google {
namespace {

using geos::algorithm::LineIntersector;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::noding::IntersectionAdder;
using geos::noding::MCIndexNoder;
using geos::noding::NodedSegmentString;
using geos::noding::Noder;
using geos::noding::SegmentString;
using geos::noding::SimpleNoder;

static_assert(std::is_abstract_v<Noder>);
static_assert(std::has_virtual_destructor_v<Noder>);
static_assert(!std::is_default_constructible_v<Noder>);
static_assert(!std::is_constructible_v<Noder>);

std::unique_ptr<NodedSegmentString> MakeSegmentString(
    std::initializer_list<Coordinate> coords, const void* context) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return std::make_unique<NodedSegmentString>(seq.release(), false, false,
                                              context);
}

// Helper RAII deleter for Noder::getNodedSubstrings() results.
class OwnedSubstrings {
 public:
  explicit OwnedSubstrings(SegmentString::NonConstVect* raw) : vec_(raw) {}

  OwnedSubstrings(const OwnedSubstrings&) = delete;
  OwnedSubstrings& operator=(const OwnedSubstrings&) = delete;

  ~OwnedSubstrings() {
    if (vec_ != nullptr) {
      for (SegmentString* ss : *vec_) {
        delete ss;
      }
      delete vec_;
    }
  }

  const SegmentString::NonConstVect& get() const { return *vec_; }

 private:
  SegmentString::NonConstVect* vec_;
};

// Minimal concrete Noder subclass verifying protected Noder() construction,
// virtual ~Noder() destruction, and caller-owned output contract.
class PassthroughCopyNoder final : public Noder {
 public:
  explicit PassthroughCopyNoder(int* destroy_count = nullptr)
      : destroy_count_(destroy_count) {}

  ~PassthroughCopyNoder() override {
    if (destroy_count_ != nullptr) {
      ++(*destroy_count_);
    }
  }

  void computeNodes(SegmentString::NonConstVect* seg_strings) override {
    inputs_ = seg_strings;
    ++compute_calls_;
  }

  SegmentString::NonConstVect* getNodedSubstrings() const override {
    auto* out = new SegmentString::NonConstVect();
    if (inputs_ == nullptr) {
      return out;
    }
    for (SegmentString* ss : *inputs_) {
      auto copy_seq = ss->getCoordinates()->clone();
      out->push_back(new NodedSegmentString(copy_seq.release(), false, false,
                                            ss->getData()));
    }
    return out;
  }

  int compute_calls() const { return compute_calls_; }

 private:
  SegmentString::NonConstVect* inputs_ = nullptr;
  int* destroy_count_ = nullptr;
  int compute_calls_ = 0;
};

TEST(NoderTest, CustomSubclassConstructionPolymorphismAndDestruction) {
  int stack_destroyed = 0;
  {
    PassthroughCopyNoder stack_noder(&stack_destroyed);
    EXPECT_EQ(stack_noder.compute_calls(), 0);
  }
  EXPECT_EQ(stack_destroyed, 1);

  int heap_destroyed = 0;
  std::unique_ptr<Noder> noder =
      std::make_unique<PassthroughCopyNoder>(&heap_destroyed);

  int tag = 7;
  auto seg =
      MakeSegmentString({Coordinate(0.0, 0.0), Coordinate(4.0, 0.0)}, &tag);
  SegmentString::NonConstVect inputs = {seg.get()};

  noder->computeNodes(&inputs);
  const Noder& const_noder = *noder;
  OwnedSubstrings substrings(const_noder.getNodedSubstrings());

  ASSERT_EQ(substrings.get().size(), 1u);
  EXPECT_NE(substrings.get()[0], seg.get());
  EXPECT_EQ(substrings.get()[0]->getData(), &tag);
  EXPECT_EQ(substrings.get()[0]->size(), 2u);
  EXPECT_TRUE(
      substrings.get()[0]->getCoordinate(1).equals2D(CoordinateXY(4.0, 0.0)));

  EXPECT_EQ(heap_destroyed, 0);
  noder.reset();
  EXPECT_EQ(heap_destroyed, 1);
}

TEST(NoderTest, SimpleNoderThroughNoderInterfaceNodesCrossingSegments) {
  LineIntersector li;
  IntersectionAdder adder(li);
  std::unique_ptr<Noder> noder = std::make_unique<SimpleNoder>(&adder);

  int ctx_a = 1;
  int ctx_b = 2;
  auto seg_a =
      MakeSegmentString({Coordinate(0.0, 0.0), Coordinate(10.0, 10.0)}, &ctx_a);
  auto seg_b =
      MakeSegmentString({Coordinate(0.0, 10.0), Coordinate(10.0, 0.0)}, &ctx_b);
  SegmentString::NonConstVect inputs = {seg_a.get(), seg_b.get()};

  noder->computeNodes(&inputs);
  OwnedSubstrings substrings(noder->getNodedSubstrings());
  ASSERT_EQ(substrings.get().size(), 4u);

  std::size_t count_a = 0;
  std::size_t count_b = 0;
  for (const SegmentString* ss : substrings.get()) {
    ASSERT_EQ(ss->size(), 2u);
    if (ss->getData() == &ctx_a) {
      ++count_a;
    } else if (ss->getData() == &ctx_b) {
      ++count_b;
    }
  }
  EXPECT_EQ(count_a, 2u);
  EXPECT_EQ(count_b, 2u);
  EXPECT_TRUE(
      substrings.get()[0]->getCoordinate(1).equals2D(CoordinateXY(5.0, 5.0)));
  EXPECT_TRUE(
      substrings.get()[1]->getCoordinate(0).equals2D(CoordinateXY(5.0, 5.0)));
  EXPECT_TRUE(
      substrings.get()[2]->getCoordinate(1).equals2D(CoordinateXY(5.0, 5.0)));
  EXPECT_TRUE(
      substrings.get()[3]->getCoordinate(0).equals2D(CoordinateXY(5.0, 5.0)));
}

TEST(NoderTest, MCIndexNoderEmptyAndDisjointInputs) {
  LineIntersector li;
  IntersectionAdder empty_adder(li);
  std::unique_ptr<Noder> empty_noder =
      std::make_unique<MCIndexNoder>(&empty_adder);
  SegmentString::NonConstVect empty_inputs;
  empty_noder->computeNodes(&empty_inputs);
  OwnedSubstrings empty_out(empty_noder->getNodedSubstrings());
  EXPECT_TRUE(empty_out.get().empty());

  IntersectionAdder disjoint_adder(li);
  std::unique_ptr<Noder> noder =
      std::make_unique<MCIndexNoder>(&disjoint_adder);
  int ctx1 = 10;
  int ctx2 = 20;
  auto seg1 =
      MakeSegmentString({Coordinate(0.0, 0.0), Coordinate(2.0, 0.0)}, &ctx1);
  auto seg2 =
      MakeSegmentString({Coordinate(0.0, 5.0), Coordinate(2.0, 5.0)}, &ctx2);
  SegmentString::NonConstVect inputs = {seg1.get(), seg2.get()};

  noder->computeNodes(&inputs);
  OwnedSubstrings substrings(noder->getNodedSubstrings());
  ASSERT_EQ(substrings.get().size(), 2u);
  EXPECT_EQ(substrings.get()[0]->getData(), &ctx1);
  EXPECT_EQ(substrings.get()[1]->getData(), &ctx2);
}

TEST(NoderTest, MCIndexNoderTJunctionAndCollinearOverlap) {
  LineIntersector li;
  IntersectionAdder tj_adder(li);
  std::unique_ptr<Noder> tj_noder = std::make_unique<MCIndexNoder>(&tj_adder);

  int ctx_h = 100;
  int ctx_v = 200;
  auto horiz =
      MakeSegmentString({Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)}, &ctx_h);
  auto vert =
      MakeSegmentString({Coordinate(5.0, 0.0), Coordinate(5.0, 5.0)}, &ctx_v);
  SegmentString::NonConstVect tj_inputs = {horiz.get(), vert.get()};
  tj_noder->computeNodes(&tj_inputs);
  OwnedSubstrings tj_out(tj_noder->getNodedSubstrings());
  ASSERT_EQ(tj_out.get().size(), 3u);
  EXPECT_EQ(tj_out.get()[0]->getData(), &ctx_h);
  EXPECT_EQ(tj_out.get()[1]->getData(), &ctx_h);
  EXPECT_EQ(tj_out.get()[2]->getData(), &ctx_v);
  EXPECT_TRUE(
      tj_out.get()[0]->getCoordinate(1).equals2D(CoordinateXY(5.0, 0.0)));
  EXPECT_TRUE(
      tj_out.get()[1]->getCoordinate(0).equals2D(CoordinateXY(5.0, 0.0)));

  IntersectionAdder col_adder(li);
  std::unique_ptr<Noder> col_noder = std::make_unique<MCIndexNoder>(&col_adder);
  auto seg_a =
      MakeSegmentString({Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)}, &ctx_h);
  auto seg_b =
      MakeSegmentString({Coordinate(4.0, 0.0), Coordinate(14.0, 0.0)}, &ctx_v);
  SegmentString::NonConstVect col_inputs = {seg_a.get(), seg_b.get()};
  col_noder->computeNodes(&col_inputs);
  OwnedSubstrings col_out(col_noder->getNodedSubstrings());
  ASSERT_EQ(col_out.get().size(), 4u);
  EXPECT_TRUE(
      col_out.get()[0]->getCoordinate(1).equals2D(CoordinateXY(4.0, 0.0)));
  EXPECT_TRUE(
      col_out.get()[1]->getCoordinate(0).equals2D(CoordinateXY(4.0, 0.0)));
  EXPECT_TRUE(
      col_out.get()[1]->getCoordinate(1).equals2D(CoordinateXY(10.0, 0.0)));
  EXPECT_TRUE(
      col_out.get()[2]->getCoordinate(0).equals2D(CoordinateXY(4.0, 0.0)));
  EXPECT_TRUE(
      col_out.get()[2]->getCoordinate(1).equals2D(CoordinateXY(10.0, 0.0)));
  EXPECT_TRUE(
      col_out.get()[3]->getCoordinate(0).equals2D(CoordinateXY(10.0, 0.0)));
}

}  // namespace
}  // namespace geos_google
