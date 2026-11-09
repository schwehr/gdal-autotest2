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
// Unit tests for HDF5 File Access Property List (FAPL) routines and callbacks
// (third_party/hdf5/src/H5Pfapl.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Pfapl.c

#ifdef NDEBUG
#define H5_TEST_WAS_NDEBUG 1
#else
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5P_FRIEND
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5FDprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5PBprivate.h"
#include "third_party/hdf5/src/H5Ppkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifndef H5_TEST_WAS_NDEBUG
#undef NDEBUG
#endif
#undef H5_TEST_WAS_NDEBUG

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::Gt;
using ::testing::Lt;

ScopedPlist CreateFapl() {
  return ScopedPlist(H5Pcreate(H5P_FILE_ACCESS));
}

ScopedPlist CreateNonFapl() {
  return ScopedPlist(H5Pcreate(H5P_DATASET_CREATE));
}

std::vector<uint8_t> EncodePlist(hid_t plist_id, hid_t enc_fapl_id = H5P_DEFAULT) {
  size_t size = 0;
  if (H5Pencode2(plist_id, nullptr, &size, enc_fapl_id) < 0 || size == 0) {
    return {};
  }
  std::vector<uint8_t> buf(size);
  if (H5Pencode2(plist_id, buf.data(), &size, enc_fapl_id) < 0) {
    return {};
  }
  return buf;
}

ScopedPlist RoundTripEncodeDecode(hid_t plist_id, hid_t enc_fapl_id = H5P_DEFAULT) {
  std::vector<uint8_t> buf = EncodePlist(plist_id, enc_fapl_id);
  if (buf.empty()) {
    return ScopedPlist();
  }
  return ScopedPlist(H5Pdecode(buf.data()));
}

size_t FindSubsequence(const std::vector<uint8_t>& haystack, const std::string& needle) {
  auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end());
  if (it == haystack.end()) {
    return static_cast<size_t>(-1);
  }
  return static_cast<size_t>(std::distance(haystack.begin(), it));
}

// =============================================================================
// Alignment, Family Offset, Multi Type, Cache, GC, Close Degree, Block Sizes
// =============================================================================

TEST(H5PfaplTest, AlignmentAndEncodeDecode) {
  ScopedPlist fapl = CreateFapl();
  ASSERT_TRUE(fapl.valid());

  hsize_t threshold = 0;
  hsize_t alignment = 0;
  ASSERT_GE(H5Pget_alignment(H5P_FILE_ACCESS_DEFAULT, &threshold, &alignment), 0);
  EXPECT_EQ(threshold, 1u);
  EXPECT_EQ(alignment, 1u);
  EXPECT_GE(H5Pget_alignment(fapl, nullptr, nullptr), 0);

  ASSERT_GE(H5Pset_alignment(fapl, 4096, 512), 0);
  ASSERT_GE(H5Pget_alignment(fapl, &threshold, &alignment), 0);
  EXPECT_EQ(threshold, 4096u);
  EXPECT_EQ(alignment, 512u);

  ScopedPlist decoded = RoundTripEncodeDecode(fapl);
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Pequal(fapl, decoded), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Pset_alignment(fapl, 4096, 0), 0);
  EXPECT_LT(H5Pset_alignment(H5I_INVALID_HID, 1, 1), 0);
  EXPECT_LT(H5Pget_alignment(H5P_DEFAULT, &threshold, &alignment), 0);
  EXPECT_LT(H5Pget_alignment(H5I_INVALID_HID, &threshold, &alignment), 0);
  ScopedPlist dcpl = CreateNonFapl();
  ASSERT_TRUE(dcpl.valid());
  EXPECT_LT(H5Pset_alignment(dcpl, 1, 1), 0);
  EXPECT_LT(H5Pget_alignment(dcpl, &threshold, &alignment), 0);
}

TEST(H5PfaplTest, FamilyOffsetAndEncodeDecode) {
  ScopedPlist fapl = CreateFapl();
  ASSERT_TRUE(fapl.valid());

  hsize_t offset = 999;
  ASSERT_GE(H5Pget_family_offset(fapl, &offset), 0);
  EXPECT_EQ(offset, 0u);
  EXPECT_GE(H5Pget_family_offset(fapl, nullptr), 0);

  ASSERT_GE(H5Pset_family_offset(fapl, 1048576), 0);
  ASSERT_GE(H5Pget_family_offset(fapl, &offset), 0);
  EXPECT_EQ(offset, 1048576u);

  ScopedPlist decoded = RoundTripEncodeDecode(fapl);
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Pequal(fapl, decoded), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Pset_family_offset(H5P_DEFAULT, 128), 0);
  EXPECT_LT(H5Pget_family_offset(H5P_DEFAULT, &offset), 0);
  EXPECT_LT(H5Pset_family_offset(H5I_INVALID_HID, 128), 0);
  EXPECT_LT(H5Pget_family_offset(H5I_INVALID_HID, &offset), 0);
  ScopedPlist dcpl = CreateNonFapl();
  ASSERT_TRUE(dcpl.valid());
  EXPECT_LT(H5Pset_family_offset(dcpl, 128), 0);
  EXPECT_LT(H5Pget_family_offset(dcpl, &offset), 0);
}

TEST(H5PfaplTest, MultiTypeAndEncodeDecode) {
  ScopedPlist fapl = CreateFapl();
  ASSERT_TRUE(fapl.valid());

  H5FD_mem_t mem_type = H5FD_MEM_NTYPES;
  ASSERT_GE(H5Pget_multi_type(fapl, &mem_type), 0);
  EXPECT_EQ(mem_type, H5FD_MEM_DEFAULT);
  EXPECT_GE(H5Pget_multi_type(fapl, nullptr), 0);

  ASSERT_GE(H5Pset_multi_type(fapl, H5FD_MEM_SUPER), 0);
  ASSERT_GE(H5Pget_multi_type(fapl, &mem_type), 0);
  EXPECT_EQ(mem_type, H5FD_MEM_SUPER);

  ScopedPlist decoded = RoundTripEncodeDecode(fapl);
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Pequal(fapl, decoded), 0);
  mem_type = H5FD_MEM_DEFAULT;
  ASSERT_GE(H5Pget_multi_type(decoded, &mem_type), 0);
  EXPECT_EQ(mem_type, H5FD_MEM_SUPER);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Pset_multi_type(H5P_DEFAULT, H5FD_MEM_DRAW), 0);
  EXPECT_LT(H5Pget_multi_type(H5P_DEFAULT, &mem_type), 0);
  EXPECT_LT(H5Pset_multi_type(H5I_INVALID_HID, H5FD_MEM_DRAW), 0);
  EXPECT_LT(H5Pget_multi_type(H5I_INVALID_HID, &mem_type), 0);
  ScopedPlist dcpl = CreateNonFapl();
  ASSERT_TRUE(dcpl.valid());
  EXPECT_LT(H5Pset_multi_type(dcpl, H5FD_MEM_DRAW), 0);
  EXPECT_LT(H5Pget_multi_type(dcpl, &mem_type), 0);
}

TEST(H5PfaplTest, RawDataChunkCacheAndEncodeDecode) {
  ScopedPlist fapl = CreateFapl();
  ASSERT_TRUE(fapl.valid());

  int mdc_nelmts = 42;
  size_t rdcc_nslots = 0;
  size_t rdcc_nbytes = 0;
  double rdcc_w0 = -1.0;
  ASSERT_GE(H5Pget_cache(H5P_FILE_ACCESS_DEFAULT, &mdc_nelmts, &rdcc_nslots, &rdcc_nbytes, &rdcc_w0),
            0);
  EXPECT_EQ(mdc_nelmts, 0);
  EXPECT_EQ(rdcc_nslots, 8191u);
  EXPECT_GE(H5Pget_cache(fapl, nullptr, nullptr, nullptr, nullptr), 0);

  ASSERT_GE(H5Pset_cache(fapl, 99, 1021, 2097152, 0.5), 0);
  ASSERT_GE(H5Pget_cache(fapl, &mdc_nelmts, &rdcc_nslots, &rdcc_nbytes, &rdcc_w0), 0);
  EXPECT_EQ(mdc_nelmts, 0);
  EXPECT_EQ(rdcc_nslots, 1021u);
  EXPECT_EQ(rdcc_nbytes, 2097152u);
  EXPECT_DOUBLE_EQ(rdcc_w0, 0.5);

  ScopedPlist decoded = RoundTripEncodeDecode(fapl);
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Pequal(fapl, decoded), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Pset_cache(fapl, 0, 100, 1024, -0.01), 0);
  EXPECT_LT(H5Pset_cache(fapl, 0, 100, 1024, 1.01), 0);
  EXPECT_LT(H5Pset_cache(H5I_INVALID_HID, 0, 100, 1024, 0.5), 0);
  EXPECT_LT(H5Pget_cache(H5P_DEFAULT, &mdc_nelmts, &rdcc_nslots, &rdcc_nbytes, &rdcc_w0), 0);
  EXPECT_LT(H5Pget_cache(H5I_INVALID_HID, &mdc_nelmts, &rdcc_nslots, &rdcc_nbytes, &rdcc_w0), 0);
  ScopedPlist dcpl = CreateNonFapl();
  ASSERT_TRUE(dcpl.valid());
  EXPECT_LT(H5Pset_cache(dcpl, 0, 100, 1024, 0.5), 0);
  EXPECT_LT(H5Pget_cache(dcpl, &mdc_nelmts, &rdcc_nslots, &rdcc_nbytes, &rdcc_w0), 0);
}

TEST(H5PfaplTest, GcReferencesAndCloseDegreeAndBlockSizes) {
  ScopedPlist fapl = CreateFapl();
  ASSERT_TRUE(fapl.valid());

  unsigned gc_ref = 99;
  ASSERT_GE(H5Pget_gc_references(fapl, &gc_ref), 0);
  EXPECT_EQ(gc_ref, 0u);
  EXPECT_GE(H5Pget_gc_references(fapl, nullptr), 0);
  ASSERT_GE(H5Pset_gc_references(fapl, 1), 0);
  ASSERT_GE(H5Pget_gc_references(fapl, &gc_ref), 0);
  EXPECT_EQ(gc_ref, 1u);

  H5F_close_degree_t degree = H5F_CLOSE_DEFAULT;
  ASSERT_GE(H5Pget_fclose_degree(fapl, &degree), 0);
  EXPECT_EQ(degree, H5F_CLOSE_DEFAULT);
  EXPECT_GE(H5Pget_fclose_degree(fapl, nullptr), 0);
  ASSERT_GE(H5Pset_fclose_degree(fapl, H5F_CLOSE_STRONG), 0);
  ASSERT_GE(H5Pget_fclose_degree(fapl, &degree), 0);
  EXPECT_EQ(degree, H5F_CLOSE_STRONG);

  hsize_t meta_size = 0;
  ASSERT_GE(H5Pget_meta_block_size(fapl, &meta_size), 0);
  EXPECT_EQ(meta_size, 2048u);
  EXPECT_GE(H5Pget_meta_block_size(fapl, nullptr), 0);
  ASSERT_GE(H5Pset_meta_block_size(fapl, 8192), 0);
  ASSERT_GE(H5Pget_meta_block_size(fapl, &meta_size), 0);
  EXPECT_EQ(meta_size, 8192u);

  size_t sieve_size = 0;
  ASSERT_GE(H5Pget_sieve_buf_size(fapl, &sieve_size), 0);
  EXPECT_EQ(sieve_size, 65536u);
  EXPECT_GE(H5Pget_sieve_buf_size(fapl, nullptr), 0);
  ASSERT_GE(H5Pset_sieve_buf_size(fapl, 131072), 0);
  ASSERT_GE(H5Pget_sieve_buf_size(fapl, &sieve_size), 0);
  EXPECT_EQ(sieve_size, 131072u);

  hsize_t sdata_size = 0;
  ASSERT_GE(H5Pget_small_data_block_size(fapl, &sdata_size), 0);
  EXPECT_EQ(sdata_size, 2048u);
  EXPECT_GE(H5Pget_small_data_block_size(fapl, nullptr), 0);
  ASSERT_GE(H5Pset_small_data_block_size(fapl, 4096), 0);
  ASSERT_GE(H5Pget_small_data_block_size(fapl, &sdata_size), 0);
  EXPECT_EQ(sdata_size, 4096u);

  unsigned efc_size = 99;
  ASSERT_GE(H5Pget_elink_file_cache_size(fapl, &efc_size), 0);
  EXPECT_EQ(efc_size, 0u);
  EXPECT_GE(H5Pget_elink_file_cache_size(fapl, nullptr), 0);
  ASSERT_GE(H5Pset_elink_file_cache_size(fapl, 16), 0);
  ASSERT_GE(H5Pget_elink_file_cache_size(fapl, &efc_size), 0);
  EXPECT_EQ(efc_size, 16u);

  ScopedPlist decoded = RoundTripEncodeDecode(fapl);
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Pequal(fapl, decoded), 0);

  H5F_close_degree_t dec_degree = H5F_CLOSE_DEFAULT;
  ASSERT_GE(H5Pget_fclose_degree(decoded, &dec_degree), 0);
  EXPECT_EQ(dec_degree, H5F_CLOSE_STRONG);

  ScopedErrorSilence silence;
  ScopedPlist dcpl = CreateNonFapl();
  ASSERT_TRUE(dcpl.valid());
  EXPECT_LT(H5Pset_gc_references(dcpl, 1), 0);
  EXPECT_LT(H5Pget_gc_references(dcpl, &gc_ref), 0);
  EXPECT_LT(H5Pset_fclose_degree(dcpl, H5F_CLOSE_WEAK), 0);
  EXPECT_LT(H5Pget_fclose_degree(dcpl, &degree), 0);
  EXPECT_LT(H5Pset_meta_block_size(dcpl, 1024), 0);
  EXPECT_LT(H5Pget_meta_block_size(dcpl, &meta_size), 0);
  EXPECT_LT(H5Pset_sieve_buf_size(dcpl, 1024), 0);
  EXPECT_LT(H5Pget_sieve_buf_size(dcpl, &sieve_size), 0);
  EXPECT_LT(H5Pset_small_data_block_size(dcpl, 1024), 0);
  EXPECT_LT(H5Pget_small_data_block_size(dcpl, &sdata_size), 0);
  EXPECT_LT(H5Pset_elink_file_cache_size(dcpl, 4), 0);
  EXPECT_LT(H5Pget_elink_file_cache_size(dcpl, &efc_size), 0);
}

