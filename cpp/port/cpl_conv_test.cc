// Copyright 2014 Google LLC
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
// Test convenience functions:
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_conv.cpp

#include "third_party/gdal/port/cpl_conv.h"

#include <features.h>
#include <utime.h>

#include <clocale>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>  // NOLINT
#include <latch>  // NOLINT
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <thread>  // NOLINT

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using testing::SrcDir;

class CplConvTest : public testing::Test {
 protected:
  void SetUp() override {
    // Prevent prior tests from impacting the current test.
    CPLErrorReset();
  }
};

TEST_F(CplConvTest, VerifyConfiguration) {
  CPLVerifyConfiguration();
  EXPECT_EQ(0, CPLGetLastErrorNo());
}

// Tests accessing and setting the GDAL runtime configuration system.
TEST_F(CplConvTest, GetSetConfigOption) {
  // Check that 2nd argument default is returned for a non-existent key.
  // Try with a nullptr default.
  EXPECT_EQ(nullptr, CPLGetConfigOption("Does not exist", nullptr));
  // Try with a string default.
  EXPECT_STREQ("mydefault", CPLGetConfigOption("Does not exist", "mydefault"));

  // Check setting and unsetting a key.
  CPLSetConfigOption("TestKey", "abc123");
  EXPECT_STREQ("abc123", CPLGetConfigOption("TestKey", nullptr));
  CPLSetConfigOption("TestKey", nullptr);
  EXPECT_EQ("A default", CPLGetConfigOption("TestKey", "A default"));
  EXPECT_EQ(nullptr, CPLGetConfigOption("TestKey", nullptr));

  // Test thread local without extra threads.
  CPLSetThreadLocalConfigOption("ThreadLocalTestKey", "local0");
  EXPECT_STREQ("local0", CPLGetConfigOption("ThreadLocalTestKey", nullptr));
  CPLSetThreadLocalConfigOption("ThreadLocalTestKey", nullptr);
  EXPECT_EQ(nullptr, CPLGetConfigOption("ThreadLocalTestKey", nullptr));

  // TODO(schwehr): Test CPLSetThreadLocalConfigOption with two threads.
}

// Tests using the portable equivalents to malloc, calloc, and realloc.
//
// There are three cases with the CPLRealloc call:
// * nNewSize is 0: The data pointer is freed if not a nullptr.
// * Casting nNewSize to a long is negative: A good chance for a memory leak.
//     Also tests an error status.
// * nNewSize is >0:  Memory is allocated and data pointer contents are copied.
//     The original memory is freed.
TEST_F(CplConvTest, Memory) {
  WithQuietHandler error_handler;

  EXPECT_EQ(nullptr, CPLMalloc(0));
  EXPECT_EQ(CPLE_None, CPLGetLastErrorNo());

  EXPECT_EQ(nullptr, CPLMalloc(-1));
  EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());

  // Use ASSERT_ (rather than EXPECT_) to assert we are not leaking memory.

  void* ptr = CPLMalloc(1);
  ASSERT_NE(nullptr, ptr);
  CPLFree(ptr);

  ptr = CPLMalloc(999999);
  ASSERT_NE(nullptr, ptr);
  CPLFree(ptr);

  EXPECT_EQ(nullptr, CPLCalloc(10, 0));
  EXPECT_EQ(nullptr, CPLCalloc(0, 10));
  char* char_ptr = static_cast<char*>(CPLCalloc(10, 1));
  for (int i = 0; i < 10; i++) EXPECT_EQ(0, char_ptr[i]);
  CPLFree(char_ptr);

  ASSERT_EQ(nullptr, CPLRealloc(nullptr, 0));
  ptr = CPLRealloc(nullptr, 10);
  ASSERT_NE(nullptr, ptr);
  // Negative size values are invalid and trigger an error.
  ASSERT_EQ(nullptr, CPLRealloc(nullptr, -1));
  ASSERT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
  // Pointer still valid.
  // A realloc of a negative size will leave the ptr contents unchanged.
  // In the next line, do NOT set ptr to the result.  If you do, it will leak!
  ASSERT_EQ(nullptr, CPLRealloc(ptr, -1));
  ASSERT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
  // Pointer still valid.
  ptr = CPLRealloc(ptr, 20);
  ASSERT_NE(nullptr, ptr);
  // Reallocate the same size leading to a copy, but no resize.
  void* ptr2 = CPLRealloc(ptr, 20);
  // The contents of ptr have been freed.
  // TODO(schwehr): Fails with optimized compile, but succeeds with debug.
  //   ASSERT_NE(ptr, ptr2);
  CPLFree(ptr2);

  // Demonstrate leak potential.
  ptr = CPLRealloc(nullptr, 10);
  ptr2 = ptr;                 // Backup the pointer.
  ptr = CPLRealloc(ptr, -1);  // Leak by design.
  ASSERT_EQ(nullptr, ptr);
  CPLFree(ptr2);
  // Not capturing the output of CPLRealloc will also cause a leak.
  // CPLRealloc(ptr, 10);

  // A size of 0 should free the pointer.
  // It is safe to pass a nullptr.
  ptr = CPLRealloc(nullptr, 0);
  ASSERT_EQ(nullptr, ptr);
  ptr = CPLRealloc(ptr, 10);
  // Free a block of data.
  ptr = CPLRealloc(ptr, 0);
  ASSERT_EQ(nullptr, ptr);
}

// Tests using the portable version of strdup and inplace lowercase of a string.
TEST_F(CplConvTest, Strings) {
  // Unlike strdup, CPLStrdup of a nullptr returns the empty string
  // per documentation.
  std::unique_ptr<char, CplFreeDeleter> ptr(CPLStrdup(nullptr));
  EXPECT_STREQ("", ptr.get());

  ptr.reset(CPLStrdup("abc123"));
  EXPECT_STREQ("abc123", ptr.get());

  char str[] = "AbC123";
  // Converts in place to lower case and returns the original pointer.
  EXPECT_EQ(str, CPLStrlwr(str));
  EXPECT_STREQ("abc123", str);
}

// Tests the fgets wrapper.
TEST_F(CplConvTest, CplFGets) {
  EXPECT_EQ(nullptr, CPLFGets(nullptr, 0, nullptr));
  EXPECT_EQ(nullptr, CPLFGets(nullptr, 1, reinterpret_cast<FILE*>(1)));
  EXPECT_EQ(nullptr, CPLFGets(reinterpret_cast<char*>(1), 0,
                              reinterpret_cast<FILE*>(1)));
  EXPECT_EQ(nullptr, CPLFGets(reinterpret_cast<char*>(1), 1, nullptr));

#if _XOPEN_SOURCE >= 700 || _POSIX_C_SOURCE >= 200809L  // fmemopen.
  std::string src_buf("\nfoo\rbar\r\nmuch longer line\n \n\n");
  FILE* src = fmemopen(src_buf.data(), src_buf.size(), "r");
  ASSERT_NE(nullptr, src);
  absl::Cleanup close_src = [src] { fclose(src); };
  constexpr size_t kBufSize = 10;
  char buf[kBufSize];

  EXPECT_STREQ("", CPLFGets(buf, kBufSize, src));
  EXPECT_STREQ("foo", CPLFGets(buf, kBufSize, src));
  EXPECT_STREQ("bar", CPLFGets(buf, kBufSize, src));
  EXPECT_STREQ("much long", CPLFGets(buf, kBufSize, src));
  EXPECT_STREQ("er line", CPLFGets(buf, kBufSize, src));
  EXPECT_STREQ(" ", CPLFGets(buf, kBufSize, src));
  EXPECT_STREQ("", CPLFGets(buf, kBufSize, src));
  EXPECT_STREQ(nullptr, CPLFGets(buf, kBufSize, src));
#endif  // POSIX-2008.1
}

// Tests replacement for CPLFGets that has an internal line buffer.
// CPLReadLine is not thread safe.
TEST_F(CplConvTest, CplReadLine) {
#if _XOPEN_SOURCE >= 700 || _POSIX_C_SOURCE >= 200809L  // fmemopen.
  std::string src_buf("\na\rb\r\nc\n \n");
  FILE* src = fmemopen(src_buf.data(), src_buf.size(), "r");
  ASSERT_NE(nullptr, src);
  absl::Cleanup close_src = [src] { fclose(src); };

  EXPECT_STREQ("", CPLReadLine(src));
  EXPECT_STREQ("a", CPLReadLine(src));
  EXPECT_STREQ("b", CPLReadLine(src));
  EXPECT_STREQ("c", CPLReadLine(src));
  EXPECT_STREQ(" ", CPLReadLine(src));
  EXPECT_STREQ(nullptr, CPLReadLine(src));
#endif  // POSIX-2008.1
}

// Tests returning a length limited copy of a string with optional
// removal of trailing white space and optional replacement of colons
// with under bars. The last 2 integer parameters are treated as booleans.
TEST_F(CplConvTest, CplScanString) {
  EXPECT_EQ(nullptr, CPLScanString(nullptr, 0, 0, 0));

  struct TestCase {
    const char* input;
    int max_len;
    int trim_spaces;
    int normalize;
    std::string_view expected;
  };
  constexpr TestCase kCases[] = {
      {.input = "",
       .max_len = 0,
       .trim_spaces = 0,
       .normalize = 0,
       .expected = ""},
      {.input = " Foo",
       .max_len = 10,
       .trim_spaces = 0,
       .normalize = 0,
       .expected = " Foo"},
      {.input = " FooBar",
       .max_len = 4,
       .trim_spaces = 0,
       .normalize = 0,
       .expected = " Foo"},
      {.input = " Foo ",
       .max_len = 5,
       .trim_spaces = 1,
       .normalize = 0,
       .expected = " Foo"},
      {.input = " :_:_",
       .max_len = 5,
       .trim_spaces = 0,
       .normalize = 1,
       .expected = " ____"},
      {.input = " :_ _:   ",
       .max_len = 6,
       .trim_spaces = 1,
       .normalize = 1,
       .expected = " __ __"},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.input);
    std::unique_ptr<char, CplFreeDeleter> ptr(
        CPLScanString(tc.input, tc.max_len, tc.trim_spaces, tc.normalize));
    ASSERT_NE(nullptr, ptr);
    EXPECT_EQ(tc.expected, ptr.get());
  }
}

