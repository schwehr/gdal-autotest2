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
// Tests dataset creation property list (DCPL) storage layout, chunking,
// virtual dataset mappings, external file lists, filter helpers, fill values,
// allocation/fill times, object header minimization, and property callbacks
// (third_party/hdf5/src/H5Pdcpl.c).
//
// Google3 build notes: zlib (H5Z_FILTER_DEFLATE) and libaec (H5Z_FILTER_SZIP)
// are compiled in with encode+decode enabled; MPI/ROS3/subfiling/direct/plugins
// are disabled.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Pdcpl.c

#include "third_party/hdf5/src/hdf5.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

// `:hdf5` is built with -DNDEBUG, which strips `#ifndef NDEBUG` fields from
// internal structs. Match `-DNDEBUG` while including internal HDF5 headers.
#ifndef NDEBUG
#define H5PDCPL_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5P_FRIEND
#pragma push_macro("try")
#undef try
#define try h5_try_param
#pragma push_macro("H5_HAVE__FLOAT16")
#undef H5_HAVE__FLOAT16
#include "third_party/hdf5/src/H5Dprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5Ppkg.h"
#include "third_party/hdf5/src/H5Sprivate.h"
#include "third_party/hdf5/src/H5Tprivate.h"
#include "third_party/hdf5/src/H5Zprivate.h"
#pragma pop_macro("H5_HAVE__FLOAT16")
#undef try
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5PDCPL_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5PDCPL_TEST_RESTORE_UNDEF_NDEBUG
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define H5PDCPL_TEST_HAS_ASAN 1
#endif
#endif
#if !defined(H5PDCPL_TEST_HAS_ASAN) && \
    (defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__))
#define H5PDCPL_TEST_HAS_ASAN 1
#endif

extern "C" {
// Custom converter requiring a H5T_BKG_TEMP buffer and intentionally failing
// during H5T_CONV_CONV to exercise H5P_get_fill_value cleanup paths.
static herr_t
H5PdcplTestFailingBkgTempConv(hid_t H5_ATTR_UNUSED src_id,
                              hid_t H5_ATTR_UNUSED dst_id,
                              H5T_cdata_t *cdata,
                              size_t H5_ATTR_UNUSED nelmts,
                              size_t H5_ATTR_UNUSED buf_stride,
                              size_t H5_ATTR_UNUSED bkg_stride,
                              void *H5_ATTR_UNUSED buf,
                              void *bkg,
                              hid_t H5_ATTR_UNUSED dxpl_id) {
  switch (cdata->command) {
    case H5T_CONV_INIT:
      cdata->need_bkg = H5T_BKG_TEMP;
      return 0;
    case H5T_CONV_CONV:
      return bkg != nullptr ? -1 : -1;
    case H5T_CONV_FREE:
      return 0;
    default:
      return -1;
  }
}

// Custom succeeding converter requiring H5T_BKG_TEMP for testing temp bkg
// allocation and conversion in H5P_get_fill_value.
static herr_t
H5PdcplTestSucceedingBkgTempConv(hid_t H5_ATTR_UNUSED src_id,
                                 hid_t dst_id,
                                 H5T_cdata_t *cdata,
                                 size_t H5_ATTR_UNUSED nelmts,
                                 size_t H5_ATTR_UNUSED buf_stride,
                                 size_t H5_ATTR_UNUSED bkg_stride,
                                 void *buf,
                                 void *bkg,
                                 hid_t H5_ATTR_UNUSED dxpl_id) {
  switch (cdata->command) {
    case H5T_CONV_INIT:
      cdata->need_bkg = H5T_BKG_TEMP;
      return 0;
    case H5T_CONV_CONV: {
      if (buf == nullptr || bkg == nullptr) {
        return -1;
      }
      const size_t dst_size = H5Tget_size(dst_id);
      if (dst_size == 0) {
        return -1;
      }
      std::memset(bkg, 0x5a, dst_size);
      std::memcpy(buf, bkg, std::min<size_t>(dst_size, 2));
      return 0;
    }
    case H5T_CONV_FREE:
      return 0;
    default:
      return -1;
  }
}
}  // extern "C"

namespace hdf5 {
namespace {

using ::testing::Each;
using ::testing::ElementsAre;

// Helper to encode a property list with extra tail padding.
// Note: upstream H5P__dcrt_layout_enc forgets `+ 2` for H5D_CRT_LAYOUT_MAGIC_TYPE
// and version in Pass 1 (*pp == NULL) when chunk dim[u] > 0xffffffff, so Pass 2
// writes 2 bytes past the reported nalloc. Callers needing 64-bit chunk encoding
// pass `extra_tail_bytes > 0` to keep Pass 2 within bounds under ASan.
std::vector<uint8_t> EncodePlist(hid_t plist_id,
                                 hid_t fapl_id = H5P_DEFAULT,
                                 size_t extra_tail_bytes = 0) {
  size_t nalloc = 0;
  if (H5Pencode2(plist_id, nullptr, &nalloc, fapl_id) < 0) {
    return {};
  }
  std::vector<uint8_t> buf(nalloc + extra_tail_bytes, 0);
  if (H5Pencode2(plist_id, buf.data(), &nalloc, fapl_id) < 0) {
    return {};
  }
  return buf;
}

int ComparePlistsInternal(hid_t plist_id1, hid_t plist_id2) {
  auto *p1 = static_cast<const H5P_genplist_t *>(H5I_object(plist_id1));
  auto *p2 = static_cast<const H5P_genplist_t *>(H5I_object(plist_id2));
  EXPECT_NE(nullptr, p1);
  EXPECT_NE(nullptr, p2);
  int cmp_ret = 0;
  EXPECT_GE(H5P__cmp_plist(p1, p2, &cmp_ret), 0);
  return cmp_ret;
}

TEST(H5PdcplTest, DefaultDatasetCreatePlistProperties) {
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());

  EXPECT_EQ(H5D_CONTIGUOUS, H5Pget_layout(dcpl.get()));
  EXPECT_EQ(H5D_CONTIGUOUS, H5Pget_layout(H5P_DATASET_CREATE_DEFAULT));

  H5D_fill_value_t fill_status = H5D_FILL_VALUE_ERROR;
  ASSERT_GE(H5Pfill_value_defined(dcpl.get(), &fill_status), 0);
  EXPECT_EQ(H5D_FILL_VALUE_DEFAULT, fill_status);

  H5D_alloc_time_t alloc_time = H5D_ALLOC_TIME_ERROR;
  ASSERT_GE(H5Pget_alloc_time(dcpl.get(), &alloc_time), 0);
  EXPECT_EQ(H5D_ALLOC_TIME_LATE, alloc_time);

  H5D_fill_time_t fill_time = H5D_FILL_TIME_ERROR;
  ASSERT_GE(H5Pget_fill_time(dcpl.get(), &fill_time), 0);
  EXPECT_EQ(H5D_FILL_TIME_IFSET, fill_time);

  EXPECT_EQ(0, H5Pget_external_count(dcpl.get()));

  bool minimize = true;
  ASSERT_GE(H5Pget_dset_no_attrs_hint(dcpl.get(), &minimize), 0);
  EXPECT_FALSE(minimize);
}

TEST(H5PdcplTest, LayoutSetGetAllocTimeCouplingAndErrors) {
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());

  // 1. Setting layout with default alloc_time_state updates alloc_time per type.
  struct LayoutAllocCase {
    H5D_layout_t layout;
    H5D_alloc_time_t expected_alloc;
  };
  constexpr auto kCases = std::to_array<LayoutAllocCase>({
      {.layout = H5D_COMPACT, .expected_alloc = H5D_ALLOC_TIME_EARLY},
      {.layout = H5D_CONTIGUOUS, .expected_alloc = H5D_ALLOC_TIME_LATE},
      {.layout = H5D_CHUNKED, .expected_alloc = H5D_ALLOC_TIME_INCR},
      {.layout = H5D_VIRTUAL, .expected_alloc = H5D_ALLOC_TIME_INCR},
  });
  for (const auto &tc : kCases) {
    ASSERT_GE(H5Pset_layout(dcpl.get(), tc.layout), 0);
    EXPECT_EQ(tc.layout, H5Pget_layout(dcpl.get()));
    H5D_alloc_time_t alloc_time = H5D_ALLOC_TIME_ERROR;
    ASSERT_GE(H5Pget_alloc_time(dcpl.get(), &alloc_time), 0);
    EXPECT_EQ(tc.expected_alloc, alloc_time);
  }

  // 2. Setting explicit alloc_time clears alloc_time_state so layout changes
  //    preserve the user-configured allocation time.
  ASSERT_GE(H5Pset_alloc_time(dcpl.get(), H5D_ALLOC_TIME_EARLY), 0);
  ASSERT_GE(H5Pset_layout(dcpl.get(), H5D_CONTIGUOUS), 0);
  H5D_alloc_time_t preserved_alloc = H5D_ALLOC_TIME_ERROR;
  ASSERT_GE(H5Pget_alloc_time(dcpl.get(), &preserved_alloc), 0);
  EXPECT_EQ(H5D_ALLOC_TIME_EARLY, preserved_alloc);

  // 3. Resetting alloc_time to H5D_ALLOC_TIME_DEFAULT restores per-layout
  //    defaults across all layout variants.
  ASSERT_GE(H5Pset_alloc_time(dcpl.get(), H5D_ALLOC_TIME_DEFAULT), 0);
  ASSERT_GE(H5Pget_alloc_time(dcpl.get(), &preserved_alloc), 0);
  EXPECT_EQ(H5D_ALLOC_TIME_LATE, preserved_alloc);

  for (const auto &tc : kCases) {
    ASSERT_GE(H5Pset_layout(dcpl.get(), tc.layout), 0);
    ASSERT_GE(H5Pset_alloc_time(dcpl.get(), H5D_ALLOC_TIME_DEFAULT), 0);
    H5D_alloc_time_t alloc_time = H5D_ALLOC_TIME_ERROR;
    ASSERT_GE(H5Pget_alloc_time(dcpl.get(), &alloc_time), 0);
    EXPECT_EQ(tc.expected_alloc, alloc_time);
  }

  // 4. Setting chunk then switching to compact exercises H5P__dcrt_layout_del.
  constexpr std::array<hsize_t, 2> kDims = {4, 8};
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 2, kDims.data()), 0);
  ASSERT_GE(H5Pset_layout(dcpl.get(), H5D_COMPACT), 0);
  EXPECT_EQ(H5D_COMPACT, H5Pget_layout(dcpl.get()));

  // 5. Exercise layout _get/_copy/_close callbacks via H5Pget and H5Pcopy.
  H5O_layout_t raw_layout{};
  ASSERT_GE(H5Pget(dcpl.get(), H5D_CRT_LAYOUT_NAME, &raw_layout), 0);
  EXPECT_EQ(H5D_COMPACT, raw_layout.type);
  EXPECT_GE(H5O_msg_reset(H5O_LAYOUT_ID, &raw_layout), 0);

  ScopedPlist copied(H5Pcopy(dcpl.get()));
  ASSERT_TRUE(copied.valid());
  EXPECT_EQ(H5D_COMPACT, H5Pget_layout(copied.get()));

  // 6. Negative tests for H5Pset_layout and H5Pget_layout.
  ScopedErrorSilence silence;
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());

  EXPECT_LT(H5Pset_layout(dcpl.get(), H5D_LAYOUT_ERROR), 0);
  EXPECT_LT(H5Pset_layout(dcpl.get(), H5D_NLAYOUTS), 0);
  EXPECT_LT(H5Pset_layout(H5P_DATASET_CREATE_DEFAULT, H5D_COMPACT), 0);
  EXPECT_LT(H5Pset_layout(fapl.get(), H5D_COMPACT), 0);
  EXPECT_LT(H5Pset_layout(H5I_INVALID_HID, H5D_COMPACT), 0);

  EXPECT_EQ(H5D_LAYOUT_ERROR, H5Pget_layout(fapl.get()));
  EXPECT_EQ(H5D_LAYOUT_ERROR, H5Pget_layout(H5I_INVALID_HID));
}

