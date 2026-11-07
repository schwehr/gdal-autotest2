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
// Tests for skipping Huffman compression I/O routines (hdf/src/cskphuff.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/cskphuff.c
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/test/comp.c

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

#ifdef SUCCEED
#undef SUCCEED
#endif
#ifdef FAIL
#undef FAIL
#endif

#include "third_party/hdf4/hdf/src/hcomp_priv.h"
#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/hfile_priv.h"
// clang-format off
#include "third_party/hdf4/hdf/src/cskphuff_priv.h"
// clang-format on
#include "third_party/hdf4/hdf/src/hcomp.h"
#include "third_party/hdf4/hdf/src/herr_priv.h"

namespace hdf4 {
namespace {

class CskphuffTest : public ::testing::Test {
 protected:
  void SetUp() override { HEclear(); }

  void TearDown() override { HEclear(); }

  std::string GetTempFilePath(const std::string& name) {
    return (std::filesystem::path(::testing::TempDir()) / name).string();
  }
};

TEST_F(CskphuffTest, FuncListPointersMatch) {
  EXPECT_EQ(cskphuff_funcs.stread, HCPcskphuff_stread);
  EXPECT_EQ(cskphuff_funcs.stwrite, HCPcskphuff_stwrite);
  EXPECT_EQ(cskphuff_funcs.seek, HCPcskphuff_seek);
  EXPECT_EQ(cskphuff_funcs.inquire, HCPcskphuff_inquire);
  EXPECT_EQ(cskphuff_funcs.read, HCPcskphuff_read);
  EXPECT_EQ(cskphuff_funcs.write, HCPcskphuff_write);
  EXPECT_EQ(cskphuff_funcs.endaccess, HCPcskphuff_endaccess);
}

TEST_F(CskphuffTest, InquireAlwaysSucceeds) {
  accrec_t access_rec;
  std::memset(&access_rec, 0, sizeof(access_rec));
  int32 file_id = 0;
  uint16 tag = 0, ref = 0;
  int32 length = 0, offset = 0, posn = 0;
  int16 access = 0, special = 0;
  EXPECT_EQ(SUCCEED,
            HCPcskphuff_inquire(&access_rec, &file_id, &tag, &ref, &length,
                                &offset, &posn, &access, &special));
}

// 1. Single-byte skipping Huffman roundtrip (skp_size = 1).
TEST_F(CskphuffTest, RoundtripSingleByteSkip) {
  std::string path = GetTempFilePath("cskphuff_single.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  c_info.skphuff.skp_size = 1;
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info,
                       COMP_CODE_SKPHUFF, &c_info);
  ASSERT_NE(FAIL, aid);

  const int count = 2500;
  std::vector<uint8_t> data(count);
  for (int i = 0; i < count; ++i) {
    data[i] = static_cast<uint8_t>((i * 7 + 13) % 256);
  }

  EXPECT_EQ(count,
            Hwrite(aid, count, reinterpret_cast<const char*>(data.data())));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Re-open and verify read back
  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);
  aid = Hstartread(fid, tag, ref);
  ASSERT_NE(FAIL, aid);

  std::vector<uint8_t> read_data(count, 0);
  EXPECT_EQ(count,
            Hread(aid, count, reinterpret_cast<char*>(read_data.data())));

  for (int i = 0; i < count; ++i) {
    EXPECT_EQ(data[i], read_data[i]) << "Mismatch at index " << i;
  }

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));
}

// 2. Multi-byte skipping Huffman roundtrip with skp_size = 3 (RGB raster
// pattern).
TEST_F(CskphuffTest, RoundtripMultiByteSkipRgb) {
  std::string path = GetTempFilePath("cskphuff_rgb.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  c_info.skphuff.skp_size = 3;
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info,
                       COMP_CODE_SKPHUFF, &c_info);
  ASSERT_NE(FAIL, aid);

  const int count = 3000;
  std::vector<uint8_t> data(count);
  for (int i = 0; i < count; ++i) {
    // Distinct patterns for R, G, B channels
    int channel = i % 3;
    data[i] = static_cast<uint8_t>((channel * 80 + i) % 256);
  }

  EXPECT_EQ(count,
            Hwrite(aid, count, reinterpret_cast<const char*>(data.data())));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);
  aid = Hstartread(fid, tag, ref);
  ASSERT_NE(FAIL, aid);

  std::vector<uint8_t> read_data(count, 0);
  EXPECT_EQ(count,
            Hread(aid, count, reinterpret_cast<char*>(read_data.data())));

  for (int i = 0; i < count; ++i) {
    EXPECT_EQ(data[i], read_data[i]) << "Mismatch at index " << i;
  }

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));
}

