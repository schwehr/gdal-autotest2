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
// Tests internal event lifecycle helpers H5ES__event_new, H5ES__event_free,
// and H5ES__event_completed (third_party/hdf5/src/H5ESevent.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5ESevent.c

#include <cstdint>
#include <limits>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer) || \
    __has_feature(hwaddress_sanitizer) || __has_feature(memory_sanitizer) || \
    __has_feature(thread_sanitizer)
#define H5ESEVENT_TEST_HAS_SANITIZER 1
#endif
#endif
#if !defined(H5ESEVENT_TEST_HAS_SANITIZER) &&                            \
    (defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__) ||   \
     defined(LEAK_SANITIZER) || defined(HWADDRESS_SANITIZER) ||       \
     defined(__SANITIZE_HWADDRESS__) || defined(MEMORY_SANITIZER) ||  \
     defined(THREAD_SANITIZER) || defined(__SANITIZE_THREAD__))
#define H5ESEVENT_TEST_HAS_SANITIZER 1
#endif

// `:hdf5` compiles with -DNDEBUG; match internal struct layouts across
// H5ESpkg.h, H5CXprivate.h, and H5VLpkg.h.
#ifndef NDEBUG
#define H5ESEVENT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5ES_FRIEND
#define H5VL_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5ESpkg.h"
#include "third_party/hdf5/src/H5FLprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5VLpkg.h"
H5FL_EXTERN(H5VL_object_t);
}  // extern "C"
#ifdef H5ESEVENT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5ESEVENT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

constexpr H5VL_class_value_t kMockVolValue = 301;
constexpr char kMockVolName[] = "h5es_event_mock_vol";
constexpr H5VL_class_value_t kMockTermVolValue = 302;
constexpr char kMockTermVolName[] = "h5es_event_term_mock_vol";

bool g_fail_terminate = false;
int g_terminate_calls = 0;

herr_t MockTerminate() {
  ++g_terminate_calls;
  return g_fail_terminate ? -1 : 0;
}

struct MockVolState {
  bool fail_free = false;
  int free_calls = 0;
};

struct MockRequestToken {
  MockVolState* state = nullptr;
  int id = 0;
};

herr_t MockRequestFree(void* req) {
  auto* token = static_cast<MockRequestToken*>(req);
  ++token->state->free_calls;
  if (token->state->fail_free) {
    return -1;
  }
  delete token;
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
  void Register(H5VL_class_value_t value = kMockVolValue,
                const char* name = kMockVolName,
                herr_t (*terminate_fn)() = nullptr) {
    H5VL_class_t cls{};
    cls.version = H5VL_VERSION;
    cls.value = value;
    cls.name = name;
    cls.terminate = terminate_fn;
    cls.request_cls.free = MockRequestFree;
    vol_id_ = H5VLregister_connector(&cls, H5P_DEFAULT);
    if (vol_id_ >= 0) {
      connector_ =
          static_cast<H5VL_connector_t*>(H5I_object_verify(vol_id_, H5I_VOL));
    }
  }

  herr_t Unregister() {
    if (vol_id_ < 0) {
      return 0;
    }
    const hid_t id = vol_id_;
    vol_id_ = H5I_INVALID_HID;
    return H5VLunregister_connector(id);
  }

  ~ScopedMockVol() {
    if (vol_id_ >= 0) {
      H5VLunregister_connector(vol_id_);
    }
  }

  hid_t id() const { return vol_id_; }
  H5VL_connector_t* connector() const { return connector_; }

 private:
  hid_t vol_id_ = H5I_INVALID_HID;
  H5VL_connector_t* connector_ = nullptr;
};

TEST(H5ESeventTest, NewAndFreeEventWithAndWithoutApiArgs) {
  ScopedMockVol vol;
  ASSERT_NO_FATAL_FAILURE(vol.Register());
  ASSERT_NE(vol.id(), H5I_INVALID_HID);
  ASSERT_NE(vol.connector(), nullptr);

  MockVolState state;
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());

  // 1. Allocate and free an event without api_args.
  auto* tok1 = new MockRequestToken{&state, 1};
  H5ES_event_t* ev1 = H5ES__event_new(vol.connector(), tok1);
  if (ev1 == nullptr) {
    delete tok1;
    ASSERT_NE(ev1, nullptr);
  }
  ASSERT_NE(ev1->request, nullptr);
  EXPECT_EQ(ev1->op_info.api_args, nullptr);
  EXPECT_GE(H5ES__event_free(ev1), 0);
  EXPECT_EQ(state.free_calls, 1);

  // 2. Allocate and free an event carrying static metadata and heap api_args.
  auto* tok2 = new MockRequestToken{&state, 2};
  H5ES_event_t* ev2 = H5ES__event_new(vol.connector(), tok2);
  if (ev2 == nullptr) {
    delete tok2;
    ASSERT_NE(ev2, nullptr);
  }
  ev2->op_info.api_name = "TestOp";
  ev2->op_info.api_args = H5MM_xstrdup("arg=1");
  ev2->op_info.app_file_name = "H5ESevent_test.cc";
  ev2->op_info.app_func_name = "NewAndFreeEventWithAndWithoutApiArgs";

  EXPECT_GE(H5ES__event_free(ev2), 0);
  EXPECT_EQ(state.free_calls, 2);
}

