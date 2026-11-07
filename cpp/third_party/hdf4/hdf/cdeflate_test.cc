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
#include "third_party/hdf4/hdf/src/cdeflate_priv.h"
// clang-format on
#include "third_party/hdf4/hdf/src/hcomp.h"
#include "third_party/hdf4/hdf/src/herr_priv.h"

namespace {

class CdeflateTest : public ::testing::Test {
 protected:
  void SetUp() override { HEclear(); }

  void TearDown() override { HEclear(); }

  std::string GetTempFilePath(const std::string& name) {
    return (std::filesystem::path(::testing::TempDir()) / name).string();
  }
};

TEST_F(CdeflateTest, FuncListPointersMatch) {
  EXPECT_EQ(cdeflate_funcs.stread, HCPcdeflate_stread);
  EXPECT_EQ(cdeflate_funcs.stwrite, HCPcdeflate_stwrite);
  EXPECT_EQ(cdeflate_funcs.seek, HCPcdeflate_seek);
  EXPECT_EQ(cdeflate_funcs.inquire, HCPcdeflate_inquire);
  EXPECT_EQ(cdeflate_funcs.read, HCPcdeflate_read);
  EXPECT_EQ(cdeflate_funcs.write, HCPcdeflate_write);
  EXPECT_EQ(cdeflate_funcs.endaccess, HCPcdeflate_endaccess);
}

TEST_F(CdeflateTest, InquireAlwaysSucceeds) {
  accrec_t access_rec;
  std::memset(&access_rec, 0, sizeof(access_rec));
  int32 file_id = 0;
  uint16 tag = 0, ref = 0;
  int32 length = 0, offset = 0, posn = 0;
  int16 access = 0, special = 0;
  EXPECT_EQ(SUCCEED,
            HCPcdeflate_inquire(&access_rec, &file_id, &tag, &ref, &length,
                                &offset, &posn, &access, &special));
}

// Test small and large roundtrips to exercise buffer flushes in encode and
// decode.
TEST_F(CdeflateTest, RoundtripLargeBuffer) {
  std::string path = GetTempFilePath("cdeflate_roundtrip.hdf");
  std::filesystem::remove(path);

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  c_info.deflate_setting.level = 6;
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info,
                       COMP_CODE_DEFLATE, &c_info);
  ASSERT_NE(FAIL, aid);

  // Generate 50,000 bytes with pattern to trigger multiple DEFLATE_BUF_SIZE
  // (4096) flushes
  const int32 total_bytes = 50000;
  std::vector<uint8_t> data(total_bytes);
  for (int32 i = 0; i < total_bytes; ++i) {
    data[i] = static_cast<uint8_t>((i % 251) ^ (i / 13));
  }

  // Write in multiple blocks
  int32 written = Hwrite(aid, 10000, data.data());
  EXPECT_EQ(10000, written);
  written = Hwrite(aid, total_bytes - 10000, data.data() + 10000);
  EXPECT_EQ(total_bytes - 10000, written);

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Re-open for reading
  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);

  aid = Hstartread(fid, tag, ref);
  ASSERT_NE(FAIL, aid);

  std::vector<uint8_t> read_buf(total_bytes, 0);
  // Read in smaller chunks first
  EXPECT_EQ(50, Hread(aid, 50, read_buf.data()));
  EXPECT_EQ(8192, Hread(aid, 8192, read_buf.data() + 50));
  int32 remaining = total_bytes - 50 - 8192;
  EXPECT_EQ(remaining, Hread(aid, remaining, read_buf.data() + 50 + 8192));

  EXPECT_EQ(data, read_buf);

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  std::filesystem::remove(path);
}

