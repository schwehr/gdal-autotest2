// Tests for GDAL's VSI virtual file system.
//
//   http://trac.osgeo.org/gdal/browser/trunk/gdal/port/cpl_vsil.cpp
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

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "gunit.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "port/cpl_vsi.h"
#include "port/cpl_vsi_virtual.h"

using std::fill;

namespace {

// Create a fake handler for testing that provides a stat
// that always succeeds and an open that always fails.
class DummyFilesystem : public VSIFilesystemHandler {
 public:
  DummyFilesystem();
  virtual VSIVirtualHandle *Open(const char *path, const char *access,
                                 bool bSetError);
  virtual int Stat(const char *path, VSIStatBufL *stat, int nFlags);

  // Record what happened during the test.
  string open_last_path_;
  string open_last_access_;
  string stat_last_path_;
};


DummyFilesystem::DummyFilesystem() {}

VSIVirtualHandle *DummyFilesystem::Open(const char *path, const char *access,
                                        bool /* bSetError */) {
  open_last_path_ = path;
  open_last_access_ = access;
  return nullptr;
}


int DummyFilesystem::Stat(const char *path, VSIStatBufL *stat, int nFlags) {
  stat_last_path_ = path;
  memset(stat, 0, sizeof(*stat));

  // Giving a size and mode is enough to fake that the file exists.
  stat->st_size = 1000;
  stat->st_mode = 33184;

  return 0;
}


// Tests adding a driver and calling open.
TEST(CplVsiFileManagerTest, FailToOpenExistingFile) {
  GDALAllRegister();

  // handler will be deleted by the VSIFilesystemHandler destructor.
  DummyFilesystem *dummy_handler(new DummyFilesystem);
  VSIFileManager::InstallHandler("/dummy/", dummy_handler);

  const string path("/dummy/foo.tif");

  GDALDataset *foo =
      reinterpret_cast<GDALDataset*>(GDALOpen(path.c_str(), GA_ReadOnly));

  ASSERT_EQ(nullptr, foo);
  // Also searches for /dummy/foo.tif.xml in GDAL 2.*.
  ASSERT_TRUE(dummy_handler->stat_last_path_.find(path) != std::string::npos);
  ASSERT_TRUE(dummy_handler->open_last_path_.find(path) != std::string::npos);
  ASSERT_STREQ("rb", dummy_handler->open_last_access_.c_str());
}

// Tests the VSI functions for non-existent file paths.
TEST(CplVSIFunctions, VSIFilesAndPathLocalDoesNotExist) {
  ASSERT_EQ(nullptr, VSIReadDir("/does/not/exist0"));
  ASSERT_EQ(nullptr, VSIReadDirRecursive("/does/not/exist1"));
  ASSERT_EQ(-1, VSIMkdir("/does/not/exist2", 0755));
  ASSERT_EQ(-1, VSIUnlink("/does/not/exist3"));
  ASSERT_EQ(-1, VSIRmdir("/does/not/exist4"));

  std::unique_ptr<VSIStatBufL> stat_buf(new VSIStatBufL);

  memset(stat_buf.get(), 0, sizeof(VSIStatBufL));
  ASSERT_EQ(-1, VSIStatL("/does/not/exist5", stat_buf.get()));
  ASSERT_EQ(0, stat_buf->st_mode);
  ASSERT_EQ(0, stat_buf->st_size);

  memset(stat_buf.get(), 0, sizeof(VSIStatBufL));
  ASSERT_EQ(-1, VSIStatExL("/does/not/exist6", stat_buf.get(), 0));
  ASSERT_EQ(0, stat_buf->st_mode);
  ASSERT_EQ(0, stat_buf->st_size);
}

// Tests most of the file operations by creating a directory and a file.
TEST(CplVSIFunctions, VSIFileAndDirOpsLocal) {
  const string temp_dir(FLAGS_test_tmpdir);

  constexpr int kSuccess = 0;
  constexpr int kFailure = -1;

  std::unique_ptr<VSIStatBufL> stat_buf(new VSIStatBufL);
  memset(stat_buf.get(), 0, sizeof(VSIStatBufL));
  ASSERT_EQ(kSuccess, VSIStatL(temp_dir.c_str(), stat_buf.get()));

  const string src_dir(temp_dir + "/foo");
  const string dst_dir(temp_dir + "/bar");
  ASSERT_EQ(kSuccess, VSIMkdir(src_dir.c_str(), 0755));
  ASSERT_EQ(kSuccess, VSIRename(src_dir.c_str(), dst_dir.c_str()));

  memset(stat_buf.get(), 0, sizeof(VSIStatBufL));
  ASSERT_EQ(kSuccess, VSIStatL(dst_dir.c_str(), stat_buf.get()));
  ASSERT_EQ(stat_buf->st_mode, 040755);

  const int case_sensitive = VSIIsCaseSensitiveFS(temp_dir.c_str());
  ASSERT_TRUE((0 == case_sensitive || 1 == case_sensitive));

  const string filename(dst_dir + "/baz.txt");

  VSILFILE *file = VSIFOpenL(filename.c_str(), "w");
  ASSERT_NE(nullptr, file);

  ASSERT_EQ(0, VSIFTellL(file));

  // Cannot seek to before the file or to
  // std::numeric_limits<vsi_l_offset>::max()
  ASSERT_EQ(kFailure, VSIFSeekL(file, -1, SEEK_SET));
  ASSERT_EQ(kFailure, VSIFSeekL(file, -1, SEEK_CUR));
  ASSERT_EQ(kFailure, VSIFSeekL(file, -1, SEEK_END));

  ASSERT_EQ(0, VSIFTellL(file));

  ASSERT_EQ(0, VSIFSeekL(file, 0, SEEK_SET));
  ASSERT_EQ(0, VSIFSeekL(file, 0, SEEK_CUR));
  ASSERT_EQ(0, VSIFSeekL(file, 0, SEEK_END));

  ASSERT_EQ(0, VSIFTellL(file));

  ASSERT_EQ(3, VSIFWriteL("012", 1, 3, file));
  ASSERT_EQ(kSuccess, VSIFFlushL(file));
  ASSERT_EQ(3, VSIFTellL(file));

  memset(stat_buf.get(), 0, sizeof(VSIStatBufL));
  ASSERT_EQ(kSuccess, VSIStatL(filename.c_str(), stat_buf.get()));
  ASSERT_EQ(3, stat_buf->st_size);

  VSIRewindL(file);
  ASSERT_EQ(0, VSIFTellL(file));

  ASSERT_EQ(kSuccess, VSIFCloseL(file));

  file = VSIFOpenL(filename.c_str(), "r");
  ASSERT_NE(nullptr, file);

  char buf[43] = {};
  ASSERT_EQ(1, VSIFReadL(buf, 1, 1, file));
  ASSERT_STREQ("0", buf);
  ASSERT_EQ(false, bool(VSIFEofL(file)));

  ASSERT_EQ(2, VSIFReadL(buf, 1, 10, file));
  ASSERT_EQ(true, bool(VSIFEofL(file)));

  ASSERT_EQ(kSuccess, VSIFSeekL(file, -2, SEEK_CUR));
  ASSERT_EQ(false, bool(VSIFEofL(file)));
  ASSERT_EQ(1, VSIFTellL(file));

  // TODO(schwehr): Why does this work this way?
  // Seeking past the end.
  ASSERT_EQ(kSuccess, VSIFSeekL(file, 5, SEEK_CUR));
  ASSERT_EQ(6, VSIFTellL(file));
  ASSERT_EQ(false, bool(VSIFEofL(file)));

  ASSERT_EQ(kSuccess, VSIFCloseL(file));

  memset(stat_buf.get(), 0, sizeof(VSIStatBufL));
  ASSERT_EQ(kSuccess, VSIStatL(filename.c_str(), stat_buf.get()));
  ASSERT_EQ(3, stat_buf->st_size);

  // Cannot truncate when opened read-only.
  file = VSIFOpenL(filename.c_str(), "r");
  ASSERT_NE(nullptr, file);
  ASSERT_EQ(-1, VSIFTruncateL(file, 1));
  ASSERT_EQ(kSuccess, VSIFCloseL(file));

  file = VSIFOpenL(filename.c_str(), "r+");
  ASSERT_NE(nullptr, file);
  ASSERT_EQ(3, VSIFReadL(buf, 1, 10, file));
  ASSERT_EQ(true, bool(VSIFEofL(file)));
  ASSERT_EQ(kSuccess, VSIFTruncateL(file, 1));
  ASSERT_EQ(true, bool(VSIFEofL(file)));
  ASSERT_EQ(kSuccess, VSIFFlushL(file));
  // Truncate does not modify the offset of open files.
  ASSERT_EQ(3, VSIFTellL(file));
  memset(stat_buf.get(), 0, sizeof(VSIStatBufL));
  ASSERT_EQ(kSuccess, VSIStatL(filename.c_str(), stat_buf.get()));
  ASSERT_EQ(1, stat_buf->st_size);

  ASSERT_EQ(1, VSIFWriteL("3", 1, 1, file));
  ASSERT_EQ(kSuccess, VSIFFlushL(file));
  ASSERT_EQ(4, VSIFTellL(file));

  VSIRewindL(file);
  ASSERT_EQ(0, VSIFTellL(file));
  ASSERT_EQ(4, VSIFReadL(buf, 1, 10, file));

  ASSERT_STREQ("0", buf);
  ASSERT_EQ('\0', buf[1]);
  ASSERT_EQ('\0', buf[2]);
  ASSERT_STREQ("3", buf+3);

  // Truncate can also grow a file.
  ASSERT_EQ(kSuccess, VSIFTruncateL(file, 42));
  ASSERT_EQ(kSuccess, VSIFFlushL(file));
  memset(stat_buf.get(), 0, sizeof(VSIStatBufL));
  ASSERT_EQ(kSuccess, VSIStatL(filename.c_str(), stat_buf.get()));
  ASSERT_EQ(42, stat_buf->st_size);

  // Truncate does not move the file index forward or backwards.
  ASSERT_EQ(4, VSIFTellL(file));
  VSIRewindL(file);
  ASSERT_EQ(42, VSIFReadL(buf, 1, 43, file));
  ASSERT_STREQ("0", buf);
  ASSERT_EQ('\0', buf[1]);
  ASSERT_EQ('\0', buf[2]);
  ASSERT_STREQ("3", buf+3);
  ASSERT_EQ('\0', buf[23]);
  ASSERT_EQ('\0', buf[24]);

  ASSERT_EQ(kSuccess, VSIFCloseL(file));

  // Fail to rmdir with contents.
  ASSERT_EQ(-1, VSIRmdir(dst_dir.c_str()));

  ASSERT_EQ(kSuccess, VSIUnlink(filename.c_str()));
  ASSERT_EQ(kSuccess, VSIRmdir(dst_dir.c_str()));
}

// Test reading more than is available in a file on a normal filesystem.
// Most likely uses Read from cpl_vsil_unix_stdio_64.cpp.
TEST(CplVSIFunctions, VSIReadTooMuchLocalFileSystem) {
  const string temp_dir(FLAGS_test_tmpdir);
  const string filename(temp_dir + "/short.txt");
  const string contents("012\n\n");
  const int length = contents.length();

  // Write a small amount of data.
  VSILFILE *file = VSIFOpenL(filename.c_str(), "w");
  ASSERT_NE(nullptr, file);
  ASSERT_EQ(length, VSIFWriteL(contents.c_str(), 1, length, file));
  ASSERT_EQ(0, VSIFFlushL(file));
  ASSERT_EQ(0, VSIFCloseL(file));
  file = nullptr;

  const int buf_size = 1024;
  char buf[buf_size] = {};

  // Read the file normally.
  file = VSIFOpenL(filename.c_str(), "r");
  ASSERT_NE(nullptr, file);
  EXPECT_EQ(length, VSIFReadL(buf, 1, length, file));
  EXPECT_STREQ(contents.c_str(), buf);
  ASSERT_EQ(0, VSIFCloseL(file));
  file = nullptr;
  fill(buf, buf + buf_size, 0);

  // Try to read way too much.
  file = VSIFOpenL(filename.c_str(), "r");
  ASSERT_NE(nullptr, file);
  EXPECT_EQ(length, VSIFReadL(buf, 1, buf_size, file));
  EXPECT_STREQ(contents.c_str(), buf);
  ASSERT_EQ(0, VSIFCloseL(file));
  file = nullptr;
  fill(buf, buf + buf_size, 0);

  // Try to read way too much with swapped size and count.
  file = VSIFOpenL(filename.c_str(), "r");
  ASSERT_NE(nullptr, file);
  // 1, 1024 -> 1024, 1
  // It will be unable to read 1 block of 1024 bytes.
  EXPECT_EQ(0, VSIFReadL(buf, buf_size, 1, file));
  // The read reports nothing read, but buf is set.
  EXPECT_STREQ(contents.c_str(), buf);
  ASSERT_EQ(0, VSIFCloseL(file));
}

// Test reading more than is available in a memory filesystem.
TEST(CplVSIFunctions, VSIFReadLTooMuchVsimem) {
  const string filename("/vsimem/short.txt");
  const string contents("\n\nabc");
  const int length = contents.length();

  // Write a small amount of data.
  VSILFILE *file = VSIFOpenL(filename.c_str(), "w");
  ASSERT_NE(nullptr, file);
  ASSERT_EQ(length, VSIFWriteL(contents.c_str(), 1, length, file));
  ASSERT_EQ(0, VSIFFlushL(file));
  ASSERT_EQ(0, VSIFCloseL(file));
  file = nullptr;

  const int buf_size = 1024;
  char buf[buf_size] = {};

  // Try to read way too much.
  file = VSIFOpenL(filename.c_str(), "r");
  ASSERT_NE(nullptr, file);
  EXPECT_EQ(length, VSIFReadL(buf, 1, buf_size, file));
  EXPECT_STREQ(contents.c_str(), buf);
  ASSERT_EQ(0, VSIFCloseL(file));
}

// TODO(schwehr): Test the rest of cpl_vsil.cpp.

}  // namespace
