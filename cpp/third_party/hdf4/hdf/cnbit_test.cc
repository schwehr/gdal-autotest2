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
// Tests for N-bit encoding I/O routines (hdf/src/cnbit.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/cnbit.c
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/test/nbit.c

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
#include "third_party/hdf4/hdf/src/cnbit_priv.h"
// clang-format on
#include "third_party/hdf4/hdf/src/hcomp.h"
#include "third_party/hdf4/hdf/src/herr_priv.h"

namespace hdf4 {
namespace {

class CnbitTest : public ::testing::Test {
 protected:
  void SetUp() override { HEclear(); }

  void TearDown() override { HEclear(); }

  std::string GetTempFilePath(const std::string& name) {
    return (std::filesystem::path(::testing::TempDir()) / name).string();
  }
};

TEST_F(CnbitTest, FuncListPointersMatch) {
  EXPECT_EQ(cnbit_funcs.stread, HCPcnbit_stread);
  EXPECT_EQ(cnbit_funcs.stwrite, HCPcnbit_stwrite);
  EXPECT_EQ(cnbit_funcs.seek, HCPcnbit_seek);
  EXPECT_EQ(cnbit_funcs.inquire, HCPcnbit_inquire);
  EXPECT_EQ(cnbit_funcs.read, HCPcnbit_read);
  EXPECT_EQ(cnbit_funcs.write, HCPcnbit_write);
  EXPECT_EQ(cnbit_funcs.endaccess, HCPcnbit_endaccess);
}

TEST_F(CnbitTest, InquireAlwaysSucceeds) {
  accrec_t access_rec;
  std::memset(&access_rec, 0, sizeof(access_rec));
  int32 file_id = 0;
  uint16 tag = 0, ref = 0;
  int32 length = 0, offset = 0, posn = 0;
  int16 access = 0, special = 0;
  EXPECT_EQ(SUCCEED,
            HCPcnbit_inquire(&access_rec, &file_id, &tag, &ref, &length,
                             &offset, &posn, &access, &special));
}

// 1. Unsigned 8-bit data roundtrip (no sign extension, fill_one = FALSE).
// Spans across multiple NBIT_BUF_SIZE (512 bytes) buffers.
TEST_F(CnbitTest, RoundtripUint8SpanningMultipleBuffers) {
  std::string path = GetTempFilePath("cnbit_uint8.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  c_info.nbit.nt = DFNT_UINT8;
  c_info.nbit.sign_ext = FALSE;
  c_info.nbit.fill_one = FALSE;
  c_info.nbit.start_bit = 5;  // 6 bits (bits 0 to 5)
  c_info.nbit.bit_len = 6;
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info, COMP_CODE_NBIT,
                       &c_info);
  ASSERT_NE(FAIL, aid);

  const int count = 2000;  // > 512 bytes buffer
  std::vector<uint8_t> data(count);
  for (int i = 0; i < count; ++i) {
    data[i] = static_cast<uint8_t>(i * 3);
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
    uint8_t expected = static_cast<uint8_t>(data[i] & 0x3F);
    EXPECT_EQ(expected, read_data[i]) << "Mismatch at index " << i;
  }

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));
}

// 2. Signed 8-bit data roundtrip with sign_ext = TRUE, fill_one = FALSE.
// Tests sign extension when sign_bit == 1 (!= fill_one) filling ones,
// and when sign_bit == 0 (== fill_one).
TEST_F(CnbitTest, RoundtripInt8SignExtensionFillZero) {
  std::string path = GetTempFilePath("cnbit_int8_fill0.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  c_info.nbit.nt = DFNT_INT8;
  c_info.nbit.sign_ext = TRUE;
  c_info.nbit.fill_one = FALSE;
  c_info.nbit.start_bit = 5;  // 6 bits: values in [-32, 31]
  c_info.nbit.bit_len = 6;
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info, COMP_CODE_NBIT,
                       &c_info);
  ASSERT_NE(FAIL, aid);

  const int count = 600;
  std::vector<int8_t> data(count);
  for (int i = 0; i < count; ++i) {
    data[i] = static_cast<int8_t>(((i * 3) % 64) - 32);
  }

  EXPECT_EQ(count,
            Hwrite(aid, count, reinterpret_cast<const char*>(data.data())));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);
  aid = Hstartread(fid, tag, ref);
  ASSERT_NE(FAIL, aid);

  std::vector<int8_t> read_data(count, 0);
  EXPECT_EQ(count,
            Hread(aid, count, reinterpret_cast<char*>(read_data.data())));

  for (int i = 0; i < count; ++i) {
    EXPECT_EQ(data[i], read_data[i]) << "Mismatch at index " << i;
  }

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));
}

