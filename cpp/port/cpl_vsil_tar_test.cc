// Copyright 2017 Google Inc. All Rights Reserved.
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
// Tests for tar virtual file system (VSI) files.
//
//   http://trac.osgeo.org/gdal/browser/trunk/gdal/port/cpl_vsil_tar.cpp

#include <stddef.h>
#include <unistd.h>
#include <string>
#include <vector>

#include "logging.h"
#include "file/base/path.h"
#include "googletest.h"
#include "gunit.h"
#include "autotest2/cpp/util/cpl_cstringlist.h"
#include "autotest2/cpp/util/vsimem.h"
#include "port/cpl_port.h"
#include "port/cpl_string.h"
#include "port/cpl_vsi.h"

namespace autotest2 {
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

constexpr char kTestData[] = "cpp/port/testdata/tar";

TEST(CplVsiTarTest, ReadEmpty) {
  constexpr char kFilename[] = "empty.tar";
  const string filepath =
      file::JoinPath(FLAGS_test_srcdir, kTestData, kFilename);
  const string vsi_path = "/vsitar/" + filepath;

  EXPECT_EQ(kSuccess, access(filepath.c_str(), F_OK));
  char **string_list = VSIReadDir(vsi_path.c_str());
  auto entries = CslToVector(string_list);
  CSLDestroy(string_list);
  EXPECT_TRUE(entries.empty());
}

TEST(CplVsiTarTest, ReadVariety) {
  constexpr char kFilename[] = "variety.tar";
  const string filepath =
      file::JoinPath(FLAGS_test_srcdir, kTestData, kFilename);
  const string vsi_path = "/vsitar/" + filepath;

  EXPECT_EQ(kSuccess, access(filepath.c_str(), F_OK));
  char **string_list = VSIReadDir(vsi_path.c_str());
  auto entries = CslToVector(string_list);
  CSLDestroy(string_list);
  ASSERT_EQ(21, entries.size());
  EXPECT_EQ("1", entries[0]);
  EXPECT_EQ("10k-ff.gz", entries[1]);
  EXPECT_EQ("10k-ff.tar", entries[2]);
  EXPECT_EQ("10k-ff.tar.gz", entries[3]);
  EXPECT_EQ("all-bits", entries[4]);
  EXPECT_EQ("all-bits-dir", entries[5]);
  EXPECT_EQ("all-perm", entries[6]);
  // EXPECT_EQ("anyfile", entries[7]);
  EXPECT_EQ("baz.zip", entries[7]);
  EXPECT_EQ("bin", entries[8]);
  EXPECT_EQ("CAPS", entries[9]);
  EXPECT_EQ("empty.zip", entries[10]);
  EXPECT_EQ("foo.gz", entries[11]);
  EXPECT_EQ("group-sticky", entries[12]);
  EXPECT_EQ("owner-read", entries[13]);
  // Looses all the puctuation.  Should be: Punc!@#$%^&*()-=_+[]{};':",<>
  EXPECT_EQ("Punc", entries[14]);
  EXPECT_EQ("qux.zip", entries[15]);
  EXPECT_EQ("sticky", entries[16]);
  EXPECT_EQ("sticky-dir", entries[17]);
  EXPECT_EQ("sticky-permission", entries[18]);
  EXPECT_EQ("z", entries[19]);
  EXPECT_EQ(".dotfile", entries[20]);

  const string long_path =
      vsi_path + "/1/2/3/4/5/6/7/8/9/10/11/12/13/14/15/16/17/18/19/20";
  string_list = VSIReadDir(long_path.c_str());
  entries = CslToVector(string_list);
  CSLDestroy(string_list);
  ASSERT_EQ(1, entries.size());
  EXPECT_EQ("ok", entries[0]);
}

TEST(CplVsiTarTest, ReadVarietyFileContents) {
  constexpr char kFilename[] = "variety.tar";
  const string filepath =
      file::JoinPath(FLAGS_test_srcdir, kTestData, kFilename);
  const string vsi_path = "/vsitar/" + filepath + "/z";

  constexpr size_t kBufSize = 10;
  constexpr size_t kFileSize = 4;
  char buf[kBufSize] = {};
  VSILFILE *file = VSIFOpenL(vsi_path.c_str(), "rb");
  ASSERT_NE(nullptr, file);
  CplVsiLFileCloser closer(file);
  EXPECT_EQ(kFileSize, VSIFReadL(buf, 1, kBufSize - 1, file));
  EXPECT_STREQ("abc\n", buf);
}

// TODO(schwehr): Test the stat of the files in variety.tar.

TEST(CplVsiTarTest, ReadTarGz) {
  constexpr char kFilename[] = "bar.tar.gz";
  const string filepath =
      file::JoinPath(FLAGS_test_srcdir, kTestData, kFilename);
  // No need to tell it about the gzip compression.
  const string vsi_path = "/vsitar/" + filepath;

  EXPECT_EQ(kSuccess, access(filepath.c_str(), F_OK));
  char **string_list = VSIReadDir(vsi_path.c_str());
  auto entries = CslToVector(string_list);
  CSLDestroy(string_list);
  ASSERT_EQ(1, entries.size());
  EXPECT_EQ("bar", entries[0]);

  const string bar_path = "/vsitar/" + filepath + "/bar";

  constexpr size_t kBufSize = 10;
  constexpr size_t kFileSize = 4;
  char buf[kBufSize] = {};
  VSILFILE *file = VSIFOpenL(vsi_path.c_str(), "rb");
  ASSERT_NE(nullptr, file);
  CplVsiLFileCloser closer(file);
  EXPECT_EQ(kFileSize, VSIFReadL(buf, 1, kBufSize - 1, file));
  EXPECT_STREQ("foo\n", buf);
}

}  // namespace
}  // namespace autotest2
