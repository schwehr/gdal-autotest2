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
// Tests for GDAL's Unix 64-bit stdio filesystem handler.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_vsil_unix_stdio_64.cpp

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

TEST(CplVsilUnixStdio64Test, OpenReadWriteSeekAndVsiCacheOption) {
  const std::string base_dir =
      absl::StrCat(::testing::TempDir(), "/unix_stdio_64_rw");
  ASSERT_EQ(0, VSIMkdir(base_dir.c_str(), 0755));
  absl::Cleanup rm_dir = [&base_dir] { VSIRmdirRecursive(base_dir.c_str()); };

  // Opening non-existent file with bSetError=true and bSetError=false.
  const std::string missing_file = absl::StrCat(base_dir, "/missing.bin");
  EXPECT_EQ(nullptr, VSIFOpenExL(missing_file.c_str(), "rb", TRUE));
  EXPECT_EQ(nullptr, VSIFOpenExL(missing_file.c_str(), "rb", FALSE));

  const std::string filepath = absl::StrCat(base_dir, "/test.bin");
  VSILFILE* fp = VSIFOpenL(filepath.c_str(), "wb+");
  ASSERT_NE(nullptr, fp);

  EXPECT_NE(nullptr, VSIFGetNativeFileDescriptorL(fp));
  EXPECT_EQ(0, VSIFTellL(fp));

  // Write followed immediately by Read (triggers internal VSI_FSEEK64 to flush
  // write buffer before reading).
  std::string data(8192, 'A');
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<char>('A' + (i % 26));
  }
  EXPECT_EQ(data.size(), VSIFWriteL(data.data(), 1, data.size(), fp));
  EXPECT_EQ(data.size(), VSIFTellL(fp));

  char buf[128] = {};
  // Reading at end of file right after write triggers bLastOpWrite seek and
  // hits EOF.
  EXPECT_EQ(0, VSIFReadL(buf, 1, 10, fp));
  EXPECT_NE(0, VSIFEofL(fp));
  EXPECT_EQ(0, VSIFErrorL(fp));
  VSIFClearErrL(fp);
  EXPECT_EQ(0, VSIFEofL(fp));

  // Seek to 10, read 10 bytes, then immediately Write (triggers bLastOpRead
  // seek before fwrite).
  EXPECT_EQ(0, VSIFSeekL(fp, 10, SEEK_SET));
  // Seeking to current offset in non-a+ mode short-circuits.
  EXPECT_EQ(0, VSIFSeekL(fp, 10, SEEK_SET));
  EXPECT_EQ(10, VSIFReadL(buf, 1, 10, fp));
  EXPECT_EQ(20, VSIFTellL(fp));
  EXPECT_EQ(5, VSIFWriteL("12345", 1, 5, fp));
  EXPECT_EQ(25, VSIFTellL(fp));

  // Negative relative seek via SEEK_CUR (passes nOffsetIn > INT_MAX to
  // VSIUnixStdioHandle::Seek).
  EXPECT_EQ(0, fp->Seek(static_cast<vsi_l_offset>(-5), SEEK_CUR));
  EXPECT_EQ(20, VSIFTellL(fp));

  // Truncate and Flush.
  EXPECT_EQ(0, VSIFFlushL(fp));
  EXPECT_EQ(0, VSIFTruncateL(fp, 6000));
  EXPECT_EQ(0, VSIFCloseL(fp));
  // Calling Close() again on the underlying handle is a no-op.

  // Reopen in read-only mode to test small forward seek optimization (< 4096
  // bytes uses fread instead of fseek) and large forward seek (>= 4096 bytes).
  fp = VSIFOpenL(filepath.c_str(), "rb");
  ASSERT_NE(nullptr, fp);
  EXPECT_EQ(0, VSIFSeekL(fp, 100, SEEK_SET));
  EXPECT_EQ(100, VSIFTellL(fp));
  EXPECT_EQ(0, VSIFSeekL(fp, 5500, SEEK_SET));
  EXPECT_EQ(5500, VSIFTellL(fp));
  EXPECT_EQ(0, VSIFCloseL(fp));

  // Reopen in "a+" mode (sets bModeAppendReadWrite = true).
  fp = VSIFOpenL(filepath.c_str(), "a+");
  ASSERT_NE(nullptr, fp);
  EXPECT_EQ(0, VSIFSeekL(fp, 0, SEEK_SET));
  EXPECT_EQ(5, VSIFReadL(buf, 1, 5, fp));
  EXPECT_EQ(0, VSIFCloseL(fp));

  // Reopen in read-only mode with VSI_CACHE=TRUE.
  {
    CPLConfigOptionSetter cache_opt("VSI_CACHE", "TRUE", false);
    VSILFILE* fp_cached = VSIFOpenL(filepath.c_str(), "rb");
    ASSERT_NE(nullptr, fp_cached);
    EXPECT_EQ(10, VSIFReadL(buf, 1, 10, fp_cached));
    EXPECT_EQ(0, VSIFCloseL(fp_cached));
  }
}

