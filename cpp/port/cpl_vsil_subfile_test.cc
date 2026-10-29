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
// Tests for GDAL's /vsisubfile/ virtual file system.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_vsil_subfile.cpp

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

// A custom filesystem handler to test underlying seek/read failures with
// /vsisubfile/.
class ControllableHandler : public VSIFilesystemHandler {
 public:
  class Handle : public VSIVirtualHandle {
   public:
    explicit Handle(ControllableHandler* parent) : parent_(parent) {}
    int Seek(vsi_l_offset nOffset, int nWhence) override {
      if (parent_->fail_seek_end_ && nWhence == SEEK_END) return -1;
      if (parent_->fail_seek_set_ && nWhence == SEEK_SET) return -1;
      if (nWhence == SEEK_SET) offset_ = nOffset;
      return 0;
    }
    vsi_l_offset Tell() override { return offset_; }
    size_t Read(void* /*pBuffer*/, size_t /*nSize*/,
                size_t /*nMemb*/) override {
      err_ = true;
      return 0;
    }
    size_t Write(const void* /*pBuffer*/, size_t /*nSize*/,
                 size_t /*nMemb*/) override {
      return 0;
    }
    void ClearErr() override { err_ = false; }
    int Eof() override { return 0; }
    int Error() override { return err_ ? 1 : 0; }
    int Close() override { return 0; }

    ControllableHandler* parent_;
    vsi_l_offset offset_ = 0;
    bool err_ = false;
  };

  VSIVirtualHandleUniquePtr Open(const char* /*pszFilename*/,
                                 const char* /*pszAccess*/, bool /*bSetError*/,
                                 CSLConstList /*papszOptions*/) override {
    return VSIVirtualHandleUniquePtr(new Handle(this));
  }

  int Stat(const char* /*pszFilename*/, VSIStatBufL* pStatBuf,
           int /*nFlags*/) override {
    memset(pStatBuf, 0, sizeof(*pStatBuf));
    return 0;
  }

  bool fail_seek_end_ = false;
  bool fail_seek_set_ = false;
};

TEST(CplVsilSubfileTest, DecomposePathAndHandlerErrors) {
  WithQuietHandler quiet;
  auto* handler = VSIFileManager::GetHandler("/vsisubfile/");
  ASSERT_NE(nullptr, handler);

  // Direct handler calls with non-/vsisubfile/ prefix.
  EXPECT_EQ(nullptr, handler->Open("/not_subfile/foo", "rb", false, nullptr));
  VSIStatBufL stat_buf;
  EXPECT_EQ(-1, handler->Stat("/not_subfile/foo", &stat_buf, 0));

  // Malformed /vsisubfile/ paths (missing comma, slash before comma).
  EXPECT_EQ(nullptr, VSIFOpenL("/vsisubfile/10_20/vsimem/foo", "rb"));
  EXPECT_EQ(nullptr, VSIFOpenL("/vsisubfile/10_20", "rb"));
  EXPECT_EQ(-1, VSIStatL("/vsisubfile/10_20/vsimem/foo", &stat_buf));
  EXPECT_EQ(-1, VSIStatL("/vsisubfile/10_20", &stat_buf));

  // Overflow in offset + size.
  std::string overflow_path =
      absl::StrCat("/vsisubfile/", std::numeric_limits<vsi_l_offset>::max() - 5,
                   "_10,", "/vsimem/foo");
  EXPECT_EQ(nullptr, VSIFOpenL(overflow_path.c_str(), "rb"));

  // Non-existent underlying file.
  EXPECT_EQ(nullptr,
            VSIFOpenL("/vsisubfile/0_10,/vsimem/does_not_exist.bin", "rb"));

  // Unsupported filesystem operations return -1 / nullptr with EACCES.
  EXPECT_EQ(-1, VSIUnlink("/vsisubfile/0_10,/vsimem/foo"));
  EXPECT_EQ(EACCES, errno);
  EXPECT_EQ(-1, VSIMkdir("/vsisubfile/0_10,/vsimem/dir", 0755));
  EXPECT_EQ(EACCES, errno);
  EXPECT_EQ(-1, VSIRmdir("/vsisubfile/0_10,/vsimem/dir"));
  EXPECT_EQ(EACCES, errno);
  EXPECT_EQ(nullptr, VSIReadDirEx("/vsisubfile/0_10,/vsimem/dir", 0));
  EXPECT_EQ(EACCES, errno);
}