TEST(H5PfaplTest, LibverBoundsMetadataReadAttemptsAndObjectFlushCb) {
  ScopedPlist fapl = CreateFapl();
  ASSERT_TRUE(fapl.valid());

  H5F_libver_t low = H5F_LIBVER_ERROR;
  H5F_libver_t high = H5F_LIBVER_ERROR;
  ASSERT_GE(H5Pget_libver_bounds(H5P_FILE_ACCESS_DEFAULT, &low, &high), 0);
  EXPECT_EQ(low, H5F_LIBVER_V18);
  EXPECT_EQ(high, H5F_LIBVER_LATEST);
  EXPECT_GE(H5Pget_libver_bounds(fapl, nullptr, nullptr), 0);

  ASSERT_GE(H5Pset_libver_bounds(fapl, H5F_LIBVER_V110, H5F_LIBVER_V114), 0);
  ASSERT_GE(H5Pget_libver_bounds(fapl, &low, &high), 0);
  EXPECT_EQ(low, H5F_LIBVER_V110);
  EXPECT_EQ(high, H5F_LIBVER_V114);

  unsigned attempts = 0;
  ASSERT_GE(H5Pget_metadata_read_attempts(H5P_FILE_ACCESS_DEFAULT, &attempts), 0);
  EXPECT_EQ(attempts, static_cast<unsigned>(H5F_METADATA_READ_ATTEMPTS));
  EXPECT_GE(H5Pget_metadata_read_attempts(fapl, nullptr), 0);

  ASSERT_GE(H5Pset_metadata_read_attempts(fapl, 7), 0);
  ASSERT_GE(H5Pget_metadata_read_attempts(fapl, &attempts), 0);
  EXPECT_EQ(attempts, 7u);

  int cb_udata = 123;
  H5F_flush_cb_t flush_fn = [](hid_t, void*) -> herr_t { return 0; };
  ASSERT_GE(H5Pset_object_flush_cb(fapl, flush_fn, &cb_udata), 0);
  H5F_flush_cb_t got_fn = nullptr;
  void* got_udata = nullptr;
  ASSERT_GE(H5Pget_object_flush_cb(fapl, &got_fn, &got_udata), 0);
  EXPECT_EQ(got_fn, flush_fn);
  EXPECT_EQ(got_udata, &cb_udata);
  EXPECT_GE(H5Pget_object_flush_cb(fapl, nullptr, nullptr), 0);
  ASSERT_GE(H5Pset_object_flush_cb(fapl, nullptr, nullptr), 0);

  ScopedPlist decoded = RoundTripEncodeDecode(fapl);
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Pequal(fapl, decoded), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Pget_libver_bounds(H5P_DEFAULT, &low, &high), 0);
  EXPECT_LT(H5Pset_libver_bounds(fapl, static_cast<H5F_libver_t>(-1), H5F_LIBVER_LATEST), 0);
  EXPECT_LT(H5Pset_libver_bounds(fapl, static_cast<H5F_libver_t>(99), H5F_LIBVER_LATEST), 0);
  EXPECT_LT(H5Pset_libver_bounds(fapl, H5F_LIBVER_EARLIEST, static_cast<H5F_libver_t>(-1)), 0);
  EXPECT_LT(H5Pset_libver_bounds(fapl, H5F_LIBVER_EARLIEST, static_cast<H5F_libver_t>(99)), 0);
  EXPECT_LT(H5Pset_libver_bounds(fapl, H5F_LIBVER_EARLIEST, H5F_LIBVER_EARLIEST), 0);
  EXPECT_LT(H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_V18), 0);
  EXPECT_LT(H5Pset_libver_bounds(H5P_DEFAULT, H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);
  EXPECT_LT(H5Pget_metadata_read_attempts(H5P_DEFAULT, &attempts), 0);
  EXPECT_LT(H5Pset_metadata_read_attempts(fapl, 0), 0);
  EXPECT_LT(H5Pset_object_flush_cb(fapl, nullptr, &cb_udata), 0);

  ScopedPlist dcpl = CreateNonFapl();
  ASSERT_TRUE(dcpl.valid());
  EXPECT_LT(H5Pset_libver_bounds(dcpl, H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);
  EXPECT_LT(H5Pget_libver_bounds(dcpl, &low, &high), 0);
  EXPECT_LT(H5Pset_metadata_read_attempts(dcpl, 3), 0);
  EXPECT_LT(H5Pget_metadata_read_attempts(dcpl, &attempts), 0);
  EXPECT_LT(H5Pset_object_flush_cb(dcpl, flush_fn, &cb_udata), 0);
  EXPECT_LT(H5Pget_object_flush_cb(dcpl, &got_fn, &got_udata), 0);
}

// =============================================================================
// Evict on Close, File Locking, RFIC Flags, Page Buffer Size
// =============================================================================

TEST(H5PfaplTest, EvictOnCloseFileLockingAndRficFlags) {
  ScopedPlist fapl = CreateFapl();
  ASSERT_TRUE(fapl.valid());

  bool evict = true;
  ASSERT_GE(H5Pget_evict_on_close(fapl, &evict), 0);
  EXPECT_FALSE(evict);
  ASSERT_GE(H5Pset_evict_on_close(fapl, true), 0);
  ASSERT_GE(H5Pget_evict_on_close(fapl, &evict), 0);
  EXPECT_TRUE(evict);

  bool use_locking = true;
  bool ignore_disabled = false;
  ASSERT_GE(H5Pget_file_locking(H5P_DEFAULT, &use_locking, &ignore_disabled), 0);
  EXPECT_FALSE(use_locking);
  EXPECT_TRUE(ignore_disabled);

  ASSERT_GE(H5Pset_file_locking(fapl, true, false), 0);
  ASSERT_GE(H5Pget_file_locking(fapl, &use_locking, &ignore_disabled), 0);
  EXPECT_TRUE(use_locking);
  EXPECT_FALSE(ignore_disabled);

  uint64_t rfic = 999;
  ASSERT_GE(H5Pget_relax_file_integrity_checks(H5P_DEFAULT, &rfic), 0);
  EXPECT_EQ(rfic, 0u);
  EXPECT_GE(H5Pget_relax_file_integrity_checks(fapl, nullptr), 0);
  ASSERT_GE(H5Pset_relax_file_integrity_checks(fapl, H5F_RFIC_UNUSUAL_NUM_UNUSED_NUMERIC_BITS), 0);
  ASSERT_GE(H5Pget_relax_file_integrity_checks(fapl, &rfic), 0);
  EXPECT_EQ(rfic, H5F_RFIC_UNUSUAL_NUM_UNUSED_NUMERIC_BITS);

  ScopedPlist decoded = RoundTripEncodeDecode(fapl);
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Pequal(fapl, decoded), 0);

  ScopedErrorSilence silence;
  ScopedPlist dcpl = CreateNonFapl();
  ASSERT_TRUE(dcpl.valid());
  EXPECT_LT(H5Pset_evict_on_close(dcpl, true), 0);
  EXPECT_LT(H5Pget_evict_on_close(dcpl, &evict), 0);
  EXPECT_LT(H5Pset_file_locking(dcpl, false, false), 0);
  EXPECT_LT(H5Pget_file_locking(dcpl, &use_locking, &ignore_disabled), 0);
  EXPECT_LT(H5Pset_relax_file_integrity_checks(H5P_DEFAULT, H5F_RFIC_ALL), 0);
  EXPECT_LT(H5Pset_relax_file_integrity_checks(fapl, H5F_RFIC_ALL | 0x4ULL), 0);
  EXPECT_LT(H5Pset_relax_file_integrity_checks(dcpl, H5F_RFIC_ALL), 0);
  EXPECT_LT(H5Pget_relax_file_integrity_checks(dcpl, &rfic), 0);
}

TEST(H5PfaplTest, PageBufferSizeAndVersionedEncodeDecode) {
  ScopedPlist fapl_def = CreateFapl();
  ASSERT_TRUE(fapl_def.valid());

  size_t pb_size = 999;
  unsigned min_meta = 99;
  unsigned min_raw = 99;
  ASSERT_GE(H5Pget_page_buffer_size(H5P_FILE_ACCESS_DEFAULT, &pb_size, &min_meta, &min_raw), 0);
  EXPECT_EQ(pb_size, 0u);
  EXPECT_EQ(min_meta, 0u);
  EXPECT_EQ(min_raw, 0u);
  EXPECT_GE(H5Pget_page_buffer_size(fapl_def, nullptr, nullptr, nullptr), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pget_page_buffer_size(H5P_DEFAULT, &pb_size, &min_meta, &min_raw), 0);
  }

  // Encode/decode default (H5F_PAGE_BUFFER_SIZE_DEFAULT) with low < V200.
  ScopedPlist enc_v18 = CreateFapl();
  ASSERT_TRUE(enc_v18.valid());
  ASSERT_GE(H5Pset_libver_bounds(enc_v18, H5F_LIBVER_V18, H5F_LIBVER_V114), 0);

  ScopedPlist dec_v18_def = RoundTripEncodeDecode(fapl_def, enc_v18);
  ASSERT_TRUE(dec_v18_def.valid());
  EXPECT_GT(H5Pequal(fapl_def, dec_v18_def), 0);

  // Encode/decode explicit 4096 with low < V200.
  ScopedPlist fapl_4k = CreateFapl();
  ASSERT_TRUE(fapl_4k.valid());
  ASSERT_GE(H5Pset_page_buffer_size(fapl_4k, 4096, 25, 30), 0);
  ASSERT_GE(H5Pget_page_buffer_size(fapl_4k, &pb_size, &min_meta, &min_raw), 0);
  EXPECT_EQ(pb_size, 4096u);
  EXPECT_EQ(min_meta, 25u);
  EXPECT_EQ(min_raw, 30u);

  ScopedPlist dec_v18_4k = RoundTripEncodeDecode(fapl_4k, enc_v18);
  ASSERT_TRUE(dec_v18_4k.valid());
  EXPECT_GT(H5Pequal(fapl_4k, dec_v18_4k), 0);

  // Encode/decode with low >= H5F_LIBVER_V200 for both explicit 0 and 4096.
  ScopedPlist enc_v200 = CreateFapl();
  ASSERT_TRUE(enc_v200.valid());
  ASSERT_GE(H5Pset_libver_bounds(enc_v200, H5F_LIBVER_V200, H5F_LIBVER_LATEST), 0);

  ScopedPlist fapl_zero = CreateFapl();
  ASSERT_TRUE(fapl_zero.valid());
  ASSERT_GE(H5Pset_page_buffer_size(fapl_zero, 0, 10, 20), 0);
  ScopedPlist dec_v200_zero = RoundTripEncodeDecode(fapl_zero, enc_v200);
  ASSERT_TRUE(dec_v200_zero.valid());
  EXPECT_GT(H5Pequal(fapl_zero, dec_v200_zero), 0);

  ScopedPlist dec_v200_4k = RoundTripEncodeDecode(fapl_4k, enc_v200);
  ASSERT_TRUE(dec_v200_4k.valid());
  EXPECT_GT(H5Pequal(fapl_4k, dec_v200_4k), 0);

  // Default H5F_PAGE_BUFFER_SIZE_DEFAULT encoded with V200.
  ScopedPlist dec_v200_def = RoundTripEncodeDecode(fapl_def, enc_v200);
  ASSERT_TRUE(dec_v200_def.valid());
  EXPECT_GT(H5Pequal(fapl_def, dec_v200_def), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Pset_page_buffer_size(fapl_def, 4096, 101, 0), 0);
  EXPECT_LT(H5Pset_page_buffer_size(fapl_def, 4096, 0, 101), 0);
  EXPECT_LT(H5Pset_page_buffer_size(fapl_def, 4096, 60, 41), 0);
  EXPECT_LT(H5Pset_page_buffer_size(H5I_INVALID_HID, 4096, 10, 10), 0);
  EXPECT_LT(H5Pget_page_buffer_size(H5I_INVALID_HID, &pb_size, &min_meta, &min_raw), 0);
  ScopedPlist dcpl = CreateNonFapl();
  ASSERT_TRUE(dcpl.valid());
  EXPECT_LT(H5Pset_page_buffer_size(dcpl, 4096, 10, 10), 0);
  EXPECT_LT(H5Pget_page_buffer_size(dcpl, &pb_size, &min_meta, &min_raw), 0);
}

// =============================================================================
// MDC Cache Config & Cache Image Config: Validation, Cmp & Encode/Decode
// =============================================================================

