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
// Tests internal HDF5 error handling routines: package initialization/
// termination, user callback state save/restore, class/message/stack object
// management, stack pushing/copying/appending/clearing/truncation, walking,
// printing, auto stack dumping, and pause/resume
// (third_party/hdf5/src/H5Eint.c). Note: google3 builds with -DNDEBUG,
// H5_HAVE_THREADSAFE_API, and deprecated v1 error symbols enabled.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Eint.c

#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` compiles with -DNDEBUG; match internal struct layouts across
// H5private.h -> H5CXprivate.h -> H5ACprivate.h -> H5Cprivate.h.
#ifndef NDEBUG
#define H5EINT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5E_FRIEND
#include "third_party/hdf5/src/H5Epkg.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5MMprivate.h"
}  // extern "C"
#ifdef H5EINT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5EINT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

using ScopedErrorClass = ScopedH5Id<H5Eunregister_class>;
using ScopedErrorMsg = ScopedH5Id<H5Eclose_msg>;
using ScopedErrorStack = ScopedH5Id<H5Eclose_stack>;

class ScopedDefaultAutoRestore {
 public:
  ScopedDefaultAutoRestore() {
    saved_ = (H5Eget_auto2(H5E_DEFAULT, &prev_func_, &prev_data_) >= 0);
    H5E_clear_stack();
  }

  ~ScopedDefaultAutoRestore() {
    H5E_clear_stack();
    if (saved_) {
      H5Eset_auto2(H5E_DEFAULT, prev_func_, prev_data_);
    }
  }

  ScopedDefaultAutoRestore(const ScopedDefaultAutoRestore&) = delete;
  ScopedDefaultAutoRestore& operator=(const ScopedDefaultAutoRestore&) = delete;

 private:
  H5E_auto2_t prev_func_ = nullptr;
  void* prev_data_ = nullptr;
  bool saved_ = false;
};

herr_t PushFormattedHelper(H5E_stack_t* estack, bool app_entry,
                           const char* file, const char* func, unsigned line,
                           hid_t cls_id, hid_t maj_id, hid_t min_id,
                           const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const herr_t rc = H5E__push_stack(estack, app_entry, file, func, line, cls_id,
                                    maj_id, min_id, fmt, &ap);
  va_end(ap);
  return rc;
}

TEST(H5EintTest, InitTermAndUserCbStatePrepareRestore) {
  ASSERT_GE(H5E_init(), 0);

  // 1. User callback state save/restore in v2 mode.
  ScopedDefaultAutoRestore restore;
  H5E_auto2_t default_fn = nullptr;
  ASSERT_GE(H5E_get_default_auto_func(&default_fn), 0);
  EXPECT_NE(default_fn, nullptr);

  int tag2 = 123;
  auto custom_v2 = [](hid_t, void*) -> herr_t { return 0; };
  ASSERT_GE(H5Eset_auto2(H5E_DEFAULT, custom_v2, &tag2), 0);

  H5E_user_cb_state_t cb_state2{};
  ASSERT_GE(H5E_user_cb_prepare(&cb_state2), 0);
  ASSERT_GE(H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr), 0);
  ASSERT_GE(H5E_user_cb_restore(&cb_state2), 0);

  H5E_auto2_t restored_v2 = nullptr;
  void* restored_data = nullptr;
  ASSERT_GE(H5Eget_auto2(H5E_DEFAULT, &restored_v2, &restored_data), 0);
  EXPECT_EQ(restored_v2, static_cast<H5E_auto2_t>(custom_v2));
  EXPECT_EQ(restored_data, &tag2);

  // 2. User callback state save/restore in v1 mode.
  int tag1 = 456;
  auto custom_v1 = [](void*) -> herr_t { return 0; };
  ASSERT_GE(H5Eset_auto1(custom_v1, &tag1), 0);

  H5E_user_cb_state_t cb_state1{};
  ASSERT_GE(H5E_user_cb_prepare(&cb_state1), 0);
  ASSERT_GE(H5Eset_auto1(nullptr, nullptr), 0);
  ASSERT_GE(H5E_user_cb_restore(&cb_state1), 0);

  H5E_auto1_t restored_v1 = nullptr;
  ASSERT_GE(H5Eget_auto1(&restored_v1, &restored_data), 0);
  EXPECT_EQ(restored_v1, static_cast<H5E_auto1_t>(custom_v1));
  EXPECT_EQ(restored_data, &tag1);
}

