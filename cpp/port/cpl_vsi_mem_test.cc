// Copyright 2017 Google Inc. All Rights Reserved.
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

#include <errno.h>
#include <stddef.h>

#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

constexpr int kSuccess = 0;

TEST(VSIBufferTest, NoTake) {
  constexpr char kFilename[] = "/vsimem/a";
  std::vector<GByte> src = {0x00, 0xff};

  // Do not take ownership.
  VSILFILE* f = VSIFileFromMemBuffer(kFilename, &src[0], src.size(), false);
  ASSERT_NE(nullptr, f);
  auto closer = absl::MakeCleanup([f, kFilename] {
    VSIFCloseL(f);
    VSIUnlink(kFilename);
  });

  VSIStatBufL stat_buf;
  ASSERT_EQ(kSuccess, VSIStatL(kFilename, &stat_buf));
  ASSERT_EQ(2, stat_buf.st_size);

  vsi_l_offset data_length;
  char* tmp = reinterpret_cast<char*>(
      VSIGetMemFileBuffer(kFilename, &data_length, false));
  ASSERT_NE(nullptr, tmp);
  EXPECT_EQ(2, data_length);
  EXPECT_EQ(0, tmp[0]);
  EXPECT_EQ(0xff, tmp[1]);
}

TEST(VSIBufferTest, TakeOwnershipWriting) {
  constexpr char kFilename[] = "/vsimem/b";
  constexpr size_t size = 2;
  GByte* src = static_cast<GByte*>(CPLMalloc(size));
  src[0] = 0x00;
  src[1] = 0xaa;

  VSILFILE* f = VSIFileFromMemBuffer(kFilename, src, size, true);
  ASSERT_NE(nullptr, f);
  auto closer = absl::MakeCleanup([f, kFilename] {
    VSIFCloseL(f);
    VSIUnlink(kFilename);
  });

  vsi_l_offset data_length;
  char* tmp = reinterpret_cast<char*>(
      VSIGetMemFileBuffer(kFilename, &data_length, false));
  ASSERT_NE(nullptr, tmp);
  EXPECT_EQ(2, data_length);
  EXPECT_EQ(0, tmp[0]);
  EXPECT_EQ(0xaa, tmp[1]);
}

TEST(VSIBufferTest, TakeOwnershipReading) {
  constexpr char kFilename[] = "/vsimem/c";
  std::vector<GByte> src = {0x00, 0x08};

  VSILFILE* f = VSIFOpenL(kFilename, "wb");
  ASSERT_NE(nullptr, f);
  ASSERT_EQ(1, VSIFWriteL(&src[0], src.size(), 1, f));
  ASSERT_EQ(kSuccess, VSIFCloseL(f));
  auto unlinker = absl::MakeCleanup([kFilename] { VSIUnlink(kFilename); });

  vsi_l_offset data_length;
  char* tmp = reinterpret_cast<char*>(
      VSIGetMemFileBuffer(kFilename, &data_length, true));
  ASSERT_NE(nullptr, tmp);
  EXPECT_EQ(2, data_length);
  EXPECT_EQ(0, tmp[0]);
  EXPECT_EQ(0x08, tmp[1]);
  CPLFree(tmp);
}

