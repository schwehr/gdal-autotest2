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

#include "third_party/geos/include/geos/noding/MCIndexNoder.h"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/index/SpatialIndex.h"
#include "third_party/geos/include/geos/index/chain/MonotoneChain.h"
#include "third_party/geos/include/geos/noding/BasicSegmentString.h"
#include "third_party/geos/include/geos/noding/IntersectionAdder.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"
#include "third_party/geos/include/geos/noding/SegmentIntersector.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/util/GEOSException.h"
#include "third_party/geos/include/geos/util/Interrupt.h"

namespace geos_google {
namespace {

using ::geos::algorithm::LineIntersector;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::geom::Envelope;
using ::geos::index::chain::MonotoneChain;
using ::geos::noding::BasicSegmentString;
using ::geos::noding::IntersectionAdder;
using ::geos::noding::MCIndexNoder;
using ::geos::noding::NodedSegmentString;
using ::geos::noding::SegmentIntersector;
using ::geos::noding::SegmentString;
using ::geos::util::CurrentThreadInterrupt;
using ::geos::util::GEOSException;
using ::testing::IsEmpty;
using ::testing::Lt;
using ::testing::SizeIs;

std::unique_ptr<NodedSegmentString> MakeNodedXY(
    std::initializer_list<CoordinateXY> coords, const void* context = nullptr) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return std::make_unique<NodedSegmentString>(seq.release(), false, false,
                                              context);
}

void DeleteSegmentStringVector(SegmentString::NonConstVect* vec) {
  if (vec == nullptr) {
    return;
  }
  for (SegmentString* ss : *vec) {
    delete ss;
  }
  delete vec;
}

struct RecordedCall {
  SegmentString* e0;
  std::size_t segIndex0;
  SegmentString* e1;
  std::size_t segIndex1;
};

class RecordingSegmentIntersector : public SegmentIntersector {
 public:
  explicit RecordingSegmentIntersector(std::size_t stop_after = 0)
      : stop_after_(stop_after) {}

  void processIntersections(SegmentString* e0, std::size_t segIndex0,
                            SegmentString* e1, std::size_t segIndex1) override {
    calls.push_back({e0, segIndex0, e1, segIndex1});
    if (stop_after_ > 0 && calls.size() >= stop_after_) {
      done_ = true;
    }
  }

  bool isDone() const override { return done_; }

  std::vector<RecordedCall> calls;

 private:
  std::size_t stop_after_;
  bool done_ = false;
};

class CountingSegmentIntersector : public SegmentIntersector {
 public:
  void processIntersections(SegmentString* /*e0*/, std::size_t /*segIndex0*/,
                            SegmentString* /*e1*/,
                            std::size_t /*segIndex1*/) override {
    ++count;
  }

