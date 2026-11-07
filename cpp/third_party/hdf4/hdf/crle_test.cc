#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>  // NOLINT
#include <numeric>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"

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
#include "third_party/hdf4/hdf/src/crle_priv.h"
// clang-format on
#include "third_party/hdf4/hdf/src/hcomp.h"
#include "third_party/hdf4/hdf/src/herr_priv.h"

namespace {

class CrleTest : public ::testing::Test {
 protected:
  void SetUp() override { HEclear(); }

  void TearDown() override { HEclear(); }

  std::string GetTempFilePath(const std::string& name) {
    return (std::filesystem::path(::testing::TempDir()) / name).string();
  }
};

TEST_F(CrleTest, FuncListPointersMatch) {
  EXPECT_EQ(crle_funcs.stread, HCPcrle_stread);
  EXPECT_EQ(crle_funcs.stwrite, HCPcrle_stwrite);
  EXPECT_EQ(crle_funcs.seek, HCPcrle_seek);
  EXPECT_EQ(crle_funcs.inquire, HCPcrle_inquire);
  EXPECT_EQ(crle_funcs.read, HCPcrle_read);
  EXPECT_EQ(crle_funcs.write, HCPcrle_write);
  EXPECT_EQ(crle_funcs.endaccess, HCPcrle_endaccess);
}

TEST_F(CrleTest, InquireAlwaysSucceeds) {
  accrec_t access_rec = {};
  int32 file_id = 0;
  uint16 tag = 0, ref = 0;
  int32 length = 0, offset = 0, posn = 0;
  int16 access = 0, special = 0;
  EXPECT_EQ(SUCCEED, HCPcrle_inquire(&access_rec, &file_id, &tag, &ref, &length,
                                     &offset, &posn, &access, &special));
}

// Test basic roundtrip with runs, mixed data, and various lengths.
TEST_F(CrleTest, RoundtripRunsAndMix) {
  std::string path = GetTempFilePath("crle_roundtrip.hdf");
  std::filesystem::remove(path);

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info, COMP_CODE_RLE,
                       &c_info);
  ASSERT_NE(FAIL, aid);

  // Construct test data that exercises:
  // 1. Initial MIX transition.
  // 2. Run detection after MIX with mixed prefix: "ABC" + "DDDD".
  // 3. Long run >= RLE_MAX_RUN (130 identical bytes) to trigger max run buffer
  // flush.
  // 4. End of run returning to MIX: "DDDD" followed by "E".
  // 5. Long MIX >= RLE_BUF_SIZE (128 bytes) without 3 consecutive identical
  // bytes to trigger mix buffer flush.
  // 6. Terminate while in RLE_RUN.
  std::vector<uint8_t> payload;

  // Prefix mix with 3 different bytes: 'A', 'B', 'C'
  payload.push_back('A');
  payload.push_back('B');
  payload.push_back('C');

  // Short run: 5 'D's
  for (int i = 0; i < 5; ++i) {
    payload.push_back('D');
  }

  // End of run transition to mix: 'E', 'F'
  payload.push_back('E');
  payload.push_back('F');

  // Long run of 150 'G's (> RLE_MAX_RUN = 130)
  for (int i = 0; i < 150; ++i) {
    payload.push_back('G');
  }

  // Run end back to mix: 'H'
  payload.push_back('H');

  // Long mix of 150 bytes without runs: 0, 1, 2, ..., 149 (mod 251)
  for (int i = 0; i < 150; ++i) {
    payload.push_back(static_cast<uint8_t>((i * 7 + 1) % 251));
  }

  // Final run to test termination in RLE_RUN state: 10 'Z's
  for (int i = 0; i < 10; ++i) {
    payload.push_back('Z');
  }

  int32 to_write = static_cast<int32>(payload.size());
  int32 written = Hwrite(aid, to_write, payload.data());
  EXPECT_EQ(to_write, written);

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Re-open and verify reading in chunks and full content
  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);

  aid = Hstartread(fid, tag, ref);
  ASSERT_NE(FAIL, aid);

  std::vector<uint8_t> read_buf(payload.size(), 0);

  // Read first 10 bytes
  EXPECT_EQ(10, Hread(aid, 10, read_buf.data()));

  // Read next 50 bytes
  EXPECT_EQ(50, Hread(aid, 50, read_buf.data() + 10));

  // Read remaining bytes
  int32 remaining = to_write - 60;
  EXPECT_EQ(remaining, Hread(aid, remaining, read_buf.data() + 60));

  EXPECT_EQ(payload, read_buf);

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  std::filesystem::remove(path);
}

