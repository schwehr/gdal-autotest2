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
// Tests public HDF5 error stack, class, message, printing, walking, auto
// reporting, appending, and pause/resume APIs (third_party/hdf5/src/H5E.c).
// Note: google3 builds with -DNDEBUG, H5_HAVE_THREADSAFE_API, and deprecated
// v1 error symbols enabled.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5E.c

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer) || \
    __has_feature(hwaddress_sanitizer)
#define H5E_TEST_HAS_ASAN_OR_LSAN 1
#endif
#if __has_feature(memory_sanitizer) || __has_feature(thread_sanitizer)
#define H5E_TEST_HAS_OTHER_SAN 1
#endif
#endif
#if !defined(H5E_TEST_HAS_ASAN_OR_LSAN) &&                          \
    (defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__) || \
     defined(LEAK_SANITIZER) || defined(HWADDRESS_SANITIZER) ||     \
     defined(__SANITIZE_HWADDRESS__))
#define H5E_TEST_HAS_ASAN_OR_LSAN 1
#endif
#if !defined(H5E_TEST_HAS_OTHER_SAN) &&                        \
    (defined(MEMORY_SANITIZER) || defined(THREAD_SANITIZER) || \
     defined(__SANITIZE_THREAD__))
#define H5E_TEST_HAS_OTHER_SAN 1
#endif

// `:hdf5` compiles with -DNDEBUG; match internal struct layouts across
// H5private.h -> H5CXprivate.h -> H5ACprivate.h -> H5Cprivate.h.
#ifndef NDEBUG
#define H5E_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5E_FRIEND
#include "third_party/hdf5/src/H5Epkg.h"
#include "third_party/hdf5/src/H5Iprivate.h"
}  // extern "C"
#ifdef H5E_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5E_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

using ScopedErrorClass = ScopedH5Id<H5Eunregister_class>;
using ScopedErrorMsg = ScopedH5Id<H5Eclose_msg>;
using ScopedErrorStack = ScopedH5Id<H5Eclose_stack>;

// Saves and restores the H5E_DEFAULT automatic error reporting callback,
// clearing H5E_DEFAULT on destruction even if the test switched to v1 auto
// mode.
class ScopedDefaultAutoRestore {
 public:
  ScopedDefaultAutoRestore() {
    saved_ = (H5Eget_auto2(H5E_DEFAULT, &prev_func_, &prev_data_) >= 0);
    H5Eclear2(H5E_DEFAULT);
  }

  ~ScopedDefaultAutoRestore() {
    H5Eclear2(H5E_DEFAULT);
    if (saved_) {
      H5Eset_auto2(H5E_DEFAULT, prev_func_, prev_data_);
    }
  }

  ScopedDefaultAutoRestore(const ScopedDefaultAutoRestore&) = delete;
  ScopedDefaultAutoRestore& operator=(const ScopedDefaultAutoRestore&) = delete;

  H5E_auto2_t prev_func() const { return prev_func_; }
  void* prev_data() const { return prev_data_; }

 private:
  H5E_auto2_t prev_func_ = nullptr;
  void* prev_data_ = nullptr;
  bool saved_ = false;
};

struct WalkRecord {
  hid_t cls_id = H5I_INVALID_HID;
  hid_t maj_num = H5I_INVALID_HID;
  hid_t min_num = H5I_INVALID_HID;
  unsigned n = 0;
  unsigned line = 0;
  std::string func_name;
  std::string file_name;
  std::string desc;
};

herr_t CollectWalkCb(unsigned n, const H5E_error2_t* err_desc,
                     void* client_data) {
  auto* records = static_cast<std::vector<WalkRecord>*>(client_data);
  if (err_desc == nullptr || records == nullptr) {
    return -1;
  }
  records->push_back({
      .cls_id = err_desc->cls_id,
      .maj_num = err_desc->maj_num,
      .min_num = err_desc->min_num,
      .n = n,
      .line = err_desc->line,
      .func_name = err_desc->func_name ? err_desc->func_name : "",
      .file_name = err_desc->file_name ? err_desc->file_name : "",
      .desc = err_desc->desc ? err_desc->desc : "",
  });
  return 0;
}

