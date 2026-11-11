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

// Unit tests for third_party/geos/src/index/sweepline/SweepLineEvent.cpp.
//
// google3 notes:
// - SweepLineEvent leaves deleteEventIndex uninitialized in its constructor;
//   tests always call setDeleteEventIndex() before getDeleteEventIndex() to
//   keep MSan clean.
// - SweepLineEvent borrows raw pointers to its paired insert SweepLineEvent
//   and SweepLineInterval; callers retain ownership and lifetime control.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/index/sweepline/SweepLineEvent.cpp

#include "third_party/geos/include/geos/index/sweepline/SweepLineEvent.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/index/sweepline/SweepLineInterval.h"

namespace geos_google {
namespace {

using geos::index::sweepline::SweepLineEvent;
using geos::index::sweepline::SweepLineEventLessThen;
using geos::index::sweepline::SweepLineInterval;

static_assert(+SweepLineEvent::INSERT_EVENT == 1);
static_assert(+SweepLineEvent::DELETE_EVENT == 2);
static_assert(+SweepLineEvent::INSERT_EVENT < +SweepLineEvent::DELETE_EVENT);

TEST(SweepLineEventTest, InsertAndDeleteEventAccessors) {
  int payload = 42;
  SweepLineInterval interval(1.5, 4.5, &payload);

  SweepLineEvent insert_ev(interval.getMin(), nullptr, &interval);
  EXPECT_TRUE(insert_ev.isInsert());
  EXPECT_FALSE(insert_ev.isDelete());
  EXPECT_EQ(insert_ev.getInsertEvent(), nullptr);
  EXPECT_EQ(insert_ev.getInterval(), &interval);

  SweepLineEvent delete_ev(interval.getMax(), &insert_ev, &interval);
  EXPECT_FALSE(delete_ev.isInsert());
  EXPECT_TRUE(delete_ev.isDelete());
  EXPECT_EQ(delete_ev.getInsertEvent(), &insert_ev);
  EXPECT_EQ(delete_ev.getInterval(), &interval);

  SweepLineEvent null_interval_ev(0.0, nullptr, nullptr);
  EXPECT_TRUE(null_interval_ev.isInsert());
  EXPECT_FALSE(null_interval_ev.isDelete());
  EXPECT_EQ(null_interval_ev.getInsertEvent(), nullptr);
  EXPECT_EQ(null_interval_ev.getInterval(), nullptr);
}

TEST(SweepLineEventTest, SetAndGetDeleteEventIndex) {
  SweepLineInterval interval(0.0, 10.0);
  SweepLineEvent insert_ev(0.0, nullptr, &interval);

  insert_ev.setDeleteEventIndex(0U);
  EXPECT_EQ(insert_ev.getDeleteEventIndex(), 0U);

  insert_ev.setDeleteEventIndex(7U);
  EXPECT_EQ(insert_ev.getDeleteEventIndex(), 7U);

  constexpr std::size_t kMaxIndex = std::numeric_limits<std::size_t>::max();
  insert_ev.setDeleteEventIndex(kMaxIndex);
  EXPECT_EQ(insert_ev.getDeleteEventIndex(), kMaxIndex);
}

TEST(SweepLineEventTest, CompareToOrdersByXThenEventType) {
  SweepLineInterval interval_a(-5.0, 2.0);
  SweepLineInterval interval_b(2.0, 6.0);
  SweepLineInterval point_interval(2.0, 2.0);

  SweepLineEvent ins_neg(-5.0, nullptr, &interval_a);
  SweepLineEvent ins_two_a(2.0, nullptr, &interval_b);
  SweepLineEvent ins_two_b(2.0, nullptr, &point_interval);
  SweepLineEvent del_two_a(2.0, &ins_neg, &interval_a);
  SweepLineEvent del_two_b(2.0, &ins_two_b, &point_interval);
  SweepLineEvent del_six(6.0, &ins_two_a, &interval_b);

  // Different x-values: x dominates eventType.
  EXPECT_EQ(ins_neg.compareTo(&ins_two_a), -1);
  EXPECT_EQ(ins_two_a.compareTo(&ins_neg), 1);
  EXPECT_EQ(del_two_a.compareTo(&ins_neg), 1);
  EXPECT_EQ(ins_neg.compareTo(&del_two_a), -1);
  EXPECT_EQ(ins_two_a.compareTo(&del_six), -1);
  EXPECT_EQ(del_six.compareTo(&ins_two_a), 1);

  // Equal x-values: INSERT_EVENT sorts before DELETE_EVENT.
  EXPECT_EQ(ins_two_a.compareTo(&del_two_a), -1);
  EXPECT_EQ(del_two_a.compareTo(&ins_two_a), 1);

  // Equal x-values and equal eventType compare equal.
  EXPECT_EQ(ins_two_a.compareTo(&ins_two_b), 0);
  EXPECT_EQ(del_two_a.compareTo(&del_two_b), 0);
  EXPECT_EQ(ins_two_a.compareTo(&ins_two_a), 0);
  EXPECT_EQ(del_two_a.compareTo(&del_two_a), 0);

  // Signed zeros compare numerically equal, breaking ties by eventType.
  SweepLineInterval zero_interval(0.0, 0.0);
  SweepLineEvent ins_neg_zero(-0.0, nullptr, &zero_interval);
  SweepLineEvent del_pos_zero(+0.0, &ins_neg_zero, &zero_interval);
  SweepLineEvent ins_pos_zero(+0.0, nullptr, &zero_interval);
  EXPECT_EQ(ins_neg_zero.compareTo(&ins_pos_zero), 0);
  EXPECT_EQ(ins_neg_zero.compareTo(&del_pos_zero), -1);
  EXPECT_EQ(del_pos_zero.compareTo(&ins_neg_zero), 1);
}

TEST(SweepLineEventTest, LessThenFunctorAndSortingZeroWidthIntervals) {
  SweepLineEventLessThen less;

  SweepLineInterval iv_left(1.0, 5.0);
  SweepLineInterval iv_point(3.0, 3.0);
  SweepLineInterval iv_touch(5.0, 8.0);

  SweepLineEvent ins_1(1.0, nullptr, &iv_left);
  SweepLineEvent del_5(5.0, &ins_1, &iv_left);
  SweepLineEvent ins_3(3.0, nullptr, &iv_point);
  SweepLineEvent del_3(3.0, &ins_3, &iv_point);
  SweepLineEvent ins_5(5.0, nullptr, &iv_touch);
  SweepLineEvent del_8(8.0, &ins_5, &iv_touch);

  // Direct predicate truth-table checks.
  EXPECT_TRUE(less(&ins_1, &ins_3));
  EXPECT_FALSE(less(&ins_3, &ins_1));
  EXPECT_TRUE(less(&ins_3, &del_3));
  EXPECT_FALSE(less(&del_3, &ins_3));
  EXPECT_TRUE(less(&ins_5, &del_5));
  EXPECT_FALSE(less(&del_5, &ins_5));
  EXPECT_FALSE(less(&ins_3, &ins_3));
  EXPECT_FALSE(less(&del_3, &del_3));

  // Unsorted input puts DELETE events before same-x INSERT events; std::sort
  // with SweepLineEventLessThen restores INSERT before DELETE.
  std::vector<SweepLineEvent*> events = {
      &del_8, &del_3, &del_5, &ins_5, &ins_3, &ins_1,
  };
  std::sort(events.begin(), events.end(), less);

  const std::array<const SweepLineEvent*, 6> expected_order = {
      &ins_1, &ins_3, &del_3, &ins_5, &del_5, &del_8,
  };
  ASSERT_EQ(events.size(), expected_order.size());
  for (std::size_t i = 0; i < events.size(); ++i) {
    EXPECT_EQ(events[i], expected_order[i]);
  }

  for (std::size_t i = 0; i < events.size(); ++i) {
    if (events[i]->isDelete()) {
      events[i]->getInsertEvent()->setDeleteEventIndex(i);
    }
  }
  EXPECT_EQ(ins_1.getDeleteEventIndex(), 4U);
  EXPECT_EQ(ins_3.getDeleteEventIndex(), 2U);
  EXPECT_EQ(ins_5.getDeleteEventIndex(), 5U);
}

}  // namespace
}  // namespace geos_google
