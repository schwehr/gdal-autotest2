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

// Clean-room unit tests for //third_party/netcdf/ncdump/nccopy.c.
//
// Exercises CLI flag validation, format/kind aliases, classic and CDF-5 record
// optimizations, unlimited-to-fixed conversion, diskless I/O, copy buffer
// resizing and multi-iteration reads, experimental chunk cache estimation,
// per-variable and per-dimension chunking specs, minimum chunk threshold,
// groups/UDTs/strings/vlens, -v/-V/-g/-G selectors, compression/checksum/endian
// propagation, and HDF5 -F filter pipelines inside isolated forked children.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/strings/str_replace.h"
#include "third_party/netcdf/google/util/cli_mains.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"

// Upstream memio.c allocates initial buffers via malloc() and record-variable
// alignment padding leaves uninitialized bytes written by NC_writefile(),
// tripping MSan under `-w` (NC_DISKLESS | NC_PERSIST) on short record files.
#if defined(MEMORY_SANITIZER)
#define NETCDF_NCCOPY_TEST_MSAN 1
#elif defined(__has_feature)
#if __has_feature(memory_sanitizer)
#define NETCDF_NCCOPY_TEST_MSAN 1
#endif
#endif

namespace netcdf {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

CliResult RunNccopy(std::initializer_list<std::string_view> args) {
  std::vector<std::string_view> argv;
  argv.reserve(args.size() + 1);
  argv.push_back("nccopy");
  argv.insert(argv.end(), args.begin(), args.end());
  return RunCliInForkedChild(nccopy_cli_main, argv);
}

void CreateMinimalClassicFile(const char* path) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR, nc_create(path, NC_CLOBBER, nc.ptr()));
  int x_dim = -1;
  int t_dim = -1;
  int fixed_var = -1;
  int rec_var = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 2, &x_dim));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "t", NC_UNLIMITED, &t_dim));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "a", NC_INT, 1, &x_dim, &fixed_var));
  const int rec_dims[2] = {t_dim, x_dim};
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "r", NC_FLOAT, 2, rec_dims, &rec_var));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));

  const int fixed_vals[2] = {11, 22};
  ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, fixed_var, fixed_vals));
  const float rec_vals[4] = {1.5F, 2.5F, 3.5F, 4.5F};
  const size_t start[2] = {0, 0};
  const size_t count[2] = {2, 2};
  ASSERT_EQ(NC_NOERR, nc_put_vara_float(nc, rec_var, start, count, rec_vals));
  ASSERT_EQ(NC_NOERR, nc.close());
}

TEST(NccopyTest, UsageAndCliValidationErrors) {
  // argc <= 1 triggers usage().
  {
    const CliResult res = RunNccopy({});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_NE(0, res.exit_code);
    EXPECT_THAT(res.stderr_output, HasSubstr("netCDF library version"));
  }

  // Unsupported option hits default: usage() (in NetCDF 4.10.1, getopt also
  // omits '5' from the option string even though usage() advertises [-5]).
  for (std::string_view bad_opt : {"-Z", "-5"}) {
    SCOPED_TRACE(bad_opt);
    const CliResult res = RunNccopy({bad_opt, "in.nc", "out.nc"});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_NE(0, res.exit_code);
    EXPECT_THAT(res.stderr_output, HasSubstr("netCDF library version"));
  }

  // Zero, one, or three positional arguments fail the argc != 2 check.
  for (std::initializer_list<std::string_view> bad_argc :
       {std::initializer_list<std::string_view>{"-u"},
        std::initializer_list<std::string_view>{"only_in.nc"},
        std::initializer_list<std::string_view>{"a.nc", "b.nc", "c.nc"}}) {
    const CliResult res = RunNccopy(bad_argc);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_NE(0, res.exit_code);
    EXPECT_THAT(res.stderr_output,
                HasSubstr("one input file and one output file required"));
  }

  // Invalid -k format traverses the entire legalkinds table and errors.
  {
    const CliResult res =
        RunNccopy({"-k", "unknown_format", "in.nc", "out.nc"});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_NE(0, res.exit_code);
    EXPECT_THAT(res.stderr_output, HasSubstr("invalid output format"));
  }

  // Out-of-range -d compression levels fail.
  for (std::string_view bad_level : {"-1", "10"}) {
    const CliResult res = RunNccopy({"-d", bad_level, "in.nc", "out.nc"});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_NE(0, res.exit_code);
    EXPECT_THAT(res.stderr_output, HasSubstr("invalid deflation level"));
  }

  // Negative values or invalid suffix letters for -m, -h, and -e fail.
  struct BadSizedArg {
    std::string_view flag;
    std::string_view value;
    std::string_view expected_substr;
  };
  constexpr BadSizedArg kBadSizedArgs[] = {
      {"-m", "-1", "Suffix used for '-m'"},
      {"-m", "10x", "Suffix used for '-m'"},
      {"-h", "-2", "Suffix used for '-h'"},
      {"-h", "10z", "Suffix used for '-h'"},
      {"-e", "-3", "Suffix used for '-e'"},
      {"-e", "10q", "Suffix used for '-e'"},
  };
  for (const BadSizedArg& tc : kBadSizedArgs) {
    const CliResult res = RunNccopy({tc.flag, tc.value, "in.nc", "out.nc"});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_NE(0, res.exit_code);
    EXPECT_THAT(res.stderr_output, HasSubstr(tc.expected_substr));
  }

  // Valid g/G, t/T, p/P suffixes parse cleanly before checking argc != 2.
  {
    const CliResult res1 = RunNccopy({"-m", "1g", "-h", "2t", "-e", "3p"});
    ASSERT_TRUE(res1.ok()) << res1.harness_error;
    EXPECT_NE(0, res1.exit_code);
    EXPECT_THAT(res1.stderr_output,
                HasSubstr("one input file and one output file required"));

    const CliResult res2 = RunNccopy({"-m", "1G", "-h", "2T", "-e", "3P"});
    ASSERT_TRUE(res2.ok()) << res2.harness_error;
    EXPECT_NE(0, res2.exit_code);
    EXPECT_THAT(res2.stderr_output,
                HasSubstr("one input file and one output file required"));
  }

  // -L exits cleanly when logging support is compiled out.
  {
    const CliResult res = RunNccopy({"-L", "0", "in.nc", "out.nc"});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_NE(0, res.exit_code);
    EXPECT_THAT(res.stderr_output,
                HasSubstr("-L specified, but logging support not enabled"));
  }

  // Identical input and output paths are rejected before overwriting input.
  {
    TempFile same_nc("same.nc");
    CreateMinimalClassicFile(same_nc.c_str());
    const std::string same_path = same_nc.string();
    const CliResult res = RunNccopy({same_path, same_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_NE(0, res.exit_code);
    EXPECT_THAT(res.stderr_output, HasSubstr("output would overwrite input"));
  }

  // Nonexistent input file exits non-zero.
  {
    TempFile missing_nc("does_not_exist.nc");
    TempFile out_nc("never_created.nc");
    const std::string missing_path = missing_nc.string();
    const std::string out_path = out_nc.string();
    const CliResult res = RunNccopy({missing_path, out_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_NE(0, res.exit_code);
  }
}

TEST(NccopyTest, CopiesClassicDatasetAcrossAllFormatsAndKindAliases) {
  TempFile in_nc("fmt_in.nc");
  CreateMinimalClassicFile(in_nc.c_str());
  const std::string in_path = in_nc.string();

  struct FormatCase {
    std::string_view tag;
    std::string_view flag;
    std::string_view arg;  // Empty means flag has no separate argument.
    int expected_format;
  };
  constexpr FormatCase kCases[] = {
      {"same", "", "", NC_FORMAT_CLASSIC},
      {"flag3", "-3", "", NC_FORMAT_CLASSIC},
      {"flag6", "-6", "", NC_FORMAT_64BIT_OFFSET},
      {"flag4", "-4", "", NC_FORMAT_NETCDF4},
      {"flag7", "-7", "", NC_FORMAT_NETCDF4_CLASSIC},
      {"k_classic", "-k", "classic", NC_FORMAT_CLASSIC},
      {"k_nc3", "-k", "nc3", NC_FORMAT_CLASSIC},
      {"k_1", "-k", "1", NC_FORMAT_CLASSIC},
      {"k_64off", "-k", "64-bit offset", NC_FORMAT_64BIT_OFFSET},
      {"k_nc6", "-k", "nc6", NC_FORMAT_64BIT_OFFSET},
      {"k_2", "-k", "2", NC_FORMAT_64BIT_OFFSET},
      {"k_64dash", "-k", "64-bit-offset", NC_FORMAT_64BIT_OFFSET},
      {"k_nc4", "-k", "netCDF-4", NC_FORMAT_NETCDF4},
      {"k_short4", "-k", "nc4", NC_FORMAT_NETCDF4},
      {"k_3", "-k", "3", NC_FORMAT_NETCDF4},
      {"k_hdf5", "-k", "hdf5", NC_FORMAT_NETCDF4},
      {"k_nc4c", "-k", "netCDF-4 classic model", NC_FORMAT_NETCDF4_CLASSIC},
      {"k_nc7", "-k", "nc7", NC_FORMAT_NETCDF4_CLASSIC},
      {"k_4", "-k", "4", NC_FORMAT_NETCDF4_CLASSIC},
      {"k_enhnc3", "-k", "enhanced-nc3", NC_FORMAT_NETCDF4_CLASSIC},
      {"k_5", "-k", "5", NC_FORMAT_CDF5},
      {"k_cdf5", "-k", "cdf5", NC_FORMAT_CDF5},
      {"k_nc5", "-k", "nc5", NC_FORMAT_CDF5},
      {"k_64data", "-k", "64-bit data", NC_FORMAT_CDF5},
      {"k_64data_dash", "-k", "64-bit-data", NC_FORMAT_CDF5},
  };

  for (const FormatCase& tc : kCases) {
    SCOPED_TRACE(tc.tag);
    TempFile out_nc(std::string("fmt_out_") + std::string(tc.tag) + ".nc");
    const std::string out_path = out_nc.string();

    CliResult res;
    if (tc.flag.empty()) {
      res = RunNccopy({in_path, out_path});
    } else if (tc.arg.empty()) {
      res = RunNccopy({tc.flag, in_path, out_path});
    } else {
      res = RunNccopy({tc.flag, tc.arg, in_path, out_path});
    }
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, out.ptr()));
    int actual_format = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_format(out, &actual_format));
    EXPECT_EQ(tc.expected_format, actual_format);

    int a_id = -1;
    int r_id = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "a", &a_id));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "r", &r_id));
    int a_vals[2] = {};
    float r_vals[4] = {};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(out, a_id, a_vals));
    ASSERT_EQ(NC_NOERR, nc_get_var_float(out, r_id, r_vals));
    EXPECT_THAT(a_vals, ElementsAre(11, 22));
    EXPECT_THAT(r_vals, ElementsAre(1.5F, 2.5F, 3.5F, 4.5F));
  }
}