TEST(H5EintTest, TermPackageCleansOpenErrorObjectsAndReopens) {
  ASSERT_GE(H5open(), 0);
  absl::Cleanup ensure_reopen = [] { H5open(); };

  // Leave an open class, message, stack, and default-stack entry when closing
  // the library so H5E_term_package exercises non-empty and empty cleanup.
  const hid_t cls = H5Eregister_class("TermCls", "TermLib", "1.0");
  ASSERT_NE(H5I_INVALID_HID, cls);
  const hid_t maj = H5Ecreate_msg(cls, H5E_MAJOR, "TermMaj");
  ASSERT_NE(H5I_INVALID_HID, maj);
  const hid_t stk = H5Ecreate_stack();
  ASSERT_NE(H5I_INVALID_HID, stk);
  ASSERT_GE(H5E_printf_stack("term.c", "term_fn", 1, H5E_ARGS_g, H5E_BADVALUE_g,
                             "residual %d", 9),
            0);

  ASSERT_GE(H5close(), 0);
  // Calling H5E_term_package after shutdown returns 0 safely.
  EXPECT_EQ(H5E_term_package(), 0);

  ASSERT_GE(H5open(), 0);
  // After reopen, pre-close IDs are no longer registered.
  EXPECT_LT(H5Eclose_stack(stk), 0);
  EXPECT_LT(H5Eclose_msg(maj), 0);
  EXPECT_LT(H5Eunregister_class(cls), 0);
}

TEST(H5EintTest, InternalPushStackTruncationFallbacksAndCopy) {
  ScopedErrorSilence silence;
  absl::Cleanup clear_default = [] { H5E_clear_stack(); };

  // 1. H5E__push_stack with NULL file, func, fmt, and ap triggers all
  //    default string fallbacks ("Unknown_File", "Unknown_Function",
  //    "No description given") without allocations beyond desc.
  H5E_stack_t* def_stk = H5E__get_my_stack();
  ASSERT_NE(def_stk, nullptr);

  ASSERT_GE(H5E__push_stack(def_stk, false, nullptr, nullptr, 12, H5E_ERR_CLS_g,
                            H5E_ARGS_g, H5E_BADVALUE_g, nullptr, nullptr),
            0);
  ASSERT_EQ(H5E__get_num(def_stk), 1);
  EXPECT_STREQ("Unknown_File", def_stk->entries[0].err.file_name);
  EXPECT_STREQ("Unknown_Function", def_stk->entries[0].err.func_name);
  EXPECT_STREQ("No description given", def_stk->entries[0].err.desc);

  // 2. Push va_list formatted entry and verify H5E_printf_stack works and
  //    respects paused state.
  ASSERT_GE(
      PushFormattedHelper(def_stk, false, "fmt.c", "fmt_fn", 34, H5E_ERR_CLS_g,
                          H5E_FILE_g, H5E_CANTOPENFILE_g, "code=%d", 77),
      0);
  EXPECT_EQ(H5E__get_num(def_stk), 2);
  EXPECT_STREQ("code=77", def_stk->entries[1].err.desc);

  H5E_pause_stack();
  EXPECT_GE(H5E_printf_stack("skip.c", "skip_fn", 55, H5E_ARGS_g,
                             H5E_BADVALUE_g, "skipped"),
            0);
  EXPECT_EQ(H5E__get_num(def_stk), 2);
  H5E_resume_stack();

  // 3. Fill beyond H5E_MAX_ENTRIES (32); extra pushes are silently ignored.
  for (int i = 0; i < H5E_MAX_ENTRIES + 5; ++i) {
    ASSERT_GE(H5E_printf_stack("fill.c", "fill_fn", static_cast<unsigned>(i),
                               H5E_ARGS_g, H5E_BADVALUE_g, "entry %d", i),
              0);
  }
  EXPECT_EQ(H5E__get_num(def_stk), H5E_MAX_ENTRIES);

  // 4. H5E__destroy_stack(nullptr) clears default stack; non-null clears
  //    the specified stack; H5E__pop removes a subset.
  ASSERT_GE(H5E__pop(def_stk, 5), 0);
  EXPECT_EQ(H5E__get_num(def_stk), H5E_MAX_ENTRIES - 5);
  ASSERT_GE(H5E__destroy_stack(def_stk), 0);
  EXPECT_EQ(H5E__get_num(def_stk), 0);

  ASSERT_GE(H5E_printf_stack("d.c", "fd", 1, H5E_ARGS_g, H5E_BADVALUE_g, "x"),
            0);
  ASSERT_GE(H5E__destroy_stack(nullptr), 0);
  EXPECT_EQ(H5E__get_num(def_stk), 0);
}