// Tests converting a string to an integer. CplScanLong is a wrapper
// around atoi that adds setting a maximum number of characters.
TEST_F(CplConvTest, CplScanLong) {
  struct TestCase {
    const char* input;
    int max_len;
    long expected;  // NOLINT
  };
  constexpr TestCase kCases[] = {
      {.input = "", .max_len = 0, .expected = 0},
      {.input = "9", .max_len = 0, .expected = 0},
      {.input = "a", .max_len = 1, .expected = 0},
      {.input = "+", .max_len = 1, .expected = 0},
      {.input = "-", .max_len = 1, .expected = 0},
      {.input = "0", .max_len = 1, .expected = 0},
      {.input = "0", .max_len = 2, .expected = 0},
      {.input = "1", .max_len = 1, .expected = 1},
      {.input = "1", .max_len = 2, .expected = 1},
      {.input = "12", .max_len = 1, .expected = 1},
      {.input = "+1", .max_len = 1, .expected = 0},
      {.input = "+1", .max_len = 2, .expected = 1},
      {.input = "-1", .max_len = 2, .expected = -1},
      {.input = "-1", .max_len = 3, .expected = -1},
      // Hex and octal are not supported.
      {.input = "0xF", .max_len = 3, .expected = 0},
      {.input = "010", .max_len = 3, .expected = 10},
      {.input = " 2", .max_len = 2, .expected = 2},
      {.input = "\t\n3", .max_len = 3, .expected = 3},
      {.input = "4 ", .max_len = 2, .expected = 4},
      {.input = " 5 ", .max_len = 3, .expected = 5},
      {.input = " 6 1", .max_len = 4, .expected = 6},
      {.input = " 7-", .max_len = 3, .expected = 7},
      {.input = " 8a", .max_len = 3, .expected = 8},
      {.input = " 9.1", .max_len = 4, .expected = 9},
      {.input = "2", .max_len = 20, .expected = 2},
      {.input = "3", .max_len = 200, .expected = 3},
      {.input = "4", .max_len = 2000, .expected = 4},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.input);
    EXPECT_EQ(tc.expected, CPLScanLong(tc.input, tc.max_len));
  }

  if constexpr (sizeof(long) > 4) {  // NOLINT
    EXPECT_EQ(1234567890123, CPLScanLong("1234567890123", 9999));
    EXPECT_EQ(-1234567890123, CPLScanLong("-1234567890123", 9999));
  }
}

// Tests converting string to unsigned long. CplScanULong is a
// wrapper around strtoul that adds setting a maximum number of
// characters.
TEST_F(CplConvTest, CplScanULong) {
  struct TestCase {
    const char* input;
    int max_len;
    unsigned long expected;  // NOLINT
  };
  constexpr TestCase kCases[] = {
      {.input = "", .max_len = 0, .expected = 0},
      {.input = "9", .max_len = 0, .expected = 0},
      {.input = "a", .max_len = 1, .expected = 0},
      {.input = "+", .max_len = 1, .expected = 0},
      {.input = "-", .max_len = 1, .expected = 0},
      {.input = "0", .max_len = 1, .expected = 0},
      {.input = "1", .max_len = 1, .expected = 1},
      {.input = "1", .max_len = 2, .expected = 1},
      {.input = "12", .max_len = 1, .expected = 1},
      {.input = "+1", .max_len = 2, .expected = 1},
      // strtoul parses "-" by negating the unsigned portion.
      {.input = "-1",
       .max_len = 2,
       .expected = static_cast<unsigned long>(-1)},  // NOLINT
      {.input = "-2",
       .max_len = 2,
       .expected = static_cast<unsigned long>(-2)},  // NOLINT
      // Hex and octal are not supported.
      {.input = "0xF", .max_len = 3, .expected = 0},
      {.input = "010", .max_len = 3, .expected = 10},
      {.input = " 2", .max_len = 2, .expected = 2},
      {.input = "\t\n3", .max_len = 3, .expected = 3},
      {.input = "4 ", .max_len = 2, .expected = 4},
      {.input = " 5 ", .max_len = 3, .expected = 5},
      {.input = " 6 1", .max_len = 4, .expected = 6},
      {.input = " 7-", .max_len = 3, .expected = 7},
      {.input = " 8a", .max_len = 3, .expected = 8},
      {.input = " 9.1", .max_len = 4, .expected = 9},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.input);
    EXPECT_EQ(tc.expected, CPLScanULong(tc.input, tc.max_len));
  }

  if constexpr (sizeof(unsigned long) > 4) {  // NOLINT
    EXPECT_EQ(1234567890123, CPLScanULong("1234567890123", 9999));
    EXPECT_EQ(static_cast<unsigned long>(-1234567890123),  // NOLINT
              CPLScanULong("-1234567890123", 9999));
  }
}

// Tests converting strings to GUIntBig.  CPLScanUIntBig is a wrapper
// around _atooi64, atoll or atol depending on platform.
TEST_F(CplConvTest, CPLScanUIntBig) {
  EXPECT_EQ(0, CPLScanUIntBig(nullptr, 0));
  EXPECT_EQ(0, CPLScanUIntBig("", 0));
  EXPECT_EQ(0, CPLScanUIntBig("", 1));
  EXPECT_EQ(0, CPLScanUIntBig("0", 1));
  EXPECT_EQ(0, CPLScanUIntBig("9", 0));
  EXPECT_EQ(0, CPLScanUIntBig("abc123", 10));

  EXPECT_EQ(1, CPLScanUIntBig(" 1", 2));
  EXPECT_EQ(9, CPLScanUIntBig("999", 1));
  EXPECT_EQ(999, CPLScanUIntBig("999", 5));

  EXPECT_LT(1e12, CPLScanUIntBig("-1", 10));
}

// Tests converting a string to a GIntBig.
TEST_F(CplConvTest, CPLAtoGIntBig) {
  EXPECT_EQ(0, CPLAtoGIntBig(""));
  EXPECT_EQ(0, CPLAtoGIntBig("abc123"));
  EXPECT_EQ(123, CPLAtoGIntBig("123"));
  EXPECT_EQ(-123, CPLAtoGIntBig("-123"));
  EXPECT_EQ(9223372036854775807LL, CPLAtoGIntBig("9223372036854775807"));
  EXPECT_EQ(-9223372036854775807LL - 1, CPLAtoGIntBig("-9223372036854775808"));
}

TEST_F(CplConvTest, CPLAtoGIntBigEx) {
  int bOverflow = 0;
  EXPECT_EQ(0, CPLAtoGIntBigEx("", FALSE, &bOverflow));
  EXPECT_EQ(0, bOverflow);
  EXPECT_EQ(0, CPLAtoGIntBigEx("abc123", FALSE, &bOverflow));
  EXPECT_EQ(0, bOverflow);
  EXPECT_EQ(123, CPLAtoGIntBigEx("123", FALSE, &bOverflow));
  EXPECT_EQ(0, bOverflow);
  EXPECT_EQ(-123, CPLAtoGIntBigEx("-123", FALSE, &bOverflow));
  EXPECT_EQ(0, bOverflow);
  EXPECT_EQ(9223372036854775807LL,
            CPLAtoGIntBigEx("9223372036854775807", FALSE, &bOverflow));
  EXPECT_EQ(0, bOverflow);
  EXPECT_EQ(-9223372036854775807LL - 1,
            CPLAtoGIntBigEx("-9223372036854775808", FALSE, &bOverflow));
  EXPECT_EQ(0, bOverflow);

  EXPECT_EQ(9223372036854775807LL,
            CPLAtoGIntBigEx("9223372036854775808", FALSE, &bOverflow));
  EXPECT_EQ(1, bOverflow);
  EXPECT_EQ(-9223372036854775807LL - 1,
            CPLAtoGIntBigEx("-9223372036854775809", FALSE, &bOverflow));
  EXPECT_EQ(1, bOverflow);
}

TEST_F(CplConvTest, CPLScanPointer) {
  EXPECT_EQ(nullptr, CPLScanPointer(nullptr, 0));
  EXPECT_EQ(nullptr, CPLScanPointer("", 0));
  EXPECT_EQ(nullptr, CPLScanPointer("abc123", 10));
  EXPECT_EQ(reinterpret_cast<void*>(0x123), CPLScanPointer("0x123", 10));
  EXPECT_EQ(reinterpret_cast<void*>(0x7b), CPLScanPointer("123", 10));
}

TEST_F(CplConvTest, CPLScanDouble) {
  EXPECT_EQ(0.0, CPLScanDouble(nullptr, 0));
  EXPECT_EQ(0.0, CPLScanDouble("", 0));
  EXPECT_EQ(123.45, CPLScanDouble("123.45", 10));
  EXPECT_EQ(-123.45, CPLScanDouble("-123.45", 10));
  EXPECT_EQ(123, CPLScanDouble("123,45", 10));
  EXPECT_EQ(123.45e+2, CPLScanDouble("123.45e+2", 100));
  EXPECT_EQ(123.45e-2, CPLScanDouble("123.45e-2", 100));
  EXPECT_EQ(123.45E+2, CPLScanDouble("123.45E+2", 100));
  EXPECT_EQ(123.45e2, CPLScanDouble("123.45e2", 100));
}

TEST_F(CplConvTest, CPLPrintString) {
  char output[64] = {};
  EXPECT_EQ(1, CPLPrintString(output, nullptr, sizeof(output) - 1));
  EXPECT_STREQ("", output);

  EXPECT_EQ(0, CPLPrintString(nullptr, "test", 4));

  const char* test_string = "test";
  EXPECT_EQ(4, CPLPrintString(output, test_string, sizeof(output) - 1));
  EXPECT_STREQ(test_string, output);

  EXPECT_EQ(2, CPLPrintString(output, test_string, 2));
  EXPECT_STREQ("test", output);
}

