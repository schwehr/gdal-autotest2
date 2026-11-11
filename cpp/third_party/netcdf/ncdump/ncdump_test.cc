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

// Unit tests for //third_party/netcdf/ncdump/ncdump.c.
//
// Covers CLI option parsing and diagnostics, path-to-dataset-name derivation,
// format/extended-format reporting (-k/-K), CDL formatting across classic and
// netCDF-4 datasets (keywords, escapes, special/hidden/filter attributes, time
// and bounds variables, user-defined types, groups, and cross-group
// dimensions), variable/group subsetting (-v/-g/-c/-h/-b/-f/-l/-d/-p/-L/-X/-F),
// and NcML XML (-x) output.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncdump/ncdump.c

#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

using ::testing::HasSubstr;
using ::testing::Not;

std::string ReadWholeFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void CreateMinimalFileWithFormat(const char* path, int cmode) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR, nc_create(path, cmode, nc.ptr()));
  int dimid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 2, &dimid));
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "v", NC_INT, 1, &dimid, &varid));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));
  constexpr int kVals[2] = {10, 20};
  ASSERT_EQ(NC_NOERR, nc_put_var_int(nc.get(), varid, kVals));
  ASSERT_EQ(NC_NOERR, nc.close());
}

TEST(NcdumpTest, CliUsageAndInvalidArguments) {
  // No positional arguments and no options (argc <= 1) prints usage & exits 0.
  {
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, {});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code);
    EXPECT_THAT(res.stderr_output, HasSubstr("netcdf library version"));
  }

  struct ErrorCase {
    const char* name;
    std::vector<std::string_view> args;
    std::string_view expected_stderr;
  };
  const ErrorCase kCases[] = {
      {"no_file_arg_with_flag", {"ncdump", "-h"}, "netcdf library version"},
      {"too_many_files", {"ncdump", "a.nc", "b.nc"}, "netcdf library version"},
      {"unknown_option", {"ncdump", "-Z", "a.nc"}, "netcdf library version"},
      {"bad_b_opt", {"ncdump", "-b", "z", "a.nc"}, "invalid value for -b"},
      {"bad_f_opt", {"ncdump", "-f", "z", "a.nc"}, "invalid value for -f"},
      {"bad_l_opt",
       {"ncdump", "-l", "9", "a.nc"},
       "unreasonably small line length"},
      {"bad_X_opt", {"ncdump", "-X", "z", "a.nc"}, "invalid value for -X"},
      {"bad_d_low",
       {"ncdump", "-d", "0", "a.nc"},
       "unreasonable value for float"},
      {"bad_d_high",
       {"ncdump", "-d", "21", "a.nc"},
       "unreasonable value for float"},
      {"bad_d_dbl_low",
       {"ncdump", "-d", "3,0", "a.nc"},
       "unreasonable value for double"},
      {"bad_d_dbl_high",
       {"ncdump", "-d", "3,21", "a.nc"},
       "unreasonable value for double"},
      {"bad_d_dbl_empty",
       {"ncdump", "-d", "3,", "a.nc"},
       "unreasonable value for double"},
      {"bad_p_low",
       {"ncdump", "-p", "0", "a.nc"},
       "unreasonable value for float"},
      {"bad_p_high",
       {"ncdump", "-p", "21", "a.nc"},
       "unreasonable value for float"},
      {"bad_p_dbl_low",
       {"ncdump", "-p", "3,0", "a.nc"},
       "unreasonable value for double"},
      {"bad_p_dbl_empty",
       {"ncdump", "-p", "3,", "a.nc"},
       "unreasonable value for double"},
      {"missing_file", {"ncdump", "/nonexistent/ncdump_missing.nc"}, ""},
      {"file_url_with_w",
       {"ncdump", "-w", "file:///nonexistent/ncdump_url_test.nc"},
       ""},
  };

  for (const ErrorCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, tc.args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code) << res.stderr_output;
    if (!tc.expected_stderr.empty()) {
      EXPECT_THAT(res.stderr_output, HasSubstr(tc.expected_stderr));
    }
  }
}

TEST(NcdumpTest, NamePathDerivationAndCustomName) {
  TempDir dir("ncdump_name_path");
  TempFile dot_nc("ncdump_name_path/dataset_dot.nc");
  TempFile no_ext("ncdump_name_path/plainfile");
  CreateMinimalFileWithFormat(dot_nc.c_str(), NC_CLOBBER);
  CreateMinimalFileWithFormat(no_ext.c_str(), NC_CLOBBER);

  // Absolute path with '/' and '.nc' extension.
  {
    const std::string path = dot_nc.string();
    const std::string_view args[] = {"ncdump", path};
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_THAT(res.stdout_output, HasSubstr("netcdf dataset_dot {"));
  }

  // Absolute path with '/' and no '.' extension.
  {
    const std::string path = no_ext.string();
    const std::string_view args[] = {"ncdump", path};
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_THAT(res.stdout_output, HasSubstr("netcdf plainfile {"));
  }

  // Explicit dataset name via -n and log-level -L option.
  {
    const std::string path = dot_nc.string();
    const std::string_view args[] = {"ncdump", "-n", "custom_name",
                                     "-L",     "0",  path};
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_THAT(res.stdout_output, HasSubstr("netcdf custom_name {"));
  }

  // Relative filenames without '/' in child cwd exercise the cp == NULL
  // branch of name_path() with and without a '.' suffix.
  const std::string dir_str = dir.string();
  for (const auto& [rel_name, expected_hdr] : {
           std::pair<const char*, const char*>{"dataset_dot.nc",
                                               "netcdf dataset_dot {"},
           std::pair<const char*, const char*>{"plainfile",
                                               "netcdf plainfile {"},
       }) {
    SCOPED_TRACE(rel_name);
    TempFile out_txt(std::string("ncdump_name_path/out_") + rel_name + ".txt");
    const std::string out_path = out_txt.string();
    const char* target_arg = rel_name;
    EXPECT_TRUE(RunInForkedChild([&]() {
      if (::chdir(dir_str.c_str()) != 0) {
        return false;
      }
      const int fd =
          ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd < 0) {
        return false;
      }
      if (::dup2(fd, STDOUT_FILENO) < 0) {
        ::close(fd);
        return false;
      }
      ::close(fd);
      optind = 1;
      opterr = 1;
      optopt = 0;
      char arg0[] = "ncdump";
      std::string arg1_buf(target_arg);
      char* argv[] = {arg0, arg1_buf.data(), nullptr};
      return ncdump_cli_main(2, argv) == 0;
    }));
    EXPECT_THAT(ReadWholeFile(out_txt.path()), HasSubstr(expected_hdr));
  }
}

