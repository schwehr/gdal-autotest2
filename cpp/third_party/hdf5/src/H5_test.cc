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
// Tests top-level HDF5 library initialization, termination, version checks,
// memory allocation, free lists, and runtime debug flags
// (third_party/hdf5/src/H5.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

// Captures pre-init version mismatch and H5dont_atexit checks in forked child
// processes before the parent process caches `checked = true` inside H5.c.
class PreInitEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    dont_atexit_ok_ = RunChild([]() {
      if (H5dont_atexit() < 0) return 1;
      if (H5dont_atexit() >= 0) return 2;
      return 0;
    });
    major_warn_ok_ = RunVersionChild(H5_VERS_MAJOR + 1, H5_VERS_MINOR,
                                     H5_VERS_RELEASE, "1");
    major_silent_ok_ = RunVersionChild(H5_VERS_MAJOR + 1, H5_VERS_MINOR,
                                       H5_VERS_RELEASE, "2");
    minor_exc_warn_ok_ =
        RunVersionChild(H5_VERS_MAJOR, 999, H5_VERS_RELEASE, "1");
    minor_exc_silent_ok_ =
        RunVersionChild(H5_VERS_MAJOR, 999, H5_VERS_RELEASE, "2");
    minor_fwd_warn_ok_ = RunVersionChild(H5_VERS_MAJOR, H5_VERS_MINOR + 1,
                                         H5_VERS_RELEASE, "1");
    minor_fwd_silent_ok_ = RunVersionChild(H5_VERS_MAJOR, H5_VERS_MINOR + 1,
                                           H5_VERS_RELEASE, "2");
    vers_info_warn_ok_ = RunChild([]() {
      unsetenv("HDF5_DISABLE_VERSION_CHECK");
      H5_lib_vers_info_g[0] = 'X';
      return H5check_version(H5_VERS_MAJOR, H5_VERS_MINOR, H5_VERS_RELEASE) >=
                     0
                 ? 0
                 : 1;
    });
  }

  static bool dont_atexit_ok() { return dont_atexit_ok_; }
  static bool major_warn_ok() { return major_warn_ok_; }
  static bool major_silent_ok() { return major_silent_ok_; }
  static bool minor_exc_warn_ok() { return minor_exc_warn_ok_; }
  static bool minor_exc_silent_ok() { return minor_exc_silent_ok_; }
  static bool minor_fwd_warn_ok() { return minor_fwd_warn_ok_; }
  static bool minor_fwd_silent_ok() { return minor_fwd_silent_ok_; }
  static bool vers_info_warn_ok() { return vers_info_warn_ok_; }

 private:
  template <typename Fn>
  static bool RunChild(Fn&& fn) {
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
      // Redirect stderr inside child to /dev/null to avoid noisy warnings.
      const int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
        close(devnull);
      }
      const int rc = fn();
      std::exit(rc);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }

  static bool RunVersionChild(unsigned maj, unsigned min, unsigned rel,
                              const char* disable_level) {
    return RunChild([=]() {
      if (setenv("HDF5_DISABLE_VERSION_CHECK", disable_level, 1) != 0) {
        return 1;
      }
      return H5check_version(maj, min, rel) >= 0 ? 0 : 2;
    });
  }

  static inline bool dont_atexit_ok_ = false;
  static inline bool major_warn_ok_ = false;
  static inline bool major_silent_ok_ = false;
  static inline bool minor_exc_warn_ok_ = false;
  static inline bool minor_exc_silent_ok_ = false;
  static inline bool minor_fwd_warn_ok_ = false;
  static inline bool minor_fwd_silent_ok_ = false;
  static inline bool vers_info_warn_ok_ = false;
};

[[maybe_unused]] const ::testing::Environment* const kPreInitEnv =
    ::testing::AddGlobalTestEnvironment(new PreInitEnvironment);

