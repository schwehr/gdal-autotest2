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

// Unit tests for //third_party/netcdf/ncgen/ncgeny.c (CDL Bison parser).
//
// Exercises grammar productions, keyword/format gating, AST symbol/special
// attribute construction, function evaluation, stack reallocation, debug trace,
// semantic/syntax error diagnostics, and end-to-end binary output inside
// isolated forked child processes so upstream parser allocations do not
// accumulate in parent sanitizer state.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncgen/ncgeny.c

#include <fcntl.h>
#include <setjmp.h>
#include <unistd.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/netcdf/google/util/isolated_child.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/google/util/ncgen_main_shim.h"
// clang-format on
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"

extern "C" {
struct yy_buffer_state;

extern FILE* ncgin;
extern int ncglex_destroy(void);
extern void ncgrestart(FILE* input_file);
extern struct yy_buffer_state* ncg_create_buffer(FILE* file, int size);
extern void ncg_delete_buffer(struct yy_buffer_state* b);
extern void ncg_switch_to_buffer(struct yy_buffer_state* new_buffer);
extern void ncg_flush_buffer(struct yy_buffer_state* b);
extern void ncgpush_buffer_state(struct yy_buffer_state* new_buffer);
extern void ncgpop_buffer_state(void);
extern struct yy_buffer_state* ncg_scan_buffer(char* base, size_t size);
extern struct yy_buffer_state* ncg_scan_string(const char* yystr);
extern struct yy_buffer_state* ncg_scan_bytes(const char* yybytes, int len);
extern int ncgget_lineno(void);
extern void ncgset_lineno(int line_number);
extern FILE* ncgget_in(void);
extern void ncgset_in(FILE* in_str);
extern FILE* ncgget_out(void);
extern void ncgset_out(FILE* out_str);
extern int ncgget_leng(void);
extern char* ncgget_text(void);
extern int ncgget_debug(void);
extern void ncgset_debug(int bdebug);

extern double double_val;
extern float float_val;
extern long long int64_val;
extern int int32_val;
extern short int16_val;
extern unsigned long long uint64_val;
extern unsigned int uint32_val;
extern unsigned short uint16_val;
extern char char_val;
extern signed char byte_val;
extern unsigned char ubyte_val;
}  // extern "C"

