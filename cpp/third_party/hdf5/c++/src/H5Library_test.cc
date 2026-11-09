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
// Unit tests for third_party/hdf5/c++/src/H5Library.cpp.
//
// Note: Do not add tests that create HDF5 objects or touch PredType/PropList
// singletons to this binary. H5Library::close() calls H5close(), leaving any
// pre-existing singleton hid_t handles stale across reopen and process exit.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5Library.cpp

#include <dlfcn.h>

#include <atomic>
#include <cstdlib>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/base/config.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"

// Sanitizers define their own __cxa_atexit interceptors, so the fault-injection
// interposer for initH5cpp() only compiles in non-sanitizer builds (including
// coverage, fastbuild, opt, arm, and no_modules).
#if !defined(ABSL_HAVE_ADDRESS_SANITIZER) &&                                  \
    !defined(ABSL_HAVE_MEMORY_SANITIZER) &&                                   \
    !defined(ABSL_HAVE_THREAD_SANITIZER) && !defined(__SANITIZE_ADDRESS__) && \
    !defined(__SANITIZE_MEMORY__) && !defined(__SANITIZE_THREAD__) &&         \
    !__has_feature(address_sanitizer) && !__has_feature(memory_sanitizer) &&  \
    !__has_feature(thread_sanitizer)
#define HDF5_ENABLE_CXA_ATEXIT_FAULT_INJECTION 1
#else
#define HDF5_ENABLE_CXA_ATEXIT_FAULT_INJECTION 0
#endif

#if HDF5_ENABLE_CXA_ATEXIT_FAULT_INJECTION
namespace {

using CxaAtexitFn = int (*)(void (*)(void*), void*, void*);

constinit std::atomic<CxaAtexitFn> g_real_cxa_atexit{nullptr};
constinit std::atomic<int> g_fail_atexit_on_call{0};
constinit std::atomic<int> g_atexit_call_count{0};

}  // namespace

// Process-global interposer relying on glibc routing std::atexit() through
// __cxa_atexit (via libc_nonshared.a).
extern "C" __attribute__((visibility("default"))) int __cxa_atexit(
    void (*func)(void*), void* arg, void* dso) {
  const int fail_target = g_fail_atexit_on_call.load(std::memory_order_relaxed);
  if (fail_target > 0) {
    const int call_num =
        g_atexit_call_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (call_num == fail_target) {
      g_fail_atexit_on_call.store(0, std::memory_order_relaxed);
      return -1;
    }
    // Suppress duplicate deleteConstants() registrations while probing later
    // std::atexit() error paths in H5Library::initH5cpp().
    return 0;
  }

  CxaAtexitFn real_fn = g_real_cxa_atexit.load(std::memory_order_relaxed);
  if (real_fn == nullptr) {
    real_fn = reinterpret_cast<CxaAtexitFn>(dlsym(RTLD_NEXT, "__cxa_atexit"));
    if (real_fn == nullptr) {
      std::abort();
    }
    g_real_cxa_atexit.store(real_fn, std::memory_order_relaxed);
  }
  return real_fn(func, arg, dso);
}
#endif  // HDF5_ENABLE_CXA_ATEXIT_FAULT_INJECTION

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

TEST(H5LibraryTest, OpenDontAtExitGarbageCollectAndCloseReopen) {
  H5::H5Library::open();
  H5::H5Library::dontAtExit();
  H5::H5Library::garbageCollect();
  H5::H5Library::close();
  // Re-open immediately so any subsequent tests remain order-independent.
  H5::H5Library::open();
}

TEST(H5LibraryTest, GetLibVersionAndCheckVersionMatch) {
  unsigned major = 0;
  unsigned minor = 0;
  unsigned release = 0;
  H5::H5Library::getLibVersion(major, minor, release);
  EXPECT_GE(major, 1u);
  // Note: checkVersion() delegates to H5check_version(), which calls abort()
  // on version mismatch rather than returning an error code.
  H5::H5Library::checkVersion(major, minor, release);
}