TEST(H5PdcplTest, LayoutEncodeDecodeAllTypesAndErrors) {
  // 1. Compact & contiguous round-trip.
  for (H5D_layout_t lt : {H5D_COMPACT, H5D_CONTIGUOUS}) {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_layout(dcpl.get(), lt), 0);

    std::vector<uint8_t> buf = EncodePlist(dcpl.get());
    ASSERT_FALSE(buf.empty());
    ScopedPlist decoded(H5Pdecode(buf.data()));
    ASSERT_TRUE(decoded.valid());
    EXPECT_EQ(lt, H5Pget_layout(decoded.get()));
    EXPECT_GT(H5Pequal(dcpl.get(), decoded.get()), 0);
  }

  // 2. Chunked with ndims == 0 (via H5Pset_layout without H5Pset_chunk) uses
  //    default chunk layout in H5P__dcrt_layout_dec.
  {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_layout(dcpl.get(), H5D_CHUNKED), 0);
    std::vector<uint8_t> buf = EncodePlist(dcpl.get());
    ASSERT_FALSE(buf.empty());
    ScopedPlist decoded(H5Pdecode(buf.data()));
    ASSERT_TRUE(decoded.valid());
    EXPECT_EQ(H5D_CHUNKED, H5Pget_layout(decoded.get()));
  }

  // 3. Chunked v0 (dims <= 2^32 - 1).
  {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    constexpr std::array<hsize_t, 3> kChunkDims = {4, 16, 32};
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 3, kChunkDims.data()), 0);
    std::vector<uint8_t> buf = EncodePlist(dcpl.get());
    ASSERT_FALSE(buf.empty());
    ScopedPlist decoded(H5Pdecode(buf.data()));
    ASSERT_TRUE(decoded.valid());
    std::array<hsize_t, 3> out_dims{};
    EXPECT_EQ(3, H5Pget_chunk(decoded.get(), 3, out_dims.data()));
    EXPECT_THAT(out_dims, ElementsAre(4, 16, 32));
    EXPECT_GT(H5Pequal(dcpl.get(), decoded.get()), 0);
  }

  // 4. Chunked v1 (dim > 2^32 - 1): fails with old libver_bounds, succeeds with
  //    V200+ libver_bounds. Pass extra_tail_bytes=16 to guard upstream Pass-1
  //    nalloc undercount (+2 bytes for magic/version).
  {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    constexpr std::array<hsize_t, 2> kLargeDims = {(hsize_t{1} << 33) + 5, 8};
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 2, kLargeDims.data()), 0);

    {
      ScopedErrorSilence silence;
      ScopedPlist v18_fapl(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_TRUE(v18_fapl.valid());
      ASSERT_GE(
          H5Pset_libver_bounds(v18_fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_V18),
          0);
      size_t nalloc = 0;
      EXPECT_LT(H5Pencode2(dcpl.get(), nullptr, &nalloc, v18_fapl.get()), 0);
    }

    ScopedPlist v200_fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(v200_fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(v200_fapl.get(), H5F_LIBVER_V200, H5F_LIBVER_V200),
        0);
    std::vector<uint8_t> buf = EncodePlist(dcpl.get(), v200_fapl.get(), 16);
    ASSERT_FALSE(buf.empty());
    ScopedPlist decoded(H5Pdecode(buf.data()));
    ASSERT_TRUE(decoded.valid());
    std::array<hsize_t, 2> out_dims{};
    EXPECT_EQ(2, H5Pget_chunk(decoded.get(), 2, out_dims.data()));
    EXPECT_THAT(out_dims, ElementsAre((hsize_t{1} << 33) + 5, 8));
  }

  // 5. Virtual 0-entry encode/decode.
  {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_layout(dcpl.get(), H5D_VIRTUAL), 0);
    std::vector<uint8_t> buf = EncodePlist(dcpl.get());
    ASSERT_FALSE(buf.empty());
    ScopedPlist decoded(H5Pdecode(buf.data()));
    ASSERT_TRUE(decoded.valid());
    size_t count = 99;
    ASSERT_GE(H5Pget_virtual_count(decoded.get(), &count), 0);
    EXPECT_EQ(0U, count);
  }

  // 6. Corrupt layout type byte in encoded buffer triggers H5P__dcrt_layout_dec
  //    H5D_NLAYOUTS/default error cleanly before any dynamic allocation.
  {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_layout(dcpl.get(), H5D_COMPACT), 0);
    std::vector<uint8_t> buf = EncodePlist(dcpl.get());
    ASSERT_FALSE(buf.empty());

    constexpr std::array<uint8_t, 7> kPropTag = {'l', 'a', 'y', 'o',
                                                 'u', 't', '\0'};
    auto it = std::search(buf.begin(), buf.end(), kPropTag.begin(),
                          kPropTag.end());
    ASSERT_NE(buf.end(), it);
    const size_t value_offset =
        static_cast<size_t>(std::distance(buf.begin(), it)) + kPropTag.size();
    ASSERT_LT(value_offset, buf.size());
    buf[value_offset] = static_cast<uint8_t>(H5D_NLAYOUTS);

    ScopedErrorSilence silence;
    EXPECT_EQ(H5I_INVALID_HID, H5Pdecode(buf.data()));
  }
}