TEST(NccopyTest, ClassicAndCdf5RecordSpecialCasesAndShellUnescape) {
  // 1. Classic input with scalar, fixed, multi-type records, and _NCProperties.
  TempFile classic_in("classic_rec_in.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(classic_in.c_str(), NC_CLOBBER, nc.ptr()));
    int rec_dim = -1;
    int cols_dim = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "rec", NC_UNLIMITED, &rec_dim));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "cols", 3, &cols_dim));

    int scalar_v = -1;
    int fixed_v = -1;
    int rec_s = -1;
    int rec_d = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "scalar_v", NC_DOUBLE, 0, nullptr, &scalar_v));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "fixed_v", NC_INT, 1, &cols_dim, &fixed_v));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "rec_s", NC_SHORT, 1, &rec_dim, &rec_s));
    const int rec_d_dims[2] = {rec_dim, cols_dim};
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "rec_d", NC_DOUBLE, 2, rec_d_dims, &rec_d));

    const int fill_val = -999;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc, fixed_v, NC_FillValue, NC_INT, 1, &fill_val));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_text(nc, NC_GLOBAL, "title", 12, "classic test"));
    // Place _NCProperties after "title" to test copy_atts() filtering it out.
    ASSERT_EQ(NC_NOERR,
              nc_put_att_text(nc, NC_GLOBAL, "_NCProperties", 9, "ignored=1"));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    const double scalar_val = 42.5;
    const int fixed_vals[3] = {10, 20, 30};
    const short rec_s_vals[2] = {7, -8};
    const double rec_d_vals[6] = {1.25, 2.5, 3.75, 4.25, 5.5, 6.75};
    ASSERT_EQ(NC_NOERR, nc_put_var_double(nc, scalar_v, &scalar_val));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, fixed_v, fixed_vals));
    const size_t s_start = 0;
    const size_t s_count = 2;
    ASSERT_EQ(NC_NOERR,
              nc_put_vara_short(nc, rec_s, &s_start, &s_count, rec_s_vals));
    const size_t d_start[2] = {0, 0};
    const size_t d_count[2] = {2, 3};
    ASSERT_EQ(NC_NOERR,
              nc_put_vara_double(nc, rec_d, d_start, d_count, rec_d_vals));
  }

  const std::string classic_in_path = classic_in.string();

  // (a) Diskless read/write (-r -w), verifies _NCProperties is skipped.
  {
    TempFile out_nc("classic_diskless.nc");
    const std::string out_path = out_nc.string();
#if defined(NETCDF_NCCOPY_TEST_MSAN)
    const CliResult res = RunNccopy({"-r", classic_in_path, out_path});
#else
    const CliResult res = RunNccopy({"-r", "-w", classic_in_path, out_path});
#endif
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, out.ptr()));
    int attid = -1;
    EXPECT_EQ(NC_ENOTATT,
              nc_inq_attid(out, NC_GLOBAL, "_NCProperties", &attid));
    char title[16] = {};
    ASSERT_EQ(NC_NOERR, nc_get_att_text(out, NC_GLOBAL, "title", title));
    EXPECT_EQ(std::string_view(title, 12), "classic test");

    int scalar_v = -1;
    int fixed_v = -1;
    int rec_s = -1;
    int rec_d = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "scalar_v", &scalar_v));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "fixed_v", &fixed_v));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "rec_s", &rec_s));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "rec_d", &rec_d));

    int fill_out = 0;
    ASSERT_EQ(NC_NOERR, nc_get_att_int(out, fixed_v, NC_FillValue, &fill_out));
    EXPECT_EQ(-999, fill_out);

    double scalar_out = 0.0;
    int fixed_out[3] = {};
    short rec_s_out[2] = {};
    double rec_d_out[6] = {};
    ASSERT_EQ(NC_NOERR, nc_get_var_double(out, scalar_v, &scalar_out));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(out, fixed_v, fixed_out));
    ASSERT_EQ(NC_NOERR, nc_get_var_short(out, rec_s, rec_s_out));
    ASSERT_EQ(NC_NOERR, nc_get_var_double(out, rec_d, rec_d_out));
    EXPECT_DOUBLE_EQ(42.5, scalar_out);
    EXPECT_THAT(fixed_out, ElementsAre(10, 20, 30));
    EXPECT_THAT(rec_s_out, ElementsAre(7, -8));
    EXPECT_THAT(rec_d_out, ElementsAre(1.25, 2.5, 3.75, 4.25, 5.5, 6.75));
  }

  // (b) -v subset on classic record file exercises classify_vars(option_nlvars
  // > 0).
  {
    TempFile out_nc("classic_v_subset.nc");
    const std::string out_path = out_nc.string();
    const CliResult res =
        RunNccopy({"-v", "scalar_v,rec_d", classic_in_path, out_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, out.ptr()));
    int scalar_v = -1;
    int fixed_v = -1;
    int rec_d = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "scalar_v", &scalar_v));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "fixed_v", &fixed_v));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "rec_d", &rec_d));
    int fill_out = 0;
    ASSERT_EQ(NC_NOERR, nc_get_att_int(out, fixed_v, NC_FillValue, &fill_out));
    EXPECT_EQ(-999, fill_out);
    double scalar_out = 0.0;
    double rec_d_out[6] = {};
    ASSERT_EQ(NC_NOERR, nc_get_var_double(out, scalar_v, &scalar_out));
    ASSERT_EQ(NC_NOERR, nc_get_var_double(out, rec_d, rec_d_out));
    EXPECT_DOUBLE_EQ(42.5, scalar_out);
    EXPECT_THAT(rec_d_out, ElementsAre(1.25, 2.5, 3.75, 4.25, 5.5, 6.75));
  }

  // (c) -u converts unlimited dimension to fixed size.
  {
    TempFile out_nc("classic_fixed_unlim.nc");
    const std::string out_path = out_nc.string();
    const CliResult res = RunNccopy({"-u", classic_in_path, out_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, out.ptr()));
    int unlim_id = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_unlimdim(out, &unlim_id));
    EXPECT_EQ(-1, unlim_id);
    int rec_dim = -1;
    size_t rec_len = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_dimid(out, "rec", &rec_dim));
    ASSERT_EQ(NC_NOERR, nc_inq_dimlen(out, rec_dim, &rec_len));
    EXPECT_EQ(2U, rec_len);
  }

  // 2. Classic file with an unused unlimited dimension and no record vars.
  {
    TempFile no_rec_in("classic_no_rec_in.nc");
    TempFile no_rec_out("classic_no_rec_out.nc");
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR, nc_create(no_rec_in.c_str(), NC_CLOBBER, nc.ptr()));
      int u_dim = -1;
      int x_dim = -1;
      int v_id = -1;
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "u", NC_UNLIMITED, &u_dim));
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 3, &x_dim));
      ASSERT_EQ(NC_NOERR, nc_def_var(nc, "f", NC_INT, 1, &x_dim, &v_id));
      ASSERT_EQ(NC_NOERR, nc_enddef(nc));
      const int vals[3] = {5, 6, 7};
      ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, v_id, vals));
    }
    const std::string in_path = no_rec_in.string();
    const std::string out_path = no_rec_out.string();
    const CliResult res = RunNccopy({in_path, out_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(no_rec_out.c_str(), NC_NOWRITE, out.ptr()));
    int f_id = -1;
    int vals[3] = {};
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "f", &f_id));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(out, f_id, vals));
    EXPECT_THAT(vals, ElementsAre(5, 6, 7));
  }

  // 3. NetCDF-4 input copied to -3 classic exercises nc3_special_case(ogrp,
  // outkind).
  {
    TempFile nc4_in("nc4_to_nc3_in.nc");
    TempFile nc3_out("nc4_to_nc3_out.nc");
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR, nc_create(nc4_in.c_str(), NC_NETCDF4, nc.ptr()));
      int t_dim = -1;
      int x_dim = -1;
      int s_id = -1;
      int f_id = -1;
      int r_id = -1;
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "t", NC_UNLIMITED, &t_dim));
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 2, &x_dim));
      ASSERT_EQ(NC_NOERR, nc_def_var(nc, "s", NC_INT, 0, nullptr, &s_id));
      ASSERT_EQ(NC_NOERR, nc_def_var(nc, "f", NC_INT, 1, &x_dim, &f_id));
      ASSERT_EQ(NC_NOERR, nc_def_var(nc, "r", NC_FLOAT, 1, &t_dim, &r_id));
      ASSERT_EQ(NC_NOERR, nc_enddef(nc));
      const int s_val = 99;
      const int f_vals[2] = {3, 4};
      const float r_vals[2] = {1.25F, 2.75F};
      ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, s_id, &s_val));
      ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, f_id, f_vals));
      const size_t start = 0;
      const size_t count = 2;
      ASSERT_EQ(NC_NOERR, nc_put_vara_float(nc, r_id, &start, &count, r_vals));
    }
    const std::string in_path = nc4_in.string();
    const std::string out_path = nc3_out.string();
    const CliResult res = RunNccopy({"-3", in_path, out_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(nc3_out.c_str(), NC_NOWRITE, out.ptr()));
    int fmt = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_format(out, &fmt));
    EXPECT_EQ(NC_FORMAT_CLASSIC, fmt);
    int s_id = -1;
    int f_id = -1;
    int r_id = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "s", &s_id));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "f", &f_id));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "r", &r_id));
    int s_val = 0;
    int f_vals[2] = {};
    float r_vals[2] = {};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(out, s_id, &s_val));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(out, f_id, f_vals));
    ASSERT_EQ(NC_NOERR, nc_get_var_float(out, r_id, r_vals));
    EXPECT_EQ(99, s_val);
    EXPECT_THAT(f_vals, ElementsAre(3, 4));
    EXPECT_THAT(r_vals, ElementsAre(1.25F, 2.75F));
  }

  // 4. CDF5 extended integer types with -r -w.
  {
    TempFile cdf5_in("cdf5_in.nc");
    TempFile cdf5_out("cdf5_out.nc");
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR, nc_create(cdf5_in.c_str(), NC_64BIT_DATA, nc.ptr()));
      int n_dim = -1;
      int t_dim = -1;
      int ub_v = -1;
      int u64_v = -1;
      int i64_r = -1;
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "n", 2, &n_dim));
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "t", NC_UNLIMITED, &t_dim));
      ASSERT_EQ(NC_NOERR, nc_def_var(nc, "ub", NC_UBYTE, 1, &n_dim, &ub_v));
      ASSERT_EQ(NC_NOERR, nc_def_var(nc, "u64", NC_UINT64, 1, &n_dim, &u64_v));
      ASSERT_EQ(NC_NOERR, nc_def_var(nc, "i64", NC_INT64, 1, &t_dim, &i64_r));
      ASSERT_EQ(NC_NOERR, nc_enddef(nc));
      const unsigned char ub_vals[2] = {200, 250};
      const unsigned long long u64_vals[2] = {1ULL << 50, (1ULL << 55) + 7ULL};
      const long long i64_vals[2] = {-(1LL << 40), 1LL << 42};
      ASSERT_EQ(NC_NOERR, nc_put_var_uchar(nc, ub_v, ub_vals));
      ASSERT_EQ(NC_NOERR, nc_put_var_ulonglong(nc, u64_v, u64_vals));
      const size_t start = 0;
      const size_t count = 2;
      ASSERT_EQ(NC_NOERR,
                nc_put_vara_longlong(nc, i64_r, &start, &count, i64_vals));
    }
    const std::string in_path = cdf5_in.string();
    const std::string out_path = cdf5_out.string();