TEST(H5PfaplTest, MdcConfigAndCacheImageConfigValidationAndCmpAndEncDec) {
  ScopedPlist fapl_base = CreateFapl();
  ASSERT_TRUE(fapl_base.valid());

  H5AC_cache_config_t base{};
  base.version = H5AC__CURR_CACHE_CONFIG_VERSION;
  ASSERT_GE(H5Pget_mdc_config(fapl_base, &base), 0);

  auto set_and_cmp = [&](auto mutator) {
    ScopedPlist f2 = CreateFapl();
    if (!f2.valid()) return;
    H5AC_cache_config_t cfg = base;
    mutator(cfg);
    ASSERT_GE(H5Pset_mdc_config(f2, &cfg), 0) << "mutator produced invalid config";
    EXPECT_EQ(H5Pequal(fapl_base, f2), 0);
    EXPECT_EQ(H5Pequal(f2, fapl_base), 0);
  };

  set_and_cmp([](H5AC_cache_config_t& c) { c.rpt_fcn_enabled = true; });
  set_and_cmp([](H5AC_cache_config_t& c) {
    c.evictions_enabled = false;
    c.incr_mode = H5C_incr__off;
    c.flash_incr_mode = H5C_flash_incr__off;
    c.decr_mode = H5C_decr__off;
  });
  set_and_cmp([](H5AC_cache_config_t& c) { c.set_initial_size = false; });
  set_and_cmp([](H5AC_cache_config_t& c) { c.initial_size = 4 * 1024 * 1024; });
  set_and_cmp([](H5AC_cache_config_t& c) { c.min_clean_fraction = 0.25; });
  set_and_cmp([](H5AC_cache_config_t& c) { c.max_size = 16 * 1024 * 1024; });
  set_and_cmp([](H5AC_cache_config_t& c) { c.min_size = 512 * 1024; });
  set_and_cmp([](H5AC_cache_config_t& c) { c.epoch_length = 20000; });
  set_and_cmp([](H5AC_cache_config_t& c) { c.incr_mode = H5C_incr__off; });
  set_and_cmp([](H5AC_cache_config_t& c) { c.lower_hr_threshold = 0.8; });
  set_and_cmp([](H5AC_cache_config_t& c) { c.increment = 1.5; });
  set_and_cmp([](H5AC_cache_config_t& c) { c.apply_max_increment = false; });
  set_and_cmp([](H5AC_cache_config_t& c) { c.max_increment = 2 * 1024 * 1024; });
  set_and_cmp([](H5AC_cache_config_t& c) { c.flash_incr_mode = H5C_flash_incr__off; });
  set_and_cmp([](H5AC_cache_config_t& c) { c.flash_multiple = 2.0; });
  set_and_cmp([](H5AC_cache_config_t& c) { c.flash_threshold = 0.5; });
  set_and_cmp([](H5AC_cache_config_t& c) { c.decr_mode = H5C_decr__off; });
  set_and_cmp([](H5AC_cache_config_t& c) { c.upper_hr_threshold = 0.95; });
  set_and_cmp([](H5AC_cache_config_t& c) { c.decrement = 0.8; });
  set_and_cmp([](H5AC_cache_config_t& c) { c.apply_max_decrement = false; });
  set_and_cmp([](H5AC_cache_config_t& c) { c.max_decrement = 512 * 1024; });
  set_and_cmp([](H5AC_cache_config_t& c) { c.epochs_before_eviction = 5; });
  set_and_cmp([](H5AC_cache_config_t& c) { c.apply_empty_reserve = false; });
  set_and_cmp([](H5AC_cache_config_t& c) { c.empty_reserve = 0.2; });
  set_and_cmp([](H5AC_cache_config_t& c) { c.dirty_bytes_threshold = 128 * 1024; });
  set_and_cmp([](H5AC_cache_config_t& c) {
    c.metadata_write_strategy = H5AC_METADATA_WRITE_STRATEGY__PROCESS_0_ONLY;
  });

  // Encode/decode modified MDC config & MDC image config.
  ScopedPlist fapl_mod = CreateFapl();
  ASSERT_TRUE(fapl_mod.valid());
  H5AC_cache_config_t mod = base;
  mod.initial_size = 4 * 1024 * 1024;
  mod.dirty_bytes_threshold = 128 * 1024;
  ASSERT_GE(H5Pset_mdc_config(fapl_mod, &mod), 0);

  H5AC_cache_image_config_t img_base{};
  img_base.version = H5AC__CURR_CACHE_IMAGE_CONFIG_VERSION;
  ASSERT_GE(H5Pget_mdc_image_config(fapl_base, &img_base), 0);

  H5AC_cache_image_config_t img_mod = img_base;
  img_mod.generate_image = true;
  img_mod.save_resize_status = false;
  img_mod.entry_ageout = H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE;
  ASSERT_GE(H5Pset_mdc_image_config(fapl_mod, &img_mod), 0);

  EXPECT_EQ(H5Pequal(fapl_base, fapl_mod), 0);
  EXPECT_EQ(H5Pequal(fapl_mod, fapl_base), 0);

  // Compare each cache_image_config field individually in both orders.
  {
    ScopedPlist f_gen = CreateFapl();
    H5AC_cache_image_config_t c1 = img_base;
    c1.generate_image = true;
    ASSERT_GE(H5Pset_mdc_image_config(f_gen, &c1), 0);
    EXPECT_EQ(H5Pequal(fapl_base, f_gen), 0);
    EXPECT_EQ(H5Pequal(f_gen, fapl_base), 0);

    // save_resize_status != false and entry_ageout != NONE are currently disallowed
    // by H5Pset_mdc_image_config validation, so exercise _cmp via H5P_set too.
    ScopedPlist f_res = CreateFapl();
    H5AC_cache_image_config_t c2 = img_base;
    c2.save_resize_status = true;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Pset_mdc_image_config(f_res, &c2), 0);
    }
    auto* p_res = static_cast<H5P_genplist_t*>(H5I_object(f_res));
    ASSERT_NE(p_res, nullptr);
    ASSERT_GE(H5P_set(p_res, H5F_ACS_META_CACHE_INIT_IMAGE_CONFIG_NAME, &c2), 0);
    EXPECT_EQ(H5Pequal(fapl_base, f_res), 0);
    EXPECT_EQ(H5Pequal(f_res, fapl_base), 0);

    ScopedPlist f_age = CreateFapl();
    H5AC_cache_image_config_t c3 = img_base;
    c3.entry_ageout = 4;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Pset_mdc_image_config(f_age, &c3), 0);
    }
    auto* p_age = static_cast<H5P_genplist_t*>(H5I_object(f_age));
    ASSERT_NE(p_age, nullptr);
    ASSERT_GE(H5P_set(p_age, H5F_ACS_META_CACHE_INIT_IMAGE_CONFIG_NAME, &c3), 0);
    EXPECT_EQ(H5Pequal(fapl_base, f_age), 0);
    EXPECT_EQ(H5Pequal(f_age, fapl_base), 0);

    ScopedPlist f_ver = CreateFapl();
    H5AC_cache_image_config_t c4 = img_base;
    c4.version = img_base.version + 1;
    auto* p_ver = static_cast<H5P_genplist_t*>(H5I_object(f_ver));
    ASSERT_NE(p_ver, nullptr);
    ASSERT_GE(H5P_set(p_ver, H5F_ACS_META_CACHE_INIT_IMAGE_CONFIG_NAME, &c4), 0);
    EXPECT_EQ(H5Pequal(fapl_base, f_ver), 0);
    EXPECT_EQ(H5Pequal(f_ver, fapl_base), 0);
  }

  std::vector<uint8_t> encoded = EncodePlist(fapl_mod);
  ASSERT_FALSE(encoded.empty());
  ScopedPlist decoded(H5Pdecode(encoded.data()));
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Pequal(fapl_mod, decoded), 0);

  // Corrupt encoded sizeof(unsigned) / sizeof(double) header bytes to hit
  // cache_config_dec and cache_image_config_dec error branches.
  ScopedErrorSilence silence;
  {
    const std::string tag = std::string(H5F_ACS_META_CACHE_INIT_CONFIG_NAME) + '\0';
    size_t pos = FindSubsequence(encoded, tag);
    ASSERT_NE(pos, static_cast<size_t>(-1));
    size_t val_pos = pos + tag.size();

    std::vector<uint8_t> bad_u = encoded;
    bad_u[val_pos] = static_cast<uint8_t>(sizeof(unsigned) + 1);
    EXPECT_LT(H5Pdecode(bad_u.data()), 0);

    std::vector<uint8_t> bad_d = encoded;
    bad_d[val_pos + 1] = static_cast<uint8_t>(sizeof(double) + 1);
    EXPECT_LT(H5Pdecode(bad_d.data()), 0);
  }
  {
    const std::string tag = std::string(H5F_ACS_META_CACHE_INIT_IMAGE_CONFIG_NAME) + '\0';
    size_t pos = FindSubsequence(encoded, tag);
    ASSERT_NE(pos, static_cast<size_t>(-1));
    size_t val_pos = pos + tag.size();

    std::vector<uint8_t> bad_u = encoded;
    bad_u[val_pos] = static_cast<uint8_t>(sizeof(unsigned) + 1);
    EXPECT_LT(H5Pdecode(bad_u.data()), 0);
  }

  // API negative cases for get/set mdc_config & mdc_image_config.
  H5AC_cache_config_t bad_cfg = base;
  bad_cfg.version = 999;
  EXPECT_LT(H5Pget_mdc_config(H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5Pget_mdc_config(H5P_DEFAULT, &bad_cfg), 0);
  EXPECT_LT(H5Pget_mdc_config(H5I_INVALID_HID, &base), 0);
  EXPECT_LT(H5Pset_mdc_config(fapl_base, nullptr), 0);
  EXPECT_LT(H5Pset_mdc_config(fapl_base, &bad_cfg), 0);
  EXPECT_LT(H5Pset_mdc_config(H5I_INVALID_HID, &base), 0);

  H5AC_cache_image_config_t bad_img = img_base;
  bad_img.version = 999;
  EXPECT_LT(H5Pget_mdc_image_config(H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5Pget_mdc_image_config(H5P_DEFAULT, &bad_img), 0);
  EXPECT_LT(H5Pget_mdc_image_config(H5I_INVALID_HID, &img_base), 0);
  EXPECT_LT(H5Pset_mdc_image_config(fapl_base, nullptr), 0);
  EXPECT_LT(H5Pset_mdc_image_config(fapl_base, &bad_img), 0);
  EXPECT_LT(H5Pset_mdc_image_config(H5I_INVALID_HID, &img_base), 0);
}

// =============================================================================
// MDC Log Options & Location Property Callbacks
// =============================================================================

TEST(H5PfaplTest, MdcLogOptionsCallbacksAndEncodeDecode) {
  ScopedPlist fapl = CreateFapl();
  ASSERT_TRUE(fapl.valid());

  bool enabled = true;
  bool start_on_access = true;
  size_t loc_size = 999;
  ASSERT_GE(H5Pget_mdc_log_options(fapl, &enabled, nullptr, &loc_size, &start_on_access), 0);
  EXPECT_FALSE(enabled);
  EXPECT_FALSE(start_on_access);
  EXPECT_EQ(loc_size, 0u);
  EXPECT_GE(H5Pget_mdc_log_options(fapl, nullptr, nullptr, nullptr, nullptr), 0);

  // Encode/decode default FAPL (location == NULL) exercises len == 0 dec branch.
  {
    ScopedPlist fapl_flag_only = CreateFapl();
    ASSERT_TRUE(fapl_flag_only.valid());
    bool val = true;
    auto* plist = static_cast<H5P_genplist_t*>(H5I_object(fapl_flag_only));
    ASSERT_NE(plist, nullptr);
    ASSERT_GE(H5P_set(plist, H5F_ACS_USE_MDC_LOGGING_NAME, &val), 0);
    ScopedPlist dec_null = RoundTripEncodeDecode(fapl_flag_only);
    ASSERT_TRUE(dec_null.valid());
    EXPECT_GT(H5Pequal(fapl_flag_only, dec_null), 0);
  }

  ASSERT_GE(H5Pset_mdc_log_options(fapl, true, "mdc_test.log", true), 0);

  loc_size = 0;
  ASSERT_GE(H5Pget_mdc_log_options(fapl, &enabled, nullptr, &loc_size, &start_on_access), 0);
  EXPECT_TRUE(enabled);
  EXPECT_TRUE(start_on_access);
  ASSERT_EQ(loc_size, std::strlen("mdc_test.log") + 1);

  std::string got_loc(loc_size, '\0');
  ASSERT_GE(H5Pget_mdc_log_options(fapl, &enabled, got_loc.data(), &loc_size, &start_on_access), 0);
  EXPECT_STREQ(got_loc.c_str(), "mdc_test.log");

  // Copy, compare (equal, null vs non-null, non-null vs null, diff strings), remove, encode/decode.
  ScopedPlist copied(H5Pcopy(fapl));
  ASSERT_TRUE(copied.valid());
  EXPECT_GT(H5Pequal(fapl, copied), 0);

  ScopedPlist empty_loc = CreateFapl();
  ASSERT_TRUE(empty_loc.valid());
  EXPECT_EQ(H5Pequal(empty_loc, fapl), 0);
  EXPECT_EQ(H5Pequal(fapl, empty_loc), 0);

  ScopedPlist other_loc = CreateFapl();
  ASSERT_TRUE(other_loc.valid());
  ASSERT_GE(H5Pset_mdc_log_options(other_loc, true, "other_mdc.log", true), 0);
  EXPECT_EQ(H5Pequal(fapl, other_loc), 0);
  EXPECT_EQ(H5Pequal(other_loc, fapl), 0);

  ScopedPlist decoded = RoundTripEncodeDecode(fapl);
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Pequal(fapl, decoded), 0);

  auto* copied_plist = static_cast<H5P_genplist_t*>(H5I_object(copied));
  ASSERT_NE(copied_plist, nullptr);
  EXPECT_GE(H5P_remove(copied_plist, H5F_ACS_MDC_LOG_LOCATION_NAME), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Pset_mdc_log_options(H5P_DEFAULT, true, "x.log", false), 0);
  EXPECT_LT(H5Pset_mdc_log_options(fapl, true, nullptr, false), 0);
  EXPECT_LT(H5Pset_mdc_log_options(H5I_INVALID_HID, true, "x.log", false), 0);
  EXPECT_LT(H5Pget_mdc_log_options(H5I_INVALID_HID, &enabled, nullptr, &loc_size, &start_on_access), 0);
  ScopedPlist dcpl = CreateNonFapl();
  ASSERT_TRUE(dcpl.valid());
  EXPECT_LT(H5Pset_mdc_log_options(dcpl, true, "x.log", false), 0);
  EXPECT_LT(H5Pget_mdc_log_options(dcpl, &enabled, nullptr, &loc_size, &start_on_access), 0);
}

