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

// Unit tests for //third_party/netcdf/ncgen/escapes.c.
//
// Covers character/string escaping (C, XML, Java/UTF-16, Fortran-77), FQN
// segment escaping, language identifier codification, hex/octal/string
// unescaping, and escape-aware character search.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncgen/escapes.c

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/ncgen_main_shim.h"

extern "C" {
char* f77escapifychar(unsigned int c, char* s0);
}  // extern "C"

namespace netcdf {
namespace {

void CleanupNcgenBuffers() {
  if (filldatalist != nullptr) {
    filldatalist->readonly = 0;
    freedatalist(filldatalist);
    filldatalist = nullptr;
  }
  bbFree(codebuffer);
  codebuffer = nullptr;
  bbFree(stmt);
  stmt = nullptr;
}

class EscapesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CleanupNcgenBuffers();
    ncgen_shim_reset_globals();
  }

  void TearDown() override { CleanupNcgenBuffers(); }
};

TEST_F(EscapesTest, EscapifyCharAndEscapifyHandleControlsQuotesAndUtf8) {
  char out[16]{};
  EXPECT_EQ(out, escapifychar('\\', out, '"'));
  EXPECT_STREQ("\\\\", out);

  EXPECT_STREQ("\\\"", escapifychar('"', out, '"'));
  EXPECT_STREQ("\\'", escapifychar('\'', out, '\''));
  EXPECT_STREQ("A", escapifychar('A', out, '"'));
  EXPECT_STREQ(" ", escapifychar(' ', out, '"'));
  EXPECT_STREQ("~", escapifychar('~', out, '"'));

  const char high = static_cast<char>(0xC3);
  EXPECT_EQ(high, escapifychar(high, out, '"')[0]);
  EXPECT_EQ('\0', out[1]);

  EXPECT_STREQ("\\b", escapifychar('\b', out, '"'));
  EXPECT_STREQ("\\f", escapifychar('\f', out, '"'));
  EXPECT_STREQ("\\n", escapifychar('\n', out, '"'));
  EXPECT_STREQ("\\r", escapifychar('\r', out, '"'));
  EXPECT_STREQ("\\t", escapifychar('\t', out, '"'));
  EXPECT_STREQ("\\v", escapifychar('\v', out, '"'));
  EXPECT_STREQ("\\000", escapifychar('\0', out, '"'));
  EXPECT_STREQ("\\001", escapifychar('\x01', out, '"'));
  EXPECT_STREQ("\\033", escapifychar('\x1b', out, '"'));
  EXPECT_STREQ("\\177", escapifychar('\x7f', out, '"'));

  EXPECT_STREQ("\\n", cescapifychar('\n', '"'));
  EXPECT_STREQ("\\'", cescapifychar('\'', '\''));

  EXPECT_STREQ("", escapify("", '"', 0));
  constexpr char kWithNul[] = {'a', '\0', 'b', '"', '\\', '\t'};
  EXPECT_STREQ("a\\000b\\\"\\\\\\t", escapify(kWithNul, '"', sizeof(kWithNul)));
  EXPECT_STREQ("var\\\"\\\\name\\n", escapifyname("var\"\\name\n"));
}

TEST_F(EscapesTest, CquotestringQuotesAndEscapesBytebuffer) {
  Bytebuffer* bb = bbNew();
  ASSERT_NE(nullptr, bb);
  const absl::Cleanup free_bb = [bb] { bbFree(bb); };

  bbCat(bb, "a\"b\nc");
  cquotestring(bb, '"');
  EXPECT_STREQ("\"a\\\"b\\nc\"", bbContents(bb));

  bbClear(bb);
  cquotestring(bb, '\'');
  EXPECT_STREQ("''", bbContents(bb));
}