// Test termination in RLE_MIX state and immediate run at start (buf_length ==
// RLE_MIN_RUN - 1).
TEST_F(CrleTest, TermInMixAndImmediateRun) {
  std::string path = GetTempFilePath("crle_term_mix.hdf");
  std::filesystem::remove(path);

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info, COMP_CODE_RLE,
                       &c_info);
  ASSERT_NE(FAIL, aid);

  // Start immediately with a run: "AAAA"
  // Byte 0: 'A' -> INIT -> MIX (buf_length=1)
  // Byte 1: 'A' -> MIX (buf_length=2, equals RLE_MIN_RUN - 1)
  // Byte 2: 'A' -> 3 identical bytes with buf_length not > RLE_MIN_RUN - 1!
  // Byte 3: 'A' -> RUN continuation
  // Then "XYZ" to finish in RLE_MIX state when closed.
  std::vector<uint8_t> data = {'A', 'A', 'A', 'A', 'X', 'Y', 'Z'};

  EXPECT_EQ(static_cast<int32>(data.size()),
            Hwrite(aid, static_cast<int32>(data.size()), data.data()));

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Re-open and verify
  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);

  aid = Hstartread(fid, tag, ref);
  ASSERT_NE(FAIL, aid);

  std::vector<uint8_t> read_data(data.size(), 0);
  EXPECT_EQ(static_cast<int32>(data.size()),
            Hread(aid, static_cast<int32>(data.size()), read_data.data()));
  EXPECT_EQ(data, read_data);

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  std::filesystem::remove(path);
}

// Test seek forward across large buffer (> TMP_BUF_SIZE = 8192) and seek
// backward.
TEST_F(CrleTest, SeekLargeBufferForwardAndBackward) {
  std::string path = GetTempFilePath("crle_seek_large.hdf");
  std::filesystem::remove(path);

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info, COMP_CODE_RLE,
                       &c_info);
  ASSERT_NE(FAIL, aid);

  // Generate 20,000 bytes with repeating patterns
  const int32 total_bytes = 20000;
  std::vector<uint8_t> data(total_bytes);
  for (int32 i = 0; i < total_bytes; ++i) {
    data[i] = static_cast<uint8_t>((i % 23) + (i / 100 % 5));
  }

  EXPECT_EQ(total_bytes, Hwrite(aid, total_bytes, data.data()));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Re-open for reading
  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);

  aid = Hstartread(fid, tag, ref);
  ASSERT_NE(FAIL, aid);

  // Seek forward beyond TMP_BUF_SIZE (e.g. offset 17,500)
  // This exercises while (offset + TMP_BUF_SIZE < offset) loop twice!
  EXPECT_EQ(SUCCEED, Hseek(aid, 17500, DF_START));
  uint8_t byte_val = 0;
  EXPECT_EQ(1, Hread(aid, 1, &byte_val));
  EXPECT_EQ(data[17500], byte_val);

  // Seek backward to 5,000 (exercises offset < rle_info->offset, reinit, and
  // seek forward)
  EXPECT_EQ(SUCCEED, Hseek(aid, 5000, DF_START));
  EXPECT_EQ(1, Hread(aid, 1, &byte_val));
  EXPECT_EQ(data[5000], byte_val);

  // Seek to 0
  EXPECT_EQ(SUCCEED, Hseek(aid, 0, DF_START));
  EXPECT_EQ(1, Hread(aid, 1, &byte_val));
  EXPECT_EQ(data[0], byte_val);

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  std::filesystem::remove(path);
}

// Test seek while writing (offset < rle_info->offset triggers HCIcrle_term and
// HCIcrle_init).
TEST_F(CrleTest, SeekBackwardWhileWriting) {
  std::string path = GetTempFilePath("crle_seek_write.hdf");
  std::filesystem::remove(path);

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info, COMP_CODE_RLE,
                       &c_info);
  ASSERT_NE(FAIL, aid);

  const std::string text = "Initial data that will be rewritten";
  EXPECT_EQ(static_cast<int32>(text.size()),
            Hwrite(aid, static_cast<int32>(text.size()), text.data()));

  // Seeking backward to 0 while in write mode flushes the RLE buffer and
  // reinits RLE state
  EXPECT_EQ(SUCCEED, Hseek(aid, 0, DF_START));

  // Rewrite dataset completely from 0
  const std::string new_text = "Completely rewritten new data string!";
  EXPECT_EQ(static_cast<int32>(new_text.size()),
            Hwrite(aid, static_cast<int32>(new_text.size()), new_text.data()));

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Verify
  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);

  aid = Hstartread(fid, tag, ref);
  ASSERT_NE(FAIL, aid);

  std::vector<char> buf(new_text.size() + 1, 0);
  EXPECT_EQ(static_cast<int32>(new_text.size()),
            Hread(aid, static_cast<int32>(new_text.size()), buf.data()));
  EXPECT_STREQ(new_text.c_str(), buf.data());

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  std::filesystem::remove(path);
}