TEST(H5Test, PreInitDontAtexitAndVersionMismatchChecks) {
  EXPECT_TRUE(PreInitEnvironment::dont_atexit_ok());
  EXPECT_TRUE(PreInitEnvironment::major_warn_ok());
  EXPECT_TRUE(PreInitEnvironment::major_silent_ok());
  EXPECT_TRUE(PreInitEnvironment::minor_exc_warn_ok());
  EXPECT_TRUE(PreInitEnvironment::minor_exc_silent_ok());
  EXPECT_TRUE(PreInitEnvironment::minor_fwd_warn_ok());
  EXPECT_TRUE(PreInitEnvironment::minor_fwd_silent_ok());
  EXPECT_TRUE(PreInitEnvironment::vers_info_warn_ok());
}

TEST(H5Test, VersionMatchesCompiledHeaders) {
  unsigned maj = 0;
  unsigned min = 0;
  unsigned rel = 0;
  ASSERT_GE(H5get_libversion(&maj, &min, &rel), 0);
  EXPECT_EQ(maj, static_cast<unsigned>(H5_VERS_MAJOR));
  EXPECT_EQ(min, static_cast<unsigned>(H5_VERS_MINOR));
  EXPECT_EQ(rel, static_cast<unsigned>(H5_VERS_RELEASE));

  // Passing nullptr outputs is explicitly permitted.
  EXPECT_GE(H5get_libversion(nullptr, nullptr, nullptr), 0);
  EXPECT_GE(H5check_version(maj, min, rel), 0);
}

TEST(H5Test, IsLibraryThreadsafeReflectsGoogle3Config) {
  bool is_ts = false;
  ASSERT_GE(H5is_library_threadsafe(&is_ts), 0);
  EXPECT_TRUE(is_ts);

  ScopedErrorSilence silence;
  EXPECT_LT(H5is_library_threadsafe(nullptr), 0);
}

TEST(H5Test, IsLibraryTerminatingChecksOutputAndNull) {
  bool terminating = true;
  ASSERT_GE(H5is_library_terminating(&terminating), 0);
  EXPECT_FALSE(terminating);

  // :hdf5 compiles with -DNDEBUG, so assert(is_terminating) is elided and the
  // defensive null check returns < 0.
  ScopedErrorSilence silence;
  EXPECT_LT(H5is_library_terminating(nullptr), 0);
}

TEST(H5Test, AllocateResizeAndFreeMemory) {
  EXPECT_EQ(H5allocate_memory(0, false), nullptr);
  EXPECT_EQ(H5allocate_memory(0, true), nullptr);
  EXPECT_GE(H5free_memory(nullptr), 0);
  EXPECT_EQ(H5resize_memory(nullptr, 0), nullptr);

  void* raw = H5allocate_memory(16, false);
  ASSERT_NE(raw, nullptr);
  absl::Cleanup free_raw = [&raw] { H5free_memory(raw); };
  std::memset(raw, 0x5a, 16);

  uint8_t* zeroed = static_cast<uint8_t*>(H5allocate_memory(8, true));
  ASSERT_NE(zeroed, nullptr);
  absl::Cleanup free_zeroed = [&zeroed] { H5free_memory(zeroed); };
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(zeroed[i], 0);
  }
  zeroed[0] = 0x11;
  zeroed[7] = 0x22;

  uint8_t* grown = static_cast<uint8_t*>(H5resize_memory(zeroed, 16));
  ASSERT_NE(grown, nullptr);
  zeroed = grown;
  EXPECT_EQ(zeroed[0], 0x11);
  EXPECT_EQ(zeroed[7], 0x22);
}

TEST(H5Test, FreeListLimitsSizesAndGarbageCollect) {
  ASSERT_GE(H5open(), 0);
  absl::Cleanup restore_limits = [] {
    H5set_free_list_limits(-1, -1, -1, -1, -1, -1);
  };

  EXPECT_GE(H5set_free_list_limits(0, 0, 0, 0, 0, 0), 0);
  EXPECT_GE(H5garbage_collect(), 0);

  constexpr size_t kSentinel = static_cast<size_t>(-1);
  size_t reg = kSentinel;
  size_t arr = kSentinel;
  size_t blk = kSentinel;
  size_t fac = kSentinel;
  ASSERT_GE(H5get_free_list_sizes(&reg, &arr, &blk, &fac), 0);
  EXPECT_NE(reg, kSentinel);
  EXPECT_NE(arr, kSentinel);
  EXPECT_NE(blk, kSentinel);
  EXPECT_NE(fac, kSentinel);

  EXPECT_GE(H5get_free_list_sizes(nullptr, nullptr, nullptr, nullptr), 0);
  EXPECT_GE(H5set_free_list_limits(1024, 256, 1024, 256, 1024, 256), 0);
  EXPECT_GE(H5garbage_collect(), 0);
}

