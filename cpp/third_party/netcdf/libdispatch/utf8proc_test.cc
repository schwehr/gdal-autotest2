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

// Unit tests for //third_party/netcdf/libdispatch/utf8proc.c.
//
// Covers version/error strings, UTF-8 decoding and encoding, codepoint
// properties (categories, widths, case conversions, grapheme breaking under
// UAX #29 including regional indicators, emoji ZWJ, and Indic conjuncts),
// decomposition/lumping/stripping, custom mapping callbacks, UTF-32
// normalization/composition, and NFC/NFD/NFKC/NFKD/NFKC_Casefold APIs.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/utf8proc.c

#include "third_party/netcdf/libdispatch/utf8proc.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"

namespace netcdf {
namespace {

struct FreeDeleter {
  void operator()(void* ptr) const { std::free(ptr); }
};

using ScopedUtf8Ptr = std::unique_ptr<nc_utf8proc_uint8_t, FreeDeleter>;

ScopedUtf8Ptr WrapMalloc(nc_utf8proc_uint8_t* ptr) {
  return ScopedUtf8Ptr(ptr);
}

constexpr nc_utf8proc_option_t Opts(int flags) {
  return static_cast<nc_utf8proc_option_t>(flags);
}

const nc_utf8proc_uint8_t* AsBytes(const char* s) {
  return reinterpret_cast<const nc_utf8proc_uint8_t*>(s);
}

const nc_utf8proc_uint8_t* AsBytes(std::string_view sv) {
  return reinterpret_cast<const nc_utf8proc_uint8_t*>(sv.data());
}

std::string AsString(const nc_utf8proc_uint8_t* ptr) {
  return ptr != nullptr ? std::string(reinterpret_cast<const char*>(ptr))
                        : std::string();
}

std::string EncodeCp(nc_utf8proc_int32_t cp) {
  std::array<nc_utf8proc_uint8_t, 4> buf{};
  const nc_utf8proc_ssize_t len = nc_utf8proc_encode_char(cp, buf.data());
  return std::string(reinterpret_cast<const char*>(buf.data()),
                     static_cast<std::size_t>(len));
}

struct CustomMapState {
  int pass_calls = 0;
  bool fail_after_first_pass = false;
  nc_utf8proc_int32_t target = 'a';
  nc_utf8proc_int32_t replacement = 'Z';
};

extern "C" {
static nc_utf8proc_int32_t CustomTransformCallback(nc_utf8proc_int32_t cp,
                                                   void* data) {
  auto* state = static_cast<CustomMapState*>(data);
  ++state->pass_calls;
  if (state->fail_after_first_pass && state->pass_calls > 1) {
    return -1;
  }
  return cp == state->target ? state->replacement : cp;
}
}  // extern "C"

TEST(Utf8procTest, VersionsErrmsgAndUtf8ClassTable) {
  EXPECT_STREQ("2.9.0", nc_utf8proc_version());
  EXPECT_STREQ("15.1.0", nc_utf8proc_unicode_version());

  EXPECT_THAT(nc_utf8proc_errmsg(UTF8PROC_ERROR_NOMEM),
              ::testing::HasSubstr("allocated"));
  EXPECT_THAT(nc_utf8proc_errmsg(UTF8PROC_ERROR_OVERFLOW),
              ::testing::HasSubstr("too long"));
  EXPECT_THAT(nc_utf8proc_errmsg(UTF8PROC_ERROR_INVALIDUTF8),
              ::testing::HasSubstr("Invalid UTF-8"));
  EXPECT_THAT(nc_utf8proc_errmsg(UTF8PROC_ERROR_NOTASSIGNED),
              ::testing::HasSubstr("Unassigned"));
  EXPECT_THAT(nc_utf8proc_errmsg(UTF8PROC_ERROR_INVALIDOPTS),
              ::testing::HasSubstr("Invalid options"));
  EXPECT_THAT(nc_utf8proc_errmsg(-999), ::testing::HasSubstr("unknown error"));

  EXPECT_EQ(1, nc_utf8proc_utf8class[0x00]);
  EXPECT_EQ(1, nc_utf8proc_utf8class[0x7F]);
  EXPECT_EQ(0, nc_utf8proc_utf8class[0x80]);
  EXPECT_EQ(0, nc_utf8proc_utf8class[0xBF]);
  EXPECT_EQ(2, nc_utf8proc_utf8class[0xC2]);
  EXPECT_EQ(3, nc_utf8proc_utf8class[0xE0]);
  EXPECT_EQ(4, nc_utf8proc_utf8class[0xF0]);
  EXPECT_EQ(0, nc_utf8proc_utf8class[0xF8]);
}

TEST(Utf8procTest, IterateDecodesValidSequencesAndRejectsMalformedBytes) {
  nc_utf8proc_int32_t cp = 0;

  // Zero-length input leaves cp == -1 and returns 0.
  EXPECT_EQ(nc_utf8proc_ssize_t{0}, nc_utf8proc_iterate(AsBytes("A"), 0, &cp));
  EXPECT_EQ(-1, cp);

  // Valid 1-, 2-, 3-, and 4-byte UTF-8 sequences (both bounded and strlen=-1).
  struct ValidSeq {
    std::array<nc_utf8proc_uint8_t, 4> bytes;
    nc_utf8proc_ssize_t len;
    nc_utf8proc_int32_t expected_cp;
  };
  constexpr ValidSeq kValid[] = {
      {{{0x00, 0, 0, 0}}, 1, 0x0000},
      {{{0x41, 0, 0, 0}}, 1, 0x0041},
      {{{0x7F, 0, 0, 0}}, 1, 0x007F},
      {{{0xC2, 0x80, 0, 0}}, 2, 0x0080},
      {{{0xDF, 0xBF, 0, 0}}, 2, 0x07FF},
      {{{0xE0, 0xA0, 0x80, 0}}, 3, 0x0800},
      {{{0xED, 0x9F, 0xBF, 0}}, 3, 0xD7FF},
      {{{0xEF, 0xBF, 0xBF, 0}}, 3, 0xFFFF},
      {{{0xF0, 0x90, 0x80, 0x80}}, 4, 0x10000},
      {{{0xF0, 0x9F, 0x98, 0x80}}, 4, 0x1F600},
      {{{0xF1, 0x80, 0x80, 0x80}}, 4, 0x40000},
      {{{0xF4, 0x8F, 0xBF, 0xBF}}, 4, 0x10FFFF},
  };
  for (const ValidSeq& tc : kValid) {
    cp = -99;
    EXPECT_EQ(tc.len, nc_utf8proc_iterate(tc.bytes.data(), tc.len, &cp));
    EXPECT_EQ(tc.expected_cp, cp);

    cp = -99;
    EXPECT_EQ(tc.len, nc_utf8proc_iterate(tc.bytes.data(), -1, &cp));
    EXPECT_EQ(tc.expected_cp, cp);
  }

  // Malformed, truncated, overlong, surrogate, and out-of-range sequences.
  struct InvalidSeq {
    std::array<nc_utf8proc_uint8_t, 4> bytes;
    nc_utf8proc_ssize_t len;
  };
  constexpr InvalidSeq kInvalid[] = {
      {{{0x80, 0, 0, 0}}, 1},           // Bare continuation byte
      {{{0xC0, 0xAF, 0, 0}}, 2},        // Overlong 2-byte lead < 0xC2
      {{{0xC1, 0xBF, 0, 0}}, 2},        // Overlong 2-byte lead 0xC1
      {{{0xF5, 0x80, 0x80, 0x80}}, 4},  // Lead > 0xF4
      {{{0xFF, 0, 0, 0}}, 1},           // 0xFF invalid lead
      {{{0xC2, 0, 0, 0}}, 1},           // Truncated 2-byte
      {{{0xC2, 0x41, 0, 0}}, 2},        // Bad 2-byte continuation
      {{{0xE0, 0xA0, 0, 0}}, 2},        // Truncated 3-byte
      {{{0xE1, 0x41, 0x80, 0}}, 3},     // Bad 3-byte 2nd byte
      {{{0xE1, 0x80, 0x41, 0}}, 3},     // Bad 3-byte 3rd byte
      {{{0xED, 0xA0, 0x80, 0}}, 3},     // UTF-16 high surrogate U+D800
      {{{0xED, 0xBF, 0xBF, 0}}, 3},     // UTF-16 low surrogate U+DFFF
      {{{0xE0, 0x9F, 0xBF, 0}}, 3},     // Overlong 3-byte (< 0x800)
      {{{0xF0, 0x90, 0x80, 0}}, 3},     // Truncated 4-byte
      {{{0xF0, 0x41, 0x80, 0x80}}, 4},  // Bad 4-byte 2nd byte
      {{{0xF0, 0x90, 0x41, 0x80}}, 4},  // Bad 4-byte 3rd byte
      {{{0xF0, 0x90, 0x80, 0x41}}, 4},  // Bad 4-byte 4th byte
      {{{0xF0, 0x8F, 0xBF, 0xBF}}, 4},  // Overlong 4-byte (< 0x10000)
      {{{0xF4, 0x90, 0x80, 0x80}}, 4},  // Codepoint > 0x10FFFF
  };
  for (const InvalidSeq& tc : kInvalid) {
    cp = 123;
    EXPECT_EQ(nc_utf8proc_ssize_t{UTF8PROC_ERROR_INVALIDUTF8},
              nc_utf8proc_iterate(tc.bytes.data(), tc.len, &cp));
    EXPECT_EQ(-1, cp);
  }
}

TEST(Utf8procTest, CodepointValidAndEncodeChar) {
  EXPECT_FALSE(nc_utf8proc_codepoint_valid(-1));
  EXPECT_TRUE(nc_utf8proc_codepoint_valid(0));
  EXPECT_TRUE(nc_utf8proc_codepoint_valid(0xD7FF));
  EXPECT_FALSE(nc_utf8proc_codepoint_valid(0xD800));
  EXPECT_FALSE(nc_utf8proc_codepoint_valid(0xDFFF));
  EXPECT_TRUE(nc_utf8proc_codepoint_valid(0xE000));
  EXPECT_TRUE(nc_utf8proc_codepoint_valid(0x10FFFF));
  EXPECT_FALSE(nc_utf8proc_codepoint_valid(0x110000));

  std::array<nc_utf8proc_uint8_t, 4> buf{};
  EXPECT_EQ(nc_utf8proc_ssize_t{0}, nc_utf8proc_encode_char(-1, buf.data()));
  EXPECT_EQ(nc_utf8proc_ssize_t{0},
            nc_utf8proc_encode_char(0x110000, buf.data()));

  EXPECT_EQ(nc_utf8proc_ssize_t{1}, nc_utf8proc_encode_char(0x41, buf.data()));
  EXPECT_EQ(0x41, buf[0]);

  EXPECT_EQ(nc_utf8proc_ssize_t{2},
            nc_utf8proc_encode_char(0x00E9, buf.data()));
  EXPECT_EQ(0xC3, buf[0]);
  EXPECT_EQ(0xA9, buf[1]);

  // Note: encode_char permits 0xD800..0xDFFF in 3-byte range by design.
  EXPECT_EQ(nc_utf8proc_ssize_t{3},
            nc_utf8proc_encode_char(0xD800, buf.data()));
  EXPECT_EQ(nc_utf8proc_ssize_t{3},
            nc_utf8proc_encode_char(0x4E2D, buf.data()));
  EXPECT_EQ(0xE4, buf[0]);
  EXPECT_EQ(0xB8, buf[1]);
  EXPECT_EQ(0xAD, buf[2]);

  EXPECT_EQ(nc_utf8proc_ssize_t{4},
            nc_utf8proc_encode_char(0x1F600, buf.data()));
  EXPECT_EQ(0xF0, buf[0]);
  EXPECT_EQ(0x9F, buf[1]);
  EXPECT_EQ(0x98, buf[2]);
  EXPECT_EQ(0x80, buf[3]);
}

TEST(Utf8procTest, PropertyLookupCaseWidthAndCategory) {
  EXPECT_EQ(UTF8PROC_CATEGORY_CN, nc_utf8proc_get_property(-1)->category);
  EXPECT_EQ(UTF8PROC_CATEGORY_CN, nc_utf8proc_get_property(0x110000)->category);

  // BMP and non-BMP (surrogate-encoded sequence table) case mappings.
  EXPECT_EQ('a', nc_utf8proc_tolower('A'));
  EXPECT_EQ('a', nc_utf8proc_tolower('a'));
  EXPECT_EQ('A', nc_utf8proc_toupper('a'));
  EXPECT_EQ('A', nc_utf8proc_toupper('A'));
  EXPECT_EQ(0x01C5, nc_utf8proc_totitle(0x01C4));  // Ǆ -> ǅ
  EXPECT_EQ('1', nc_utf8proc_totitle('1'));
  EXPECT_EQ(0x10428, nc_utf8proc_tolower(0x10400));  // Deseret Long I
  EXPECT_EQ(0x10400, nc_utf8proc_toupper(0x10428));

  EXPECT_EQ(1, nc_utf8proc_islower('a'));
  EXPECT_EQ(0, nc_utf8proc_islower('A'));
  EXPECT_EQ(0, nc_utf8proc_islower(0x01C5));
  EXPECT_EQ(1, nc_utf8proc_isupper('A'));
  EXPECT_EQ(0, nc_utf8proc_isupper('a'));
  EXPECT_EQ(0, nc_utf8proc_isupper(0x01C5));

  EXPECT_EQ(0, nc_utf8proc_charwidth('\n'));
  EXPECT_EQ(0, nc_utf8proc_charwidth(0x0301));
  EXPECT_EQ(1, nc_utf8proc_charwidth('a'));
  EXPECT_EQ(2, nc_utf8proc_charwidth(0x4E2D));   // '中'
  EXPECT_EQ(2, nc_utf8proc_charwidth(0x1F600));  // '😀'

  EXPECT_EQ(UTF8PROC_CATEGORY_CN, nc_utf8proc_category(0x0378));
  EXPECT_STREQ("Cn", nc_utf8proc_category_string(0x0378));
  EXPECT_STREQ("Lu", nc_utf8proc_category_string('A'));
  EXPECT_STREQ("Ll", nc_utf8proc_category_string('a'));
  EXPECT_STREQ("Lt", nc_utf8proc_category_string(0x01C5));
  EXPECT_STREQ("Nd", nc_utf8proc_category_string('0'));
  EXPECT_STREQ("Mn", nc_utf8proc_category_string(0x0301));
  EXPECT_STREQ("Zs", nc_utf8proc_category_string(' '));
  EXPECT_STREQ("Cc", nc_utf8proc_category_string('\n'));
  EXPECT_STREQ("Cs", nc_utf8proc_category_string(0xD800));
  EXPECT_STREQ("Co", nc_utf8proc_category_string(0xE000));
}

TEST(Utf8procTest, GraphemeBreakCoversStatelessAndStatefulUax29Rules) {
  // Stateless UAX #29 rules (GB3, GB4, GB5, GB6, GB7, GB8, GB9, GB9a, GB9b,
  // GB12/13, GB999).
  EXPECT_FALSE(nc_utf8proc_grapheme_break('\r', '\n'));        // GB3
  EXPECT_TRUE(nc_utf8proc_grapheme_break('\r', 'a'));          // GB4
  EXPECT_TRUE(nc_utf8proc_grapheme_break('\n', 'a'));          // GB4
  EXPECT_TRUE(nc_utf8proc_grapheme_break(0x0000, 'a'));        // GB4
  EXPECT_TRUE(nc_utf8proc_grapheme_break('a', '\r'));          // GB5
  EXPECT_TRUE(nc_utf8proc_grapheme_break('a', 0x0000));        // GB5
  EXPECT_FALSE(nc_utf8proc_grapheme_break(0x1100, 0x1100));    // GB6 L+L
  EXPECT_FALSE(nc_utf8proc_grapheme_break(0x1100, 0x1161));    // GB6 L+V
  EXPECT_FALSE(nc_utf8proc_grapheme_break(0x1100, 0xAC00));    // GB6 L+LV
  EXPECT_FALSE(nc_utf8proc_grapheme_break(0x1100, 0xAC01));    // GB6 L+LVT
  EXPECT_FALSE(nc_utf8proc_grapheme_break(0xAC00, 0x1161));    // GB7 LV+V
  EXPECT_FALSE(nc_utf8proc_grapheme_break(0x1161, 0x11A8));    // GB7 V+T
  EXPECT_FALSE(nc_utf8proc_grapheme_break(0xAC01, 0x11A8));    // GB8 LVT+T
  EXPECT_FALSE(nc_utf8proc_grapheme_break(0x11A8, 0x11A8));    // GB8 T+T
  EXPECT_FALSE(nc_utf8proc_grapheme_break('a', 0x0301));       // GB9 Extend
  EXPECT_FALSE(nc_utf8proc_grapheme_break('a', 0x200D));       // GB9 ZWJ
  EXPECT_FALSE(nc_utf8proc_grapheme_break(0x0915, 0x093E));    // GB9a Spacing
  EXPECT_FALSE(nc_utf8proc_grapheme_break(0x0600, '1'));       // GB9b Prepend
  EXPECT_FALSE(nc_utf8proc_grapheme_break(0x1F1FA, 0x1F1F8));  // GB12/13
  EXPECT_TRUE(nc_utf8proc_grapheme_break('a', 'b'));           // GB999

  // Stateful GB12/13: regional indicator pairs break after 2 indicators.
  {
    nc_utf8proc_int32_t state = 0;
    EXPECT_FALSE(nc_utf8proc_grapheme_break_stateful(0x1F1FA, 0x1F1F8, &state));
    EXPECT_TRUE(nc_utf8proc_grapheme_break_stateful(0x1F1F8, 0x1F1EC, &state));
    EXPECT_FALSE(nc_utf8proc_grapheme_break_stateful(0x1F1EC, 0x1F1E7, &state));
  }

  // Stateful GB11: Extended_Pictographic Extend* ZWJ Extended_Pictographic.
  {
    nc_utf8proc_int32_t state = 0;
    EXPECT_FALSE(nc_utf8proc_grapheme_break_stateful(0x1F469, 0x1F3FD, &state));
    EXPECT_FALSE(nc_utf8proc_grapheme_break_stateful(0x1F3FD, 0x200D, &state));
    EXPECT_FALSE(nc_utf8proc_grapheme_break_stateful(0x200D, 0x1F680, &state));
    EXPECT_TRUE(nc_utf8proc_grapheme_break_stateful(0x1F680, 'a', &state));
  }

  // Stateful GB9c: Indic conjunct break (Consonant + Extend* + Virama +
  // Consonant, as well as Linker followed by Extend and by non-Extend).
  {
    nc_utf8proc_int32_t state = 0;
    EXPECT_FALSE(nc_utf8proc_grapheme_break_stateful(0x0915, 0x094D, &state));
    EXPECT_FALSE(nc_utf8proc_grapheme_break_stateful(0x094D, 0x200D, &state));
    EXPECT_FALSE(nc_utf8proc_grapheme_break_stateful(0x200D, 0x0924, &state));
  }
  {
    nc_utf8proc_int32_t state = 0;
    EXPECT_FALSE(nc_utf8proc_grapheme_break_stateful(0x0915, 0x200D, &state));
    EXPECT_FALSE(nc_utf8proc_grapheme_break_stateful(0x200D, 0x094D, &state));
    EXPECT_TRUE(nc_utf8proc_grapheme_break_stateful(0x094D, 'a', &state));
  }
}

TEST(Utf8procTest, DecomposeCharCoversHangulLumpStrippingAndCharbound) {
  std::array<nc_utf8proc_int32_t, 32> out{};

  // Out-of-range codepoints fail with NOTASSIGNED.
  EXPECT_EQ(
      nc_utf8proc_ssize_t{UTF8PROC_ERROR_NOTASSIGNED},
      nc_utf8proc_decompose_char(-1, out.data(), out.size(), Opts(0), nullptr));
  EXPECT_EQ(nc_utf8proc_ssize_t{UTF8PROC_ERROR_NOTASSIGNED},
            nc_utf8proc_decompose_char(0x110000, out.data(), out.size(),
                                       Opts(0), nullptr));

  // Hangul algorithmic decomposition (2-jamo LV and 3-jamo LVT, with short &
  // full buffers).
  EXPECT_EQ(nc_utf8proc_ssize_t{2},
            nc_utf8proc_decompose_char(0xAC00, out.data(), 0,
                                       UTF8PROC_DECOMPOSE, nullptr));
  EXPECT_EQ(nc_utf8proc_ssize_t{2},
            nc_utf8proc_decompose_char(0xAC00, out.data(), 1,
                                       UTF8PROC_DECOMPOSE, nullptr));
  EXPECT_EQ(nc_utf8proc_ssize_t{2},
            nc_utf8proc_decompose_char(0xAC00, out.data(), 2,
                                       UTF8PROC_DECOMPOSE, nullptr));
  EXPECT_EQ(0x1100, out[0]);
  EXPECT_EQ(0x1161, out[1]);

  EXPECT_EQ(nc_utf8proc_ssize_t{3},
            nc_utf8proc_decompose_char(0xAC01, out.data(), 2,
                                       UTF8PROC_DECOMPOSE, nullptr));
  EXPECT_EQ(nc_utf8proc_ssize_t{3},
            nc_utf8proc_decompose_char(0xAC01, out.data(), 3,
                                       UTF8PROC_DECOMPOSE, nullptr));
  EXPECT_EQ(0x1100, out[0]);
  EXPECT_EQ(0x1161, out[1]);
  EXPECT_EQ(0x11A8, out[2]);

  // REJECTNA, IGNORE, STRIPNA, STRIPMARK (Mn, Mc, Me).
  EXPECT_EQ(nc_utf8proc_ssize_t{UTF8PROC_ERROR_NOTASSIGNED},
            nc_utf8proc_decompose_char(0x0378, out.data(), out.size(),
                                       UTF8PROC_REJECTNA, nullptr));
  EXPECT_EQ(nc_utf8proc_ssize_t{0},
            nc_utf8proc_decompose_char(0x00AD, out.data(), out.size(),
                                       UTF8PROC_IGNORE, nullptr));
  EXPECT_EQ(nc_utf8proc_ssize_t{0},
            nc_utf8proc_decompose_char(0x0378, out.data(), out.size(),
                                       UTF8PROC_STRIPNA, nullptr));
  for (const nc_utf8proc_int32_t mark : {0x0301, 0x0903, 0x20DD}) {
    EXPECT_EQ(nc_utf8proc_ssize_t{0},
              nc_utf8proc_decompose_char(mark, out.data(), out.size(),
                                         UTF8PROC_STRIPMARK, nullptr));
  }

  // Exercise all UTF8PROC_LUMP replacement rules.
  struct LumpCase {
    nc_utf8proc_int32_t input;
    nc_utf8proc_int32_t expected;
  };
  constexpr LumpCase kLumps[] = {
      {0x00A0, 0x0020},  // Zs -> space
      {0x2018, 0x0027},  // quote -> '
      {0x2014, 0x002D},  // Pd -> -
      {0x2212, 0x002D},  // minus -> -
      {0x2044, 0x002F},  // fraction slash -> /
      {0x2236, 0x003A},  // ratio -> :
      {0x2039, 0x003C},  // single left angle -> <
      {0x203A, 0x003E},  // single right angle -> >
      {0x2216, 0x005C},  // set minus -> backslash
      {0x02C6, 0x005E},  // circumflex -> ^
      {0x203F, 0x005F},  // Pc -> _
      {0x02CD, 0x005F},  // low macron -> _
      {0x02CB, 0x0060},  // grave -> `
      {0x2223, 0x007C},  // divides -> |
      {0x223C, 0x007E},  // tilde -> ~
  };
  for (const LumpCase& tc : kLumps) {
    out[0] = -1;
    EXPECT_EQ(nc_utf8proc_ssize_t{1},
              nc_utf8proc_decompose_char(tc.input, out.data(), out.size(),
                                         UTF8PROC_LUMP, nullptr));
    EXPECT_EQ(tc.expected, out[0]);
  }
  // LUMP + NLF2LF maps Zl (U+2028) and Zp (U+2029) to '\n'.
  for (const nc_utf8proc_int32_t sep : {0x2028, 0x2029}) {
    out[0] = -1;
    EXPECT_EQ(nc_utf8proc_ssize_t{1},
              nc_utf8proc_decompose_char(sep, out.data(), out.size(),
                                         Opts(UTF8PROC_LUMP | UTF8PROC_NLF2LF),
                                         nullptr));
    EXPECT_EQ(0x000A, out[0]);

    // Without both NLF2LS and NLF2PS, Zl/Zp remains unchanged under LUMP.
    out[0] = -1;
    EXPECT_EQ(nc_utf8proc_ssize_t{1},
              nc_utf8proc_decompose_char(sep, out.data(), out.size(),
                                         UTF8PROC_LUMP, nullptr));
    EXPECT_EQ(sep, out[0]);
  }

  // CASEFOLD ('ß' -> "ss") and canonical vs compatibility decomposition.
  EXPECT_EQ(nc_utf8proc_ssize_t{2},
            nc_utf8proc_decompose_char(0x00DF, out.data(), out.size(),
                                       UTF8PROC_CASEFOLD, nullptr));
  EXPECT_EQ('s', out[0]);
  EXPECT_EQ('s', out[1]);

  EXPECT_EQ(nc_utf8proc_ssize_t{1},
            nc_utf8proc_decompose_char(0xFB01, out.data(), out.size(),
                                       UTF8PROC_DECOMPOSE, nullptr));
  EXPECT_EQ(0xFB01, out[0]);

  EXPECT_EQ(nc_utf8proc_ssize_t{2},
            nc_utf8proc_decompose_char(
                0xFB01, out.data(), out.size(),
                Opts(UTF8PROC_DECOMPOSE | UTF8PROC_COMPAT), nullptr));
  EXPECT_EQ('f', out[0]);
  EXPECT_EQ('i', out[1]);

  // Multi-char (>3 codepoints) sequence lookup (U+337F -> 4 kanji, U+FDFA ->
  // 18).
  EXPECT_EQ(nc_utf8proc_ssize_t{4},
            nc_utf8proc_decompose_char(
                0x337F, out.data(), out.size(),
                Opts(UTF8PROC_DECOMPOSE | UTF8PROC_COMPAT), nullptr));
  EXPECT_EQ(nc_utf8proc_ssize_t{18},
            nc_utf8proc_decompose_char(
                0xFDFA, out.data(), out.size(),
                Opts(UTF8PROC_DECOMPOSE | UTF8PROC_COMPAT), nullptr));

  // UTF8PROC_CHARBOUND tracks last_boundclass across calls and respects
  // short bufsizes (0, 1, 2).
  int last_bc = UTF8PROC_BOUNDCLASS_START;
  EXPECT_EQ(nc_utf8proc_ssize_t{2},
            nc_utf8proc_decompose_char('a', out.data(), 0, UTF8PROC_CHARBOUND,
                                       &last_bc));
  last_bc = UTF8PROC_BOUNDCLASS_START;
  EXPECT_EQ(nc_utf8proc_ssize_t{2},
            nc_utf8proc_decompose_char('a', out.data(), 1, UTF8PROC_CHARBOUND,
                                       &last_bc));
  EXPECT_EQ(-1, out[0]);
  last_bc = UTF8PROC_BOUNDCLASS_START;
  EXPECT_EQ(nc_utf8proc_ssize_t{2},
            nc_utf8proc_decompose_char('a', out.data(), out.size(),
                                       UTF8PROC_CHARBOUND, &last_bc));
  EXPECT_EQ(-1, out[0]);
  EXPECT_EQ('a', out[1]);
  // Combining accent attaches to 'a' without a grapheme break sentinel.
  EXPECT_EQ(nc_utf8proc_ssize_t{1},
            nc_utf8proc_decompose_char(0x0301, out.data(), out.size(),
                                       UTF8PROC_CHARBOUND, &last_bc));
  EXPECT_EQ(0x0301, out[0]);
}

TEST(Utf8procTest, DecomposeAndCustomOptionsValidationAndCombiningReorder) {
  std::array<nc_utf8proc_int32_t, 16> buf{};

  // Conflicting or incomplete option combinations fail with INVALIDOPTS.
  EXPECT_EQ(nc_utf8proc_ssize_t{UTF8PROC_ERROR_INVALIDOPTS},
            nc_utf8proc_decompose(AsBytes("a"), 1, buf.data(), buf.size(),
                                  Opts(UTF8PROC_COMPOSE | UTF8PROC_DECOMPOSE)));
  EXPECT_EQ(nc_utf8proc_ssize_t{UTF8PROC_ERROR_INVALIDOPTS},
            nc_utf8proc_decompose(AsBytes("a"), 1, buf.data(), buf.size(),
                                  UTF8PROC_STRIPMARK));

  // Invalid UTF-8 under both UTF8PROC_NULLTERM and explicit length.
  constexpr nc_utf8proc_uint8_t kBadUtf8[] = {0xFF, 0x00, 0x00, 0x00};
  EXPECT_EQ(nc_utf8proc_ssize_t{UTF8PROC_ERROR_INVALIDUTF8},
            nc_utf8proc_decompose(kBadUtf8, 0, buf.data(), buf.size(),
                                  UTF8PROC_NULLTERM));
  EXPECT_EQ(
      nc_utf8proc_ssize_t{UTF8PROC_ERROR_INVALIDUTF8},
      nc_utf8proc_decompose(kBadUtf8, 1, buf.data(), buf.size(), Opts(0)));

  // REJECTNA propagates from decompose_char.
  EXPECT_EQ(
      nc_utf8proc_ssize_t{UTF8PROC_ERROR_NOTASSIGNED},
      nc_utf8proc_decompose(AsBytes("\xCD\xB8"), 0, buf.data(), buf.size(),
                            Opts(UTF8PROC_NULLTERM | UTF8PROC_REJECTNA)));

  // Explicit length preserves embedded NUL bytes.
  constexpr std::string_view kWithNul("a\0b", 3);
  EXPECT_EQ(nc_utf8proc_ssize_t{3},
            nc_utf8proc_decompose(AsBytes(kWithNul), 3, buf.data(), buf.size(),
                                  Opts(0)));
  EXPECT_EQ('a', buf[0]);
  EXPECT_EQ(0, buf[1]);
  EXPECT_EQ('b', buf[2]);

  // Combining class reordering swaps ccc=230 before ccc=202 both at pos==0
  // and pos>0.
  EXPECT_EQ(nc_utf8proc_ssize_t{2},
            nc_utf8proc_decompose(
                AsBytes("\xCC\x81\xCC\xA7"), 0, buf.data(), buf.size(),
                Opts(UTF8PROC_NULLTERM | UTF8PROC_DECOMPOSE)));
  EXPECT_EQ(0x0327, buf[0]);
  EXPECT_EQ(0x0301, buf[1]);

  EXPECT_EQ(nc_utf8proc_ssize_t{3},
            nc_utf8proc_decompose(
                AsBytes("c\xCC\x81\xCC\xA7"), 0, buf.data(), buf.size(),
                Opts(UTF8PROC_NULLTERM | UTF8PROC_DECOMPOSE)));
  EXPECT_EQ('c', buf[0]);
  EXPECT_EQ(0x0327, buf[1]);
  EXPECT_EQ(0x0301, buf[2]);

  // Short bufsize returns required length without touching reordering loop.
  EXPECT_EQ(
      nc_utf8proc_ssize_t{3},
      nc_utf8proc_decompose(AsBytes("c\xCC\x81\xCC\xA7"), 0, buf.data(), 1,
                            Opts(UTF8PROC_NULLTERM | UTF8PROC_DECOMPOSE)));

  // Custom mapping function via nc_utf8proc_map_custom.
  CustomMapState state{};
  nc_utf8proc_uint8_t* mapped = nullptr;
  const nc_utf8proc_ssize_t mapped_len =
      nc_utf8proc_map_custom(AsBytes("ab"), 0, &mapped, UTF8PROC_NULLTERM,
                             &CustomTransformCallback, &state);
  ScopedUtf8Ptr mapped_guard = WrapMalloc(mapped);
  EXPECT_EQ(nc_utf8proc_ssize_t{2}, mapped_len);
  EXPECT_EQ("Zb", AsString(mapped_guard.get()));

  // Stateful callback succeeding on pass 1 ("a" -> 1 call) and failing on
  // pass 2 exercises second-pass buffer cleanup in nc_utf8proc_map_custom.
  CustomMapState fail_pass2{};
  fail_pass2.fail_after_first_pass = true;
  nc_utf8proc_uint8_t* failed_ptr = nullptr;
  const nc_utf8proc_ssize_t fail_status =
      nc_utf8proc_map_custom(AsBytes("a"), 0, &failed_ptr, UTF8PROC_NULLTERM,
                             &CustomTransformCallback, &fail_pass2);
  ScopedUtf8Ptr failed_guard = WrapMalloc(failed_ptr);
  EXPECT_EQ(nc_utf8proc_ssize_t{UTF8PROC_ERROR_NOTASSIGNED}, fail_status);
  EXPECT_EQ(nullptr, failed_guard.get());
  EXPECT_EQ(2, fail_pass2.pass_calls);
}

TEST(Utf8procTest, NormalizeUtf32AndReencodeCoverNlfStripccAndComposition) {
  // NLF conversions (CRLF, LF, CR, NEL, VT, FF) + STRIPCC (TAB + controls).
  {
    std::vector<nc_utf8proc_int32_t> buf = {'a',    '\r',   '\n',   'b',
                                            0x0085, 0x000B, 0x000C, '\t',
                                            0x0001, 0x007F, 0x009F, '\r'};
    const nc_utf8proc_ssize_t len = nc_utf8proc_normalize_utf32(
        buf.data(), static_cast<nc_utf8proc_ssize_t>(buf.size()),
        Opts(UTF8PROC_NLF2LS | UTF8PROC_STRIPCC));
    EXPECT_THAT(
        std::vector<nc_utf8proc_int32_t>(buf.begin(), buf.begin() + len),
        ::testing::ElementsAre('a', 0x2028, 'b', 0x2028, 0x2028, 0x2028, ' ',
                               0x2028));
  }
  {
    std::vector<nc_utf8proc_int32_t> buf = {'\n', 0x0085};
    const nc_utf8proc_ssize_t len = nc_utf8proc_normalize_utf32(
        buf.data(), static_cast<nc_utf8proc_ssize_t>(buf.size()),
        UTF8PROC_NLF2PS);
    EXPECT_THAT(
        std::vector<nc_utf8proc_int32_t>(buf.begin(), buf.begin() + len),
        ::testing::ElementsAre(0x2029, 0x2029));
  }
  {
    std::vector<nc_utf8proc_int32_t> buf = {'\r', '\n', '\r', 'x', '\n'};
    const nc_utf8proc_ssize_t len = nc_utf8proc_normalize_utf32(
        buf.data(), static_cast<nc_utf8proc_ssize_t>(buf.size()),
        UTF8PROC_NLF2LF);
    EXPECT_THAT(
        std::vector<nc_utf8proc_int32_t>(buf.begin(), buf.begin() + len),
        ::testing::ElementsAre('\n', '\n', 'x', '\n'));
  }
  {
    std::vector<nc_utf8proc_int32_t> buf = {'a', '\n', 'b'};
    const nc_utf8proc_ssize_t len = nc_utf8proc_normalize_utf32(
        buf.data(), static_cast<nc_utf8proc_ssize_t>(buf.size()),
        UTF8PROC_STRIPCC);
    EXPECT_THAT(
        std::vector<nc_utf8proc_int32_t>(buf.begin(), buf.begin() + len),
        ::testing::ElementsAre('a', ' ', 'b'));
  }

  // Hangul Jamo composition (L+V -> LV, LV+T -> LVT) and non-combining tails.
  {
    std::vector<nc_utf8proc_int32_t> buf = {0x1100, 0x1161, 0x11A8, 0x11A8,
                                            0x1100, 'a',    0xAC00, 'a'};
    const nc_utf8proc_ssize_t len = nc_utf8proc_normalize_utf32(
        buf.data(), static_cast<nc_utf8proc_ssize_t>(buf.size()),
        UTF8PROC_COMPOSE);
    EXPECT_THAT(
        std::vector<nc_utf8proc_int32_t>(buf.begin(), buf.begin() + len),
        ::testing::ElementsAre(0xAC01, 0x11A8, 0x1100, 'a', 0xAC00, 'a'));
  }

  // Blocked combining mark (equal ccc), out-of-range/sparse combination table
  // misses, 32-bit (>0xFFFF) composition, and UTF8PROC_STABLE exclusion check.
  {
    std::vector<nc_utf8proc_int32_t> buf = {'x',    0x0301, 0x0301, 'J',
                                            0x0301, 'j',    0x0327};
    const nc_utf8proc_ssize_t len = nc_utf8proc_normalize_utf32(
        buf.data(), static_cast<nc_utf8proc_ssize_t>(buf.size()),
        UTF8PROC_COMPOSE);
    EXPECT_THAT(
        std::vector<nc_utf8proc_int32_t>(buf.begin(), buf.begin() + len),
        ::testing::ElementsAre('x', 0x0301, 0x0301, 'J', 0x0301, 'j', 0x0327));
  }
  {
    // Kaithi U+11099 + U+110BA composes to U+1109A (> 16-bit table entry).
    std::vector<nc_utf8proc_int32_t> buf = {0x11099, 0x110BA};
    const nc_utf8proc_ssize_t len = nc_utf8proc_normalize_utf32(
        buf.data(), static_cast<nc_utf8proc_ssize_t>(buf.size()),
        Opts(UTF8PROC_COMPOSE | UTF8PROC_STABLE));
    EXPECT_THAT(
        std::vector<nc_utf8proc_int32_t>(buf.begin(), buf.begin() + len),
        ::testing::ElementsAre(0x1109A));
  }
  {
    // Musical Note U+1D157 + U+1D165 -> U+1D15E is excluded when STABLE is set,
    // and composes when STABLE is omitted.
    std::vector<nc_utf8proc_int32_t> stable_buf = {0x1D157, 0x1D165};
    const nc_utf8proc_ssize_t slen = nc_utf8proc_normalize_utf32(
        stable_buf.data(), 2, Opts(UTF8PROC_COMPOSE | UTF8PROC_STABLE));
    EXPECT_THAT(std::vector<nc_utf8proc_int32_t>(stable_buf.begin(),
                                                 stable_buf.begin() + slen),
                ::testing::ElementsAre(0x1D157, 0x1D165));

    std::vector<nc_utf8proc_int32_t> unstable_buf = {0x1D157, 0x1D165};
    const nc_utf8proc_ssize_t ulen =
        nc_utf8proc_normalize_utf32(unstable_buf.data(), 2, UTF8PROC_COMPOSE);
    EXPECT_THAT(std::vector<nc_utf8proc_int32_t>(unstable_buf.begin(),
                                                 unstable_buf.begin() + ulen),
                ::testing::ElementsAre(0x1D15E));
  }

  // Reencode with UTF8PROC_CHARBOUND across 1..4 byte codepoints plus
  // out-of-range codepoints (-2 and 0x110000) which nc_charbound_encode_char
  // silently skips.
  {
    std::vector<nc_utf8proc_int32_t> buf = {
        -1, 'A', -1, 0x00E9, -1, 0x4E2D, -1, 0x1F600, -2, 0x110000};
    const nc_utf8proc_ssize_t bytes_len = nc_utf8proc_reencode(
        buf.data(), static_cast<nc_utf8proc_ssize_t>(buf.size()),
        UTF8PROC_CHARBOUND);
    const std::string expected = std::string(
        "\xFF"
        "A\xFF\xC3\xA9\xFF\xE4\xB8\xAD\xFF\xF0\x9F\x98\x80");
    ASSERT_EQ(static_cast<nc_utf8proc_ssize_t>(expected.size()), bytes_len);
    EXPECT_EQ(0, std::memcmp(buf.data(), expected.data(), expected.size() + 1));
  }

  // Negative length passed directly to reencode propagates without writes.
  {
    nc_utf8proc_int32_t dummy = 0;
    EXPECT_EQ(nc_utf8proc_ssize_t{-1},
              nc_utf8proc_reencode(&dummy, -1, Opts(0)));
  }
}

TEST(Utf8procTest, NormalizationShortcutsAndCharboundMap) {
  const std::string decomposed_e = std::string("e") + EncodeCp(0x0301);
  const std::string composed_e = EncodeCp(0x00E9);
  const std::string ligature_fi = EncodeCp(0xFB01);
  const std::string sharp_s = EncodeCp(0x00DF);

  ScopedUtf8Ptr nfd = WrapMalloc(nc_utf8proc_NFD(AsBytes(composed_e.c_str())));
  EXPECT_EQ(decomposed_e, AsString(nfd.get()));

  ScopedUtf8Ptr nfc =
      WrapMalloc(nc_utf8proc_NFC(AsBytes(decomposed_e.c_str())));
  EXPECT_EQ(composed_e, AsString(nfc.get()));

  ScopedUtf8Ptr nfkd = WrapMalloc(
      nc_utf8proc_NFKD(AsBytes((ligature_fi + EncodeCp(0x00E9)).c_str())));
  EXPECT_EQ(std::string("fi") + decomposed_e, AsString(nfkd.get()));

  ScopedUtf8Ptr nfkc = WrapMalloc(
      nc_utf8proc_NFKC(AsBytes((ligature_fi + decomposed_e).c_str())));
  EXPECT_EQ(std::string("fi") + composed_e, AsString(nfkc.get()));

  ScopedUtf8Ptr nfkc_cf = WrapMalloc(nc_utf8proc_NFKC_Casefold(
      AsBytes((sharp_s + EncodeCp(0x00AD) + "A").c_str())));
  EXPECT_EQ("ssa", AsString(nfkc_cf.get()));

  // Shortcut helpers return nullptr and nc_utf8proc_map returns error on
  // invalid UTF-8 input.
  constexpr nc_utf8proc_uint8_t kBad[] = {0x80, 0x00, 0x00, 0x00};
  EXPECT_EQ(nullptr, WrapMalloc(nc_utf8proc_NFD(kBad)).get());
  EXPECT_EQ(nullptr, WrapMalloc(nc_utf8proc_NFC(kBad)).get());
  EXPECT_EQ(nullptr, WrapMalloc(nc_utf8proc_NFKD(kBad)).get());
  EXPECT_EQ(nullptr, WrapMalloc(nc_utf8proc_NFKC(kBad)).get());
  EXPECT_EQ(nullptr, WrapMalloc(nc_utf8proc_NFKC_Casefold(kBad)).get());
  nc_utf8proc_uint8_t* err_out = nullptr;
  const nc_utf8proc_ssize_t err_status =
      nc_utf8proc_map(kBad, 0, &err_out, UTF8PROC_NULLTERM);
  ScopedUtf8Ptr err_guard = WrapMalloc(err_out);
  EXPECT_EQ(nc_utf8proc_ssize_t{UTF8PROC_ERROR_INVALIDUTF8}, err_status);
  EXPECT_EQ(nullptr, err_guard.get());

  // Full nc_utf8proc_map round-trip with UTF8PROC_CHARBOUND.
  // UTF8PROC_CHARBOUND writes -1 grapheme-break sentinels into the UTF-32
  // buffer, so it must not be combined with UTF8PROC_COMPOSE or
  // UTF8PROC_DECOMPOSE (which pass buffer entries to nc_unsafe_get_property).
  nc_utf8proc_uint8_t* cb_out = nullptr;
  const std::string cb_in = decomposed_e + "b";
  const nc_utf8proc_ssize_t cb_len =
      nc_utf8proc_map(AsBytes(cb_in.c_str()), 0, &cb_out,
                      Opts(UTF8PROC_NULLTERM | UTF8PROC_CHARBOUND));
  ScopedUtf8Ptr cb_guard = WrapMalloc(cb_out);
  const std::string cb_expected =
      std::string("\xFF") + decomposed_e + "\xFF" + "b";
  EXPECT_EQ(static_cast<nc_utf8proc_ssize_t>(cb_expected.size()), cb_len);
  EXPECT_EQ(cb_expected, AsString(cb_guard.get()));
}

}  // namespace
}  // namespace netcdf