namespace netcdf {
namespace {

using ::testing::HasSubstr;

void ResetParserState(int format_flag = 0, Language lang = L_UNDEFINED) {
  ncgen_shim_set_finalize_jmp(nullptr, nullptr);
  ncgen_shim_reset_globals();
  static char kCdlFileName[] = "cleanroom_test.cdl";
  cdlname = kCdlFileName;
  rootgroup = nullptr;
  debug = 0;
  ncgdebug = 0;
  specialconstants = 0;
  lineno = 1;
  double_val = 0.0;
  float_val = 0.0f;
  int64_val = 0;
  int32_val = 0;
  int16_val = 0;
  uint64_val = 0;
  uint32_val = 0;
  uint16_val = 0;
  char_val = 0;
  byte_val = 0;
  ubyte_val = 0;
  k_flag = format_flag;
  l_flag = lang;
}

bool WriteTextFile(const char* path, std::string_view contents) {
  FILE* fp = std::fopen(path, "wb");
  if (fp == nullptr) {
    ADD_FAILURE() << "fopen(" << path << ", wb) failed";
    return false;
  }
  absl::Cleanup close_fp = [&fp] {
    if (fp != nullptr) {
      std::fclose(fp);
    }
  };
  if (!contents.empty() &&
      std::fwrite(contents.data(), 1, contents.size(), fp) != contents.size()) {
    ADD_FAILURE() << "fwrite(" << path << ") short write";
    return false;
  }
  return true;
}

bool ReadTextFile(const char* path, std::string* out) {
  out->clear();
  FILE* fp = std::fopen(path, "rb");
  if (fp == nullptr) {
    ADD_FAILURE() << "fopen(" << path << ", rb) failed";
    return false;
  }
  absl::Cleanup close_fp = [&fp] {
    if (fp != nullptr) {
      std::fclose(fp);
    }
  };
  char buf[4096];
  while (true) {
    const size_t n = std::fread(buf, 1, sizeof(buf), fp);
    if (n > 0) {
      out->append(buf, n);
    }
    if (n < sizeof(buf)) {
      break;
    }
  }
  return true;
}

int ParseCdl(const char* cdl_path, std::string_view cdl, int format_flag,
             Language lang, const char* stderr_path = nullptr,
             std::string* captured_stderr = nullptr, int dbg = 0,
             int bison_dbg = 0) {
  ResetParserState(format_flag, lang);
  debug = dbg;
  ncgdebug = bison_dbg;
  if (!WriteTextFile(cdl_path, cdl)) {
    return -1;
  }
  FILE* in_fp = std::fopen(cdl_path, "rb");
  if (in_fp == nullptr) {
    ADD_FAILURE() << "fopen(" << cdl_path << ", rb) failed";
    return -1;
  }

  int parse_rc = -1;
  {
    int saved_stderr_fd = -1;
    FILE* err_fp = nullptr;
    if (stderr_path != nullptr) {
      std::fflush(stderr);
      saved_stderr_fd = ::dup(STDERR_FILENO);
      err_fp = std::fopen(stderr_path, "wb");
      if (err_fp != nullptr) {
        ::dup2(::fileno(err_fp), STDERR_FILENO);
      }
    }

    absl::Cleanup cleanup = [&] {
      std::fflush(stderr);
      ncglex_destroy();
      ncgin = nullptr;
      std::fclose(in_fp);
      debug = 0;
      ncgdebug = 0;
      if (saved_stderr_fd >= 0) {
        ::dup2(saved_stderr_fd, STDERR_FILENO);
        ::close(saved_stderr_fd);
      }
      if (err_fp != nullptr) {
        std::fclose(err_fp);
      }
    };

    parse_init();
    ncgin = in_fp;
    parse_rc = ncgparse();
  }

  if (stderr_path != nullptr && captured_stderr != nullptr) {
    static_cast<void>(ReadTextFile(stderr_path, captured_stderr));
  }
  return parse_rc;
}

// Pure POD stack frame for sigsetjmp around processsemantics()/define_netcdf().
bool RunSemanticsAndDefine(char* mutable_out_path, int* jmp_retcode) {
  if (k_flag == 0) {
    k_flag = globalspecials._Format;
  }
  if (cdf5_flag && !enhanced_flag && k_flag == 0) {
    k_flag = NC_FORMAT_64BIT_DATA;
  }
  if (enhanced_flag && k_flag == 0) {
    k_flag = NC_FORMAT_NETCDF4;
  }
  if (specials_flag > 0 && k_flag == 0) {
    k_flag = NC_FORMAT_NETCDF4;
  }
  if (k_flag == 0) {
    k_flag = NC_FORMAT_CLASSIC;
  }
  switch (k_flag) {
    case NC_FORMAT_CLASSIC:
      usingclassic = 1;
      cmode_modifier = 0;
      break;
    case NC_FORMAT_64BIT_OFFSET:
      usingclassic = 1;
      cmode_modifier = NC_64BIT_OFFSET;
      break;
    case NC_FORMAT_NETCDF4_CLASSIC:
      usingclassic = 1;
      cmode_modifier = NC_NETCDF4 | NC_CLASSIC_MODEL;
      break;
    case NC_FORMAT_64BIT_DATA:
      usingclassic = 1;
      cmode_modifier = NC_CDF5;
      break;
    case NC_FORMAT_NETCDF4:
    default:
      usingclassic = 0;
      cmode_modifier = NC_NETCDF4;
      break;
  }

  netcdf_name = mutable_out_path;
  sigjmp_buf env;
  int retcode = -1;
  ncgen_shim_set_finalize_jmp(&env, &retcode);
  if (sigsetjmp(env, 1) != 0) {
    ncgen_shim_set_finalize_jmp(nullptr, nullptr);
    netcdf_name = nullptr;
    if (jmp_retcode != nullptr) {
      *jmp_retcode = retcode;
    }
    return false;
  }

  processsemantics();
  if (error_count == 0) {
    define_netcdf();
  }
  ncgen_shim_set_finalize_jmp(nullptr, nullptr);
  netcdf_name = nullptr;
  return error_count == 0;
}

Symbol* FindSymbolInList(List* list, std::string_view name) {
  if (list == nullptr) {
    return nullptr;
  }
  const size_t n = static_cast<size_t>(listlength(list));
  for (size_t i = 0; i < n; ++i) {
    auto* sym =
        static_cast<Symbol*>(listget(list, static_cast<unsigned long>(i)));
    if (sym != nullptr && sym->name != nullptr && sym->name == name) {
      return sym;
    }
  }
  return nullptr;
}

TEST(NcgenyTest, SpecialNameMapsAllKnownFlagsAndUnknownFallback) {
  struct TagExpectation {
    int tag;
    const char* expected;
  };
  constexpr TagExpectation kCases[] = {
      {_FILLVALUE_FLAG, NC_FillValue},
      {_FORMAT_FLAG, "_Format"},
      {_STORAGE_FLAG, "_Storage"},
      {_CHUNKSIZES_FLAG, "_ChunkSizes"},
      {_FLETCHER32_FLAG, "_Fletcher32"},
      {_DEFLATE_FLAG, "_DeflateLevel"},
      {_SHUFFLE_FLAG, "_Shuffle"},
      {_ENDIAN_FLAG, "_Endianness"},
      {_NOFILL_FLAG, "_NoFill"},
      {_NCPROPS_FLAG, "_NCProperties"},
      {_ISNETCDF4_FLAG, "_IsNetcdf4"},
      {_SUPERBLOCK_FLAG, "_SuperblockVersion"},
      {_FILTER_FLAG, "_Filter"},
      {_CODECS_FLAG, "_Codecs"},
      {_QUANTIZEBG_FLAG, "_QuantizeBitGroomNumberOfSignificantDigits"},
      {_QUANTIZEGBR_FLAG, "_QuantizeGranularBitRoundNumberOfSignificantDigits"},
      {_QUANTIZEBR_FLAG, "_QuantizeBitRoundNumberOfSignificantDigits"},
      {0, "<unknown>"},
      {-1, "<unknown>"},
      {0x20000, "<unknown>"},
  };

  for (const TagExpectation& tc : kCases) {
    SCOPED_TRACE(tc.tag);
    EXPECT_STREQ(tc.expected, specialname(tc.tag));
  }
}

TEST(NcgenyTest, PrimitivesKeywordGatingAndSymbolInstallHelpers) {
  TempFile cdl_file("ncgeny_primitives.cdl");

  ASSERT_TRUE(RunInForkedChild([&]() {
    [&]() {
      // Verify parse_init(), basetypefor(), install(), and installin()
      // directly.
      ResetParserState(NC_FORMAT_NETCDF4, L_BINARY);
      parse_init();
      EXPECT_EQ(nullptr, basetypefor(NC_NAT));
      for (nc_type t = NC_BYTE; t <= NC_STRING; ++t) {
        Symbol* base = basetypefor(t);
        ASSERT_NE(nullptr, base);
        EXPECT_EQ(t, base->typ.typecode);
      }
      Symbol* installed = install("standalone_sym");
      ASSERT_NE(nullptr, installed);
      EXPECT_STREQ("standalone_sym", installed->name);
      Symbol* scoped = installin("explicit_null_group_sym", nullptr);
      ASSERT_NE(nullptr, scoped);
      EXPECT_STREQ("explicit_null_group_sym", scoped->name);
      EXPECT_EQ(nullptr, scoped->container);

      // All NetCDF-4 primitive types under k_flag = NC_FORMAT_NETCDF4.
      constexpr std::string_view kNc4PrimCdl = R"cdl(
netcdf nc4_prims {
dimensions:
  d1 = 2;
variables:
  char v_char(d1);
  byte v_byte;
  short v_short;
  int v_int;
  integer v_integer;
  long v_long;
  float v_float;
  real v_real;
  double v_double;
  ubyte v_ubyte;
  ushort v_ushort;
  uint v_uint;
  ulong v_ulong;
  uinteger v_uint_kw;
  int64 v_int64;
  uint64 v_uint64;
  string v_string;
  int data;
data:
  data = 7;
}
)cdl";
      EXPECT_EQ(0, ParseCdl(cdl_file.c_str(), kNc4PrimCdl, NC_FORMAT_NETCDF4,
                            L_BINARY));
      EXPECT_EQ(0, error_count);
      EXPECT_EQ(1, enhanced_flag);
      ASSERT_NE(nullptr, rootgroup);
      EXPECT_STREQ("nc4_prims", datasetname);

      struct VarTypeExpectation {
        const char* name;
        nc_type typecode;
      };
      constexpr VarTypeExpectation kVars[] = {
          {"v_char", NC_CHAR},     {"v_byte", NC_BYTE},
          {"v_short", NC_SHORT},   {"v_int", NC_INT},
          {"v_integer", NC_INT},   {"v_long", NC_INT},
          {"v_float", NC_FLOAT},   {"v_real", NC_FLOAT},
          {"v_double", NC_DOUBLE}, {"v_ubyte", NC_UBYTE},
          {"v_ushort", NC_USHORT}, {"v_uint", NC_UINT},
          {"v_ulong", NC_UINT},    {"v_uint_kw", NC_UINT},
          {"v_int64", NC_INT64},   {"v_uint64", NC_UINT64},
          {"v_string", NC_STRING}, {"data", NC_INT},
      };
      EXPECT_EQ(sizeof(kVars) / sizeof(kVars[0]),
                static_cast<size_t>(listlength(vardefs)));
      for (const VarTypeExpectation& v : kVars) {
        Symbol* sym = FindSymbolInList(vardefs, v.name);
        ASSERT_NE(nullptr, sym) << v.name;
        ASSERT_NE(nullptr, sym->typ.basetype) << v.name;
        EXPECT_EQ(v.typecode, sym->typ.basetype->typ.typecode) << v.name;
      }

      // In classic mode, enhanced type keywords lex as plain identifiers.
      constexpr std::string_view kClassicIdentCdl = R"cdl(
netcdf classic_idents {
dimensions:
  string = 2;
variables:
  int ubyte(string), ushort, uint, ulong, int64, uint64;
data:
  ubyte = 1, 2;
}
)cdl";
      EXPECT_EQ(0, ParseCdl(cdl_file.c_str(), kClassicIdentCdl,
                            NC_FORMAT_CLASSIC, L_BINARY));
      EXPECT_EQ(0, error_count);
      EXPECT_NE(nullptr, FindSymbolInList(dimdefs, "string"));
      EXPECT_NE(nullptr, FindSymbolInList(vardefs, "ubyte"));
      EXPECT_NE(nullptr, FindSymbolInList(vardefs, "ushort"));
      EXPECT_NE(nullptr, FindSymbolInList(vardefs, "uint"));
      EXPECT_NE(nullptr, FindSymbolInList(vardefs, "ulong"));
      EXPECT_NE(nullptr, FindSymbolInList(vardefs, "int64"));
      EXPECT_NE(nullptr, FindSymbolInList(vardefs, "uint64"));

      // In 64-bit-data (CDF-5) mode, unsigned/64-bit types are keywords while
      // string remains a plain identifier (identcheck).
      constexpr std::string_view kCdf5PrimCdl = R"cdl(
netcdf cdf5_prims {
dimensions:
  string = 2;
variables:
  ubyte v_ub(string);
  ushort v_us;
  uint v_ui;
  ulong v_ul;
  int64 v_i64;
  uint64 v_u64;
data:
  v_ub = 1ub, 2ub;
  v_ui = 42u;
  v_i64 = 5000000000ll;
}
)cdl";
      EXPECT_EQ(0, ParseCdl(cdl_file.c_str(), kCdf5PrimCdl,
                            NC_FORMAT_64BIT_DATA, L_BINARY));
      EXPECT_EQ(0, error_count);
      EXPECT_NE(nullptr, FindSymbolInList(dimdefs, "string"));
      EXPECT_NE(nullptr, FindSymbolInList(vardefs, "v_ub"));
      EXPECT_NE(nullptr, FindSymbolInList(vardefs, "v_us"));
      EXPECT_NE(nullptr, FindSymbolInList(vardefs, "v_ui"));
      EXPECT_NE(nullptr, FindSymbolInList(vardefs, "v_ul"));
      EXPECT_NE(nullptr, FindSymbolInList(vardefs, "v_i64"));
      EXPECT_NE(nullptr, FindSymbolInList(vardefs, "v_u64"));
    }();
    return !::testing::Test::HasFailure();
  }));
}

