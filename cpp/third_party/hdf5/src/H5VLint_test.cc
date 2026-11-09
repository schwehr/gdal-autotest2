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
// Tests internal Virtual Object Layer (VOL) connector management, object
// wrapping, library state, and argument setup routines
// (third_party/hdf5/src/H5VLint.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5VLint.c

#ifndef NDEBUG
#define H5VL_INT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5VL_FRIEND
#include "third_party/hdf5/src/H5VLpkg.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Pprivate.h"
#include "third_party/hdf5/src/H5PLprivate.h"
#include "third_party/hdf5/src/H5VLnative_private.h"
#include "third_party/hdf5/src/H5VLpassthru_private.h"
}  // extern "C"

#ifdef H5VL_INT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5VL_INT_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

struct DummyVolInfo {
  int magic = 0;
};

struct DummyFileObj {
  int file_id_val = 0;
  bool is_equal = true;
};

struct DummyWrapCtx {
  int ctx_tag = 0;
};

struct DummyWrappedObj {
  void* under = nullptr;
};

struct DummyVolHooks {
  bool fail_init = false;
  bool fail_info_copy = false;
  bool fail_info_cmp = false;
  bool fail_info_free = false;
  bool fail_get_cap_flags = false;
  bool fail_file_is_equal = false;
  bool file_is_equal_result = true;
  bool fail_get_wrap_ctx = false;
  bool fail_wrap_object = false;
  bool fail_unwrap_object = false;
  bool fail_datatype_get = false;
  bool fail_get_object = false;
  bool wrap_returns_raw_obj = false;
  int fail_get_conn_cls_countdown = 0;
  int init_calls = 0;
  int term_calls = 0;
  int next_file_val = 1;
};

struct LiveInfoTracker {
  std::array<void*, 32> ptrs{};
  size_t count = 0;

  void Insert(void* ptr) {
    if (ptr && count < ptrs.size()) {
      ptrs[count++] = ptr;
    }
  }

  bool Erase(void* ptr) {
    for (size_t i = 0; i < count; ++i) {
      if (ptrs[i] == ptr) {
        ptrs[i] = ptrs[--count];
        ptrs[count] = nullptr;
        return true;
      }
    }
    return false;
  }

  bool empty() const { return count == 0; }
};

DummyVolHooks g_hooks{};
LiveInfoTracker g_live_infos{};

void ResetHooks() { g_hooks = DummyVolHooks{}; }

DummyVolInfo* AllocDummyInfo(int magic) {
  auto* info = static_cast<DummyVolInfo*>(std::malloc(sizeof(DummyVolInfo)));
  if (info) {
    info->magic = magic;
    g_live_infos.Insert(info);
  }
  return info;
}

herr_t DummyInit(hid_t /*vipl_id*/) {
  ++g_hooks.init_calls;
  return g_hooks.fail_init ? -1 : 0;
}

herr_t DummyTerm() {
  ++g_hooks.term_calls;
  return 0;
}

void* DummyInfoCopy(const void* info) {
  if (g_hooks.fail_info_copy || info == nullptr) return nullptr;
  const auto* src = static_cast<const DummyVolInfo*>(info);
  return AllocDummyInfo(src->magic);
}

herr_t DummyInfoCmp(int* cmp_value, const void* info1, const void* info2) {
  if (g_hooks.fail_info_cmp) return -1;
  const auto* a = static_cast<const DummyVolInfo*>(info1);
  const auto* b = static_cast<const DummyVolInfo*>(info2);
  if (a == b) {
    *cmp_value = 0;
    return 0;
  }
  if (!a) {
    *cmp_value = -1;
    return 0;
  }
  if (!b) {
    *cmp_value = 1;
    return 0;
  }
  *cmp_value = (a->magic < b->magic) ? -1 : (a->magic > b->magic ? 1 : 0);
  return 0;
}

herr_t DummyInfoFree(void* info) {
  if (info && g_live_infos.Erase(info)) {
    std::free(info);
  }
  return g_hooks.fail_info_free ? -1 : 0;
}

herr_t DummyInfoToStr(const void* info, char** str) {
  const auto* d = static_cast<const DummyVolInfo*>(info);
  const int val = d ? d->magic : 0;
  *str = static_cast<char*>(H5allocate_memory(32, false));
  if (!*str) return -1;
  std::snprintf(*str, 32, "magic=%d", val);
  return 0;
}

herr_t DummyInfoFromStr(const char* str, void** info) {
  int val = 0;
  if (!str || std::sscanf(str, "magic=%d", &val) != 1) {
    *info = nullptr;
    return -1;
  }
  DummyVolInfo* out = AllocDummyInfo(val);
  if (!out) return -1;
  *info = out;
  return 0;
}

herr_t DummyWrapGetCtx(const void* /*obj*/, void** wrap_ctx) {
  if (g_hooks.fail_get_wrap_ctx) {
    *wrap_ctx = nullptr;
    return -1;
  }
  auto* ctx = static_cast<DummyWrapCtx*>(std::malloc(sizeof(DummyWrapCtx)));
  if (!ctx) return -1;
  ctx->ctx_tag = 77;
  *wrap_ctx = ctx;
  return 0;
}

void* DummyWrapGetObject(const void* obj) {
  if (g_hooks.fail_get_object) return nullptr;
  const auto* w = static_cast<const DummyWrappedObj*>(obj);
  return w ? w->under : nullptr;
}

void* DummyWrapObject(void* obj, H5I_type_t /*obj_type*/, void* /*wrap_ctx*/) {
  if (g_hooks.fail_wrap_object) return nullptr;
  if (g_hooks.wrap_returns_raw_obj) return obj;
  auto* w = static_cast<DummyWrappedObj*>(std::malloc(sizeof(DummyWrappedObj)));
  if (!w) return nullptr;
  w->under = obj;
  return w;
}

void* DummyUnwrapObject(void* obj) {
  if (g_hooks.fail_unwrap_object) return nullptr;
  auto* w = static_cast<DummyWrappedObj*>(obj);
  if (!w) return nullptr;
  void* under = w->under;
  std::free(w);
  return under;
}

herr_t DummyFreeWrapCtx(void* wrap_ctx) {
  std::free(wrap_ctx);
  return 0;
}

void* DummyFileCreate(const char* /*name*/, unsigned /*flags*/,
                      hid_t /*fcpl_id*/, hid_t /*fapl_id*/, hid_t /*dxpl_id*/,
                      void** /*req*/) {
  auto* f = static_cast<DummyFileObj*>(std::malloc(sizeof(DummyFileObj)));
  if (f) {
    f->file_id_val = g_hooks.next_file_val++;
    f->is_equal = g_hooks.file_is_equal_result;
  }
  return f;
}

void* DummyFileOpen(const char* /*name*/, unsigned /*flags*/, hid_t /*fapl_id*/,
                    hid_t /*dxpl_id*/, void** /*req*/) {
  auto* f = static_cast<DummyFileObj*>(std::malloc(sizeof(DummyFileObj)));
  if (f) {
    f->file_id_val = g_hooks.next_file_val++;
    f->is_equal = g_hooks.file_is_equal_result;
  }
  return f;
}

herr_t DummyFileSpecific(void* /*obj*/, H5VL_file_specific_args_t* args,
                         hid_t /*dxpl_id*/, void** /*req*/) {
  if (args->op_type == H5VL_FILE_IS_EQUAL) {
    if (g_hooks.fail_file_is_equal) return -1;
    *args->args.is_equal.same_file = g_hooks.file_is_equal_result;
    return 0;
  }
  return 0;
}

herr_t DummyFileClose(void* file, hid_t /*dxpl_id*/, void** /*req*/) {
  std::free(file);
  return 0;
}

herr_t DummyGroupClose(void* grp, hid_t /*dxpl_id*/, void** /*req*/) {
  std::free(grp);
  return 0;
}

