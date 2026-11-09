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
// Tests deprecated HDF5 v1.6 error handling APIs: H5Eget_major, H5Eget_minor,
// H5Epush1, H5Eclear1, H5Eprint1, H5Ewalk1, H5Eget_auto1, and H5Eset_auto1
// (third_party/hdf5/src/H5Edeprec.c). Note: google3 compiles HDF5 with
// H5_NO_DEPRECATED_SYMBOLS undefined, -DNDEBUG, and H5_HAVE_THREADSAFE_API.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Edeprec.c

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
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer) || \
    __has_feature(hwaddress_sanitizer)
#define H5E_DEPREC_TEST_HAS_ASAN_OR_LSAN 1
#endif
#if __has_feature(memory_sanitizer) || __has_feature(thread_sanitizer)
#define H5E_DEPREC_TEST_HAS_OTHER_SAN 1
#endif
#endif
#if !defined(H5E_DEPREC_TEST_HAS_ASAN_OR_LSAN) &&                   \
    (defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__) || \
     defined(LEAK_SANITIZER) || defined(HWADDRESS_SANITIZER) ||     \
     defined(__SANITIZE_HWADDRESS__))
#define H5E_DEPREC_TEST_HAS_ASAN_OR_LSAN 1
#endif
#if !defined(H5E_DEPREC_TEST_HAS_OTHER_SAN) &&                 \
    (defined(MEMORY_SANITIZER) || defined(THREAD_SANITIZER) || \
     defined(__SANITIZE_THREAD__))
#define H5E_DEPREC_TEST_HAS_OTHER_SAN 1
#endif

// `:hdf5` compiles with -DNDEBUG; match internal struct layouts across
// H5private.h -> H5CXprivate.h -> H5ACprivate.h -> H5Cprivate.h.
#ifndef NDEBUG
#define H5EDEPREC_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5E_FRIEND
#include "third_party/hdf5/src/H5Epkg.h"
}  // extern "C"
#ifdef H5EDEPREC_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5EDEPREC_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

using ScopedErrorClass = ScopedH5Id<H5Eunregister_class>;
using ScopedErrorMsg = ScopedH5Id<H5Eclose_msg>;

class ScopedDefaultAutoRestore {
 public:
  ScopedDefaultAutoRestore() {
    saved_ = (H5Eget_auto2(H5E_DEFAULT, &prev_func_, &prev_data_) >= 0);
    H5Eclear1();
  }

