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
// Tests for SZIP compression I/O routines (hdf/src/cszip.c).
//
// In google3, SZIP compression is compiled without libsz support
// (H4_HAVE_LIBSZ and H4_HAVE_SZIP_ENCODER are undefined). These tests
// verify the dispatch table, parameter manipulation helpers, and the
// fallback/error pathways when SZIP is disabled.
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/cszip.c

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>

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
#include "third_party/hdf4/hdf/src/cszip_priv.h"
// clang-format on
#include "third_party/hdf4/hdf/src/hcomp.h"
#include "third_party/hdf4/hdf/src/herr_priv.h"

namespace hdf4 {
namespace {

class CszipTest : public ::testing::Test {
 protected:
  void SetUp() override { HEclear(); }

  void TearDown() override { HEclear(); }

  std::string GetTempFilePath(const std::string& name) {
    return (std::filesystem::path(::testing::TempDir()) / name).string();
  }
};

TEST_F(CszipTest, FuncListPointersMatch) {
  EXPECT_EQ(cszip_funcs.stread, HCPcszip_stread);
  EXPECT_EQ(cszip_funcs.stwrite, HCPcszip_stwrite);
  EXPECT_EQ(cszip_funcs.seek, HCPcszip_seek);
  EXPECT_EQ(cszip_funcs.inquire, HCPcszip_inquire);
  EXPECT_EQ(cszip_funcs.read, HCPcszip_read);
  EXPECT_EQ(cszip_funcs.write, HCPcszip_write);
  EXPECT_EQ(cszip_funcs.endaccess, HCPcszip_endaccess);
}

TEST_F(CszipTest, InquireAlwaysSucceeds) {
  accrec_t access_rec;
  std::memset(&access_rec, 0, sizeof(access_rec));
  int32 file_id = 0;
  uint16 tag = 0, ref = 0;
  int32 length = 0, offset = 0, posn = 0;
  int16 access = 0, special = 0;
  EXPECT_EQ(SUCCEED,
            HCPcszip_inquire(&access_rec, &file_id, &tag, &ref, &length,
                             &offset, &posn, &access, &special));
}

TEST_F(CszipTest, SetupSzipParmsFailsWhenDisabled) {
  comp_info c_info = {};
  int32 dims[2] = {10, 20};
  // When SZIP is not enabled, HCPsetup_szip_parms always returns FAIL
  EXPECT_EQ(FAIL,
            HCPsetup_szip_parms(&c_info, DFNT_INT32, 1, 2, dims, nullptr));
}

TEST_F(CszipTest, RmSzipSpecialBit) {
  // 1. Null pointer check
  HEclear();
  EXPECT_EQ(FAIL, HCPrm_szip_special_bit(nullptr));
  EXPECT_EQ(DFE_INTERNAL, HEvalue(1));

  // 2. Options mask with SZ_H4_REV_2 bit set
  comp_info c_info = {};
  c_info.szip.options_mask = H4_SZ_NN_OPTION_MASK | SZ_H4_REV_2;
  EXPECT_EQ(SUCCEED, HCPrm_szip_special_bit(&c_info));
  EXPECT_EQ(H4_SZ_NN_OPTION_MASK, c_info.szip.options_mask);

  // 3. Options mask without SZ_H4_REV_2 bit
  c_info.szip.options_mask = H4_SZ_EC_OPTION_MASK;
  EXPECT_EQ(SUCCEED, HCPrm_szip_special_bit(&c_info));
  EXPECT_EQ(H4_SZ_EC_OPTION_MASK, c_info.szip.options_mask);
}

TEST_F(CszipTest, WriteFailsWhenDisabled) {
  accrec_t access_rec;
  std::memset(&access_rec, 0, sizeof(access_rec));
  uint8_t buf[10] = {1, 2, 3};

  HEclear();
  EXPECT_EQ(FAIL, HCPcszip_write(&access_rec, 10, buf));
  // In cszip.c when H4_HAVE_SZIP_ENCODER is undefined, write returns
  // DFE_CANTDECOMP
  EXPECT_EQ(DFE_CANTDECOMP, HEvalue(1));
}

TEST_F(CszipTest, StwriteFailsWhenDisabled) {
  accrec_t access_rec;
  std::memset(&access_rec, 0, sizeof(access_rec));

  HEclear();
  EXPECT_EQ(FAIL, HCPcszip_stwrite(&access_rec));
  EXPECT_EQ(DFE_CINIT, HEvalue(1));
}

TEST_F(CszipTest, ReadFailsWhenDisabled) {
  compinfo_t cinfo;
  std::memset(&cinfo, 0, sizeof(cinfo));

  accrec_t access_rec;
  std::memset(&access_rec, 0, sizeof(access_rec));
  access_rec.special_info = reinterpret_cast<uint8*>(&cinfo);

  uint8_t buf[10] = {};
  HEclear();
  EXPECT_EQ(FAIL, HCPcszip_read(&access_rec, 10, buf));
  EXPECT_EQ(DFE_CDECODE, HEvalue(1));
}

TEST_F(CszipTest, StreadAndLifecycle) {
  std::string path = GetTempFilePath("cszip_test.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  // Write a dummy raw DFTAG_COMPRESSED element
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);
  int32 aid = Hstartwrite(fid, DFTAG_COMPRESSED, ref, 20);
  ASSERT_NE(FAIL, aid);
  const char dummy_data[20] = "1234567890123456789";
  EXPECT_EQ(20, Hwrite(aid, 20, dummy_data));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Re-open for reading
  fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid);