TEST_F(CplConvTest, CPLPrintStringFill) {
  char output[64] = {};
  EXPECT_EQ(63, CPLPrintStringFill(output, nullptr, 63));
  EXPECT_EQ(' ', output[0]);
  EXPECT_EQ(' ', output[62]);
  output[63] = '\0';
  EXPECT_STREQ(
      "                                                               ",
      output);

  EXPECT_EQ(0, CPLPrintStringFill(nullptr, "test", 4));

  const char* test_string = "test";
  EXPECT_EQ(2, CPLPrintStringFill(output, test_string, 6));
  output[6] = '\0';
  EXPECT_STREQ("test  ", output);

  EXPECT_EQ(0, CPLPrintStringFill(output, test_string, 2));
  output[2] = '\0';
  EXPECT_STREQ("te", output);
}

TEST_F(CplConvTest, CPLPrintInt32) {
  char output[64] = {};
  EXPECT_EQ(0, CPLPrintInt32(nullptr, 123, 4));

  EXPECT_EQ(63, CPLPrintInt32(output, 123, 63));
  output[3] = '\0';
  EXPECT_STREQ("   ", output);

  EXPECT_EQ(63, CPLPrintInt32(output, -1234, 63));
  output[4] = '\0';
  EXPECT_STREQ("    ", output);

  EXPECT_EQ(2, CPLPrintInt32(output, 9876, 2));
  output[2] = '\0';
  EXPECT_STREQ("98", output);
}

TEST_F(CplConvTest, CPLPrintUIntBig) {
  char output[64] = {};
  EXPECT_EQ(0, CPLPrintUIntBig(nullptr, 123, 4));

  EXPECT_EQ(63, CPLPrintUIntBig(output, 123, 63));
  output[3] = '\0';
  EXPECT_STREQ("   ", output);

  EXPECT_EQ(63, CPLPrintUIntBig(output, 18446744073709551615ULL, 63));
  output[20] = '\0';
  EXPECT_STREQ("                    ", output);

  EXPECT_EQ(2, CPLPrintUIntBig(output, 9876, 2));
  output[2] = '\0';
  EXPECT_STREQ("98", output);
}

TEST_F(CplConvTest, CPLPrintPointer) {
  char output[64] = {};
  EXPECT_EQ(0, CPLPrintPointer(nullptr, reinterpret_cast<void*>(0x123), 4));

  EXPECT_EQ(5, CPLPrintPointer(output, reinterpret_cast<void*>(0x123), 63));
  output[5] = '\0';
  EXPECT_STREQ("0x123", output);

  EXPECT_EQ(3, CPLPrintPointer(output, reinterpret_cast<void*>(0xabc), 3));
  output[3] = '\0';
  EXPECT_STREQ("0xa", output);
}

TEST_F(CplConvTest, CPLPrintDouble) {
  char output[64] = {};
  EXPECT_EQ(0, CPLPrintDouble(nullptr, "%f", 123.45, nullptr));

  EXPECT_EQ(10, CPLPrintDouble(output, "%f", 123.45, nullptr));
  output[6] = '\0';
  EXPECT_STREQ("123.45", output);

  EXPECT_EQ(10, CPLPrintDouble(output, "%10.2E", 123.45, nullptr));
  output[10] = '\0';
  EXPECT_STREQ("  1.23D+02", output);

  EXPECT_EQ(2, CPLPrintDouble(output, "%.0f", 98.76, nullptr));
  output[4] = '\0';
  EXPECT_STREQ("991.", output);
}

TEST_F(CplConvTest, CPLPrintTime) {
  char output[64] = {};
  struct tm tm{
      .tm_sec = 56,
      .tm_min = 34,
      .tm_hour = 12,
      .tm_mday = 20,
      .tm_mon = 6 - 1,
      .tm_year = 2018 - 1900,
      .tm_wday = 3,   // Wednesday
      .tm_yday = 0,   // unused
      .tm_isdst = 0,  // unused
  };

  int ret = CPLPrintTime(output, sizeof(output) - 1,
                         "%a, %d %b %Y %H:%M:%S GMT", &tm, "C");

  constexpr char kExpected[] = "Wed, 20 Jun 2018 12:34:56 GMT";
  EXPECT_EQ(strlen(kExpected), ret);
  EXPECT_STREQ(kExpected, output);
}

TEST(ConfigOptions, ConfigOptionsGetterAndSetterShouldWorkAsExpected) {
  static const char kTestOption[] = "TEST_CONFIG_OPTION";
  static const char kTestOptionValue[] = "option_value";

  CPLSetConfigOption(kTestOption, kTestOptionValue);
  absl::Cleanup config_remover = [] {
    CPLSetConfigOption(kTestOption, nullptr);
  };

  EXPECT_STREQ(CPLGetConfigOption(kTestOption, nullptr), kTestOptionValue);
}

TEST(ConfigOptions, GetNonExistentConfigOptionShouldReturnDefault) {
  static const char kTestOption[] = "TEST_CONFIG_NON_EXISTENT";
  static const char kTestDefaultValue[] = "option_default_value";

  EXPECT_STREQ(CPLGetConfigOption(kTestOption, kTestDefaultValue),
               kTestDefaultValue);
  EXPECT_EQ(CPLGetConfigOption(kTestOption, nullptr), nullptr);
}

TEST(ConfigOptions, ConfigOptionsBulkGetterAndSetterShouldWorkAsExpected) {
  char** original_config_options = CPLGetConfigOptions();
  absl::Cleanup config_resetter = [original_config_options] {
    CPLSetConfigOptions(original_config_options);
    CSLDestroy(original_config_options);
  };

  static const char kTestOption1[] = "TEST_CONFIG_OPTION_1";
  static const char kTestOption1Value[] = "option_1_value";
  static const char kTestOption2[] = "TEST_CONFIG_OPTION_2";
  static const char kTestOption2Value[] = "option_2_value";
  static const char kTestOptionNonExistent[] = "TEST_CONFIG_NON_EXISTENT";

  CPLSetConfigOption(kTestOption1, kTestOption1Value);
  CPLSetConfigOption(kTestOption2, kTestOption2Value);

  char** config_options = CPLGetConfigOptions();
  absl::Cleanup config_cleanup = [config_options] {
    CSLDestroy(config_options);
  };

  EXPECT_STREQ(CSLFetchNameValue(config_options, kTestOption1),
               kTestOption1Value);
  EXPECT_STREQ(CSLFetchNameValue(config_options, kTestOption2),
               kTestOption2Value);
  EXPECT_EQ(CSLFetchNameValue(config_options, kTestOptionNonExistent), nullptr);
}

TEST(ConfigOptions, ConfigShouldBeGlobal) {
  static constexpr char kTestOption[] = "TEST_CONFIG_OPTION";
  static constexpr char kTestOptionValue[] = "test_option_value";

  absl::Cleanup config_remover = [] {
    CPLSetConfigOption(kTestOption, nullptr);
  };

  std::latch value_set(2);

  auto thread_function = [&](bool should_set) {
    if (should_set) {
      CPLSetConfigOption(kTestOption, kTestOptionValue);
    }

    value_set.arrive_and_wait();

    EXPECT_STREQ(CPLGetConfigOption(kTestOption, nullptr), kTestOptionValue);
  };

  {
    std::jthread thread_1(thread_function, true);
    std::jthread thread_2(thread_function, false);
  }

  EXPECT_STREQ(CPLGetConfigOption(kTestOption, nullptr), kTestOptionValue);
}

TEST(ThreadLocalConfigOptions, GetterAndSetterShouldWorkAsExpected) {
  static constexpr char kTestOption[] = "TEST_CONFIG_OPTION";
  static constexpr char kTestOptionValue[] = "option_value";

  CPLSetThreadLocalConfigOption(kTestOption, kTestOptionValue);
  absl::Cleanup config_remover = [] {
    CPLSetThreadLocalConfigOption(kTestOption, nullptr);
  };

  EXPECT_STREQ(CPLGetThreadLocalConfigOption(kTestOption, nullptr),
               kTestOptionValue);
}

TEST(ThreadLocalConfigOptions, GetNonExistentConfigOptionShouldReturnDefault) {
  constexpr char kTestOption[] = "TEST_CONFIG_NON_EXISTENT";
  constexpr char kTestDefaultValue[] = "option_default_value";

  EXPECT_STREQ(CPLGetThreadLocalConfigOption(kTestOption, kTestDefaultValue),
               kTestDefaultValue);
  EXPECT_EQ(CPLGetThreadLocalConfigOption(kTestOption, nullptr), nullptr);
}

TEST(ThreadLocalConfigOptions, BulkGetterAndSetterShouldWorkAsExpected) {
  char** original_config_options = CPLGetThreadLocalConfigOptions();
  absl::Cleanup config_resetter = [original_config_options] {
    CPLSetThreadLocalConfigOptions(original_config_options);
    CSLDestroy(original_config_options);
  };

  constexpr char kTestOption1[] = "TEST_CONFIG_OPTION_1";
  constexpr char kTestOption1Value[] = "option_1_value";
  constexpr char kTestOption2[] = "TEST_CONFIG_OPTION_2";
  constexpr char kTestOption2Value[] = "option_2_value";
  constexpr char kTestOptionNonExistent[] = "TEST_CONFIG_NON_EXISTENT";

  CPLSetThreadLocalConfigOption(kTestOption1, kTestOption1Value);
  CPLSetThreadLocalConfigOption(kTestOption2, kTestOption2Value);

  StringListPtr config_options(CPLGetThreadLocalConfigOptions(), CSLDestroy);

  EXPECT_STREQ(CSLFetchNameValue(config_options.get(), kTestOption1),
               kTestOption1Value);
  EXPECT_STREQ(CSLFetchNameValue(config_options.get(), kTestOption2),
               kTestOption2Value);
  EXPECT_EQ(CSLFetchNameValue(config_options.get(), kTestOptionNonExistent),
            nullptr);
}

