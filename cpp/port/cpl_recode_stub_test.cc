// Copyright 2026 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests for GDAL's stub recoding implementation (cpl_recode_stub.cpp).

#include <cwchar>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory_closer.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_string.h"

extern void CPLClearRecodeStubWarningFlags();
extern char* CPLRecodeStub(const char* pszSource, const char* pszSrcEncoding,
                           const char* pszDstEncoding);
extern char* CPLRecodeFromWCharStub(const wchar_t* pwszSource,
                                    const char* pszSrcEncoding,
                                    const char* pszDstEncoding);
extern wchar_t* CPLRecodeToWCharStub(const char* pszSource,
                                     const char* pszSrcEncoding,
                                     const char* pszDstEncoding);

namespace gdal {
namespace {

TEST(CplRecodeStubTest, RecodeStubAllEncodingsAndFallbackWarnings) {
  WithQuietHandler quiet_handler;
  CPLClearRecodeStubWarningFlags();

  // 1) Empty source/destination encodings default to ISO-8859-1.
  {
    char* res = CPLRecodeStub("hello", "", "");
    autotest2::CPLMemoryCloser<char> closer(res);
    EXPECT_STREQ(res, "hello");
  }

  // 2) ISO-8859-1 -> UTF-8 (with ASCII and high bytes >= 0x80).
  {
    char* res = CPLRecodeStub("A\xE9\xFF", CPL_ENC_ISO8859_1, CPL_ENC_UTF8);
    autotest2::CPLMemoryCloser<char> closer(res);
    EXPECT_STREQ(res, "A\xC3\xA9\xC3\xBF");
  }

  // 3) UTF-8 -> ISO-8859-1: ASCII, 2-byte Latin1 (\xC3\xA9 -> \xE9), invalid
  // UTF-8 byte (\x80), and > 0xFF codepoint (\xE2\x82\xAC Euro -> '?' + warn).
  {
    char* res = CPLRecodeStub("A\xC3\xA9\x80\xE2\x82\xAC", CPL_ENC_UTF8,
                              CPL_ENC_ISO8859_1);
    autotest2::CPLMemoryCloser<char> closer(res);
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(std::string(res), "A\xE9\x80?");
  }

  // 4) Codepage tables to UTF-8: CP437, CP1250, CP1251, CP1252, ISO-8859-2,
  // ISO-8859-15. Include ASCII, 2-byte, 3-byte, and unmapped bytes (e.g. 0x81
  // in CP1252).
  const char* const kCodepages[] = {"CP437",  "CP1250",     "CP1251",
                                    "CP1252", "ISO-8859-2", "ISO-8859-15"};
  const char kSampleBytes[] = "A\x80\x81\x85\xA0\xE0\xFF";
  for (const char* cp : kCodepages) {
    CPLClearRecodeStubWarningFlags();
    char* res = CPLRecodeStub(kSampleBytes, cp, CPL_ENC_UTF8);
    autotest2::CPLMemoryCloser<char> closer(res);
    ASSERT_NE(res, nullptr);
    EXPECT_TRUE(CPLIsUTF8(res, -1));
  }

  // 5) Unsupported encoding to UTF-8 triggers bHaveWarned1 and Latin1 fallback.
  {
    CPLClearRecodeStubWarningFlags();
    char* res = CPLRecodeStub("abc\xE9", "KOI8-R", CPL_ENC_UTF8);
    autotest2::CPLMemoryCloser<char> closer(res);
    EXPECT_STREQ(res, "abc\xC3\xA9");
  }

  // 6) Non-UTF8 to non-UTF8 triggers bHaveWarned3 and returns raw copy.
  {
    CPLClearRecodeStubWarningFlags();
    char* res = CPLRecodeStub("abc", "CP1252", CPL_ENC_ISO8859_1);
    autotest2::CPLMemoryCloser<char> closer(res);
    EXPECT_STREQ(res, "abc");
  }
}

TEST(CplRecodeStubTest, RecodeFromWCharStub) {
  WithQuietHandler quiet_handler;
  CPLClearRecodeStubWarningFlags();

  // Unsupported source encoding -> nullptr.
  EXPECT_EQ(CPLRecodeFromWCharStub(L"abc", CPL_ENC_ASCII, CPL_ENC_UTF8),
            nullptr);

  // Empty wide string -> "".
  {
    char* res = CPLRecodeFromWCharStub(L"", CPL_ENC_UCS2, CPL_ENC_UTF8);
    autotest2::CPLMemoryCloser<char> closer(res);
    EXPECT_STREQ(res, "");
  }

  // Test all utf8fromwc ranges:
  // < 0x80 ('A'), < 0x800 (0x00E9), < 0x10000 (0x20AC),
  // < 0x110000 (0x1F600), and >= 0x110000 (0x110000 -> replaced with 0xFFFD).
  const wchar_t kWide[] = {L'A',
                           0x00E9,
                           0x20AC,
                           static_cast<wchar_t>(0x1F600),
                           static_cast<wchar_t>(0x110000),
                           0};
  {
    char* res = CPLRecodeFromWCharStub(kWide, "WCHAR_T", CPL_ENC_UTF8);
    autotest2::CPLMemoryCloser<char> closer(res);
    ASSERT_NE(res, nullptr);
    EXPECT_STREQ(res, "A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\xEF\xBF\xBD");
  }

  // Non-UTF-8 destination encoding (converts via CPLRecodeStub to ISO-8859-1).
  const wchar_t kLatin1Wide[] = {L'A', 0x00E9, 0};
  {
    char* res =
        CPLRecodeFromWCharStub(kLatin1Wide, CPL_ENC_UCS4, CPL_ENC_ISO8859_1);
    autotest2::CPLMemoryCloser<char> closer(res);
    EXPECT_STREQ(res, "A\xE9");
  }
}

TEST(CplRecodeStubTest, RecodeToWCharStubAndIsUTF8) {
  WithQuietHandler quiet_handler;
  CPLClearRecodeStubWarningFlags();

  // Unsupported destination encoding (both from UTF-8 and from ISO-8859-1).
  EXPECT_EQ(CPLRecodeToWCharStub("abc", CPL_ENC_UTF8, CPL_ENC_ASCII), nullptr);
  EXPECT_EQ(CPLRecodeToWCharStub("abc", CPL_ENC_ISO8859_1, CPL_ENC_ASCII),
            nullptr);

  // Valid conversions from ISO-8859-1 and UTF-8 to WCHAR_T / UCS2.
  {
    wchar_t* wres =
        CPLRecodeToWCharStub("A\xE9", CPL_ENC_ISO8859_1, CPL_ENC_UCS2);
    autotest2::CPLMemoryCloser<wchar_t> closer(wres);
    ASSERT_NE(wres, nullptr);
    EXPECT_EQ(wres[0], L'A');
    EXPECT_EQ(wres[1], static_cast<wchar_t>(0x00E9));
    EXPECT_EQ(wres[2], 0);
  }

  // Valid 1-, 2-, 3-, and 4-byte UTF-8 sequences in CPLIsUTF8 and
  // CPLRecodeToWCharStub.
  const char* kValidUtf8 =
      "A\xC3\xA9\xE0\xA0\x80\xE2\x82\xAC\xF0\x90\x80\x80\xF1\x80\x80\x80\xF4"
      "\x8F\xBF\xBF";
  EXPECT_TRUE(CPLIsUTF8("pure_ascii", -1));
  EXPECT_TRUE(CPLIsUTF8(kValidUtf8, -1));
  {
    wchar_t* wres = CPLRecodeToWCharStub(kValidUtf8, CPL_ENC_UTF8, "WCHAR_T");
    autotest2::CPLMemoryCloser<wchar_t> closer(wres);
    ASSERT_NE(wres, nullptr);
    EXPECT_EQ(wres[0], L'A');
    EXPECT_EQ(wres[1], static_cast<wchar_t>(0x00E9));
  }

  // Embedded NUL byte with explicit length > 0 returns FALSE in CPLIsUTF8.
  EXPECT_FALSE(CPLIsUTF8("ab\0cd", 5));

  // Exhaustively test every invalid UTF-8 branch in utf8decode via both
  // CPLIsUTF8 and CPLRecodeToWCharStub (including 0x80..0x9F CP1252 fallback).
  const std::string kInvalidUtf8Cases[] = {
      "\x80",              // 0x80..0x9F (CP1252 fallback in utf8decode)
      "\xA0",              // 0xA0..0xBF unexpected continuation byte
      "\xC0\x80",          // 0xC0..0xC1 overlong 2-byte
      "\xC2",              // Truncated 2-byte at end
      "\xC2\x20",          // Invalid 2nd byte in 2-byte
      "\xE0\x80\x80",      // Overlong 3-byte (0xE0 with 2nd byte < 0xA0)
      "\xE2\x82",          // Truncated 3-byte at end
      "\xE2\x82\x20",      // Invalid 3rd byte in 3-byte
      "\xF0\x80\x80\x80",  // Overlong 4-byte (0xF0 with 2nd byte < 0x90)
      "\xF0\x90\x80",      // Truncated 4-byte at end
      "\xF0\x90\x20\x80",  // Invalid 3rd byte in 4-byte
      "\xF0\x90\x80\x20",  // Invalid 4th byte in 4-byte
      "\xF4\x90\x80\x80",  // > U+10FFFF (0xF4 with 2nd byte > 0x8F)
      "\xF5\x80\x80\x80",  // >= 0xF5
  };

  for (const std::string& bad : kInvalidUtf8Cases) {
    EXPECT_FALSE(CPLIsUTF8(bad.c_str(), static_cast<int>(bad.size())));
    wchar_t* wres =
        CPLRecodeToWCharStub(bad.c_str(), CPL_ENC_UTF8, CPL_ENC_UCS4);
    autotest2::CPLMemoryCloser<wchar_t> closer(wres);
    ASSERT_NE(wres, nullptr);
  }
}

}  // namespace
}  // namespace gdal