// Test seek forward across large buffer (> DEFLATE_TMP_BUF_SIZE = 16384),
// seek backward, and seek when acc_init is 0.
TEST_F(CdeflateTest, SeekForwardAndBackward) {
  std::string path = GetTempFilePath("cdeflate_seek.hdf");
  std::filesystem::remove(path);

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  c_info.deflate_setting.level = 1;
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info,
                       COMP_CODE_DEFLATE, &c_info);
  ASSERT_NE(FAIL, aid);

  const int32 total_bytes = 45000;
  std::vector<uint8_t> data(total_bytes);
  for (int32 i = 0; i < total_bytes; ++i) {
    data[i] = static_cast<uint8_t>(i & 0xFF);
  }

  EXPECT_EQ(total_bytes, Hwrite(aid, total_bytes, data.data()));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Re-open for reading
  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);

  aid = Hstartread(fid, tag, ref);
  ASSERT_NE(FAIL, aid);

  // Seek immediately when acc_init is 0 forward past DEFLATE_TMP_BUF_SIZE (e.g.
  // 35,000) This exercises while (offset + DEFLATE_TMP_BUF_SIZE < offset)
  // twice!
  EXPECT_EQ(SUCCEED, Hseek(aid, 35000, DF_START));
  uint8_t val = 0;
  EXPECT_EQ(1, Hread(aid, 1, &val));
  EXPECT_EQ(data[35000], val);

  // Seek backward to 5,000 (exercises offset < deflate_info->offset, term,
  // staccess2, Hseek)
  EXPECT_EQ(SUCCEED, Hseek(aid, 5000, DF_START));
  EXPECT_EQ(1, Hread(aid, 1, &val));
  EXPECT_EQ(data[5000], val);

  // Seek backward to 0
  EXPECT_EQ(SUCCEED, Hseek(aid, 0, DF_START));
  EXPECT_EQ(1, Hread(aid, 1, &val));
  EXPECT_EQ(data[0], val);

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  std::filesystem::remove(path);
}

// Test reading an existing compressed element with Hstartread
TEST_F(CdeflateTest, ReadExistingWithHstartread) {
  std::string path = GetTempFilePath("cdeflate_read_existing.hdf");
  std::filesystem::remove(path);

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  c_info.deflate_setting.level = 9;
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info,
                       COMP_CODE_DEFLATE, &c_info);
  ASSERT_NE(FAIL, aid);

  const std::string text = "Initial data written to compressed stream.";
  EXPECT_EQ(static_cast<int32>(text.size()),
            Hwrite(aid, static_cast<int32>(text.size()), text.data()));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Open with DFACC_READ and use Hstartread
  // The first Hread call triggers HCPcdeflate_read's check:
  // deflate_info->acc_init != DFACC_READ
  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);
  aid = Hstartread(fid, tag, ref);
  ASSERT_NE(FAIL, aid);

  std::vector<char> read_buf(text.size() + 1, 0);
  EXPECT_EQ(static_cast<int32>(text.size()),
            Hread(aid, static_cast<int32>(text.size()), read_buf.data()));
  EXPECT_STREQ(text.c_str(), read_buf.data());

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  std::filesystem::remove(path);
}

// Test very large write in a single call to trigger repeated io_buf flushes
// in HCIcdeflate_encode (lines 184-186).
TEST_F(CdeflateTest, VeryLargeSingleWrite) {
  std::string path = GetTempFilePath("cdeflate_very_large.hdf");
  std::filesystem::remove(path);

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  c_info.deflate_setting.level = 1;
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info,
                       COMP_CODE_DEFLATE, &c_info);
  ASSERT_NE(FAIL, aid);

  // 300,000 bytes with pseudorandom sequence to prevent high compression ratio
  const int32 total_bytes = 300000;
  std::vector<uint8_t> data(total_bytes);
  uint32_t val = 12345;
  for (int32 i = 0; i < total_bytes; ++i) {
    val = val * 1103515245 + 12345;
    data[i] = static_cast<uint8_t>((val >> 16) & 0xFF);
  }

  EXPECT_EQ(total_bytes, Hwrite(aid, total_bytes, data.data()));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Verify reading it back
  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);
  aid = Hstartread(fid, tag, ref);
  ASSERT_NE(FAIL, aid);

  std::vector<uint8_t> read_buf(total_bytes, 0);
  EXPECT_EQ(total_bytes, Hread(aid, total_bytes, read_buf.data()));
  EXPECT_EQ(data, read_buf);

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  std::filesystem::remove(path);
}

// Test invalid compression level fails in deflateInit (line 350)
TEST_F(CdeflateTest, InvalidCompressionLevel) {
  compinfo_t cinfo;
  std::memset(&cinfo, 0, sizeof(cinfo));
  cinfo.cinfo.coder_info.deflate_info.deflate_level =
      99;  // Invalid level (> 9)
  accrec_t access_rec;
  std::memset(&access_rec, 0, sizeof(access_rec));
  access_rec.access = DFACC_WRITE;
  access_rec.special_info = reinterpret_cast<uint8*>(&cinfo);

  uint8_t byte = 'X';
  EXPECT_EQ(FAIL, HCPcdeflate_write(&access_rec, 1, &byte));
  EXPECT_EQ(DFE_CINIT, HEvalue(1));
}