// =============================================================================
// File Image & File Image Callbacks
// =============================================================================

struct ImageTracker {
  bool fail_malloc = false;
  bool fail_memcpy = false;
  bool fail_free = false;
  bool fail_udata_copy = false;
  bool fail_udata_free = false;
  int udata_tag = 0;
  std::vector<void*> live_allocs;
};

struct TrackedUdata {
  ImageTracker* tracker;
};

void* TestImageMalloc(size_t size, H5FD_file_image_op_t, void* udata) {
  auto* u = static_cast<TrackedUdata*>(udata);
  if (u && u->tracker && u->tracker->fail_malloc) {
    return nullptr;
  }
  void* ptr = std::malloc(size);
  if (ptr && u && u->tracker) {
    u->tracker->live_allocs.push_back(ptr);
  }
  return ptr;
}

void* TestImageMemcpy(void* dest, const void* src, size_t size, H5FD_file_image_op_t, void* udata) {
  auto* u = static_cast<TrackedUdata*>(udata);
  if (u && u->tracker && u->tracker->fail_memcpy) {
    return nullptr;
  }
  return std::memcpy(dest, src, size);
}

herr_t TestImageFree(void* ptr, H5FD_file_image_op_t, void* udata) {
  auto* u = static_cast<TrackedUdata*>(udata);
  if (u && u->tracker && u->tracker->fail_free) {
    return -1;
  }
  if (u && u->tracker) {
    auto& allocs = u->tracker->live_allocs;
    allocs.erase(std::remove(allocs.begin(), allocs.end(), ptr), allocs.end());
  }
  std::free(ptr);
  return 0;
}

void* TestUdataCopy(void* udata) {
  auto* u = static_cast<TrackedUdata*>(udata);
  if (u && u->tracker && u->tracker->fail_udata_copy) {
    return nullptr;
  }
  auto* copy = new TrackedUdata{u ? u->tracker : nullptr};
  return copy;
}

herr_t TestUdataFree(void* udata) {
  auto* u = static_cast<TrackedUdata*>(udata);
  if (!u) {
    return 0;
  }
  if (u->tracker && u->tracker->fail_udata_free) {
    return -1;
  }
  delete u;
  return 0;
}

void* AlternateImageMalloc(size_t size, H5FD_file_image_op_t op, void* udata) {
  return TestImageMalloc(size, op, udata);
}
void* AlternateImageMemcpy(void* d, const void* s, size_t n, H5FD_file_image_op_t op, void* u) {
  return TestImageMemcpy(d, s, n, op, u);
}
void* DummyImageRealloc(void* ptr, size_t size, H5FD_file_image_op_t, void*) {
  return std::realloc(ptr, size);
}
herr_t AlternateImageFree(void* ptr, H5FD_file_image_op_t op, void* udata) {
  return TestImageFree(ptr, op, udata);
}
void* PassthruUdataCopy(void* udata) { return udata; }
herr_t PassthruUdataFree(void*) { return 0; }
void* AlternatePassthruUdataCopy(void* udata) { return udata; }
herr_t AlternatePassthruUdataFree(void*) { return 0; }

TEST(H5PfaplTest, FileImagePlainAndCallbacksAndErrors) {
  // Always use >= 128 byte buffers when calling H5Pequal() because
  // H5P__facc_file_image_info_cmp compares `size == sizeof(H5FD_file_image_info_t)` bytes.
  std::vector<uint8_t> buf1(128, 0x11);
  std::vector<uint8_t> buf2(128, 0x22);
  std::vector<uint8_t> buf_large(256, 0x33);

  // Plain set / overwrite / get / compare / clear without callbacks.
  {
    ScopedPlist fapl = CreateFapl();
    ASSERT_TRUE(fapl.valid());

    void* got_buf = reinterpret_cast<void*>(0x1);
    size_t got_len = 99;
    ASSERT_GE(H5Pget_file_image(fapl, &got_buf, &got_len), 0);
    EXPECT_EQ(got_buf, nullptr);
    EXPECT_EQ(got_len, 0u);
    EXPECT_GE(H5Pget_file_image(fapl, nullptr, nullptr), 0);

    ASSERT_GE(H5Pset_file_image(fapl, buf1.data(), buf1.size()), 0);
    ASSERT_GE(H5Pset_file_image(fapl, buf2.data(), buf2.size()), 0);

    ASSERT_GE(H5Pget_file_image(fapl, &got_buf, &got_len), 0);
    ASSERT_NE(got_buf, nullptr);
    absl::Cleanup free_got = [&] { H5free_memory(got_buf); };
    EXPECT_EQ(got_len, buf2.size());
    EXPECT_EQ(std::memcmp(got_buf, buf2.data(), buf2.size()), 0);

    ScopedPlist fapl_copy(H5Pcopy(fapl));
    ASSERT_TRUE(fapl_copy.valid());
    EXPECT_GT(H5Pequal(fapl, fapl_copy), 0);

    ScopedPlist fapl_empty = CreateFapl();
    ASSERT_TRUE(fapl_empty.valid());
    EXPECT_EQ(H5Pequal(fapl_empty, fapl), 0);
    EXPECT_EQ(H5Pequal(fapl, fapl_empty), 0);

    // Setting callbacks when an image is already set is disallowed.
    {
      ScopedErrorSilence silence;
      H5FD_file_image_callbacks_t cb{};
      EXPECT_LT(H5Pset_file_image_callbacks(fapl, &cb), 0);
    }

    // Clear existing image with (nullptr, 0).
    ASSERT_GE(H5Pset_file_image(fapl, nullptr, 0), 0);
    EXPECT_GT(H5Pequal(fapl, fapl_empty), 0);
  }

  // Full custom callback lifecycle + error paths.
  ImageTracker tracker;
  absl::Cleanup free_leftovers = [&] {
    for (void* p : tracker.live_allocs) {
      std::free(p);
    }
    tracker.live_allocs.clear();
  };

  TrackedUdata initial_udata{&tracker};
  H5FD_file_image_callbacks_t cb{};
  cb.image_malloc = TestImageMalloc;
  cb.image_memcpy = TestImageMemcpy;
  cb.image_realloc = nullptr;
  cb.image_free = TestImageFree;
  cb.udata_copy = TestUdataCopy;
  cb.udata_free = TestUdataFree;
  cb.udata = &initial_udata;

  {
    ScopedPlist fapl = CreateFapl();
    ASSERT_TRUE(fapl.valid());

    // Error: udata set without both udata_copy and udata_free.
    {
      ScopedErrorSilence silence;
      H5FD_file_image_callbacks_t bad_cb{};
      bad_cb.udata = &initial_udata;
      EXPECT_LT(H5Pset_file_image_callbacks(fapl, &bad_cb), 0);
      bad_cb.udata_copy = TestUdataCopy;
      EXPECT_LT(H5Pset_file_image_callbacks(fapl, &bad_cb), 0);
      EXPECT_LT(H5Pset_file_image_callbacks(fapl, nullptr), 0);
    }

    ASSERT_GE(H5Pset_file_image_callbacks(fapl, &cb), 0);

    // Replace existing callbacks with udata -> invokes old udata_free.
    ASSERT_GE(H5Pset_file_image_callbacks(fapl, &cb), 0);

    H5FD_file_image_callbacks_t got_cb{};
    ASSERT_GE(H5Pget_file_image_callbacks(fapl, &got_cb), 0);
    ASSERT_NE(got_cb.udata, nullptr);
    EXPECT_NE(got_cb.udata, &initial_udata);
    EXPECT_GE(TestUdataFree(got_cb.udata), 0);

    ASSERT_GE(H5Pset_file_image(fapl, buf1.data(), buf1.size()), 0);
    EXPECT_EQ(tracker.live_allocs.size(), 1u);

    // Retrieve with custom callbacks.
    void* out_ptr = nullptr;
    size_t out_len = 0;
    ASSERT_GE(H5Pget_file_image(fapl, &out_ptr, &out_len), 0);
    ASSERT_NE(out_ptr, nullptr);
    EXPECT_EQ(out_len, buf1.size());
    EXPECT_GE(TestImageFree(out_ptr, H5FD_FILE_IMAGE_OP_PROPERTY_LIST_GET, &initial_udata), 0);

    // Trigger H5P__facc_file_image_info_get via H5P_get.
    {
      auto* plist = static_cast<H5P_genplist_t*>(H5I_object(fapl));
      ASSERT_NE(plist, nullptr);
      H5FD_file_image_info_t direct_info{};
      ASSERT_GE(H5P_get(plist, H5F_ACS_FILE_IMAGE_INFO_NAME, &direct_info), 0);
      EXPECT_EQ(direct_info.size, buf1.size());
      ASSERT_NE(direct_info.buffer, nullptr);
      EXPECT_GE(TestImageFree(direct_info.buffer, H5FD_FILE_IMAGE_OP_PROPERTY_LIST_GET, &initial_udata), 0);
      EXPECT_GE(TestUdataFree(direct_info.callbacks.udata), 0);
    }

    // Overwrite image when callbacks are set.
    ASSERT_GE(H5Pset_file_image(fapl, buf2.data(), buf2.size()), 0);
    EXPECT_EQ(tracker.live_allocs.size(), 1u);

    // Copy plist and compare: different udata pointers compare unequal (0).
    ScopedPlist fapl_copy(H5Pcopy(fapl));
    ASSERT_TRUE(fapl_copy.valid());
    EXPECT_EQ(tracker.live_allocs.size(), 2u);
    EXPECT_EQ(H5Pequal(fapl, fapl_copy), 0);

    // Delete property from copy -> invokes H5P__facc_file_image_info_del.
    auto* copy_plist = static_cast<H5P_genplist_t*>(H5I_object(fapl_copy));
    ASSERT_NE(copy_plist, nullptr);
    ASSERT_GE(H5P_remove(copy_plist, H5F_ACS_FILE_IMAGE_INFO_NAME), 0);
    EXPECT_EQ(tracker.live_allocs.size(), 1u);

    // Inject callback failure modes while cleanup stays intact.
    {
      ScopedErrorSilence silence;
      auto* fapl_plist = static_cast<H5P_genplist_t*>(H5I_object(fapl));
      ASSERT_NE(fapl_plist, nullptr);
      H5FD_file_image_info_t direct_fail_info{};

      tracker.fail_malloc = true;
      EXPECT_LT(H5Pget_file_image(fapl, &out_ptr, &out_len), 0);
      EXPECT_LT(H5P_get(fapl_plist, H5F_ACS_FILE_IMAGE_INFO_NAME, &direct_fail_info), 0);
      // Invoke the property copy callback directly rather than failing mid-list
      // inside H5Pcopy(), which upstream leaves class->plists and unseen
      // default property refs out of balance in H5P_close().
      H5P_genprop_t* img_prop = H5P__find_prop_plist(fapl_plist, H5F_ACS_FILE_IMAGE_INFO_NAME);
      ASSERT_NE(img_prop, nullptr);
      ASSERT_NE(img_prop->copy, nullptr);
      ASSERT_GE(H5P_peek(fapl_plist, H5F_ACS_FILE_IMAGE_INFO_NAME, &direct_fail_info), 0);
      EXPECT_LT(img_prop->copy(img_prop->name, img_prop->size, &direct_fail_info), 0);
      tracker.fail_malloc = false;

      tracker.fail_memcpy = true;
      EXPECT_LT(H5Pget_file_image(fapl, &out_ptr, &out_len), 0);
      direct_fail_info = {};
      EXPECT_LT(H5P_get(fapl_plist, H5F_ACS_FILE_IMAGE_INFO_NAME, &direct_fail_info), 0);
      if (direct_fail_info.buffer) {
        TestImageFree(direct_fail_info.buffer, H5FD_FILE_IMAGE_OP_PROPERTY_LIST_GET,
                      &initial_udata);
      }
      tracker.fail_memcpy = false;

      // Clear image first so fail_malloc/fail_memcpy in H5Pset_file_image don't fail on image_free.
      ASSERT_GE(H5Pset_file_image(fapl, nullptr, 0), 0);
      tracker.fail_malloc = true;
      EXPECT_LT(H5Pset_file_image(fapl, buf1.data(), buf1.size()), 0);
      tracker.fail_malloc = false;

      tracker.fail_memcpy = true;
      EXPECT_LT(H5Pset_file_image(fapl, buf1.data(), buf1.size()), 0);
      tracker.fail_memcpy = false;

      ASSERT_GE(H5Pset_file_image(fapl, buf1.data(), buf1.size()), 0);
      tracker.fail_free = true;
      EXPECT_LT(H5Pset_file_image(fapl, buf2.data(), buf2.size()), 0);
      tracker.fail_free = false;
      // Clear cleanly after resetting fail_free.
      ASSERT_GE(H5Pset_file_image(fapl, nullptr, 0), 0);

      tracker.fail_udata_copy = true;
      EXPECT_LT(H5Pget_file_image_callbacks(fapl, &got_cb), 0);
      ScopedPlist fapl_fresh_ucopy = CreateFapl();
      ASSERT_TRUE(fapl_fresh_ucopy.valid());
      EXPECT_LT(H5Pset_file_image_callbacks(fapl_fresh_ucopy, &cb), 0);
      tracker.fail_udata_copy = false;

      // Replace existing udata while fail_udata_free is active.
      ScopedPlist fapl_udata = CreateFapl();
      ASSERT_TRUE(fapl_udata.valid());
      ASSERT_GE(H5Pset_file_image_callbacks(fapl_udata, &cb), 0);
      tracker.fail_udata_free = true;
      EXPECT_LT(H5Pset_file_image_callbacks(fapl_udata, &cb), 0);
      auto* uplist = static_cast<H5P_genplist_t*>(H5I_object(fapl_udata));
      ASSERT_NE(uplist, nullptr);
      H5FD_file_image_info_t uinfo{};
      ASSERT_GE(H5P_peek(uplist, H5F_ACS_FILE_IMAGE_INFO_NAME, &uinfo), 0);
      tracker.fail_udata_free = false;
    }

    // Exercise H5P__file_image_info_free failures safely via H5P_remove on dedicated plists.
    // Because H5P_remove keeps the property intact when del fails, resetting the fault
    // flags lets ScopedPlist cleanup release the image and udata cleanly via close().
    {
      ScopedErrorSilence silence;
      ScopedPlist f_fail_free = CreateFapl();
      ASSERT_GE(H5Pset_file_image_callbacks(f_fail_free, &cb), 0);
      ASSERT_GE(H5Pset_file_image(f_fail_free, buf1.data(), buf1.size()), 0);
      auto* plist1 = static_cast<H5P_genplist_t*>(H5I_object(f_fail_free));
      tracker.fail_free = true;
      EXPECT_LT(H5P_remove(plist1, H5F_ACS_FILE_IMAGE_INFO_NAME), 0);
      tracker.fail_free = false;

      ScopedPlist f_fail_ufree = CreateFapl();
      ASSERT_GE(H5Pset_file_image_callbacks(f_fail_ufree, &cb), 0);
      auto* plist2 = static_cast<H5P_genplist_t*>(H5I_object(f_fail_ufree));
      tracker.fail_udata_free = true;
      EXPECT_LT(H5P_remove(plist2, H5F_ACS_FILE_IMAGE_INFO_NAME), 0);
      tracker.fail_udata_free = false;
    }
  }

  // Test all comparison branches in H5P__facc_file_image_info_cmp.
  {
    std::array<int, 2> ordered_udata = {1, 2};
    H5FD_file_image_callbacks_t cmp_base{};
    cmp_base.image_malloc = TestImageMalloc;
    cmp_base.image_memcpy = TestImageMemcpy;
    cmp_base.image_realloc = DummyImageRealloc;
    cmp_base.image_free = TestImageFree;
    cmp_base.udata_copy = PassthruUdataCopy;
    cmp_base.udata_free = PassthruUdataFree;
    cmp_base.udata = &ordered_udata[0];

    ScopedPlist f_base = CreateFapl();
    ASSERT_GE(H5Pset_file_image_callbacks(f_base, &cmp_base), 0);

    auto check_cmp_diff = [&](H5FD_file_image_callbacks_t other_cb) {
      ScopedPlist f_other = CreateFapl();
      ASSERT_GE(H5Pset_file_image_callbacks(f_other, &other_cb), 0);
      EXPECT_EQ(H5Pequal(f_base, f_other), 0);
      EXPECT_EQ(H5Pequal(f_other, f_base), 0);
    };

    H5FD_file_image_callbacks_t c = cmp_base;
    c.image_malloc = AlternateImageMalloc;
    check_cmp_diff(c);

    c = cmp_base;
    c.image_memcpy = AlternateImageMemcpy;
    check_cmp_diff(c);

    c = cmp_base;
    c.image_realloc = nullptr;
    check_cmp_diff(c);

    c = cmp_base;
    c.image_free = AlternateImageFree;
    check_cmp_diff(c);

    c = cmp_base;
    c.udata_copy = AlternatePassthruUdataCopy;
    check_cmp_diff(c);

    c = cmp_base;
    c.udata_free = AlternatePassthruUdataFree;
    check_cmp_diff(c);

    c = cmp_base;
    c.udata = &ordered_udata[1];
    check_cmp_diff(c);

    // Same callbacks/udata, different buffer contents vs equal vs different sizes.
    ScopedPlist f_b1 = CreateFapl();
    ScopedPlist f_b2 = CreateFapl();
    ScopedPlist f_big = CreateFapl();
    ASSERT_GE(H5Pset_file_image(f_b1, buf1.data(), buf1.size()), 0);
    ASSERT_GE(H5Pset_file_image(f_b2, buf2.data(), buf2.size()), 0);
    ASSERT_GE(H5Pset_file_image(f_big, buf_large.data(), buf_large.size()), 0);
    EXPECT_EQ(H5Pequal(f_b1, f_b2), 0);
    EXPECT_EQ(H5Pequal(f_b1, f_big), 0);
    EXPECT_EQ(H5Pequal(f_big, f_b1), 0);
  }

  // Exercise H5P_set(H5F_ACS_FILE_IMAGE_INFO_NAME) success & failure,
  // H5Pset_file_image_callbacks cleanup on H5P_poke failure, and missing udata callbacks.
  {
    ScopedErrorSilence silence;

    // 1. Direct H5P_set invokes H5P__facc_file_image_info_set.
    const size_t prev_live = tracker.live_allocs.size();
    {
      ScopedPlist f_set = CreateFapl();
      ASSERT_TRUE(f_set.valid());
      auto* p_set = static_cast<H5P_genplist_t*>(H5I_object(f_set));
      ASSERT_NE(p_set, nullptr);
      H5FD_file_image_info_t set_info{};
      set_info.buffer = buf1.data();
      set_info.size = buf1.size();
      set_info.callbacks = cb;
      ASSERT_GE(H5P_set(p_set, H5F_ACS_FILE_IMAGE_INFO_NAME, &set_info), 0);
      EXPECT_EQ(tracker.live_allocs.size(), prev_live + 1u);

      tracker.fail_malloc = true;
      EXPECT_LT(H5P_set(p_set, H5F_ACS_FILE_IMAGE_INFO_NAME, &set_info), 0);
      tracker.fail_malloc = false;
    }
    EXPECT_EQ(tracker.live_allocs.size(), prev_live);

    // 2. H5Pset_file_image_callbacks cleanup when H5P_poke fails after udata_copy.
    struct PoisonUdata {
      hid_t target_fapl;
      bool fail_free;
      int free_calls;
      int* last_copied;
    };
    auto poison_copy = [](void* u) -> void* {
      auto* p = static_cast<PoisonUdata*>(u);
      auto* plist = static_cast<H5P_genplist_t*>(H5I_object(p->target_fapl));
      H5P_remove(plist, H5F_ACS_FILE_IMAGE_INFO_NAME);
      p->last_copied = new int(42);
      return p;
    };
    auto poison_free = [](void* u) -> herr_t {
      auto* p = static_cast<PoisonUdata*>(u);
      ++p->free_calls;
      if (p->fail_free) {
        return -1;
      }
      delete p->last_copied;
      p->last_copied = nullptr;
      return 0;
    };
    {
      ScopedPlist f_poke1 = CreateFapl();
      ASSERT_TRUE(f_poke1.valid());
      PoisonUdata pu1{f_poke1.get(), false, 0, nullptr};
      H5FD_file_image_callbacks_t cb_p1{};
      cb_p1.udata = &pu1;
      cb_p1.udata_copy = +poison_copy;
      cb_p1.udata_free = +poison_free;
      // Note: upstream overwrites ret_value with udata_free's return value (0) on cleanup.
      EXPECT_GE(H5Pset_file_image_callbacks(f_poke1, &cb_p1), 0);
      EXPECT_EQ(H5Pexist(f_poke1, H5F_ACS_FILE_IMAGE_INFO_NAME), 0);
      EXPECT_EQ(pu1.free_calls, 1);
      EXPECT_EQ(pu1.last_copied, nullptr);

      ScopedPlist f_poke2 = CreateFapl();
      ASSERT_TRUE(f_poke2.valid());
      PoisonUdata pu2{f_poke2.get(), true, 0, nullptr};
      H5FD_file_image_callbacks_t cb_p2{};
      cb_p2.udata = &pu2;
      cb_p2.udata_copy = +poison_copy;
      cb_p2.udata_free = +poison_free;
      EXPECT_LT(H5Pset_file_image_callbacks(f_poke2, &cb_p2), 0);
      EXPECT_EQ(pu2.free_calls, 1);
      delete pu2.last_copied;
      pu2.last_copied = nullptr;
    }

    // 3. Direct property callbacks: missing udata_copy / udata_free and NULL vs non-NULL image cmp.
    ScopedPlist f_prop = CreateFapl();
    ASSERT_TRUE(f_prop.valid());
    auto* p_prop = static_cast<H5P_genplist_t*>(H5I_object(f_prop));
    ASSERT_NE(p_prop, nullptr);
    H5P_genprop_t* img_prop = H5P__find_prop_plist(p_prop, H5F_ACS_FILE_IMAGE_INFO_NAME);
    ASSERT_NE(img_prop, nullptr);

    int dummy_ud = 7;
    H5FD_file_image_info_t missing_ucb{};
    missing_ucb.callbacks.udata = &dummy_ud;
    EXPECT_LT(img_prop->copy(img_prop->name, img_prop->size, &missing_ucb), 0);
    EXPECT_LT(img_prop->close(img_prop->name, img_prop->size, &missing_ucb), 0);

    H5FD_file_image_info_t i_buf{};
    i_buf.buffer = buf1.data();
    i_buf.size = buf1.size();
    H5FD_file_image_info_t i_null{};
    i_null.size = buf1.size();
    EXPECT_EQ(img_prop->cmp(&i_buf, &i_null, sizeof(i_buf)), -1);
    EXPECT_EQ(img_prop->cmp(&i_null, &i_buf, sizeof(i_buf)), 1);
  }

  // API negative tests.
  {
    ScopedErrorSilence silence;
    void* ptr = nullptr;
    size_t len = 0;
    H5FD_file_image_callbacks_t out_cb{};
    EXPECT_LT(H5Pset_file_image(H5I_INVALID_HID, buf1.data(), buf1.size()), 0);
    EXPECT_LT(H5Pset_file_image(CreateFapl(), nullptr, 16), 0);
    EXPECT_LT(H5Pset_file_image(CreateFapl(), buf1.data(), 0), 0);
    EXPECT_LT(H5Pget_file_image(H5I_INVALID_HID, &ptr, &len), 0);
    EXPECT_LT(H5Pset_file_image_callbacks(H5I_INVALID_HID, &out_cb), 0);
    EXPECT_LT(H5Pget_file_image_callbacks(H5I_INVALID_HID, &out_cb), 0);
    ScopedPlist f_ok = CreateFapl();
    EXPECT_LT(H5Pget_file_image_callbacks(f_ok, nullptr), 0);
  }
}

