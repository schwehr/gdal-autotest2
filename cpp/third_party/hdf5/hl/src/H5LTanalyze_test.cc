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
// Tests HDF5 High-Level DDL lexical scanner
// (third_party/hdf5/hl/src/H5LTanalyze.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/hl/src/H5LTanalyze.l
//   https://github.com/HDFGroup/hdf5/blob/develop/hl/src/H5LTanalyze.c
//   https://github.com/HDFGroup/hdf5/blob/develop/hl/test/test_lite.c

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/hl/src/hdf5_hl.h"
#include "third_party/hdf5/src/hdf5.h"

extern "C" {
#include "third_party/hdf5/hl/src/H5LTparse.h"

struct yy_buffer_state;
using YY_BUFFER_STATE = yy_buffer_state*;
using yy_size_t = size_t;

extern char* myinput;
extern size_t input_len;

int H5LTyylex(void);
int H5LTyylex_destroy(void);
void H5LTyyrestart(FILE* input_file);
void H5LTyy_switch_to_buffer(YY_BUFFER_STATE new_buffer);
YY_BUFFER_STATE H5LTyy_create_buffer(FILE* file, int size);
void H5LTyy_delete_buffer(YY_BUFFER_STATE b);
void H5LTyy_flush_buffer(YY_BUFFER_STATE b);
void H5LTyypush_buffer_state(YY_BUFFER_STATE new_buffer);
void H5LTyypop_buffer_state(void);
YY_BUFFER_STATE H5LTyy_scan_buffer(char* base, yy_size_t size);
YY_BUFFER_STATE H5LTyy_scan_string(const char* yy_str);
YY_BUFFER_STATE H5LTyy_scan_bytes(const char* bytes, int len);
void* H5LTyyalloc(yy_size_t size);
void* H5LTyyrealloc(void* ptr, yy_size_t size);
void H5LTyyfree(void* ptr);
int H5LTyyget_lineno(void);
FILE* H5LTyyget_in(void);
FILE* H5LTyyget_out(void);
int H5LTyyget_leng(void);
char* H5LTyyget_text(void);
void H5LTyyset_lineno(int line_number);
void H5LTyyset_in(FILE* in_str);
void H5LTyyset_out(FILE* out_str);
int H5LTyyget_debug(void);
void H5LTyyset_debug(int bdebug);
int H5LTyyerror(const char* msg);
int H5LTyywrap(void);
}  // extern "C"