TEST(H5Test, AtcloseNullCallbackFails) {
  ScopedErrorSilence silence;
  EXPECT_LT(H5atclose(nullptr, nullptr), 0);
}

TEST(H5Test, CloseInvokesAtcloseCallbacksAndReopens) {
  ASSERT_GE(H5open(), 0);

  struct CallbackState {
    int calls = 0;
    bool saw_terminating = false;
  } state;

  auto cb = [](void* ctx) {
    auto* st = static_cast<CallbackState*>(ctx);
    ++st->calls;
    bool terminating = false;
    if (H5is_library_terminating(&terminating) >= 0) {
      st->saw_terminating = terminating;
    }
  };

  ASSERT_GE(H5atclose(cb, &state), 0);
  (void)H5dont_atexit();
  EXPECT_LT(H5dont_atexit(), 0);

  ASSERT_GE(H5close(), 0);
  EXPECT_EQ(state.calls, 1);
  EXPECT_TRUE(state.saw_terminating);

  // Calling H5close() when already shut down is a safe no-op.
  EXPECT_GE(H5close(), 0);
  EXPECT_EQ(state.calls, 1);

  // Re-initialize library for subsequent tests in randomized Testify runs.
  ASSERT_GE(H5open(), 0);
}

TEST(H5Test, DebugMaskEnvVarExercisesStreamAndPackageFlags) {
  const int devnull_fd = open("/dev/null", O_WRONLY);
  ASSERT_GE(devnull_fd, 0);
  bool fd_owned_by_hdf5 = false;
  absl::Cleanup cleanup_fd = [&] {
    if (!fd_owned_by_hdf5) {
      close(devnull_fd);
    }
  };

  const char* prev_env = getenv("HDF5_DEBUG");
  std::string saved_env = prev_env ? prev_env : "";
  const bool had_env = (prev_env != nullptr);
  absl::Cleanup restore_env = [&] {
    if (had_env) {
      setenv("HDF5_DEBUG", saved_env.c_str(), 1);
    } else {
      unsetenv("HDF5_DEBUG");
    }
    H5close();
    H5open();
  };

  // Close first so H5open() runs H5_init_library() -> H5__debug_mask().
  ASSERT_GE(H5close(), 0);
  const std::string mask =
      std::to_string(devnull_fd) +
      ",+all,-all,+trace,-trace,+ttop,-ttop,+ttimes,-ttimes,+a,-a,unknown_pkg";
  ASSERT_EQ(setenv("HDF5_DEBUG", mask.c_str(), 1), 0);

  ASSERT_GE(H5open(), 0);
  // H5__debug_mask called HDfdopen(devnull_fd, "w") and stored it in
  // H5_debug_g.open_stream; H5close() will fclose(stream) and own devnull_fd.
  fd_owned_by_hdf5 = (H5_debug_g.open_stream != nullptr);

  // Exercise invalid fd (negative integer parsed by strtol? Wait: '-' is
  // treated as clear flag, while huge fd like 999999 fails HDfdopen).
  ASSERT_GE(H5close(), 0);
  ASSERT_EQ(setenv("HDF5_DEBUG", "999999", 1), 0);
  ASSERT_GE(H5open(), 0);
}

TEST(H5Test, UserCbPrepareAndRestore) {
  ASSERT_GE(H5open(), 0);
  H5_user_cb_state_t cb_state{};
  ASSERT_GE(H5_user_cb_prepare(&cb_state), 0);
  EXPECT_GE(H5_user_cb_restore(&cb_state), 0);
}

}  // namespace
}  // namespace hdf5