TEST_F(EscapesTest, CodifyHandlesBinaryCJavaAndF77) {
  l_flag = L_BINARY;
  EXPECT_STREQ("grp/var", codify("/grp/var"));
  EXPECT_STREQ("plain", codify("plain"));

  l_flag = L_C;
  EXPECT_STREQ("DIGIT_4_LFTX", codify("4LFTX"));
  EXPECT_STREQ("var_MINUS_1", codify("/var-1"));
  EXPECT_STREQ("SPACE_a", codify(" a"));
  EXPECT_STREQ("temp_2m", codify(codify("temp_2m")));
  EXPECT_STREQ("x_X80_XFF", codify("x\x80\xFF"));

  const char* all_specials = " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
  const std::string codified = codify(all_specials);
  EXPECT_NE(std::string::npos, codified.find("SPACE_"));
  EXPECT_NE(std::string::npos, codified.find("_SLASH_"));
  EXPECT_NE(std::string::npos, codified.find("_TILDE_"));
  EXPECT_NE(std::string::npos, codified.find("_BACKSLASH_"));

  l_flag = L_JAVA;
  EXPECT_STREQ("a_PERIOD_b", codify("a.b"));

  // Empty F77 string does not leak in f77quotestring; non-empty f77quotestring
  // intentionally leaks a temporary buffer upstream, so isolate in child.
  EXPECT_TRUE(RunInForkedChild([]() -> bool {
    l_flag = L_F77;
    std::unique_ptr<char, decltype(&std::free)> empty_f77(codify(""),
                                                          &std::free);
    if (empty_f77 == nullptr || std::strcmp(empty_f77.get(), "char(0)") != 0) {
      return false;
    }
    std::unique_ptr<char, decltype(&std::free)> quoted(codify("a'b\nc"),
                                                       &std::free);
    return quoted != nullptr &&
           std::strcmp(quoted.get(), "'a''b'//char(10)//'c'") == 0;
  }));
}

TEST_F(EscapesTest, XescapifyEscapesXmlEntitiesAndControls) {
  char empty[] = "";
  EXPECT_STREQ("", xescapify(empty, '"', 0));

  char text[] = {'a', '"', '&', '<', '>', '\0', '\n', '\x7f', '~'};
  EXPECT_STREQ("a&quot;&amp;&lt;&gt;&#00;&#0A;&#7F;~",
               xescapify(text, '"', sizeof(text)));
}

TEST_F(EscapesTest, JescapifyEscapesAsciiControlsAndUtf8) {
  char ascii[] = "hi\"'\t\b\n\r\f\x01\x7f";
  EXPECT_STREQ("hi\\\"'\\t\\b\\n\\r\\f\\u0001\\u007F",
               jescapify(ascii, '"', std::strlen(ascii)));

  char single_quote[] = "it's";
  EXPECT_STREQ("it\\'s", jescapify(single_quote, '\'', 4));

  char utf8[] = "\xC3\xA9\xE4\xB8\xAD";
  EXPECT_STREQ("\\u00E9\\u4E2D", jescapify(utf8, '"', std::strlen(utf8)));

  char jname[] = "var\"name\t";
  EXPECT_STREQ("var\\\"name\\t", jescapifyname(jname));

  // Invalid UTF-8 / non-BMP codepoints fail nc_utf8_to_utf16 and skip bbFree.
  EXPECT_TRUE(RunInForkedChild([]() -> bool {
    char bad_utf8[] = "\xFF\xFE";
    if (jescapify(bad_utf8, '"', 2) != nullptr || error_count != 1) {
      return false;
    }
    char non_bmp[] = "\xF0\x9F\x98\x80";
    return jescapify(non_bmp, '"', 4) == nullptr && error_count == 2;
  }));
}

TEST_F(EscapesTest, F77EscapifyCharAndF77Quotestring) {
  char out[32]{};
  EXPECT_EQ(out, f77escapifychar('\'', out));
  EXPECT_STREQ("''", out);
  EXPECT_STREQ("Z", f77escapifychar('Z', out));
  EXPECT_STREQ("//char(10)", f77escapifychar('\n', out));
  EXPECT_STREQ("//char(128)", f77escapifychar(0x80U, out));

  Bytebuffer* empty_bb = bbNew();
  ASSERT_NE(nullptr, empty_bb);
  const absl::Cleanup free_empty = [empty_bb] { bbFree(empty_bb); };
  f77quotestring(empty_bb);
  EXPECT_STREQ("char(0)", bbContents(empty_bb));

  EXPECT_TRUE(RunInForkedChild([]() -> bool {
    auto check = [](std::string_view input, std::string_view expected) -> bool {
      Bytebuffer* bb = bbNew();
      if (bb == nullptr) {
        return false;
      }
      bbAppendn(bb, input.data(), input.size());
      f77quotestring(bb);
      const bool ok = std::string_view(bbContents(bb)) == expected;
      bbFree(bb);
      return ok;
    };
    return check("abc", "'abc'") && check("\n", "char(10)") &&
           check("\na", "char(10)//'a'") &&
           check("a'b\x01\x02", "'a''b'//char(1)//char(2)") &&
           check("a\nb", "'a'//char(10)//'b'");
  }));
}

TEST_F(EscapesTest, FqnescapeReplacesSlashesAndDots) {
  EXPECT_STREQ("", fqnescape(""));
  EXPECT_STREQ("simple", fqnescape("simple"));
  EXPECT_STREQ("_SLASH_a_DOT_b_SLASH_c_DOT_", fqnescape("/a.b/c."));
}

