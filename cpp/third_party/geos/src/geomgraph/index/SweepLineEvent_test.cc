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

#include "third_party/geos/include/geos/geomgraph/index/SweepLineEvent.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geomgraph/index/SweepLineEventObj.h"

namespace geos_google {
namespace {

using geos::geomgraph::index::SweepLineEvent;
using geos::geomgraph::index::SweepLineEventLessThen;
using geos::geomgraph::index::SweepLineEventOBJ;

class TestSweepObj final : public SweepLineEventOBJ {
 public:
  explicit TestSweepObj(int id) : id_(id) {}
  int id() const { return id_; }

 private:
  int id_;
};

TEST(SweepLineEventTest, InsertEventConstructionAndAccessors) {
  int edge_set_tag = 7;
  TestSweepObj obj(101);
  SweepLineEvent ev(&edge_set_tag, -3.5, nullptr, &obj);

  EXPECT_TRUE(ev.isInsert());
  EXPECT_FALSE(ev.isDelete());
  EXPECT_EQ(ev.eventType(), +SweepLineEvent::INSERT_EVENT);
  EXPECT_EQ(ev.getInsertEvent(), nullptr);
  EXPECT_EQ(ev.getDeleteEventIndex(), 0U);
  EXPECT_EQ(ev.getObject(), &obj);
  EXPECT_EQ(ev.edgeSet, &edge_set_tag);

  SweepLineEvent null_ev(nullptr, 0.0, nullptr, nullptr);
  EXPECT_TRUE(null_ev.isInsert());
  EXPECT_EQ(null_ev.getObject(), nullptr);
  EXPECT_EQ(null_ev.edgeSet, nullptr);
}

TEST(SweepLineEventTest, DeleteEventConstructionAndAccessors) {
  int edge_set_tag = 42;
  TestSweepObj obj(202);
  SweepLineEvent insert_ev(&edge_set_tag, 1.0, nullptr, &obj);
  SweepLineEvent delete_ev(&edge_set_tag, 4.5, &insert_ev, &obj);

  EXPECT_FALSE(delete_ev.isInsert());
  EXPECT_TRUE(delete_ev.isDelete());
  EXPECT_EQ(delete_ev.eventType(), +SweepLineEvent::DELETE_EVENT);
  EXPECT_LT(+SweepLineEvent::INSERT_EVENT, +SweepLineEvent::DELETE_EVENT);
  EXPECT_EQ(delete_ev.getInsertEvent(), &insert_ev);
  EXPECT_EQ(delete_ev.getDeleteEventIndex(), 0U);
  EXPECT_EQ(delete_ev.getObject(), &obj);
  EXPECT_EQ(delete_ev.edgeSet, &edge_set_tag);
}

TEST(SweepLineEventTest, DeleteEventIndexGetAndSet) {
  TestSweepObj obj(1);
  SweepLineEvent ev(nullptr, 2.0, nullptr, &obj);
  EXPECT_EQ(ev.getDeleteEventIndex(), 0U);

  ev.setDeleteEventIndex(17U);
  EXPECT_EQ(ev.getDeleteEventIndex(), 17U);

  constexpr std::size_t kMaxIndex = std::numeric_limits<std::size_t>::max();
  ev.setDeleteEventIndex(kMaxIndex);
  EXPECT_EQ(ev.getDeleteEventIndex(), kMaxIndex);

  ev.setDeleteEventIndex(0U);
  EXPECT_EQ(ev.getDeleteEventIndex(), 0U);
}

TEST(SweepLineEventTest, CompareToOrdersByXThenInsertBeforeDelete) {
  TestSweepObj obj_a(1);
  TestSweepObj obj_b(2);

  SweepLineEvent ins_neg(nullptr, -5.0, nullptr, &obj_a);
  SweepLineEvent ins_1(nullptr, 1.0, nullptr, &obj_a);
  SweepLineEvent del_1(nullptr, 1.0, &ins_neg, &obj_a);
  SweepLineEvent ins_1_b(nullptr, 1.0, nullptr, &obj_b);
  SweepLineEvent del_1_b(nullptr, 1.0, &ins_neg, &obj_b);
  SweepLineEvent ins_2(nullptr, 2.5, nullptr, &obj_b);
  SweepLineEvent del_2(nullptr, 2.5, &ins_1, &obj_b);

  // Distinct xValue comparisons.
  EXPECT_EQ(ins_neg.compareTo(&ins_1), -1);
  EXPECT_EQ(ins_1.compareTo(&ins_neg), 1);
  EXPECT_EQ(del_1.compareTo(&ins_2), -1);
  EXPECT_EQ(ins_2.compareTo(&del_1), 1);
  EXPECT_EQ(ins_1.compareTo(&del_2), -1);
  EXPECT_EQ(del_2.compareTo(&ins_1), 1);

  // Equal xValue: INSERT_EVENT sorts before DELETE_EVENT.
  EXPECT_EQ(ins_1.compareTo(&del_1), -1);
  EXPECT_EQ(del_1.compareTo(&ins_1), 1);

  // Equal xValue and equal eventType: returns 0.
  EXPECT_EQ(ins_1.compareTo(&ins_1), 0);
  EXPECT_EQ(ins_1.compareTo(&ins_1_b), 0);
  EXPECT_EQ(del_1.compareTo(&del_1), 0);
  EXPECT_EQ(del_1.compareTo(&del_1_b), 0);

  // Signed zero equality.
  SweepLineEvent ins_pos_zero(nullptr, 0.0, nullptr, &obj_a);
  SweepLineEvent ins_neg_zero(nullptr, -0.0, nullptr, &obj_b);
  SweepLineEvent del_pos_zero(nullptr, 0.0, &ins_neg_zero, &obj_b);
  EXPECT_EQ(ins_neg_zero.compareTo(&ins_pos_zero), 0);
  EXPECT_EQ(ins_neg_zero.compareTo(&del_pos_zero), -1);
}

TEST(SweepLineEventTest, CompareToWithInfiniteXValues) {
  constexpr double kInf = std::numeric_limits<double>::infinity();
  SweepLineEvent ev_neg_inf(nullptr, -kInf, nullptr, nullptr);
  SweepLineEvent ev_zero(nullptr, 0.0, nullptr, nullptr);
  SweepLineEvent ev_pos_inf_ins(nullptr, kInf, nullptr, nullptr);
  SweepLineEvent ev_pos_inf_del(nullptr, kInf, &ev_neg_inf, nullptr);

  EXPECT_EQ(ev_neg_inf.compareTo(&ev_zero), -1);
  EXPECT_EQ(ev_zero.compareTo(&ev_neg_inf), 1);
  EXPECT_EQ(ev_zero.compareTo(&ev_pos_inf_ins), -1);
  EXPECT_EQ(ev_pos_inf_ins.compareTo(&ev_zero), 1);
  EXPECT_EQ(ev_pos_inf_ins.compareTo(&ev_pos_inf_del), -1);
  EXPECT_EQ(ev_pos_inf_del.compareTo(&ev_pos_inf_ins), 1);
  EXPECT_EQ(ev_pos_inf_ins.compareTo(&ev_pos_inf_ins), 0);
}

TEST(SweepLineEventTest, LessThenComparatorAndSorting) {
  TestSweepObj obj0(0);
  TestSweepObj obj1(1);
  TestSweepObj obj2(2);

  // Zero-width segment at x=2.0 plus overlapping segments [0.0, 5.0] and
  // [2.0, 4.0].
  auto ins_a = std::make_unique<SweepLineEvent>(nullptr, 0.0, nullptr, &obj0);
  auto del_a =
      std::make_unique<SweepLineEvent>(nullptr, 5.0, ins_a.get(), &obj0);
  auto ins_b = std::make_unique<SweepLineEvent>(nullptr, 2.0, nullptr, &obj1);
  auto del_b =
      std::make_unique<SweepLineEvent>(nullptr, 4.0, ins_b.get(), &obj1);
  auto ins_pt = std::make_unique<SweepLineEvent>(nullptr, 2.0, nullptr, &obj2);
  auto del_pt =
      std::make_unique<SweepLineEvent>(nullptr, 2.0, ins_pt.get(), &obj2);

  SweepLineEventLessThen less;
  // Direct predicate checks for all branches.
  EXPECT_TRUE(less(ins_a.get(), ins_b.get()));
  EXPECT_FALSE(less(ins_b.get(), ins_a.get()));
  EXPECT_TRUE(less(ins_pt.get(), del_pt.get()));
  EXPECT_FALSE(less(del_pt.get(), ins_pt.get()));
  EXPECT_FALSE(less(ins_b.get(), ins_pt.get()));
  EXPECT_FALSE(less(del_b.get(), del_b.get()));
  // Verify template works on smart pointers as well as raw pointers.
  EXPECT_TRUE(less(ins_a, del_a));
  EXPECT_FALSE(less(del_a, ins_a));

  std::vector<SweepLineEvent*> events = {
      del_a.get(), del_pt.get(), del_b.get(),
      ins_b.get(), ins_pt.get(), ins_a.get(),
  };
  std::sort(events.begin(), events.end(), less);

  // Populate deleteEventIndex as sweepline algorithms do.
  for (std::size_t i = 0; i < events.size(); ++i) {
    if (events[i]->isDelete()) {
      events[i]->getInsertEvent()->setDeleteEventIndex(i);
    }
  }

  ASSERT_EQ(events.size(), 6U);
  EXPECT_EQ(events[0], ins_a.get());
  // At x=2.0, both inserts precede del_pt.
  EXPECT_TRUE(events[1]->isInsert());
  EXPECT_TRUE(events[2]->isInsert());
  EXPECT_EQ(events[3], del_pt.get());
  EXPECT_EQ(events[4], del_b.get());
  EXPECT_EQ(events[5], del_a.get());

  EXPECT_EQ(ins_a->getDeleteEventIndex(), 5U);
  EXPECT_EQ(ins_b->getDeleteEventIndex(), 4U);
  EXPECT_EQ(ins_pt->getDeleteEventIndex(), 3U);
}

TEST(SweepLineEventTest, PrintFormatsInsertAndDeleteEvents) {
  TestSweepObj obj(1);
  SweepLineEvent insert_ev(nullptr, 1.5, nullptr, &obj);
  EXPECT_EQ(insert_ev.print(),
            "SweepLineEvent: xValue=1.5 deleteEventIndex=0 INSERT_EVENT\n"
            "\tinsertEvent=NULL");

  insert_ev.setDeleteEventIndex(7);
  EXPECT_EQ(insert_ev.print(),
            "SweepLineEvent: xValue=1.5 deleteEventIndex=7 INSERT_EVENT\n"
            "\tinsertEvent=NULL");

  SweepLineEvent delete_ev(nullptr, 4.25, &insert_ev, &obj);
  EXPECT_EQ(delete_ev.print(),
            "SweepLineEvent: xValue=4.25 deleteEventIndex=0 DELETE_EVENT\n"
            "\tinsertEvent=SweepLineEvent: xValue=1.5 deleteEventIndex=7 "
            "INSERT_EVENT\n"
            "\tinsertEvent=NULL");
}

TEST(SweepLineEventTest, PolymorphicSweepLineEventObjOwnership) {
  std::unique_ptr<SweepLineEventOBJ> obj = std::make_unique<TestSweepObj>(99);
  SweepLineEvent ev(nullptr, -2.0, nullptr, obj.get());

  EXPECT_EQ(ev.getObject(), obj.get());
  auto* typed = dynamic_cast<TestSweepObj*>(ev.getObject());
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(typed->id(), 99);
}

}  // namespace
}  // namespace geos_google