// 3. Signed 8-bit data roundtrip with sign_ext = TRUE, fill_one = TRUE.
// Tests sign extension when sign_bit == 0 (!= fill_one) filling zeroes,
// and when sign_bit == 1 (== fill_one).
TEST_F(CnbitTest, RoundtripInt8SignExtensionFillOne) {
  std::string path = GetTempFilePath("cnbit_int8_fill1.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  c_info.nbit.nt = DFNT_INT8;
  c_info.nbit.sign_ext = TRUE;
  c_info.nbit.fill_one = TRUE;
  c_info.nbit.start_bit = 5;  // 6 bits: values in [-32, 31]
  c_info.nbit.bit_len = 6;
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info, COMP_CODE_NBIT,
                       &c_info);
  ASSERT_NE(FAIL, aid);

  const int count = 600;
  std::vector<int8_t> data(count);
  for (int i = 0; i < count; ++i) {
    data[i] = static_cast<int8_t>(((i * 3) % 64) - 32);
  }

  EXPECT_EQ(count,
            Hwrite(aid, count, reinterpret_cast<const char*>(data.data())));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);
  aid = Hstartread(fid, tag, ref);
  ASSERT_NE(FAIL, aid);

  std::vector<int8_t> read_data(count, 0);
  EXPECT_EQ(count,
            Hread(aid, count, reinterpret_cast<char*>(read_data.data())));

  for (int i = 0; i < count; ++i) {
    EXPECT_EQ(data[i], read_data[i]) << "Mismatch at index " << i;
  }

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));
}

// 4. 16-bit integer roundtrip using DFKconvert (sign_ext = FALSE).
// Hits multi-byte encoding and decoding.
TEST_F(CnbitTest, RoundtripUint16WithDfkConvert) {
  std::string path = GetTempFilePath("cnbit_uint16.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  c_info.nbit.nt = DFNT_UINT16;
  c_info.nbit.sign_ext = FALSE;
  c_info.nbit.fill_one = FALSE;
  c_info.nbit.start_bit = 11;  // 12 bits
  c_info.nbit.bit_len = 12;
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info, COMP_CODE_NBIT,
                       &c_info);
  ASSERT_NE(FAIL, aid);

  const int count = 400;
  std::vector<uint16_t> orig_data(count);
  for (int i = 0; i < count; ++i) {
    orig_data[i] = static_cast<uint16_t>(i * 7);
  }

  int32 bytes = count * sizeof(uint16_t);
  std::vector<uint8_t> conv_buf(bytes);
  ASSERT_EQ(SUCCEED, DFKconvert(orig_data.data(), conv_buf.data(), DFNT_UINT16,
                                count, DFACC_WRITE, 0, 0));

  EXPECT_EQ(bytes,
            Hwrite(aid, bytes, reinterpret_cast<const char*>(conv_buf.data())));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);
  aid = Hstartread(fid, tag, ref);
  ASSERT_NE(FAIL, aid);

  std::vector<uint8_t> read_conv_buf(bytes, 0);
  EXPECT_EQ(bytes,
            Hread(aid, bytes, reinterpret_cast<char*>(read_conv_buf.data())));

  std::vector<uint16_t> read_data(count, 0);
  ASSERT_EQ(SUCCEED, DFKconvert(read_conv_buf.data(), read_data.data(),
                                DFNT_UINT16, count, DFACC_READ, 0, 0));

  for (int i = 0; i < count; ++i) {
    uint16_t expected = orig_data[i] & 0x0FFF;
    EXPECT_EQ(expected, read_data[i]) << "Mismatch at index " << i;
  }

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));
}

