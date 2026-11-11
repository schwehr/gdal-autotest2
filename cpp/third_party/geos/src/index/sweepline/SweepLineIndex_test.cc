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

// Unit tests for third_party/geos/src/index/sweepline/SweepLineIndex.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for exception-propagation tests.
// - Upstream SweepLineIndex::add allocates SweepLineEvent objects via raw `new`
//   and ~SweepLineIndex() = default never deletes them (upstream FIXME). Leak
//   checking is scoped out strictly around SweepLineIndex::add() calls so ASan
//   LSan and HeapLeakChecker still verify all other allocations.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/sweepline/SweepLineIndex.cpp

#include "third_party/geos/include/geos/index/sweepline/SweepLineIndex.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <deque>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/index/sweepline/SweepLineInterval.h"
#include "third_party/geos/include/geos/index/sweepline/SweepLineOverlapAction.h"

#if defined(LEAK_SANITIZER)
#include <sanitizer/lsan_interface.h>
#elif defined(__has_attribute)
#if __has_attribute(weak)
extern "C" {
__attribute__((weak)) void __lsan_disable();
__attribute__((weak)) void __lsan_enable();
}  // extern "C"
#endif
#endif

namespace geos_google {
namespace {

using geos::index::sweepline::SweepLineIndex;
using geos::index::sweepline::SweepLineInterval;
using geos::index::sweepline::SweepLineOverlapAction;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Pair;

void DisableLeakChecking() {
#if defined(LEAK_SANITIZER)
  __lsan_disable();
#elif defined(__has_attribute)
#if __has_attribute(weak)
  if (__lsan_disable != nullptr) {
    __lsan_disable();
  }
#endif
#endif
}

void EnableLeakChecking() {
#if defined(LEAK_SANITIZER)
  __lsan_enable();
#elif defined(__has_attribute)
#if __has_attribute(weak)
  if (__lsan_enable != nullptr) {
    __lsan_enable();
  }
#endif
#endif
}

void AddInterval(SweepLineIndex& index, SweepLineInterval* interval) {
  DisableLeakChecking();
  absl::Cleanup reenable = [] { EnableLeakChecking(); };
  index.add(interval);
}

struct IntervalSpec {
  double min;
  double max;
  int id;
};

struct IntervalRecord {
  IntervalRecord(int id_val, double min_val, double max_val)
      : id(id_val), interval(min_val, max_val, &id) {}

  int id;
  SweepLineInterval interval;
};

class RecordingOverlapAction final : public SweepLineOverlapAction {
 public:
  void overlap(SweepLineInterval* s0, SweepLineInterval* s1) override {
    ASSERT_NE(s0, nullptr);
    ASSERT_NE(s1, nullptr);
    raw_pairs_.emplace_back(s0, s1);
    if (s0->getItem() != nullptr && s1->getItem() != nullptr) {
      const int id0 = *static_cast<const int*>(s0->getItem());
      const int id1 = *static_cast<const int*>(s1->getItem());
      id_pairs_.emplace_back(id0, id1);
    }
  }

  const std::vector<std::pair<SweepLineInterval*, SweepLineInterval*>>&
  raw_pairs() const {
    return raw_pairs_;
  }

  const std::vector<std::pair<int, int>>& id_pairs() const { return id_pairs_; }

  std::vector<std::pair<int, int>> canonical_id_pairs() const {
    std::vector<std::pair<int, int>> result;
    result.reserve(id_pairs_.size());
    for (const auto& [a, b] : id_pairs_) {
      result.emplace_back(std::min(a, b), std::max(a, b));
    }
    std::sort(result.begin(), result.end());
    return result;
  }

 private:
  std::vector<std::pair<SweepLineInterval*, SweepLineInterval*>> raw_pairs_;
  std::vector<std::pair<int, int>> id_pairs_;
};

class ThrowingOverlapAction final : public SweepLineOverlapAction {
 public:
  explicit ThrowingOverlapAction(std::size_t throw_on_call)
      : throw_on_call_(throw_on_call) {}

  void overlap(SweepLineInterval* /*s0*/, SweepLineInterval* /*s1*/) override {
    ++calls_;
    if (calls_ == throw_on_call_) {
      throw std::runtime_error("simulated action failure");
    }
  }