TEST(H5PdcplTest, LayoutComparisonsCompactContigChunkedAndVirtual) {
  ScopedPlist p_compact(H5Pcreate(H5P_DATASET_CREATE));
  ScopedPlist p_contig(H5Pcreate(H5P_DATASET_CREATE));
  ScopedPlist p_chunk_zero(H5Pcreate(H5P_DATASET_CREATE));
  ScopedPlist p_chunk1d(H5Pcreate(H5P_DATASET_CREATE));
  ScopedPlist p_chunk2d_a(H5Pcreate(H5P_DATASET_CREATE));
  ScopedPlist p_chunk2d_b(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(p_compact.valid() && p_contig.valid() && p_chunk_zero.valid() &&
              p_chunk1d.valid() && p_chunk2d_a.valid() && p_chunk2d_b.valid());

  ASSERT_GE(H5Pset_layout(p_compact.get(), H5D_COMPACT), 0);
  ASSERT_GE(H5Pset_layout(p_contig.get(), H5D_CONTIGUOUS), 0);
  ASSERT_GE(H5Pset_layout(p_chunk_zero.get(), H5D_CHUNKED), 0);
  const std::array<hsize_t, 1> d1 = {4};
  const std::array<hsize_t, 2> d2_a = {4, 8};
  const std::array<hsize_t, 2> d2_b = {6, 8};
  ASSERT_GE(H5Pset_chunk(p_chunk1d.get(), 1, d1.data()), 0);
  ASSERT_GE(H5Pset_chunk(p_chunk2d_a.get(), 2, d2_a.data()), 0);
  ASSERT_GE(H5Pset_chunk(p_chunk2d_b.get(), 2, d2_b.data()), 0);

  // Align alloc_time across DCPLs so property comparison isolates layout_cmp.
  for (hid_t id : {p_compact.get(), p_contig.get(), p_chunk_zero.get(),
                   p_chunk1d.get(), p_chunk2d_a.get(), p_chunk2d_b.get()}) {
    ASSERT_GE(H5Pset_alloc_time(id, H5D_ALLOC_TIME_EARLY), 0);
  }

  // Different layout types (< and >).
  EXPECT_LT(ComparePlistsInternal(p_compact.get(), p_contig.get()), 0);
  EXPECT_GT(ComparePlistsInternal(p_contig.get(), p_compact.get()), 0);
  EXPECT_EQ(0, ComparePlistsInternal(p_compact.get(), p_compact.get()));
  EXPECT_EQ(0, ComparePlistsInternal(p_contig.get(), p_contig.get()));

  // Chunked ndims (< and >). Note: never compare two ndims==0 chunked DCPLs
  // due to upstream `ndims - 1` unsigned underflow in H5P__dcrt_layout_cmp.
  EXPECT_LT(ComparePlistsInternal(p_chunk_zero.get(), p_chunk1d.get()), 0);
  EXPECT_GT(ComparePlistsInternal(p_chunk1d.get(), p_chunk_zero.get()), 0);
  EXPECT_LT(ComparePlistsInternal(p_chunk1d.get(), p_chunk2d_a.get()), 0);
  EXPECT_GT(ComparePlistsInternal(p_chunk2d_a.get(), p_chunk1d.get()), 0);

  // Chunked dim[0] (<, >, ==).
  EXPECT_LT(ComparePlistsInternal(p_chunk2d_a.get(), p_chunk2d_b.get()), 0);
  EXPECT_GT(ComparePlistsInternal(p_chunk2d_b.get(), p_chunk2d_a.get()), 0);
  EXPECT_EQ(0, ComparePlistsInternal(p_chunk2d_a.get(), p_chunk2d_a.get()));

  // Virtual layout comparisons covering list_nused, virtual extent, virtual
  // shape, source_file_name, source_dset_name, source extent, source shape.
  const std::array<hsize_t, 2> dims4x4 = {4, 4};
  const std::array<hsize_t, 2> dims4x8 = {4, 8};
  ScopedSpace vspace_row(H5Screate_simple(2, dims4x4.data(), nullptr));
  ScopedSpace vspace_blk(H5Screate_simple(2, dims4x4.data(), nullptr));
  ScopedSpace vspace_ext(H5Screate_simple(2, dims4x8.data(), nullptr));
  ScopedSpace src_row(H5Screate_simple(2, dims4x4.data(), nullptr));
  ScopedSpace src_blk(H5Screate_simple(2, dims4x4.data(), nullptr));
  ScopedSpace src_ext(H5Screate_simple(2, dims4x8.data(), nullptr));
  ASSERT_TRUE(vspace_row.valid() && vspace_blk.valid() && vspace_ext.valid() &&
              src_row.valid() && src_blk.valid() && src_ext.valid());

  const std::array<hsize_t, 2> zero = {0, 0};
  const std::array<hsize_t, 2> count_1x4 = {1, 4};
  const std::array<hsize_t, 2> count_2x2 = {2, 2};
  ASSERT_GE(H5Sselect_hyperslab(vspace_row.get(), H5S_SELECT_SET, zero.data(),
                                nullptr, count_1x4.data(), nullptr),
            0);
  ASSERT_GE(H5Sselect_hyperslab(vspace_blk.get(), H5S_SELECT_SET, zero.data(),
                                nullptr, count_2x2.data(), nullptr),
            0);
  ASSERT_GE(H5Sselect_hyperslab(vspace_ext.get(), H5S_SELECT_SET, zero.data(),
                                nullptr, count_1x4.data(), nullptr),
            0);
  ASSERT_GE(H5Sselect_hyperslab(src_row.get(), H5S_SELECT_SET, zero.data(),
                                nullptr, count_1x4.data(), nullptr),
            0);
  ASSERT_GE(H5Sselect_hyperslab(src_blk.get(), H5S_SELECT_SET, zero.data(),
                                nullptr, count_2x2.data(), nullptr),
            0);
  ASSERT_GE(H5Sselect_hyperslab(src_ext.get(), H5S_SELECT_SET, zero.data(),
                                nullptr, count_1x4.data(), nullptr),
            0);

  auto make_vds = [&](const char *fname, const char *dname, hid_t vs, hid_t ss,
                      int entries) {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    if (dcpl.valid()) {
      EXPECT_GE(H5Pset_alloc_time(dcpl.get(), H5D_ALLOC_TIME_INCR), 0);
      for (int i = 0; i < entries; ++i) {
        EXPECT_GE(H5Pset_virtual(dcpl.get(), vs, fname, dname, ss), 0);
      }
    }
    return dcpl;
  };

  ScopedPlist v_base =
      make_vds("a.h5", "d1", vspace_row.get(), src_row.get(), 1);
  ScopedPlist v_two =
      make_vds("a.h5", "d1", vspace_row.get(), src_row.get(), 2);
  ScopedPlist v_vext =
      make_vds("a.h5", "d1", vspace_ext.get(), src_row.get(), 1);
  ScopedPlist v_vshape =
      make_vds("a.h5", "d1", vspace_blk.get(), src_row.get(), 1);
  ScopedPlist v_fname =
      make_vds("b.h5", "d1", vspace_row.get(), src_row.get(), 1);
  ScopedPlist v_dname =
      make_vds("a.h5", "d2", vspace_row.get(), src_row.get(), 1);
  ScopedPlist v_sext =
      make_vds("a.h5", "d1", vspace_row.get(), src_ext.get(), 1);
  ScopedPlist v_sshape =
      make_vds("a.h5", "d1", vspace_row.get(), src_blk.get(), 1);
  ASSERT_TRUE(v_base.valid() && v_two.valid() && v_vext.valid() &&
              v_vshape.valid() && v_fname.valid() && v_dname.valid() &&
              v_sext.valid() && v_sshape.valid());

  EXPECT_EQ(0, ComparePlistsInternal(v_base.get(), v_base.get()));
  EXPECT_LT(ComparePlistsInternal(v_base.get(), v_two.get()), 0);
  EXPECT_GT(ComparePlistsInternal(v_two.get(), v_base.get()), 0);
  EXPECT_NE(0, ComparePlistsInternal(v_base.get(), v_vext.get()));
  EXPECT_NE(0, ComparePlistsInternal(v_base.get(), v_vshape.get()));
  EXPECT_LT(ComparePlistsInternal(v_base.get(), v_fname.get()), 0);
  EXPECT_GT(ComparePlistsInternal(v_fname.get(), v_base.get()), 0);
  EXPECT_LT(ComparePlistsInternal(v_base.get(), v_dname.get()), 0);
  EXPECT_GT(ComparePlistsInternal(v_dname.get(), v_base.get()), 0);
  EXPECT_NE(0, ComparePlistsInternal(v_base.get(), v_sext.get()));
  EXPECT_NE(0, ComparePlistsInternal(v_base.get(), v_sshape.get()));
}

TEST(H5PdcplTest, ChunkAndChunkOptsRoundTripAndErrors) {
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());

  // 1. Set and get 2D chunks with max_ndims <, ==, >, and dim == nullptr.
  constexpr std::array<hsize_t, 2> kDims = {16, 32};
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 2, kDims.data()), 0);
  EXPECT_EQ(H5D_CHUNKED, H5Pget_layout(dcpl.get()));

  std::array<hsize_t, 3> partial = {999, 999, 999};
  EXPECT_EQ(2, H5Pget_chunk(dcpl.get(), 1, partial.data()));
  EXPECT_THAT(partial, ElementsAre(16, 999, 999));

  std::array<hsize_t, 3> padded = {999, 999, 999};
  EXPECT_EQ(2, H5Pget_chunk(dcpl.get(), 3, padded.data()));
  EXPECT_THAT(padded, ElementsAre(16, 32, 999));

  EXPECT_EQ(2, H5Pget_chunk(dcpl.get(), 0, nullptr));

  // 2. Max-rank chunk dimensions.
  std::array<hsize_t, H5S_MAX_RANK> max_dims{};
  max_dims.fill(2);
  ASSERT_GE(H5Pset_chunk(dcpl.get(), H5S_MAX_RANK, max_dims.data()), 0);
  std::array<hsize_t, H5S_MAX_RANK> got_max{};
  EXPECT_EQ(H5S_MAX_RANK,
            H5Pget_chunk(dcpl.get(), H5S_MAX_RANK, got_max.data()));
  EXPECT_THAT(got_max, Each(2));

  // 3. Chunk options round-trip.
  unsigned opts = 999;
  ASSERT_GE(H5Pget_chunk_opts(dcpl.get(), &opts), 0);
  EXPECT_EQ(0U, opts);
  EXPECT_GE(H5Pget_chunk_opts(dcpl.get(), nullptr), 0);

  ASSERT_GE(
      H5Pset_chunk_opts(dcpl.get(), H5D_CHUNK_DONT_FILTER_PARTIAL_CHUNKS), 0);
  ASSERT_GE(H5Pget_chunk_opts(dcpl.get(), &opts), 0);
  EXPECT_EQ(H5D_CHUNK_DONT_FILTER_PARTIAL_CHUNKS, opts);

  ASSERT_GE(H5Pset_chunk_opts(dcpl.get(), 0), 0);
  ASSERT_GE(H5Pget_chunk_opts(dcpl.get(), &opts), 0);
  EXPECT_EQ(0U, opts);

  // 4. Error paths.
  ScopedErrorSilence silence;
  ScopedPlist fapl = MakeCoreFapl();
  ScopedPlist non_chunk_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(fapl.valid() && non_chunk_dcpl.valid());

  std::array<hsize_t, 2> bad_zero = {4, 0};
  std::array<hsize_t, H5S_MAX_RANK + 1> too_many{};
  too_many.fill(2);

  EXPECT_LT(H5Pset_chunk(dcpl.get(), 0, kDims.data()), 0);
  EXPECT_LT(H5Pset_chunk(dcpl.get(), -1, kDims.data()), 0);
  EXPECT_LT(H5Pset_chunk(dcpl.get(), H5S_MAX_RANK + 1, too_many.data()), 0);
  EXPECT_LT(H5Pset_chunk(dcpl.get(), 2, nullptr), 0);
  EXPECT_LT(H5Pset_chunk(dcpl.get(), 2, bad_zero.data()), 0);
  EXPECT_LT(H5Pset_chunk(H5P_DATASET_CREATE_DEFAULT, 2, kDims.data()), 0);
  EXPECT_LT(H5Pset_chunk(fapl.get(), 2, kDims.data()), 0);
  EXPECT_LT(H5Pset_chunk(H5I_INVALID_HID, 2, kDims.data()), 0);

  EXPECT_LT(H5Pget_chunk(non_chunk_dcpl.get(), 2, padded.data()), 0);
  EXPECT_LT(H5Pget_chunk(H5P_DATASET_CREATE_DEFAULT, 2, padded.data()), 0);
  EXPECT_LT(H5Pget_chunk(fapl.get(), 2, padded.data()), 0);
  EXPECT_LT(H5Pget_chunk(H5I_INVALID_HID, 2, padded.data()), 0);

  EXPECT_LT(H5Pset_chunk_opts(dcpl.get(),
                              H5D_CHUNK_DONT_FILTER_PARTIAL_CHUNKS << 4),
            0);
  EXPECT_LT(H5Pset_chunk_opts(non_chunk_dcpl.get(), 0), 0);
  EXPECT_LT(H5Pset_chunk_opts(H5P_DATASET_CREATE_DEFAULT, 0), 0);
  EXPECT_LT(H5Pset_chunk_opts(fapl.get(), 0), 0);
  EXPECT_LT(H5Pset_chunk_opts(H5I_INVALID_HID, 0), 0);

  EXPECT_LT(H5Pget_chunk_opts(non_chunk_dcpl.get(), &opts), 0);
  EXPECT_LT(H5Pget_chunk_opts(H5P_DATASET_CREATE_DEFAULT, &opts), 0);
  EXPECT_LT(H5Pget_chunk_opts(fapl.get(), &opts), 0);
  EXPECT_LT(H5Pget_chunk_opts(H5I_INVALID_HID, &opts), 0);
}

TEST(H5PdcplTest, VirtualDatasetMappingsGrowthEncodeDecodeAndGetters) {
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());

  // 1. Add >8 finite mappings (grows list past H5D_VIRTUAL_DEF_LIST_SIZE=8,
  //    triggering HASH_ADJUST_PTRS), including duplicate file/dset names and
  //    escaped "%%" literal segments (parsed != nullptr with nsubs == 0).
  const std::array<hsize_t, 1> vdims = {20};
  const std::array<hsize_t, 1> sdims = {2};
  ScopedSpace vspace(H5Screate_simple(1, vdims.data(), nullptr));
  ScopedSpace sspace(H5Screate_simple(1, sdims.data(), nullptr));
  ASSERT_TRUE(vspace.valid() && sspace.valid());
  ASSERT_GE(H5Sselect_all(sspace.get()), 0);

  for (size_t i = 0; i < 9; ++i) {
    const hsize_t start = static_cast<hsize_t>(i);
    const hsize_t count = 2;
    ASSERT_GE(H5Sselect_hyperslab(vspace.get(), H5S_SELECT_SET, &start, nullptr,
                                  &count, nullptr),
              0);
    const char *fname = (i % 2 == 0) ? "shared%%.h5" : "alt.h5";
    const char *dname = (i < 4) ? "src%%_a" : "src_b";
    ASSERT_GE(
        H5Pset_virtual(dcpl.get(), vspace.get(), fname, dname, sspace.get()),
        0);
  }

  // 2. Add a printf "%b" unlimited mapping.
  const std::array<hsize_t, 1> unlim_init = {2};
  const std::array<hsize_t, 1> unlim_max = {H5S_UNLIMITED};
  ScopedSpace unlim_vs(
      H5Screate_simple(1, unlim_init.data(), unlim_max.data()));
  ASSERT_TRUE(unlim_vs.valid());
  const hsize_t u_start = 0;
  const hsize_t u_stride = 2;
  const hsize_t u_count = H5S_UNLIMITED;
  const hsize_t u_block = 2;
  ASSERT_GE(H5Sselect_hyperslab(unlim_vs.get(), H5S_SELECT_SET, &u_start,
                                &u_stride, &u_count, &u_block),
            0);
  ASSERT_GE(H5Pset_virtual(dcpl.get(), unlim_vs.get(), "part_%b.h5", "d_%b",
                           sspace.get()),
            0);

  // 3. Query count, virtual/source spaces, filenames, and dsetnames.
  size_t vcount = 0;
  ASSERT_GE(H5Pget_virtual_count(dcpl.get(), &vcount), 0);
  EXPECT_EQ(10U, vcount);
  EXPECT_GE(H5Pget_virtual_count(dcpl.get(), nullptr), 0);

  ScopedSpace got_vs(H5Pget_virtual_vspace(dcpl.get(), 0));
  ScopedSpace got_ss(H5Pget_virtual_srcspace(dcpl.get(), 0));
  ASSERT_TRUE(got_vs.valid() && got_ss.valid());
  EXPECT_EQ(2, H5Sget_select_npoints(got_vs.get()));
  EXPECT_EQ(2, H5Sget_select_npoints(got_ss.get()));

  const ssize_t fn_len = H5Pget_virtual_filename(dcpl.get(), 0, nullptr, 0);
  EXPECT_EQ(static_cast<ssize_t>(std::strlen("shared%%.h5")), fn_len);
  std::string trunc_fn(4, '\0');
  EXPECT_EQ(fn_len, H5Pget_virtual_filename(dcpl.get(), 0, trunc_fn.data(),
                                            trunc_fn.size()));
  EXPECT_EQ("shar", trunc_fn);
  std::string full_fn(static_cast<size_t>(fn_len) + 1, '\0');
  EXPECT_EQ(fn_len, H5Pget_virtual_filename(dcpl.get(), 0, full_fn.data(),
                                            full_fn.size()));
  EXPECT_STREQ("shared%%.h5", full_fn.c_str());

  const ssize_t dn_len = H5Pget_virtual_dsetname(dcpl.get(), 9, nullptr, 0);
  EXPECT_EQ(static_cast<ssize_t>(std::strlen("d_%b")), dn_len);
  std::string trunc_dn(2, '\0');
  EXPECT_EQ(dn_len, H5Pget_virtual_dsetname(dcpl.get(), 9, trunc_dn.data(),
                                            trunc_dn.size()));
  EXPECT_EQ("d_", trunc_dn);
  std::string full_dn(static_cast<size_t>(dn_len) + 1, '\0');
  EXPECT_EQ(dn_len, H5Pget_virtual_dsetname(dcpl.get(), 9, full_dn.data(),
                                            full_dn.size()));
  EXPECT_STREQ("d_%b", full_dn.c_str());

  // 4. Encode, decode, copy, and overwrite layout cleanly.
  std::vector<uint8_t> encoded = EncodePlist(dcpl.get());
  ASSERT_FALSE(encoded.empty());
