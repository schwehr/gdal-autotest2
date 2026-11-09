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
// Tests filter pipeline object header message encoding, decoding, validation,
// copying, sizing, resetting, freeing, pre-copy file version bounds checks,
// debug output, and dataset filter integration
// (third_party/hdf5/src/H5Opline.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Opline.c

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from cache/object
// header structs pulled in via H5Opkg.h. Define NDEBUG around internal headers
// so struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5OPLINE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5O_FRIEND
#define H5Z_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#include "third_party/hdf5/src/H5Zpkg.h"
}  // extern "C"
#ifdef H5OPLINE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5OPLINE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

void AppendUint16LE(std::vector<uint8_t>& buf, uint16_t val) {
  buf.push_back(static_cast<uint8_t>(val & 0xffU));
  buf.push_back(static_cast<uint8_t>((val >> 8) & 0xffU));
}

void AppendUint32LE(std::vector<uint8_t>& buf, uint32_t val) {
  for (int i = 0; i < 4; ++i) {
    buf.push_back(static_cast<uint8_t>((val >> (8 * i)) & 0xffU));
  }
}

std::string ReadTmpFile(FILE* fp) {
  std::fflush(fp);
  std::rewind(fp);
  std::string out;
  char chunk[256];
  while (true) {
    const size_t n = std::fread(chunk, 1, sizeof(chunk), fp);
    if (n == 0) {
      break;
    }
    out.append(chunk, n);
  }
  return out;
}