TEST(ThreadLocalConfigOptions, ConfigShouldBeThreadLocal) {
  static constexpr char kTestOption[] = "TEST_CONFIG_OPTION";
  static constexpr char kTestOptionThread1Value[] = "option_value_1";
  static constexpr char kTestOptionThread2Value[] = "option_value_2";

  std::latch values_set(2);

  auto thread_function = [&](const char* value) {
    CPLSetThreadLocalConfigOption(kTestOption, value);
    absl::Cleanup cleanup = [] {
      CPLSetThreadLocalConfigOption(kTestOption, nullptr);
    };

    values_set.arrive_and_wait();

    EXPECT_STREQ(CPLGetThreadLocalConfigOption(kTestOption, nullptr), value);
  };

  {
    std::jthread thread_1(thread_function, kTestOptionThread1Value);
    std::jthread thread_2(thread_function, kTestOptionThread2Value);
  }

  EXPECT_STREQ(CPLGetThreadLocalConfigOption(kTestOption, nullptr), nullptr);
}

TEST_F(CplConvTest, CPLStat) {
  constexpr int kFailure = -1;

  VSIStatBuf buf = {};
  EXPECT_EQ(kFailure, CPLStat("c:", &buf));
}

TEST_F(CplConvTest, CPLDMSToDec) {
  // nullptr crashes.
  struct TestCase {
    const char* input;
    double expected;
  };
  constexpr TestCase kCases[] = {
      {.input = "", .expected = 0},
      {.input = " ", .expected = 0},
      {.input = "a", .expected = 0},
      {.input = "  0d 0' 0.9720\"N", .expected = 0},
      {.input = "  0d 0' 0.9720\"S", .expected = 0},
      {.input = "1", .expected = 1},
      {.input = "1N", .expected = 1},
      {.input = "1n", .expected = 1},
      {.input = "+1", .expected = 1},
      {.input = "\t\n\r 1 ", .expected = 1},
      {.input = "-1", .expected = -1},
      {.input = "1S", .expected = -1},
      {.input = "1s", .expected = -1},
      {.input = "1E", .expected = 1},
      {.input = "1e", .expected = 1},
      {.input = "1W", .expected = -1},
      {.input = "1w", .expected = -1},
      {.input = "2d", .expected = 2},
      // These expected values pin upstream CPLDMSToDec's inexact
      // decimal-minute/second scaling.
      {.input = "3'", .expected = 0.0500000000001},
      {.input = "4\"", .expected = 0.0011111111200},
      {.input = "+1\"", .expected = 0.00027777778},
      {.input = "-1\"", .expected = -0.00027777778},
      {.input = "9.87r", .expected = 9.87},
      {.input = "-9.87R", .expected = -9.87},
      {.input = "2e5R", .expected = 200000},
      {.input = " 1R ", .expected = 1},
      {.input = "R", .expected = 0},
      {.input = " R ", .expected = 0},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.input);
    EXPECT_DOUBLE_EQ(tc.expected, CPLDMSToDec(tc.input));
  }
}

TEST_F(CplConvTest, CPLDecToDMS) {
  struct TestCase {
    double angle;
    const char* axis;
    int precision;
    std::string_view expected;
  };
  constexpr TestCase kCases[] = {
      {.angle = std::numeric_limits<double>::quiet_NaN(),
       .axis = "",
       .precision = 0,
       .expected = "Invalid angle"},
      {.angle = std::numeric_limits<double>::infinity(),
       .axis = "",
       .precision = 0,
       .expected = "Invalid angle"},
      {.angle = -std::numeric_limits<double>::infinity(),
       .axis = "",
       .precision = 0,
       .expected = "Invalid angle"},
      {.angle = 361.1, .axis = "", .precision = 0, .expected = "Invalid angle"},
      {.angle = -361.1,
       .axis = "",
       .precision = 0,
       .expected = "Invalid angle"},
      {.angle = 360.0, .axis = "", .precision = 0, .expected = "360d 0'  0\"N"},
      {.angle = 360.0,
       .axis = "Long",
       .precision = 0,
       .expected = "360d 0'  0\"E"},
      {.angle = -360.0,
       .axis = "",
       .precision = 0,
       .expected = "360d 0'  0\"S"},
      {.angle = -360.0,
       .axis = "Long",
       .precision = 0,
       .expected = "360d 0'  0\"W"},
      {.angle = 90.0, .axis = "", .precision = 0, .expected = " 90d 0'  0\"N"},
      {.angle = -90.0, .axis = "", .precision = 0, .expected = " 90d 0'  0\"S"},
      // Just under 1 minute.
      {.angle = 0.016, .axis = "", .precision = 0, .expected = "  0d 0' 58\"N"},
      {.angle = -0.016,
       .axis = "",
       .precision = 0,
       .expected = "  0d 0' 58\"S"},
      {.angle = 0.016,
       .axis = "",
       .precision = 1,
       .expected = "  0d 0'57.6\"N"},
      {.angle = -0.016,
       .axis = "",
       .precision = 1,
       .expected = "  0d 0'57.6\"S"},
      {.angle = 0.016,
       .axis = "",
       .precision = 2,
       .expected = "  0d 0'57.60\"N"},
      {.angle = -0.016,
       .axis = "",
       .precision = 2,
       .expected = "  0d 0'57.60\"S"},
      // Just under 1 second.
      {.angle = 0.00027,
       .axis = "",
       .precision = 2,
       .expected = "  0d 0' 0.97\"N"},
      {.angle = -0.00027,
       .axis = "",
       .precision = 2,
       .expected = "  0d 0' 0.97\"S"},
      {.angle = 0.00027,
       .axis = "",
       .precision = 4,
       .expected = "  0d 0' 0.9720\"N"},
      {.angle = -0.00027,
       .axis = "",
       .precision = 4,
       .expected = "  0d 0' 0.9720\"S"},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.expected);
    EXPECT_EQ(tc.expected, CPLDecToDMS(tc.angle, tc.axis, tc.precision));
  }
}

TEST_F(CplConvTest, CPLPackedDMSToDec) {
  struct TestCase {
    double packed;
    double expected;
  };
  constexpr TestCase kCases[] = {
      {.packed = 720000000.0, .expected = 720.0},
      {.packed = 1000000.0, .expected = 1.0},
      {.packed = 6000.0, .expected = 0.1},
      {.packed = 0.36, .expected = 0.0001},
      {.packed = -0.36, .expected = -0.0001},
      {.packed = -6000.0, .expected = -0.1},
      {.packed = -1000000.0, .expected = -1.0},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.packed);
    EXPECT_DOUBLE_EQ(tc.expected, CPLPackedDMSToDec(tc.packed));
  }
}

TEST_F(CplConvTest, CPLDecToPackedDMS) {
  struct TestCase {
    double dec;
    double expected;
  };
  constexpr TestCase kCases[] = {
      {.dec = 0.0, .expected = 0.0},
      {.dec = 720.0, .expected = 720000000.0},
      {.dec = 360.0, .expected = 360000000.0},
      {.dec = 1.0, .expected = 1000000.0},
      {.dec = 0.1, .expected = 6000.0},
      {.dec = 0.01, .expected = 36.0},
      {.dec = 0.001, .expected = 3.6},
      {.dec = 0.0001, .expected = 0.36},
      {.dec = 0.00001, .expected = 0.036},
      {.dec = 0.000001, .expected = 0.0036},
      {.dec = 0.0000001, .expected = 0.00036},
      {.dec = -0.0000001, .expected = -0.00036},
      {.dec = -1.0, .expected = -1000000.0},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.dec);
    EXPECT_DOUBLE_EQ(tc.expected, CPLDecToPackedDMS(tc.dec));
  }
}

// TODO(schwehr): CPLStringToComplex
// TODO(schwehr): CPLOpenShared / CPLCloseShared
// TODO(schwehr): CPLCleanupSharedFileMutex
// TODO(schwehr): CPLGetSharedList
// TODO(schwehr): CPLDumpSharedList
// TODO(schwehr): CPLUnlinkTree
// TODO(schwehr): CPLCopyFile

constexpr int kFailure = -1;
constexpr int kSuccess = 0;

TEST_F(CplConvTest, CPLCopyTreeSourceDoesNotExist) {
  WithQuietHandler error_handler;
  EXPECT_EQ(kFailure, CPLCopyTree("/vsimem/does/not/exist", "/vsimem/nowhere"));
  EXPECT_THAT(CPLGetLastErrorMsg(),
              testing::HasSubstr("no file system object"));
}

TEST_F(CplConvTest, CPLCopyTree) {
  static constexpr char kDirpath[] = "/vsimem/cplcopytree";
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kDirpath); };
  const std::string subdir = CPLFormFilename(kDirpath, "subdir", nullptr);
  ASSERT_EQ(kSuccess, VSIMkdir(subdir.c_str(), 0755));
  const std::string subfile =
      CPLFormFilename(subdir.c_str(), "subfile", nullptr);
  const std::string data = "abc";
  {
    VSILFILE* file = VSIFOpenL(subfile.c_str(), "wb");
    ASSERT_NE(nullptr, file);
    absl::Cleanup close_file = [file] {
      EXPECT_EQ(kSuccess, VSIFCloseL(file));
    };
    EXPECT_EQ(1, VSIFWriteL(data.c_str(), data.length(), 1, file));
  }

  const std::string destdir = CPLFormFilename(kDirpath, "dest", nullptr);
  EXPECT_EQ(kSuccess, CPLCopyTree(destdir.c_str(), subdir.c_str()));
  const std::string dest_subfile =
      CPLFormFilename(destdir.c_str(), "subfile", nullptr);

  {
    VSILFILE* file = VSIFOpenL(dest_subfile.c_str(), "r");
    ASSERT_NE(nullptr, file);
    absl::Cleanup close_file = [file] {
      EXPECT_EQ(kSuccess, VSIFCloseL(file));
    };
    constexpr int kMaxSize = 5;
    char buf[kMaxSize] = {};
    EXPECT_EQ(data.length(), VSIFReadL(buf, 1, kMaxSize - 1, file));
  }

  // Does not allow writing over self.
  WithQuietHandler error_handler;
  EXPECT_EQ(kFailure, CPLCopyTree(subdir.c_str(), subdir.c_str()));
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("already exists"));
}