TEST(H5ESeventTest, FreeEventHandlesNullRequestSafely) {
  ScopedMockVol vol;
  vol.Register();
  ASSERT_NE(vol.id(), H5I_INVALID_HID);
  ASSERT_NE(vol.connector(), nullptr);

  MockVolState state;
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());

  auto* tok = new MockRequestToken{&state, 3};
  H5ES_event_t* ev = H5ES__event_new(vol.connector(), tok);
  if (ev == nullptr) {
    delete tok;
    ASSERT_NE(ev, nullptr);
  }

  // Detach and release the VOL request manually so H5ES__event_free exercises
  // its `ev->request == NULL` branch cleanly without leaking resources.
  ASSERT_GE(H5VL_request_free(ev->request), 0);
  ASSERT_GE(H5VL_free_object(ev->request), 0);
  ev->request = nullptr;

  EXPECT_GE(H5ES__event_free(ev), 0);
  EXPECT_EQ(state.free_calls, 1);
}

TEST(H5ESeventTest, FreeEventFailureRetriesWithoutLeak) {
  ScopedMockVol vol;
  vol.Register();
  ASSERT_NE(vol.id(), H5I_INVALID_HID);
  ASSERT_NE(vol.connector(), nullptr);

  MockVolState state;
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());

  auto* tok = new MockRequestToken{&state, 4};
  H5ES_event_t* ev = H5ES__event_new(vol.connector(), tok);
  if (ev == nullptr) {
    delete tok;
    ASSERT_NE(ev, nullptr);
  }
  absl::Cleanup cleanup_ev = [&] {
    if (ev != nullptr) {
      state.fail_free = false;
      H5ES__event_free(ev);
      ev = nullptr;
    }
  };

  {
    ScopedErrorSilence silence;
    state.fail_free = true;
    EXPECT_LT(H5ES__event_free(ev), 0);
  }
  EXPECT_EQ(state.free_calls, 1);

  state.fail_free = false;
  EXPECT_GE(H5ES__event_free(ev), 0);
  ev = nullptr;
  EXPECT_EQ(state.free_calls, 2);
}

TEST(H5ESeventTest, EventCompletedRemovesFromListAndPropagatesFreeError) {
  ScopedMockVol vol;
  vol.Register();
  ASSERT_NE(vol.id(), H5I_INVALID_HID);
  ASSERT_NE(vol.connector(), nullptr);

  MockVolState state;
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());

  auto* tok1 = new MockRequestToken{&state, 5};
  H5ES_event_t* ev1 = H5ES__event_new(vol.connector(), tok1);
  if (ev1 == nullptr) {
    delete tok1;
    ASSERT_NE(ev1, nullptr);
  }
  absl::Cleanup cleanup_ev1 = [&] {
    if (ev1 != nullptr) {
      state.fail_free = false;
      H5ES__event_free(ev1);
      ev1 = nullptr;
    }
  };

  auto* tok2 = new MockRequestToken{&state, 6};
  H5ES_event_t* ev2 = H5ES__event_new(vol.connector(), tok2);
  if (ev2 == nullptr) {
    delete tok2;
    ASSERT_NE(ev2, nullptr);
  }
  absl::Cleanup cleanup_ev2 = [&] {
    if (ev2 != nullptr) {
      state.fail_free = false;
      H5ES__event_free(ev2);
      ev2 = nullptr;
    }
  };

  H5ES_event_list_t list{};
  H5ES__list_append(&list, ev1);
  H5ES__list_append(&list, ev2);
  ASSERT_EQ(H5ES__list_count(&list), 2u);

  // 1. Normal completion unlinks and frees ev1.
  EXPECT_GE(H5ES__event_completed(ev1, &list), 0);
  ev1 = nullptr;
  EXPECT_EQ(H5ES__list_count(&list), 1u);
  EXPECT_EQ(list.head, ev2);
  EXPECT_EQ(list.tail, ev2);

  // 2. Failing request_cls.free unlinks ev2 from list and returns < 0.
  {
    ScopedErrorSilence silence;
    state.fail_free = true;
    EXPECT_LT(H5ES__event_completed(ev2, &list), 0);
  }
  EXPECT_EQ(H5ES__list_count(&list), 0u);
  EXPECT_EQ(list.head, nullptr);
  EXPECT_EQ(list.tail, nullptr);

  state.fail_free = false;
  EXPECT_GE(H5ES__event_free(ev2), 0);
  ev2 = nullptr;
  EXPECT_EQ(state.free_calls, 3);
}

