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

// Unit tests for //third_party/netcdf/ncdump/ncfilteravail.c.
//
// All invocations run inside a forked child whose CWD is chdir()'d into a
// TempDir so temporary `tmp_<pid>.nc` files never touch the test runner CWD.
// Upstream `getopt(argc, argv, "t:")` omits 'h' and 'x' (expecting `-t` while
// switching on `'h'` and `'x'`), so a child-scoped `getopt()` interposer is
// armed for dedicated `-h`/`-x` cases to exercise `case 'h'`, `case 'x'`,
// `getformat()` string comparisons, and the NCZarr URL branch.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncdump/ncfilteravail.c

#include <dlfcn.h>
#include <getopt.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string_view>
#include <system_error>  // NOLINT
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/types/span.h"
#include "third_party/netcdf/google/util/cli_mains.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/temp_file.h"

namespace {

using GetoptFn = int (*)(int, char* const*, const char*);

GetoptFn g_real_getopt = nullptr;
bool g_real_getopt_resolved = false;
const char* g_getopt_override_optstring = nullptr;

void EnsureRealGetoptResolved() {
  if (!g_real_getopt_resolved) {
#ifdef RTLD_NEXT
    g_real_getopt = reinterpret_cast<GetoptFn>(::dlsym(RTLD_NEXT, "getopt"));
#endif
    g_real_getopt_resolved = true;
  }
}

int FallbackPosixGetopt(int argc, char* const* argv, const char* optstring) {
  if (optind == 0) {
    optind = 1;
  }
  if (optstring == nullptr || argv == nullptr || optind < 1 || optind >= argc) {
    return -1;
  }
  char* arg = argv[optind];
  if (arg == nullptr || arg[0] != '-' || arg[1] == '\0') {
    return -1;
  }
  if (arg[1] == '-' && arg[2] == '\0') {
    ++optind;
    return -1;
  }
  const char ch = arg[1];
  const char* spec = std::strchr(optstring, static_cast<unsigned char>(ch));
  if (spec == nullptr || ch == ':') {
    optopt = static_cast<unsigned char>(ch);
    ++optind;
    return '?';
  }
  if (spec[1] == ':') {
    if (arg[2] != '\0') {
      optarg = arg + 2;
      ++optind;
    } else if (optind + 1 < argc) {
      optarg = argv[optind + 1];
      optind += 2;
    } else {
      optopt = static_cast<unsigned char>(ch);
      ++optind;
      return '?';
    }
  } else {
    ++optind;
  }
  return static_cast<unsigned char>(ch);
}

}  // namespace

extern "C" int getopt(int argc, char* const* argv,
                      const char* optstring) noexcept {
  const char* effective_optstring = (g_getopt_override_optstring != nullptr)
                                        ? g_getopt_override_optstring
                                        : optstring;
  if (g_real_getopt != nullptr) {
    return g_real_getopt(argc, argv, effective_optstring);
  }
  return FallbackPosixGetopt(argc, argv, effective_optstring);
}

