// Tests for cpl_vsil_uploadonclose:
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_vsil_uploadonclose.cpp
//
// Copyright 2026 Google Inc. All Rights Reserved.
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

#include <cstring>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

class FailingVirtualHandle final : public VSIVirtualHandle {
 public:
  FailingVirtualHandle(bool fail_seek, bool fail_write)
      : fail_seek_(fail_seek), fail_write_(fail_write) {}

  int Seek(vsi_l_offset nOffset, int nWhence) override {
    return fail_seek_ ? -1 : 0;
  }
  vsi_l_offset Tell() override { return 10; }
  size_t Read(void* pBuffer, size_t nSize, size_t nCount) override { return 0; }
  size_t Write(const void* pBuffer, size_t nSize, size_t nCount) override {
    return fail_write_ ? 0 : nCount;
  }
  void ClearErr() override {}
  int Error() override { return 0; }
  int Eof() override { return 0; }
  int Close() override { return 0; }

 private:
  bool fail_seek_;
  bool fail_write_;
};

TEST(CplVsilUploadOnCloseTest, WriteReadAndUploadOnClose) {
  const std::string dst_file = "/vsimem/upload_on_close_dst.bin";
  const std::string tmp_file = "/vsimem/upload_on_close_tmp.bin";

  VSIVirtualHandleUniquePtr writable(VSIFOpenL(dst_file.c_str(), "wb"));
  ASSERT_NE(nullptr, writable);
  VSIVirtualHandleUniquePtr tmp(VSIFOpenL(tmp_file.c_str(), "w+b"));
  ASSERT_NE(nullptr, tmp);

  std::unique_ptr<VSIVirtualHandle> handle(VSICreateUploadOnCloseFile(
      std::move(writable), std::move(tmp), tmp_file));
  ASSERT_NE(nullptr, handle);

  const char payload[] = "hello upload on close";
  EXPECT_EQ(1u, handle->Write(payload, strlen(payload), 1));
  EXPECT_EQ(0, handle->Flush());
  EXPECT_EQ(strlen(payload), handle->Tell());
  EXPECT_EQ(0, handle->Seek(0, SEEK_SET));
  EXPECT_EQ(0u, handle->Tell());

  char buf[64] = {};
  EXPECT_EQ(1u, handle->Read(buf, strlen(payload), 1));
  EXPECT_STREQ(payload, buf);

  handle->ClearErr();
  EXPECT_EQ(0, handle->Error());
  EXPECT_EQ(0, handle->Eof());
  EXPECT_EQ(VSI_RANGE_STATUS_UNKNOWN, handle->GetRangeStatus(0, 5));
  EXPECT_EQ(0, handle->Truncate(5));
  EXPECT_EQ(0, handle->Close());
  // Second Close() returns -1 because m_fpTemp is already reset.
  EXPECT_EQ(-1, handle->Close());
  handle.reset();

  VSIStatBufL stat_buf;
  ASSERT_EQ(0, VSIStatL(dst_file.c_str(), &stat_buf));
  EXPECT_EQ(5, stat_buf.st_size);
  EXPECT_EQ(0, VSIUnlink(dst_file.c_str()));
}

TEST(CplVsilUploadOnCloseTest, FailurePathsAndUnlinkOnDestroy) {
  // 1. Seek failure in Close() and non-existent tmp file at creation time
  //    so m_osTmpFilename is retained and unlinked in destructor.
  const std::string late_tmp = "/vsimem/upload_on_close_late_tmp.bin";
  {
    VSIVirtualHandleUniquePtr writable(new FailingVirtualHandle(false, false));
    VSIVirtualHandleUniquePtr bad_tmp(new FailingVirtualHandle(true, false));
    std::unique_ptr<VSIVirtualHandle> handle(VSICreateUploadOnCloseFile(
        std::move(writable), std::move(bad_tmp), late_tmp));
    EXPECT_EQ(-1, handle->Close());
  }

  // 2. Write failure in Close() when copying chunks.
  const std::string tmp_file2 = "/vsimem/upload_on_close_tmp2.bin";
  {
    VSIVirtualHandleUniquePtr writable(new FailingVirtualHandle(false, true));
    VSIVirtualHandleUniquePtr tmp(VSIFOpenL(tmp_file2.c_str(), "w+b"));
    ASSERT_NE(nullptr, tmp);
    std::unique_ptr<VSIVirtualHandle> handle(VSICreateUploadOnCloseFile(
        std::move(writable), std::move(tmp), tmp_file2));
    EXPECT_EQ(1u, handle->Write("abc", 3, 1));
    EXPECT_EQ(-1, handle->Close());
  }
}

}  // namespace
}  // namespace autotest2
