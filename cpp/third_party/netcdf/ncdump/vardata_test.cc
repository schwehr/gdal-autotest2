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

// Unit tests for //third_party/netcdf/ncdump/vardata.c.
//
// Exercises lput/lput2 wrapping, attribute value formatting, CDL vardata across
// scalar/1D/2D/3D/unlimited/text/string/fill variables and brief/full C and
// Fortran comment annotations, and NcML vardatax including upcorner odometer
// carry. Subtests run inside isolated forked children because dumplib.c and
// vardata.c hold process-wide static caches (is_unlim_dim, init_types, linep)
// and print directly to stdout.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncdump/vardata.c

#include <cstdio>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/ncdump_globals_stub.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

namespace netcdf {
namespace {

using ::testing::HasSubstr;

void InitDumpRuntime(int ncid) {
  ncdump_stub_reset_globals();
  set_formats(FLT_DIGITS, DBL_DIGITS);
  init_epsilons();
  indent_init();
  set_max_len(80);
  init_types(ncid);
}

TEST(VardataTest, LputAndLput2WrapAndResetIndent) {
  constexpr std::string_view kArgs[] = {"vardata_lput"};
  const CliResult res = RunCliInForkedChild(
      [](int /*argc*/, char** /*argv*/) -> int {
        ncdump_stub_reset_globals();
        set_formats(FLT_DIGITS, DBL_DIGITS);
        init_epsilons();
        indent_init();
        indent_more();
        set_max_len(12);  // max_line_len = 10

        lput("");
        lput("ab");        // linep = 2, nn == 2
        lput("12345");     // linep = 7 <= 10
        lput("67890");     // 7 + 5 > 10 && nn > 2 -> wraps; linep = 6 + 5 = 11
        lput("XY");        // nn == 2 -> never wraps even when 11 + 2 > 10
        set_max_len(20);   // max_line_len = 18 >= 13 + 4
        lput("end\n");     // trailing '\n' resets linep = indent_get() (2)
        set_max_len(12);   // max_line_len = 10
        lput("12345678");  // 2 + 8 <= 10 -> does not wrap after '\n' reset

        set_max_len(30);  // max_line_len + 2 = 30
        lput2("alpha", 1, 1);
        lput2("beta", 0, 1);
        set_max_len(80);
        lput2("g", 0, 1);
        lput2("delta", 1, 0);
        lput2("epsilon", 0, 0);
        return 0;
      },
      kArgs);

  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
  EXPECT_THAT(res.stdout_output,
              HasSubstr("ab12345\n      67890XYend\n12345678"));
  EXPECT_THAT(res.stdout_output, HasSubstr("\n  \t\t  // alpha\n"));
  EXPECT_THAT(res.stdout_output, HasSubstr("\t\t  // beta g // delta epsilon"));
}

TEST(VardataTest, PrAnyAttValsPrintsSingleAndWrappedMultiElements) {
  constexpr std::string_view kArgs[] = {"vardata_pr_att"};
  const CliResult res = RunCliInForkedChild(
      [](int /*argc*/, char** /*argv*/) -> int {
        ScopedNcFile nc;
        if (nc_create_mem("att_vals.nc", NC_NETCDF4, 4096, nc.ptr()) !=
            NC_NOERR) {
          return 1;
        }
        if (nc_enddef(nc) != NC_NOERR) {
          return 2;
        }
        InitDumpRuntime(nc);

        ncatt_t att{};
        att.tinfo = get_typeinfo(NC_INT);

        constexpr int kSingle = 77;
        att.len = 1;
        pr_any_att_vals(&att, &kSingle);
        lput("\n");

        set_max_len(16);
        constexpr int kMulti[6] = {100, 200, 300, 400, 500, 600};
        att.len = 6;
        pr_any_att_vals(&att, kMulti);
        lput("\n");
        return 0;
      },
      kArgs);

  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
  EXPECT_THAT(res.stdout_output, HasSubstr("77\n"));
  EXPECT_THAT(res.stdout_output, HasSubstr("100, 200, "));
  EXPECT_THAT(res.stdout_output, HasSubstr("\n    300, "));
  EXPECT_THAT(res.stdout_output, HasSubstr("600\n"));
}

TEST(VardataTest, VardataPrintsScalarsArraysTextStringsFillAndUnlimDims) {
  constexpr std::string_view kArgs[] = {"vardata_cdl"};
  const CliResult res = RunCliInForkedChild(
      [](int /*argc*/, char** /*argv*/) -> int {
        ScopedMemio memio;
        {
          ScopedNcFile nc;
          if (nc_create_mem("cdl.nc", NC_NETCDF4, 16384, nc.ptr()) !=
              NC_NOERR) {
            return 1;
          }
          int u1 = -1;
          int u2 = -1;
          int d1 = -1;
          int d2 = -1;
          int d3 = -1;
          int d20 = -1;
          if (nc_def_dim(nc, "u1", NC_UNLIMITED, &u1) != NC_NOERR ||
              nc_def_dim(nc, "u2", NC_UNLIMITED, &u2) != NC_NOERR ||
              nc_def_dim(nc, "d1", 1, &d1) != NC_NOERR ||
              nc_def_dim(nc, "d2", 2, &d2) != NC_NOERR ||
              nc_def_dim(nc, "d3", 3, &d3) != NC_NOERR ||
              nc_def_dim(nc, "d20", 20, &d20) != NC_NOERR) {
            return 2;
          }

          int v_sfill = -1;
          int v_sval = -1;
          int v_1d = -1;
          int v_str = -1;
          int v_2d_col1 = -1;
          int v_2d = -1;
          int v_unlim = -1;
          int v_text = -1;
          int v_char_fmt = -1;

          const int dims_2d_col1[2] = {d2, d1};
          const int dims_2d[2] = {d2, d3};
          const int dims_unlim[3] = {u1, u2, d3};
          const int dims_text[2] = {d3, d20};

          if (nc_def_var(nc, "s_fill", NC_INT, 0, nullptr, &v_sfill) !=
                  NC_NOERR ||
              nc_def_var(nc, "s_val", NC_INT, 0, nullptr, &v_sval) !=
                  NC_NOERR ||
              nc_def_var(nc, "v_1d", NC_INT, 1, &d3, &v_1d) != NC_NOERR ||
              nc_def_var(nc, "v_str", NC_STRING, 1, &d2, &v_str) != NC_NOERR ||
              nc_def_var(nc, "v_2d_col1", NC_DOUBLE, 2, dims_2d_col1,
                         &v_2d_col1) != NC_NOERR ||
              nc_def_var(nc, "v_2d", NC_INT, 2, dims_2d, &v_2d) != NC_NOERR ||
              nc_def_var(nc, "v_unlim", NC_INT, 3, dims_unlim, &v_unlim) !=
                  NC_NOERR ||
              nc_def_var(nc, "v_text", NC_CHAR, 2, dims_text, &v_text) !=
                  NC_NOERR ||
              nc_def_var(nc, "v_char_fmt", NC_CHAR, 1, &d3, &v_char_fmt) !=
                  NC_NOERR) {
            return 3;
          }

          if (nc_enddef(nc) != NC_NOERR) {
            return 4;
          }

          constexpr int kFillInt = NC_FILL_INT;
          constexpr int kScalar42 = 42;
          constexpr int kOneD[3] = {10, 20, 30};
          const char* kStrs[2] = {"one", "two"};
          constexpr double kCol1[2] = {1.5, 2.5};
          constexpr int kTwoD[6] = {1, 2, 3, 4, 5, 6};
          constexpr size_t kZero3[3] = {0, 0, 0};
          constexpr size_t kUnlimCount[3] = {2, 2, 3};
          constexpr int kCube[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
          constexpr char kText[3][20] = {
              "\b\f\n\r\t\v\\\'\"A\x01",
              "hello",
              "",
          };
          constexpr char kRawChars[3] = {'x', 'y', 'z'};

          if (nc_put_var_int(nc, v_sfill, &kFillInt) != NC_NOERR ||
              nc_put_var_int(nc, v_sval, &kScalar42) != NC_NOERR ||
              nc_put_var_int(nc, v_1d, kOneD) != NC_NOERR ||
              nc_put_var_string(nc, v_str, kStrs) != NC_NOERR ||
              nc_put_var_double(nc, v_2d_col1, kCol1) != NC_NOERR ||
              nc_put_var_int(nc, v_2d, kTwoD) != NC_NOERR ||
              nc_put_vara_int(nc, v_unlim, kZero3, kUnlimCount, kCube) !=
                  NC_NOERR ||
              nc_put_var_text(nc, v_text, &kText[0][0]) != NC_NOERR ||
              nc_put_var_text(nc, v_char_fmt, kRawChars) != NC_NOERR) {
            return 5;
          }

          if (nc_close_memio(nc.release(), memio.get()) != NC_NOERR) {
            return 6;
          }
        }

        ScopedNcFile nc;
        if (nc_open_mem("cdl.nc", NC_NOWRITE, memio.size(), memio.get()->memory,
                        nc.ptr()) != NC_NOERR) {
          return 7;
        }
        InitDumpRuntime(nc);

        auto dump_var = [&](const char* name, bool use_fill, const char* fmt) {
          int vid = -1;
          if (nc_inq_varid(nc, name, &vid) != NC_NOERR) {
            return false;
          }
          ncvar_t var{};
          int dimids[NC_MAX_VAR_DIMS]{};
          if (nc_inq_var(nc, vid, var.name, &var.type, &var.ndims, dimids,
                         &var.natts) != NC_NOERR) {
            return false;
          }
          size_t vdims[NC_MAX_VAR_DIMS]{};
          for (int i = 0; i < var.ndims; ++i) {
            if (nc_inq_dimlen(nc, dimids[i], &vdims[i]) != NC_NOERR) {
              return false;
            }
          }
          var.dims = var.ndims > 0 ? dimids : nullptr;
          var.tinfo = get_typeinfo(var.type);
          var.locid = nc;
          var.fmt = fmt != nullptr ? fmt : get_fmt(nc, vid, var.type);
          int fill_storage = NC_FILL_INT;
          if (use_fill) {
            var.has_fillval = 1;
            var.fillvalp = &fill_storage;
          }
          set_tostring_func(&var);
          return vardata(&var, vdims, nc, vid) == 0;
        };

        if (!dump_var("s_fill", true, "%d") || !dump_var("s_val", true, "%d") ||
            !dump_var("v_1d", false, "%d") ||
            !dump_var("v_str", false, "\"%s\"") ||
            !dump_var("v_2d_col1", false, nullptr) ||
            !dump_var("v_2d", false, "%d") ||
            !dump_var("v_unlim", false, "%d") ||
            !dump_var("v_text", false, "%s") ||
            !dump_var("v_char_fmt", false, "") ||
            !dump_var("v_char_fmt", false, "%c")) {
          return 8;
        }

        // Brief data comments with C and Fortran indexing and cols==1 vs >1.
        formatting_specs.brief_data_cmnts = 1;
        formatting_specs.data_lang = LANG_C;
        if (!dump_var("v_2d_col1", false, nullptr) ||
            !dump_var("v_2d", false, "%d")) {
          return 9;
        }
        formatting_specs.data_lang = LANG_F;
        if (!dump_var("v_2d_col1", false, nullptr) ||
            !dump_var("v_2d", false, "%d") ||
            !dump_var("v_unlim", false, "%d")) {
          return 10;
        }

        // Full data comments on ranked vars (including nested unlim braces).
        formatting_specs.brief_data_cmnts = 0;
        formatting_specs.full_data_cmnts = 1;
        formatting_specs.data_lang = LANG_C;
        if (!dump_var("v_1d", false, "%d") ||
            !dump_var("v_unlim", false, "%d")) {
          return 11;
        }
        formatting_specs.data_lang = LANG_F;
        if (!dump_var("v_unlim", false, "%d")) {
          return 12;
        }
        return 0;
      },
      kArgs);

  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;

  EXPECT_THAT(res.stdout_output, HasSubstr(" s_fill = _ ;"));
  EXPECT_THAT(res.stdout_output, HasSubstr(" s_val = 42 ;"));
  EXPECT_THAT(res.stdout_output, HasSubstr(" v_1d = 10, 20, 30 ;"));
  EXPECT_THAT(res.stdout_output, HasSubstr(R"( v_str = "one", "two" ;)"));
  EXPECT_THAT(res.stdout_output, HasSubstr("1.5,\n  2.5 ;"));
  EXPECT_THAT(res.stdout_output, HasSubstr(" v_2d =\n  1, 2, 3,\n  4, 5, 6 ;"));
  EXPECT_THAT(res.stdout_output, HasSubstr("{1, 2, 3,"));
  EXPECT_THAT(res.stdout_output, HasSubstr("10, 11, 12} ;"));
  EXPECT_THAT(res.stdout_output, HasSubstr(R"("\b\f\n",)"
                                           "\n    "
                                           R"("\r\t\v\\\'\"A\001",)"));
  EXPECT_THAT(res.stdout_output, HasSubstr(R"("hello",)"
                                           "\n  "
                                           R"("" ;)"));
  EXPECT_THAT(res.stdout_output, HasSubstr(R"( v_char_fmt = "xyz" ;)"));
  EXPECT_THAT(res.stdout_output, HasSubstr(" v_char_fmt = x, y, z ;"));
  EXPECT_THAT(res.stdout_output, HasSubstr("// v_2d_col1(0,0)"));
  EXPECT_THAT(res.stdout_output, HasSubstr("// v_2d(0, 0-2)"));
  EXPECT_THAT(res.stdout_output, HasSubstr("// v_2d_col1(1,1)"));
  EXPECT_THAT(res.stdout_output, HasSubstr("// v_2d(1-3 ,1)"));
  EXPECT_THAT(res.stdout_output, HasSubstr("30;  // v_1d(2)"));
  EXPECT_THAT(res.stdout_output, HasSubstr("// v_unlim(0,0,0)"));
  EXPECT_THAT(res.stdout_output, HasSubstr("6},  // v_unlim(0,1,2)"));
  EXPECT_THAT(res.stdout_output, HasSubstr("12};  // v_unlim(1,1,2)"));
  EXPECT_THAT(res.stdout_output, HasSubstr("// v_unlim(1,1,1)"));
  EXPECT_THAT(res.stdout_output, HasSubstr("12};  // v_unlim(3,2,2)"));
}

TEST(VardataTest, VardataxOutputsNcmlScalarMultiDimCarryAndChars) {
  constexpr std::string_view kArgs[] = {"vardatax_ncml"};
  const CliResult res = RunCliInForkedChild(
      [](int /*argc*/, char** /*argv*/) -> int {
        ScopedMemio memio;
        {
          ScopedNcFile nc;
          if (nc_create_mem("ncml.nc", NC_NETCDF4, 8192, nc.ptr()) !=
              NC_NOERR) {
            return 1;
          }
          int d2 = -1;
          int d3 = -1;
          int d20 = -1;
          if (nc_def_dim(nc, "d2", 2, &d2) != NC_NOERR ||
              nc_def_dim(nc, "d3", 3, &d3) != NC_NOERR ||
              nc_def_dim(nc, "d20", 20, &d20) != NC_NOERR) {
            return 2;
          }

          int x_scalar = -1;
          int x_1d = -1;
          int x_3d = -1;
          int x_text = -1;
          int x_char_fmt = -1;
          const int dims_3d[3] = {d2, d2, d3};
          const int dims_text[2] = {d3, d20};

          if (nc_def_var(nc, "x_scalar", NC_INT, 0, nullptr, &x_scalar) !=
                  NC_NOERR ||
              nc_def_var(nc, "x_1d", NC_INT, 1, &d3, &x_1d) != NC_NOERR ||
              nc_def_var(nc, "x_3d", NC_INT, 3, dims_3d, &x_3d) != NC_NOERR ||
              nc_def_var(nc, "x_text", NC_CHAR, 2, dims_text, &x_text) !=
                  NC_NOERR ||
              nc_def_var(nc, "x_char_fmt", NC_CHAR, 1, &d3, &x_char_fmt) !=
                  NC_NOERR) {
            return 3;
          }
          if (nc_enddef(nc) != NC_NOERR) {
            return 4;
          }

          constexpr int kScalar = 99;
          constexpr int kOneD[3] = {5, 6, 7};
          constexpr int kCube[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
          constexpr char kText[3][20] = {
              "\b\f\n\r\t\v\\\'\"A\x01",
              "ok",
              "",
          };
          constexpr char kChars[3] = {'p', 'q', 'r'};

          if (nc_put_var_int(nc, x_scalar, &kScalar) != NC_NOERR ||
              nc_put_var_int(nc, x_1d, kOneD) != NC_NOERR ||
              nc_put_var_int(nc, x_3d, kCube) != NC_NOERR ||
              nc_put_var_text(nc, x_text, &kText[0][0]) != NC_NOERR ||
              nc_put_var_text(nc, x_char_fmt, kChars) != NC_NOERR) {
            return 5;
          }
          if (nc_close_memio(nc.release(), memio.get()) != NC_NOERR) {
            return 6;
          }
        }

        ScopedNcFile nc;
        if (nc_open_mem("ncml.nc", NC_NOWRITE, memio.size(),
                        memio.get()->memory, nc.ptr()) != NC_NOERR) {
          return 7;
        }
        InitDumpRuntime(nc);

        auto dump_x = [&](const char* name, const char* fmt) {
          int vid = -1;
          if (nc_inq_varid(nc, name, &vid) != NC_NOERR) {
            return false;
          }
          ncvar_t var{};
          int dimids[NC_MAX_VAR_DIMS]{};
          if (nc_inq_var(nc, vid, var.name, &var.type, &var.ndims, dimids,
                         &var.natts) != NC_NOERR) {
            return false;
          }
          size_t vdims[NC_MAX_VAR_DIMS]{};
          for (int i = 0; i < var.ndims; ++i) {
            if (nc_inq_dimlen(nc, dimids[i], &vdims[i]) != NC_NOERR) {
              return false;
            }
          }
          var.dims = var.ndims > 0 ? dimids : nullptr;
          var.tinfo = get_typeinfo(var.type);
          var.locid = nc;
          var.fmt = fmt != nullptr ? fmt : get_fmt(nc, vid, var.type);
          set_tostring_func(&var);
          return vardatax(&var, vdims, nc, vid) == 0;
        };

        if (!dump_x("x_scalar", "%d") || !dump_x("x_1d", "%d") ||
            !dump_x("x_3d", "%d") || !dump_x("x_text", "%s") ||
            !dump_x("x_char_fmt", "") || !dump_x("x_char_fmt", "%c")) {
          return 8;
        }
        return 0;
      },
      kArgs);

  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
  EXPECT_THAT(res.stdout_output, HasSubstr("<values>\n     99\n    </values>"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("<values>\n     5 6 7\n    </values>"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("<values>\n     1 2 3\n     4 5 6\n     7 8 9\n     "
                        "10 11 12\n    </values>"));
  EXPECT_THAT(res.stdout_output, HasSubstr(R"("\b\f\n",)"
                                           "\n    "
                                           R"("\r\t\v\\\'\"A\001")"));
  EXPECT_THAT(res.stdout_output, HasSubstr(R"("ok")"
                                           "\n     "
                                           R"("")"));
  EXPECT_THAT(res.stdout_output, HasSubstr(R"("pqr")"
                                           "\n    </values>"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("<values>\n     p q r\n    </values>"));
}

}  // namespace
}  // namespace netcdf