namespace netcdf {
namespace {

void ExpectFilterAvailInDir(const TempDir& workdir,
                            absl::Span<const std::string_view> args,
                            int expected_exit, std::string_view expected_stdout,
                            std::string_view expected_stderr_substr = "",
                            const char* getopt_optstring = nullptr) {
  EnsureRealGetoptResolved();
  ASSERT_TRUE(RunInForkedChild([&]() -> bool {
    if (::chdir(workdir.c_str()) != 0) {
      std::fprintf(stderr, "chdir(%s) failed\n", workdir.c_str());
      return false;
    }
    auto cli_entry = [getopt_optstring](int argc, char** argv) -> int {
      g_getopt_override_optstring = getopt_optstring;
      const int rc = ncfilteravail_cli_main(argc, argv);
      g_getopt_override_optstring = nullptr;
      return rc;
    };
    const CliResult res = RunCliInForkedChild(cli_entry, args);
    if (!res.ok()) {
      std::fprintf(stderr, "harness error: %s\n", res.harness_error.c_str());
      return false;
    }
    if (res.exit_code != expected_exit) {
      std::fprintf(stderr, "expected exit %d, got %d (stderr: %s)\n",
                   expected_exit, res.exit_code, res.stderr_output.c_str());
      return false;
    }
    if (res.stdout_output != expected_stdout) {
      std::fprintf(stderr, "expected stdout '%.*s', got '%s'\n",
                   static_cast<int>(expected_stdout.size()),
                   expected_stdout.data(), res.stdout_output.c_str());
      return false;
    }
    if (!expected_stderr_substr.empty() &&
        res.stderr_output.find(expected_stderr_substr) ==
            std::string_view::npos) {
      std::fprintf(stderr, "stderr '%s' missing '%.*s'\n",
                   res.stderr_output.c_str(),
                   static_cast<int>(expected_stderr_substr.size()),
                   expected_stderr_substr.data());
      return false;
    }
    return true;
  }));
}

TEST(NcfilteravailTest, ReportsKnownBuiltInAndMissingFilterNames) {
  struct FilterCase {
    std::string_view name;
    std::string_view expected_stdout;
  };
  constexpr FilterCase kCases[] = {
      {"deflate", "yes"}, {"szip", "yes"},     {"bzip2", "no"},
      {"zstd", "no"},     {"zstandard", "no"}, {"blosc", "no"},
  };

  TempDir workdir("ncfilteravail_names");
  for (const FilterCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const std::string_view args[] = {"ncfilteravail", tc.name};
    ExpectFilterAvailInDir(workdir, args, 0, tc.expected_stdout);
  }
}

TEST(NcfilteravailTest, ReportsNumericOutOfRangeAndInvalidFilterSpecs) {
  struct IdCase {
    std::string_view arg;
    std::string_view expected_stdout;
  };
  constexpr IdCase kCases[] = {
      {"1", "yes"},     // deflate
      {"2", "yes"},     // shuffle
      {"3", "yes"},     // fletcher32
      {"4", "yes"},     // szip
      {"307", "no"},    // bzip2
      {"9999", "no"},   // unregistered valid ID
      {"65536", "no"},  // > H5Z_FILTER_MAX -> NC_EHDFERR -> default switch arm
      {"not_a_id", "no"},  // non-numeric -> makefilterid returns NC_EINVAL
  };

  TempDir workdir("ncfilteravail_ids");
  for (const IdCase& tc : kCases) {
    SCOPED_TRACE(tc.arg);
    const std::string_view args[] = {"ncfilteravail", tc.arg};
    ExpectFilterAvailInDir(workdir, args, 0, tc.expected_stdout);
  }
}

TEST(NcfilteravailTest, AcceptsTFlagAndRejectsInvalidCliArguments) {
  TempDir workdir("ncfilteravail_args");

  constexpr std::string_view kTFlagArgs[] = {"ncfilteravail", "-t", "nc4",
                                             "deflate"};
  ExpectFilterAvailInDir(workdir, kTFlagArgs, 0, "yes");

  struct ErrorCase {
    const char* name;
    std::vector<std::string_view> args;
    std::string_view expected_stderr;
  };
  const ErrorCase kErrors[] = {
      {"no_filter", {"ncfilteravail"}, "no filter specified"},
      {"too_many_filters",
       {"ncfilteravail", "deflate", "szip"},
       "more than one filter specified"},
      {"unknown_option", {"ncfilteravail", "-Z", "deflate"}, "unknown option"},
      // Upstream getopt string is "t:" without 'h', so -h hits case '?'.
      {"dash_h_unknown", {"ncfilteravail", "-h"}, "unknown option"},
      {"missing_t_arg", {"ncfilteravail", "-t"}, "unknown option"},
  };

  for (const ErrorCase& tc : kErrors) {
    SCOPED_TRACE(tc.name);
    ExpectFilterAvailInDir(workdir, tc.args, 1, "", tc.expected_stderr);
  }
}

TEST(NcfilteravailTest, HandlesHelpAndFormatOptionsWhenGetoptRecognizesHX) {
  struct HxCase {
    const char* name;
    std::vector<std::string_view> args;
    int expected_exit;
    std::string_view expected_stdout;
    std::string_view expected_stderr;
  };
  const HxCase kCases[] = {
      {"help_flag",
       {"ncfilteravail", "-h"},
       1,
       "",
       "ncfilteravail [-t <format>]"},
      {"format_nc4", {"ncfilteravail", "-x", "nc4", "deflate"}, 0, "yes", ""},
      {"format_other",
       {"ncfilteravail", "-x", "other", "deflate"},
       0,
       "yes",
       ""},
      {"format_empty", {"ncfilteravail", "-x", "", "deflate"}, 0, "yes", ""},
      {"format_nczarr",
       {"ncfilteravail", "-x", "nczarr", "deflate"},
       1,
       "no",
       "error:"},
  };

  TempDir workdir("ncfilteravail_hx");
  constexpr char kHxOptstring[] = "ht:x:";
  for (const HxCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    ExpectFilterAvailInDir(workdir, tc.args, tc.expected_exit,
                           tc.expected_stdout, tc.expected_stderr,
                           kHxOptstring);
  }
}

TEST(NcfilteravailTest, ReportsErrorWhenTempFileCreationFails) {
  EnsureRealGetoptResolved();
  TempDir unlinked_dir("ncfilteravail_unlinked");
  ASSERT_TRUE(RunInForkedChild([&]() -> bool {
    if (::chdir(unlinked_dir.c_str()) != 0) {
      return false;
    }
    std::error_code ec;
    const bool removed = std::filesystem::remove(unlinked_dir.path(), ec);
    constexpr std::string_view kArgs[] = {"ncfilteravail", "deflate"};
    const CliResult res = RunCliInForkedChild(ncfilteravail_cli_main, kArgs);
    if (!res.ok()) {
      return false;
    }
    if (removed) {
      return res.exit_code == 1 && res.stdout_output == "no" &&
             res.stderr_output.find("error:") != std::string_view::npos;
    }
    return res.exit_code == 0 && res.stdout_output == "yes";
  }));
}

}  // namespace
}  // namespace netcdf