// Tests looking for files with a list.
TEST_F(CplConvTest, CPLCheckForFileWithFileList) {
  char kFilename[] = "Aa";
  char* files[2] = {kFilename, nullptr};

  constexpr char kDoesNotExist[] = "does-not-exist";
  std::unique_ptr<char, CplFreeDeleter> filename(CPLStrdup(kDoesNotExist));
  EXPECT_FALSE(CPLCheckForFile(filename.get(), files));
  EXPECT_STREQ(kDoesNotExist, filename.get());

  filename.reset(CPLStrdup(kFilename));
  EXPECT_TRUE(CPLCheckForFile(filename.get(), files));
  EXPECT_STREQ(kFilename, filename.get());

  constexpr char kLower[] = "aa";
  filename.reset(CPLStrdup(kLower));
  EXPECT_TRUE(CPLCheckForFile(filename.get(), files));
  EXPECT_STREQ(kFilename, filename.get());

  constexpr char kUpper[] = "AA";
  filename.reset(CPLStrdup(kUpper));
  EXPECT_TRUE(CPLCheckForFile(filename.get(), files));
  EXPECT_STREQ(kFilename, filename.get());
}

// Tests looking for files without a filelist.
// Probes for the actual file.
TEST_F(CplConvTest, CPLCheckForFileVsimemFiles) {
  static constexpr char kFilename[] = "/vsimem/CheckForFile";
  constexpr char kLower[] = "/vsimem/checkforfile";
  constexpr char kUpper[] = "/vsimem/CHECKFORFILE";

  std::unique_ptr<char, CplFreeDeleter> filename(CPLStrdup(kFilename));
  EXPECT_FALSE(CPLCheckForFile(filename.get(), nullptr));
  EXPECT_STREQ(kFilename, filename.get());

  {
    // Create an empty file.
    VSILFILE* file = VSIFOpenL(filename.get(), "wb");
    ASSERT_NE(nullptr, file);
    VSIFCloseL(file);
  }
  absl::Cleanup remove_file = [] { VSIUnlink(kFilename); };
  EXPECT_TRUE(CPLCheckForFile(filename.get(), nullptr));
  EXPECT_STREQ(kFilename, filename.get());

  // The /vsimem filesystem is case sensitive.
  // Mismatches return false and do not alter the filename.
  filename.reset(CPLStrdup(kLower));
  EXPECT_FALSE(CPLCheckForFile(filename.get(), nullptr));
  EXPECT_STREQ(kLower, filename.get());

  filename.reset(CPLStrdup(kUpper));
  EXPECT_FALSE(CPLCheckForFile(filename.get(), nullptr));
  EXPECT_STREQ(kUpper, filename.get());
}

TEST_F(CplConvTest, CPLIsPowerOfTwo) {
  EXPECT_FALSE(CPLIsPowerOfTwo(0));
  EXPECT_TRUE(CPLIsPowerOfTwo(1));
  EXPECT_TRUE(CPLIsPowerOfTwo(2));
  EXPECT_FALSE(CPLIsPowerOfTwo(3));
  EXPECT_TRUE(CPLIsPowerOfTwo(8));
  EXPECT_FALSE(CPLIsPowerOfTwo(18));

  EXPECT_TRUE(CPLIsPowerOfTwo(1U << 31));
  EXPECT_FALSE(CPLIsPowerOfTwo((1U << 31) + 1));

  EXPECT_FALSE(CPLIsPowerOfTwo(std::numeric_limits<unsigned int>::max() - 1));
  EXPECT_FALSE(CPLIsPowerOfTwo(std::numeric_limits<unsigned int>::max()));
}

constexpr std::string_view kTestConfigsDirectory =
    "google3/third_party/gdal/autotest2/cpp/port/testdata/config";

std::filesystem::path GetTestGdalConfig(std::string_view filename) {
  return std::filesystem::path(SrcDir()) / kTestConfigsDirectory / filename;
}

TEST(CplLoadConfigOptionsFromFile, Basic) {
  const auto path = GetTestGdalConfig("basic.gdalrc");

  CPLLoadConfigOptionsFromFile(path.c_str(), false);
  absl::Cleanup config_cleaner = [] {
    CPLSetConfigOption("FOO_CONFIGOPTION", nullptr);
  };

  ASSERT_STREQ(CPLGetConfigOption("FOO_CONFIGOPTION", ""), "BAR");
}

TEST(CplLoadConfigOptionsFromFile, NonExistentFileNoErrors) {
  const auto path = GetTestGdalConfig("nonexistent.gdalrc");

  CPLLoadConfigOptionsFromFile(path.c_str(), false);
}

TEST(CplLoadConfigOptionsFromPredefinedFiles, UseConfigOptionGdalConfigFile) {
  const auto path = GetTestGdalConfig("basic.gdalrc");
  CPLConfigOptionSetter config_file("GDAL_CONFIG_FILE", path.c_str(),
                                    /*bSetOnlyIfUndefined=*/false);

  CPLLoadConfigOptionsFromPredefinedFiles();
  absl::Cleanup config_cleaner = [] {
    CPLSetConfigOption("FOO_CONFIGOPTION", nullptr);
  };

  ASSERT_STREQ(CPLGetConfigOption("FOO_CONFIGOPTION", ""), "BAR");
}

TEST(CplLoadConfigOptionsFromPredefinedFiles, UseHomeDirGdalConfigFile) {
  // GDAL_CONFIG_FILE must be nullptr for fallback to home directory.
  CPLConfigOptionSetter config_file("GDAL_CONFIG_FILE", nullptr,
                                    /*bSetOnlyIfUndefined=*/false);

#ifdef _WIN32
  constexpr char kHomeEnvVarName[] = "USERPROFILE";
#else
  constexpr char kHomeEnvVarName[] = "HOME";
#endif

  // Setup home directory using config options to test configs directory.
  const auto path = GetTestGdalConfig("");
  CPLConfigOptionSetter home_config(kHomeEnvVarName, path.c_str(),
                                    /*bSetOnlyIfUndefined=*/false);

  CPLLoadConfigOptionsFromPredefinedFiles();
  absl::Cleanup config_cleaner = [] {
    CPLSetConfigOption("FOO_CONFIGOPTION", nullptr);
  };

  ASSERT_STREQ(CPLGetConfigOption("FOO_CONFIGOPTION", ""), "DOT_GDALRC");
}

void WriteConfig(const char* filename, std::string_view contents) {
  VSILFILE* fp = VSIFOpenL(filename, "wb");
  ASSERT_NE(fp, nullptr);
  absl::Cleanup fp_closer = [fp] { VSIFCloseL(fp); };

  size_t count = VSIFWriteL(contents.data(), 1, contents.size(), fp);
  ASSERT_EQ(count, contents.size());
}

TEST(CplLoadConfigOptionsFromFileCredentialSectionParsing,
     PathSpecificOptions) {
  constexpr std::string_view gdalrc_contents = R"gdalrc(
[credentials]

[.foobar_subsection]
path=/vsi_test/foo/bar
TEST_KEY1=FOOBAR_VAL1
TEST_KEY2=FOOBAR_VAL2

[.barbaz_subsection]
path=/vsi_test/bar/baz
TEST_KEY1=BARBAZ_VAL1
TEST_KEY2=BARBAZ_VAL2

[configoptions]
TEST_KEY1=CONFIGOPTIONS_VAL1
TEST_KEY2=CONFIGOPTIONS_VAL2
)gdalrc";

  static constexpr char filename[] = "/vsimem/credentials";
  WriteConfig(filename, gdalrc_contents);
  absl::Cleanup config_remover = [] { VSIUnlink(filename); };

  static constexpr char foobar_path[] = "/vsi_test/foo/bar";
  static constexpr char barbaz_path[] = "/vsi_test/bar/baz";

  CPLErrorReset();
  CPLLoadConfigOptionsFromFile(filename, false);
  absl::Cleanup clear_options = [] {
    VSIClearPathSpecificOptions(foobar_path);
    VSIClearPathSpecificOptions(barbaz_path);
    CPLSetConfigOption("TEST_KEY1", nullptr);
    CPLSetConfigOption("TEST_KEY2", nullptr);
  };
  EXPECT_EQ(CPLGetLastErrorType(), CE_None);

  EXPECT_STREQ(VSIGetPathSpecificOption(foobar_path, "TEST_KEY1", nullptr),
               "FOOBAR_VAL1");
  EXPECT_STREQ(VSIGetPathSpecificOption(foobar_path, "TEST_KEY2", nullptr),
               "FOOBAR_VAL2");

  EXPECT_STREQ(VSIGetPathSpecificOption(barbaz_path, "TEST_KEY1", nullptr),
               "BARBAZ_VAL1");
  EXPECT_STREQ(VSIGetPathSpecificOption(barbaz_path, "TEST_KEY2", nullptr),
               "BARBAZ_VAL2");

  EXPECT_STREQ(CPLGetConfigOption("TEST_KEY1", nullptr), "CONFIGOPTIONS_VAL1");
  EXPECT_STREQ(CPLGetConfigOption("TEST_KEY2", nullptr), "CONFIGOPTIONS_VAL2");
}

