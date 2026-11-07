#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>  // NOLINT
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
#include "third_party/hdf4/hdf/src/cnone_priv.h"
// clang-format on
#include "third_party/hdf4/hdf/src/hcomp.h"
#include "third_party/hdf4/hdf/src/herr_priv.h"

namespace {

class CnoneTest : public ::testing::Test {
 protected:
  void SetUp() override { HEclear(); }

  void TearDown() override { HEclear(); }

  std::string GetTempFilePath(const std::string& name) {
    return (std::filesystem::path(::testing::TempDir()) / name).string();
  }
};

TEST_F(CnoneTest, FuncListPointersMatch) {
  EXPECT_EQ(cnone_funcs.stread, HCPcnone_stread);
  EXPECT_EQ(cnone_funcs.stwrite, HCPcnone_stwrite);
  EXPECT_EQ(cnone_funcs.seek, HCPcnone_seek);
  EXPECT_EQ(cnone_funcs.inquire, HCPcnone_inquire);
  EXPECT_EQ(cnone_funcs.read, HCPcnone_read);
  EXPECT_EQ(cnone_funcs.write, HCPcnone_write);
  EXPECT_EQ(cnone_funcs.endaccess, HCPcnone_endaccess);
}

TEST_F(CnoneTest, RoundtripCompressionNone) {
  std::string path = GetTempFilePath("cnone_roundtrip.hdf");
  std::filesystem::remove(path);

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  uint16 tag = DFTAG_RI;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  // Create a compressed element using COMP_CODE_NONE
  model_info m_info = {};
  comp_info c_info = {};
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &m_info, COMP_CODE_NONE,
                       &c_info);
  ASSERT_NE(FAIL, aid);

  const std::string test_data = "The quick brown fox jumps over the lazy dog";
  int32 written =
      Hwrite(aid, static_cast<int32>(test_data.size()), test_data.data());
  EXPECT_EQ(static_cast<int32>(test_data.size()), written);

  // Inquire through AID
  int32 file_id = FAIL;
  uint16 inq_tag = 0, inq_ref = 0;
  int32 length = 0, offset = 0, posn = 0;
  int16 access = 0, special = 0;
  EXPECT_EQ(SUCCEED, Hinquire(aid, &file_id, &inq_tag, &inq_ref, &length,
                              &offset, &posn, &access, &special));
  EXPECT_EQ(fid, file_id);
  EXPECT_EQ(MKSPECIALTAG(tag), inq_tag);
  EXPECT_EQ(ref, inq_ref);

  // Seek and read while still in write mode
  EXPECT_EQ(SUCCEED, Hseek(aid, 4, DF_START));
  char read_buf[6] = {};
  EXPECT_EQ(4, Hread(aid, 4, read_buf));
  read_buf[4] = '\0';
  EXPECT_STREQ("quic", read_buf);

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Reopen to verify read access via HCPcnone_stread
  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);

  aid = Hstartread(fid, tag, ref);
  ASSERT_NE(FAIL, aid);

  std::vector<char> full_read(test_data.size() + 1, 0);
  EXPECT_EQ(static_cast<int32>(test_data.size()),
            Hread(aid, static_cast<int32>(test_data.size()), full_read.data()));
  EXPECT_EQ(test_data, std::string(full_read.data(), test_data.size()));

  // Test seek
  EXPECT_EQ(SUCCEED, Hseek(aid, 10, DF_START));
  EXPECT_EQ(SUCCEED, Hseek(aid, 5, DF_START));

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Reopen with write access via HCPcnone_stwrite
  fid = Hopen(path.c_str(), DFACC_WRITE, 0);
  ASSERT_NE(FAIL, fid);

  aid = Hstartwrite(fid, tag, ref, static_cast<int32>(test_data.size()));
  ASSERT_NE(FAIL, aid);

  const std::string overwrite_data = "OVERWRITE";
  EXPECT_EQ(static_cast<int32>(overwrite_data.size()),
            Hwrite(aid, static_cast<int32>(overwrite_data.size()),
                   overwrite_data.data()));

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  std::filesystem::remove(path);
}

TEST_F(CnoneTest, DirectFunctionInquire) {
  accrec_t access_rec = {};
  int32 file_id = 0;
  uint16 tag = 0, ref = 0;
  int32 length = 0, offset = 0, posn = 0;
  int16 access = 0, special = 0;
  EXPECT_EQ(SUCCEED,
            HCPcnone_inquire(&access_rec, &file_id, &tag, &ref, &length,
                             &offset, &posn, &access, &special));
}

TEST_F(CnoneTest, DirectFunctionErrorHandling) {
  compinfo_t cinfo = {};
  cinfo.aid = FAIL;  // Invalid AID
  cinfo.comp_ref = 12345;
  cinfo.length = 100;

  accrec_t access_rec = {};
  access_rec.file_id = FAIL;
  access_rec.special_info = reinterpret_cast<uint8*>(&cinfo);

  // 1. HCPcnone_stread with invalid file_id fails
  EXPECT_EQ(FAIL, HCPcnone_stread(&access_rec));

  // 2. HCPcnone_stwrite with invalid file_id fails
  EXPECT_EQ(FAIL, HCPcnone_stwrite(&access_rec));

  // 3. HCPcnone_seek with invalid aid fails
  EXPECT_EQ(FAIL, HCPcnone_seek(&access_rec, 0, DF_START));

  // 4. HCPcnone_read with invalid aid fails
  char buf[10] = {};
  EXPECT_EQ(FAIL, HCPcnone_read(&access_rec, 10, buf));

  // 5. HCPcnone_write with invalid aid fails
  EXPECT_EQ(FAIL, HCPcnone_write(&access_rec, 10, buf));

  // 6. HCPcnone_endaccess with invalid aid fails
  EXPECT_EQ(FAIL, HCPcnone_endaccess(&access_rec));
}

}  // namespace