#if !defined(H5PDCPL_TEST_HAS_ASAN)
  // Upstream H5S_decode calls H5S_select_deserialize(..., SIZE_MAX), where
  // `*p + SIZE_MAX - 1` in H5Sselect.c triggers UBSan pointer-overflow under
  // `--config=asan-chlor` (note UNDEFINED_BEHAVIOR_SANITIZER is defined
  // globally even in unsanitized builds).
  ScopedPlist decoded(H5Pdecode(encoded.data()));
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Pequal(dcpl.get(), decoded.get()), 0);
#endif

  ScopedPlist copied(H5Pcopy(dcpl.get()));
  ASSERT_TRUE(copied.valid());
  EXPECT_GT(H5Pequal(dcpl.get(), copied.get()), 0);
  ASSERT_GE(H5Pset_layout(copied.get(), H5D_CONTIGUOUS), 0);
  EXPECT_EQ(H5D_CONTIGUOUS, H5Pget_layout(copied.get()));

  // 5. Core VFD VDS creation -> H5Dget_create_plist leaves
  //    source_space_status == H5O_VIRTUAL_STATUS_INVALID and null hash tables.
  ScopedFile file = CreateInMemoryFile("vds_mem.h5");
  ASSERT_TRUE(file.valid());
  const std::array<hsize_t, 1> src_ext = {12};
  ScopedSpace src_file_space(H5Screate_simple(1, src_ext.data(), nullptr));
  ASSERT_TRUE(src_file_space.valid());
  ScopedDataset src_dset(H5Dcreate2(file.get(), "src_a", H5T_NATIVE_INT,
                                    src_file_space.get(), H5P_DEFAULT,
                                    H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(src_dset.valid());
  src_dset.reset();

  ScopedPlist vds_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(vds_dcpl.valid());
  const std::array<hsize_t, 1> vds_dims = {10};
  ScopedSpace vds_space(H5Screate_simple(1, vds_dims.data(), nullptr));
  ScopedSpace vsel(H5Screate_simple(1, vds_dims.data(), nullptr));
  ScopedSpace ssel(H5Screate_simple(1, src_ext.data(), nullptr));
  ASSERT_TRUE(vds_space.valid() && vsel.valid() && ssel.valid());

  const hsize_t vs_start0 = 0;
  const hsize_t ss_start0 = 2;
  const hsize_t map_cnt = 3;
  ASSERT_GE(H5Sselect_hyperslab(vsel.get(), H5S_SELECT_SET, &vs_start0, nullptr,
                                &map_cnt, nullptr),
            0);
  ASSERT_GE(H5Sselect_hyperslab(ssel.get(), H5S_SELECT_SET, &ss_start0, nullptr,
                                &map_cnt, nullptr),
            0);
  ASSERT_GE(H5Pset_virtual(vds_dcpl.get(), vsel.get(), ".", "src_a", ssel.get()),
            0);
  const hsize_t vs_start1 = 3;
  ASSERT_GE(H5Sselect_hyperslab(vsel.get(), H5S_SELECT_SET, &vs_start1, nullptr,
                                &map_cnt, nullptr),
            0);
  ASSERT_GE(H5Pset_virtual(vds_dcpl.get(), vsel.get(), ".", "src_b", ssel.get()),
            0);
  const hsize_t vs_start2 = 6;
  ASSERT_GE(H5Sselect_hyperslab(vsel.get(), H5S_SELECT_SET, &vs_start2, nullptr,
                                &map_cnt, nullptr),
            0);
  ASSERT_GE(H5Pset_virtual(vds_dcpl.get(), vsel.get(), "other.h5", "src_a",
                           ssel.get()),
            0);

  ScopedDataset vds(H5Dcreate2(file.get(), "vds", H5T_NATIVE_INT,
                               vds_space.get(), H5P_DEFAULT, vds_dcpl.get(),
                               H5P_DEFAULT));
  ASSERT_TRUE(vds.valid());
  vds.reset();

  // Reopen the dataset so H5D__open_oid loads VDS layout with
  // source_space_status == H5O_VIRTUAL_STATUS_INVALID and null hash tables.
  ScopedDataset reopened_vds(H5Dopen2(file.get(), "vds", H5P_DEFAULT));
  ASSERT_TRUE(reopened_vds.valid());
  ScopedPlist reopened_dcpl(H5Dget_create_plist(reopened_vds.get()));
  ASSERT_TRUE(reopened_dcpl.valid());

  // H5Pget_virtual_srcspace patches extent from selection bounds
  // ([2..4] -> bounds_end + 1 = 5).
  ScopedSpace patched_ss(H5Pget_virtual_srcspace(reopened_dcpl.get(), 2));
  ASSERT_TRUE(patched_ss.valid());
  hsize_t patched_dim = 0;
  EXPECT_EQ(1, H5Sget_simple_extent_dims(patched_ss.get(), &patched_dim,
                                         nullptr));
  EXPECT_EQ(5U, patched_dim);

  // Adding a mapping to reopened_dcpl rebuilds null hash tables (L2193-2206).
  const hsize_t vs_start3 = 9;
  const hsize_t cnt1 = 1;
  ASSERT_GE(H5Sselect_hyperslab(vsel.get(), H5S_SELECT_SET, &vs_start3, nullptr,
                                &cnt1, nullptr),
            0);
  ASSERT_GE(H5Sselect_hyperslab(ssel.get(), H5S_SELECT_SET, &ss_start0, nullptr,
                                &cnt1, nullptr),
            0);
  ASSERT_GE(H5Pset_virtual(reopened_dcpl.get(), vsel.get(), "extra.h5", "src_c",
                           ssel.get()),
            0);
}

TEST(H5PdcplTest, VirtualDatasetErrorsAndPartialEntryCleanup) {
  ScopedErrorSilence silence;
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ScopedPlist fapl = MakeCoreFapl();
  ScopedPlist non_vds_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid() && fapl.valid() && non_vds_dcpl.valid());

  const std::array<hsize_t, 1> dims4 = {4};
  const std::array<hsize_t, 1> dims8 = {8};
  const std::array<hsize_t, 1> max_unlim = {H5S_UNLIMITED};
  ScopedSpace lim_vs(H5Screate_simple(1, dims4.data(), nullptr));
  ScopedSpace lim_ss(H5Screate_simple(1, dims4.data(), nullptr));
  ScopedSpace big_ss(H5Screate_simple(1, dims8.data(), nullptr));
  ScopedSpace pt_space(H5Screate_simple(1, dims4.data(), nullptr));
  ScopedSpace unlim_vs(H5Screate_simple(1, dims4.data(), max_unlim.data()));
  ASSERT_TRUE(lim_vs.valid() && lim_ss.valid() && big_ss.valid() &&
              pt_space.valid() && unlim_vs.valid());

  ASSERT_GE(H5Sselect_all(lim_vs.get()), 0);
  ASSERT_GE(H5Sselect_all(lim_ss.get()), 0);
  ASSERT_GE(H5Sselect_all(big_ss.get()), 0);
  const hsize_t coord = 1;
  ASSERT_GE(H5Sselect_elements(pt_space.get(), H5S_SELECT_SET, 1, &coord), 0);

  const hsize_t u_start = 0;
  const hsize_t u_stride = 4;
  const hsize_t u_count = H5S_UNLIMITED;
  const hsize_t u_block = 4;
  ASSERT_GE(H5Sselect_hyperslab(unlim_vs.get(), H5S_SELECT_SET, &u_start,
                                &u_stride, &u_count, &u_block),
            0);

  // 1. Seed a valid mapping in `dcpl` for out-of-range getter checks below.
  ASSERT_GE(H5Pset_virtual(dcpl.get(), unlim_vs.get(), "shared_%b.h5",
                           "shared_%b", lim_ss.get()),
            0);

  // 2. Argument & pre-check failures.
  EXPECT_LT(
      H5Pset_virtual(dcpl.get(), lim_vs.get(), nullptr, "d", lim_ss.get()), 0);
  EXPECT_LT(
      H5Pset_virtual(dcpl.get(), lim_vs.get(), "f.h5", nullptr, lim_ss.get()),
      0);
  EXPECT_LT(
      H5Pset_virtual(dcpl.get(), H5I_INVALID_HID, "f.h5", "d", lim_ss.get()),
      0);
  EXPECT_LT(
      H5Pset_virtual(dcpl.get(), lim_vs.get(), "f.h5", "d", H5I_INVALID_HID),
      0);
  EXPECT_LT(
      H5Pset_virtual(dcpl.get(), pt_space.get(), "f.h5", "d", lim_ss.get()), 0);
  EXPECT_LT(
      H5Pset_virtual(dcpl.get(), lim_vs.get(), "f.h5", "d", big_ss.get()), 0);
  EXPECT_LT(H5Pset_virtual(H5P_DATASET_CREATE_DEFAULT, lim_vs.get(), "f.h5",
                           "d", lim_ss.get()),
            0);
  EXPECT_LT(H5Pset_virtual(fapl.get(), lim_vs.get(), "f.h5", "d", lim_ss.get()),
            0);

  // 3. Parse-source-name failures & post-mapping check failures trigger partial
  //    entry cleanup. Isolate each failure in a fresh DCPL without later
  //    H5Pset_virtual calls, because upstream H5Pset_virtual leaves freed name
  //    keys in the uthash table (and HASH_DELETEs uninserted shared entries).
  for (const auto &[src_file, src_dset] :
       std::array<std::pair<const char *, const char *>, 3>{{
           {"bad_%x.h5", "d"},
           {"ok%%.h5", "bad_%y"},
           {"uniq_%b.h5", "uniq_%b"},
       }}) {
    ScopedPlist bad_dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(bad_dcpl.valid());
    EXPECT_LT(H5Pset_virtual(bad_dcpl.get(), lim_vs.get(), src_file, src_dset,
                             lim_ss.get()),
              0);
  }

  // Confirm earlier valid mapping stayed intact (count == 1).
  size_t count = 0;
  ASSERT_GE(H5Pget_virtual_count(dcpl.get(), &count), 0);
  EXPECT_EQ(1U, count);

  // 4. Getter errors: bad ID, non-DCPL, non-virtual layout, out-of-range index.
  std::array<char, 16> name_buf{};
  for (hid_t bad_id :
       {static_cast<hid_t>(H5I_INVALID_HID), fapl.get(), non_vds_dcpl.get(),
        static_cast<hid_t>(H5P_DATASET_CREATE_DEFAULT)}) {
    EXPECT_LT(H5Pget_virtual_count(bad_id, &count), 0);
    EXPECT_LT(H5Pget_virtual_vspace(bad_id, 0), 0);
    EXPECT_LT(H5Pget_virtual_srcspace(bad_id, 0), 0);
    EXPECT_LT(H5Pget_virtual_filename(bad_id, 0, name_buf.data(),
                                      name_buf.size()),
              0);
    EXPECT_LT(H5Pget_virtual_dsetname(bad_id, 0, name_buf.data(),
                                      name_buf.size()),
              0);
  }
  EXPECT_LT(H5Pget_virtual_vspace(dcpl.get(), 1), 0);
  EXPECT_LT(H5Pget_virtual_srcspace(dcpl.get(), 1), 0);
  EXPECT_LT(H5Pget_virtual_filename(dcpl.get(), 1, name_buf.data(),
                                    name_buf.size()),
            0);
  EXPECT_LT(H5Pget_virtual_dsetname(dcpl.get(), 1, name_buf.data(),
                                    name_buf.size()),
            0);
}

