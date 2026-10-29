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
// Tests for GDAL's /vsistdin/ virtual file system.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_vsil_stdin.cpp

#include <cstdio>
#include <cstring>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

TEST(CplVsilStdinTest, HandlerCapabilitiesAndParseFilenameErrors) {
  auto* handler = VSIFileManager::GetHandler("/vsistdin/");
  ASSERT_NE(nullptr, handler);
  EXPECT_FALSE(handler->SupportsSequentialWrite("/vsistdin/", false));
  EXPECT_FALSE(handler->SupportsRandomWrite("/vsistdin/", false));

  // Invalid filenames (dots in query or wrong path).
  EXPECT_EQ(nullptr, VSIFOpenL("/vsistdin/extra", "rb"));
  EXPECT_EQ(nullptr, VSIFOpenL("/vsistdin?foo.bar", "rb"));
  VSIStatBufL stat_buf;
  EXPECT_EQ(-1, VSIStatL("/vsistdin/extra", &stat_buf));

  // Disabling /vsistdin/ via CPL_ALLOW_VSISTDIN=NO.
  {
    CPLConfigOptionSetter disallow("CPL_ALLOW_VSISTDIN", "NO", false);
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, VSIFOpenL("/vsistdin/", "rb"));
    EXPECT_EQ(-1, VSIStatL("/vsistdin/", &stat_buf));
  }

  // Opening for write or update fails.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, VSIFOpenL("/vsistdin/", "wb"));
    EXPECT_EQ(nullptr, VSIFOpenL("/vsistdin/", "r+"));
  }

  // Non-existent CPL_VSISTDIN_FILE fails ParseFilename.
  {
    CPLConfigOptionSetter bad_file("CPL_VSISTDIN_FILE",
                                   "/does/not/exist/stdin_test.bin", false);
    EXPECT_EQ(nullptr, VSIFOpenL("/vsistdin/", "rb"));
  }
}

