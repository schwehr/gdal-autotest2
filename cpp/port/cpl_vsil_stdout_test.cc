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
// Tests for GDAL's /vsistdout/ and /vsistdout_redirect/ virtual file system.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_vsil_stdout.cpp

#include <cstdio>
#include <cstring>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

size_t CaptureWriteFunction(const void* ptr, size_t size, size_t nmemb,
                            FILE* stream) {
  auto* output = reinterpret_cast<std::string*>(stream);
  if (output != nullptr && ptr != nullptr) {
    output->append(static_cast<const char*>(ptr), size * nmemb);
  }
  return nmemb;
}

TEST(CplVsilStdoutTest, StdoutHandlerBasicAndRedirection) {
  auto* handler = VSIFileManager::GetHandler("/vsistdout/");
  ASSERT_NE(nullptr, handler);
  EXPECT_FALSE(handler->SupportsRead("/vsistdout/"));

  VSIStatBufL stat_buf;
  EXPECT_EQ(-1, VSIStatL("/vsistdout/", &stat_buf));
  EXPECT_EQ(0, stat_buf.st_size);

  // Opening for read or update must fail.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, VSIFOpenL("/vsistdout/", "rb"));
    EXPECT_EQ(nullptr, VSIFOpenL("/vsistdout/", "w+"));
  }

  // Test default stdout flush path before redirecting.
  VSILFILE* fp_default = VSIFOpenL("/vsistdout/", "wb");
  ASSERT_NE(nullptr, fp_default);
  EXPECT_EQ(0, VSIFFlushL(fp_default));
  EXPECT_EQ(0, VSIFCloseL(fp_default));

  // Redirect stdout to a custom function and string buffer.
  std::string captured;
  VSIStdoutSetRedirection(CaptureWriteFunction,
                          reinterpret_cast<FILE*>(&captured));
  absl::Cleanup reset_redirect = [] {
    VSIStdoutSetRedirection(fwrite, stdout);
  };

  VSILFILE* fp = VSIFOpenL("/vsistdout/", "wb");
  ASSERT_NE(nullptr, fp);

  EXPECT_EQ(0, VSIFTellL(fp));
  EXPECT_EQ(0, VSIFEofL(fp));
  EXPECT_EQ(0, VSIFErrorL(fp));

  // Write data and verify offset and captured buffer.
  EXPECT_EQ(5, VSIFWriteL("hello", 1, 5, fp));
  EXPECT_EQ(5, VSIFTellL(fp));
  EXPECT_EQ("hello", captured);

  // Seek tests: no-op seeks succeed, others fail.
  EXPECT_EQ(0, VSIFSeekL(fp, 0, SEEK_CUR));
  EXPECT_EQ(0, VSIFSeekL(fp, 0, SEEK_END));
  EXPECT_EQ(0, VSIFSeekL(fp, 5, SEEK_SET));
  {
    WithQuietHandler quiet;
    EXPECT_EQ(-1, VSIFSeekL(fp, 0, SEEK_SET));
    EXPECT_EQ(-1, VSIFSeekL(fp, 1, SEEK_CUR));
  }

  // Read tests: 0-size read succeeds without error, non-zero fails and sets
  // error.
  char buf[10] = {};
  EXPECT_EQ(0, VSIFReadL(buf, 0, 5, fp));
  EXPECT_EQ(0, VSIFErrorL(fp));
  {
    WithQuietHandler quiet;
    EXPECT_EQ(0, VSIFReadL(buf, 1, 5, fp));
  }
  EXPECT_NE(0, VSIFErrorL(fp));
  VSIFClearErrL(fp);
  EXPECT_EQ(0, VSIFErrorL(fp));

  // Flush when redirected returns 0.
  EXPECT_EQ(0, VSIFFlushL(fp));
  EXPECT_EQ(0, VSIFCloseL(fp));
}

TEST(CplVsilStdoutTest, StdoutRedirectHandler) {
  auto* handler = VSIFileManager::GetHandler("/vsistdout_redirect/");
  ASSERT_NE(nullptr, handler);
  EXPECT_FALSE(handler->SupportsRead("/vsistdout_redirect/"));

  VSIStatBufL stat_buf;
  EXPECT_EQ(-1, VSIStatL("/vsistdout_redirect//vsimem/out.bin", &stat_buf));
  EXPECT_EQ(0, stat_buf.st_size);

  // Opening for read or update must fail.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, VSIFOpenL("/vsistdout_redirect//vsimem/out.bin", "rb"));
    EXPECT_EQ(nullptr, VSIFOpenL("/vsistdout_redirect//vsimem/out.bin", "w+"));
    // Opening with an underlying path that cannot be created (a file inside a
    // non-existent directory) must fail.
    const std::string bad_path =
        "/vsistdout_redirect/" + testing::TempDir() + "/no_such_dir/x.bin";
    EXPECT_EQ(nullptr, VSIFOpenL(bad_path.c_str(), "w"));
  }

  const char* kMemFile = "/vsimem/stdout_redirect_test.txt";
  absl::Cleanup cleanup_mem = [kMemFile] { VSIUnlink(kMemFile); };

  std::string redirect_path = std::string("/vsistdout_redirect/") + kMemFile;
  VSILFILE* fp = VSIFOpenL(redirect_path.c_str(), "wb");
  ASSERT_NE(nullptr, fp);

  EXPECT_EQ(0, VSIFTellL(fp));
  EXPECT_EQ(0, VSIFEofL(fp));
  EXPECT_EQ(0, VSIFErrorL(fp));

  EXPECT_EQ(6, VSIFWriteL("world!", 1, 6, fp));
  EXPECT_EQ(6, VSIFTellL(fp));
  EXPECT_EQ(0, VSIFFlushL(fp));

  {
    WithQuietHandler quiet;
    EXPECT_EQ(-1, VSIFSeekL(fp, 0, SEEK_SET));
  }

  char buf[10] = {};
  EXPECT_EQ(0, VSIFReadL(buf, 0, 5, fp));
  EXPECT_EQ(0, VSIFErrorL(fp));
  {
    WithQuietHandler quiet;
    EXPECT_EQ(0, VSIFReadL(buf, 1, 5, fp));
  }
  EXPECT_NE(0, VSIFErrorL(fp));
  VSIFClearErrL(fp);
  EXPECT_EQ(0, VSIFErrorL(fp));

  EXPECT_EQ(0, VSIFCloseL(fp));

  // Verify the underlying /vsimem/ file received the data.
  vsi_l_offset data_len = 0;
  GByte* mem_buf = VSIGetMemFileBuffer(kMemFile, &data_len, FALSE);
  ASSERT_NE(nullptr, mem_buf);
  EXPECT_EQ(6, data_len);
  EXPECT_EQ(0, memcmp(mem_buf, "world!", 6));
}

}  // namespace
}  // namespace autotest2
