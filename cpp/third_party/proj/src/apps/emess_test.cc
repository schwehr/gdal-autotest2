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
// Unit tests for CLI error reporting and process termination helpers
// (proj/src/apps/emess.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/apps/emess.cpp
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/apps/emess.h

#include "third_party/proj4/proj/src/apps/emess.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/temp_file.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

using ::testing::AllOf;
using ::testing::ExitedWithCode;
using ::testing::HasSubstr;

class ScopedEnvVar {
 public:
  explicit ScopedEnvVar(const char* name) : name_(name) {
    if (const char* existing = std::getenv(name_)) {
      original_ = existing;
    }
  }

  ~ScopedEnvVar() {
    if (original_.has_value()) {
      setenv(name_, original_->c_str(), /*replace=*/1);
    } else {
      unsetenv(name_);
    }
  }

  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

  void Set(const char* value) const { setenv(name_, value, /*replace=*/1); }
  void Unset() const { unsetenv(name_); }

 private:
  const char* name_;
  std::optional<std::string> original_;
};

// Redirects STDERR_FILENO to a scoped TempFile under ::testing::TempDir() so
// stderr output from emess() is captured without touching /tmp.
class ScopedStderrCapture {
 public:
  explicit ScopedStderrCapture(std::string_view temp_filename)
      : temp_(temp_filename) {
    std::fflush(stderr);
    const int fd = ::open(temp_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
      return;
    }
    saved_stderr_fd_ = ::dup(STDERR_FILENO);
    if (saved_stderr_fd_ < 0) {
      ::close(fd);
      return;
    }
    if (::dup2(fd, STDERR_FILENO) < 0) {
      ::close(saved_stderr_fd_);
      saved_stderr_fd_ = -1;
    }
    ::close(fd);
  }

  ~ScopedStderrCapture() { Restore(); }

  ScopedStderrCapture(const ScopedStderrCapture&) = delete;
  ScopedStderrCapture& operator=(const ScopedStderrCapture&) = delete;

  bool is_active() const noexcept { return saved_stderr_fd_ >= 0; }

  std::string StopAndRead() {
    Restore();
    std::ifstream ifs(temp_.path(), std::ios::binary);
    if (!ifs) {
      return {};
    }
    return std::string(std::istreambuf_iterator<char>(ifs),
                       std::istreambuf_iterator<char>());
  }

 private:
  void Restore() {
    if (saved_stderr_fd_ >= 0) {
      std::fflush(stderr);
      ::dup2(saved_stderr_fd_, STDERR_FILENO);
      ::close(saved_stderr_fd_);
      saved_stderr_fd_ = -1;
    }
  }

  TempFile temp_;
  int saved_stderr_fd_ = -1;
};

TEST(EmessTest, DefaultGlobalEmessDatStartsZeroed) {
  EXPECT_EQ(nullptr, emess_dat.File_name);
  EXPECT_EQ(nullptr, emess_dat.Prog_name);
  EXPECT_EQ(0, emess_dat.File_line);
}

TEST(EmessTest, NullProgAndNullOrEmptyFileEmitMessageOnly) {
  absl::Cleanup restore_global = [] { emess_dat = {nullptr, nullptr, 0}; };

  // 1. Both Prog_name and File_name null.
  emess_dat = {nullptr, nullptr, 0};
  {
    ScopedStderrCapture capture("emess_test/null_both.txt");
    ASSERT_TRUE(capture.is_active());
    emess(0, "plain %s %d", "error", 42);
    EXPECT_EQ("\nplain error 42\n", capture.StopAndRead());
  }

  // 2. Non-null empty File_name ("") takes the same branch as nullptr and
  // ignores File_line.
  char empty_file[] = "";
  emess_dat = {.File_name = empty_file, .Prog_name = nullptr, .File_line = 99};
  {
    ScopedStderrCapture capture("emess_test/empty_file.txt");
    ASSERT_TRUE(capture.is_active());
    emess(0, "ignored line %d", 1);
    EXPECT_EQ("\nignored line 1\n", capture.StopAndRead());
  }
}

TEST(EmessTest, ProgNameAndDisplayEnvVarControlReleasePrefix) {
  ScopedEnvVar env_display("PROJ_DISPLAY_PROGRAM_NAME");
  absl::Cleanup restore_global = [] { emess_dat = {nullptr, nullptr, 0}; };

  char prog[] = "cs2cs";
  emess_dat = {.File_name = nullptr, .Prog_name = prog, .File_line = 0};
  const std::string release = pj_get_release();

  // 1. Unset env var prints release and <Prog_name>: prefix.
  env_display.Unset();
  {
    ScopedStderrCapture capture("emess_test/env_unset.txt");
    ASSERT_TRUE(capture.is_active());
    emess(-1, "bad arg: %s", "--foo");
    EXPECT_EQ(release + "\n<cs2cs>: \nbad arg: --foo\n", capture.StopAndRead());
  }

  // 2. Env var set to any value other than "NO" still prints the prefix.
  env_display.Set("YES");
  {
    ScopedStderrCapture capture("emess_test/env_yes.txt");
    ASSERT_TRUE(capture.is_active());
    emess(-1, "still shown");
    EXPECT_EQ(release + "\n<cs2cs>: \nstill shown\n", capture.StopAndRead());
  }

  // 3. Env var set to "NO" suppresses release and program name prefix.
  env_display.Set("NO");
  {
    ScopedStderrCapture capture("emess_test/env_no.txt");
    ASSERT_TRUE(capture.is_active());
    emess(-1, "suppressed prefix");
    EXPECT_EQ("\nsuppressed prefix\n", capture.StopAndRead());
  }
}

