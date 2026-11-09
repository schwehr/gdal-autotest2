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
// Tests public HDF5 Event Set APIs (third_party/hdf5/src/H5ES.c) using a
// lightweight in-memory asynchronous mock VOL connector registered cleanly per
// test.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5ES.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

constexpr H5VL_class_value_t kMockVolValue = 302;
constexpr char kMockVolName[] = "h5es_public_mock_vol";

struct MockVolController {
  std::deque<H5VL_request_status_t> create_wait_statuses;
  bool fail_create_get_err_stack = false;
  uint64_t exec_ts = 1000;
  uint64_t exec_time = 250;
};

struct MockRequest {
  int id = 0;
  H5VL_request_status_t wait_status = H5VL_REQUEST_STATUS_SUCCEED;
  H5VL_request_status_t cancel_status = H5VL_REQUEST_STATUS_CANCELED;
  bool fail_wait = false;
  bool fail_cancel = false;
  bool fail_get_err_stack = false;
  bool fail_get_exec_time = false;
  uint64_t exec_ts = 100;
  uint64_t exec_time = 50;
  int wait_calls = 0;
  int cancel_calls = 0;
  int err_stack_calls = 0;
};

struct MockFile {
  MockVolController* controller = nullptr;
};

MockVolController* g_active_controller = nullptr;

void* MockFileCreate(const char* /*name*/, unsigned /*flags*/,
                     hid_t /*fcpl_id*/, hid_t /*fapl_id*/, hid_t /*dxpl_id*/,
                     void** req) {
  auto* file = new MockFile{g_active_controller};
  if (req != nullptr && g_active_controller != nullptr) {
    auto* token = new MockRequest{};
    if (!g_active_controller->create_wait_statuses.empty()) {
      token->wait_status = g_active_controller->create_wait_statuses.front();
      g_active_controller->create_wait_statuses.pop_front();
    }
    token->fail_get_err_stack = g_active_controller->fail_create_get_err_stack;
    token->exec_ts = g_active_controller->exec_ts;
    token->exec_time = g_active_controller->exec_time;
    *req = token;
  }
  return file;
}

herr_t MockFileClose(void* file, hid_t /*dxpl_id*/, void** req) {
  if (req != nullptr) {
    *req = nullptr;
  }
  delete static_cast<MockFile*>(file);
  return 0;
}

herr_t MockIntrospectOptQuery(void* /*obj*/, H5VL_subclass_t /*subcls*/,
                              int /*opt_type*/, uint64_t* flags) {
  if (flags != nullptr) {
    *flags = 0;
  }
  return 0;
}

herr_t MockRequestWait(void* req, uint64_t /*timeout*/,
                       H5VL_request_status_t* status) {
  auto* r = static_cast<MockRequest*>(req);
  ++r->wait_calls;
  if (r->fail_wait) {
    return -1;
  }
  *status = r->wait_status;
  return 0;
}

herr_t MockRequestCancel(void* req, H5VL_request_status_t* status) {
  auto* r = static_cast<MockRequest*>(req);
  ++r->cancel_calls;
  if (r->fail_cancel) {
    return -1;
  }
  *status = r->cancel_status;
  return 0;
}