// Test corrupted compressed data triggers error in HCIcdeflate_decode (lines
// 135-136, 541)
TEST_F(CdeflateTest, CorruptedCompressedData) {
  std::string path = GetTempFilePath("cdeflate_corrupt.hdf");
  std::filesystem::remove(path);

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  c_info.deflate_setting.level = 6;
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info,
                       COMP_CODE_DEFLATE, &c_info);
  ASSERT_NE(FAIL, aid);

  const int32 total_bytes = 40000;
  std::vector<uint8_t> data(total_bytes);
  for (int32 i = 0; i < total_bytes; ++i) {
    data[i] = static_cast<uint8_t>(i % 127);
  }
  EXPECT_EQ(total_bytes, Hwrite(aid, total_bytes, data.data()));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Find the compressed tag/ref and corrupt its raw data bytes
  fid = Hopen(path.c_str(), DFACC_RDWR, 0);
  ASSERT_NE(FAIL, fid);

  // Look for DFTAG_COMPRESSED elements
  int32 raw_aid = Hstartaccess(fid, DFTAG_COMPRESSED, DFREF_WILDCARD,
                               DFACC_RDWR | DFACC_APPENDABLE);
  if (raw_aid != FAIL) {
    // Seek into raw compressed stream and write garbage
    EXPECT_EQ(SUCCEED, Hseek(raw_aid, 10, DF_START));
    std::vector<uint8_t> garbage(50, 0xFF);
    EXPECT_EQ(50, Hwrite(raw_aid, 50, garbage.data()));
    EXPECT_EQ(SUCCEED, Hendaccess(raw_aid));
  }
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Now open with Hstartread and try to read corrupted data
  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);

  aid = Hstartread(fid, tag, ref);
  ASSERT_NE(FAIL, aid);

  std::vector<uint8_t> buf(total_bytes, 0);
  EXPECT_EQ(FAIL, Hread(aid, total_bytes, buf.data()));
  EXPECT_EQ(DFE_READCOMP, HEvalue(4));
  EXPECT_EQ(DFE_CDECODE, HEvalue(3));

  EXPECT_EQ(SUCCEED, Hendaccess(aid));

  // Also test seek failure with corrupted data
  aid = Hstartread(fid, tag, ref);
  ASSERT_NE(FAIL, aid);
  EXPECT_EQ(FAIL, Hseek(aid, 30000, DF_START));
  EXPECT_EQ(DFE_READCOMP, HEvalue(4));
  EXPECT_EQ(DFE_CDECODE, HEvalue(3));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));

  EXPECT_EQ(SUCCEED, Hclose(fid));

  std::filesystem::remove(path);
}

constexpr int kDeflateBufSize = 4096;

// Test zlib stream error paths in encode, term, and endaccess
TEST_F(CdeflateTest, StreamErrorCoverage) {
  // Test deflate error in encode (lines 193-194)
  {
    compinfo_t cinfo;
    std::memset(&cinfo, 0, sizeof(cinfo));
    cinfo.cinfo.coder_info.deflate_info.acc_init = DFACC_WRITE;
    cinfo.cinfo.coder_info.deflate_info.acc_mode = DFACC_WRITE;
    cinfo.cinfo.coder_info.deflate_info.io_buf = malloc(kDeflateBufSize);
    cinfo.cinfo.coder_info.deflate_info.deflate_context.avail_out =
        kDeflateBufSize;
    cinfo.cinfo.coder_info.deflate_info.deflate_context.next_out =
        static_cast<Bytef*>(cinfo.cinfo.coder_info.deflate_info.io_buf);
    cinfo.cinfo.coder_info.deflate_info.deflate_context.state =
        nullptr;  // Causes deflate to return Z_STREAM_ERROR

    accrec_t access_rec;
    std::memset(&access_rec, 0, sizeof(access_rec));
    access_rec.access = DFACC_WRITE;
    access_rec.special_info = reinterpret_cast<uint8*>(&cinfo);

    uint8_t byte = 'A';
    EXPECT_EQ(FAIL, HCPcdeflate_write(&access_rec, 1, &byte));
    EXPECT_EQ(DFE_CENCODE, HEvalue(1));

    free(cinfo.cinfo.coder_info.deflate_info.io_buf);
  }

  // Test deflateEnd failure in term (line 254) and HCPcdeflate_endaccess
  // failure (line 664)
  {
    compinfo_t cinfo;
    std::memset(&cinfo, 0, sizeof(cinfo));
    cinfo.cinfo.coder_info.deflate_info.acc_init = DFACC_WRITE;
    cinfo.cinfo.coder_info.deflate_info.acc_mode = DFACC_WRITE;
    cinfo.cinfo.coder_info.deflate_info.io_buf = malloc(kDeflateBufSize);
    deflateInit(&(cinfo.cinfo.coder_info.deflate_info.deflate_context), 1);
    cinfo.cinfo.coder_info.deflate_info.deflate_context.avail_out =
        kDeflateBufSize;
    cinfo.cinfo.coder_info.deflate_info.deflate_context.next_out =
        static_cast<Bytef*>(cinfo.cinfo.coder_info.deflate_info.io_buf);
    deflate(&(cinfo.cinfo.coder_info.deflate_info.deflate_context), Z_FINISH);
    deflateEnd(&(cinfo.cinfo.coder_info.deflate_info.deflate_context));

    accrec_t access_rec;
    std::memset(&access_rec, 0, sizeof(access_rec));
    access_rec.access = DFACC_WRITE;
    access_rec.special_info = reinterpret_cast<uint8*>(&cinfo);

    EXPECT_EQ(FAIL, HCPcdeflate_endaccess(&access_rec));
    EXPECT_EQ(DFE_CTERM, HEvalue(1));

    free(cinfo.cinfo.coder_info.deflate_info.io_buf);
  }

  // Test inflateEnd failure in term (line 259)
  {
    compinfo_t cinfo;
    std::memset(&cinfo, 0, sizeof(cinfo));
    cinfo.cinfo.coder_info.deflate_info.acc_init = DFACC_READ;
    cinfo.cinfo.coder_info.deflate_info.acc_mode = DFACC_READ;
    cinfo.cinfo.coder_info.deflate_info.deflate_context.state = nullptr;

    accrec_t access_rec;
    std::memset(&access_rec, 0, sizeof(access_rec));
    access_rec.access = DFACC_READ;
    access_rec.special_info = reinterpret_cast<uint8*>(&cinfo);

    EXPECT_EQ(FAIL, HCPcdeflate_endaccess(&access_rec));
    EXPECT_EQ(DFE_CTERM, HEvalue(1));
  }
}

