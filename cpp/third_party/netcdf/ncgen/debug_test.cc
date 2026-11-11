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

// Unit tests for //third_party/netcdf/ncgen/debug.c.
//
// Exercises trace toggles, debug logging, checked memory allocation wrappers,
// out-of-memory handling, and panic/abort paths cleanly across sanitizers.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncgen/debug.c

#if !defined(ADDRESS_SANITIZER) && !defined(MEMORY_SANITIZER) && \
    !defined(THREAD_SANITIZER)
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#endif
#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#if !defined(ADDRESS_SANITIZER) && !defined(MEMORY_SANITIZER) && \
    !defined(THREAD_SANITIZER)
#include <limits>
#endif
#include <string>
#include <utility>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/ncgen_main_shim.h"

namespace netcdf {
namespace {

class DeathTestStyleGuard {
 public:
  DeathTestStyleGuard() : prev_(GTEST_FLAG_GET(death_test_style)) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
  }
  ~DeathTestStyleGuard() { GTEST_FLAG_SET(death_test_style, prev_); }

 private:
  std::string prev_;
};

#if !defined(ADDRESS_SANITIZER) && !defined(MEMORY_SANITIZER) && \
    !defined(THREAD_SANITIZER)
sigjmp_buf g_abort_jmp_buf;
volatile sig_atomic_t g_abort_jmp_active = 0;

void HandleAbortForCoverage(int /*sig*/) {
  if (g_abort_jmp_active) {
    siglongjmp(g_abort_jmp_buf, 1);
  }
}

template <typename Fn>
::testing::AssertionResult ExpectAbortInForkedChild(Fn&& fn) {
  return RunInForkedChild([&]() -> bool {
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
    }
    struct sigaction sa{};
    sa.sa_handler = HandleAbortForCoverage;
    sigemptyset(&sa.sa_mask);
    struct sigaction prev_sa{};
    if (::sigaction(SIGABRT, &sa, &prev_sa) != 0) {
      return false;
    }
    g_abort_jmp_active = 1;
    if (sigsetjmp(g_abort_jmp_buf, 1) == 0) {
      std::forward<Fn>(fn)();
      g_abort_jmp_active = 0;
      ::sigaction(SIGABRT, &prev_sa, nullptr);
      return false;
    }
    g_abort_jmp_active = 0;
    ::sigaction(SIGABRT, &prev_sa, nullptr);
    return true;
  });
}
#endif

template <typename Fn>
bool RunCapturingStderrInChild(Fn&& fn, std::string* captured_out) {
  int pipefd[2] = {-1, -1};
  if (::pipe(pipefd) != 0) {
    return false;
  }
  const int saved_stderr = ::dup(STDERR_FILENO);
  if (saved_stderr < 0) {
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    return false;
  }
  std::fflush(stderr);
  ::dup2(pipefd[1], STDERR_FILENO);
  ::close(pipefd[1]);

  std::forward<Fn>(fn)();
  std::fflush(stderr);

  ::dup2(saved_stderr, STDERR_FILENO);
  ::close(saved_stderr);

  captured_out->clear();
  char buf[1024];
  while (true) {
    const ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
    if (n > 0) {
      captured_out->append(buf, static_cast<size_t>(n));
    } else {
      break;
    }
  }
  ::close(pipefd[0]);
  return true;
}

TEST(DebugTest, FdebugRespectsDebugFlag) {
  const auto restore = absl::MakeCleanup([]() {
    debug = 0;
    settrace(0);
  });

  ASSERT_TRUE(RunInForkedChild([]() -> bool {
    std::string out;
    debug = 0;
    if (!RunCapturingStderrInChild([]() { fdebug("ignored %d", 1); }, &out)) {
      return false;
    }
    if (!out.empty()) {
      return false;
    }

    debug = 1;
    if (!RunCapturingStderrInChild(
            []() { fdebug("dbg val=%d str=%s", 42, "ok"); }, &out)) {
      return false;
    }
    return out == "dbg val=42 str=ok";
  }));
}