// =============================================================================
// File Driver Property, Public & Internal APIs, Custom VFDs, Set Def Driver
// =============================================================================

H5FD_t* DummyVfdOpen(const char*, unsigned, hid_t, haddr_t) { return nullptr; }
herr_t DummyVfdClose(H5FD_t*) { return 0; }
haddr_t DummyVfdGetEoa(const H5FD_t*, H5FD_mem_t) { return 0; }
herr_t DummyVfdSetEoa(H5FD_t*, H5FD_mem_t, haddr_t) { return 0; }
haddr_t DummyVfdGetEof(const H5FD_t*, H5FD_mem_t) { return 0; }
herr_t DummyVfdRead(H5FD_t*, H5FD_mem_t, hid_t, haddr_t, size_t, void*) { return 0; }
herr_t DummyVfdWrite(H5FD_t*, H5FD_mem_t, hid_t, haddr_t, size_t, const void*) { return 0; }

struct CustomVfdInfo {
  uint32_t magic;
  uint32_t tag;
};

struct CustomVfdInfoLarge {
  uint32_t magic;
  uint32_t tag;
  uint64_t extra;
};

bool g_vfd_fail_copy = false;
bool g_vfd_fail_free = false;

void* CustomVfdFaplCopy(const void* info) {
  if (g_vfd_fail_copy) return nullptr;
  auto* copy = new CustomVfdInfo(*static_cast<const CustomVfdInfo*>(info));
  return copy;
}

herr_t CustomVfdFaplFree(void* info) {
  if (g_vfd_fail_free) return -1;
  delete static_cast<CustomVfdInfo*>(info);
  return 0;
}

H5FD_class_t MakeDummyVfdClass(const char* name, H5FD_class_value_t value, size_t fapl_size,
                               void* (*fapl_copy)(const void*), herr_t (*fapl_free)(void*)) {
  H5FD_class_t cls{};
  cls.version = H5FD_CLASS_VERSION;
  cls.value = value;
  cls.name = name;
  cls.maxaddr = HADDR_MAX;
  cls.fc_degree = H5F_CLOSE_WEAK;
  cls.fapl_size = fapl_size;
  cls.fapl_copy = fapl_copy;
  cls.fapl_free = fapl_free;
  cls.open = DummyVfdOpen;
  cls.close = DummyVfdClose;
  cls.get_eoa = DummyVfdGetEoa;
  cls.set_eoa = DummyVfdSetEoa;
  cls.get_eof = DummyVfdGetEof;
  cls.read = DummyVfdRead;
  cls.write = DummyVfdWrite;
  for (int i = 0; i < H5FD_MEM_NTYPES; ++i) {
    cls.fl_map[i] = H5FD_MEM_DEFAULT;
  }
  return cls;
}

