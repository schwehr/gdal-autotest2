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

// Unit tests for //third_party/netcdf/ncdump/echon.c.
//
// Exercises argument concatenation, escape sequence decoding, -e/-x/-X/-n/-N
// options, unknown flag handling, and the upstream -N buffer-pointer bug inside
// isolated forked children.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncdump/echon.c

#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/cli_mains.h"
#include "third_party/netcdf/google/util/isolated_child.h"

namespace netcdf {
namespace {

TEST(EchonTest, ConcatenatesZeroOneAndMultiplePositionalArgs) {
  struct ConcatCase {
    const char* name;
    std::vector<std::string_view> args;
    std::string_view expected_stdout;
  };
  const ConcatCase kCases[] = {
      {"zero_args", {"echon"}, ""},
      {"single_arg", {"echon", "alpha"}, "alpha"},
      {"multi_args", {"echon", "alpha", "beta", "gamma"}, "alpha beta gamma"},
  };

  for (const ConcatCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const CliResult res = RunCliInForkedChild(echon_cli_main, tc.args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code);
    EXPECT_EQ(tc.expected_stdout, res.stdout_output);
  }
}

TEST(EchonTest, DecodesSupportedAndFallbackEscapeSequences) {
  struct EscapeCase {
    const char* name;
    std::string_view input;
    std::string_view expected_stdout;
  };
  constexpr EscapeCase kCases[] = {
      {"bell", "\\a", "\a"},
      {"backspace", "\\b", "\b"},
      {"formfeed", "\\f", "\f"},
      {"carriage_return", "\\r", "\r"},
      {"newline", "\\n", "\n"},
      {"tab", "\\t", "\t"},
      {"vertical_tab", "\\v", "\v"},
      {"escaped_backslash", "\\\\", "\\"},
      {"unknown_escape", "\\q", "q"},
      {"mixed", "row1\\tcol2\\n\\a\\b\\f\\r\\v\\\\end\\z",
       "row1\tcol2\n\a\b\f\r\v\\endz"},
  };

  for (const EscapeCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const std::string_view args[] = {"echon", tc.input};
    const CliResult res = RunCliInForkedChild(echon_cli_main, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code);
    EXPECT_EQ(tc.expected_stdout, res.stdout_output);
  }
}

TEST(EchonTest, HandlesEscapeAndNewlineOptionFlags) {
  struct FlagCase {
    const char* name;
    std::vector<std::string_view> args;
    std::string_view expected_stdout;
  };
  const FlagCase kCases[] = {
      {"raw_no_escape", {"echon", "-X", "a\\nb\\tc"}, "a\\nb\\tc"},
      {"toggle_escape_back_on", {"echon", "-X", "-x", "a\\nb"}, "a\nb"},
      {"explicit_noeol", {"echon", "-n", "plain"}, "plain"},
      // Upstream echon.c line 99 writes '\n' to `p` (`input`) instead of `q`
      // (`output`), so `-N` leaves stdout without a trailing newline. Passing a
      // non-empty positional arg ensures `input` has room for the write.
      {"upstream_noeol_off", {"echon", "-N", "plain"}, "plain"},
      {"toggle_noeol_back_on", {"echon", "-N", "-n", "plain"}, "plain"},
  };

  for (const FlagCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const CliResult res = RunCliInForkedChild(echon_cli_main, tc.args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code);
    EXPECT_EQ(tc.expected_stdout, res.stdout_output);
  }
}

TEST(EchonTest, StopsOptionParsingAtDashEAndIgnoresUnknownFlags) {
  constexpr std::string_view kDashEArgs[] = {"echon", "-e", "-X", "a\\nb"};
  const CliResult dash_e_res = RunCliInForkedChild(echon_cli_main, kDashEArgs);
  ASSERT_TRUE(dash_e_res.ok()) << dash_e_res.harness_error;
  EXPECT_EQ(0, dash_e_res.exit_code);
  EXPECT_EQ("-X a\nb", dash_e_res.stdout_output);

  constexpr std::string_view kUnknownArgs[] = {"echon", "-Z", "kept"};
  const CliResult unknown_res =
      RunCliInForkedChild(echon_cli_main, kUnknownArgs);
  ASSERT_TRUE(unknown_res.ok()) << unknown_res.harness_error;
  EXPECT_EQ(0, unknown_res.exit_code);
  EXPECT_EQ("kept", unknown_res.stdout_output);
}

}  // namespace
}  // namespace netcdf