namespace hdf5 {
namespace {

using ::testing::HasSubstr;
using ::testing::StrEq;

// Non-null empty sentinel for myinput so my_yyinput()'s unconditional
// memcpy(buf, myinput, input_len) never passes nullptr (UBSan nonnull check).
char kEmptyInput = '\0';

auto MakeScannerCleanup() {
  return absl::MakeCleanup([] {
    myinput = &kEmptyInput;
    input_len = 0;
    H5LTyyset_lineno(1);
    H5LTyylex_destroy();
  });
}

struct TokenCase {
  const char* text;
  int expected_token;
};

constexpr TokenCase kKeywordTokens[] = {
    {.text = "H5T_STD_I8BE", .expected_token = H5T_STD_I8BE_TOKEN},
    {.text = "H5T_STD_I8LE", .expected_token = H5T_STD_I8LE_TOKEN},
    {.text = "H5T_STD_I16BE", .expected_token = H5T_STD_I16BE_TOKEN},
    {.text = "H5T_STD_I16LE", .expected_token = H5T_STD_I16LE_TOKEN},
    {.text = "H5T_STD_I32BE", .expected_token = H5T_STD_I32BE_TOKEN},
    {.text = "H5T_STD_I32LE", .expected_token = H5T_STD_I32LE_TOKEN},
    {.text = "H5T_STD_I64BE", .expected_token = H5T_STD_I64BE_TOKEN},
    {.text = "H5T_STD_I64LE", .expected_token = H5T_STD_I64LE_TOKEN},
    {.text = "H5T_STD_U8BE", .expected_token = H5T_STD_U8BE_TOKEN},
    {.text = "H5T_STD_U8LE", .expected_token = H5T_STD_U8LE_TOKEN},
    {.text = "H5T_STD_U16BE", .expected_token = H5T_STD_U16BE_TOKEN},
    {.text = "H5T_STD_U16LE", .expected_token = H5T_STD_U16LE_TOKEN},
    {.text = "H5T_STD_U32BE", .expected_token = H5T_STD_U32BE_TOKEN},
    {.text = "H5T_STD_U32LE", .expected_token = H5T_STD_U32LE_TOKEN},
    {.text = "H5T_STD_U64BE", .expected_token = H5T_STD_U64BE_TOKEN},
    {.text = "H5T_STD_U64LE", .expected_token = H5T_STD_U64LE_TOKEN},
    {.text = "H5T_NATIVE_CHAR", .expected_token = H5T_NATIVE_CHAR_TOKEN},
    {.text = "H5T_NATIVE_SCHAR", .expected_token = H5T_NATIVE_SCHAR_TOKEN},
    {.text = "H5T_NATIVE_UCHAR", .expected_token = H5T_NATIVE_UCHAR_TOKEN},
    {.text = "H5T_NATIVE_SHORT", .expected_token = H5T_NATIVE_SHORT_TOKEN},
    {.text = "H5T_NATIVE_USHORT", .expected_token = H5T_NATIVE_USHORT_TOKEN},
    {.text = "H5T_NATIVE_INT", .expected_token = H5T_NATIVE_INT_TOKEN},
    {.text = "H5T_NATIVE_UINT", .expected_token = H5T_NATIVE_UINT_TOKEN},
    {.text = "H5T_NATIVE_LONG", .expected_token = H5T_NATIVE_LONG_TOKEN},
    {.text = "H5T_NATIVE_ULONG", .expected_token = H5T_NATIVE_ULONG_TOKEN},
    {.text = "H5T_NATIVE_LLONG", .expected_token = H5T_NATIVE_LLONG_TOKEN},
    {.text = "H5T_NATIVE_ULLONG", .expected_token = H5T_NATIVE_ULLONG_TOKEN},
    {.text = "H5T_IEEE_F16BE", .expected_token = H5T_IEEE_F16BE_TOKEN},
    {.text = "H5T_IEEE_F16LE", .expected_token = H5T_IEEE_F16LE_TOKEN},
    {.text = "H5T_IEEE_F32BE", .expected_token = H5T_IEEE_F32BE_TOKEN},
    {.text = "H5T_IEEE_F32LE", .expected_token = H5T_IEEE_F32LE_TOKEN},
    {.text = "H5T_IEEE_F64BE", .expected_token = H5T_IEEE_F64BE_TOKEN},
    {.text = "H5T_IEEE_F64LE", .expected_token = H5T_IEEE_F64LE_TOKEN},
    {.text = "H5T_FLOAT_BFLOAT16BE",
     .expected_token = H5T_FLOAT_BFLOAT16BE_TOKEN},
    {.text = "H5T_FLOAT_BFLOAT16LE",
     .expected_token = H5T_FLOAT_BFLOAT16LE_TOKEN},
    {.text = "H5T_FLOAT_F8E4M3", .expected_token = H5T_FLOAT_F8E4M3_TOKEN},
    {.text = "H5T_FLOAT_F8E5M2", .expected_token = H5T_FLOAT_F8E5M2_TOKEN},
    {.text = "H5T_FLOAT_F6E2M3", .expected_token = H5T_FLOAT_F6E2M3_TOKEN},
    {.text = "H5T_FLOAT_F6E3M2", .expected_token = H5T_FLOAT_F6E3M2_TOKEN},
    {.text = "H5T_FLOAT_F4E2M1", .expected_token = H5T_FLOAT_F4E2M1_TOKEN},
    {.text = "H5T_NATIVE_FLOAT16", .expected_token = H5T_NATIVE_FLOAT16_TOKEN},
    {.text = "H5T_NATIVE_FLOAT", .expected_token = H5T_NATIVE_FLOAT_TOKEN},
    {.text = "H5T_NATIVE_DOUBLE", .expected_token = H5T_NATIVE_DOUBLE_TOKEN},
    {.text = "H5T_NATIVE_LDOUBLE", .expected_token = H5T_NATIVE_LDOUBLE_TOKEN},
    {.text = "H5T_COMPLEX_IEEE_F16BE",
     .expected_token = H5T_COMPLEX_IEEE_F16BE_TOKEN},
    {.text = "H5T_COMPLEX_IEEE_F16LE",
     .expected_token = H5T_COMPLEX_IEEE_F16LE_TOKEN},
    {.text = "H5T_COMPLEX_IEEE_F32BE",
     .expected_token = H5T_COMPLEX_IEEE_F32BE_TOKEN},
    {.text = "H5T_COMPLEX_IEEE_F32LE",
     .expected_token = H5T_COMPLEX_IEEE_F32LE_TOKEN},
    {.text = "H5T_COMPLEX_IEEE_F64BE",
     .expected_token = H5T_COMPLEX_IEEE_F64BE_TOKEN},
    {.text = "H5T_COMPLEX_IEEE_F64LE",
     .expected_token = H5T_COMPLEX_IEEE_F64LE_TOKEN},
    {.text = "H5T_NATIVE_FLOAT_COMPLEX",
     .expected_token = H5T_NATIVE_FLOAT_COMPLEX_TOKEN},
    {.text = "H5T_NATIVE_DOUBLE_COMPLEX",
     .expected_token = H5T_NATIVE_DOUBLE_COMPLEX_TOKEN},
    {.text = "H5T_NATIVE_LDOUBLE_COMPLEX",
     .expected_token = H5T_NATIVE_LDOUBLE_COMPLEX_TOKEN},
    {.text = "H5T_STRING", .expected_token = H5T_STRING_TOKEN},
    {.text = "STRSIZE", .expected_token = STRSIZE_TOKEN},
    {.text = "STRPAD", .expected_token = STRPAD_TOKEN},
    {.text = "CSET", .expected_token = CSET_TOKEN},
    {.text = "CTYPE", .expected_token = CTYPE_TOKEN},
    {.text = "H5T_STR_NULLTERM", .expected_token = H5T_STR_NULLTERM_TOKEN},
    {.text = "H5T_STR_NULLPAD", .expected_token = H5T_STR_NULLPAD_TOKEN},
    {.text = "H5T_STR_SPACEPAD", .expected_token = H5T_STR_SPACEPAD_TOKEN},
    {.text = "H5T_CSET_ASCII", .expected_token = H5T_CSET_ASCII_TOKEN},
    {.text = "H5T_CSET_UTF8", .expected_token = H5T_CSET_UTF8_TOKEN},
    {.text = "H5T_C_S1", .expected_token = H5T_C_S1_TOKEN},
    {.text = "H5T_FORTRAN_S1", .expected_token = H5T_FORTRAN_S1_TOKEN},
    {.text = "H5T_VARIABLE", .expected_token = H5T_VARIABLE_TOKEN},
    {.text = "H5T_COMPOUND", .expected_token = H5T_COMPOUND_TOKEN},
    {.text = "H5T_ENUM", .expected_token = H5T_ENUM_TOKEN},
    {.text = "H5T_ARRAY", .expected_token = H5T_ARRAY_TOKEN},
    {.text = "H5T_VLEN", .expected_token = H5T_VLEN_TOKEN},
    {.text = "H5T_COMPLEX", .expected_token = H5T_COMPLEX_TOKEN},
    {.text = "H5T_OPAQUE", .expected_token = H5T_OPAQUE_TOKEN},
    {.text = "OPQ_SIZE", .expected_token = OPQ_SIZE_TOKEN},
    {.text = "OPQ_TAG", .expected_token = OPQ_TAG_TOKEN},
};
static_assert(std::size(kKeywordTokens) == 74);

TEST(H5ltAnalyzeTest, ColdInitMyInputEchoBackupAndEof) {
  auto cleanup = MakeScannerCleanup();
  H5LTyylex_destroy();

  // 1. Exercise cold !yy_init initialization (yyin/yyout default, default
  // 256K buffer, and my_yyinput reading a self-delimited string).
  std::array<char, 16> cold_buf = {" \t\n42 :"};
  myinput = cold_buf.data();
  input_len = std::strlen(cold_buf.data());
  EXPECT_EQ(NUMBER, H5LTyylex());
  EXPECT_EQ(42, H5LTyylval.ival);
  EXPECT_EQ(':', H5LTyylex());

  // Reset input_len = 0 so the next refill returns EOF (0) cleanly.
  myinput = &kEmptyInput;
  input_len = 0;
  EXPECT_EQ(0, H5LTyylex());
  EXPECT_EQ(0, H5LTyylex_destroy());

  // 2. Capture ECHO output into an in-memory FILE* stream on both an
  // unmatched single character and a partial-keyword DFA backup ("CSEX").
  char* out_buf = nullptr;
  size_t out_size = 0;
  FILE* out_stream = open_memstream(&out_buf, &out_size);
  ASSERT_NE(nullptr, out_stream);
  const absl::Cleanup close_out = [&] {
    H5LTyyset_out(nullptr);
    std::fclose(out_stream);
    std::free(out_buf);
  };

  ASSERT_NE(nullptr, H5LTyy_scan_string("? CSEX :"));
  H5LTyyset_out(out_stream);
  EXPECT_EQ(':', H5LTyylex());
  EXPECT_EQ(0, H5LTyylex());
  std::fflush(out_stream);
  EXPECT_THAT(std::string_view(out_buf, out_size), HasSubstr("?"));
  EXPECT_THAT(std::string_view(out_buf, out_size), HasSubstr("CSEX"));
}

TEST(H5ltAnalyzeTest, AllKeywordsPunctuationLiteralsAndEmbeddedNul) {
  auto cleanup = MakeScannerCleanup();

  // 1. Every keyword token rule in H5LTanalyze.l (rules 1..74).
  for (const TokenCase& tc : kKeywordTokens) {
    ASSERT_NE(nullptr, H5LTyy_scan_string(tc.text));
    EXPECT_EQ(tc.expected_token, H5LTyylex()) << tc.text;
    EXPECT_EQ(static_cast<int>(std::strlen(tc.text)), H5LTyyget_leng());
    EXPECT_THAT(H5LTyyget_text(), StrEq(tc.text));
    EXPECT_EQ(0, H5LTyylex());
    H5LTyylex_destroy();
  }

  // 2. Punctuation rules, NUMBER, and STRING (including multiline literal;
  // rule 76 requires >= 1 char inside double quotes).
  ASSERT_NE(nullptr,
            H5LTyy_scan_string("{ } [ ] : ; 0 98765 \"hello\nworld\" \"x\""));
  EXPECT_EQ('{', H5LTyylex());
  EXPECT_EQ('}', H5LTyylex());
  EXPECT_EQ('[', H5LTyylex());
  EXPECT_EQ(']', H5LTyylex());
  EXPECT_EQ(':', H5LTyylex());
  EXPECT_EQ(';', H5LTyylex());

  EXPECT_EQ(NUMBER, H5LTyylex());
  EXPECT_EQ(0, H5LTyylval.ival);
  EXPECT_EQ(NUMBER, H5LTyylex());
  EXPECT_EQ(98765, H5LTyylval.ival);

  EXPECT_EQ(STRING, H5LTyylex());
  ASSERT_NE(nullptr, H5LTyylval.sval);
  EXPECT_THAT(H5LTyylval.sval, StrEq("hello\nworld"));
  std::free(H5LTyylval.sval);
  H5LTyylval.sval = nullptr;

  EXPECT_EQ(STRING, H5LTyylex());
  ASSERT_NE(nullptr, H5LTyylval.sval);
  EXPECT_THAT(H5LTyylval.sval, StrEq("x"));
  std::free(H5LTyylval.sval);
  H5LTyylval.sval = nullptr;

  EXPECT_EQ(0, H5LTyylex());
  H5LTyylex_destroy();

  // 3. Embedded NUL bytes exercise yy_try_NUL_trans() and rule 84 ECHO into an
  // in-memory stream.
  char* nul_out = nullptr;
  size_t nul_size = 0;
  FILE* out_stream = open_memstream(&nul_out, &nul_size);
  ASSERT_NE(nullptr, out_stream);
  const absl::Cleanup close_out = [&] {
    H5LTyyset_out(nullptr);
    std::fclose(out_stream);
    std::free(nul_out);
  };

  constexpr std::array<char, 8> kBytesWithNul = {'4', '2',  '\0', 'H',
                                                 'X', '\0', ':',  ' '};
  ASSERT_NE(nullptr, H5LTyy_scan_bytes(kBytesWithNul.data(),
                                       static_cast<int>(kBytesWithNul.size())));
  H5LTyyset_out(out_stream);
  EXPECT_EQ(NUMBER, H5LTyylex());
  EXPECT_EQ(42, H5LTyylval.ival);
  EXPECT_EQ(':', H5LTyylex());
  EXPECT_EQ(0, H5LTyylex());
  std::fflush(out_stream);
  EXPECT_THAT(std::string_view(nul_out, nul_size), HasSubstr("HX"));
}

TEST(H5ltAnalyzeTest, ScanBufferValidationAndInPlaceScan) {
  auto cleanup = MakeScannerCleanup();

  std::array<char, 8> small_buf = {'a', '\0', '\0'};
  EXPECT_EQ(nullptr, H5LTyy_scan_buffer(small_buf.data(), 1));

  std::array<char, 4> missing_eob1 = {'a', 'b', 'c', '\0'};
  EXPECT_EQ(nullptr, H5LTyy_scan_buffer(missing_eob1.data(), 4));

  // base[size - 2] == '\0' succeeds while base[size - 1] != '\0' fails.
  std::array<char, 4> missing_eob2 = {'a', 'b', '\0', 'd'};
  EXPECT_EQ(nullptr, H5LTyy_scan_buffer(missing_eob2.data(), 4));

  // Flex mutates yy_ch_buf in place and requires two trailing YY_END_OF_BUFFER
  // NUL bytes at base[size - 2] and base[size - 1].
  std::array<char, 6> valid_buf = {'1', '2', '3', ':', '\0', '\0'};
  YY_BUFFER_STATE buf = H5LTyy_scan_buffer(valid_buf.data(), valid_buf.size());
  ASSERT_NE(nullptr, buf);
  EXPECT_EQ(NUMBER, H5LTyylex());
  EXPECT_EQ(123, H5LTyylval.ival);
  EXPECT_EQ(':', H5LTyylex());
  EXPECT_EQ(0, H5LTyylex());
}

TEST(H5ltAnalyzeTest, BufferManagementStackGrowthRestartAndAccessors) {
  auto cleanup = MakeScannerCleanup();
  H5LTyylex_destroy();

  std::array<char, 8> in_data = {'7', ':'};
  FILE* in_stream = fmemopen(in_data.data(), 2, "r");
  ASSERT_NE(nullptr, in_stream);
  char* out_buf = nullptr;
  size_t out_size = 0;
  FILE* out_stream = open_memstream(&out_buf, &out_size);
  ASSERT_NE(nullptr, out_stream);
  const absl::Cleanup close_streams = [&] {
    H5LTyyset_in(nullptr);
    H5LTyyset_out(nullptr);
    std::fclose(in_stream);
    std::fclose(out_stream);
    std::free(out_buf);
  };

  // 1. Safe create/switch/flush/delete/restart transitions with size >= 256 so
  // my_yyinput never overruns if filled.
  H5LTyy_delete_buffer(nullptr);
  H5LTyy_flush_buffer(nullptr);

  // yyrestart on NULL current buffer allocates a default YY_BUF_SIZE buffer;
  // destroy it before switching buffers since yy_switch_to_buffer does not
  // free the previous active buffer.
  H5LTyyrestart(nullptr);
  H5LTyyrestart(in_stream);
  EXPECT_EQ(in_stream, H5LTyyget_in());
  H5LTyylex_destroy();

  YY_BUFFER_STATE buf_a = H5LTyy_create_buffer(in_stream, 256);
  YY_BUFFER_STATE buf_b = H5LTyy_create_buffer(nullptr, 256);
  ASSERT_NE(nullptr, buf_a);
  ASSERT_NE(nullptr, buf_b);

  H5LTyy_switch_to_buffer(buf_a);
  H5LTyy_switch_to_buffer(buf_a);  // No-op when already active.
  H5LTyy_flush_buffer(buf_a);      // Active-buffer flush reloads state.
  H5LTyy_flush_buffer(buf_b);      // Non-current buffer flush.
  H5LTyy_switch_to_buffer(buf_b);
  H5LTyy_delete_buffer(buf_a);
  H5LTyylex_destroy();

  // 2. Push 12 buffers to grow the internal Flex stack past initial capacity
  // (triggers yyrealloc with grow_size = 8 in yyensure_buffer_stack), then pop
  // every buffer cleanly before H5LTyylex_destroy().
  H5LTyypush_buffer_state(nullptr);  // Null push no-op.
  for (int i = 0; i < 12; ++i) {
    YY_BUFFER_STATE pushed = H5LTyy_create_buffer(nullptr, 256);
    ASSERT_NE(nullptr, pushed);
    H5LTyypush_buffer_state(pushed);
  }
  for (int i = 0; i < 12; ++i) {
    H5LTyypop_buffer_state();
  }
  H5LTyypop_buffer_state();  // Pop on empty stack no-op.
  H5LTyylex_destroy();

  // 3. Accessors, allocator entry points, yywrap, yyerror, and active-buffer
  // teardown via H5LTyylex_destroy().
  ASSERT_NE(nullptr, H5LTyy_scan_string("42"));
  EXPECT_EQ(NUMBER, H5LTyylex());
  EXPECT_EQ(42, H5LTyylval.ival);

  H5LTyyset_lineno(77);
  EXPECT_EQ(77, H5LTyyget_lineno());
  H5LTyyset_in(in_stream);
  EXPECT_EQ(in_stream, H5LTyyget_in());
  H5LTyyset_out(out_stream);
  EXPECT_EQ(out_stream, H5LTyyget_out());
  H5LTyyset_debug(1);
  EXPECT_EQ(1, H5LTyyget_debug());
  H5LTyyset_debug(0);
  EXPECT_EQ(0, H5LTyyget_debug());
  EXPECT_EQ(2, H5LTyyget_leng());
  EXPECT_THAT(H5LTyyget_text(), StrEq("42"));
  EXPECT_EQ(1, H5LTyywrap());
  // H5LTyyerror prints to stdout (printf in H5LTparse.y) and returns 0.
  EXPECT_EQ(0, H5LTyyerror("synthetic error"));

  void* raw = H5LTyyalloc(32);
  ASSERT_NE(nullptr, raw);
  raw = H5LTyyrealloc(raw, 64);
  ASSERT_NE(nullptr, raw);
  H5LTyyfree(raw);

  // H5LTyy_scan_string("42") left exactly one buffer on top (stack_top == 0),
  // so H5LTyylex_destroy() frees it cleanly without leaking.
  EXPECT_EQ(0, H5LTyylex_destroy());
}

TEST(H5ltAnalyzeTest, NextBufferMoveGrowExtendAndEofPending) {
  auto cleanup = MakeScannerCleanup();
  H5LTyylex_destroy();

  // 1. Sequence A: trigger number_to_move > 0, while (num_to_read <= 0) buffer
  // doubling, EOB_ACT_LAST_MATCH, and YY_BUFFER_EOF_PENDING with bounds-safe
  // ':' pause framing in an 8-byte (+ 2 EOB = 10-byte) buffer.
  {
    YY_BUFFER_STATE buf = H5LTyy_create_buffer(nullptr, /*size=*/8);
    ASSERT_NE(nullptr, buf);
    H5LTyy_switch_to_buffer(buf);

    std::array<char, 9> chunk1 = {":1234567"};
    myinput = chunk1.data();
    input_len = 8;
    EXPECT_EQ(':', H5LTyylex());

    myinput = &kEmptyInput;
    input_len = 0;
    EXPECT_EQ(NUMBER, H5LTyylex());
    EXPECT_EQ(1234567, H5LTyylval.ival);
    EXPECT_EQ(0, H5LTyylex());
    EXPECT_EQ(0, H5LTyylex_destroy());
  }

  // 2. Sequence B: trigger yy_get_next_buffer() 50%-extension realloc when
  // (yy_n_chars + number_to_move) > yy_buf_size.
  {
    YY_BUFFER_STATE buf = H5LTyy_create_buffer(nullptr, /*size=*/8);
    ASSERT_NE(nullptr, buf);
    H5LTyy_switch_to_buffer(buf);

    std::array<char, 6> part1 = {":1234"};
    myinput = part1.data();
    input_len = 5;
    EXPECT_EQ(':', H5LTyylex());

    std::array<char, 6> part2 = {"5678:"};
    myinput = part2.data();
    input_len = 5;
    EXPECT_EQ(NUMBER, H5LTyylex());
    EXPECT_EQ(12345678, H5LTyylval.ival);

    myinput = &kEmptyInput;
    input_len = 0;
    EXPECT_EQ(':', H5LTyylex());
    EXPECT_EQ(0, H5LTyylex());
    EXPECT_EQ(0, H5LTyylex_destroy());
  }
}

// TODO(schwehr): Remaining uncovered lines in H5LTanalyze.c are fatal
// OOM/internal error paths (YY_FATAL_ERROR), unreachable !yywrap() transitions
// (H5LTyywrap() unconditionally returns 1), and DFA case-0 state backup.

}  // namespace
}  // namespace hdf5