TEST(NcdumpTest, ReportsKindAndExtendedFormatAcrossAllFormats) {
  struct FormatCase {
    const char* name;
    int cmode;
    const char* expected_kind;
    const char* expected_ext;
  };
  constexpr FormatCase kCases[] = {
      {"classic", NC_CLOBBER, "classic\n", "classic mode="},
      {"offset64", NC_CLOBBER | NC_64BIT_OFFSET, "64-bit offset\n",
       "64-bit offset mode="},
      {"cdf5", NC_CLOBBER | NC_CDF5, "cdf5\n", "64-bit data mode="},
      {"nc4", NC_CLOBBER | NC_NETCDF4, "netCDF-4\n", "HDF5 mode="},
      {"nc4_classic", NC_CLOBBER | NC_NETCDF4 | NC_CLASSIC_MODEL,
       "netCDF-4 classic model\n", "HDF5 mode="},
  };

  for (const FormatCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    TempFile nc_file(std::string("fmt_") + tc.name + ".nc");
    CreateMinimalFileWithFormat(nc_file.c_str(), tc.cmode);
    const std::string path = nc_file.string();

    {
      const std::string_view k_args[] = {"ncdump", "-k", path};
      const CliResult res = RunCliInForkedChild(ncdump_cli_main, k_args);
      ASSERT_TRUE(res.ok()) << res.harness_error;
      EXPECT_EQ(0, res.exit_code) << res.stderr_output;
      EXPECT_EQ(tc.expected_kind, res.stdout_output);
    }

    {
      const std::string_view ext_args[] = {"ncdump", "-K", path};
      const CliResult res = RunCliInForkedChild(ncdump_cli_main, ext_args);
      ASSERT_TRUE(res.ok()) << res.harness_error;
      EXPECT_EQ(0, res.exit_code) << res.stderr_output;
      EXPECT_THAT(res.stdout_output, HasSubstr(tc.expected_ext));
    }
  }

  // Exercise -Xm (NC_DISKLESS | NC_NOWRITE) open path for classic & netCDF-4.
  for (const auto& [tag, cmode] : {
           std::pair<const char*, int>{"xm_classic", NC_CLOBBER},
           std::pair<const char*, int>{"xm_nc4", NC_CLOBBER | NC_NETCDF4},
       }) {
    SCOPED_TRACE(tag);
    TempFile xm_file(std::string(tag) + ".nc");
    CreateMinimalFileWithFormat(xm_file.c_str(), cmode);
    const std::string path = xm_file.string();
    const std::string_view args[] = {"ncdump", "-Xm", path};
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_THAT(res.stdout_output, HasSubstr("v = 10, 20 ;"));
  }
}