herr_t DummyDatatypeGet(void* /*obj*/, H5VL_datatype_get_args_t* /*args*/,
                        hid_t /*dxpl_id*/, void** /*req*/) {
  return g_hooks.fail_datatype_get ? -1 : -1;
}

extern const H5VL_class_t kDummyVolClsA;
extern const H5VL_class_t kDummyVolClsB;
extern const H5VL_class_t kDummyVolClsC;

herr_t DummyIntrospectGetConnClsA(void* /*obj*/, H5VL_get_conn_lvl_t /*lvl*/,
                                  const H5VL_class_t** conn_cls) {
  if (g_hooks.fail_get_conn_cls_countdown > 0 &&
      --g_hooks.fail_get_conn_cls_countdown == 0) {
    *conn_cls = nullptr;
    return -1;
  }
  *conn_cls = &kDummyVolClsA;
  return 0;
}

herr_t DummyIntrospectGetConnClsB(void* /*obj*/, H5VL_get_conn_lvl_t /*lvl*/,
                                  const H5VL_class_t** conn_cls) {
  *conn_cls = &kDummyVolClsB;
  return 0;
}

herr_t DummyIntrospectGetCapFlags(const void* /*info*/, uint64_t* cap_flags) {
  if (g_hooks.fail_get_cap_flags) return -1;
  if (cap_flags) *cap_flags = H5VL_CAP_FLAG_THREADSAFE | H5VL_CAP_FLAG_ASYNC;
  return 0;
}

herr_t DummyIntrospectOptQuery(void* /*obj*/, H5VL_subclass_t /*subcls*/,
                               int /*opt_type*/, uint64_t* flags) {
  if (flags) *flags = 0;
  return 0;
}

const H5VL_class_t kDummyVolClsA = {
    .version = H5VL_VERSION,
    .value = 301,
    .name = "google_test_dummy_vol_a",
    .conn_version = 1,
    .cap_flags = H5VL_CAP_FLAG_THREADSAFE,
    .initialize = DummyInit,
    .terminate = DummyTerm,
    .info_cls =
        {
            .size = sizeof(DummyVolInfo),
            .copy = DummyInfoCopy,
            .cmp = DummyInfoCmp,
            .free = DummyInfoFree,
            .to_str = DummyInfoToStr,
            .from_str = DummyInfoFromStr,
        },
    .wrap_cls = {},
    .attr_cls = {},
    .dataset_cls = {},
    .datatype_cls = {.get = DummyDatatypeGet},
    .file_cls =
        {
            .create = DummyFileCreate,
            .open = DummyFileOpen,
            .specific = DummyFileSpecific,
            .close = DummyFileClose,
        },
    .group_cls = {},
    .link_cls = {},
    .object_cls = {},
    .introspect_cls =
        {
            .get_conn_cls = DummyIntrospectGetConnClsA,
            .get_cap_flags = DummyIntrospectGetCapFlags,
            .opt_query = DummyIntrospectOptQuery,
        },
};

const H5VL_class_t kDummyVolClsB = {
    .version = H5VL_VERSION,
    .value = 302,
    .name = "google_test_dummy_vol_b",
    .conn_version = 1,
    .cap_flags = H5VL_CAP_FLAG_NONE,
    .initialize = nullptr,
    .terminate = nullptr,
    .info_cls = {},
    .wrap_cls =
        {
            .get_object = DummyWrapGetObject,
            .get_wrap_ctx = DummyWrapGetCtx,
            .wrap_object = DummyWrapObject,
            .unwrap_object = DummyUnwrapObject,
            .free_wrap_ctx = DummyFreeWrapCtx,
        },
    .attr_cls = {},
    .dataset_cls = {},
    .datatype_cls = {.get = DummyDatatypeGet},
    .file_cls =
        {
            .create = DummyFileCreate,
            .close = DummyFileClose,
        },
    .group_cls = {.close = DummyGroupClose},
    .link_cls = {},
    .object_cls = {},
    .introspect_cls =
        {
            .get_conn_cls = DummyIntrospectGetConnClsB,
            .get_cap_flags = nullptr,
            .opt_query = DummyIntrospectOptQuery,
        },
};

const H5VL_class_t kDummyVolClsC = {
    .version = H5VL_VERSION,
    .value = 303,
    .name = "google_test_dummy_vol_c",
    .conn_version = 2,
    .cap_flags = H5VL_CAP_FLAG_NONE,
    .initialize = DummyInit,
    .terminate = DummyTerm,
};

TEST(H5VLintTest, LibraryCloseAndReopenTerminatesAndInitializesVolPackage) {
  ASSERT_GE(H5open(), 0);
  ResetHooks();

  // Register a dynamic optional operation so H5VL_term_package exercises
  // releasing remaining dyn ops during library teardown.
  int dyn_op = -1;
  ASSERT_GE(H5VLregister_opt_operation(H5VL_SUBCLS_FILE,
                                       "org.google.h5vlint.close_test",
                                       &dyn_op),
            0);

  // Shut down via H5close (runs H5VL_term_package steps for def_conn, H5I_VOL,
  // dyn_ops, and type refcount drop). Calling H5VL_term_package again while
  // uninitialized hits the inactive-package guard.
  ASSERT_GE(H5close(), 0);
  EXPECT_EQ(H5VL_term_package(), 0);

  ASSERT_GE(H5open(), 0);
  EXPECT_NE(H5VL_NATIVE_conn_g, nullptr);
  EXPECT_NE(H5VL_PASSTHRU_conn_g, nullptr);
}

TEST(H5VLintTest, SetDefConnParsesEnvVarAndHandlesErrors) {
  ASSERT_GE(H5open(), 0);
  ResetHooks();

  const char* prev_env = std::getenv(HDF5_VOL_CONNECTOR);
  std::string saved_env = prev_env ? prev_env : "";
  const bool had_env = (prev_env != nullptr);

  hid_t dummy_vol_id = H5I_INVALID_HID;
  absl::Cleanup restore_state = [&] {
    if (had_env) {
      setenv(HDF5_VOL_CONNECTOR, saved_env.c_str(), 1);
    } else {
      unsetenv(HDF5_VOL_CONNECTOR);
    }
    (void)H5VL__set_def_conn();
    if (dummy_vol_id != H5I_INVALID_HID) {
      (void)H5VLunregister_connector(dummy_vol_id);
    }
    ResetHooks();
  };

  // Unset or empty HDF5_VOL_CONNECTOR selects H5_DEFAULT_VOL ("native").
  unsetenv(HDF5_VOL_CONNECTOR);
  ASSERT_GE(H5VL__set_def_conn(), 0);
  bool is_def = false;
  H5VL__is_default_conn(H5P_FILE_ACCESS_DEFAULT, H5VL_NATIVE_conn_g, &is_def);
  EXPECT_TRUE(is_def);

  setenv(HDF5_VOL_CONNECTOR, "", 1);
  ASSERT_GE(H5VL__set_def_conn(), 0);

  // Explicit "native" hits the already-registered path.
  setenv(HDF5_VOL_CONNECTOR, "native", 1);
  ASSERT_GE(H5VL__set_def_conn(), 0);

  // Explicit "pass_through" with under_vol info parses str_to_info.
  setenv(HDF5_VOL_CONNECTOR, "pass_through under_vol=0;under_info={}", 1);
  ASSERT_GE(H5VL__set_def_conn(), 0);
  H5VL__is_default_conn(H5P_FILE_ACCESS_DEFAULT, H5VL_NATIVE_conn_g, &is_def);
  EXPECT_FALSE(is_def);

  // Register custom dummy connector with info_cls.from_str for env parsing tests.
  dummy_vol_id = H5VLregister_connector(&kDummyVolClsA, H5P_DEFAULT);
  ASSERT_GE(dummy_vol_id, 0);

  setenv(HDF5_VOL_CONNECTOR, "google_test_dummy_vol_a magic=99", 1);
  ASSERT_GE(H5VL__set_def_conn(), 0);

  // Error cases for HDF5_VOL_CONNECTOR.
  ScopedErrorSilence silence;
  setenv(HDF5_VOL_CONNECTOR, "   \t\n ", 1);
  EXPECT_LT(H5VL__set_def_conn(), 0);

  setenv(HDF5_VOL_CONNECTOR, "no_such_vol_connector_xyz", 1);
  EXPECT_LT(H5VL__set_def_conn(), 0);

  // Registered connector whose from_str fails exercises connector ref cleanup.
  setenv(HDF5_VOL_CONNECTOR, "google_test_dummy_vol_a bad_info_token", 1);
  EXPECT_LT(H5VL__set_def_conn(), 0);

  // H5P_set_vol failure cleans up vol_info and handles free-info errors too.
  unsetenv(HDF5_VOL_CONNECTOR);
  ASSERT_GE(H5VL__set_def_conn(), 0);
  setenv(HDF5_VOL_CONNECTOR, "google_test_dummy_vol_a magic=99", 1);
  g_hooks.fail_info_copy = true;
  g_hooks.fail_info_free = true;
  EXPECT_LT(H5VL__set_def_conn(), 0);
  g_hooks.fail_info_copy = false;
  g_hooks.fail_info_free = false;
  unsetenv(HDF5_VOL_CONNECTOR);
  ASSERT_GE(H5VL__set_def_conn(), 0);
  EXPECT_TRUE(g_live_infos.empty());
}