  std::size_t count = 0;
};

TEST(MCIndexNoderTest, EmptyInputProducesEmptyChainsAndSubstrings) {
  LineIntersector li;
  IntersectionAdder si(li);
  MCIndexNoder noder(&si);

  SegmentString::NonConstVect empty;
  noder.computeNodes(&empty);

  EXPECT_THAT(noder.getMonotoneChains(), IsEmpty());

  std::vector<void*> hits;
  Envelope queryEnv(0.0, 10.0, 0.0, 10.0);
  noder.getIndex().query(&queryEnv, hits);
  EXPECT_THAT(hits, IsEmpty());

  SegmentString::NonConstVect* noded = noder.getNodedSubstrings();
  absl::Cleanup cleanup_noded = [&] { DeleteSegmentStringVector(noded); };
  ASSERT_NE(noded, nullptr);
  EXPECT_THAT(*noded, IsEmpty());
}

TEST(MCIndexNoderTest, BuildsMonotoneChainsAndPopulatesSpatialIndex) {
  LineIntersector li;
  IntersectionAdder si(li);
  MCIndexNoder noder(&si);

  // Zig-zag switches quadrant on every segment -> 3 monotone chains.
  auto zigzag = MakeNodedXY({{0.0, 0.0}, {5.0, 5.0}, {10.0, 0.0}, {15.0, 5.0}});
  // Strictly increasing X and Y -> 1 monotone chain.
  auto monotone = MakeNodedXY({{20.0, 20.0}, {25.0, 25.0}, {30.0, 30.0}});

  SegmentString::NonConstVect inputs{zigzag.get(), monotone.get()};
  noder.computeNodes(&inputs);

  ASSERT_THAT(noder.getMonotoneChains(), SizeIs(4));

  std::vector<void*> hitsA;
  Envelope envA(0.0, 15.0, 0.0, 5.0);
  noder.getIndex().query(&envA, hitsA);
  ASSERT_THAT(hitsA, SizeIs(3));
  for (void* item : hitsA) {
    const auto* mc = static_cast<const MonotoneChain*>(item);
    EXPECT_EQ(mc->getContext(), zigzag.get());
  }

  std::vector<void*> hitsB;
  Envelope envB(20.0, 30.0, 20.0, 30.0);
  noder.getIndex().query(&envB, hitsB);
  ASSERT_THAT(hitsB, SizeIs(1));
  EXPECT_EQ(static_cast<const MonotoneChain*>(hitsB[0])->getContext(),
            monotone.get());

  SegmentString::NonConstVect* noded = noder.getNodedSubstrings();
  absl::Cleanup cleanup_noded = [&] { DeleteSegmentStringVector(noded); };
  EXPECT_THAT(*noded, SizeIs(2));
}

TEST(MCIndexNoderTest, CrossingTIntersectionAndSelfIntersectionNoding) {
  LineIntersector li;
  IntersectionAdder si(li);
  MCIndexNoder noder;
  noder.setSegmentIntersector(&si);

  int ctx0 = 1;
  int ctx1 = 2;
  int ctx2 = 3;
  auto diag = MakeNodedXY({{0.0, 0.0}, {10.0, 10.0}}, &ctx0);
  auto cross = MakeNodedXY({{0.0, 10.0}, {10.0, 0.0}}, &ctx1);
  auto tStem = MakeNodedXY({{0.0, 5.0}, {5.0, 5.0}}, &ctx2);

  SegmentString::NonConstVect inputs{diag.get(), cross.get(), tStem.get()};
  noder.computeNodes(&inputs);

  SegmentString::NonConstVect* noded = noder.getNodedSubstrings();
  absl::Cleanup cleanup_noded = [&] { DeleteSegmentStringVector(noded); };
  // diag split at (5,5) -> 2, cross split at (5,5) -> 2, tStem ends at (5,5)
  // -> 1.
  ASSERT_THAT(*noded, SizeIs(5));
  EXPECT_EQ((*noded)[0]->getData(), &ctx0);
  EXPECT_EQ((*noded)[2]->getData(), &ctx1);
  EXPECT_EQ((*noded)[4]->getData(), &ctx2);

  // Separate noder for self-intersecting bowtie polyline.
  LineIntersector selfLi;
  IntersectionAdder selfSi(selfLi);
  MCIndexNoder selfNoder(&selfSi);
  auto bowtie = MakeNodedXY({{0.0, 0.0}, {4.0, 4.0}, {4.0, 0.0}, {0.0, 4.0}});
  SegmentString::NonConstVect selfInputs{bowtie.get()};
  selfNoder.computeNodes(&selfInputs);

  SegmentString::NonConstVect* selfNoded = selfNoder.getNodedSubstrings();
  absl::Cleanup cleanup_self = [&] { DeleteSegmentStringVector(selfNoded); };
  ASSERT_THAT(*selfNoded, SizeIs(3));
}

TEST(MCIndexNoderTest, OverlapToleranceExpandsEnvelopeAndChainChecks) {
  CoordinateSequence seq0({CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0)});
  CoordinateSequence seq1({CoordinateXY(0.0, 0.5), CoordinateXY(10.0, 0.5)});
  BasicSegmentString ss0(&seq0, nullptr);
  BasicSegmentString ss1(&seq1, nullptr);
  SegmentString::NonConstVect inputs{&ss0, &ss1};