TEST(CplLoadConfigOptionsFromFileCredentialSectionParsing,
     WarningContentOutsideSubsection) {
  constexpr std::string_view gdalrc_contents = R"gdalrc(
[credentials]

path=/vsi_test/foo/bar
TEST_CREDENTIALS_OUTSIDE_SUBSECTION_KEY=VAL
)gdalrc";

  static constexpr char filename[] = "/vsimem/credentials";
  WriteConfig(filename, gdalrc_contents);
  absl::Cleanup config_remover = [] { VSIUnlink(filename); };

  CPLErrorReset();
  {
    WithQuietHandler silence_warnings;
    CPLLoadConfigOptionsFromFile(filename, false);
  }

  EXPECT_EQ(CPLGetLastErrorType(), CE_Warning);
  EXPECT_STREQ(CPLGetLastErrorMsg(),
               "Ignoring content in [credential] section that is not in a "
               "[.xxxxx] subsection");
  EXPECT_EQ(CPLGetLastErrorNo(), CPLE_AppDefined);

  EXPECT_EQ(VSIGetPathSpecificOption("/vsi_test/foo/bar",
                                     "TEST_CREDENTIALS_OUTSIDE_SUBSECTION_KEY",
                                     nullptr),
            nullptr);

  EXPECT_EQ(
      CPLGetConfigOption("TEST_CREDENTIALS_OUTSIDE_SUBSECTION_KEY", nullptr),
      nullptr);
}

TEST(CplLoadConfigOptionsFromFileCredentialSectionParsing,
     WarningSubsectionFirstKeyIsPath) {
  constexpr std::string_view gdalrc_contents = R"gdalrc(
[credentials]

[.test_subsection]
TEST_CREDENTIALS_SUBSECTION_KEY=VAL
)gdalrc";

  static constexpr char filename[] = "/vsimem/credentials";
  WriteConfig(filename, gdalrc_contents);
  absl::Cleanup config_remover = [] { VSIUnlink(filename); };

  CPLErrorReset();
  {
    WithQuietHandler silence_warnings;
    CPLLoadConfigOptionsFromFile(filename, false);
  }

  EXPECT_EQ(CPLGetLastErrorType(), CE_Warning);
  EXPECT_STREQ(CPLGetLastErrorMsg(),
               "First entry in a credentials subsection should be 'path'.");
  EXPECT_EQ(CPLGetLastErrorNo(), CPLE_AppDefined);

  EXPECT_EQ(
      VSIGetPathSpecificOption("/vsi_test/foo/bar",
                               "TEST_CREDENTIALS_SUBSECTION_KEY", nullptr),
      nullptr);

  EXPECT_EQ(CPLGetConfigOption("TEST_CREDENTIALS_SUBSECTION_KEY", nullptr),
            nullptr);
}

TEST(CplLoadConfigOptionsFromFileCredentialSectionParsing,
     WarningDuplicatePath) {
  constexpr std::string_view gdalrc_contents = R"gdalrc(
[credentials]

[.test_subsection]
path=/vsi_test/foo
path=/vsi_test/bar
TEST_CREDENTIALS_SUBSECTION_KEY=VAL
)gdalrc";

  static constexpr char filename[] = "/vsimem/credentials";
  WriteConfig(filename, gdalrc_contents);
  absl::Cleanup config_remover = [] { VSIUnlink(filename); };

  CPLErrorReset();
  {
    WithQuietHandler silence_warnings;
    CPLLoadConfigOptionsFromFile(filename, false);
  }
  absl::Cleanup clear_options = [] {
    VSIClearPathSpecificOptions("/vsi_test/foo");
  };

  EXPECT_EQ(CPLGetLastErrorType(), CE_Warning);
  EXPECT_STREQ(CPLGetLastErrorMsg(),
               "Duplicated 'path' key in the same subsection. Ignoring "
               "path=/vsi_test/bar");
  EXPECT_EQ(CPLGetLastErrorNo(), CPLE_AppDefined);

  // TEST_CREDENTIALS_SUBSECTION_KEY=VAL should be set up for /vsi_test/foo.
  EXPECT_STREQ(VSIGetPathSpecificOption(
                   "/vsi_test/foo", "TEST_CREDENTIALS_SUBSECTION_KEY", nullptr),
               "VAL");

  // But not for the ignored /vsi_test/bar.
  EXPECT_EQ(VSIGetPathSpecificOption(
                "/vsi_test/bar", "TEST_CREDENTIALS_SUBSECTION_KEY", nullptr),
            nullptr);

  // And not generically.
  EXPECT_EQ(CPLGetConfigOption("TEST_CREDENTIALS_SUBSECTION_KEY", nullptr),
            nullptr);
}

TEST(CplLoadConfigOptionsFromFileParsing, UnknownSectionsIgnored) {
  constexpr std::string_view gdalrc_contents = R"gdalrc(
[unknown]
TEST_UNKNOWN_KEY=UNKNOWN_VAL

[configoptions]
TEST_CONFIG_KEY=CONFIG_VAL
)gdalrc";

  static constexpr char filename[] = "/vsimem/config";
  WriteConfig(filename, gdalrc_contents);
  absl::Cleanup config_remover = [] { VSIUnlink(filename); };

  CPLErrorReset();
  CPLLoadConfigOptionsFromFile(filename, false);
  absl::Cleanup clear_options = [] {
    CPLSetConfigOption("TEST_CONFIG_KEY", nullptr);
  };

  EXPECT_STREQ(CPLGetConfigOption("TEST_CONFIG_KEY", nullptr), "CONFIG_VAL");
  EXPECT_EQ(CPLGetConfigOption("TEST_UNKNOWN_KEY", nullptr), nullptr);
}

struct DownCastTestBase {};
struct DownCastTestDerived : DownCastTestBase {};

TEST(DownCast, WillCastDerivedToBaseClass) {
  DownCastTestDerived derived;
  DownCastTestBase* p_derived = &derived;

  ASSERT_EQ(cpl::down_cast<DownCastTestDerived*>(p_derived), &derived);
}

TEST(DownCast, WillCastDerivedToBaseClassWorksOnNullptrs) {
  ASSERT_EQ(cpl::down_cast<DownCastTestDerived*>(
                static_cast<DownCastTestBase*>(nullptr)),
            static_cast<DownCastTestDerived*>(nullptr));
}

TEST(CPLFormatReadableFileSize, Uint64) {
  struct TestCase {
    uint64_t size;
    std::string_view expected;
  };
  constexpr TestCase kCases[] = {
      {.size = 0ULL, .expected = "000,000 bytes"},
      {.size = 1ULL, .expected = "000,001 bytes"},
      {.size = 1000000ULL, .expected = "1000,000 bytes"},
      {.size = 1234567ULL, .expected = "1.23 MB"},
      {.size = 1000000000ULL, .expected = "1000.00 MB"},
      {.size = 1234567890ULL, .expected = "1.23 GB"},
      {.size = 1000000000000ULL, .expected = "1000.00 GB"},
      {.size = 1234567890123ULL, .expected = "1.23 TB"},
      {.size = 1000000000000000ULL, .expected = "1000.00 TB"},
      {.size = 1234567890123456ULL, .expected = "1.23 PB"},
      {.size = 1000000000000000000ULL, .expected = "1000.00 PB"},
      {.size = std::numeric_limits<uint64_t>::max(), .expected = "18.45 HB"},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.expected);
    EXPECT_EQ(tc.expected, CPLFormatReadableFileSize(tc.size));
  }
}

TEST(CPLFormatReadableFileSize, Double) {
  struct TestCase {
    double size;
    std::string_view expected;
  };
  constexpr TestCase kCases[] = {
      {.size = 0.0, .expected = "000,000 bytes"},
      {.size = 1.0, .expected = "000,001 bytes"},
      {.size = 1000000.0, .expected = "1000,000 bytes"},
      {.size = 1234567.0, .expected = "1.23 MB"},
      {.size = 1000000000.0, .expected = "1000.00 MB"},
      {.size = 1234567890.0, .expected = "1.23 GB"},
      {.size = 1000000000000.0, .expected = "1000.00 GB"},
      {.size = 1234567890123.0, .expected = "1.23 TB"},
      {.size = 1000000000000000.0, .expected = "1000.00 TB"},
      {.size = 1234567890123456.0, .expected = "1.23 PB"},
      {.size = 1000000000000000000.0, .expected = "1000.00 PB"},
      {.size = std::numeric_limits<double>::max(),
       .expected =
           "179769313486231569011752584012189387881197018537899519684975567093"
           "221424140287316977399479372650156877497075426045784429462895627128"
           "692503729547397989056174152722947655352627933902673885763380310970"
           "863459777311366506329324386668754953244789789343800952415837295514"
           "371927114812537179201339392.00 HB"},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.expected);
    EXPECT_EQ(tc.expected, CPLFormatReadableFileSize(tc.size));
  }
}