// 5. 16-bit integer with field inside a single byte (start_bit = 5, bit_len =
// 4). Hits lines 126-131 in cnbit.c: entire bit-field is inside a single byte.
TEST_F(CnbitTest, RoundtripUint16FieldInsideSingleByte) {
  std::string path = GetTempFilePath("cnbit_uint16_single_byte.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  c_info.nbit.nt = DFNT_UINT16;
  c_info.nbit.sign_ext = FALSE;
  c_info.nbit.fill_one = FALSE;
  c_info.nbit.start_bit = 5;  // inside lower byte (bits 2 to 5)
  c_info.nbit.bit_len = 4;
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info, COMP_CODE_NBIT,
                       &c_info);
  ASSERT_NE(FAIL, aid);

  const int count = 200;
  std::vector<uint16_t> orig_data(count);
  for (int i = 0; i < count; ++i) {
    orig_data[i] = static_cast<uint16_t>((i % 16) << 2);
  }

  int32 bytes = count * sizeof(uint16_t);
  std::vector<uint8_t> conv_buf(bytes);
  ASSERT_EQ(SUCCEED, DFKconvert(orig_data.data(), conv_buf.data(), DFNT_UINT16,
                                count, DFACC_WRITE, 0, 0));

  EXPECT_EQ(bytes,
            Hwrite(aid, bytes, reinterpret_cast<const char*>(conv_buf.data())));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);
  aid = Hstartread(fid, tag, ref);
  ASSERT_NE(FAIL, aid);

  std::vector<uint8_t> read_conv_buf(bytes, 0);
  EXPECT_EQ(bytes,
            Hread(aid, bytes, reinterpret_cast<char*>(read_conv_buf.data())));

  std::vector<uint16_t> read_data(count, 0);
  ASSERT_EQ(SUCCEED, DFKconvert(read_conv_buf.data(), read_data.data(),
                                DFNT_UINT16, count, DFACC_READ, 0, 0));

  for (int i = 0; i < count; ++i) {
    uint16_t expected = orig_data[i] & 0x003C;
    EXPECT_EQ(expected, read_data[i]) << "Mismatch at index " << i;
  }

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));
}

// 6. 32-bit signed integer roundtrip with sign extension.
// Hits multi-byte sign extension across sign_byte loops (lines 224-225 &
// 229-230).
TEST_F(CnbitTest, RoundtripInt32SignExtension) {
  std::string path = GetTempFilePath("cnbit_int32_signext.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  c_info.nbit.nt = DFNT_INT32;
  c_info.nbit.sign_ext = TRUE;
  c_info.nbit.fill_one = FALSE;
  c_info.nbit.start_bit = 27;  // within byte 3
  c_info.nbit.bit_len = 18;    // spans across byte 3, byte 2, byte 1
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info, COMP_CODE_NBIT,
                       &c_info);
  ASSERT_NE(FAIL, aid);

  const int count = 300;
  std::vector<int32_t> orig_data(count);
  for (int i = 0; i < count; ++i) {
    // 18-bit signed range is [-131072, 131071]
    int32_t val = ((i * 1000) % 262144) - 131072;
    orig_data[i] = val << 10;
  }

  int32 bytes = count * sizeof(int32_t);
  std::vector<uint8_t> conv_buf(bytes);
  ASSERT_EQ(SUCCEED, DFKconvert(orig_data.data(), conv_buf.data(), DFNT_INT32,
                                count, DFACC_WRITE, 0, 0));

  EXPECT_EQ(bytes,
            Hwrite(aid, bytes, reinterpret_cast<const char*>(conv_buf.data())));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);
  aid = Hstartread(fid, tag, ref);
  ASSERT_NE(FAIL, aid);

  std::vector<uint8_t> read_conv_buf(bytes, 0);
  EXPECT_EQ(bytes,
            Hread(aid, bytes, reinterpret_cast<char*>(read_conv_buf.data())));

  std::vector<int32_t> read_data(count, 0);
  ASSERT_EQ(SUCCEED, DFKconvert(read_conv_buf.data(), read_data.data(),
                                DFNT_INT32, count, DFACC_READ, 0, 0));

  for (int i = 0; i < count; ++i) {
    EXPECT_EQ(orig_data[i], read_data[i]) << "Mismatch at index " << i;
  }

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));
}