TEST(VSIMemTest, HandleOperationsSeekReadWriteTruncatePRead) {
  WithQuietHandler quiet;
  constexpr char kFile[] = "/vsimem/handle_ops.bin";
  absl::Cleanup cleanup = [&] { VSIUnlink(kFile); };

  // Open with "w+" (read & update allowed)
  VSILFILE* fp = VSIFOpenL(kFile, "w+b");
  ASSERT_NE(nullptr, fp);
  EXPECT_EQ(0, VSIFFlushL(fp));
  EXPECT_EQ(0, VSIFEofL(fp));
  EXPECT_EQ(0, VSIFErrorL(fp));
  VSIFClearErrL(fp);

  // Write 0 bytes
  EXPECT_EQ(0u, VSIFWriteL("abc", 1, 0, fp));
  // Write 4 bytes
  EXPECT_EQ(1u, VSIFWriteL("abcd", 4, 1, fp));
  EXPECT_EQ(4u, VSIFTellL(fp));

  // Seek with invalid whence fails
  EXPECT_EQ(-1, VSIFSeekL(fp, 0, 999));
  // Seek relative and SEEK_END
  EXPECT_EQ(0, VSIFSeekL(fp, 0, SEEK_SET));
  EXPECT_EQ(0, VSIFSeekL(fp, 2, SEEK_CUR));
  EXPECT_EQ(2u, VSIFTellL(fp));
  EXPECT_EQ(0, VSIFSeekL(fp, 0, SEEK_END));
  EXPECT_EQ(4u, VSIFTellL(fp));

  // Seek beyond EOF and then write -> extends file and zero-fills gap
  EXPECT_EQ(0, VSIFSeekL(fp, 8, SEEK_SET));
  EXPECT_EQ(1u, VSIFWriteL("xy", 2, 1, fp));
  EXPECT_EQ(10u, VSIFTellL(fp));

  // Read at EOF returns 0 and sets EOF flag
  char buf[16] = {};
  EXPECT_EQ(0u, VSIFReadL(buf, 1, 4, fp));
  EXPECT_NE(0, VSIFEofL(fp));

  // Read 0 bytes
  EXPECT_EQ(0, VSIFSeekL(fp, 0, SEEK_SET));
  EXPECT_EQ(0u, VSIFReadL(buf, 0, 4, fp));

  // Read past EOF truncates count and sets EOF
  EXPECT_EQ(0, VSIFSeekL(fp, 8, SEEK_SET));
  EXPECT_EQ(2u, VSIFReadL(buf, 1, 10, fp));
  EXPECT_NE(0, VSIFEofL(fp));

  // Test PRead via VSIVirtualHandle (VSILFILE is a typedef of it).
  VSIVirtualHandle* vh = fp;
  EXPECT_TRUE(vh->HasPRead());
  EXPECT_EQ(4u, vh->PRead(buf, 4, 0));
  EXPECT_EQ(2u, vh->PRead(buf, 10, 8));
  EXPECT_EQ(0u, vh->PRead(buf, 10, 100));

  // Truncate smaller and larger
  EXPECT_EQ(0, VSIFTruncateL(fp, 2));
  EXPECT_EQ(0, VSIFTruncateL(fp, 6));
  EXPECT_EQ(0, VSIFCloseL(fp));

  // Open write-only ("wb"): Read should fail with EBADF
  VSILFILE* fp_w = VSIFOpenL(kFile, "wb");
  ASSERT_NE(nullptr, fp_w);
  EXPECT_EQ(1u, VSIFWriteL("1234", 4, 1, fp_w));
  EXPECT_EQ(0u, VSIFReadL(buf, 1, 2, fp_w));
  EXPECT_EQ(0, VSIFCloseL(fp_w));

  // Open read-only ("rb"): Write and Truncate should fail
  VSILFILE* fp_r = VSIFOpenL(kFile, "rb");
  ASSERT_NE(nullptr, fp_r);
  EXPECT_EQ(0u, VSIFWriteL("12", 2, 1, fp_r));
  EXPECT_EQ(-1, VSIFTruncateL(fp_r, 1));
  EXPECT_EQ(0, VSIFCloseL(fp_r));

  // Open "r+b" and "ab" / "a+b"
  VSILFILE* fp_rp = VSIFOpenL(kFile, "r+b");
  ASSERT_NE(nullptr, fp_rp);
  EXPECT_EQ(0, VSIFCloseL(fp_rp));

  VSILFILE* fp_ap = VSIFOpenL(kFile, "a+b");
  ASSERT_NE(nullptr, fp_ap);
  EXPECT_EQ(4u, VSIFTellL(fp_ap));
  EXPECT_EQ(1u, VSIFWriteL("56", 2, 1, fp_ap));
  EXPECT_EQ(0, VSIFCloseL(fp_ap));
}