  compinfo_t cinfo;
  std::memset(&cinfo, 0, sizeof(cinfo));
  cinfo.comp_ref = ref;
  // Pre-allocate buffer in szip_info to exercise cleanup in HCIcszip_init
  cinfo.cinfo.coder_info.szip_info.buffer = static_cast<uint8*>(malloc(32));
  cinfo.cinfo.coder_info.szip_info.buffer_size = 32;

  accrec_t access_rec;
  std::memset(&access_rec, 0, sizeof(access_rec));
  access_rec.file_id = fid;
  access_rec.access = DFACC_READ;
  access_rec.special_info = reinterpret_cast<uint8*>(&cinfo);

  // HCPcszip_stread should succeed, opening the element and calling
  // HCIcszip_init
  EXPECT_EQ(SUCCEED, HCPcszip_stread(&access_rec));
  EXPECT_NE(FAIL, cinfo.aid);
  EXPECT_EQ(0, cinfo.cinfo.coder_info.szip_info.buffer_size);
  EXPECT_EQ(nullptr, cinfo.cinfo.coder_info.szip_info.buffer);

  // Seek forward (> TMP_BUF_SIZE = 8192) triggers decode failure in chunk loop
  HEclear();
  EXPECT_EQ(FAIL, HCPcszip_seek(&access_rec, 9000, DF_START));
  EXPECT_EQ(DFE_CDECODE, HEvalue(1));

  // Seek backward with clean state re-initializes successfully
  cinfo.cinfo.coder_info.szip_info.offset = 50;
  EXPECT_EQ(SUCCEED, HCPcszip_seek(&access_rec, 0, DF_START));
  EXPECT_EQ(0, cinfo.cinfo.coder_info.szip_info.offset);

  // Seek backward with dirty state calls term, which fails
  cinfo.cinfo.coder_info.szip_info.offset = 50;
  cinfo.cinfo.coder_info.szip_info.szip_dirty =
      comp_coder_szip_info_t::SZIP_DIRTY;
  cinfo.cinfo.coder_info.szip_info.szip_state =
      comp_coder_szip_info_t::SZIP_RUN;
  HEclear();
  EXPECT_EQ(FAIL, HCPcszip_seek(&access_rec, 10, DF_START));
  EXPECT_EQ(DFE_CTERM, HEvalue(1));

  // HCPcszip_endaccess with dirty state also fails term
  HEclear();
  EXPECT_EQ(FAIL, HCPcszip_endaccess(&access_rec));
  EXPECT_EQ(DFE_CTERM, HEvalue(1));

  // HCPcszip_endaccess with clean state succeeds
  cinfo.cinfo.coder_info.szip_info.szip_dirty =
      comp_coder_szip_info_t::SZIP_CLEAN;
  cinfo.cinfo.coder_info.szip_info.szip_state =
      comp_coder_szip_info_t::SZIP_INIT;
  EXPECT_EQ(SUCCEED, HCPcszip_endaccess(&access_rec));

  // Endaccess again with already-closed AID fails
  HEclear();
  EXPECT_EQ(FAIL, HCPcszip_endaccess(&access_rec));
  EXPECT_EQ(DFE_CANTCLOSE, HEvalue(1));

  EXPECT_EQ(SUCCEED, Hclose(fid));
}

TEST_F(CszipTest, StreadFailureInvalidFile) {
  compinfo_t cinfo;
  std::memset(&cinfo, 0, sizeof(cinfo));
  cinfo.comp_ref = 9999;

  accrec_t access_rec;
  std::memset(&access_rec, 0, sizeof(access_rec));
  access_rec.file_id = FAIL;
  access_rec.access = DFACC_READ;
  access_rec.special_info = reinterpret_cast<uint8*>(&cinfo);

  HEclear();
  EXPECT_EQ(FAIL, HCPcszip_stread(&access_rec));
  EXPECT_EQ(DFE_CINIT, HEvalue(1));
}

TEST_F(CszipTest, InitFailureInvalidAid) {
  compinfo_t cinfo;
  std::memset(&cinfo, 0, sizeof(cinfo));
  cinfo.aid = FAIL;

  accrec_t access_rec;
  std::memset(&access_rec, 0, sizeof(access_rec));
  access_rec.special_info = reinterpret_cast<uint8*>(&cinfo);

  // Calling seek backward when aid is invalid triggers HCIcszip_init failure
  cinfo.cinfo.coder_info.szip_info.offset = 50;
  cinfo.cinfo.coder_info.szip_info.szip_dirty =
      comp_coder_szip_info_t::SZIP_CLEAN;
  HEclear();
  EXPECT_EQ(FAIL, HCPcszip_seek(&access_rec, 10, DF_START));
  EXPECT_EQ(DFE_CINIT, HEvalue(1));
}

}  // namespace
}  // namespace hdf4