ScopedFile CreateFileWithBounds(const char* name, H5F_libver_t low,
                                H5F_libver_t high) {
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) {
    return ScopedFile();
  }
  if (H5Pset_libver_bounds(fapl.get(), low, high) < 0) {
    return ScopedFile();
  }
  return ScopedFile(H5Fcreate(name, H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
}

size_t DummyFilterCallback(unsigned /*flags*/, size_t /*cd_nelmts*/,
                           const unsigned /*cd_values*/[], size_t nbytes,
                           size_t* buf_size, void** buf) {
  (void)buf_size;
  (void)buf;
  return nbytes;
}

TEST(H5OplineTest, DescriptorAndVersionBoundsMatchSpec) {
  ScopedFile file = CreateInMemoryFile("pline_desc.h5");
  ASSERT_TRUE(file.valid());

  const H5O_msg_class_t& msg = H5O_MSG_PLINE[0];
  EXPECT_EQ(msg.id, static_cast<unsigned>(H5O_PLINE_ID));
  EXPECT_STREQ(msg.name, "filter pipeline");
  EXPECT_EQ(msg.native_size, sizeof(H5O_pline_t));
  EXPECT_EQ(msg.share_flags,
            static_cast<unsigned>(H5O_SHARE_IS_SHARABLE | H5O_SHARE_IN_OHDR));
  EXPECT_EQ(H5O_msg_class_g[H5O_PLINE_ID], H5O_MSG_PLINE);

  EXPECT_EQ(H5O_pline_ver_bounds[H5F_LIBVER_EARLIEST],
            static_cast<unsigned>(H5O_PLINE_VERSION_1));
  EXPECT_EQ(H5O_pline_ver_bounds[H5F_LIBVER_V18],
            static_cast<unsigned>(H5O_PLINE_VERSION_2));
  EXPECT_EQ(H5O_pline_ver_bounds[H5F_LIBVER_LATEST],
            static_cast<unsigned>(H5O_PLINE_VERSION_LATEST));
}

TEST(H5OplineTest, EncodeDecodeSizeCopyResetAndDebugVersions) {
  ScopedFile file = CreateInMemoryFile("pline_codec.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  // Version 1 pipeline with:
  //   0: internal filter (DEFLATE) with null name (resolved via H5Z_find),
  //      odd cd_nelmts = 1 (exercises v1 4-byte padding & H5Z_COMMON_NAME_LEN)
  //   1: unregistered custom filter (id=900) with null name & odd cd_nelmts=5
  //      (> H5Z_COMMON_CD_VALUES -> heap cd_values)
  //   2: custom filter with long name (> H5Z_COMMON_NAME_LEN -> heap name) and
  //      even cd_nelmts = 2
  //   3: custom filter with short name (<= H5Z_COMMON_NAME_LEN -> inline _name)
  //      and cd_nelmts = 0
  H5O_pline_t v1{};
  v1.version = H5O_PLINE_VERSION_1;
  v1.nused = 4;
  v1.nalloc = 4;
  v1.filter = static_cast<H5Z_filter_info_t*>(
      H5MM_calloc(v1.nalloc * sizeof(H5Z_filter_info_t)));
  ASSERT_NE(v1.filter, nullptr);
  absl::Cleanup reset_v1 = [&v1] { H5O_msg_reset(H5O_PLINE_ID, &v1); };

  v1.filter[0].id = H5Z_FILTER_DEFLATE;
  v1.filter[0].flags = H5Z_FLAG_OPTIONAL;
  v1.filter[0].name = nullptr;
  v1.filter[0].cd_nelmts = 1;
  v1.filter[0].cd_values = v1.filter[0]._cd_values;
  v1.filter[0].cd_values[0] = 6;

  v1.filter[1].id = 900;
  v1.filter[1].flags = 0;
  v1.filter[1].name = nullptr;
  v1.filter[1].cd_nelmts = 5;
  v1.filter[1].cd_values =
      static_cast<unsigned*>(H5MM_malloc(5 * sizeof(unsigned)));
  ASSERT_NE(v1.filter[1].cd_values, nullptr);
  for (unsigned j = 0; j < 5; ++j) {
    v1.filter[1].cd_values[j] = 100 + j;
  }

  v1.filter[2].id = 901;
  v1.filter[2].flags = H5Z_FLAG_OPTIONAL;
  v1.filter[2].name = H5MM_xstrdup("very_long_custom_filter_name");
  v1.filter[2].cd_nelmts = 2;
  v1.filter[2].cd_values = v1.filter[2]._cd_values;
  v1.filter[2].cd_values[0] = 77;
  v1.filter[2].cd_values[1] = 88;

  v1.filter[3].id = 902;
  v1.filter[3].flags = 0;
  std::strcpy(v1.filter[3]._name, "short");
  v1.filter[3].name = v1.filter[3]._name;
  v1.filter[3].cd_nelmts = 0;

  const size_t v1_size = H5O_msg_raw_size(f, H5O_PLINE_ID, true, &v1);
  EXPECT_GT(v1_size, 8U);
  std::vector<uint8_t> v1_raw(v1_size, 0xff);
  ASSERT_GE(H5O_msg_encode(f, H5O_PLINE_ID, true, v1_raw.data(), &v1), 0);

  H5O_pline_t* dec_v1 = static_cast<H5O_pline_t*>(
      H5O_msg_decode(f, nullptr, H5O_PLINE_ID, v1_raw.size(), v1_raw.data()));
  ASSERT_NE(dec_v1, nullptr);
  absl::Cleanup free_dec_v1 = [dec_v1] { H5O_msg_free(H5O_PLINE_ID, dec_v1); };

  EXPECT_EQ(dec_v1->version, static_cast<unsigned>(H5O_PLINE_VERSION_1));
  EXPECT_EQ(dec_v1->nused, 4U);
  EXPECT_STREQ(dec_v1->filter[0].name, "deflate");
  EXPECT_EQ(dec_v1->filter[0].cd_values[0], 6U);
  EXPECT_EQ(dec_v1->filter[1].name, nullptr);
  EXPECT_EQ(dec_v1->filter[1].cd_nelmts, 5U);
  EXPECT_NE(dec_v1->filter[1].cd_values, dec_v1->filter[1]._cd_values);
  EXPECT_EQ(dec_v1->filter[1].cd_values[4], 104U);
  EXPECT_STREQ(dec_v1->filter[2].name, "very_long_custom_filter_name");
  EXPECT_NE(dec_v1->filter[2].name, dec_v1->filter[2]._name);
  EXPECT_STREQ(dec_v1->filter[3].name, "short");
  EXPECT_EQ(dec_v1->filter[3].name, dec_v1->filter[3]._name);

  // Version 2 encode/decode skips internal filter name fields and padding.
  v1.version = H5O_PLINE_VERSION_2;
  const size_t v2_size = H5O_msg_raw_size(f, H5O_PLINE_ID, true, &v1);
  EXPECT_LT(v2_size, v1_size);
  std::vector<uint8_t> v2_raw(v2_size, 0xff);
  ASSERT_GE(H5O_msg_encode(f, H5O_PLINE_ID, true, v2_raw.data(), &v1), 0);

  H5O_pline_t* dec_v2 = static_cast<H5O_pline_t*>(
      H5O_msg_decode(f, nullptr, H5O_PLINE_ID, v2_raw.size(), v2_raw.data()));
  ASSERT_NE(dec_v2, nullptr);
  absl::Cleanup free_dec_v2 = [dec_v2] { H5O_msg_free(H5O_PLINE_ID, dec_v2); };
  EXPECT_EQ(dec_v2->version, static_cast<unsigned>(H5O_PLINE_VERSION_2));
  EXPECT_EQ(dec_v2->nused, 4U);
  EXPECT_EQ(dec_v2->filter[0].name, nullptr);
  EXPECT_STREQ(dec_v2->filter[2].name, "very_long_custom_filter_name");
  EXPECT_STREQ(dec_v2->filter[3].name, "short");

  // Copy (allocating and into existing struct), including empty pipeline
  // branch.
  H5O_pline_t* copy_alloc =
      static_cast<H5O_pline_t*>(H5O_msg_copy(H5O_PLINE_ID, &v1, nullptr));
  ASSERT_NE(copy_alloc, nullptr);
  EXPECT_EQ(copy_alloc->nused, 4U);
  EXPECT_STREQ(copy_alloc->filter[2].name, "very_long_custom_filter_name");
  EXPECT_NE(copy_alloc->filter[2].name, v1.filter[2].name);
  EXPECT_EQ(copy_alloc->filter[1].cd_values[4], 104U);
  H5O_msg_free(H5O_PLINE_ID, copy_alloc);

  H5O_pline_t copy_stack{};
  EXPECT_EQ(H5O_msg_copy(H5O_PLINE_ID, &v1, &copy_stack), &copy_stack);
  EXPECT_GE(H5O_msg_reset(H5O_PLINE_ID, &copy_stack), 0);
  // Resetting again when filter == nullptr is a safe no-op.
  EXPECT_GE(H5O_msg_reset(H5O_PLINE_ID, &copy_stack), 0);

  // Empty pipeline encode/decode/copy (dst->filter == nullptr).
  H5O_pline_t empty{};
  empty.version = H5O_PLINE_VERSION_2;
  EXPECT_EQ(H5O_msg_raw_size(f, H5O_PLINE_ID, true, &empty), 2U);
  std::vector<uint8_t> empty_raw(2, 0);
  ASSERT_GE(H5O_msg_encode(f, H5O_PLINE_ID, true, empty_raw.data(), &empty), 0);
  H5O_pline_t* empty_dec = static_cast<H5O_pline_t*>(H5O_msg_decode(
      f, nullptr, H5O_PLINE_ID, empty_raw.size(), empty_raw.data()));
  ASSERT_NE(empty_dec, nullptr);
  EXPECT_EQ(empty_dec->nused, 0U);
  H5O_msg_free(H5O_PLINE_ID, empty_dec);

  H5O_pline_t* empty_copy =
      static_cast<H5O_pline_t*>(H5O_msg_copy(H5O_PLINE_ID, &empty, nullptr));
  ASSERT_NE(empty_copy, nullptr);
  EXPECT_EQ(empty_copy->filter, nullptr);
  H5O_msg_free(H5O_PLINE_ID, empty_copy);

  // Debug output with both named and unnamed filters and narrow fwidth.
  FILE* fp = std::tmpfile();
  ASSERT_NE(fp, nullptr);
  absl::Cleanup close_fp = [fp] { std::fclose(fp); };
  ASSERT_GE(H5O_debug_id(H5O_PLINE_ID, f, dec_v2, fp, 2, 1), 0);
  const std::string dbg = ReadTmpFile(fp);
  EXPECT_THAT(dbg, HasSubstr("Number of filters:"));
  EXPECT_THAT(dbg, HasSubstr("NONE"));
  EXPECT_THAT(dbg, HasSubstr("\"very_long_custom_filter_name\""));
  EXPECT_THAT(dbg, HasSubstr("CD value 4"));
}

TEST(H5OplineTest, DecodeValidationErrorsAndCleanupPaths) {
  ScopedFile file = CreateInMemoryFile("pline_decode_errs.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedErrorSilence silence;
  const uint8_t kEmptyGuard[1] = {0};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_PLINE_ID, 0, kEmptyGuard), nullptr);

  // Bad versions (0 and 3).
  const std::vector<uint8_t> kVerZero = {0, 0};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_PLINE_ID, kVerZero.size(),
                           kVerZero.data()),
            nullptr);
  const std::vector<uint8_t> kVerThree = {3, 0};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_PLINE_ID, kVerThree.size(),
                           kVerThree.data()),
            nullptr);

  // Truncated before nused.
  const std::vector<uint8_t> kNoNused = {H5O_PLINE_VERSION_2};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_PLINE_ID, kNoNused.size(),
                           kNoNused.data()),
            nullptr);

  // Too many filters (> H5Z_MAX_NFILTERS).
  const std::vector<uint8_t> kTooMany = {H5O_PLINE_VERSION_2,
                                         H5Z_MAX_NFILTERS + 1};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_PLINE_ID, kTooMany.size(),
                           kTooMany.data()),
            nullptr);

  // Version 1 truncated reserved bytes.
  const std::vector<uint8_t> kV1ShortResv = {H5O_PLINE_VERSION_1, 0, 0, 0};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_PLINE_ID, kV1ShortResv.size(),
                           kV1ShortResv.data()),
            nullptr);

  // Build a valid v2 header with 2 filters where filter 0 allocates heap name
  // and >4 cd_values, and filter 1 fails midway to test partial cleanup.
  std::vector<uint8_t> two_filt = {H5O_PLINE_VERSION_2, 2};
  AppendUint16LE(two_filt, 500);
  const char kLongName[] = "long_filter_name_for_cleanup";
  AppendUint16LE(two_filt, sizeof(kLongName));
  AppendUint16LE(two_filt, 0);
  AppendUint16LE(two_filt, 5);
  two_filt.insert(two_filt.end(), kLongName, kLongName + sizeof(kLongName));
  for (uint32_t j = 0; j < 5; ++j) {
    AppendUint32LE(two_filt, j);
  }
  // Truncated before filter 1 ID.
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_PLINE_ID, two_filt.size(),
                           two_filt.data()),
            nullptr);

  // Truncated filter 1 name_length.
  {
    std::vector<uint8_t> buf = two_filt;
    AppendUint16LE(buf, 501);
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_PLINE_ID, buf.size(), buf.data()),
              nullptr);
  }

  // Truncated filter 1 flags.
  {
    std::vector<uint8_t> buf = two_filt;
    AppendUint16LE(buf, 501);
    AppendUint16LE(buf, 4);
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_PLINE_ID, buf.size(), buf.data()),
              nullptr);
  }

  // Truncated filter 1 cd_nelmts.
  {
    std::vector<uint8_t> buf = two_filt;
    AppendUint16LE(buf, 501);
    AppendUint16LE(buf, 4);
    AppendUint16LE(buf, 0);
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_PLINE_ID, buf.size(), buf.data()),
              nullptr);
  }

  // Unterminated filter name (no NUL byte within remaining buffer).
  {
    std::vector<uint8_t> buf = two_filt;
    AppendUint16LE(buf, 501);
    AppendUint16LE(buf, 4);
    AppendUint16LE(buf, 0);
    AppendUint16LE(buf, 0);
    buf.push_back('a');
    buf.push_back('b');
    buf.push_back('c');
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_PLINE_ID, buf.size(), buf.data()),
              nullptr);
  }

  // NUL-terminated name present, but claimed name_length overruns remaining
  // buffer.
  {
    std::vector<uint8_t> buf = two_filt;
    AppendUint16LE(buf, 501);
    AppendUint16LE(buf, 16);
    AppendUint16LE(buf, 0);
    AppendUint16LE(buf, 0);
    buf.push_back('a');
    buf.push_back('\0');
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_PLINE_ID, buf.size(), buf.data()),
              nullptr);
  }

  // Truncated inside filter cd_values.
  {
    std::vector<uint8_t> buf = two_filt;
    AppendUint16LE(buf, H5Z_FILTER_DEFLATE);
    AppendUint16LE(buf, 0);
    AppendUint16LE(buf, 2);
    AppendUint32LE(buf, 1);
    buf.push_back(0x01);  // only 1 byte of 2nd cd_value
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_PLINE_ID, buf.size(), buf.data()),
              nullptr);
  }

  // Version 1 invalid name_length not a multiple of 8.
  {
    std::vector<uint8_t> buf = {H5O_PLINE_VERSION_1, 1, 0, 0, 0, 0, 0, 0};
    AppendUint16LE(buf, H5Z_FILTER_DEFLATE);
    AppendUint16LE(buf, 5);
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_PLINE_ID, buf.size(), buf.data()),
              nullptr);
  }

  // Version 1 missing 4-byte odd cd_nelmts padding.
  {
    std::vector<uint8_t> buf = {H5O_PLINE_VERSION_1, 1, 0, 0, 0, 0, 0, 0};
    AppendUint16LE(buf, H5Z_FILTER_DEFLATE);
    AppendUint16LE(buf, 0);  // name_length
    AppendUint16LE(buf, 0);  // flags
    AppendUint16LE(buf, 1);  // cd_nelmts (odd -> expects 4 padding bytes)
    AppendUint32LE(buf, 4);  // 1 cd_value, missing padding
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_PLINE_ID, buf.size(), buf.data()),
              nullptr);
  }
}