// 7. Test initialization mask cases:
// - mask_top >= top_bit and mask_bot > bot_bit (only top part of byte in mask,
// lines 112-117)
// - mask_top < top_bit and mask_top >= bot_bit and mask_bot < bot_bit (mask top
// to bottom of byte, lines 122-125)
TEST_F(CnbitTest, MaskBoundaryCases) {
  std::string path = GetTempFilePath("cnbit_mask_cases.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  // Case A: nt = DFNT_UINT8, start_bit = 7, bit_len = 4
  // top_bit = 7, bot_bit = 0. mask_top = 7 >= 7, mask_bot = 4 > 0.
  // Hits lines 112-117.
  model_info m_info = {};
  comp_info c_info = {};
  c_info.nbit.nt = DFNT_UINT8;
  c_info.nbit.sign_ext = FALSE;
  c_info.nbit.fill_one = TRUE;
  c_info.nbit.start_bit = 7;
  c_info.nbit.bit_len = 4;
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info, COMP_CODE_NBIT,
                       &c_info);
  ASSERT_NE(FAIL, aid);

  uint8_t val8 = 0xF0;
  EXPECT_EQ(1, Hwrite(aid, 1, reinterpret_cast<const char*>(&val8)));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));

  // Case B: nt = DFNT_UINT16, start_bit = 12, bit_len = 8.
  // Byte 0: top_bit = 15, bot_bit = 8. mask_top = 12 < 15, mask_top >= 8,
  // mask_bot = 5 < 8. Hits lines 122-125!
  uint16 ref2 = Hnewref(fid);
  ASSERT_NE(0, ref2);
  c_info.nbit.nt = DFNT_UINT16;
  c_info.nbit.sign_ext = FALSE;
  c_info.nbit.fill_one = FALSE;
  c_info.nbit.start_bit = 12;
  c_info.nbit.bit_len = 8;
  int32 aid2 = HCcreate(fid, tag, ref2, COMP_MODEL_STDIO, &m_info,
                        COMP_CODE_NBIT, &c_info);
  ASSERT_NE(FAIL, aid2);

  uint16_t val16 = 0x1FE0;
  uint8_t conv16[2];
  ASSERT_EQ(SUCCEED,
            DFKconvert(&val16, conv16, DFNT_UINT16, 1, DFACC_WRITE, 0, 0));
  EXPECT_EQ(2, Hwrite(aid2, 2, reinterpret_cast<const char*>(conv16)));
  EXPECT_EQ(SUCCEED, Hendaccess(aid2));

  EXPECT_EQ(SUCCEED, Hclose(fid));
}

// 8. Test seek: valid aligned seek and invalid unaligned seek
TEST_F(CnbitTest, SeekAlignedAndUnaligned) {
  std::string path = GetTempFilePath("cnbit_seek.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  c_info.nbit.nt = DFNT_UINT8;
  c_info.nbit.sign_ext = FALSE;
  c_info.nbit.fill_one = FALSE;
  c_info.nbit.start_bit = 7;
  c_info.nbit.bit_len = 8;
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info, COMP_CODE_NBIT,
                       &c_info);
  ASSERT_NE(FAIL, aid);

  const int count = 100;
  std::vector<uint8_t> data(count);
  for (int i = 0; i < count; ++i) {
    data[i] = static_cast<uint8_t>(i + 1);
  }
  EXPECT_EQ(count,
            Hwrite(aid, count, reinterpret_cast<const char*>(data.data())));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);
  aid = Hstartread(fid, tag, ref);
  ASSERT_NE(FAIL, aid);

  // Aligned seek to item 10 (offset 10)
  EXPECT_EQ(SUCCEED, Hseek(aid, 10, DF_START));
  uint8_t val = 0;
  EXPECT_EQ(1, Hread(aid, 1, reinterpret_cast<char*>(&val)));
  EXPECT_EQ(data[10], val);

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));
}