  // Exact tolerance 0.0: parallel segments 0.5 apart do not overlap.
  {
    RecordingSegmentIntersector si;
    MCIndexNoder noder(&si, 0.0);
    noder.computeNodes(&inputs);
    EXPECT_THAT(si.calls, IsEmpty());
  }

  // Tolerance 0.2 < half-gap (0.25): envelopes expanded by 0.2 remain disjoint.
  {
    RecordingSegmentIntersector si;
    MCIndexNoder noder(&si, 0.2);
    noder.computeNodes(&inputs);
    EXPECT_THAT(si.calls, IsEmpty());
  }

  // Tolerance 1.0 > distance 0.5: expanded envelopes and chains overlap.
  {
    RecordingSegmentIntersector si;
    MCIndexNoder noder(&si, 1.0);
    noder.computeNodes(&inputs);
    ASSERT_THAT(si.calls, SizeIs(1));
    EXPECT_EQ(si.calls[0].segIndex0, 0u);
    EXPECT_EQ(si.calls[0].segIndex1, 0u);
  }

  // Multi-segment chains with tolerance 1.0 exercise subchain overlap checks.
  {
    CoordinateSequence multiSeq0({CoordinateXY(0.0, 0.0),
                                  CoordinateXY(5.0, 0.0),
                                  CoordinateXY(10.0, 0.0)});
    CoordinateSequence multiSeq1({CoordinateXY(0.0, 0.5),
                                  CoordinateXY(5.0, 0.5),
                                  CoordinateXY(10.0, 0.5)});
    BasicSegmentString mss0(&multiSeq0, nullptr);
    BasicSegmentString mss1(&multiSeq1, nullptr);
    SegmentString::NonConstVect multiInputs{&mss0, &mss1};

    RecordingSegmentIntersector si;
    MCIndexNoder noder(&si, 1.0);
    noder.computeNodes(&multiInputs);
    // Segments within overlapTolerance (1.0) of each other are reported.
    EXPECT_FALSE(si.calls.empty());
  }
}

TEST(MCIndexNoderTest, ReusesBuiltIndexOnSubsequentComputeNodesCall) {
  CoordinateSequence seq0({CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 10.0)});
  CoordinateSequence seq1({CoordinateXY(0.0, 10.0), CoordinateXY(10.0, 0.0)});
  BasicSegmentString ss0(&seq0, nullptr);
  BasicSegmentString ss1(&seq1, nullptr);
  SegmentString::NonConstVect inputs{&ss0, &ss1};

  RecordingSegmentIntersector firstSi;
  MCIndexNoder noder(&firstSi);
  noder.computeNodes(&inputs);
  ASSERT_THAT(firstSi.calls, SizeIs(1));
  ASSERT_THAT(noder.getMonotoneChains(), SizeIs(2));

  // Swapping SegmentIntersector and calling computeNodes with an empty
  // vector takes the indexBuilt == true branch and re-queries existing chains.
  RecordingSegmentIntersector secondSi;
  noder.setSegmentIntersector(&secondSi);
  SegmentString::NonConstVect noNewStrings;
  noder.computeNodes(&noNewStrings);

  EXPECT_THAT(noder.getMonotoneChains(), SizeIs(2));
  ASSERT_THAT(secondSi.calls, SizeIs(1));
}

TEST(MCIndexNoderTest, AbortsQueryPairsEarlyWhenSegmentIntersectorIsDone) {
  constexpr int kNumSegments = 12;
  std::vector<std::unique_ptr<CoordinateSequence>> seqs;
  std::vector<std::unique_ptr<BasicSegmentString>> owned;
  SegmentString::NonConstVect inputs;
  seqs.reserve(kNumSegments);
  owned.reserve(kNumSegments);
  inputs.reserve(kNumSegments);

  for (int i = 0; i < kNumSegments; ++i) {
    double offset = static_cast<double>(i) * 0.01;
    seqs.push_back(std::make_unique<CoordinateSequence>(
        std::initializer_list<CoordinateXY>{{0.0, offset},
                                            {10.0, 10.0 - offset}}));
    owned.push_back(
        std::make_unique<BasicSegmentString>(seqs.back().get(), nullptr));
    inputs.push_back(owned.back().get());
  }

  // Without early abort, all 12*11/2 = 66 overlapping pairs are visited.
  RecordingSegmentIntersector fullSi;
  MCIndexNoder fullNoder(&fullSi);
  fullNoder.computeNodes(&inputs);
  EXPECT_THAT(fullSi.calls, SizeIs(66));

  // When isDone() flips to true after the first intersection, queryPairs
  // short-circuits early so far fewer pairs are visited.
  RecordingSegmentIntersector abortSi(/*stop_after=*/1);
  MCIndexNoder abortNoder(&abortSi);
  abortNoder.computeNodes(&inputs);
  EXPECT_TRUE(abortSi.isDone());
  EXPECT_THAT(abortSi.calls, SizeIs(Lt(std::size_t{66})));
  EXPECT_FALSE(abortSi.calls.empty());
}