TEST(H5EintTest, InternalAppendStackTruncationAndCopyAppEntries) {
  ScopedErrorSilence silence;

  ScopedErrorClass cls(H5Eregister_class("IntCls", "IntLib", "1.0"));
  ASSERT_NE(H5I_INVALID_HID, cls.get());
  ScopedErrorMsg maj(H5Ecreate_msg(cls.get(), H5E_MAJOR, "Int maj"));
  ScopedErrorMsg min(H5Ecreate_msg(cls.get(), H5E_MINOR, "Int min"));
  ASSERT_NE(H5I_INVALID_HID, maj.get());
  ASSERT_NE(H5I_INVALID_HID, min.get());

  absl::Cleanup clear_default = [] { H5E_clear_stack(); };

  // 1. Mix an app entry (with custom class/msg) and an internal entry on
  //    default stack, then get/set current stack to exercise
  //    H5E__copy_stack_entry on both entry kinds.
  ASSERT_GE(H5Epush2(H5E_DEFAULT, "app.c", "app_fn", 10, cls.get(), maj.get(),
                     min.get(), "app desc"),
            0);
  ASSERT_GE(H5E_printf_stack("lib.c", "lib_fn", 20, H5E_ARGS_g, H5E_BADVALUE_g,
                             "lib desc"),
            0);

  H5E_stack_t* copied = H5E__get_current_stack();
  ASSERT_NE(copied, nullptr);
  EXPECT_EQ(H5E__get_num(H5E__get_my_stack()), 0);
  EXPECT_EQ(H5E__get_num(copied), 2);

  ASSERT_GE(H5E__set_current_stack(copied), 0);
  EXPECT_EQ(H5Eget_num(H5E_DEFAULT), 2);
  // Register and close `copied` cleanly via H5I_register + H5Eclose_stack
  // (which exercises H5E__close_stack internally and clears H5E_DEFAULT).
  const hid_t copied_id = H5I_register(H5I_ERROR_STACK, copied, true);
  ASSERT_NE(H5I_INVALID_HID, copied_id);
  EXPECT_GE(H5Eclose_stack(copied_id), 0);
  EXPECT_GE(H5E_clear_stack(), 0);

  // 2. Append 20 entries onto a stack that already has 20 entries so
  //    H5E__append_stack hits the H5E_MAX_ENTRIES (32) truncation break.
  ScopedErrorStack dst_id(H5Ecreate_stack());
  ScopedErrorStack src_id(H5Ecreate_stack());
  ASSERT_NE(H5I_INVALID_HID, dst_id.get());
  ASSERT_NE(H5I_INVALID_HID, src_id.get());

  auto* dst_stk = static_cast<H5E_stack_t*>(
      H5I_object_verify(dst_id.get(), H5I_ERROR_STACK));
  auto* src_stk = static_cast<H5E_stack_t*>(
      H5I_object_verify(src_id.get(), H5I_ERROR_STACK));
  ASSERT_NE(dst_stk, nullptr);
  ASSERT_NE(src_stk, nullptr);

  for (int i = 0; i < 20; ++i) {
    ASSERT_GE(H5E__push_stack(dst_stk, false, "d.c", "fd",
                              static_cast<unsigned>(i), H5E_ERR_CLS_g,
                              H5E_ARGS_g, H5E_BADVALUE_g, "d", nullptr),
              0);
    ASSERT_GE(H5E__push_stack(src_stk, false, "s.c", "fs",
                              static_cast<unsigned>(i), H5E_ERR_CLS_g,
                              H5E_ARGS_g, H5E_BADVALUE_g, "s", nullptr),
              0);
  }
  ASSERT_GE(H5E__append_stack(dst_stk, src_stk), 0);
  EXPECT_EQ(H5E__get_num(dst_stk), H5E_MAX_ENTRIES);
}

