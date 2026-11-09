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
// Tests internal POSIX locking, time conversion, path helpers, nanosleep,
// command-line option parsing, and case-insensitive substring matching
// (third_party/hdf5/src/H5system.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5system.c

#ifndef NDEBUG
#define H5SYSTEM_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

#ifdef H5SYSTEM_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5SYSTEM_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <array>
#include <climits>
#include <cstdio>
#include <ctime>
#include <filesystem>  // NOLINT
#include <span>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"

namespace hdf5 {
namespace {

std::string TakeH5String(char* ptr) {
  absl::Cleanup free_ptr([ptr] { H5free_memory(ptr); });
  return ptr ? std::string(ptr) : std::string();
}

// Invokes H5_get_option while optionally suppressing stderr output when
// testing H5_opterr == 1 error messages so test logs stay clean.
int CallGetOption(std::span<const char* const> argv, const char* opts,
                  const struct h5_long_options* l_opts,
                  bool silence_stderr = false) {
  const int argc = static_cast<int>(argv.size());
  if (!silence_stderr) {
    return H5_get_option(argc, argv.data(), opts, l_opts);
  }
  std::fflush(stderr);
  const int saved_stderr = dup(STDERR_FILENO);
  const int devnull = open("/dev/null", O_WRONLY);
  if (saved_stderr >= 0 && devnull >= 0) {
    dup2(devnull, STDERR_FILENO);
  }
  if (devnull >= 0) {
    close(devnull);
  }
  absl::Cleanup restore_stderr([saved_stderr] {
    std::fflush(stderr);
    if (saved_stderr >= 0) {
      dup2(saved_stderr, STDERR_FILENO);
      close(saved_stderr);
    }
  });
  return H5_get_option(argc, argv.data(), opts, l_opts);
}

TEST(H5SystemTest, PflockAndNflockLockUnlockAndErrorPaths) {
  // Nflock always succeeds regardless of descriptor or operation.
  EXPECT_EQ(Nflock(-1, LOCK_EX), 0);
  EXPECT_EQ(Nflock(0, LOCK_SH), 0);
  EXPECT_EQ(Nflock(0, LOCK_UN), 0);

  // Invalid descriptor fails fcntl(F_SETLK).
  EXPECT_EQ(Pflock(-1, LOCK_EX), -1);

  TempFile temp("h5system_pflock.dat");
  const int rw_fd = open(temp.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0600);
  ASSERT_GE(rw_fd, 0);
  absl::Cleanup close_rw([rw_fd] { close(rw_fd); });

  EXPECT_EQ(Pflock(rw_fd, LOCK_EX), 0);
  EXPECT_EQ(Pflock(rw_fd, LOCK_SH), 0);
  EXPECT_EQ(Pflock(rw_fd, LOCK_UN), 0);

  // Exclusive write lock on a read-only fd fails with EBADF.
  const int ro_fd = open(temp.c_str(), O_RDONLY);
  ASSERT_GE(ro_fd, 0);
  absl::Cleanup close_ro([ro_fd] { close(ro_fd); });
  EXPECT_EQ(Pflock(ro_fd, LOCK_EX), -1);
}

TEST(H5SystemTest, MakeTimeConvertsUtcAndRejectsOverflow) {
  // Zero-initialize struct tm before populating fields so tm_gmtoff and
  // padding bytes are fully initialized under MSan.
  struct tm epoch_tm{};
  epoch_tm.tm_year = 70;
  epoch_tm.tm_mon = 0;
  epoch_tm.tm_mday = 1;
  epoch_tm.tm_hour = 0;
  epoch_tm.tm_min = 0;
  epoch_tm.tm_sec = 0;
  epoch_tm.tm_isdst = -1;
  EXPECT_EQ(H5_make_time(&epoch_tm), static_cast<time_t>(0));

  // Second call exercises the cached H5_ntzset == true branch.
  struct tm leap_tm{};
  leap_tm.tm_year = 100;  // 2000
  leap_tm.tm_mon = 1;     // Feb
  leap_tm.tm_mday = 29;
  leap_tm.tm_hour = 23;
  leap_tm.tm_min = 59;
  leap_tm.tm_sec = 59;
  leap_tm.tm_isdst = -1;
  EXPECT_EQ(H5_make_time(&leap_tm), static_cast<time_t>(951868799));

  ScopedErrorSilence silence;
  struct tm bad_tm{};
  bad_tm.tm_year = INT_MAX;
  bad_tm.tm_mon = INT_MAX;
  EXPECT_EQ(H5_make_time(&bad_tm), static_cast<time_t>(-1));
}

TEST(H5SystemTest, BuildExtpathHandlesAbsoluteAndRelativePaths) {
  char* out = nullptr;
  ASSERT_GE(H5_build_extpath("/alpha/beta/target.h5", &out), 0);
  EXPECT_EQ(TakeH5String(out), "/alpha/beta/");

  out = nullptr;
  ASSERT_GE(H5_build_extpath("/top.h5", &out), 0);
  EXPECT_EQ(TakeH5String(out), "/");

  const std::string cwd = std::filesystem::current_path().native();
  const std::string cwd_slash =
      (!cwd.empty() && cwd.back() == '/') ? cwd : cwd + "/";

  out = nullptr;
  ASSERT_GE(H5_build_extpath("relative.h5", &out), 0);
  EXPECT_EQ(TakeH5String(out), cwd_slash);

  out = nullptr;
  ASSERT_GE(H5_build_extpath("nested/dir/file.h5", &out), 0);
  EXPECT_EQ(TakeH5String(out), cwd_slash + "nested/dir/");
}

TEST(H5SystemTest, CombinePathJoinsAndOverridesRelativeVsAbsolute) {
  struct CombineCase {
    const char* path1;
    const char* path2;
    std::string_view expected;
  };
  constexpr auto kCases = std::to_array<CombineCase>({
      {.path1 = nullptr, .path2 = "rel.h5", .expected = "rel.h5"},
      {.path1 = "", .path2 = "rel.h5", .expected = "rel.h5"},
      {.path1 = "/base/dir",
       .path2 = "/abs/file.h5",
       .expected = "/abs/file.h5"},
      {.path1 = "/base/dir/",
       .path2 = "sub/f.h5",
       .expected = "/base/dir/sub/f.h5"},
      {.path1 = "/base/dir",
       .path2 = "sub/f.h5",
       .expected = "/base/dir/sub/f.h5"},
  });

  for (const auto& tc : kCases) {
    char* full = nullptr;
    ASSERT_GE(H5_combine_path(tc.path1, tc.path2, &full), 0);
    EXPECT_EQ(TakeH5String(full), tc.expected);
  }
}

TEST(H5SystemTest, DirnameAndBasenameCoverPosixEdgeCasesAndErrors) {
  struct PathCase {
    const char* input;
    std::string_view expected_dir;
    std::string_view expected_base;
  };
  constexpr auto kCases = std::to_array<PathCase>({
      {.input = "", .expected_dir = ".", .expected_base = "."},
      {.input = "file", .expected_dir = ".", .expected_base = "file"},
      {.input = "/", .expected_dir = "/", .expected_base = "/"},
      {.input = "/file", .expected_dir = "/", .expected_base = "file"},
      {.input = "///", .expected_dir = "/", .expected_base = "/"},
      {.input = "dir/", .expected_dir = ".", .expected_base = "dir"},
      {.input = "dir///", .expected_dir = ".", .expected_base = "dir"},
      {.input = "/usr/", .expected_dir = "/", .expected_base = "usr"},
      {.input = "///usr///", .expected_dir = "/", .expected_base = "usr"},
      {.input = "/a/b/", .expected_dir = "/a", .expected_base = "b"},
      {.input = "///file", .expected_dir = "/", .expected_base = "file"},
      {.input = "dir///file", .expected_dir = "dir", .expected_base = "file"},
      {.input = "/a/b/c", .expected_dir = "/a/b", .expected_base = "c"},
  });

  for (const auto& tc : kCases) {
    char* dir = nullptr;
    char* base = nullptr;
    ASSERT_GE(H5_dirname(tc.input, &dir), 0) << tc.input;
    EXPECT_EQ(TakeH5String(dir), tc.expected_dir) << tc.input;
    ASSERT_GE(H5_basename(tc.input, &base), 0) << tc.input;
    EXPECT_EQ(TakeH5String(base), tc.expected_base) << tc.input;
  }

  ScopedErrorSilence silence;
  char* out = nullptr;
  EXPECT_LT(H5_dirname(nullptr, &out), 0);
  EXPECT_LT(H5_dirname("/a", nullptr), 0);
  EXPECT_LT(H5_basename(nullptr, &out), 0);
  EXPECT_LT(H5_basename("/a", nullptr), 0);
}

TEST(H5SystemTest, NanosleepSleepsCleanly) {
  H5_nanosleep(0ULL);
  H5_nanosleep(1000ULL);
  H5_nanosleep(1000000ULL);
}

TEST(H5SystemTest, GetOptionParsesShortLongWildcardAndErrors) {
  const int orig_opterr = H5_opterr;
  const int orig_optind = H5_optind;
  const char* const orig_optarg = H5_optarg;
  absl::Cleanup restore_globals([&] {
    H5_opterr = orig_opterr;
    H5_optind = orig_optind;
    H5_optarg = orig_optarg;
  });

  constexpr struct h5_long_options kLongOpts[] = {
      {"flag", no_arg, 'a'},
      {"req", require_arg, 'r'},
      {"opt", optional_arg, 'o'},
      {nullptr, no_arg, 0},
  };
  constexpr char kShortOpts[] = "abf:w*";

  // 1. Mixed long and short options including clusters, inline/split args,
  //    optional args, double-dash stop, and positional tail.
  {
    constexpr auto kArgv = std::to_array<const char*>({
        "prog",
        "--flag",
        "--req=eq_val",
        "--req",
        "split_val",
        "--opt",
        "--opt=opt_v",
        "-ab",
        "-finline",
        "-f",
        "next_arg",
        "--",
        "pos_arg",
    });
    H5_optind = 1;
    H5_optarg = nullptr;
    H5_opterr = 0;

    EXPECT_EQ(CallGetOption(kArgv, kShortOpts, kLongOpts), 'a');
    EXPECT_EQ(CallGetOption(kArgv, kShortOpts, kLongOpts), 'r');
    EXPECT_STREQ(H5_optarg, "eq_val");
    EXPECT_EQ(CallGetOption(kArgv, kShortOpts, kLongOpts), 'r');
    EXPECT_STREQ(H5_optarg, "split_val");
    EXPECT_EQ(CallGetOption(kArgv, kShortOpts, kLongOpts), 'o');
    EXPECT_EQ(H5_optarg, nullptr);
    EXPECT_EQ(CallGetOption(kArgv, kShortOpts, kLongOpts), 'o');
    EXPECT_STREQ(H5_optarg, "opt_v");

    // Short cluster "-ab": finishes cleanly so static sp resets to 1.
    EXPECT_EQ(CallGetOption(kArgv, kShortOpts, kLongOpts), 'a');
    EXPECT_EQ(H5_optarg, nullptr);
    EXPECT_EQ(CallGetOption(kArgv, kShortOpts, kLongOpts), 'b');
    EXPECT_EQ(H5_optarg, nullptr);

    // Inline and next-token short required values.
    EXPECT_EQ(CallGetOption(kArgv, kShortOpts, kLongOpts), 'f');
    EXPECT_STREQ(H5_optarg, "inline");
    EXPECT_EQ(CallGetOption(kArgv, kShortOpts, kLongOpts), 'f');
    EXPECT_STREQ(H5_optarg, "next_arg");

    // "--" stops option parsing and advances past "--".
    EXPECT_EQ(CallGetOption(kArgv, kShortOpts, kLongOpts), EOF);
    EXPECT_EQ(H5_optind, 12);
    // Non-flag positional argument and past-end return EOF.
    EXPECT_EQ(CallGetOption(kArgv, kShortOpts, kLongOpts), EOF);
    H5_optind = static_cast<int>(kArgv.size());
    EXPECT_EQ(CallGetOption(kArgv, kShortOpts, kLongOpts), EOF);
  }

  // 2. Wildcard ("w*") and long require_arg followed by '-' or at end.
  {
    constexpr auto kWVal =
        std::to_array<const char*>({"prog", "-w", "wild", "tail"});
    H5_optind = 1;
    EXPECT_EQ(CallGetOption(kWVal, kShortOpts, kLongOpts), 'w');
    EXPECT_STREQ(H5_optarg, "wild");
    EXPECT_EQ(CallGetOption(kWVal, kShortOpts, kLongOpts), EOF);

    constexpr auto kWDash =
        std::to_array<const char*>({"prog", "-w", "-a", "tail"});
    H5_optind = 1;
    EXPECT_EQ(CallGetOption(kWDash, kShortOpts, kLongOpts), 'w');
    EXPECT_EQ(H5_optarg, nullptr);
    EXPECT_EQ(CallGetOption(kWDash, kShortOpts, kLongOpts), 'a');
    EXPECT_EQ(CallGetOption(kWDash, kShortOpts, kLongOpts), EOF);

    constexpr auto kWLast = std::to_array<const char*>({"prog", "-w", "last"});
    H5_optind = 1;
    EXPECT_EQ(CallGetOption(kWLast, kShortOpts, kLongOpts), 'w');
    EXPECT_EQ(H5_optarg, nullptr);

    // Lone "-" terminates option scanning; "--req" before "-a" or at end leaves
    // H5_optarg == nullptr.
    constexpr auto kEdgeLong =
        std::to_array<const char*>({"prog", "--req", "-a", "--req"});
    H5_optind = 1;
    EXPECT_EQ(CallGetOption(kEdgeLong, kShortOpts, kLongOpts), 'r');
    EXPECT_EQ(H5_optarg, nullptr);
    EXPECT_EQ(CallGetOption(kEdgeLong, kShortOpts, kLongOpts), 'a');
    EXPECT_EQ(CallGetOption(kEdgeLong, kShortOpts, kLongOpts), 'r');
    EXPECT_EQ(H5_optarg, nullptr);

    constexpr auto kLoneDash = std::to_array<const char*>({"prog", "-"});
    H5_optind = 1;
    EXPECT_EQ(CallGetOption(kLoneDash, kShortOpts, kLongOpts), EOF);
  }

  // 3. Error branches exercised with both H5_opterr == 0 and H5_opterr == 1.
  for (const int opterr_val : {0, 1}) {
    H5_opterr = opterr_val;
    const bool silence = (opterr_val == 1);

    constexpr auto kErrArgs = std::to_array<const char*>({
        "prog",
        "--flag=bad",
        "--unknown",
        "-x",
        "-ya",
        "-:",
        "-f",
    });
    H5_optind = 1;
    EXPECT_EQ(CallGetOption(kErrArgs, kShortOpts, kLongOpts, silence),
              '?');  // --flag=bad
    EXPECT_EQ(CallGetOption(kErrArgs, kShortOpts, kLongOpts, silence),
              '?');  // --unknown
    EXPECT_EQ(CallGetOption(kErrArgs, kShortOpts, kLongOpts, silence),
              '?');  // -x
    // "-ya": first '?' leaves sp=2 so the next call consumes 'a' and resets sp.
    EXPECT_EQ(CallGetOption(kErrArgs, kShortOpts, kLongOpts, silence), '?');
    EXPECT_EQ(CallGetOption(kErrArgs, kShortOpts, kLongOpts, silence), 'a');
    EXPECT_EQ(CallGetOption(kErrArgs, kShortOpts, kLongOpts, silence),
              '?');  // -:
    EXPECT_EQ(CallGetOption(kErrArgs, kShortOpts, kLongOpts, silence),
              '?');  // -f missing value

    // Long option when l_opts is nullptr.
    constexpr auto kNoLopts =
        std::to_array<const char*>({"prog", "--anything"});
    H5_optind = 1;
    EXPECT_EQ(CallGetOption(kNoLopts, kShortOpts, nullptr, silence), '?');
  }
}

TEST(H5SystemTest, StrcasestrMatchesCaseInsensitivelyAndHandlesEdgeCases) {
  const char hay1[] = "Hello World";
  EXPECT_EQ(H5_strcasestr(hay1, "wOrLd"), hay1 + 6);
  EXPECT_EQ(H5_strcasestr(hay1, "HELLO"), hay1);
  EXPECT_EQ(H5_strcasestr(hay1, ""), hay1);
  EXPECT_EQ(H5_strcasestr(hay1, "missing"), nullptr);

  const char hay2[] = "aabABC";
  EXPECT_EQ(H5_strcasestr(hay2, "abA"), hay2 + 1);

  const char empty[] = "";
  EXPECT_EQ(H5_strcasestr(empty, ""), empty);
  EXPECT_EQ(H5_strcasestr(empty, "x"), nullptr);
}

}  // namespace
}  // namespace hdf5
