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
// Tests for Arc/Info vector coverage raw binary I/O (avc_rawbin.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/avc/avc_rawbin.cpp

#include <climits>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogrsf_frmts/avc/avc.h"
#include "third_party/gdal/ogr/ogrsf_frmts/avc/avc_mbyte.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

TEST(AvcRawBinTest, OpenAndCloseAccessModesAndErrors) {
  const std::string dir = "/vsimem/avc_rawbin_open_test";
  ASSERT_EQ(0, VSIMkdir(dir.c_str(), 0777));
  absl::Cleanup dir_cleaner = [&dir] { VSIRmdirRecursive(dir.c_str()); };

  // 1. Close null pointer is safe.
  AVCRawBinClose(nullptr);

  // 2. Open invalid access mode fails.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    AVCRawBinFile* file = AVCRawBinOpen((dir + "/invalid.bin").c_str(), "x",
                                        AVCBigEndian, nullptr);
    EXPECT_EQ(nullptr, file);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
  }

  // 3. Open non-existent file in read mode fails.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    AVCRawBinFile* file = AVCRawBinOpen((dir + "/nonexistent.bin").c_str(), "r",
                                        AVCBigEndian, nullptr);
    EXPECT_EQ(nullptr, file);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_OpenFailed, CPLGetLastErrorNo());
  }

  // 4. Open in write "w" mode.
  const std::string test_file = dir + "/test.bin";
  AVCRawBinFile* file_w =
      AVCRawBinOpen(test_file.c_str(), "w", AVCBigEndian, nullptr);
  ASSERT_NE(nullptr, file_w);
  EXPECT_EQ(AVCWrite, file_w->eAccess);
  AVCRawBinWriteInt32(file_w, 100);
  AVCRawBinClose(file_w);

  // 5. Open in append "a" mode.
  AVCRawBinFile* file_a =
      AVCRawBinOpen(test_file.c_str(), "a", AVCBigEndian, nullptr);
  ASSERT_NE(nullptr, file_a);
  EXPECT_EQ(AVCWrite, file_a->eAccess);
  AVCRawBinWriteInt32(file_a, 200);
  AVCRawBinClose(file_a);

  // 6. Open in read "r" mode.
  AVCRawBinFile* file_r =
      AVCRawBinOpen(test_file.c_str(), "r", AVCBigEndian, nullptr);
  ASSERT_NE(nullptr, file_r);
  EXPECT_EQ(AVCRead, file_r->eAccess);
  EXPECT_EQ(100, AVCRawBinReadInt32(file_r));
  EXPECT_EQ(200, AVCRawBinReadInt32(file_r));
  AVCRawBinClose(file_r);

  // 7. Open in read/write "r+" mode.
  AVCRawBinFile* file_rw =
      AVCRawBinOpen(test_file.c_str(), "r+", AVCBigEndian, nullptr);
  ASSERT_NE(nullptr, file_rw);
  EXPECT_EQ(AVCReadWrite, file_rw->eAccess);
  EXPECT_EQ(100, AVCRawBinReadInt32(file_rw));
  AVCRawBinClose(file_rw);
}