TEST(CplVsilUnixStdio64Test, CreateOnlyVisibleAtCloseTimeAndCancelCreation) {
  const std::string base_dir =
      absl::StrCat(::testing::TempDir(), "/unix_stdio_64_atomic");
  ASSERT_EQ(0, VSIMkdir(base_dir.c_str(), 0755));
  absl::Cleanup rm_dir = [&base_dir] { VSIRmdirRecursive(base_dir.c_str()); };

  auto* handler = VSIFileManager::GetHandler(base_dir.c_str());
  ASSERT_NE(nullptr, handler);

  // 1. Create file only visible at close time and close normally.
  const std::string target1 = absl::StrCat(base_dir, "/visible_on_close.bin");
  {
    auto handle1 =
        handler->CreateOnlyVisibleAtCloseTime(target1.c_str(), true, nullptr);
    ASSERT_NE(nullptr, handle1);
    EXPECT_EQ(4, handle1->Write("data", 1, 4));
    EXPECT_EQ(0, handle1->Close());
  }
  VSIStatBufL stat_buf;
  EXPECT_EQ(0, VSIStatL(target1.c_str(), &stat_buf));
  EXPECT_EQ(4, stat_buf.st_size);

  // 2. Create file only visible at close time and call CancelCreation().
  const std::string target2 = absl::StrCat(base_dir, "/cancelled_on_close.bin");
  {
    auto handle2 =
        handler->CreateOnlyVisibleAtCloseTime(target2.c_str(), true, nullptr);
    ASSERT_NE(nullptr, handle2);
    EXPECT_EQ(4, handle2->Write("data", 1, 4));
    handle2->CancelCreation();
    EXPECT_EQ(0, handle2->Close());
  }
  EXPECT_EQ(-1, VSIStatL(target2.c_str(), &stat_buf));

  // 3. Also test CancelCreation() on a normal handle (where !m_bUnlinkedFile).
  const std::string target3 = absl::StrCat(base_dir, "/normal_cancelled.bin");
  {
    auto handle3 = handler->Open(target3.c_str(), "wb+", false, nullptr);
    ASSERT_NE(nullptr, handle3);
    handle3->CancelCreation();
    EXPECT_EQ(0, handle3->Close());
  }
  EXPECT_EQ(-1, VSIStatL(target3.c_str(), &stat_buf));

  // 4. Test CreateOnlyVisibleAtCloseTime in a non-existent directory so
  // open(O_TMPFILE) fails and falls back to emulation.
  const std::string bad_target =
      absl::StrCat(base_dir, "/no_such_subdir/file.bin");
  EXPECT_EQ(nullptr, handler->CreateOnlyVisibleAtCloseTime(bad_target.c_str(),
                                                           false, nullptr));
}

TEST(CplVsilUnixStdio64Test, FilesystemQueriesSparseFilesAndRangeStatus) {
  const std::string base_dir =
      absl::StrCat(::testing::TempDir(), "/unix_stdio_64_fs");
  ASSERT_EQ(0, VSIMkdir(base_dir.c_str(), 0755));
  absl::Cleanup rm_dir = [&base_dir] { VSIRmdirRecursive(base_dir.c_str()); };

  EXPECT_GT(VSIGetDiskFreeSpace(base_dir.c_str()), 0);
  EXPECT_TRUE(VSIIsLocal(base_dir.c_str()));
  EXPECT_TRUE(VSISupportsSequentialWrite(base_dir.c_str(), false));
  EXPECT_TRUE(VSISupportsRandomWrite(base_dir.c_str(), false));

  const std::string non_existent_child =
      absl::StrCat(base_dir, "/non_existent_child.bin");
  EXPECT_TRUE(VSISupportsSequentialWrite(non_existent_child.c_str(), false));
  EXPECT_TRUE(VSISupportsRandomWrite(non_existent_child.c_str(), false));

  const int supports_sparse = VSISupportsSparseFiles(base_dir.c_str());
  EXPECT_EQ(FALSE, VSISupportsSparseFiles("/does/not/exist/path"));

  // Create a file with a sparse hole and query VSIFGetRangeStatusL.
  const std::string sparse_file = absl::StrCat(base_dir, "/sparse.bin");
  VSILFILE* fp = VSIFOpenL(sparse_file.c_str(), "wb+");
  ASSERT_NE(nullptr, fp);
  // Seek forward 1MB and write 4KB at the end.
  ASSERT_EQ(0, VSIFSeekL(fp, 1024 * 1024, SEEK_SET));
  std::string block(4096, 'X');
  ASSERT_EQ(block.size(), VSIFWriteL(block.data(), 1, block.size(), fp));
  ASSERT_EQ(0, VSIFFlushL(fp));

  const VSIRangeStatus status_hole = VSIFGetRangeStatusL(fp, 0, 4096);
  const VSIRangeStatus status_data = VSIFGetRangeStatusL(fp, 1024 * 1024, 4096);
  // GetRangeStatus is implemented with the FS_IOC_FIEMAP ioctl, which can fail
  // (-> VSI_RANGE_STATUS_UNKNOWN) under the test sandbox even when
  // VSISupportsSparseFiles() reports a sparse-capable filesystem. When the
  // ioctl does work, the hole and the written range must be classified
  // correctly; it must never misclassify one as the other.
  if (status_hole != VSI_RANGE_STATUS_UNKNOWN) {
    EXPECT_EQ(VSI_RANGE_STATUS_HOLE, status_hole);
  }
  if (status_data != VSI_RANGE_STATUS_UNKNOWN) {
    EXPECT_EQ(VSI_RANGE_STATUS_DATA, status_data);
  }
  if (supports_sparse != TRUE) {
    // On filesystems without sparse support, the ioctl path is not expected
    // to yield HOLE for written data either.
    EXPECT_NE(VSI_RANGE_STATUS_HOLE, status_data);
  }

  EXPECT_EQ(0, VSIFCloseL(fp));
}

