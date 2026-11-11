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

// Unit tests for //third_party/netcdf/ncgen/genc.c (C code generator).
//
// Exercises type-name helper tables, format/cmode branches, nested netCDF-4
// groups and user-defined types, special variable attributes (chunking,
// filters, deflate/shuffle, endianness, fill, quantization), scalar/array
// data emission, global/variable attributes across atomic and user-defined
// types, and classic-mode attribute error diagnostics in forked child runners.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncgen/genc.c

#include <setjmp.h>

#include <cstdio>
#include <functional>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/isolated_child.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/google/util/ncgen_main_shim.h"
// clang-format on

extern "C" {
extern FILE* ncgin;
}  // extern "C"

namespace netcdf {
namespace {

using ::testing::HasSubstr;

struct GencOptions {
  int k_flag = NC_FORMAT_NETCDF4;
  int nofill_flag = 0;
  int header_only = 0;
  int ncloglevel = -1;
  int wholevarsize = 1024;
  std::string main_name = "main";
  // Optional hook invoked after processsemantics() and before genc_netcdf().
  std::function<void()> post_semantics_hook;
};

void ApplyFormatSettings(int requested_k_flag) {
  int resolved_k = requested_k_flag;
  if (resolved_k == 0) resolved_k = globalspecials._Format;
  if (cdf5_flag && !enhanced_flag && resolved_k == 0) {
    resolved_k = NC_FORMAT_64BIT_DATA;
  }
  if (enhanced_flag && resolved_k == 0) resolved_k = NC_FORMAT_NETCDF4;
  if (specials_flag > 0 && resolved_k == 0) resolved_k = NC_FORMAT_NETCDF4;
  if (resolved_k == 0) resolved_k = NC_FORMAT_CLASSIC;
  k_flag = resolved_k;

  switch (k_flag) {
    case NC_FORMAT_64BIT_DATA:
    case NC_FORMAT_CLASSIC:
    case NC_FORMAT_64BIT_OFFSET:
    case NC_FORMAT_NETCDF4_CLASSIC:
      usingclassic = 1;
      break;
    case NC_FORMAT_NETCDF4:
    default:
      usingclassic = 0;
      break;
  }

  switch (k_flag) {
    case NC_FORMAT_CLASSIC:
      cmode_modifier = 0;
      break;
    case NC_FORMAT_64BIT_OFFSET:
      cmode_modifier = NC_64BIT_OFFSET;
      break;
    case NC_FORMAT_NETCDF4:
      cmode_modifier = NC_NETCDF4;
      break;
    case NC_FORMAT_NETCDF4_CLASSIC:
      cmode_modifier = NC_NETCDF4 | NC_CLASSIC_MODEL;
      break;
    case NC_FORMAT_64BIT_DATA:
      cmode_modifier = NC_CDF5;
      break;
    default:
      cmode_modifier = 0;
      break;
  }
}

CliResult RunGenc(std::string_view cdl, const GencOptions& opts = {}) {
  constexpr std::string_view kArgv[] = {"genc_test"};
  return RunCliInForkedChild(
      [&opts](int /*argc*/, char** /*argv*/) -> int {
        std::string main_name_buf = opts.main_name;
        ncgen_shim_reset_globals();
        specialconstants = 0;
        debug = 0;
        ncgdebug = 0;
        l_flag = L_C;
        k_flag = opts.k_flag;
        nofill_flag = opts.nofill_flag;
        header_only = opts.header_only;
        ncloglevel = opts.ncloglevel;
        wholevarsize = opts.wholevarsize;
        mainname = main_name_buf.data();

        sigjmp_buf jmp_env;
        int finalize_code = -1;
        if (sigsetjmp(jmp_env, 1) != 0) {
          ncgen_shim_set_finalize_jmp(nullptr, nullptr);
          return finalize_code != 0 ? finalize_code : 101;
        }
        ncgen_shim_set_finalize_jmp(&jmp_env, &finalize_code);

        parse_init();
        ncgin = stdin;
        if (ncgparse() != 0) {
          ncgen_shim_set_finalize_jmp(nullptr, nullptr);
          return 200;
        }

        ApplyFormatSettings(opts.k_flag);
        processsemantics();
        if (opts.post_semantics_hook) {
          opts.post_semantics_hook();
        }
        genc_netcdf();
        genc_close();
        const int errs = error_count;
        cleanup();
        ncgen_shim_set_finalize_jmp(nullptr, nullptr);
        return errs;
      },
      kArgv, cdl);
}

TEST(GencTest, TypeNameLookupHelpersCoverAllAtomicTypesAndInvalidSuffix) {
  struct TypeNames {
    nc_type type;
    const char* nc_macro;
    const char* c_type;
    const char* suffix;
  };
  constexpr TypeNames kCases[] = {
      {NC_CHAR, "NC_CHAR", "char", "text"},
      {NC_BYTE, "NC_BYTE", "signed char", "schar"},
      {NC_SHORT, "NC_SHORT", "short", "short"},
      {NC_INT, "NC_INT", "int", "int"},
      {NC_FLOAT, "NC_FLOAT", "float", "float"},
      {NC_DOUBLE, "NC_DOUBLE", "double", "double"},
      {NC_UBYTE, "NC_UBYTE", "unsigned char", "ubyte"},
      {NC_USHORT, "NC_USHORT", "unsigned short", "ushort"},
      {NC_UINT, "NC_UINT", "unsigned int", "uint"},
      {NC_INT64, "NC_INT64", "signed long long", "longlong"},
      {NC_UINT64, "NC_UINT64", "unsigned long long", "ulonglong"},
      {NC_STRING, "NC_STRING", "char*", "string"},
  };

  for (const TypeNames& tc : kCases) {
    SCOPED_TRACE(tc.nc_macro);
    EXPECT_STREQ(tc.nc_macro, nctype(tc.type));
    EXPECT_STREQ(tc.c_type, ncctype(tc.type));
    EXPECT_STREQ(tc.suffix, ncstype(tc.type));
  }

  // ncstype logs via derror() and returns NULL for unknown type codes.
  constexpr std::string_view kArgs[] = {"genc_test"};
  const CliResult err_res = RunCliInForkedChild(
      [](int /*argc*/, char** /*argv*/) -> int {
        ncgen_shim_reset_globals();
        const char* result = ncstype(999);
        if (result != nullptr) return 2;
        return error_count == 1 ? 0 : 3;
      },
      kArgs);
  ASSERT_TRUE(err_res.ok()) << err_res.harness_error;
  EXPECT_EQ(0, err_res.exit_code);
  EXPECT_THAT(err_res.stderr_output, HasSubstr("ncstype: bad type code: 999"));
}

TEST(GencTest, GeneratesClassicOffsetNetcdf4ClassicAndCdf5Formats) {
  // Empty classic schema with nofill, header_only, log level, and empty attr.
  constexpr std::string_view kEmptyClassicCdl =
      "netcdf empty_classic {\n"
      "  :empty = \"\";\n"
      "}\n";
  {
    GencOptions opts;
    opts.k_flag = NC_FORMAT_CLASSIC;
    opts.nofill_flag = 1;
    opts.header_only = 1;
    opts.ncloglevel = 2;
    const CliResult res = RunGenc(kEmptyClassicCdl, opts);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_THAT(res.stdout_output, HasSubstr("nc_set_log_level(2);"));
    EXPECT_THAT(
        res.stdout_output,
        HasSubstr("nc_create(\"empty_classic.nc\", NC_CLOBBER, &ncid)"));
    EXPECT_THAT(res.stdout_output,
                HasSubstr("/* don't initialize variables with fill values */\n"
                          "    CHECK_ERR(stat);"));
    EXPECT_THAT(
        res.stdout_output,
        HasSubstr("nc_put_att_text(ncid, NC_GLOBAL, \"empty\", 1, \"\");"));
    EXPECT_THAT(res.stdout_output, HasSubstr("nc_close(ncid);"));
    EXPECT_THAT(res.stdout_output,
                ::testing::Not(HasSubstr("assign variable data")));
  }

  // Empty schema with header_only = 0 exercises nvars == 0 inside data step.
  {
    GencOptions opts;
    opts.k_flag = NC_FORMAT_CLASSIC;
    opts.header_only = 0;
    const CliResult res = RunGenc(kEmptyClassicCdl, opts);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_THAT(res.stdout_output,
                ::testing::Not(HasSubstr("assign variable data")));
  }

  // Classic schema with dim, var, and data for 64-bit offset & netcdf4-classic.
  constexpr std::string_view kClassicDataCdl =
      "netcdf classic_data {\n"
      "dimensions:\n"
      "  d = 2;\n"
      "variables:\n"
      "  int v(d);\n"
      "  v:units = \"m\";\n"
      "data:\n"
      "  v = 10, 20;\n"
      "}\n";

  {
    GencOptions opts;
    opts.k_flag = NC_FORMAT_64BIT_OFFSET;
    const CliResult res = RunGenc(kClassicDataCdl, opts);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_THAT(res.stdout_output, HasSubstr("NC_CLOBBER|NC_64BIT_OFFSET"));
    EXPECT_THAT(res.stdout_output, HasSubstr("nc_put_vara(ncid, v_id"));
  }

  {
    GencOptions opts;
    opts.k_flag = NC_FORMAT_NETCDF4_CLASSIC;
    const CliResult res = RunGenc(kClassicDataCdl, opts);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
    EXPECT_THAT(res.stdout_output,
                HasSubstr("NC_CLOBBER|NC_NETCDF4|NC_CLASSIC_MODEL"));
  }

  // CDF-5 sets usingclassic = 1 and k_flag = NC_FORMAT_64BIT_DATA (5 > 2):
  // genc.c reports unknown cmode modifier, while unsigned/64-bit attributes
  // bypass the k_flag <= 2 restriction and emit nc_put_att_* calls.
  constexpr std::string_view kCdf5Cdl =
      "netcdf cdf5_test {\n"
      "variables:\n"
      "  ubyte ub;\n"
      "  ubyte ub:a_ub = 10ub;\n"
      "  ushort ub:a_us = 20us;\n"
      "  uint ub:a_ui = 30u;\n"
      "  int64 ub:a_i64 = -40ll;\n"
      "  uint64 ub:a_u64 = 50ull;\n"
      "  ubyte :g_ub = 1ub;\n"
      "}\n";
  {
    GencOptions opts;
    opts.k_flag = NC_FORMAT_64BIT_DATA;
    const CliResult res = RunGenc(kCdf5Cdl, opts);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stderr_output, HasSubstr("unknown cmode modifier"));
    EXPECT_THAT(res.stdout_output, HasSubstr("nc_put_att_ubyte(ncid, ub_id"));
    EXPECT_THAT(res.stdout_output, HasSubstr("nc_put_att_ushort(ncid, ub_id"));
    EXPECT_THAT(res.stdout_output, HasSubstr("nc_put_att_uint(ncid, ub_id"));
    EXPECT_THAT(res.stdout_output,
                HasSubstr("nc_put_att_longlong(ncid, ub_id"));
    EXPECT_THAT(res.stdout_output,
                HasSubstr("nc_put_att_ulonglong(ncid, ub_id"));
    EXPECT_THAT(res.stdout_output,
                HasSubstr("nc_put_att_ubyte(ncid, NC_GLOBAL"));
  }
}

TEST(GencTest, GeneratesScalarOnlyVariablesNullDataAndSpecialConstants) {
  constexpr std::string_view kScalarCdl =
      "netcdf 4ds {\n"
      "variables:\n"
      "  float f_nan;\n"
      "  float f_inf;\n"
      "  float f_ninf;\n"
      "  double d_nan;\n"
      "  double d_inf;\n"
      "  double d_ninf;\n"
      "  char ch;\n"
      "  int empty_var;\n"
      "data:\n"
      "  f_nan = nanf;\n"
      "  f_inf = Infinityf;\n"
      "  f_ninf = -Infinityf;\n"
      "  d_nan = nan;\n"
      "  d_inf = Infinity;\n"
      "  d_ninf = -Infinity;\n"
      "  ch = 'Z';\n"
      "}\n";

  GencOptions opts;
  opts.k_flag = NC_FORMAT_NETCDF4;
  opts.main_name = "generate_sample";
  const CliResult res = RunGenc(kScalarCdl, opts);
  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
  EXPECT_THAT(res.stdout_output, HasSubstr("#define nanf (0.0f/0.0f)"));
  EXPECT_THAT(res.stdout_output, HasSubstr("#define inf  (1.0/0.0)"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("generate_sample() {/* create 4ds.nc */"));
  EXPECT_THAT(res.stdout_output, HasSubstr("int DIGIT_4_ds_grp;"));
  EXPECT_THAT(res.stdout_output, HasSubstr("DIGIT_4_ds_grp = ncid;"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("static float f_inf_data[1] = {((float)inf)};"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("static float f_ninf_data[1] = {((float)-inf)};"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("static double d_nan_data[1] = {nan};"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("static double d_inf_data[1] = {((double)inf)};"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("static double d_ninf_data[1] = {((double)-inf)};"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_put_var1(DIGIT_4_ds_grp, f_nan_id"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_put_var1(DIGIT_4_ds_grp, f_inf_id"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_put_var1(DIGIT_4_ds_grp, d_inf_id"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_put_var1(DIGIT_4_ds_grp, ch_id"));
  EXPECT_THAT(res.stdout_output, ::testing::Not(HasSubstr("empty_var_data")));
  EXPECT_THAT(res.stdout_output, ::testing::Not(HasSubstr("variable shapes")));
}

TEST(GencTest, GeneratesNetcdf4NestedGroupsAndFormatAttribute) {
  constexpr std::string_view kGroupsCdl =
      "netcdf nested_groups {\n"
      ":_Format = \"netCDF-4\";\n"
      "dimensions:\n"
      "  gx = 2;\n"
      "variables:\n"
      "  int root_v(gx);\n"
      "data:\n"
      "  root_v = 1, 2;\n"
      "group: sub {\n"
      "  dimensions:\n"
      "    sx = 3;\n"
      "  variables:\n"
      "    float sub_v(sx);\n"
      "  data:\n"
      "    sub_v = 1.0f, 2.0f, 3.0f;\n"
      "  group: leaf {\n"
      "    variables:\n"
      "      double leaf_v;\n"
      "    data:\n"
      "      leaf_v = 4.5;\n"
      "  }\n"
      "}\n"
      "}\n";

  GencOptions opts;
  opts.k_flag =
      0;  // Resolved from :_Format attribute and format_attribute = 1.
  const CliResult res = RunGenc(kGroupsCdl, opts);
  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
  EXPECT_THAT(res.stdout_output, HasSubstr("NC_CLOBBER|NC_NETCDF4"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_def_grp(nested_groups_grp, \"sub\", &sub_grp)"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_def_grp(sub_grp, \"leaf\", &sub_SLASH_leaf_grp)"));
  EXPECT_THAT(res.stdout_output, HasSubstr("nc_def_dim(sub_grp, \"sx\""));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_def_var(sub_SLASH_leaf_grp, \"leaf_v\""));
}

TEST(GencTest, GeneratesSpecialVariableAttributesChunkingFiltersAndQuantize) {
  constexpr std::string_view kSpecialsCdl =
      "netcdf specials_test {\n"
      "dimensions:\n"
      "  x = 4;\n"
      "  y = 6;\n"
      "variables:\n"
      "  float v_contig(x);\n"
      "    v_contig:_Storage = \"contiguous\";\n"
      "  float v_compact(x);\n"
      "    v_compact:_Storage = \"compact\";\n"
      "  float v_chunk_only(x);\n"
      "    v_chunk_only:_Storage = \"chunked\";\n"
      "  float v_nullchunk(x);\n"
      "    v_nullchunk:_Storage = \"chunked\";\n"
      "  float v_filtered(x, y);\n"
      "    v_filtered:_Storage = \"chunked\";\n"
      "    v_filtered:_ChunkSizes = 2, 3;\n"
      "    v_filtered:_Fletcher32 = \"true\";\n"
      "    v_filtered:_Shuffle = \"true\";\n"
      "    v_filtered:_DeflateLevel = 5;\n"
      "    v_filtered:_Endianness = \"little\";\n"
      "    v_filtered:_NoFill = \"true\";\n"
      "    v_filtered:_Filter = \"307|32001,1,2\";\n"
      "    v_filtered:_QuantizeBitGroomNumberOfSignificantDigits = 3;\n"
      "  double v_alt(x);\n"
      "    v_alt:_Fletcher32 = \"false\";\n"
      "    v_alt:_Shuffle = \"false\";\n"
      "    v_alt:_DeflateLevel = -1;\n"
      "    v_alt:_Endianness = \"big\";\n"
      "    v_alt:_NoFill = \"false\";\n"
      "    v_alt:_QuantizeGranularBitRoundNumberOfSignificantDigits = 4;\n"
      "}\n";

  GencOptions opts;
  opts.k_flag = NC_FORMAT_NETCDF4;
  opts.post_semantics_hook = []() {
    // Exercise special->nchunks == 0 with _CHUNKSIZES_FLAG set (genc.c:103-105)
    // and the NC_QUANTIZE_BITGROOM case (genc.c:549), since the CDL lexer only
    // recognizes _QuantizeGranularBitRoundNumberOfSignificantDigits as a
    // special token.
    for (size_t i = 0; i < listlength(vardefs); ++i) {
      Symbol* var = static_cast<Symbol*>(listget(vardefs, i));
      if (std::string_view(var->name) == "v_nullchunk") {
        var->var.special.flags |= _CHUNKSIZES_FLAG | _QUANTIZEBG_FLAG;
        var->var.special.nchunks = 0;
        var->var.special._ChunkSizes = nullptr;
        var->var.special._Quantizer = NC_QUANTIZE_BITGROOM;
        var->var.special._NSD = 3;
      }
    }
    // Exercise groupncid(NULL) fallback (genc.c:694) on a dimension whose fqn
    // was already materialized by processsemantics().
    if (listlength(dimdefs) > 0) {
      Symbol* d0 = static_cast<Symbol*>(listget(dimdefs, 0));
      d0->container = nullptr;
    }
  };

  const CliResult res = RunGenc(kSpecialsCdl, opts);
  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
  EXPECT_THAT(
      res.stdout_output,
      HasSubstr("nc_def_dim(specials_test_grp, \"x\", x_len, &x_dim);"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("static size_t v_filtered_chunksizes[2] = {2, 3} ;"));
  EXPECT_THAT(
      res.stdout_output,
      HasSubstr(
          "static unsigned int v_filtered_1_filterparams[2] = {1U, 2U} ;"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_def_var_chunking(specials_test_grp, v_contig_id, "
                        "NC_CONTIGUOUS, NULL);"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_def_var_chunking(specials_test_grp, v_compact_id, "
                        "NC_COMPACT, NULL);"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_def_var_chunking(specials_test_grp, "
                        "v_chunk_only_id, NC_CHUNKED, NULL);"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_def_var_chunking(specials_test_grp, v_filtered_id, "
                        "NC_CHUNKED, v_filtered_chunksizes);"));
  EXPECT_THAT(
      res.stdout_output,
      HasSubstr("nc_def_var_fletcher32(specials_test_grp, v_filtered_id, 1);"));
  EXPECT_THAT(
      res.stdout_output,
      HasSubstr("nc_def_var_fletcher32(specials_test_grp, v_alt_id, 0);"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_def_var_deflate(specials_test_grp, v_filtered_id, "
                        "NC_SHUFFLE, 1, 5);"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_def_var_deflate(specials_test_grp, v_alt_id, "
                        "NC_NOSHUFFLE, 0, 0);"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_def_var_endian(specials_test_grp, v_filtered_id, "
                        "NC_ENDIAN_LITTLE);"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_def_var_endian(specials_test_grp, v_alt_id, "
                        "NC_ENDIAN_BIG);"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_def_var_fill(specials_test_grp, v_filtered_id, "
                        "NC_NOFILL, NULL);"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_def_var_fill(specials_test_grp, v_alt_id, "
                        "NC_FILL, NULL);"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_def_var_filter(specials_test_grp, v_filtered_id, "
                        "307, 0, NULL);"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_def_var_filter(specials_test_grp, v_filtered_id, "
                        "32001, 2, v_filtered_1_filterparams);"));
  EXPECT_THAT(
      res.stdout_output,
      HasSubstr("nc_def_var_quantize(specials_test_grp, v_nullchunk_id, "
                "NC_NOQUANTIZE, 3);"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_def_var_quantize(specials_test_grp, v_alt_id, "
                        "NC_NOQUANTIZE, 4);"));
  EXPECT_THAT(
      res.stdout_output,
      HasSubstr("nc_put_att_int(specials_test_grp, v_filtered_id, "
                "\"_QuantizeBitGroomNumberOfSignificantDigits\", NC_INT, 1, "
                "_QuantizeBitGroomNumberOfSignificantDigits_att);"));
}

TEST(GencTest, GeneratesAllUserDefinedTypesAndCompoundArrayFields) {
  constexpr std::string_view kUdtsCdl =
      "netcdf udts {\n"
      "types:\n"
      "  opaque(4) opq_t;\n"
      "  ubyte enum flags_t { F_NONE = 0, F_READ = 1, F_WRITE = 2 };\n"
      "  int enum sign_t { S_NEG = -5, S_POS = 7 };\n"
      "  int(*) ivlen_t;\n"
      "  compound cmp_t {\n"
      "    int id;\n"
      "    float vec(2);\n"
      "    double mat(2, 3);\n"
      "  };\n"
      "  compound outer_t {\n"
      "    ivlen_t items;\n"
      "    opq_t tag;\n"
      "  };\n"
      "dimensions:\n"
      "  n = 2;\n"
      "variables:\n"
      "  opq_t v_opq;\n"
      "  flags_t v_flag;\n"
      "  sign_t v_sign;\n"
      "  ivlen_t v_vlen;\n"
      "  ivlen_t v_vlen_arr(n);\n"
      "  cmp_t v_cmp;\n"
      "  outer_t v_outer;\n"
      "data:\n"
      "  v_opq = 0x01020304;\n"
      "  v_flag = F_READ;\n"
      "  v_sign = S_NEG;\n"
      "  v_vlen = {10, 20};\n"
      "  v_vlen_arr = {1}, {2, 3};\n"
      "  v_cmp = {42, {1.5f, 2.5f}, {1.0, 2.0, 3.0, 4.0, 5.0, 6.0}};\n"
      "  v_outer = {{7, 8}, 0xaabbccdd};\n"
      "}\n";

  GencOptions opts;
  opts.k_flag = NC_FORMAT_NETCDF4;
  opts.post_semantics_hook = []() {
    // Exercise NC_PRIM and NC_ARRAY no-op branches in definectype/genc_netcdf.
    listpush(typdefs, basetypefor(NC_INT));
    Symbol* synth_arr = install("synth_arr");
    synth_arr->objectclass = NC_TYPE;
    synth_arr->subclass = NC_ARRAY;
    synth_arr->container = rootgroup;
    synth_arr->prefix = listnew();
    topfqn(synth_arr);
    listpush(typdefs, synth_arr);
  };

  const CliResult res = RunGenc(kUdtsCdl, opts);
  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
  EXPECT_THAT(res.stdout_output, HasSubstr("typedef unsigned char opq_t[4];"));
  EXPECT_THAT(res.stdout_output, HasSubstr("#define flags_t_PERIOD_F_NONE"));
  EXPECT_THAT(res.stdout_output, HasSubstr("typedef unsigned char flags_t;"));
  EXPECT_THAT(res.stdout_output, HasSubstr("typedef int sign_t;"));
  EXPECT_THAT(res.stdout_output, HasSubstr("typedef nc_vlen_t ivlen_t;"));
  EXPECT_THAT(res.stdout_output, HasSubstr("typedef struct cmp_t {"));
  EXPECT_THAT(res.stdout_output, HasSubstr("float cmp_t_PERIOD_vec[2];"));
  EXPECT_THAT(res.stdout_output, HasSubstr("double cmp_t_PERIOD_mat[2][3];"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_def_opaque(udts_grp, 4, \"opq_t\""));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_def_enum(udts_grp, NC_UBYTE, \"flags_t\""));
  EXPECT_THAT(
      res.stdout_output,
      HasSubstr("nc_insert_enum(udts_grp, flags_t_typ, \"F_NONE\", &econst);"));
  EXPECT_THAT(
      res.stdout_output,
      HasSubstr("nc_def_vlen(udts_grp, \"ivlen_t\", NC_INT, &ivlen_t_typ)"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_def_compound(udts_grp, sizeof(cmp_t)"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("static int cmp_t_PERIOD_mat_dims[2] = {"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_insert_compound(udts_grp, cmp_t_typ, \"id\""));
  EXPECT_THAT(
      res.stdout_output,
      HasSubstr("nc_insert_array_compound(udts_grp, cmp_t_typ, \"mat\""));
  EXPECT_THAT(res.stdout_output, HasSubstr("static const int vlen_"));
}

TEST(GencTest,
     GeneratesArrayVariablesCharArraysUnlimitedDimsAndSlicedWholeVar) {
  constexpr std::string_view kArraysCdl =
      "netcdf arrays {\n"
      "dimensions:\n"
      "  rec = UNLIMITED;\n"
      "  rows = 2;\n"
      "  cols = 3;\n"
      "  strlen = 5;\n"
      "variables:\n"
      "  int s_int;\n"
      "  string s_str;\n"
      "  char c1d(strlen);\n"
      "  char c2d(rec, strlen);\n"
      "  int small_fixed(rows);\n"
      "  int big_fixed(rows, cols);\n"
      "  int big_unlim(rec, cols);\n"
      "  string str_arr(rec);\n"
      "data:\n"
      "  s_int = 99;\n"
      "  s_str = \"scalar_text\";\n"
      "  c1d = \"hello\";\n"
      "  c2d = \"alpha\", \"beta\";\n"
      "  small_fixed = 7, 8;\n"
      "  big_fixed = 1, 2, 3, 4, 5, 6;\n"
      "  big_unlim = 10, 20, 30, 40, 50, 60;\n"
      "  str_arr = \"first\", NIL;\n"
      "}\n";

  GencOptions opts;
  opts.k_flag = NC_FORMAT_NETCDF4;
  opts.wholevarsize =
      2;  // Forces big_fixed (6 > 2) to emit multi-slice writes.
  const CliResult res = RunGenc(kArraysCdl, opts);
  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
  EXPECT_THAT(res.stdout_output, HasSubstr("size_t rec_len = NC_UNLIMITED;"));
  EXPECT_THAT(res.stdout_output, HasSubstr("size_t rows_len = 2;"));
  EXPECT_THAT(res.stdout_output, HasSubstr("#   define RANK_s_int 0"));
  EXPECT_THAT(res.stdout_output, HasSubstr("#   define RANK_big_fixed 2"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_def_var(arrays_grp, \"s_int\", NC_INT, RANK_s_int, "
                        "0, &s_int_id)"));
  EXPECT_THAT(res.stdout_output, HasSubstr("char* c1d_data = \"hello\" ;"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("char* c2d_data = \"alphabeta\\000\" ;"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("static char* s_str_data[1] = {\"scalar_text\"};"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("char* str_arr_data[2] = {\"first\", NULL} ;"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("size_t big_fixed_startset[2] = {0, 0} ;"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("size_t big_fixed_startset[2] = {1, 0} ;"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("size_t big_fixed_countset[2] = {1, 3};"));
}

TEST(GencTest, GeneratesGlobalAndVariableAttributesAcrossAllTypesAndVlenAttrs) {
  constexpr std::string_view kAttrsCdl =
      "netcdf attrs_all {\n"
      "types:\n"
      "  opaque(4) opq_t;\n"
      "  int enum color_t { RED = 1, GREEN = 2 };\n"
      "  int(*) vlen_t;\n"
      "  compound pair_t { int a; double b; };\n"
      "dimensions:\n"
      "  d = 2;\n"
      "variables:\n"
      "  int v(d);\n"
      "  v:" NC_FillValue
      " = -999;\n"
      "  v:a_char = \"abc\";\n"
      "  v:a_empty = \"\";\n"
      "  byte v:a_byte = 1b, 2b;\n"
      "  short v:a_short = 10s, 20s;\n"
      "  int v:a_int = 100, 200;\n"
      "  float v:a_float = 1.5f, 2.5f;\n"
      "  double v:a_double = 3.5, 4.5;\n"
      "  ubyte v:a_ubyte = 5ub, 6ub;\n"
      "  ushort v:a_ushort = 7us, 8us;\n"
      "  uint v:a_uint = 9u, 10u;\n"
      "  int64 v:a_int64 = -11ll, 12ll;\n"
      "  uint64 v:a_uint64 = 13ull, 14ull;\n"
      "  string v:a_string = \"one\", \"two\";\n"
      "  opq_t v:a_opq = 0xdeadbeef;\n"
      "  color_t v:a_enum = RED, GREEN;\n"
      "  vlen_t v:a_vlen = {1, 2}, {3};\n"
      "  pair_t v:a_cmp = {1, 2.0};\n"
      "  :g_char = \"global_text\";\n"
      "  byte :g_byte = -1b;\n"
      "  short :g_short = -2s;\n"
      "  int :g_int = -3;\n"
      "  float :g_float = 0.5f;\n"
      "  double :g_double = 1.25;\n"
      "  ubyte :g_ubyte = 200ub;\n"
      "  ushort :g_ushort = 50000us;\n"
      "  uint :g_uint = 3000000u;\n"
      "  int64 :g_int64 = -9000000000ll;\n"
      "  uint64 :g_uint64 = 18000000000ull;\n"
      "  string :g_string = \"g1\", \"g2\";\n"
      "  opq_t :g_opq = 0x01020304;\n"
      "  color_t :g_enum = GREEN;\n"
      "  vlen_t :g_vlen = {7, 8, 9};\n"
      "  pair_t :g_cmp = {3, 4.5};\n"
      "}\n";

  GencOptions opts;
  opts.k_flag = NC_FORMAT_NETCDF4;
  const CliResult res = RunGenc(kAttrsCdl, opts);
  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
  EXPECT_THAT(
      res.stdout_output,
      HasSubstr("nc_put_att_int(attrs_all_grp, v_id, \"" NC_FillValue "\""));
  EXPECT_THAT(
      res.stdout_output,
      HasSubstr(
          "nc_put_att_text(attrs_all_grp, v_id, \"a_char\", 3, \"abc\");"));
  EXPECT_THAT(
      res.stdout_output,
      HasSubstr("nc_put_att_text(attrs_all_grp, v_id, \"a_empty\", 1, \"\");"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_put_att_schar(attrs_all_grp, v_id, \"a_byte\""));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_put_att_short(attrs_all_grp, v_id, \"a_short\""));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_put_att_float(attrs_all_grp, v_id, \"a_float\""));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_put_att_double(attrs_all_grp, v_id, \"a_double\""));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_put_att_ubyte(attrs_all_grp, v_id, \"a_ubyte\""));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_put_att_ushort(attrs_all_grp, v_id, \"a_ushort\""));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_put_att_uint(attrs_all_grp, v_id, \"a_uint\""));
  EXPECT_THAT(
      res.stdout_output,
      HasSubstr("nc_put_att_longlong(attrs_all_grp, v_id, \"a_int64\""));
  EXPECT_THAT(
      res.stdout_output,
      HasSubstr("nc_put_att_ulonglong(attrs_all_grp, v_id, \"a_uint64\""));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_put_att_string(attrs_all_grp, v_id, \"a_string\""));
  EXPECT_THAT(
      res.stdout_output,
      HasSubstr("nc_put_att(attrs_all_grp, v_id, \"a_opq\", opq_t_typ"));
  EXPECT_THAT(
      res.stdout_output,
      HasSubstr("nc_put_att_int(attrs_all_grp, v_id, \"a_enum\", color_t_typ"));
  EXPECT_THAT(
      res.stdout_output,
      HasSubstr("nc_put_att(attrs_all_grp, v_id, \"a_vlen\", vlen_t_typ"));
  EXPECT_THAT(
      res.stdout_output,
      HasSubstr("nc_put_att(attrs_all_grp, v_id, \"a_cmp\", pair_t_typ"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr("nc_put_att_text(attrs_all_grp, NC_GLOBAL, \"g_char\", "
                        "11, \"global_text\");"));
  EXPECT_THAT(
      res.stdout_output,
      HasSubstr("nc_put_att_string(attrs_all_grp, NC_GLOBAL, \"g_string\""));
  EXPECT_THAT(
      res.stdout_output,
      HasSubstr("nc_put_att(attrs_all_grp, NC_GLOBAL, \"g_vlen\", vlen_t_typ"));
  EXPECT_THAT(res.stdout_output, HasSubstr("static const int vlen_"));
}

TEST(GencTest, ClassicModeReportsVerrorForNonClassicAttributes) {
  constexpr std::string_view kNonClassicAttrsCdl =
      "netcdf classic_verr {\n"
      "types:\n"
      "  opaque(2) opq_t;\n"
      "variables:\n"
      "  int v;\n"
      "  ubyte :g_ub = 1ub;\n"
      "  ushort :g_us = 2us;\n"
      "  uint :g_ui = 3u;\n"
      "  int64 :g_i64 = 4ll;\n"
      "  uint64 :g_u64 = 5ull;\n"
      "  string :g_str = \"hello\";\n"
      "  opq_t :g_opq = 0xabcd;\n"
      "}\n";

  GencOptions opts;
  opts.k_flag = NC_FORMAT_NETCDF4;
  opts.post_semantics_hook = []() {
    k_flag = NC_FORMAT_CLASSIC;
    usingclassic = 1;
    cmode_modifier = 0;
  };

  const CliResult res = RunGenc(kNonClassicAttrsCdl, opts);
  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(7, res.exit_code);
  EXPECT_THAT(
      res.stderr_output,
      HasSubstr("netCDF classic: not supported: Non-classic type: NC_UBYTE"));
  EXPECT_THAT(
      res.stderr_output,
      HasSubstr("netCDF classic: not supported: Non-classic type: NC_USHORT"));
  EXPECT_THAT(
      res.stderr_output,
      HasSubstr("netCDF classic: not supported: Non-classic type: NC_UINT"));
  EXPECT_THAT(
      res.stderr_output,
      HasSubstr("netCDF classic: not supported: Non-classic type: NC_INT64"));
  EXPECT_THAT(
      res.stderr_output,
      HasSubstr("netCDF classic: not supported: Non-classic type: NC_UINT64"));
  EXPECT_THAT(
      res.stderr_output,
      HasSubstr("netCDF classic: not supported: Non-classic type: NC_STRING"));
  EXPECT_THAT(
      res.stderr_output,
      HasSubstr("netCDF classic: not supported: Non-classic type: NC_OPAQUE"));
}

}  // namespace
}  // namespace netcdf