TEST(H5PdcplTest, ExternalFileListOperationsCompareEncodeAndErrors) {
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());

  // 1. Add 18 entries (> H5O_EFL_ALLOC = 16) to force realloc growth both in
  //    H5Pset_external and in H5P__dcrt_ext_file_list_dec.
  for (int i = 0; i < 18; ++i) {
    const std::string fname = "ext_" + std::to_string(i) + ".dat";
    ASSERT_GE(H5Pset_external(dcpl.get(), fname.c_str(),
                              static_cast<off_t>(i * 10),
                              static_cast<hsize_t>(100 + i)),
              0);
  }
  EXPECT_EQ(18, H5Pget_external_count(dcpl.get()));

  // 2. Get external metadata (full, truncated, and optional null pointers).
  std::string buf(32, 'x');
  off_t off = -1;
  hsize_t sz = 0;
  ASSERT_GE(
      H5Pget_external(dcpl.get(), 17, buf.size(), buf.data(), &off, &sz), 0);
  EXPECT_STREQ("ext_17.dat", buf.c_str());
  EXPECT_EQ(170, off);
  EXPECT_EQ(117U, sz);

  std::array<char, 3> tiny_buf{'a', 'b', 'c'};
  ASSERT_GE(H5Pget_external(dcpl.get(), 0, tiny_buf.size(), tiny_buf.data(),
                            nullptr, nullptr),
            0);
  EXPECT_THAT(tiny_buf, ElementsAre('e', 'x', 't'));
  EXPECT_GE(H5Pget_external(dcpl.get(), 0, 0, nullptr, nullptr, nullptr), 0);

  // 3. Encode/decode, copy, get, and remove callbacks.
  std::vector<uint8_t> encoded = EncodePlist(dcpl.get());
  ASSERT_FALSE(encoded.empty());
  ScopedPlist decoded(H5Pdecode(encoded.data()));
  ASSERT_TRUE(decoded.valid());
  EXPECT_EQ(18, H5Pget_external_count(decoded.get()));
  EXPECT_GT(H5Pequal(dcpl.get(), decoded.get()), 0);

  ScopedPlist copied(H5Pcopy(dcpl.get()));
  ASSERT_TRUE(copied.valid());
  EXPECT_GT(H5Pequal(dcpl.get(), copied.get()), 0);

  H5O_efl_t raw_efl{};
  ASSERT_GE(H5Pget(dcpl.get(), H5D_CRT_EXT_FILE_LIST_NAME, &raw_efl), 0);
  EXPECT_EQ(18U, raw_efl.nused);
  // Exercise _set + _del and name_offset comparison via modified copy.
  ASSERT_GT(raw_efl.nused, 0U);
  raw_efl.slot[0].name_offset = 42;
  auto *copied_plist = static_cast<H5P_genplist_t *>(H5I_object(copied.get()));
  ASSERT_NE(nullptr, copied_plist);
  ASSERT_GE(H5P_set(copied_plist, H5D_CRT_EXT_FILE_LIST_NAME, &raw_efl), 0);
  EXPECT_GE(H5O_msg_reset(H5O_EFL_ID, &raw_efl), 0);
  EXPECT_LT(ComparePlistsInternal(dcpl.get(), copied.get()), 0);
  EXPECT_GT(ComparePlistsInternal(copied.get(), dcpl.get()), 0);

  EXPECT_GE(H5Premove(copied.get(), H5D_CRT_EXT_FILE_LIST_NAME), 0);

  // 4. EFL comparisons covering nalloc, nused, name, offset, size, equal.
  ScopedPlist e_empty(H5Pcreate(H5P_DATASET_CREATE));
  ScopedPlist e_one_a(H5Pcreate(H5P_DATASET_CREATE));
  ScopedPlist e_one_b(H5Pcreate(H5P_DATASET_CREATE));
  ScopedPlist e_one_off(H5Pcreate(H5P_DATASET_CREATE));
  ScopedPlist e_one_sz(H5Pcreate(H5P_DATASET_CREATE));
  ScopedPlist e_two(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(e_empty.valid() && e_one_a.valid() && e_one_b.valid() &&
              e_one_off.valid() && e_one_sz.valid() && e_two.valid());

  ASSERT_GE(H5Pset_external(e_one_a.get(), "a.dat", 0, 100), 0);
  ASSERT_GE(H5Pset_external(e_one_b.get(), "b.dat", 0, 100), 0);
  ASSERT_GE(H5Pset_external(e_one_off.get(), "a.dat", 64, 100), 0);
  ASSERT_GE(H5Pset_external(e_one_sz.get(), "a.dat", 0, 200), 0);
  ASSERT_GE(H5Pset_external(e_two.get(), "a.dat", 0, 100), 0);
  ASSERT_GE(H5Pset_external(e_two.get(), "b.dat", 0, 100), 0);

  EXPECT_LT(ComparePlistsInternal(e_empty.get(), e_one_a.get()), 0);
  EXPECT_GT(ComparePlistsInternal(e_one_a.get(), e_empty.get()), 0);
  EXPECT_LT(ComparePlistsInternal(e_one_a.get(), e_two.get()), 0);
  EXPECT_GT(ComparePlistsInternal(e_two.get(), e_one_a.get()), 0);
  EXPECT_LT(ComparePlistsInternal(e_one_a.get(), e_one_b.get()), 0);
  EXPECT_GT(ComparePlistsInternal(e_one_b.get(), e_one_a.get()), 0);
  EXPECT_LT(ComparePlistsInternal(e_one_a.get(), e_one_off.get()), 0);
  EXPECT_GT(ComparePlistsInternal(e_one_off.get(), e_one_a.get()), 0);
  EXPECT_LT(ComparePlistsInternal(e_one_a.get(), e_one_sz.get()), 0);
  EXPECT_GT(ComparePlistsInternal(e_one_sz.get(), e_one_a.get()), 0);
  EXPECT_EQ(0, ComparePlistsInternal(e_one_a.get(), e_one_a.get()));

  // 5. H5F_UNLIMITED & overflow / error handling.
  ScopedErrorSilence silence;
  ScopedPlist e_unlim(H5Pcreate(H5P_DATASET_CREATE));
  ScopedPlist e_ovf(H5Pcreate(H5P_DATASET_CREATE));
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(e_unlim.valid() && e_ovf.valid() && fapl.valid());

  ASSERT_GE(H5Pset_external(e_unlim.get(), "unlim.dat", 0, H5F_UNLIMITED), 0);
  EXPECT_LT(H5Pset_external(e_unlim.get(), "after.dat", 0, 10), 0);

  ASSERT_GE(H5Pset_external(e_ovf.get(), "big.dat", 0,
                            std::numeric_limits<hsize_t>::max() - 8),
            0);
  EXPECT_LT(H5Pset_external(e_ovf.get(), "ovf.dat", 0, 16), 0);

  EXPECT_LT(H5Pset_external(e_empty.get(), nullptr, 0, 10), 0);
  EXPECT_LT(H5Pset_external(e_empty.get(), "", 0, 10), 0);
  EXPECT_LT(H5Pset_external(e_empty.get(), "neg.dat", -1, 10), 0);
  EXPECT_LT(H5Pset_external(H5P_DATASET_CREATE_DEFAULT, "def.dat", 0, 10), 0);
  EXPECT_LT(H5Pset_external(fapl.get(), "fapl.dat", 0, 10), 0);
  EXPECT_LT(H5Pset_external(H5I_INVALID_HID, "bad.dat", 0, 10), 0);

  EXPECT_LT(H5Pget_external_count(fapl.get()), 0);
  EXPECT_LT(H5Pget_external_count(H5I_INVALID_HID), 0);
  EXPECT_LT(
      H5Pget_external(e_one_a.get(), 1, buf.size(), buf.data(), &off, &sz), 0);
  EXPECT_LT(
      H5Pget_external(fapl.get(), 0, buf.size(), buf.data(), &off, &sz), 0);
  EXPECT_LT(
      H5Pget_external(H5I_INVALID_HID, 0, buf.size(), buf.data(), &off, &sz),
      0);
}

