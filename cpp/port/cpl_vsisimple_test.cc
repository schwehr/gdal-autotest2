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
// Tests for GDAL's simple POSIX VSI functions and memory/time/RAM helpers.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_vsisimple.cpp

#include <sys/resource.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

TEST(CplVsisimpleTest, LegacyStdioFileFunctionsAndVSIStat) {
  const std::string filepath =
      absl::StrCat(::testing::TempDir(), "/vsisimple_stdio_test.txt");
  absl::Cleanup cleanup = [&filepath] { VSIUnlink(filepath.c_str()); };

  FILE* fp = VSIFOpen(filepath.c_str(), "w+");
  ASSERT_NE(nullptr, fp);

  EXPECT_EQ(0, VSIFTell(fp));
  EXPECT_EQ(5, VSIFWrite("hello", 1, 5, fp));
  EXPECT_EQ(5, VSIFTell(fp));
  EXPECT_GE(VSIFPuts(" world", fp), 0);
  EXPECT_EQ('!', VSIFPutc('!', fp));
  EXPECT_EQ(5, VSIFPrintf(fp, " %d\n", 123));
  VSIFFlush(fp);

  VSIRewind(fp);
  EXPECT_EQ(0, VSIFTell(fp));
  EXPECT_EQ(0, VSIFSeek(fp, 0, SEEK_SET));

  char buf[64] = {};
  EXPECT_EQ(5, VSIFRead(buf, 1, 5, fp));
  EXPECT_STREQ("hello", buf);

  int ch = VSIFGetc(fp);
  EXPECT_EQ(' ', ch);
  EXPECT_EQ(' ', VSIUngetc(ch, fp));

  memset(buf, 0, sizeof(buf));
  ASSERT_NE(nullptr, VSIFGets(buf, sizeof(buf), fp));
  EXPECT_STREQ(" world! 123\n", buf);

  // Read one more char to trigger EOF.
  EXPECT_EQ(EOF, VSIFGetc(fp));
  EXPECT_NE(0, VSIFEof(fp));

  EXPECT_EQ(0, VSIFClose(fp));

  VSIStatBuf stat_buf;
  memset(&stat_buf, 0, sizeof(stat_buf));
  ASSERT_EQ(0, VSIStat(filepath.c_str(), &stat_buf));
  EXPECT_GT(stat_buf.st_size, 0);
}