// 3. Multi-byte skipping Huffman roundtrip with skp_size = 4 (32-bit pattern).
TEST_F(CskphuffTest, RoundtripMultiByteSkip4) {
  std::string path = GetTempFilePath("cskphuff_skip4.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  c_info.skphuff.skp_size = 4;
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info,
                       COMP_CODE_SKPHUFF, &c_info);
  ASSERT_NE(FAIL, aid);

  const int count = 4000;
  std::vector<uint8_t> data(count);
  for (int i = 0; i < count; ++i) {
    data[i] = static_cast<uint8_t>((i * 11) % 256);
  }

  EXPECT_EQ(count,
            Hwrite(aid, count, reinterpret_cast<const char*>(data.data())));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);
  aid = Hstartread(fid, tag, ref);
  ASSERT_NE(FAIL, aid);

  std::vector<uint8_t> read_data(count, 0);
  EXPECT_EQ(count,
            Hread(aid, count, reinterpret_cast<char*>(read_data.data())));

  for (int i = 0; i < count; ++i) {
    EXPECT_EQ(data[i], read_data[i]) << "Mismatch at index " << i;
  }

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));
}

// 4. Seek forward across multiple TMP_BUF_SIZE (8192 bytes) buffers,
// and seek backward (offset < skphuff_info->offset) calling
// HCIcskphuff_init(..., FALSE).
TEST_F(CskphuffTest, SeekForwardAndBackwardAcrossBuffers) {
  std::string path = GetTempFilePath("cskphuff_seek.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  c_info.skphuff.skp_size = 1;
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info,
                       COMP_CODE_SKPHUFF, &c_info);
  ASSERT_NE(FAIL, aid);

  // Write 24,000 bytes (> 2 * TMP_BUF_SIZE)
  const int count = 24000;
  std::vector<uint8_t> data(count);
  for (int i = 0; i < count; ++i) {
    data[i] = static_cast<uint8_t>((i % 251) + 1);
  }

  EXPECT_EQ(count,
            Hwrite(aid, count, reinterpret_cast<const char*>(data.data())));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);
  aid = Hstartread(fid, tag, ref);
  ASSERT_NE(FAIL, aid);

  // Seek forward to offset 18,000 (past 2 * TMP_BUF_SIZE)
  EXPECT_EQ(SUCCEED, Hseek(aid, 18000, DF_START));
  uint8_t buf[10];
  EXPECT_EQ(10, Hread(aid, 10, reinterpret_cast<char*>(buf)));
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(data[18000 + i], buf[i])
        << "Mismatch at forward seek offset " << 18000 + i;
  }

  // Seek backward to offset 500 (triggers HCIcskphuff_init with alloc_buf ==
  // FALSE)
  EXPECT_EQ(SUCCEED, Hseek(aid, 500, DF_START));
  EXPECT_EQ(10, Hread(aid, 10, reinterpret_cast<char*>(buf)));
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(data[500 + i], buf[i])
        << "Mismatch at backward seek offset " << 500 + i;
  }

  // Seek forward again to offset 1000
  EXPECT_EQ(SUCCEED, Hseek(aid, 1000, DF_START));
  EXPECT_EQ(10, Hread(aid, 10, reinterpret_cast<char*>(buf)));
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(data[1000 + i], buf[i])
        << "Mismatch at second forward seek offset " << 1000 + i;
  }

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));
}

// 5. Test Hstartwrite on existing compressed element.
TEST_F(CskphuffTest, StartWriteExisting) {
  std::string path = GetTempFilePath("cskphuff_startwrite.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  c_info.skphuff.skp_size = 1;
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info,
                       COMP_CODE_SKPHUFF, &c_info);
  ASSERT_NE(FAIL, aid);

  const int count = 50;
  std::vector<uint8_t> orig(count, 111);
  EXPECT_EQ(count,
            Hwrite(aid, count, reinterpret_cast<const char*>(orig.data())));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Open with DFACC_WRITE and use Hstartwrite
  fid = Hopen(path.c_str(), DFACC_WRITE, 0);
  ASSERT_NE(FAIL, fid);

  aid = Hstartwrite(fid, tag, ref, 0);
  ASSERT_NE(FAIL, aid);

  std::vector<uint8_t> replacement(count, 222);
  EXPECT_EQ(count, Hwrite(aid, count,
                          reinterpret_cast<const char*>(replacement.data())));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Verify replacement was written
  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);
  aid = Hstartread(fid, tag, ref);
  ASSERT_NE(FAIL, aid);

  std::vector<uint8_t> read_buf(count, 0);
  EXPECT_EQ(count, Hread(aid, count, reinterpret_cast<char*>(read_buf.data())));
  for (int i = 0; i < count; ++i) {
    EXPECT_EQ(222, read_buf[i]);
  }

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));
}