TEST(CplVsilStdinTest, ReadSeekCacheLimitAndStat) {
  // Create a temporary file with 20,000 bytes to serve as CPL_VSISTDIN_FILE.
  const std::string temp_file =
      absl::StrCat(::testing::TempDir(), "/vsistdin_test_input.bin");
  std::string content(20000, '\0');
  for (size_t i = 0; i < content.size(); ++i) {
    content[i] = static_cast<char>('A' + (i % 26));
  }
  {
    FILE* f = fopen(temp_file.c_str(), "wb");
    ASSERT_NE(nullptr, f);
    ASSERT_EQ(content.size(), fwrite(content.data(), 1, content.size(), f));
    ASSERT_EQ(0, fclose(f));
  }
  absl::Cleanup unlink_temp = [&temp_file] { VSIUnlink(temp_file.c_str()); };

  CPLConfigOptionSetter stdin_file("CPL_VSISTDIN_FILE", temp_file.c_str(),
                                   false);
  // Start with a small buffer limit (100 bytes) to test buffer growth and
  // backward seek failures beyond the buffer limit.
  CPLConfigOptionSetter buf_limit("CPL_VSISTDIN_BUFFER_LIMIT", "100", false);

  // Open with unsupported query option to exercise warning branch.
  VSILFILE* fp = nullptr;
  {
    WithQuietHandler quiet;
    fp = VSIFOpenL("/vsistdin?buffer_limit=100&unknown_opt=1", "rb");
  }
  ASSERT_NE(nullptr, fp);

  EXPECT_EQ(0, VSIFTellL(fp));
  EXPECT_EQ(0, VSIFEofL(fp));
  EXPECT_EQ(0, VSIFErrorL(fp));

  // Write is unsupported.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(0, VSIFWriteL("abc", 1, 3, fp));
  }

  // Zero-byte read returns 0.
  char buf[200] = {};
  EXPECT_EQ(0, VSIFReadL(buf, 0, 10, fp));

  // Read 40 bytes (triggers realloc growth < gnBufferLimit - newAlloc/3).
  EXPECT_EQ(40, VSIFReadL(buf, 1, 40, fp));
  EXPECT_EQ(40, VSIFTellL(fp));
  EXPECT_EQ(0, memcmp(buf, content.data(), 40));

  // Read another 80 bytes (crosses gnBufferLimit=100; newAlloc clamps to
  // gnBufferLimit, and gnRealPos becomes 120 >= gnBufferLimit).
  EXPECT_EQ(80, VSIFReadL(buf, 1, 80, fp));
  EXPECT_EQ(120, VSIFTellL(fp));
  EXPECT_EQ(0, memcmp(buf, content.data() + 40, 80));

  // Seek to current offset is a no-op.
  EXPECT_EQ(0, VSIFSeekL(fp, 120, SEEK_SET));

  // Seeking to an offset >= gnBufferLimit when gnRealPos >= gnBufferLimit
  // fails.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(-1, VSIFSeekL(fp, 110, SEEK_SET));
  }

  // Seeking backward within cached buffer (< gnBufferLen = 100) succeeds!
  EXPECT_EQ(0, VSIFSeekL(fp, 20, SEEK_SET));
  EXPECT_EQ(20, VSIFTellL(fp));

  // Reading entirely from cached buffer [20..50).
  EXPECT_EQ(30, VSIFReadL(buf, 1, 30, fp));
  EXPECT_EQ(50, VSIFTellL(fp));
  EXPECT_EQ(0, memcmp(buf, content.data() + 20, 30));

  // SEEK_CUR within cached buffer [50 + 30 = 80).
  EXPECT_EQ(0, VSIFSeekL(fp, 30, SEEK_CUR));
  EXPECT_EQ(80, VSIFTellL(fp));

  // Attempting to read 30 bytes from offset 80 (80 + 30 > gnBufferLimit=100)
  // while gnRealPos (120) >= gnBufferLimit fails because bytes [100..110) were
  // not cached!
  {
    WithQuietHandler quiet;
    EXPECT_EQ(0, VSIFReadL(buf, 1, 30, fp));
  }

  {
    CPLConfigOptionSetter close_first("CPL_VSISTDIN_FILE_CLOSE", "YES", false);
    EXPECT_EQ(0, VSIFCloseL(fp));
  }

  // Now test larger buffer_limit via MB/GB suffixes in GetBufferLimit,
  // forward seeks, Stat, SEEK_END, and CPL_VSISTDIN_RESET_POSITION=YES.
  {
    VSILFILE* fp2 = VSIFOpenL("/vsistdin/?buffer_limit=1MB", "rb");
    ASSERT_NE(nullptr, fp2);

    // Read 50 bytes, seek back to 30, then read 50 bytes (20 from cache + 30
    // fresh from file).
    EXPECT_EQ(50, VSIFReadL(buf, 1, 50, fp2));
    EXPECT_EQ(0, VSIFSeekL(fp2, 30, SEEK_SET));
    EXPECT_EQ(50, VSIFReadL(buf, 1, 50, fp2));
    EXPECT_EQ(80, VSIFTellL(fp2));
    EXPECT_EQ(0, memcmp(buf, content.data() + 30, 50));

    // Seek forward past 8192 bytes (loops in Seek()).
    EXPECT_EQ(0, VSIFSeekL(fp2, 10000, SEEK_SET));
    EXPECT_EQ(10000, VSIFTellL(fp2));

    // Seek with non-zero SEEK_END is unsupported.
    {
      WithQuietHandler quiet;
      EXPECT_EQ(-1, VSIFSeekL(fp2, -10, SEEK_END));
    }

    // Stat before seeking to end (Stat opens a handle and seeks to end).
    VSIStatBufL stat_buf;
    ASSERT_EQ(0, VSIStatExL("/vsistdin/", &stat_buf, VSI_STAT_SIZE_FLAG));
    EXPECT_EQ(content.size(), stat_buf.st_size);

    // Since gbHasSoughtToEnd is now true, SEEK_END returns immediately, and
    // Stat also uses the cached gnFileSize.
    EXPECT_EQ(0, VSIFSeekL(fp2, 0, SEEK_END));
    EXPECT_EQ(content.size(), VSIFTellL(fp2));
    ASSERT_EQ(0, VSIStatExL("/vsistdin/", &stat_buf, VSI_STAT_SIZE_FLAG));
    EXPECT_EQ(content.size(), stat_buf.st_size);

    // Seeking beyond actual EOF with SEEK_SET returns -1.
    EXPECT_EQ(-1, VSIFSeekL(fp2, content.size() + 500, SEEK_SET));

    VSIFClearErrL(fp2);
    EXPECT_EQ(0, VSIFEofL(fp2));
    EXPECT_EQ(0, VSIFErrorL(fp2));

    // Exercise CPL_VSISTDIN_RESET_POSITION=YES when gosStdinFilename matches.
    {
      CPLConfigOptionSetter reset_pos("CPL_VSISTDIN_RESET_POSITION", "YES",
                                      false);
      EXPECT_EQ(0, VSIStatExL("/vsistdin/", &stat_buf, 0));
    }

    // Closing with CPL_VSISTDIN_FILE_CLOSE=YES closes gStdinFile and resets
    // state.
    CPLConfigOptionSetter close_opt("CPL_VSISTDIN_FILE_CLOSE", "YES", false);
    EXPECT_EQ(0, VSIFCloseL(fp2));
  }

  // Exercise GetBufferLimit with GB, huge MB/GB/bytes, and resetting back to
  // "stdin" when gosStdinFilename is non-empty.
  {
    VSILFILE* fp3 = VSIFOpenL("/vsistdin?buffer_limit=1GB", "rb");
    ASSERT_NE(nullptr, fp3);
    EXPECT_EQ(0, VSIFCloseL(fp3));

    VSILFILE* fp4 =
        VSIFOpenL("/vsistdin?buffer_limit=18446744073709551615MB", "rb");
    ASSERT_NE(nullptr, fp4);
    EXPECT_EQ(0, VSIFCloseL(fp4));

    VSILFILE* fp5 =
        VSIFOpenL("/vsistdin?buffer_limit=18446744073709551615GB", "rb");
    ASSERT_NE(nullptr, fp5);
    EXPECT_EQ(0, VSIFCloseL(fp5));

    VSILFILE* fp6 =
        VSIFOpenL("/vsistdin?buffer_limit=18446744073709551615", "rb");
    ASSERT_NE(nullptr, fp6);
    EXPECT_EQ(0, VSIFCloseL(fp6));

    // Switch CPL_VSISTDIN_FILE back to "stdin" so ParseFilename cleans up
    // gosStdinFilename.
    CPLConfigOptionSetter default_stdin("CPL_VSISTDIN_FILE", "stdin", false);
    VSILFILE* fp7 = VSIFOpenL("/vsistdin/", "rb");
    ASSERT_NE(nullptr, fp7);
    EXPECT_EQ(0, VSIFCloseL(fp7));
  }
}

}  // namespace
}  // namespace autotest2
