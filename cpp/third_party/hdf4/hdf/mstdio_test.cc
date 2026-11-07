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
// Tests for stdio-like modeling compression I/O (hdf/src/mstdio.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/mstdio.c

#include <cstdint>
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

#include "third_party/hdf4/hdf/src/hdf.h"
// clang-format off
#include "third_party/hdf4/hdf/src/hcomp.h"
#include "third_party/hdf4/hdf/src/hcomp_priv.h"
#include "third_party/hdf4/hdf/src/hfile_priv.h"
#include "third_party/hdf4/hdf/src/mstdio_priv.h"
// clang-format on
#include "third_party/hdf4/hdf/src/herr_priv.h"

namespace hdf4 {
namespace {

using ::testing::ElementsAreArray;

// Mock coder functions for unit testing
static int32 MockStreadSuccess(accrec_t* /*rec*/) { return SUCCEED; }
static int32 MockStreadFail(accrec_t* /*rec*/) { return FAIL; }

static int32 MockStwriteSuccess(accrec_t* /*rec*/) { return SUCCEED; }
static int32 MockStwriteFail(accrec_t* /*rec*/) { return FAIL; }

static int32 MockSeekSuccess(accrec_t* /*rec*/, int32 offset, int /*origin*/) {
  return offset;
}
static int32 MockSeekFail(accrec_t* /*rec*/, int32 /*offset*/, int /*origin*/) {
  return FAIL;
}

static int32 MockReadSuccess(accrec_t* /*rec*/, int32 length, void* data) {
  if (data != nullptr && length > 0) {
    std::memset(data, 0xAB, static_cast<size_t>(length));
  }
  return length;
}
static int32 MockReadFail(accrec_t* /*rec*/, int32 /*length*/, void* /*data*/) {
  return FAIL;
}

static int32 MockWriteSuccess(accrec_t* /*rec*/, int32 length,
                              const void* /*data*/) {
  return length;
}
static int32 MockWriteFail(accrec_t* /*rec*/, int32 /*length*/,
                           const void* /*data*/) {
  return FAIL;
}

static int32 MockInquireSuccess(accrec_t* /*rec*/, int32* pfile_id,
                                uint16* ptag, uint16* pref, int32* plength,
                                int32* poffset, int32* pposn, int16* paccess,
                                int16* pspecial) {
  if (pfile_id) *pfile_id = 42;
  if (ptag) *ptag = DFTAG_COMPRESSED;
  if (pref) *pref = 100;
  if (plength) *plength = 1024;
  if (poffset) *poffset = 512;
  if (pposn) *pposn = 256;
  if (paccess) *paccess = DFACC_READ;
  if (pspecial) *pspecial = SPECIAL_COMP;
  return SUCCEED;
}
static int32 MockInquireFail(accrec_t* /*rec*/, int32* /*pfile_id*/,
                             uint16* /*ptag*/, uint16* /*pref*/,
                             int32* /*plength*/, int32* /*poffset*/,
                             int32* /*pposn*/, int16* /*paccess*/,
                             int16* /*pspecial*/) {
  return FAIL;
}

static int MockEndaccessSuccess(accrec_t* /*rec*/) { return SUCCEED; }
static int MockEndaccessFail(accrec_t* /*rec*/) { return FAIL; }

class MstdioTest : public ::testing::Test {
 protected:
  void SetUp() override { HEclear(); }

  void TearDown() override { HEclear(); }