TEST(NcgenyTest, DebugTraceCommentsEmptySectionsAndStackGrowth) {
  TempFile cdl_file("ncgeny_debug_stack.cdl");
  TempFile err_file("ncgeny_debug_stack.stderr");

  ASSERT_TRUE(RunInForkedChild([&] {
    // Enable both flex lexdebug (debug >= 2) and Bison trace (ncgdebug = 1).
    constexpr std::string_view kDebugTraceCdl = R"cdl(
netcdf my-dataset_1.2 {
/* Multi-line C comment
   inside root group */
types:
// empty types section
dimensions:
variables:
data:
}
)cdl";
    std::string trace_stderr;
    EXPECT_EQ(0, ParseCdl(cdl_file.c_str(), kDebugTraceCdl, NC_FORMAT_NETCDF4,
                          L_BINARY, err_file.c_str(), &trace_stderr,
                          /*dbg=*/2, /*bison_dbg=*/1));
    EXPECT_EQ(0, error_count);
    EXPECT_THAT(trace_stderr, HasSubstr("Starting parse"));
    EXPECT_THAT(trace_stderr, HasSubstr("Reducing stack by rule"));
    EXPECT_THAT(trace_stderr, HasSubstr("Stack now"));
    EXPECT_THAT(trace_stderr, HasSubstr("Token="));

    // Exercise right-recursive attrdecllist with 260 global attributes so the
    // Bison state/value stack grows past YYINITDEPTH (200 -> 400 -> 800),
    // exercising both YYSTACK_RELOCATE and YYSTACK_FREE on yyss1 != yyssa.
    std::string deep_cdl = "netcdf deep_attrs {\n";
    for (int i = 0; i < 260; ++i) {
      absl::StrAppend(&deep_cdl, "  :attr_", i, " = ", i, ";\n");
    }
    deep_cdl += "}\n";
    EXPECT_EQ(
        0, ParseCdl(cdl_file.c_str(), deep_cdl, NC_FORMAT_NETCDF4, L_BINARY));
    EXPECT_EQ(0, error_count);
    EXPECT_EQ(260u, static_cast<size_t>(listlength(gattdefs)));

    // Unterminated /* comment hits EOF inside ST_C_COMMENT start state.
    std::string eof_comment_stderr;
    EXPECT_NE(0, ParseCdl(cdl_file.c_str(), "netcdf open_c {\n/* unterminated",
                          NC_FORMAT_NETCDF4, L_BINARY, err_file.c_str(),
                          &eof_comment_stderr));
    EXPECT_THAT(eof_comment_stderr, HasSubstr("unterminated /**/ comment"));

    // A single C comment exceeding YY_BUF_SIZE (16384) exercises Flex input
    // buffer growth inside yy_get_next_buffer().
    std::string long_comment_cdl = "netcdf big_cmt {\n/* ";
    long_comment_cdl.append(20000, 'a');
    long_comment_cdl += " */\n}\n";
    EXPECT_EQ(0, ParseCdl(cdl_file.c_str(), long_comment_cdl, NC_FORMAT_NETCDF4,
                          L_BINARY));
    EXPECT_EQ(0, error_count);
    return !::testing::Test::HasFailure();
  }));
}

TEST(NcgenyTest, UserDefinedTypesNestedGroupsPathsAndLargeFieldDim) {
  TempFile cdl_file("ncgeny_udt_groups.cdl");

  ASSERT_TRUE(RunInForkedChild([&]() {
    [&]() {
      constexpr std::string_view kComplexTypesCdl = R"cdl(
netcdf complex_udts {
types:
  :type_sec_att = "in_types";
  byte enum color_t { red = 1, green = 2, blue = 3 };
  int enum flag_t { off = 0, on = 1 }
  opaque(16) raw16_t;
  opaque(8) raw8_t
  int(*) vlen_int_t;
  raw8_t(*) vlen_raw8_t
  compound pair_t {
    int a;
    float b(2, 3), c;
  };
  compound plain_t {
    double d;
  }
dimensions:
  :dim_sec_att = 10;
  dx = 2, dy = 3;
  d_unlim = UNLIMITED;
  d_uint = 2u;
  d_int64 = 2ll;
variables:
  :var_sec_att = 20;
  int scalar_v;
  int empty_v;
  int arr_v(dx, dy);
  pair_t rec_v(dx);
  color_t col_v(/dx);
data:
  scalar_v = 1;
  empty_v = ;
group: outer {
  :outer_att = 99;
  dimensions:
    dz = 4;
  variables:
    int outer_v(/dx, dz);
  data:
    outer_v = 1, 2, 3, 4, 5, 6, 7, 8;
  group: inner {
    types:
      int enum inner_e { k0 = 10, k1 = 20 };
    dimensions:
      dw = 2;
    variables:
      inner_e leaf_v(/outer/dz, dw);
    data:
      leaf_v = k0;
  }
}
}
)cdl";
      EXPECT_EQ(0, ParseCdl(cdl_file.c_str(), kComplexTypesCdl,
                            NC_FORMAT_NETCDF4, L_BINARY));
      EXPECT_EQ(0, error_count);
      EXPECT_EQ(1, enhanced_flag);
      EXPECT_EQ(3u, static_cast<size_t>(listlength(grpdefs)));
      Symbol* empty_v_sym = FindSymbolInList(vardefs, "empty_v");
      ASSERT_NE(nullptr, empty_v_sym);
      ASSERT_NE(nullptr, empty_v_sym->data);
      EXPECT_EQ(0u, empty_v_sym->data->length);
      EXPECT_NE(nullptr, FindSymbolInList(typdefs, "color_t"));
      EXPECT_NE(nullptr, FindSymbolInList(typdefs, "raw16_t"));
      EXPECT_NE(nullptr, FindSymbolInList(typdefs, "vlen_raw8_t"));
      EXPECT_NE(nullptr, FindSymbolInList(typdefs, "pair_t"));
      EXPECT_NE(nullptr, FindSymbolInList(typdefs, "inner_e"));

      Symbol* pair_sym = FindSymbolInList(typdefs, "pair_t");
      ASSERT_NE(nullptr, pair_sym);
      EXPECT_EQ(NC_COMPOUND, pair_sym->subclass);
      ASSERT_EQ(3u, static_cast<size_t>(listlength(pair_sym->subnodes)));
      Symbol* field_b = FindSymbolInList(pair_sym->subnodes, "b");
      ASSERT_NE(nullptr, field_b);
      EXPECT_EQ(2, field_b->typ.dimset.ndims);
      EXPECT_EQ(2u, field_b->typ.dimset.dimsyms[0]->dim.declsize);
      EXPECT_EQ(3u, field_b->typ.dimset.dimsyms[1]->dim.declsize);

      Symbol* unlim_sym = FindSymbolInList(dimdefs, "d_unlim");
      ASSERT_NE(nullptr, unlim_sym);
      EXPECT_EQ(1, unlim_sym->dim.isunlimited);

      // fielddim: UINT_CONST (> INT32_MAX, <= UINT32_MAX), parse-only check.
      constexpr std::string_view kUintFieldDimCdl = R"cdl(
netcdf uint_fielddim {
types:
  compound wide_t {
    int huge(2147483648);
  };
}
)cdl";
      EXPECT_EQ(0, ParseCdl(cdl_file.c_str(), kUintFieldDimCdl,
                            NC_FORMAT_NETCDF4, L_BINARY));
      EXPECT_EQ(0, error_count);
      Symbol* wide_sym = FindSymbolInList(typdefs, "wide_t");
      ASSERT_NE(nullptr, wide_sym);
      Symbol* huge_field = FindSymbolInList(wide_sym->subnodes, "huge");
      ASSERT_NE(nullptr, huge_field);
      ASSERT_EQ(1, huge_field->typ.dimset.ndims);
      EXPECT_EQ(2147483648ULL,
                static_cast<unsigned long long>(
                    huge_field->typ.dimset.dimsyms[0]->dim.declsize));
    }();
    return !::testing::Test::HasFailure();
  }));
}

