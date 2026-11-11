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

// Unit tests for //third_party/netcdf/libdispatch/dutf8.c (UTF-8 validation,
// NFC normalization, and UTF-8 to truncated BMP UTF-16 conversion).
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dutf8.c

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/libdispatch/utf8proc.h"

namespace {
std::atomic<nc_utf8proc_ssize_t> g_forced_iterate_err{0};
std::atomic<nc_utf8proc_ssize_t> g_forced_map_err{0};
}  // namespace

extern "C" {
#include "third_party/netcdf/include/ncutf8.h"

nc_utf8proc_ssize_t __real_nc_utf8proc_iterate(
    const nc_utf8proc_uint8_t* str, nc_utf8proc_ssize_t strlen,
    nc_utf8proc_int32_t* codepoint_ref);
nc_utf8proc_ssize_t __real_nc_utf8proc_map(const nc_utf8proc_uint8_t* str,
                                           nc_utf8proc_ssize_t strlen,
                                           nc_utf8proc_uint8_t** dstptr,
                                           nc_utf8proc_option_t options);

nc_utf8proc_ssize_t __wrap_nc_utf8proc_iterate(
    const nc_utf8proc_uint8_t* str, nc_utf8proc_ssize_t strlen,
    nc_utf8proc_int32_t* codepoint_ref) {
  const nc_utf8proc_ssize_t forced = g_forced_iterate_err.load();
  if (forced < 0) {
    if (codepoint_ref != nullptr) {
      *codepoint_ref = -1;
    }
    return forced;
  }
  return __real_nc_utf8proc_iterate(str, strlen, codepoint_ref);
}

nc_utf8proc_ssize_t __wrap_nc_utf8proc_map(const nc_utf8proc_uint8_t* str,
                                           nc_utf8proc_ssize_t strlen,
                                           nc_utf8proc_uint8_t** dstptr,
                                           nc_utf8proc_option_t options) {
  const nc_utf8proc_ssize_t forced = g_forced_map_err.load();
  if (forced < 0) {
    if (dstptr != nullptr) {
      *dstptr = nullptr;
    }
    return forced;
  }
  return __real_nc_utf8proc_map(str, strlen, dstptr, options);
}
}  // extern "C"

