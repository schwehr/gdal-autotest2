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

// Unit tests for //third_party/netcdf/ncgen/main.c.
//
// Exercises option parsing, format selection, BOM handling, CDL input streams,
// binary/C output modes, and CLI error exits inside isolated forked children.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncgen/main.c

#include <filesystem>  // NOLINT
#include <fstream>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/cli_mains.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
// clang-format on

namespace netcdf {
namespace {

constexpr std::string_view kSimpleCdl =
    "netcdf simple {\n"
    "dimensions:\n"
    "  x = 2;\n"
    "variables:\n"
    "  int a(x);\n"
    "data:\n"
    "  a = 10, 20;\n"
    "}\n";

constexpr std::string_view kEnhancedCdl =
    "netcdf enhanced {\n"
    "group: g {\n"
    "dimensions:\n"
    "  x = 2;\n"
    "variables:\n"
    "  int v(x);\n"
    "data:\n"
    "  v = 1, 2;\n"
    "}\n"
    "}\n";

constexpr std::string_view kFormatAttrCdl =
    "netcdf fmt_attr {\n"
    "dimensions:\n"
    "  x = 2;\n"
    "variables:\n"
    "  int v(x);\n"
    ":_Format = \"netCDF-4\";\n"
    "data:\n"
    "  v = 3, 4;\n"
    "}\n";

TEST(MainTest, HelpOptionPrintsUsageAndExitsZero) {
  constexpr std::string_view kArgs[] = {"/usr/bin/ncgen", "-h"};
  const CliResult res = RunCliInForkedChild(ncgen_cli_main, kArgs);
  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code);
  EXPECT_THAT(res.stderr_output, ::testing::HasSubstr("Usage: ncgen"));
}

TEST(MainTest, GeneratesBinaryOutputsAcrossFormats) {
  struct FormatCase {
    const char* name;
    std::string_view flag;
    int expected_format;
  };
  constexpr FormatCase kCases[] = {
      {"classic", "-3", NC_FORMAT_CLASSIC},
      {"offset64", "-6", NC_FORMAT_64BIT_OFFSET},
      {"cdf5", "-5", NC_FORMAT_CDF5},
      {"nc4", "-4", NC_FORMAT_NETCDF4},
      {"nc4_classic", "-7", NC_FORMAT_NETCDF4_CLASSIC},
      {"k_nc3", "classic", NC_FORMAT_CLASSIC},
      {"k_nc4", "netCDF-4", NC_FORMAT_NETCDF4},
  };

  for (const FormatCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    TempFile out_nc(std::string("out_") + tc.name + ".nc");
    const std::string out_path = out_nc.string();
    const bool use_k = (tc.flag[0] != '-');
    CliResult res;
    if (use_k) {
      TempFile unused_nc(std::string("unused_") + tc.name + ".nc");
      const std::string unused_path = unused_nc.string();
      const std::string_view args[] = {"ncgen",     "-v", tc.flag,     "-N",
                                       "first_ds",  "-N", "second_ds", "-o",
                                       unused_path, "-o", out_path,    "-b",
                                       "-"};
      res = RunCliInForkedChild(ncgen_cli_main, args, kSimpleCdl);
    } else {
      const std::string_view args[] = {
          "ncgen", tc.flag, "-N", "custom_ds", "-x", "-W",     "16",
          "-L",    "0",     "-B", "ignored",   "-o", out_path, "-"};
      res = RunCliInForkedChild(ncgen_cli_main, args, kSimpleCdl);
    }
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, nc.ptr()));
    int actual_format = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_format(nc.get(), &actual_format));
    EXPECT_EQ(tc.expected_format, actual_format);
  }
}