#if defined(NETCDF_NCCOPY_TEST_MSAN)
    const CliResult res = RunNccopy({"-r", in_path, out_path});
#else
    const CliResult res = RunNccopy({"-r", "-w", in_path, out_path});
#endif
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(cdf5_out.c_str(), NC_NOWRITE, out.ptr()));
    int fmt = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_format(out, &fmt));
    EXPECT_EQ(NC_FORMAT_CDF5, fmt);
    int ub_id = -1;
    int u64_id = -1;
    int i64_id = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "ub", &ub_id));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "u64", &u64_id));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "i64", &i64_id));
    unsigned char ub_out[2] = {};
    unsigned long long u64_out[2] = {};
    long long i64_out[2] = {};
    ASSERT_EQ(NC_NOERR, nc_get_var_uchar(out, ub_id, ub_out));
    ASSERT_EQ(NC_NOERR, nc_get_var_ulonglong(out, u64_id, u64_out));
    ASSERT_EQ(NC_NOERR, nc_get_var_longlong(out, i64_id, i64_out));
    EXPECT_THAT(ub_out, ElementsAre(200, 250));
    EXPECT_THAT(u64_out, ElementsAre(1ULL << 50, (1ULL << 55) + 7ULL));
    EXPECT_THAT(i64_out, ElementsAre(-(1LL << 40), 1LL << 42));
  }

  // 5. NC_shellUnescape strips backslash before '#'.
  {
    TempFile esc_in("escaped#in.nc");
    TempFile esc_out("escaped#out.nc");
    CreateMinimalClassicFile(esc_in.c_str());
    const std::string in_esc =
        absl::StrReplaceAll(esc_in.string(), {{"#", "\\#"}});
    const std::string out_esc =
        absl::StrReplaceAll(esc_out.string(), {{"#", "\\#"}});
    const CliResult res = RunNccopy({in_esc, out_esc});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(esc_out.c_str(), NC_NOWRITE, out.ptr()));
    int a_id = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "a", &a_id));
    int a_vals[2] = {};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(out, a_id, a_vals));
    EXPECT_THAT(a_vals, ElementsAre(11, 22));
  }
}

TEST(NccopyTest, BufferResizingMultiIterationAndChunkCacheFlags) {
  // 1. Small -m buffer grows when value_size exceeds buffer (1-byte -> 8-byte).
  {
    TempFile in_nc("buf_grow_in.nc");
    TempFile out_nc("buf_grow_out.nc");
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR, nc_create(in_nc.c_str(), NC_NETCDF4, nc.ptr()));
      int one_dim = -1;
      int b_id = -1;
      int d_id = -1;
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "one", 1, &one_dim));
      ASSERT_EQ(NC_NOERR,
                nc_def_var(nc, "v_byte", NC_BYTE, 1, &one_dim, &b_id));
      ASSERT_EQ(NC_NOERR,
                nc_def_var(nc, "v_double", NC_DOUBLE, 1, &one_dim, &d_id));
      ASSERT_EQ(NC_NOERR, nc_enddef(nc));
      const signed char b_val = -42;
      const double d_val = 3.14159265;
      ASSERT_EQ(NC_NOERR, nc_put_var_schar(nc, b_id, &b_val));
      ASSERT_EQ(NC_NOERR, nc_put_var_double(nc, d_id, &d_val));
    }
    const std::string in_path = in_nc.string();
    const std::string out_path = out_nc.string();
    const CliResult res =
        RunNccopy({"-m", "2", "-h", "64k", "-e", "1k", in_path, out_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, out.ptr()));
    int b_out_id = -1;
    int d_out_id = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "v_byte", &b_out_id));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "v_double", &d_out_id));
    signed char b_out = 0;
    double d_out = 0.0;
    ASSERT_EQ(NC_NOERR, nc_get_var_schar(out, b_out_id, &b_out));
    ASSERT_EQ(NC_NOERR, nc_get_var_double(out, d_out_id, &d_out));
    EXPECT_EQ(-42, b_out);
    EXPECT_DOUBLE_EQ(3.14159265, d_out);
  }

  // 2. Empty unlimited var (nvalues == 0), multi-iteration nc_next_iter,
  //    chunksize > copy_buffer_size realloc, and all 4 -x cache branches.
  TempFile cache_in("chunk_cache_in.nc");
  std::vector<int> expected_grid(64);
  for (int i = 0; i < 64; ++i) {
    expected_grid[static_cast<size_t>(i)] = (i * 3) + 7;
  }
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(cache_in.c_str(), NC_NETCDF4, nc.ptr()));
    int x_dim = -1;
    int d0_dim = -1;
    int d1_dim = -1;
    int empty_u_dim = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 2, &x_dim));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d0", 8, &d0_dim));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d1", 8, &d1_dim));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "empty_u", NC_UNLIMITED, &empty_u_dim));

    int v_empty = -1;
    int v_small = -1;
    int v_large_contig = -1;
    int v_chunked = -1;
    int v_to_contig = -1;
    int v_to_chunk = -1;
    const int grid_dims[2] = {d0_dim, d1_dim};
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "v_empty", NC_INT, 1, &empty_u_dim, &v_empty));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_small", NC_INT, 1, &x_dim, &v_small));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_large_contig", NC_INT, 2, grid_dims,
                                   &v_large_contig));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "v_chunked", NC_INT, 2, grid_dims, &v_chunked));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_to_contig", NC_INT, 2, grid_dims,
                                   &v_to_contig));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "v_to_chunk", NC_INT, 2, grid_dims, &v_to_chunk));

    const size_t chunks4x4[2] = {4, 4};
    ASSERT_EQ(NC_NOERR,
              nc_def_var_chunking(nc, v_chunked, NC_CHUNKED, chunks4x4));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_chunking(nc, v_to_contig, NC_CHUNKED, chunks4x4));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    const int small_vals[2] = {101, 202};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, v_small, small_vals));
    ASSERT_EQ(NC_NOERR,
              nc_put_var_int(nc, v_large_contig, expected_grid.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, v_chunked, expected_grid.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, v_to_contig, expected_grid.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, v_to_chunk, expected_grid.data()));
  }

  const std::string in_path = cache_in.string();

  // (a) -m 16 forces multi-iteration copy on v_large_contig (256 B) and
  //     chunksize realloc (64 B > 16 B) on v_chunked.
  {
    TempFile out_nc("multi_iter_out.nc");
    const std::string out_path = out_nc.string();
    const CliResult res = RunNccopy(
        {"-m", "16", "-h", "1M", "-e", "2K", "-M", "0", in_path, out_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, out.ptr()));
    size_t u_len = 99;
    int empty_u_dim = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_dimid(out, "empty_u", &empty_u_dim));
    ASSERT_EQ(NC_NOERR, nc_inq_dimlen(out, empty_u_dim, &u_len));
    EXPECT_EQ(0U, u_len);

    int v_large = -1;
    int v_chunked = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "v_large_contig", &v_large));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "v_chunked", &v_chunked));
    std::vector<int> actual_large(64);
    std::vector<int> actual_chunked(64);
    ASSERT_EQ(NC_NOERR, nc_get_var_int(out, v_large, actual_large.data()));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(out, v_chunked, actual_chunked.data()));
    EXPECT_EQ(expected_grid, actual_large);
    EXPECT_EQ(expected_grid, actual_chunked);
  }

  // (b) -x covers all four (input contig/chunked x output contig/chunked)
  // paths.
  {
    TempFile out_nc("x_cache_out.nc");
    const std::string out_path = out_nc.string();
    const CliResult res = RunNccopy({"-x", "-M", "0", "-c", "v_chunked:2,4",
                                     "-c", "v_to_contig:contiguous", "-c",
                                     "v_to_chunk:4,4", in_path, out_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, out.ptr()));
    int storage = -1;
    size_t chunks[2] = {};
    int id = -1;

    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "v_small", &id));
    ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(out, id, &storage, nullptr));
    EXPECT_EQ(NC_CONTIGUOUS, storage);

    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "v_to_contig", &id));
    ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(out, id, &storage, nullptr));
    EXPECT_EQ(NC_CONTIGUOUS, storage);

    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "v_to_chunk", &id));
    ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(out, id, &storage, chunks));
    EXPECT_EQ(NC_CHUNKED, storage);
    EXPECT_THAT(chunks, ElementsAre(4U, 4U));

    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "v_chunked", &id));
    ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(out, id, &storage, chunks));
    EXPECT_EQ(NC_CHUNKED, storage);
    EXPECT_THAT(chunks, ElementsAre(2U, 4U));

    std::vector<int> actual(64);
    ASSERT_EQ(NC_NOERR, nc_get_var_int(out, id, actual.data()));
    EXPECT_EQ(expected_grid, actual);
  }
}