TEST(NcdumpTest, ClassicFormatCdlAttributesKeywordsTimeAndErrors) {
  TempFile classic_nc("classic_full.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(classic_nc.c_str(), NC_CLOBBER, nc.ptr()));

    int d_x = -1;
    int d_nv = -1;
    int d_time = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 2, &d_x));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "nv", 2, &d_nv));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "time", NC_UNLIMITED, &d_time));

    // Variable names matching CDL keywords trigger escaped keyword spacing.
    const char* kKeywords[] = {"variables", "dimensions", "data", "group",
                               "types"};
    std::vector<int> kw_varids;
    for (const char* kw : kKeywords) {
      int vid = -1;
      ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), kw, NC_INT, 1, &d_x, &vid));
      ASSERT_EQ(NC_NOERR, nc_put_att_text(nc.get(), vid, "long_name", 2, "kw"));
      kw_varids.push_back(vid);
    }

    // Time variables, bounds variable, and time-formatted numeric attributes.
    int v_time = -1;
    int v_time_bnds = -1;
    int v_time2 = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc.get(), "time", NC_DOUBLE, 1, &d_time, &v_time));
    const int bnds_dims[2] = {d_time, d_nv};
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "time_bnds", NC_DOUBLE, 2,
                                   bnds_dims, &v_time_bnds));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc.get(), "time2", NC_DOUBLE, 1, &d_time, &v_time2));

    constexpr char kUnitsDays[] = "days since 2000-01-01";
    constexpr char kCalendar[] = "standard";
    constexpr char kBoundsName[] = "time_bnds";
    ASSERT_EQ(NC_NOERR, nc_put_att_text(nc.get(), v_time, "units",
                                        sizeof(kUnitsDays) - 1, kUnitsDays));
    ASSERT_EQ(NC_NOERR, nc_put_att_text(nc.get(), v_time, "calendar",
                                        sizeof(kCalendar) - 1, kCalendar));
    ASSERT_EQ(NC_NOERR, nc_put_att_text(nc.get(), v_time, "bounds",
                                        sizeof(kBoundsName) - 1, kBoundsName));
    // Two values -> inline time comment; three values -> wrapped time comment.
    constexpr double kActualRange[2] = {0.0, 10.0};
    constexpr double kValidRange[3] = {0.0, 5.0, 10.0};
    ASSERT_EQ(NC_NOERR, nc_put_att_double(nc.get(), v_time, "actual_range",
                                          NC_DOUBLE, 2, kActualRange));
    ASSERT_EQ(NC_NOERR, nc_put_att_double(nc.get(), v_time, "valid_range",
                                          NC_DOUBLE, 3, kValidRange));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_text(nc.get(), v_time, "non_numeric_time", 3, "abc"));

    constexpr char kUnitsHours[] = "hours since 2000-01-01";
    ASSERT_EQ(NC_NOERR, nc_put_att_text(nc.get(), v_time2, "units",
                                        sizeof(kUnitsHours) - 1, kUnitsHours));

    // Variables without _FillValue for classic types (get_fill_info paths) +
    // one explicit _FillValue.
    int v_byte = -1;
    int v_char = -1;
    int v_short = -1;
    int v_int = -1;
    int v_float = -1;
    int v_double = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc.get(), "v_byte", NC_BYTE, 1, &d_x, &v_byte));
    constexpr signed char kByteFill = -10;
    ASSERT_EQ(NC_NOERR, nc_put_att_schar(nc.get(), v_byte, NC_FillValue,
                                         NC_BYTE, 1, &kByteFill));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc.get(), "v_char", NC_CHAR, 1, &d_x, &v_char));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc.get(), "v_short", NC_SHORT, 1, &d_x, &v_short));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "v_int", NC_INT, 1, &d_x, &v_int));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc.get(), "v_float", NC_FLOAT, 1, &d_x, &v_float));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc.get(), "v_double", NC_DOUBLE, 1, &d_x, &v_double));

    // Global attributes: escaped chars with trailing NULs (in classic format,
    // '\n' splits CDL literal), zero-length att, explicit _NCProperties,
    // special float/double values (tztrim + NaN / +Inf / -Inf).
    constexpr char kCdlEscBytes[] = {'\b', '\f', '\n',   '\r', '\t', '\v', '\\',
                                     '\'', '"',  '\x01', 'A',  '\0', '\0'};
    ASSERT_EQ(NC_NOERR, nc_put_att_text(nc.get(), NC_GLOBAL, "esc_text",
                                        sizeof(kCdlEscBytes), kCdlEscBytes));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_text(nc.get(), NC_GLOBAL, "empty_text", 0, ""));
    constexpr short kShortAttVals[2] = {-12, 34};
    ASSERT_EQ(NC_NOERR, nc_put_att_short(nc.get(), NC_GLOBAL, "g_short",
                                         NC_SHORT, 2, kShortAttVals));
    constexpr char kFakeProps[] = "version=2,classic=test";
    ASSERT_EQ(NC_NOERR, nc_put_att_text(nc.get(), NC_GLOBAL, "_NCProperties",
                                        sizeof(kFakeProps) - 1, kFakeProps));

    const float kFloatAttVals[] = {
        -1.5f,
        1.5f,
        1.234567f,
        1000000.0f,
        1.5e10f,
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
    };
    ASSERT_EQ(NC_NOERR, nc_put_att_float(nc.get(), NC_GLOBAL, "g_floats",
                                         NC_FLOAT, 8, kFloatAttVals));

    const double kDoubleAttVals[] = {
        -2.5,
        2.5,
        1.23456789012345,
        100000000000000.0,
        -100000000000000.0,
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
    };
    ASSERT_EQ(NC_NOERR, nc_put_att_double(nc.get(), NC_GLOBAL, "g_doubles",
                                          NC_DOUBLE, 8, kDoubleAttVals));

    ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));

    constexpr int kKwVals[2] = {1, 2};
    for (int vid : kw_varids) {
      ASSERT_EQ(NC_NOERR, nc_put_var_int(nc.get(), vid, kKwVals));
    }

    constexpr size_t kZero = 0;
    constexpr size_t kTwo = 2;
    constexpr double kTimeVals[2] = {0.0, 5.0};
    ASSERT_EQ(NC_NOERR,
              nc_put_vara_double(nc.get(), v_time, &kZero, &kTwo, kTimeVals));
    constexpr size_t kBndsStart[2] = {0, 0};
    constexpr size_t kBndsCount[2] = {2, 2};
    constexpr double kBndsVals[4] = {0.0, 1.0, 5.0, 6.0};
    ASSERT_EQ(NC_NOERR, nc_put_vara_double(nc.get(), v_time_bnds, kBndsStart,
                                           kBndsCount, kBndsVals));
    constexpr double kTime2Vals[2] = {0.0, 12.0};
    ASSERT_EQ(NC_NOERR,
              nc_put_vara_double(nc.get(), v_time2, &kZero, &kTwo, kTime2Vals));

    constexpr signed char kBytes[2] = {-10, 42};
    constexpr char kChars[2] = {'x', 'y'};
    constexpr short kShorts[2] = {NC_FILL_SHORT, 7};
    constexpr int kInts[2] = {NC_FILL_INT, 99};
    constexpr float kFloats[2] = {NC_FILL_FLOAT, 3.25f};
    constexpr double kDoubles[2] = {NC_FILL_DOUBLE, 6.5};
    ASSERT_EQ(NC_NOERR, nc_put_var_schar(nc.get(), v_byte, kBytes));
    ASSERT_EQ(NC_NOERR, nc_put_var_text(nc.get(), v_char, kChars));
    ASSERT_EQ(NC_NOERR, nc_put_var_short(nc.get(), v_short, kShorts));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc.get(), v_int, kInts));
    ASSERT_EQ(NC_NOERR, nc_put_var_float(nc.get(), v_float, kFloats));
    ASSERT_EQ(NC_NOERR, nc_put_var_double(nc.get(), v_double, kDoubles));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  const std::string path = classic_nc.string();

  // Dump with -s -t -b c -d 4,6 (time comments, bounds, specials, -d
  // float,dbl).
  {
    const std::string_view args[] = {"ncdump", "-s", "-t",  "-b",
                                     "c",      "-d", "4,6", path};
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_THAT(res.stdout_output,
                HasSubstr("variables :long_name = \"kw\" ;"));
    EXPECT_THAT(res.stdout_output, HasSubstr("types :long_name = \"kw\" ;"));
    EXPECT_THAT(res.stdout_output, HasSubstr("\\n\",\n\t\t\t\""));
    EXPECT_THAT(res.stdout_output, HasSubstr(":empty_text = \"\" ;"));
    EXPECT_THAT(res.stdout_output, HasSubstr(":g_short = -12s, 34s ;"));
    EXPECT_THAT(res.stdout_output, HasSubstr("NaNf, Infinityf, -Infinityf"));
    EXPECT_THAT(res.stdout_output, HasSubstr("NaN, Infinity, -Infinity"));
    EXPECT_THAT(res.stdout_output,
                HasSubstr(":_NCProperties = \"version=2,classic=test\""));
    EXPECT_THAT(res.stdout_output, HasSubstr(":_Format = \"classic\" ;"));
    EXPECT_THAT(res.stdout_output, HasSubstr("\"2000-01-01\""));
    EXPECT_THAT(res.stdout_output, HasSubstr("\"2000-01-06\""));
  }

  // Dump with -i -f f -l 20 -p 3,5 (ISO 'T', Fortran comments, line wrap).
  {
    const std::string_view args[] = {"ncdump", "-i", "-f",  "f", "-l",
                                     "20",     "-p", "3,5", path};
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_THAT(res.stdout_output, HasSubstr("2000-01-01T"));
  }

  // Dump with -b f -v time_bnds (brief Fortran comments on rank > 1 var),
  // -f c -v time_bnds (full C index comments), and -t -v time (ensures
  // var.timeinfo is non-null at done: cleanup in do_ncdump_rec).
  {
    const std::string_view bf_args[] = {"ncdump", "-b",        "f",
                                        "-v",     "time_bnds", path};
    const CliResult bf_res = RunCliInForkedChild(ncdump_cli_main, bf_args);
    ASSERT_TRUE(bf_res.ok()) << bf_res.harness_error;
    EXPECT_EQ(0, bf_res.exit_code) << bf_res.stderr_output;
    EXPECT_THAT(bf_res.stdout_output, HasSubstr("// time_bnds(1-2 ,1)"));
    EXPECT_THAT(bf_res.stdout_output, HasSubstr("// time_bnds(1-2 ,2)"));

    const std::string_view fc_args[] = {"ncdump", "-f",        "c",
                                        "-v",     "time_bnds", path};
    const CliResult fc_res = RunCliInForkedChild(ncdump_cli_main, fc_args);
    ASSERT_TRUE(fc_res.ok()) << fc_res.harness_error;
    EXPECT_EQ(0, fc_res.exit_code) << fc_res.stderr_output;
    EXPECT_THAT(fc_res.stdout_output, HasSubstr("// time_bnds(0,0)"));
    EXPECT_THAT(fc_res.stdout_output, HasSubstr("// time_bnds(1,1)"));

    const std::string_view t_args[] = {"ncdump", "-t", "-v", "time", path};
    const CliResult t_res = RunCliInForkedChild(ncdump_cli_main, t_args);
    ASSERT_TRUE(t_res.ok()) << t_res.harness_error;
    EXPECT_EQ(0, t_res.exit_code) << t_res.stderr_output;
    EXPECT_THAT(t_res.stdout_output, HasSubstr("\"2000-01-01\""));
  }

  // Single float precision and comma-only double precision variants for -d/-p.
  for (const auto& [opt, val] : {
           std::pair<const char*, const char*>{"-d", "4"},
           std::pair<const char*, const char*>{"-d", ",6"},
           std::pair<const char*, const char*>{"-p", "3"},
           std::pair<const char*, const char*>{"-p", ",5"},
       }) {
    const std::string_view args[] = {"ncdump", "-h", opt, val, path};
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
  }

  // Error paths on classic file: -g on non-nc4 and missing -v variable.
  {
    const std::string_view g_args[] = {"ncdump", "-g", "somegrp", path};
    const CliResult g_res = RunCliInForkedChild(ncdump_cli_main, g_args);
    ASSERT_TRUE(g_res.ok()) << g_res.harness_error;
    EXPECT_EQ(1, g_res.exit_code);

    const std::string_view v_args[] = {"ncdump", "-v", "no_such_var", path};
    const CliResult v_res = RunCliInForkedChild(ncdump_cli_main, v_args);
    ASSERT_TRUE(v_res.ok()) << v_res.harness_error;
    EXPECT_EQ(1, v_res.exit_code);
    EXPECT_THAT(v_res.stderr_output,
                HasSubstr("no_such_var: No such variable"));
  }

  // 64-bit offset format with -s hits kind == 2 early return and missing hidden
  // attributes branch.
  TempFile off64_nc("offset64_specials.nc");
  CreateMinimalFileWithFormat(off64_nc.c_str(), NC_CLOBBER | NC_64BIT_OFFSET);
  {
    const std::string off64_path = off64_nc.string();
    const std::string_view args[] = {"ncdump", "-s", "-h", off64_path};
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_THAT(res.stdout_output, HasSubstr(":_Format = \"64-bit offset\" ;"));
  }
}