TEST(H5VLintTest, ConnectorRegistrationValidationLookupAndListRemoval) {
  ASSERT_GE(H5open(), 0);
  ResetHooks();
  absl::Cleanup reset_hooks = [] { ResetHooks(); };

  // Validation errors in H5VL__register_connector_by_class.
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5VL__register_connector_by_class(nullptr, H5P_DEFAULT), nullptr);

    H5VL_class_t bad_ver = kDummyVolClsA;
    bad_ver.version = H5VL_VERSION + 1;
    EXPECT_EQ(H5VL__register_connector_by_class(&bad_ver, H5P_DEFAULT),
              nullptr);

    H5VL_class_t null_name = kDummyVolClsA;
    null_name.name = nullptr;
    EXPECT_EQ(H5VL__register_connector_by_class(&null_name, H5P_DEFAULT),
              nullptr);

    H5VL_class_t empty_name = kDummyVolClsA;
    empty_name.name = "";
    EXPECT_EQ(H5VL__register_connector_by_class(&empty_name, H5P_DEFAULT),
              nullptr);

    H5VL_class_t copy_no_free = kDummyVolClsA;
    copy_no_free.info_cls.free = nullptr;
    EXPECT_EQ(H5VL__register_connector_by_class(&copy_no_free, H5P_DEFAULT),
              nullptr);

    H5VL_class_t wrap_no_free = kDummyVolClsB;
    wrap_no_free.wrap_cls.free_wrap_ctx = nullptr;
    EXPECT_EQ(H5VL__register_connector_by_class(&wrap_no_free, H5P_DEFAULT),
              nullptr);

    g_hooks.fail_init = true;
    EXPECT_EQ(H5VL__register_connector_by_class(&kDummyVolClsA, H5P_DEFAULT),
              nullptr);
    g_hooks.fail_init = false;
  }

  // Register 3 connectors so linked list removal covers head, middle, and tail.
  H5VL_connector_t* c1 =
      H5VL__register_connector_by_class(&kDummyVolClsA, H5P_DEFAULT);
  ASSERT_NE(c1, nullptr);
  H5VL_connector_t* c2 =
      H5VL__register_connector_by_class(&kDummyVolClsB, H5P_DEFAULT);
  ASSERT_NE(c2, nullptr);
  H5VL_connector_t* c3 =
      H5VL__register_connector_by_class(&kDummyVolClsC, H5P_DEFAULT);
  ASSERT_NE(c3, nullptr);

  bool freed_all = false;
  absl::Cleanup release_conns = [&] {
    if (!freed_all) {
      if (c2) (void)H5VL_conn_dec_rc(c2);
      if (c1) (void)H5VL_conn_dec_rc(c1);
      if (c3) (void)H5VL_conn_dec_rc(c3);
    }
  };

  // Re-registering same class/name/value returns the existing connector.
  H5VL_connector_t* c1_dup =
      H5VL__register_connector_by_class(&kDummyVolClsA, H5P_DEFAULT);
  EXPECT_EQ(c1_dup, c1);
  ASSERT_GE(H5VL_conn_dec_rc(c1_dup), 1);

  H5VL_connector_t* by_name =
      H5VL__register_connector_by_name(kDummyVolClsA.name, H5P_DEFAULT);
  EXPECT_EQ(by_name, c1);
  ASSERT_GE(H5VL_conn_dec_rc(by_name), 1);

  H5VL_connector_t* by_val =
      H5VL__register_connector_by_value(kDummyVolClsB.value, H5P_DEFAULT);
  EXPECT_EQ(by_val, c2);
  ASSERT_GE(H5VL_conn_dec_rc(by_val), 1);

  EXPECT_GT(H5VL__is_connector_registered_by_name(kDummyVolClsA.name), 0);
  EXPECT_EQ(H5VL__is_connector_registered_by_name("missing_vol_name"), 0);
  EXPECT_GT(H5VL__is_connector_registered_by_value(kDummyVolClsB.value), 0);
  EXPECT_EQ(H5VL__is_connector_registered_by_value(9999), 0);

  H5VL_connector_t* got_name = H5VL__get_connector_by_name(kDummyVolClsA.name);
  EXPECT_EQ(got_name, c1);
  ASSERT_GE(H5VL_conn_dec_rc(got_name), 1);
  EXPECT_EQ(H5VL__get_connector_by_name("missing_vol_name"), nullptr);

  H5VL_connector_t* got_val =
      H5VL__get_connector_by_value(kDummyVolClsB.value);
  EXPECT_EQ(got_val, c2);
  ASSERT_GE(H5VL_conn_dec_rc(got_val), 1);
  EXPECT_EQ(H5VL__get_connector_by_value(9999), nullptr);

  // Unknown plugin name/value attempts H5PL_load and fails gracefully.
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5VL__register_connector_by_name("missing_vol_plugin", H5P_DEFAULT),
              nullptr);
    EXPECT_EQ(H5VL__register_connector_by_value(9998, H5P_DEFAULT), nullptr);
  }

  // Name retrieval: NULL buffer, truncated buffer, and full buffer.
  const size_t full_len = H5VL__get_connector_name(c1, nullptr, 0);
  EXPECT_EQ(full_len, std::strlen(kDummyVolClsA.name));

  std::array<char, 7> short_buf{};
  short_buf.fill('Z');
  EXPECT_EQ(H5VL__get_connector_name(c1, short_buf.data(), short_buf.size()),
            full_len);
  EXPECT_STREQ(short_buf.data(), "google");

  std::string full_buf(full_len + 1, '\0');
  EXPECT_EQ(H5VL__get_connector_name(c1, full_buf.data(), full_buf.size()),
            full_len);
  EXPECT_STREQ(full_buf.c_str(), kDummyVolClsA.name);

  // Same class check and ID registration.
  EXPECT_GT(H5VL_conn_same_class(c1, c1), 0);
  EXPECT_EQ(H5VL_conn_same_class(c1, c2), 0);

  // Two distinct connector structs wrapping the same class compare equal.
  H5VL_connector_t* c1_sep =
      H5VL__register_connector(&kDummyVolClsA, H5P_DEFAULT);
  ASSERT_NE(c1_sep, nullptr);
  EXPECT_GT(H5VL_conn_same_class(c1, c1_sep), 0);
  EXPECT_EQ(H5VL_conn_inc_rc(c1_sep), 1);
  EXPECT_EQ(H5VL_conn_dec_rc(c1_sep), 0);

  hid_t reg_id = H5VL_conn_register(c1);
  ASSERT_GE(reg_id, 0);
  EXPECT_GE(H5Idec_ref(reg_id), 0);

  // Remove middle (c2), then tail (c1), then head (c3).
  EXPECT_EQ(H5VL_conn_dec_rc(c2), 0);
  c2 = nullptr;
  EXPECT_EQ(H5VL_conn_dec_rc(c1), 0);
  c1 = nullptr;
  EXPECT_EQ(H5VL_conn_dec_rc(c3), 0);
  c3 = nullptr;
  freed_all = true;
  EXPECT_GE(g_hooks.term_calls, 2);
}

