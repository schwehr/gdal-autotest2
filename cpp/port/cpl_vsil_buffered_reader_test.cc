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
// Tests for GDAL's buffered reader virtual file handle.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_vsil_buffered_reader.cpp

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace {

// A customizable underlying VSIVirtualHandle to test seek failures, short
// reads, EOF, and read errors in VSIBufferedReaderHandle.
class FakeBaseHandle : public VSIVirtualHandle {
 public:
  explicit FakeBaseHandle(std::string data) : data_(std::move(data)) {}

  int Seek(vsi_l_offset nOffset, int nWhence) override {
    if (fail_seek_) {
      return -1;
    }
    eof_ = false;
    if (nWhence == SEEK_SET) {
      offset_ = nOffset;
    } else if (nWhence == SEEK_CUR) {
      offset_ += nOffset;
    } else if (nWhence == SEEK_END) {
      offset_ = data_.size() + nOffset;
    }
    return 0;
  }

  vsi_l_offset Tell() override { return offset_; }

  size_t Read(void* pBuffer, size_t nSize, size_t nMemb) override {
    size_t to_read = nSize * nMemb;
    if (to_read == 0) return 0;
    if (fail_read_error_) {
      err_ = true;
      return 0;
    }
    if (offset_ >= data_.size()) {
      eof_ = true;
      return 0;
    }
    size_t avail = static_cast<size_t>(data_.size() - offset_);
    size_t n = std::min(to_read, avail);
    memcpy(pBuffer, data_.data() + offset_, n);
    offset_ += n;
    if (n < to_read) {
      eof_ = true;
    }
    return n / nSize;
  }

  size_t Write(const void* /*pBuffer*/, size_t /*nSize*/,
               size_t /*nMemb*/) override {
    return 0;
  }

  void ClearErr() override {
    eof_ = false;
    err_ = false;
  }

  int Eof() override { return eof_ ? 1 : 0; }
  int Error() override { return err_ ? 1 : 0; }
  int Flush() override { return 0; }
  int Close() override { return 0; }

  std::string data_;
  vsi_l_offset offset_ = 0;
  bool fail_seek_ = false;
  bool fail_read_error_ = false;
  bool eof_ = false;
  bool err_ = false;
};

TEST(CplVsilBufferedReaderTest, BasicReadAndBackwardSeekWithinBuffer) {
  std::string content(20000, '\0');
  for (size_t i = 0; i < content.size(); ++i) {
    content[i] = static_cast<char>(i % 251);
  }

  auto* base = new FakeBaseHandle(content);
  std::unique_ptr<VSIVirtualHandle> reader(VSICreateBufferedReaderHandle(base));
  ASSERT_NE(nullptr, reader);

  EXPECT_EQ(0, reader->Tell());
  EXPECT_EQ(0, reader->Eof());
  EXPECT_EQ(0, reader->Error());
  EXPECT_EQ(0, reader->Flush());

  // Write is unsupported.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(0, reader->Write("abc", 1, 3));
  }

  // Zero-size read returns 0.
  char buf[1024];
  EXPECT_EQ(0, reader->Read(buf, 0, 10));

  // Initial read outside buffer (populates buffer [0..100)).
  EXPECT_EQ(100, reader->Read(buf, 1, 100));
  EXPECT_EQ(100, reader->Tell());
  EXPECT_EQ(0, memcmp(buf, content.data(), 100));

  // Seek backward within buffer using SEEK_SET and read completely from buffer.
  EXPECT_EQ(0, reader->Seek(20, SEEK_SET));
  EXPECT_EQ(20, reader->Tell());
  EXPECT_EQ(30, reader->Read(buf, 1, 30));
  EXPECT_EQ(50, reader->Tell());
  EXPECT_EQ(0, memcmp(buf, content.data() + 20, 30));

  // Seek using SEEK_CUR and read overlapping buffer and underlying file.
  EXPECT_EQ(0, reader->Seek(10, SEEK_CUR));
  EXPECT_EQ(60, reader->Tell());
  // Buffer currently holds [0..100): reading 100 bytes consumes the remaining
  // 40 bytes from the buffer and the other 60 from the base handle (with
  // bNeedBaseHandleSeek still false).  The bNeedBaseHandleSeek == true path is
  // exercised below via SEEK_END.
  EXPECT_EQ(100, reader->Read(buf, 1, 100));
  EXPECT_EQ(160, reader->Tell());
  EXPECT_EQ(0, memcmp(buf, content.data() + 60, 100));

  // Seek to SEEK_END sets bNeedBaseHandleSeek = true.
  EXPECT_EQ(0, reader->Seek(0, SEEK_END));
  EXPECT_EQ(content.size(), reader->Tell());

  // Now seek back into the current buffer [60..160) and read across the end of
  // the buffer so that bNeedBaseHandleSeek is true when reading from file!
  EXPECT_EQ(0, reader->Seek(100, SEEK_SET));
  EXPECT_EQ(100, reader->Read(buf, 1, 100));
  EXPECT_EQ(200, reader->Tell());
  EXPECT_EQ(0, memcmp(buf, content.data() + 100, 100));

  // Read past EOF starting outside buffer.
  EXPECT_EQ(0, reader->Seek(content.size() - 10, SEEK_SET));
  EXPECT_EQ(10, reader->Read(buf, 1, 50));
  EXPECT_NE(0, reader->Eof());
  EXPECT_EQ(0, reader->Error());

  reader->ClearErr();
  EXPECT_EQ(0, reader->Eof());
  EXPECT_EQ(0, reader->Error());

  // Read past EOF starting inside buffer.
  // Currently buffer holds [content.size() - 10 .. content.size()).
  EXPECT_EQ(0, reader->Seek(content.size() - 5, SEEK_SET));
  EXPECT_EQ(5, reader->Read(buf, 1, 20));
  EXPECT_NE(0, reader->Eof());

  EXPECT_EQ(0, reader->Close());
  // Calling Close() a second time is safe.
  EXPECT_EQ(0, reader->Close());
}