TEST(NcdumpTest, Netcdf4AllTypesEnumsSpecialsFiltersAndXmlReject) {
  TempFile nc4_file("nc4_comprehensive.nc");

  struct CompoundVal {
    int32_t id;
    double mat[2][2];
  };

  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create(nc4_file.c_str(), NC_CLOBBER | NC_NETCDF4, nc.ptr()));

    int d2 = -1;
    int d4 = -1;
    int d8 = -1;
    int d_unlim = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "d2", 2, &d2));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "d4", 4, &d4));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "d8", 8, &d8));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "unlim", NC_UNLIMITED, &d_unlim));

    // All 8 enum integer base types with >= 2 positive members (portable across
    // signed/unsigned char architectures such as ARM).
    nc_type t_ei8 = 0, t_eu8 = 0, t_ei16 = 0, t_eu16 = 0;
    nc_type t_ei32 = 0, t_eu32 = 0, t_ei64 = 0, t_eu64 = 0;
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc.get(), NC_BYTE, "ei8_t", &t_ei8));
    {
      constexpr signed char v1 = 1, v2 = 2;
      ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), t_ei8, "I8_A", &v1));
      ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), t_ei8, "I8_B", &v2));
      const signed char attv[2] = {v1, v2};
      ASSERT_EQ(NC_NOERR,
                nc_put_att(nc.get(), NC_GLOBAL, "a_ei8", t_ei8, 2, attv));
    }
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc.get(), NC_UBYTE, "eu8_t", &t_eu8));
    {
      constexpr unsigned char v1 = 1, v2 = 2;
      ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), t_eu8, "U8_A", &v1));
      ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), t_eu8, "U8_B", &v2));
      const unsigned char attv[2] = {v1, v2};
      ASSERT_EQ(NC_NOERR,
                nc_put_att(nc.get(), NC_GLOBAL, "a_eu8", t_eu8, 2, attv));
    }
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc.get(), NC_SHORT, "ei16_t", &t_ei16));
    {
      constexpr short v1 = 1, v2 = 2;
      ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), t_ei16, "I16_A", &v1));
      ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), t_ei16, "I16_B", &v2));
      const short attv[2] = {v1, v2};
      ASSERT_EQ(NC_NOERR,
                nc_put_att(nc.get(), NC_GLOBAL, "a_ei16", t_ei16, 2, attv));
    }
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc.get(), NC_USHORT, "eu16_t", &t_eu16));
    {
      constexpr unsigned short v1 = 1, v2 = 2;
      ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), t_eu16, "U16_A", &v1));
      ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), t_eu16, "U16_B", &v2));
      const unsigned short attv[2] = {v1, v2};
      ASSERT_EQ(NC_NOERR,
                nc_put_att(nc.get(), NC_GLOBAL, "a_eu16", t_eu16, 2, attv));
    }
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc.get(), NC_INT, "ei32_t", &t_ei32));
    {
      constexpr int v1 = 1, v2 = 2;
      ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), t_ei32, "I32_A", &v1));
      ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), t_ei32, "I32_B", &v2));
      const int attv[2] = {v1, v2};
      ASSERT_EQ(NC_NOERR,
                nc_put_att(nc.get(), NC_GLOBAL, "a_ei32", t_ei32, 2, attv));
    }
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc.get(), NC_UINT, "eu32_t", &t_eu32));
    {
      constexpr unsigned int v1 = 1, v2 = 2;
      ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), t_eu32, "U32_A", &v1));
      ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), t_eu32, "U32_B", &v2));
      const unsigned int attv[2] = {v1, v2};
      ASSERT_EQ(NC_NOERR,
                nc_put_att(nc.get(), NC_GLOBAL, "a_eu32", t_eu32, 2, attv));
    }
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc.get(), NC_INT64, "ei64_t", &t_ei64));
    {
      constexpr long long v1 = 1, v2 = 2;
      ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), t_ei64, "I64_A", &v1));
      ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), t_ei64, "I64_B", &v2));
      const long long attv[2] = {v1, v2};
      ASSERT_EQ(NC_NOERR,
                nc_put_att(nc.get(), NC_GLOBAL, "a_ei64", t_ei64, 2, attv));
    }
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc.get(), NC_UINT64, "eu64_t", &t_eu64));
    {
      constexpr unsigned long long v1 = 1, v2 = 2;
      ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), t_eu64, "U64_A", &v1));
      ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), t_eu64, "U64_B", &v2));
      const unsigned long long attv[2] = {v1, v2};
      ASSERT_EQ(NC_NOERR,
                nc_put_att(nc.get(), NC_GLOBAL, "a_eu64", t_eu64, 2, attv));
    }

    // Opaque, VLEN, and Compound types (including a 2D array compound field).
    nc_type t_opq = 0;
    nc_type t_vlen = 0;
    nc_type t_cmp = 0;
    ASSERT_EQ(NC_NOERR, nc_def_opaque(nc.get(), 2, "opq2_t", &t_opq));
    constexpr unsigned char kOpqAtt[4] = {0xAB, 0xCD, 0x01, 0xEF};
    ASSERT_EQ(NC_NOERR,
              nc_put_att(nc.get(), NC_GLOBAL, "a_opq", t_opq, 2, kOpqAtt));

    ASSERT_EQ(NC_NOERR, nc_def_vlen(nc.get(), "vlen_i_t", NC_INT, &t_vlen));
    int vl1[2] = {10, 20};
    int vl2[1] = {30};
    const nc_vlen_t kVlenAtt[2] = {{2, vl1}, {1, vl2}};
    ASSERT_EQ(NC_NOERR,
              nc_put_att(nc.get(), NC_GLOBAL, "a_vlen", t_vlen, 2, kVlenAtt));

    ASSERT_EQ(NC_NOERR,
              nc_def_compound(nc.get(), sizeof(CompoundVal), "cmp_t", &t_cmp));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc.get(), t_cmp, "id",
                                 NC_COMPOUND_OFFSET(CompoundVal, id), NC_INT));
    constexpr int kMatDims[2] = {2, 2};
    ASSERT_EQ(NC_NOERR,
              nc_insert_array_compound(nc.get(), t_cmp, "mat",
                                       NC_COMPOUND_OFFSET(CompoundVal, mat),
                                       NC_DOUBLE, 2, kMatDims));
    CompoundVal cmp_att[2];
    std::memset(cmp_att, 0, sizeof(cmp_att));
    cmp_att[0].id = 11;
    cmp_att[0].mat[0][0] = 1.5;
    cmp_att[0].mat[1][1] = 2.5;
    cmp_att[1].id = 22;
    cmp_att[1].mat[0][1] = 3.5;
    cmp_att[1].mat[1][0] = 4.5;
    ASSERT_EQ(NC_NOERR,
              nc_put_att(nc.get(), NC_GLOBAL, "a_cmp", t_cmp, 2, cmp_att));

    // All atomic netCDF-4 global attributes, including multi-string & '\n'.
    constexpr unsigned char kUbAtt[2] = {1, 254};
    constexpr unsigned short kUsAtt[2] = {10, 65000};
    constexpr unsigned int kUiAtt[2] = {100U, 3000000000U};
    constexpr long long kLlAtt[2] = {-5000000000LL, 5000000000LL};
    constexpr unsigned long long kUllAtt[2] = {1ULL, 9000000000ULL};
    ASSERT_EQ(NC_NOERR, nc_put_att_uchar(nc.get(), NC_GLOBAL, "a_ub", NC_UBYTE,
                                         2, kUbAtt));
    ASSERT_EQ(NC_NOERR, nc_put_att_ushort(nc.get(), NC_GLOBAL, "a_us",
                                          NC_USHORT, 2, kUsAtt));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_uint(nc.get(), NC_GLOBAL, "a_ui", NC_UINT, 2, kUiAtt));
    ASSERT_EQ(NC_NOERR, nc_put_att_longlong(nc.get(), NC_GLOBAL, "a_ll",
                                            NC_INT64, 2, kLlAtt));
    ASSERT_EQ(NC_NOERR, nc_put_att_ulonglong(nc.get(), NC_GLOBAL, "a_ull",
                                             NC_UINT64, 2, kUllAtt));
    constexpr char kNc4Text[] = "line1\nline2\t\"quoted\"";
    ASSERT_EQ(NC_NOERR, nc_put_att_text(nc.get(), NC_GLOBAL, "a_nc4_text",
                                        sizeof(kNc4Text) - 1, kNc4Text));
    const char* kStringsAtt[2] = {"first", "second\nline"};
    ASSERT_EQ(NC_NOERR,
              nc_put_att_string(nc.get(), NC_GLOBAL, "a_strs", 2, kStringsAtt));

    // Variables without _FillValue across all 12 primitive types + enum UDT to
    // cover every switch case in get_fill_info().
    struct PrimVarSpec {
      const char* name;
      nc_type type;
    };
    const PrimVarSpec kPrimSpecs[] = {
        {"vu_b", NC_BYTE},   {"vu_c", NC_CHAR},     {"vu_s", NC_SHORT},
        {"vu_i", NC_INT},    {"vu_f", NC_FLOAT},    {"vu_d", NC_DOUBLE},
        {"vu_ub", NC_UBYTE}, {"vu_us", NC_USHORT},  {"vu_ui", NC_UINT},
        {"vu_ll", NC_INT64}, {"vu_ull", NC_UINT64}, {"vu_str", NC_STRING},
        {"vu_enum", t_ei32},
    };
    std::vector<int> prim_vars;
    for (const PrimVarSpec& spec : kPrimSpecs) {
      int vid = -1;
      ASSERT_EQ(NC_NOERR,
                nc_def_var(nc.get(), spec.name, spec.type, 1, &d2, &vid));
      prim_vars.push_back(vid);
    }

    // Special attributes: contiguous, compact,
    // chunked+fletcher32+shuffle+deflate +little-endian+nofill, big-endian,
    // szip-compressed, and 0-record unlim.
    int v_contig = -1;
    int v_compact = -1;
    int v_chunked_flt = -1;
    int v_big_end = -1;
    int v_szip = -1;
    int v_empty_unlim = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc.get(), "v_contig", NC_INT, 1, &d2, &v_contig));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_chunking(nc.get(), v_contig, NC_CONTIGUOUS, nullptr));
    constexpr int kExplicitFill = 999;
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), v_contig, NC_FillValue, NC_INT,
                                       1, &kExplicitFill));

    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc.get(), "v_compact", NC_INT, 1, &d2, &v_compact));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_chunking(nc.get(), v_compact, NC_COMPACT, nullptr));

    const int dims_2d[2] = {d4, d8};
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "v_chunked_flt", NC_INT, 2,
                                   dims_2d, &v_chunked_flt));
    constexpr size_t kChunk2d[2] = {2, 4};
    ASSERT_EQ(NC_NOERR, nc_def_var_chunking(nc.get(), v_chunked_flt, NC_CHUNKED,
                                            kChunk2d));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_fletcher32(nc.get(), v_chunked_flt, NC_FLETCHER32));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_deflate(nc.get(), v_chunked_flt, NC_SHUFFLE, 1, 5));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_endian(nc.get(), v_chunked_flt, NC_ENDIAN_LITTLE));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_fill(nc.get(), v_chunked_flt, NC_NOFILL, nullptr));

    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc.get(), "v_big_end", NC_INT, 1, &d2, &v_big_end));
    ASSERT_EQ(NC_NOERR, nc_def_var_endian(nc.get(), v_big_end, NC_ENDIAN_BIG));

    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc.get(), "v_szip", NC_INT, 2, dims_2d, &v_szip));
    constexpr size_t kSzipChunk[2] = {4, 8};
    ASSERT_EQ(NC_NOERR,
              nc_def_var_chunking(nc.get(), v_szip, NC_CHUNKED, kSzipChunk));
    ASSERT_EQ(NC_NOERR, nc_def_var_szip(nc.get(), v_szip, NC_SZIP_EC, 8));

    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "v_empty_unlim", NC_INT, 1,
                                   &d_unlim, &v_empty_unlim));

    ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));

    // Populate variables so reads succeed cleanly across all formats/filters.
    constexpr signed char vb[2] = {1, 2};
    constexpr char vc[2] = {'a', 'b'};
    constexpr short vs[2] = {10, NC_FILL_SHORT};
    constexpr int vi[2] = {100, NC_FILL_INT};
    constexpr float vf[2] = {1.25f, NC_FILL_FLOAT};
    constexpr double vd[2] = {2.5, NC_FILL_DOUBLE};
    constexpr unsigned char vub[2] = {3, 4};
    constexpr unsigned short vus[2] = {30, NC_FILL_USHORT};
    constexpr unsigned int vui[2] = {300, NC_FILL_UINT};
    constexpr long long vll[2] = {3000, NC_FILL_INT64};
    constexpr unsigned long long vull[2] = {30000, NC_FILL_UINT64};
    const char* vstr[2] = {"", "ok"};
    constexpr int venum[2] = {1, 2};

    ASSERT_EQ(NC_NOERR, nc_put_var_schar(nc.get(), prim_vars[0], vb));
    ASSERT_EQ(NC_NOERR, nc_put_var_text(nc.get(), prim_vars[1], vc));
    ASSERT_EQ(NC_NOERR, nc_put_var_short(nc.get(), prim_vars[2], vs));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc.get(), prim_vars[3], vi));
    ASSERT_EQ(NC_NOERR, nc_put_var_float(nc.get(), prim_vars[4], vf));
    ASSERT_EQ(NC_NOERR, nc_put_var_double(nc.get(), prim_vars[5], vd));
    ASSERT_EQ(NC_NOERR, nc_put_var_uchar(nc.get(), prim_vars[6], vub));
    ASSERT_EQ(NC_NOERR, nc_put_var_ushort(nc.get(), prim_vars[7], vus));
    ASSERT_EQ(NC_NOERR, nc_put_var_uint(nc.get(), prim_vars[8], vui));
    ASSERT_EQ(NC_NOERR, nc_put_var_longlong(nc.get(), prim_vars[9], vll));
    ASSERT_EQ(NC_NOERR, nc_put_var_ulonglong(nc.get(), prim_vars[10], vull));
    ASSERT_EQ(NC_NOERR, nc_put_var_string(nc.get(), prim_vars[11], vstr));
    ASSERT_EQ(NC_NOERR, nc_put_var(nc.get(), prim_vars[12], venum));

    constexpr int v2[2] = {7, 8};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc.get(), v_contig, v2));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc.get(), v_compact, v2));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc.get(), v_big_end, v2));
    std::vector<int> grid(32);
    for (int idx = 0; idx < 32; ++idx) {
      grid[idx] = idx;
    }
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc.get(), v_chunked_flt, grid.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc.get(), v_szip, grid.data()));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  const std::string path = nc4_file.string();

  // Dump with -s and -XF.
  {
    const std::string_view args[] = {"ncdump", "-s", "-XF", path};
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_THAT(res.stdout_output,
                HasSubstr("byte enum ei8_t {I8_A = 1, I8_B = 2} ;"));
    EXPECT_THAT(res.stdout_output, HasSubstr("opaque(2) opq2_t ;"));
    EXPECT_THAT(res.stdout_output, HasSubstr("int(*) vlen_i_t ;"));
    EXPECT_THAT(res.stdout_output, HasSubstr("double mat(2, 2) ;"));
    EXPECT_THAT(res.stdout_output, HasSubstr(":a_opq = 0XABCD, 0X01EF ;"));
    EXPECT_THAT(
        res.stdout_output,
        HasSubstr(":a_nc4_text = \"line1\\nline2\\t\\\"quoted\\\"\" ;"));
    EXPECT_THAT(res.stdout_output,
                HasSubstr("int v_contig:_FillValue = 999 ;"));
    EXPECT_THAT(res.stdout_output,
                HasSubstr("v_contig:_Storage = \"contiguous\" ;"));
    EXPECT_THAT(res.stdout_output,
                HasSubstr("v_compact:_Storage = \"compact\" ;"));
    EXPECT_THAT(res.stdout_output,
                HasSubstr("v_chunked_flt:_Storage = \"chunked\" ;"));
    EXPECT_THAT(res.stdout_output,
                HasSubstr("v_chunked_flt:_ChunkSizes = 2, 4 ;"));
    EXPECT_THAT(res.stdout_output,
                HasSubstr("v_chunked_flt:_Fletcher32 = \"true\" ;"));
    EXPECT_THAT(res.stdout_output,
                HasSubstr("v_chunked_flt:_Shuffle = \"true\" ;"));
    EXPECT_THAT(res.stdout_output,
                HasSubstr("v_chunked_flt:_DeflateLevel = 5 ;"));
    EXPECT_THAT(res.stdout_output,
                HasSubstr("v_chunked_flt:_Endianness = \"little\" ;"));
    EXPECT_THAT(res.stdout_output,
                HasSubstr("v_chunked_flt:_NoFill = \"true\" ;"));
    EXPECT_THAT(res.stdout_output,
                HasSubstr("v_big_end:_Endianness = \"big\" ;"));
    EXPECT_THAT(res.stdout_output, HasSubstr("v_szip:_Filter = \"4,"));
    EXPECT_THAT(res.stdout_output, HasSubstr(":_SuperblockVersion = "));
    EXPECT_THAT(res.stdout_output, HasSubstr(":_IsNetcdf4 = 1 ;"));
    EXPECT_THAT(res.stdout_output, HasSubstr(":_Format = \"netCDF-4\" ;"));
    EXPECT_THAT(res.stdout_output, Not(HasSubstr("v_empty_unlim =")));
  }

  // Dump with -s -F (-h header only) to exercise _Filter formatting with
  // zero-parameter fletcher32 filter and '|' filter separator.
  {
    const std::string_view args[] = {"ncdump", "-s", "-F", "-h", path};
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_THAT(res.stdout_output,
                HasSubstr("v_chunked_flt:_Filter = \"3|2,4|1,5\" ;"));
    EXPECT_THAT(res.stdout_output, Not(HasSubstr("_Fletcher32")));
    EXPECT_THAT(res.stdout_output, Not(HasSubstr("_Shuffle")));
    EXPECT_THAT(res.stdout_output, Not(HasSubstr("_DeflateLevel")));
  }

  // Reject -x (NcML) on non-classic netCDF-4 files.
  {
    const std::string_view args[] = {"ncdump", "-x", path};
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stderr_output,
                HasSubstr("currently only permitted for netCDF classic model"));
  }
}