TEST(NcgenyTest, SpecialAttributesGlobalAndVariableFlags) {
  TempFile cdl_file("ncgeny_specials.cdl");

  ASSERT_TRUE(RunInForkedChild([&]() {
    [&]() {
      constexpr std::string_view kSpecialsCdl = R"cdl(
netcdf specials_test {
types:
  opaque(4) tag_t;
dimensions:
  d0 = 2;
  d1 = 4;
variables:
  int v1(d0, d1);
    v1:_FillValue = -999;
    v1:_Storage = "chunked";
    v1:_ChunkSizes = 2, 4;
    v1:_Fletcher32 = "true";
    v1:_DeflateLevel = 5;
    v1:_Shuffle = 1;
    v1:_Endianness = "little";
    v1:_NoFill = "true";
    v1:_Filter = "1,4";
    int v1:typed_att = 10, 20;
  double v2(d0);
    double v2:_FillValue = -1.25;
    v2:_Storage = "contiguous";
    v2:_ChunkSizes = 2;
    v2:_Fletcher32 = 0;
    v2:_Shuffle = "false";
    v2:_Endianness = "big";
    v2:_NoFill = 0;
  int v3;
    v3:_Storage = "compact";
  float v4(d0);
    v4:_Codecs = "[{\"id\":\"zlib\",\"level\":4}]";
    v4:_QuantizeBitGroomNumberOfSignificantDigits = 3;
  float v5(d0);
    v5:_QuantizeGranularBitRoundNumberOfSignificantDigits = 4;
  float v6(d0);
    v6:_QuantizeBitRoundNumberOfSignificantDigits = 5;
  tag_t:type_att = "type_attached";
:_NCProperties = "version=2,netcdf=4.10.1";
:_IsNetcdf4 = "true";
:_SuperblockVersion = 2;
:_Format = "netCDF-4";
:empty_global = ;
}
)cdl";
      EXPECT_EQ(0, ParseCdl(cdl_file.c_str(), kSpecialsCdl, NC_FORMAT_NETCDF4,
                            L_BINARY));
      EXPECT_EQ(0, error_count);
      EXPECT_GT(specials_flag, 0);
      EXPECT_EQ(1, format_attribute);
      EXPECT_EQ(NC_FORMAT_NETCDF4, globalspecials._Format);
      EXPECT_EQ(1, globalspecials._IsNetcdf4);
      EXPECT_EQ(2, globalspecials._Superblock);
      ASSERT_NE(nullptr, globalspecials._NCProperties);
      EXPECT_STREQ("version=2,netcdf=4.10.1", globalspecials._NCProperties);

      Symbol* v1 = FindSymbolInList(vardefs, "v1");
      ASSERT_NE(nullptr, v1);
      const Specialdata& s1 = v1->var.special;
      EXPECT_EQ(NC_CHUNKED, s1._Storage);
      ASSERT_EQ(2u, s1.nchunks);
      EXPECT_EQ(2u, s1._ChunkSizes[0]);
      EXPECT_EQ(4u, s1._ChunkSizes[1]);
      EXPECT_EQ(1, s1._Fletcher32);
      EXPECT_EQ(5, s1._DeflateLevel);
      EXPECT_EQ(1, s1._Shuffle);
      EXPECT_EQ(1, s1._Endianness);
      EXPECT_EQ(0, s1._Fill);
      EXPECT_NE(nullptr, s1._Fillvalue);
      ASSERT_EQ(1u, s1.nfilters);
      ASSERT_NE(nullptr, s1._Filters);
      EXPECT_EQ(1u, s1._Filters[0]->filterid);
      ASSERT_EQ(1u, s1._Filters[0]->nparams);
      EXPECT_EQ(4u, s1._Filters[0]->params[0]);

      Symbol* v2 = FindSymbolInList(vardefs, "v2");
      ASSERT_NE(nullptr, v2);
      EXPECT_EQ(NC_CHUNKED,
                v2->var.special._Storage);  // Overridden by _ChunkSizes
      EXPECT_EQ(0, v2->var.special._Fletcher32);
      EXPECT_EQ(0, v2->var.special._Shuffle);
      EXPECT_EQ(2, v2->var.special._Endianness);
      EXPECT_EQ(1, v2->var.special._Fill);

      Symbol* v3 = FindSymbolInList(vardefs, "v3");
      ASSERT_NE(nullptr, v3);
      EXPECT_EQ(NC_COMPACT, v3->var.special._Storage);

      Symbol* v4 = FindSymbolInList(vardefs, "v4");
      ASSERT_NE(nullptr, v4);
      ASSERT_NE(nullptr, v4->var.special._Codecs);
      EXPECT_STREQ("[{\"id\":\"zlib\",\"level\":4}]", v4->var.special._Codecs);
      // Upstream ncgenl.h rule 38 compiled "SignificantDIgits" while specials[]
      // has "SignificantDigits", so the lowercase spelling lexes as IDENT.
      EXPECT_EQ(0, v4->var.special._Quantizer);
      EXPECT_EQ(0, v4->var.special._NSD);

      Symbol* v5 = FindSymbolInList(vardefs, "v5");
      ASSERT_NE(nullptr, v5);
      EXPECT_EQ(NC_QUANTIZE_GRANULARBR, v5->var.special._Quantizer);
      EXPECT_EQ(4, v5->var.special._NSD);

      Symbol* v6 = FindSymbolInList(vardefs, "v6");
      ASSERT_NE(nullptr, v6);
      EXPECT_EQ(NC_QUANTIZE_BITROUND, v6->var.special._Quantizer);
      EXPECT_EQ(5, v6->var.special._NSD);

      // Check remaining boolean & format literals accepted by
      // makespecial/truefalse.
      struct BoolFormatCase {
        const char* bool_lit;
        int expected_bool;
        const char* format_lit;
        int expected_format;
      };
      constexpr BoolFormatCase kBoolCases[] = {
          {"\"false\"", 0, "classic", NC_FORMAT_CLASSIC},
          {"\"0\"", 0, "64-bit offset", NC_FORMAT_64BIT_OFFSET},
          {"\"1\"", 1, "netCDF-4 classic model", NC_FORMAT_NETCDF4_CLASSIC},
          {"0", 0, "64-bit data", NC_FORMAT_64BIT_DATA},
          {"1", 1, "netCDF-4", NC_FORMAT_NETCDF4},
      };
      for (const BoolFormatCase& tc : kBoolCases) {
        const std::string cdl =
            absl::StrCat("netcdf bool_fmt {\n  :_IsNetcdf4 = ", tc.bool_lit,
                         ";\n  :_Format = \"", tc.format_lit, "\";\n}\n");
        EXPECT_EQ(0, ParseCdl(cdl_file.c_str(), cdl, 0, L_BINARY))
            << tc.format_lit;
        EXPECT_EQ(tc.expected_bool, globalspecials._IsNetcdf4);
        EXPECT_EQ(tc.expected_format, globalspecials._Format);
      }
    }();
    return !::testing::Test::HasFailure();
  }));
}