TEST(MCIndexNoderTest, SegmentOverlapActionForwardsSegmentStringsAndIndices) {
  CoordinateSequence seq0({CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 1.0),
                           CoordinateXY(2.0, 2.0), CoordinateXY(3.0, 3.0)});
  CoordinateSequence seq1({CoordinateXY(0.0, 3.0), CoordinateXY(1.0, 2.0),
                           CoordinateXY(2.0, 1.0), CoordinateXY(3.0, 0.0)});
  BasicSegmentString ss0(&seq0, nullptr);
  BasicSegmentString ss1(&seq1, nullptr);
  MonotoneChain mc0(seq0, 0, 3, &ss0);
  MonotoneChain mc1(seq1, 0, 3, &ss1);

  RecordingSegmentIntersector si;
  MCIndexNoder::SegmentOverlapAction action(si);
  action.overlap(mc0, 1, mc1, 2);

  ASSERT_THAT(si.calls, SizeIs(1));
  EXPECT_EQ(si.calls[0].e0, &ss0);
  EXPECT_EQ(si.calls[0].segIndex0, 1u);
  EXPECT_EQ(si.calls[0].e1, &ss1);
  EXPECT_EQ(si.calls[0].segIndex1, 2u);
}

TEST(MCIndexNoderTest, ChecksForInterruptsAtHundredThousandOverlaps) {
  // 450 mutually envelope-overlapping segments produce 450 * 449 / 2 = 101,025
  // chain overlap pairs, crossing the nOverlaps % 100000 == 0 check.
  constexpr int kSegments = 450;
  std::vector<std::unique_ptr<CoordinateSequence>> seqs;
  std::vector<std::unique_ptr<BasicSegmentString>> owned;
  SegmentString::NonConstVect inputs;
  seqs.reserve(kSegments);
  owned.reserve(kSegments);
  inputs.reserve(kSegments);

  for (int i = 0; i < kSegments; ++i) {
    double eps = static_cast<double>(i) * 1e-6;
    seqs.push_back(std::make_unique<CoordinateSequence>(
        std::initializer_list<CoordinateXY>{{0.0, eps}, {1.0, 1.0 - eps}}));
    owned.push_back(
        std::make_unique<BasicSegmentString>(seqs.back().get(), nullptr));
    inputs.push_back(owned.back().get());
  }

  // Normal execution past 100,000 overlaps without a pending interrupt.
  {
    CountingSegmentIntersector countingSi;
    MCIndexNoder noder(&countingSi);
    noder.computeNodes(&inputs);
    EXPECT_EQ(countingSi.count, 101025u);
  }

  // Pending current-thread interrupt fires at exactly overlap #100,000.
  {
    CountingSegmentIntersector countingSi;
    MCIndexNoder noder(&countingSi);

    auto interruptCb = [](void* /*data*/) -> int { return 1; };
    CurrentThreadInterrupt::ThreadCallback* prevCb =
        CurrentThreadInterrupt::registerCallback(interruptCb, nullptr);
    absl::Cleanup restore_interrupt = [prevCb] {
      CurrentThreadInterrupt::registerCallback(prevCb, nullptr);
    };

    EXPECT_THROW(noder.computeNodes(&inputs), GEOSException);
    EXPECT_EQ(countingSi.count, 100000u);
  }
}

}  // namespace
}  // namespace geos_google
