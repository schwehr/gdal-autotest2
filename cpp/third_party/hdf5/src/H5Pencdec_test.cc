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
// Tests property list binary encoding and decoding, including public
// H5Pencode2/H5Pencode1/H5Pdecode APIs across standard HDF5 property list
// classes, synthesized header/property corruption cases, internal
// non-default-only encoding, and primitive type encode/decode callbacks
// (third_party/hdf5/src/H5Pencdec.c).
//
// Google3 build notes: zlib (deflate) and libaec (szip) are compiled in;
// parallel MPI, plugins, and external VFD drivers (ROS3/direct/subfiling) are
// disabled.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Pencdec.c

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG; match its struct layout for internal headers.
#ifndef NDEBUG
#define H5PENCDEC_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5P_FRIEND
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Lprivate.h"
#include "third_party/hdf5/src/H5Ppkg.h"
}  // extern "C"
#ifdef H5PENCDEC_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5PENCDEC_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::Eq;

using ScopedPClass = ScopedH5Id<H5Pclose_class>;

constexpr uint8_t kEncodeVers = 0;

// Encodes `plist_id` using H5Pencode2: first queries required size with a null
// buffer, then encodes into a sentinel-filled byte vector.
std::vector<uint8_t> EncodePlistV2(hid_t plist_id, hid_t fapl_id = H5P_DEFAULT) {
  size_t nalloc = 0;
  if (H5Pencode2(plist_id, nullptr, &nalloc, fapl_id) < 0 || nalloc == 0) {
    return {};
  }
  std::vector<uint8_t> buf(nalloc, 0xa5);
  size_t written = nalloc;
  if (H5Pencode2(plist_id, buf.data(), &written, fapl_id) < 0) {
    return {};
  }
  EXPECT_EQ(written, nalloc);
  return buf;
}

// Encodes `plist_id` using deprecated H5Pencode1.
std::vector<uint8_t> EncodePlistV1(hid_t plist_id) {
  size_t nalloc = 0;
  if (H5Pencode1(plist_id, nullptr, &nalloc) < 0 || nalloc == 0) {
    return {};
  }
  std::vector<uint8_t> buf(nalloc, 0x5a);
  size_t written = nalloc;
  if (H5Pencode1(plist_id, buf.data(), &written) < 0) {
    return {};
  }
  EXPECT_EQ(written, nalloc);
  return buf;
}

void AppendCString(std::vector<uint8_t>& out, std::string_view s) {
  out.insert(out.end(), s.begin(), s.end());
  out.push_back(0);
}

void AppendLeU32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xffU));
  }
}

void AppendLeU64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xffULL));
  }
}

TEST(H5PencdecTest, EncodeAndDecodeStandardClassesRoundTrip) {
  // Note: HDF5 public class ID macros expand to runtime H5P_CLS_* globals.
  const std::array<hid_t, 21> classes = {
      H5P_OBJECT_CREATE,    H5P_FILE_CREATE,      H5P_FILE_ACCESS,
      H5P_DATASET_CREATE,   H5P_DATASET_ACCESS,   H5P_DATASET_XFER,
      H5P_FILE_MOUNT,       H5P_GROUP_CREATE,     H5P_GROUP_ACCESS,
      H5P_DATATYPE_CREATE,  H5P_DATATYPE_ACCESS,  H5P_MAP_CREATE,
      H5P_MAP_ACCESS,       H5P_STRING_CREATE,    H5P_ATTRIBUTE_CREATE,
      H5P_ATTRIBUTE_ACCESS, H5P_OBJECT_COPY,      H5P_LINK_CREATE,
      H5P_LINK_ACCESS,      H5P_VOL_INITIALIZE,   H5P_REFERENCE_ACCESS,
  };

  for (hid_t cls_id : classes) {
    ScopedPlist plist(H5Pcreate(cls_id));
    ASSERT_NE(H5I_INVALID_HID, plist.get()) << "cls_id=" << cls_id;

    std::vector<uint8_t> encoded = EncodePlistV2(plist.get());
    ASSERT_GE(encoded.size(), 3U);
    EXPECT_EQ(encoded[0], kEncodeVers);
    EXPECT_GT(encoded[1], static_cast<uint8_t>(H5P_TYPE_USER));
    EXPECT_LT(encoded[1], static_cast<uint8_t>(H5P_TYPE_MAX_TYPE));
    EXPECT_EQ(encoded.back(), 0);

    ScopedPlist decoded(H5Pdecode(encoded.data()));
    ASSERT_NE(H5I_INVALID_HID, decoded.get());

    ScopedPClass dec_cls(H5Pget_class(decoded.get()));
    ASSERT_NE(H5I_INVALID_HID, dec_cls.get());
    EXPECT_GT(H5Pequal(cls_id, dec_cls.get()), 0);
    EXPECT_GT(H5Pequal(plist.get(), decoded.get()), 0);
  }
}