TEST(CplVsilSubfileTest, ReadWriteSeekTellAndStatWithBoundedSubregion) {
  WithQuietHandler quiet;
  const std::string mem_file = "/vsimem/subfile_bounded.bin";
  VsiMemTempWrapper wrapper(mem_file, "0123456789abcdefghij");

  // Subregion at offset 5, size 10 ("56789abcde").
  const std::string subfile = absl::StrCat("/vsisubfile/5_10,", mem_file);

  VSIStatBufL stat_buf;
  ASSERT_EQ(0, VSIStatL(subfile.c_str(), &stat_buf));
  EXPECT_EQ(10, stat_buf.st_size);

  // Also test Open in read-only mode when offset + size > file size (clamps
  // size) and when offset > file size (fails).
  const std::string clamped_subfile =
      absl::StrCat("/vsisubfile/15_20,", mem_file);
  VSILFILE* fp_clamped = VSIFOpenL(clamped_subfile.c_str(), "rb");
  ASSERT_NE(nullptr, fp_clamped);
  EXPECT_EQ(0, VSIFSeekL(fp_clamped, 0, SEEK_END));
  EXPECT_EQ(5, VSIFTellL(fp_clamped));
  EXPECT_EQ(0, VSIFCloseL(fp_clamped));

  const std::string past_end_subfile =
      absl::StrCat("/vsisubfile/25_5,", mem_file);
  EXPECT_EQ(nullptr, VSIFOpenL(past_end_subfile.c_str(), "rb"));
  ASSERT_EQ(0, VSIStatL(past_end_subfile.c_str(), &stat_buf));
  EXPECT_EQ(5, stat_buf.st_size);

  // Open with "w" mode (internally converted to "r+" to update in place).
  VSILFILE* fp = VSIFOpenL(subfile.c_str(), "w");
  ASSERT_NE(nullptr, fp);

  EXPECT_EQ(0, VSIFTellL(fp));
  EXPECT_EQ(0, VSIFEofL(fp));
  EXPECT_EQ(0, VSIFErrorL(fp));

  // Read 0 bytes.
  char buf[32] = {};
  EXPECT_EQ(0, VSIFReadL(buf, 0, 5, fp));

  // Read 4 bytes within subregion.
  EXPECT_EQ(4, VSIFReadL(buf, 1, 4, fp));
  EXPECT_EQ(4, VSIFTellL(fp));
  EXPECT_EQ(0, memcmp(buf, "5678", 4));

  // Read past the end of the subregion (requests 10, only 6 remain).
  memset(buf, 0, sizeof(buf));
  EXPECT_EQ(6, VSIFReadL(buf, 1, 10, fp));
  EXPECT_EQ(10, VSIFTellL(fp));
  EXPECT_EQ(0, memcmp(buf, "9abcde", 6));

  // Reading again at the end of the subregion sets EOF and returns 0.
  EXPECT_EQ(0, VSIFReadL(buf, 1, 1, fp));
  EXPECT_NE(0, VSIFEofL(fp));
  VSIFClearErrL(fp);
  EXPECT_EQ(0, VSIFEofL(fp));
  EXPECT_EQ(0, VSIFErrorL(fp));

  // Seek tests: SEEK_SET, SEEK_CUR, SEEK_END, overflow, invalid whence.
  EXPECT_EQ(0, VSIFSeekL(fp, 2, SEEK_SET));
  EXPECT_EQ(2, VSIFTellL(fp));
  EXPECT_EQ(0, VSIFSeekL(fp, 3, SEEK_CUR));
  EXPECT_EQ(5, VSIFTellL(fp));
  EXPECT_EQ(0, VSIFSeekL(fp, 0, SEEK_END));
  EXPECT_EQ(10, VSIFTellL(fp));
  EXPECT_EQ(-1,
            VSIFSeekL(fp, std::numeric_limits<vsi_l_offset>::max(), SEEK_SET));
  EXPECT_EQ(-1, fp->Seek(0, 9999));

  // Write tests within and across subregion boundary.
  EXPECT_EQ(0, VSIFSeekL(fp, 8, SEEK_SET));
  EXPECT_EQ(0, VSIFWriteL("XY", 0, 2, fp));
  // Request writing 5 bytes at offset 8 of size-10 subregion; only 2 written.
  EXPECT_EQ(2, VSIFWriteL("VWXYZ", 1, 5, fp));
  EXPECT_EQ(10, VSIFTellL(fp));
  // Writing at end of subregion returns 0.
  EXPECT_EQ(0, VSIFWriteL("Z", 1, 1, fp));

  // Write completely within subregion.
  EXPECT_EQ(0, VSIFSeekL(fp, 0, SEEK_SET));
  EXPECT_EQ(3, VSIFWriteL("ABC", 1, 3, fp));

  // If underlying handle is sought before nSubregionOffset, Tell() returns 0.
  EXPECT_EQ(0, VSIFSeekL(fp, 0, SEEK_SET));
  EXPECT_EQ(0, fp->Seek(static_cast<vsi_l_offset>(-2), SEEK_CUR));
  EXPECT_EQ(0, VSIFTellL(fp));

  EXPECT_EQ(0, fp->Close());
  // Second Close() on already-closed VSISubFileHandle returns -1.
  EXPECT_EQ(-1, fp->Close());
  delete fp;
}