TEST(NccopyTest, ChunkingSpecsMinChunkThresholdAndAutoFormatUpgrade) {
  // 1. Classic input with -d, -s, or -c auto-upgrades to
  // NC_FORMAT_NETCDF4_CLASSIC.
  {
    TempFile classic_in("upgrade_in.nc");
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR, nc_create(classic_in.c_str(), NC_CLOBBER, nc.ptr()));
      int rows = -1;
      int cols = -1;
      int mat_id = -1;
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "rows", 4, &rows));
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "cols", 6, &cols));
      const int dims[2] = {rows, cols};
      ASSERT_EQ(NC_NOERR, nc_def_var(nc, "mat", NC_INT, 2, dims, &mat_id));
      ASSERT_EQ(NC_NOERR, nc_enddef(nc));
      std::vector<int> data(24, 17);
      ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, mat_id, data.data()));
    }
    const std::string in_path = classic_in.string();

    struct UpgradeCase {
      std::string_view tag;
      std::string_view flag;
      std::string_view val;
    };
    constexpr UpgradeCase kUpgradeCases[] = {
        {"deflate", "-d", "3"},
        {"shuffle", "-s", ""},
        {"chunk", "-c", "rows/2"},
    };
    for (const UpgradeCase& tc : kUpgradeCases) {
      SCOPED_TRACE(tc.tag);
      TempFile out_nc(std::string("upgrade_out_") + std::string(tc.tag) +
                      ".nc");
      const std::string out_path = out_nc.string();
      CliResult res;
      if (tc.val.empty()) {
        res = RunNccopy({tc.flag, "-M", "0", in_path, out_path});
      } else {
        res = RunNccopy({tc.flag, tc.val, "-M", "0", in_path, out_path});
      }
      ASSERT_TRUE(res.ok()) << res.harness_error;
      ASSERT_EQ(0, res.exit_code) << res.stderr_output;

      ScopedNcFile out;
      ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, out.ptr()));
      int fmt = -1;
      ASSERT_EQ(NC_NOERR, nc_inq_format(out, &fmt));
      EXPECT_EQ(NC_FORMAT_NETCDF4_CLASSIC, fmt);

      int mat_id = -1;
      ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "mat", &mat_id));
      std::vector<int> actual_mat(24);
      ASSERT_EQ(NC_NOERR, nc_get_var_int(out, mat_id, actual_mat.data()));
      EXPECT_EQ(std::vector<int>(24, 17), actual_mat);
      if (tc.tag == "chunk") {
        int storage = -1;
        size_t chunks[2] = {};
        ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(out, mat_id, &storage, chunks));
        EXPECT_EQ(NC_CHUNKED, storage);
        EXPECT_THAT(chunks, ElementsAre(2U, 6U));
      }
    }
  }

  // 2. NetCDF-4 chunkspecs: per-dim, per-var precedence, compact, contiguous,
  //    var:, unlimited default chunking, and -M minimum chunk threshold.
  TempFile nc4_in("chunk_modes_in.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(nc4_in.c_str(), NC_NETCDF4, nc.ptr()));
    int a_dim = -1;
    int b_dim = -1;
    int p_dim = -1;
    int q_dim = -1;
    int u_dim = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "a", 4, &a_dim));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "b", 6, &b_dim));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "p", 4, &p_dim));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "q", 4, &q_dim));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "u", NC_UNLIMITED, &u_dim));

    const int ab_dims[2] = {a_dim, b_dim};
    const int pq_dims[2] = {p_dim, q_dim};
    const int ub_dims[2] = {u_dim, b_dim};
    int v_dimspec = -1;
    int v_varspec = -1;
    int v_compact = -1;
    int v_to_compact = -1;
    int v_keep_chunk = -1;
    int v_to_contig = -1;
    int v_omit = -1;
    int v_unlim = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "v_dimspec", NC_INT, 2, ab_dims, &v_dimspec));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "v_varspec", NC_INT, 2, ab_dims, &v_varspec));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "v_compact", NC_INT, 2, pq_dims, &v_compact));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_to_compact", NC_INT, 2, pq_dims,
                                   &v_to_compact));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_keep_chunk", NC_INT, 2, pq_dims,
                                   &v_keep_chunk));
    // Use pq_dims for v_to_contig and v_omit because copy_chunking() lets a
    // matching per-dimension -c dim/n spec (like a/2,b/) force NC_CHUNKED even
    // when a per-variable :contiguous or omit spec was provided.
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "v_to_contig", NC_INT, 2, pq_dims, &v_to_contig));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_omit", NC_INT, 2, pq_dims, &v_omit));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "v_unlim", NC_INT, 2, ub_dims, &v_unlim));

    const size_t chunks2x2[2] = {2, 2};
    ASSERT_EQ(NC_NOERR,
              nc_def_var_chunking(nc, v_compact, NC_COMPACT, nullptr));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_chunking(nc, v_keep_chunk, NC_CHUNKED, chunks2x2));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_chunking(nc, v_to_contig, NC_CHUNKED, chunks2x2));
    ASSERT_EQ(NC_NOERR, nc_def_var_chunking(nc, v_omit, NC_CHUNKED, chunks2x2));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    const std::vector<int> ab_vals(24, 5);
    const std::vector<int> pq_vals(16, 9);
    const std::vector<int> ub_vals(12, 3);
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, v_dimspec, ab_vals.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, v_varspec, ab_vals.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, v_compact, pq_vals.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, v_to_compact, pq_vals.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, v_keep_chunk, pq_vals.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, v_to_contig, pq_vals.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, v_omit, pq_vals.data()));
    const size_t u_start[2] = {0, 0};
    const size_t u_count[2] = {2, 6};
    ASSERT_EQ(NC_NOERR,
              nc_put_vara_int(nc, v_unlim, u_start, u_count, ub_vals.data()));
  }

  const std::string nc4_in_path = nc4_in.string();

  // (a) With -M 0, explicit chunkspecs and compact/contiguous layouts stick.
  {
    TempFile out_nc("chunk_modes_m0_out.nc");
    const std::string out_path = out_nc.string();
    const CliResult res =
        RunNccopy({"-M", "0", "-c", "a/2,b/", "-c", "v_varspec:4,3", "-c",
                   "v_to_compact:compact", "-c", "v_to_contig:contiguous", "-c",
                   "v_omit:", nc4_in_path, out_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, out.ptr()));
    auto check_layout = [&](const char* name, int expected_storage,
                            std::initializer_list<size_t> expected_chunks) {
      int varid = -1;
      int storage = -1;
      size_t chunks[2] = {};
      ASSERT_EQ(NC_NOERR, nc_inq_varid(out, name, &varid));
      ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(out, varid, &storage, chunks));
      EXPECT_EQ(expected_storage, storage) << name;
      if (expected_storage == NC_CHUNKED) {
        std::vector<size_t> got(chunks, chunks + expected_chunks.size());
        std::vector<size_t> want(expected_chunks);
        EXPECT_EQ(want, got) << name;
      }
    };
    check_layout("v_dimspec", NC_CHUNKED, {2U, 6U});
    check_layout("v_varspec", NC_CHUNKED, {4U, 3U});
    check_layout("v_compact", NC_COMPACT, {});
    check_layout("v_to_compact", NC_COMPACT, {});
    check_layout("v_keep_chunk", NC_CHUNKED, {2U, 2U});
    check_layout("v_to_contig", NC_CONTIGUOUS, {});
    check_layout("v_omit", NC_CONTIGUOUS, {});
    check_layout("v_unlim", NC_CHUNKED, {1U, 6U});
  }

  // (b) Default -M (8192 bytes) coerces tiny non-unlimited vars to contiguous
  //     while keeping unlimited variable chunked.
  {
    TempFile out_nc("chunk_modes_default_m_out.nc");
    const std::string out_path = out_nc.string();
    const CliResult res = RunNccopy({"-c", "a/2", nc4_in_path, out_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, out.ptr()));
    int id = -1;
    int storage = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "v_dimspec", &id));
    ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(out, id, &storage, nullptr));
    EXPECT_EQ(NC_CONTIGUOUS, storage);

    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "v_compact", &id));
    ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(out, id, &storage, nullptr));
    EXPECT_EQ(NC_CONTIGUOUS, storage);

    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "v_unlim", &id));
    ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(out, id, &storage, nullptr));
    EXPECT_EQ(NC_CHUNKED, storage);
  }

  // (c) Global chunk suppression with -c / and -c // on fixed-only file.
  {
    TempFile fixed_chunked_in("fixed_chunked_in.nc");
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR,
                nc_create(fixed_chunked_in.c_str(), NC_NETCDF4, nc.ptr()));
      int a_dim = -1;
      int b_dim = -1;
      int cvar = -1;
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "a", 4, &a_dim));
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "b", 4, &b_dim));
      const int ab[2] = {a_dim, b_dim};
      ASSERT_EQ(NC_NOERR, nc_def_var(nc, "cvar", NC_INT, 2, ab, &cvar));
      const size_t c2[2] = {2, 2};
      ASSERT_EQ(NC_NOERR, nc_def_var_chunking(nc, cvar, NC_CHUNKED, c2));
      ASSERT_EQ(NC_NOERR, nc_enddef(nc));
      const std::vector<int> vals(16, 12);
      ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, cvar, vals.data()));
    }
    const std::string fixed_in_path = fixed_chunked_in.string();

    for (std::string_view omit_spec : {"/", "//"}) {
      SCOPED_TRACE(omit_spec);
      TempFile out_nc(std::string("omit_") +
                      (omit_spec == "/" ? "one" : "two") + ".nc");
      const std::string out_path = out_nc.string();
      const CliResult res =
          RunNccopy({"-c", omit_spec, fixed_in_path, out_path});
      ASSERT_TRUE(res.ok()) << res.harness_error;
      ASSERT_EQ(0, res.exit_code) << res.stderr_output;

      ScopedNcFile out;
      ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, out.ptr()));
      int cvar = -1;
      int storage = -1;
      ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "cvar", &cvar));
      ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(out, cvar, &storage, nullptr));
      EXPECT_EQ(NC_CONTIGUOUS, storage);
    }
  }

  // (d) Malformed or out-of-range -c chunkspecs exit non-zero.
  for (std::string_view bad_spec :
       {"no_such_var:1,2", "v_varspec:1,2,3", "v_varspec:99,99",
        "v_varspec:bad", "no_such_dim/2", "a/0", "a/bad", "a/2,,b/2"}) {
    SCOPED_TRACE(bad_spec);
    TempFile out_nc("bad_chunk_out.nc");
    const std::string out_path = out_nc.string();
    const CliResult res = RunNccopy({"-c", bad_spec, nc4_in_path, out_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_NE(0, res.exit_code);
  }

  // (e) Multiple netCDF-4 unlimited dimensions preserve unlim count normally
  //     and convert cleanly to fixed dimensions with -u.
  {
    TempFile multi_u_in("multi_unlim_in.nc");
    TempFile multi_u_keep("multi_unlim_keep.nc");
    TempFile multi_u_fixed("multi_unlim_fixed.nc");
    TempFile multi_u_fixed_m0("multi_unlim_fixed_m0.nc");
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR, nc_create(multi_u_in.c_str(), NC_NETCDF4, nc.ptr()));
      int u1_dim = -1;
      int u2_dim = -1;
      int v_id = -1;
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "u1", NC_UNLIMITED, &u1_dim));
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "u2", NC_UNLIMITED, &u2_dim));
      const int u_dims[2] = {u1_dim, u2_dim};
      ASSERT_EQ(NC_NOERR, nc_def_var(nc, "uv", NC_INT, 2, u_dims, &v_id));
      // Use explicit small chunks (total 8 B < 8192) because NetCDF-4's default
      // chunks for an all-unlimited 2D var exceed the 2x3 fixed dimensions.
      const size_t uv_chunks[2] = {1, 2};
      ASSERT_EQ(NC_NOERR, nc_def_var_chunking(nc, v_id, NC_CHUNKED, uv_chunks));
      ASSERT_EQ(NC_NOERR, nc_enddef(nc));
      const size_t start[2] = {0, 0};
      const size_t count[2] = {2, 3};
      const int vals[6] = {1, 2, 3, 4, 5, 6};
      ASSERT_EQ(NC_NOERR, nc_put_vara_int(nc, v_id, start, count, vals));
    }

    const CliResult keep_res =
        RunNccopy({multi_u_in.string(), multi_u_keep.string()});
    ASSERT_TRUE(keep_res.ok()) << keep_res.harness_error;
    ASSERT_EQ(0, keep_res.exit_code) << keep_res.stderr_output;
    {
      ScopedNcFile out;
      ASSERT_EQ(NC_NOERR, nc_open(multi_u_keep.c_str(), NC_NOWRITE, out.ptr()));
      int nunlims = 0;
      ASSERT_EQ(NC_NOERR, nc_inq_unlimdims(out, &nunlims, nullptr));
      EXPECT_EQ(2, nunlims);
      int v_id = -1;
      int storage = -1;
      size_t chunks[2] = {};
      ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "uv", &v_id));
      ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(out, v_id, &storage, chunks));
      EXPECT_EQ(NC_CHUNKED, storage);
      EXPECT_THAT(chunks, ElementsAre(1U, 2U));
      int vals[6] = {};
      ASSERT_EQ(NC_NOERR, nc_get_var_int(out, v_id, vals));
      EXPECT_THAT(vals, ElementsAre(1, 2, 3, 4, 5, 6));
    }

    // With default -M (8192) and -u, previously chunked unlimited dims become
    // fixed dimensions and tiny variables drop back to contiguous storage.
    const CliResult fix_res =
        RunNccopy({"-u", multi_u_in.string(), multi_u_fixed.string()});
    ASSERT_TRUE(fix_res.ok()) << fix_res.harness_error;
    ASSERT_EQ(0, fix_res.exit_code) << fix_res.stderr_output;
    {
      ScopedNcFile out;
      ASSERT_EQ(NC_NOERR,
                nc_open(multi_u_fixed.c_str(), NC_NOWRITE, out.ptr()));
      int nunlims = 99;
      ASSERT_EQ(NC_NOERR, nc_inq_unlimdims(out, &nunlims, nullptr));
      EXPECT_EQ(0, nunlims);
      int v_id = -1;
      int storage = -1;
      ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "uv", &v_id));
      ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(out, v_id, &storage, nullptr));
      EXPECT_EQ(NC_CONTIGUOUS, storage);
      int vals[6] = {};
      ASSERT_EQ(NC_NOERR, nc_get_var_int(out, v_id, vals));
      EXPECT_THAT(vals, ElementsAre(1, 2, 3, 4, 5, 6));
    }

    // With -u and -M 0, fixed dimensions retain the input chunk sizes.
    const CliResult fix_m0_res = RunNccopy(
        {"-u", "-M", "0", multi_u_in.string(), multi_u_fixed_m0.string()});
    ASSERT_TRUE(fix_m0_res.ok()) << fix_m0_res.harness_error;
    ASSERT_EQ(0, fix_m0_res.exit_code) << fix_m0_res.stderr_output;
    {
      ScopedNcFile out;
      ASSERT_EQ(NC_NOERR,
                nc_open(multi_u_fixed_m0.c_str(), NC_NOWRITE, out.ptr()));
      int nunlims = 99;
      ASSERT_EQ(NC_NOERR, nc_inq_unlimdims(out, &nunlims, nullptr));
      EXPECT_EQ(0, nunlims);
      int v_id = -1;
      int storage = -1;
      size_t chunks[2] = {};
      ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "uv", &v_id));
      ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(out, v_id, &storage, chunks));
      EXPECT_EQ(NC_CHUNKED, storage);
      EXPECT_THAT(chunks, ElementsAre(1U, 2U));
      int vals[6] = {};
      ASSERT_EQ(NC_NOERR, nc_get_var_int(out, v_id, vals));
      EXPECT_THAT(vals, ElementsAre(1, 2, 3, 4, 5, 6));
    }
  }

  // (f) All-unlimited variable with NetCDF-4 default chunksizes exceeds small
  //     fixed bounds under -u unless chunking is omitted via -c /.
  {
    TempFile def_u_in("multi_unlim_def_in.nc");
    TempFile def_u_bad("multi_unlim_def_bad.nc");
    TempFile def_u_ok("multi_unlim_def_ok.nc");
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR, nc_create(def_u_in.c_str(), NC_NETCDF4, nc.ptr()));
      int u1_dim = -1;
      int u2_dim = -1;
      int v_id = -1;
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "u1", NC_UNLIMITED, &u1_dim));
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "u2", NC_UNLIMITED, &u2_dim));
      const int u_dims[2] = {u1_dim, u2_dim};
      ASSERT_EQ(NC_NOERR, nc_def_var(nc, "uv", NC_INT, 2, u_dims, &v_id));
      ASSERT_EQ(NC_NOERR, nc_enddef(nc));
      const size_t start[2] = {0, 0};
      const size_t count[2] = {2, 3};
      const int vals[6] = {1, 2, 3, 4, 5, 6};
      ASSERT_EQ(NC_NOERR, nc_put_vara_int(nc, v_id, start, count, vals));
    }

    const CliResult bad_res =
        RunNccopy({"-u", def_u_in.string(), def_u_bad.string()});
    ASSERT_TRUE(bad_res.ok()) << bad_res.harness_error;
    EXPECT_NE(0, bad_res.exit_code);
    EXPECT_THAT(bad_res.stderr_output, HasSubstr("Bad chunk sizes"));

    const CliResult ok_res =
        RunNccopy({"-u", "-c", "/", def_u_in.string(), def_u_ok.string()});
    ASSERT_TRUE(ok_res.ok()) << ok_res.harness_error;
    ASSERT_EQ(0, ok_res.exit_code) << ok_res.stderr_output;
    {
      ScopedNcFile out;
      ASSERT_EQ(NC_NOERR, nc_open(def_u_ok.c_str(), NC_NOWRITE, out.ptr()));
      int nunlims = 99;
      ASSERT_EQ(NC_NOERR, nc_inq_unlimdims(out, &nunlims, nullptr));
      EXPECT_EQ(0, nunlims);
      int v_id = -1;
      int storage = -1;
      ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "uv", &v_id));
      ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(out, v_id, &storage, nullptr));
      EXPECT_EQ(NC_CONTIGUOUS, storage);
      int vals[6] = {};
      ASSERT_EQ(NC_NOERR, nc_get_var_int(out, v_id, vals));
      EXPECT_THAT(vals, ElementsAre(1, 2, 3, 4, 5, 6));
    }
  }
}