TEST(H5VLintTest, ClassCompareInfoPropsAndPluginLoadChecks) {
  ASSERT_GE(H5open(), 0);
  ResetHooks();
  absl::Cleanup reset_hooks = [] { ResetHooks(); };

  H5VL_class_t base = kDummyVolClsA;
  H5VL_class_t copy_same = kDummyVolClsA;
  int cmp = 99;

  ASSERT_GE(H5VL_cmp_connector_cls(&cmp, &base, &base), 0);
  EXPECT_EQ(cmp, 0);
  ASSERT_GE(H5VL_cmp_connector_cls(&cmp, &base, &copy_same), 0);
  EXPECT_EQ(cmp, 0);

  H5VL_class_t other = copy_same;
  other.value = base.value + 1;
  ASSERT_GE(H5VL_cmp_connector_cls(&cmp, &base, &other), 0);
  EXPECT_LT(cmp, 0);
  ASSERT_GE(H5VL_cmp_connector_cls(&cmp, &other, &base), 0);
  EXPECT_GT(cmp, 0);

  other = copy_same;
  other.name = nullptr;
  ASSERT_GE(H5VL_cmp_connector_cls(&cmp, &other, &base), 0);
  EXPECT_LT(cmp, 0);
  ASSERT_GE(H5VL_cmp_connector_cls(&cmp, &base, &other), 0);
  EXPECT_GT(cmp, 0);

  other.name = "zzzzzz";
  ASSERT_GE(H5VL_cmp_connector_cls(&cmp, &base, &other), 0);
  EXPECT_LT(cmp, 0);

  other = copy_same;
  other.version = base.version + 1;
  ASSERT_GE(H5VL_cmp_connector_cls(&cmp, &base, &other), 0);
  EXPECT_LT(cmp, 0);
  ASSERT_GE(H5VL_cmp_connector_cls(&cmp, &other, &base), 0);
  EXPECT_GT(cmp, 0);

  other = copy_same;
  other.info_cls.size = base.info_cls.size + 8;
  ASSERT_GE(H5VL_cmp_connector_cls(&cmp, &base, &other), 0);
  EXPECT_LT(cmp, 0);
  ASSERT_GE(H5VL_cmp_connector_cls(&cmp, &other, &base), 0);
  EXPECT_GT(cmp, 0);

  // Connector property copy, cmp, get_cap_flags, and free.
  EXPECT_GE(H5VL_conn_prop_copy(nullptr), 0);
  EXPECT_GE(H5VL_conn_prop_free(nullptr), 0);

  H5VL_connector_prop_t empty_prop{nullptr, nullptr};
  EXPECT_GE(H5VL_conn_prop_copy(&empty_prop), 0);
  EXPECT_GE(H5VL_conn_prop_free(&empty_prop), 0);
  {
    ScopedErrorSilence silence;
    uint64_t caps = 0;
    EXPECT_LT(H5VL_conn_prop_get_cap_flags(&empty_prop, &caps), 0);
  }

  H5VL_connector_t* conn_a =
      H5VL__register_connector_by_class(&kDummyVolClsA, H5P_DEFAULT);
  ASSERT_NE(conn_a, nullptr);
  H5VL_connector_t* conn_b =
      H5VL__register_connector_by_class(&kDummyVolClsB, H5P_DEFAULT);
  ASSERT_NE(conn_b, nullptr);
  absl::Cleanup drop_conns = [&] {
    (void)H5VL_conn_dec_rc(conn_b);
    (void)H5VL_conn_dec_rc(conn_a);
  };

  DummyVolInfo* info1 = AllocDummyInfo(10);
  DummyVolInfo* info2 = AllocDummyInfo(20);
  ASSERT_NE(info1, nullptr);
  ASSERT_NE(info2, nullptr);

  H5VL_connector_prop_t prop1{conn_a, info1};
  H5VL_connector_prop_t prop2{conn_a, info2};
  H5VL_connector_prop_t prop_b{conn_b, nullptr};

  H5VL_conn_inc_rc(conn_a);
  H5VL_conn_inc_rc(conn_a);
  H5VL_conn_inc_rc(conn_b);

  ASSERT_GE(H5VL_conn_prop_cmp(&cmp, &prop1, &prop1), 0);
  EXPECT_EQ(cmp, 0);
  ASSERT_GE(H5VL_conn_prop_cmp(&cmp, &prop1, &prop2), 0);
  EXPECT_LT(cmp, 0);
  ASSERT_GE(H5VL_conn_prop_cmp(&cmp, &prop1, &prop_b), 0);
  EXPECT_NE(cmp, 0);

  uint64_t caps = 0;
  ASSERT_GE(H5VL_conn_prop_get_cap_flags(&prop1, &caps), 0);
  EXPECT_EQ(caps, H5VL_CAP_FLAG_THREADSAFE | H5VL_CAP_FLAG_ASYNC);

  {
    ScopedErrorSilence silence;
    g_hooks.fail_info_cmp = true;
    EXPECT_LT(H5VL_conn_prop_cmp(&cmp, &prop1, &prop2), 0);
    g_hooks.fail_info_cmp = false;

    g_hooks.fail_get_cap_flags = true;
    EXPECT_LT(H5VL_conn_prop_get_cap_flags(&prop1, &caps), 0);
    g_hooks.fail_get_cap_flags = false;
  }

  H5VL_connector_prop_t copied_prop = prop1;
  ASSERT_GE(H5VL_conn_prop_copy(&copied_prop), 0);
  EXPECT_NE(copied_prop.connector_info, prop1.connector_info);

  {
    ScopedErrorSilence silence;
    g_hooks.fail_info_copy = true;
    H5VL_connector_prop_t bad_copy = prop1;
    // H5VL_conn_prop_copy increments conn_a refcount before copying info.
    EXPECT_LT(H5VL_conn_prop_copy(&bad_copy), 0);
    EXPECT_GE(H5VL_conn_dec_rc(conn_a), 0);
    g_hooks.fail_info_copy = false;
  }

  EXPECT_GE(H5VL_conn_prop_free(&copied_prop), 0);
  EXPECT_GE(H5VL_conn_prop_free(&prop_b), 0);
  {
    ScopedErrorSilence silence;
    g_hooks.fail_info_free = true;
    EXPECT_LT(H5VL_conn_prop_free(&prop2), 0);
    g_hooks.fail_info_free = false;
    // fail_info_free skipped H5VL_conn_dec_rc on prop2, so balance it now.
    EXPECT_GE(H5VL_conn_dec_rc(conn_a), 0);
  }
  EXPECT_GE(H5VL_conn_prop_free(&prop1), 0);

  // Plugin match checks.
  H5PL_key_t key{};
  bool success = false;
  key.vol.kind = H5VL_GET_CONNECTOR_BY_NAME;
  key.vol.u.name = kDummyVolClsA.name;
  ASSERT_GE(H5VL_check_plugin_load(&kDummyVolClsA, &key, &success), 0);
  EXPECT_TRUE(success);

  success = false;
  key.vol.u.name = "other_name";
  ASSERT_GE(H5VL_check_plugin_load(&kDummyVolClsA, &key, &success), 0);
  EXPECT_FALSE(success);

  success = false;
  key.vol.kind = H5VL_GET_CONNECTOR_BY_VALUE;
  key.vol.u.value = kDummyVolClsA.value;
  ASSERT_GE(H5VL_check_plugin_load(&kDummyVolClsA, &key, &success), 0);
  EXPECT_TRUE(success);

  success = false;
  key.vol.u.value = kDummyVolClsA.value + 100;
  ASSERT_GE(H5VL_check_plugin_load(&kDummyVolClsA, &key, &success), 0);
  EXPECT_FALSE(success);

  H5VL_class_t wrong_ver = kDummyVolClsA;
  wrong_ver.version = H5VL_VERSION + 2;
  key.vol.u.value = wrong_ver.value;
  success = false;
  ASSERT_GE(H5VL_check_plugin_load(&wrong_ver, &key, &success), 0);
  EXPECT_FALSE(success);
}