TEST(H5OplineTest, SetVersionPreCopyFileAndPublicDatasetPipeline) {
  constexpr H5Z_filter_t kCustomFilterId = 311;
  H5Z_class2_t custom_cls{};
  custom_cls.version = H5Z_CLASS_T_VERS;
  custom_cls.id = kCustomFilterId;
  custom_cls.encoder_present = 1;
  custom_cls.decoder_present = 1;
  custom_cls.name = "pline_test_passthrough";
  custom_cls.filter = DummyFilterCallback;
  ASSERT_GE(H5Zregister(&custom_cls), 0);
  absl::Cleanup unregister_filter = [] { H5Zunregister(kCustomFilterId); };

  ScopedFile src_v18 =
      CreateFileWithBounds("pline_src.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
  ScopedFile dst_v18 = CreateFileWithBounds("pline_dst_ok.h5", H5F_LIBVER_V18,
                                            H5F_LIBVER_LATEST);
  ScopedFile dst_earliest = CreateFileWithBounds(
      "pline_dst_old.h5", H5F_LIBVER_EARLIEST, H5F_LIBVER_V18);
  ASSERT_TRUE(src_v18.valid());
  ASSERT_TRUE(dst_v18.valid());
  ASSERT_TRUE(dst_earliest.valid());

  H5F_t* f_v18 = UnwrapFile(src_v18.get());
  H5F_t* f_earliest = UnwrapFile(dst_earliest.get());
  ASSERT_NE(f_v18, nullptr);
  ASSERT_NE(f_earliest, nullptr);

  // Exercise H5O_pline_set_version upgrade and out-of-bounds error.
  H5O_pline_t ver_check{};
  ver_check.version = H5O_PLINE_VERSION_1;
  EXPECT_GE(H5O_pline_set_version(f_earliest, &ver_check), 0);
  EXPECT_EQ(ver_check.version, static_cast<unsigned>(H5O_PLINE_VERSION_1));

  EXPECT_GE(H5O_pline_set_version(f_v18, &ver_check), 0);
  EXPECT_EQ(ver_check.version, static_cast<unsigned>(H5O_PLINE_VERSION_2));

  {
    ScopedErrorSilence silence;
    ver_check.version = H5O_PLINE_VERSION_LATEST + 1;
    EXPECT_LT(H5O_pline_set_version(f_v18, &ver_check), 0);
  }

  // Create chunked + filtered dataset using shuffle, deflate, and custom
  // filter.
  const hsize_t dims[1] = {16};
  const hsize_t chunk_dims[1] = {8};
  ScopedSpace space(H5Screate_simple(1, dims, nullptr));
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(space.valid());
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, chunk_dims), 0);
  ASSERT_GE(H5Pset_shuffle(dcpl.get()), 0);
  ASSERT_GE(H5Pset_deflate(dcpl.get(), 6), 0);
  const unsigned int cd_vals[2] = {11, 22};
  ASSERT_GE(
      H5Pset_filter(dcpl.get(), kCustomFilterId, H5Z_FLAG_OPTIONAL, 2, cd_vals),
      0);

  ScopedDataset dset(H5Dcreate2(src_v18.get(), "/filtered", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, dcpl.get(),
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  std::vector<int32_t> write_data(16, 42);
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, write_data.data()),
            0);

  // Direct pre_copy_file invocation: succeeds with udata == nullptr, and
  // rejects out-of-bounds version >
  // H5O_pline_ver_bounds[H5F_HIGH_BOUND(file_dst)].
  H5O_pline_t direct_pline{};
  direct_pline.version = H5O_PLINE_VERSION_1;
  bool deleted = false;
  H5O_copy_t cpy_info{};
  cpy_info.file_dst = f_v18;
  ASSERT_GE(H5O_MSG_PLINE->pre_copy_file(f_v18, &direct_pline, &deleted,
                                         &cpy_info, nullptr),
            0);
  {
    ScopedErrorSilence silence;
    H5O_pline_t bad_ver_pline{};
    bad_ver_pline.version = H5O_PLINE_VERSION_LATEST + 1;
    EXPECT_LT(H5O_MSG_PLINE->pre_copy_file(f_v18, &bad_ver_pline, &deleted,
                                           &cpy_info, nullptr),
              0);
  }
  dset.reset();

  // H5Ocopy exercises pre_copy_file with udata != nullptr.
  EXPECT_GE(H5Ocopy(src_v18.get(), "/filtered", dst_v18.get(), "/filtered_copy",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_GE(H5Ocopy(src_v18.get(), "/filtered", dst_earliest.get(),
                    "/filtered_v18", H5P_DEFAULT, H5P_DEFAULT),
            0);
}

}  // namespace
}  // namespace hdf5