TEST(VSIMemTest, NonOwnedBufferExtendFailuresAndAnonymousFile) {
  WithQuietHandler quiet;
  GByte raw_buf[4] = {1, 2, 3, 4};
  constexpr char kNoOwnFile[] = "/vsimem/no_own.bin";
  absl::Cleanup cleanup = [&] { VSIUnlink(kNoOwnFile); };

  VSILFILE* fp =
      VSIFileFromMemBuffer(kNoOwnFile, raw_buf, sizeof(raw_buf), FALSE);
  ASSERT_NE(nullptr, fp);
  // Extending non-owned buffer via write past end fails
  EXPECT_EQ(0, VSIFSeekL(fp, 2, SEEK_SET));
  EXPECT_EQ(0u, VSIFWriteL("abcd", 4, 1, fp));
  // Extending non-owned buffer via seek past end + write fails
  EXPECT_EQ(0, VSIFSeekL(fp, 10, SEEK_SET));
  EXPECT_EQ(0u, VSIFWriteL("a", 1, 1, fp));
  VSIFCloseL(fp);

  // Seizing non-owned buffer logs debug and returns pointer without freeing
  vsi_l_offset len = 0;
  EXPECT_EQ(raw_buf, VSIGetMemFileBuffer(kNoOwnFile, &len, TRUE));

  // Anonymous file (pszFilename == nullptr)
  VSILFILE* anon =
      VSIFileFromMemBuffer(nullptr, raw_buf, sizeof(raw_buf), FALSE);
  ASSERT_NE(nullptr, anon);
  VSIFCloseL(anon);

  // Illegal filename "/vsimem/"
  EXPECT_EQ(nullptr,
            VSIFileFromMemBuffer("/vsimem/", raw_buf, sizeof(raw_buf), FALSE));
  EXPECT_EQ(nullptr, VSIGetMemFileBuffer(nullptr, &len, FALSE));
  EXPECT_EQ(nullptr,
            VSIGetMemFileBuffer("/vsimem/does_not_exist", &len, FALSE));
}