TEST(AvcRawBinTest, PrimitivesReadWriteBothEndiannesses) {
  const std::string dir = "/vsimem/avc_rawbin_prims_test";
  ASSERT_EQ(0, VSIMkdir(dir.c_str(), 0777));
  absl::Cleanup dir_cleaner = [&dir] { VSIRmdirRecursive(dir.c_str()); };

  const AVCByteOrder byte_orders[] = {AVCBigEndian, AVCLittleEndian};

  for (size_t i = 0; i < std::size(byte_orders); ++i) {
    const AVCByteOrder order = byte_orders[i];
    const std::string path = dir + "/data_" + std::to_string(i) + ".bin";

    // Setup DBCS info for string testing.
    AVCDBCSInfo* dbcs = AVCAllocDBCSInfo();
    ASSERT_NE(nullptr, dbcs);
    absl::Cleanup dbcs_cleaner = [dbcs] { AVCFreeDBCSInfo(dbcs); };
    dbcs->nDBCSCodePage = AVC_DBCS_JAPANESE;
    dbcs->nDBCSEncoding = AVC_CODE_JAP_SHIFTJIS;

    AVCRawBinFile* file_out = AVCRawBinOpen(path.c_str(), "w", order, dbcs);
    ASSERT_NE(nullptr, file_out);

    // Int16
    AVCRawBinWriteInt16(file_out, 0x1234);
    // Int32
    AVCRawBinWriteInt32(file_out, 0x12345678);
    // Float
    AVCRawBinWriteFloat(file_out, 12.34f);
    // Double
    AVCRawBinWriteDouble(file_out, 56.789012);
    // Zeros (13 bytes: 8 bytes chunk + 5 bytes chunk)
    AVCRawBinWriteZeros(file_out, 13);
    // PaddedString: short string (pad spaces), exact length, and Katakana DBCS
    AVCRawBinWritePaddedString(file_out, 8,
                               reinterpret_cast<const GByte*>("abc"));
    AVCRawBinWritePaddedString(file_out, 4,
                               reinterpret_cast<const GByte*>("1234"));
    // DBCS Katakana character 0xB1 gets prefixed with 0x8E in Arc coverage
    // format
    const GByte kana_src[] = {0xB1, 0x00};
    AVCRawBinWritePaddedString(file_out, 4, kana_src);
    // Raw bytes
    const GByte raw_data[] = {0xAA, 0xBB, 0xCC};
    AVCRawBinWriteBytes(file_out, sizeof(raw_data), raw_data);

    AVCRawBinClose(file_out);

    // Now read back and verify.
    AVCRawBinFile* file_in = AVCRawBinOpen(path.c_str(), "r", order, dbcs);
    ASSERT_NE(nullptr, file_in);

    EXPECT_EQ(0x1234, AVCRawBinReadInt16(file_in));
    EXPECT_EQ(0x12345678, AVCRawBinReadInt32(file_in));
    EXPECT_FLOAT_EQ(12.34f, AVCRawBinReadFloat(file_in));
    EXPECT_DOUBLE_EQ(56.789012, AVCRawBinReadDouble(file_in));

    GByte zero_buf[13];
    AVCRawBinReadBytes(file_in, 13, zero_buf);
    for (GByte b : zero_buf) {
      EXPECT_EQ(0, b);
    }

    GByte str_buf[16];
    std::memset(str_buf, 0, sizeof(str_buf));
    AVCRawBinReadString(file_in, 8, str_buf);
    EXPECT_STREQ("abc     ", reinterpret_cast<char*>(str_buf));

    std::memset(str_buf, 0, sizeof(str_buf));
    AVCRawBinReadString(file_in, 4, str_buf);
    EXPECT_STREQ("1234", reinterpret_cast<char*>(str_buf));

    // DBCS Katakana string read back (0x8E gets converted back to single
    // Katakana)
    std::memset(str_buf, 0, sizeof(str_buf));
    AVCRawBinReadString(file_in, 4, str_buf);
    EXPECT_EQ(0xB1, str_buf[0]);

    GByte raw_read[3] = {};
    AVCRawBinReadBytes(file_in, 3, raw_read);
    EXPECT_EQ(0xAA, raw_read[0]);
    EXPECT_EQ(0xBB, raw_read[1]);
    EXPECT_EQ(0xCC, raw_read[2]);

    AVCRawBinClose(file_in);
  }
}