TEST(NcgenyTest, ConstantsDataListsEscapesAndFunctions) {
  TempFile cdl_file("ncgeny_constants.cdl");

  ASSERT_TRUE(RunInForkedChild([&]() {
    [&]() {
      constexpr std::string_view kConstantsCdl = R"cdl(
netcdf const_all {
types:
  byte enum dir_t { north = 1, south = 2 };
  opaque(4) op_t;
dimensions:
  n = 15;
  nh = 5;
variables:
  byte vb(n);
  ubyte vub;
  short vs;
  ushort vus;
  int vi;
  uint vui;
  int64 vi64;
  uint64 vui64;
  int vhex(nh);
  float vf;
  double vd;
  string vstr;
  op_t vop;
  dir_t vdir;
  double vtime;
  int vsub;
:mixed_attr = "plain", "\n\t\\\"\x41\101", "";
data:
  vb = 'a', '\a', '\b', '\f', '\n', '\r', '\t', '\v', '\0', '\\', '\?', '\'', '\z', '\101', '\x41';
  vub = 200ub;
  vs = -12s;
  vus = 50000us;
  vi = 012, +5, 12b, -5B, 12S, 12L, 42l;
  vui = 42u, 42U, 42ul, 42UL, 42lu, 3000000000;
  vi64 = -42ll, 42LL, -3000000000, 5000000000ll;
  vui64 = 42ull, 42ULL, 42llu, 5000000000, 18446744073709551615ull;
  vhex = 0x12s, 0x1bs, 0x12l, 0x12ll, 0x123456789abcdef01l;
  vf = 1.5f, -2.5e-2f, Inff, -Inff, NaNf;
  vd = 1.5, -2.5e10, Infinity, -Infinity, NaN;
  vstr = "first", NIL;
  vop = 0xDEADBEEF, 0XABC;
  vdir = north, _, dir_t.south;
  vtime = TIME("2020-01-02 03:04:05");
  vsub = { { 1, 2 }, {} };
}
)cdl";
      EXPECT_EQ(0, ParseCdl(cdl_file.c_str(), kConstantsCdl, NC_FORMAT_NETCDF4,
                            L_BINARY));
      EXPECT_EQ(0, error_count);
      EXPECT_EQ(1, specialconstants);

      Symbol* vhex = FindSymbolInList(vardefs, "vhex");
      ASSERT_NE(nullptr, vhex);
      ASSERT_NE(nullptr, vhex->data);
      EXPECT_EQ(5u, vhex->data->length);

      Symbol* vtime = FindSymbolInList(vardefs, "vtime");
      ASSERT_NE(nullptr, vtime);
      ASSERT_NE(nullptr, vtime->data);
      ASSERT_EQ(1u, vtime->data->length);
      EXPECT_EQ(NC_DOUBLE, vtime->data->data[0]->nctype);
      EXPECT_TRUE(std::isfinite(vtime->data->data[0]->value.doublev));
      EXPECT_GT(vtime->data->data[0]->value.doublev, 400000.0);

      Symbol* vop = FindSymbolInList(vardefs, "vop");
      ASSERT_NE(nullptr, vop);
      ASSERT_NE(nullptr, vop->data);
      ASSERT_EQ(2u, vop->data->length);
      EXPECT_STREQ("deadbeef", vop->data->data[0]->value.opaquev.stringv);
      EXPECT_STREQ("abc0", vop->data->data[1]->value.opaquev.stringv);

      Symbol* vstr = FindSymbolInList(vardefs, "vstr");
      ASSERT_NE(nullptr, vstr);
      ASSERT_NE(nullptr, vstr->data);
      ASSERT_EQ(2u, vstr->data->length);
      EXPECT_EQ(NC_STRING, vstr->data->data[0]->nctype);
      EXPECT_EQ(NC_NIL, vstr->data->data[1]->nctype);

      // Verify NIL is accepted under L_C as well as L_BINARY.
      constexpr std::string_view kNilCLangCdl = R"cdl(
netcdf nil_c {
variables:
  string s;
data:
  s = NIL;
}
)cdl";
      EXPECT_EQ(
          0, ParseCdl(cdl_file.c_str(), kNilCLangCdl, NC_FORMAT_NETCDF4, L_C));
      EXPECT_EQ(0, error_count);
      Symbol* s_c = FindSymbolInList(vardefs, "s");
      ASSERT_NE(nullptr, s_c);
      ASSERT_NE(nullptr, s_c->data);
      ASSERT_EQ(1u, s_c->data->length);
      EXPECT_EQ(NC_NIL, s_c->data->data[0]->nctype);
    }();
    return !::testing::Test::HasFailure();
  }));
}

TEST(NcgenyTest, EndToEndBinaryGenerationAndVerification) {
  TempFile cdl_file("ncgeny_e2e.cdl");
  TempFile nc_file("ncgeny_e2e.nc");

  ASSERT_TRUE(RunInForkedChild([&] {
    constexpr std::string_view kE2eCdl = R"cdl(
netcdf e2e_generated {
types:
  byte enum color_t { red = 1, green = 2, blue = 3 };
  opaque(4) tag_t;
  int(*) vlen_int_t;
  compound point_t {
    int x;
    double y;
  };
dimensions:
  n = 2;
  unlim = UNLIMITED;
variables:
  int vals(n);
    vals:long_name = "values";
    vals:_FillValue = -999;
    vals:_Storage = "chunked";
    vals:_ChunkSizes = 2;
    vals:_DeflateLevel = 3;
    vals:_Shuffle = 1;
    vals:_Fletcher32 = "true";
    vals:_Endianness = "little";
    vals:_NoFill = "false";
  double stamp;
  color_t colors(n);
  tag_t blob;
  vlen_int_t ragged(n);
  point_t pts(n);
  int seq(unlim);
:_Format = "netCDF-4";
:title = "e2e";
data:
  vals = 10, 20;
  stamp = time("2020-01-02 03:04:05");
  colors = red, green;
  blob = 0xDEADBEEF;
  ragged = {1, 2}, {3};
  pts = {1, 1.5}, {3, 3.5};
  seq = 100, 200, 300;
group: sub {
  dimensions:
    m = 3;
  variables:
    string names(m);
    int refvals(/n);
  data:
    names = "alpha", "beta", "gamma";
    refvals = 7, 8;
}
}
)cdl";
    EXPECT_EQ(0, ParseCdl(cdl_file.c_str(), kE2eCdl, 0, L_BINARY));
    EXPECT_EQ(0, error_count);
    std::string out_path = nc_file.string();
    int jmp_ret = 0;
    EXPECT_TRUE(RunSemanticsAndDefine(out_path.data(), &jmp_ret))
        << "jmp_ret=" << jmp_ret;
    return !::testing::Test::HasFailure();
  }));

  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR, nc_open(nc_file.c_str(), NC_NOWRITE, nc.ptr()));
  int fmt = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_format(nc.get(), &fmt));
  EXPECT_EQ(NC_FORMAT_NETCDF4, fmt);

  int vals_id = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(nc.get(), "vals", &vals_id));
  int fill_val = 0;
  ASSERT_EQ(NC_NOERR,
            nc_get_att_int(nc.get(), vals_id, NC_FillValue, &fill_val));
  EXPECT_EQ(-999, fill_val);
  std::array<int, 2> vals{};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(nc.get(), vals_id, vals.data()));
  EXPECT_EQ(10, vals[0]);
  EXPECT_EQ(20, vals[1]);

  int stamp_id = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(nc.get(), "stamp", &stamp_id));
  double stamp = 0.0;
  ASSERT_EQ(NC_NOERR, nc_get_var_double(nc.get(), stamp_id, &stamp));
  EXPECT_TRUE(std::isfinite(stamp));
  EXPECT_GT(stamp, 400000.0);
  EXPECT_LT(stamp, 500000.0);

  int colors_id = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(nc.get(), "colors", &colors_id));
  std::array<signed char, 2> colors{};
  ASSERT_EQ(NC_NOERR, nc_get_var(nc.get(), colors_id, colors.data()));
  EXPECT_EQ(1, colors[0]);
  EXPECT_EQ(2, colors[1]);

  int blob_id = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(nc.get(), "blob", &blob_id));
  std::array<unsigned char, 4> blob{};
  ASSERT_EQ(NC_NOERR, nc_get_var(nc.get(), blob_id, blob.data()));
  EXPECT_THAT(blob, ::testing::ElementsAre(0xDE, 0xAD, 0xBE, 0xEF));

  {
    ScopedNcVlens ragged(2);
    int ragged_id = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(nc.get(), "ragged", &ragged_id));
    ASSERT_EQ(NC_NOERR, nc_get_var(nc.get(), ragged_id, ragged.data()));
    ASSERT_EQ(2u, ragged[0].len);
    ASSERT_EQ(1u, ragged[1].len);
    const auto* r0 = static_cast<const int*>(ragged[0].p);
    const auto* r1 = static_cast<const int*>(ragged[1].p);
    EXPECT_EQ(1, r0[0]);
    EXPECT_EQ(2, r0[1]);
    EXPECT_EQ(3, r1[0]);
  }

  int pts_id = -1;
  nc_type pts_tid = 0;
  size_t pts_size = 0;
  size_t x_off = 0;
  size_t y_off = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(nc.get(), "pts", &pts_id));
  ASSERT_EQ(NC_NOERR, nc_inq_vartype(nc.get(), pts_id, &pts_tid));
  ASSERT_EQ(NC_NOERR,
            nc_inq_compound(nc.get(), pts_tid, nullptr, &pts_size, nullptr));
  ASSERT_EQ(NC_NOERR,
            nc_inq_compound_fieldoffset(nc.get(), pts_tid, 0, &x_off));
  ASSERT_EQ(NC_NOERR,
            nc_inq_compound_fieldoffset(nc.get(), pts_tid, 1, &y_off));
  std::vector<unsigned char> pts_buf(pts_size * 2);
  ASSERT_EQ(NC_NOERR, nc_get_var(nc.get(), pts_id, pts_buf.data()));
  int x0 = 0;
  double y1 = 0.0;
  std::memcpy(&x0, pts_buf.data() + x_off, sizeof(x0));
  std::memcpy(&y1, pts_buf.data() + pts_size + y_off, sizeof(y1));
  EXPECT_EQ(1, x0);
  EXPECT_DOUBLE_EQ(3.5, y1);

  int sub_id = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_grp_ncid(nc.get(), "sub", &sub_id));
  {
    ScopedNcStrings names(3);
    int names_id = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(sub_id, "names", &names_id));
    ASSERT_EQ(NC_NOERR, nc_get_var_string(sub_id, names_id, names.data()));
    EXPECT_STREQ("alpha", names[0]);
    EXPECT_STREQ("beta", names[1]);
    EXPECT_STREQ("gamma", names[2]);
  }
  int refvals_id = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(sub_id, "refvals", &refvals_id));
  std::array<int, 2> refvals{};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(sub_id, refvals_id, refvals.data()));
  EXPECT_EQ(7, refvals[0]);
  EXPECT_EQ(8, refvals[1]);
}