TEST(VSIMemTest, DirectoriesStatRenameReadDirAndHiddenFiles) {
  WithQuietHandler quiet;
  const char* kBase = "/vsimem/dir_test";
  absl::Cleanup cleanup = [&] {
    VSIRmdirRecursive(kBase);
    VSIRmdirRecursive("/vsimem/.#!HIDDEN!#.");
  };

  // Root stat and illegal open on "/vsimem/"
  VSIStatBufL st;
  EXPECT_EQ(0, VSIStatL("/vsimem/", &st));
  EXPECT_EQ(nullptr, VSIFOpenL("/vsimem/", "rb"));
  EXPECT_EQ(nullptr, VSIFOpenL("/vsimem/nonexistent_xyz", "rb"));

  // Mkdir and duplicate Mkdir
  EXPECT_EQ(0, VSIMkdir(kBase, 0755));
  EXPECT_EQ(-1, VSIMkdir(kBase, 0755));
  EXPECT_EQ(0, VSIStatL(kBase, &st));

  // Opening a directory with "r", "w", "a", or invalid mode "z" fails
  EXPECT_EQ(nullptr, VSIFOpenL(kBase, "rb"));
  EXPECT_EQ(nullptr, VSIFOpenL(kBase, "wb"));
  EXPECT_EQ(nullptr, VSIFOpenL(kBase, "ab"));
  EXPECT_EQ(nullptr, VSIFOpenL("/vsimem/dir_test/f.txt", "z"));

  // Create a regular file, then try to create a child under it (parent is not
  // a directory).
  VSILFILE* f_reg = VSIFOpenL("/vsimem/dir_test/regfile", "ab");
  ASSERT_NE(nullptr, f_reg);
  VSIFCloseL(f_reg);
  EXPECT_EQ(nullptr, VSIFOpenL("/vsimem/dir_test/regfile/sub", "wb"));
  EXPECT_EQ(nullptr, VSIFOpenL("/vsimem/dir_test/regfile/sub", "ab"));
  GByte dummy = 0;
  EXPECT_EQ(nullptr, VSIFileFromMemBuffer("/vsimem/dir_test/regfile/sub",
                                          &dummy, 1, FALSE));

  // Create multiple files to exercise ReadDirEx array reallocation and
  // nMaxFiles.
  for (int i = 0; i < 5; ++i) {
    std::string p = std::string(kBase) + "/item" + std::to_string(i);
    VSILFILE* f = VSIFOpenL(p.c_str(), "wb");
    ASSERT_NE(nullptr, f);
    VSIFCloseL(f);
  }
  char** dir_all = VSIReadDirEx(kBase, 0);
  EXPECT_EQ(6, CSLCount(dir_all));
  CSLDestroy(dir_all);

  char** dir_lim = VSIReadDirEx(kBase, 2);
  EXPECT_EQ(3, CSLCount(dir_lim));
  CSLDestroy(dir_lim);

  // Rename tests: invalid target prefix, self-rename, missing source,
  // directory rename.
  EXPECT_EQ(-1, VSIRename("/vsimem/dir_test/item0", "/non_vsimem/item0"));
  EXPECT_EQ(0, VSIRename("/vsimem/dir_test/item0", "/vsimem/dir_test/item0"));
  EXPECT_EQ(-1, VSIRename("/vsimem/dir_test/missing", "/vsimem/dir_test/new"));

  EXPECT_EQ(0, VSIMkdir("/vsimem/dir_test/sub", 0755));
  EXPECT_EQ(0, VSIMkdir("/vsimem/dir_test/sub_other", 0755));
  VSILFILE* f_sub = VSIFOpenL("/vsimem/dir_test/sub/child.txt", "wb");
  ASSERT_NE(nullptr, f_sub);
  VSIFCloseL(f_sub);

  EXPECT_EQ(0,
            VSIRename("/vsimem/dir_test/sub", "/vsimem/dir_test/sub_renamed"));
  EXPECT_EQ(0, VSIStatL("/vsimem/dir_test/sub_renamed/child.txt", &st));
  EXPECT_EQ(0, VSIRmdir("/vsimem/dir_test/sub_other"));

  // GetDiskFreeSpace
  EXPECT_GT(VSIGetDiskFreeSpace("/vsimem/"), 0);

  // Hidden filenames and hidden directory Mkdir / ReadDirEx / RmdirRecursive
  EXPECT_EQ(0, VSIMkdir("/vsimem/.#!HIDDEN!#.", 0755));
  std::string h1 = VSIMemGenerateHiddenFilename("file1.bin");
  std::string h2 = VSIMemGenerateHiddenFilename(nullptr);
  std::string h3 = VSIMemGenerateHiddenFilename("sub/file3.bin");
  VSILFILE* fh1 = VSIFOpenL(h1.c_str(), "wb");
  ASSERT_NE(nullptr, fh1);
  VSIFCloseL(fh1);
  // Create a sibling in the same hidden directory as h1 to hit the duplicate
  // subdir check.
  std::string h1_sibling = h1 + "_sibling";
  VSILFILE* fh1s = VSIFOpenL(h1_sibling.c_str(), "wb");
  ASSERT_NE(nullptr, fh1s);
  VSIFCloseL(fh1s);
  VSILFILE* fh2 = VSIFOpenL(h2.c_str(), "wb");
  ASSERT_NE(nullptr, fh2);
  VSIFCloseL(fh2);
  VSILFILE* fh3 = VSIFOpenL(h3.c_str(), "wb");
  ASSERT_NE(nullptr, fh3);
  VSIFCloseL(fh3);

  char** hidden_all = VSIReadDirEx("/vsimem/.#!HIDDEN!#.", 0);
  EXPECT_GE(CSLCount(hidden_all), 3);
  CSLDestroy(hidden_all);

  char** hidden_lim = VSIReadDirEx("/vsimem/.#!HIDDEN!#.", 1);
  EXPECT_EQ(2, CSLCount(hidden_lim));
  CSLDestroy(hidden_lim);

  // RmdirRecursive on this test's directory removes it (and its children) but
  // leaves the hidden files alone. VSIRmdirRecursive("/vsimem") is
  // deliberately not used here: it would wipe the process-wide in-memory
  // filesystem for every other test in this binary.
  EXPECT_EQ(0, VSIRmdirRecursive(kBase));
  EXPECT_EQ(-1, VSIStatL(kBase, &st));
  EXPECT_EQ(-1, VSIStatL("/vsimem/dir_test/sub_renamed/child.txt", &st));
  EXPECT_EQ(0, VSIStatL(h1.c_str(), &st));
  EXPECT_EQ(0, VSIRmdirRecursive("/vsimem/.#!HIDDEN!#."));
  EXPECT_EQ(-1, VSIStatL(h1.c_str(), &st));
}

}  // namespace
}  // namespace autotest2