TEST(H5PencdecTest, EncodeAndDecodeConfiguredFileCreateAndAccessRoundTrip) {
  // 1. Configure File Creation Property List (exercises hsize_t, unsigned,
  //    uint8_t, bool, and skipping non-encodable "super_version" property).
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_NE(H5I_INVALID_HID, fcpl.get());
  ASSERT_GE(H5Pset_userblock(fcpl.get(), 4096), 0);
  ASSERT_GE(H5Pset_sym_k(fcpl.get(), 24, 12), 0);
  ASSERT_GE(H5Pset_istore_k(fcpl.get(), 48), 0);
  ASSERT_GE(H5Pset_sizes(fcpl.get(), 8, 8), 0);
  ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 2), 0);
  ASSERT_GE(H5Pset_shared_mesg_phase_change(fcpl.get(), 35, 15), 0);
  ASSERT_GE(
      H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_PAGE, true,
                                 2048),
      0);
  ASSERT_GE(H5Pset_file_space_page_size(fcpl.get(), 8192), 0);

  const std::vector<uint8_t> fcpl_v2 = EncodePlistV2(fcpl.get());
  ASSERT_FALSE(fcpl_v2.empty());
  const std::vector<uint8_t> fcpl_v1 = EncodePlistV1(fcpl.get());
  EXPECT_EQ(fcpl_v1, fcpl_v2);

  ScopedPlist dec_fcpl(H5Pdecode(fcpl_v2.data()));
  ASSERT_NE(H5I_INVALID_HID, dec_fcpl.get());
  EXPECT_GT(H5Pequal(fcpl.get(), dec_fcpl.get()), 0);

  hsize_t userblock = 0;
  unsigned ik = 0;
  unsigned lk = 0;
  H5F_fspace_strategy_t strategy = H5F_FSPACE_STRATEGY_FSM_AGGR;
  hbool_t persist = 0;
  hsize_t threshold = 0;
  hsize_t page_size = 0;
  ASSERT_GE(H5Pget_userblock(dec_fcpl.get(), &userblock), 0);
  ASSERT_GE(H5Pget_sym_k(dec_fcpl.get(), &ik, &lk), 0);
  ASSERT_GE(H5Pget_file_space_strategy(dec_fcpl.get(), &strategy, &persist,
                                       &threshold),
            0);
  ASSERT_GE(H5Pget_file_space_page_size(dec_fcpl.get(), &page_size), 0);
  EXPECT_EQ(userblock, 4096U);
  EXPECT_EQ(ik, 24U);
  EXPECT_EQ(lk, 12U);
  EXPECT_EQ(strategy, H5F_FSPACE_STRATEGY_PAGE);
  EXPECT_NE(persist, 0);
  EXPECT_EQ(threshold, 2048U);
  EXPECT_EQ(page_size, 8192U);

  // 2. Configure File Access Property List (exercises size_t, double, hsize_t,
  //    unsigned, bool, and uint64_t rfic_flags).
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_NE(H5I_INVALID_HID, fapl.get());
  ASSERT_GE(H5Pset_cache(fapl.get(), 0, 521, 1048576, 0.25), 0);
  ASSERT_GE(H5Pset_alignment(fapl.get(), 4096, 8192), 0);
  ASSERT_GE(H5Pset_meta_block_size(fapl.get(), 4096), 0);
  ASSERT_GE(H5Pset_sieve_buf_size(fapl.get(), 131072), 0);
  ASSERT_GE(H5Pset_small_data_block_size(fapl.get(), 4096), 0);
  ASSERT_GE(H5Pset_gc_references(fapl.get(), 1), 0);
  ASSERT_GE(H5Pset_elink_file_cache_size(fapl.get(), 16), 0);
  ASSERT_GE(H5Pset_metadata_read_attempts(fapl.get(), 5), 0);
  ASSERT_GE(H5Pset_evict_on_close(fapl.get(), true), 0);
  ASSERT_GE(H5Pset_mdc_log_options(fapl.get(), true, "mdc.log", true), 0);
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST),
            0);
  ASSERT_GE(H5Pset_page_buffer_size(fapl.get(), 16384, 25, 25), 0);
  ASSERT_GE(H5Pset_relax_file_integrity_checks(
                fapl.get(), H5F_RFIC_UNUSUAL_NUM_UNUSED_NUMERIC_BITS),
            0);
  ASSERT_GE(H5Pset_fclose_degree(fapl.get(), H5F_CLOSE_STRONG), 0);

  // Also test encoding with an explicit non-default encoding FAPL argument.
  ScopedPlist enc_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_NE(H5I_INVALID_HID, enc_fapl.get());
  ASSERT_GE(
      H5Pset_libver_bounds(enc_fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST),
      0);

  const std::vector<uint8_t> fapl_buf = EncodePlistV2(fapl.get(), enc_fapl.get());
  ASSERT_FALSE(fapl_buf.empty());

  // Verify the versioned H5Pencode macro matches H5Pencode2.
  size_t macro_nalloc = 0;
  ASSERT_GE(H5Pencode(fapl.get(), nullptr, &macro_nalloc, enc_fapl.get()), 0);
  EXPECT_EQ(macro_nalloc, fapl_buf.size());

  ScopedPlist dec_fapl(H5Pdecode(fapl_buf.data()));
  ASSERT_NE(H5I_INVALID_HID, dec_fapl.get());
  EXPECT_GT(H5Pequal(fapl.get(), dec_fapl.get()), 0);

  size_t rdcc_nslots = 0;
  size_t rdcc_nbytes = 0;
  double rdcc_w0 = 0.0;
  hsize_t align_thrhd = 0;
  hsize_t align = 0;
  unsigned elink_cache = 0;
  hbool_t evict_on_close = 0;
  uint64_t rfic_flags = 0;
  ASSERT_GE(
      H5Pget_cache(dec_fapl.get(), nullptr, &rdcc_nslots, &rdcc_nbytes, &rdcc_w0),
      0);
  ASSERT_GE(H5Pget_alignment(dec_fapl.get(), &align_thrhd, &align), 0);
  ASSERT_GE(H5Pget_elink_file_cache_size(dec_fapl.get(), &elink_cache), 0);
  ASSERT_GE(H5Pget_evict_on_close(dec_fapl.get(), &evict_on_close), 0);
  ASSERT_GE(H5Pget_relax_file_integrity_checks(dec_fapl.get(), &rfic_flags), 0);
  EXPECT_EQ(rdcc_nslots, 521U);
  EXPECT_EQ(rdcc_nbytes, 1048576U);
  EXPECT_DOUBLE_EQ(rdcc_w0, 0.25);
  EXPECT_EQ(align_thrhd, 4096U);
  EXPECT_EQ(align, 8192U);
  EXPECT_EQ(elink_cache, 16U);
  EXPECT_NE(evict_on_close, 0);
  EXPECT_EQ(rfic_flags,
            static_cast<uint64_t>(H5F_RFIC_UNUSUAL_NUM_UNUSED_NUMERIC_BITS));
}