TEST(NcgenyTest, DeclarationAndSemanticErrorPaths) {
  TempFile cdl_file("ncgeny_sem_errs.cdl");
  TempFile err_file("ncgeny_sem_errs.stderr");

  ASSERT_TRUE(RunInForkedChild([&] {
    struct ErrorCase {
      const char* label;
      std::string cdl;
      int format_flag;
      Language lang;
      const char* expected_diag;
    };

    std::string too_many_dims_field =
        "netcdf tmf {\ntypes:\n  compound c {\n    int f(";
    for (int i = 0; i < NC_MAX_VAR_DIMS + 1; ++i) {
      if (i > 0) too_many_dims_field += ",";
      too_many_dims_field += "1";
    }
    too_many_dims_field += ");\n  };\n}\n";

    std::string too_many_dims_var =
        "netcdf tmv {\ndimensions:\n  d = 1;\nvariables:\n  int v(";
    for (int i = 0; i < NC_MAX_VAR_DIMS + 1; ++i) {
      if (i > 0) too_many_dims_var += ",";
      too_many_dims_var += "d";
    }
    too_many_dims_var += ");\n}\n";

    const std::vector<ErrorCase> kCases = {
        {"dup_group", "netcdf dg { group: g {} group: g {} }",
         NC_FORMAT_NETCDF4, L_BINARY, "Duplicate group name"},
        {"dup_type", "netcdf dt { types: byte enum t { a = 0 }; opaque(4) t; }",
         NC_FORMAT_NETCDF4, L_BINARY, "duplicate type declaration"},
        {"dup_econst", "netcdf de { types: byte enum e { a = 0, a = 1 }; }",
         NC_FORMAT_NETCDF4, L_BINARY, "duplicate enum declaration"},
        {"dup_field", "netcdf df { types: compound c { int x; float x; }; }",
         NC_FORMAT_NETCDF4, L_BINARY, "duplicate field declaration"},
        {"nonpos_field_dim_zero",
         "netcdf pf0 { types: compound c { int f(0); }; }", NC_FORMAT_NETCDF4,
         L_BINARY, "field dimension must be positive"},
        {"nonpos_field_dim_neg",
         "netcdf pf1 { types: compound c { int f(-2); }; }", NC_FORMAT_NETCDF4,
         L_BINARY, "field dimension must be positive"},
        {"too_many_field_dims", std::move(too_many_dims_field),
         NC_FORMAT_NETCDF4, L_BINARY, "too many dimensions"},
        {"dup_dim", "netcdf dd { dimensions: d = 1; d = 2; }",
         NC_FORMAT_NETCDF4, L_BINARY, "Duplicate dimension declaration"},
        {"uint64_dim", "netcdf ud { dimensions: d = 9223372036854775808ull; }",
         NC_FORMAT_NETCDF4, L_BINARY, "Not a signed integer type"},
        {"dup_var", "netcdf dv { variables: int v; float v; }",
         NC_FORMAT_NETCDF4, L_BINARY, "Duplicate variable declaration"},
        {"too_many_var_dims", std::move(too_many_dims_var), NC_FORMAT_NETCDF4,
         L_BINARY, "too many dimensions"},
        {"undef_dim", "netcdf udim { variables: int v(no_dim); }",
         NC_FORMAT_NETCDF4, L_BINARY,
         "Undefined or forward referenced dimension"},
        {"varref_mismatch",
         "netcdf vm { types: opaque(4) op; data: op = 0x01; }",
         NC_FORMAT_NETCDF4, L_BINARY,
         "Undefined or forward referenced variable"},
        {"typeref_mismatch", "netcdf tm { variables: int v; v bad; }",
         NC_FORMAT_NETCDF4, L_BINARY, "Undefined or forward referenced type"},
        {"undef_ambig", "netcdf ua { data: missing_var = 1; }",
         NC_FORMAT_NETCDF4, L_BINARY, "Undefined name"},
        {"doubly_typed_att",
         "netcdf dta { types: opaque(4) op; variables: int op:a = 1; }",
         NC_FORMAT_NETCDF4, L_BINARY, "Doubly typed attribute"},
        {"fill_in_global_att", "netcdf fa { :a = _; :b = {1, _}; }",
         NC_FORMAT_NETCDF4, L_BINARY, "Attribute data may not contain fill"},
        {"fillvalue_multi",
         "netcdf fm { variables: int v; v:_FillValue = 1, 2; }",
         NC_FORMAT_NETCDF4, L_BINARY, "must be a single"},
        {"fillvalue_empty", "netcdf fe { variables: int v; v:_FillValue = ; }",
         NC_FORMAT_NETCDF4, L_BINARY, "must be a single"},
        {"fillvalue_fill", "netcdf ff { variables: int v; v:_FillValue = _; }",
         NC_FORMAT_NETCDF4, L_BINARY, "Attribute data may not contain fill"},
        {"fillvalue_not_var",
         "netcdf fnv { types: opaque(4) op; variables: op:_FillValue = 0x01; }",
         NC_FORMAT_NETCDF4, L_BINARY, "not associated with variable"},
        {"bad_format_special", "netcdf bf { :_Format = \"bogus_fmt\"; }",
         NC_FORMAT_NETCDF4, L_BINARY, "_Format: illegal value"},
        {"bad_storage_special",
         "netcdf bs { variables: int v; v:_Storage = \"sparse\"; }",
         NC_FORMAT_NETCDF4, L_BINARY, "_Storage: illegal value"},
        {"bad_endian_special",
         "netcdf be { variables: int v; v:_Endianness = \"middle\"; }",
         NC_FORMAT_NETCDF4, L_BINARY, "_Endianness: illegal value"},
        {"bad_bool_string", "netcdf bbs { :_IsNetcdf4 = \"maybe\"; }",
         NC_FORMAT_NETCDF4, L_BINARY, "_IsNetcdf4: illegal value"},
        {"bad_bool_int", "netcdf bbi { :_IsNetcdf4 = 2; }", NC_FORMAT_NETCDF4,
         L_BINARY, "_IsNetcdf4: illegal value"},
        {"bad_filter_empty",
         "netcdf bfe { variables: int v; v:_Filter = \"\"; }",
         NC_FORMAT_NETCDF4, L_BINARY, "_Filter: unparsable filter spec"},
        {"bad_filter_spec",
         "netcdf bfs { variables: int v; v:_Filter = \"not_a_filter\"; }",
         NC_FORMAT_NETCDF4, L_BINARY, "Malformed filter spec"},
        {"bad_codecs_empty",
         "netcdf bce { variables: int v; v:_Codecs = \"\"; }",
         NC_FORMAT_NETCDF4, L_BINARY, "_Codecs: unparsable codec spec"},
        {"time_bad_calendar",
         "netcdf tbc { variables: double t; data: t = time(\"cal\", "
         "\"2020-01-01\"); }",
         NC_FORMAT_NETCDF4, L_BINARY, "not supported"},
        {"time_bad_arg0",
         "netcdf tba0 { variables: double t; data: t = time(123); }",
         NC_FORMAT_NETCDF4, L_BINARY, "Expected function signature"},
        {"time_bad_arg1",
         "netcdf tba1 { variables: double t; data: t = time(\"cd\", 456); }",
         NC_FORMAT_NETCDF4, L_BINARY, "Expected function signature"},
        {"time_three_args",
         "netcdf tba3 { variables: double t; data: t = time(\"a\", \"b\", "
         "\"c\"); }",
         NC_FORMAT_NETCDF4, L_BINARY, "Expected function signature"},
        {"unknown_func",
         "netcdf uf { variables: double t; data: t = nope(1); }",
         NC_FORMAT_NETCDF4, L_BINARY, "Unknown function name"},
        {"nil_disallowed", "netcdf nd { variables: string s; data: s = NIL; }",
         NC_FORMAT_NETCDF4, L_UNDEFINED, "NIL only allowed"},
        {"undef_path_group",
         "netcdf upg { variables: int v; data: /missing_grp/v = 1; }",
         NC_FORMAT_NETCDF4, L_BINARY, "Undefined or forward referenced group"},
        {"classic_uint_const",
         "netcdf cuc { variables: int v; data: v = 42u; }", NC_FORMAT_CLASSIC,
         L_BINARY, "Illegal integer constant for classic"},
        {"neg_bool_int", "netcdf nbi { :_IsNetcdf4 = -1; }", NC_FORMAT_NETCDF4,
         L_BINARY, "_IsNetcdf4: illegal value"},
        {"bad_hex_suffix", "netcdf bhs { variables: int v; data: v = 0xabbs; }",
         NC_FORMAT_NETCDF4, L_BINARY, "Illegal integer suffix"},
        {"bad_hex_suffix_long",
         "netcdf bhsl { variables: int v; data: v = 0x1bbbbbs; }",
         NC_FORMAT_NETCDF4, L_BINARY, "Illegal integer suffix"},
        {"neg_unsigned_int", "netcdf nui { variables: uint v; data: v = -1u; }",
         NC_FORMAT_NETCDF4, L_BINARY, "Unsigned integer cannot be signed"},
        {"uint64_erange",
         "netcdf u64r { variables: uint64 v; data: v = "
         "18446744073709551616ull; }",
         NC_FORMAT_NETCDF4, L_BINARY, "integer constant out of range"},
        {"ubyte_outofrange",
         "netcdf ubr { variables: ubyte v; data: v = 256ub; }",
         NC_FORMAT_NETCDF4, L_BINARY, "Value out of range"},
        {"ushort_outofrange",
         "netcdf usr { variables: ushort v; data: v = 65536us; }",
         NC_FORMAT_NETCDF4, L_BINARY, "Value out of range"},
        {"uint_outofrange",
         "netcdf uir { variables: uint v; data: v = 4294967296u; }",
         NC_FORMAT_NETCDF4, L_BINARY, "Value out of range"},
        {"bad_hex_char_after_neg_byte",
         "netcdf bhc { dimensions: n = 2; variables: byte vb(n); data: vb = "
         "-1b, '\\x41'; }",
         NC_FORMAT_NETCDF4, L_BINARY, "bad hex character constant"},
        {"root_slash_path", "netcdf rsp { variables: int v; data: / = 1; }",
         NC_FORMAT_NETCDF4, L_BINARY, "Undefined name (line 1): rsp"},
    };

    for (const ErrorCase& tc : kCases) {
      SCOPED_TRACE(tc.label);
      std::string err_text;
      const int rc = ParseCdl(cdl_file.c_str(), tc.cdl, tc.format_flag, tc.lang,
                              err_file.c_str(), &err_text);
      EXPECT_NE(0, rc) << tc.label;
      EXPECT_GT(error_count, 0) << tc.label;
      EXPECT_THAT(err_text, HasSubstr(tc.expected_diag)) << tc.label;
    }

    // In classic mode, an out-of-range tagged integer triggers both
    // downconvert() error reporting and the !tagmatch() semwarn() diagnostic.
    {
      std::string classic_warn_stderr;
      const int rc = ParseCdl(
          cdl_file.c_str(), "netcdf cw { variables: int v; data: v = 256ub; }",
          NC_FORMAT_CLASSIC, L_BINARY, err_file.c_str(), &classic_warn_stderr);
      EXPECT_NE(0, rc);
      EXPECT_GT(error_count, 0);
      EXPECT_THAT(classic_warn_stderr, HasSubstr("Value out of range"));
      EXPECT_THAT(classic_warn_stderr,
                  HasSubstr("Warning: Integer out of range for tag"));
    }
    return !::testing::Test::HasFailure();
  }));
}

