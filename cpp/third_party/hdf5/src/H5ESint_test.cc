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
// Tests internal event set implementation functions in
// third_party/hdf5/src/H5ESint.c (H5ES_insert, H5ES__*, and
// H5ES_term_package).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5ESint.c

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` compiles with -DNDEBUG; match internal struct layouts across
// H5ESpkg.h, H5CXprivate.h, and H5VLpkg.h.
#ifndef NDEBUG
#define H5ESINT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5ES_FRIEND
#define H5VL_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5ESpkg.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5VLpkg.h"
}  // extern "C"
#ifdef H5ESINT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5ESINT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

constexpr H5VL_class_value_t kMockVolValue = 303;
constexpr char kMockVolName[] = "h5es_int_mock_vol";

struct IntMockRequest {
  int id = 0;
  H5VL_request_status_t wait_status = H5VL_REQUEST_STATUS_SUCCEED;
  H5VL_request_status_t cancel_status = H5VL_REQUEST_STATUS_CANCELED;
  bool fail_wait = false;
  bool fail_cancel = false;
  bool fail_get_exec_time = false;
  bool fail_get_err_stack = false;
  int sleep_us_on_wait = 0;
  uint64_t exec_ts = 555;
  uint64_t exec_time = 77;
  std::vector<uint64_t>* observed_wait_timeouts = nullptr;
};

herr_t IntMockWait(void* req, uint64_t timeout,
                   H5VL_request_status_t* status) {
  auto* r = static_cast<IntMockRequest*>(req);
  if (r->observed_wait_timeouts != nullptr) {
    r->observed_wait_timeouts->push_back(timeout);
  }
  if (r->sleep_us_on_wait > 0) {
    std::this_thread::sleep_for(std::chrono::microseconds(r->sleep_us_on_wait));
  }
  if (r->fail_wait) {
    return -1;
  }
  *status = r->wait_status;
  return 0;
}

herr_t IntMockCancel(void* req, H5VL_request_status_t* status) {
  auto* r = static_cast<IntMockRequest*>(req);
  if (r->fail_cancel) {
    return -1;
  }
  *status = r->cancel_status;
  return 0;
}

herr_t IntMockSpecific(void* req, H5VL_request_specific_args_t* args) {
  auto* r = static_cast<IntMockRequest*>(req);
  switch (args->op_type) {
    case H5VL_REQUEST_GET_EXEC_TIME:
      if (r->fail_get_exec_time) {
        return -1;
      }
      *args->args.get_exec_time.exec_ts = r->exec_ts;
      *args->args.get_exec_time.exec_time = r->exec_time;
      return 0;
    case H5VL_REQUEST_GET_ERR_STACK: {
      if (r->fail_get_err_stack) {
        return -1;
      }
      const hid_t stack_id = H5Ecreate_stack();
      if (stack_id < 0) {
        return -1;
      }
      args->args.get_err_stack.err_stack_id = stack_id;
      return 0;
    }
    default:
      return -1;
  }
}

herr_t IntMockFree(void* req) {
  delete static_cast<IntMockRequest*>(req);
  return 0;
}

class ScopedApiContext {
 public:
  ScopedApiContext() { pushed_ = (H5CX_push(&node_) >= 0); }
  ~ScopedApiContext() {
    if (pushed_) {
      H5CX_pop(false);
    }
  }
  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool pushed() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

class ScopedMockVol {
 public:
  void Register() {
    H5VL_class_t cls{};
    cls.version = H5VL_VERSION;
    cls.value = kMockVolValue;
    cls.name = kMockVolName;
    cls.request_cls.wait = IntMockWait;
    cls.request_cls.cancel = IntMockCancel;
    cls.request_cls.specific = IntMockSpecific;
    cls.request_cls.free = IntMockFree;
    vol_id_ = H5VLregister_connector(&cls, H5P_DEFAULT);
    if (vol_id_ >= 0) {
      connector_ =
          static_cast<H5VL_connector_t*>(H5I_object_verify(vol_id_, H5I_VOL));
    }
  }

  ~ScopedMockVol() {
    if (vol_id_ >= 0) {
      H5VLunregister_connector(vol_id_);
    }
  }

  ScopedMockVol() = default;
  ScopedMockVol(const ScopedMockVol&) = delete;
  ScopedMockVol& operator=(const ScopedMockVol&) = delete;

  hid_t id() const { return vol_id_; }
  H5VL_connector_t* connector() const { return connector_; }

 private:
  hid_t vol_id_ = H5I_INVALID_HID;
  H5VL_connector_t* connector_ = nullptr;
};

void FreePartialErrInfoStrings(H5ES_err_info_t* info) {
  H5free_memory(info->api_name);
  H5free_memory(info->api_args);
  H5free_memory(info->app_file_name);
  H5free_memory(info->app_func_name);
  info->api_name = nullptr;
  info->api_args = nullptr;
  info->app_file_name = nullptr;
  info->app_func_name = nullptr;
}

void DrainInternalEventSet(H5ES_t* es) {
  if (es == nullptr) {
    return;
  }
  es->ins_func = nullptr;
  es->comp_func = nullptr;
  for (H5ES_event_t* ev = es->active.head; ev != nullptr; ev = ev->next) {
    if (ev->request != nullptr) {
      auto* r = static_cast<IntMockRequest*>(H5VL_object_data(ev->request));
      if (r != nullptr) {
        r->fail_wait = false;
        r->fail_cancel = false;
        r->fail_get_exec_time = false;
        r->fail_get_err_stack = false;
        r->sleep_us_on_wait = 0;
        r->wait_status = H5VL_REQUEST_STATUS_SUCCEED;
      }
    }
  }
  if (es->active.count > 0) {
    size_t prog = 0;
    bool failed = false;
    H5ES__wait(es, H5ES_WAIT_NONE, &prog, &failed);
  }
}

TEST(H5ESintTest, InsertTracesArgsHandlesErrorsAndInvokesCallback) {
  ScopedMockVol vol;
  vol.Register();
  ASSERT_NE(vol.id(), H5I_INVALID_HID);
  ASSERT_NE(vol.connector(), nullptr);

  ScopedEventSet es_id(H5EScreate());
  ASSERT_NE(es_id.get(), H5I_INVALID_HID);
  auto* es = static_cast<H5ES_t*>(H5I_object_verify(es_id.get(), H5I_EVENTSET));
  ASSERT_NE(es, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());

  absl::Cleanup drain_es = [&] { DrainInternalEventSet(es); };

  struct InsState {
    int calls = 0;
    int rc = 0;
    H5ES_op_info_t last_info{};
  } ins_state;

  es->ins_func = +[](const H5ES_op_info_t* info, void* ctx_ptr) -> int {
    auto* st = static_cast<InsState*>(ctx_ptr);
    ++st->calls;
    st->last_info = *info;
    return st->rc;
  };
  es->ins_ctx = &ins_state;

  // 1. Valid H5ES_insert with H5ARG_TRACE4 populates caller & formatted args.
  auto* r1 = new IntMockRequest{1};
  ASSERT_GE(H5ES_insert(es_id.get(), vol.connector(), r1,
                        H5ARG_TRACE4("MockApi", "*s*sIui", "src.cc", "CallerFn",
                                     42u, 777)),
            0);
  EXPECT_EQ(ins_state.calls, 1);
  EXPECT_EQ(es->op_counter, 1u);
  EXPECT_EQ(es->active.count, 1u);
  ASSERT_NE(es->active.head, nullptr);
  EXPECT_STREQ(es->active.head->op_info.api_name, "MockApi");
  EXPECT_STREQ(es->active.head->op_info.app_file_name, "src.cc");
  EXPECT_STREQ(es->active.head->op_info.app_func_name, "CallerFn");
  EXPECT_EQ(es->active.head->op_info.app_line_num, 42u);
  EXPECT_THAT(es->active.head->op_info.api_args, HasSubstr("777"));

  // 2. Failing insert callback removes & frees the newly inserted event.
  ins_state.rc = -1;
  {
    ScopedErrorSilence silence;
    auto* r_ins_fail = new IntMockRequest{2};
    EXPECT_LT(
        H5ES_insert(es_id.get(), vol.connector(), r_ins_fail,
                    H5ARG_TRACE4("FailCb", "*s*sIui", "src.cc", "Fn", 1u, 9)),
        0);
    auto* r_anon_fail = new IntMockRequest{3};
    EXPECT_LT(H5ES__insert_request(es, vol.connector(), r_anon_fail), 0);
  }
  EXPECT_EQ(es->active.count, 1u);
  ins_state.rc = 0;

  // 3. Invalid es_id and unknown arg type ('?'). Note: passing zero args
  //    beyond "*s*sIu" leaves H5RS_create(NULL) with rs->s == nullptr, which
  //    crashes upstream H5RS_decr() inside H5FL_blk_free().
  {
    ScopedErrorSilence silence;
    int stack_tok = 1;
    EXPECT_LT(
        H5ES_insert(H5I_INVALID_HID, vol.connector(), &stack_tok,
                    H5ARG_TRACE4("BadEs", "*s*sIui", "src.cc", "Fn", 1u, 0)),
        0);
    EXPECT_LT(
        H5ES_insert(es_id.get(), vol.connector(), &stack_tok,
                    H5ARG_TRACE4("BadFmt", "*s*sIu?", "src.cc", "Fn", 1u, 0)),
        0);
  }

  // 4. Error-occurred flag blocks H5ES_insert.
  es->err_occurred = true;
  {
    ScopedErrorSilence silence;
    int stack_tok = 2;
    EXPECT_LT(H5ES_insert(es_id.get(), vol.connector(), &stack_tok,
                          H5ARG_TRACE4("Blocked", "*s*sIui", "src.cc", "Fn",
                                       1u, 5)),
              0);
  }
  es->err_occurred = false;
}

TEST(H5ESintTest, GetRequestsSupportsPartialAndSubsetOutputs) {
  ScopedMockVol vol;
  vol.Register();
  ASSERT_NE(vol.id(), H5I_INVALID_HID);
  ASSERT_NE(vol.connector(), nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());

  H5ES_t* es = H5ES__create();
  ASSERT_NE(es, nullptr);
  ScopedEventSet es_id(H5I_register(H5I_EVENTSET, es, true));
  ASSERT_NE(es_id.get(), H5I_INVALID_HID);
  absl::Cleanup drain_es = [&] { DrainInternalEventSet(es); };

  auto* r1 = new IntMockRequest{1};
  auto* r2 = new IntMockRequest{2};
  auto* r3 = new IntMockRequest{3};
  ASSERT_GE(H5ES__insert_request(es, vol.connector(), r1), 0);
  ASSERT_GE(H5ES__insert_request(es, vol.connector(), r2), 0);
  ASSERT_GE(H5ES__insert_request(es, vol.connector(), r3), 0);

  // Truncated array_len triggers H5_ITER_STOP after 2 elements.
  std::array<hid_t, 2> conn_ids = {H5I_INVALID_HID, H5I_INVALID_HID};
  std::array<void*, 2> reqs = {nullptr, nullptr};
  absl::Cleanup close_conn_ids = [&] {
    for (hid_t& id : conn_ids) {
      if (id >= 0) {
        H5VLclose(id);
        id = H5I_INVALID_HID;
      }
    }
  };
  ASSERT_GE(H5ES__get_requests(es, H5_ITER_INC, conn_ids.data(), reqs.data(),
                               conn_ids.size()),
            0);
  EXPECT_EQ(reqs[0], r1);
  EXPECT_EQ(reqs[1], r2);
  EXPECT_NE(conn_ids[0], H5I_INVALID_HID);
  EXPECT_NE(conn_ids[1], H5I_INVALID_HID);
  H5VLclose(conn_ids[0]);
  H5VLclose(conn_ids[1]);
  conn_ids = {H5I_INVALID_HID, H5I_INVALID_HID};

  // connector_ids == nullptr branch.
  std::array<void*, 3> all_reqs = {nullptr, nullptr, nullptr};
  ASSERT_GE(H5ES__get_requests(es, H5_ITER_DEC, nullptr, all_reqs.data(),
                               all_reqs.size()),
            0);
  EXPECT_THAT(all_reqs, ElementsAre(r3, r2, r1));

  // requests == nullptr branch.
  std::array<hid_t, 1> single_conn = {H5I_INVALID_HID};
  ASSERT_GE(
      H5ES__get_requests(es, H5_ITER_NATIVE, single_conn.data(), nullptr, 1), 0);
  EXPECT_NE(single_conn[0], H5I_INVALID_HID);
  EXPECT_GE(H5VLclose(single_conn[0]), 0);
}

TEST(H5ESintTest, WaitTimeoutsStatusesAndCompletionErrors) {
  ScopedMockVol vol;
  vol.Register();
  ASSERT_NE(vol.id(), H5I_INVALID_HID);
  ASSERT_NE(vol.connector(), nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());

  H5ES_t* es = H5ES__create();
  ASSERT_NE(es, nullptr);
  ScopedEventSet es_id(H5I_register(H5I_EVENTSET, es, true));
  ASSERT_NE(es_id.get(), H5I_INVALID_HID);
  absl::Cleanup drain_es = [&] { DrainInternalEventSet(es); };

  struct CompState {
    int calls = 0;
    int rc = 0;
    std::vector<H5ES_status_t> statuses;
    uint64_t last_exec_ts = 0;
    uint64_t last_exec_time = 0;
    bool saw_valid_err_stack = false;
  } comp_state;

  es->comp_func = +[](const H5ES_op_info_t* info, H5ES_status_t status,
                      hid_t err_stack, void* ctx_ptr) -> int {
    auto* st = static_cast<CompState*>(ctx_ptr);
    ++st->calls;
    st->statuses.push_back(status);
    st->last_exec_ts = info->op_exec_ts;
    st->last_exec_time = info->op_exec_time;
    st->saw_valid_err_stack = (err_stack != H5I_INVALID_HID);
    return st->rc;
  };
  es->comp_ctx = &comp_state;

  // 1. Timeout exhaustion: first wait sleeps >10ns budget so second wait sees 0.
  std::vector<uint64_t> timeouts;
  auto* r1 = new IntMockRequest{1, H5VL_REQUEST_STATUS_IN_PROGRESS};
  r1->sleep_us_on_wait = 200;
  r1->observed_wait_timeouts = &timeouts;
  auto* r2 = new IntMockRequest{2, H5VL_REQUEST_STATUS_CANCELED};
  r2->observed_wait_timeouts = &timeouts;
  ASSERT_GE(H5ES__insert_request(es, vol.connector(), r1), 0);
  ASSERT_GE(H5ES__insert_request(es, vol.connector(), r2), 0);

  size_t in_progress = 99;
  bool op_failed = true;
  ASSERT_GE(H5ES__wait(es, /*timeout=*/10, &in_progress, &op_failed), 0);
  EXPECT_EQ(in_progress, 1u);
  EXPECT_FALSE(op_failed);
  EXPECT_THAT(timeouts, ElementsAre(10u, H5ES_WAIT_NONE));
  EXPECT_THAT(comp_state.statuses, ElementsAre(H5ES_STATUS_CANCELED));

  // 2. Large timeout subtracts elapsed time cleanly (`timeout -= elapsed*1000`),
  //    and tests SUCCEED + GET_EXEC_TIME error + comp_func error + CANT_CANCEL.
  r1->sleep_us_on_wait = 0;
  r1->observed_wait_timeouts = nullptr;
  r1->wait_status = H5VL_REQUEST_STATUS_SUCCEED;

  {
    ScopedErrorSilence silence;
    // GET_EXEC_TIME failure leaves r1 active.
    r1->fail_get_exec_time = true;
    EXPECT_LT(H5ES__wait(es, 1'000'000'000ULL, &in_progress, &op_failed), 0);
    r1->fail_get_exec_time = false;

    // comp_func returning < 0 on SUCCEED leaves r1 active.
    comp_state.rc = -1;
    EXPECT_LT(H5ES__wait(es, 1'000'000'000ULL, &in_progress, &op_failed), 0);

    // comp_func returning < 0 on CANCELED leaves r1 active.
    r1->wait_status = H5VL_REQUEST_STATUS_CANCELED;
    EXPECT_LT(H5ES__wait(es, H5ES_WAIT_NONE, &in_progress, &op_failed), 0);
    comp_state.rc = 0;

    // CANT_CANCEL status from wait is rejected as an error.
    r1->wait_status = H5VL_REQUEST_STATUS_CANT_CANCEL;
    EXPECT_LT(H5ES__wait(es, H5ES_WAIT_NONE, &in_progress, &op_failed), 0);

    // Raw wait callback failure.
    r1->fail_wait = true;
    EXPECT_LT(H5ES__wait(es, H5ES_WAIT_NONE, &in_progress, &op_failed), 0);
    r1->fail_wait = false;
  }

  // Successful wait populates op_exec_ts / op_exec_time via GET_EXEC_TIME.
  r1->wait_status = H5VL_REQUEST_STATUS_SUCCEED;
  ASSERT_GE(H5ES__wait(es, 1'000'000'000ULL, &in_progress, &op_failed), 0);
  EXPECT_EQ(in_progress, 0u);
  EXPECT_FALSE(op_failed);
  EXPECT_EQ(comp_state.last_exec_ts, 555u);
  EXPECT_EQ(comp_state.last_exec_time, 77u);

  // 3. Finite timeout subtraction across two active IN_PROGRESS requests,
  //    plus FAIL stopping iteration before touching subsequent requests.
  timeouts.clear();
  auto* r3 = new IntMockRequest{3, H5VL_REQUEST_STATUS_IN_PROGRESS};
  r3->observed_wait_timeouts = &timeouts;
  auto* r4 = new IntMockRequest{4, H5VL_REQUEST_STATUS_IN_PROGRESS};
  r4->observed_wait_timeouts = &timeouts;
  ASSERT_GE(H5ES__insert_request(es, vol.connector(), r3), 0);
  ASSERT_GE(H5ES__insert_request(es, vol.connector(), r4), 0);

  ASSERT_GE(H5ES__wait(es, 1'000'000'000ULL, &in_progress, &op_failed), 0);
  EXPECT_EQ(in_progress, 2u);
  EXPECT_FALSE(op_failed);
  ASSERT_EQ(timeouts.size(), 2u);
  EXPECT_EQ(timeouts[0], 1'000'000'000ULL);
  EXPECT_GT(timeouts[1], 0u);
  EXPECT_LE(timeouts[1], 1'000'000'000ULL);
  r3->observed_wait_timeouts = nullptr;
  r4->observed_wait_timeouts = nullptr;

  // Test FAIL error paths in H5ES__op_complete: GET_ERR_STACK failure,
  // comp_func failure, and normal FAIL (stops before r4!).
  r3->wait_status = H5VL_REQUEST_STATUS_FAIL;
  {
    ScopedErrorSilence silence;
    r3->fail_get_err_stack = true;
    EXPECT_LT(H5ES__wait(es, H5ES_WAIT_NONE, &in_progress, &op_failed), 0);
    r3->fail_get_err_stack = false;

    comp_state.rc = -1;
    EXPECT_LT(H5ES__wait(es, H5ES_WAIT_NONE, &in_progress, &op_failed), 0);
    comp_state.rc = 0;
  }

  comp_state.saw_valid_err_stack = false;
  ASSERT_GE(H5ES__wait(es, H5ES_WAIT_NONE, &in_progress, &op_failed), 0);
  EXPECT_TRUE(op_failed);
  EXPECT_TRUE(comp_state.saw_valid_err_stack);
  EXPECT_EQ(es->failed.count, 1u);
  EXPECT_EQ(es->active.count, 1u);

  // Drain remaining r4 before H5ES__close.
  r4->wait_status = H5VL_REQUEST_STATUS_SUCCEED;
  ASSERT_GE(H5ES__wait(es, H5ES_WAIT_NONE, &in_progress, &op_failed), 0);
  EXPECT_EQ(es->active.count, 0u);
}

TEST(H5ESintTest, CancelCoversAllRequestStatusesAndErrors) {
  ScopedMockVol vol;
  vol.Register();
  ASSERT_NE(vol.id(), H5I_INVALID_HID);
  ASSERT_NE(vol.connector(), nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());

  H5ES_t* es = H5ES__create();
  ASSERT_NE(es, nullptr);
  ScopedEventSet es_id(H5I_register(H5I_EVENTSET, es, true));
  ASSERT_NE(es_id.get(), H5I_INVALID_HID);
  absl::Cleanup drain_es = [&] { DrainInternalEventSet(es); };

  int comp_rc = 0;
  es->comp_func = +[](const H5ES_op_info_t*, H5ES_status_t, hid_t,
                      void* ctx_ptr) -> int { return *static_cast<int*>(ctx_ptr); };
  es->comp_ctx = &comp_rc;

  // Test error paths on single request first: cancel fail, comp_func fail on
  // SUCCEED / CANCELED / FAIL.
  auto* r_err = new IntMockRequest{1};
  ASSERT_GE(H5ES__insert_request(es, vol.connector(), r_err), 0);

  size_t not_canceled = 0;
  bool op_failed = false;
  {
    ScopedErrorSilence silence;
    r_err->fail_cancel = true;
    EXPECT_LT(H5ES__cancel(es, &not_canceled, &op_failed), 0);
    r_err->fail_cancel = false;

    comp_rc = -1;
    r_err->cancel_status = H5VL_REQUEST_STATUS_SUCCEED;
    EXPECT_LT(H5ES__cancel(es, &not_canceled, &op_failed), 0);

    r_err->cancel_status = H5VL_REQUEST_STATUS_CANCELED;
    EXPECT_LT(H5ES__cancel(es, &not_canceled, &op_failed), 0);

    r_err->cancel_status = H5VL_REQUEST_STATUS_FAIL;
    EXPECT_LT(H5ES__cancel(es, &not_canceled, &op_failed), 0);
    comp_rc = 0;
  }

  // Drain r_err via CANCELED.
  r_err->cancel_status = H5VL_REQUEST_STATUS_CANCELED;
  ASSERT_GE(H5ES__cancel(es, &not_canceled, &op_failed), 0);
  EXPECT_EQ(not_canceled, 0u);
  EXPECT_FALSE(op_failed);

  // Exercise all 5 statuses in one event set (FAIL stops iteration early before
  // reaching r_after_fail).
  auto* r_ok = new IntMockRequest{10, H5VL_REQUEST_STATUS_SUCCEED,
                                  H5VL_REQUEST_STATUS_SUCCEED};
  auto* r_cant = new IntMockRequest{11, H5VL_REQUEST_STATUS_SUCCEED,
                                    H5VL_REQUEST_STATUS_CANT_CANCEL};
  auto* r_prog = new IntMockRequest{12, H5VL_REQUEST_STATUS_SUCCEED,
                                    H5VL_REQUEST_STATUS_IN_PROGRESS};
  auto* r_canc = new IntMockRequest{13, H5VL_REQUEST_STATUS_SUCCEED,
                                    H5VL_REQUEST_STATUS_CANCELED};
  auto* r_fail = new IntMockRequest{14, H5VL_REQUEST_STATUS_FAIL,
                                    H5VL_REQUEST_STATUS_FAIL};
  auto* r_after_fail = new IntMockRequest{15, H5VL_REQUEST_STATUS_SUCCEED,
                                          H5VL_REQUEST_STATUS_CANCELED};
  for (IntMockRequest* r :
       {r_ok, r_cant, r_prog, r_canc, r_fail, r_after_fail}) {
    ASSERT_GE(H5ES__insert_request(es, vol.connector(), r), 0);
  }

  ASSERT_GE(H5ES__cancel(es, &not_canceled, &op_failed), 0);
  // r_ok, r_cant, r_prog all increment not_canceled; r_canc is removed without
  // incrementing; r_fail stops iteration and moves to failed.
  EXPECT_EQ(not_canceled, 3u);
  EXPECT_TRUE(op_failed);
  EXPECT_EQ(es->active.count, 3u);
  EXPECT_EQ(es->failed.count, 1u);
}

TEST(H5ESintTest, GetErrInfoPartialDrainErrorPathsAndCloseFailedEvents) {
  ScopedMockVol vol;
  vol.Register();
  ASSERT_NE(vol.id(), H5I_INVALID_HID);
  ASSERT_NE(vol.connector(), nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());

  ScopedEventSet es_id(H5EScreate());
  ASSERT_NE(es_id.get(), H5I_INVALID_HID);
  auto* es = static_cast<H5ES_t*>(H5I_object_verify(es_id.get(), H5I_EVENTSET));
  ASSERT_NE(es, nullptr);

  // 1. Fail 3 full-metadata ops via H5ES_insert; closing while active fails.
  for (int i = 0; i < 3; ++i) {
    auto* r = new IntMockRequest{i, H5VL_REQUEST_STATUS_FAIL};
    ASSERT_GE(H5ES_insert(es_id.get(), vol.connector(), r,
                          H5ARG_TRACE4("Op", "*s*sIui", "f.cc", "Fn", 10u, i)),
              0);
  }
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5ESclose(es_id.get()), 0);
  }

  size_t in_progress = 0;
  bool op_failed = false;
  for (int i = 0; i < 3; ++i) {
    ASSERT_GE(H5ES__wait(es, H5ES_WAIT_NONE, &in_progress, &op_failed), 0);
    EXPECT_TRUE(op_failed);
  }
  ASSERT_EQ(es->active.count, 0u);
  ASSERT_EQ(es->failed.count, 3u);

  // Partial drain (num_err_info = 1 -> H5_ITER_STOP).
  H5ES_err_info_t info1{};
  size_t cleared = 0;
  ASSERT_GE(H5ES__get_err_info(es, 1, &info1, &cleared), 0);
  EXPECT_EQ(cleared, 1u);
  EXPECT_STREQ(info1.api_name, "Op");
  EXPECT_STREQ(info1.app_file_name, "f.cc");
  EXPECT_STREQ(info1.app_func_name, "Fn");
  EXPECT_EQ(info1.app_line_num, 10u);
  ASSERT_GE(H5ESfree_err_info(1, &info1), 0);
  EXPECT_EQ(es->failed.count, 2u);

  // Drain remaining with oversized buffer (`array` longer than failed list).
  std::array<H5ES_err_info_t, 4> rest{};
  ASSERT_GE(H5ES__get_err_info(es, rest.size(), rest.data(), &cleared), 0);
  EXPECT_EQ(cleared, 2u);
  ASSERT_GE(H5ESfree_err_info(cleared, rest.data()), 0);
  EXPECT_EQ(es->failed.count, 0u);
  es->err_occurred = false;

  // 2. Cover each error branch inside H5ES__get_err_info_cb without leaks:
  //    a) GET_ERR_STACK failure after 4 H5MM_xstrdup copies.
  auto* r_stack_err = new IntMockRequest{20, H5VL_REQUEST_STATUS_FAIL};
  r_stack_err->fail_get_err_stack = true;
  ASSERT_GE(H5ES_insert(es_id.get(), vol.connector(), r_stack_err,
                        H5ARG_TRACE4("OpStackErr", "*s*sIui", "f.cc", "Fn",
                                     11u, 1)),
            0);
  ASSERT_GE(H5ES__wait(es, H5ES_WAIT_NONE, &in_progress, &op_failed), 0);
  {
    ScopedErrorSilence silence;
    H5ES_err_info_t err{};
    EXPECT_LT(H5ES__get_err_info(es, 1, &err, &cleared), 0);
    FreePartialErrInfoStrings(&err);
  }
  r_stack_err->fail_get_err_stack = false;

  //    b) api_args == NULL failure after api_name strdup.
  ASSERT_NE(es->failed.head, nullptr);
  H5MM_xfree(es->failed.head->op_info.api_args);
  es->failed.head->op_info.api_args = nullptr;
  {
    ScopedErrorSilence silence;
    H5ES_err_info_t err{};
    EXPECT_LT(H5ES__get_err_info(es, 1, &err, &cleared), 0);
    FreePartialErrInfoStrings(&err);
  }

  //    c) app_func_name == NULL and app_file_name == NULL and api_name == NULL.
  es->err_occurred = false;
  const char* null_file = nullptr;
  const char* null_func = nullptr;
  auto* r_no_func = new IntMockRequest{21, H5VL_REQUEST_STATUS_FAIL};
  ASSERT_GE(H5ES_insert(es_id.get(), vol.connector(), r_no_func,
                        H5ARG_TRACE4("OpNoFunc", "*s*sIui", "f.cc", null_func,
                                     12u, 2)),
            0);
  ASSERT_GE(H5ES__wait(es, H5ES_WAIT_NONE, &in_progress, &op_failed), 0);

  es->err_occurred = false;
  auto* r_no_file = new IntMockRequest{22, H5VL_REQUEST_STATUS_FAIL};
  ASSERT_GE(H5ES_insert(es_id.get(), vol.connector(), r_no_file,
                        H5ARG_TRACE4("OpNoFile", "*s*sIui", null_file, "Fn",
                                     13u, 3)),
            0);
  ASSERT_GE(H5ES__wait(es, H5ES_WAIT_NONE, &in_progress, &op_failed), 0);

  es->err_occurred = false;
  auto* r_no_name = new IntMockRequest{23, H5VL_REQUEST_STATUS_FAIL};
  ASSERT_GE(H5ES__insert_request(es, vol.connector(), r_no_name), 0);
  ASSERT_GE(H5ES__wait(es, H5ES_WAIT_NONE, &in_progress, &op_failed), 0);

  // Move each malformed failed op to head / test and inspect via close: first
  // drain r_stack_err (with valid api_args restored), then r_no_func, r_no_file,
  // and leave r_no_name on es->failed for H5ES__close_failed_cb.
  r_stack_err->fail_get_err_stack = false;
  es->failed.head->op_info.api_args = H5MM_xstrdup("restored");
  ASSERT_GE(H5ES__get_err_info(es, 1, &info1, &cleared), 0);
  ASSERT_GE(H5ESfree_err_info(1, &info1), 0);

  {
    ScopedErrorSilence silence;
    // Head is now r_no_func (app_func_name == NULL).
    H5ES_err_info_t err{};
    EXPECT_LT(H5ES__get_err_info(es, 1, &err, &cleared), 0);
    FreePartialErrInfoStrings(&err);
  }
  ASSERT_GE(H5ES__event_completed(es->failed.head, &es->failed), 0);

  {
    ScopedErrorSilence silence;
    // Head is now r_no_file (app_file_name == NULL).
    H5ES_err_info_t err{};
    EXPECT_LT(H5ES__get_err_info(es, 1, &err, &cleared), 0);
    FreePartialErrInfoStrings(&err);
  }
  ASSERT_GE(H5ES__event_completed(es->failed.head, &es->failed), 0);

  {
    ScopedErrorSilence silence;
    // Head is now r_no_name (api_name == NULL).
    H5ES_err_info_t err{};
    EXPECT_LT(H5ES__get_err_info(es, 1, &err, &cleared), 0);
    FreePartialErrInfoStrings(&err);
  }

  // Closing event set with r_no_name still in `failed` invokes
  // H5ES__close_failed_cb cleanly.
  EXPECT_GE(H5ESclose(es_id.release()), 0);
}

TEST(H5ESintTest, TermPackageShutsDownAndIgnoresRepeatCalls) {
  absl::Cleanup restore_lib = [] { H5open(); };
  ASSERT_GE(H5open(), 0);

  // Initialize H5ES package via H5EScreate/H5ESclose before closing library.
  const hid_t es_id = H5EScreate();
  ASSERT_NE(es_id, H5I_INVALID_HID);
  ASSERT_GE(H5ESclose(es_id), 0);

  // H5close() terminates H5ES via H5ES_term_package(), setting H5_PKG_INIT_VAR
  // back to false. Calling H5ES_term_package() afterwards exercises the
  // uninitialized fast path (`if (H5_PKG_INIT_VAR)` false -> returns 0).
  ASSERT_GE(H5close(), 0);
  EXPECT_EQ(H5ES_term_package(), 0);
  ASSERT_GE(H5open(), 0);
}

}  // namespace
}  // namespace hdf5