// Test Hstartwrite path on existing compressed element
TEST_F(CrleTest, StartWriteExisting) {
  std::string path = GetTempFilePath("crle_startwrite.hdf");
  std::filesystem::remove(path);

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info, COMP_CODE_RLE,
                       &c_info);
  ASSERT_NE(FAIL, aid);

  const std::string text = "Original text to be updated";
  EXPECT_EQ(static_cast<int32>(text.size()),
            Hwrite(aid, static_cast<int32>(text.size()), text.data()));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Open with DFACC_WRITE and use Hstartwrite
  fid = Hopen(path.c_str(), DFACC_WRITE, 0);
  ASSERT_NE(FAIL, fid);

  aid = Hstartwrite(fid, tag, ref, static_cast<int32>(text.size()));
  ASSERT_NE(FAIL, aid);

  const std::string updated = "Replacement data replacing original";
  EXPECT_EQ(static_cast<int32>(updated.size()),
            Hwrite(aid, static_cast<int32>(updated.size()), updated.data()));

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  std::filesystem::remove(path);
}

// Test random write restriction:
// HCPcrle_write rejects writes in the middle unless offset == 0 or append at
// the end.
TEST_F(CrleTest, RandomWriteUnsupported) {
  compinfo_t cinfo = {};
  cinfo.length = 100;
  cinfo.cinfo.coder_info.rle_info.offset = 10;

  accrec_t access_rec = {};
  access_rec.access = DFACC_WRITE;
  access_rec.special_info = reinterpret_cast<uint8*>(&cinfo);

  uint8_t dummy[10] = {};
  // Writing 20 bytes at offset 10 where length is 100: offset != 0 && offset !=
  // length && length <= 90
  EXPECT_EQ(FAIL, HCPcrle_write(&access_rec, 20, dummy));
  EXPECT_EQ(DFE_UNSUPPORTED, HEvalue(1));
}