TEST(CplVsilUnixStdio64Test, ReadDirExAndOpenDirWithRecursionAndFilters) {
  const std::string base_dir =
      absl::StrCat(::testing::TempDir(), "/unix_stdio_64_dir");
  ASSERT_EQ(0, VSIMkdir(base_dir.c_str(), 0755));
  absl::Cleanup rm_dir = [&base_dir] { VSIRmdirRecursive(base_dir.c_str()); };

  const std::string sub_dir = absl::StrCat(base_dir, "/sub");
  ASSERT_EQ(0, VSIMkdir(sub_dir.c_str(), 0755));

  const std::string file1 = absl::StrCat(base_dir, "/file1.txt");
  const std::string file2 = absl::StrCat(sub_dir, "/file2.txt");
  VSILFILE* fp1 = VSIFOpenL(file1.c_str(), "wb");
  ASSERT_NE(nullptr, fp1);
  EXPECT_EQ(3, VSIFWriteL("abc", 1, 3, fp1));
  ASSERT_EQ(0, VSIFCloseL(fp1));

  VSILFILE* fp2 = VSIFOpenL(file2.c_str(), "wb");
  ASSERT_NE(nullptr, fp2);
  EXPECT_EQ(4, VSIFWriteL("defg", 1, 4, fp2));
  ASSERT_EQ(0, VSIFCloseL(fp2));

  // Create a symlink inside base_dir to exercise DT_LNK.
  const std::string link_path = absl::StrCat(base_dir, "/link1.txt");
  EXPECT_EQ(0, symlink(file1.c_str(), link_path.c_str()));

  // ReadDirEx with max_files=1 stops after the first entry past the limit, so
  // exactly two of base_dir's five entries (".", "..", file1.txt, link1.txt,
  // sub) are returned.
  CPLStringList limited(VSIReadDirEx(base_dir.c_str(), 1));
  EXPECT_EQ(2, limited.size());

  // Unlimited listing of a directory with known contents.  The raw handler
  // includes "." and "..".
  auto* handler = VSIFileManager::GetHandler(base_dir.c_str());
  ASSERT_NE(nullptr, handler);
  CPLStringList sub_listing(handler->ReadDirEx(sub_dir.c_str(), 0));
  EXPECT_EQ(3, sub_listing.size());
  EXPECT_NE(-1, sub_listing.FindString("."));
  EXPECT_NE(-1, sub_listing.FindString(".."));
  EXPECT_NE(-1, sub_listing.FindString("file2.txt"));

  // VSIOpenDir on non-existent directory returns nullptr.
  EXPECT_EQ(nullptr, VSIOpenDir("/does/not/exist/dir", 0, nullptr));

  // Recursive VSIOpenDir without options.
  VSIDIR* dir = VSIOpenDir(base_dir.c_str(), /*nRecurseDepth=*/-1, nullptr);
  ASSERT_NE(nullptr, dir);
  int count = 0;
  while (const VSIDIREntry* entry = VSIGetNextDirEntry(dir)) {
    EXPECT_NE(nullptr, entry->pszName);
    ++count;
  }
  EXPECT_GE(count, 4);  // file1.txt, link1.txt, sub, sub/file2.txt
  VSICloseDir(dir);

  // Recursive VSIOpenDir with PREFIX="sub/file2" and NAME_AND_TYPE_ONLY="YES".
  CPLStringList options;
  options.SetNameValue("PREFIX", "sub/file2");
  options.SetNameValue("NAME_AND_TYPE_ONLY", "YES");
  VSIDIR* filtered_dir =
      VSIOpenDir(base_dir.c_str(), /*nRecurseDepth=*/-1, options.List());
  ASSERT_NE(nullptr, filtered_dir);
  int filtered_count = 0;
  while (const VSIDIREntry* entry = VSIGetNextDirEntry(filtered_dir)) {
    EXPECT_STREQ("sub/file2.txt", entry->pszName);
    ++filtered_count;
  }
  EXPECT_EQ(1, filtered_count);
  VSICloseDir(filtered_dir);

  // Clean up symlink before VSIRmdirRecursive.
  EXPECT_EQ(0, VSIUnlink(link_path.c_str()));
}

}  // namespace
}  // namespace autotest2