TEST(NcgenyTest, SyntaxErrorsExerciseBisonReportingAndCleanup) {
  TempFile cdl_file("ncgeny_syntax_errs.cdl");
  TempFile err_file("ncgeny_syntax_errs.stderr");

  ASSERT_TRUE(RunInForkedChild([&] {
    struct SyntaxCase {
      const char* label;
      const char* cdl;
      const char* expected_diag;
    };
    constexpr SyntaxCase kCases[] = {
        {"unexpected_eof", "netcdf eof_case {", "unexpected end of file"},
        {"unexpected_invalid_token", "netcdf bad_tok { @ }",
         "unexpected invalid token"},
        {"expecting_one_token_netcdf", "bogus_start {}", "expecting NETCDF"},
        {"expecting_one_token_lbrace", "netcdf missing_brace dimensions: d=1;",
         "expecting '{'"},
        {"expecting_two_tokens_fielddim",
         "netcdf fdim_err { types: compound c { int f(\"s\"); }; }",
         "expecting INT_CONST or UINT_CONST"},
        {"expecting_four_tokens_constint",
         "netcdf cint_err { types: byte enum e { a = \"s\" }; }",
         "expecting INT_CONST or INT64_CONST or UINT_CONST or UINT64_CONST"},
        {"expecting_two_tokens_group_or_rbrace", "netcdf grp_or_rbrace { = }",
         "expecting GROUP or '}'"},
        {"many_expected_tokens_fallback", "netcdf many_err { : = }",
         "syntax error, unexpected '='"},
    };

    for (const SyntaxCase& tc : kCases) {
      SCOPED_TRACE(tc.label);
      std::string err_text;
      const int rc = ParseCdl(cdl_file.c_str(), tc.cdl, NC_FORMAT_NETCDF4,
                              L_BINARY, err_file.c_str(), &err_text);
      EXPECT_NE(0, rc) << tc.label;
      EXPECT_GT(error_count, 0) << tc.label;
      EXPECT_THAT(err_text, HasSubstr(tc.expected_diag)) << tc.label;
    }
    return !::testing::Test::HasFailure();
  }));
}