TEST(H5PfaplTest, FileDriverPublicInternalAndCustomVfdCallbacks) {
  g_vfd_fail_copy = false;
  g_vfd_fail_free = false;

  // Built-in core/sec2 + H5Pset_driver + H5Pset_driver_by_name / _by_value + config string.
  ScopedPlist f_core = CreateFapl();
  ASSERT_TRUE(f_core.valid());
  ASSERT_GE(H5Pset_fapl_core(f_core, 8192, false), 0);
  EXPECT_EQ(H5Pget_driver(f_core), H5FD_CORE);

  void* core_info = H5Pget_driver_info(f_core);
  ASSERT_NE(core_info, nullptr);

  ScopedPlist f_core2 = CreateFapl();
  ASSERT_TRUE(f_core2.valid());
  ASSERT_GE(H5Pset_driver(f_core2, H5FD_CORE, core_info), 0);
  EXPECT_GT(H5Pequal(f_core, f_core2), 0);

  // Set by name and by value with/without config string.
  ScopedPlist f_byname = CreateFapl();
  ASSERT_TRUE(f_byname.valid());
  ASSERT_GE(H5Pset_driver_by_name(f_byname, "core", nullptr), 0);
  EXPECT_EQ(H5Pget_driver(f_byname), H5FD_CORE);
  EXPECT_EQ(H5Pget_driver_config_str(f_byname, nullptr, 0), 0);

  ASSERT_GE(H5Pset_driver_by_name(f_byname, "core", "4096 0"), 0);
  ssize_t cfg_len = H5Pget_driver_config_str(f_byname, nullptr, 0);
  ASSERT_EQ(cfg_len, 6);
  std::string full_cfg(cfg_len + 1, '\0');
  EXPECT_EQ(H5Pget_driver_config_str(f_byname, full_cfg.data(), full_cfg.size()), 6);
  EXPECT_STREQ(full_cfg.c_str(), "4096 0");
  // Truncated config string buffer hits config_buf[buf_size - 1] = '\0'.
  std::array<char, 4> short_cfg{};
  EXPECT_EQ(H5Pget_driver_config_str(f_byname, short_cfg.data(), short_cfg.size()), 6);
  EXPECT_STREQ(short_cfg.data(), "409");

  ScopedPlist f_byval = CreateFapl();
  ASSERT_TRUE(f_byval.valid());
  ASSERT_GE(H5Pset_driver_by_value(f_byval, H5FD_CORE_VALUE, "8192 1"), 0);
  EXPECT_EQ(H5Pget_driver(f_byval), H5FD_CORE);

  // Register custom VFDs to cover no-copy (H5MM_malloc), zero-size, callback copy/free, and cmp.
  H5FD_class_t cls_cb = MakeDummyVfdClass("ztest_cb_vfd", 550, sizeof(CustomVfdInfo),
                                          CustomVfdFaplCopy, CustomVfdFaplFree);
  hid_t drv_cb = H5FDregister(&cls_cb);
  ASSERT_GE(drv_cb, 0);
  absl::Cleanup unreg_cb = [&] {
    g_vfd_fail_copy = false;
    g_vfd_fail_free = false;
    H5FDunregister(drv_cb);
  };

  H5FD_class_t cls_nocb = MakeDummyVfdClass("atest_nocb_vfd", 551, sizeof(CustomVfdInfo),
                                            nullptr, nullptr);
  hid_t drv_nocb = H5FDregister(&cls_nocb);
  ASSERT_GE(drv_nocb, 0);
  absl::Cleanup unreg_nocb = [&] { H5FDunregister(drv_nocb); };

  H5FD_class_t cls_nocb_large = MakeDummyVfdClass("atest_nocb_vfd", 552, sizeof(CustomVfdInfoLarge),
                                                  nullptr, nullptr);
  hid_t drv_nocb_large = H5FDregister(&cls_nocb_large);
  ASSERT_GE(drv_nocb_large, 0);
  absl::Cleanup unreg_nocb_large = [&] { H5FDunregister(drv_nocb_large); };

  // Custom VFD with no fapl_copy -> H5MM_malloc + H5MM_memcpy copy and H5MM_xfree_const free.
  CustomVfdInfo vinfo1{0xAABBCCDD, 1};
  CustomVfdInfo vinfo2{0xAABBCCDD, 2};
  CustomVfdInfoLarge vinfo_large{0xAABBCCDD, 1, 99};

  ScopedPlist f_nocb1 = CreateFapl();
  ScopedPlist f_nocb2 = CreateFapl();
  ScopedPlist f_nocb_null = CreateFapl();
  ScopedPlist f_nocb_large = CreateFapl();
  ASSERT_GE(H5Pset_driver(f_nocb1, drv_nocb, &vinfo1), 0);
  ASSERT_GE(H5Pset_driver(f_nocb2, drv_nocb, &vinfo2), 0);
  ASSERT_GE(H5Pset_driver(f_nocb_null, drv_nocb, nullptr), 0);
  ASSERT_GE(H5Pset_driver(f_nocb_large, drv_nocb_large, &vinfo_large), 0);

  EXPECT_EQ(H5Pequal(f_nocb1, f_nocb2), 0);
  EXPECT_EQ(H5Pequal(f_nocb1, f_nocb_null), 0);
  EXPECT_EQ(H5Pequal(f_nocb_null, f_nocb1), 0);
  EXPECT_EQ(H5Pequal(f_nocb1, f_nocb_large), 0);
  EXPECT_EQ(H5Pequal(f_nocb_large, f_nocb1), 0);

  // Callback VFD: set with config str via internal H5P_set_driver, copy, peek, get, cmp, remove.
  ScopedPlist f_cb1 = CreateFapl();
  ASSERT_GE(H5Pset_driver(f_cb1, drv_cb, &vinfo1), 0);
  ScopedPlist f_cb2 = CreateFapl();
  {
    auto* p_cb2 = static_cast<H5P_genplist_t*>(H5I_object(f_cb2));
    ASSERT_NE(p_cb2, nullptr);
    ASSERT_GE(H5P_set_driver(p_cb2, drv_cb, nullptr, "cfg_a"), 0);
    EXPECT_EQ(H5P_peek_driver(p_cb2), drv_cb);
    EXPECT_EQ(H5P_peek_driver_info(p_cb2), nullptr);
    EXPECT_STREQ(H5P_peek_driver_config_str(p_cb2), "cfg_a");
  }

  ScopedPlist f_cb_cfg_b = CreateFapl();
  {
    auto* p_cfg_b = static_cast<H5P_genplist_t*>(H5I_object(f_cb_cfg_b));
    ASSERT_GE(H5P_set_driver(p_cfg_b, drv_cb, nullptr, "cfg_b"), 0);
  }
  ScopedPlist f_cb_nocfg = CreateFapl();
  ASSERT_GE(H5Pset_driver(f_cb_nocfg, drv_cb, nullptr), 0);

  // Exercise all remaining branches in H5P__facc_file_driver_cmp.
  EXPECT_EQ(H5Pequal(f_nocb1, f_cb1), 0);
  EXPECT_EQ(H5Pequal(f_cb1, f_nocb1), 0);
  EXPECT_EQ(H5Pequal(f_cb2, f_cb_nocfg), 0);
  EXPECT_EQ(H5Pequal(f_cb_nocfg, f_cb2), 0);
  EXPECT_EQ(H5Pequal(f_cb2, f_cb_cfg_b), 0);

  // Trigger H5P__facc_file_driver_get via H5P_get + clean returned struct properly.
  {
    auto* plist = static_cast<H5P_genplist_t*>(H5I_object(f_cb2));
    H5FD_driver_prop_t got_prop{};
    ASSERT_GE(H5P_get(plist, H5F_ACS_FILE_DRV_NAME, &got_prop), 0);
    EXPECT_EQ(got_prop.driver_id, drv_cb);
    EXPECT_STREQ(got_prop.driver_config_str, "cfg_a");
    ASSERT_GE(H5FD_free_driver_info(got_prop.driver_id, got_prop.driver_info), 0);
    H5MM_xfree_const(got_prop.driver_config_str);
    ASSERT_GE(H5I_dec_ref(got_prop.driver_id), 0);
  }

  // Trigger H5P__facc_file_driver_copy + H5P__facc_file_driver_del.
  {
    ScopedPlist copied(H5Pcopy(f_cb1));
    ASSERT_TRUE(copied.valid());
    EXPECT_GT(H5Pequal(f_cb1, copied), 0);
    auto* p_copied = static_cast<H5P_genplist_t*>(H5I_object(copied));
    EXPECT_GE(H5P_remove(p_copied, H5F_ACS_FILE_DRV_NAME), 0);
  }

  // Callback error paths & internal invalid type errors.
  {
    ScopedErrorSilence silence;
    g_vfd_fail_copy = true;
    EXPECT_LT(H5Pset_driver(f_cb_nocfg, drv_cb, &vinfo1), 0);
    g_vfd_fail_copy = false;

    ScopedPlist f_fail_free = CreateFapl();
    ASSERT_GE(H5Pset_driver(f_fail_free, drv_cb, &vinfo1), 0);
    auto* p_ff = static_cast<H5P_genplist_t*>(H5I_object(f_fail_free));
    g_vfd_fail_free = true;
    EXPECT_LT(H5P_remove(p_ff, H5F_ACS_FILE_DRV_NAME), 0);
    g_vfd_fail_free = false;

    H5FD_class_t cls_zero = MakeDummyVfdClass("ztest_zero_vfd", 553, 0, nullptr, nullptr);
    hid_t drv_zero = H5FDregister(&cls_zero);
    ASSERT_GE(drv_zero, 0);
    absl::Cleanup unreg_zero = [&] { H5FDunregister(drv_zero); };
    EXPECT_LT(H5Pset_driver(f_cb_nocfg, drv_zero, &vinfo1), 0);

    // Invalid API / internal calls.
    ScopedPlist dcpl = CreateNonFapl();
    ASSERT_TRUE(dcpl.valid());
    auto* dcpl_ptr = static_cast<H5P_genplist_t*>(H5I_object(dcpl));
    EXPECT_LT(H5P_set_driver(dcpl_ptr, H5FD_CORE, nullptr, nullptr), 0);
    EXPECT_LT(H5P_set_driver(dcpl_ptr, H5I_INVALID_HID, nullptr, nullptr), 0);
    EXPECT_LT(H5P_peek_driver(dcpl_ptr), 0);
    EXPECT_EQ(H5P_peek_driver_info(dcpl_ptr), nullptr);
    EXPECT_EQ(H5P_peek_driver_config_str(dcpl_ptr), nullptr);

    EXPECT_LT(H5Pset_driver(H5I_INVALID_HID, H5FD_SEC2, nullptr), 0);
    EXPECT_LT(H5Pset_driver(f_cb_nocfg, H5I_INVALID_HID, nullptr), 0);
    EXPECT_LT(H5Pset_driver_by_name(H5I_INVALID_HID, "sec2", nullptr), 0);
    EXPECT_LT(H5Pset_driver_by_name(f_cb_nocfg, nullptr, nullptr), 0);
    EXPECT_LT(H5Pset_driver_by_name(f_cb_nocfg, "", nullptr), 0);
    EXPECT_LT(H5Pset_driver_by_name(f_cb_nocfg, "nonexistent_vfd_xyz", nullptr), 0);
    EXPECT_LT(H5Pset_driver_by_value(H5I_INVALID_HID, H5FD_SEC2_VALUE, nullptr), 0);
    EXPECT_LT(H5Pset_driver_by_value(f_cb_nocfg, -1, nullptr), 0);
    EXPECT_LT(H5Pset_driver_by_value(f_cb_nocfg, 999999, nullptr), 0);
    EXPECT_LT(H5Pget_driver(H5I_INVALID_HID), 0);
    EXPECT_EQ(H5Pget_driver_info(H5I_INVALID_HID), nullptr);
    ScopedPlist f_sec2 = CreateFapl();
    EXPECT_EQ(H5Pget_driver_info(f_sec2), nullptr);  // sec2 has no driver_info
    EXPECT_LT(H5Pget_driver_config_str(f_sec2, nullptr, 8), 0);
    EXPECT_LT(H5Pget_driver_config_str(H5I_INVALID_HID, full_cfg.data(), 4), 0);
  }
}

