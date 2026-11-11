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

// Unit tests for //third_party/netcdf/ncdump/ncpathcvt.c.
//
// Exercises path conversion across Unix/Cygwin/Windows/MSYS targets, input
// platform overrides (-I), local/input path kind queries (-k/-p), environment
// macro reporting (-X), escaping (-x), blank translation (-B), drive letters
// (-D), path list separators (-S), slash conversion (-F), and CLI error paths.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncdump/ncpathcvt.c

#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/cli_mains.h"
#include "third_party/netcdf/google/util/isolated_child.h"

namespace netcdf {
namespace {

TEST(NcpathcvtTest, PrintsUsageAndBuildEnvironmentFlags) {
  constexpr std::string_view kHelpArgs[] = {"ncpathcvt", "-h"};
  const CliResult help_res = RunCliInForkedChild(ncpathcvt_cli_main, kHelpArgs);
  ASSERT_TRUE(help_res.ok()) << help_res.harness_error;
  EXPECT_EQ(0, help_res.exit_code);
  EXPECT_THAT(help_res.stderr_output, ::testing::HasSubstr("ncpathcvt"));

  constexpr std::string_view kEnvArgs[] = {"ncpathcvt", "-X"};
  const CliResult env_res = RunCliInForkedChild(ncpathcvt_cli_main, kEnvArgs);
  ASSERT_TRUE(env_res.ok()) << env_res.harness_error;
  EXPECT_EQ(0, env_res.exit_code);
  EXPECT_THAT(env_res.stdout_output, ::testing::HasSubstr("!_WIN32"));
  EXPECT_THAT(env_res.stdout_output, ::testing::HasSubstr("!__CYGWIN__"));
}

TEST(NcpathcvtTest, ReportsLocalKindWithOverridesAndInputPathKinds) {
  struct KindCase {
    const char* name;
    std::vector<std::string_view> args;
    std::string_view expected_stdout;
  };
  const KindCase kCases[] = {
      {"local_default", {"ncpathcvt", "-k"}, "unix"},
      {"local_cygwin", {"ncpathcvt", "-Ic", "-k"}, "cygwin"},
      {"local_msys", {"ncpathcvt", "-Im", "-k"}, "msys"},
      {"local_unix", {"ncpathcvt", "-Iu", "-k"}, "unix"},
      {"local_win", {"ncpathcvt", "-Iw", "-k"}, "win"},
      {"local_reset", {"ncpathcvt", "-Iw", "-I_", "-k"}, "unix"},
      {"input_unix", {"ncpathcvt", "-p", "/alpha/beta"}, "unix"},
      {"input_msys", {"ncpathcvt", "-Im", "-p", "/c/alpha/beta"}, "msys"},
      {"input_cygwin", {"ncpathcvt", "-p", "/cygdrive/c/alpha/beta"}, "cygwin"},
      {"input_win", {"ncpathcvt", "-p", "C:\\alpha\\beta"}, "win"},
      {"input_relative", {"ncpathcvt", "-p", "rel/alpha/beta"}, "relative"},
      {"input_url_unknown",
       {"ncpathcvt", "-p", "http://example.com/data.nc"},
       "unknown"},
  };

  for (const KindCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const CliResult res = RunCliInForkedChild(ncpathcvt_cli_main, tc.args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_EQ(tc.expected_stdout, res.stdout_output);
  }
}

TEST(NcpathcvtTest, ConvertsPathsAcrossPlatformTargets) {
  struct ConvertCase {
    const char* name;
    std::vector<std::string_view> args;
    std::string_view expected_stdout;
  };
  const ConvertCase kCases[] = {
      {"default_cygwin_to_host", {"ncpathcvt", "/cygdrive/c/work"}, "/c/work"},
      {"default_win_to_host",
       {"ncpathcvt", "D:\\data\\file.nc"},
       "/D/data/file.nc"},
      {"default_unix_passthrough",
       {"ncpathcvt", "/already/unix"},
       "/already/unix"},
      {"empty_path", {"ncpathcvt", ""}, ""},
      {"unix_from_win", {"ncpathcvt", "-u", "C:\\foo\\bar"}, "/C/foo/bar"},
      {"unix_from_cygwin",
       {"ncpathcvt", "-u", "/cygdrive/D/foo/bar"},
       "/D/foo/bar"},
      {"unix_relative", {"ncpathcvt", "-u", "rel/sub"}, "rel/sub"},
      {"cygwin_from_unix",
       {"ncpathcvt", "-c", "/alpha/beta"},
       "/cygdrive/c/alpha/beta"},
      {"cygwin_from_unix_drive_d",
       {"ncpathcvt", "-c", "-D", "d", "/alpha/beta"},
       "/cygdrive/d/alpha/beta"},
      {"cygwin_special_usr",
       {"ncpathcvt", "-c", "/usr/local/bin"},
       "/usr/local/bin"},
      {"cygwin_from_win",
       {"ncpathcvt", "-c", "C:\\foo\\bar"},
       "/cygdrive/C/foo/bar"},
      {"win_from_unix", {"ncpathcvt", "-w", "/alpha/beta"}, "c:\\alpha\\beta"},
      {"win_from_unix_drive_d",
       {"ncpathcvt", "-w", "-D", "d", "/alpha/beta"},
       "d:\\alpha\\beta"},
      {"msys_alias_from_unix",
       {"ncpathcvt", "-m", "-D", "e", "/alpha/beta"},
       "e:\\alpha\\beta"},
      {"win_from_cygwin",
       {"ncpathcvt", "-w", "/cygdrive/D/foo/bar"},
       "D:\\foo\\bar"},
      {"win_relative", {"ncpathcvt", "-w", "rel/sub"}, "rel\\sub"},
  };

  for (const ConvertCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const CliResult res = RunCliInForkedChild(ncpathcvt_cli_main, tc.args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_EQ(tc.expected_stdout, res.stdout_output);
  }
}

TEST(NcpathcvtTest, HandlesEscapesBlankSubstitutionSeparatorsAndSlashes) {
  struct TransformCase {
    const char* name;
    std::vector<std::string_view> args;
    std::string_view expected_stdout;
  };
  const TransformCase kCases[] = {
      {"escape_spaces_unix",
       {"ncpathcvt", "-u", "-x", "/dir with spaces/file"},
       "/dir\\ with\\ spaces/file"},
      {"escape_spaces_and_backslashes_win",
       {"ncpathcvt", "-w", "-x", "/dir with spaces/file"},
       "c:\\\\dir\\ with\\ spaces\\\\file"},
      {"blank_substitution",
       {"ncpathcvt", "-u", "-B", "+", "/dir+with+plus/file"},
       "/dir with plus/file"},
      {"blank_and_escape",
       {"ncpathcvt", "-u", "-B", "+", "-x", "/dir+with+plus/file"},
       "/dir\\ with\\ plus/file"},
      {"default_semicolon_path_list",
       {"ncpathcvt", "-u", "/cygdrive/c/a;D:\\b"},
       "/c/a;/D/b"},
      {"custom_colon_path_list",
       {"ncpathcvt", "-w", "-S", ":", "/a:/b"},
       "c:\\a:c:\\b"},
      {"slash_convert_unix",
       {"ncpathcvt", "-u", "-F", "http://example.com/a\\b"},
       "http://example.com/a/b"},
      {"slash_convert_cygwin",
       {"ncpathcvt", "-c", "-F", "http://example.com/a\\b"},
       "http://example.com/a/b"},
      {"slash_convert_win",
       {"ncpathcvt", "-w", "-F", "http://example.com/a/b"},
       "http:\\\\example.com\\a\\b"},
      {"slash_convert_msys",
       {"ncpathcvt", "-m", "-F", "http://example.com/a/b"},
       "http:\\\\example.com\\a\\b"},
      {"slash_flag_ignored_without_target",
       {"ncpathcvt", "-F", "/a/b"},
       "/a/b"},
  };

  for (const TransformCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const CliResult res = RunCliInForkedChild(ncpathcvt_cli_main, tc.args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_EQ(tc.expected_stdout, res.stdout_output);
  }
}

TEST(NcpathcvtTest, RejectsInvalidOptionsAndMalformedArguments) {
  struct ErrorCase {
    const char* name;
    std::vector<std::string_view> args;
    std::string_view expected_stderr;
  };
  const ErrorCase kCases[] = {
      {"no_path", {"ncpathcvt"}, "no path specified"},
      {"too_many_paths",
       {"ncpathcvt", "/a", "/b"},
       "more than one path specified"},
      {"unknown_opt", {"ncpathcvt", "-Z", "/a"}, "unknown option"},
      {"bad_blank_nul", {"ncpathcvt", "-B", "", "/a"}, "Bad -B argument"},
      {"bad_blank_del", {"ncpathcvt", "-B", "\x7f", "/a"}, "Bad -B argument"},
  };

  for (const ErrorCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const CliResult res = RunCliInForkedChild(ncpathcvt_cli_main, tc.args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stderr_output, ::testing::HasSubstr(tc.expected_stderr));
  }
}

}  // namespace
}  // namespace netcdf