TEST(H5PdcplTest, FilterConvenienceApisSzipShuffleNbitAndScaleoffset) {
  // 1. SZIP is enabled via libaec in google3. Verify mask filtering and errors.
  htri_t szip_avail = H5Zfilter_avail(H5Z_FILTER_SZIP);
  ASSERT_GT(szip_avail, 0);
  unsigned szip_cfg = 0;
  ASSERT_GE(H5Zget_filter_info(H5Z_FILTER_SZIP, &szip_cfg), 0);
  ASSERT_NE(0U, szip_cfg & H5Z_FILTER_CONFIG_ENCODE_ENABLED);

  {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    const unsigned in_mask = H5_SZIP_EC_OPTION_MASK | H5_SZIP_CHIP_OPTION_MASK |
                             H5_SZIP_LSB_OPTION_MASK | H5_SZIP_MSB_OPTION_MASK;
    ASSERT_GE(H5Pset_szip(dcpl.get(), in_mask, 8), 0);

    unsigned flags = 0;
    size_t cd_nelmts = 4;
    std::array<unsigned, 4> cd_values{};
    ASSERT_GE(H5Pget_filter_by_id2(dcpl.get(), H5Z_FILTER_SZIP, &flags,
                                   &cd_nelmts, cd_values.data(), 0, nullptr,
                                   nullptr),
              0);
    EXPECT_GE(cd_nelmts, 2U);
    EXPECT_EQ(static_cast<unsigned>(H5_SZIP_EC_OPTION_MASK |
                                    H5_SZIP_ALLOW_K13_OPTION_MASK |
                                    H5_SZIP_RAW_OPTION_MASK),
              cd_values[0]);
    EXPECT_EQ(8U, cd_values[1]);

    ScopedPlist dcpl_nn(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_nn.valid());
    EXPECT_GE(
        H5Pset_szip(dcpl_nn.get(), H5_SZIP_NN_OPTION_MASK,
                    H5_SZIP_MAX_PIXELS_PER_BLOCK),
        0);
  }

  // 2. Shuffle, Nbit, and ScaleOffset valid configurations.
  {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    EXPECT_GE(H5Pset_shuffle(dcpl.get()), 0);
    EXPECT_GE(H5Pset_nbit(dcpl.get()), 0);
    EXPECT_GE(H5Pset_scaleoffset(dcpl.get(), H5Z_SO_FLOAT_DSCALE, 3), 0);
    EXPECT_GE(H5Pset_scaleoffset(dcpl.get(), H5Z_SO_FLOAT_ESCALE, 16), 0);
    EXPECT_GE(H5Pset_scaleoffset(dcpl.get(), H5Z_SO_INT, 0), 0);
  }

  // 3. Error paths across all filter helpers.
  ScopedErrorSilence silence;
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(dcpl.valid() && fapl.valid());

  EXPECT_LT(H5Pset_szip(dcpl.get(), H5_SZIP_NN_OPTION_MASK, 3), 0);
  EXPECT_LT(H5Pset_szip(dcpl.get(), H5_SZIP_NN_OPTION_MASK,
                        H5_SZIP_MAX_PIXELS_PER_BLOCK + 2),
            0);
  EXPECT_LT(H5Pset_szip(H5P_DATASET_CREATE_DEFAULT, H5_SZIP_EC_OPTION_MASK, 8),
            0);
  EXPECT_LT(H5Pset_szip(fapl.get(), H5_SZIP_EC_OPTION_MASK, 8), 0);
  EXPECT_LT(H5Pset_szip(H5I_INVALID_HID, H5_SZIP_EC_OPTION_MASK, 8), 0);

  // Note: upstream H5Pset_shuffle / H5Pset_nbit / H5Pset_scaleoffset only
  // check H5P_isa_class instead of H5P_object_verify(..., false), so passing
  // H5P_DATASET_CREATE_DEFAULT would mutate the global default singleton.
  EXPECT_LT(H5Pset_shuffle(fapl.get()), 0);
  EXPECT_LT(H5Pset_shuffle(H5I_INVALID_HID), 0);

  EXPECT_LT(H5Pset_nbit(fapl.get()), 0);
  EXPECT_LT(H5Pset_nbit(H5I_INVALID_HID), 0);

  EXPECT_LT(H5Pset_scaleoffset(dcpl.get(), H5Z_SO_INT, -1), 0);
  EXPECT_LT(
      H5Pset_scaleoffset(dcpl.get(), static_cast<H5Z_SO_scale_type_t>(99), 0),
      0);
  EXPECT_LT(H5Pset_scaleoffset(fapl.get(), H5Z_SO_INT, 0), 0);
  EXPECT_LT(H5Pset_scaleoffset(H5I_INVALID_HID, H5Z_SO_INT, 0), 0);
}

TEST(H5PdcplTest, FillValueSetGetConvertEncodeDecodeAndDefined) {
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());

  // 1. Default fill value produces zeros of the requested type.
  int32_t zero_i32 = 999;
  ASSERT_GE(H5Pget_fill_value(dcpl.get(), H5T_NATIVE_INT32, &zero_i32), 0);
  EXPECT_EQ(0, zero_i32);

  // 2. Scalar set/get covering narrower and wider destination datatypes.
  const int64_t val_i64 = 42;
  ASSERT_GE(H5Pset_fill_value(dcpl.get(), H5T_NATIVE_INT64, &val_i64), 0);
  H5D_fill_value_t status = H5D_FILL_VALUE_ERROR;
  ASSERT_GE(H5Pfill_value_defined(dcpl.get(), &status), 0);
  EXPECT_EQ(H5D_FILL_VALUE_USER_DEFINED, status);

  int16_t narrowed = -1;
  ASSERT_GE(H5Pget_fill_value(dcpl.get(), H5T_NATIVE_INT16, &narrowed), 0);
  EXPECT_EQ(42, narrowed);

  double widened = -1.0;
  ASSERT_GE(H5Pget_fill_value(dcpl.get(), H5T_NATIVE_DOUBLE, &widened), 0);
  EXPECT_DOUBLE_EQ(42.0, widened);

  // 3. Compound fill value exercises H5T_BKG_YES temporary background buffer in
  //    H5P_get_fill_value.
  struct SrcPair {
    int32_t a;
    float b;
  };
  struct DstPair {
    double b;
    int64_t a;
  };
  ScopedType comp_src(H5Tcreate(H5T_COMPOUND, sizeof(SrcPair)));
  ScopedType comp_dst(H5Tcreate(H5T_COMPOUND, sizeof(DstPair)));
  ASSERT_TRUE(comp_src.valid() && comp_dst.valid());
  ASSERT_GE(H5Tinsert(comp_src.get(), "a", offsetof(SrcPair, a),
                      H5T_NATIVE_INT32),
            0);
  ASSERT_GE(H5Tinsert(comp_src.get(), "b", offsetof(SrcPair, b),
                      H5T_NATIVE_FLOAT),
            0);
  ASSERT_GE(H5Tinsert(comp_dst.get(), "b", offsetof(DstPair, b),
                      H5T_NATIVE_DOUBLE),
            0);
  ASSERT_GE(H5Tinsert(comp_dst.get(), "a", offsetof(DstPair, a),
                      H5T_NATIVE_INT64),
            0);

  const SrcPair src_fill{.a = 7, .b = 3.5f};
  ASSERT_GE(H5Pset_fill_value(dcpl.get(), comp_src.get(), &src_fill), 0);
  DstPair dst_fill{};
  ASSERT_GE(H5Pget_fill_value(dcpl.get(), comp_dst.get(), &dst_fill), 0);
  EXPECT_EQ(7, dst_fill.a);
  EXPECT_DOUBLE_EQ(3.5, dst_fill.b);

  // 4. VL string fill value exercises non-noop H5T_convert in H5Pset_fill_value
  //    and H5T_VLEN detection in H5P_get_fill_value.
  ScopedType vlen_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(vlen_str.valid());
  ASSERT_GE(H5Tset_size(vlen_str.get(), H5T_VARIABLE), 0);
  const char *cstr_fill = "hello_fill";
  ASSERT_GE(H5Pset_fill_value(dcpl.get(), vlen_str.get(), &cstr_fill), 0);

  char *out_cstr = nullptr;
  ASSERT_GE(H5Pget_fill_value(dcpl.get(), vlen_str.get(), &out_cstr), 0);
  ScopedSpace scalar_sp(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar_sp.valid());
  absl::Cleanup reclaim_vlen = [&]() {
    if (out_cstr != nullptr) {
      H5Treclaim(vlen_str.get(), scalar_sp.get(), H5P_DEFAULT, &out_cstr);
    }
  };
  ASSERT_NE(nullptr, out_cstr);
  EXPECT_STREQ("hello_fill", out_cstr);

  // Compound containing a VL string sets force_conv and triggers
  // H5T__conv_struct_opt (need_bkg = H5T_BKG_YES), exercising temp bkg_buf
  // allocation and free in H5Pset_fill_value.
  {
    struct VlenComp {
      int32_t tag;
      const char *text;
    };
    ScopedType comp_vlen(H5Tcreate(H5T_COMPOUND, sizeof(VlenComp)));
    ASSERT_TRUE(comp_vlen.valid());
    ASSERT_GE(
        H5Tinsert(comp_vlen.get(), "tag", offsetof(VlenComp, tag),
                  H5T_NATIVE_INT32),
        0);
    ASSERT_GE(
        H5Tinsert(comp_vlen.get(), "text", offsetof(VlenComp, text),
                  vlen_str.get()),
        0);

    ScopedPlist comp_vlen_dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(comp_vlen_dcpl.valid());
    const VlenComp comp_in{.tag = 19, .text = "comp_vlen_fill"};
    ASSERT_GE(
        H5Pset_fill_value(comp_vlen_dcpl.get(), comp_vlen.get(), &comp_in), 0);

    VlenComp comp_out{};
    ASSERT_GE(
        H5Pget_fill_value(comp_vlen_dcpl.get(), comp_vlen.get(), &comp_out), 0);
    absl::Cleanup reclaim_comp = [&]() {
      H5Treclaim(comp_vlen.get(), scalar_sp.get(), H5P_DEFAULT, &comp_out);
    };
    EXPECT_EQ(19, comp_out.tag);
    ASSERT_NE(nullptr, comp_out.text);
    EXPECT_STREQ("comp_vlen_fill", comp_out.text);
  }

  // 5. Reference datatype fill value exercises H5T_REFERENCE detection.
  {
    ScopedPlist ref_dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(ref_dcpl.valid());
    hobj_ref_t ref_in{};
    ASSERT_GE(H5Pset_fill_value(ref_dcpl.get(), H5T_STD_REF_OBJ, &ref_in), 0);
    hobj_ref_t ref_out{};
    EXPECT_GE(H5Pget_fill_value(ref_dcpl.get(), H5T_STD_REF_OBJ, &ref_out), 0);
  }

  // 6. Encode/decode/get/copy/remove on user-defined and undefined fill values.
  {
    ScopedPlist enc_dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(enc_dcpl.valid());
    const int32_t fv = 1234;
    ASSERT_GE(H5Pset_fill_value(enc_dcpl.get(), H5T_NATIVE_INT32, &fv), 0);

    std::vector<uint8_t> buf = EncodePlist(enc_dcpl.get());
    ASSERT_FALSE(buf.empty());
    ScopedPlist dec_dcpl(H5Pdecode(buf.data()));
    ASSERT_TRUE(dec_dcpl.valid());
    int32_t dec_fv = 0;
    ASSERT_GE(H5Pget_fill_value(dec_dcpl.get(), H5T_NATIVE_INT32, &dec_fv), 0);
    EXPECT_EQ(1234, dec_fv);
    EXPECT_GT(H5Pequal(enc_dcpl.get(), dec_dcpl.get()), 0);

    ScopedPlist cp_dcpl(H5Pcopy(enc_dcpl.get()));
    ASSERT_TRUE(cp_dcpl.valid());
    EXPECT_GT(H5Pequal(enc_dcpl.get(), cp_dcpl.get()), 0);

    H5O_fill_t raw_fill{};
    ASSERT_GE(H5Pget(enc_dcpl.get(), H5D_CRT_FILL_VALUE_NAME, &raw_fill), 0);
    EXPECT_EQ(sizeof(int32_t), static_cast<size_t>(raw_fill.size));
    // Re-setting triggers _set + _del.
    auto *p_enc = static_cast<H5P_genplist_t *>(H5I_object(enc_dcpl.get()));
    ASSERT_NE(nullptr, p_enc);
    ASSERT_GE(H5P_set(p_enc, H5D_CRT_FILL_VALUE_NAME, &raw_fill), 0);
    EXPECT_GE(H5O_msg_reset(H5O_FILL_ID, &raw_fill), 0);
    EXPECT_GE(H5Premove(cp_dcpl.get(), H5D_CRT_FILL_VALUE_NAME), 0);

    // Undefine fill value -> encode/decode -> verify undefined status.
    ASSERT_GE(H5Pset_fill_value(enc_dcpl.get(), H5T_NATIVE_INT32, nullptr), 0);
    ASSERT_GE(H5Pfill_value_defined(enc_dcpl.get(), &status), 0);
    EXPECT_EQ(H5D_FILL_VALUE_UNDEFINED, status);

    std::vector<uint8_t> ubuf = EncodePlist(enc_dcpl.get());
    ASSERT_FALSE(ubuf.empty());
    ScopedPlist udec(H5Pdecode(ubuf.data()));
    ASSERT_TRUE(udec.valid());
    H5D_fill_value_t ustatus = H5D_FILL_VALUE_ERROR;
    ASSERT_GE(H5Pfill_value_defined(udec.get(), &ustatus), 0);
    EXPECT_EQ(H5D_FILL_VALUE_UNDEFINED, ustatus);
  }
}

