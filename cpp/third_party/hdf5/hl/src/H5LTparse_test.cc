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
// Tests HDF5 High-Level DDL parser (third_party/hdf5/hl/src/H5LTparse.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/hl/src/H5LTparse.y
//   https://github.com/HDFGroup/hdf5/blob/develop/hl/src/H5LTparse.c
//   https://github.com/HDFGroup/hdf5/blob/develop/hl/test/test_lite.c

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/h5_test_util.h"
#include "third_party/hdf5/hl/src/hdf5_hl.h"
#include "third_party/hdf5/src/hdf5.h"

namespace {

std::atomic<bool> g_inject_yyerror_token{false};
std::atomic<int> g_yyerror_token_injected{0};
std::atomic<bool> g_fail_bison_stack_malloc{false};
std::atomic<int> g_bison_malloc_failed{0};

using MallocFn = void* (*)(size_t);

void* InterceptBisonMalloc(size_t size, MallocFn real_fn) {
  if (g_fail_bison_stack_malloc.exchange(false, std::memory_order_relaxed)) {
    g_bison_malloc_failed.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  return real_fn(size);
}

}  // namespace

extern "C" {
#include "third_party/hdf5/hl/src/H5LTparse.h"

extern int H5LTyydebug;

struct yy_buffer_state;
using YY_BUFFER_STATE = yy_buffer_state*;

YY_BUFFER_STATE H5LTyy_scan_string(const char* yy_str);
int H5LTyylex_destroy(void);

int __real_H5LTyylex(void);
int __wrap_H5LTyylex(void) {
  if (g_inject_yyerror_token.exchange(false, std::memory_order_relaxed)) {
    g_yyerror_token_injected.fetch_add(1, std::memory_order_relaxed);
    return YYerror;
  }
  return __real_H5LTyylex();
}

void* __real_malloc(size_t size);
void* __wrap_malloc(size_t size) {
  return InterceptBisonMalloc(size, __real_malloc);
}

#ifdef __SANITIZE_ALLOC_TOKEN__
void* __real___alloc_token_0_malloc(size_t size);
void* __wrap___alloc_token_0_malloc(size_t size) {
  return InterceptBisonMalloc(size, __real___alloc_token_0_malloc);
}

void* __real___alloc_token_1_malloc(size_t size);
void* __wrap___alloc_token_1_malloc(size_t size) {
  return InterceptBisonMalloc(size, __real___alloc_token_1_malloc);
}
#endif  // __SANITIZE_ALLOC_TOKEN__
}  // extern "C"

namespace hdf5 {
namespace {

using ::testing::StrEq;

// Invokes H5LTyyparse() on a DDL string and destroys scanner state afterwards.
// H5LTyyparse() returns a valid open HDF5 datatype identifier on success, or
// Bison status codes 0 (empty input), 1 (syntax/semantic error), or 2 (stack
// exhaustion).
hid_t ParseDdlRaw(const char* ddl) {
  YY_BUFFER_STATE buf = H5LTyy_scan_string(ddl);
  if (buf == nullptr) {
    return -1;
  }
  const absl::Cleanup destroy_lexer = [] { H5LTyylex_destroy(); };
  return H5LTyyparse();
}

// Parses a valid DDL datatype description into an owned ScopedDatatype.
ScopedDatatype ParseDdl(const char* ddl) {
  const hid_t id = ParseDdlRaw(ddl);
  return ScopedDatatype(H5Iis_valid(id) > 0 ? id : H5I_INVALID_HID);
}

struct AtomicDdlCase {
  const char* ddl;
  H5T_class_t expected_class;
  size_t expected_size;
};

constexpr AtomicDdlCase kIntegerDdlCases[] = {
    {.ddl = "H5T_STD_I8BE", .expected_class = H5T_INTEGER, .expected_size = 1},
    {.ddl = "H5T_STD_I8LE", .expected_class = H5T_INTEGER, .expected_size = 1},
    {.ddl = "H5T_STD_I16BE", .expected_class = H5T_INTEGER, .expected_size = 2},
    {.ddl = "H5T_STD_I16LE", .expected_class = H5T_INTEGER, .expected_size = 2},
    {.ddl = "H5T_STD_I32BE", .expected_class = H5T_INTEGER, .expected_size = 4},
    {.ddl = "H5T_STD_I32LE", .expected_class = H5T_INTEGER, .expected_size = 4},
    {.ddl = "H5T_STD_I64BE", .expected_class = H5T_INTEGER, .expected_size = 8},
    {.ddl = "H5T_STD_I64LE", .expected_class = H5T_INTEGER, .expected_size = 8},
    {.ddl = "H5T_STD_U8BE", .expected_class = H5T_INTEGER, .expected_size = 1},
    {.ddl = "H5T_STD_U8LE", .expected_class = H5T_INTEGER, .expected_size = 1},
    {.ddl = "H5T_STD_U16BE", .expected_class = H5T_INTEGER, .expected_size = 2},
    {.ddl = "H5T_STD_U16LE", .expected_class = H5T_INTEGER, .expected_size = 2},
    {.ddl = "H5T_STD_U32BE", .expected_class = H5T_INTEGER, .expected_size = 4},
    {.ddl = "H5T_STD_U32LE", .expected_class = H5T_INTEGER, .expected_size = 4},
    {.ddl = "H5T_STD_U64BE", .expected_class = H5T_INTEGER, .expected_size = 8},
    {.ddl = "H5T_STD_U64LE", .expected_class = H5T_INTEGER, .expected_size = 8},
    {.ddl = "H5T_NATIVE_CHAR",
     .expected_class = H5T_INTEGER,
     .expected_size = sizeof(char)},
    {.ddl = "H5T_NATIVE_SCHAR",
     .expected_class = H5T_INTEGER,
     .expected_size = sizeof(signed char)},
    {.ddl = "H5T_NATIVE_UCHAR",
     .expected_class = H5T_INTEGER,
     .expected_size = sizeof(unsigned char)},
    {.ddl = "H5T_NATIVE_SHORT",
     .expected_class = H5T_INTEGER,
     .expected_size = sizeof(short)},  // NOLINT(runtime/int)
    {.ddl = "H5T_NATIVE_USHORT",
     .expected_class = H5T_INTEGER,
     .expected_size = sizeof(unsigned short)},  // NOLINT(runtime/int)
    {.ddl = "H5T_NATIVE_INT",
     .expected_class = H5T_INTEGER,
     .expected_size = sizeof(int)},
    {.ddl = "H5T_NATIVE_UINT",
     .expected_class = H5T_INTEGER,
     .expected_size = sizeof(unsigned int)},
    {.ddl = "H5T_NATIVE_LONG",
     .expected_class = H5T_INTEGER,
     .expected_size = sizeof(long)},  // NOLINT(runtime/int)
    {.ddl = "H5T_NATIVE_ULONG",
     .expected_class = H5T_INTEGER,
     .expected_size = sizeof(unsigned long)},  // NOLINT(runtime/int)
    {.ddl = "H5T_NATIVE_LLONG",
     .expected_class = H5T_INTEGER,
     .expected_size = sizeof(long long)},  // NOLINT(runtime/int)
    {.ddl = "H5T_NATIVE_ULLONG",
     .expected_class = H5T_INTEGER,
     .expected_size = sizeof(unsigned long long)},  // NOLINT(runtime/int)
};
static_assert(std::size(kIntegerDdlCases) == 27);

constexpr AtomicDdlCase kFloatAndComplexDdlCases[] = {
    {.ddl = "H5T_IEEE_F16BE", .expected_class = H5T_FLOAT, .expected_size = 2},
    {.ddl = "H5T_IEEE_F16LE", .expected_class = H5T_FLOAT, .expected_size = 2},
    {.ddl = "H5T_IEEE_F32BE", .expected_class = H5T_FLOAT, .expected_size = 4},
    {.ddl = "H5T_IEEE_F32LE", .expected_class = H5T_FLOAT, .expected_size = 4},
    {.ddl = "H5T_IEEE_F64BE", .expected_class = H5T_FLOAT, .expected_size = 8},
    {.ddl = "H5T_IEEE_F64LE", .expected_class = H5T_FLOAT, .expected_size = 8},
    {.ddl = "H5T_FLOAT_BFLOAT16BE",
     .expected_class = H5T_FLOAT,
     .expected_size = 2},
    {.ddl = "H5T_FLOAT_BFLOAT16LE",
     .expected_class = H5T_FLOAT,
     .expected_size = 2},
    {.ddl = "H5T_FLOAT_F8E4M3",
     .expected_class = H5T_FLOAT,
     .expected_size = 1},
    {.ddl = "H5T_FLOAT_F8E5M2",
     .expected_class = H5T_FLOAT,
     .expected_size = 1},
    {.ddl = "H5T_FLOAT_F6E2M3",
     .expected_class = H5T_FLOAT,
     .expected_size = 1},
    {.ddl = "H5T_FLOAT_F6E3M2",
     .expected_class = H5T_FLOAT,
     .expected_size = 1},
    {.ddl = "H5T_FLOAT_F4E2M1",
     .expected_class = H5T_FLOAT,
     .expected_size = 1},
    {.ddl = "H5T_NATIVE_FLOAT16",
     .expected_class = H5T_FLOAT,
     .expected_size = 2},
    {.ddl = "H5T_NATIVE_FLOAT",
     .expected_class = H5T_FLOAT,
     .expected_size = sizeof(float)},
    {.ddl = "H5T_NATIVE_DOUBLE",
     .expected_class = H5T_FLOAT,
     .expected_size = sizeof(double)},
    {.ddl = "H5T_NATIVE_LDOUBLE",
     .expected_class = H5T_FLOAT,
     .expected_size = sizeof(long double)},
    {.ddl = "H5T_COMPLEX_IEEE_F16BE",
     .expected_class = H5T_COMPLEX,
     .expected_size = 4},
    {.ddl = "H5T_COMPLEX_IEEE_F16LE",
     .expected_class = H5T_COMPLEX,
     .expected_size = 4},
    {.ddl = "H5T_COMPLEX_IEEE_F32BE",
     .expected_class = H5T_COMPLEX,
     .expected_size = 8},
    {.ddl = "H5T_COMPLEX_IEEE_F32LE",
     .expected_class = H5T_COMPLEX,
     .expected_size = 8},
    {.ddl = "H5T_COMPLEX_IEEE_F64BE",
     .expected_class = H5T_COMPLEX,
     .expected_size = 16},
    {.ddl = "H5T_COMPLEX_IEEE_F64LE",
     .expected_class = H5T_COMPLEX,
     .expected_size = 16},
    {.ddl = "H5T_NATIVE_FLOAT_COMPLEX",
     .expected_class = H5T_COMPLEX,
     .expected_size = 2 * sizeof(float)},
    {.ddl = "H5T_NATIVE_DOUBLE_COMPLEX",
     .expected_class = H5T_COMPLEX,
     .expected_size = 2 * sizeof(double)},
    {.ddl = "H5T_NATIVE_LDOUBLE_COMPLEX",
     .expected_class = H5T_COMPLEX,
     .expected_size = 2 * sizeof(long double)},
};
static_assert(std::size(kFloatAndComplexDdlCases) == 26);

TEST(H5ltParseTest, EmptyInputReturnsZero) { EXPECT_EQ(0, ParseDdlRaw("")); }

TEST(H5ltParseTest, AllIntegerFloatAndComplexAtomicTypes) {
  for (const AtomicDdlCase& tc : kIntegerDdlCases) {
    ScopedDatatype dt = ParseDdl(tc.ddl);
    ASSERT_TRUE(dt.valid()) << tc.ddl;
    EXPECT_EQ(tc.expected_class, H5Tget_class(dt)) << tc.ddl;
    EXPECT_EQ(tc.expected_size, H5Tget_size(dt)) << tc.ddl;
  }

  for (const AtomicDdlCase& tc : kFloatAndComplexDdlCases) {
    ScopedDatatype dt = ParseDdl(tc.ddl);
    ASSERT_TRUE(dt.valid()) << tc.ddl;
    EXPECT_EQ(tc.expected_class, H5Tget_class(dt)) << tc.ddl;
    EXPECT_EQ(tc.expected_size, H5Tget_size(dt)) << tc.ddl;
  }

  ScopedDatatype custom_complex = ParseDdl("H5T_COMPLEX { H5T_IEEE_F32LE }");
  ASSERT_TRUE(custom_complex.valid());
  EXPECT_EQ(H5T_COMPLEX, H5Tget_class(custom_complex));
  EXPECT_EQ(8U, H5Tget_size(custom_complex));
}

TEST(H5ltParseTest, StringOpaqueVlenArrayAndCompoundTypes) {
  // 1. String permutations: variable vs fixed length, 3 pad modes, 2 csets,
  // and both H5T_C_S1 and H5T_FORTRAN_S1 base types.
  struct StringCase {
    const char* ddl;
    bool expected_var;
    size_t expected_size;
    H5T_str_t expected_pad;
    H5T_cset_t expected_cset;
  };
  constexpr StringCase kStringCases[] = {
      {.ddl = "H5T_STRING { STRSIZE H5T_VARIABLE; STRPAD H5T_STR_NULLTERM; "
              "CSET H5T_CSET_ASCII; CTYPE H5T_C_S1; }",
       .expected_var = true,
       .expected_size = H5T_VARIABLE,
       .expected_pad = H5T_STR_NULLTERM,
       .expected_cset = H5T_CSET_ASCII},
      {.ddl = "H5T_STRING { STRSIZE 16; STRPAD H5T_STR_NULLPAD; "
              "CSET H5T_CSET_UTF8; CTYPE H5T_FORTRAN_S1; }",
       .expected_var = false,
       .expected_size = 16,
       .expected_pad = H5T_STR_NULLPAD,
       .expected_cset = H5T_CSET_UTF8},
      {.ddl = "H5T_STRING { STRSIZE 8; STRPAD H5T_STR_SPACEPAD; "
              "CSET H5T_CSET_ASCII; CTYPE H5T_C_S1; }",
       .expected_var = false,
       .expected_size = 8,
       .expected_pad = H5T_STR_SPACEPAD,
       .expected_cset = H5T_CSET_ASCII},
  };
  for (const StringCase& tc : kStringCases) {
    ScopedDatatype str_dt = ParseDdl(tc.ddl);
    ASSERT_TRUE(str_dt.valid()) << tc.ddl;
    EXPECT_EQ(H5T_STRING, H5Tget_class(str_dt));
    EXPECT_EQ(tc.expected_var ? 1 : 0, H5Tis_variable_str(str_dt));
    if (!tc.expected_var) {
      EXPECT_EQ(tc.expected_size, H5Tget_size(str_dt));
    }
    EXPECT_EQ(tc.expected_pad, H5Tget_strpad(str_dt));
    EXPECT_EQ(tc.expected_cset, H5Tget_cset(str_dt));
  }

  // 2. Opaque type (lexer rule 76 in H5LTanalyze.l requires non-empty quoted
  // literal).
  {
    ScopedDatatype opq =
        ParseDdl("H5T_OPAQUE { OPQ_SIZE 7; OPQ_TAG \"opq_tag\"; }");
    ASSERT_TRUE(opq.valid());
    EXPECT_EQ(H5T_OPAQUE, H5Tget_class(opq));
    EXPECT_EQ(7U, H5Tget_size(opq));
    char* tag = H5Tget_tag(opq);
    ASSERT_NE(nullptr, tag);
    const absl::Cleanup free_tag = [tag] { H5free_memory(tag); };
    EXPECT_THAT(tag, StrEq("opq_tag"));
  }

  // 3. Variable-length and multi-dimensional array types.
  {
    ScopedDatatype vlen = ParseDdl("H5T_VLEN { H5T_STD_I32LE }");
    ASSERT_TRUE(vlen.valid());
    EXPECT_EQ(H5T_VLEN, H5Tget_class(vlen));

    ScopedDatatype arr = ParseDdl("H5T_ARRAY { [2][3][5] H5T_IEEE_F64LE }");
    ASSERT_TRUE(arr.valid());
    EXPECT_EQ(H5T_ARRAY, H5Tget_class(arr));
    EXPECT_EQ(3, H5Tget_array_ndims(arr));
    std::array<hsize_t, 3> dims = {};
    ASSERT_GE(H5Tget_array_dims2(arr, dims.data()), 0);
    EXPECT_EQ(dims[0], 2U);
    EXPECT_EQ(dims[1], 3U);
    EXPECT_EQ(dims[2], 5U);
  }

  // 4. Compound types: empty, implicit offsets, explicit offsets, and nested.
  {
    ScopedDatatype empty_comp = ParseDdl("H5T_COMPOUND { }");
    ASSERT_TRUE(empty_comp.valid());
    EXPECT_EQ(H5T_COMPOUND, H5Tget_class(empty_comp));
    EXPECT_EQ(0, H5Tget_nmembers(empty_comp));

    ScopedDatatype implicit_comp =
        ParseDdl("H5T_COMPOUND { H5T_STD_I32LE \"a\"; H5T_IEEE_F64LE \"b\"; }");
    ASSERT_TRUE(implicit_comp.valid());
    EXPECT_EQ(2, H5Tget_nmembers(implicit_comp));
    EXPECT_EQ(0U, H5Tget_member_offset(implicit_comp, 0));
    EXPECT_EQ(4U, H5Tget_member_offset(implicit_comp, 1));
    EXPECT_EQ(12U, H5Tget_size(implicit_comp));

    ScopedDatatype explicit_comp = ParseDdl(
        "H5T_COMPOUND { H5T_STD_I32LE \"x\" : 0; "
        "H5T_STD_I16LE \"y\" : 4; H5T_STD_U8LE \"z\" : 8; }");
    ASSERT_TRUE(explicit_comp.valid());
    EXPECT_EQ(3, H5Tget_nmembers(explicit_comp));
    EXPECT_EQ(0U, H5Tget_member_offset(explicit_comp, 0));
    EXPECT_EQ(4U, H5Tget_member_offset(explicit_comp, 1));
    EXPECT_EQ(8U, H5Tget_member_offset(explicit_comp, 2));
    EXPECT_EQ(9U, H5Tget_size(explicit_comp));

    ScopedDatatype nested_comp = ParseDdl(
        "H5T_COMPOUND { "
        "H5T_COMPOUND { H5T_STD_I16LE \"inner\"; } \"sub\"; "
        "H5T_ARRAY { [2] H5T_STD_I32LE } \"arr\"; }");
    ASSERT_TRUE(nested_comp.valid());
    EXPECT_EQ(2, H5Tget_nmembers(nested_comp));
    EXPECT_EQ(10U, H5Tget_size(nested_comp));
  }
}

TEST(H5ltParseTest, EnumWidthsAndOppositeEndianConversions) {
  {
    ScopedDatatype empty_enum = ParseDdl("H5T_ENUM { H5T_STD_I32LE; }");
    ASSERT_TRUE(empty_enum.valid());
    EXPECT_EQ(H5T_ENUM, H5Tget_class(empty_enum));
    EXPECT_EQ(0, H5Tget_nmembers(empty_enum));
  }

  // Test 8/16/32/64-bit signed BE and unsigned LE base types plus
  // H5Tconvert() byte-swap paths in enum_def. On LP64 (where
  // sizeof(long) == sizeof(long long)), H5Tget_native_type matches
  // H5T_NATIVE_LONG/ULONG before LLONG/ULLONG, leaving LLONG/ULLONG
  // unreachable. H5LTanalyze.l's NUMBER rule accepts non-negative integers
  // only.
  struct EnumBaseCase {
    const char* base_token;
    size_t expected_size;
  };
  constexpr EnumBaseCase kEnumBases[] = {
      {.base_token = "H5T_STD_I8BE", .expected_size = 1},
      {.base_token = "H5T_STD_U8LE", .expected_size = 1},
      {.base_token = "H5T_STD_I16BE", .expected_size = 2},
      {.base_token = "H5T_STD_U16LE", .expected_size = 2},
      {.base_token = "H5T_STD_I32BE", .expected_size = 4},
      {.base_token = "H5T_STD_U32LE", .expected_size = 4},
      {.base_token = "H5T_STD_I64BE", .expected_size = 8},
      {.base_token = "H5T_STD_U64LE", .expected_size = 8},
      {.base_token = "H5T_NATIVE_LONG",
       .expected_size = sizeof(long)},  // NOLINT(runtime/int)
      {.base_token = "H5T_NATIVE_LLONG",
       .expected_size = sizeof(long long)},  // NOLINT(runtime/int)
  };
  for (const EnumBaseCase& tc : kEnumBases) {
    const std::string ddl = std::string("H5T_ENUM { ") + tc.base_token +
                            "; \"ZERO\" 0; \"TWO\" 2; }";
    ScopedDatatype enum_dt = ParseDdl(ddl.c_str());
    ASSERT_TRUE(enum_dt.valid()) << tc.base_token;
    EXPECT_EQ(H5T_ENUM, H5Tget_class(enum_dt));
    EXPECT_EQ(tc.expected_size, H5Tget_size(enum_dt));
    EXPECT_EQ(2, H5Tget_nmembers(enum_dt));
  }
}

TEST(H5ltParseTest, SyntaxErrorsStackRelocationAndStackExhaustion) {
  // 1. Stateless syntax errors return 1 without leaking HDF5 IDs or yyss.
  EXPECT_EQ(1, ParseDdlRaw(":"));
  EXPECT_EQ(1, ParseDdlRaw("H5T_VLEN {"));
  EXPECT_EQ(1, ParseDdlRaw("H5T_VLEN { : }"));

  // 2. Relocate Bison's parser stack past YYINITDEPTH (200 -> 400 -> 800) with
  // 210 stateless VLEN prefixes, then abort via syntax error so yyreturnlab
  // runs YYSTACK_FREE(yyss) cleanly.
  {
    std::string reloc_ddl;
    reloc_ddl.reserve(210 * 11 + 2);
    for (int i = 0; i < 210; ++i) {
      reloc_ddl += "H5T_VLEN { ";
    }
    reloc_ddl += ":";
    EXPECT_EQ(1, ParseDdlRaw(reloc_ddl.c_str()));
  }

  // 3. Exhaust Bison's YYMAXDEPTH (10000 states) via 5005 VLEN prefixes,
  // returning 2 through yyexhaustedlab + yyreturnlab with zero leaks.
  {
    std::string exhaust_ddl;
    exhaust_ddl.reserve(5005 * 11 + 16);
    for (int i = 0; i < 5005; ++i) {
      exhaust_ddl += "H5T_VLEN { ";
    }
    exhaust_ddl += "H5T_STD_I32LE";
    EXPECT_EQ(2, ParseDdlRaw(exhaust_ddl.c_str()));
  }

  // 4. Parser globals recover cleanly on the next valid parse.
  ScopedDatatype ok = ParseDdl("H5T_STD_I32LE");
  ASSERT_TRUE(ok.valid());
  EXPECT_EQ(H5T_INTEGER, H5Tget_class(ok));
}

TEST(H5ltParseTest, DebugTraceLexerErrorAndStackAllocFailure) {
  // 1. Exercise YYDEBUG=1 Bison trace helpers (yysymbol_name, yy_symbol_print,
  // yy_stack_print, yy_reduce_print) on short inputs; reset before deep parses
  // so O(N^2) stack traces do not spam stderr.
  {
    H5LTyydebug = 1;
    const absl::Cleanup reset_yydebug = [] { H5LTyydebug = 0; };

    ScopedDatatype dt = ParseDdl("H5T_COMPOUND { H5T_STD_I32LE \"a\" : 0; }");
    ASSERT_TRUE(dt.valid());
    EXPECT_EQ(H5T_COMPOUND, H5Tget_class(dt));

    EXPECT_EQ(1, ParseDdlRaw("H5T_VLEN { : }"));
  }

  // 2. Inject Bison's YYerror token (256) from H5LTyylex via --wrap so
  // yybackup's `else if (yychar == YYerror)` branch runs cleanly.
  {
    g_yyerror_token_injected.store(0, std::memory_order_relaxed);
    g_inject_yyerror_token.store(true, std::memory_order_relaxed);
    const absl::Cleanup reset_err = [] {
      g_inject_yyerror_token.store(false, std::memory_order_relaxed);
    };
    EXPECT_EQ(1, ParseDdlRaw("H5T_STD_I32LE"));
    EXPECT_EQ(1, g_yyerror_token_injected.load(std::memory_order_relaxed));
  }

  // 3. Inject nullptr on Bison's stack growth malloc (200 -> 400 states) after
  // H5LTyy_scan_string allocates its buffer, hitting YYNOMEM -> return 2.
  {
    std::string oom_ddl;
    oom_ddl.reserve(205 * 11 + 16);
    for (int i = 0; i < 205; ++i) {
      oom_ddl += "H5T_VLEN { ";
    }
    oom_ddl += "H5T_STD_I32LE";

    YY_BUFFER_STATE buf = H5LTyy_scan_string(oom_ddl.c_str());
    ASSERT_NE(nullptr, buf);
    const absl::Cleanup destroy_lexer = [] { H5LTyylex_destroy(); };

    g_bison_malloc_failed.store(0, std::memory_order_relaxed);
    g_fail_bison_stack_malloc.store(true, std::memory_order_relaxed);
    const absl::Cleanup reset_oom = [] {
      g_fail_bison_stack_malloc.store(false, std::memory_order_relaxed);
    };
    EXPECT_EQ(2, H5LTyyparse());
    EXPECT_EQ(1, g_bison_malloc_failed.load(std::memory_order_relaxed));
  }
}

// TODO(schwehr): Remaining uncovered lines in H5LTparse.c are statically
// unreachable Bison skeleton branches: yydestruct null yymsg,
// yy_symbol_value_print null yyvaluep, unsigned yytable <= 0, post-resize
// YYABORT guard, and error-token recovery without grammar error productions.

}  // namespace
}  // namespace hdf5