TEST(H5EintTest, UnregisterClassLeavesUnrelatedClassMessagesOpen) {
  ScopedErrorSilence silence;

  ScopedErrorClass cls_a(H5Eregister_class("ClsA", "LibA", "1.0"));
  ScopedErrorClass cls_b(H5Eregister_class("ClsB", "LibB", "1.0"));
  ASSERT_NE(H5I_INVALID_HID, cls_a.get());
  ASSERT_NE(H5I_INVALID_HID, cls_b.get());

  const hid_t msg_a = H5Ecreate_msg(cls_a.get(), H5E_MAJOR, "MsgA");
  ScopedErrorMsg msg_b(H5Ecreate_msg(cls_b.get(), H5E_MAJOR, "MsgB"));
  ASSERT_NE(H5I_INVALID_HID, msg_a);
  ASSERT_NE(H5I_INVALID_HID, msg_b.get());

  // Unregistering cls_a iterates all H5I_ERROR_MSG objects, removing msg_a
  // while leaving msg_b intact.
  ASSERT_GE(H5Eunregister_class(cls_a.release()), 0);
  EXPECT_LT(H5Eclose_msg(msg_a), 0);

  std::array<char, 16> buf{};
  EXPECT_EQ(H5Eget_msg(msg_b.get(), nullptr, buf.data(), buf.size()), 4);
  EXPECT_STREQ("MsgB", buf.data());
}