  std::string GetTempFilePath(const std::string& name) {
    return (std::filesystem::path(::testing::TempDir()) / name).string();
  }
};

// 1. Verify mstdio_funcs function pointer table
TEST_F(MstdioTest, FunctionTableInitializedCorrectly) {
  EXPECT_EQ(mstdio_funcs.stread, HCPmstdio_stread);
  EXPECT_EQ(mstdio_funcs.stwrite, HCPmstdio_stwrite);
  EXPECT_EQ(mstdio_funcs.seek, HCPmstdio_seek);
  EXPECT_EQ(mstdio_funcs.inquire, HCPmstdio_inquire);
  EXPECT_EQ(mstdio_funcs.read, HCPmstdio_read);
  EXPECT_EQ(reinterpret_cast<void*>(mstdio_funcs.write),
            reinterpret_cast<void*>(HCPmstdio_write));
  EXPECT_EQ(mstdio_funcs.endaccess, HCPmstdio_endaccess);
}

// 2. Unit tests for HCPmstdio_stread
TEST_F(MstdioTest, StreadSuccessAndFailure) {
  accrec_t rec = {};
  compinfo_t info = {};
  rec.special_info = &info;
  info.minfo.model_info.stdio_info.pos = 999;

  // Success path
  info.cinfo.coder_funcs.stread = MockStreadSuccess;
  EXPECT_EQ(SUCCEED, HCPmstdio_stread(&rec));
  EXPECT_EQ(0, info.minfo.model_info.stdio_info.pos);

  // Failure path
  info.minfo.model_info.stdio_info.pos = 555;
  info.cinfo.coder_funcs.stread = MockStreadFail;
  HEclear();
  EXPECT_EQ(FAIL, HCPmstdio_stread(&rec));
  EXPECT_EQ(0, info.minfo.model_info.stdio_info.pos);
  EXPECT_EQ(DFE_CODER, HEvalue(1));
}

// 3. Unit tests for HCPmstdio_stwrite
TEST_F(MstdioTest, StwriteSuccessAndFailure) {
  accrec_t rec = {};
  compinfo_t info = {};
  rec.special_info = &info;
  info.minfo.model_info.stdio_info.pos = 999;

  // Success path
  info.cinfo.coder_funcs.stwrite = MockStwriteSuccess;
  EXPECT_EQ(SUCCEED, HCPmstdio_stwrite(&rec));
  EXPECT_EQ(0, info.minfo.model_info.stdio_info.pos);

  // Failure path
  info.minfo.model_info.stdio_info.pos = 555;
  info.cinfo.coder_funcs.stwrite = MockStwriteFail;
  HEclear();
  EXPECT_EQ(FAIL, HCPmstdio_stwrite(&rec));
  EXPECT_EQ(0, info.minfo.model_info.stdio_info.pos);
  EXPECT_EQ(DFE_CODER, HEvalue(1));
}

// 4. Unit tests for HCPmstdio_seek
TEST_F(MstdioTest, SeekSuccessAndFailure) {
  accrec_t rec = {};
  compinfo_t info = {};
  rec.special_info = &info;
  info.minfo.model_info.stdio_info.pos = 0;

  // Success path
  info.cinfo.coder_funcs.seek = MockSeekSuccess;
  EXPECT_EQ(1234, HCPmstdio_seek(&rec, 1234, 0));
  EXPECT_EQ(1234, info.minfo.model_info.stdio_info.pos);

  // Failure path
  info.cinfo.coder_funcs.seek = MockSeekFail;
  HEclear();
  EXPECT_EQ(FAIL, HCPmstdio_seek(&rec, 5678, 0));
  EXPECT_EQ(5678, info.minfo.model_info.stdio_info.pos);
  EXPECT_EQ(DFE_CODER, HEvalue(1));
}

// 5. Unit tests for HCPmstdio_read
TEST_F(MstdioTest, ReadSuccessAndFailure) {
  accrec_t rec = {};
  compinfo_t info = {};
  rec.special_info = &info;
  info.minfo.model_info.stdio_info.pos = 100;

  uint8_t buf[16] = {};

  // Success path
  info.cinfo.coder_funcs.read = MockReadSuccess;
  EXPECT_EQ(16, HCPmstdio_read(&rec, 16, buf));
  EXPECT_EQ(116, info.minfo.model_info.stdio_info.pos);
  EXPECT_EQ(0xAB, buf[0]);

  // Failure path
  info.cinfo.coder_funcs.read = MockReadFail;
  HEclear();
  EXPECT_EQ(FAIL, HCPmstdio_read(&rec, 8, buf));
  EXPECT_EQ(124, info.minfo.model_info.stdio_info.pos);
  EXPECT_EQ(DFE_CODER, HEvalue(1));
}

// 6. Unit tests for HCPmstdio_write
TEST_F(MstdioTest, WriteSuccessAndFailure) {
  accrec_t rec = {};
  compinfo_t info = {};
  rec.special_info = &info;
  info.minfo.model_info.stdio_info.pos = 200;

  const uint8_t data[32] = {};

  // Success path
  info.cinfo.coder_funcs.write = MockWriteSuccess;
  EXPECT_EQ(32, HCPmstdio_write(&rec, 32, data));
  EXPECT_EQ(232, info.minfo.model_info.stdio_info.pos);

  // Failure path
  info.cinfo.coder_funcs.write = MockWriteFail;
  HEclear();
  EXPECT_EQ(FAIL, HCPmstdio_write(&rec, 10, data));
  EXPECT_EQ(242, info.minfo.model_info.stdio_info.pos);
  EXPECT_EQ(DFE_CODER, HEvalue(1));
}

// 7. Unit tests for HCPmstdio_inquire
TEST_F(MstdioTest, InquireSuccessAndFailure) {
  accrec_t rec = {};
  compinfo_t info = {};
  rec.special_info = &info;

  int32 file_id = 0, length = 0, offset = 0, posn = 0;
  uint16 tag = 0, ref = 0;
  int16 access = 0, special = 0;

  // Success path
  info.cinfo.coder_funcs.inquire = MockInquireSuccess;
  EXPECT_EQ(SUCCEED, HCPmstdio_inquire(&rec, &file_id, &tag, &ref, &length,
                                       &offset, &posn, &access, &special));
  EXPECT_EQ(42, file_id);
  EXPECT_EQ(DFTAG_COMPRESSED, tag);
  EXPECT_EQ(100, ref);
  EXPECT_EQ(1024, length);
  EXPECT_EQ(512, offset);
  EXPECT_EQ(256, posn);
  EXPECT_EQ(DFACC_READ, access);
  EXPECT_EQ(SPECIAL_COMP, special);

  // Failure path
  info.cinfo.coder_funcs.inquire = MockInquireFail;
  HEclear();
  EXPECT_EQ(FAIL, HCPmstdio_inquire(&rec, &file_id, &tag, &ref, &length,
                                    &offset, &posn, &access, &special));
  EXPECT_EQ(DFE_CODER, HEvalue(1));
}

// 8. Unit tests for HCPmstdio_endaccess
TEST_F(MstdioTest, EndaccessSuccessAndFailure) {
  accrec_t rec = {};
  compinfo_t info = {};
  rec.special_info = &info;

  // Success path
  info.cinfo.coder_funcs.endaccess = MockEndaccessSuccess;
  EXPECT_EQ(SUCCEED, HCPmstdio_endaccess(&rec));

  // Failure path
  info.cinfo.coder_funcs.endaccess = MockEndaccessFail;
  HEclear();
  EXPECT_EQ(FAIL, HCPmstdio_endaccess(&rec));
  EXPECT_EQ(DFE_CODER, HEvalue(1));
}

// 9. End-to-end integration test exercising mstdio layer through HCcreate
TEST_F(MstdioTest, EndToEndCompressedElementUsingMstdioLayer) {
  std::string path = GetTempFilePath("mstdio_integration.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  const std::string text = "HDF4 stdio-like modeling layer test payload 12345";
  constexpr uint16_t tag = 1000;
  uint16 ref = 0;

  {
    int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ref = Hnewref(fid);
    ASSERT_NE(0, ref);

    comp_info cinfo = {};
    cinfo.deflate_setting.level = 6;
    model_info minfo = {};

    // HCcreate sets up COMP_MODEL_STDIO (mstdio_funcs)
    int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &minfo,
                         COMP_CODE_DEFLATE, &cinfo);
    ASSERT_NE(FAIL, aid);
    absl::Cleanup close_aid = [aid] { EXPECT_EQ(SUCCEED, Hendaccess(aid)); };

    EXPECT_EQ(static_cast<int32_t>(text.size()),
              Hwrite(aid, text.size(), text.data()));

    // Inquire through mstdio layer
    int32 length = 0;
    EXPECT_EQ(SUCCEED, Hinquire(aid, nullptr, nullptr, nullptr, &length,
                                nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ(static_cast<int32_t>(text.size()), length);
  }

  // Re-open for read and verify seek and read through mstdio layer
  {
    int32 fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    int32 aid = Hstartread(fid, tag, ref);
    ASSERT_NE(FAIL, aid);
    absl::Cleanup close_aid = [aid] { EXPECT_EQ(SUCCEED, Hendaccess(aid)); };

    std::vector<char> read_buf(text.size(), '\0');
    EXPECT_EQ(static_cast<int32_t>(text.size()),
              Hread(aid, text.size(), read_buf.data()));
    EXPECT_EQ(text, std::string(read_buf.begin(), read_buf.end()));

    // Seek and read partial data
    EXPECT_EQ(SUCCEED, Hseek(aid, 5, 0));
    char partial[6] = {};
    EXPECT_EQ(5, Hread(aid, 5, partial));
    EXPECT_STREQ("stdio", partial);
  }
}

}  // namespace
}  // namespace hdf4