TEST_F(CplConvTest, ReadLineAndFGetsEdgeCases) {
  WithQuietHandler quiet;
  EXPECT_EQ(nullptr, CPLStrlwr(nullptr));
  EXPECT_EQ(nullptr, CPLReadLine(nullptr));
  EXPECT_EQ(nullptr, CPLReadLineL(nullptr));

  const std::string temp_dir(::testing::TempDir());
  const std::string local_file = temp_dir + "/readline_test.txt";
  absl::Cleanup unlink_local = [&local_file] { VSIUnlink(local_file.c_str()); };
  {
    FILE* fp = fopen(local_file.c_str(), "wb");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup close_fp = [fp] { fclose(fp); };
    // 1. Short line for CPLFGets where buffer hits \r followed by \n, and \r
    // followed by non-\n.
    fwrite("ab\r\ncd\ref\n", 1, 10, fp);
    // 2. Long line (> 600 chars) to exercise CPLReadLine buffer growth.
    std::string long_line(650, 'X');
    long_line += '\n';
    fwrite(long_line.data(), 1, long_line.size(), fp);
  }

  {
    FILE* fp = fopen(local_file.c_str(), "rb");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup close_fp = [fp] {
      fclose(fp);
      CPLReadLine(nullptr);
    };
    char small_buf[4] = {};
    // Reads "ab\r" (3 chars into 4-byte buffer), sees next char is '\n' and
    // consumes it.
    EXPECT_NE(nullptr, CPLFGets(small_buf, sizeof(small_buf), fp));
    EXPECT_STREQ("ab", small_buf);
    // Reads "cd\r" (3 chars into 4-byte buffer), sees next char is 'e' and
    // seeks back 1 byte.
    EXPECT_NE(nullptr, CPLFGets(small_buf, sizeof(small_buf), fp));
    EXPECT_STREQ("cd", small_buf);
    // Next read gets "ef".
    EXPECT_NE(nullptr, CPLFGets(small_buf, sizeof(small_buf), fp));
    EXPECT_STREQ("ef", small_buf);

    // Now test CPLReadLine on the 650-char line and EOF.
    const char* line = CPLReadLine(fp);
    ASSERT_NE(nullptr, line);
    EXPECT_EQ(650U, strlen(line));
    EXPECT_EQ(nullptr, CPLReadLine(fp));
  }

  // Test CPLReadLineL / CPLReadLine2L / CPLReadLine3L on /vsimem/.
  static constexpr char kVsiLineFile[] = "/vsimem/vsi_readline.txt";
  absl::Cleanup unlink_vsi = [] { VSIUnlink(kVsiLineFile); };
  {
    VSILFILE* fp = VSIFOpenL(kVsiLineFile, "wb");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup close_fp = [fp] { VSIFCloseL(fp); };
    // Exactly 39 chars + \r\n to hit the 40-byte chunk boundary in
    // CPLReadLine3L.
    std::string chunk_boundary(39, 'A');
    chunk_boundary += "\r\n";
    VSIFWriteL(chunk_boundary.data(), 1, chunk_boundary.size(), fp);
    // Line with \n\r.
    VSIFWriteL("line_nr\n\r", 1, 9, fp);
    // Line without trailing newline at EOF.
    VSIFWriteL("no_newline_eof", 1, 14, fp);
  }
  {
    VSILFILE* fp = VSIFOpenL(kVsiLineFile, "rb");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup close_fp = [fp] { VSIFCloseL(fp); };
    const char* l1 = CPLReadLineL(fp);
    ASSERT_NE(nullptr, l1);
    EXPECT_EQ(39U, strlen(l1));

    const char* l2 = CPLReadLine2L(fp, 100, nullptr);
    ASSERT_NE(nullptr, l2);
    EXPECT_STREQ("line_nr", l2);

    // Exceed nMaxCars limit.
    EXPECT_EQ(nullptr, CPLReadLine2L(fp, 5, nullptr));
  }
  {
    VSILFILE* fp = VSIFOpenL(kVsiLineFile, "rb");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup close_fp = [fp] {
      VSIFCloseL(fp);
      CPLReadLineL(nullptr);
    };
    EXPECT_NE(nullptr, CPLReadLineL(fp));
    EXPECT_NE(nullptr, CPLReadLineL(fp));
    const char* l3 = CPLReadLineL(fp);
    ASSERT_NE(nullptr, l3);
    EXPECT_STREQ("no_newline_eof", l3);
    EXPECT_EQ(nullptr, CPLReadLineL(fp));
  }
}

TEST_F(CplConvTest, ScanPrintAndComplexEdgeCases) {
  WithQuietHandler quiet;
  EXPECT_EQ(0, CPLScanLong(nullptr, 10));
  EXPECT_EQ(0U, CPLScanULong(nullptr, 10));

  int overflow = FALSE;
  CPLAtoGIntBigEx("   9999999999999999999999999999999", TRUE, &overflow);
  EXPECT_EQ(TRUE, overflow);

  char ptr_buf[256] = {};
  int dummy_var = 123;
  EXPECT_GT(CPLPrintPointer(ptr_buf, &dummy_var, 128), 0);
  EXPECT_EQ(&dummy_var, CPLScanPointer(ptr_buf, 128));
  EXPECT_EQ(nullptr, CPLScanPointer("0x0", 10));

  EXPECT_DOUBLE_EQ(150.0, CPLScanDouble("1.5d2", 10));
  EXPECT_DOUBLE_EQ(150.0, CPLScanDouble("1.5D2", 10));

  char num_buf[128] = {};
  EXPECT_GT(CPLPrintInt32(num_buf, -42, 100), 0);
  EXPECT_GT(CPLPrintUIntBig(num_buf, 42000ULL, 100), 0);

  struct tm tm{};
  tm.tm_sec = 10;
  tm.tm_min = 20;
  tm.tm_hour = 15;
  tm.tm_mday = 5;
  tm.tm_mon = 3;
  tm.tm_year = 120;
  EXPECT_GT(CPLPrintTime(num_buf, sizeof(num_buf) - 1, "%Y-%m-%d", &tm, "C"),
            0);
  EXPECT_GT(CPLPrintTime(num_buf, sizeof(num_buf) - 1, "%Y-%m-%d", &tm, ""), 0);

  // CPLStringToComplex.
  double re = 0.0;
  double im = 0.0;
  EXPECT_EQ(CE_None, CPLStringToComplex("1.5+2.5i", &re, &im));
  EXPECT_DOUBLE_EQ(1.5, re);
  EXPECT_DOUBLE_EQ(2.5, im);

  EXPECT_EQ(CE_None, CPLStringToComplex(" -3.0-4.0i ", &re, &im));
  EXPECT_DOUBLE_EQ(-3.0, re);
  EXPECT_DOUBLE_EQ(-4.0, im);

  EXPECT_EQ(CE_None, CPLStringToComplex("5.0", &re, &im));
  EXPECT_DOUBLE_EQ(5.0, re);
  EXPECT_DOUBLE_EQ(0.0, im);

  EXPECT_EQ(CE_Failure, CPLStringToComplex("", &re, &im));
  EXPECT_EQ(CE_Failure, CPLStringToComplex("abc", &re, &im));
  EXPECT_EQ(CE_Failure, CPLStringToComplex("1.5i", &re, &im));
  EXPECT_EQ(CE_Failure, CPLStringToComplex("1.5+abc", &re, &im));
  EXPECT_EQ(CE_Failure, CPLStringToComplex("1.5+2.5", &re, &im));
  EXPECT_EQ(CE_Failure, CPLStringToComplex("1.5+2.5i trailing", &re, &im));
}

TEST_F(CplConvTest, ConfigSubscribersKnownOptionsAndDirectives) {
  WithQuietHandler quiet;

  // Subscribers.
  int count1 = 0;
  int count2 = 0;
  int count3 = 0;
  int count4 = 0;
  auto cb = [](const char*, const char*, bool, void* user_data) {
    ++(*static_cast<int*>(user_data));
  };
  int id1 = CPLSubscribeToSetConfigOption(cb, &count1);
  int id2 = CPLSubscribeToSetConfigOption(cb, &count2);
  int id3 = CPLSubscribeToSetConfigOption(cb, &count3);
  int id4 = -1;
  absl::Cleanup cleanup_subscribers = [&] {
    if (id1 >= 0) CPLUnsubscribeToSetConfigOption(id1);
    if (id2 >= 0) CPLUnsubscribeToSetConfigOption(id2);
    if (id3 >= 0) CPLUnsubscribeToSetConfigOption(id3);
    if (id4 >= 0) CPLUnsubscribeToSetConfigOption(id4);
    CPLSetConfigOption("AWS_TEST_SUB_OPT", nullptr);
    CPLSetThreadLocalConfigOption("GS_TEST_SUB_OPT", nullptr);
  };

  // Setting AWS_ / GS_ options also exercises
  // NotifyOtherComponentsConfigOptionChanged.
  CPLSetConfigOption("AWS_TEST_SUB_OPT", "1");
  CPLSetThreadLocalConfigOption("GS_TEST_SUB_OPT", "2");
  EXPECT_GE(count1, 2);
  EXPECT_GE(count2, 2);
  EXPECT_GE(count3, 2);

  // Unsubscribe middle (id2), then subscribe id4 to reuse freed slot.
  CPLUnsubscribeToSetConfigOption(id2);
  const int old_id2 = id2;
  id2 = -1;
  id4 = CPLSubscribeToSetConfigOption(cb, &count4);
  EXPECT_EQ(old_id2, id4);

  CPLUnsubscribeToSetConfigOption(id1);
  id1 = -1;
  CPLUnsubscribeToSetConfigOption(id4);
  id4 = -1;
  CPLUnsubscribeToSetConfigOption(id3);
  id3 = -1;
  CPLSetConfigOption("AWS_TEST_SUB_OPT", nullptr);
  CPLSetThreadLocalConfigOption("GS_TEST_SUB_OPT", nullptr);

  // Known config options & CPL_DEBUG unknown config option detection.
  CPLDeclareKnownConfigOption("TEST_DECLARED_OPTION_123", "test desc");
  StringListPtr known(CPLGetKnownConfigOptions(), CSLDestroy);
  EXPECT_NE(nullptr, known);

  CPLSetConfigOption("CPL_DEBUG", "ON");
  CPLSetConfigOption("TEST_DECLARED_OPTION_123", "val");
  CPLSetConfigOption("TOTALLY_UNKNOWN_OPTION_XYZ_987", "val");
  CPLSetConfigOption("TOTALLY_UNKNOWN_OPTION_XYZ_987", nullptr);
  CPLSetConfigOption("TEST_DECLARED_OPTION_123", nullptr);
  CPLSetConfigOption("CPL_DEBUG", nullptr);

  // CPLConfigOptionSetter.
  {
    CPLSetThreadLocalConfigOption("SETTER_TEST_OPT", "initial");
    {
      CPLConfigOptionSetter setter1("SETTER_TEST_OPT", "override", false);
      EXPECT_STREQ("override", CPLGetConfigOption("SETTER_TEST_OPT", ""));
      CPLConfigOptionSetter setter2("SETTER_TEST_OPT", "ignored", true);
      EXPECT_STREQ("override", CPLGetConfigOption("SETTER_TEST_OPT", ""));
    }
    EXPECT_STREQ("initial", CPLGetConfigOption("SETTER_TEST_OPT", ""));
    CPLSetThreadLocalConfigOption("SETTER_TEST_OPT", nullptr);

    {
      CPLConfigOptionSetter setter3("SETTER_TEST_OPT", "set_when_undef", true);
      EXPECT_STREQ("set_when_undef", CPLGetConfigOption("SETTER_TEST_OPT", ""));
    }
    EXPECT_EQ(nullptr, CPLGetConfigOption("SETTER_TEST_OPT", nullptr));
  }

  // CPLLoadConfigOptionsFromFile with [directives] section.
  static constexpr char kDirectivesFile[] = "/vsimem/directives.gdalrc";
  // ignore-env-vars=yes flips a process-global flag that affects
  // CPLGetConfigOption() for every other test in this binary, so always
  // restore it (and remove the file) even if an assertion below fails.
  absl::Cleanup restore_env_vars = [] {
    VSILFILE* fp = VSIFOpenL(kDirectivesFile, "wb");
    if (fp != nullptr) {
      static constexpr char kRestore[] = "[directives]\nignore-env-vars=no\n";
      VSIFWriteL(kRestore, 1, sizeof(kRestore) - 1, fp);
      VSIFCloseL(fp);
      CPLLoadConfigOptionsFromFile(kDirectivesFile, false);
    }
    VSIUnlink(kDirectivesFile);
  };
  WriteConfig(kDirectivesFile,
              "# comment\n"
              "[directives]\n"
              "ignore-env-vars=yes\n"
              "unknown-directive=foo\n"
              "[configoptions]\n"
              "DIRECTIVE_FILE_OPT=CUSTOM_VAL\n"
              "INVALID_LINE_NO_EQUALS\n"
              "[directives]\n");
  // ignore-env-vars=yes causes CPLGetConfigOption("PATH") to ignore
  // getenv("PATH") and return the default value.
  CPLLoadConfigOptionsFromFile(kDirectivesFile, false);
  EXPECT_STREQ("DEFAULT_PATH", CPLGetConfigOption("PATH", "DEFAULT_PATH"));
  EXPECT_STREQ("CUSTOM_VAL", CPLGetConfigOption("DIRECTIVE_FILE_OPT", ""));
  CPLSetConfigOption("DIRECTIVE_FILE_OPT", nullptr);
}

