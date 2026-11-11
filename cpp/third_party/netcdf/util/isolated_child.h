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
// Isolated child-process runners that sandbox destructive global-state tests
// (e.g., nc_finalize(), V2 fatal ncopts, C CLI main() entry points calling
// exit()) while preserving LLVM/gcov line coverage across the child process.

#ifndef THIRD_PARTY_NETCDF_GOOGLE_UTIL_ISOLATED_CHILD_H_
#define THIRD_PARTY_NETCDF_GOOGLE_UTIL_ISOLATED_CHILD_H_

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/types/span.h"

extern "C" {
void __gcov_dump() __attribute__((weak));
void __gcov_reset() __attribute__((weak));
void MallocExtension_Internal_SetBackgroundProcessActionsEnabled(bool value)
    __attribute__((weak));
extern volatile sig_atomic_t netcdf_google_in_forked_child;
}  // extern "C"

namespace netcdf {

// Pauses TCMalloc background process actions once per process before fork().
inline void PauseTcmallocBackgroundActionsOnce() {
  static const bool background_paused = []() {
    if (&MallocExtension_Internal_SetBackgroundProcessActionsEnabled !=
        nullptr) {
      MallocExtension_Internal_SetBackgroundProcessActionsEnabled(false);
      struct timespec req{0, 20000000L};
      while (::nanosleep(&req, &req) != 0 && errno == EINTR) {
      }
    }
    return true;
  }();
  static_cast<void>(background_paused);
}

// Runs `fn()` via GoogleTest's "threadsafe" death-test runner (`clone`/`fork`
// followed immediately by `execve`). Does NOT preserve parent TempFile/TempDir
// objects or pre-fork state inside the same TEST body.
template <typename Fn>
void RunInExecveChild(Fn&& fn) {
  struct DeathTestStyleGuard {
    std::string prev = GTEST_FLAG_GET(death_test_style);
    DeathTestStyleGuard() { GTEST_FLAG_SET(death_test_style, "threadsafe"); }
    ~DeathTestStyleGuard() { GTEST_FLAG_SET(death_test_style, prev); }
  } style_guard;
  EXPECT_EXIT(
      {
        const bool ok = std::forward<Fn>(fn)();
        if (&__gcov_dump != nullptr) {
          __gcov_dump();
        }
        _exit(ok ? 0 : 1);
      },
      ::testing::ExitedWithCode(0), "");
}

namespace detail {

inline void CloseFd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

inline bool SetNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

inline ::testing::AssertionResult WaitForChild(pid_t pid, int* exit_code_out) {
  constexpr int64_t kMaxWaitMs = 15000;
  struct timespec start_ts{};
  const bool have_clock = ::clock_gettime(CLOCK_MONOTONIC, &start_ts) == 0;
  int status = 0;
  int64_t slept_ms = 0;
  while (true) {
    const pid_t waited = ::waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
      break;
    }
    if (waited < 0) {
      if (errno == EINTR) {
        continue;
      }
      return ::testing::AssertionFailure()
             << "waitpid() failed: " << std::strerror(errno);
    }

    int64_t elapsed_ms = slept_ms;
    struct timespec now_ts{};
    if (have_clock && ::clock_gettime(CLOCK_MONOTONIC, &now_ts) == 0) {
      elapsed_ms = (now_ts.tv_sec - start_ts.tv_sec) * 1000 +
                   (now_ts.tv_nsec - start_ts.tv_nsec) / 1000000;
    }
    if (elapsed_ms >= kMaxWaitMs) {
      static_cast<void>(::kill(pid, SIGKILL));
      pid_t reaped = -1;
      do {
        reaped = ::waitpid(pid, &status, 0);
      } while (reaped < 0 && errno == EINTR);
      return ::testing::AssertionFailure()
             << "forked child timed out after " << kMaxWaitMs << " ms";
    }

    struct timespec poll_ts{0, 5000000L};
    while (::nanosleep(&poll_ts, &poll_ts) != 0 && errno == EINTR) {
    }
    slept_ms += 5;
  }
  if (!WIFEXITED(status)) {
    if (WIFSIGNALED(status)) {
      return ::testing::AssertionFailure()
             << "child terminated by signal " << WTERMSIG(status);
    }
    return ::testing::AssertionFailure() << "child did not exit normally";
  }
  if (exit_code_out != nullptr) {
    *exit_code_out = WEXITSTATUS(status);
  }
  if (WEXITSTATUS(status) != 0) {
    return ::testing::AssertionFailure()
           << "child exited with status " << WEXITSTATUS(status);
  }
  return ::testing::AssertionSuccess();
}

}  // namespace detail