TEST(H5PfaplTest, SetDefDriverFromEnvironment) {
  const char* orig_drv = std::getenv(HDF5_DRIVER);
  const std::string saved_drv = orig_drv ? orig_drv : "";
  const bool had_drv = (orig_drv != nullptr);

  const char* orig_cfg = std::getenv(HDF5_DRIVER_CONFIG);
  const std::string saved_cfg = orig_cfg ? orig_cfg : "";
  const bool had_cfg = (orig_cfg != nullptr);

  H5FD_driver_prop_t orig_cls_prop{};
  ASSERT_GE(H5P__class_get(H5P_CLS_FILE_ACCESS_g, H5F_ACS_FILE_DRV_NAME, &orig_cls_prop), 0);
  hid_t orig_def_drv = H5Pget_driver(H5P_FILE_ACCESS_DEFAULT);
  ASSERT_GE(orig_def_drv, 0);

  absl::Cleanup restore_env = [&] {
    auto* def_fapl = static_cast<H5P_genplist_t*>(H5I_object(H5P_FILE_ACCESS_DEFAULT));
    if (def_fapl) {
      H5P_set_driver(def_fapl, orig_def_drv, nullptr, nullptr);
    }
    H5P__class_set(H5P_CLS_FILE_ACCESS_g, H5F_ACS_FILE_DRV_NAME, &orig_cls_prop);
    if (had_drv) {
      setenv(HDF5_DRIVER, saved_drv.c_str(), 1);
    } else {
      unsetenv(HDF5_DRIVER);
    }
    if (had_cfg) {
      setenv(HDF5_DRIVER_CONFIG, saved_cfg.c_str(), 1);
    } else {
      unsetenv(HDF5_DRIVER_CONFIG);
    }
  };

  auto run_set_def_and_balance_ref = []() {
    herr_t rc = H5P__facc_set_def_driver();
    if (rc < 0) {
      return rc;
    }
    // H5P__facc_set_def_driver increments the driver's app_ref by +1. Drop that
    // app_ref here so temporary/predefined drivers don't retain extra app refs.
    hid_t cur_drv = H5Pget_driver(H5P_FILE_ACCESS_DEFAULT);
    if (cur_drv >= 0) {
      H5I_dec_app_ref(cur_drv);
    }
    return rc;
  };

  // Exercise predefined enabled VFD names via H5P__facc_set_def_driver.
  ASSERT_EQ(setenv(HDF5_DRIVER_CONFIG, "test_cfg", 1), 0);
  constexpr const char* kEnabledDrivers[] = {
      "sec2", "core", "core_paged", "log", "family", "multi", "split", "stdio", "splitter",
  };
  for (const char* drv : kEnabledDrivers) {
    ASSERT_EQ(setenv(HDF5_DRIVER, drv, 1), 0);
    EXPECT_GE(run_set_def_and_balance_ref(), 0) << "driver=" << drv;
  }

  // Verify the updated default FAPL reflects the driver & config string.
  EXPECT_EQ(H5Pget_driver(H5P_FILE_ACCESS_DEFAULT), H5FD_SPLITTER);
  EXPECT_GT(H5Pget_driver_config_str(H5P_FILE_ACCESS_DEFAULT, nullptr, 0), 0);

  // Exercise already-registered VFD name path (`driver_is_registered == true`).
  H5FD_class_t cls_env = MakeDummyVfdClass("custom_env_vfd", 554, 0, nullptr, nullptr);
  hid_t env_drv = H5FDregister(&cls_env);
  ASSERT_GE(env_drv, 0);
  ASSERT_EQ(setenv(HDF5_DRIVER, "custom_env_vfd", 1), 0);
  unsetenv(HDF5_DRIVER_CONFIG);
  EXPECT_GE(run_set_def_and_balance_ref(), 0);
  EXPECT_EQ(H5Pget_driver(H5P_FILE_ACCESS_DEFAULT), env_drv);

  // Restore sec2 before unregistering env_drv so no property list holds env_drv.
  ASSERT_EQ(setenv(HDF5_DRIVER, "sec2", 1), 0);
  ASSERT_GE(run_set_def_and_balance_ref(), 0);
  EXPECT_GE(H5FDunregister(env_drv), 0);

  // Empty HDF5_DRIVER behaves as unset.
  ASSERT_EQ(setenv(HDF5_DRIVER, "", 1), 0);
  EXPECT_GE(H5P__facc_set_def_driver(), 0);

  // Disabled and unknown VFD names fail cleanly.
  ScopedErrorSilence silence;
  constexpr const char* kDisabledOrUnknown[] = {
      "mpio", "direct", "mirror", "hdfs", "ros3", "subfiling", "windows", "unknown_vfd_xyz",
  };
  for (const char* drv : kDisabledOrUnknown) {
    ASSERT_EQ(setenv(HDF5_DRIVER, drv, 1), 0);
    EXPECT_LT(H5P__facc_set_def_driver(), 0) << "driver=" << drv;
  }
}

// =============================================================================
// VOL Connector Property, Public/Internal APIs & Callbacks
// =============================================================================

struct CustomVolInfo {
  int code;
};

void* CustomVolInfoCopy(const void* info) {
  const auto* src = static_cast<const CustomVolInfo*>(info);
  return new CustomVolInfo{src->code};
}

herr_t CustomVolInfoCmp(int* cmp_value, const void* i1, const void* i2) {
  const auto* a = static_cast<const CustomVolInfo*>(i1);
  const auto* b = static_cast<const CustomVolInfo*>(i2);
  *cmp_value = (a->code > b->code) - (a->code < b->code);
  return 0;
}

herr_t CustomVolInfoFree(void* info) {
  delete static_cast<CustomVolInfo*>(info);
  return 0;
}