TEST(NccopyTest, CopiesGroupsUserDefinedTypesStringsAndSelectors) {
  struct PackedRecord {
    int id;
    uint8_t mode;
    uint8_t tag[3];
    double weight;
    int grid[2][2];
  };

  TempFile udt_in("udt_groups_in.nc");
  std::array<PackedRecord, 2> expected_cmpd{};
  std::memset(expected_cmpd.data(), 0,
              expected_cmpd.size() * sizeof(PackedRecord));
  expected_cmpd[0].id = 10;
  expected_cmpd[0].mode = 1;
  expected_cmpd[0].tag[0] = 'A';
  expected_cmpd[0].tag[1] = 'B';
  expected_cmpd[0].tag[2] = 'C';
  expected_cmpd[0].weight = 1.5;
  expected_cmpd[0].grid[0][0] = 1;
  expected_cmpd[0].grid[0][1] = 2;
  expected_cmpd[0].grid[1][0] = 3;
  expected_cmpd[0].grid[1][1] = 4;

  expected_cmpd[1].id = 20;
  expected_cmpd[1].mode = 2;
  expected_cmpd[1].tag[0] = 'X';
  expected_cmpd[1].tag[1] = 'Y';
  expected_cmpd[1].tag[2] = 'Z';
  expected_cmpd[1].weight = 2.75;
  expected_cmpd[1].grid[0][0] = 5;
  expected_cmpd[1].grid[0][1] = 6;
  expected_cmpd[1].grid[1][0] = 7;
  expected_cmpd[1].grid[1][1] = 8;

  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(udt_in.c_str(), NC_NETCDF4, nc.ptr()));
    int n_dim = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "n", 2, &n_dim));

    nc_type enum_t = NC_NAT;
    nc_type opaque_t = NC_NAT;
    nc_type vlen_t = NC_NAT;
    nc_type cmpd_t = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc, NC_UBYTE, "mode_t", &enum_t));
    const uint8_t m_one = 1;
    const uint8_t m_two = 2;
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_t, "LOW", &m_one));
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_t, "HIGH", &m_two));

    ASSERT_EQ(NC_NOERR, nc_def_opaque(nc, 3, "tag_t", &opaque_t));
    ASSERT_EQ(NC_NOERR, nc_def_vlen(nc, "ints_t", NC_INT, &vlen_t));

    ASSERT_EQ(NC_NOERR,
              nc_def_compound(nc, sizeof(PackedRecord), "record_t", &cmpd_t));
    ASSERT_EQ(NC_NOERR, nc_insert_compound(nc, cmpd_t, "id",
                                           offsetof(PackedRecord, id), NC_INT));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc, cmpd_t, "mode",
                                 offsetof(PackedRecord, mode), enum_t));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc, cmpd_t, "tag", offsetof(PackedRecord, tag),
                                 opaque_t));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc, cmpd_t, "weight",
                                 offsetof(PackedRecord, weight), NC_DOUBLE));
    const int grid_dims[2] = {2, 2};
    ASSERT_EQ(NC_NOERR, nc_insert_array_compound(nc, cmpd_t, "grid",
                                                 offsetof(PackedRecord, grid),
                                                 NC_INT, 2, grid_dims));

    int str_var = -1;
    int enum_var = -1;
    int opaque_var = -1;
    int vlen_var = -1;
    int cmpd_var = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "str_var", NC_STRING, 1, &n_dim, &str_var));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "enum_var", enum_t, 1, &n_dim, &enum_var));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "opaque_var", opaque_t, 1, &n_dim, &opaque_var));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "vlen_var", vlen_t, 1, &n_dim, &vlen_var));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "cmpd_var", cmpd_t, 1, &n_dim, &cmpd_var));

    int g1_id = -1;
    int g1a_id = -1;
    int g2_id = -1;
    int empty_id = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc, "g1", &g1_id));
    ASSERT_EQ(NC_NOERR, nc_def_grp(g1_id, "g1a", &g1a_id));
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc, "g2", &g2_id));
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc, "empty_grp", &empty_id));

    int g1_dim = -1;
    int g1_var = -1;
    int leaf_var = -1;
    int g2_var = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(g1_id, "sub_n", 3, &g1_dim));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(g1_id, "g1_var", NC_INT, 1, &g1_dim, &g1_var));
    ASSERT_EQ(NC_NOERR, nc_put_att_text(g1_id, g1_var, "note", 5, "in_g1"));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(g1a_id, "leaf_var", NC_DOUBLE, 0, nullptr, &leaf_var));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(g2_id, "g2_var", NC_INT, 1, &n_dim, &g2_var));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    const char* strs[2] = {"alpha", "beta-gamma"};
    const uint8_t enums[2] = {1, 2};
    const uint8_t opaques[6] = {'p', 'q', 'r', 's', 't', 'u'};
    int vlen_data0[2] = {10, 20};
    int vlen_data1[3] = {30, 40, 50};
    nc_vlen_t vlens[2] = {{2, vlen_data0}, {3, vlen_data1}};
    ASSERT_EQ(NC_NOERR, nc_put_var_string(nc, str_var, strs));
    ASSERT_EQ(NC_NOERR, nc_put_var(nc, enum_var, enums));
    ASSERT_EQ(NC_NOERR, nc_put_var(nc, opaque_var, opaques));
    ASSERT_EQ(NC_NOERR, nc_put_var(nc, vlen_var, vlens));
    ASSERT_EQ(NC_NOERR, nc_put_var(nc, cmpd_var, expected_cmpd.data()));

    const int g1_vals[3] = {100, 200, 300};
    const double leaf_val = 9.25;
    const int g2_vals[2] = {77, 88};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(g1_id, g1_var, g1_vals));
    ASSERT_EQ(NC_NOERR, nc_put_var_double(g1a_id, leaf_var, &leaf_val));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(g2_id, g2_var, g2_vals));
  }

  const std::string in_path = udt_in.string();

  // 1. Full copy preserves groups, all 4 UDT classes, strings, and vlens.
  {
    TempFile out_nc("udt_full_out.nc");
    const std::string out_path = out_nc.string();
    const CliResult res = RunNccopy({in_path, out_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, out.ptr()));

    ScopedNcStrings str_out(2);
    int str_var = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "str_var", &str_var));
    ASSERT_EQ(NC_NOERR, nc_get_var_string(out, str_var, str_out.data()));
    EXPECT_STREQ("alpha", str_out[0]);
    EXPECT_STREQ("beta-gamma", str_out[1]);

    ScopedNcVlens vlen_out(2);
    int vlen_var = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "vlen_var", &vlen_var));
    ASSERT_EQ(NC_NOERR, nc_get_var(out, vlen_var, vlen_out.data()));
    ASSERT_EQ(2U, vlen_out[0].len);
    ASSERT_EQ(3U, vlen_out[1].len);
    EXPECT_EQ(10, static_cast<const int*>(vlen_out[0].p)[0]);
    EXPECT_EQ(20, static_cast<const int*>(vlen_out[0].p)[1]);
    EXPECT_EQ(30, static_cast<const int*>(vlen_out[1].p)[0]);
    EXPECT_EQ(40, static_cast<const int*>(vlen_out[1].p)[1]);
    EXPECT_EQ(50, static_cast<const int*>(vlen_out[1].p)[2]);

    uint8_t enum_out[2] = {};
    uint8_t opaque_out[6] = {};
    int enum_var = -1;
    int opaque_var = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "enum_var", &enum_var));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "opaque_var", &opaque_var));
    ASSERT_EQ(NC_NOERR, nc_get_var(out, enum_var, enum_out));
    ASSERT_EQ(NC_NOERR, nc_get_var(out, opaque_var, opaque_out));
    EXPECT_THAT(enum_out, ElementsAre(1, 2));
    EXPECT_THAT(opaque_out, ElementsAre('p', 'q', 'r', 's', 't', 'u'));

    std::array<PackedRecord, 2> cmpd_out{};
    std::memset(cmpd_out.data(), 0, cmpd_out.size() * sizeof(PackedRecord));
    int cmpd_var = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "cmpd_var", &cmpd_var));
    ASSERT_EQ(NC_NOERR, nc_get_var(out, cmpd_var, cmpd_out.data()));
    EXPECT_EQ(10, cmpd_out[0].id);
    EXPECT_EQ(2, cmpd_out[1].mode);
    EXPECT_DOUBLE_EQ(2.75, cmpd_out[1].weight);
    EXPECT_EQ(8, cmpd_out[1].grid[1][1]);

    int g1_id = -1;
    int g1a_id = -1;
    int g2_id = -1;
    int empty_id = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_grp_full_ncid(out, "/g1", &g1_id));
    ASSERT_EQ(NC_NOERR, nc_inq_grp_full_ncid(out, "/g1/g1a", &g1a_id));
    ASSERT_EQ(NC_NOERR, nc_inq_grp_full_ncid(out, "/g2", &g2_id));
    ASSERT_EQ(NC_NOERR, nc_inq_grp_full_ncid(out, "/empty_grp", &empty_id));
    int empty_nvars = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_nvars(empty_id, &empty_nvars));
    EXPECT_EQ(0, empty_nvars);

    int g1_var = -1;
    int leaf_var = -1;
    int g2_var = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(g1_id, "g1_var", &g1_var));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(g1a_id, "leaf_var", &leaf_var));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(g2_id, "g2_var", &g2_var));

    int g1_vals[3] = {};
    double leaf_val = 0.0;
    int g2_vals[2] = {};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(g1_id, g1_var, g1_vals));
    ASSERT_EQ(NC_NOERR, nc_get_var_double(g1a_id, leaf_var, &leaf_val));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(g2_id, g2_var, g2_vals));
    EXPECT_THAT(g1_vals, ElementsAre(100, 200, 300));
    EXPECT_DOUBLE_EQ(9.25, leaf_val);
    EXPECT_THAT(g2_vals, ElementsAre(77, 88));
  }

  // 2. -v keeps all variable definitions, copies data for listed vars only.
  {
    TempFile out_nc("udt_v_out.nc");
    const std::string out_path = out_nc.string();
    const CliResult res =
        RunNccopy({"-v", "str_var,/g1/g1a/leaf_var", in_path, out_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, out.ptr()));
    int root_nvars = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_nvars(out, &root_nvars));
    EXPECT_EQ(5, root_nvars);
    int str_var = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "str_var", &str_var));
    ScopedNcStrings str_out(2);
    ASSERT_EQ(NC_NOERR, nc_get_var_string(out, str_var, str_out.data()));
    EXPECT_STREQ("alpha", str_out[0]);
    EXPECT_STREQ("beta-gamma", str_out[1]);

    int g1a_id = -1;
    int leaf_var = -1;
    double leaf_val = 0.0;
    ASSERT_EQ(NC_NOERR, nc_inq_grp_full_ncid(out, "/g1/g1a", &g1a_id));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(g1a_id, "leaf_var", &leaf_var));
    ASSERT_EQ(NC_NOERR, nc_get_var_double(g1a_id, leaf_var, &leaf_val));
    EXPECT_DOUBLE_EQ(9.25, leaf_val);
  }

  // 3. -V keeps definitions and data ONLY for listed variables.
  {
    TempFile out_nc("udt_V_out.nc");
    const std::string out_path = out_nc.string();
    const CliResult res =
        RunNccopy({"-V", "vlen_var,/g1/g1_var", in_path, out_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, out.ptr()));
    int id = -1;
    EXPECT_EQ(NC_ENOTVAR, nc_inq_varid(out, "str_var", &id));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "vlen_var", &id));

    ScopedNcVlens vlen_out(2);
    ASSERT_EQ(NC_NOERR, nc_get_var(out, id, vlen_out.data()));
    ASSERT_EQ(2U, vlen_out[0].len);
    EXPECT_EQ(20, static_cast<const int*>(vlen_out[0].p)[1]);

    int g1_id = -1;
    int g2_id = -1;
    int g2_nvars = -1;
    int g1_var = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_grp_full_ncid(out, "/g1", &g1_id));
    ASSERT_EQ(NC_NOERR, nc_inq_grp_full_ncid(out, "/g2", &g2_id));
    ASSERT_EQ(NC_NOERR, nc_inq_nvars(g2_id, &g2_nvars));
    EXPECT_EQ(0, g2_nvars);
    ASSERT_EQ(NC_NOERR, nc_inq_varid(g1_id, "g1_var", &g1_var));
    int g1_vals[3] = {};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(g1_id, g1_var, g1_vals));
    EXPECT_THAT(g1_vals, ElementsAre(100, 200, 300));
  }

  // 4. -g keeps full group structure, populates selected group's data.
  {
    TempFile out_nc("udt_g_out.nc");
    const std::string out_path = out_nc.string();
    const CliResult res = RunNccopy({"-g", "/g1", in_path, out_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, out.ptr()));
    int g1_id = -1;
    int g2_id = -1;
    int g1_var = -1;
    int g2_var = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_grp_full_ncid(out, "/g1", &g1_id));
    ASSERT_EQ(NC_NOERR, nc_inq_grp_full_ncid(out, "/g2", &g2_id));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(g1_id, "g1_var", &g1_var));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(g2_id, "g2_var", &g2_var));
    int g1_vals[3] = {};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(g1_id, g1_var, g1_vals));
    EXPECT_THAT(g1_vals, ElementsAre(100, 200, 300));
  }

  // 5. -G omits non-selected groups entirely.
  {
    TempFile out_nc("udt_G_out.nc");
    const std::string out_path = out_nc.string();
    const CliResult res = RunNccopy({"-G", "/g1", in_path, out_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, out.ptr()));
    int g1_id = -1;
    int g1_var = -1;
    int missing_id = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_grp_full_ncid(out, "/g1", &g1_id));
    EXPECT_EQ(NC_ENOGRP, nc_inq_grp_full_ncid(out, "/g2", &missing_id));
    EXPECT_EQ(NC_ENOGRP, nc_inq_grp_full_ncid(out, "/g1/g1a", &missing_id));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(g1_id, "g1_var", &g1_var));
    int g1_vals[3] = {};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(g1_id, g1_var, g1_vals));
    EXPECT_THAT(g1_vals, ElementsAre(100, 200, 300));
  }

  // 6. Selector error paths (-G subgroup without parent, missing grp/var,
  //    -g on classic file).
  {
    TempFile out_nc("sel_err_out.nc");
    const std::string out_path = out_nc.string();

    const CliResult missing_parent =
        RunNccopy({"-G", "/g1/g1a", in_path, out_path});
    ASSERT_TRUE(missing_parent.ok()) << missing_parent.harness_error;
    EXPECT_NE(0, missing_parent.exit_code);
    EXPECT_THAT(missing_parent.stderr_output,
                HasSubstr("trying to copy a group but not the parent"));

    const CliResult missing_grp =
        RunNccopy({"-g", "/no_such_group", in_path, out_path});
    ASSERT_TRUE(missing_grp.ok()) << missing_grp.harness_error;
    EXPECT_NE(0, missing_grp.exit_code);

    const CliResult missing_var =
        RunNccopy({"-v", "no_such_var", in_path, out_path});
    ASSERT_TRUE(missing_var.ok()) << missing_var.harness_error;
    EXPECT_NE(0, missing_var.exit_code);

    TempFile classic_in("sel_classic_in.nc");
    CreateMinimalClassicFile(classic_in.c_str());
    const std::string classic_path = classic_in.string();
    const CliResult grp_on_classic =
        RunNccopy({"-g", "/g1", classic_path, out_path});
    ASSERT_TRUE(grp_on_classic.ok()) << grp_on_classic.harness_error;
    EXPECT_NE(0, grp_on_classic.exit_code);
    EXPECT_THAT(grp_on_classic.stderr_output,
                HasSubstr("only permitted for netCDF-4 file"));
  }
}