herr_t MockRequestSpecific(void* req, H5VL_request_specific_args_t* args) {
  auto* r = static_cast<MockRequest*>(req);
  switch (args->op_type) {
    case H5VL_REQUEST_GET_EXEC_TIME:
      if (r->fail_get_exec_time) {
        return -1;
      }
      *args->args.get_exec_time.exec_ts = r->exec_ts;
      *args->args.get_exec_time.exec_time = r->exec_time;
      return 0;
    case H5VL_REQUEST_GET_ERR_STACK: {
      ++r->err_stack_calls;
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

herr_t MockRequestFree(void* req) {
  delete static_cast<MockRequest*>(req);
  return 0;
}

class ScopedMockVol {
 public:
  explicit ScopedMockVol(MockVolController* controller = nullptr) {
    g_active_controller = controller;
    H5VL_class_t cls{};
    cls.version = H5VL_VERSION;
    cls.value = kMockVolValue;
    cls.name = kMockVolName;
    cls.file_cls.create = MockFileCreate;
    cls.file_cls.close = MockFileClose;
    cls.introspect_cls.opt_query = MockIntrospectOptQuery;
    cls.request_cls.wait = MockRequestWait;
    cls.request_cls.cancel = MockRequestCancel;
    cls.request_cls.specific = MockRequestSpecific;
    cls.request_cls.free = MockRequestFree;
    vol_id_ = H5VLregister_connector(&cls, H5P_DEFAULT);
  }

  ~ScopedMockVol() {
    if (vol_id_ >= 0) {
      H5VLunregister_connector(vol_id_);
    }
    g_active_controller = nullptr;
  }

  ScopedMockVol(const ScopedMockVol&) = delete;
  ScopedMockVol& operator=(const ScopedMockVol&) = delete;

  hid_t id() const { return vol_id_; }

 private:
  hid_t vol_id_ = H5I_INVALID_HID;
};

char* AllocH5String(const char* src) {
  const size_t len = std::strlen(src) + 1;
  auto* dst = static_cast<char*>(H5allocate_memory(len, /*clear=*/false));
  if (dst != nullptr) {
    std::memcpy(dst, src, len);
  }
  return dst;
}

TEST(H5ESTest, CreateCloseAndH5esNoneNoOps) {
  ScopedEventSet es(H5EScreate());
  ASSERT_NE(es.get(), H5I_INVALID_HID);
  EXPECT_EQ(H5Iget_type(es.get()), H5I_EVENTSET);

  // H5ES_NONE is explicitly accepted as a no-op across H5ES APIs.
  constexpr size_t kSentinelSize = 999;
  constexpr uint64_t kSentinelCounter = 888;
  size_t count = kSentinelSize;
  uint64_t op_counter = kSentinelCounter;
  size_t in_progress = kSentinelSize;
  bool err_occurred = true;
  bool err_status = true;
  size_t err_count = kSentinelSize;
  size_t err_cleared = kSentinelSize;
  size_t not_canceled = kSentinelSize;
  H5ES_err_info_t err_info{};

  EXPECT_GE(H5ESget_count(H5ES_NONE, &count), 0);
  EXPECT_EQ(count, kSentinelSize);

  EXPECT_GE(H5ESget_op_counter(H5ES_NONE, &op_counter), 0);
  EXPECT_EQ(op_counter, kSentinelCounter);

  EXPECT_GE(H5ESwait(H5ES_NONE, H5ES_WAIT_NONE, &in_progress, &err_occurred), 0);
  EXPECT_EQ(in_progress, kSentinelSize);
  EXPECT_TRUE(err_occurred);

  EXPECT_GE(H5EScancel(H5ES_NONE, &not_canceled, &err_occurred), 0);
  EXPECT_EQ(not_canceled, kSentinelSize);

  EXPECT_GE(H5ESget_err_status(H5ES_NONE, &err_status), 0);
  EXPECT_TRUE(err_status);

  EXPECT_GE(H5ESget_err_count(H5ES_NONE, &err_count), 0);
  EXPECT_EQ(err_count, kSentinelSize);

  EXPECT_GE(H5ESget_err_info(H5ES_NONE, 1, &err_info, &err_cleared), 0);
  EXPECT_EQ(err_cleared, kSentinelSize);

  EXPECT_GE(H5ESregister_insert_func(H5ES_NONE, nullptr, nullptr), 0);
  EXPECT_GE(H5ESregister_complete_func(H5ES_NONE, nullptr, nullptr), 0);
  EXPECT_GE(H5ESclose(H5ES_NONE), 0);

  // NULL optional out-pointers on a valid event set also succeed.
  EXPECT_GE(H5ESget_count(es.get(), nullptr), 0);
  EXPECT_GE(H5ESget_op_counter(es.get(), nullptr), 0);
  EXPECT_GE(H5ESget_err_status(es.get(), nullptr), 0);
  EXPECT_GE(H5ESget_err_count(es.get(), nullptr), 0);

  EXPECT_GE(H5ESclose(es.release()), 0);
}

TEST(H5ESTest, InvalidArgumentsFailCleanly) {
  ScopedMockVol vol;
  ASSERT_NE(vol.id(), H5I_INVALID_HID);
  ScopedEventSet es(H5EScreate());
  ASSERT_NE(es.get(), H5I_INVALID_HID);
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_NE(fapl.get(), H5I_INVALID_HID);

  int stack_token = 1;
  size_t sz = 0;
  uint64_t u64 = 0;
  bool flag = false;
  H5ES_err_info_t err_info{};

  ScopedErrorSilence silence;

  // Invalid es_id (H5I_INVALID_HID and non-event-set ID, plus H5ES_NONE where
  // disallowed).
  EXPECT_LT(H5ESinsert_request(H5I_INVALID_HID, vol.id(), &stack_token), 0);
  EXPECT_LT(H5ESinsert_request(fapl.get(), vol.id(), &stack_token), 0);
  EXPECT_LT(H5ESinsert_request(H5ES_NONE, vol.id(), &stack_token), 0);
  EXPECT_LT(H5ESinsert_request(es.get(), vol.id(), nullptr), 0);
  EXPECT_LT(H5ESinsert_request(es.get(), H5I_INVALID_HID, &stack_token), 0);
  EXPECT_LT(H5ESinsert_request(es.get(), es.get(), &stack_token), 0);

  for (hid_t bad_id : {static_cast<hid_t>(H5I_INVALID_HID), fapl.get()}) {
    EXPECT_LT(H5ESget_count(bad_id, &sz), 0);
    EXPECT_LT(H5ESget_op_counter(bad_id, &u64), 0);
    EXPECT_LT(H5ESwait(bad_id, 0, &sz, &flag), 0);
    EXPECT_LT(H5EScancel(bad_id, &sz, &flag), 0);
    EXPECT_LT(H5ESget_err_status(bad_id, &flag), 0);
    EXPECT_LT(H5ESget_err_count(bad_id, &sz), 0);
    EXPECT_LT(H5ESget_err_info(bad_id, 1, &err_info, &sz), 0);
    EXPECT_LT(H5ESregister_insert_func(
                  bad_id, +[](const H5ES_op_info_t*, void*) { return 0; },
                  nullptr),
              0);
    EXPECT_LT(
        H5ESregister_complete_func(
            bad_id,
            +[](const H5ES_op_info_t*, H5ES_status_t, hid_t, void*) { return 0; },
            nullptr),
        0);
    EXPECT_LT(H5ESclose(bad_id), 0);
  }

  // H5ESget_requests validation.
  EXPECT_LT(
      H5ESget_requests(H5I_INVALID_HID, H5_ITER_INC, nullptr, nullptr, 0, &sz),
      0);
  EXPECT_LT(H5ESget_requests(es.get(), H5_ITER_UNKNOWN, nullptr, nullptr, 0, &sz),
            0);
  EXPECT_LT(H5ESget_requests(es.get(), H5_ITER_N, nullptr, nullptr, 0, &sz), 0);

  // NULL / zero-length pointers.
  EXPECT_LT(H5ESwait(es.get(), 0, nullptr, &flag), 0);
  EXPECT_LT(H5ESwait(es.get(), 0, &sz, nullptr), 0);
  EXPECT_LT(H5EScancel(es.get(), nullptr, &flag), 0);
  EXPECT_LT(H5EScancel(es.get(), &sz, nullptr), 0);
  EXPECT_LT(H5ESget_err_info(es.get(), 0, &err_info, &sz), 0);
  EXPECT_LT(H5ESget_err_info(es.get(), 1, nullptr, &sz), 0);
  EXPECT_LT(H5ESget_err_info(es.get(), 1, &err_info, nullptr), 0);
  EXPECT_LT(H5ESfree_err_info(0, &err_info), 0);
  EXPECT_LT(H5ESfree_err_info(1, nullptr), 0);
  EXPECT_LT(H5ESregister_insert_func(es.get(), nullptr, nullptr), 0);
  EXPECT_LT(H5ESregister_complete_func(es.get(), nullptr, nullptr), 0);
}

void DrainEventSet(hid_t es_id) {
  if (es_id == H5I_INVALID_HID || es_id == H5ES_NONE) {
    return;
  }
  size_t count = 0;
  if (H5ESget_count(es_id, &count) < 0 || count == 0) {
    return;
  }
  std::vector<void*> active(count, nullptr);
  size_t got = 0;
  if (H5ESget_requests(es_id, H5_ITER_INC, nullptr, active.data(),
                       active.size(), &got) >= 0) {
    for (size_t i = 0; i < got; ++i) {
      if (active[i] != nullptr) {
        auto* r = static_cast<MockRequest*>(active[i]);
        r->fail_wait = false;
        r->fail_cancel = false;
        r->fail_get_err_stack = false;
        r->fail_get_exec_time = false;
        r->wait_status = H5VL_REQUEST_STATUS_SUCCEED;
      }
    }
  }
  size_t prog = 0;
  bool failed = false;
  H5ESwait(es_id, H5ES_WAIT_NONE, &prog, &failed);
}

TEST(H5ESTest, InsertGetRequestsWaitCloseActiveFailureAndCallbacks) {
  ScopedMockVol vol;
  ASSERT_NE(vol.id(), H5I_INVALID_HID);

  ScopedEventSet es(H5EScreate());
  ASSERT_NE(es.get(), H5I_INVALID_HID);

  absl::Cleanup drain_es = [&] {
    if (es.valid()) {
      DrainEventSet(es.get());
    }
  };

  struct InsertCtx {
    int count = 0;
    int return_val = 0;
    std::vector<uint64_t> op_counts;
  } ins_ctx;

  struct CompleteCtx {
    int count = 0;
    std::vector<H5ES_status_t> statuses;
  } comp_ctx;

  auto insert_cb = +[](const H5ES_op_info_t* info, void* ctx) -> int {
    auto* st = static_cast<InsertCtx*>(ctx);
    ++st->count;
    st->op_counts.push_back(info->op_ins_count);
    return st->return_val;
  };
  auto complete_cb = +[](const H5ES_op_info_t* /*info*/, H5ES_status_t status,
                         hid_t /*err_stack*/, void* ctx) -> int {
    auto* st = static_cast<CompleteCtx*>(ctx);
    ++st->count;
    st->statuses.push_back(status);
    return 0;
  };

  ASSERT_GE(H5ESregister_insert_func(es.get(), insert_cb, &ins_ctx), 0);
  ASSERT_GE(H5ESregister_complete_func(es.get(), complete_cb, &comp_ctx), 0);

  // 1. Failing insert callback causes H5ESinsert_request to return < 0 cleanly.
  ins_ctx.return_val = -1;
  {
    ScopedErrorSilence silence;
    auto* failing_tok = new MockRequest{99};
    EXPECT_LT(H5ESinsert_request(es.get(), vol.id(), failing_tok), 0);
  }
  EXPECT_EQ(ins_ctx.count, 1);

  size_t count = 99;
  ASSERT_GE(H5ESget_count(es.get(), &count), 0);
  EXPECT_EQ(count, 0u);

  // 2. Replace callbacks and insert two requests (one in-progress, one
  // succeeding).
  InsertCtx ins_ctx2;
  ASSERT_GE(H5ESregister_insert_func(es.get(), insert_cb, &ins_ctx2), 0);

  auto* r1 = new MockRequest{101, H5VL_REQUEST_STATUS_IN_PROGRESS};
  auto* r2 = new MockRequest{102, H5VL_REQUEST_STATUS_SUCCEED};

  ASSERT_GE(H5ESinsert_request(es.get(), vol.id(), r1), 0);
  ASSERT_GE(H5ESinsert_request(es.get(), vol.id(), r2), 0);
  EXPECT_EQ(ins_ctx2.count, 2);
  EXPECT_THAT(ins_ctx2.op_counts, ElementsAre(1u, 2u));

  uint64_t op_counter = 0;
  ASSERT_GE(H5ESget_op_counter(es.get(), &op_counter), 0);
  EXPECT_EQ(op_counter, 3u);

  ASSERT_GE(H5ESget_count(es.get(), &count), 0);
  EXPECT_EQ(count, 2u);

  // 3. Query requests in INC and DEC order, plus zero array_len / null array
  // branches.
  EXPECT_GE(H5ESget_requests(es.get(), H5_ITER_INC, nullptr, nullptr, 0, &count),
            0);
  EXPECT_EQ(count, 2u);
  EXPECT_GE(H5ESget_requests(es.get(), H5_ITER_INC, nullptr, nullptr, 2, nullptr),
            0);

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
  ASSERT_GE(H5ESget_requests(es.get(), H5_ITER_INC, conn_ids.data(),
                             reqs.data(), conn_ids.size(), &count),
            0);
  EXPECT_EQ(count, 2u);
  EXPECT_EQ(reqs[0], r1);
  EXPECT_EQ(reqs[1], r2);
  EXPECT_NE(conn_ids[0], H5I_INVALID_HID);
  EXPECT_NE(conn_ids[1], H5I_INVALID_HID);
  H5VLclose(conn_ids[0]);
  H5VLclose(conn_ids[1]);
  conn_ids = {H5I_INVALID_HID, H5I_INVALID_HID};

  ASSERT_GE(H5ESget_requests(es.get(), H5_ITER_DEC, conn_ids.data(),
                             reqs.data(), reqs.size(), nullptr),
            0);
  EXPECT_EQ(reqs[0], r2);
  EXPECT_EQ(reqs[1], r1);
  H5VLclose(conn_ids[0]);
  H5VLclose(conn_ids[1]);
  conn_ids = {H5I_INVALID_HID, H5I_INVALID_HID};

  // 4. First wait completes r2 and leaves r1 active.
  size_t in_progress = 0;
  bool err_occurred = true;
  ASSERT_GE(H5ESwait(es.get(), H5ES_WAIT_NONE, &in_progress, &err_occurred), 0);
  EXPECT_EQ(in_progress, 1u);
  EXPECT_FALSE(err_occurred);
  EXPECT_EQ(comp_ctx.count, 1);
  EXPECT_THAT(comp_ctx.statuses, ElementsAre(H5ES_STATUS_SUCCEED));

  // 5. Closing while r1 is active fails (H5ES__close_cb error branch).
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5ESclose(es.get()), 0);
  }

  // 6. Trigger a wait error on r1, then let r1 succeed and close cleanly.
  {
    ScopedErrorSilence silence;
    r1->fail_wait = true;
    EXPECT_LT(H5ESwait(es.get(), H5ES_WAIT_NONE, &in_progress, &err_occurred),
              0);
  }
  r1->fail_wait = false;
  r1->wait_status = H5VL_REQUEST_STATUS_SUCCEED;
  ASSERT_GE(H5ESwait(es.get(), H5ES_WAIT_FOREVER, &in_progress, &err_occurred),
            0);
  EXPECT_EQ(in_progress, 0u);
  EXPECT_FALSE(err_occurred);
  EXPECT_EQ(comp_ctx.count, 2);
  EXPECT_GE(H5ESclose(es.release()), 0);
}

TEST(H5ESTest, CancelOperationsAndErrorPropagation) {
  ScopedMockVol vol;
  ASSERT_NE(vol.id(), H5I_INVALID_HID);

  ScopedEventSet es(H5EScreate());
  ASSERT_NE(es.get(), H5I_INVALID_HID);

  absl::Cleanup drain_es = [&] {
    if (es.valid()) {
      DrainEventSet(es.get());
    }
  };

  auto* r_cant_cancel = new MockRequest{1, H5VL_REQUEST_STATUS_SUCCEED,
                                        H5VL_REQUEST_STATUS_CANT_CANCEL};
  auto* r_canceled =
      new MockRequest{2, H5VL_REQUEST_STATUS_SUCCEED, H5VL_REQUEST_STATUS_CANCELED};

  ASSERT_GE(H5ESinsert_request(es.get(), vol.id(), r_cant_cancel), 0);
  ASSERT_GE(H5ESinsert_request(es.get(), vol.id(), r_canceled), 0);

  size_t not_canceled = 0;
  bool err_occurred = true;
  ASSERT_GE(H5EScancel(es.get(), &not_canceled, &err_occurred), 0);
  EXPECT_EQ(not_canceled, 1u);
  EXPECT_FALSE(err_occurred);

  // Remaining op can fail to cancel, exercising H5EScancel's error path.
  {
    ScopedErrorSilence silence;
    r_cant_cancel->fail_cancel = true;
    EXPECT_LT(H5EScancel(es.get(), &not_canceled, &err_occurred), 0);
  }
  r_cant_cancel->fail_cancel = false;
  r_cant_cancel->cancel_status = H5VL_REQUEST_STATUS_CANCELED;
  ASSERT_GE(H5EScancel(es.get(), &not_canceled, &err_occurred), 0);
  EXPECT_EQ(not_canceled, 0u);
  EXPECT_FALSE(err_occurred);
}

TEST(H5ESTest, FailedAsyncOpPopulatesErrStatusCountInfoAndFreesCleanly) {
  MockVolController controller;
  controller.create_wait_statuses.push_back(H5VL_REQUEST_STATUS_FAIL);
  ScopedMockVol vol(&controller);
  ASSERT_NE(vol.id(), H5I_INVALID_HID);

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_NE(fapl.get(), H5I_INVALID_HID);
  ASSERT_GE(H5Pset_vol(fapl.get(), vol.id(), nullptr), 0);

  ScopedEventSet es(H5EScreate());
  ASSERT_NE(es.get(), H5I_INVALID_HID);

  // Before any failure, error status is false and count is 0.
  bool err_status = true;
  size_t err_count = 99;
  ASSERT_GE(H5ESget_err_status(es.get(), &err_status), 0);
  EXPECT_FALSE(err_status);
  ASSERT_GE(H5ESget_err_count(es.get(), &err_count), 0);
  EXPECT_EQ(err_count, 0u);

  // Public H5Fcreate_async inserts a token populated with caller metadata
  // (api_name, api_args, app_file_name, app_func_name, app_line_num).
  ScopedFile file(H5Fcreate_async("async_mock.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                                  fapl.get(), es.get()));
  ASSERT_NE(file.get(), H5I_INVALID_HID);
  file.reset();

  size_t in_progress = 0;
  bool op_failed = false;
  ASSERT_GE(H5ESwait(es.get(), H5ES_WAIT_FOREVER, &in_progress, &op_failed), 0);
  EXPECT_TRUE(op_failed);

  ASSERT_GE(H5ESget_err_status(es.get(), &err_status), 0);
  EXPECT_TRUE(err_status);
  ASSERT_GE(H5ESget_err_count(es.get(), &err_count), 0);
  EXPECT_EQ(err_count, 1u);

  H5ES_err_info_t err_info{};
  size_t num_cleared = 0;
  ASSERT_GE(H5ESget_err_info(es.get(), 1, &err_info, &num_cleared), 0);
  EXPECT_EQ(num_cleared, 1u);
  EXPECT_STREQ(err_info.api_name, "H5Fcreate_async");
  EXPECT_THAT(err_info.api_args, HasSubstr("filename=\"async_mock.h5\""));
  EXPECT_THAT(err_info.app_file_name, HasSubstr("H5ES_test.cc"));
  EXPECT_NE(err_info.app_func_name, nullptr);
  EXPECT_GT(err_info.app_line_num, 0u);
  EXPECT_NE(err_info.err_stack_id, H5I_INVALID_HID);

  ASSERT_GE(H5ESfree_err_info(1, &err_info), 0);

  ASSERT_GE(H5ESget_err_count(es.get(), &err_count), 0);
  EXPECT_EQ(err_count, 0u);

  // Failed request inserted via H5ESinsert_request lacks api_name, so
  // H5ESget_err_info returns < 0 cleanly while closing es drops the failed op.
  ScopedEventSet es_anon(H5EScreate());
  ASSERT_NE(es_anon.get(), H5I_INVALID_HID);
  auto* anon_fail = new MockRequest{55, H5VL_REQUEST_STATUS_FAIL};
  ASSERT_GE(H5ESinsert_request(es_anon.get(), vol.id(), anon_fail), 0);
  ASSERT_GE(H5ESwait(es_anon.get(), H5ES_WAIT_NONE, &in_progress, &op_failed),
            0);
  EXPECT_TRUE(op_failed);

  {
    ScopedErrorSilence silence;
    H5ES_err_info_t bad_info{};
    EXPECT_LT(H5ESget_err_info(es_anon.get(), 1, &bad_info, &num_cleared), 0);
  }
}

TEST(H5ESTest, FreeErrInfoHandlesValidStructsAndInvalidErrStack) {
  std::array<H5ES_err_info_t, 2> infos{};
  infos[0].api_name = AllocH5String("Op0");
  infos[0].api_args = AllocH5String("a=0");
  infos[0].app_file_name = AllocH5String("file.cc");
  infos[0].app_func_name = AllocH5String("Func0");
  infos[0].err_stack_id = H5Ecreate_stack();
  ASSERT_NE(infos[0].err_stack_id, H5I_INVALID_HID);

  // NULL strings + valid error stack.
  infos[1].err_stack_id = H5Ecreate_stack();
  ASSERT_NE(infos[1].err_stack_id, H5I_INVALID_HID);

  EXPECT_GE(H5ESfree_err_info(infos.size(), infos.data()), 0);

  // Invalid err_stack_id returns < 0 after releasing strings.
  H5ES_err_info_t invalid_info{};
  invalid_info.api_name = AllocH5String("OpBad");
  invalid_info.err_stack_id = H5I_INVALID_HID;
  ScopedErrorSilence silence;
  EXPECT_LT(H5ESfree_err_info(1, &invalid_info), 0);
}

}  // namespace
}  // namespace hdf5