TEST(AvcRawBinTest, ReadAcrossBufferBoundariesAndAccessErrors) {
  const std::string dir = "/vsimem/avc_rawbin_buffer_test";
  ASSERT_EQ(0, VSIMkdir(dir.c_str(), 0777));
  absl::Cleanup dir_cleaner = [&dir] { VSIRmdirRecursive(dir.c_str()); };

  const std::string path = dir + "/large.bin";
  constexpr int kTotalSize = 2500;
  std::vector<GByte> payload(kTotalSize);
  for (int i = 0; i < kTotalSize; ++i) {
    payload[i] = static_cast<GByte>(i % 251);
  }

  VsiMemTempWrapper wrapper(
      path,
      std::string(reinterpret_cast<char*>(payload.data()), payload.size()));

  AVCRawBinFile* file = AVCRawBinOpen(path.c_str(), "r", AVCBigEndian, nullptr);
  ASSERT_NE(nullptr, file);

  // 1. Initial read: loads buffer (1024 bytes).
  std::vector<GByte> read_buf(kTotalSize);
  AVCRawBinReadBytes(file, 100, read_buf.data());
  EXPECT_EQ(0, std::memcmp(read_buf.data(), payload.data(), 100));

  // 2. Fast-path read: within already loaded buffer (nCurPos + nBytesToRead <=
  // nCurSize).
  AVCRawBinReadBytes(file, 50, read_buf.data() + 100);
  EXPECT_EQ(0, std::memcmp(read_buf.data() + 100, payload.data() + 100, 50));

  // 3. Boundary crossing read: needs bytes from current buffer and subsequent
  // buffers.
  AVCRawBinReadBytes(file, 1500, read_buf.data() + 150);
  EXPECT_EQ(0, std::memcmp(read_buf.data() + 150, payload.data() + 150, 1500));

  // 4. Read remaining bytes.
  AVCRawBinReadBytes(file, kTotalSize - 1650, read_buf.data() + 1650);
  EXPECT_EQ(0, std::memcmp(read_buf.data() + 1650, payload.data() + 1650,
                           kTotalSize - 1650));

  // 5. Read past EOF generates CPLError.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GByte extra;
    AVCRawBinReadBytes(file, 1, &extra);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_FileIO, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("EOF encountered"));
  }

  AVCRawBinClose(file);

  // 6. Access mode errors.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GByte b = 0;
    AVCRawBinReadBytes(nullptr, 1, &b);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    AVCRawBinWriteBytes(nullptr, 1, &b);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  AVCRawBinFile* write_file = AVCRawBinOpen((dir + "/write_only.bin").c_str(),
                                            "w", AVCBigEndian, nullptr);
  ASSERT_NE(nullptr, write_file);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GByte b = 0;
    AVCRawBinReadBytes(write_file, 1, &b);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  AVCRawBinClose(write_file);

  AVCRawBinFile* read_file =
      AVCRawBinOpen(path.c_str(), "r", AVCBigEndian, nullptr);
  ASSERT_NE(nullptr, read_file);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GByte b = 0;
    AVCRawBinWriteBytes(read_file, 1, &b);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  AVCRawBinClose(read_file);
}