TEST(NccopyTest, CompressionChecksumsEndiannessAndFilterSpecs) {
  TempFile in_nc("filters_in.nc");
  std::vector<int> expected(16);
  for (int i = 0; i < 16; ++i) {
    expected[static_cast<size_t>(i)] = (i + 1) * 11;
  }

  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(in_nc.c_str(), NC_NETCDF4, nc.ptr()));
    int r_dim = -1;
    int c_dim = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "r", 4, &r_dim));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "c", 4, &c_dim));
    const int rc_dims[2] = {r_dim, c_dim};

    int v_deflate = -1;
    int v_fletcher = -1;
    int v_bigendian = -1;
    int v_szip = -1;
    int v_plain = -1;
    int v_escaped = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "v_deflate", NC_INT, 2, rc_dims, &v_deflate));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "v_fletcher", NC_INT, 2, rc_dims, &v_fletcher));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "v_bigendian", NC_INT, 2, rc_dims, &v_bigendian));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_szip", NC_INT, 2, rc_dims, &v_szip));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "v_plain", NC_INT, 2, rc_dims, &v_plain));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "0var a", NC_INT, 2, rc_dims, &v_escaped));

    int sub_id = -1;
    int sub_var = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc, "sub", &sub_id));
    ASSERT_EQ(NC_NOERR, nc_def_var(sub_id, "sv", NC_INT, 2, rc_dims, &sub_var));

    const size_t chunks4x4[2] = {4, 4};
    ASSERT_EQ(NC_NOERR, nc_def_var_deflate(nc, v_deflate, 1, 1, 4));
    ASSERT_EQ(NC_NOERR, nc_def_var_fletcher32(nc, v_fletcher, NC_FLETCHER32));
    ASSERT_EQ(NC_NOERR, nc_def_var_endian(nc, v_bigendian, NC_ENDIAN_BIG));
    ASSERT_EQ(NC_NOERR, nc_def_var_chunking(nc, v_szip, NC_CHUNKED, chunks4x4));
    ASSERT_EQ(NC_NOERR, nc_def_var_szip(nc, v_szip, NC_SZIP_EC, 8));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    for (int id :
         {v_deflate, v_fletcher, v_bigendian, v_szip, v_plain, v_escaped}) {
      ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, id, expected.data()));
    }
    ASSERT_EQ(NC_NOERR, nc_put_var_int(sub_id, sub_var, expected.data()));
  }

  const std::string in_path = in_nc.string();

  // 1. Default copy preserves deflate+shuffle, fletcher32, big-endian, and
  //    existing HDF5 filters (SZIP via copy_var_filter row 4).
  {
    TempFile out_nc("filters_preserve_out.nc");
    const std::string out_path = out_nc.string();
    const CliResult res = RunNccopy({"-M", "0", in_path, out_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, out.ptr()));
    int id = -1;
    int shuffle = 0;
    int deflate = 0;
    int level = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "v_deflate", &id));
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_deflate(out, id, &shuffle, &deflate, &level));
    EXPECT_EQ(1, shuffle);
    EXPECT_EQ(1, deflate);
    EXPECT_EQ(4, level);

    int fletcher = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "v_fletcher", &id));
    ASSERT_EQ(NC_NOERR, nc_inq_var_fletcher32(out, id, &fletcher));
    EXPECT_EQ(NC_FLETCHER32, fletcher);

    int endian = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "v_bigendian", &id));
    ASSERT_EQ(NC_NOERR, nc_inq_var_endian(out, id, &endian));
    EXPECT_EQ(NC_ENDIAN_BIG, endian);

    size_t nfilters = 0;
    unsigned int filter_id = 0;
    size_t nparams = 0;
    unsigned int params[2] = {};
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "v_szip", &id));
    ASSERT_EQ(NC_NOERR, nc_inq_var_filter_ids(out, id, &nfilters, &filter_id));
    ASSERT_EQ(1U, nfilters);
    EXPECT_EQ(static_cast<unsigned int>(H5Z_FILTER_SZIP), filter_id);
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_filter_info(out, id, filter_id, &nparams, params));
    EXPECT_EQ(2U, nparams);
    EXPECT_EQ(static_cast<unsigned int>(NC_SZIP_EC), params[0]);
    EXPECT_EQ(8U, params[1]);

    std::vector<int> actual(16);
    ASSERT_EQ(NC_NOERR, nc_get_var_int(out, id, actual.data()));
    EXPECT_EQ(expected, actual);
  }

  // 2. -d 0 removes input deflate/shuffle and suppresses non-deflate filters.
  {
    TempFile out_nc("filters_d0_out.nc");
    const std::string out_path = out_nc.string();
    const CliResult res = RunNccopy({"-d", "0", "-M", "0", in_path, out_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, out.ptr()));
    int id = -1;
    int shuffle = -1;
    int deflate = -1;
    int level = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "v_deflate", &id));
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_deflate(out, id, &shuffle, &deflate, &level));
    EXPECT_EQ(0, shuffle);
    EXPECT_EQ(0, deflate);

    size_t nfilters = 99;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "v_szip", &id));
    ASSERT_EQ(NC_NOERR, nc_inq_var_filter_ids(out, id, &nfilters, nullptr));
    EXPECT_EQ(0U, nfilters);
  }

  // 3. -d 6 -s adds shuffle+deflate across variables cleanly.
  {
    TempFile out_nc("filters_d6s_out.nc");
    const std::string out_path = out_nc.string();
    const CliResult res =
        RunNccopy({"-d", "6", "-s", "-M", "0", in_path, out_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, out.ptr()));
    int id = -1;
    int shuffle = 0;
    int deflate = 0;
    int level = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "v_plain", &id));
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_deflate(out, id, &shuffle, &deflate, &level));
    EXPECT_EQ(1, shuffle);
    EXPECT_EQ(1, deflate);
    EXPECT_EQ(6, level);
    std::vector<int> actual(16);
    ASSERT_EQ(NC_NOERR, nc_get_var_int(out, id, actual.data()));
    EXPECT_EQ(expected, actual);
  }

  // 4. -F none and -F *,NONE suppress all filters on output.
  for (std::string_view none_spec : {"none", "*,NONE"}) {
    SCOPED_TRACE(none_spec);
    TempFile out_nc(std::string("filters_") +
                    (none_spec == "none" ? "none" : "starnone") + "_out.nc");
    const std::string out_path = out_nc.string();
    const CliResult res =
        RunNccopy({"-F", none_spec, "-M", "0", in_path, out_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, out.ptr()));
    for (const char* name : {"v_deflate", "v_fletcher", "v_szip"}) {
      int id = -1;
      size_t nfilters = 99;
      ASSERT_EQ(NC_NOERR, nc_inq_varid(out, name, &id));
      ASSERT_EQ(NC_NOERR, nc_inq_var_filter_ids(out, id, &nfilters, nullptr));
      EXPECT_EQ(0U, nfilters) << name;
    }
  }

  // 5. Wildcard -F *,1,4 applies deflate level 4 to every variable.
  {
    TempFile out_nc("filters_wildcard_out.nc");
    const std::string out_path = out_nc.string();
    const CliResult res =
        RunNccopy({"-F", "*,1,4", "-M", "0", in_path, out_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, out.ptr()));
    int id = -1;
    int shuffle = 0;
    int deflate = 0;
    int level = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "v_plain", &id));
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_deflate(out, id, &shuffle, &deflate, &level));
    EXPECT_EQ(1, deflate);
    EXPECT_EQ(4, level);

    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "v_szip", &id));
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_deflate(out, id, &shuffle, &deflate, &level));
    EXPECT_EQ(1, deflate);
    EXPECT_EQ(4, level);
  }

  // 6. Targeted -F specs: empty spec, filterless var name, per-var "none",
  //    '&' varlist with '/sub/sv', escaped FQN "\0var\ a" multi-filter
  //    pipeline "2|3|1,4", and explicit SZIP override "4,32,8".
  {
    TempFile out_nc("filters_targeted_out.nc");
    const std::string out_path = out_nc.string();
    const CliResult res = RunNccopy(
        {"-M", "0", "-F", "", "-F", "v_bigendian", "-F", "v_plain,none", "-F",
         "v_fletcher&&/sub/sv,1,5", "-F", "\\0var\\ a,2|3|1,4", "-F",
         "v_szip,4,32,8", in_path, out_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    ASSERT_EQ(0, res.exit_code) << res.stderr_output;

    ScopedNcFile out;
    ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, out.ptr()));

    int id = -1;
    size_t nfilters = 99;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "v_plain", &id));
    ASSERT_EQ(NC_NOERR, nc_inq_var_filter_ids(out, id, &nfilters, nullptr));
    EXPECT_EQ(0U, nfilters);

    int shuffle = 0;
    int deflate = 0;
    int level = 0;
    int fletcher = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "v_fletcher", &id));
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_deflate(out, id, &shuffle, &deflate, &level));
    ASSERT_EQ(NC_NOERR, nc_inq_var_fletcher32(out, id, &fletcher));
    EXPECT_EQ(1, deflate);
    EXPECT_EQ(5, level);
    EXPECT_EQ(NC_FLETCHER32, fletcher);

    int sub_id = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_grp_full_ncid(out, "/sub", &sub_id));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(sub_id, "sv", &id));
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_deflate(sub_id, id, &shuffle, &deflate, &level));
    EXPECT_EQ(1, deflate);
    EXPECT_EQ(5, level);

    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "0var a", &id));
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_deflate(out, id, &shuffle, &deflate, &level));
    ASSERT_EQ(NC_NOERR, nc_inq_var_fletcher32(out, id, &fletcher));
    EXPECT_EQ(1, shuffle);
    EXPECT_EQ(1, deflate);
    EXPECT_EQ(4, level);
    EXPECT_EQ(NC_FLETCHER32, fletcher);
    std::vector<int> actual_esc(16);
    ASSERT_EQ(NC_NOERR, nc_get_var_int(out, id, actual_esc.data()));
    EXPECT_EQ(expected, actual_esc);

    unsigned int szip_id = 0;
    size_t szip_nparams = 0;
    unsigned int szip_params[2] = {};
    ASSERT_EQ(NC_NOERR, nc_inq_varid(out, "v_szip", &id));
    nfilters = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_var_filter_ids(out, id, &nfilters, &szip_id));
    ASSERT_EQ(1U, nfilters);
    EXPECT_EQ(static_cast<unsigned int>(H5Z_FILTER_SZIP), szip_id);
    ASSERT_EQ(NC_NOERR, nc_inq_var_filter_info(out, id, szip_id, &szip_nparams,
                                               szip_params));
    EXPECT_EQ(2U, szip_nparams);
    EXPECT_EQ(static_cast<unsigned int>(NC_SZIP_NN), szip_params[0]);
    EXPECT_EQ(8U, szip_params[1]);
    std::vector<int> actual_szip(16);
    ASSERT_EQ(NC_NOERR, nc_get_var_int(out, id, actual_szip.data()));
    EXPECT_EQ(expected, actual_szip);
  }

  // 7. Invalid -F filter specs fail cleanly.
  for (std::string_view bad_filter :
       {",1,4", "v_plain,bad", "v_plain,99999,1"}) {
    SCOPED_TRACE(bad_filter);
    TempFile out_nc("filters_bad_out.nc");
    const std::string out_path = out_nc.string();
    const CliResult res = RunNccopy({"-F", bad_filter, in_path, out_path});
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_NE(0, res.exit_code);
  }
}

}  // namespace
}  // namespace netcdf