  ~ScopedDefaultAutoRestore() {
    H5Eclear1();
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

struct Walk1Record {
  H5E_major_t maj_num = H5I_INVALID_HID;
  H5E_minor_t min_num = H5I_INVALID_HID;
  int n = 0;
  unsigned line = 0;
  std::string func_name;
  std::string file_name;
  std::string desc;
};

herr_t CollectWalk1Cb(int n, H5E_error1_t* err_desc, void* client_data) {
  auto* records = static_cast<std::vector<Walk1Record>*>(client_data);
  if (err_desc == nullptr || records == nullptr) {
    return -1;
  }
  records->push_back({
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

TEST(H5EdeprecTest, GetMajorAndMinorStrings) {
  ScopedErrorSilence silence;

  ScopedErrorClass cls(H5Eregister_class("DepCls", "DepLib", "1.0"));
  ASSERT_NE(H5I_INVALID_HID, cls.get());
  ScopedErrorMsg custom_maj(
      H5Ecreate_msg(cls.get(), H5E_MAJOR, "Custom major msg"));
  ScopedErrorMsg custom_min(
      H5Ecreate_msg(cls.get(), H5E_MINOR, "Custom minor msg"));
  ASSERT_NE(H5I_INVALID_HID, custom_maj.get());
  ASSERT_NE(H5I_INVALID_HID, custom_min.get());

  // 1. Retrieve built-in and custom major error strings; caller owns heap mem.
  char* lib_maj = H5Eget_major(H5E_ARGS);
  ASSERT_NE(lib_maj, nullptr);
  absl::Cleanup free_lib_maj = [lib_maj] { H5free_memory(lib_maj); };
  EXPECT_STREQ("Invalid arguments to routine", lib_maj);

  char* app_maj = H5Eget_major(custom_maj.get());
  ASSERT_NE(app_maj, nullptr);
  absl::Cleanup free_app_maj = [app_maj] { H5free_memory(app_maj); };
  EXPECT_STREQ("Custom major msg", app_maj);

  // 2. Retrieve built-in and custom minor error strings.
  char* lib_min = H5Eget_minor(H5E_BADVALUE);
  ASSERT_NE(lib_min, nullptr);
  absl::Cleanup free_lib_min = [lib_min] { H5free_memory(lib_min); };
  EXPECT_STREQ("Bad value", lib_min);

  char* app_min = H5Eget_minor(custom_min.get());
  ASSERT_NE(app_min, nullptr);
  absl::Cleanup free_app_min = [app_min] { H5free_memory(app_min); };
  EXPECT_STREQ("Custom minor msg", app_min);

  // 3. Error paths: invalid IDs and mismatched major/minor message types.
  EXPECT_EQ(H5Eget_major(H5I_INVALID_HID), nullptr);
  EXPECT_EQ(H5Eget_major(H5E_BADVALUE), nullptr);
  EXPECT_EQ(H5Eget_major(cls.get()), nullptr);

  EXPECT_EQ(H5Eget_minor(H5I_INVALID_HID), nullptr);
  EXPECT_EQ(H5Eget_minor(H5E_ARGS), nullptr);
  EXPECT_EQ(H5Eget_minor(cls.get()), nullptr);
}

TEST(H5EdeprecTest, Push1Walk1Print1AndClear1) {
  ScopedErrorSilence silence;

  ScopedErrorClass cls(H5Eregister_class("Walk1Cls", "Walk1Lib", "1.0"));
  ASSERT_NE(H5I_INVALID_HID, cls.get());
  ScopedErrorMsg custom_maj(H5Ecreate_msg(cls.get(), H5E_MAJOR, "Dep major"));
  ScopedErrorMsg custom_min(H5Ecreate_msg(cls.get(), H5E_MINOR, "Dep minor"));
  ASSERT_NE(H5I_INVALID_HID, custom_maj.get());
  ASSERT_NE(H5I_INVALID_HID, custom_min.get());

  absl::Cleanup clear_default = [] { H5Eclear1(); };

  // 1. Push both library and custom major/minor errors via H5Epush1.
  ASSERT_GE(
      H5Epush1("f1.c", "fn1", 10, H5E_ARGS, H5E_BADVALUE, "literal %s desc"),
      0);
  ASSERT_GE(H5Epush1("f2.c", "fn2", 20, custom_maj.get(), custom_min.get(), ""),
            0);
  EXPECT_EQ(H5Eget_num(H5E_DEFAULT), 2);

  // 2. Walk upward and downward with H5Ewalk1.
  std::vector<Walk1Record> upward;
  ASSERT_GE(H5Ewalk1(H5E_WALK_UPWARD, CollectWalk1Cb, &upward), 0);
  ASSERT_EQ(upward.size(), 2);
  EXPECT_EQ(upward[0].n, 0);
  EXPECT_EQ(upward[0].func_name, "fn1");
  EXPECT_EQ(upward[0].desc, "literal %s desc");
  EXPECT_EQ(upward[1].n, 1);
  EXPECT_EQ(upward[1].func_name, "fn2");
  EXPECT_EQ(upward[1].desc, "");

  std::vector<Walk1Record> downward;
  ASSERT_GE(H5Ewalk1(H5E_WALK_DOWNWARD, CollectWalk1Cb, &downward), 0);
  ASSERT_EQ(downward.size(), 2);
  EXPECT_EQ(downward[0].func_name, "fn2");
  EXPECT_EQ(downward[1].func_name, "fn1");

  // 3. Callback returning < 0 makes H5Ewalk1 fail; nullptr cb is a no-op.
  EXPECT_GE(H5Ewalk1(H5E_WALK_UPWARD, nullptr, nullptr), 0);
  auto fail_cb = [](int, H5E_error1_t*, void*) -> herr_t { return -1; };
  EXPECT_LT(H5Ewalk1(H5E_WALK_UPWARD, fail_cb, nullptr), 0);

  // 4. Print stack with H5Eprint1 into an in-memory FILE*.
  std::array<char, 4096> buf{};
  FILE* stream = fmemopen(buf.data(), buf.size(), "w+");
  ASSERT_NE(stream, nullptr);
  absl::Cleanup close_stream = [&] { std::fclose(stream); };

  ASSERT_GE(H5Eprint1(stream), 0);
  ASSERT_EQ(std::fflush(stream), 0);

  const std::string_view printed(buf.data());
  EXPECT_THAT(printed, HasSubstr("Walk1Cls-DIAG: Error detected in Walk1Lib"));
  EXPECT_THAT(printed, HasSubstr("HDF5-DIAG: Error detected in HDF5"));
  EXPECT_THAT(printed, HasSubstr("literal %s desc"));
  EXPECT_THAT(printed, HasSubstr("Dep major"));

  // 5. Clear with H5Eclear1 and verify print with stream == nullptr is silent
  //    on an empty stack.
  EXPECT_GE(H5Eclear1(), 0);
  EXPECT_EQ(H5Eget_num(H5E_DEFAULT), 0);
  EXPECT_GE(H5Eprint1(nullptr), 0);

  // 6. H5Epush1 is ignored while H5E_DEFAULT is paused.
  ASSERT_GE(H5Epause_stack(H5E_DEFAULT), 0);
  absl::Cleanup resume_default = [] { H5Eresume_stack(H5E_DEFAULT); };
  EXPECT_GE(H5Epush1("paused.c", "fp", 99, H5E_ARGS, H5E_BADVALUE, "ignored"),
            0);
  EXPECT_EQ(H5Eget_num(H5E_DEFAULT), 0);
}

TEST(H5EdeprecTest, Print1FailsWhenStackEntryIsNotErrorMsg) {
  ScopedErrorSilence silence;
  absl::Cleanup clear_default = [] { H5Eclear1(); };

  // H5Epush1 increments refcount via H5I_inc_ref without checking
  // H5I_ERROR_MSG, so pushing a valid non-message ID causes H5E__walk1_cb ->
  // H5Eprint1 to fail cleanly while H5Eclear1 releases refcounts without
  // leaking.
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ASSERT_GE(
      H5Epush1("bad.c", "fbad", 1, space.get(), H5E_BADVALUE, "not a msg"), 0);

  std::array<char, 512> buf{};
  FILE* stream = fmemopen(buf.data(), buf.size(), "w+");
  ASSERT_NE(stream, nullptr);
  absl::Cleanup close_stream = [&] { std::fclose(stream); };

  EXPECT_LT(H5Eprint1(stream), 0);
  EXPECT_GE(H5Eclear1(), 0);
}

TEST(H5EdeprecTest, GetAndSetAuto1LifecycleAndMixing) {
  ScopedDefaultAutoRestore restore;

  // 1. Default handler allows H5Eget_auto1 (is_default == true).
  H5E_auto1_t fn1 = nullptr;
  void* data = nullptr;
  ASSERT_GE(H5Eget_auto1(&fn1, &data), 0);
  EXPECT_NE(fn1, nullptr);
  EXPECT_GE(H5Eget_auto1(nullptr, nullptr), 0);

  // 2. Setting a custom v1 auto callback invokes it on HDF5 API error.
  struct Auto1State {
    int calls = 0;
  } state;
  auto custom_auto1 = [](void* client_data) -> herr_t {
    auto* st = static_cast<Auto1State*>(client_data);
    ++st->calls;
    return 0;
  };
  ASSERT_GE(H5Eset_auto1(custom_auto1, &state), 0);

  ASSERT_GE(H5Eget_auto1(&fn1, &data), 0);
  EXPECT_EQ(fn1, static_cast<H5E_auto1_t>(custom_auto1));
  EXPECT_EQ(data, &state);

  EXPECT_LT(H5Eclose_msg(H5I_INVALID_HID), 0);
  EXPECT_EQ(state.calls, 1);

  // 3. Passing H5Eprint1 marks is_default = true; passing nullptr disables it.
  ASSERT_GE(H5Eset_auto1(reinterpret_cast<H5E_auto1_t>(H5Eprint1), stderr), 0);
  unsigned is_v2 = 1;
  ASSERT_GE(H5Eauto_is_v2(H5E_DEFAULT, &is_v2), 0);
  EXPECT_EQ(is_v2, 0u);

  ASSERT_GE(H5Eset_auto1(nullptr, nullptr), 0);
  EXPECT_LT(H5Eclose_msg(H5I_INVALID_HID), 0);
  EXPECT_EQ(state.calls, 1);

  // 4. Mixing custom H5Eset_auto2 with H5Eget_auto1 fails.
  auto custom_auto2 = [](hid_t, void*) -> herr_t { return 0; };
  ASSERT_GE(H5Eset_auto2(H5E_DEFAULT, custom_auto2, nullptr), 0);
  EXPECT_LT(H5Eget_auto1(&fn1, &data), 0);
}

TEST(H5EdeprecTest, Clear1FailsCleanlyOnInvalidEntryId) {
  ScopedErrorSilence silence;

  const hid_t stale_cls = H5Eregister_class("ClrCls", "ClrLib", "1.0");
  ASSERT_NE(H5I_INVALID_HID, stale_cls);
  const hid_t stale_min = H5Ecreate_msg(stale_cls, H5E_MINOR, "ClrMin");
  ASSERT_NE(H5I_INVALID_HID, stale_min);
  ASSERT_GE(H5Eunregister_class(stale_cls), 0);

  ASSERT_GE(H5Epush1("c.c", "fc", 1, H5E_ARGS, H5E_BADVALUE, "entry"), 0);
  H5E_stack_t* def_stk = H5E__get_my_stack();
  ASSERT_NE(def_stk, nullptr);
  ASSERT_EQ(def_stk->nused, 1u);

  def_stk->entries[0].err.min_num = stale_min;
  absl::Cleanup restore = [&] {
    def_stk->entries[0].err.min_num = H5E_BADVALUE_g;
    H5Eclear1();
  };
  EXPECT_LT(H5Eclear1(), 0);
}

#if !defined(H5E_DEPREC_TEST_HAS_ASAN_OR_LSAN)
TEST(H5EdeprecTest, Push1FailsOnInvalidMajorOrMinorIds) {
  // Run in an isolated threadsafe death-test child process: H5Epush1 duplicates
  // file/func before incrementing maj/min refcounts and leaks them on
  // H5I_inc_ref failure upstream, while HeapLeakChecker disables itself when
  // --gunit_internal_run_death_test is present.
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
              H5Eregister_class("DepStale", "DepLib", "1.0");
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
          if (H5Epush1("p.c", "fp", 1, stale_maj, H5E_BADVALUE, "bad maj") >=
              0) {
            std::exit(4);
          }
          if (H5Epush1("p.c", "fp", 2, H5E_ARGS, stale_min, "bad min") >= 0) {
            std::exit(5);
          }
        }
        std::exit(0);
      },
      ::testing::ExitedWithCode(0), "");
}
#endif

#if !defined(H5E_DEPREC_TEST_HAS_ASAN_OR_LSAN) && \
    !defined(H5E_DEPREC_TEST_HAS_OTHER_SAN)
TEST(H5EdeprecTest, Push1HandlesStrdupAllocationFailure) {
  // Run in an isolated threadsafe death-test child process so RLIMIT_AS can be
  // tightened without affecting HeapLeakChecker's LowLevelAlloc arenas.
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
      H5Eclear1();
      H5Epush1("w.c", "fw", 1, H5E_ARGS, H5E_BADVALUE, "warmup");
      H5Eclear1();

      constexpr size_t kLargeLen = 16ULL << 20;
      std::string large_str(kLargeLen, 'x');
      std::vector<void*> drain;
      drain.reserve(256);

      // Warm small size classes so HGOTO_ERROR and short strings succeed
      // after large spans are exhausted.
      constexpr std::array<size_t, 8> kWarmSizes = {16,  32,  64,   128,
                                                    256, 512, 1024, 2048};
      for (size_t sz : kWarmSizes) {
        void* p = std::malloc(sz);
        std::free(p);
      }

      std::ifstream statm("/proc/self/statm");
      unsigned long vm_pages = 0;
      if (!(statm >> vm_pages)) {
        std::exit(1);
      }
      const long page_sz = ::sysconf(_SC_PAGESIZE);
      if (page_sz <= 0) {
        std::exit(2);
      }
      const rlim_t vm_bytes =
          static_cast<rlim_t>(vm_pages) * static_cast<rlim_t>(page_sz);

      struct rlimit old_lim{};
      if (::getrlimit(RLIMIT_AS, &old_lim) != 0) {
        std::exit(3);
      }
      struct rlimit tight_lim = old_lim;
      tight_lim.rlim_cur = std::min<rlim_t>(
          old_lim.rlim_max, vm_bytes + static_cast<rlim_t>(4ULL << 20));
      if (::setrlimit(RLIMIT_AS, &tight_lim) != 0) {
        std::exit(4);
      }

      // Exhaust any pre-mapped free spans in the allocator page heap.
      for (int i = 0; i < 256; ++i) {
        void* p = std::malloc(kLargeLen + 1);
        if (p == nullptr) {
          break;
        }
        drain.push_back(p);
      }

      rc_file =
          H5Epush1(large_str.c_str(), "fn", 1, H5E_ARGS, H5E_BADVALUE, "d");
      rc_func =
          H5Epush1("f.c", large_str.c_str(), 2, H5E_ARGS, H5E_BADVALUE, "d");
      rc_desc =
          H5Epush1("f.c", "fn", 3, H5E_ARGS, H5E_BADVALUE, large_str.c_str());

      ::setrlimit(RLIMIT_AS, &old_lim);
      for (void* p : drain) {
        std::free(p);
      }
    }
    if (rc_file >= 0 || rc_func >= 0 || rc_desc >= 0) {
      std::exit(5);
    }
    std::exit(0);
  };

  EXPECT_EXIT(run_oom_checks(), ::testing::ExitedWithCode(0), "");
}
#endif

}  // namespace
}  // namespace hdf5