TEST_F(CplConvTest, SharedFilesTreeOpsLocaleAndLocking) {
  WithQuietHandler quiet;
  const std::string temp_dir(::testing::TempDir());

  // CPLStat on valid directory.
  VSIStatBuf stat_buf = {};
  EXPECT_EQ(0, CPLStat(temp_dir.c_str(), &stat_buf));

  // CPLOpenShared / CPLCloseShared / CPLGetSharedList / CPLDumpSharedList.
  const std::string shared_file = temp_dir + "/shared_test.bin";
  absl::Cleanup cleanup_shared_file = [&shared_file] {
    CPLCleanupSharedFileMutex();
    VSIUnlink(shared_file.c_str());
  };
  {
    FILE* fp = fopen(shared_file.c_str(), "wb");
    ASSERT_NE(nullptr, fp);
    fwrite("shared_content", 1, 14, fp);
    fclose(fp);
  }
  FILE* s1 = CPLOpenShared(shared_file.c_str(), "rb", FALSE);
  FILE* s2 = CPLOpenShared(shared_file.c_str(), "rb", FALSE);
  EXPECT_EQ(s1, s2);

  FILE* s_large1 = CPLOpenShared(shared_file.c_str(), "rb", TRUE);
  FILE* s_large2 = CPLOpenShared(shared_file.c_str(), "rb", TRUE);
  EXPECT_EQ(s_large1, s_large2);

  int n_shared = 0;
  EXPECT_NE(nullptr, CPLGetSharedList(&n_shared));
  EXPECT_GE(n_shared, 2);

  const std::string dump_path = temp_dir + "/dump_shared.txt";
  {
    absl::Cleanup unlink_dump = [&dump_path] { VSIUnlink(dump_path.c_str()); };
    FILE* dump_fp = fopen(dump_path.c_str(), "wb");
    ASSERT_NE(nullptr, dump_fp);
    CPLDumpSharedList(dump_fp);
    fclose(dump_fp);
  }

  CPLCloseShared(s2);
  CPLCloseShared(s1);
  CPLCloseShared(s_large2);
  CPLCloseShared(s_large1);
  // Closing unknown pointer logs an error.
  CPLCloseShared(reinterpret_cast<FILE*>(0x1));

  // CPLUnlinkTree, CPLCopyFile, CPLMoveFile, CPLSymlink.
  EXPECT_NE(0, CPLUnlinkTree("/vsimem/does_not_exist_tree"));
  WriteConfig("/vsimem/single_unlink.txt", "abc");
  EXPECT_EQ(0, CPLUnlinkTree("/vsimem/single_unlink.txt"));

  ASSERT_EQ(0, VSIMkdirRecursive("/vsimem/tree_to_unlink/sub", 0755));
  WriteConfig("/vsimem/tree_to_unlink/a.txt", "a");
  WriteConfig("/vsimem/tree_to_unlink/sub/b.txt", "b");
  EXPECT_EQ(0, CPLUnlinkTree("/vsimem/tree_to_unlink"));

  WriteConfig("/vsimem/conv_src.txt", "move_and_copy_data");
  EXPECT_EQ(0, CPLCopyFile("/vsimem/conv_copied.txt", "/vsimem/conv_src.txt"));
  EXPECT_EQ(0,
            CPLMoveFile("/vsimem/conv_moved.txt", "/vsimem/conv_copied.txt"));
  // Cross-FS move (/vsimem/ -> temp_dir).
  const std::string cross_moved = temp_dir + "/conv_cross_moved.txt";
  EXPECT_EQ(0, CPLMoveFile(cross_moved.c_str(), "/vsimem/conv_moved.txt"));
  VSIUnlink(cross_moved.c_str());
  VSIUnlink("/vsimem/conv_src.txt");

  const std::string sym_link = temp_dir + "/test_symlink";
  EXPECT_EQ(0, CPLSymlink(temp_dir.c_str(), sym_link.c_str(), nullptr));
  EXPECT_EQ(0, VSIUnlink(sym_link.c_str()));

  // CPLLocaleC, CPLThreadLocaleC, CPLsetlocale.
  {
    CPLLocaleC loc1;
    CPLThreadLocaleC tloc1;
  }
  {
    CPLConfigOptionSetter disable_locale("GDAL_DISABLE_CPLLOCALEC", "YES",
                                         false);
    CPLLocaleC loc2;
  }
  EXPECT_NE(nullptr, CPLsetlocale(LC_NUMERIC, nullptr));
  EXPECT_NE(nullptr, CPLsetlocale(LC_NUMERIC, "C"));
  CPLCleanupSetlocaleMutex();

  // CPLIsInteractive and CPLGetRemainingFileDescriptorCount.
  CPLIsInteractive(stdin);
  EXPECT_GT(CPLGetRemainingFileDescriptorCount(), 0);

  // CPLLockFileEx and CPLUnlockFileEx.
  CPLLockFileHandle lock_handle = nullptr;
  EXPECT_EQ(CLFS_API_MISUSE, CPLLockFileEx(nullptr, &lock_handle, nullptr));
  EXPECT_EQ(CLFS_API_MISUSE, CPLLockFileEx("foo", nullptr, nullptr));
  CPLUnlockFileEx(nullptr);

  EXPECT_EQ(CLFS_CANNOT_CREATE_LOCK,
            CPLLockFileEx("/does_not_exist_dir_xyz/locktarget", &lock_handle,
                          nullptr));

  const std::string lock_target = temp_dir + "/lock_target.bin";
  EXPECT_EQ(CLFS_OK, CPLLockFileEx(lock_target.c_str(), &lock_handle, nullptr));
  ASSERT_NE(nullptr, lock_handle);

  // Attempt second lock while held -> CLFS_LOCK_BUSY.
  CPLLockFileHandle lock_handle2 = nullptr;
  const char* busy_opts[] = {"WAIT_TIME=0.02", "VERBOSE_WAIT_MESSAGE=YES",
                             nullptr};
  EXPECT_EQ(CLFS_LOCK_BUSY,
            CPLLockFileEx(lock_target.c_str(), &lock_handle2, busy_opts));
  CPLUnlockFileEx(lock_handle);

  // Stalled lock recovery.  CPLLockFileEx() locks the given path itself (it
  // does not append a ".lock" suffix), so pre-create that exact file and push
  // its mtime far enough into the past (more than STALLED_DELAY seconds) that
  // the stalled branch (EEXIST -> stale check -> unlink -> retry) is taken.
  const std::string stalled_target = temp_dir + "/stalled_target.bin";
  {
    FILE* fp = fopen(stalled_target.c_str(), "wb");
    ASSERT_NE(nullptr, fp);
    fclose(fp);
  }
  const time_t stale_time = time(nullptr) - 100;
  struct utimbuf old_times = {stale_time, stale_time};
  ASSERT_EQ(0, utime(stalled_target.c_str(), &old_times));
  const char* stalled_opts[] = {"STALLED_DELAY=10", "WAIT_TIME=0.05", nullptr};
  lock_handle = nullptr;
  EXPECT_EQ(CLFS_OK,
            CPLLockFileEx(stalled_target.c_str(), &lock_handle, stalled_opts));
  ASSERT_NE(nullptr, lock_handle);
  // The stale lock file was replaced by a fresh one that we now hold.
  VSIStatBufL stalled_stat = {};
  ASSERT_EQ(0, VSIStatL(stalled_target.c_str(), &stalled_stat));
  EXPECT_GT(stalled_stat.st_mtime, stale_time);
  CPLUnlockFileEx(lock_handle);
  // Unlocking removes the lock file.
  EXPECT_NE(0, VSIStatL(stalled_target.c_str(), &stalled_stat));
}

// TODO: b/335317901 - Cover CPLFreeConfig, CPLLocaleC, CPLThreadLocaleC.

}  // namespace
}  // namespace autotest2