TEST(H5PencdecTest, EncodeAndDecodeDatasetLinkAndNestedFaplProperties) {
  // 1. Dataset Creation, Access, and Transfer property lists.
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_NE(H5I_INVALID_HID, dcpl.get());
  constexpr std::array<hsize_t, 2> kChunkDims = {4, 8};
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 2, kChunkDims.data()), 0);
  ASSERT_GE(H5Pset_deflate(dcpl.get(), 6), 0);
  ASSERT_GE(H5Pset_alloc_time(dcpl.get(), H5D_ALLOC_TIME_EARLY), 0);
  ASSERT_GE(H5Pset_dset_no_attrs_hint(dcpl.get(), true), 0);

  const std::vector<uint8_t> dcpl_buf = EncodePlistV2(dcpl.get());
  ASSERT_FALSE(dcpl_buf.empty());
  ScopedPlist dec_dcpl(H5Pdecode(dcpl_buf.data()));
  ASSERT_NE(H5I_INVALID_HID, dec_dcpl.get());
  EXPECT_GT(H5Pequal(dcpl.get(), dec_dcpl.get()), 0);

  hbool_t no_attrs_hint = 0;
  ASSERT_GE(H5Pget_dset_no_attrs_hint(dec_dcpl.get(), &no_attrs_hint), 0);
  EXPECT_NE(no_attrs_hint, 0);

  ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_NE(H5I_INVALID_HID, dapl.get());
  ASSERT_GE(H5Pset_chunk_cache(dapl.get(), 103, 524288, 0.5), 0);
  ASSERT_GE(H5Pset_virtual_printf_gap(dapl.get(), 32), 0);
  const std::vector<uint8_t> dapl_buf = EncodePlistV2(dapl.get());
  ASSERT_FALSE(dapl_buf.empty());
  ScopedPlist dec_dapl(H5Pdecode(dapl_buf.data()));
  ASSERT_NE(H5I_INVALID_HID, dec_dapl.get());
  EXPECT_GT(H5Pequal(dapl.get(), dec_dapl.get()), 0);

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_NE(H5I_INVALID_HID, dxpl.get());
  ASSERT_GE(H5Pset_buffer(dxpl.get(), 262144, nullptr, nullptr), 0);
  ASSERT_GE(H5Pset_hyper_vector_size(dxpl.get(), 2048), 0);
  ASSERT_GE(H5Pset_edc_check(dxpl.get(), H5Z_DISABLE_EDC), 0);
  const std::vector<uint8_t> dxpl_buf = EncodePlistV2(dxpl.get());
  ASSERT_FALSE(dxpl_buf.empty());
  ScopedPlist dec_dxpl(H5Pdecode(dxpl_buf.data()));
  ASSERT_NE(H5I_INVALID_HID, dec_dxpl.get());
  EXPECT_GT(H5Pequal(dxpl.get(), dec_dxpl.get()), 0);

  // 2. Link Creation and Link Access with nested elink FAPL (triggers recursive
  //    H5P__encode / H5P__decode from H5Plapl.c).
  ScopedPlist lcpl(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_NE(H5I_INVALID_HID, lcpl.get());
  ASSERT_GE(H5Pset_create_intermediate_group(lcpl.get(), 1), 0);
  ASSERT_GE(H5Pset_char_encoding(lcpl.get(), H5T_CSET_UTF8), 0);
  const std::vector<uint8_t> lcpl_buf = EncodePlistV2(lcpl.get());
  ASSERT_FALSE(lcpl_buf.empty());
  ScopedPlist dec_lcpl(H5Pdecode(lcpl_buf.data()));
  ASSERT_NE(H5I_INVALID_HID, dec_lcpl.get());
  EXPECT_GT(H5Pequal(lcpl.get(), dec_lcpl.get()), 0);

  ScopedPlist nested_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_NE(H5I_INVALID_HID, nested_fapl.get());
  ASSERT_GE(H5Pset_sieve_buf_size(nested_fapl.get(), 32768), 0);

  ScopedPlist lapl(H5Pcreate(H5P_LINK_ACCESS));
  ASSERT_NE(H5I_INVALID_HID, lapl.get());
  ASSERT_GE(H5Pset_nlinks(lapl.get(), 64), 0);
  ASSERT_GE(H5Pset_elink_fapl(lapl.get(), nested_fapl.get()), 0);
  const std::vector<uint8_t> lapl_buf = EncodePlistV2(lapl.get());
  ASSERT_FALSE(lapl_buf.empty());
  ScopedPlist dec_lapl(H5Pdecode(lapl_buf.data()));
  ASSERT_NE(H5I_INVALID_HID, dec_lapl.get());

  size_t nlinks = 0;
  ASSERT_GE(H5Pget_nlinks(dec_lapl.get(), &nlinks), 0);
  EXPECT_EQ(nlinks, 64U);
  ScopedPlist dec_nested_fapl(H5Pget_elink_fapl(dec_lapl.get()));
  ASSERT_NE(H5I_INVALID_HID, dec_nested_fapl.get());
  size_t sieve_size = 0;
  ASSERT_GE(H5Pget_sieve_buf_size(dec_nested_fapl.get(), &sieve_size), 0);
  EXPECT_EQ(sieve_size, 32768U);
}