TEST(H5ETest, RegisterGetNameAndUnregisterClass) {
  ScopedErrorSilence silence;

  // 1. Register a custom error class and inspect its name with exact,
  //    query-only, and truncated buffer sizes.
  constexpr char kClsName[] = "CustomErrCls";
  ScopedErrorClass cls(H5Eregister_class(kClsName, "CustomLib", "1.2.3"));
  ASSERT_NE(H5I_INVALID_HID, cls.get());

  EXPECT_EQ(H5Eget_class_name(cls.get(), nullptr, 0),
            static_cast<ssize_t>(sizeof(kClsName) - 1));

  std::array<char, 5> truncated{};
  truncated.fill('X');
  EXPECT_EQ(H5Eget_class_name(cls.get(), truncated.data(), truncated.size()),
            static_cast<ssize_t>(sizeof(kClsName) - 1));
  EXPECT_STREQ("Cust", truncated.data());

  std::array<char, 1> tiny{'X'};
  EXPECT_EQ(H5Eget_class_name(cls.get(), tiny.data(), tiny.size()),
            static_cast<ssize_t>(sizeof(kClsName) - 1));
  EXPECT_STREQ("", tiny.data());

  std::array<char, 32> full{};
  EXPECT_EQ(H5Eget_class_name(cls.get(), full.data(), full.size()),
            static_cast<ssize_t>(sizeof(kClsName) - 1));
  EXPECT_STREQ(kClsName, full.data());

  // 2. Built-in HDF5 error class also returns its name.
  std::array<char, 16> lib_name{};
  EXPECT_EQ(H5Eget_class_name(H5E_ERR_CLS, lib_name.data(), lib_name.size()),
            4);
  EXPECT_STREQ("HDF5", lib_name.data());

  // 3. Empty class name returns length 0.
  ScopedErrorClass empty_cls(H5Eregister_class("", "Lib", "0.1"));
  ASSERT_NE(H5I_INVALID_HID, empty_cls.get());
  EXPECT_EQ(H5Eget_class_name(empty_cls.get(), nullptr, 0), 0);

  // 4. Error paths: NULL arguments, invalid IDs, and wrong ID types.
  EXPECT_EQ(H5I_INVALID_HID, H5Eregister_class(nullptr, "Lib", "1.0"));
  EXPECT_EQ(H5I_INVALID_HID, H5Eregister_class("Cls", nullptr, "1.0"));
  EXPECT_EQ(H5I_INVALID_HID, H5Eregister_class("Cls", "Lib", nullptr));

  EXPECT_LT(H5Eget_class_name(H5I_INVALID_HID, full.data(), full.size()), 0);
  EXPECT_LT(H5Eget_class_name(H5E_ARGS, full.data(), full.size()), 0);
  EXPECT_LT(H5Eunregister_class(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Eunregister_class(H5E_ARGS), 0);

  // 5. Unregistering a class automatically removes any still-open messages
  //    that belong to it without leaking.
  const hid_t dangling_maj =
      H5Ecreate_msg(cls.get(), H5E_MAJOR, "Dangling major");
  const hid_t dangling_min =
      H5Ecreate_msg(cls.get(), H5E_MINOR, "Dangling minor");
  ASSERT_NE(H5I_INVALID_HID, dangling_maj);
  ASSERT_NE(H5I_INVALID_HID, dangling_min);

  EXPECT_GE(H5Eunregister_class(cls.release()), 0);
  // Both messages were already removed when the class unregistered.
  EXPECT_LT(H5Eclose_msg(dangling_maj), 0);
  EXPECT_LT(H5Eclose_msg(dangling_min), 0);
}

TEST(H5ETest, CreateGetAndCloseMessages) {
  ScopedErrorSilence silence;

  ScopedErrorClass cls(H5Eregister_class("MsgTestCls", "MsgLib", "2.0"));
  ASSERT_NE(H5I_INVALID_HID, cls.get());

  // 1. Create major, minor, and empty messages.
  constexpr char kMajText[] = "Custom major failure";
  constexpr char kMinText[] = "Custom minor detail";
  ScopedErrorMsg maj(H5Ecreate_msg(cls.get(), H5E_MAJOR, kMajText));
  ScopedErrorMsg min(H5Ecreate_msg(cls.get(), H5E_MINOR, kMinText));
  ScopedErrorMsg empty_msg(H5Ecreate_msg(cls.get(), H5E_MINOR, ""));
  ASSERT_NE(H5I_INVALID_HID, maj.get());
  ASSERT_NE(H5I_INVALID_HID, min.get());
  ASSERT_NE(H5I_INVALID_HID, empty_msg.get());

  // 2. Query length, type, truncated buffer, and full buffer.
  H5E_type_t type = H5E_MINOR;
  EXPECT_EQ(H5Eget_msg(maj.get(), &type, nullptr, 0),
            static_cast<ssize_t>(sizeof(kMajText) - 1));
  EXPECT_EQ(type, H5E_MAJOR);

  std::array<char, 7> trunc{};
  trunc.fill('Z');
  EXPECT_EQ(H5Eget_msg(min.get(), &type, trunc.data(), trunc.size()),
            static_cast<ssize_t>(sizeof(kMinText) - 1));
  EXPECT_EQ(type, H5E_MINOR);
  EXPECT_STREQ("Custom", trunc.data());

  std::array<char, 64> buf{};
  EXPECT_EQ(H5Eget_msg(min.get(), nullptr, buf.data(), buf.size()),
            static_cast<ssize_t>(sizeof(kMinText) - 1));
  EXPECT_STREQ(kMinText, buf.data());

  EXPECT_EQ(H5Eget_msg(empty_msg.get(), &type, buf.data(), buf.size()), 0);
  EXPECT_STREQ("", buf.data());

  // 3. Built-in library major and minor messages.
  EXPECT_GT(H5Eget_msg(H5E_ARGS, &type, buf.data(), buf.size()), 0);
  EXPECT_EQ(type, H5E_MAJOR);
  EXPECT_GT(H5Eget_msg(H5E_BADVALUE, &type, buf.data(), buf.size()), 0);
  EXPECT_EQ(type, H5E_MINOR);

  // 4. H5Eget_msg does not clear H5E_DEFAULT (FUNC_ENTER_API_NOCLEAR).
  ASSERT_GE(H5Epush2(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS,
                     H5E_ARGS, H5E_BADVALUE, "preserved"),
            0);
  EXPECT_EQ(H5Eget_num(H5E_DEFAULT), 1);
  EXPECT_GT(H5Eget_msg(maj.get(), &type, buf.data(), buf.size()), 0);
  EXPECT_EQ(H5Eget_num(H5E_DEFAULT), 1);
  EXPECT_GE(H5Eclear2(H5E_DEFAULT), 0);

  // 5. Error paths for create/get/close.
  EXPECT_EQ(H5I_INVALID_HID,
            H5Ecreate_msg(cls.get(), static_cast<H5E_type_t>(99), "bad type"));
  EXPECT_EQ(H5I_INVALID_HID, H5Ecreate_msg(cls.get(), H5E_MAJOR, nullptr));
  EXPECT_EQ(H5I_INVALID_HID,
            H5Ecreate_msg(H5I_INVALID_HID, H5E_MAJOR, "bad cls"));
  EXPECT_EQ(H5I_INVALID_HID, H5Ecreate_msg(maj.get(), H5E_MAJOR, "wrong type"));

  EXPECT_LT(H5Eget_msg(H5I_INVALID_HID, &type, buf.data(), buf.size()), 0);
  EXPECT_LT(H5Eget_msg(cls.get(), &type, buf.data(), buf.size()), 0);

  EXPECT_LT(H5Eclose_msg(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Eclose_msg(cls.get()), 0);
  EXPECT_GE(H5Eclose_msg(empty_msg.release()), 0);
}

TEST(H5ETest, CreateCloseGetSetAndClearStack) {
  ScopedErrorSilence silence;

  // 1. Create an empty stack, verify 0 entries, clear, and close.
  ScopedErrorStack stack(H5Ecreate_stack());
  ASSERT_NE(H5I_INVALID_HID, stack.get());
  EXPECT_EQ(H5Eget_num(stack.get()), 0);
  EXPECT_GE(H5Eclear2(stack.get()), 0);

  // 2. Push two errors onto H5E_DEFAULT, capture with H5Eget_current_stack
  //    (which clears H5E_DEFAULT), then restore via H5Eset_current_stack
  //    (which closes the passed stack ID).
  ASSERT_GE(H5Epush2(H5E_DEFAULT, "f1.c", "fn1", 10, H5E_ERR_CLS, H5E_ARGS,
                     H5E_BADVALUE, "first %d", 1),
            0);
  ASSERT_GE(H5Epush2(H5E_DEFAULT, "f2.c", "fn2", 20, H5E_ERR_CLS, H5E_FILE,
                     H5E_CANTOPENFILE, "second"),
            0);
  EXPECT_EQ(H5Eget_num(H5E_DEFAULT), 2);

  hid_t captured = H5Eget_current_stack();
  ASSERT_NE(H5I_INVALID_HID, captured);
  EXPECT_EQ(H5Eget_num(H5E_DEFAULT), 0);
  EXPECT_EQ(H5Eget_num(captured), 2);

  ASSERT_GE(H5Eset_current_stack(captured), 0);
  EXPECT_EQ(H5Eget_num(H5E_DEFAULT), 2);
  // H5Eset_current_stack closes `captured`.
  EXPECT_LT(H5Eclose_stack(captured), 0);
  EXPECT_GE(H5Eclear2(H5E_DEFAULT), 0);
  EXPECT_EQ(H5Eget_num(H5E_DEFAULT), 0);

  // 3. H5E_DEFAULT is a valid no-op for set_current_stack and close_stack.
  EXPECT_GE(H5Eset_current_stack(H5E_DEFAULT), 0);
  EXPECT_GE(H5Eclose_stack(H5E_DEFAULT), 0);

  // 4. Operating on a non-default stack clears H5E_DEFAULT.
  ASSERT_GE(H5Epush2(H5E_DEFAULT, "f1.c", "fn1", 1, H5E_ERR_CLS, H5E_ARGS,
                     H5E_BADVALUE, "ephemeral"),
            0);
  EXPECT_EQ(H5Eget_num(stack.get()), 0);
  EXPECT_EQ(H5Eget_num(H5E_DEFAULT), 0);

  // 5. Invalid stack IDs fail cleanly.
  EXPECT_LT(H5Eget_num(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Eget_num(H5E_ARGS), 0);
  EXPECT_LT(H5Eclear2(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Eclear2(H5E_ARGS), 0);
  EXPECT_LT(H5Eset_current_stack(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Eset_current_stack(H5E_ARGS), 0);
  EXPECT_LT(H5Eclose_stack(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Eclose_stack(H5E_ARGS), 0);
}

TEST(H5ETest, PushPopAndConvenienceMacros) {
  ScopedErrorSilence silence;

  ScopedErrorClass cls(H5Eregister_class("PushCls", "PushLib", "1.0"));
  ASSERT_NE(H5I_INVALID_HID, cls.get());
  ScopedErrorMsg maj(H5Ecreate_msg(cls.get(), H5E_MAJOR, "Push major"));
  ScopedErrorMsg min(H5Ecreate_msg(cls.get(), H5E_MINOR, "Push minor"));
  ASSERT_NE(H5I_INVALID_HID, maj.get());
  ASSERT_NE(H5I_INVALID_HID, min.get());

  absl::Cleanup clear_default = [] { H5Eclear2(H5E_DEFAULT); };

  // 1. Push onto a custom stack with custom class/messages, NULL fmt fallback,
  //    and formatted description.
  ScopedErrorStack stack(H5Ecreate_stack());
  ASSERT_NE(H5I_INVALID_HID, stack.get());

  ASSERT_GE(H5Epush2(stack.get(), "a.c", "fa", 101, cls.get(), maj.get(),
                     min.get(), "val=%s", "ok"),
            0);
  ASSERT_GE(H5Epush2(stack.get(), "b.c", "fb", 102, H5E_ERR_CLS, H5E_ARGS,
                     H5E_BADVALUE, nullptr),
            0);
  ASSERT_GE(H5Epush2(stack.get(), "c.c", "fc", 103, H5E_ERR_CLS, H5E_ARGS,
                     H5E_BADRANGE, ""),
            0);
  EXPECT_EQ(H5Eget_num(stack.get()), 3);

  // 2. Pop 0, pop 1, and pop more than remaining entries (clamped).
  EXPECT_GE(H5Epop(stack.get(), 0), 0);
  EXPECT_EQ(H5Eget_num(stack.get()), 3);
  EXPECT_GE(H5Epop(stack.get(), 1), 0);
  EXPECT_EQ(H5Eget_num(stack.get()), 2);
  EXPECT_GE(H5Epop(stack.get(), 50), 0);
  EXPECT_EQ(H5Eget_num(stack.get()), 0);

  // 3. Exercise convenience macros H5Epush_sim, H5Epush_ret, and H5Epush_goto
  //    on H5E_DEFAULT, plus H5Epop(H5E_DEFAULT, ...).
  ASSERT_GE(H5Epush_sim("SimFunc", cls.get(), maj.get(), min.get(), "sim"), 0);
  auto push_ret_fn = [&]() -> herr_t {
    H5Epush_ret("RetFunc", cls.get(), maj.get(), min.get(), "ret", 0);
  };
  ASSERT_GE(push_ret_fn(), 0);
  auto push_goto_fn = [&]() -> herr_t {
    herr_t rc = -1;
    H5Epush_goto("GotoFunc", cls.get(), maj.get(), min.get(), "goto", done);
  done:
    return rc;
  };
  EXPECT_LT(push_goto_fn(), 0);
  EXPECT_EQ(H5Eget_num(H5E_DEFAULT), 3);

  EXPECT_GE(H5Epop(H5E_DEFAULT, 2), 0);
  EXPECT_EQ(H5Eget_num(H5E_DEFAULT), 1);

  std::vector<WalkRecord> recs;
  ASSERT_GE(H5Ewalk2(H5E_DEFAULT, H5E_WALK_UPWARD, CollectWalkCb, &recs), 0);
  ASSERT_EQ(recs.size(), 1);
  EXPECT_EQ(recs[0].func_name, "SimFunc");
  EXPECT_EQ(recs[0].desc, "sim");

  EXPECT_GE(H5Eclear2(H5E_DEFAULT), 0);

  // 4. Error paths for push/pop on invalid stack IDs.
  EXPECT_LT(H5Epush2(H5I_INVALID_HID, "x.c", "fx", 1, H5E_ERR_CLS, H5E_ARGS,
                     H5E_BADVALUE, "bad"),
            0);
  EXPECT_LT(H5Epop(H5I_INVALID_HID, 1), 0);
  EXPECT_LT(H5Epop(H5E_ARGS, 1), 0);
}

TEST(H5ETest, WalkDirectionsEarlyStopFailureAndPrint) {
  ScopedErrorSilence silence;

  ScopedErrorClass cls1(H5Eregister_class("WalkCls1", "LibOne", "1.0"));
  ScopedErrorClass cls2(H5Eregister_class("WalkCls2", "LibTwo", "2.0"));
  ASSERT_NE(H5I_INVALID_HID, cls1.get());
  ASSERT_NE(H5I_INVALID_HID, cls2.get());

  ScopedErrorMsg maj1(H5Ecreate_msg(cls1.get(), H5E_MAJOR, "MajOne"));
  ScopedErrorMsg min1(H5Ecreate_msg(cls1.get(), H5E_MINOR, "MinOne"));
  ScopedErrorMsg maj2(H5Ecreate_msg(cls2.get(), H5E_MAJOR, "MajTwo"));
  ScopedErrorMsg min2(H5Ecreate_msg(cls2.get(), H5E_MINOR, "MinTwo"));
  ASSERT_NE(H5I_INVALID_HID, maj1.get());
  ASSERT_NE(H5I_INVALID_HID, min1.get());
  ASSERT_NE(H5I_INVALID_HID, maj2.get());
  ASSERT_NE(H5I_INVALID_HID, min2.get());

  absl::Cleanup clear_default = [] { H5Eclear2(H5E_DEFAULT); };

  ScopedErrorStack stack(H5Ecreate_stack());
  ASSERT_NE(H5I_INVALID_HID, stack.get());

  ASSERT_GE(H5Epush2(stack.get(), "one.c", "f_one", 11, cls1.get(), maj1.get(),
                     min1.get(), "desc one"),
            0);
  ASSERT_GE(H5Epush2(stack.get(), "two.c", "f_two", 22, cls2.get(), maj2.get(),
                     min2.get(), ""),
            0);
  ASSERT_GE(H5Epush2(stack.get(), "three.c", "f_three", 33, H5E_ERR_CLS,
                     H5E_ARGS, H5E_BADVALUE, "desc three"),
            0);

  // 1. Walk upward (oldest first) and downward (newest first).
  std::vector<WalkRecord> upward;
  ASSERT_GE(H5Ewalk2(stack.get(), H5E_WALK_UPWARD, CollectWalkCb, &upward), 0);
  ASSERT_EQ(upward.size(), 3);
  EXPECT_EQ(upward[0].n, 0);
  EXPECT_EQ(upward[0].func_name, "f_one");
  EXPECT_EQ(upward[1].n, 1);
  EXPECT_EQ(upward[1].func_name, "f_two");
  EXPECT_EQ(upward[2].n, 2);
  EXPECT_EQ(upward[2].func_name, "f_three");

  std::vector<WalkRecord> downward;
  ASSERT_GE(H5Ewalk2(stack.get(), H5E_WALK_DOWNWARD, CollectWalkCb, &downward),
            0);
  ASSERT_EQ(downward.size(), 3);
  EXPECT_EQ(downward[0].n, 0);
  EXPECT_EQ(downward[0].func_name, "f_three");
  EXPECT_EQ(downward[1].n, 1);
  EXPECT_EQ(downward[1].func_name, "f_two");
  EXPECT_EQ(downward[2].n, 2);
  EXPECT_EQ(downward[2].func_name, "f_one");

  // 2. Invalid walk direction falls back to upward; NULL callback is a no-op.
  std::vector<WalkRecord> fallback;
  ASSERT_GE(H5Ewalk2(stack.get(), static_cast<H5E_direction_t>(99),
                     CollectWalkCb, &fallback),
            0);
  ASSERT_EQ(fallback.size(), 3);
  EXPECT_EQ(fallback[0].func_name, "f_one");
  EXPECT_GE(H5Ewalk2(stack.get(), H5E_WALK_UPWARD, nullptr, nullptr), 0);

  // 3. Callback returning positive stops early (>= 0); returning < 0 fails.
  int count = 0;
  auto stop_early = [](unsigned, const H5E_error2_t*, void* data) -> herr_t {
    int* c = static_cast<int*>(data);
    ++(*c);
    return 1;
  };
  EXPECT_GE(H5Ewalk2(stack.get(), H5E_WALK_UPWARD, stop_early, &count), 0);
  EXPECT_EQ(count, 1);

  auto fail_cb = [](unsigned, const H5E_error2_t*, void*) -> herr_t {
    return -1;
  };
  EXPECT_LT(H5Ewalk2(stack.get(), H5E_WALK_UPWARD, fail_cb, nullptr), 0);

  // 4. Print multi-class stack to an in-memory FILE* via fmemopen.
  std::array<char, 4096> buf{};
  FILE* stream = fmemopen(buf.data(), buf.size(), "w+");
  ASSERT_NE(stream, nullptr);
  absl::Cleanup close_stream = [&] { std::fclose(stream); };

  ASSERT_GE(H5Eprint2(stack.get(), stream), 0);
  ASSERT_EQ(std::fflush(stream), 0);

  const std::string_view printed(buf.data());
  EXPECT_THAT(printed, HasSubstr("HDF5-DIAG: Error detected in HDF5"));
  EXPECT_THAT(printed, HasSubstr("WalkCls2-DIAG: Error detected in LibTwo"));
  EXPECT_THAT(printed, HasSubstr("WalkCls1-DIAG: Error detected in LibOne"));
  EXPECT_THAT(printed, HasSubstr("desc one"));
  EXPECT_THAT(printed, HasSubstr("desc three"));

  // 5. Print H5E_DEFAULT and empty stack with stream == nullptr (no stderr).
  ASSERT_GE(H5Epush2(H5E_DEFAULT, "d.c", "fd", 44, H5E_ERR_CLS, H5E_ARGS,
                     H5E_BADVALUE, "on default"),
            0);
  std::array<char, 2048> def_buf{};
  FILE* def_stream = fmemopen(def_buf.data(), def_buf.size(), "w+");
  ASSERT_NE(def_stream, nullptr);
  absl::Cleanup close_def_stream = [&] { std::fclose(def_stream); };
  ASSERT_GE(H5Eprint2(H5E_DEFAULT, def_stream), 0);
  ASSERT_EQ(std::fflush(def_stream), 0);
  EXPECT_THAT(std::string_view(def_buf.data()), HasSubstr("on default"));

  EXPECT_GE(H5Eclear2(H5E_DEFAULT), 0);
  EXPECT_GE(H5Eprint2(H5E_DEFAULT, nullptr), 0);

  // 6. Invalid stack ID fails for walk2 and print2.
  EXPECT_LT(
      H5Ewalk2(H5I_INVALID_HID, H5E_WALK_UPWARD, CollectWalkCb, &fallback), 0);
  EXPECT_LT(H5Eprint2(H5I_INVALID_HID, stream), 0);
  EXPECT_LT(H5Eprint2(H5E_ARGS, stream), 0);

  // 7. Printing a stack with a valid hid_t of the wrong type fails inside
  //    H5E__walk2_cb cleanly and releases refcounts on clear.
  ScopedSpace wrong_id(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(wrong_id.valid());
  ScopedErrorStack bad_entry_stack(H5Ecreate_stack());
  ASSERT_NE(H5I_INVALID_HID, bad_entry_stack.get());
  ASSERT_GE(H5Epush2(bad_entry_stack.get(), "w.c", "fw", 1, H5E_ERR_CLS,
                     wrong_id.get(), H5E_BADVALUE, "wrong maj"),
            0);
  EXPECT_LT(H5Eprint2(bad_entry_stack.get(), stream), 0);

  ASSERT_GE(H5Eclear2(bad_entry_stack.get()), 0);
  ASSERT_GE(H5Epush2(bad_entry_stack.get(), "w.c", "fw", 2, wrong_id.get(),
                     H5E_ARGS, H5E_BADVALUE, "wrong cls"),
            0);
  EXPECT_LT(H5Eprint2(bad_entry_stack.get(), stream), 0);
}

TEST(H5ETest, AutoHandlerGetSetIsV2AndBeginTry) {
  ScopedDefaultAutoRestore restore;

  // 1. Default auto handler is v2.
  unsigned is_v2 = 0;
  ASSERT_GE(H5Eauto_is_v2(H5E_DEFAULT, &is_v2), 0);
  EXPECT_EQ(is_v2, 1u);
  EXPECT_GE(H5Eauto_is_v2(H5E_DEFAULT, nullptr), 0);

  H5E_auto2_t cur_fn = nullptr;
  void* cur_data = nullptr;
  ASSERT_GE(H5Eget_auto2(H5E_DEFAULT, &cur_fn, &cur_data), 0);
  EXPECT_NE(cur_fn, nullptr);
  EXPECT_GE(H5Eget_auto2(H5E_DEFAULT, nullptr, nullptr), 0);

  // 2. Install a custom v2 auto handler and trigger an API error.
  struct AutoState {
    int calls = 0;
    hid_t estack = H5I_INVALID_HID;
  } auto_state;

  auto custom_auto2 = [](hid_t estack, void* client_data) -> herr_t {
    auto* st = static_cast<AutoState*>(client_data);
    ++st->calls;
    st->estack = estack;
    return 0;
  };
  ASSERT_GE(H5Eset_auto2(H5E_DEFAULT, custom_auto2, &auto_state), 0);

  ASSERT_GE(H5Eget_auto2(H5E_DEFAULT, &cur_fn, &cur_data), 0);
  EXPECT_EQ(cur_fn, static_cast<H5E_auto2_t>(custom_auto2));
  EXPECT_EQ(cur_data, &auto_state);

  // Trigger an error; custom_auto2 should fire once.
  EXPECT_LT(H5Eclose_stack(H5I_INVALID_HID), 0);
  EXPECT_EQ(auto_state.calls, 1);
  EXPECT_EQ(auto_state.estack, static_cast<hid_t>(H5E_DEFAULT));
  EXPECT_GT(H5Eget_num(H5E_DEFAULT), 0);

  // 3. H5E_BEGIN_TRY / H5E_END_TRY suppresses auto reporting temporarily and
  //    restores the custom callback afterward.
  H5E_BEGIN_TRY { EXPECT_LT(H5Eclose_msg(H5I_INVALID_HID), 0); }
  H5E_END_TRY;
  EXPECT_EQ(auto_state.calls, 1);

  EXPECT_LT(H5Eclose_msg(H5I_INVALID_HID), 0);
  EXPECT_EQ(auto_state.calls, 2);

  // 4. Custom stack auto settings are independent from H5E_DEFAULT.
  ScopedErrorStack stack(H5Ecreate_stack());
  ASSERT_NE(H5I_INVALID_HID, stack.get());
  ASSERT_GE(H5Eauto_is_v2(stack.get(), &is_v2), 0);
  EXPECT_EQ(is_v2, 1u);

  int tag = 7;
  ASSERT_GE(H5Eset_auto2(stack.get(), custom_auto2, &tag), 0);
  ASSERT_GE(H5Eget_auto2(stack.get(), &cur_fn, &cur_data), 0);
  EXPECT_EQ(cur_fn, static_cast<H5E_auto2_t>(custom_auto2));
  EXPECT_EQ(cur_data, &tag);

  // 5. Mixing custom H5Eset_auto1 with H5Eget_auto2 fails; default H5Eprint1
  //    allows H5Eget_auto2.
  auto custom_auto1 = [](void*) -> herr_t { return 0; };
  ASSERT_GE(H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr), 0);
  ASSERT_GE(H5Eset_auto1(custom_auto1, nullptr), 0);
  ASSERT_GE(H5Eauto_is_v2(H5E_DEFAULT, &is_v2), 0);
  EXPECT_EQ(is_v2, 0u);

  H5E_BEGIN_TRY { EXPECT_LT(H5Eclose_msg(H5I_INVALID_HID), 0); }
  H5E_END_TRY;
  EXPECT_LT(H5Eget_auto2(H5E_DEFAULT, &cur_fn, &cur_data), 0);

  // Restore to library default v1 handler (is_default = true), then v2 works.
  ASSERT_GE(H5Eset_auto1(reinterpret_cast<H5E_auto1_t>(H5Eprint1), stderr), 0);
  EXPECT_GE(H5Eget_auto2(H5E_DEFAULT, &cur_fn, &cur_data), 0);

  // 6. Error paths on invalid stack IDs.
  ASSERT_GE(H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr), 0);
  EXPECT_LT(H5Eget_auto2(H5I_INVALID_HID, &cur_fn, &cur_data), 0);
  EXPECT_LT(H5Eset_auto2(H5I_INVALID_HID, nullptr, nullptr), 0);
  EXPECT_LT(H5Eauto_is_v2(H5I_INVALID_HID, &is_v2), 0);
}

TEST(H5ETest, AppendStackPreservingAndClosingSource) {
  ScopedErrorSilence silence;

  ScopedErrorStack dst(H5Ecreate_stack());
  ScopedErrorStack src1(H5Ecreate_stack());
  ASSERT_NE(H5I_INVALID_HID, dst.get());
  ASSERT_NE(H5I_INVALID_HID, src1.get());

  ASSERT_GE(H5Epush2(dst.get(), "d.c", "fd", 1, H5E_ERR_CLS, H5E_ARGS,
                     H5E_BADVALUE, "dst0"),
            0);
  ASSERT_GE(H5Epush2(src1.get(), "s1.c", "fs1", 2, H5E_ERR_CLS, H5E_FILE,
                     H5E_CANTOPENFILE, "src1"),
            0);

  // 1. Append with close_source_stack = false keeps src1 open.
  ASSERT_GE(H5Eappend_stack(dst.get(), src1.get(), false), 0);
  EXPECT_EQ(H5Eget_num(dst.get()), 2);
  EXPECT_EQ(H5Eget_num(src1.get()), 1);

  // 2. Append with close_source_stack = true closes src1.
  ASSERT_GE(H5Eappend_stack(dst.get(), src1.release(), true), 0);
  EXPECT_EQ(H5Eget_num(dst.get()), 3);

  // 3. Invalid dst or src IDs fail (including H5E_DEFAULT).
  EXPECT_LT(H5Eappend_stack(H5E_DEFAULT, dst.get(), false), 0);
  EXPECT_LT(H5Eappend_stack(dst.get(), H5E_DEFAULT, false), 0);
  EXPECT_LT(H5Eappend_stack(H5I_INVALID_HID, dst.get(), false), 0);
  EXPECT_LT(H5Eappend_stack(dst.get(), H5I_INVALID_HID, false), 0);
}

TEST(H5ETest, PauseResumeAndIsPaused) {
  ScopedErrorSilence silence;

  // 1. Nested pause/resume on H5E_DEFAULT suppresses both H5Epush2 and
  //    internal HDF5 API errors.
  bool paused = true;
  ASSERT_GE(H5Eis_paused(H5E_DEFAULT, &paused), 0);
  EXPECT_FALSE(paused);

  ASSERT_GE(H5Epause_stack(H5E_DEFAULT), 0);
  absl::Cleanup resume_default = [] {
    bool still_paused = false;
    while (H5Eis_paused(H5E_DEFAULT, &still_paused) >= 0 && still_paused) {
      H5Eresume_stack(H5E_DEFAULT);
    }
  };
  ASSERT_GE(H5Epause_stack(H5E_DEFAULT), 0);

  ASSERT_GE(H5Eis_paused(H5E_DEFAULT, &paused), 0);
  EXPECT_TRUE(paused);

  // Pushing onto a paused stack succeeds silently without adding entries.
  ASSERT_GE(H5Epush2(H5E_DEFAULT, "p.c", "fp", 1, H5E_ERR_CLS, H5E_ARGS,
                     H5E_BADVALUE, "ignored"),
            0);
  EXPECT_EQ(H5Eget_num(H5E_DEFAULT), 0);

  // Failing API call also pushes 0 entries while paused.
  EXPECT_LT(H5Eclose_msg(H5I_INVALID_HID), 0);
  EXPECT_EQ(H5Eget_num(H5E_DEFAULT), 0);

  ASSERT_GE(H5Eresume_stack(H5E_DEFAULT), 0);
  ASSERT_GE(H5Eis_paused(H5E_DEFAULT, &paused), 0);
  EXPECT_TRUE(paused);

  ASSERT_GE(H5Eresume_stack(H5E_DEFAULT), 0);
  ASSERT_GE(H5Eis_paused(H5E_DEFAULT, &paused), 0);
  EXPECT_FALSE(paused);

  // Resuming when not paused fails with H5E_BADRANGE.
  EXPECT_LT(H5Eresume_stack(H5E_DEFAULT), 0);
  EXPECT_GE(H5Eclear2(H5E_DEFAULT), 0);

  // 2. H5Eis_paused does not clear H5E_DEFAULT.
  ASSERT_GE(H5Epush2(H5E_DEFAULT, "p.c", "fp", 2, H5E_ERR_CLS, H5E_ARGS,
                     H5E_BADVALUE, "kept"),
            0);
  ASSERT_GE(H5Eis_paused(H5E_DEFAULT, &paused), 0);
  EXPECT_EQ(H5Eget_num(H5E_DEFAULT), 1);
  EXPECT_GE(H5Eclear2(H5E_DEFAULT), 0);

  // 3. Pause/resume on a custom stack + error paths.
  ScopedErrorStack stack(H5Ecreate_stack());
  ASSERT_NE(H5I_INVALID_HID, stack.get());
  ASSERT_GE(H5Epause_stack(stack.get()), 0);
  ASSERT_GE(H5Eis_paused(stack.get(), &paused), 0);
  EXPECT_TRUE(paused);
  ASSERT_GE(H5Epush2(stack.get(), "p.c", "fp", 3, H5E_ERR_CLS, H5E_ARGS,
                     H5E_BADVALUE, "ignored"),
            0);
  EXPECT_EQ(H5Eget_num(stack.get()), 0);
  ASSERT_GE(H5Eresume_stack(stack.get()), 0);
  ASSERT_GE(H5Eis_paused(stack.get(), &paused), 0);
  EXPECT_FALSE(paused);

  EXPECT_LT(H5Eis_paused(H5E_DEFAULT, nullptr), 0);
  EXPECT_LT(H5Eis_paused(H5I_INVALID_HID, &paused), 0);
  EXPECT_LT(H5Epause_stack(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Eresume_stack(H5I_INVALID_HID), 0);
}

TEST(H5ETest, ErrorStacksArePerThreadAndTransferable) {
  ScopedErrorSilence silence;

  hid_t worker_stack = H5I_INVALID_HID;
  std::thread worker([&]() {
    H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr);
    if (H5Epush2(H5E_DEFAULT, "worker.c", "worker_fn", 77, H5E_ERR_CLS, H5E_IO,
                 H5E_READERROR, "from worker thread") >= 0) {
      worker_stack = H5Eget_current_stack();
    }
  });
  worker.join();

  ASSERT_NE(H5I_INVALID_HID, worker_stack);
  EXPECT_EQ(H5Eget_num(H5E_DEFAULT), 0);

  ASSERT_GE(H5Eset_current_stack(worker_stack), 0);
  EXPECT_EQ(H5Eget_num(H5E_DEFAULT), 1);

  std::vector<WalkRecord> recs;
  ASSERT_GE(H5Ewalk2(H5E_DEFAULT, H5E_WALK_UPWARD, CollectWalkCb, &recs), 0);
  ASSERT_EQ(recs.size(), 1);
  EXPECT_EQ(recs[0].func_name, "worker_fn");
  EXPECT_EQ(recs[0].desc, "from worker thread");
  EXPECT_GE(H5Eclear2(H5E_DEFAULT), 0);
}

TEST(H5ETest, CorruptedEntryFailsCopyAppendClearAndPopCleanly) {
  ScopedErrorSilence silence;

  // Create and close a temporary class/messages to obtain positive,
  // out-of-range closed hid_t values that reliably fail H5I_inc_ref /
  // H5I_dec_ref.
  const hid_t stale_cls = H5Eregister_class("StaleCls", "StaleLib", "1.0");
  ASSERT_NE(H5I_INVALID_HID, stale_cls);
  const hid_t stale_maj = H5Ecreate_msg(stale_cls, H5E_MAJOR, "StaleMaj");
  const hid_t stale_min = H5Ecreate_msg(stale_cls, H5E_MINOR, "StaleMin");
  ASSERT_NE(H5I_INVALID_HID, stale_maj);
  ASSERT_NE(H5I_INVALID_HID, stale_min);
  ASSERT_GE(H5Eunregister_class(stale_cls), 0);
  ASSERT_NE(stale_cls, H5E_ERR_CLS_g);

  // 1. H5Eget_current_stack fails when copying an app entry with stale cls_id
  //    before duplicating any strings, then restores cleanly with zero leaks.
  ASSERT_GE(H5Epush2(H5E_DEFAULT, "cur.c", "fcur", 1, H5E_ERR_CLS, H5E_ARGS,
                     H5E_BADVALUE, "cur"),
            0);
  H5E_stack_t* def_stk = H5E__get_my_stack();
  ASSERT_NE(def_stk, nullptr);
  ASSERT_EQ(def_stk->nused, 1u);
  def_stk->entries[0].err.cls_id = stale_cls;
  {
    absl::Cleanup restore_def = [&] {
      def_stk->entries[0].err.cls_id = H5E_ERR_CLS_g;
      H5Eclear2(H5E_DEFAULT);
    };
    EXPECT_EQ(H5I_INVALID_HID, H5Eget_current_stack());
  }

  // 2. H5Eset_current_stack, H5Eappend_stack, H5Epop, and H5Eclear2 fail
  //    cleanly on a custom stack whose entry carries a stale cls_id.
  ScopedErrorStack src(H5Ecreate_stack());
  ScopedErrorStack dst(H5Ecreate_stack());
  ASSERT_NE(H5I_INVALID_HID, src.get());
  ASSERT_NE(H5I_INVALID_HID, dst.get());
  ASSERT_GE(H5Epush2(src.get(), "src.c", "fsrc", 2, H5E_ERR_CLS, H5E_ARGS,
                     H5E_BADVALUE, "src"),
            0);
  H5E_stack_t* src_ptr =
      static_cast<H5E_stack_t*>(H5I_object_verify(src.get(), H5I_ERROR_STACK));
  ASSERT_NE(src_ptr, nullptr);
  ASSERT_EQ(src_ptr->nused, 1u);
  src_ptr->entries[0].err.cls_id = stale_cls;
  absl::Cleanup restore_src = [&] {
    src_ptr->entries[0].err.cls_id = H5E_ERR_CLS_g;
  };

  EXPECT_LT(H5Eset_current_stack(src.get()), 0);
  // H5E__set_current_stack sets def_stk->nused before H5E__copy_stack_entry
  // increments IDs or duplicates strings, leaving shallow alias pointers from
  // src_ptr in def_stk->entries[0] when copying fails on cls_id. Clear those
  // aliases before emptying def_stk so src_ptr still owns them cleanly.
  if (def_stk->nused >= 1) {
    def_stk->entries[0].app_entry = false;
    def_stk->entries[0].err.cls_id = H5E_ERR_CLS_g;
    def_stk->entries[0].err.maj_num = H5E_ARGS_g;
    def_stk->entries[0].err.min_num = H5E_BADVALUE_g;
    def_stk->entries[0].err.file_name = nullptr;
    def_stk->entries[0].err.func_name = nullptr;
    def_stk->entries[0].err.desc = nullptr;
  }
  ASSERT_GE(H5Eclear2(H5E_DEFAULT), 0);

  EXPECT_LT(H5Eappend_stack(dst.get(), src.get(), false), 0);
  EXPECT_LT(H5Epop(src.get(), 1), 0);
  EXPECT_LT(H5Eclear2(src.get()), 0);
}

#if !defined(H5E_TEST_HAS_ASAN_OR_LSAN)
TEST(H5ETest, Push2FailsOnUnregisteredClassOrMessageIds) {
  // Run in an isolated threadsafe death-test child process: H5Epush2
  // strdup()s file/func before calling H5I_inc_ref on cls/maj/min, leaking
  // those two strings on failure upstream, while HeapLeakChecker disables
  // itself when --gunit_internal_run_death_test is present.
  const std::string prev_style = GTEST_FLAG_GET(death_test_style);
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  absl::Cleanup restore_style = [&] {
    GTEST_FLAG_SET(death_test_style, prev_style);
  };

  EXPECT_EXIT(
      {
        {
          ScopedErrorSilence silence;
          const hid_t stale_cls =
              H5Eregister_class("Push2Stale", "Push2Lib", "1.0");
          if (stale_cls == H5I_INVALID_HID) {
            std::exit(1);
          }
          const hid_t stale_maj =
              H5Ecreate_msg(stale_cls, H5E_MAJOR, "StaleMaj");
          const hid_t stale_min =
              H5Ecreate_msg(stale_cls, H5E_MINOR, "StaleMin");
          if (stale_maj == H5I_INVALID_HID || stale_min == H5I_INVALID_HID) {
            std::exit(2);
          }
          if (H5Eunregister_class(stale_cls) < 0) {
            std::exit(3);
          }
          ScopedErrorStack stack(H5Ecreate_stack());
          if (!stack.valid()) {
            std::exit(4);
          }
          if (H5Epush2(stack.get(), "p.c", "fp", 1, stale_cls, H5E_ARGS,
                       H5E_BADVALUE, "bad cls") >= 0) {
            std::exit(5);
          }
          if (H5Epush2(stack.get(), "p.c", "fp", 2, H5E_ERR_CLS, stale_maj,
                       H5E_BADVALUE, "bad maj") >= 0) {
            std::exit(6);
          }
          if (H5Epush2(stack.get(), "p.c", "fp", 3, H5E_ERR_CLS, H5E_ARGS,
                       stale_min, "bad min") >= 0) {
            std::exit(7);
          }
        }
        std::exit(0);
      },
      ::testing::ExitedWithCode(0), "");
}
#endif

#if !defined(H5E_TEST_HAS_ASAN_OR_LSAN) && !defined(H5E_TEST_HAS_OTHER_SAN)
TEST(H5ETest, Push2HandlesStrdupAndVasprintfAllocationFailure) {
  // Run in an isolated threadsafe death-test child process with RLIMIT_AS
  // tightened so large strdup/vasprintf calls fail while sanitizers and
  // HeapLeakChecker are inactive.
  const std::string prev_style = GTEST_FLAG_GET(death_test_style);
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  absl::Cleanup restore_style = [&] {
    GTEST_FLAG_SET(death_test_style, prev_style);
  };

  const auto run_oom_checks = []() {
    herr_t rc_file = 0;
    herr_t rc_func = 0;
    herr_t rc_desc = 0;
    {
      ScopedErrorSilence silence;
      ScopedErrorStack stack(H5Ecreate_stack());
      if (!stack.valid()) {
        std::exit(1);
      }
      H5Epush2(stack.get(), "w.c", "fw", 1, H5E_ERR_CLS, H5E_ARGS, H5E_BADVALUE,
               "warmup");
      H5Eclear2(stack.get());

      constexpr size_t kLargeLen = 16ULL << 20;
      std::string large_str(kLargeLen, 'x');
      std::vector<void*> drain;
      drain.reserve(256);

      constexpr std::array<size_t, 8> kWarmSizes = {16,  32,  64,   128,
                                                    256, 512, 1024, 2048};
      for (size_t sz : kWarmSizes) {
        void* p = std::malloc(sz);
        std::free(p);
      }

      std::ifstream statm("/proc/self/statm");
      unsigned long vm_pages = 0;
      if (!(statm >> vm_pages)) {
        std::exit(2);
      }
      const long page_sz = ::sysconf(_SC_PAGESIZE);
      if (page_sz <= 0) {
        std::exit(3);
      }
      const rlim_t vm_bytes =
          static_cast<rlim_t>(vm_pages) * static_cast<rlim_t>(page_sz);

      struct rlimit old_lim{};
      if (::getrlimit(RLIMIT_AS, &old_lim) != 0) {
        std::exit(4);
      }
      struct rlimit tight_lim = old_lim;
      tight_lim.rlim_cur = std::min<rlim_t>(
          old_lim.rlim_max, vm_bytes + static_cast<rlim_t>(4ULL << 20));
      if (::setrlimit(RLIMIT_AS, &tight_lim) != 0) {
        std::exit(5);
      }

      for (int i = 0; i < 256; ++i) {
        void* p = std::malloc(kLargeLen + 1);
        if (p == nullptr) {
          break;
        }
        drain.push_back(p);
      }

      rc_file = H5Epush2(stack.get(), large_str.c_str(), "fn", 1, H5E_ERR_CLS,
                         H5E_ARGS, H5E_BADVALUE, "d");
      rc_func = H5Epush2(stack.get(), "f.c", large_str.c_str(), 2, H5E_ERR_CLS,
                         H5E_ARGS, H5E_BADVALUE, "d");
      rc_desc = H5Epush2(stack.get(), "f.c", "fn", 3, H5E_ERR_CLS, H5E_ARGS,
                         H5E_BADVALUE, "%s", large_str.c_str());

      ::setrlimit(RLIMIT_AS, &old_lim);
      for (void* p : drain) {
        std::free(p);
      }
    }
    if (rc_file >= 0 || rc_func >= 0 || rc_desc >= 0) {
      std::exit(6);
    }
    std::exit(0);
  };

  EXPECT_EXIT(run_oom_checks(), ::testing::ExitedWithCode(0), "");
}
#endif

}  // namespace
}  // namespace hdf5