TEST(NcgenyTest, DebugCleanupOnErrorAndSemanticAbort) {
  TempFile cdl_file("ncgeny_dbg_cleanup.cdl");
  TempFile err_file("ncgeny_dbg_cleanup.stderr");

  ASSERT_TRUE(RunInForkedChild([&] {
    // Syntax error with ncgdebug = 1 exercises yydestruct("Error: popping") and
    // stack prints along error recovery unwinding.
    std::string syntax_dbg;
    EXPECT_EQ(1, ParseCdl(cdl_file.c_str(), "netcdf bad_dbg { @ }",
                          NC_FORMAT_NETCDF4, L_BINARY, err_file.c_str(),
                          &syntax_dbg, /*dbg=*/0, /*bison_dbg=*/1));
    EXPECT_GT(error_count, 0);
    EXPECT_THAT(syntax_dbg, HasSubstr("Error: popping"));

    // Inside namedgroup attrdecllist, referencing /v followed by IDENT 'w'
    // forces lookahead 'w' before default reduction typeref: ambiguous_ref
    // triggers YYABORT, exercising Cleanup: discarding lookahead & popping.
    std::string abort_dbg;
    constexpr std::string_view kAbortLookaheadCdl = R"cdl(
netcdf abort_la {
dimensions:
  d = 1;
variables:
  int v;
group: g {
  /v w;
}
}
)cdl";
    EXPECT_EQ(1, ParseCdl(cdl_file.c_str(), kAbortLookaheadCdl,
                          NC_FORMAT_NETCDF4, L_BINARY, err_file.c_str(),
                          &abort_dbg, /*dbg=*/0, /*bison_dbg=*/1));
    EXPECT_GT(error_count, 0);
    EXPECT_THAT(abort_dbg, HasSubstr("Undefined or forward referenced type"));
    EXPECT_THAT(abort_dbg, HasSubstr("Cleanup: discarding lookahead"));
    EXPECT_THAT(abort_dbg, HasSubstr("Cleanup: popping"));
    return !::testing::Test::HasFailure();
  }));
}

TEST(NcgenyTest, ParserStackExhaustionReturnsTwoAndFreesStack) {
  TempFile cdl_file("ncgeny_stack_max.cdl");
  TempFile err_file("ncgeny_stack_max.stderr");

  ASSERT_TRUE(RunInForkedChild([&] {
    // Right-recursive attrdecllist pushes 2 stack slots per global attribute,
    // exceeding YYMAXDEPTH (10000) and exercising clamping, yyexhaustedlab, and
    // heap stack free.
    std::string huge_cdl = "netcdf stack_max {\n";
    for (int i = 0; i < 5005; ++i) {
      absl::StrAppend(&huge_cdl, "  :a_", i, " = ", i, ";\n");
    }
    huge_cdl += "}\n";

    std::string err_text;
    const int rc = ParseCdl(cdl_file.c_str(), huge_cdl, NC_FORMAT_NETCDF4,
                            L_BINARY, err_file.c_str(), &err_text);
    EXPECT_EQ(2, rc);
    EXPECT_GT(error_count, 0);
    EXPECT_THAT(err_text, HasSubstr("memory exhausted"));
    return !::testing::Test::HasFailure();
  }));
}

TEST(NcgenyTest, LexerBufferAndAccessors) {
  TempFile cdl_file("ncgeny_flex_buf.cdl");

  ASSERT_TRUE(RunInForkedChild([&]() {
    [&]() {
      ResetParserState(NC_FORMAT_NETCDF4, L_BINARY);

      // Safe no-ops on empty stack and null buffer pointers.
      ncgpop_buffer_state();
      ncg_flush_buffer(nullptr);
      ncg_delete_buffer(nullptr);

      // ncgrestart(nullptr) initializes a buffer when none is active.
      ncgrestart(nullptr);
      ncgset_debug(1);
      EXPECT_EQ(1, ncgget_debug());
      ncgset_debug(0);
      EXPECT_EQ(0, ncgget_debug());
      ncgset_lineno(77);
      EXPECT_EQ(77, ncgget_lineno());
      ncgset_lineno(1);
      ncgset_out(stderr);
      EXPECT_EQ(stderr, ncgget_out());
      ncgset_out(nullptr);
      ncgset_in(nullptr);
      EXPECT_EQ(nullptr, ncgget_in());
      ncglex_destroy();

      // ncg_scan_buffer rejects sizes < 2 or buffers missing trailing '\0\0'.
      std::array<char, 1> tiny = {'\0'};
      EXPECT_EQ(nullptr, ncg_scan_buffer(tiny.data(), tiny.size()));
      std::array<char, 2> bad_eob_first = {'a', '\0'};
      EXPECT_EQ(nullptr,
                ncg_scan_buffer(bad_eob_first.data(), bad_eob_first.size()));
      std::array<char, 2> bad_eob_second = {'\0', 'a'};
      EXPECT_EQ(nullptr,
                ncg_scan_buffer(bad_eob_second.data(), bad_eob_second.size()));

      // Buffer creation, stack push/grow/pop, switching, and flushing.
      ASSERT_TRUE(WriteTextFile(cdl_file.c_str(), "netcdf file_buf {}\n"));
      {
        FILE* fp = std::fopen(cdl_file.c_str(), "rb");
        ASSERT_NE(nullptr, fp);
        absl::Cleanup close_fp = [&fp] {
          if (fp != nullptr) {
            std::fclose(fp);
          }
        };

        constexpr std::string_view kRawSrc = "netcdf raw_buf {}";
        std::vector<char> raw_buf(kRawSrc.begin(), kRawSrc.end());
        raw_buf.push_back('\0');
        raw_buf.push_back('\0');
        struct yy_buffer_state* b_raw =
            ncg_scan_buffer(raw_buf.data(), raw_buf.size());
        ASSERT_NE(nullptr, b_raw);

        ncgpush_buffer_state(nullptr);
        struct yy_buffer_state* b_file = ncg_create_buffer(fp, 16);
        ASSERT_NE(nullptr, b_file);
        ncgpush_buffer_state(b_file);
        ncg_switch_to_buffer(b_file);
        ncg_switch_to_buffer(b_raw);
        ncg_switch_to_buffer(b_file);
        ncg_flush_buffer(b_file);

        ncgpop_buffer_state();
        ncgpop_buffer_state();
        ncglex_destroy();
      }

      // Complete parse driven by ncg_scan_string().
      ResetParserState(NC_FORMAT_NETCDF4, L_BINARY);
      {
        absl::Cleanup destroy_str = [] { ncglex_destroy(); };
        parse_init();
        ASSERT_NE(nullptr, ncg_scan_string("netcdf scan_str_ok {}"));
        EXPECT_EQ(0, ncgparse());
        EXPECT_EQ(0, error_count);
        EXPECT_STREQ("scan_str_ok", datasetname);
        EXPECT_GE(ncgget_leng(), 0);
        EXPECT_NE(nullptr, ncgget_text());
      }

      // Complete parse driven by ncg_scan_bytes().
      ResetParserState(NC_FORMAT_NETCDF4, L_BINARY);
      {
        absl::Cleanup destroy_bytes = [] { ncglex_destroy(); };
        parse_init();
        constexpr std::string_view kBytes = "netcdf scan_bytes_ok {}";
        ASSERT_NE(nullptr, ncg_scan_bytes(kBytes.data(),
                                          static_cast<int>(kBytes.size())));
        EXPECT_EQ(0, ncgparse());
        EXPECT_EQ(0, error_count);
        EXPECT_STREQ("scan_bytes_ok", datasetname);
      }
    }();
    return !::testing::Test::HasFailure();
  }));
}

}  // namespace
}  // namespace netcdf