TEST(MainTest, GeneratesCOutputAndHandlesDebugAndSyntaxOnly) {
  constexpr std::string_view kCArgs[] = {"ncgen",    "-d",  "-H", "-M",
                                         "gen_main", "-lc", "-"};
  const CliResult c_res =
      RunCliInForkedChild(ncgen_cli_main, kCArgs, kSimpleCdl);
  ASSERT_TRUE(c_res.ok()) << c_res.harness_error;
  EXPECT_EQ(0, c_res.exit_code) << c_res.stderr_output;
  EXPECT_THAT(c_res.stdout_output, ::testing::HasSubstr("nc_create"));

  constexpr std::string_view kDeprecatedCArgs[] = {"ncgen", "-D", "2", "-c",
                                                   "-"};
  const CliResult dep_c_res =
      RunCliInForkedChild(ncgen_cli_main, kDeprecatedCArgs, kSimpleCdl);
  ASSERT_TRUE(dep_c_res.ok()) << dep_c_res.harness_error;
  EXPECT_EQ(0, dep_c_res.exit_code);
  EXPECT_THAT(dep_c_res.stderr_output, ::testing::HasSubstr("deprecated"));

  TempDir tmpdir("ncgen_n_mode");
  TempFile out_cdf("ncgen_n_mode/sample.cdf");
  const std::string out_path = out_cdf.string();
  const std::string_view n_args[] = {"ncgen", "-n", "-o", out_path, "-"};
  const CliResult n_res =
      RunCliInForkedChild(ncgen_cli_main, n_args, kSimpleCdl);
  ASSERT_TRUE(n_res.ok()) << n_res.harness_error;
  EXPECT_EQ(0, n_res.exit_code);

  // Syntax-only default when neither -l nor -k/-o is given.
  constexpr std::string_view kSyntaxArgs[] = {"ncgen", "-"};
  const CliResult syntax_res =
      RunCliInForkedChild(ncgen_cli_main, kSyntaxArgs, kSimpleCdl);
  ASSERT_TRUE(syntax_res.ok()) << syntax_res.harness_error;
  EXPECT_EQ(0, syntax_res.exit_code);
}

TEST(MainTest, ReadsDiskFilesWithUtf8BomAndRejectsInvalidBoms) {
  TempFile cdl_file("with_bom.cdl");
  {
    std::ofstream out(cdl_file.path(), std::ios::binary);
    out << "\xEF\xBB\xBF" << kSimpleCdl;
  }
  const std::string cdl_path = cdl_file.string();
  const std::string_view args[] = {"ncgen", cdl_path};
  const CliResult ok_res = RunCliInForkedChild(ncgen_cli_main, args);
  ASSERT_TRUE(ok_res.ok()) << ok_res.harness_error;
  EXPECT_EQ(0, ok_res.exit_code) << ok_res.stderr_output;

  for (const unsigned char lead : {0x00, 0xFF, 0xFE}) {
    TempFile bad_bom("bad_bom.cdl");
    {
      std::ofstream out(bad_bom.path(), std::ios::binary);
      out.put(static_cast<char>(lead));
      out.put('\x01');
      out << kSimpleCdl;
    }
    const std::string bad_path = bad_bom.string();
    const std::string_view bad_args[] = {"ncgen", bad_path};
    const CliResult bad_res = RunCliInForkedChild(ncgen_cli_main, bad_args);
    ASSERT_TRUE(bad_res.ok()) << bad_res.harness_error;
    EXPECT_EQ(1, bad_res.exit_code);
    EXPECT_THAT(bad_res.stderr_output, ::testing::HasSubstr("BOM"));
  }
}