// Test seek backward error branch when Hseek fails (line 474)
TEST_F(CdeflateTest, SeekBackwardHseekFailure) {
  compinfo_t cinfo;
  std::memset(&cinfo, 0, sizeof(cinfo));
  cinfo.cinfo.coder_info.deflate_info.offset = 50;
  cinfo.cinfo.coder_info.deflate_info.acc_init = DFACC_READ;
  cinfo.cinfo.coder_info.deflate_info.acc_mode = DFACC_READ;
  // Initialize context so term succeeds
  inflateInit(&(cinfo.cinfo.coder_info.deflate_info.deflate_context));
  cinfo.aid = FAIL;  // Causes Hseek to fail

  accrec_t access_rec;
  std::memset(&access_rec, 0, sizeof(access_rec));
  access_rec.special_info = reinterpret_cast<uint8*>(&cinfo);

  EXPECT_EQ(FAIL, HCPcdeflate_seek(&access_rec, 10, DF_START));
  EXPECT_EQ(DFE_SEEKERROR, HEvalue(1));
  inflateEnd(&(cinfo.cinfo.coder_info.deflate_info.deflate_context));
}
TEST_F(CdeflateTest, StartWriteExisting) {
  std::string path = GetTempFilePath("cdeflate_startwrite.hdf");
  std::filesystem::remove(path);

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info m_info = {};
  comp_info c_info = {};
  c_info.deflate_setting.level = 6;
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info,
                       COMP_CODE_DEFLATE, &c_info);
  ASSERT_NE(FAIL, aid);

  const std::string text = "Initial data to overwrite";
  EXPECT_EQ(static_cast<int32>(text.size()),
            Hwrite(aid, static_cast<int32>(text.size()), text.data()));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Open with DFACC_WRITE and use Hstartwrite
  fid = Hopen(path.c_str(), DFACC_WRITE, 0);
  ASSERT_NE(FAIL, fid);

  aid = Hstartwrite(fid, tag, ref, static_cast<int32>(text.size()));
  ASSERT_NE(FAIL, aid);

  const std::string updated =
      "Replacement content completely replacing original";
  EXPECT_EQ(static_cast<int32>(updated.size()),
            Hwrite(aid, static_cast<int32>(updated.size()), updated.data()));

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  std::filesystem::remove(path);
}