TEST(H5PdcplTest, FillValueCustomConverterBkgAndErrors) {
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());

  // 1. Register succeeding and failing custom H5T_BKG_TEMP converters on distinct
  //    opaque types so both H5T_get_size(dst) >= H5T_get_size(src) and < paths
  //    are covered cleanly in H5P_get_fill_value without global side effects.
  ScopedType opq_src(H5Tcreate(H5T_OPAQUE, 4));
  ScopedType opq_ok_dst(H5Tcreate(H5T_OPAQUE, 4));
  ScopedType opq_fail_big(H5Tcreate(H5T_OPAQUE, 8));
  ScopedType opq_fail_small(H5Tcreate(H5T_OPAQUE, 2));
  ASSERT_TRUE(opq_src.valid() && opq_ok_dst.valid() && opq_fail_big.valid() &&
              opq_fail_small.valid());
  ASSERT_GE(H5Tset_tag(opq_src.get(), "h5pdcpl_src"), 0);
  ASSERT_GE(H5Tset_tag(opq_ok_dst.get(), "h5pdcpl_ok_dst"), 0);
  ASSERT_GE(H5Tset_tag(opq_fail_big.get(), "h5pdcpl_fail_big"), 0);
  ASSERT_GE(H5Tset_tag(opq_fail_small.get(), "h5pdcpl_fail_small"), 0);

  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "h5pdcpl_bkg_ok", opq_src.get(),
                        opq_ok_dst.get(), H5PdcplTestSucceedingBkgTempConv),
            0);
  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "h5pdcpl_bkg_fail_big", opq_src.get(),
                        opq_fail_big.get(), H5PdcplTestFailingBkgTempConv),
            0);
  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "h5pdcpl_bkg_fail_small", opq_src.get(),
                        opq_fail_small.get(), H5PdcplTestFailingBkgTempConv),
            0);
  absl::Cleanup unregister_convs = [&]() {
    H5Tunregister(H5T_PERS_HARD, "h5pdcpl_bkg_ok", opq_src.get(),
                  opq_ok_dst.get(), H5PdcplTestSucceedingBkgTempConv);
    H5Tunregister(H5T_PERS_HARD, "h5pdcpl_bkg_fail_big", opq_src.get(),
                  opq_fail_big.get(), H5PdcplTestFailingBkgTempConv);
    H5Tunregister(H5T_PERS_HARD, "h5pdcpl_bkg_fail_small", opq_src.get(),
                  opq_fail_small.get(), H5PdcplTestFailingBkgTempConv);
  };

  const std::array<uint8_t, 4> src_val = {1, 2, 3, 4};
  ASSERT_GE(H5Pset_fill_value(dcpl.get(), opq_src.get(), src_val.data()), 0);
  std::array<uint8_t, 4> ok_out{};
  ASSERT_GE(H5Pget_fill_value(dcpl.get(), opq_ok_dst.get(), ok_out.data()), 0);
  EXPECT_EQ(0x5a, ok_out[0]);
  EXPECT_EQ(0x5a, ok_out[1]);

  // 2. Negative tests cleanly free temporary buf/bkg in H5P_get_fill_value.
  ScopedErrorSilence silence;
  std::array<uint8_t, 8> big_out{};
  std::array<uint8_t, 2> small_out{};
  EXPECT_LT(
      H5Pget_fill_value(dcpl.get(), opq_fail_big.get(), big_out.data()), 0);
  EXPECT_LT(
      H5Pget_fill_value(dcpl.get(), opq_fail_small.get(), small_out.data()), 0);

  // Incompatible datatype conversion (H5T_path_find == NULL).
  int32_t incompat_out = 0;
  EXPECT_LT(
      H5Pget_fill_value(dcpl.get(), H5T_NATIVE_INT32, &incompat_out), 0);

  // Invalid H5O_fill_t combination in H5P_is_fill_value_defined.
  uint8_t dummy = 0;
  H5O_fill_t bad_fill{};
  bad_fill.size = 0;
  bad_fill.buf = &dummy;
  H5D_fill_value_t status = H5D_FILL_VALUE_DEFAULT;
  EXPECT_LT(H5P_is_fill_value_defined(&bad_fill, &status), 0);
  EXPECT_EQ(H5D_FILL_VALUE_ERROR, status);

  // Undefined fill value -> H5Pget_fill_value fails.
  ASSERT_GE(H5Pset_fill_value(dcpl.get(), opq_src.get(), nullptr), 0);
  EXPECT_LT(H5Pget_fill_value(dcpl.get(), opq_ok_dst.get(), ok_out.data()), 0);

  // Argument & ID validation errors for set/get/defined.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  EXPECT_LT(H5Pset_fill_value(dcpl.get(), H5I_INVALID_HID, src_val.data()), 0);
  EXPECT_LT(
      H5Pset_fill_value(H5P_DATASET_CREATE_DEFAULT, H5T_NATIVE_INT32, &dummy),
      0);
  EXPECT_LT(H5Pset_fill_value(fapl.get(), H5T_NATIVE_INT32, &dummy), 0);
  EXPECT_LT(H5Pset_fill_value(H5I_INVALID_HID, H5T_NATIVE_INT32, &dummy), 0);

  EXPECT_LT(H5Pget_fill_value(dcpl.get(), H5I_INVALID_HID, &dummy), 0);
  EXPECT_LT(H5Pget_fill_value(dcpl.get(), H5T_NATIVE_INT32, nullptr), 0);
  EXPECT_LT(H5Pget_fill_value(fapl.get(), H5T_NATIVE_INT32, &dummy), 0);
  EXPECT_LT(H5Pget_fill_value(H5I_INVALID_HID, H5T_NATIVE_INT32, &dummy), 0);

  EXPECT_LT(H5Pfill_value_defined(fapl.get(), &status), 0);
  EXPECT_LT(H5Pfill_value_defined(H5I_INVALID_HID, &status), 0);
}

TEST(H5PdcplTest, FillValueCmpCoversAllFields) {
  auto *t_i32 = static_cast<H5T_t *>(H5I_object(H5T_NATIVE_INT32));
  auto *t_f32 = static_cast<H5T_t *>(H5I_object(H5T_NATIVE_FLOAT));
  ASSERT_NE(nullptr, t_i32);
  ASSERT_NE(nullptr, t_f32);

  int32_t v1 = 10;
  int32_t v2 = 20;

  H5O_fill_t f1{};
  H5O_fill_t f2{};
  f1.size = 4;
  f1.type = t_i32;
  f1.buf = &v1;
  f1.alloc_time = H5D_ALLOC_TIME_EARLY;
  f1.fill_time = H5D_FILL_TIME_ALLOC;

  f2 = f1;
  EXPECT_EQ(0, H5P_fill_value_cmp(&f1, &f2, sizeof(H5O_fill_t)));

  // size < / >
  f2.size = 8;
  EXPECT_LT(H5P_fill_value_cmp(&f1, &f2, sizeof(H5O_fill_t)), 0);
  EXPECT_GT(H5P_fill_value_cmp(&f2, &f1, sizeof(H5O_fill_t)), 0);
  f2.size = 4;

  // type NULL vs non-NULL
  f1.type = nullptr;
  EXPECT_LT(H5P_fill_value_cmp(&f1, &f2, sizeof(H5O_fill_t)), 0);
  EXPECT_GT(H5P_fill_value_cmp(&f2, &f1, sizeof(H5O_fill_t)), 0);
  f1.type = t_i32;

  // type mismatch
  f2.type = t_f32;
  EXPECT_NE(0, H5P_fill_value_cmp(&f1, &f2, sizeof(H5O_fill_t)));
  f2.type = t_i32;

  // buf NULL vs non-NULL
  f1.buf = nullptr;
  EXPECT_LT(H5P_fill_value_cmp(&f1, &f2, sizeof(H5O_fill_t)), 0);
  EXPECT_GT(H5P_fill_value_cmp(&f2, &f1, sizeof(H5O_fill_t)), 0);
  f1.buf = &v1;

  // buf contents mismatch
  f2.buf = &v2;
  EXPECT_NE(0, H5P_fill_value_cmp(&f1, &f2, sizeof(H5O_fill_t)));
  f2.buf = &v1;

  // alloc_time < / >
  f2.alloc_time = H5D_ALLOC_TIME_LATE;
  EXPECT_LT(H5P_fill_value_cmp(&f1, &f2, sizeof(H5O_fill_t)), 0);
  EXPECT_GT(H5P_fill_value_cmp(&f2, &f1, sizeof(H5O_fill_t)), 0);
  f2.alloc_time = H5D_ALLOC_TIME_EARLY;

  // fill_time < / >
  f2.fill_time = H5D_FILL_TIME_NEVER;
  EXPECT_LT(H5P_fill_value_cmp(&f1, &f2, sizeof(H5O_fill_t)), 0);
  EXPECT_GT(H5P_fill_value_cmp(&f2, &f1, sizeof(H5O_fill_t)), 0);
}