// Runs `fn()` directly in a `fork()`ed child preserving parent TempFile/TempDir
// objects, resetting coverage counters before execution and dumping them before
// `_exit()`. Returns AssertionSuccess iff child exited normally with code 0.
template <typename Fn>
::testing::AssertionResult RunInForkedChild(Fn&& fn) {
  PauseTcmallocBackgroundActionsOnce();

  std::fflush(nullptr);
  const pid_t pid = ::fork();
  if (pid < 0) {
    return ::testing::AssertionFailure()
           << "fork() failed: " << std::strerror(errno);
  }
  if (pid == 0) {
    netcdf_google_in_forked_child = 1;
    if (&__gcov_reset != nullptr) {
      __gcov_reset();
    }
    const bool ok = std::forward<Fn>(fn)();
    std::fflush(nullptr);
    if (&__gcov_dump != nullptr) {
      __gcov_dump();
    }
    ::_exit(ok ? 0 : 1);
  }

  return detail::WaitForChild(pid, nullptr);
}

struct CliResult {
  int exit_code = -1;
  std::string stdout_output;
  std::string stderr_output;
  std::string harness_error;

  bool ok() const { return harness_error.empty(); }
};

// Runs a C `main(int argc, char** argv)` function in an isolated `fork()`ed
// child with `stdin`, `stdout`, and `stderr` redirected to pipes and `optind`
// reset. Tests whose CLI target calls `exit()` must also depend on
// `//third_party/netcdf/google/util:cli_exit_wrap` so `exit()` dumps coverage
// and `_exit()`s cleanly without running parent atexit/LSan checks.
template <typename CliMainFn>
CliResult RunCliInForkedChild(CliMainFn&& cli_main,
                              absl::Span<const std::string_view> args,
                              std::string_view stdin_data = "") {
  CliResult result;
  PauseTcmallocBackgroundActionsOnce();

  // Build mutable storage for argv prior to fork().
  std::vector<std::string> owned_args;
  owned_args.reserve(args.empty() ? 1 : args.size());
  if (args.empty()) {
    owned_args.emplace_back("cli");
  } else {
    for (std::string_view arg : args) {
      owned_args.emplace_back(arg);
    }
  }
  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(owned_args.size() + 1);
  for (std::string& arg : owned_args) {
    argv_ptrs.push_back(arg.data());
  }
  argv_ptrs.push_back(nullptr);

  int in_pipe[2] = {-1, -1};
  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0) {
    result.harness_error =
        std::string("pipe() failed: ") + std::strerror(errno);
    detail::CloseFd(in_pipe[0]);
    detail::CloseFd(in_pipe[1]);
    detail::CloseFd(out_pipe[0]);
    detail::CloseFd(out_pipe[1]);
    detail::CloseFd(err_pipe[0]);
    detail::CloseFd(err_pipe[1]);
    return result;
  }

  std::fflush(nullptr);
  const pid_t pid = ::fork();
  if (pid < 0) {
    result.harness_error =
        std::string("fork() failed: ") + std::strerror(errno);
    detail::CloseFd(in_pipe[0]);
    detail::CloseFd(in_pipe[1]);
    detail::CloseFd(out_pipe[0]);
    detail::CloseFd(out_pipe[1]);
    detail::CloseFd(err_pipe[0]);
    detail::CloseFd(err_pipe[1]);
    return result;
  }

  if (pid == 0) {
    netcdf_google_in_forked_child = 1;
    detail::CloseFd(in_pipe[1]);
    detail::CloseFd(out_pipe[0]);
    detail::CloseFd(err_pipe[0]);
    ::dup2(in_pipe[0], STDIN_FILENO);
    ::dup2(out_pipe[1], STDOUT_FILENO);
    ::dup2(err_pipe[1], STDERR_FILENO);
    detail::CloseFd(in_pipe[0]);
    detail::CloseFd(out_pipe[1]);
    detail::CloseFd(err_pipe[1]);

    if (&__gcov_reset != nullptr) {
      __gcov_reset();
    }
    optind = 1;
    opterr = 1;
    optopt = 0;
    std::clearerr(stdin);
    std::clearerr(stdout);
    std::clearerr(stderr);

    const int rc = std::forward<CliMainFn>(cli_main)(
        static_cast<int>(owned_args.size()), argv_ptrs.data());
    std::fflush(nullptr);
    if (&__gcov_dump != nullptr) {
      __gcov_dump();
    }
    ::_exit(rc);
  }

  detail::CloseFd(in_pipe[0]);
  detail::CloseFd(out_pipe[1]);
  detail::CloseFd(err_pipe[1]);

  if (stdin_data.empty()) {
    detail::CloseFd(in_pipe[1]);
  } else {
    detail::SetNonBlocking(in_pipe[1]);
  }
  detail::SetNonBlocking(out_pipe[0]);
  detail::SetNonBlocking(err_pipe[0]);

  size_t in_offset = 0;
  while (in_pipe[1] >= 0 || out_pipe[0] >= 0 || err_pipe[0] >= 0) {
    struct pollfd fds[3]{};
    nfds_t nfds = 0;
    if (in_pipe[1] >= 0) {
      fds[nfds++] = {in_pipe[1], POLLOUT, 0};
    }
    if (out_pipe[0] >= 0) {
      fds[nfds++] = {out_pipe[0], POLLIN, 0};
    }
    if (err_pipe[0] >= 0) {
      fds[nfds++] = {err_pipe[0], POLLIN, 0};
    }
    const int pr = ::poll(fds, nfds, 15000);
    if (pr < 0) {
      if (errno == EINTR) {
        continue;
      }
      result.harness_error =
          std::string("poll() failed: ") + std::strerror(errno);
      break;
    }
    if (pr == 0) {
      result.harness_error = "timed out waiting on CLI child pipes";
      static_cast<void>(::kill(pid, SIGKILL));
      break;
    }

    for (nfds_t i = 0; i < nfds; ++i) {
      const int fd = fds[i].fd;
      if (fd == in_pipe[1] &&
          (fds[i].revents & (POLLOUT | POLLERR | POLLHUP))) {
        const ssize_t wrote = ::write(in_pipe[1], stdin_data.data() + in_offset,
                                      stdin_data.size() - in_offset);
        if (wrote > 0) {
          in_offset += static_cast<size_t>(wrote);
          if (in_offset >= stdin_data.size()) {
            detail::CloseFd(in_pipe[1]);
          }
        } else if (wrote < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                   errno != EINTR) {
          detail::CloseFd(in_pipe[1]);
        }
      } else if ((fd == out_pipe[0] || fd == err_pipe[0]) &&
                 (fds[i].revents & (POLLIN | POLLHUP | POLLERR))) {
        char buf[4096];
        std::string& dst =
            (fd == out_pipe[0]) ? result.stdout_output : result.stderr_output;
        while (true) {
          const ssize_t n = ::read(fd, buf, sizeof(buf));
          if (n > 0) {
            dst.append(buf, static_cast<size_t>(n));
          } else if (n == 0) {
            detail::CloseFd(fd == out_pipe[0] ? out_pipe[0] : err_pipe[0]);
            break;
          } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
          } else if (errno != EINTR) {
            detail::CloseFd(fd == out_pipe[0] ? out_pipe[0] : err_pipe[0]);
            break;
          }
        }
      }
    }
  }

  detail::CloseFd(in_pipe[1]);
  detail::CloseFd(out_pipe[0]);
  detail::CloseFd(err_pipe[0]);

  int raw_status = 0;
  const ::testing::AssertionResult wait_res =
      detail::WaitForChild(pid, &raw_status);
  // WaitForChild returns failure on any non-zero exit code too, whereas CLI
  // tests often assert non-zero exit codes cleanly. Distinguish signal/timeout
  // failures from normal process exits via raw_status initialization.
  if (raw_status >= 0 && wait_res) {
    result.exit_code = raw_status;
  } else if (!wait_res) {
    const std::string msg = wait_res.message();
    if (msg.rfind("child exited with status ", 0) == 0) {
      result.exit_code = raw_status;
    } else if (result.harness_error.empty()) {
      result.harness_error = msg;
    }
  }
  return result;
}

}  // namespace netcdf

#endif  // THIRD_PARTY_NETCDF_GOOGLE_UTIL_ISOLATED_CHILD_H_