// Test random write restriction:
// HCPcdeflate_write rejects writes in the middle unless offset == 0 and
// rewriting entire dataset.
TEST_F(CdeflateTest, RandomWriteUnsupported) {
  compinfo_t cinfo;
  std::memset(&cinfo, 0, sizeof(cinfo));
  cinfo.length = 100;
  cinfo.cinfo.coder_info.deflate_info.offset = 10;

  accrec_t access_rec;
  std::memset(&access_rec, 0, sizeof(access_rec));
  access_rec.access = DFACC_WRITE;
  access_rec.special_info = reinterpret_cast<uint8*>(&cinfo);

  uint8_t dummy[10] = {};
  // Writing at offset 10 where length is 100: offset != 0 && offset != length
  EXPECT_EQ(FAIL, HCPcdeflate_write(&access_rec, 20, dummy));
  EXPECT_EQ(DFE_UNSUPPORTED, HEvalue(1));
}

// Test direct error handling paths
TEST_F(CdeflateTest, DirectFunctionErrorHandling) {
  // 1. HCPcdeflate_stread with invalid file_id fails
  {
    compinfo_t cinfo;
    std::memset(&cinfo, 0, sizeof(cinfo));
    cinfo.aid = FAIL;
    cinfo.comp_ref = 9999;
    accrec_t access_rec;
    std::memset(&access_rec, 0, sizeof(access_rec));
    access_rec.file_id = FAIL;
    access_rec.special_info = reinterpret_cast<uint8*>(&cinfo);

    EXPECT_EQ(FAIL, HCPcdeflate_stread(&access_rec));
    EXPECT_EQ(DFE_CINIT, HEvalue(1));
  }

  // 2. HCPcdeflate_stwrite with invalid file_id fails
  {
    compinfo_t cinfo;
    std::memset(&cinfo, 0, sizeof(cinfo));
    cinfo.aid = FAIL;
    cinfo.comp_ref = 9999;
    accrec_t access_rec;
    std::memset(&access_rec, 0, sizeof(access_rec));
    access_rec.file_id = FAIL;
    access_rec.special_info = reinterpret_cast<uint8*>(&cinfo);

    EXPECT_EQ(FAIL, HCPcdeflate_stwrite(&access_rec));
    EXPECT_EQ(DFE_CINIT, HEvalue(1));
  }

  // 3. HCPcdeflate_read with invalid aid fails during Hseek
  {
    compinfo_t cinfo;
    std::memset(&cinfo, 0, sizeof(cinfo));
    cinfo.aid = FAIL;
    accrec_t access_rec;
    std::memset(&access_rec, 0, sizeof(access_rec));
    access_rec.special_info = reinterpret_cast<uint8*>(&cinfo);

    uint8_t buf[10] = {};
    EXPECT_EQ(FAIL, HCPcdeflate_read(&access_rec, 10, buf));
    // Clean up zlib resources initialized by staccess2 before Hseek failure
    inflateEnd(&(cinfo.cinfo.coder_info.deflate_info.deflate_context));
  }

  // 4. HCPcdeflate_write with invalid aid fails during Hseek
  {
    compinfo_t cinfo;
    std::memset(&cinfo, 0, sizeof(cinfo));
    cinfo.aid = FAIL;
    accrec_t access_rec;
    std::memset(&access_rec, 0, sizeof(access_rec));
    access_rec.special_info = reinterpret_cast<uint8*>(&cinfo);

    uint8_t buf[10] = {};
    EXPECT_EQ(FAIL, HCPcdeflate_write(&access_rec, 1, buf));
    // Clean up zlib resources initialized by staccess2 before Hseek failure
    deflateEnd(&(cinfo.cinfo.coder_info.deflate_info.deflate_context));
  }

  // 5. HCPcdeflate_seek with invalid aid fails during decode
  {
    compinfo_t cinfo;
    std::memset(&cinfo, 0, sizeof(cinfo));
    cinfo.aid = FAIL;
    accrec_t access_rec;
    std::memset(&access_rec, 0, sizeof(access_rec));
    access_rec.special_info = reinterpret_cast<uint8*>(&cinfo);

    EXPECT_EQ(FAIL, HCPcdeflate_seek(&access_rec, 100, DF_START));
    inflateEnd(&(cinfo.cinfo.coder_info.deflate_info.deflate_context));
  }

  // 6. HCPcdeflate_endaccess with invalid aid fails
  {
    compinfo_t cinfo;
    std::memset(&cinfo, 0, sizeof(cinfo));
    cinfo.aid = FAIL;
    accrec_t access_rec;
    std::memset(&access_rec, 0, sizeof(access_rec));
    access_rec.special_info = reinterpret_cast<uint8*>(&cinfo);

    EXPECT_EQ(FAIL, HCPcdeflate_endaccess(&access_rec));
  }
}

}  // namespace
