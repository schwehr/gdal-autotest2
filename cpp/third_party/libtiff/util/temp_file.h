// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// RAII temporary files/directories scoped to ::testing::TempDir(), runfiles
// path resolution, and fork+execv subprocess capture preserving coverage env.

#ifndef THIRD_PARTY_TIFF_GOOGLE_UTIL_TEMP_FILE_H_
#define THIRD_PARTY_TIFF_GOOGLE_UTIL_TEMP_FILE_H_

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>  // NOLINT(build/c++17)
#include <string>
#include <string_view>
#include <system_error>  // NOLINT(build/c++11)
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"

namespace libtiff {

// Creates a temporary file path under ::testing::TempDir() and removes any
// pre-existing or created file upon destruction.
class TempFile {
 public:
  explicit TempFile(std::string_view filename)
      : path_(std::filesystem::path(::testing::TempDir()) / filename) {
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    std::filesystem::remove(path_, ec);
  }

  ~TempFile() {
    if (!path_.empty()) {
      std::error_code ec;
      std::filesystem::remove(path_, ec);
    }
  }

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  TempFile(TempFile&& other) noexcept : path_(std::move(other.path_)) {
    other.path_.clear();
  }
  TempFile& operator=(TempFile&& other) noexcept {
    if (this != &other) {
      if (!path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
      }
      path_ = std::move(other.path_);
      other.path_.clear();
    }
    return *this;
  }

  const std::filesystem::path& path() const { return path_; }
  const std::string& string() const { return path_.native(); }
  const char* c_str() const { return path_.c_str(); }

 private:
  std::filesystem::path path_;
};

// Creates an empty directory under ::testing::TempDir() and removes its tree
// upon destruction.
class TempDir {
 public:
  explicit TempDir(std::string_view dirname)
      : path_(std::filesystem::path(::testing::TempDir()) / dirname) {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_, ec);
  }

  ~TempDir() {
    if (!path_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(path_, ec);
    }
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  TempDir(TempDir&& other) noexcept : path_(std::move(other.path_)) {
    other.path_.clear();
  }
  TempDir& operator=(TempDir&& other) noexcept {
    if (this != &other) {
      if (!path_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
      }
      path_ = std::move(other.path_);
      other.path_.clear();
    }
    return *this;
  }

  const std::filesystem::path& path() const { return path_; }
  std::filesystem::path child(std::string_view name) const {
    return path_ / name;
  }
  const std::string& string() const { return path_.native(); }
  const char* c_str() const { return path_.c_str(); }

 private:
  std::filesystem::path path_;
};

// Resolves a data dependency relative to google3/ via TEST_SRCDIR.
inline std::filesystem::path ResolveRunfilesPath(
    std::string_view rel_from_google3) {
  const char* srcdir = std::getenv("TEST_SRCDIR");
  if (srcdir == nullptr || *srcdir == '\0') {
    srcdir = ".";
  }
  return std::filesystem::path(srcdir) / "google3" / rel_from_google3;
}

struct SubprocessResult {
  int exit_code = -1;
  int term_signal = 0;
  std::string stdout_output;
  std::string stderr_output;
};

// Executes `executable` with `args` (argv[1..]) using fork() + execv(),
// preserving the parent environment (LLVM_PROFILE_FILE / GCOV_PREFIX /
// sanitizer vars) so child tool invocations contribute to LCOV coverage.
inline SubprocessResult RunSubprocess(const std::filesystem::path& executable,
                                      const std::vector<std::string>& args) {
  SubprocessResult res;
  std::array<int, 2> out_pipe = {-1, -1};
  std::array<int, 2> err_pipe = {-1, -1};
  if (::pipe(out_pipe.data()) != 0) return res;
  if (::pipe(err_pipe.data()) != 0) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    return res;
  }

  std::vector<std::string> argv_storage;
  argv_storage.reserve(args.size() + 1);
  argv_storage.push_back(executable.native());
  argv_storage.insert(argv_storage.end(), args.begin(), args.end());
  std::vector<char*> argv;
  argv.reserve(argv_storage.size() + 1);
  for (std::string& s : argv_storage) {
    argv.push_back(s.data());
  }
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);
    return res;
  }
  if (pid == 0) {
    ::dup2(out_pipe[1], STDOUT_FILENO);
    ::dup2(err_pipe[1], STDERR_FILENO);
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);
    const int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0 && devnull != STDIN_FILENO) {
      ::dup2(devnull, STDIN_FILENO);
      ::close(devnull);
    }
    ::execv(executable.c_str(), argv.data());
    ::_exit(127);
  }

  ::close(out_pipe[1]);
  ::close(err_pipe[1]);

  std::array<struct pollfd, 2> fds = {{
      {.fd = out_pipe[0], .events = POLLIN, .revents = 0},
      {.fd = err_pipe[0], .events = POLLIN, .revents = 0},
  }};
  int open_count = 2;
  std::array<char, 4096> buf{};
  while (open_count > 0) {
    const int rc = ::poll(fds.data(), fds.size(), -1);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (size_t i = 0; i < fds.size(); ++i) {
      if (fds[i].fd < 0 || fds[i].revents == 0) continue;
      if (fds[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
        const ssize_t n = ::read(fds[i].fd, buf.data(), buf.size());
        if (n > 0) {
          std::string& dst = (i == 0) ? res.stdout_output : res.stderr_output;
          dst.append(buf.data(), static_cast<size_t>(n));
        } else if (n == 0 || (n < 0 && errno != EINTR)) {
          ::close(fds[i].fd);
          fds[i].fd = -1;
          --open_count;
        }
      }
    }
  }
  if (fds[0].fd >= 0) ::close(fds[0].fd);
  if (fds[1].fd >= 0) ::close(fds[1].fd);

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) return res;
  }
  if (WIFEXITED(status)) {
    res.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    res.term_signal = WTERMSIG(status);
  }
  return res;
}

}  // namespace libtiff

#endif  // THIRD_PARTY_TIFF_GOOGLE_UTIL_TEMP_FILE_H_