TEST(CplVsisimpleTest, MemoryAllocationFunctionsAndOverflowPaths) {
  // Basic malloc, calloc, realloc, free, strdup.
  void* p1 = VSIMalloc(32);
  ASSERT_NE(nullptr, p1);
  p1 = VSIRealloc(p1, 64);
  ASSERT_NE(nullptr, p1);
  VSIFree(p1);
  VSIFree(nullptr);

  void* p2 = VSICalloc(8, 8);
  ASSERT_NE(nullptr, p2);
  VSIFree(p2);

  char* s1 = VSIStrdup("gdal_test");
  ASSERT_NE(nullptr, s1);
  EXPECT_STREQ("gdal_test", s1);
  VSIFree(s1);

  char* s2 = VSI_STRDUP_VERBOSE("verbose_test");
  ASSERT_NE(nullptr, s2);
  EXPECT_STREQ("verbose_test", s2);
  VSIFree(s2);

  // Aligned allocation and free.
  EXPECT_EQ(nullptr, VSIMallocAligned(64, 0));
  // Invalid alignment (not power of 2) triggers posix_memalign failure.
  EXPECT_EQ(nullptr, VSIMallocAligned(3, 64));

  void* aligned1 = VSIMallocAligned(64, 128);
  ASSERT_NE(nullptr, aligned1);
  EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(aligned1) % 64);
  VSIFreeAligned(aligned1);

  void* aligned2 = VSIMallocAlignedAuto(256);
  ASSERT_NE(nullptr, aligned2);
  EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(aligned2) % 64);
  VSIFreeAligned(aligned2);

  void* aligned3 = VSI_MALLOC_ALIGNED_AUTO_VERBOSE(128);
  ASSERT_NE(nullptr, aligned3);
  VSIFreeAligned(aligned3);
  EXPECT_EQ(nullptr, VSI_MALLOC_ALIGNED_AUTO_VERBOSE(0));

  // VSIMalloc2 / VSIMalloc3 normal and zero-size cases.
  void* m2 = VSIMalloc2(10, 20);
  ASSERT_NE(nullptr, m2);
  VSIFree(m2);
  EXPECT_EQ(nullptr, VSIMalloc2(0, 20));
  EXPECT_EQ(nullptr, VSIMalloc2(10, 0));

  void* m3 = VSIMalloc3(4, 5, 6);
  ASSERT_NE(nullptr, m3);
  VSIFree(m3);
  EXPECT_EQ(nullptr, VSIMalloc3(0, 5, 6));
  EXPECT_EQ(nullptr, VSIMalloc3(4, 0, 6));
  EXPECT_EQ(nullptr, VSIMalloc3(4, 5, 0));

  // Verbose allocation macros on normal sizes.
  void* mv = VSI_MALLOC_VERBOSE(16);
  ASSERT_NE(nullptr, mv);
  mv = VSI_REALLOC_VERBOSE(mv, 32);
  ASSERT_NE(nullptr, mv);
  VSIFree(mv);

  void* cv = VSI_CALLOC_VERBOSE(4, 8);
  ASSERT_NE(nullptr, cv);
  VSIFree(cv);

  // Multiplication overflow in VSIMalloc2 / VSIMalloc3.
  const size_t kBig = std::numeric_limits<size_t>::max() / 2 + 2;
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, VSIMalloc2(kBig, 2));
    EXPECT_EQ(nullptr, VSI_MALLOC2_VERBOSE(kBig, 2));
    // Overflow in first multiplication of VSICheckMul3:
    EXPECT_EQ(nullptr, VSIMalloc3(kBig, 2, 2));
    EXPECT_EQ(nullptr, VSI_MALLOC3_VERBOSE(kBig, 2, 2));
    // Overflow in second multiplication of VSICheckMul3:
    EXPECT_EQ(nullptr, VSIMalloc3(2, 2, kBig));
    EXPECT_EQ(nullptr, VSI_MALLOC3_VERBOSE(2, 2, kBig));
  }
  // Note: the allocator-failure (OOM) paths of the *Verbose functions are
  // deliberately not exercised; huge allocations abort under sanitizer
  // allocators and are otherwise dependent on vm.overcommit_memory.
}

TEST(CplVsisimpleTest, TimeStrerrorAndPhysicalRamFunctions) {
  unsigned long t_out = 0;
  unsigned long t_ret = VSITime(&t_out);
  EXPECT_GT(t_ret, 0UL);
  EXPECT_EQ(t_ret, t_out);
  EXPECT_GT(VSITime(nullptr), 0UL);

  const char* ctime_str = VSICTime(1000000UL);
  ASSERT_NE(nullptr, ctime_str);
  EXPECT_GT(strlen(ctime_str), 0u);

  time_t fixed_time = 1700000000;
  struct tm gm_res = {};
  EXPECT_EQ(&gm_res, VSIGMTime(&fixed_time, &gm_res));
  EXPECT_EQ(2023 - 1900, gm_res.tm_year);

  struct tm local_res = {};
  EXPECT_EQ(&local_res, VSILocalTime(&fixed_time, &local_res));

  const char* err_str = VSIStrerror(ENOENT);
  ASSERT_NE(nullptr, err_str);
  EXPECT_GT(strlen(err_str), 0u);

  GIntBig phys_ram = CPLGetPhysicalRAM();
  EXPECT_GT(phys_ram, 0);

  GIntBig usable_ram = CPLGetUsablePhysicalRAM();
  EXPECT_GT(usable_ram, 0);
  EXPECT_LE(usable_ram, phys_ram);

#ifdef RLIMIT_RSS
  struct rlimit orig_rss;
  if (getrlimit(RLIMIT_RSS, &orig_rss) == 0) {
    absl::Cleanup restore_rss = [&orig_rss] {
      setrlimit(RLIMIT_RSS, &orig_rss);
    };
    struct rlimit low_rss = orig_rss;
    low_rss.rlim_cur = 1024 * 1024;
    if (setrlimit(RLIMIT_RSS, &low_rss) == 0) {
      EXPECT_LE(CPLGetUsablePhysicalRAM(), 1024 * 1024);
    }
  }
#endif
}

}  // namespace
}  // namespace autotest2