TEST(H5VLintTest, ObjectCreationLookupExistingIdIsNativeAndFileIsSame) {
  ASSERT_GE(H5open(), 0);
  ResetHooks();
  absl::Cleanup reset_hooks = [] { ResetHooks(); };

  ScopedFile file1 = CreateInMemoryFile("in_memory_vlint_1.h5");
  ASSERT_TRUE(file1.valid());
  ScopedFile file2 = CreateInMemoryFile("in_memory_vlint_2.h5");
  ASSERT_TRUE(file2.valid());

  ScopedGroup grp(
      H5Gcreate2(file1.get(), "g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file1.get(), "d", H5T_NATIVE_INT, space.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  ScopedType named_dt(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(named_dt.valid());
  ASSERT_GE(H5Tcommit2(file1.get(), "t", named_dt.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  ScopedAttr attr(H5Acreate2(dset.get(), "a", H5T_NATIVE_INT, space.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());
  ScopedType transient_dt(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(transient_dt.valid());

  // Object lookups and verification across all object types.
  EXPECT_NE(H5VL_vol_object(file1.get()), nullptr);
  EXPECT_NE(H5VL_vol_object(grp.get()), nullptr);
  EXPECT_NE(H5VL_vol_object(dset.get()), nullptr);
  EXPECT_NE(H5VL_vol_object(named_dt.get()), nullptr);
  EXPECT_NE(H5VL_vol_object(attr.get()), nullptr);

  EXPECT_NE(H5VL_object_verify(file1.get(), H5I_FILE), nullptr);
  EXPECT_NE(H5VL_object_verify(grp.get(), H5I_GROUP), nullptr);
  EXPECT_NE(H5VL_object_verify(dset.get(), H5I_DATASET), nullptr);
  EXPECT_NE(H5VL_object_verify(named_dt.get(), H5I_DATATYPE), nullptr);
  EXPECT_NE(H5VL_object_verify(attr.get(), H5I_ATTR), nullptr);

  // H5Orefresh invokes H5VL_register_using_existing_id under the hood on
  // read-only files, and H5Topen2 constructs a VOL-backed datatype object.
  ASSERT_GE(H5Orefresh(dset.get()), 0);
  ASSERT_GE(H5Orefresh(grp.get()), 0);
  ASSERT_GE(H5Orefresh(named_dt.get()), 0);

  TempFile ro_tmp("vlint_ro_refresh.h5");
  {
    ScopedFile rw(H5Fcreate(ro_tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                           H5P_DEFAULT));
    ASSERT_TRUE(rw.valid());
    ScopedGroup g(
        H5Gcreate2(rw.get(), "g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
    ScopedDataset d(H5Dcreate2(rw.get(), "d", H5T_NATIVE_INT, space.get(),
                               H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d.valid());
    ScopedType t(H5Tcopy(H5T_NATIVE_INT));
    ASSERT_TRUE(t.valid());
    ASSERT_GE(H5Tcommit2(rw.get(), "t", t.get(), H5P_DEFAULT, H5P_DEFAULT,
                         H5P_DEFAULT),
              0);
  }
  {
    ScopedFile ro_file(H5Fopen(ro_tmp.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(ro_file.valid());
    ScopedGroup ro_grp(H5Gopen2(ro_file.get(), "g", H5P_DEFAULT));
    ASSERT_TRUE(ro_grp.valid());
    ScopedDataset ro_dset(H5Dopen2(ro_file.get(), "d", H5P_DEFAULT));
    ASSERT_TRUE(ro_dset.valid());
    ScopedType ro_dt(H5Topen2(ro_file.get(), "t", H5P_DEFAULT));
    ASSERT_TRUE(ro_dt.valid());
    ASSERT_GE(H5Orefresh(ro_grp.get()), 0);
    ASSERT_GE(H5Orefresh(ro_dset.get()), 0);
    ASSERT_GE(H5Orefresh(ro_dt.get()), 0);
  }

  // Stacked pass-through VOL over native (using an app-visible native VOL ID).
  hid_t native_vol_id = H5VLget_connector_id_by_name("native");
  ASSERT_GE(native_vol_id, 0);
  absl::Cleanup close_native_vol = [&] { (void)H5VLclose(native_vol_id); };

  ScopedPlist pt_fapl = MakeCoreFapl();
  H5VL_pass_through_info_t pt_info{native_vol_id, nullptr};
  ASSERT_GE(H5Pset_vol(pt_fapl.get(), H5VL_PASSTHRU, &pt_info), 0);
  ScopedFile pt_file(H5Fcreate("in_memory_pt_vlint.h5", H5F_ACC_TRUNC,
                               H5P_DEFAULT, pt_fapl.get()));
  ASSERT_TRUE(pt_file.valid());

  // Custom dummy terminal VOL files.
  hid_t dummy_vol_id = H5VLregister_connector(&kDummyVolClsA, H5P_DEFAULT);
  ASSERT_GE(dummy_vol_id, 0);
  absl::Cleanup unreg_dummy = [&] {
    (void)H5VLunregister_connector(dummy_vol_id);
  };

  ScopedPlist dummy_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(dummy_fapl.valid());
  ASSERT_GE(H5Pset_vol(dummy_fapl.get(), dummy_vol_id, nullptr), 0);
  ScopedFile df1(
      H5Fcreate("dummy1", H5F_ACC_TRUNC, H5P_DEFAULT, dummy_fapl.get()));
  ASSERT_TRUE(df1.valid());
  ScopedFile df2(
      H5Fopen("dummy2", H5F_ACC_RDONLY, dummy_fapl.get()));
  ASSERT_TRUE(df2.valid());

  // Internal introspection / file comparison helpers require an active API
  // context in H5CX.
  void* lib_ctx = nullptr;
  ASSERT_GE(H5VL_start_lib_state(&lib_ctx), 0);
  absl::Cleanup finish_lib = [&] {
    if (lib_ctx) (void)H5VL_finish_lib_state(lib_ctx);
  };

  // H5VL_object_is_native and H5VL_file_is_same for native files.
  bool is_native = false;
  ASSERT_GE(H5VL_object_is_native(H5VL_vol_object(file1.get()), &is_native),
            0);
  EXPECT_TRUE(is_native);

  bool same_file = false;
  ASSERT_GE(H5VL_file_is_same(H5VL_vol_object(file1.get()),
                              H5VL_vol_object(file1.get()), &same_file),
            0);
  EXPECT_TRUE(same_file);
  ASSERT_GE(H5VL_file_is_same(H5VL_vol_object(file1.get()),
                              H5VL_vol_object(file2.get()), &same_file),
            0);
  EXPECT_FALSE(same_file);

  // Stacked pass-through VOL over native still reports native terminal class.
  is_native = false;
  ASSERT_GE(H5VL_object_is_native(H5VL_vol_object(pt_file.get()), &is_native),
            0);
  EXPECT_TRUE(is_native);

  // H5VL_object_data extracts the underlying object via get_object callback.
  EXPECT_NE(H5VL_object_data(H5VL_vol_object(pt_file.get())), nullptr);

  // Compare native file against custom dummy terminal VOL files.
  ASSERT_GE(H5VL_object_is_native(H5VL_vol_object(df1.get()), &is_native), 0);
  EXPECT_FALSE(is_native);

  // Different connector classes -> same_file == false.
  same_file = true;
  ASSERT_GE(H5VL_file_is_same(H5VL_vol_object(file1.get()),
                              H5VL_vol_object(df1.get()), &same_file),
            0);
  EXPECT_FALSE(same_file);

  // Same custom connector -> delegates to H5VL_FILE_IS_EQUAL.
  g_hooks.file_is_equal_result = true;
  ASSERT_GE(H5VL_file_is_same(H5VL_vol_object(df1.get()),
                              H5VL_vol_object(df2.get()), &same_file),
            0);
  EXPECT_TRUE(same_file);

  g_hooks.file_is_equal_result = false;
  ASSERT_GE(H5VL_file_is_same(H5VL_vol_object(df1.get()),
                              H5VL_vol_object(df2.get()), &same_file),
            0);
  EXPECT_FALSE(same_file);

  // Internal H5VL_create_object, H5VL_object_inc_rc, and H5VL_free_object.
  H5VL_connector_t* conn_a =
      H5VL__get_connector_by_name(kDummyVolClsA.name);
  ASSERT_NE(conn_a, nullptr);
  absl::Cleanup drop_conn_a = [&] { (void)H5VL_conn_dec_rc(conn_a); };

  int dummy_payload = 123;
  H5VL_object_t* internal_obj = H5VL_create_object(&dummy_payload, conn_a);
  ASSERT_NE(internal_obj, nullptr);
  EXPECT_EQ(H5VL_object_inc_rc(internal_obj), 2u);
  EXPECT_GE(H5VL_free_object(internal_obj), 0);
  EXPECT_GE(H5VL_free_object(internal_obj), 0);

  // H5VL_new_vol_obj with wrap_obj=true and null wrap_ctx returns raw object.
  H5VL_object_t* no_ctx_wrap_vol =
      H5VL_new_vol_obj(H5I_GROUP, &dummy_payload, conn_a, true);
  ASSERT_NE(no_ctx_wrap_vol, nullptr);
  EXPECT_EQ(H5VL_object_data(no_ctx_wrap_vol), &dummy_payload);
  EXPECT_GE(H5VL_free_object(no_ctx_wrap_vol), 0);

  // Create and immediately close IDs to exercise stale-ID lookup paths where
  // H5I_get_type(id) still decodes the type bits but H5I_object(id) is NULL.
  hid_t stale_grp_id = H5Gcreate2(file1.get(), "stale_g", H5P_DEFAULT,
                                  H5P_DEFAULT, H5P_DEFAULT);
  ASSERT_GE(stale_grp_id, 0);
  ASSERT_GE(H5Gclose(stale_grp_id), 0);
  ScopedType stale_dt(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(stale_dt.valid());
  hid_t stale_dt_id = stale_dt.release();
  ASSERT_GE(H5Tclose(stale_dt_id), 0);

  // Error checks for object lookup/creation helpers.
  ScopedErrorSilence silence;
  EXPECT_EQ(H5VL_vol_object(H5I_INVALID_HID), nullptr);
  EXPECT_EQ(H5VL_vol_object(space.get()), nullptr);
  EXPECT_EQ(H5VL_vol_object(transient_dt.get()), nullptr);
  EXPECT_EQ(H5VL_vol_object_verify(file1.get(), H5I_GROUP), nullptr);
  EXPECT_EQ(H5VL_vol_object_verify(space.get(), H5I_DATASPACE), nullptr);

  EXPECT_EQ(H5VL_object(H5I_INVALID_HID), nullptr);
  EXPECT_EQ(H5VL_object(space.get()), nullptr);
  EXPECT_EQ(H5VL_object(transient_dt.get()), nullptr);
  EXPECT_EQ(H5VL_object_verify(file1.get(), H5I_GROUP), nullptr);
  EXPECT_EQ(H5VL_object_verify(space.get(), H5I_FILE), nullptr);
  EXPECT_EQ(H5VL_object_verify(H5I_INVALID_HID, H5I_DATASPACE), nullptr);
  EXPECT_EQ(H5VL_object_verify(stale_grp_id, H5I_GROUP), nullptr);
  EXPECT_EQ(H5VL_object_verify(stale_dt_id, H5I_DATATYPE), nullptr);
  EXPECT_EQ(H5VL_object_verify(transient_dt.get(), H5I_DATATYPE), nullptr);

  EXPECT_EQ(H5VL_register(H5I_DATASPACE, &dummy_payload, conn_a, false),
            H5I_INVALID_HID);
  EXPECT_EQ(H5VL_new_vol_obj(H5I_DATASPACE, &dummy_payload, conn_a, false),
            nullptr);
  EXPECT_EQ(H5VL_new_vol_obj(H5I_DATATYPE, &dummy_payload, conn_a, false),
            nullptr);
  EXPECT_LT(H5VL_register_using_existing_id(H5I_DATASPACE, &dummy_payload,
                                            conn_a, false, file1.get()),
            0);

  g_hooks.fail_get_conn_cls_countdown = 1;
  EXPECT_LT(H5VL_object_is_native(H5VL_vol_object(df1.get()), &is_native), 0);
  g_hooks.fail_get_conn_cls_countdown = 1;
  EXPECT_LT(H5VL_file_is_same(H5VL_vol_object(df1.get()),
                              H5VL_vol_object(df2.get()), &same_file),
            0);
  g_hooks.fail_get_conn_cls_countdown = 2;
  EXPECT_LT(H5VL_file_is_same(H5VL_vol_object(df1.get()),
                              H5VL_vol_object(df2.get()), &same_file),
            0);
  g_hooks.fail_get_conn_cls_countdown = 0;

  g_hooks.fail_file_is_equal = true;
  EXPECT_LT(H5VL_file_is_same(H5VL_vol_object(df1.get()),
                              H5VL_vol_object(df2.get()), &same_file),
            0);
  g_hooks.fail_file_is_equal = false;
}

TEST(H5VLintTest, WrapContextLifecycleAndErrorPaths) {
  ASSERT_GE(H5open(), 0);
  ResetHooks();
  absl::Cleanup reset_hooks = [] { ResetHooks(); };

  // Also trigger wrapper paths publicly via H5Ovisit3 under pass-through VOL.
  hid_t native_vol_id = H5VLget_connector_id_by_name("native");
  ASSERT_GE(native_vol_id, 0);
  absl::Cleanup close_native_vol = [&] { (void)H5VLclose(native_vol_id); };

  ScopedPlist pt_fapl = MakeCoreFapl();
  H5VL_pass_through_info_t pt_info{native_vol_id, nullptr};
  ASSERT_GE(H5Pset_vol(pt_fapl.get(), H5VL_PASSTHRU, &pt_info), 0);
  ScopedFile pt_file(H5Fcreate("in_memory_pt_visit.h5", H5F_ACC_TRUNC,
                               H5P_DEFAULT, pt_fapl.get()));
  ASSERT_TRUE(pt_file.valid());
  ScopedGroup pt_grp(
      H5Gcreate2(pt_file.get(), "sub", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(pt_grp.valid());
  pt_grp = ScopedGroup{};

  int visited = 0;
  auto visit_cb = [](hid_t /*obj*/, const char* /*name*/,
                     const H5O_info2_t* /*info*/, void* op_data) -> herr_t {
    ++(*static_cast<int*>(op_data));
    return 0;
  };
  ASSERT_GE(H5Ovisit3(pt_file.get(), H5_INDEX_NAME, H5_ITER_NATIVE, visit_cb,
                      &visited, H5O_INFO_BASIC),
            0);
  EXPECT_GE(visited, 2);

  // Create committed datatype to test already-VOL-managed guard on native.
  ScopedFile nat_file = CreateInMemoryFile("in_memory_wrap_nat.h5");
  ASSERT_TRUE(nat_file.valid());
  ScopedType named_dt(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(named_dt.valid());
  ASSERT_GE(H5Tcommit2(nat_file.get(), "named", named_dt.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Set up custom terminal + wrapper connectors.
  hid_t wrap_vol_id = H5VLregister_connector(&kDummyVolClsB, H5P_DEFAULT);
  ASSERT_GE(wrap_vol_id, 0);
  absl::Cleanup unreg_wrap = [&] { (void)H5VLunregister_connector(wrap_vol_id); };

  ScopedPlist wrap_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(wrap_fapl.valid());
  ASSERT_GE(H5Pset_vol(wrap_fapl.get(), wrap_vol_id, nullptr), 0);
  ScopedFile wrap_file(
      H5Fcreate("wrap_f", H5F_ACC_TRUNC, H5P_DEFAULT, wrap_fapl.get()));
  ASSERT_TRUE(wrap_file.valid());
  H5VL_object_t* wrap_vol_obj = H5VL_vol_object(wrap_file.get());
  ASSERT_NE(wrap_vol_obj, nullptr);

  H5VL_connector_t* wrap_conn =
      H5VL__get_connector_by_name(kDummyVolClsB.name);
  ASSERT_NE(wrap_conn, nullptr);
  absl::Cleanup drop_wrap_conn = [&] { (void)H5VL_conn_dec_rc(wrap_conn); };

  // Calling wrap helpers with no active H5CX context exercises the
  // H5CX_get_vol_wrap_ctx < 0 error paths cleanly.
  {
    ScopedErrorSilence silence;
    int pre_payload = 42;
    EXPECT_LT(H5VL_set_vol_wrapper(wrap_vol_obj), 0);
    EXPECT_LT(H5VL_reset_vol_wrapper(), 0);
    EXPECT_EQ(H5VL_wrap_register(H5I_GROUP, &pre_payload, true),
              H5I_INVALID_HID);
    EXPECT_EQ(H5VL_new_vol_obj(H5I_GROUP, &pre_payload, wrap_conn, true),
              nullptr);
  }

  // Explicit library API context for direct wrapper ctx testing.
  void* lib_ctx = nullptr;
  ASSERT_GE(H5VL_start_lib_state(&lib_ctx), 0);
  absl::Cleanup finish_ctx = [&] {
    void* active_ctx = nullptr;
    while (H5CX_get_vol_wrap_ctx(&active_ctx) >= 0 && active_ctx != nullptr) {
      if (H5VL_reset_vol_wrapper() < 0) break;
    }
    if (lib_ctx) {
      (void)H5VL_finish_lib_state(lib_ctx);
    }
  };

  {
    ScopedErrorSilence silence;
    int dummy_item = 1;
    EXPECT_LT(H5VL_inc_vol_wrapper(nullptr), 0);
    EXPECT_LT(H5VL_dec_vol_wrapper(nullptr), 0);
    EXPECT_LT(H5VL_reset_vol_wrapper(), 0);
    EXPECT_EQ(H5VL_wrap_register(H5I_GROUP, &dummy_item, true),
              H5I_INVALID_HID);

    g_hooks.fail_get_wrap_ctx = true;
    EXPECT_LT(H5VL_set_vol_wrapper(wrap_vol_obj), 0);
    g_hooks.fail_get_wrap_ctx = false;
  }

  // Create and stack wrapper context (rc goes 1 -> 2 -> 3 -> 2 -> 1 -> 0).
  ASSERT_GE(H5VL_set_vol_wrapper(wrap_vol_obj), 0);
  ASSERT_GE(H5VL_set_vol_wrapper(wrap_vol_obj), 0);

  void* raw_wrap_ctx = nullptr;
  ASSERT_GE(H5CX_get_vol_wrap_ctx(&raw_wrap_ctx), 0);
  ASSERT_NE(raw_wrap_ctx, nullptr);

  ASSERT_GE(H5VL_inc_vol_wrapper(raw_wrap_ctx), 0);
  ASSERT_GE(H5VL_dec_vol_wrapper(raw_wrap_ctx), 0);

  // Wrapping succeeds for an underlying object; closing unwraps/frees cleanly.
  int payload = 55;
  H5VL_object_t* wrapped_vol =
      H5VL_new_vol_obj(H5I_GROUP, &payload, wrap_conn, true);
  ASSERT_NE(wrapped_vol, nullptr);
  EXPECT_EQ(H5VL_object_unwrap(wrapped_vol), &payload);
  EXPECT_GE(H5VL_free_object(wrapped_vol), 0);

  // H5VL_wrap_register succeeds and closing the ID invokes DummyGroupClose.
  ScopedGroup wrapped_grp(H5VL_wrap_register(H5I_GROUP, &payload, true));
  ASSERT_TRUE(wrapped_grp.valid());
  wrapped_grp.reset();

  // H5VL_new_vol_obj(H5I_DATATYPE, ..., wrap_obj=true) cleanup on datatype fail.
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5VL_new_vol_obj(H5I_DATATYPE, &payload, wrap_conn, true),
              nullptr);

    g_hooks.fail_wrap_object = true;
    EXPECT_EQ(H5VL_new_vol_obj(H5I_GROUP, &payload, wrap_conn, true), nullptr);
    EXPECT_EQ(H5VL_wrap_register(H5I_GROUP, &payload, true), H5I_INVALID_HID);
    g_hooks.fail_wrap_object = false;

    // Wrap returns raw stack pointer so H5VL_register failure on datatype
    // exercises H5VL_wrap_register's register failure path without leaking.
    g_hooks.wrap_returns_raw_obj = true;
    EXPECT_EQ(H5VL_wrap_register(H5I_DATATYPE, &payload, true),
              H5I_INVALID_HID);
    g_hooks.wrap_returns_raw_obj = false;

    // Failing get_object on wrapper triggers H5VL_file_is_same's unwrap error.
    g_hooks.fail_get_object = true;
    bool same_file = false;
    EXPECT_LT(H5VL_file_is_same(wrap_vol_obj, wrap_vol_obj, &same_file), 0);
    g_hooks.fail_get_object = false;

    H5VL_object_t* temp_vol = H5VL_create_object(&payload, wrap_conn);
    ASSERT_NE(temp_vol, nullptr);
    g_hooks.fail_unwrap_object = true;
    EXPECT_EQ(H5VL_object_unwrap(temp_vol), nullptr);
    g_hooks.fail_unwrap_object = false;
    EXPECT_GE(H5VL_free_object(temp_vol), 0);
  }

  // Drop remaining wrapper rc via H5VL_reset_vol_wrapper (rc=2 -> 1 -> 0).
  ASSERT_GE(H5VL_reset_vol_wrapper(), 0);
  ASSERT_GE(H5VL_reset_vol_wrapper(), 0);
  raw_wrap_ctx = nullptr;

  // Test H5VL_dec_vol_wrapper dropping rc to 0 cleanly when held directly.
  ASSERT_GE(H5VL_set_vol_wrapper(wrap_vol_obj), 0);
  ASSERT_GE(H5CX_get_vol_wrap_ctx(&raw_wrap_ctx), 0);
  ASSERT_NE(raw_wrap_ctx, nullptr);
  ASSERT_GE(H5CX_set_vol_wrap_ctx(nullptr), 0);
  ASSERT_GE(H5VL_dec_vol_wrapper(raw_wrap_ctx), 0);

  // Test H5VL_wrap_register guard for already-VOL-managed native datatype.
  ASSERT_GE(H5VL_set_vol_wrapper(H5VL_vol_object(nat_file.get())), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(
        H5VL_wrap_register(H5I_DATATYPE, H5I_object(named_dt.get()), true),
        H5I_INVALID_HID);
  }
  ASSERT_GE(H5VL_reset_vol_wrapper(), 0);
}

TEST(H5VLintTest, LibStateSaveRestoreAndSetupArgHelpers) {
  ASSERT_GE(H5open(), 0);
  ResetHooks();
  absl::Cleanup reset_hooks = [] { ResetHooks(); };

  ScopedFile file = CreateInMemoryFile("in_memory_setup_args.h5");
  ASSERT_TRUE(file.valid());
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "dset", H5T_NATIVE_INT, space.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  ScopedAttr attr(H5Acreate2(dset.get(), "attr", H5T_NATIVE_INT, space.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());
  ScopedPlist custom_lapl(H5Pcreate(H5P_LINK_ACCESS));
  ASSERT_TRUE(custom_lapl.valid());

  // Library state start / retrieve / restore / free / finish.
  // H5VL_retrieve_lib_state requires an active API context on the thread stack.
  void* started_ctx = nullptr;
  ASSERT_GE(H5VL_start_lib_state(&started_ctx), 0);
  ASSERT_NE(started_ctx, nullptr);
  absl::Cleanup finish_started = [&] {
    if (started_ctx) (void)H5VL_finish_lib_state(started_ctx);
  };

  void* saved_state = nullptr;
  ASSERT_GE(H5VL_retrieve_lib_state(&saved_state), 0);
  ASSERT_NE(saved_state, nullptr);
  absl::Cleanup free_state = [&] {
    if (saved_state) (void)H5VL_free_lib_state(saved_state);
  };

  ASSERT_GE(H5VL_restore_lib_state(saved_state), 0);

  // H5VL_retrieve_lib_state fails when copying connector info fails in H5CX.
  {
    H5VL_connector_t* conn_a =
        H5VL__register_connector_by_class(&kDummyVolClsA, H5P_DEFAULT);
    ASSERT_NE(conn_a, nullptr);
    DummyVolInfo* cx_info = AllocDummyInfo(7);
    ASSERT_NE(cx_info, nullptr);
    // H5CX_free_state decrements api_state->vol_connector_prop.connector rc.
    H5VL_conn_inc_rc(conn_a);
    H5VL_connector_prop_t cx_prop{conn_a, cx_info};
    ASSERT_GE(H5CX_set_vol_connector_prop(&cx_prop), 0);

    ScopedErrorSilence silence;
    g_hooks.fail_info_copy = true;
    void* err_state = nullptr;
    EXPECT_LT(H5VL_retrieve_lib_state(&err_state), 0);
    EXPECT_EQ(err_state, nullptr);
    g_hooks.fail_info_copy = false;

    H5VL_connector_prop_t null_prop{nullptr, nullptr};
    ASSERT_GE(H5CX_set_vol_connector_prop(&null_prop), 0);
    EXPECT_EQ(H5VL_conn_dec_rc(conn_a), 0);
    EXPECT_TRUE(g_live_infos.empty());
  }

  // Argument setup helpers: happy paths.
  H5VL_object_t* vol_obj = nullptr;
  H5VL_loc_params_t loc_params{};
  EXPECT_GE(H5VL_setup_args(attr.get(), H5I_ATTR, &vol_obj), 0);
  EXPECT_NE(vol_obj, nullptr);

  EXPECT_GE(H5VL_setup_loc_args(file.get(), &vol_obj, &loc_params), 0);
  EXPECT_EQ(loc_params.type, H5VL_OBJECT_BY_SELF);
  EXPECT_EQ(loc_params.obj_type, H5I_FILE);

  hid_t lapl = H5P_DEFAULT;
  EXPECT_GE(H5VL_setup_acc_args(dset.get(), H5P_CLS_LACC, false, &lapl,
                                &vol_obj, &loc_params),
            0);
  EXPECT_NE(lapl, H5I_INVALID_HID);

  hid_t custom_lapl_id = custom_lapl.get();
  EXPECT_GE(H5VL_setup_acc_args(dset.get(), H5P_CLS_LACC, false,
                                &custom_lapl_id, &vol_obj, &loc_params),
            0);

  EXPECT_GE(H5VL_setup_self_args(dset.get(), &vol_obj, &loc_params), 0);
  EXPECT_EQ(loc_params.type, H5VL_OBJECT_BY_SELF);
  EXPECT_EQ(loc_params.obj_type, H5I_DATASET);

  EXPECT_GE(H5VL_setup_name_args(file.get(), "dset", false, custom_lapl.get(),
                                 &vol_obj, &loc_params),
            0);
  EXPECT_EQ(loc_params.type, H5VL_OBJECT_BY_NAME);

  EXPECT_GE(H5VL_setup_idx_args(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                                false, H5P_DEFAULT, &vol_obj, &loc_params),
            0);
  EXPECT_EQ(loc_params.type, H5VL_OBJECT_BY_IDX);

  H5O_token_t token{};
  EXPECT_GE(H5VL_setup_token_args(file.get(), &token, &vol_obj, &loc_params),
            0);
  EXPECT_EQ(loc_params.type, H5VL_OBJECT_BY_TOKEN);

  // Argument setup helpers: error paths.
  ScopedErrorSilence silence;
  EXPECT_LT(H5VL_setup_args(file.get(), H5I_ATTR, &vol_obj), 0);
  EXPECT_LT(H5VL_setup_args(attr.get(), H5I_FILE, &vol_obj), 0);

  EXPECT_LT(H5VL_setup_loc_args(space.get(), &vol_obj, &loc_params), 0);

  hid_t def_apl = H5P_DEFAULT;
  EXPECT_LT(H5VL_setup_acc_args(space.get(), H5P_CLS_LACC, false, &def_apl,
                                &vol_obj, &loc_params),
            0);

  EXPECT_LT(H5VL_setup_self_args(space.get(), &vol_obj, &loc_params), 0);

  EXPECT_LT(H5VL_setup_name_args(file.get(), nullptr, false, H5P_DEFAULT,
                                 &vol_obj, &loc_params),
            0);
  EXPECT_LT(H5VL_setup_name_args(file.get(), "", false, H5P_DEFAULT, &vol_obj,
                                 &loc_params),
            0);
  EXPECT_LT(H5VL_setup_name_args(space.get(), "dset", false, H5P_DEFAULT,
                                 &vol_obj, &loc_params),
            0);

  EXPECT_LT(H5VL_setup_idx_args(file.get(), nullptr, H5_INDEX_NAME, H5_ITER_INC,
                                0, false, H5P_DEFAULT, &vol_obj, &loc_params),
            0);
  EXPECT_LT(H5VL_setup_idx_args(file.get(), "", H5_INDEX_NAME, H5_ITER_INC, 0,
                                false, H5P_DEFAULT, &vol_obj, &loc_params),
            0);
  EXPECT_LT(H5VL_setup_idx_args(file.get(), ".", H5_INDEX_UNKNOWN, H5_ITER_INC,
                                0, false, H5P_DEFAULT, &vol_obj, &loc_params),
            0);
  EXPECT_LT(H5VL_setup_idx_args(file.get(), ".", H5_INDEX_N, H5_ITER_INC, 0,
                                false, H5P_DEFAULT, &vol_obj, &loc_params),
            0);
  EXPECT_LT(H5VL_setup_idx_args(file.get(), ".", H5_INDEX_NAME, H5_ITER_UNKNOWN,
                                0, false, H5P_DEFAULT, &vol_obj, &loc_params),
            0);
  EXPECT_LT(H5VL_setup_idx_args(file.get(), ".", H5_INDEX_NAME, H5_ITER_N, 0,
                                false, H5P_DEFAULT, &vol_obj, &loc_params),
            0);
  EXPECT_LT(H5VL_setup_idx_args(space.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                                false, H5P_DEFAULT, &vol_obj, &loc_params),
            0);

  EXPECT_LT(H5VL_setup_token_args(space.get(), &token, &vol_obj, &loc_params),
            0);
}

}  // namespace
}  // namespace hdf5