TEST(DebugTest, AllocCallocReallocStrdupAndFreeWithoutTrace) {
  const auto restore = absl::MakeCleanup([]() {
    debug = 0;
    settrace(0);
  });
  EXPECT_EQ(1, settrace(0));

  void* raw = chkmalloc(16);
  ASSERT_NE(nullptr, raw);
  const absl::Cleanup free_raw([&]() {
    if (raw != nullptr) {
      chkfree(raw);
    }
  });
  std::memset(raw, 0x5A, 16);

  unsigned char* zeroed = static_cast<unsigned char*>(chkcalloc(32));
  ASSERT_NE(nullptr, zeroed);
  const absl::Cleanup free_zeroed([&]() { chkfree(zeroed); });
  for (size_t i = 0; i < 32; ++i) {
    EXPECT_EQ(0, zeroed[i]);
  }

  // Reallocate at same size (same pointer path) and then grow substantially
  // (moved pointer path).
  raw = chkrealloc(raw, 16);
  ASSERT_NE(nullptr, raw);
  raw = chkrealloc(raw, 4 * 1024 * 1024);
  ASSERT_NE(nullptr, raw);
  EXPECT_EQ(0x5A, static_cast<unsigned char*>(raw)[0]);

  char* dup = chkstrdup("netcdf_debug_token");
  ASSERT_NE(nullptr, dup);
  const absl::Cleanup free_dup([&]() { chkfree(dup); });
  EXPECT_STREQ("netcdf_debug_token", dup);
}

TEST(DebugTest, TracingEmitsMemoryDiagnosticsToStderr) {
  ASSERT_TRUE(RunInForkedChild([]() -> bool {
    std::string out;
    const bool ok = RunCapturingStderrInChild(
        []() {
          settrace(1);
          void* p = chkmalloc(16);
          void* q = chkcalloc(24);
          void* r = chkrealloc(p, 4 * 1024 * 1024);
          char* s = chkstrdup("traced");
          chkfree(s);
          chkfree(q);
          chkfree(r);
          settrace(0);
        },
        &out);
    if (!ok) {
      return false;
    }
    return out.find("X: malloc:") != std::string::npos &&
           out.find("X: calloc:") != std::string::npos &&
           out.find("X: realloc:") != std::string::npos &&
           out.find("X: strdup:") != std::string::npos &&
           out.find("X: free:") != std::string::npos;
  }));
}

TEST(DebugTest, NullPointersAndPanicAbortProcess) {
  DeathTestStyleGuard death_guard;

  EXPECT_DEATH(chkfree(nullptr), "free: null memory");
  EXPECT_DEATH(chkstrdup(nullptr), "strdup: null argument");
  EXPECT_DEATH(panic("fatal %s %d", "err", 7), "fatal err 7");
  EXPECT_DEATH(panic(nullptr), "panic");

#if !defined(ADDRESS_SANITIZER) && !defined(MEMORY_SANITIZER) && \
    !defined(THREAD_SANITIZER)
  // Cover abort() paths inside forked children so gcov counters flush cleanly.
  EXPECT_TRUE(ExpectAbortInForkedChild([]() { chkfree(nullptr); }));
  EXPECT_TRUE(ExpectAbortInForkedChild([]() { chkstrdup(nullptr); }));
  EXPECT_TRUE(
      ExpectAbortInForkedChild([]() { panic("fatal %s %d", "err", 7); }));
  EXPECT_TRUE(ExpectAbortInForkedChild([]() { panic(nullptr); }));

  // Cover OOM panic branches cleanly outside sanitizers, where tcmalloc
  // returns nullptr for SIZE_MAX instead of terminating inside the sanitizer.
  EXPECT_TRUE(ExpectAbortInForkedChild([]() {
    const volatile size_t huge = std::numeric_limits<size_t>::max();
    static_cast<void>(chkmalloc(huge));
  }));
  EXPECT_TRUE(ExpectAbortInForkedChild([]() {
    const volatile size_t huge = std::numeric_limits<size_t>::max();
    static_cast<void>(chkcalloc(huge));
  }));
  EXPECT_TRUE(ExpectAbortInForkedChild([]() {
    void* p = chkmalloc(8);
    const volatile size_t huge = std::numeric_limits<size_t>::max();
    static_cast<void>(chkrealloc(p, huge));
  }));
#endif
}

}  // namespace
}  // namespace netcdf