TEST(MainTest, HandlesUnsupportedAndInvalidArguments) {
  struct ErrCase {
    const char* name;
    std::vector<std::string_view> args;
    int expected_code;
  };
  const ErrCase kErrCases[] = {
      {"dup_lang_b", {"ncgen", "-c", "-b"}, 1},
      {"dup_lang_c", {"ncgen", "-b", "-c"}, 1},
      {"dup_lang_f", {"ncgen", "-b", "-f"}, 1},
      {"dup_lang_l", {"ncgen", "-b", "-lc"}, 1},
      {"dup_lang_n", {"ncgen", "-b", "-n"}, 1},
      {"bad_lang", {"ncgen", "-l", "python"}, 1},
      {"f_flag_disabled", {"ncgen", "-f"}, 1},
      {"f77_disabled", {"ncgen", "-lf77"}, 1},
      {"java_disabled", {"ncgen", "-ljava"}, 1},
      {"bad_kind", {"ncgen", "-k", "no_such_format"}, 2},
      {"unknown_opt", {"ncgen", "-Z"}, 8},
      {"too_many_inputs", {"ncgen", "a.cdl", "b.cdl"}, 6},
      {"missing_file", {"ncgen", "/nonexistent/ncgen_input.cdl"}, 7},
  };

  for (const ErrCase& tc : kErrCases) {
    SCOPED_TRACE(tc.name);
    const CliResult res =
        RunCliInForkedChild(ncgen_cli_main, tc.args, kSimpleCdl);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(tc.expected_code, res.exit_code) << res.stderr_output;
  }

  // Syntax error inside input CDL exits with 1.
  constexpr std::string_view kSyntaxErrArgs[] = {"ncgen", "-"};
  const CliResult parse_err =
      RunCliInForkedChild(ncgen_cli_main, kSyntaxErrArgs, "not valid cdl {{{");
  ASSERT_TRUE(parse_err.ok()) << parse_err.harness_error;
  EXPECT_EQ(1, parse_err.exit_code);

  // Enhanced CDL with conflicting -k classic emits an error diagnostic.
  TempFile conflict_out("conflict_out.nc");
  const std::string conflict_path = conflict_out.string();
  const std::string_view kConflictArgs[] = {"ncgen", "-3", "-o", conflict_path,
                                            "-"};
  const CliResult conflict_res =
      RunCliInForkedChild(ncgen_cli_main, kConflictArgs, kEnhancedCdl);
  ASSERT_TRUE(conflict_res.ok()) << conflict_res.harness_error;
  EXPECT_EQ(0, conflict_res.exit_code);
  EXPECT_THAT(conflict_res.stderr_output, ::testing::HasSubstr("conflicts"));
}

TEST(MainTest, EnhancedCdlFormatAttrAndLongInputFilenameWork) {
  // Exercise enhanced_flag default format selection (k_flag == 0).
  constexpr std::string_view kEnhArgs[] = {"ncgen", "-"};
  const CliResult enh_res =
      RunCliInForkedChild(ncgen_cli_main, kEnhArgs, kEnhancedCdl);
  ASSERT_TRUE(enh_res.ok()) << enh_res.harness_error;
  EXPECT_EQ(0, enh_res.exit_code) << enh_res.stderr_output;

  // Exercise globalspecials._Format fallback when -k is omitted.
  TempFile fmt_out("fmt_attr_out.nc");
  const std::string fmt_path = fmt_out.string();
  const std::string_view fmt_args[] = {"ncgen", "-o", fmt_path, "-"};
  const CliResult fmt_res =
      RunCliInForkedChild(ncgen_cli_main, fmt_args, kFormatAttrCdl);
  ASSERT_TRUE(fmt_res.ok()) << fmt_res.harness_error;
  EXPECT_EQ(0, fmt_res.exit_code) << fmt_res.stderr_output;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_open(fmt_out.c_str(), NC_NOWRITE, nc.ptr()));
    int actual_format = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_format(nc.get(), &actual_format));
    EXPECT_EQ(NC_FORMAT_NETCDF4, actual_format);
  }

  // Exercise cdlname truncation when path > NC_MAX_NAME (256 chars) across
  // nested subdirectories whose individual path components fit NAME_MAX.
  TempDir long_root("ncgen_long");
  TempFile long_cdl(std::string("ncgen_long/") + std::string(140, 'a') + "/" +
                    std::string(140, 'b') + "/sample.cdl");
  {
    std::ofstream out(long_cdl.path());
    out << kSimpleCdl;
  }
  const std::string long_path = long_cdl.string();
  ASSERT_GT(long_path.size(), static_cast<size_t>(NC_MAX_NAME));
  const std::string_view long_args[] = {"ncgen", long_path};
  const CliResult long_res = RunCliInForkedChild(ncgen_cli_main, long_args);
  ASSERT_TRUE(long_res.ok()) << long_res.harness_error;
  EXPECT_EQ(0, long_res.exit_code) << long_res.stderr_output;
}

}  // namespace
}  // namespace netcdf
