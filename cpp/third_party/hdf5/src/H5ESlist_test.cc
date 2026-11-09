// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests internal event set doubly-linked list helpers
// (third_party/hdf5/src/H5ESlist.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5ESlist.c

#include <cstddef>
#include <cstdint>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` compiles with -DNDEBUG; match internal struct layouts.
#ifndef NDEBUG
#define H5ESLIST_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5ES_FRIEND
#include "third_party/hdf5/src/H5ESpkg.h"
}  // extern "C"
#ifdef H5ESLIST_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5ESLIST_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

H5ES_event_t MakeEvent(uint64_t id) {
  H5ES_event_t ev{};
  ev.op_info.op_ins_count = id;
  return ev;
}

int RecordCallback(H5ES_event_t* ev, void* ctx) {
  auto* ids = static_cast<std::vector<uint64_t>*>(ctx);
  ids->push_back(ev->op_info.op_ins_count);
  return H5_ITER_CONT;
}

TEST(H5ESlistTest, EmptyListCountAndIterate) {
  H5ES_event_list_t list{};
  EXPECT_EQ(H5ES__list_count(&list), 0u);

  for (H5_iter_order_t order : {H5_ITER_INC, H5_ITER_NATIVE, H5_ITER_DEC}) {
    std::vector<uint64_t> visited;
    EXPECT_EQ(H5ES__list_iterate(&list, order, RecordCallback, &visited),
              H5_ITER_CONT);
    EXPECT_THAT(visited, IsEmpty());
  }
}

TEST(H5ESlistTest, AppendAndIterateOrders) {
  H5ES_event_list_t list{};
  H5ES_event_t ev1 = MakeEvent(10);
  H5ES_event_t ev2 = MakeEvent(20);
  H5ES_event_t ev3 = MakeEvent(30);

  H5ES__list_append(&list, &ev1);
  EXPECT_EQ(H5ES__list_count(&list), 1u);
  EXPECT_EQ(list.head, &ev1);
  EXPECT_EQ(list.tail, &ev1);

  H5ES__list_append(&list, &ev2);
  H5ES__list_append(&list, &ev3);
  EXPECT_EQ(H5ES__list_count(&list), 3u);
  EXPECT_EQ(list.head, &ev1);
  EXPECT_EQ(list.tail, &ev3);

  std::vector<uint64_t> inc_ids;
  EXPECT_EQ(H5ES__list_iterate(&list, H5_ITER_INC, RecordCallback, &inc_ids),
            H5_ITER_CONT);
  EXPECT_THAT(inc_ids, ElementsAre(10, 20, 30));

  std::vector<uint64_t> native_ids;
  EXPECT_EQ(
      H5ES__list_iterate(&list, H5_ITER_NATIVE, RecordCallback, &native_ids),
      H5_ITER_CONT);
  EXPECT_THAT(native_ids, ElementsAre(10, 20, 30));

  std::vector<uint64_t> dec_ids;
  EXPECT_EQ(H5ES__list_iterate(&list, H5_ITER_DEC, RecordCallback, &dec_ids),
            H5_ITER_CONT);
  EXPECT_THAT(dec_ids, ElementsAre(30, 20, 10));
}

TEST(H5ESlistTest, IterateStopAndErrorTerminateEarly) {
  H5ES_event_list_t list{};
  H5ES_event_t ev1 = MakeEvent(1);
  H5ES_event_t ev2 = MakeEvent(2);
  H5ES_event_t ev3 = MakeEvent(3);
  H5ES__list_append(&list, &ev1);
  H5ES__list_append(&list, &ev2);
  H5ES__list_append(&list, &ev3);

  struct IterState {
    int stop_status = H5_ITER_STOP;
    uint64_t target_id = 2;
    std::vector<uint64_t> visited;
  };

  auto stop_cb = [](H5ES_event_t* ev, void* ctx) -> int {
    auto* st = static_cast<IterState*>(ctx);
    st->visited.push_back(ev->op_info.op_ins_count);
    return (ev->op_info.op_ins_count == st->target_id) ? st->stop_status
                                                       : H5_ITER_CONT;
  };

  IterState stop_state{H5_ITER_STOP, 2, {}};
  EXPECT_EQ(H5ES__list_iterate(&list, H5_ITER_INC, stop_cb, &stop_state),
            H5_ITER_STOP);
  EXPECT_THAT(stop_state.visited, ElementsAre(1, 2));

  // Ensure HDF5 and H5E are initialized and quiet when HERROR runs inside
  // H5ES__list_iterate on H5_ITER_ERROR.
  ScopedErrorSilence silence;
  IterState err_state{H5_ITER_ERROR, 2, {}};
  EXPECT_EQ(H5ES__list_iterate(&list, H5_ITER_DEC, stop_cb, &err_state),
            H5_ITER_ERROR);
  EXPECT_THAT(err_state.visited, ElementsAre(3, 2));
}

