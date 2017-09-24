// Test convenience functions:
//   http://trac.osgeo.org/gdal/browser/trunk/gdal/port/cpl_conv.cpp
//
// Copyright 2014 Google Inc. All Rights Reserved.
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

#include "port/cpl_conv.h"

#include <features.h>
#include <stddef.h>
#include <string.h>
#include <cstdio>
#include <memory>
#include <string>

#include "gunit.h"
#include "autotest2/cpp/util/cpl_memory.h"
#include "autotest2/cpp/util/error_handler.h"
#include "gcore/gdal_priv.h"
#include "port/cpl_error.h"
#include "port/cpl_vsi.h"

namespace autotest2 {
namespace {

TEST(CplConvTest, VerifyConfiguration) {
  CPLVerifyConfiguration();
  EXPECT_EQ(0, CPLGetLastErrorNo());
}

// Tests accessing and setting the GDAL runtime configuration system.
TEST(CplConvTest, GetSetConfigOption) {
  // Check that 2nd argument default is returned for a non-existant key.
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
TEST(CplConvTest, Memory) {
  WithQuietHandler error_handler;

  EXPECT_EQ(nullptr, CPLMalloc(0));
  EXPECT_EQ(CPLE_None, CPLGetLastErrorNo());

  EXPECT_EQ(nullptr, CPLMalloc(-1));
  EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());

  // Use ASSERT_ (rather than EXPECT_) to assert we are not leaking memory.

  void *ptr = CPLMalloc(1);
  ASSERT_NE(nullptr, ptr);
  CPLFree(ptr);

  ptr = CPLMalloc(999999);
  ASSERT_NE(nullptr, ptr);
  CPLFree(ptr);

  EXPECT_EQ(nullptr, CPLCalloc(10, 0));
  EXPECT_EQ(nullptr, CPLCalloc(0, 10));
  char *char_ptr = static_cast<char *>(CPLCalloc(10, 1));
  for (int i = 0; i < 10; i++)
    EXPECT_EQ(0, char_ptr[i]);
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
  void *ptr2 = CPLRealloc(ptr, 20);
  // The contents of ptr have been freed.
  // TODO(schwehr): Fails with optimized compile, but succeeds with debug.
  //   ASSERT_NE(ptr, ptr2);
  CPLFree(ptr2);

  // Demonstrate leak potential.
  ptr = CPLRealloc(nullptr, 10);
  ptr2 = ptr;  // Backup the pointer.
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
TEST(CplConvTest, Strings) {
  // Unlike strdup, CPLStrdup of a nullptr returns the empty string
  // per documentation.
  char *ptr = CPLStrdup(nullptr);
  EXPECT_STREQ("", ptr);
  CPLFree(ptr);

  ptr = CPLStrdup("abc123");
  EXPECT_STREQ("abc123", ptr);
  CPLFree(ptr);

  char str[] = "AbC123";
  // Converts in place to lower case and returns the original pointer.
  EXPECT_EQ(str, CPLStrlwr(str));
  EXPECT_STREQ("abc123", str);
}

// Tests the fgets wrapper.
TEST(CplConvTest, CplFGets) {
  EXPECT_EQ(nullptr, CPLFGets(nullptr, 0, nullptr));
  EXPECT_EQ(nullptr, CPLFGets(nullptr, 1, reinterpret_cast<FILE*>(1)));
  EXPECT_EQ(nullptr, CPLFGets(reinterpret_cast<char*>(1), 0,
                              reinterpret_cast<FILE*>(1)));
  EXPECT_EQ(nullptr, CPLFGets(reinterpret_cast<char*>(1), 1, nullptr));

#if _XOPEN_SOURCE >= 700 || _POSIX_C_SOURCE >= 200809L  // fmemopen.
  string src_buf("\nfoo\rbar\r\nmuch longer line\n \n\n");
  FILE *src = fmemopen(const_cast<char*>(src_buf.c_str()), src_buf.size(), "r");
  const size_t kBufSize = 10;
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
TEST(CplConvTest, CplReadLine) {
#if _XOPEN_SOURCE >= 700 || _POSIX_C_SOURCE >= 200809L  // fmemopen.
  string src_buf("\na\rb\r\nc\n \n");
  FILE *src = fmemopen(const_cast<char*>(src_buf.c_str()), src_buf.size(), "r");

  EXPECT_STREQ("", CPLReadLine(src));
  EXPECT_STREQ("a", CPLReadLine(src));
  EXPECT_STREQ("b", CPLReadLine(src));
  EXPECT_STREQ("c", CPLReadLine(src));
  EXPECT_STREQ(" ", CPLReadLine(src));
  EXPECT_STREQ(nullptr, CPLReadLine(src));
#endif  // POSIX-2008.1
}

// TODO(schwehr): Write tests for CPLReadLineL.
// TODO(schwehr): Write tests for CPLReadLine2L.

// Tests returning a length limited copy of a string with optional
// removal of trailing white space and optional replacement of colons
// with under bars.  The last 2 integer parameters are treated as booleans.
TEST(CplConvTest, CplScanString) {
  EXPECT_EQ(nullptr, CPLScanString(nullptr, 0, 0, 0));
  char *ptr = CPLScanString("", 0, 0, 0);
  EXPECT_STREQ("", ptr);
  CPLFree(ptr);

  // nMaxLength greater than the string.
  ptr = CPLScanString(" Foo", 10, 0, 0);
  EXPECT_STREQ(" Foo", ptr);
  CPLFree(ptr);

  // nMaxLength less than the string.
  ptr = CPLScanString(" FooBar", 4, 0, 0);
  EXPECT_STREQ(" Foo", ptr);
  CPLFree(ptr);

  // Trim trailing spaces.
  ptr = CPLScanString(" Foo ", 5, 1, 0);
  EXPECT_STREQ(" Foo", ptr);
  CPLFree(ptr);

  // "Normalization" means replace all occurrences of ":" with "_".
  ptr = CPLScanString(" :_:_", 5, 0, 1);
  EXPECT_STREQ(" ____", ptr);
  CPLFree(ptr);

  // Trim trailing spaces and "normalize" by replacing ":" with "_".
  ptr = CPLScanString(" :_ _:   ", 6, 1, 1);
  EXPECT_STREQ(" __ __", ptr);
  CPLFree(ptr);
}

// Tests converting a string to an integer.  CplScanLong is a wrapper
// around atoi that adds setting a maximum number of characters.
TEST(CplConvTest, CplScanLong) {
  // A maximum length of 0 might be undefined based on the definitions
  // atoi and strtol.
  EXPECT_EQ(0, CPLScanLong("", 0));
  EXPECT_EQ(0, CPLScanLong("9", 0));
  EXPECT_EQ(0, CPLScanLong("a", 1));
  EXPECT_EQ(0, CPLScanLong("+", 1));
  EXPECT_EQ(0, CPLScanLong("-", 1));

  EXPECT_EQ(0, CPLScanLong("0", 1));
  EXPECT_EQ(0, CPLScanLong("0", 2));
  EXPECT_EQ(1, CPLScanLong("1", 1));
  EXPECT_EQ(1, CPLScanLong("1", 2));
  EXPECT_EQ(1, CPLScanLong("12", 1));
  EXPECT_EQ(0, CPLScanLong("+1", 1));
  EXPECT_EQ(1, CPLScanLong("+1", 2));
  EXPECT_EQ(-1, CPLScanLong("-1", 2));
  EXPECT_EQ(-1, CPLScanLong("-1", 3));

  // Hex and octal are not supported.
  EXPECT_EQ(0, CPLScanLong("0xF", 3));
  EXPECT_EQ(10, CPLScanLong("010", 3));

  EXPECT_EQ(2, CPLScanLong(" 2", 2));
  EXPECT_EQ(3, CPLScanLong("\t\n3", 3));
  EXPECT_EQ(4, CPLScanLong("4 ", 2));
  EXPECT_EQ(5, CPLScanLong(" 5 ", 3));
  EXPECT_EQ(6, CPLScanLong(" 6 1", 4));
  EXPECT_EQ(7, CPLScanLong(" 7-", 3));
  EXPECT_EQ(8, CPLScanLong(" 8a", 3));
  EXPECT_EQ(9, CPLScanLong(" 9.1", 4));

  EXPECT_EQ(1, CPLScanLong("1", 2));
  EXPECT_EQ(2, CPLScanLong("2", 20));
  EXPECT_EQ(3, CPLScanLong("3", 200));
  EXPECT_EQ(4, CPLScanLong("4", 2000));

  if (sizeof(long) > 4) {  // NOLINT
    EXPECT_EQ(1234567890123, CPLScanLong("1234567890123", 9999));
    EXPECT_EQ(-1234567890123, CPLScanLong("-1234567890123", 9999));
  }
}

// Tests converting string to unsigned long.  CplScanULong is a
// wrapper around strtoul that adds setting a maximum number of
// characters.
TEST(CplConvTest, CplScanULong) {
  EXPECT_EQ(0, CPLScanULong("", 0));
  EXPECT_EQ(0, CPLScanULong("9", 0));
  EXPECT_EQ(0, CPLScanULong("a", 1));
  EXPECT_EQ(0, CPLScanULong("+", 1));
  EXPECT_EQ(0, CPLScanULong("-", 1));

  EXPECT_EQ(0, CPLScanULong("0", 1));
  EXPECT_EQ(1, CPLScanULong("1", 1));
  EXPECT_EQ(1, CPLScanULong("1", 2));
  EXPECT_EQ(1, CPLScanULong("12", 1));
  EXPECT_EQ(1, CPLScanULong("+1", 2));

  // strtoul does parse the "-".  It negates the unsigned portion and then
  // casts to unsigned long.
  EXPECT_EQ(static_cast<unsigned long>(-1), CPLScanULong("-1", 2));  // NOLINT
  EXPECT_EQ(static_cast<unsigned long>(-2), CPLScanULong("-2", 2));  // NOLINT

  // Hex and octal are not supported.
  EXPECT_EQ(0, CPLScanULong("0xF", 3));
  EXPECT_EQ(10, CPLScanULong("010", 3));

  EXPECT_EQ(2, CPLScanULong(" 2", 2));
  EXPECT_EQ(3, CPLScanULong("\t\n3", 3));
  EXPECT_EQ(4, CPLScanULong("4 ", 2));
  EXPECT_EQ(5, CPLScanULong(" 5 ", 3));
  EXPECT_EQ(6, CPLScanULong(" 6 1", 4));
  EXPECT_EQ(7, CPLScanULong(" 7-", 3));
  EXPECT_EQ(8, CPLScanULong(" 8a", 3));
  EXPECT_EQ(9, CPLScanULong(" 9.1", 4));

  if (sizeof(unsigned long) > 4) { // NOLINT
    EXPECT_EQ(1234567890123, CPLScanULong("1234567890123", 9999));
    EXPECT_EQ(static_cast<unsigned long>(-1234567890123), // NOLINT
              CPLScanULong("-1234567890123", 9999));
  }
}

// Tests converting strings to GUIntBig.  CPLScanUIntBig is a wrapper
// around _atooi64, atoll or atol depending on platform.
TEST(CplConvTest, CPLScanUIntBig) {
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

// TODO(schwehr): CPLAtoGIntBig;
// TODO(schwehr): CPLAtoGIntBigEx;
// TODO(schwehr): CPLScanPointer
// TODO(schwehr): CPLScanDouble
// TODO(schwehr): CPLPrintString
// TODO(schwehr): CPLPrintStringFill
// TODO(schwehr): CPLPrintInt32
// TODO(schwehr): CPLPrintUIntBig
// TODO(schwehr): CPLPrintPointer
// TODO(schwehr): CPLPrintDouble
// TODO(schwehr): CPLPrintTime
// TODO(schwehr): CPLVerifyConfiguration
// TODO(schwehr): CPLGetConfigOption
// TODO(schwehr): CPLGetThreadLocalConfigOption
// TODO(schwehr): CPLSetConfigOption
// TODO(schwehr): CPLSetThreadLocalConfigOption
// TODO(schwehr): CPLFreeConfig

TEST(CplConvTest, CPLStat) {
  constexpr int kFailure = -1;

  VSIStatBuf buf = {};
  EXPECT_EQ(kFailure, CPLStat("c:", &buf));
}

TEST(CplConvTest, CPLDMSToDec) {
  // nullptr crashes.
  EXPECT_DOUBLE_EQ(0, CPLDMSToDec(""));
  EXPECT_DOUBLE_EQ(0, CPLDMSToDec(" "));
  EXPECT_DOUBLE_EQ(0, CPLDMSToDec("a"));
  EXPECT_DOUBLE_EQ(0, CPLDMSToDec("  0d 0' 0.9720\"N"));
  EXPECT_DOUBLE_EQ(0, CPLDMSToDec("  0d 0' 0.9720\"S"));

  EXPECT_DOUBLE_EQ(1, CPLDMSToDec("1"));
  EXPECT_DOUBLE_EQ(1, CPLDMSToDec("1N"));
  EXPECT_DOUBLE_EQ(1, CPLDMSToDec("1n"));
  EXPECT_DOUBLE_EQ(1, CPLDMSToDec("+1"));

  EXPECT_DOUBLE_EQ(1, CPLDMSToDec("\t\n\r 1 "));

  EXPECT_DOUBLE_EQ(-1, CPLDMSToDec("-1"));
  EXPECT_DOUBLE_EQ(-1, CPLDMSToDec("1S"));
  EXPECT_DOUBLE_EQ(-1, CPLDMSToDec("1s"));

  EXPECT_DOUBLE_EQ(1, CPLDMSToDec("1E"));
  EXPECT_DOUBLE_EQ(1, CPLDMSToDec("1e"));
  EXPECT_DOUBLE_EQ(-1, CPLDMSToDec("1W"));
  EXPECT_DOUBLE_EQ(-1, CPLDMSToDec("1w"));

  EXPECT_DOUBLE_EQ(2, CPLDMSToDec("2d"));
  EXPECT_DOUBLE_EQ(0.0500000000001, CPLDMSToDec("3'"));
  EXPECT_DOUBLE_EQ(0.0011111111200, CPLDMSToDec("4\""));

  EXPECT_DOUBLE_EQ(0.00027777778, CPLDMSToDec("+1\""));
  EXPECT_DOUBLE_EQ(-0.00027777778, CPLDMSToDec("-1\""));

  EXPECT_DOUBLE_EQ(9.87, CPLDMSToDec("9.87r"));
  EXPECT_DOUBLE_EQ(-9.87, CPLDMSToDec("-9.87R"));

  EXPECT_DOUBLE_EQ(200000, CPLDMSToDec("2e5R"));

  EXPECT_DOUBLE_EQ(1, CPLDMSToDec(" 1R "));
  EXPECT_DOUBLE_EQ(0, CPLDMSToDec("R"));
  EXPECT_DOUBLE_EQ(0, CPLDMSToDec(" R "));
}

TEST(CplConvTest, CPLDecToDMS) {
  EXPECT_STREQ("Invalid angle",
               CPLDecToDMS(std::numeric_limits<double>::quiet_NaN(), "", 0));
  EXPECT_STREQ("Invalid angle",
               CPLDecToDMS(std::numeric_limits<double>::infinity(), "", 0));
  EXPECT_STREQ("Invalid angle",
               CPLDecToDMS(-std::numeric_limits<double>::infinity(), "", 0));
  EXPECT_STREQ("Invalid angle", CPLDecToDMS(361.1, "", 0));
  EXPECT_STREQ("Invalid angle", CPLDecToDMS(-361.1, "", 0));

  EXPECT_STREQ("360d 0'  0\"N", CPLDecToDMS(360.0, "", 0));
  EXPECT_STREQ("360d 0'  0\"E", CPLDecToDMS(360.0, "Long", 0));

  EXPECT_STREQ("360d 0'  0\"S", CPLDecToDMS(-360.0, "", 0));
  EXPECT_STREQ("360d 0'  0\"W", CPLDecToDMS(-360.0, "Long", 0));

  EXPECT_STREQ(" 90d 0'  0\"N", CPLDecToDMS(90.0, "", 0));
  EXPECT_STREQ(" 90d 0'  0\"S", CPLDecToDMS(-90.0, "", 0));

  // Just under 1 minute.
  EXPECT_STREQ("  0d 0' 58\"N", CPLDecToDMS(0.016, "", 0));
  EXPECT_STREQ("  0d 0' 58\"S", CPLDecToDMS(-0.016, "", 0));

  EXPECT_STREQ("  0d 0'57.6\"N", CPLDecToDMS(0.016, "", 1));
  EXPECT_STREQ("  0d 0'57.6\"S", CPLDecToDMS(-0.016, "", 1));

  EXPECT_STREQ("  0d 0'57.60\"N", CPLDecToDMS(0.016, "", 2));
  EXPECT_STREQ("  0d 0'57.60\"S", CPLDecToDMS(-0.016, "", 2));

  // Just under 1 second
  EXPECT_STREQ("  0d 0' 0.97\"N", CPLDecToDMS(0.00027, "", 2));
  EXPECT_STREQ("  0d 0' 0.97\"S", CPLDecToDMS(-0.00027, "", 2));

  EXPECT_STREQ("  0d 0' 0.9720\"N", CPLDecToDMS(0.00027, "", 4));
  EXPECT_STREQ("  0d 0' 0.9720\"S", CPLDecToDMS(-0.00027, "", 4));
}

TEST(CplConvTest, CPLPackedDMSToDec) {
  EXPECT_DOUBLE_EQ(720.0, CPLPackedDMSToDec(720000000.0));
  EXPECT_DOUBLE_EQ(1.0, CPLPackedDMSToDec(1000000.0));
  EXPECT_DOUBLE_EQ(0.1, CPLPackedDMSToDec(6000.0));
  EXPECT_DOUBLE_EQ(0.0001, CPLPackedDMSToDec(0.36));
  EXPECT_DOUBLE_EQ(-0.0001, CPLPackedDMSToDec(-0.36));
  EXPECT_DOUBLE_EQ(-0.1, CPLPackedDMSToDec(-6000.0));
  EXPECT_DOUBLE_EQ(-1.0, CPLPackedDMSToDec(-1000000.0));
}

TEST(CplConvTest, CPLDecToPackedDMS) {
  EXPECT_DOUBLE_EQ(0.0, CPLDecToPackedDMS(0.0));
  EXPECT_DOUBLE_EQ(720000000.0, CPLDecToPackedDMS(720.0));
  EXPECT_DOUBLE_EQ(360000000.0, CPLDecToPackedDMS(360.0));
  EXPECT_DOUBLE_EQ(1000000.0, CPLDecToPackedDMS(1.0));
  EXPECT_DOUBLE_EQ(6000.0, CPLDecToPackedDMS(0.1));
  EXPECT_DOUBLE_EQ(36.0, CPLDecToPackedDMS(0.01));
  EXPECT_DOUBLE_EQ(3.6, CPLDecToPackedDMS(0.001));
  EXPECT_DOUBLE_EQ(0.36, CPLDecToPackedDMS(0.0001));
  EXPECT_DOUBLE_EQ(0.036, CPLDecToPackedDMS(0.00001));
  EXPECT_DOUBLE_EQ(0.0036, CPLDecToPackedDMS(0.000001));
  EXPECT_DOUBLE_EQ(0.00036, CPLDecToPackedDMS(0.0000001));
  EXPECT_DOUBLE_EQ(-0.00036, CPLDecToPackedDMS(-0.0000001));
  EXPECT_DOUBLE_EQ(-1000000.0, CPLDecToPackedDMS(-1.0));
}

// TODO(schwehr): CPLStringToComplex
// TODO(schwehr): CPLOpenShared / CPLCloseShared
// TODO(schwehr): CPLCleanupSharedFileMutex
// TODO(schwehr): CPLGetSharedList
// TODO(schwehr): CPLDumpSharedList
// TODO(schwehr): CPLUnlinkTree
// TODO(schwehr): CPLCopyFile
// TODO(schwehr): CPLCopyTree
// TODO(schwehr): CPLMoveFile
// TODO(schwehr): CPLSymlink
// TODO(schwehr): CPLLocaleC
// TODO(schwehr): CPLThreadLocaleC
// TODO(schwehr): CPLsetlocale

// Tests looking for files with a list.
TEST(CplConvTest, CPLCheckForFileWithFileList) {
  char kFilename[] = "Aa";
  char * files[2] = {kFilename, nullptr};

  const char kDoesNotExist[] = "does-not-exist";
  std::unique_ptr<char, CplFreeDeleter> filename(strdup(kDoesNotExist));
  EXPECT_FALSE(CPLCheckForFile(const_cast<char *>(filename.get()), files));
  EXPECT_STREQ(kDoesNotExist, filename.get());

  filename.reset(strdup(kFilename));
  EXPECT_TRUE(CPLCheckForFile(const_cast<char *>(filename.get()), files));
  EXPECT_STREQ(kFilename, filename.get());

  const char kLower[] = "aa";
  filename.reset(strdup(kLower));
  EXPECT_TRUE(CPLCheckForFile(const_cast<char *>(filename.get()), files));
  EXPECT_STREQ(kFilename, filename.get());

  const char kUpper[] = "AA";
  filename.reset(strdup(kUpper));
  EXPECT_TRUE(CPLCheckForFile(const_cast<char *>(filename.get()), files));
  EXPECT_STREQ(kFilename, filename.get());
}

// Tests looking for files without a filelist.
// Probes for the actual file.
TEST(CplConvTest, CPLCheckForFileVsimemFiles) {
  const char kFilename[] = "/vsimem/CheckForFile";
  const char kLower[] = "/vsimem/checkforfile";
  const char kUpper[] = "/vsimem/CHECKFORFILE";

  std::unique_ptr<char, CplFreeDeleter> filename(strdup(kFilename));
  EXPECT_FALSE(CPLCheckForFile(const_cast<char *>(filename.get()), nullptr));
  EXPECT_STREQ(kFilename, filename.get());

  {
    // Create an empty file.
    VSILFILE *file = VSIFOpenL(filename.get(), "wb");
    VSIFCloseL(file);
  }
  EXPECT_TRUE(CPLCheckForFile(const_cast<char *>(filename.get()), nullptr));
  EXPECT_STREQ(kFilename, filename.get());

  // The /vsimem filesystem is case sensitive.
  // Mismatches return false and do not alter the filename.
  filename.reset(strdup(kLower));
  EXPECT_FALSE(CPLCheckForFile((filename.get()), nullptr));
  EXPECT_STREQ(kLower, filename.get());

  filename.reset(strdup(kUpper));
  EXPECT_FALSE(CPLCheckForFile((filename.get()), nullptr));
  EXPECT_STREQ(kUpper, filename.get());
}

}  // namespace
}  // namespace autotest2