TEST(CplVsilSubfileTest, UnboundedSubregionAndUnderlyingErrors) {
  WithQuietHandler quiet;
  const std::string mem_file = "/vsimem/subfile_unbounded.bin";
  VsiMemTempWrapper wrapper(mem_file, "0123456789abcdefghij");

  // Size -1 (or omitted) means size 0 (unbounded up to underlying file end).
  const std::string subfile_neg1 = absl::StrCat("/vsisubfile/5_-1,", mem_file);
  const std::string subfile_no_size = absl::StrCat("/vsisubfile/5,", mem_file);
  const std::string subfile_past_end =
      absl::StrCat("/vsisubfile/50,", mem_file);

  VSIStatBufL stat_buf;
  ASSERT_EQ(0, VSIStatL(subfile_neg1.c_str(), &stat_buf));
  EXPECT_EQ(15, stat_buf.st_size);
  ASSERT_EQ(0, VSIStatL(subfile_past_end.c_str(), &stat_buf));
  EXPECT_EQ(0, stat_buf.st_size);

  VSILFILE* fp = VSIFOpenL(subfile_no_size.c_str(), "r+");
  ASSERT_NE(nullptr, fp);

  char buf[32] = {};
  // Read past underlying EOF sets bAtEOF.
  EXPECT_EQ(15, VSIFReadL(buf, 1, 25, fp));
  EXPECT_NE(0, VSIFEofL(fp));
  VSIFClearErrL(fp);

  // Unbounded write.
  EXPECT_EQ(0, VSIFSeekL(fp, 0, SEEK_END));
  EXPECT_EQ(3, VSIFWriteL("XYZ", 1, 3, fp));
  EXPECT_EQ(0, VSIFCloseL(fp));

  // Install controllable handler to test underlying seek/read failures.
  auto* ctrl_handler = new ControllableHandler();
  VSIFileManager::InstallHandler("/vsictrl_subfile/", ctrl_handler);

  ctrl_handler->fail_seek_end_ = true;
  EXPECT_EQ(nullptr, VSIFOpenL("/vsisubfile/5_10,/vsictrl_subfile/a", "rb"));
  ctrl_handler->fail_seek_end_ = false;

  ctrl_handler->fail_seek_set_ = true;
  EXPECT_EQ(nullptr, VSIFOpenL("/vsisubfile/5_10,/vsictrl_subfile/a", "r+"));
  ctrl_handler->fail_seek_set_ = false;

  VSILFILE* fp_err = VSIFOpenL("/vsisubfile/0_10,/vsictrl_subfile/a", "r+");
  ASSERT_NE(nullptr, fp_err);
  EXPECT_EQ(0, VSIFReadL(buf, 1, 4, fp_err));
  EXPECT_NE(0, VSIFErrorL(fp_err));
  EXPECT_EQ(0, VSIFCloseL(fp_err));
}

}  // namespace
}  // namespace autotest2