namespace netcdf {
namespace {

const unsigned char* AsUBytes(const char* s) {
  return reinterpret_cast<const unsigned char*>(s);
}

using ScopedUChar = std::unique_ptr<unsigned char, decltype(&std::free)>;
using ScopedUShort = std::unique_ptr<unsigned short, decltype(&std::free)>;

TEST(Dutf8Test, ValidateAcceptsValidAsciiAndMultibyteSequences) {
  constexpr const char* kValid[] = {
      "",
      "simple_ascii_0123",
      "\xC3\xA9",                   // U+00E9 Latin small e with acute (2B)
      "\xE2\x82\xAC",               // U+20AC Euro sign (3B)
      "\xF0\x9F\x98\x80",           // U+1F600 Grinning face (4B)
      "var_\xC3\xA9_\xE2\x82\xAC",  // Mixed ASCII and multibyte UTF-8
      "\xEF\xBF\xBF",               // U+FFFF max BMP codepoint
  };

  for (const char* input : kValid) {
    SCOPED_TRACE(input);
    EXPECT_EQ(NC_NOERR, nc_utf8_validate(AsUBytes(input)));
  }
}

TEST(Dutf8Test, ValidateRejectsMalformedUtf8Sequences) {
  constexpr const char* kInvalid[] = {
      "\x80",                // Unexpected continuation byte
      "\xFF",                // Invalid leading byte
      "\xC0\x80",            // Overlong NUL encoding
      "\xC1\xBF",            // Overlong 2-byte encoding
      "\xC3",                // Truncated 2-byte sequence
      "\xE2\x82",            // Truncated 3-byte sequence
      "\xED\xA0\x80",        // UTF-16 high surrogate U+D800
      "\xF4\x90\x80\x80",    // Codepoint above U+10FFFF
      "prefix_\xFE_suffix",  // Invalid byte mid-string
  };

  for (const char* input : kInvalid) {
    EXPECT_EQ(NC_EBADNAME, nc_utf8_validate(AsUBytes(input)));
  }
}

TEST(Dutf8Test, NormalizeAppliesNfcAndRejectsInvalidUtf8) {
  struct NormCase {
    const char* name;
    const char* input;
    const char* expected_nfc;
  };
  constexpr NormCase kCases[] = {
      {"empty", "", ""},
      {"ascii", "temp_2m", "temp_2m"},
      {"already_nfc", "caf\xC3\xA9", "caf\xC3\xA9"},
      // 'e' + U+0301 COMBINING ACUTE ACCENT -> U+00E9
      {"nfd_latin_to_nfc", "cafe\xCC\x81", "caf\xC3\xA9"},
      // Hangul Jamo L+V (U+1100 U+1161) -> Precomposed syllable U+AC00
      {"hangul_jamo_compose", "\xE1\x84\x80\xE1\x85\xA1", "\xEA\xB0\x80"},
  };

  for (const NormCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    unsigned char* raw_normal = nullptr;
    ASSERT_EQ(NC_NOERR, nc_utf8_normalize(AsUBytes(tc.input), &raw_normal));
    ScopedUChar normal(raw_normal, &std::free);
    ASSERT_NE(nullptr, normal.get());
    EXPECT_EQ(std::string(tc.expected_nfc),
              std::string(reinterpret_cast<const char*>(normal.get())));
  }

  // Invalid UTF-8 returns NC_EBADNAME without leaking.
  unsigned char* err_out = nullptr;
  EXPECT_EQ(NC_EBADNAME,
            nc_utf8_normalize(AsUBytes("bad_\xED\xA0\x80"), &err_out));
  EXPECT_EQ(nullptr, err_out);
}

TEST(Dutf8Test, ToUtf16ConvertsBmpAndRejectsNonBmpOrInvalidUtf8) {
  // Empty string produces empty NUL-terminated UTF-16 output.
  {
    unsigned short* raw_u16 = nullptr;
    size_t len16 = 999;
    ASSERT_EQ(NC_NOERR, nc_utf8_to_utf16(AsUBytes(""), &raw_u16, &len16));
    ScopedUShort u16(raw_u16, &std::free);
    ASSERT_NE(nullptr, u16.get());
    EXPECT_EQ(size_t{0}, len16);
    EXPECT_EQ(0U, u16.get()[0]);
  }

  // ASCII + 2-byte + 3-byte BMP codepoints.
  {
    // "A" (U+0041) + "é" (U+00E9) + "€" (U+20AC) + U+FFFF
    constexpr char kBmpInput[] = "A\xC3\xA9\xE2\x82\xAC\xEF\xBF\xBF";
    unsigned short* raw_u16 = nullptr;
    size_t len16 = 0;
    ASSERT_EQ(NC_NOERR,
              nc_utf8_to_utf16(AsUBytes(kBmpInput), &raw_u16, &len16));
    ScopedUShort u16(raw_u16, &std::free);
    ASSERT_NE(nullptr, u16.get());
    EXPECT_EQ(size_t{4}, len16);
    std::vector<unsigned short> units(u16.get(), u16.get() + len16 + 1);
    constexpr unsigned short kExpected[] = {0x0041U, 0x00E9U, 0x20ACU, 0xFFFFU,
                                            0x0000U};
    EXPECT_THAT(units, ::testing::ElementsAreArray(kExpected));
  }

  // Null optional output pointers must succeed and free internally.
  EXPECT_EQ(NC_NOERR,
            nc_utf8_to_utf16(AsUBytes("ok_\xC3\xA9"), nullptr, nullptr));

  // Valid UTF-8 containing a non-BMP (> 0xFFFF) codepoint must fail cleanly.
  {
    unsigned short* raw_u16 = nullptr;
    size_t len16 = 42;
    EXPECT_EQ(NC_EBADNAME, nc_utf8_to_utf16(AsUBytes("hi\xF0\x9F\x98\x80"),
                                            &raw_u16, &len16));
    EXPECT_EQ(nullptr, raw_u16);
    EXPECT_EQ(size_t{42}, len16);
  }

  // Invalid UTF-8 sequences must return NC_EBADNAME and free internal buffer.
  EXPECT_EQ(NC_EBADNAME,
            nc_utf8_to_utf16(AsUBytes("abc\x80"), nullptr, nullptr));
}

TEST(Dutf8Test, MapsWrappedUtf8procNomemOverflowAndInvalidOptsErrors) {
  absl::Cleanup reset_hooks = [] {
    g_forced_iterate_err.store(0);
    g_forced_map_err.store(0);
  };

  struct ErrorMapCase {
    nc_utf8proc_ssize_t utf8proc_err;
    int expected_nc_err;
  };
  constexpr ErrorMapCase kCases[] = {
      {UTF8PROC_ERROR_NOMEM, NC_ENOMEM},
      {UTF8PROC_ERROR_OVERFLOW, NC_ENOMEM},
      {UTF8PROC_ERROR_INVALIDOPTS, NC_EINVAL},
  };

  for (const ErrorMapCase& tc : kCases) {
    g_forced_iterate_err.store(tc.utf8proc_err);
    EXPECT_EQ(tc.expected_nc_err, nc_utf8_validate(AsUBytes("a")));

    unsigned short* raw_u16 = nullptr;
    size_t len16 = 7;
    EXPECT_EQ(tc.expected_nc_err,
              nc_utf8_to_utf16(AsUBytes("a"), &raw_u16, &len16));
    EXPECT_EQ(nullptr, raw_u16);
    EXPECT_EQ(size_t{7}, len16);
    g_forced_iterate_err.store(0);

    g_forced_map_err.store(tc.utf8proc_err);
    unsigned char* raw_norm = nullptr;
    EXPECT_EQ(tc.expected_nc_err, nc_utf8_normalize(AsUBytes("a"), &raw_norm));
    EXPECT_EQ(nullptr, raw_norm);
    g_forced_map_err.store(0);
  }
}

}  // namespace
}  // namespace netcdf