TEST_F(EscapesTest, UnescapeHexOctAndStringsWork) {
  EXPECT_EQ(-1, unescapehex("g0"));
  EXPECT_EQ(-1, unescapehex("0z"));
  EXPECT_EQ(0xaf, unescapehex("af"));
  EXPECT_EQ(0xaf, unescapehex("AF"));
  EXPECT_NE(-1, unescapehex("09"));

  EXPECT_EQ(-1, unescapeoct("800"));
  EXPECT_EQ(-1, unescapeoct("080"));
  EXPECT_EQ(-1, unescapeoct("008"));
  EXPECT_EQ(0, unescapeoct("000"));
  EXPECT_EQ(83, unescapeoct("123"));
  EXPECT_EQ(255, unescapeoct("377"));

  constexpr char kSeq[] = "\\a\\b\\f\\n\\r\\t\\v\\\\\\?\\'\\\"\\qhi";
  char* out = nullptr;
  const int len = unescape(kSeq, std::strlen(kSeq), /*isident=*/0, &out);
  ASSERT_NE(nullptr, out);
  const std::unique_ptr<char, decltype(&std::free)> out_guard(out, &std::free);
  constexpr char kExpectedSeq[] = "\a\b\f\n\r\t\v\\\x7f'\"qhi";
  ASSERT_EQ(static_cast<int>(sizeof(kExpectedSeq) - 1), len);
  EXPECT_EQ(0, std::memcmp(out, kExpectedSeq, len + 1));

  constexpr char kOctHex[] =
      "\\xaf\\xAF\\000\\101\\200\\301\\400\\500\\600\\700";
  char* octhex_out = nullptr;
  const int oh_len =
      unescape(kOctHex, std::strlen(kOctHex), /*isident=*/0, &octhex_out);
  ASSERT_NE(nullptr, octhex_out);
  const std::unique_ptr<char, decltype(&std::free)> oh_guard(octhex_out,
                                                             &std::free);
  EXPECT_EQ(12, oh_len);
  EXPECT_EQ(static_cast<char>(-1), octhex_out[0]);
  EXPECT_EQ('f', octhex_out[1]);
  EXPECT_EQ(static_cast<char>(-1), octhex_out[2]);
  EXPECT_EQ('F', octhex_out[3]);
  EXPECT_EQ('\0', octhex_out[4]);
  EXPECT_EQ('A', octhex_out[5]);

  char* bad_oct_out = nullptr;
  const int bad_len = unescape("\\089", 4, /*isident=*/0, &bad_oct_out);
  ASSERT_NE(nullptr, bad_oct_out);
  const std::unique_ptr<char, decltype(&std::free)> bad_guard(bad_oct_out,
                                                              &std::free);
  EXPECT_EQ(1, bad_len);
  EXPECT_EQ('\0', bad_oct_out[0]);
  EXPECT_EQ(1, error_count);

  char* ident_out = nullptr;
  const int id_len = unescape("\\xab\\012", 8, /*isident=*/1, &ident_out);
  ASSERT_NE(nullptr, ident_out);
  const std::unique_ptr<char, decltype(&std::free)> id_guard(ident_out,
                                                             &std::free);
  EXPECT_EQ(6, id_len);
  EXPECT_STREQ("xab012", ident_out);

  EXPECT_TRUE(RunInForkedChild([]() -> bool {
    return unescape("abc", 3, /*isident=*/0, nullptr) == 3;
  }));
}

TEST_F(EscapesTest, EscStrchrSkipsEscapesAndOctHexSequences) {
  char not_found[] = "abcdef";
  EXPECT_EQ(not_found + 6, esc_strchr(not_found, 'z', /*octhex=*/0));

  char escaped_quote[] = "a\\\"b\"c";
  EXPECT_EQ(escaped_quote + 4, esc_strchr(escaped_quote, '"', /*octhex=*/0));

  char trailing_slash[] = "abc\\";
  EXPECT_EQ(trailing_slash + 3, esc_strchr(trailing_slash, '\\', /*octhex=*/0));

  char octhex_str[] = "\\x1a\\XAF\\123/end";
  EXPECT_EQ(octhex_str + 12, esc_strchr(octhex_str, '/', /*octhex=*/1));

  char non_octhex[] = "\\xz\\x1z\\189\\128/end";
  EXPECT_EQ(non_octhex + 15, esc_strchr(non_octhex, '/', /*octhex=*/1));

  char plain_hex[] = "\\x1a";
  EXPECT_EQ(plain_hex + 3, esc_strchr(plain_hex, 'a', /*octhex=*/0));

  char plain_oct[] = "\\123";
  EXPECT_EQ(plain_oct + 3, esc_strchr(plain_oct, '3', /*octhex=*/0));
}

}  // namespace
}  // namespace netcdf