TEST(H5PdcplTest, AllocTimeFillTimeAndNoAttrsHint) {
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());

  // 1. Alloc time explicit values & nullptr query.
  for (H5D_alloc_time_t at :
       {H5D_ALLOC_TIME_EARLY, H5D_ALLOC_TIME_LATE, H5D_ALLOC_TIME_INCR}) {
    ASSERT_GE(H5Pset_alloc_time(dcpl.get(), at), 0);
    H5D_alloc_time_t got = H5D_ALLOC_TIME_ERROR;
    ASSERT_GE(H5Pget_alloc_time(dcpl.get(), &got), 0);
    EXPECT_EQ(at, got);
  }
  EXPECT_GE(H5Pget_alloc_time(dcpl.get(), nullptr), 0);

  // 2. Fill time values & nullptr query.
  for (H5D_fill_time_t ft :
       {H5D_FILL_TIME_ALLOC, H5D_FILL_TIME_NEVER, H5D_FILL_TIME_IFSET}) {
    ASSERT_GE(H5Pset_fill_time(dcpl.get(), ft), 0);
    H5D_fill_time_t got = H5D_FILL_TIME_ERROR;
    ASSERT_GE(H5Pget_fill_time(dcpl.get(), &got), 0);
    EXPECT_EQ(ft, got);
  }
  EXPECT_GE(H5Pget_fill_time(dcpl.get(), nullptr), 0);

  // 3. No-attrs hint toggle, encode/decode, and dataset creation in Core VFD.
  ASSERT_GE(H5Pset_dset_no_attrs_hint(dcpl.get(), true), 0);
  bool hint = false;
  ASSERT_GE(H5Pget_dset_no_attrs_hint(dcpl.get(), &hint), 0);
  EXPECT_TRUE(hint);

  std::vector<uint8_t> encoded = EncodePlist(dcpl.get());
  ASSERT_FALSE(encoded.empty());
  ScopedPlist decoded(H5Pdecode(encoded.data()));
  ASSERT_TRUE(decoded.valid());
  hint = false;
  ASSERT_GE(H5Pget_dset_no_attrs_hint(decoded.get(), &hint), 0);
  EXPECT_TRUE(hint);

  ScopedFile file = CreateInMemoryFile("no_attrs.h5");
  ASSERT_TRUE(file.valid());
  const std::array<hsize_t, 1> dims = {4};
  ScopedSpace space(H5Screate_simple(1, dims.data(), nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "d", H5T_NATIVE_INT32, space.get(),
                                H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  ScopedPlist read_dcpl(H5Dget_create_plist(dset.get()));
  ASSERT_TRUE(read_dcpl.valid());

  ASSERT_GE(H5Pset_dset_no_attrs_hint(dcpl.get(), false), 0);
  ASSERT_GE(H5Pget_dset_no_attrs_hint(dcpl.get(), &hint), 0);
  EXPECT_FALSE(hint);

  // 4. Error paths for alloc_time, fill_time, and dset_no_attrs_hint.
  ScopedErrorSilence silence;
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());

  EXPECT_LT(
      H5Pset_alloc_time(dcpl.get(), static_cast<H5D_alloc_time_t>(99)), 0);
  EXPECT_LT(H5Pset_alloc_time(dcpl.get(), H5D_ALLOC_TIME_ERROR), 0);
  EXPECT_LT(
      H5Pset_alloc_time(H5P_DATASET_CREATE_DEFAULT, H5D_ALLOC_TIME_EARLY), 0);
  EXPECT_LT(H5Pset_alloc_time(fapl.get(), H5D_ALLOC_TIME_EARLY), 0);
  EXPECT_LT(H5Pset_alloc_time(H5I_INVALID_HID, H5D_ALLOC_TIME_EARLY), 0);

  H5D_alloc_time_t bad_at = H5D_ALLOC_TIME_ERROR;
  EXPECT_LT(H5Pget_alloc_time(fapl.get(), &bad_at), 0);
  EXPECT_LT(H5Pget_alloc_time(H5I_INVALID_HID, &bad_at), 0);

  EXPECT_LT(H5Pset_fill_time(dcpl.get(), static_cast<H5D_fill_time_t>(99)), 0);
  EXPECT_LT(H5Pset_fill_time(dcpl.get(), H5D_FILL_TIME_ERROR), 0);
  EXPECT_LT(H5Pset_fill_time(H5P_DATASET_CREATE_DEFAULT, H5D_FILL_TIME_ALLOC),
            0);
  EXPECT_LT(H5Pset_fill_time(fapl.get(), H5D_FILL_TIME_ALLOC), 0);
  EXPECT_LT(H5Pset_fill_time(H5I_INVALID_HID, H5D_FILL_TIME_ALLOC), 0);

  H5D_fill_time_t bad_ft = H5D_FILL_TIME_ERROR;
  EXPECT_LT(H5Pget_fill_time(fapl.get(), &bad_ft), 0);
  EXPECT_LT(H5Pget_fill_time(H5I_INVALID_HID, &bad_ft), 0);

  EXPECT_LT(H5Pget_dset_no_attrs_hint(dcpl.get(), nullptr), 0);
  EXPECT_LT(H5Pget_dset_no_attrs_hint(fapl.get(), &hint), 0);
  EXPECT_LT(H5Pget_dset_no_attrs_hint(H5I_INVALID_HID, &hint), 0);

  EXPECT_LT(H5Pset_dset_no_attrs_hint(H5P_DATASET_CREATE_DEFAULT, true), 0);
  EXPECT_LT(H5Pset_dset_no_attrs_hint(fapl.get(), true), 0);
  EXPECT_LT(H5Pset_dset_no_attrs_hint(H5I_INVALID_HID, true), 0);
}

TEST(H5PdcplTest, MissingPropertiesAndInternalCallbackEdgeCases) {
  // 1. Direct callback edge cases for invalid layout decode byte and
  //    external file list NULL slot / NULL slot name comparisons.
  {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    auto *plist = static_cast<H5P_genplist_t *>(H5I_object(dcpl.get()));
    ASSERT_NE(nullptr, plist);

    H5P_genprop_t *layout_prop =
        H5P__find_prop_plist(plist, H5D_CRT_LAYOUT_NAME);
    ASSERT_NE(nullptr, layout_prop);
    ASSERT_NE(nullptr, layout_prop->decode);
    {
      ScopedErrorSilence silence;
      const uint8_t bad_byte = static_cast<uint8_t>(H5D_NLAYOUTS);
      const void *pp = &bad_byte;
      H5O_layout_t bad_layout{};
      EXPECT_LT(layout_prop->decode(&pp, &bad_layout), 0);
    }

    H5P_genprop_t *efl_prop =
        H5P__find_prop_plist(plist, H5D_CRT_EXT_FILE_LIST_NAME);
    ASSERT_NE(nullptr, efl_prop);
    ASSERT_NE(nullptr, efl_prop->cmp);

    H5O_efl_entry_t slot_null_name{};
    H5O_efl_entry_t slot_with_name{};
    char sample_name[] = "ext.bin";
    slot_with_name.name = sample_name;

    H5O_efl_t efl_null_slot{};
    efl_null_slot.nalloc = 1;
    H5O_efl_t efl_nonnull_slot{};
    efl_nonnull_slot.nalloc = 1;
    efl_nonnull_slot.slot = &slot_null_name;
    EXPECT_LT(
        efl_prop->cmp(&efl_null_slot, &efl_nonnull_slot, sizeof(H5O_efl_t)),
        0);
    EXPECT_GT(
        efl_prop->cmp(&efl_nonnull_slot, &efl_null_slot, sizeof(H5O_efl_t)),
        0);

    H5O_efl_t efl_entry_null_name{};
    efl_entry_null_name.nalloc = 1;
    efl_entry_null_name.nused = 1;
    efl_entry_null_name.slot = &slot_null_name;

    H5O_efl_t efl_entry_with_name{};
    efl_entry_with_name.nalloc = 1;
    efl_entry_with_name.nused = 1;
    efl_entry_with_name.slot = &slot_with_name;
    EXPECT_LT(efl_prop->cmp(&efl_entry_null_name, &efl_entry_with_name,
                            sizeof(H5O_efl_t)),
              0);
    EXPECT_GT(efl_prop->cmp(&efl_entry_with_name, &efl_entry_null_name,
                            sizeof(H5O_efl_t)),
              0);
  }

  // 2. Removing individual DCPL properties via H5Premove exercises all
  //    H5P_peek / H5P_get / H5P_set property lookup failure paths cleanly.
  ScopedErrorSilence silence;
  const std::array<hsize_t, 1> dims = {4};
  ScopedSpace space(H5Screate_simple(1, dims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  {
    ScopedPlist no_layout(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(no_layout.valid());
    ASSERT_GE(H5Premove(no_layout.get(), H5D_CRT_LAYOUT_NAME), 0);

    EXPECT_LT(H5Pset_layout(no_layout.get(), H5D_CONTIGUOUS), 0);
    EXPECT_EQ(H5D_LAYOUT_ERROR, H5Pget_layout(no_layout.get()));
    EXPECT_LT(H5Pset_chunk(no_layout.get(), 1, dims.data()), 0);
    hsize_t chunk_out = 0;
    EXPECT_LT(H5Pget_chunk(no_layout.get(), 1, &chunk_out), 0);
    EXPECT_LT(H5Pset_virtual(no_layout.get(), space.get(), "a.h5", "d",
                             space.get()),
              0);
    size_t vcount = 0;
    EXPECT_LT(H5Pget_virtual_count(no_layout.get(), &vcount), 0);
    EXPECT_LT(H5Pget_virtual_vspace(no_layout.get(), 0), 0);
    EXPECT_LT(H5Pget_virtual_srcspace(no_layout.get(), 0), 0);
    EXPECT_LT(H5Pget_virtual_filename(no_layout.get(), 0, nullptr, 0), 0);
    EXPECT_LT(H5Pget_virtual_dsetname(no_layout.get(), 0, nullptr, 0), 0);
    EXPECT_LT(H5Pset_chunk_opts(no_layout.get(), 0), 0);
    unsigned opts = 0;
    EXPECT_LT(H5Pget_chunk_opts(no_layout.get(), &opts), 0);
    EXPECT_LT(H5Pset_alloc_time(no_layout.get(), H5D_ALLOC_TIME_DEFAULT), 0);
  }

  {
    ScopedPlist no_alloc_state(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(no_alloc_state.valid());
    ASSERT_GE(H5Premove(no_alloc_state.get(), H5D_CRT_ALLOC_TIME_STATE_NAME),
              0);
    EXPECT_LT(H5Pset_layout(no_alloc_state.get(), H5D_CONTIGUOUS), 0);
    EXPECT_LT(H5Pset_alloc_time(no_alloc_state.get(), H5D_ALLOC_TIME_EARLY), 0);
  }

  {
    ScopedPlist no_fill(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(no_fill.valid());
    ASSERT_GE(H5Premove(no_fill.get(), H5D_CRT_FILL_VALUE_NAME), 0);
    EXPECT_LT(H5Pset_layout(no_fill.get(), H5D_CONTIGUOUS), 0);
    int32_t val = 1;
    EXPECT_LT(H5Pset_fill_value(no_fill.get(), H5T_NATIVE_INT32, &val), 0);
    EXPECT_LT(H5Pget_fill_value(no_fill.get(), H5T_NATIVE_INT32, &val), 0);
    H5D_fill_value_t fv_status = H5D_FILL_VALUE_ERROR;
    EXPECT_LT(H5Pfill_value_defined(no_fill.get(), &fv_status), 0);
    EXPECT_LT(H5Pset_alloc_time(no_fill.get(), H5D_ALLOC_TIME_EARLY), 0);
    H5D_alloc_time_t at = H5D_ALLOC_TIME_ERROR;
    EXPECT_LT(H5Pget_alloc_time(no_fill.get(), &at), 0);
    EXPECT_LT(H5Pset_fill_time(no_fill.get(), H5D_FILL_TIME_ALLOC), 0);
    H5D_fill_time_t ft = H5D_FILL_TIME_ERROR;
    EXPECT_LT(H5Pget_fill_time(no_fill.get(), &ft), 0);
  }

  {
    ScopedPlist no_efl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(no_efl.valid());
    ASSERT_GE(H5Premove(no_efl.get(), H5D_CRT_EXT_FILE_LIST_NAME), 0);
    EXPECT_LT(H5Pset_external(no_efl.get(), "x.dat", 0, 16), 0);
    EXPECT_LT(H5Pget_external_count(no_efl.get()), 0);
    EXPECT_LT(H5Pget_external(no_efl.get(), 0, 0, nullptr, nullptr, nullptr), 0);
  }

  {
    ScopedPlist no_pline(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(no_pline.valid());
    ASSERT_GE(H5Premove(no_pline.get(), H5O_CRT_PIPELINE_NAME), 0);
    EXPECT_LT(H5Pset_szip(no_pline.get(), H5_SZIP_EC_OPTION_MASK, 8), 0);
    EXPECT_LT(H5Pset_shuffle(no_pline.get()), 0);
    EXPECT_LT(H5Pset_nbit(no_pline.get()), 0);
    EXPECT_LT(H5Pset_scaleoffset(no_pline.get(), H5Z_SO_INT, 0), 0);
  }

  {
    ScopedPlist no_hint(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(no_hint.valid());
    ASSERT_GE(H5Premove(no_hint.get(), H5D_CRT_MIN_DSET_HDR_SIZE_NAME), 0);
    bool hint_out = false;
    EXPECT_LT(H5Pget_dset_no_attrs_hint(no_hint.get(), &hint_out), 0);
    EXPECT_LT(H5Pset_dset_no_attrs_hint(no_hint.get(), true), 0);
  }
}

}  // namespace
}  // namespace hdf5