TEST(H5EintTest, WalkV1V2PrintAndDumpApiStack) {
  ScopedDefaultAutoRestore restore;

  ScopedErrorClass cls1(H5Eregister_class("PrintCls1", "PrintLib1", "1.0"));
  ScopedErrorClass cls2(H5Eregister_class("PrintCls2", "PrintLib2", "2.0"));
  ASSERT_NE(H5I_INVALID_HID, cls1.get());
  ASSERT_NE(H5I_INVALID_HID, cls2.get());
  ScopedErrorMsg maj1(H5Ecreate_msg(cls1.get(), H5E_MAJOR, "Maj1"));
  ScopedErrorMsg min1(H5Ecreate_msg(cls1.get(), H5E_MINOR, "Min1"));
  ScopedErrorMsg maj2(H5Ecreate_msg(cls2.get(), H5E_MAJOR, "Maj2"));
  ScopedErrorMsg min2(H5Ecreate_msg(cls2.get(), H5E_MINOR, "Min2"));
  ASSERT_NE(H5I_INVALID_HID, maj1.get());
  ASSERT_NE(H5I_INVALID_HID, min1.get());
  ASSERT_NE(H5I_INVALID_HID, maj2.get());
  ASSERT_NE(H5I_INVALID_HID, min2.get());

  absl::Cleanup clear_default = [] { H5E_clear_stack(); };

  // 1. Exercise H5E_dump_api_stack with v1 (null & non-null) and v2 (null &
  //    non-null) auto handlers.
  int v1_calls = 0;
  auto v1_cb = [](void* data) -> herr_t {
    ++(*static_cast<int*>(data));
    return 0;
  };
  ASSERT_GE(H5Eset_auto1(v1_cb, &v1_calls), 0);
  ASSERT_GE(H5E_dump_api_stack(), 0);
  EXPECT_EQ(v1_calls, 1);

  ASSERT_GE(H5Eset_auto1(nullptr, nullptr), 0);
  ASSERT_GE(H5E_dump_api_stack(), 0);
  EXPECT_EQ(v1_calls, 1);

  int v2_calls = 0;
  auto v2_cb = [](hid_t, void* data) -> herr_t {
    ++(*static_cast<int*>(data));
    return 0;
  };
  ASSERT_GE(H5Eset_auto2(H5E_DEFAULT, v2_cb, &v2_calls), 0);
  ASSERT_GE(H5E_dump_api_stack(), 0);
  EXPECT_EQ(v2_calls, 1);

  ASSERT_GE(H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr), 0);
  ASSERT_GE(H5E_dump_api_stack(), 0);
  EXPECT_EQ(v2_calls, 1);

  // 2. Exercise H5E__walk with v1/v2, upward/downward, invalid direction, null
  //    callback, and negative callback return.
  ScopedErrorStack stk_id(H5Ecreate_stack());
  ASSERT_NE(H5I_INVALID_HID, stk_id.get());
  auto* stk = static_cast<H5E_stack_t*>(
      H5I_object_verify(stk_id.get(), H5I_ERROR_STACK));
  ASSERT_NE(stk, nullptr);

  ASSERT_GE(H5Epush2(stk_id.get(), "p1.c", "fp1", 1, cls1.get(), maj1.get(),
                     min1.get(), "first"),
            0);
  ASSERT_GE(H5Epush2(stk_id.get(), "p2.c", "fp2", 2, cls2.get(), maj2.get(),
                     min2.get(), ""),
            0);

  H5E_walk_op_t op{};
  op.vers = 1;
  op.u.func1 = nullptr;
  EXPECT_GE(H5E__walk(stk, H5E_WALK_UPWARD, &op, nullptr), 0);

  int seen_v1 = 0;
  op.u.func1 = [](int, H5E_error1_t*, void* d) -> herr_t {
    ++(*static_cast<int*>(d));
    return 0;
  };
  EXPECT_GE(H5E__walk(stk, H5E_WALK_UPWARD, &op, &seen_v1), 0);
  EXPECT_EQ(seen_v1, 2);
  EXPECT_GE(H5E__walk(stk, H5E_WALK_DOWNWARD, &op, &seen_v1), 0);
  EXPECT_EQ(seen_v1, 4);

  op.u.func1 = [](int, H5E_error1_t*, void*) -> herr_t { return -1; };
  EXPECT_LT(H5E__walk(stk, H5E_WALK_UPWARD, &op, nullptr), 0);

  op.vers = 2;
  op.u.func2 = nullptr;
  EXPECT_GE(H5E__walk(stk, static_cast<H5E_direction_t>(42), &op, nullptr), 0);

  int seen_v2 = 0;
  op.u.func2 = [](unsigned, const H5E_error2_t*, void* d) -> herr_t {
    ++(*static_cast<int*>(d));
    return 0;
  };
  EXPECT_GE(H5E__walk(stk, H5E_WALK_UPWARD, &op, &seen_v2), 0);
  EXPECT_EQ(seen_v2, 2);
  EXPECT_GE(H5E__walk(stk, H5E_WALK_DOWNWARD, &op, &seen_v2), 0);
  EXPECT_EQ(seen_v2, 4);

  op.u.func2 = [](unsigned, const H5E_error2_t*, void* d) -> herr_t {
    ++(*static_cast<int*>(d));
    return 1;
  };
  EXPECT_GT(H5E__walk(stk, H5E_WALK_UPWARD, &op, &seen_v2), 0);
  EXPECT_EQ(seen_v2, 5);

  op.u.func2 = [](unsigned, const H5E_error2_t*, void*) -> herr_t {
    return -1;
  };
  EXPECT_LT(H5E__walk(stk, H5E_WALK_UPWARD, &op, nullptr), 0);

  // 3. Exercise H5E__print(..., bk_compatible=true/false) across multiple
  //    classes and empty descriptions.
  std::array<char, 4096> buf1{};
  FILE* stream1 = fmemopen(buf1.data(), buf1.size(), "w+");
  ASSERT_NE(stream1, nullptr);
  absl::Cleanup close1 = [&] { std::fclose(stream1); };
  ASSERT_GE(H5E__print(stk, stream1, true), 0);
  ASSERT_EQ(std::fflush(stream1), 0);
  EXPECT_THAT(std::string_view(buf1.data()), HasSubstr("PrintCls2-DIAG"));
  EXPECT_THAT(std::string_view(buf1.data()), HasSubstr("PrintCls1-DIAG"));

  std::array<char, 4096> buf2{};
  FILE* stream2 = fmemopen(buf2.data(), buf2.size(), "w+");
  ASSERT_NE(stream2, nullptr);
  absl::Cleanup close2 = [&] { std::fclose(stream2); };
  ASSERT_GE(H5E__print2(stk_id.get(), stream2), 0);
  ASSERT_EQ(std::fflush(stream2), 0);
  EXPECT_THAT(std::string_view(buf2.data()), HasSubstr("PrintCls2-DIAG"));
  EXPECT_THAT(std::string_view(buf2.data()), HasSubstr("PrintCls1-DIAG"));

  // 4. H5E__print2 invalid stack ID and H5E__print(empty, nullptr, false).
  EXPECT_LT(H5E__print2(H5I_INVALID_HID, stream2), 0);
  EXPECT_GE(H5E__print2(H5E_DEFAULT, nullptr), 0);

  // 5. Wrong-type hid_t triggers failure inside H5E__walk1_cb and
  //    H5E__walk2_cb cleanly without leaks.
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedErrorStack bad_stk_id(H5Ecreate_stack());
  ASSERT_NE(H5I_INVALID_HID, bad_stk_id.get());
  auto* bad_stk = static_cast<H5E_stack_t*>(
      H5I_object_verify(bad_stk_id.get(), H5I_ERROR_STACK));
  ASSERT_NE(bad_stk, nullptr);

  ASSERT_GE(H5Epush2(bad_stk_id.get(), "b.c", "fb", 1, H5E_ERR_CLS_g,
                     space.get(), H5E_BADVALUE_g, "bad maj"),
            0);
  EXPECT_LT(H5E__print(bad_stk, stream1, true), 0);
  EXPECT_LT(H5E__print(bad_stk, stream2, false), 0);
  EXPECT_LT(H5E__print2(bad_stk_id.get(), stream2), 0);

  ASSERT_GE(H5E__destroy_stack(bad_stk), 0);
  ASSERT_GE(H5Epush2(bad_stk_id.get(), "b.c", "fb", 2, space.get(), H5E_ARGS_g,
                     H5E_BADVALUE_g, "bad cls"),
            0);
  EXPECT_LT(H5E__print(bad_stk, stream2, false), 0);
}