TEST(H5PencdecTest, EncodeAndDecodePublicErrorCases) {
  ScopedErrorSilence silence;

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_NE(H5I_INVALID_HID, dcpl.get());
  ScopedPlist non_fapl_plist(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_NE(H5I_INVALID_HID, non_fapl_plist.get());
  ScopedSpace non_plist_id(H5Screate(H5S_SCALAR));
  ASSERT_NE(H5I_INVALID_HID, non_plist_id.get());

  size_t nalloc = 0;
  std::array<uint8_t, 64> small_buf{};

  EXPECT_LT(H5Pencode2(H5I_INVALID_HID, nullptr, &nalloc, H5P_DEFAULT), 0);
  EXPECT_LT(H5Pencode2(H5P_DEFAULT, nullptr, &nalloc, H5P_DEFAULT), 0);
  EXPECT_LT(H5Pencode2(non_plist_id.get(), nullptr, &nalloc, H5P_DEFAULT), 0);
  EXPECT_LT(H5Pencode2(dcpl.get(), small_buf.data(), nullptr, H5P_DEFAULT), 0);
  EXPECT_LT(H5Pencode2(dcpl.get(), nullptr, &nalloc, /*fapl_id=*/-99), 0);
  // Note: upstream H5CX_set_apl only checks `fapl_id` type under H5CX_DEBUG,
  // so test wrong plist class (a valid H5P_genplist_t missing fapl properties)
  // rather than a non-plist hid_t for `fapl_id` under -DNDEBUG.
  EXPECT_LT(H5Pencode2(dcpl.get(), nullptr, &nalloc, non_fapl_plist.get()), 0);

  EXPECT_LT(H5Pencode1(H5I_INVALID_HID, nullptr, &nalloc), 0);
  EXPECT_LT(H5Pencode1(non_plist_id.get(), nullptr, &nalloc), 0);
  EXPECT_LT(H5Pencode1(dcpl.get(), small_buf.data(), nullptr), 0);

  EXPECT_EQ(H5Pdecode(nullptr), H5I_INVALID_HID);
}

TEST(H5PencdecTest, DecodeSynthesizedBuffersHandlesHeadersErrorsAndBufferGrowth) {
  // 1. Minimal valid encoded buffers (header + terminator, zero properties):
  //    exercises `value_buf == NULL` on cleanup in H5P__decode.
  constexpr std::array<uint8_t, 3> kEmptyDcplBuf = {
      kEncodeVers, static_cast<uint8_t>(H5P_TYPE_DATASET_CREATE), 0};
  ScopedPlist empty_dcpl(H5Pdecode(kEmptyDcplBuf.data()));
  ASSERT_NE(H5I_INVALID_HID, empty_dcpl.get());
  EXPECT_GT(H5Pequal(empty_dcpl.get(), H5P_DATASET_CREATE_DEFAULT), 0);

  // 2. Buffer ordering small -> medium -> large -> small properties to exercise
  //    both `prop->size > value_buf_size` (true realloc) and `false` (reuse).
  std::vector<uint8_t> mixed_fcpl;
  mixed_fcpl.push_back(kEncodeVers);
  mixed_fcpl.push_back(static_cast<uint8_t>(H5P_TYPE_FILE_CREATE));
  // 1-byte uint8_t property ("addr_byte_num") -> grows value_buf to 1.
  AppendCString(mixed_fcpl, H5F_CRT_ADDR_BYTE_NUM_NAME);
  mixed_fcpl.push_back(16);
  // 4-byte unsigned property ("symbol_leaf") -> grows value_buf to 4.
  AppendCString(mixed_fcpl, H5F_CRT_SYM_LEAF_NAME);
  mixed_fcpl.push_back(static_cast<uint8_t>(sizeof(unsigned)));
  AppendLeU32(mixed_fcpl, 12U);
  // 8-byte hsize_t property ("block_size") -> grows value_buf to 8.
  AppendCString(mixed_fcpl, H5F_CRT_USER_BLOCK_NAME);
  mixed_fcpl.push_back(2);  // enc_size = 2 bytes for 4096
  mixed_fcpl.push_back(0x00);
  mixed_fcpl.push_back(0x10);
  // 1-byte bool property ("free_space_persist") -> prop->size <= 8 (no realloc).
  AppendCString(mixed_fcpl, H5F_CRT_FREE_SPACE_PERSIST_NAME);
  mixed_fcpl.push_back(1);
  mixed_fcpl.push_back(0);  // terminator

  ScopedPlist dec_mixed(H5Pdecode(mixed_fcpl.data()));
  ASSERT_NE(H5I_INVALID_HID, dec_mixed.get());
  size_t addr_size = 0;
  size_t obj_size = 0;
  unsigned sym_lk = 0;
  hsize_t userblock = 0;
  H5F_fspace_strategy_t strategy = H5F_FSPACE_STRATEGY_FSM_AGGR;
  hbool_t persist = 0;
  hsize_t threshold = 0;
  ASSERT_GE(H5Pget_sizes(dec_mixed.get(), &addr_size, &obj_size), 0);
  ASSERT_GE(H5Pget_sym_k(dec_mixed.get(), nullptr, &sym_lk), 0);
  ASSERT_GE(H5Pget_userblock(dec_mixed.get(), &userblock), 0);
  ASSERT_GE(H5Pget_file_space_strategy(dec_mixed.get(), &strategy, &persist,
                                       &threshold),
            0);
  EXPECT_EQ(addr_size, 16U);
  EXPECT_EQ(sym_lk, 12U);
  EXPECT_EQ(userblock, 4096U);
  EXPECT_NE(persist, 0);

  // 3. Negative header / property validation cases.
  ScopedErrorSilence silence;

  struct HeaderErrorCase {
    std::string_view label;
    std::array<uint8_t, 3> buf;
  };
  constexpr auto kHeaderErrorCases = std::to_array<HeaderErrorCase>({
      {.label = "bad_version_1",
       .buf = {1, static_cast<uint8_t>(H5P_TYPE_DATASET_CREATE), 0}},
      {.label = "bad_version_255",
       .buf = {255, static_cast<uint8_t>(H5P_TYPE_DATASET_CREATE), 0}},
      {.label = "type_user_0",
       .buf = {kEncodeVers, static_cast<uint8_t>(H5P_TYPE_USER), 0}},
      {.label = "type_max",
       .buf = {kEncodeVers, static_cast<uint8_t>(H5P_TYPE_MAX_TYPE), 0}},
      {.label = "type_255", .buf = {kEncodeVers, 255, 0}},
      {.label = "type_root_uncreatable",
       .buf = {kEncodeVers, static_cast<uint8_t>(H5P_TYPE_ROOT), 0}},
  });
  for (const auto& tc : kHeaderErrorCases) {
    EXPECT_EQ(H5Pdecode(tc.buf.data()), H5I_INVALID_HID) << tc.label;
  }

  // User-defined class encodes H5P_TYPE_USER, so H5Pdecode rejects it.
  ScopedPClass user_cls(H5Pcreate_class(H5P_DATASET_CREATE, "user_enc_cls",
                                        nullptr, nullptr, nullptr, nullptr,
                                        nullptr, nullptr));
  ASSERT_NE(H5I_INVALID_HID, user_cls.get());
  ScopedPlist user_plist(H5Pcreate(user_cls.get()));
  ASSERT_NE(H5I_INVALID_HID, user_plist.get());
  const std::vector<uint8_t> user_buf = EncodePlistV2(user_plist.get());
  ASSERT_GE(user_buf.size(), 3U);
  EXPECT_EQ(user_buf[1], static_cast<uint8_t>(H5P_TYPE_USER));
  EXPECT_EQ(H5Pdecode(user_buf.data()), H5I_INVALID_HID);

  // Unknown property name -> H5P__find_prop_plist returns NULL, cleans up plist.
  std::vector<uint8_t> unknown_prop = {
      kEncodeVers, static_cast<uint8_t>(H5P_TYPE_DATASET_CREATE)};
  AppendCString(unknown_prop, "nonexistent_property_name");
  unknown_prop.push_back(0);
  EXPECT_EQ(H5Pdecode(unknown_prop.data()), H5I_INVALID_HID);

  // Existing property without a decode callback ("super_version" in FCPL).
  std::vector<uint8_t> no_decode_cb = {
      kEncodeVers, static_cast<uint8_t>(H5P_TYPE_FILE_CREATE)};
  AppendCString(no_decode_cb, H5F_CRT_SUPER_VERS_NAME);
  no_decode_cb.push_back(0);
  EXPECT_EQ(H5Pdecode(no_decode_cb.data()), H5I_INVALID_HID);

  // Corrupt size bytes causing prop->decode to fail for unsigned / double /
  // uint64_t inside H5P__decode.
  std::vector<uint8_t> bad_unsigned = {
      kEncodeVers, static_cast<uint8_t>(H5P_TYPE_LINK_CREATE)};
  AppendCString(bad_unsigned, H5L_CRT_INTERMEDIATE_GROUP_NAME);
  bad_unsigned.push_back(1);  // != sizeof(unsigned)
  bad_unsigned.push_back(0);
  EXPECT_EQ(H5Pdecode(bad_unsigned.data()), H5I_INVALID_HID);

  std::vector<uint8_t> bad_double = {
      kEncodeVers, static_cast<uint8_t>(H5P_TYPE_FILE_ACCESS)};
  AppendCString(bad_double, H5F_ACS_PREEMPT_READ_CHUNKS_NAME);
  bad_double.push_back(3);  // != sizeof(double)
  bad_double.push_back(0);
  EXPECT_EQ(H5Pdecode(bad_double.data()), H5I_INVALID_HID);

  std::vector<uint8_t> bad_uint64 = {
      kEncodeVers, static_cast<uint8_t>(H5P_TYPE_FILE_ACCESS)};
  AppendCString(bad_uint64, H5F_ACS_RFIC_FLAGS_NAME);
  bad_uint64.push_back(4);  // != sizeof(uint64_t)
  bad_uint64.push_back(0);
  EXPECT_EQ(H5Pdecode(bad_uint64.data()), H5I_INVALID_HID);
}

TEST(H5PencdecTest, InternalEncodeNonDefaultPropertiesOnlyAndNullNalloc) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_NE(H5I_INVALID_HID, fcpl.get());
  H5P_genplist_t* plist = H5P_object_verify(fcpl.get(), H5P_FILE_CREATE, false);
  ASSERT_NE(plist, nullptr);

  // 1. Null nalloc fails in H5P__encode.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5P__encode(plist, false, nullptr, nullptr), 0);
  }

  // 2. Unmodified plist with enc_all_prop=false encodes only 2-byte header +
  //    1-byte terminator.
  size_t nalloc = 0;
  ASSERT_GE(H5P__encode(plist, /*enc_all_prop=*/false, nullptr, &nalloc), 0);
  EXPECT_EQ(nalloc, 3U);

  std::vector<uint8_t> empty_buf(nalloc, 0xaa);
  size_t written = nalloc;
  ASSERT_GE(
      H5P__encode(plist, /*enc_all_prop=*/false, empty_buf.data(), &written),
      0);
  EXPECT_EQ(written, 3U);
  EXPECT_THAT(empty_buf,
              ElementsAre(kEncodeVers,
                          static_cast<uint8_t>(H5P_TYPE_FILE_CREATE), 0));

  // 3. Modify one property and encode with enc_all_prop=false: only
  //    "block_size" (userblock) is serialized, and decoding restores it while
  //    keeping defaults.
  ASSERT_GE(H5Pset_userblock(fcpl.get(), 4096), 0);
  size_t mod_size = 0;
  ASSERT_GE(H5P__encode(plist, /*enc_all_prop=*/false, nullptr, &mod_size), 0);
  EXPECT_GT(mod_size, 3U);
  size_t all_size = 0;
  ASSERT_GE(H5P__encode(plist, /*enc_all_prop=*/true, nullptr, &all_size), 0);
  EXPECT_LT(mod_size, all_size);

  std::vector<uint8_t> mod_buf(mod_size, 0);
  written = mod_size;
  ASSERT_GE(
      H5P__encode(plist, /*enc_all_prop=*/false, mod_buf.data(), &written), 0);
  EXPECT_EQ(written, mod_size);

  ScopedPlist decoded(H5P__decode(mod_buf.data()));
  ASSERT_NE(H5I_INVALID_HID, decoded.get());
  hsize_t userblock = 0;
  ASSERT_GE(H5Pget_userblock(decoded.get(), &userblock), 0);
  EXPECT_EQ(userblock, 4096U);
  EXPECT_GT(H5Pequal(fcpl.get(), decoded.get()), 0);
}