TEST(NcdumpTest, Netcdf4GroupsCrossGroupDimsAndSubsetting) {
  TempFile grp_nc("nc4_groups.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create(grp_nc.c_str(), NC_CLOBBER | NC_NETCDF4, nc.ptr()));

    int d_root_shared = -1;
    int d_coord = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "shared_d", 2, &d_root_shared));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "coord_d", 2, &d_coord));

    int v_root = -1;
    int v_coord = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "v_root", NC_INT, 1,
                                   &d_root_shared, &v_root));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc.get(), "coord_d", NC_INT, 1, &d_coord, &v_coord));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_text(nc.get(), NC_GLOBAL, "root_g", 4, "root"));

    // Empty group (nids == 0 in searchgroupdim).
    int grp_empty = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc.get(), "grp_empty", &grp_empty));

    // Define /grp_b/sub_c and its cousin dimension before grp_a so that
    // netCDF-4 nc_enddef creates d_cousin's HDF5 dimscale before attaching it
    // to grp_a's variable (netCDF-4 requires referenced dimscales to exist
    // earlier in group traversal order).
    int grp_b = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc.get(), "grp_b", &grp_b));
    int d_b = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(grp_b, "dim_b", 2, &d_b));
    int sub_c = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(grp_b, "sub_c", &sub_c));
    int d_cousin = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(sub_c, "dim_cousin", 2, &d_cousin));
    int v_sub = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(sub_c, "v_sub", NC_INT, 1, &d_cousin, &v_sub));

    // Group A shadows shared_d and defines variables using root's shadowed dim
    // as well as the cousin dimension from /grp_b/sub_c.
    int grp_a = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc.get(), "grp_a", &grp_a));
    int d_local_shared = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(grp_a, "shared_d", 3, &d_local_shared));

    int v_local = -1;
    int v_shadowed = -1;
    int v_cousin = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(grp_a, "v_local", NC_INT, 1, &d_local_shared,
                                   &v_local));
    ASSERT_EQ(NC_NOERR, nc_def_var(grp_a, "v_shadowed", NC_INT, 1,
                                   &d_root_shared, &v_shadowed));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(grp_a, "v_cousin", NC_INT, 1, &d_cousin, &v_cousin));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_text(grp_a, NC_GLOBAL, "ga_note", 4, "in_a"));

    ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));

    constexpr int kTwo[2] = {1, 2};
    constexpr int kThree[3] = {10, 20, 30};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc.get(), v_root, kTwo));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc.get(), v_coord, kTwo));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(sub_c, v_sub, kTwo));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(grp_a, v_local, kThree));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(grp_a, v_shadowed, kTwo));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(grp_a, v_cousin, kTwo));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  const std::string path = grp_nc.string();

  // Full dump exercises BFS dimension search & FQN prefixes.
  {
    const std::string_view args[] = {"ncdump", path};
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_THAT(res.stdout_output, HasSubstr("v_local(shared_d) ;"));
    EXPECT_THAT(res.stdout_output, HasSubstr("v_shadowed(/shared_d) ;"));
    EXPECT_THAT(res.stdout_output,
                HasSubstr("v_cousin(/grp_b/sub_c/dim_cousin) ;"));
    EXPECT_THAT(res.stdout_output, HasSubstr("// group attributes:"));
  }

  // -g grp_a selects only grp_a data while keeping other groups' headers.
  {
    const std::string_view args[] = {"ncdump", "-g", "grp_a", path};
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_THAT(res.stdout_output, HasSubstr("v_local = 10, 20, 30 ;"));
    EXPECT_THAT(res.stdout_output, Not(HasSubstr("v_root =")));
    EXPECT_THAT(res.stdout_output, Not(HasSubstr("v_sub =")));
  }

  // Non-existent group in -g exits with error 1.
  {
    const std::string_view args[] = {"ncdump", "-g", "missing_grp", path};
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
  }

  // -c prints only coordinate variable data.
  {
    const std::string_view args[] = {"ncdump", "-c", path};
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_THAT(res.stdout_output, HasSubstr("coord_d = 1, 2 ;"));
    EXPECT_THAT(res.stdout_output, Not(HasSubstr("v_root =")));
  }

  // -v with qualified variable paths.
  {
    const std::string_view args[] = {"ncdump", "-v",
                                     "grp_a/v_local,/grp_b/sub_c/v_sub", path};
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_THAT(res.stdout_output, HasSubstr("v_local = 10, 20, 30 ;"));
    EXPECT_THAT(res.stdout_output, HasSubstr("v_sub = 1, 2 ;"));
    EXPECT_THAT(res.stdout_output, Not(HasSubstr("v_root =")));
  }
}