TEST(H5EintTest, GetAutoAndClassNameAndMsgHelpers) {
  ScopedErrorSilence silence;

  ScopedErrorClass cls(H5Eregister_class("HelperCls", "HelperLib", "3.0"));
  ASSERT_NE(H5I_INVALID_HID, cls.get());
  ScopedErrorMsg msg(H5Ecreate_msg(cls.get(), H5E_MINOR, "HelperMsg"));
  ASSERT_NE(H5I_INVALID_HID, msg.get());

  auto* cls_ptr =
      static_cast<H5E_cls_t*>(H5I_object_verify(cls.get(), H5I_ERROR_CLASS));
  auto* msg_ptr =
      static_cast<H5E_msg_t*>(H5I_object_verify(msg.get(), H5I_ERROR_MSG));
  ASSERT_NE(cls_ptr, nullptr);
  ASSERT_NE(msg_ptr, nullptr);

  std::array<char, 6> short_buf{};
  EXPECT_EQ(H5E__get_class_name(cls_ptr, short_buf.data(), short_buf.size()),
            9);
  EXPECT_STREQ("Helpe", short_buf.data());

  H5E_type_t out_type = H5E_MAJOR;
  EXPECT_EQ(
      H5E__get_msg(msg_ptr, &out_type, short_buf.data(), short_buf.size()), 9);
  EXPECT_EQ(out_type, H5E_MINOR);
  EXPECT_STREQ("Helpe", short_buf.data());

  H5E_auto_op_t op{};
  void* data = nullptr;
  H5E_stack_t* stk = H5E__get_my_stack();
  ASSERT_NE(stk, nullptr);
  EXPECT_GE(H5E__get_auto(stk, nullptr, nullptr), 0);
  EXPECT_GE(H5E__get_auto(stk, &op, &data), 0);
}

