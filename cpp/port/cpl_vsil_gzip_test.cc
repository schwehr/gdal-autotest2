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
//
// Tests for gzipped virtual file system (VSI) files.
//
//   http://trac.osgeo.org/gdal/browser/trunk/gdal/port/cpl_vsil_gzip.cpp

#include "port/cpl_port.h"

#include <sys/types.h>
#include <string>

#include "logging.h"
#include "file/base/path.h"
#include "gmock.h"
#include "gunit.h"
#include "autotest2/cpp/util/vsimem.h"
#include "port/cpl_vsi.h"

namespace {

constexpr int kSuccess = 0;
constexpr int kFailure = -1;

// TODO(schwehr): Move CplVsiLFileCloser to utils.
class CplVsiLFileCloser {
 public:
  CplVsiLFileCloser(VSILFILE *file) : file_(CHECK_NOTNULL(file)) {}
  ~CplVsiLFileCloser() { CHECK_EQ(kSuccess, VSIFCloseL(file_)); }

 private:
  VSILFILE *file_;
};

constexpr char kTestData[] =
    "autotest2/cpp/port/testdata/gzip";
constexpr char kFilename[] = "count.gz";

constexpr off_t kCompressedSize = 67;
constexpr off_t kUncompressedSize = 125;

TEST(CplVsiGzipTest, Read) {
  const string filepath =
      file::JoinPath(FLAGS_test_srcdir, kTestData, kFilename);

  constexpr int kBufSize = 256;

  // Read raw.
  {
    char buf[kBufSize] = {};
    VSILFILE *file = VSIFOpenL(filepath.c_str(), "r");
    ASSERT_NE(nullptr, file);
    CplVsiLFileCloser closer(file);
    EXPECT_EQ(kCompressedSize, VSIFReadL(buf, 1, kBufSize - 1, file));

    // The gzip file contains the original filename.
    EXPECT_THAT(buf, testing::ContainsRegex("count"));
  }

  // Read uncompressed.
  {
    const string vsipath = "/vsigzip/" + filepath;
    char buf[kBufSize] = {};
    VSILFILE *file = VSIFOpenL(vsipath.c_str(), "r");
    ASSERT_NE(nullptr, file) << vsipath;
    CplVsiLFileCloser closer(file);
    EXPECT_EQ(kUncompressedSize, VSIFReadL(buf, 1, kBufSize - 1, file));

    // Look for some text in the original file.
    EXPECT_THAT(buf, testing::ContainsRegex("end"));
  }

  // Read from vsimem.
  const char kCountGz[] =
      "\x1f\x8b\x08\x08\x27\x82\xda\x58\x02\x03\x63\x6f\x75\x6e\x74\x00\x4d\xc6"
      "\xab\x01\x00\x20\x08\x40\xc1\xce\x34\x80\xfc\xdc\x47\xab\xfb\x47\xeb\xbb"
      "\x74\xaa\xe6\x2b\xb2\x7a\xb6\x18\xee\xf8\xc2\x03\x4f\xbc\xf0\xc6\x07\xdf"
      "\xf8\x7d\x47\xe4\x03\x9c\xa1\xb4\x30\x7d\x00\x00\x00";

  const char kFilename[] = "/vsimem/a";
  const string data(reinterpret_cast<const char *>(kCountGz),
                    CPL_ARRAYSIZE(kCountGz) - 1);
  autotest2::VsiMemTempWrapper wrapper(kFilename, data);

  // Read raw.
  {
    char buf[kBufSize] = {};
    VSILFILE *file = VSIFOpenL(kFilename, "r");
    ASSERT_NE(nullptr, file);
    CplVsiLFileCloser closer(file);
    EXPECT_EQ(kCompressedSize, VSIFReadL(buf, 1, kBufSize - 1, file));

    // The gzip file contains the original filename.
    EXPECT_THAT(buf, testing::ContainsRegex("count"));
  }

  // Read uncompressed.
  {
    // Note the double slash: //
    // A single slash between the two vsi paths does not work:
    //   /vsigzip/vsimem/a
    char buf[kBufSize] = {};
    VSILFILE *file = VSIFOpenL("/vsigzip//vsimem/a", "r");
    ASSERT_NE(nullptr, file);
    CplVsiLFileCloser closer(file);
    EXPECT_EQ(kUncompressedSize, VSIFReadL(buf, 1, kBufSize - 1, file));

    // Look for some text in the original file.
    EXPECT_THAT(buf, testing::ContainsRegex("end"));
  }
}

// TODO(schwehr): Test opening an empty file that claims to be a gzip.
// TODO(schwehr): Test non-empty files that claims to be a gzip, but is not.
// TODO(schwehr): Test corrupt gzip file(s).

// Test how stat behaves outside and inside of gzipped file.
TEST(CplVsiGzipTest, Stat) {
  const string filepath =
      file::JoinPath(FLAGS_test_srcdir, kTestData, kFilename);

  VSIStatBufL stat;
  ASSERT_EQ(kSuccess, VSIStatExL(filepath.c_str(), &stat, VSI_STAT_SIZE_FLAG));
  EXPECT_EQ(kCompressedSize, stat.st_size);

  const string vsipath = "/vsigzip/" + filepath;
  ASSERT_EQ(kSuccess, VSIStatExL(vsipath.c_str(), &stat, VSI_STAT_SIZE_FLAG))
      << vsipath;
  EXPECT_EQ(kUncompressedSize, stat.st_size) << vsipath;
}

// TODO(schwehr): Test writing, seek, tell, and other operations.

}  // namespace