// 9. Test Hstartwrite on existing compressed element
TEST_F(CnbitTest, StartWriteExisting) {
  std::string path = GetTempFilePath("cnbit_startwrite.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  c_info.nbit.nt = DFNT_UINT8;
  c_info.nbit.sign_ext = FALSE;
  c_info.nbit.fill_one = FALSE;
  c_info.nbit.start_bit = 7;
  c_info.nbit.bit_len = 8;
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info, COMP_CODE_NBIT,
                       &c_info);
  ASSERT_NE(FAIL, aid);

  const int count = 20;
  std::vector<uint8_t> orig(count, 111);
  EXPECT_EQ(count,
            Hwrite(aid, count, reinterpret_cast<const char*>(orig.data())));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Open with DFACC_WRITE and use Hstartwrite
  fid = Hopen(path.c_str(), DFACC_WRITE, 0);
  ASSERT_NE(FAIL, fid);

  aid = Hstartwrite(fid, tag, ref, count);
  ASSERT_NE(FAIL, aid);

  std::vector<uint8_t> replacement(count, 222);
  EXPECT_EQ(count, Hwrite(aid, count,
                          reinterpret_cast<const char*>(replacement.data())));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));
}

// 10. Direct function error handling
TEST_F(CnbitTest, DirectFunctionErrorHandling) {
  compinfo_t cinfo;
  std::memset(&cinfo, 0, sizeof(cinfo));
  cinfo.aid = FAIL;
  cinfo.comp_ref = 9999;
  cinfo.cinfo.coder_info.nbit_info.nt_size = 4;
  cinfo.cinfo.coder_info.nbit_info.mask_len = 16;
  cinfo.cinfo.coder_info.nbit_info.buf_pos = NBIT_BUF_SIZE;  // force refill
  cinfo.cinfo.coder_info.nbit_info.sign_ext = FALSE;
  cinfo.cinfo.coder_info.nbit_info.mask_info[0].length = 8;
  cinfo.cinfo.coder_info.nbit_info.mask_info[0].mask = 0xFF;

  accrec_t access_rec;
  std::memset(&access_rec, 0, sizeof(access_rec));
  access_rec.file_id = FAIL;
  access_rec.access = DFACC_WRITE;
  access_rec.special_info = reinterpret_cast<uint8*>(&cinfo);

  // HCPcnbit_stread with invalid file_id fails
  HEclear();
  EXPECT_EQ(FAIL, HCPcnbit_stread(&access_rec));
  EXPECT_EQ(DFE_CINIT, HEvalue(1));

  // HCPcnbit_stwrite with invalid file_id fails
  HEclear();
  EXPECT_EQ(FAIL, HCPcnbit_stwrite(&access_rec));
  EXPECT_EQ(DFE_CINIT, HEvalue(1));

  // HCPcnbit_seek with unaligned offset fails
  HEclear();
  EXPECT_EQ(FAIL, HCPcnbit_seek(&access_rec, 3, DF_START));
  EXPECT_EQ(DFE_CSEEK, HEvalue(1));

  // HCPcnbit_seek with aligned offset but invalid aid fails
  HEclear();
  EXPECT_EQ(FAIL, HCPcnbit_seek(&access_rec, 4, DF_START));
  EXPECT_EQ(DFE_CSEEK, HEvalue(1));

  // HCPcnbit_read with invalid aid and forced buffer refill fails
  HEclear();
  uint8_t buf[10] = {};
  EXPECT_EQ(FAIL, HCPcnbit_read(&access_rec, 4, buf));
  EXPECT_EQ(DFE_CDECODE, HEvalue(1));

  // HCPcnbit_endaccess with read access and invalid aid fails
  HEclear();
  access_rec.access = DFACC_READ;
  EXPECT_EQ(FAIL, HCPcnbit_endaccess(&access_rec));
  EXPECT_EQ(DFE_CANTCLOSE, HEvalue(1));

  // HCPcnbit_endaccess with write access
  HEclear();
  access_rec.access = DFACC_WRITE;
  EXPECT_EQ(FAIL, HCPcnbit_endaccess(&access_rec));
  EXPECT_EQ(DFE_CANTCLOSE, HEvalue(1));
}

}  // namespace
}  // namespace hdf4