TEST(H5EintTest, CopyAppendClearDestroyAndPopErrorPathsCleanly) {
  ScopedErrorSilence silence;

  const hid_t stale_cls = H5Eregister_class("IntStale", "IntLib", "1.0");
  ASSERT_NE(H5I_INVALID_HID, stale_cls);
  const hid_t stale_maj = H5Ecreate_msg(stale_cls, H5E_MAJOR, "IntStaleMaj");
  const hid_t stale_min = H5Ecreate_msg(stale_cls, H5E_MINOR, "IntStaleMin");
  ASSERT_NE(H5I_INVALID_HID, stale_maj);
  ASSERT_NE(H5I_INVALID_HID, stale_min);
  ASSERT_GE(H5Eunregister_class(stale_cls), 0);

  // 1. Trigger H5E__copy_stack_entry failures on cls_id, maj_num, and min_num
  //    across H5E__get_current_stack, H5E__set_current_stack, and
  //    H5E__append_stack, keeping library IDs for preceding fields so zero
  //    refcounts or strings leak.
  ScopedErrorStack src_id(H5Ecreate_stack());
  ScopedErrorStack dst_id(H5Ecreate_stack());
  ASSERT_NE(H5I_INVALID_HID, src_id.get());
  ASSERT_NE(H5I_INVALID_HID, dst_id.get());
  auto* src = static_cast<H5E_stack_t*>(
      H5I_object_verify(src_id.get(), H5I_ERROR_STACK));
  auto* dst = static_cast<H5E_stack_t*>(
      H5I_object_verify(dst_id.get(), H5I_ERROR_STACK));
  ASSERT_NE(src, nullptr);
  ASSERT_NE(dst, nullptr);

  H5E_stack_t* def_stk = H5E__get_my_stack();
  ASSERT_NE(def_stk, nullptr);

  ASSERT_GE(H5Epush2(src_id.get(), "s.c", "fs", 1, H5E_ERR_CLS_g, H5E_ARGS_g,
                     H5E_BADVALUE_g, "src"),
            0);
  {
    absl::Cleanup restore_src = [&] {
      src->entries[0].err.cls_id = H5E_ERR_CLS_g;
      src->entries[0].err.maj_num = H5E_ARGS_g;
      src->entries[0].err.min_num = H5E_BADVALUE_g;
      H5E_clear_stack();
    };
    src->entries[0].err.cls_id = stale_cls;
    EXPECT_LT(H5E__set_current_stack(src), 0);
    // H5E__set_current_stack sets def_stk->nused before H5E__copy_stack_entry
    // increments IDs or duplicates strings, leaving shallow aliases from src
    // in def_stk->entries[0]. Clear those aliases before emptying def_stk.
    if (def_stk->nused >= 1) {
      def_stk->entries[0].app_entry = false;
      def_stk->entries[0].err.cls_id = H5E_ERR_CLS_g;
      def_stk->entries[0].err.maj_num = H5E_ARGS_g;
      def_stk->entries[0].err.min_num = H5E_BADVALUE_g;
      def_stk->entries[0].err.file_name = nullptr;
      def_stk->entries[0].err.func_name = nullptr;
      def_stk->entries[0].err.desc = nullptr;
    }
    ASSERT_GE(H5E_clear_stack(), 0);

    src->entries[0].err.cls_id = H5E_ERR_CLS_g;
    src->entries[0].err.maj_num = stale_maj;
    EXPECT_LT(H5E__append_stack(dst, src), 0);
    ASSERT_GE(H5E_clear_stack(), 0);

    src->entries[0].err.maj_num = H5E_ARGS_g;
    src->entries[0].err.min_num = stale_min;
    EXPECT_LT(H5E__append_stack(dst, src), 0);
    ASSERT_GE(H5E_clear_stack(), 0);
  }

  ASSERT_GE(H5Epush2(H5E_DEFAULT, "c.c", "fc", 2, H5E_ERR_CLS_g, H5E_ARGS_g,
                     H5E_BADVALUE_g, "cur"),
            0);
  ASSERT_EQ(def_stk->nused, 1u);
  {
    absl::Cleanup restore_def = [&] {
      def_stk->entries[0].err.cls_id = H5E_ERR_CLS_g;
      H5E_clear_stack();
    };
    def_stk->entries[0].err.cls_id = stale_cls;
    EXPECT_EQ(H5E__get_current_stack(), nullptr);
    EXPECT_LT(H5E_clear_stack(), 0);
  }

  // 2. Trigger H5E__clear_entries failures on cls_id, maj_num, and min_num
  //    via H5E__destroy_stack and H5E__pop on an isolated stack so HGOTO_ERROR
  //    appends only to def_stk and leaves bad_stk->nused == 1 intact.
  ScopedErrorStack bad_stk_id(H5Ecreate_stack());
  ASSERT_NE(H5I_INVALID_HID, bad_stk_id.get());
  auto* bad_stk = static_cast<H5E_stack_t*>(
      H5I_object_verify(bad_stk_id.get(), H5I_ERROR_STACK));
  ASSERT_NE(bad_stk, nullptr);
  ASSERT_GE(H5Epush2(bad_stk_id.get(), "b.c", "fb", 3, H5E_ERR_CLS_g,
                     H5E_ARGS_g, H5E_BADVALUE_g, "bad"),
            0);
  ASSERT_EQ(bad_stk->nused, 1u);
  {
    absl::Cleanup restore_bad = [&] {
      bad_stk->entries[0].err.cls_id = H5E_ERR_CLS_g;
      bad_stk->entries[0].err.maj_num = H5E_ARGS_g;
      bad_stk->entries[0].err.min_num = H5E_BADVALUE_g;
      H5E_clear_stack();
    };
    bad_stk->entries[0].err.cls_id = stale_cls;
    EXPECT_LT(H5E__destroy_stack(bad_stk), 0);

    bad_stk->entries[0].err.cls_id = H5E_ERR_CLS_g;
    bad_stk->entries[0].err.maj_num = stale_maj;
    EXPECT_LT(H5E__pop(bad_stk, 1), 0);

    bad_stk->entries[0].err.maj_num = H5E_ARGS_g;
    bad_stk->entries[0].err.min_num = stale_min;
    EXPECT_LT(H5E__destroy_stack(bad_stk), 0);
  }
}

}  // namespace
}  // namespace hdf5