TEST(AvcRawBinTest, SeekEofAndFileDataSize) {
  const std::string dir = "/vsimem/avc_rawbin_seek_test";
  ASSERT_EQ(0, VSIMkdir(dir.c_str(), 0777));
  absl::Cleanup dir_cleaner = [&dir] { VSIRmdirRecursive(dir.c_str()); };

  const std::string path = dir + "/seek.bin";
  constexpr int kSize = 3000;
  std::vector<GByte> data(kSize);
  for (int i = 0; i < kSize; ++i) {
    data[i] = static_cast<GByte>(i & 0xFF);
  }
  VsiMemTempWrapper wrapper(
      path, std::string(reinterpret_cast<char*>(data.data()), data.size()));

  // 1. AVCRawBinSetFileDataSize with null is safe.
  AVCRawBinSetFileDataSize(nullptr, 100);

  AVCRawBinFile* file = AVCRawBinOpen(path.c_str(), "r", AVCBigEndian, nullptr);
  ASSERT_NE(nullptr, file);
  absl::Cleanup file_cleaner = [file] { AVCRawBinClose(file); };

  // 2. AVCRawBinIsFileGreaterThan.
  EXPECT_TRUE(AVCRawBinIsFileGreaterThan(file, 2000));
  EXPECT_TRUE(AVCRawBinIsFileGreaterThan(file, 3000));
  EXPECT_FALSE(AVCRawBinIsFileGreaterThan(file, 4000));

  // 3. AVCRawBinEOF on fresh read file.
  EXPECT_FALSE(AVCRawBinEOF(file));

  // 4. Read 500 bytes to populate buffer.
  GByte buf[1024];
  AVCRawBinReadBytes(file, 500, buf);

  // 5. Seek within buffer: SEEK_SET to 200.
  AVCRawBinFSeek(file, 200, SEEK_SET);
  EXPECT_EQ(200, file->nCurPos);
  AVCRawBinReadBytes(file, 1, buf);
  EXPECT_EQ(data[200], buf[0]);

  // 6. Seek within buffer: SEEK_CUR + 50.
  AVCRawBinFSeek(file, 50, SEEK_CUR);
  EXPECT_EQ(251, file->nCurPos);
  AVCRawBinReadBytes(file, 1, buf);
  EXPECT_EQ(data[251], buf[0]);

  // 7. Seek outside buffer: SEEK_SET to 2000.
  AVCRawBinFSeek(file, 2000, SEEK_SET);
  EXPECT_EQ(0, file->nCurPos);
  EXPECT_EQ(2000, file->nOffset);
  AVCRawBinReadBytes(file, 1, buf);
  EXPECT_EQ(data[2000], buf[0]);

  // 8. Seek within second buffer window: SEEK_CUR + 500.
  AVCRawBinFSeek(file, 500, SEEK_CUR);
  AVCRawBinReadBytes(file, 1, buf);
  EXPECT_EQ(data[2501], buf[0]);

  // 9. Seek overflow and negative guards.
  AVCRawBinFSeek(file, -10000, SEEK_CUR);
  // Position should not become negative; stays unchanged.
  EXPECT_EQ(2000, file->nOffset);
  EXPECT_EQ(502, file->nCurPos);

  // Seek overflow guard: nTargetBig > INT_MAX stays unchanged.
  AVCRawBinFSeek(file, INT_MAX, SEEK_CUR);
  EXPECT_EQ(2000, file->nOffset);
  EXPECT_EQ(502, file->nCurPos);

  // 10. FileDataSize truncation effect on EOF.
  AVCRawBinSetFileDataSize(file, 2600);
  AVCRawBinFSeek(file, 2599, SEEK_SET);
  EXPECT_FALSE(AVCRawBinEOF(file));
  AVCRawBinFSeek(file, 2600, SEEK_SET);
  EXPECT_TRUE(AVCRawBinEOF(file));

  // Reset file data size.
  AVCRawBinSetFileDataSize(file, -1);
  AVCRawBinFSeek(file, 3000, SEEK_SET);
  EXPECT_TRUE(AVCRawBinEOF(file));

  // 11. EOF on null and write handles.
  EXPECT_TRUE(AVCRawBinEOF(nullptr));
  AVCRawBinFile* write_file =
      AVCRawBinOpen((dir + "/w.bin").c_str(), "w", AVCBigEndian, nullptr);
  ASSERT_NE(nullptr, write_file);
  absl::Cleanup write_cleaner = [write_file] { AVCRawBinClose(write_file); };
  EXPECT_TRUE(AVCRawBinEOF(write_file));

  // In non-DEBUG builds, seeking on a write handle returns early without
  // changes. Note: in upstream GDAL with DEBUG defined, this triggers
  // CPLAssert.
#ifndef DEBUG
  AVCRawBinFSeek(write_file, 10, SEEK_SET);
  EXPECT_EQ(0, write_file->nCurPos);
#endif
}

}  // namespace
}  // namespace autotest2