TEST(H5PencdecTest, InternalEncodeCallbackFailurePropagates) {
  ScopedPlist plist_id(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_NE(H5I_INVALID_HID, plist_id.get());
  H5P_genplist_t* plist =
      H5P_object_verify(plist_id.get(), H5P_DATASET_CREATE, false);
  ASSERT_NE(plist, nullptr);

  auto failing_encode = [](const void*, void**, size_t*) -> herr_t {
    return -1;
  };
  uint32_t prop_val = 7;
  ASSERT_GE(H5P_insert(plist, "failing_enc_prop", sizeof(prop_val), &prop_val,
                       nullptr, nullptr, failing_encode, nullptr, nullptr,
                       nullptr, nullptr, nullptr),
            0);

  ScopedErrorSilence silence;
  // Both size-query pass (buf == nullptr) and encode pass (buf != nullptr)
  // propagate the H5_ITER_ERROR failure out of H5P__encode_cb and H5P__encode.
  size_t nalloc = 0;
  EXPECT_LT(H5P__encode(plist, /*enc_all_prop=*/false, nullptr, &nalloc), 0);

  std::array<uint8_t, 256> buf{};
  nalloc = buf.size();
  EXPECT_LT(H5P__encode(plist, /*enc_all_prop=*/false, buf.data(), &nalloc), 0);
}

TEST(H5PencdecTest, PrimitiveSizeTAndHsizeTEncodeDecodeHelpers) {
  struct SizeTCase {
    size_t value;
    size_t expected_enc_bytes;
  };
  constexpr auto kSizeTCases = std::to_array<SizeTCase>({
      {.value = 0, .expected_enc_bytes = 2},
      {.value = 1, .expected_enc_bytes = 2},
      {.value = 255, .expected_enc_bytes = 2},
      {.value = 256, .expected_enc_bytes = 3},
      {.value = 0x12345678UL, .expected_enc_bytes = 5},
      {.value = std::numeric_limits<size_t>::max(),
       .expected_enc_bytes = 1 + sizeof(size_t)},
  });

  for (const auto& tc : kSizeTCases) {
    void* null_pp = nullptr;
    size_t size_only = 7;
    ASSERT_GE(H5P__encode_size_t(&tc.value, &null_pp, &size_only), 0);
    EXPECT_EQ(size_only, 7 + tc.expected_enc_bytes);

    std::array<uint8_t, 16> buf{};
    void* enc_pp = buf.data();
    size_t enc_size = 0;
    ASSERT_GE(H5P__encode_size_t(&tc.value, &enc_pp, &enc_size), 0);
    EXPECT_EQ(enc_size, tc.expected_enc_bytes);
    EXPECT_EQ(static_cast<uint8_t*>(enc_pp) - buf.data(),
              static_cast<std::ptrdiff_t>(tc.expected_enc_bytes));
    EXPECT_EQ(buf[0], tc.expected_enc_bytes - 1);

    const void* dec_pp = buf.data();
    size_t decoded = ~tc.value;
    ASSERT_GE(H5P__decode_size_t(&dec_pp, &decoded), 0);
    EXPECT_EQ(decoded, tc.value);
    EXPECT_EQ(static_cast<const uint8_t*>(dec_pp) - buf.data(),
              static_cast<std::ptrdiff_t>(tc.expected_enc_bytes));
  }

  constexpr auto kHsizeValues = std::to_array<hsize_t>({
      0ULL,
      4096ULL,
      0x100000000ULL,
      std::numeric_limits<hsize_t>::max(),
  });
  for (hsize_t val : kHsizeValues) {
    void* null_pp = nullptr;
    size_t size_only = 3;
    ASSERT_GE(H5P__encode_hsize_t(&val, &null_pp, &size_only), 0);
    EXPECT_GT(size_only, 4U);

    std::array<uint8_t, 16> buf{};
    void* enc_pp = buf.data();
    size_t enc_size = 0;
    ASSERT_GE(H5P__encode_hsize_t(&val, &enc_pp, &enc_size), 0);
    EXPECT_EQ(enc_size, size_only - 3);

    const void* dec_pp = buf.data();
    hsize_t decoded = ~val;
    ASSERT_GE(H5P__decode_hsize_t(&dec_pp, &decoded), 0);
    EXPECT_EQ(decoded, val);
    EXPECT_EQ(static_cast<const uint8_t*>(dec_pp),
              static_cast<const uint8_t*>(enc_pp));
  }
}

TEST(H5PencdecTest, PrimitiveUnsignedUint8BoolDoubleAndUint64Helpers) {
  // 1. unsigned encode / decode + mismatched size error.
  for (unsigned val : {0U, 42U, 0xdeadbeefU, std::numeric_limits<unsigned>::max()}) {
    void* null_pp = nullptr;
    size_t sz = 5;
    ASSERT_GE(H5P__encode_unsigned(&val, &null_pp, &sz), 0);
    EXPECT_EQ(sz, 5 + 1 + sizeof(unsigned));

    std::array<uint8_t, 8> buf{};
    void* enc_pp = buf.data();
    size_t enc_sz = 0;
    ASSERT_GE(H5P__encode_unsigned(&val, &enc_pp, &enc_sz), 0);
    EXPECT_EQ(enc_sz, 1 + sizeof(unsigned));
    EXPECT_EQ(buf[0], sizeof(unsigned));

    const void* dec_pp = buf.data();
    unsigned decoded = ~val;
    ASSERT_GE(H5P__decode_unsigned(&dec_pp, &decoded), 0);
    EXPECT_EQ(decoded, val);
    EXPECT_EQ(static_cast<const uint8_t*>(dec_pp) - buf.data(),
              static_cast<std::ptrdiff_t>(enc_sz));
  }
  {
    ScopedErrorSilence silence;
    std::array<uint8_t, 8> bad_len = {sizeof(unsigned) + 1, 0, 0, 0, 0};
    const void* dec_pp = bad_len.data();
    unsigned out = 0;
    EXPECT_LT(H5P__decode_unsigned(&dec_pp, &out), 0);
  }

  // 2. uint8_t encode / decode.
  for (uint8_t val : {uint8_t{0}, uint8_t{1}, uint8_t{0x5a}, uint8_t{0xff}}) {
    void* null_pp = nullptr;
    size_t sz = 10;
    ASSERT_GE(H5P__encode_uint8_t(&val, &null_pp, &sz), 0);
    EXPECT_EQ(sz, 11U);

    uint8_t byte = 0;
    void* enc_pp = &byte;
    size_t enc_sz = 0;
    ASSERT_GE(H5P__encode_uint8_t(&val, &enc_pp, &enc_sz), 0);
    EXPECT_EQ(enc_sz, 1U);
    EXPECT_EQ(byte, val);

    const void* dec_pp = &byte;
    uint8_t decoded = static_cast<uint8_t>(~val);
    ASSERT_GE(H5P__decode_uint8_t(&dec_pp, &decoded), 0);
    EXPECT_EQ(decoded, val);
  }

  // 3. bool encode / decode.
  for (bool val : {false, true}) {
    void* null_pp = nullptr;
    size_t sz = 2;
    ASSERT_GE(H5P__encode_bool(&val, &null_pp, &sz), 0);
    EXPECT_EQ(sz, 3U);

    uint8_t byte = 0x99;
    void* enc_pp = &byte;
    size_t enc_sz = 0;
    ASSERT_GE(H5P__encode_bool(&val, &enc_pp, &enc_sz), 0);
    EXPECT_EQ(enc_sz, 1U);
    EXPECT_EQ(byte, val ? 1U : 0U);

    const void* dec_pp = &byte;
    bool decoded = !val;
    ASSERT_GE(H5P__decode_bool(&dec_pp, &decoded), 0);
    EXPECT_EQ(decoded, val);
  }

  // 4. double encode / decode + mismatched size error.
  for (double val : {0.0, -0.0, 0.75, -1234.5678,
                     std::numeric_limits<double>::infinity()}) {
    void* null_pp = nullptr;
    size_t sz = 1;
    ASSERT_GE(H5P__encode_double(&val, &null_pp, &sz), 0);
    EXPECT_EQ(sz, 1 + 1 + sizeof(double));

    std::array<uint8_t, 16> buf{};
    void* enc_pp = buf.data();
    size_t enc_sz = 0;
    ASSERT_GE(H5P__encode_double(&val, &enc_pp, &enc_sz), 0);
    EXPECT_EQ(enc_sz, 1 + sizeof(double));
    EXPECT_EQ(buf[0], sizeof(double));

    const void* dec_pp = buf.data();
    double decoded = 1.0;
    ASSERT_GE(H5P__decode_double(&dec_pp, &decoded), 0);
    EXPECT_EQ(std::bit_cast<uint64_t>(decoded), std::bit_cast<uint64_t>(val));
    EXPECT_EQ(static_cast<const uint8_t*>(dec_pp) - buf.data(),
              static_cast<std::ptrdiff_t>(enc_sz));
  }
  {
    ScopedErrorSilence silence;
    std::array<uint8_t, 12> bad_len = {sizeof(double) - 1};
    const void* dec_pp = bad_len.data();
    double out = 0.0;
    EXPECT_LT(H5P__decode_double(&dec_pp, &out), 0);
  }

  // 5. uint64_t encode / decode + mismatched size error.
  //    Note: upstream H5P__encode_uint64_t (H5Pencdec.c:301) reads
  //    `*(const unsigned *)value` (only 32 bits on LP64), so test the
  //    encode/decode round-trip with 32-bit-range values and verify full
  //    64-bit little-endian decoding directly on H5P__decode_uint64_t.
  for (uint64_t val : {0ULL, 1ULL, 0xabcdef01ULL}) {
    void* null_pp = nullptr;
    size_t sz = 4;
    ASSERT_GE(H5P__encode_uint64_t(&val, &null_pp, &sz), 0);
    EXPECT_EQ(sz, 4 + 1 + sizeof(uint64_t));

    std::array<uint8_t, 16> buf{};
    void* enc_pp = buf.data();
    size_t enc_sz = 0;
    ASSERT_GE(H5P__encode_uint64_t(&val, &enc_pp, &enc_sz), 0);
    EXPECT_EQ(enc_sz, 1 + sizeof(uint64_t));
    EXPECT_EQ(buf[0], sizeof(uint64_t));

    const void* dec_pp = buf.data();
    uint64_t decoded = ~val;
    ASSERT_GE(H5P__decode_uint64_t(&dec_pp, &decoded), 0);
    EXPECT_EQ(decoded, val);
  }
  {
    constexpr uint64_t kFull64 = 0x0123456789abcdefULL;
    std::vector<uint8_t> raw_64;
    raw_64.push_back(sizeof(uint64_t));
    AppendLeU64(raw_64, kFull64);
    const void* dec_pp = raw_64.data();
    uint64_t decoded = 0;
    ASSERT_GE(H5P__decode_uint64_t(&dec_pp, &decoded), 0);
    EXPECT_THAT(decoded, Eq(kFull64));
  }
  {
    ScopedErrorSilence silence;
    std::array<uint8_t, 12> bad_len = {sizeof(unsigned)};
    const void* dec_pp = bad_len.data();
    uint64_t out = 0;
    EXPECT_LT(H5P__decode_uint64_t(&dec_pp, &out), 0);
  }
}

}  // namespace
}  // namespace hdf5
