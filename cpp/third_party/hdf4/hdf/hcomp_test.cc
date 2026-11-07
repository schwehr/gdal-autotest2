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
// Tests for HDF4 compression layer (hdf/src/hcomp.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/hcomp.c

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

#ifdef FAIL
#undef FAIL
#endif
#ifdef SUCCEED
#undef SUCCEED
#endif

#include "third_party/hdf4/hdf/src/hdf.h"
// clang-format off
#include "third_party/hdf4/hdf/src/hcomp.h"
#include "third_party/hdf4/hdf/src/hcomp_priv.h"
#include "third_party/hdf4/hdf/src/hfile_priv.h"
// clang-format on
#include "third_party/hdf4/hdf/src/herr.h"
#include "third_party/hdf4/hdf/src/hfile.h"
#include "third_party/hdf4/hdf/src/hproto.h"

namespace hdf4 {
namespace {

using ::testing::Eq;
using ::testing::Ge;
using ::testing::Ne;

std::string GetTempFilePath(const std::string& filename) {
  return (std::filesystem::path(::testing::TempDir()) / filename).string();
}

// ----------------------------------------------------------------------------
// 1. HCget_config_info tests
// ----------------------------------------------------------------------------
TEST(HcompConfigTest, CompressMapTable) {
  EXPECT_EQ(0, compress_map[COMP_NONE]);
  EXPECT_EQ(DFTAG_JPEG5, compress_map[COMP_JPEG]);
  EXPECT_EQ(DFTAG_RLE, compress_map[COMP_RLE]);
  EXPECT_EQ(DFTAG_IMC, compress_map[COMP_IMCOMP]);
}

TEST(HcompConfigTest, ConfigurationFlagsForCoders) {
  uint32_t config = 0;

  // Coders that support both encoding and decoding in this build
  constexpr comp_coder_t kEnabledCoders[] = {
      COMP_CODE_NONE, COMP_CODE_RLE,     COMP_CODE_NBIT,
      COMP_CODE_JPEG, COMP_CODE_SKPHUFF, COMP_CODE_DEFLATE,
  };
  for (comp_coder_t coder : kEnabledCoders) {
    config = 0;
    EXPECT_EQ(SUCCEED, HCget_config_info(coder, &config));
    EXPECT_EQ(COMP_DECODER_ENABLED | COMP_ENCODER_ENABLED, config);
  }

  // IMCOMP is obsolete / inquiry only, config is 0
  config = 0xFF;
  EXPECT_EQ(SUCCEED, HCget_config_info(COMP_CODE_IMCOMP, &config));
  EXPECT_EQ(0u, config);

  // SZIP in google3 has H4_HAVE_LIBSZ disabled, config is 0
  config = 0xFF;
  EXPECT_EQ(SUCCEED, HCget_config_info(COMP_CODE_SZIP, &config));
  EXPECT_EQ(0u, config);

  // Invalid coders return FAIL and DFE_BADCODER
  HEclear();
  config = 0xFF;
  EXPECT_EQ(FAIL, HCget_config_info(COMP_CODE_INVALID, &config));
  EXPECT_EQ(0u, config);
  EXPECT_EQ(DFE_BADCODER, HEvalue(1));
  HEclear();

  EXPECT_EQ(FAIL, HCget_config_info(static_cast<comp_coder_t>(999), &config));
  EXPECT_EQ(0u, config);
  EXPECT_EQ(DFE_BADCODER, HEvalue(1));
  HEclear();
}

// ----------------------------------------------------------------------------
// 2. Header query, encoding and decoding tests
// ----------------------------------------------------------------------------
TEST(HcompHeaderTest, QueryEncodeHeaderValidationAndLengths) {
  model_info minfo = {};
  comp_info cinfo = {};

  // Null arguments
  HEclear();
  EXPECT_EQ(FAIL, HCPquery_encode_header(COMP_MODEL_STDIO, nullptr,
                                         COMP_CODE_NONE, &cinfo));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  HEclear();

  EXPECT_EQ(FAIL, HCPquery_encode_header(COMP_MODEL_STDIO, &minfo,
                                         COMP_CODE_NONE, nullptr));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  HEclear();

  // Model len = 2, Coder len = 2 + extra
  // NONE / RLE: 2 + 2 = 4
  EXPECT_EQ(4, HCPquery_encode_header(COMP_MODEL_STDIO, &minfo, COMP_CODE_NONE,
                                      &cinfo));
  EXPECT_EQ(4, HCPquery_encode_header(COMP_MODEL_STDIO, &minfo, COMP_CODE_RLE,
                                      &cinfo));

  // NBIT: 4 + 16 = 20
  EXPECT_EQ(20, HCPquery_encode_header(COMP_MODEL_STDIO, &minfo, COMP_CODE_NBIT,
                                       &cinfo));

  // SKPHUFF: 4 + 8 = 12
  EXPECT_EQ(12, HCPquery_encode_header(COMP_MODEL_STDIO, &minfo,
                                       COMP_CODE_SKPHUFF, &cinfo));

  // DEFLATE: 4 + 2 = 6
  EXPECT_EQ(6, HCPquery_encode_header(COMP_MODEL_STDIO, &minfo,
                                      COMP_CODE_DEFLATE, &cinfo));

  // SZIP: 4 + 14 = 18
  EXPECT_EQ(18, HCPquery_encode_header(COMP_MODEL_STDIO, &minfo, COMP_CODE_SZIP,
                                       &cinfo));

  // IMCOMP: unsupported, returns FAIL and DFE_BADCODER
  HEclear();
  EXPECT_EQ(FAIL, HCPquery_encode_header(COMP_MODEL_STDIO, &minfo,
                                         COMP_CODE_IMCOMP, &cinfo));
  EXPECT_EQ(DFE_BADCODER, HEvalue(1));
  HEclear();
}

TEST(HcompHeaderTest, EncodeAndDecodeValidation) {
  model_info minfo = {};
  comp_info cinfo = {};
  uint8_t buffer[64] = {};

  // Null arguments for encode
  HEclear();
  EXPECT_EQ(FAIL, HCPencode_header(nullptr, COMP_MODEL_STDIO, &minfo,
                                   COMP_CODE_NONE, &cinfo));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  HEclear();

  EXPECT_EQ(FAIL, HCPencode_header(buffer, COMP_MODEL_STDIO, nullptr,
                                   COMP_CODE_NONE, &cinfo));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  HEclear();

  EXPECT_EQ(FAIL, HCPencode_header(buffer, COMP_MODEL_STDIO, &minfo,
                                   COMP_CODE_NONE, nullptr));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  HEclear();

  // Null arguments for decode
  comp_model_t dec_model = COMP_MODEL_STDIO;
  comp_coder_t dec_coder = COMP_CODE_NONE;
  model_info dec_minfo = {};
  comp_info dec_cinfo = {};

  HEclear();
  EXPECT_EQ(FAIL, HCPdecode_header(nullptr, &dec_model, &dec_minfo, &dec_coder,
                                   &dec_cinfo));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  HEclear();

  EXPECT_EQ(FAIL, HCPdecode_header(buffer, nullptr, &dec_minfo, &dec_coder,
                                   &dec_cinfo));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  HEclear();

  EXPECT_EQ(FAIL, HCPdecode_header(buffer, &dec_model, nullptr, &dec_coder,
                                   &dec_cinfo));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  HEclear();

  EXPECT_EQ(FAIL, HCPdecode_header(buffer, &dec_model, &dec_minfo, nullptr,
                                   &dec_cinfo));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  HEclear();

  EXPECT_EQ(FAIL, HCPdecode_header(buffer, &dec_model, &dec_minfo, &dec_coder,
                                   nullptr));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  HEclear();

  // IMCOMP fails encode with DFE_BADCODER
  HEclear();
  EXPECT_EQ(FAIL, HCPencode_header(buffer, COMP_MODEL_STDIO, &minfo,
                                   COMP_CODE_IMCOMP, &cinfo));
  EXPECT_EQ(DFE_BADCODER, HEvalue(1));
  HEclear();

  // SKPHUFF with skp_size < 1 fails encode
  cinfo.skphuff.skp_size = 0;
  HEclear();
  EXPECT_EQ(FAIL, HCPencode_header(buffer, COMP_MODEL_STDIO, &minfo,
                                   COMP_CODE_SKPHUFF, &cinfo));
  EXPECT_EQ(DFE_BADCODER, HEvalue(1));
  HEclear();

  // DEFLATE with invalid level fails encode
  cinfo.deflate_setting.level = -1;
  HEclear();
  EXPECT_EQ(FAIL, HCPencode_header(buffer, COMP_MODEL_STDIO, &minfo,
                                   COMP_CODE_DEFLATE, &cinfo));
  EXPECT_EQ(DFE_BADCODER, HEvalue(1));
  HEclear();

  cinfo.deflate_setting.level = 10;
  HEclear();
  EXPECT_EQ(FAIL, HCPencode_header(buffer, COMP_MODEL_STDIO, &minfo,
                                   COMP_CODE_DEFLATE, &cinfo));
  EXPECT_EQ(DFE_BADCODER, HEvalue(1));
  HEclear();

  // IMCOMP is no longer supported for encoding
  HEclear();
  EXPECT_EQ(FAIL, HCPencode_header(buffer, COMP_MODEL_STDIO, &minfo,
                                   COMP_CODE_IMCOMP, &cinfo));
  EXPECT_EQ(DFE_BADCODER, HEvalue(1));
  HEclear();
}

TEST(HcompHeaderTest, EncodeAndDecodeRoundTripAllCoders) {
  model_info minfo = {};
  comp_info cinfo = {};
  uint8_t buffer[64] = {};

  comp_model_t dec_model = static_cast<comp_model_t>(-1);
  comp_coder_t dec_coder = static_cast<comp_coder_t>(-1);
  model_info dec_minfo = {};
  comp_info dec_cinfo = {};

  // 1. COMP_CODE_NONE
  std::memset(buffer, 0, sizeof(buffer));
  EXPECT_EQ(SUCCEED, HCPencode_header(buffer, COMP_MODEL_STDIO, &minfo,
                                      COMP_CODE_NONE, &cinfo));
  EXPECT_EQ(SUCCEED, HCPdecode_header(buffer, &dec_model, &dec_minfo,
                                      &dec_coder, &dec_cinfo));
  EXPECT_EQ(COMP_MODEL_STDIO, dec_model);
  EXPECT_EQ(COMP_CODE_NONE, dec_coder);

  // 2. COMP_CODE_RLE
  std::memset(buffer, 0, sizeof(buffer));
  EXPECT_EQ(SUCCEED, HCPencode_header(buffer, COMP_MODEL_STDIO, &minfo,
                                      COMP_CODE_RLE, &cinfo));
  EXPECT_EQ(SUCCEED, HCPdecode_header(buffer, &dec_model, &dec_minfo,
                                      &dec_coder, &dec_cinfo));
  EXPECT_EQ(COMP_MODEL_STDIO, dec_model);
  EXPECT_EQ(COMP_CODE_RLE, dec_coder);

  // 3. COMP_CODE_NBIT
  cinfo.nbit.nt = DFNT_INT32;
  cinfo.nbit.sign_ext = 1;
  cinfo.nbit.fill_one = 0;
  cinfo.nbit.start_bit = 4;
  cinfo.nbit.bit_len = 24;
  std::memset(buffer, 0, sizeof(buffer));
  EXPECT_EQ(SUCCEED, HCPencode_header(buffer, COMP_MODEL_STDIO, &minfo,
                                      COMP_CODE_NBIT, &cinfo));
  EXPECT_EQ(SUCCEED, HCPdecode_header(buffer, &dec_model, &dec_minfo,
                                      &dec_coder, &dec_cinfo));
  EXPECT_EQ(COMP_MODEL_STDIO, dec_model);
  EXPECT_EQ(COMP_CODE_NBIT, dec_coder);
  EXPECT_EQ(DFNT_INT32, dec_cinfo.nbit.nt);
  EXPECT_EQ(1, dec_cinfo.nbit.sign_ext);
  EXPECT_EQ(0, dec_cinfo.nbit.fill_one);
  EXPECT_EQ(4, dec_cinfo.nbit.start_bit);
  EXPECT_EQ(24, dec_cinfo.nbit.bit_len);

  // 4. COMP_CODE_SKPHUFF
  cinfo.skphuff.skp_size = 4;
  std::memset(buffer, 0, sizeof(buffer));
  EXPECT_EQ(SUCCEED, HCPencode_header(buffer, COMP_MODEL_STDIO, &minfo,
                                      COMP_CODE_SKPHUFF, &cinfo));
  EXPECT_EQ(SUCCEED, HCPdecode_header(buffer, &dec_model, &dec_minfo,
                                      &dec_coder, &dec_cinfo));
  EXPECT_EQ(COMP_MODEL_STDIO, dec_model);
  EXPECT_EQ(COMP_CODE_SKPHUFF, dec_coder);
  EXPECT_EQ(4, dec_cinfo.skphuff.skp_size);

  // 5. COMP_CODE_DEFLATE
  cinfo.deflate_setting.level = 6;
  std::memset(buffer, 0, sizeof(buffer));
  EXPECT_EQ(SUCCEED, HCPencode_header(buffer, COMP_MODEL_STDIO, &minfo,
                                      COMP_CODE_DEFLATE, &cinfo));
  EXPECT_EQ(SUCCEED, HCPdecode_header(buffer, &dec_model, &dec_minfo,
                                      &dec_coder, &dec_cinfo));
  EXPECT_EQ(COMP_MODEL_STDIO, dec_model);
  EXPECT_EQ(COMP_CODE_DEFLATE, dec_coder);
  EXPECT_EQ(6, dec_cinfo.deflate_setting.level);

  // 6. COMP_CODE_SZIP
  cinfo.szip.pixels = 1000;
  cinfo.szip.pixels_per_scanline = 100;
  cinfo.szip.options_mask = 0x04;
  cinfo.szip.bits_per_pixel = 8;
  cinfo.szip.pixels_per_block = 16;
  std::memset(buffer, 0, sizeof(buffer));
  EXPECT_EQ(SUCCEED, HCPencode_header(buffer, COMP_MODEL_STDIO, &minfo,
                                      COMP_CODE_SZIP, &cinfo));
  EXPECT_EQ(SUCCEED, HCPdecode_header(buffer, &dec_model, &dec_minfo,
                                      &dec_coder, &dec_cinfo));
  EXPECT_EQ(COMP_MODEL_STDIO, dec_model);
  EXPECT_EQ(COMP_CODE_SZIP, dec_coder);
  EXPECT_EQ(1000, dec_cinfo.szip.pixels);
  EXPECT_EQ(100, dec_cinfo.szip.pixels_per_scanline);
  // options_mask has SZ_H4_REV_2 (0x00010000) masked in by HCPencode_header
  EXPECT_EQ(0x04 | 0x00010000, dec_cinfo.szip.options_mask);
  EXPECT_EQ(8, dec_cinfo.szip.bits_per_pixel);
  EXPECT_EQ(16, dec_cinfo.szip.pixels_per_block);

  // 7. COMP_CODE_IMCOMP decode falls back to default empty header
  {
    uint8_t dec_buf[16] = {};
    uint8_t* p = dec_buf;
    INT16ENCODE(p, COMP_MODEL_STDIO);
    INT16ENCODE(p, COMP_CODE_IMCOMP);
    comp_model_t m = COMP_MODEL_STDIO;
    comp_coder_t c = COMP_CODE_INVALID;
    EXPECT_EQ(SUCCEED, HCPdecode_header(dec_buf, &m, &minfo, &c, &cinfo));
    EXPECT_EQ(COMP_CODE_IMCOMP, c);
  }
}

// ----------------------------------------------------------------------------
// 3. HCcreate Argument Validation & Error Paths
// ----------------------------------------------------------------------------
TEST(HcompCreateTest, ArgumentAndPermissionValidation) {
  std::string path = GetTempFilePath("hcomp_create_errors.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info minfo = {};
  comp_info cinfo = {};
  cinfo.deflate_setting.level = 6;

  // 1. Invalid file_id
  HEclear();
  EXPECT_EQ(FAIL, HCcreate(FAIL, 1000, ref, COMP_MODEL_STDIO, &minfo,
                           COMP_CODE_DEFLATE, &cinfo));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  HEclear();

  // 2. Special tag passed as tag
  EXPECT_EQ(FAIL, HCcreate(fid, MKSPECIALTAG(1000), ref, COMP_MODEL_STDIO,
                           &minfo, COMP_CODE_DEFLATE, &cinfo));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  HEclear();

  EXPECT_EQ(FAIL, HCcreate(fid, 0x8000, ref, COMP_MODEL_STDIO, &minfo,
                           COMP_CODE_DEFLATE, &cinfo));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  HEclear();

  // 3. Bad coder types:
  // 3a. SKPHUFF with skp_size < 1
  cinfo.skphuff.skp_size = 0;
  EXPECT_EQ(FAIL, HCcreate(fid, 1000, ref, COMP_MODEL_STDIO, &minfo,
                           COMP_CODE_SKPHUFF, &cinfo));
  EXPECT_EQ(DFE_CINIT, HEvalue(1));
  EXPECT_EQ(DFE_BADCODER, HEvalue(2));
  HEclear();

  // 3b. DEFLATE with invalid level
  cinfo.deflate_setting.level = -1;
  EXPECT_EQ(FAIL, HCcreate(fid, 1000, ref, COMP_MODEL_STDIO, &minfo,
                           COMP_CODE_DEFLATE, &cinfo));
  EXPECT_EQ(DFE_CINIT, HEvalue(1));
  EXPECT_EQ(DFE_BADCODER, HEvalue(2));
  HEclear();

  // 3c. NBIT with invalid number type
  cinfo.nbit.nt = -1;
  EXPECT_EQ(FAIL, HCcreate(fid, 1000, ref, COMP_MODEL_STDIO, &minfo,
                           COMP_CODE_NBIT, &cinfo));
  EXPECT_EQ(DFE_CINIT, HEvalue(1));
  EXPECT_EQ(DFE_BADNUMTYPE, HEvalue(2));
  HEclear();

  // 3d. Invalid coder type code
  EXPECT_EQ(FAIL, HCcreate(fid, 1000, ref, COMP_MODEL_STDIO, &minfo,
                           static_cast<comp_coder_t>(999), &cinfo));
  EXPECT_EQ(DFE_CINIT, HEvalue(1));
  EXPECT_EQ(DFE_BADCODER, HEvalue(2));
  HEclear();

  // 4. Bad model type
  cinfo.deflate_setting.level = 6;
  EXPECT_EQ(FAIL, HCcreate(fid, 1000, ref, static_cast<comp_model_t>(999),
                           &minfo, COMP_CODE_DEFLATE, &cinfo));
  EXPECT_EQ(DFE_MINIT, HEvalue(1));
  EXPECT_EQ(DFE_BADMODEL, HEvalue(2));
  HEclear();
}

TEST(HcompCreateTest, SzipInitCoderCovered) {
  std::string path = GetTempFilePath("hcomp_szip_init.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info minfo = {};
  comp_info cinfo = {};
  cinfo.szip.pixels = 1000;
  cinfo.szip.bits_per_pixel = 8;
  cinfo.szip.pixels_per_block = 16;
  cinfo.szip.pixels_per_scanline = 100;
  cinfo.szip.options_mask = 0x04;

  // In google3 without SZIP encoder, stwrite in HCcreate fails. Failure
  // teardown ends the allocated DD atom via HTPendaccess, which clears the
  // error stack.
  HEclear();
  EXPECT_EQ(FAIL, HCcreate(fid, 1000, ref, COMP_MODEL_STDIO, &minfo,
                           COMP_CODE_SZIP, &cinfo));
  HEclear();
}

TEST(HcompCreateTest, ReadOnlyFileFailsWithDfeDenied) {
  std::string path = GetTempFilePath("hcomp_readonly.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  {
    int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    EXPECT_EQ(SUCCEED, Hclose(fid));
  }

  int32 ro_fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, ro_fid);
  absl::Cleanup close_ro = [ro_fid] { EXPECT_EQ(SUCCEED, Hclose(ro_fid)); };

  model_info minfo = {};
  comp_info cinfo = {};
  cinfo.deflate_setting.level = 6;

  HEclear();
  EXPECT_EQ(FAIL, HCcreate(ro_fid, 1000, 1, COMP_MODEL_STDIO, &minfo,
                           COMP_CODE_DEFLATE, &cinfo));
  EXPECT_EQ(DFE_DENIED, HEvalue(1));
  HEclear();
}

TEST(HcompCreateTest, CompressingExistingSpecialElementFailsWithDfeCantMod) {
  std::string path = GetTempFilePath("hcomp_already_special.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  constexpr uint16_t tag = 1000;
  uint16 ref = Hnewref(fid);
  ASSERT_NE(0, ref);

  model_info minfo = {};
  comp_info cinfo = {};
  cinfo.deflate_setting.level = 6;

  // Create compressed element
  int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &minfo,
                       COMP_CODE_DEFLATE, &cinfo);
  ASSERT_NE(FAIL, aid);
  EXPECT_EQ(SUCCEED, Hendaccess(aid));

  // Attempting to HCcreate again on the same tag/ref must fail with DFE_CANTMOD
  HEclear();
  EXPECT_EQ(FAIL, HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &minfo,
                           COMP_CODE_DEFLATE, &cinfo));
  EXPECT_EQ(DFE_CANTMOD, HEvalue(1));
  HEclear();
}

TEST(HcompCreateTest, CompressingExistingUncompressedElement) {
  std::string path = GetTempFilePath("hcomp_compress_existing.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  constexpr uint16_t tag = 1000;
  uint16 ref = 0;
  const std::string original_data =
      "Uncompressed initial data block for Hcomp existing element test 12345";

  {
    int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ref = Hnewref(fid);
    ASSERT_NE(0, ref);

    // Write a regular (uncompressed) element
    int32 aid = Hstartaccess(fid, tag, ref, DFACC_WRITE);
    ASSERT_NE(FAIL, aid);
    EXPECT_EQ(static_cast<int32_t>(original_data.size()),
              Hwrite(aid, original_data.size(), original_data.data()));
    EXPECT_EQ(SUCCEED, Hendaccess(aid));

    // Now call HCcreate on the existing uncompressed element
    model_info minfo = {};
    comp_info cinfo = {};
    cinfo.deflate_setting.level = 6;

    int32 comp_aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &minfo,
                              COMP_CODE_DEFLATE, &cinfo);
    ASSERT_NE(FAIL, comp_aid);
    absl::Cleanup close_comp = [comp_aid] {
      EXPECT_EQ(SUCCEED, Hendaccess(comp_aid));
    };

    // Verify uncompressed length matches original data
    int32 length = 0;
    EXPECT_EQ(SUCCEED, Hinquire(comp_aid, nullptr, nullptr, nullptr, &length,
                                nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ(static_cast<int32_t>(original_data.size()), length);

    // Read back data through the newly created compressed access handle
    std::vector<char> read_buf(original_data.size(), '\0');
    EXPECT_EQ(static_cast<int32_t>(original_data.size()),
              Hread(comp_aid, original_data.size(), read_buf.data()));
    EXPECT_EQ(original_data, std::string(read_buf.begin(), read_buf.end()));
  }

  // Re-open and verify persistent reading from disk
  {
    int32 fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    int32 aid = Hstartread(fid, tag, ref);
    ASSERT_NE(FAIL, aid);
    absl::Cleanup close_aid = [aid] { EXPECT_EQ(SUCCEED, Hendaccess(aid)); };

    std::vector<char> read_buf(original_data.size(), '\0');
    EXPECT_EQ(static_cast<int32_t>(original_data.size()),
              Hread(aid, original_data.size(), read_buf.data()));
    EXPECT_EQ(original_data, std::string(read_buf.begin(), read_buf.end()));
  }
}

// ----------------------------------------------------------------------------
// 4. HCPstread, HCPstwrite, HCPseek, HCPread, HCPwrite, HCPinquire, HCPinfo
// ----------------------------------------------------------------------------
TEST(HcompAccessTest, ReadWriteSeekInquireAndInfo) {
  std::string path = GetTempFilePath("hcomp_access_test.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  constexpr uint16_t tag = 1000;
  uint16 ref = 0;
  const std::string text = "ABCDEFGHIJ0123456789klmnopqrstuvwxyz!@#$%^&*()_+";

  {
    int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ref = Hnewref(fid);
    ASSERT_NE(0, ref);

    model_info minfo = {};
    comp_info cinfo = {};
    cinfo.deflate_setting.level = 6;

    int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &minfo,
                         COMP_CODE_DEFLATE, &cinfo);
    ASSERT_NE(FAIL, aid);
    absl::Cleanup close_aid = [aid] { EXPECT_EQ(SUCCEED, Hendaccess(aid)); };

    // Write initial half
    int32 half = text.size() / 2;
    EXPECT_EQ(half, Hwrite(aid, half, text.data()));

    // Write second half, triggering file extension in HCPwrite (lines
    // 1079-1094)
    EXPECT_EQ(static_cast<int32_t>(text.size() - half),
              Hwrite(aid, text.size() - half, text.data() + half));

    // Negative length in Hwrite
    HEclear();
    EXPECT_EQ(FAIL, Hwrite(aid, -1, text.data()));
    EXPECT_EQ(DFE_RANGE, HEvalue(1));
    HEclear();

    // HCPinquire / Hinquire
    int32 qfid = 0;
    uint16 qtag = 0, qref = 0;
    int32 qlen = 0, qoff = 0, qpos = 0;
    int16 qacc = 0, qspec = 0;
    EXPECT_EQ(SUCCEED, Hinquire(aid, &qfid, &qtag, &qref, &qlen, &qoff, &qpos,
                                &qacc, &qspec));
    EXPECT_EQ(fid, qfid);
    EXPECT_EQ(MKSPECIALTAG(tag), qtag);
    EXPECT_EQ(ref, qref);
    EXPECT_EQ(static_cast<int32_t>(text.size()), qlen);
    EXPECT_EQ(static_cast<int32_t>(text.size()), qpos);
    EXPECT_EQ(SPECIAL_COMP, qspec);
  }

  // Re-open for reading and seeking
  {
    int32 fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    int32 aid = Hstartread(fid, tag, ref);
    ASSERT_NE(FAIL, aid);
    absl::Cleanup close_aid = [aid] { EXPECT_EQ(SUCCEED, Hendaccess(aid)); };

    // HCPinfo test on readable element
    accrec_t* access_rec = static_cast<accrec_t*>(HAatom_object(aid));
    ASSERT_NE(nullptr, access_rec);
    sp_info_block_t info_block = {};
    EXPECT_EQ(SUCCEED, HCPinfo(access_rec, &info_block));
    EXPECT_EQ(SPECIAL_COMP, info_block.key);
    EXPECT_EQ(COMP_CODE_DEFLATE, info_block.comp_type);
    EXPECT_EQ(COMP_MODEL_STDIO, info_block.model_type);
    EXPECT_GT(info_block.comp_size, 0);

    // HCPinfo on non-special element fails with DFE_INTERNAL
    access_rec->special = 0;
    HEclear();
    EXPECT_EQ(FAIL, HCPinfo(access_rec, &info_block));
    EXPECT_EQ(DFE_INTERNAL, HEvalue(1));
    access_rec->special = SPECIAL_COMP;
    HEclear();

    // Seeking: DF_START, DF_CURRENT, DF_END
    EXPECT_EQ(SUCCEED, Hseek(aid, 10, DF_START));
    EXPECT_EQ(10, Htell(aid));

    EXPECT_EQ(SUCCEED, Hseek(aid, 5, DF_CURRENT));
    EXPECT_EQ(15, Htell(aid));

    EXPECT_EQ(SUCCEED, Hseek(aid, -5, DF_END));
    EXPECT_EQ(static_cast<int32_t>(text.size() - 5), Htell(aid));

    // Seek out of range (negative target offset)
    HEclear();
    EXPECT_EQ(FAIL, Hseek(aid, -1, DF_START));
    EXPECT_EQ(DFE_RANGE, HEvalue(1));
    HEclear();

    // Seek back to start and read full buffer
    EXPECT_EQ(SUCCEED, Hseek(aid, 0, DF_START));

    // Reading with length = 0 reads to end of element
    std::vector<char> full_buf(text.size(), '\0');
    EXPECT_EQ(static_cast<int32_t>(text.size()),
              Hread(aid, 0, full_buf.data()));
    EXPECT_EQ(text, std::string(full_buf.begin(), full_buf.end()));

    // Reading past EOF fails with DFE_RANGE
    HEclear();
    char dummy = 0;
    EXPECT_EQ(FAIL, Hread(aid, 1, &dummy));
    EXPECT_EQ(DFE_RANGE, HEvalue(1));
    HEclear();

    // Negative read length
    HEclear();
    EXPECT_EQ(FAIL, Hread(aid, -1, &dummy));
    EXPECT_EQ(DFE_RANGE, HEvalue(1));
    HEclear();
  }

  // HCPinfo fails on non-compressed element
  {
    accrec_t dummy_rec = {};
    dummy_rec.special = SPECIAL_LINKED;
    sp_info_block_t info_block = {};
    HEclear();
    EXPECT_EQ(FAIL, HCPinfo(&dummy_rec, &info_block));
    EXPECT_EQ(DFE_INTERNAL, HEvalue(1));
    HEclear();
  }
}

TEST(HcompAccessTest, StartWriteAccessOnExistingCompressedElement) {
  std::string path = GetTempFilePath("hcomp_stwrite.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  constexpr uint16_t tag = 1000;
  uint16 ref = 0;
  const std::string text1 = "Initial payload";
  const std::string text2 = "Appended payload";

  {
    int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ref = Hnewref(fid);
    ASSERT_NE(0, ref);

    model_info minfo = {};
    comp_info cinfo = {};
    cinfo.deflate_setting.level = 6;

    int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &minfo,
                         COMP_CODE_DEFLATE, &cinfo);
    ASSERT_NE(FAIL, aid);
    EXPECT_EQ(static_cast<int32_t>(text1.size()),
              Hwrite(aid, text1.size(), text1.data()));
    EXPECT_EQ(SUCCEED, Hendaccess(aid));
  }

  // Open with DFACC_RDWR and start write access using HCPstwrite
  {
    int32 fid = Hopen(path.c_str(), DFACC_RDWR, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    int32 aid = Hstartaccess(fid, tag, ref, DFACC_WRITE);
    ASSERT_NE(FAIL, aid);
    absl::Cleanup close_aid = [aid] { EXPECT_EQ(SUCCEED, Hendaccess(aid)); };

    // Seek to 0 and overwrite
    EXPECT_EQ(SUCCEED, Hseek(aid, 0, DF_START));
    EXPECT_EQ(static_cast<int32_t>(text2.size()),
              Hwrite(aid, text2.size(), text2.data()));
  }
}

TEST(HcompAccessTest, EndaccessValidation) {
  HEclear();
  EXPECT_EQ(FAIL, HCPendaccess(nullptr));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  HEclear();

  accrec_t* rec = HIget_access_rec();
  ASSERT_NE(nullptr, rec);
  rec->file_id = FAIL;
  EXPECT_EQ(FAIL, HCPendaccess(rec));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  HEclear();
}

TEST(HcompAccessTest, StreadAndStwriteValidation) {
  accrec_t rec = {};
  rec.file_id = FAIL;

  HEclear();
  EXPECT_EQ(FAIL, HCPstread(&rec));
  EXPECT_EQ(DFE_DENIED, HEvalue(1));
  EXPECT_EQ(DFE_ARGS, HEvalue(2));
  HEclear();

  EXPECT_EQ(FAIL, HCPstwrite(&rec));
  EXPECT_EQ(DFE_DENIED, HEvalue(1));
  EXPECT_EQ(DFE_ARGS, HEvalue(2));
  HEclear();
}

TEST(HcompAccessTest, LargeWriteExtendsStartBlockAndBoundaryErrors) {
  std::string path = GetTempFilePath("hcomp_large_write.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  constexpr uint16_t tag = 1234;
  uint16 ref = 0;
  const std::string large_data(5000, 'Z');

  {
    int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ref = Hnewref(fid);
    ASSERT_NE(0, ref);

    model_info minfo = {};
    comp_info cinfo = {};
    cinfo.deflate_setting.level = 6;

    int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &minfo,
                         COMP_CODE_DEFLATE, &cinfo);
    ASSERT_NE(FAIL, aid);

    // Negative length write fails with DFE_RANGE
    HEclear();
    EXPECT_EQ(FAIL, Hwrite(aid, -1, large_data.data()));
    EXPECT_EQ(DFE_RANGE, HEvalue(1));
    HEclear();

    // Large write > 4096 (COMP_START_BLOCK) extends uncompressed length record
    EXPECT_EQ(static_cast<int32_t>(large_data.size()),
              Hwrite(aid, large_data.size(), large_data.data()));

    EXPECT_EQ(SUCCEED, Hendaccess(aid));
  }

  // Read back in read-only mode and test read/seek error paths
  {
    int32 ro_fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_NE(FAIL, ro_fid);
    absl::Cleanup close_ro = [ro_fid] { EXPECT_EQ(SUCCEED, Hclose(ro_fid)); };

    // Attempting write access on read-only file fails with DFE_DENIED
    HEclear();
    EXPECT_EQ(FAIL, Hstartaccess(ro_fid, tag, ref, DFACC_WRITE));
    EXPECT_EQ(DFE_DENIED, HEvalue(1));
    HEclear();

    int32 aid = Hstartread(ro_fid, tag, ref);
    ASSERT_NE(FAIL, aid);
    absl::Cleanup close_aid = [aid] { EXPECT_EQ(SUCCEED, Hendaccess(aid)); };

    // Negative length read fails with DFE_RANGE
    char buf[10];
    HEclear();
    EXPECT_EQ(FAIL, Hread(aid, -1, buf));
    EXPECT_EQ(DFE_RANGE, HEvalue(1));
    HEclear();

    // Negative seek offset fails with DFE_RANGE
    EXPECT_EQ(FAIL, Hseek(aid, -1, DF_START));
    EXPECT_EQ(DFE_RANGE, HEvalue(1));
    HEclear();

    // Read full content
    std::string read_back(large_data.size(), '\0');
    EXPECT_EQ(static_cast<int32_t>(large_data.size()),
              Hread(aid, large_data.size(), read_back.data()));
    EXPECT_EQ(large_data, read_back);

    // Reading past EOF fails with DFE_RANGE
    EXPECT_EQ(FAIL, Hread(aid, 10, buf));
    EXPECT_EQ(DFE_RANGE, HEvalue(1));
    HEclear();
  }
}

// ----------------------------------------------------------------------------
// 5. HCPgetcompinfo, HCPgetcomptype, HCPgetdatasize tests
// ----------------------------------------------------------------------------
TEST(HcompQueryTest, GetcompinfoArgumentValidation) {
  std::string path = GetTempFilePath("hcomp_query_argval.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  comp_coder_t coder_type;
  comp_info cinfo;

  // Null outputs fail with DFE_ARGS
  HEclear();
  EXPECT_EQ(FAIL, HCPgetcompinfo(fid, 1000, 1, nullptr, &cinfo));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  HEclear();

  EXPECT_EQ(FAIL, HCPgetcompinfo(fid, 1000, 1, &coder_type, nullptr));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  HEclear();

  // Missing element fails with DFE_ARGS and ends invalid AID
  // (DFE_CANTENDACCESS)
  EXPECT_EQ(FAIL, HCPgetcompinfo(fid, 9999, 9999, &coder_type, &cinfo));
  EXPECT_EQ(DFE_CANTENDACCESS, HEvalue(1));
  EXPECT_EQ(DFE_ARGS, HEvalue(2));
  HEclear();
}
TEST(HcompQueryTest, QueriesOnMissingAndNonCompressedElements) {
  std::string path = GetTempFilePath("hcomp_query_tests.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  constexpr uint16_t regular_tag = 1001;
  uint16 regular_ref = Hnewref(fid);
  ASSERT_NE(0, regular_ref);

  const std::string payload = "Non-compressed test payload 123456789";
  int32 aid = Hstartaccess(fid, regular_tag, regular_ref, DFACC_WRITE);
  ASSERT_NE(FAIL, aid);
  EXPECT_EQ(static_cast<int32_t>(payload.size()),
            Hwrite(aid, payload.size(), payload.data()));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));

  // 1. Argument validation: Null pointers
  comp_coder_t coder_type = COMP_CODE_INVALID;
  comp_info cinfo = {};
  int32 comp_size = 0, orig_size = 0;

  HEclear();
  EXPECT_EQ(FAIL,
            HCPgetcompinfo(fid, regular_tag, regular_ref, nullptr, &cinfo));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  HEclear();

  EXPECT_EQ(FAIL, HCPgetcompinfo(fid, regular_tag, regular_ref, &coder_type,
                                 nullptr));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  HEclear();

  // Invalid fid
  EXPECT_EQ(FAIL, HCPgetcomptype(FAIL, regular_tag, regular_ref, &coder_type));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  HEclear();

  EXPECT_EQ(FAIL, HCPgetdatasize(FAIL, regular_tag, regular_ref, &comp_size,
                                 &orig_size));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  HEclear();

  // Missing element: tag/ref not in file
  coder_type = COMP_CODE_INVALID;
  EXPECT_EQ(SUCCEED, HCPgetcomptype(fid, 9999, 9999, &coder_type));
  EXPECT_EQ(COMP_CODE_NONE, coder_type);

  HEclear();
  EXPECT_EQ(FAIL, HCPgetdatasize(fid, 9999, 9999, &comp_size, &orig_size));
  EXPECT_EQ(DFE_CANTACCESS, HEvalue(1));
  HEclear();

  // Regular (uncompressed) element queries
  coder_type = COMP_CODE_INVALID;
  EXPECT_EQ(SUCCEED,
            HCPgetcompinfo(fid, regular_tag, regular_ref, &coder_type, &cinfo));
  EXPECT_EQ(COMP_CODE_NONE, coder_type);

  coder_type = COMP_CODE_INVALID;
  EXPECT_EQ(SUCCEED,
            HCPgetcomptype(fid, regular_tag, regular_ref, &coder_type));
  EXPECT_EQ(COMP_CODE_NONE, coder_type);

  comp_size = orig_size = 0;
  EXPECT_EQ(SUCCEED, HCPgetdatasize(fid, regular_tag, regular_ref, &comp_size,
                                    &orig_size));
  EXPECT_EQ(static_cast<int32_t>(payload.size()), orig_size);
  EXPECT_EQ(static_cast<int32_t>(payload.size()), comp_size);
}

TEST(HcompQueryTest, QueriesOnCompressedElementsWithAndWithoutData) {
  std::string path = GetTempFilePath("hcomp_query_compressed.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  constexpr uint16_t tag_empty = 2001;
  constexpr uint16_t tag_data = 2002;
  uint16 ref_empty = 0;
  uint16 ref_data = 0;
  const std::string text =
      "Compressed payload to check HCPgetdatasize and getcompinfo";

  {
    int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ref_empty = Hnewref(fid);
    ref_data = Hnewref(fid);
    ASSERT_NE(0, ref_empty);
    ASSERT_NE(0, ref_data);

    model_info minfo = {};
    comp_info cinfo = {};
    cinfo.deflate_setting.level = 7;

    // 1. Compressed element without data written
    int32 aid_empty = HCcreate(fid, tag_empty, ref_empty, COMP_MODEL_STDIO,
                               &minfo, COMP_CODE_DEFLATE, &cinfo);
    ASSERT_NE(FAIL, aid_empty);
    EXPECT_EQ(SUCCEED, Hendaccess(aid_empty));

    // 2. Compressed element with data written
    int32 aid_data = HCcreate(fid, tag_data, ref_data, COMP_MODEL_STDIO, &minfo,
                              COMP_CODE_DEFLATE, &cinfo);
    ASSERT_NE(FAIL, aid_data);
    EXPECT_EQ(static_cast<int32_t>(text.size()),
              Hwrite(aid_data, text.size(), text.data()));
    EXPECT_EQ(SUCCEED, Hendaccess(aid_data));
  }

  // Re-open for read and test query functions
  {
    int32 fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    // Empty compressed element
    comp_coder_t coder_type = COMP_CODE_INVALID;
    comp_info retrieved_info = {};
    EXPECT_EQ(SUCCEED, HCPgetcompinfo(fid, tag_empty, ref_empty, &coder_type,
                                      &retrieved_info));
    EXPECT_EQ(COMP_CODE_DEFLATE, coder_type);
    EXPECT_EQ(7, retrieved_info.deflate_setting.level);

    coder_type = COMP_CODE_INVALID;
    EXPECT_EQ(SUCCEED, HCPgetcomptype(fid, tag_empty, ref_empty, &coder_type));
    EXPECT_EQ(COMP_CODE_DEFLATE, coder_type);

    int32 comp_size = -1, orig_size = -1;
    EXPECT_EQ(SUCCEED, HCPgetdatasize(fid, tag_empty, ref_empty, &comp_size,
                                      &orig_size));
    EXPECT_EQ(0, orig_size);
    EXPECT_EQ(0, comp_size);

    // Compressed element with data
    coder_type = COMP_CODE_INVALID;
    std::memset(&retrieved_info, 0, sizeof(retrieved_info));
    EXPECT_EQ(SUCCEED, HCPgetcompinfo(fid, tag_data, ref_data, &coder_type,
                                      &retrieved_info));
    EXPECT_EQ(COMP_CODE_DEFLATE, coder_type);
    EXPECT_EQ(7, retrieved_info.deflate_setting.level);

    coder_type = COMP_CODE_INVALID;
    EXPECT_EQ(SUCCEED, HCPgetcomptype(fid, tag_data, ref_data, &coder_type));
    EXPECT_EQ(COMP_CODE_DEFLATE, coder_type);

    comp_size = orig_size = -1;
    EXPECT_EQ(SUCCEED,
              HCPgetdatasize(fid, tag_data, ref_data, &comp_size, &orig_size));
    EXPECT_EQ(static_cast<int32_t>(text.size()), orig_size);
    EXPECT_GT(comp_size, 0);
  }
}

TEST(HcompQueryTest, QueriesOnLinkedBlockElements) {
  std::string path = GetTempFilePath("hcomp_linked_query.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  constexpr uint16_t tag = 3001;
  uint16 ref = 0;
  const std::string text =
      "Linked block payload for HCPgetdatasize and getcomptype";

  {
    int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ref = Hnewref(fid);
    ASSERT_NE(0, ref);

    int32 aid = HLcreate(fid, tag, ref, 16, 16);
    ASSERT_NE(FAIL, aid);
    EXPECT_EQ(static_cast<int32_t>(text.size()),
              Hwrite(aid, text.size(), text.data()));
    EXPECT_EQ(SUCCEED, Hendaccess(aid));
  }

  {
    int32 fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    comp_coder_t coder_type = COMP_CODE_INVALID;
    comp_info cinfo = {};
    EXPECT_EQ(SUCCEED, HCPgetcompinfo(fid, tag, ref, &coder_type, &cinfo));
    EXPECT_EQ(COMP_CODE_NONE, coder_type);

    coder_type = COMP_CODE_INVALID;
    EXPECT_EQ(SUCCEED, HCPgetcomptype(fid, tag, ref, &coder_type));
    EXPECT_EQ(COMP_CODE_NONE, coder_type);

    int32 comp_size = 0, orig_size = 0;
    EXPECT_EQ(SUCCEED, HCPgetdatasize(fid, tag, ref, &comp_size, &orig_size));
    EXPECT_EQ(static_cast<int32_t>(text.size()), orig_size);
    EXPECT_EQ(static_cast<int32_t>(text.size()), comp_size);
  }
}

TEST(HcompQueryTest, QueriesOnChunkedElements) {
  std::string path = GetTempFilePath("hcomp_chunked_query.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  constexpr uint16_t tag = 5001;
  uint16 ref = 0;
  const std::vector<int32_t> data(20, 42);

  {
    int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ref = Hnewref(fid);
    ASSERT_NE(0, ref);

    DIM_DEF dim = {.dim_length = 20, .chunk_length = 10, .distrib_type = 0};
    comp_info cinfo = {};
    cinfo.deflate_setting.level = 6;
    model_info minfo = {};
    HCHUNK_DEF chk = {
        .chunk_size = 10 * static_cast<int32_t>(sizeof(int32_t)),
        .nt_size = static_cast<int32_t>(sizeof(int32_t)),
        .num_dims = 1,
        .pdims = &dim,
        .chunk_flag = SPECIAL_COMP,
        .comp_type = COMP_CODE_DEFLATE,
        .model_type = COMP_MODEL_STDIO,
        .cinfo = &cinfo,
        .minfo = &minfo,
    };

    int32_t fill_val = 0;
    int32 aid = HMCcreate(fid, tag, ref, 1, sizeof(int32_t), &fill_val, &chk);
    ASSERT_NE(FAIL, aid);
    int32 bytes = data.size() * sizeof(int32_t);
    EXPECT_EQ(bytes, Hwrite(aid, bytes, data.data()));
    EXPECT_EQ(SUCCEED, Hendaccess(aid));
  }

  {
    int32 fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    comp_coder_t coder = COMP_CODE_INVALID;
    comp_info retrieved_info = {};
    EXPECT_EQ(SUCCEED, HCPgetcompinfo(fid, tag, ref, &coder, &retrieved_info));
    EXPECT_EQ(COMP_CODE_DEFLATE, coder);

    coder = COMP_CODE_INVALID;
    EXPECT_EQ(SUCCEED, HCPgetcomptype(fid, tag, ref, &coder));
    EXPECT_EQ(COMP_CODE_DEFLATE, coder);

    int32 comp_sz = 0, orig_sz = 0;
    EXPECT_EQ(SUCCEED, HCPgetdatasize(fid, tag, ref, &comp_sz, &orig_sz));
    EXPECT_EQ(320, orig_sz);
    EXPECT_GT(comp_sz, 0);
  }
}

TEST(HcompQueryTest, InvalidSpecialTagInGetcompinfoAndGetcomptype) {
  std::string path = GetTempFilePath("hcomp_invalid_special.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  constexpr uint16_t tag = 6001;
  uint16 ref = 0;

  {
    int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ref = Hnewref(fid);
    ASSERT_NE(0, ref);

    // Create a special element with an unrecognized special tag (999) in its
    // description record
    uint16 special_tag = MKSPECIALTAG(tag);
    int32 aid = Hstartaccess(fid, special_tag, ref, DFACC_WRITE);
    ASSERT_NE(FAIL, aid);

    uint8_t header[16] = {};
    uint8_t* p = header;
    INT16ENCODE(p, 999);  // invalid special code (triggers lines 1437-1439)
    EXPECT_EQ(16, Hwrite(aid, 16, header));
    EXPECT_EQ(SUCCEED, Hendaccess(aid));
  }

  {
    int32 fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    comp_coder_t coder = COMP_CODE_NONE;
    EXPECT_EQ(FAIL, HCPgetcomptype(fid, tag, ref, &coder));
    EXPECT_EQ(COMP_CODE_INVALID, coder);
  }
}

TEST(HcompQueryTest, SpecialExtAndSpecialComprasQueries) {
  std::string path = GetTempFilePath("hcomp_special_ext_query.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  constexpr uint16_t tag_ext = 7001;
  constexpr uint16_t tag_compras = 7002;
  uint16 ref_ext = 0;
  uint16 ref_compras = 0;

  {
    int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ref_ext = Hnewref(fid);
    ref_compras = Hnewref(fid);
    ASSERT_NE(0, ref_ext);
    ASSERT_NE(0, ref_compras);

    // Write SPECIAL_EXT header: 2 bytes special code + 4 bytes length
    int32 aid_ext =
        Hstartaccess(fid, MKSPECIALTAG(tag_ext), ref_ext, DFACC_WRITE);
    ASSERT_NE(FAIL, aid_ext);
    uint8_t ext_hdr[16] = {};
    uint8_t* p = ext_hdr;
    INT16ENCODE(p, SPECIAL_EXT);
    INT32ENCODE(p, 12345);
    EXPECT_EQ(16, Hwrite(aid_ext, 16, ext_hdr));
    EXPECT_EQ(SUCCEED, Hendaccess(aid_ext));

    // Write SPECIAL_COMPRAS header
    int32 aid_cp =
        Hstartaccess(fid, MKSPECIALTAG(tag_compras), ref_compras, DFACC_WRITE);
    ASSERT_NE(FAIL, aid_cp);
    uint8_t cp_hdr[16] = {};
    p = cp_hdr;
    INT16ENCODE(p, SPECIAL_COMPRAS);
    EXPECT_EQ(16, Hwrite(aid_cp, 16, cp_hdr));
    EXPECT_EQ(SUCCEED, Hendaccess(aid_cp));
  }

  {
    int32 fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    comp_coder_t coder = COMP_CODE_INVALID;
    EXPECT_EQ(SUCCEED, HCPgetcomptype(fid, tag_ext, ref_ext, &coder));
    EXPECT_EQ(COMP_CODE_NONE, coder);

    coder = COMP_CODE_INVALID;
    EXPECT_EQ(SUCCEED, HCPgetcomptype(fid, tag_compras, ref_compras, &coder));
    EXPECT_EQ(COMP_CODE_NONE, coder);

    int32 comp_sz = 0, orig_sz = 0;
    EXPECT_EQ(SUCCEED,
              HCPgetdatasize(fid, tag_ext, ref_ext, &comp_sz, &orig_sz));
    EXPECT_EQ(12345, orig_sz);
    EXPECT_EQ(12345, comp_sz);
  }
}

TEST(HcompQueryTest, SpecialHeaderMissingCompRefFailsWithDfeBadlen) {
  std::string path = GetTempFilePath("hcomp_missing_compref.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  constexpr uint16_t tag = 7013;
  constexpr uint16_t tag_0byte = 7014;
  uint16 ref = 0;
  uint16 ref_0byte = 0;

  {
    int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ref = Hnewref(fid);
    ASSERT_NE(0, ref);
    ref_0byte = Hnewref(fid);
    ASSERT_NE(0, ref_0byte);

    // SPECIAL_COMP header with non-zero len but missing DFTAG_COMPRESSED ref
    // (triggers line 1556 in HCPgetdatasize)
    int32 aid = Hstartaccess(fid, MKSPECIALTAG(tag), ref, DFACC_WRITE);
    ASSERT_NE(FAIL, aid);
    uint8_t miss_hdr[16] = {};
    uint8_t* p = miss_hdr;
    INT16ENCODE(p, SPECIAL_COMP);
    UINT16ENCODE(p, 1);     // version
    INT32ENCODE(p, 100);    // uncompressed length > 0
    UINT16ENCODE(p, 9999);  // comp_ref that does NOT exist
    EXPECT_EQ(16, Hwrite(aid, 16, miss_hdr));
    EXPECT_EQ(SUCCEED, Hendaccess(aid));

    // Special header with 0 bytes length (triggers line 1534 in HCPgetdatasize)
    int32 aid_0 =
        Hstartaccess(fid, MKSPECIALTAG(tag_0byte), ref_0byte, DFACC_WRITE);
    ASSERT_NE(FAIL, aid_0);
    EXPECT_EQ(SUCCEED, Hendaccess(aid_0));
  }

  {
    int32 fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    // Missing comp_ref fails in HCPgetdatasize; HTPendaccess in teardown clears
    // error stack.
    int32 comp_sz = 0, orig_sz = 0;
    HEclear();
    EXPECT_EQ(FAIL, HCPgetdatasize(fid, tag, ref, &comp_sz, &orig_sz));
    HEclear();

    // 0-byte special description record fails in HCPgetdatasize with
    // DFE_INTERNAL
    EXPECT_EQ(FAIL,
              HCPgetdatasize(fid, tag_0byte, ref_0byte, &comp_sz, &orig_sz));
    HEclear();
  }
}

// ----------------------------------------------------------------------------
// 6. Round-trip compression using all other supported coders via HCcreate
// ----------------------------------------------------------------------------
TEST(HcompCreateTest, RoundTripRleEncoding) {
  std::string path = GetTempFilePath("hcomp_rle.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  constexpr uint16_t tag = 4001;
  uint16 ref = 0;
  const std::string text = "AAAAABBBBBCCCCCDDDDDEEEEEFFFFFGGGGG";

  {
    int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ref = Hnewref(fid);
    ASSERT_NE(0, ref);

    model_info minfo = {};
    comp_info cinfo = {};

    int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &minfo, COMP_CODE_RLE,
                         &cinfo);
    ASSERT_NE(FAIL, aid);
    absl::Cleanup close_aid = [aid] { EXPECT_EQ(SUCCEED, Hendaccess(aid)); };

    EXPECT_EQ(static_cast<int32_t>(text.size()),
              Hwrite(aid, text.size(), text.data()));
  }

  {
    int32 fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    int32 aid = Hstartread(fid, tag, ref);
    ASSERT_NE(FAIL, aid);
    absl::Cleanup close_aid = [aid] { EXPECT_EQ(SUCCEED, Hendaccess(aid)); };

    std::vector<char> buf(text.size(), '\0');
    EXPECT_EQ(static_cast<int32_t>(text.size()),
              Hread(aid, text.size(), buf.data()));
    EXPECT_EQ(text, std::string(buf.begin(), buf.end()));
  }
}

TEST(HcompCreateTest, RoundTripNoneEncoding) {
  std::string path = GetTempFilePath("hcomp_none.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  constexpr uint16_t tag = 4002;
  uint16 ref = 0;
  const std::string text = "COMP_CODE_NONE payload through special comp layer";

  {
    int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ref = Hnewref(fid);
    ASSERT_NE(0, ref);

    model_info minfo = {};
    comp_info cinfo = {};

    int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &minfo,
                         COMP_CODE_NONE, &cinfo);
    ASSERT_NE(FAIL, aid);
    absl::Cleanup close_aid = [aid] { EXPECT_EQ(SUCCEED, Hendaccess(aid)); };

    EXPECT_EQ(static_cast<int32_t>(text.size()),
              Hwrite(aid, text.size(), text.data()));
  }

  {
    int32 fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    int32 aid = Hstartread(fid, tag, ref);
    ASSERT_NE(FAIL, aid);
    absl::Cleanup close_aid = [aid] { EXPECT_EQ(SUCCEED, Hendaccess(aid)); };

    std::vector<char> buf(text.size(), '\0');
    EXPECT_EQ(static_cast<int32_t>(text.size()),
              Hread(aid, text.size(), buf.data()));
    EXPECT_EQ(text, std::string(buf.begin(), buf.end()));
  }
}

TEST(HcompCreateTest, RoundTripSkphuffEncoding) {
  std::string path = GetTempFilePath("hcomp_skphuff.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  constexpr uint16_t tag = 4003;
  uint16 ref = 0;
  const std::string text = "Skphuff payload 123456789012345678901234567890";

  {
    int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ref = Hnewref(fid);
    ASSERT_NE(0, ref);

    model_info minfo = {};
    comp_info cinfo = {};
    cinfo.skphuff.skp_size = 1;

    int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &minfo,
                         COMP_CODE_SKPHUFF, &cinfo);
    ASSERT_NE(FAIL, aid);
    absl::Cleanup close_aid = [aid] { EXPECT_EQ(SUCCEED, Hendaccess(aid)); };

    EXPECT_EQ(static_cast<int32_t>(text.size()),
              Hwrite(aid, text.size(), text.data()));
  }

  {
    int32 fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    int32 aid = Hstartread(fid, tag, ref);
    ASSERT_NE(FAIL, aid);
    absl::Cleanup close_aid = [aid] { EXPECT_EQ(SUCCEED, Hendaccess(aid)); };

    std::vector<char> buf(text.size(), '\0');
    EXPECT_EQ(static_cast<int32_t>(text.size()),
              Hread(aid, text.size(), buf.data()));
    EXPECT_EQ(text, std::string(buf.begin(), buf.end()));
  }
}

TEST(HcompCreateTest, RoundTripNbitEncoding) {
  std::string path = GetTempFilePath("hcomp_nbit.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  constexpr uint16_t tag = 4004;
  uint16 ref = 0;
  const std::vector<uint8_t> data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  {
    int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    ref = Hnewref(fid);
    ASSERT_NE(0, ref);

    model_info minfo = {};
    comp_info cinfo = {};
    cinfo.nbit.nt = DFNT_UINT8;
    cinfo.nbit.sign_ext = 0;
    cinfo.nbit.fill_one = 0;
    cinfo.nbit.start_bit = 7;
    cinfo.nbit.bit_len = 8;

    int32 aid = HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &minfo,
                         COMP_CODE_NBIT, &cinfo);
    ASSERT_NE(FAIL, aid);
    absl::Cleanup close_aid = [aid] { EXPECT_EQ(SUCCEED, Hendaccess(aid)); };

    int32 byte_count = data.size();
    EXPECT_EQ(byte_count, Hwrite(aid, byte_count, data.data()));
  }

  {
    int32 fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    int32 aid = Hstartread(fid, tag, ref);
    ASSERT_NE(FAIL, aid);
    absl::Cleanup close_aid = [aid] { EXPECT_EQ(SUCCEED, Hendaccess(aid)); };

    std::vector<uint8_t> read_data(data.size(), 0);
    int32 byte_count = data.size();
    EXPECT_EQ(byte_count, Hread(aid, byte_count, read_data.data()));
    EXPECT_EQ(data, read_data);
  }
}

}  // namespace
}  // namespace hdf4