TEST(H5ESeventTest, FreeEventPropagatesVolObjectReleaseFailure) {
  g_fail_terminate = false;
  g_terminate_calls = 0;
  absl::Cleanup reset_term = [] { g_fail_terminate = false; };

  ScopedMockVol vol;
  vol.Register(kMockTermVolValue, kMockTermVolName, MockTerminate);
  ASSERT_NE(vol.id(), H5I_INVALID_HID);
  ASSERT_NE(vol.connector(), nullptr);

  MockVolState state;
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());

  auto* tok = new MockRequestToken{&state, 7};
  H5ES_event_t* ev = H5ES__event_new(vol.connector(), tok);
  if (ev == nullptr) {
    delete tok;
    ASSERT_NE(ev, nullptr);
  }
  bool manual_request_cleanup = false;
  absl::Cleanup cleanup_ev = [&] {
    if (ev != nullptr) {
      g_fail_terminate = false;
      state.fail_free = false;
      if (manual_request_cleanup && ev->request != nullptr) {
        H5VL_conn_inc_rc(ev->request->connector);
        H5VL_object_inc_rc(ev->request);
        H5VL_free_object(ev->request);
        ev->request = nullptr;
      }
      H5ES__event_free(ev);
      ev = nullptr;
    }
  };

  // Unregister the connector ID so ev->request holds the final reference to
  // vol.connector(), causing H5VL_free_object(ev->request) to invoke
  // cls->terminate().
  ASSERT_GE(vol.Unregister(), 0);
  EXPECT_EQ(g_terminate_calls, 0);

  g_fail_terminate = true;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5ES__event_free(ev), 0);
  }
  manual_request_cleanup = true;
  EXPECT_EQ(state.free_calls, 1);
  EXPECT_EQ(g_terminate_calls, 1);

  // Restore refcounts on the failed VOL object and connector so both terminate
  // cleanly without leaking before freeing ev.
  g_fail_terminate = false;
  H5VL_conn_inc_rc(ev->request->connector);
  H5VL_object_inc_rc(ev->request);
  ASSERT_GE(H5VL_free_object(ev->request), 0);
  ev->request = nullptr;
  manual_request_cleanup = false;
  EXPECT_EQ(g_terminate_calls, 2);
  EXPECT_GE(H5ES__event_free(ev), 0);
  ev = nullptr;
}

#if !defined(H5ESEVENT_TEST_HAS_SANITIZER)
TEST(H5ESeventTest, NewEventHandlesRequestObjectAllocationFailure) {
  ScopedMockVol vol;
  vol.Register();
  ASSERT_NE(vol.id(), H5I_INVALID_HID);
  ASSERT_NE(vol.connector(), nullptr);

  MockVolState state;
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());

  // Warm the H5VL_object_t free list, then flush cached blocks so the next
  // H5FL_CALLOC(H5VL_object_t) hits H5FL__malloc().
  auto* warm_tok = new MockRequestToken{&state, 8};
  H5ES_event_t* warm_ev = H5ES__event_new(vol.connector(), warm_tok);
  if (warm_ev == nullptr) {
    delete warm_tok;
    ASSERT_NE(warm_ev, nullptr);
  }
  ASSERT_GE(H5ES__event_free(warm_ev), 0);
  ASSERT_GE(H5FL_garbage_coll(), 0);
  ASSERT_TRUE(H5_H5VL_object_t_reg_free_list.init);
  ASSERT_EQ(H5_H5VL_object_t_reg_free_list.list, nullptr);

  const size_t saved_size = H5_H5VL_object_t_reg_free_list.size;
  absl::Cleanup restore_fl = [&] {
    H5_H5VL_object_t_reg_free_list.size = saved_size;
  };
  H5_H5VL_object_t_reg_free_list.size = std::numeric_limits<size_t>::max();

  // Upstream H5ES__event_new passes `token` directly to H5VL_request_free() on
  // H5VL_create_object() failure; pass a stack H5VL_object_t wrapper so both
  // the request-free success and error branches run cleanly.
  auto* tok1 = new MockRequestToken{&state, 9};
  H5VL_object_t token_obj1{tok1, vol.connector(), 1};
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5ES__event_new(vol.connector(), &token_obj1), nullptr);
  }
  EXPECT_EQ(state.free_calls, 2);

  auto* tok2 = new MockRequestToken{&state, 10};
  H5VL_object_t token_obj2{tok2, vol.connector(), 1};
  state.fail_free = true;
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5ES__event_new(vol.connector(), &token_obj2), nullptr);
  }
  EXPECT_EQ(state.free_calls, 3);
  state.fail_free = false;
  delete tok2;
}
#endif

}  // namespace
}  // namespace hdf5