// Test error handling paths for HCPcrle_* functions
TEST_F(CrleTest, DirectFunctionErrorHandling) {
  compinfo_t cinfo;
  std::memset(&cinfo, 0, sizeof(cinfo));
  cinfo.aid = FAIL;
  cinfo.comp_ref = 9999;
  cinfo.length = 0;

  accrec_t access_rec;
  std::memset(&access_rec, 0, sizeof(access_rec));
  access_rec.file_id = FAIL;
  access_rec.special_info = reinterpret_cast<uint8*>(&cinfo);

  // HCPcrle_stread with invalid file_id fails
  EXPECT_EQ(FAIL, HCPcrle_stread(&access_rec));
  EXPECT_EQ(DFE_CINIT, HEvalue(1));

  // HCPcrle_stwrite with invalid file_id fails
  EXPECT_EQ(FAIL, HCPcrle_stwrite(&access_rec));
  EXPECT_EQ(DFE_CINIT, HEvalue(1));

  // HCPcrle_read with invalid aid fails
  uint8_t buf[10] = {};
  printf("&rle_info = %p\n", &(cinfo.cinfo.coder_info.rle_info));
  printf("&rle_state = %p, rle_state = %d\n",
         &(cinfo.cinfo.coder_info.rle_info.rle_state),
         (int)cinfo.cinfo.coder_info.rle_info.rle_state);
  EXPECT_EQ(FAIL, HCPcrle_read(&access_rec, 10, buf));
  EXPECT_EQ(DFE_CDECODE, HEvalue(1));

  // HCPcrle_write with invalid aid fails when flushed
  cinfo.cinfo.coder_info.rle_info.rle_state =
      decltype(cinfo.cinfo.coder_info.rle_info.rle_state)::RLE_INIT;
  // Write triggers encode
  cinfo.aid = FAIL;
  // If rle_state is invalid enum, encode hits default
  cinfo.cinfo.coder_info.rle_info.rle_state =
      static_cast<decltype(cinfo.cinfo.coder_info.rle_info.rle_state)>(99);
  EXPECT_EQ(FAIL, HCPcrle_write(&access_rec, 1, buf));
  EXPECT_EQ(DFE_CENCODE, HEvalue(1));

  // HCPcrle_seek backward with invalid rle_state in write mode hits term
  // default
  access_rec.access = DFACC_WRITE;
  cinfo.cinfo.coder_info.rle_info.offset = 50;
  cinfo.cinfo.coder_info.rle_info.rle_state =
      static_cast<decltype(cinfo.cinfo.coder_info.rle_info.rle_state)>(99);
  EXPECT_EQ(FAIL, HCPcrle_seek(&access_rec, 10, DF_START));
  EXPECT_EQ(DFE_CTERM, HEvalue(1));

  // HCPcrle_seek backward with valid rle_state but invalid aid fails during
  // init
  cinfo.cinfo.coder_info.rle_info.rle_state =
      decltype(cinfo.cinfo.coder_info.rle_info.rle_state)::RLE_INIT;
  EXPECT_EQ(FAIL, HCPcrle_seek(&access_rec, 10, DF_START));
  EXPECT_EQ(DFE_CINIT, HEvalue(1));

  // HCPcrle_seek forward past TMP_BUF_SIZE with invalid aid fails in while loop
  cinfo.aid = FAIL;
  cinfo.cinfo.coder_info.rle_info.offset = 0;
  cinfo.cinfo.coder_info.rle_info.rle_state =
      decltype(cinfo.cinfo.coder_info.rle_info.rle_state)::RLE_INIT;
  EXPECT_EQ(FAIL, HCPcrle_seek(&access_rec, 10000, DF_START));
  EXPECT_EQ(DFE_CDECODE, HEvalue(1));

  // HCPcrle_seek forward within TMP_BUF_SIZE with invalid aid fails in final
  // chunk
  cinfo.cinfo.coder_info.rle_info.offset = 0;
  EXPECT_EQ(FAIL, HCPcrle_seek(&access_rec, 100, DF_START));
  EXPECT_EQ(DFE_CDECODE, HEvalue(1));

  // HCPcrle_write: end-of-run write failure when next byte differs and aid is
  // invalid
  cinfo.aid = FAIL;
  cinfo.cinfo.coder_info.rle_info.rle_state =
      decltype(cinfo.cinfo.coder_info.rle_info.rle_state)::RLE_RUN;
  cinfo.cinfo.coder_info.rle_info.buf_length = 5;
  cinfo.cinfo.coder_info.rle_info.last_byte = 'A';
  cinfo.cinfo.coder_info.rle_info.offset = 0;
  cinfo.length = 0;
  uint8_t diff_byte = 'B';
  EXPECT_EQ(FAIL, HCPcrle_write(&access_rec, 1, &diff_byte));
  EXPECT_EQ(DFE_CENCODE, HEvalue(1));

  // HCPcrle_write: max mix buffer flush failure when aid is invalid
  cinfo.cinfo.coder_info.rle_info.rle_state =
      decltype(cinfo.cinfo.coder_info.rle_info.rle_state)::RLE_MIX;
  cinfo.cinfo.coder_info.rle_info.buf_length = RLE_BUF_SIZE - 1;
  cinfo.cinfo.coder_info.rle_info.buf_pos = RLE_BUF_SIZE - 1;
  cinfo.cinfo.coder_info.rle_info.last_byte = 'X';
  cinfo.cinfo.coder_info.rle_info.second_byte = 'Y';
  uint8_t mix_byte = 'Z';
  EXPECT_EQ(FAIL, HCPcrle_write(&access_rec, 1, &mix_byte));
  EXPECT_EQ(DFE_CENCODE, HEvalue(1));

  // HCPcrle_endaccess with write mode and rle_state in RLE_MIX but invalid aid
  // fails during term
  access_rec.access = DFACC_WRITE;
  cinfo.cinfo.coder_info.rle_info.rle_state =
      decltype(cinfo.cinfo.coder_info.rle_info.rle_state)::RLE_MIX;
  cinfo.cinfo.coder_info.rle_info.buf_length = 5;
  EXPECT_EQ(FAIL, HCPcrle_endaccess(&access_rec));
  EXPECT_EQ(DFE_CTERM, HEvalue(1));

  // HCPcrle_endaccess with write mode and rle_state in RLE_RUN but invalid aid
  // fails during term
  access_rec.access = DFACC_WRITE;
  cinfo.cinfo.coder_info.rle_info.rle_state =
      decltype(cinfo.cinfo.coder_info.rle_info.rle_state)::RLE_RUN;
  cinfo.cinfo.coder_info.rle_info.buf_length = 5;
  EXPECT_EQ(FAIL, HCPcrle_endaccess(&access_rec));
  EXPECT_EQ(DFE_CTERM, HEvalue(1));

  // HCPcrle_endaccess with rle_state in RLE_INIT but invalid aid fails
  // Hendaccess
  cinfo.cinfo.coder_info.rle_info.rle_state =
      decltype(cinfo.cinfo.coder_info.rle_info.rle_state)::RLE_INIT;
  EXPECT_EQ(FAIL, HCPcrle_endaccess(&access_rec));
  EXPECT_EQ(DFE_CANTCLOSE, HEvalue(1));
}

}  // namespace