TEST(NcdumpTest, NcmlOutputCoversClassicCdf5AndNc4Classic) {
  // Part A: Classic file for XML entity escaping, embedded NULs, _NCProperties
  // toggling, shape attribute, empty record var, -h/-c/-v.
  TempFile classic_nc("ncml_classic.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(classic_nc.c_str(), NC_CLOBBER, nc.ptr()));
    int dx = -1;
    int dy = -1;
    int drec = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 2, &dx));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "y", 3, &dy));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "rec", NC_UNLIMITED, &drec));

    int vx = -1;
    int vscalar = -1;
    int v2d = -1;
    int vrec = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "x", NC_INT, 1, &dx, &vx));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "v_scalar", NC_DOUBLE, 0, nullptr,
                                   &vscalar));
    const int dxy[2] = {dx, dy};
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "v_2d", NC_INT, 2, dxy, &v2d));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "v_rec", NC_INT, 1, &drec, &vrec));

    constexpr char kProps[] = "version=2,ncml=1";
    ASSERT_EQ(NC_NOERR, nc_put_att_text(nc.get(), NC_GLOBAL, "_NCProperties",
                                        sizeof(kProps) - 1, kProps));
    constexpr char kXmlBytes[] = {'"',    '<', '>',  '&',  '\n', '\r', '\t',
                                  '\x01', 'a', '\0', '\0', 'b',  '\0', '\0'};
    ASSERT_EQ(NC_NOERR, nc_put_att_text(nc.get(), NC_GLOBAL, "xml_esc",
                                        sizeof(kXmlBytes), kXmlBytes));
    constexpr int kVarAtt[2] = {5, 6};
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), vscalar, "scalar_att", NC_INT,
                                       2, kVarAtt));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));

    constexpr int kX[2] = {1, 2};
    constexpr double kSc = 3.5;
    constexpr int kGrid[6] = {1, 2, 3, 4, 5, 6};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc.get(), vx, kX));
    ASSERT_EQ(NC_NOERR, nc_put_var_double(nc.get(), vscalar, &kSc));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc.get(), v2d, kGrid));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  const std::string cpath = classic_nc.string();

  // Default -x omits _NCProperties, escapes XML chars, warns once on NUL.
  {
    const std::string_view args[] = {"ncdump", "-x", cpath};
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_THAT(res.stderr_output, HasSubstr("value contains nul characters"));
    EXPECT_THAT(
        res.stdout_output,
        HasSubstr(
            "value=\"&quot;&lt;&gt;&amp;&#xA;&#xD;&#x9;&#1;a&#0;&#0;b\""));
    EXPECT_THAT(
        res.stdout_output,
        HasSubstr(
            "<dimension name=\"rec\" length=\"0\" isUnlimited=\"true\" />"));
    EXPECT_THAT(
        res.stdout_output,
        HasSubstr("<variable name=\"v_2d\" shape=\"x y\" type=\"int\">"));
    EXPECT_THAT(
        res.stdout_output,
        HasSubstr("<variable name=\"v_rec\" shape=\"rec\" type=\"int\" />"));
    EXPECT_THAT(res.stdout_output, Not(HasSubstr("_NCProperties")));
  }

  // -x -s prints _NCProperties; -x -s -Xp suppresses _NCProperties.
  {
    const std::string_view args[] = {"ncdump", "-x", "-s", cpath};
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_THAT(res.stdout_output, HasSubstr("name=\"_NCProperties\""));
  }
  {
    const std::string_view args[] = {"ncdump", "-x", "-s", "-Xp", cpath};
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_THAT(res.stdout_output, Not(HasSubstr("_NCProperties")));
  }

  // -x with -h, -c, and -v covers self-closing variable element branches.
  for (const auto& [opt, val] : {
           std::pair<const char*, const char*>{"-h", ""},
           std::pair<const char*, const char*>{"-c", ""},
           std::pair<const char*, const char*>{"-v", "v_scalar"},
       }) {
    std::vector<std::string_view> args = {"ncdump", "-x", opt};
    if (val[0] != '\0') {
      args.push_back(val);
    }
    args.push_back(cpath);
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_THAT(
        res.stdout_output,
        HasSubstr("<variable name=\"v_2d\" shape=\"x y\" type=\"int\" />"));
  }

  // Part B: CDF-5 file covers all 11 classic/CDF-5 primitive type names and
  // attribute value formatters in NcML, plus a 0-length numeric attribute.
  TempFile cdf5_nc("ncml_cdf5.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create(cdf5_nc.c_str(), NC_CLOBBER | NC_CDF5, nc.ptr()));
    int dx = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 1, &dx));

    const struct {
      const char* name;
      nc_type type;
    } kCdf5Types[] = {
        {"vb", NC_BYTE},   {"vc", NC_CHAR},     {"vs", NC_SHORT},
        {"vi", NC_INT},    {"vf", NC_FLOAT},    {"vd", NC_DOUBLE},
        {"vub", NC_UBYTE}, {"vus", NC_USHORT},  {"vui", NC_UINT},
        {"vll", NC_INT64}, {"vull", NC_UINT64},
    };
    for (const auto& spec : kCdf5Types) {
      int vid = -1;
      ASSERT_EQ(NC_NOERR,
                nc_def_var(nc.get(), spec.name, spec.type, 1, &dx, &vid));
    }

    constexpr signed char kb[2] = {-1, 2};
    constexpr short ks[2] = {-10, 20};
    constexpr int ki[2] = {-100, 200};
    constexpr float kf[2] = {1.5f, 2.5f};
    constexpr double kd[2] = {3.25, 4.5};
    constexpr unsigned char kub[2] = {1, 250};
    constexpr unsigned short kus[2] = {10, 50000};
    constexpr unsigned int kui[2] = {100U, 4000000000U};
    constexpr long long kll[2] = {-5000000000LL, 5000000000LL};
    constexpr unsigned long long kull[2] = {1ULL, 9000000000ULL};
    ASSERT_EQ(NC_NOERR,
              nc_put_att_schar(nc.get(), NC_GLOBAL, "ab", NC_BYTE, 2, kb));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_short(nc.get(), NC_GLOBAL, "as", NC_SHORT, 2, ks));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc.get(), NC_GLOBAL, "ai", NC_INT, 2, ki));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_float(nc.get(), NC_GLOBAL, "af", NC_FLOAT, 2, kf));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_double(nc.get(), NC_GLOBAL, "ad", NC_DOUBLE, 2, kd));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_uchar(nc.get(), NC_GLOBAL, "aub", NC_UBYTE, 2, kub));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_ushort(nc.get(), NC_GLOBAL, "aus", NC_USHORT, 2, kus));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_uint(nc.get(), NC_GLOBAL, "aui", NC_UINT, 2, kui));
    ASSERT_EQ(NC_NOERR, nc_put_att_longlong(nc.get(), NC_GLOBAL, "all",
                                            NC_INT64, 2, kll));
    ASSERT_EQ(NC_NOERR, nc_put_att_ulonglong(nc.get(), NC_GLOBAL, "aull",
                                             NC_UINT64, 2, kull));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), NC_GLOBAL, "a_empty_int",
                                       NC_INT, 0, nullptr));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  {
    const std::string path = cdf5_nc.string();
    const std::string_view args[] = {"ncdump", "-x", path};
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_THAT(res.stdout_output, HasSubstr("type=\"ubyte\""));
    EXPECT_THAT(res.stdout_output, HasSubstr("type=\"ushort\""));
    EXPECT_THAT(res.stdout_output, HasSubstr("type=\"uint\""));
    EXPECT_THAT(res.stdout_output, HasSubstr("type=\"int64\""));
    EXPECT_THAT(res.stdout_output, HasSubstr("type=\"uint64\""));
    EXPECT_THAT(res.stdout_output,
                HasSubstr("name=\"a_empty_int\" type=\"int\" value=\"\""));
  }

  // Part C: netCDF-4 classic model succeeds with -x.
  TempFile nc4c_nc("ncml_nc4_classic.nc");
  CreateMinimalFileWithFormat(nc4c_nc.c_str(),
                              NC_CLOBBER | NC_NETCDF4 | NC_CLASSIC_MODEL);
  {
    const std::string path = nc4c_nc.string();
    const std::string_view args[] = {"ncdump", "-x", path};
    const CliResult res = RunCliInForkedChild(ncdump_cli_main, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_THAT(res.stdout_output,
                HasSubstr("<variable name=\"v\" shape=\"x\" type=\"int\">"));
  }
}

}  // namespace
}  // namespace netcdf