 private:
  std::size_t throw_on_call_;
  std::size_t calls_ = 0;
};

bool IntervalsIntersect(SweepLineInterval& a, SweepLineInterval& b) {
  return !(a.getMax() < b.getMin() || b.getMax() < a.getMin());
}

std::vector<std::pair<int, int>> BruteForceOverlapPairs(
    std::deque<IntervalRecord>& records) {
  std::vector<std::pair<int, int>> expected;
  for (std::size_t i = 0; i < records.size(); ++i) {
    for (std::size_t j = i; j < records.size(); ++j) {
      if (IntervalsIntersect(records[i].interval, records[j].interval)) {
        const int a = records[i].id;
        const int b = records[j].id;
        expected.emplace_back(std::min(a, b), std::max(a, b));
      }
    }
  }
  std::sort(expected.begin(), expected.end());
  return expected;
}

std::deque<IntervalRecord> BuildAndAddRecords(
    SweepLineIndex& index, const std::vector<IntervalSpec>& specs) {
  std::deque<IntervalRecord> records;
  for (const auto& spec : specs) {
    IntervalRecord& back = records.emplace_back(spec.id, spec.min, spec.max);
    AddInterval(index, &back.interval);
  }
  return records;
}

void VerifyMatchesBruteForce(SweepLineIndex& index,
                             std::deque<IntervalRecord>& records) {
  RecordingOverlapAction action;
  index.computeOverlaps(&action);

  for (const auto& [s0, s1] : action.raw_pairs()) {
    EXPECT_LE(s0->getMin(), s1->getMin());
    EXPECT_TRUE(IntervalsIntersect(*s0, *s1));
  }

  const std::vector<std::pair<int, int>> canonical =
      action.canonical_id_pairs();
  EXPECT_EQ(std::adjacent_find(canonical.begin(), canonical.end()),
            canonical.end())
      << "Duplicate overlap pair reported";
  EXPECT_EQ(canonical, BruteForceOverlapPairs(records));
}

TEST(SweepLineIndexTest, EmptyIndexReportsNoOverlapsAcrossRepeatedCalls) {
  SweepLineIndex index;
  RecordingOverlapAction action;

  index.computeOverlaps(&action);
  EXPECT_THAT(action.raw_pairs(), IsEmpty());

  // Second call exercises the indexBuilt == true fast path in buildIndex().
  index.computeOverlaps(&action);
  EXPECT_THAT(action.raw_pairs(), IsEmpty());
}

TEST(SweepLineIndexTest, SingleIntervalReportsSelfOverlapOnly) {
  SweepLineIndex index;
  int id = 1;
  SweepLineInterval interval(2.0, 5.0, &id);
  AddInterval(index, &interval);

  RecordingOverlapAction first_action;
  index.computeOverlaps(&first_action);
  EXPECT_THAT(first_action.raw_pairs(),
              ElementsAre(Pair(&interval, &interval)));
  EXPECT_THAT(first_action.id_pairs(), ElementsAre(Pair(1, 1)));

  RecordingOverlapAction second_action;
  index.computeOverlaps(&second_action);
  EXPECT_THAT(second_action.raw_pairs(),
              ElementsAre(Pair(&interval, &interval)));
  EXPECT_THAT(second_action.id_pairs(), ElementsAre(Pair(1, 1)));
}

TEST(SweepLineIndexTest, SinglePointIntervalReportsSelfOverlap) {
  SweepLineIndex index;
  int id = 10;
  SweepLineInterval point(3.5, 3.5, &id);
  AddInterval(index, &point);

  RecordingOverlapAction action;
  index.computeOverlaps(&action);
  EXPECT_THAT(action.raw_pairs(), ElementsAre(Pair(&point, &point)));
  EXPECT_THAT(action.id_pairs(), ElementsAre(Pair(10, 10)));
}

TEST(SweepLineIndexTest, DisjointIntervalsReportOnlySelfOverlaps) {
  SweepLineIndex index;
  std::deque<IntervalRecord> records =
      BuildAndAddRecords(index, {
                                    {.min = 10.0, .max = 12.0, .id = 1},
                                    {.min = -5.0, .max = -1.0, .id = 2},
                                    {.min = 1.0, .max = 3.0, .id = 3},
                                    {.min = 5.0, .max = 7.0, .id = 4},
                                });

  RecordingOverlapAction action;
  index.computeOverlaps(&action);
  EXPECT_THAT(action.id_pairs(),
              ElementsAre(Pair(2, 2), Pair(3, 3), Pair(4, 4), Pair(1, 1)));
  VerifyMatchesBruteForce(index, records);
}

TEST(SweepLineIndexTest, TouchingEndpointsOverlapWhileUlpSeparatedDoNot) {
  SweepLineIndex touching_index;
  std::deque<IntervalRecord> touching_records =
      BuildAndAddRecords(touching_index, {
                                             {.min = 3.0, .max = 5.0, .id = 2},
                                             {.min = 1.0, .max = 3.0, .id = 1},
                                         });
  RecordingOverlapAction touching_action;
  touching_index.computeOverlaps(&touching_action);
  EXPECT_THAT(touching_action.id_pairs(),
              ElementsAre(Pair(1, 1), Pair(1, 2), Pair(2, 2)));
  VerifyMatchesBruteForce(touching_index, touching_records);

  SweepLineIndex separated_index;
  const double after_three = std::nextafter(3.0, 4.0);
  std::deque<IntervalRecord> separated_records = BuildAndAddRecords(
      separated_index, {
                           {.min = after_three, .max = 5.0, .id = 2},
                           {.min = 1.0, .max = 3.0, .id = 1},
                       });
  RecordingOverlapAction separated_action;
  separated_index.computeOverlaps(&separated_action);
  EXPECT_THAT(separated_action.id_pairs(), ElementsAre(Pair(1, 1), Pair(2, 2)));
  VerifyMatchesBruteForce(separated_index, separated_records);
}

TEST(SweepLineIndexTest, NestedIntervalsSkipInterveningDeleteEvents) {
  SweepLineIndex index;
  // Inner intervals end well before outer ends, ensuring DELETE_EVENTs appear
  // inside outer's [start, end) sweep window in processOverlaps().
  std::deque<IntervalRecord> records =
      BuildAndAddRecords(index, {
                                    {.min = 15.0, .max = 18.0, .id = 4},
                                    {.min = 0.0, .max = 20.0, .id = 1},
                                    {.min = 8.0, .max = 12.0, .id = 3},
                                    {.min = 2.0, .max = 5.0, .id = 2},
                                });

  RecordingOverlapAction action;
  index.computeOverlaps(&action);
  EXPECT_THAT(action.id_pairs(),
              ElementsAre(Pair(1, 1), Pair(1, 2), Pair(1, 3), Pair(1, 4),
                          Pair(2, 2), Pair(3, 3), Pair(4, 4)));
  VerifyMatchesBruteForce(index, records);
}

TEST(SweepLineIndexTest, IdenticalAndDuplicatePointsReportAllPairsOnce) {
  SweepLineIndex span_index;
  std::deque<IntervalRecord> span_records =
      BuildAndAddRecords(span_index, {
                                         {.min = 2.0, .max = 6.0, .id = 1},
                                         {.min = 2.0, .max = 6.0, .id = 2},
                                         {.min = 2.0, .max = 6.0, .id = 3},
                                         {.min = 2.0, .max = 6.0, .id = 4},
                                     });
  VerifyMatchesBruteForce(span_index, span_records);

  SweepLineIndex point_index;
  std::deque<IntervalRecord> point_records =
      BuildAndAddRecords(point_index, {
                                          {.min = 4.0, .max = 4.0, .id = 1},
                                          {.min = 4.0, .max = 4.0, .id = 2},
                                          {.min = 4.0, .max = 4.0, .id = 3},
                                      });
  VerifyMatchesBruteForce(point_index, point_records);
}

TEST(SweepLineIndexTest, PointIntervalsWithSpanAndEndpoints) {
  SweepLineIndex index;
  std::deque<IntervalRecord> records =
      BuildAndAddRecords(index, {
                                    {.min = 6.0, .max = 6.0, .id = 6},
                                    {.min = 5.0, .max = 5.0, .id = 5},
                                    {.min = 3.0, .max = 3.0, .id = 4},
                                    {.min = 3.0, .max = 3.0, .id = 3},
                                    {.min = 1.0, .max = 1.0, .id = 2},
                                    {.min = 0.0, .max = 0.0, .id = 1},
                                    {.min = 1.0, .max = 5.0, .id = 7},
                                });
  VerifyMatchesBruteForce(index, records);
}

TEST(SweepLineIndexTest, ReversedIntervalBoundsAreNormalizedBeforeIndexing) {
  SweepLineIndex index;
  std::deque<IntervalRecord> records =
      BuildAndAddRecords(index, {
                                    {.min = 10.0, .max = 2.0, .id = 1},
                                    {.min = 8.0, .max = 5.0, .id = 2},
                                    {.min = 1.0, .max = -3.0, .id = 3},
                                });

  RecordingOverlapAction action;
  index.computeOverlaps(&action);
  EXPECT_THAT(action.id_pairs(),
              ElementsAre(Pair(3, 3), Pair(1, 1), Pair(1, 2), Pair(2, 2)));
  VerifyMatchesBruteForce(index, records);
}

TEST(SweepLineIndexTest, PreservesNullItemPointersInOverlapCallbacks) {
  SweepLineIndex index;
  SweepLineInterval first(0.0, 4.0);
  SweepLineInterval second(2.0, 6.0);
  SweepLineInterval third(10.0, 12.0);
  AddInterval(index, &first);
  AddInterval(index, &second);
  AddInterval(index, &third);

  RecordingOverlapAction action;
  index.computeOverlaps(&action);
  EXPECT_THAT(action.raw_pairs(),
              ElementsAre(Pair(&first, &first), Pair(&first, &second),
                          Pair(&second, &second), Pair(&third, &third)));
  for (const auto& [s0, s1] : action.raw_pairs()) {
    EXPECT_EQ(s0->getItem(), nullptr);
    EXPECT_EQ(s1->getItem(), nullptr);
  }
}

TEST(SweepLineIndexTest, DiverseIntervalsAndIdempotentRecomputation) {
  constexpr auto kSpecs = std::to_array<IntervalSpec>({
      {.min = 25.0, .max = 30.0, .id = 1},
      {.min = -50.0, .max = -40.0, .id = 2},
      {.min = 7.0, .max = 7.0, .id = 3},
      {.min = 10.0, .max = 20.0, .id = 4},
      {.min = 14.0, .max = 16.0, .id = 5},
      {.min = 25.0, .max = 30.0, .id = 6},
      {.min = -5.0, .max = 5.0, .id = 7},
      {.min = 0.0, .max = 0.0, .id = 8},
      {.min = 1.0, .max = 2.0, .id = 9},
      {.min = 2.0, .max = 3.0, .id = 10},
      {.min = -1e6, .max = -999990.0, .id = 11},
      {.min = 1e6, .max = 1000010.0, .id = 12},
      {.min = 3.5, .max = 4.5, .id = 13},
      {.min = -20.0, .max = -15.0, .id = 14},
      {.min = 100.0, .max = 105.0, .id = 15},
      {.min = -42.0, .max = -18.0, .id = 16},
      {.min = 16.0, .max = 25.0, .id = 17},
      {.min = -5.0, .max = 15.0, .id = 18},
  });

  SweepLineIndex index;
  std::deque<IntervalRecord> records =
      BuildAndAddRecords(index, {kSpecs.begin(), kSpecs.end()});

  VerifyMatchesBruteForce(index, records);
  // Repeat after indexBuilt == true to confirm idempotent overlap reporting.
  VerifyMatchesBruteForce(index, records);
}

TEST(SweepLineIndexTest, InfiniteBoundsSortAndOverlapCorrectly) {
  constexpr double kInf = std::numeric_limits<double>::infinity();
  SweepLineIndex index;
  std::deque<IntervalRecord> records =
      BuildAndAddRecords(index, {
                                    {.min = 5.0, .max = kInf, .id = 1},
                                    {.min = -kInf, .max = -10.0, .id = 2},
                                    {.min = -kInf, .max = kInf, .id = 3},
                                    {.min = -20.0, .max = 5.0, .id = 4},
                                    {.min = 0.0, .max = 10.0, .id = 5},
                                });
  VerifyMatchesBruteForce(index, records);
}

TEST(SweepLineIndexTest, DensePermutedIntervalsMatchBruteForce) {
  constexpr std::size_t kCount = 40;
  std::vector<IntervalSpec> specs;
  specs.reserve(kCount);
  for (std::size_t i = 0; i < kCount; ++i) {
    const std::size_t perm = (i * 17 + 5) % kCount;
    const double start = static_cast<double>(perm) * 1.5;
    const double width = 2.0 + static_cast<double>(perm % 5);
    specs.push_back(
        {.min = start, .max = start + width, .id = static_cast<int>(perm)});
  }

  SweepLineIndex index;
  std::deque<IntervalRecord> records = BuildAndAddRecords(index, specs);
  VerifyMatchesBruteForce(index, records);
}

TEST(SweepLineIndexTest, ActionExceptionPropagatesAndIndexRemainsReusable) {
  SweepLineIndex index;
  std::deque<IntervalRecord> records =
      BuildAndAddRecords(index, {
                                    {.min = 0.0, .max = 5.0, .id = 1},
                                    {.min = 2.0, .max = 7.0, .id = 2},
                                });

  ThrowingOverlapAction throwing_action(/*throw_on_call=*/2);
  EXPECT_THROW(index.computeOverlaps(&throwing_action), std::runtime_error);

  VerifyMatchesBruteForce(index, records);
}

}  // namespace
}  // namespace geos_google