TEST(CplVsilBufferedReaderTest, CheatFileSizeAndBeginningContentConstructor) {
  std::string content = "0123456789abcdefghijklmnopqrstuvwxyz";
  auto* base = new FakeBaseHandle(content);
  // Advance base handle to 10 to simulate already-read beginning content.
  ASSERT_EQ(0, base->Seek(10, SEEK_SET));

  std::unique_ptr<VSIVirtualHandle> reader(VSICreateBufferedReaderHandle(
      base, reinterpret_cast<const GByte*>(content.data()),
      /*nCheatFileSizeIn=*/100));
  ASSERT_NE(nullptr, reader);

  // SEEK_END uses nCheatFileSize when non-zero.
  EXPECT_EQ(0, reader->Seek(0, SEEK_END));
  EXPECT_EQ(100, reader->Tell());

  // Seek back to 5 (inside beginning content [0..10)) and read 15 bytes
  // (5 from buffer + 10 from base handle, with bNeedBaseHandleSeek == true).
  EXPECT_EQ(0, reader->Seek(5, SEEK_SET));
  char buf[32] = {};
  EXPECT_EQ(15, reader->Read(buf, 1, 15));
  EXPECT_EQ(20, reader->Tell());
  EXPECT_EQ(0, memcmp(buf, content.data() + 5, 15));
}

TEST(CplVsilBufferedReaderTest, SeekBaseToEmulationAndFailurePaths) {
  // Create content larger than 8192 bytes so SeekBaseTo loops over >1 chunk.
  std::string content(12000, 'A');
  for (size_t i = 0; i < content.size(); ++i) {
    content[i] = static_cast<char>('A' + (i % 26));
  }

  auto* base = new FakeBaseHandle(content);
  base->fail_seek_ = true;  // Force SeekBaseTo to emulate forward seek by read.
  std::unique_ptr<VSIVirtualHandle> reader(VSICreateBufferedReaderHandle(base));

  // Forward seek to 9000 (> 8192) and read.
  EXPECT_EQ(0, reader->Seek(9000, SEEK_SET));
  char buf[100] = {};
  EXPECT_EQ(50, reader->Read(buf, 1, 50));
  EXPECT_EQ(9050, reader->Tell());
  EXPECT_EQ(0, memcmp(buf, content.data() + 9000, 50));

  // Attempting to seek backward before base->Tell() (which is 9050) when
  // fail_seek_ is true must fail in SeekBaseTo (nCurOffset > nTargetOffset).
  EXPECT_EQ(0, reader->Seek(100, SEEK_SET));
  EXPECT_EQ(0, reader->Read(buf, 1, 10));

  // Seeking forward past actual EOF when fail_seek_ is true hits short read in
  // SeekBaseTo and sets EOF.
  EXPECT_EQ(0, reader->Seek(15000, SEEK_SET));
  EXPECT_EQ(0, reader->Read(buf, 1, 10));
  EXPECT_NE(0, reader->Eof());
}

TEST(CplVsilBufferedReaderTest, ReadErrorAndSeekBaseToFailureFromBuffer) {
  std::string content = "0123456789abcdefghijklmnopqrstuvwxyz";
  auto* base = new FakeBaseHandle(content);
  // Advance base to 10 so beginning content is [0..10).
  ASSERT_EQ(0, base->Seek(10, SEEK_SET));

  std::unique_ptr<VSIVirtualHandle> reader(VSICreateBufferedReaderHandle(
      base, reinterpret_cast<const GByte*>(content.data()), 0));

  // Make base handle at offset 20 and fail_seek_ = true so that
  // SeekBaseTo(10) fails when reading across buffer boundary [5..15).
  base->offset_ = 20;
  base->fail_seek_ = true;

  EXPECT_EQ(0, reader->Seek(5, SEEK_SET));
  char buf[32] = {};
  // Should return only the 5 bytes available in the buffer.
  EXPECT_EQ(5, reader->Read(buf, 1, 15));
  EXPECT_EQ(25, reader->Tell());
  EXPECT_EQ(0, memcmp(buf, "56789", 5));

  // Now allow seek, but trigger read error on base handle during cross-buffer
  // read.
  base->fail_seek_ = false;
  base->fail_read_error_ = true;
  EXPECT_EQ(0, reader->Seek(5, SEEK_SET));
  EXPECT_EQ(5, reader->Read(buf, 1, 15));
  EXPECT_NE(0, reader->Error());
  reader->ClearErr();
  EXPECT_EQ(0, reader->Error());

  // Also trigger read error on non-buffer read.
  EXPECT_EQ(0, reader->Seek(25, SEEK_SET));
  EXPECT_EQ(0, reader->Read(buf, 1, 5));
  EXPECT_NE(0, reader->Error());
}

}  // namespace