// 6. Test unsupported random write in the middle of dataset (lines 592-593).
TEST_F(CskphuffTest, UnsupportedRandomWriteInMiddle) {
  compinfo_t cinfo;
  std::memset(&cinfo, 0, sizeof(cinfo));
  cinfo.length = 100;
  cinfo.cinfo.coder_info.skphuff_info.offset = 10;

  accrec_t access_rec;
  std::memset(&access_rec, 0, sizeof(access_rec));
  access_rec.access = DFACC_WRITE;
  access_rec.special_info = reinterpret_cast<uint8*>(&cinfo);

  uint8_t data[10] = {1, 2, 3};
  HEclear();
  EXPECT_EQ(FAIL, HCPcskphuff_write(&access_rec, 10, data));
  EXPECT_EQ(DFE_UNSUPPORTED, HEvalue(1));
}

// 7. Direct function error handling
TEST_F(CskphuffTest, DirectFunctionErrorHandling) {
  compinfo_t cinfo;
  std::memset(&cinfo, 0, sizeof(cinfo));
  cinfo.aid = FAIL;
  cinfo.comp_ref = 9999;
  cinfo.cinfo.coder_info.skphuff_info.skip_size = 0;

  accrec_t access_rec;
  std::memset(&access_rec, 0, sizeof(access_rec));
  access_rec.file_id = FAIL;
  access_rec.access = DFACC_WRITE;
  access_rec.special_info = reinterpret_cast<uint8*>(&cinfo);

  // HCPcskphuff_stread with invalid file_id fails
  HEclear();
  EXPECT_EQ(FAIL, HCPcskphuff_stread(&access_rec));
  EXPECT_EQ(DFE_CINIT, HEvalue(1));

  // HCPcskphuff_stwrite with invalid file_id fails
  HEclear();
  EXPECT_EQ(FAIL, HCPcskphuff_stwrite(&access_rec));
  EXPECT_EQ(DFE_CINIT, HEvalue(1));

  // HCPcskphuff_read with invalid aid fails
  HEclear();
  uint8_t buf[10] = {};
  EXPECT_EQ(FAIL, HCPcskphuff_read(&access_rec, 4, buf));
  EXPECT_EQ(DFE_CDECODE, HEvalue(1));

  // HCPcskphuff_write with initialized tree but invalid aid fails on Hbitwrite
  HEclear();
  cinfo.cinfo.coder_info.skphuff_info.skip_size = 1;
  unsigned* left[1];
  unsigned* right[1];
  uint8_t* up[1];
  unsigned left_arr[SUCCMAX];
  unsigned right_arr[SUCCMAX];
  uint8_t up_arr[TWICEMAX];
  left[0] = left_arr;
  right[0] = right_arr;
  up[0] = up_arr;
  for (int i = 0; i < TWICEMAX; ++i) {
    up_arr[i] = static_cast<uint8_t>(i >> 1);
  }
  for (int j = 0; j < SUCCMAX; ++j) {
    left_arr[j] = static_cast<unsigned>(j << 1);
    right_arr[j] = static_cast<unsigned>((j << 1) + 1);
  }
  cinfo.cinfo.coder_info.skphuff_info.left = left;
  cinfo.cinfo.coder_info.skphuff_info.right = right;
  cinfo.cinfo.coder_info.skphuff_info.up = up;
  cinfo.aid = FAIL;  // causes Hbitwrite to fail

  EXPECT_EQ(FAIL, HCPcskphuff_write(&access_rec, 4, buf));
  EXPECT_EQ(DFE_CENCODE, HEvalue(1));

  // Reset pointers so endaccess won't free stack pointers
  cinfo.cinfo.coder_info.skphuff_info.left = nullptr;
  cinfo.cinfo.coder_info.skphuff_info.right = nullptr;
  cinfo.cinfo.coder_info.skphuff_info.up = nullptr;
  cinfo.cinfo.coder_info.skphuff_info.skip_size = 0;

  // HCPcskphuff_endaccess with invalid aid fails
  HEclear();
  EXPECT_EQ(FAIL, HCPcskphuff_endaccess(&access_rec));
  EXPECT_EQ(DFE_CANTCLOSE, HEvalue(1));

  // HCPcskphuff_seek backward with invalid aid fails
  HEclear();
  cinfo.cinfo.coder_info.skphuff_info.offset = 100;
  EXPECT_EQ(FAIL, HCPcskphuff_seek(&access_rec, 10, DF_START));
  EXPECT_EQ(DFE_CINIT, HEvalue(1));
}

}  // namespace
}  // namespace hdf4