TEST(H5LibraryTest, SetFreeListLimitsValidAndReset) {
  H5::H5Library::setFreeListLimits(1024 * 1024, 64 * 1024, 1024 * 1024,
                                   64 * 1024, 1024 * 1024, 64 * 1024);
  // Upstream H5FL_set_free_list_limits casts non-(-1) values to size_t and
  // treats -1 as unlimited. Reset to unlimited so sibling tests stay clean.
  H5::H5Library::setFreeListLimits(-2, 0, -1, -1, -1, -1);
  H5::H5Library::setFreeListLimits(-1, -1, -1, -1, -1, -1);
}

#if HDF5_ENABLE_CXA_ATEXIT_FAULT_INJECTION
TEST(H5LibraryTest, InitH5cppThrowsWhenAtExitRegistrationFails) {
  // Ensure global initialization has completed before arming the interposer.
  H5::H5Library::open();

  absl::Cleanup disarm = []() {
    g_fail_atexit_on_call.store(0, std::memory_order_relaxed);
  };

  // Pinned to the 12 std::atexit() registrations in H5Library::initH5cpp()
  // (third_party/hdf5/c++/src/H5Library.cpp:169-221).
  constexpr int kNumAtExitRegistrations = 12;
  for (int target_call = 1; target_call <= kNumAtExitRegistrations;
       ++target_call) {
    g_atexit_call_count.store(0, std::memory_order_relaxed);
    g_fail_atexit_on_call.store(target_call, std::memory_order_relaxed);
    try {
      H5::H5Library::initH5cpp();
      FAIL() << "Expected LibraryIException on atexit call " << target_call;
    } catch (const H5::LibraryIException& ex) {
      EXPECT_THAT(ex.getDetailMsg(), HasSubstr("Registering "));
      EXPECT_THAT(ex.getDetailMsg(), HasSubstr(" failed"));
    }
    g_fail_atexit_on_call.store(0, std::memory_order_relaxed);
  }
}
#endif  // HDF5_ENABLE_CXA_ATEXIT_FAULT_INJECTION

// Excluded under TSan: mutating process-wide `environ` via setenv/unsetenv
// races with concurrent getenv() calls in runtime background threads.
#if !defined(ABSL_HAVE_THREAD_SANITIZER) && !defined(__SANITIZE_THREAD__) && \
    !__has_feature(thread_sanitizer)
TEST(H5LibraryTest, LibraryInitFailureThrowsLibraryIException) {
  ScopedQuietHdf5Errors quiet;
  H5::H5Library::open();

  const char* prev_vol = getenv("HDF5_VOL_CONNECTOR");
  const bool had_prev_vol = (prev_vol != nullptr);
  const std::string saved_vol = had_prev_vol ? prev_vol : "";

  absl::Cleanup restore = [&]() {
    if (had_prev_vol) {
      setenv("HDF5_VOL_CONNECTOR", saved_vol.c_str(), 1);
    } else {
      unsetenv("HDF5_VOL_CONNECTOR");
    }
    H5::H5Library::close();
    H5::H5Library::open();
  };

  H5::H5Library::close();
  ASSERT_EQ(setenv("HDF5_VOL_CONNECTOR", "   ", 1), 0);

  EXPECT_THROW(H5::H5Library::open(), H5::LibraryIException);
  H5::H5Library::close();

  unsigned maj = 0;
  unsigned min = 0;
  unsigned rel = 0;
  EXPECT_THROW(H5::H5Library::getLibVersion(maj, min, rel),
               H5::LibraryIException);
  H5::H5Library::close();

  EXPECT_THROW(H5::H5Library::garbageCollect(), H5::LibraryIException);
  H5::H5Library::close();

  EXPECT_THROW(H5::H5Library::setFreeListLimits(-1, -1, -1, -1, -1, -1),
               H5::LibraryIException);
}
#endif

}  // namespace
}  // namespace hdf5