TEST(EmessTest, FileNameAndLineBranchesFormatProcessingContext) {
  ScopedEnvVar env_display("PROJ_DISPLAY_PROGRAM_NAME");
  env_display.Unset();
  absl::Cleanup restore_global = [] { emess_dat = {nullptr, nullptr, 0}; };

  char prog[] = "proj";
  char file[] = "input.dat";
  const std::string release = pj_get_release();

  // 1. Positive File_line appends ", line N".
  emess_dat = {.File_name = file, .Prog_name = prog, .File_line = 17};
  {
    ScopedStderrCapture capture("emess_test/file_pos_line.txt");
    ASSERT_TRUE(capture.is_active());
    emess(0, "syntax %.1f %%", 2.5);
    EXPECT_EQ(release +
                  "\n<proj>: while processing file: input.dat, line 17\n"
                  "syntax 2.5 %\n",
              capture.StopAndRead());
  }

  // 2. Zero File_line omits ", line N".
  emess_dat.File_line = 0;
  {
    ScopedStderrCapture capture("emess_test/file_zero_line.txt");
    ASSERT_TRUE(capture.is_active());
    emess(-1, "eof");
    EXPECT_EQ(release + "\n<proj>: while processing file: input.dat\neof\n",
              capture.StopAndRead());
  }

  // 3. Negative File_line also omits ", line N" without printing Prog_name.
  emess_dat = {.File_name = file, .Prog_name = nullptr, .File_line = -5};
  {
    ScopedStderrCapture capture("emess_test/file_neg_line.txt");
    ASSERT_TRUE(capture.is_active());
    emess(-3, "negative line");
    EXPECT_EQ("while processing file: input.dat\nnegative line\n",
              capture.StopAndRead());
  }
}

TEST(EmessTest, NegativeTwoLogsSystemErrnoWithoutExiting) {
  absl::Cleanup restore_global = [] {
    emess_dat = {nullptr, nullptr, 0};
    errno = 0;
  };

  emess_dat = {nullptr, nullptr, 0};
  const std::string expected_err = std::strerror(ENOENT);
  errno = ENOENT;
  ScopedStderrCapture capture("emess_test/neg_two.txt");
  ASSERT_TRUE(capture.is_active());
  emess(-2, "cannot open %s", "missing.txt");
  EXPECT_EQ("\nSys errno: " + std::to_string(ENOENT) + ": " + expected_err +
                "\ncannot open missing.txt\n",
            capture.StopAndRead());
}

TEST(EmessTest, PositiveCodesExitWithExpectedStatusAndDiagnostics) {
  // 1. Positive code 1 formats Prog_name, File_name, File_line, and exits(1).
  EXPECT_EXIT(
      ([]() {
        unsetenv("PROJ_DISPLAY_PROGRAM_NAME");
        char prog[] = "cs2cs";
        char file[] = "coords.txt";
        emess_dat = {.File_name = file, .Prog_name = prog, .File_line = 7};
        emess(1, "invalid option: %s", "-Z");
      })(),
      ExitedWithCode(1),
      AllOf(HasSubstr(pj_get_release()),
            HasSubstr("<cs2cs>: while processing file: coords.txt, line 7\n"),
            HasSubstr("invalid option: -Z\nprogram abnormally terminated\n")));

  // 2. Positive code 2 prints Sys errno and exits(2).
  EXPECT_EXIT(
      ([]() {
        unsetenv("PROJ_DISPLAY_PROGRAM_NAME");
        char prog[] = "proj";
        emess_dat = {.File_name = nullptr, .Prog_name = prog, .File_line = 0};
        errno = EACCES;
        emess(2, "cannot read %s", "secret.dat");
      })(),
      ExitedWithCode(2),
      AllOf(HasSubstr("<proj>: \nSys errno: " + std::to_string(EACCES) + ": "),
            HasSubstr(std::strerror(EACCES)),
            HasSubstr("cannot read secret.dat\n"
                      "program abnormally terminated\n")));

  // 3. Positive code 3 without Prog_name/File_name/errno exits(3).
  EXPECT_EXIT(emess(3, "missing CRS"), ExitedWithCode(3),
              HasSubstr("\nmissing CRS\nprogram abnormally terminated\n"));
}

}  // namespace
}  // namespace proj