TEST(H5PfaplTest, VolConnectorApisCallbacksAndResetClass) {
  ScopedPlist fapl_native = CreateFapl();
  ASSERT_TRUE(fapl_native.valid());

  hid_t vol_id = H5I_INVALID_HID;
  ASSERT_GE(H5Pget_vol_id(H5P_DEFAULT, &vol_id), 0);
  EXPECT_GE(vol_id, 0);
  EXPECT_GE(H5VLclose(vol_id), 0);

  uint64_t cap_flags = 0;
  ASSERT_GE(H5Pget_vol_cap_flags(H5P_DEFAULT, &cap_flags), 0);
  EXPECT_NE(cap_flags & H5VL_CAP_FLAG_NATIVE_FILES, 0u);
  EXPECT_GE(H5Pget_vol_cap_flags(fapl_native, nullptr), 0);

  void* vol_info = reinterpret_cast<void*>(0x1);
  ASSERT_GE(H5Pget_vol_info(fapl_native, &vol_info), 0);
  EXPECT_EQ(vol_info, nullptr);

  // Passthrough VOL wrapping Native VOL (requires an application-visible VOL ID).
  ScopedPlist fapl_pt = CreateFapl();
  ASSERT_TRUE(fapl_pt.valid());
  hid_t under_vol_id = H5VLregister_connector_by_name(H5VL_NATIVE_NAME, H5P_DEFAULT);
  ASSERT_GE(under_vol_id, 0);
  absl::Cleanup close_under_vol = [&] { H5VLclose(under_vol_id); };
  H5VL_pass_through_info_t pt_info{under_vol_id, nullptr};
  ASSERT_GE(H5Pset_vol(fapl_pt, H5VL_PASSTHRU, &pt_info), 0);

  ASSERT_GE(H5Pget_vol_id(fapl_pt, &vol_id), 0);
  EXPECT_GE(H5VLclose(vol_id), 0);

  void* got_pt_info = nullptr;
  ASSERT_GE(H5Pget_vol_info(fapl_pt, &got_pt_info), 0);
  ASSERT_NE(got_pt_info, nullptr);
  EXPECT_GE(H5VLfree_connector_info(H5VL_PASSTHRU, got_pt_info), 0);

  ASSERT_GE(H5Pget_vol_cap_flags(fapl_pt, &cap_flags), 0);
  EXPECT_EQ(H5Pequal(fapl_native, fapl_pt), 0);
  EXPECT_EQ(H5Pequal(fapl_pt, fapl_native), 0);

  // Custom VOL connector + H5P_get + H5Pcopy + H5Pequal + H5P_remove + close.
  H5VL_class_t custom_vol_cls{};
  custom_vol_cls.version = H5VL_VERSION;
  custom_vol_cls.value = static_cast<H5VL_class_value_t>(501);
  custom_vol_cls.name = "test_fapl_custom_vol";
  custom_vol_cls.info_cls.size = sizeof(CustomVolInfo);
  custom_vol_cls.info_cls.copy = CustomVolInfoCopy;
  custom_vol_cls.info_cls.cmp = CustomVolInfoCmp;
  custom_vol_cls.info_cls.free = CustomVolInfoFree;

  hid_t custom_vol_id = H5VLregister_connector(&custom_vol_cls, H5P_DEFAULT);
  ASSERT_GE(custom_vol_id, 0);
  absl::Cleanup unreg_vol = [&] { H5VLunregister_connector(custom_vol_id); };

  ScopedPlist f_v1 = CreateFapl();
  ScopedPlist f_v2 = CreateFapl();
  CustomVolInfo ci1{10};
  CustomVolInfo ci2{20};
  ASSERT_GE(H5Pset_vol(f_v1, custom_vol_id, &ci1), 0);
  ASSERT_GE(H5Pset_vol(f_v2, custom_vol_id, &ci2), 0);
  EXPECT_EQ(H5Pequal(f_v1, f_v2), 0);

  ScopedPlist f_v1_copy(H5Pcopy(f_v1));
  ASSERT_TRUE(f_v1_copy.valid());
  EXPECT_GT(H5Pequal(f_v1, f_v1_copy), 0);

  // Exercise H5P__facc_vol_get & H5P__facc_vol_del.
  {
    auto* plist = static_cast<H5P_genplist_t*>(H5I_object(f_v1_copy));
    ASSERT_NE(plist, nullptr);
    H5VL_connector_prop_t got_prop{};
    ASSERT_GE(H5P_get(plist, H5F_ACS_VOL_CONN_NAME, &got_prop), 0);
    EXPECT_NE(got_prop.connector, nullptr);
    EXPECT_NE(got_prop.connector_info, nullptr);
    ASSERT_GE(H5VL_conn_prop_free(&got_prop), 0);
    ASSERT_GE(H5P_remove(plist, H5F_ACS_VOL_CONN_NAME), 0);
  }

  // Exercise H5P_reset_vol_class safely (restoring exact original struct).
  {
    H5VL_connector_prop_t orig_prop{};
    ASSERT_GE(H5P__class_get(H5P_CLS_FILE_ACCESS_g, H5F_ACS_VOL_CONN_NAME, &orig_prop), 0);
    EXPECT_GE(H5P_reset_vol_class(H5P_CLS_FILE_ACCESS_g, &orig_prop), 0);

    // Derived class lacks H5F_ACS_VOL_CONN_NAME in its own props -> error branch.
    hid_t sub_cls_id = H5Pcreate_class(H5P_FILE_ACCESS, "sub_fapl_test",
                                       nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    ASSERT_GE(sub_cls_id, 0);
    absl::Cleanup close_sub = [&] { H5Pclose_class(sub_cls_id); };
    auto* sub_cls = static_cast<const H5P_genclass_t*>(H5I_object(sub_cls_id));
    ASSERT_NE(sub_cls, nullptr);
    ScopedErrorSilence silence;
    EXPECT_LT(H5P_reset_vol_class(sub_cls, &orig_prop), 0);
  }

  // API / internal negative tests.
  {
    ScopedErrorSilence silence;
    ScopedPlist dcpl = CreateNonFapl();
    ASSERT_TRUE(dcpl.valid());
    auto* dcpl_ptr = static_cast<H5P_genplist_t*>(H5I_object(dcpl));
    auto* native_conn = static_cast<H5VL_connector_t*>(H5I_object(H5VL_NATIVE));
    ASSERT_NE(native_conn, nullptr);

    EXPECT_LT(H5P_set_vol(dcpl_ptr, native_conn, nullptr), 0);
    EXPECT_LT(H5Pset_vol(H5I_INVALID_HID, H5VL_NATIVE, nullptr), 0);
    EXPECT_LT(H5Pset_vol(dcpl, H5VL_NATIVE, nullptr), 0);
    EXPECT_LT(H5Pset_vol(fapl_native, H5I_INVALID_HID, nullptr), 0);
    EXPECT_LT(H5Pget_vol_id(H5I_INVALID_HID, &vol_id), 0);
    EXPECT_LT(H5Pget_vol_id(dcpl, &vol_id), 0);
    EXPECT_LT(H5Pget_vol_info(H5I_INVALID_HID, &vol_info), 0);
    EXPECT_LT(H5Pget_vol_info(dcpl, &vol_info), 0);
    EXPECT_LT(H5Pget_vol_cap_flags(H5I_INVALID_HID, &cap_flags), 0);
    EXPECT_LT(H5Pget_vol_cap_flags(dcpl, &cap_flags), 0);
  }
}

TEST(H5PfaplTest, PropertyComparisonBoundariesAndRemovedPropertyErrors) {
  ScopedErrorSilence silence;

  // Direct comparison edge cases & H5FD_VFD_DEFAULT handling.
  {
    ScopedPlist fapl = CreateFapl();
    ASSERT_TRUE(fapl.valid());
    auto* plist = static_cast<H5P_genplist_t*>(H5I_object(fapl));
    ASSERT_NE(plist, nullptr);

    // H5P_peek_driver maps H5FD_VFD_DEFAULT (0) to H5_DEFAULT_VFD.
    H5FD_driver_prop_t zero_drv{H5FD_VFD_DEFAULT, nullptr, nullptr};
    ASSERT_GE(H5P_set(plist, H5F_ACS_FILE_DRV_NAME, &zero_drv), 0);
    EXPECT_EQ(H5P_peek_driver(plist), H5_DEFAULT_VFD);

    // H5P__facc_file_driver_cmp invalid driver ID branches.
    H5P_genprop_t* drv_prop = H5P__find_prop_plist(plist, H5F_ACS_FILE_DRV_NAME);
    ASSERT_NE(drv_prop, nullptr);
    H5FD_driver_prop_t bad_drv{H5I_INVALID_HID, nullptr, nullptr};
    H5FD_driver_prop_t ok_drv{H5FD_SEC2, nullptr, nullptr};
    EXPECT_EQ(drv_prop->cmp(&bad_drv, &ok_drv, sizeof(bad_drv)), -1);
    EXPECT_EQ(drv_prop->cmp(&ok_drv, &bad_drv, sizeof(bad_drv)), 1);

    // H5P__facc_cache_image_config_cmp NULL checks.
    H5P_genprop_t* icfg_prop = H5P__find_prop_plist(plist, H5F_ACS_META_CACHE_INIT_IMAGE_CONFIG_NAME);
    ASSERT_NE(icfg_prop, nullptr);
    H5AC_cache_image_config_t icfg{};
    EXPECT_EQ(icfg_prop->cmp(nullptr, &icfg, sizeof(icfg)), -1);
    EXPECT_EQ(icfg_prop->cmp(&icfg, nullptr, sizeof(icfg)), 1);

    // H5P__facc_cache_config_cmp NULL & version mismatch checks.
    H5P_genprop_t* cfg_prop = H5P__find_prop_plist(plist, H5F_ACS_META_CACHE_INIT_CONFIG_NAME);
    ASSERT_NE(cfg_prop, nullptr);
    H5AC_cache_config_t cfg1{};
    H5AC_cache_config_t cfg2{};
    cfg1.version = 1;
    cfg2.version = 2;
    EXPECT_EQ(cfg_prop->cmp(nullptr, &cfg1, sizeof(cfg1)), -1);
    EXPECT_EQ(cfg_prop->cmp(&cfg1, nullptr, sizeof(cfg1)), 1);
    EXPECT_EQ(cfg_prop->cmp(&cfg1, &cfg2, sizeof(cfg1)), -1);
    EXPECT_EQ(cfg_prop->cmp(&cfg2, &cfg1, sizeof(cfg1)), 1);

    // Null config pointers & bad version checks for H5Pget_mdc_{image_}_config.
    EXPECT_LT(H5Pget_mdc_image_config(fapl, nullptr), 0);
    icfg.version = 999;
    EXPECT_LT(H5Pget_mdc_image_config(fapl, &icfg), 0);
    EXPECT_LT(H5Pget_mdc_config(fapl, nullptr), 0);
    cfg1.version = 999;
    EXPECT_LT(H5Pget_mdc_config(fapl, &cfg1), 0);
  }

  auto make_plist_without = [](const std::vector<const char*>& names) -> ScopedPlist {
    ScopedPlist fapl = CreateFapl();
    if (!fapl.valid()) return ScopedPlist();
    auto* plist = static_cast<H5P_genplist_t*>(H5I_object(fapl));
    for (const char* name : names) {
      if (H5P_remove(plist, name) < 0) return ScopedPlist();
    }
    return fapl;
  };

  // Alignment progressive removals.
  {
    hsize_t thr = 0;
    hsize_t align = 0;
    ScopedPlist f1 = make_plist_without({H5F_ACS_ALIGN_THRHD_NAME});
    ASSERT_TRUE(f1.valid());
    EXPECT_LT(H5Pset_alignment(f1, 16, 16), 0);
    EXPECT_LT(H5Pget_alignment(f1, &thr, &align), 0);

    ScopedPlist f2 = make_plist_without({H5F_ACS_ALIGN_NAME});
    ASSERT_TRUE(f2.valid());
    EXPECT_LT(H5Pset_alignment(f2, 16, 16), 0);
    EXPECT_LT(H5Pget_alignment(f2, &thr, &align), 0);
  }

  // Raw data chunk cache progressive removals.
  {
    int nelmts = 0;
    size_t nslots = 0;
    size_t nbytes = 0;
    double w0 = 0.0;

    ScopedPlist f1 = make_plist_without({H5F_ACS_DATA_CACHE_NUM_SLOTS_NAME});
    ASSERT_TRUE(f1.valid());
    EXPECT_LT(H5Pset_cache(f1, 0, 10, 1024, 0.5), 0);
    EXPECT_LT(H5Pget_cache(f1, &nelmts, &nslots, &nbytes, &w0), 0);

    ScopedPlist f2 = make_plist_without({H5F_ACS_DATA_CACHE_BYTE_SIZE_NAME});
    ASSERT_TRUE(f2.valid());
    EXPECT_LT(H5Pset_cache(f2, 0, 10, 1024, 0.5), 0);
    EXPECT_LT(H5Pget_cache(f2, &nelmts, &nslots, &nbytes, &w0), 0);

    ScopedPlist f3 = make_plist_without({H5F_ACS_PREEMPT_READ_CHUNKS_NAME});
    ASSERT_TRUE(f3.valid());
    EXPECT_LT(H5Pset_cache(f3, 0, 10, 1024, 0.5), 0);
    EXPECT_LT(H5Pget_cache(f3, &nelmts, &nslots, &nbytes, &w0), 0);
  }

  // Library version bounds progressive removals.
  {
    H5F_libver_t low = H5F_LIBVER_EARLIEST;
    H5F_libver_t high = H5F_LIBVER_LATEST;

    ScopedPlist f1 = make_plist_without({H5F_ACS_LIBVER_LOW_BOUND_NAME});
    ASSERT_TRUE(f1.valid());
    EXPECT_LT(H5Pset_libver_bounds(f1, H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);
    EXPECT_LT(H5Pget_libver_bounds(f1, &low, &high), 0);

    ScopedPlist f2 = make_plist_without({H5F_ACS_LIBVER_HIGH_BOUND_NAME});
    ASSERT_TRUE(f2.valid());
    EXPECT_LT(H5Pset_libver_bounds(f2, H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);
    EXPECT_LT(H5Pget_libver_bounds(f2, &low, &high), 0);
  }

  // MDC log options removals. Notice we only call H5Pset_mdc_log_options when
  // START_MDC_LOG_ON_ACCESS is missing (after MDC_LOG_LOCATION succeeds) so
  // H5MM_xstrdup'd location is owned and freed cleanly by plist close.
  {
    bool enabled = false;
    bool start_on_access = false;
    std::array<char, 64> loc{};
    size_t loc_sz = loc.size();

    ScopedPlist f1 = make_plist_without({H5F_ACS_USE_MDC_LOGGING_NAME});
    ASSERT_TRUE(f1.valid());
    EXPECT_LT(H5Pget_mdc_log_options(f1, &enabled, loc.data(), &loc_sz, &start_on_access), 0);

    ScopedPlist f2 = make_plist_without({H5F_ACS_MDC_LOG_LOCATION_NAME});
    ASSERT_TRUE(f2.valid());
    EXPECT_LT(H5Pget_mdc_log_options(f2, &enabled, loc.data(), &loc_sz, &start_on_access), 0);

    ScopedPlist f3 = make_plist_without({H5F_ACS_START_MDC_LOG_ON_ACCESS_NAME});
    ASSERT_TRUE(f3.valid());
    EXPECT_LT(H5Pset_mdc_log_options(f3, true, "test_mdc.log", true), 0);
    EXPECT_LT(H5Pget_mdc_log_options(f3, &enabled, loc.data(), &loc_sz, &start_on_access), 0);
  }

  // File locking progressive removals.
  {
    bool use_locks = false;
    bool ignore_disabled = false;

    ScopedPlist f1 = make_plist_without({H5F_ACS_USE_FILE_LOCKING_NAME});
    ASSERT_TRUE(f1.valid());
    EXPECT_LT(H5Pset_file_locking(f1, true, false), 0);
    EXPECT_LT(H5Pget_file_locking(f1, &use_locks, &ignore_disabled), 0);

    ScopedPlist f2 = make_plist_without({H5F_ACS_IGNORE_DISABLED_FILE_LOCKS_NAME});
    ASSERT_TRUE(f2.valid());
    EXPECT_LT(H5Pset_file_locking(f2, true, false), 0);
    EXPECT_LT(H5Pget_file_locking(f2, &use_locks, &ignore_disabled), 0);
  }

  // Page buffer progressive removals.
  {
    size_t pb_size = 0;
    unsigned meta_p = 0;
    unsigned raw_p = 0;

    ScopedPlist f1 = make_plist_without({H5F_ACS_PAGE_BUFFER_SIZE_NAME});
    ASSERT_TRUE(f1.valid());
    EXPECT_LT(H5Pset_page_buffer_size(f1, 4096, 20, 20), 0);
    EXPECT_LT(H5Pget_page_buffer_size(f1, &pb_size, &meta_p, &raw_p), 0);

    ScopedPlist f2 = make_plist_without({H5F_ACS_PAGE_BUFFER_MIN_META_PERC_NAME});
    ASSERT_TRUE(f2.valid());
    EXPECT_LT(H5Pset_page_buffer_size(f2, 4096, 20, 20), 0);
    EXPECT_LT(H5Pget_page_buffer_size(f2, &pb_size, &meta_p, &raw_p), 0);

    ScopedPlist f3 = make_plist_without({H5F_ACS_PAGE_BUFFER_MIN_RAW_PERC_NAME});
    ASSERT_TRUE(f3.valid());
    EXPECT_LT(H5Pset_page_buffer_size(f3, 4096, 20, 20), 0);
    EXPECT_LT(H5Pget_page_buffer_size(f3, &pb_size, &meta_p, &raw_p), 0);
  }

  // Remaining single-property FAPL setters & getters when their properties are removed.
  {
    ScopedPlist f = make_plist_without({
        H5F_ACS_FILE_DRV_NAME,
        H5F_ACS_FAMILY_OFFSET_NAME,
        H5F_ACS_MULTI_TYPE_NAME,
        H5F_ACS_META_CACHE_INIT_IMAGE_CONFIG_NAME,
        H5F_ACS_META_CACHE_INIT_CONFIG_NAME,
        H5F_ACS_GARBG_COLCT_REF_NAME,
        H5F_ACS_CLOSE_DEGREE_NAME,
        H5F_ACS_META_BLOCK_SIZE_NAME,
        H5F_ACS_SIEVE_BUF_SIZE_NAME,
        H5F_ACS_SDATA_BLOCK_SIZE_NAME,
        H5F_ACS_EFC_SIZE_NAME,
        H5F_ACS_FILE_IMAGE_INFO_NAME,
        H5F_ACS_METADATA_READ_ATTEMPTS_NAME,
        H5F_ACS_OBJECT_FLUSH_CB_NAME,
        H5F_ACS_EVICT_ON_CLOSE_FLAG_NAME,
        H5F_ACS_VOL_CONN_NAME,
        H5F_ACS_RFIC_FLAGS_NAME,
    });
    ASSERT_TRUE(f.valid());
    auto* plist = static_cast<H5P_genplist_t*>(H5I_object(f));
    ASSERT_NE(plist, nullptr);

    // Driver.
    EXPECT_LT(H5Pset_driver_by_name(f, "sec2", nullptr), 0);
    EXPECT_LT(H5Pset_driver_by_value(f, H5FD_SEC2_VALUE, nullptr), 0);
    EXPECT_LT(H5P_peek_driver(plist), 0);
    EXPECT_EQ(H5P_peek_driver_info(plist), nullptr);
    EXPECT_EQ(H5P_peek_driver_config_str(plist), nullptr);

    // Family, multi, MDC image config, MDC config.
    hsize_t fam_off = 0;
    EXPECT_LT(H5Pset_family_offset(f, 1024), 0);
    EXPECT_LT(H5Pget_family_offset(f, &fam_off), 0);

    H5FD_mem_t mem_t = H5FD_MEM_DEFAULT;
    EXPECT_LT(H5Pset_multi_type(f, H5FD_MEM_SUPER), 0);
    EXPECT_LT(H5Pget_multi_type(f, &mem_t), 0);

    H5AC_cache_image_config_t icfg{};
    icfg.version = H5AC__CURR_CACHE_IMAGE_CONFIG_VERSION;
    icfg.entry_ageout = H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE;
    EXPECT_LT(H5Pset_mdc_image_config(f, &icfg), 0);
    EXPECT_LT(H5Pget_mdc_image_config(f, &icfg), 0);

    H5AC_cache_config_t cfg{};
    cfg.version = H5AC__CURR_CACHE_CONFIG_VERSION;
    ASSERT_GE(H5Pget_mdc_config(H5P_FILE_ACCESS_DEFAULT, &cfg), 0);
    EXPECT_LT(H5Pset_mdc_config(f, &cfg), 0);
    EXPECT_LT(H5Pget_mdc_config(f, &cfg), 0);

    // GC, close degree, block sizes, elink file cache size.
    unsigned uval = 0;
    H5F_close_degree_t deg = H5F_CLOSE_DEFAULT;
    hsize_t hsz = 0;
    size_t sz = 0;
    EXPECT_LT(H5Pset_gc_references(f, 1), 0);
    EXPECT_LT(H5Pget_gc_references(f, &uval), 0);
    EXPECT_LT(H5Pset_fclose_degree(f, H5F_CLOSE_STRONG), 0);
    EXPECT_LT(H5Pget_fclose_degree(f, &deg), 0);
    EXPECT_LT(H5Pset_meta_block_size(f, 4096), 0);
    EXPECT_LT(H5Pget_meta_block_size(f, &hsz), 0);
    EXPECT_LT(H5Pset_sieve_buf_size(f, 4096), 0);
    EXPECT_LT(H5Pget_sieve_buf_size(f, &sz), 0);
    EXPECT_LT(H5Pset_small_data_block_size(f, 4096), 0);
    EXPECT_LT(H5Pget_small_data_block_size(f, &hsz), 0);
    EXPECT_LT(H5Pset_elink_file_cache_size(f, 8), 0);
    EXPECT_LT(H5Pget_elink_file_cache_size(f, &uval), 0);

    // File image, metadata read attempts, flush callback, evict on close.
    uint8_t dummy_byte = 0x5A;
    void* img_ptr = nullptr;
    size_t img_len = 0;
    H5FD_file_image_callbacks_t img_cb{};
    EXPECT_LT(H5Pset_file_image(f, &dummy_byte, 1), 0);
    EXPECT_LT(H5Pget_file_image(f, &img_ptr, &img_len), 0);
    EXPECT_LT(H5Pset_file_image_callbacks(f, &img_cb), 0);
    EXPECT_LT(H5Pget_file_image_callbacks(f, &img_cb), 0);
    EXPECT_LT(H5Pset_metadata_read_attempts(f, 3), 0);
    EXPECT_LT(H5Pget_metadata_read_attempts(f, &uval), 0);

    H5F_flush_cb_t flush_fn = nullptr;
    void* flush_ud = nullptr;
    EXPECT_LT(H5Pset_object_flush_cb(f, nullptr, nullptr), 0);
    EXPECT_LT(H5Pget_object_flush_cb(f, &flush_fn, &flush_ud), 0);

    bool evict = false;
    EXPECT_LT(H5Pset_evict_on_close(f, true), 0);
    EXPECT_LT(H5Pget_evict_on_close(f, &evict), 0);

    // VOL connector & RFIC flags.
    hid_t vol_id = H5I_INVALID_HID;
    void* vol_info = nullptr;
    uint64_t cap_flags = 0;
    uint64_t rfic_flags = 0;
    EXPECT_LT(H5Pset_vol(f, H5VL_NATIVE, nullptr), 0);
    EXPECT_LT(H5Pget_vol_id(f, &vol_id), 0);
    EXPECT_LT(H5Pget_vol_info(f, &vol_info), 0);
    EXPECT_LT(H5Pget_vol_cap_flags(f, &cap_flags), 0);
    EXPECT_LT(H5Pset_relax_file_integrity_checks(f, H5F_RFIC_ALL), 0);
    EXPECT_LT(H5Pget_relax_file_integrity_checks(f, &rfic_flags), 0);
  }
}

}  // namespace
}  // namespace hdf5