TEST(H5ESlistTest, RemoveSingleMiddleHeadAndTail) {
  // 1. Single node removal clears both head and tail.
  H5ES_event_list_t single{};
  H5ES_event_t only = MakeEvent(100);
  H5ES__list_append(&single, &only);
  H5ES__list_remove(&single, &only);
  EXPECT_EQ(H5ES__list_count(&single), 0u);
  EXPECT_EQ(single.head, nullptr);
  EXPECT_EQ(single.tail, nullptr);

  // 2. Remove middle, head, then final node from a 3-node list.
  H5ES_event_list_t list{};
  H5ES_event_t ev1 = MakeEvent(1);
  H5ES_event_t ev2 = MakeEvent(2);
  H5ES_event_t ev3 = MakeEvent(3);
  H5ES__list_append(&list, &ev1);
  H5ES__list_append(&list, &ev2);
  H5ES__list_append(&list, &ev3);

  H5ES__list_remove(&list, &ev2);
  EXPECT_EQ(H5ES__list_count(&list), 2u);
  EXPECT_EQ(list.head, &ev1);
  EXPECT_EQ(list.tail, &ev3);

  std::vector<uint64_t> ids;
  EXPECT_EQ(H5ES__list_iterate(&list, H5_ITER_INC, RecordCallback, &ids),
            H5_ITER_CONT);
  EXPECT_THAT(ids, ElementsAre(1, 3));
  ids.clear();
  EXPECT_EQ(H5ES__list_iterate(&list, H5_ITER_DEC, RecordCallback, &ids),
            H5_ITER_CONT);
  EXPECT_THAT(ids, ElementsAre(3, 1));

  H5ES__list_remove(&list, &ev1);
  EXPECT_EQ(H5ES__list_count(&list), 1u);
  EXPECT_EQ(list.head, &ev3);
  EXPECT_EQ(list.tail, &ev3);

  H5ES__list_remove(&list, &ev3);
  EXPECT_EQ(H5ES__list_count(&list), 0u);
  EXPECT_EQ(list.head, nullptr);
  EXPECT_EQ(list.tail, nullptr);

  // 3. Remove non-head tail node (`ev_tail.next == nullptr` while `head !=
  // nullptr`): upstream H5ES__list_remove only updates `el->tail` when
  // `el->head == nullptr`, so keep `ev_tail` alive until `ev_head` is removed
  // and inspect via forward iteration only.
  H5ES_event_list_t two{};
  H5ES_event_t ev_head = MakeEvent(11);
  H5ES_event_t ev_tail = MakeEvent(22);
  H5ES__list_append(&two, &ev_head);
  H5ES__list_append(&two, &ev_tail);
  H5ES__list_remove(&two, &ev_tail);
  EXPECT_EQ(H5ES__list_count(&two), 1u);
  EXPECT_EQ(two.head, &ev_head);
  EXPECT_EQ(ev_head.next, nullptr);

  std::vector<uint64_t> fwd_ids;
  EXPECT_EQ(H5ES__list_iterate(&two, H5_ITER_INC, RecordCallback, &fwd_ids),
            H5_ITER_CONT);
  EXPECT_THAT(fwd_ids, ElementsAre(11));

  H5ES__list_remove(&two, &ev_head);
  EXPECT_EQ(H5ES__list_count(&two), 0u);
  EXPECT_EQ(two.head, nullptr);
  EXPECT_EQ(two.tail, nullptr);
}

TEST(H5ESlistTest, RemoveCurrentNodeDuringForwardAndReverseIteration) {
  struct RemoveCtx {
    H5ES_event_list_t* list = nullptr;
    std::vector<uint64_t> removed_ids;
  };

  auto remove_cb = [](H5ES_event_t* ev, void* raw_ctx) -> int {
    auto* ctx = static_cast<RemoveCtx*>(raw_ctx);
    ctx->removed_ids.push_back(ev->op_info.op_ins_count);
    H5ES__list_remove(ctx->list, ev);
    return H5_ITER_CONT;
  };

  // Forward removal clears head-to-tail safely.
  H5ES_event_list_t fwd_list{};
  H5ES_event_t f1 = MakeEvent(1);
  H5ES_event_t f2 = MakeEvent(2);
  H5ES_event_t f3 = MakeEvent(3);
  H5ES__list_append(&fwd_list, &f1);
  H5ES__list_append(&fwd_list, &f2);
  H5ES__list_append(&fwd_list, &f3);

  RemoveCtx fwd_ctx{&fwd_list, {}};
  EXPECT_EQ(H5ES__list_iterate(&fwd_list, H5_ITER_INC, remove_cb, &fwd_ctx),
            H5_ITER_CONT);
  EXPECT_THAT(fwd_ctx.removed_ids, ElementsAre(1, 2, 3));
  EXPECT_EQ(H5ES__list_count(&fwd_list), 0u);
  EXPECT_EQ(fwd_list.head, nullptr);
  EXPECT_EQ(fwd_list.tail, nullptr);

  // Reverse removal also caches `ev->prev` before calling callback.
  H5ES_event_list_t rev_list{};
  H5ES_event_t r1 = MakeEvent(4);
  H5ES_event_t r2 = MakeEvent(5);
  H5ES_event_t r3 = MakeEvent(6);
  H5ES__list_append(&rev_list, &r1);
  H5ES__list_append(&rev_list, &r2);
  H5ES__list_append(&rev_list, &r3);

  RemoveCtx rev_ctx{&rev_list, {}};
  EXPECT_EQ(H5ES__list_iterate(&rev_list, H5_ITER_DEC, remove_cb, &rev_ctx),
            H5_ITER_CONT);
  EXPECT_THAT(rev_ctx.removed_ids, ElementsAre(6, 5, 4));
  EXPECT_EQ(H5ES__list_count(&rev_list), 0u);
  EXPECT_EQ(rev_list.head, nullptr);
  EXPECT_EQ(rev_list.tail, nullptr);
}

}  // namespace
}  // namespace hdf5
