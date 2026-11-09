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
// Tests "all" dataspace selections and selection iterators in
// third_party/hdf5/src/H5Sall.c via public and internal APIs.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Sall.c

#include "third_party/hdf5/src/hdf5.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define H5SALL_TEST_HAS_ASAN 1
#endif
#endif
#if !defined(H5SALL_TEST_HAS_ASAN) && \
    (defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__))
#define H5SALL_TEST_HAS_ASAN 1
#endif

// clang-format off
// `:hdf5` compiles with -DNDEBUG; match internal struct layouts before
// including package/private headers.
#ifndef NDEBUG
#define H5SALL_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#ifdef try
#undef try
#endif
#define try hdf5_try_param
extern "C" {
#define H5S_FRIEND
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Spkg.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5SALL_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5SALL_TEST_RESTORE_UNDEF_NDEBUG
#endif
// clang-format on

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

H5S_t* UnwrapSpace(hid_t space_id) {
  return static_cast<H5S_t*>(H5I_object_verify(space_id, H5I_DATASPACE));
}

void EncodeUint32LittleEndian(uint8_t* dst, uint32_t value) {
  // HDF5's UINT32ENCODE serializes 32-bit unsigned integers in little-endian.
  dst[0] = static_cast<uint8_t>(value & 0xFF);
  dst[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  dst[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  dst[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

TEST(H5SallTest, SelectAllReplacesPriorSelectionsAndValidatesIds) {
  const hsize_t dims[2] = {4, 5};
  const hsize_t start[2] = {1, 1};
  const hsize_t count[2] = {2, 2};
  const hsize_t coord[2] = {0, 3};

  ScopedSpace space(H5Screate_simple(2, dims, nullptr));
  ASSERT_TRUE(space.valid());
  EXPECT_EQ(H5Sget_select_type(space.get()), H5S_SEL_ALL);
  EXPECT_EQ(H5Sget_select_npoints(space.get()), 20);
  EXPECT_GT(H5Sselect_valid(space.get()), 0);

  // Replace hyperslab selection with all.
  ASSERT_GE(
      H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, start, nullptr, count,
                          nullptr),
      0);
  ASSERT_EQ(H5Sget_select_npoints(space.get()), 4);
  ASSERT_GE(H5Sselect_all(space.get()), 0);
  EXPECT_EQ(H5Sget_select_type(space.get()), H5S_SEL_ALL);
  EXPECT_EQ(H5Sget_select_npoints(space.get()), 20);

  // Replace point selection and none selection with all.
  ASSERT_GE(H5Sselect_elements(space.get(), H5S_SELECT_SET, 1, coord), 0);
  ASSERT_GE(H5Sselect_all(space.get()), 0);
  EXPECT_EQ(H5Sget_select_npoints(space.get()), 20);

  ASSERT_GE(H5Sselect_none(space.get()), 0);
  ASSERT_EQ(H5Sget_select_npoints(space.get()), 0);
  ASSERT_GE(H5Sselect_all(space.get()), 0);
  EXPECT_EQ(H5Sget_select_npoints(space.get()), 20);

  // Scalar and null dataspaces.
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  EXPECT_GE(H5Sselect_all(scalar.get()), 0);
  EXPECT_EQ(H5Sget_select_npoints(scalar.get()), 1);

  ScopedSpace null_space(H5Screate(H5S_NULL));
  ASSERT_TRUE(null_space.valid());
  EXPECT_GE(H5Sselect_all(null_space.get()), 0);
  EXPECT_EQ(H5Sget_select_npoints(null_space.get()), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Sselect_all(H5I_INVALID_HID), 0);
}

TEST(H5SallTest, BoundsCopyPropertiesAdjustAndIntersectBlock) {
  const hsize_t dims[2] = {3, 4};
  ScopedSpace src(H5Screate_simple(2, dims, nullptr));
  ASSERT_TRUE(src.valid());
  ScopedSpace dst(H5Screate_simple(2, dims, nullptr));
  ASSERT_TRUE(dst.valid());
  ASSERT_GE(H5Sselect_none(dst.get()), 0);
  ASSERT_GE(H5Sselect_copy(dst.get(), src.get()), 0);
  EXPECT_EQ(H5Sget_select_type(dst.get()), H5S_SEL_ALL);
  EXPECT_EQ(H5Sget_select_npoints(dst.get()), 12);

  hsize_t start[2] = {99, 99};
  hsize_t end[2] = {99, 99};
  ASSERT_GE(H5Sget_select_bounds(src.get(), start, end), 0);
  EXPECT_THAT(start, ElementsAre(0, 0));
  EXPECT_THAT(end, ElementsAre(2, 3));

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  hsize_t scalar_start = 7;
  hsize_t scalar_end = 7;
  EXPECT_GE(H5Sget_select_bounds(scalar.get(), &scalar_start, &scalar_end), 0);

  const hsize_t blk_start[2] = {1, 1};
  const hsize_t blk_end[2] = {2, 2};
  EXPECT_GT(H5Sselect_intersect_block(src.get(), blk_start, blk_end), 0);

  const hssize_t s_off[2] = {0, 0};
  EXPECT_GE(H5Sselect_adjust(src.get(), s_off), 0);

  H5S_t* src_ptr = UnwrapSpace(src.get());
  ASSERT_NE(src_ptr, nullptr);

  hsize_t offset = 99;
  ASSERT_GE(H5S_get_select_offset(src_ptr, &offset), 0);
  EXPECT_EQ(offset, 0);
  EXPECT_EQ(H5S_get_select_unlim_dim(src_ptr), -1);
  EXPECT_GT(H5S_select_is_contiguous(src_ptr), 0);
  EXPECT_GT(H5S_select_is_single(src_ptr), 0);
  EXPECT_GT(H5S_select_is_regular(src_ptr), 0);

  const hsize_t u_off[2] = {1, 2};
  EXPECT_GE(H5S_select_adjust_u(src_ptr, u_off), 0);

  // Copy sharing selection internally.
  ScopedSpace shared_dst(H5Screate_simple(2, dims, nullptr));
  ASSERT_TRUE(shared_dst.valid());
  H5S_t* shared_ptr = UnwrapSpace(shared_dst.get());
  ASSERT_NE(shared_ptr, nullptr);
  EXPECT_GE(H5S_select_copy(shared_ptr, src_ptr, /*share_selection=*/true), 0);
}

TEST(H5SallTest, ShapeSameAndProjections) {
  const hsize_t dims_2d[2] = {3, 4};
  const hsize_t dims_2d_alt[2] = {2, 6};
  const hsize_t dims_4d_lead1[4] = {1, 1, 3, 4};
  const hsize_t dims_3d_bad[3] = {2, 3, 4};
  const hsize_t dims_one[2] = {1, 1};

  ScopedSpace s_2d(H5Screate_simple(2, dims_2d, nullptr));
  ScopedSpace s_2d_same(H5Screate_simple(2, dims_2d, nullptr));
  ScopedSpace s_2d_alt(H5Screate_simple(2, dims_2d_alt, nullptr));
  ScopedSpace s_4d_lead1(H5Screate_simple(4, dims_4d_lead1, nullptr));
  ScopedSpace s_3d_bad(H5Screate_simple(3, dims_3d_bad, nullptr));
  ScopedSpace s_one(H5Screate_simple(2, dims_one, nullptr));
  ASSERT_TRUE(s_2d.valid() && s_2d_same.valid() && s_2d_alt.valid() &&
              s_4d_lead1.valid() && s_3d_bad.valid() && s_one.valid());

  // Public H5Sselect_shape_same against a 1-block contiguous hyperslab exercises
  // iterators and single-block matching.
  const hsize_t h_start[2] = {0, 0};
  const hsize_t h_count[2] = {3, 4};
  ScopedSpace h_space(H5Screate_simple(2, dims_2d, nullptr));
  ASSERT_TRUE(h_space.valid());
  ASSERT_GE(
      H5Sselect_hyperslab(h_space.get(), H5S_SELECT_SET, h_start, nullptr,
                          h_count, nullptr),
      0);
  EXPECT_GT(H5Sselect_shape_same(s_2d.get(), h_space.get()), 0);

  // H5S_select_shape_same short-circuits when both spaces are single-block
  // (`H5S__all_is_single` == true), so invoke `H5S_sel_all[0].shape_same`
  // directly to cover all dimensions/extra-rank checks.
  H5S_t* p_2d = UnwrapSpace(s_2d.get());
  H5S_t* p_2d_same = UnwrapSpace(s_2d_same.get());
  H5S_t* p_2d_alt = UnwrapSpace(s_2d_alt.get());
  H5S_t* p_4d_lead1 = UnwrapSpace(s_4d_lead1.get());
  H5S_t* p_3d_bad = UnwrapSpace(s_3d_bad.get());
  H5S_t* p_one = UnwrapSpace(s_one.get());
  ASSERT_TRUE(p_2d && p_2d_same && p_2d_alt && p_4d_lead1 && p_3d_bad &&
              p_one);

  EXPECT_GT(H5S_sel_all[0].shape_same(p_2d, p_2d_same), 0);
  EXPECT_EQ(H5S_sel_all[0].shape_same(p_2d, p_2d_alt), 0);
  EXPECT_GT(H5S_sel_all[0].shape_same(p_4d_lead1, p_2d), 0);
  EXPECT_EQ(H5S_sel_all[0].shape_same(p_3d_bad, p_2d), 0);

  // Scalar and simple projections.
  hsize_t proj_off = 42;
  EXPECT_GE(H5S_select_project_scalar(p_one, &proj_off), 0);
  EXPECT_EQ(proj_off, 0);

  const hsize_t proj_dims[2] = {3, 4};
  ScopedSpace proj_dst(H5Screate_simple(2, proj_dims, nullptr));
  ASSERT_TRUE(proj_dst.valid());
  ASSERT_GE(H5Sselect_none(proj_dst.get()), 0);
  H5S_t* p_proj_dst = UnwrapSpace(proj_dst.get());
  ASSERT_NE(p_proj_dst, nullptr);
  EXPECT_GE(H5S_select_project_simple(p_2d, p_proj_dst, &proj_off), 0);
  EXPECT_EQ(H5S_get_select_type(p_proj_dst), H5S_SEL_ALL);

  // Construct projections to scalar (rank 0) and higher rank (rank 3).
  H5S_t* new_scalar_proj = nullptr;
  ptrdiff_t buf_adj = 0;
  ASSERT_GE(H5S_select_construct_projection(p_one, &new_scalar_proj, 0,
                                            sizeof(int), &buf_adj),
            0);
  ASSERT_NE(new_scalar_proj, nullptr);
  EXPECT_EQ(H5S_get_simple_extent_type(new_scalar_proj), H5S_SCALAR);
  EXPECT_GE(H5S_close(new_scalar_proj), 0);

  H5S_t* new_simple_proj = nullptr;
  ASSERT_GE(H5S_select_construct_projection(p_2d, &new_simple_proj, 3,
                                            sizeof(int), &buf_adj),
            0);
  ASSERT_NE(new_simple_proj, nullptr);
  EXPECT_EQ(H5S_get_select_type(new_simple_proj), H5S_SEL_ALL);
  EXPECT_GE(H5S_close(new_simple_proj), 0);
}

TEST(H5SallTest, SerializeDeserializeRoundTripAndErrorPaths) {
  const hsize_t dims[2] = {2, 3};
  ScopedSpace src(H5Screate_simple(2, dims, nullptr));
  ASSERT_TRUE(src.valid());
  H5S_t* src_ptr = UnwrapSpace(src.get());
  ASSERT_NE(src_ptr, nullptr);

  EXPECT_EQ(H5S_select_serial_size(src_ptr), 16);

  std::vector<uint8_t> ser_buf(16, 0);
  uint8_t* write_ptr = ser_buf.data();
  ASSERT_GE(H5S_select_serialize(src_ptr, &write_ptr), 0);
  EXPECT_EQ(write_ptr - ser_buf.data(), 16);

  // Deserialize with *space == nullptr allocates temporary dataspace.
  H5S_t* alloc_space = nullptr;
  const uint8_t* read_ptr = ser_buf.data();
  ASSERT_GE(H5S_select_deserialize(&alloc_space, &read_ptr, ser_buf.size()), 0);
  ASSERT_NE(alloc_space, nullptr);
  absl::Cleanup close_alloc = [&] {
    if (alloc_space != nullptr) {
      H5S_close(alloc_space);
    }
  };
  EXPECT_EQ(H5S_get_select_type(alloc_space), H5S_SEL_ALL);
  EXPECT_EQ(read_ptr - ser_buf.data(), 16);

  // Deserialize into existing dataspace with prior non-all selection.
  ScopedSpace existing(H5Screate_simple(2, dims, nullptr));
  ASSERT_TRUE(existing.valid());
  const hsize_t coord[2] = {1, 2};
  ASSERT_GE(H5Sselect_elements(existing.get(), H5S_SELECT_SET, 1, coord), 0);
  H5S_t* existing_ptr = UnwrapSpace(existing.get());
  ASSERT_NE(existing_ptr, nullptr);
  read_ptr = ser_buf.data();
  ASSERT_GE(H5S_select_deserialize(&existing_ptr, &read_ptr, ser_buf.size()),
            0);
  EXPECT_EQ(H5S_get_select_type(existing_ptr), H5S_SEL_ALL);
  EXPECT_EQ(H5S_get_select_npoints(existing_ptr), 6);

  // Public H5Sencode2 / H5Sdecode round-trip (uses SIZE_MAX -> skip = true).
  size_t enc_size = 0;
  ASSERT_GE(H5Sencode2(src.get(), nullptr, &enc_size, H5P_DEFAULT), 0);
  std::vector<uint8_t> enc_buf(enc_size, 0);
  ASSERT_GE(H5Sencode2(src.get(), enc_buf.data(), &enc_size, H5P_DEFAULT), 0);
#if !defined(H5SALL_TEST_HAS_ASAN)
  // Upstream H5S_decode calls H5S_select_deserialize(..., SIZE_MAX), where
  // `*p + SIZE_MAX - 1` in H5Sselect.c triggers UBSan pointer-overflow under
  // `--config=asan-chlor`.
  ScopedSpace decoded(H5Sdecode(enc_buf.data()));
  ASSERT_TRUE(decoded.valid());
  EXPECT_EQ(H5Sget_select_type(decoded.get()), H5S_SEL_ALL);
#endif

  // Buffer-overflow & bad-version error paths with *space == nullptr.
  ScopedErrorSilence silence;
  H5S_t* err_space = nullptr;
  const uint8_t* err_p = ser_buf.data();
  // Truncated before version (`p_size = 6` -> 2 bytes after sel_type).
  EXPECT_LT(H5S_select_deserialize(&err_space, &err_p, 6), 0);
  EXPECT_EQ(err_space, nullptr);

  // Truncated after version (`p_size = 10` -> 2 bytes after version < 8).
  err_p = ser_buf.data();
  EXPECT_LT(H5S_select_deserialize(&err_space, &err_p, 10), 0);
  EXPECT_EQ(err_space, nullptr);

  // Bad version numbers: 0 and 2.
  for (const uint32_t bad_ver : {uint32_t{0}, uint32_t{2}}) {
    std::vector<uint8_t> bad_buf = ser_buf;
    EncodeUint32LittleEndian(bad_buf.data() + 4, bad_ver);
    err_p = bad_buf.data();
    EXPECT_LT(H5S_select_deserialize(&err_space, &err_p, bad_buf.size()), 0);
    EXPECT_EQ(err_space, nullptr);
  }
}

TEST(H5SallTest, SelectionIteratorPublicAndInternalCallbacks) {
  const hsize_t dims[2] = {3, 4};
  ScopedSpace space(H5Screate_simple(2, dims, nullptr));
  ASSERT_TRUE(space.valid());

  // Public iterator API across partial batches, reset, and completion.
  hid_t iter_id = H5Ssel_iter_create(space.get(), sizeof(int32_t), 0);
  ASSERT_GE(iter_id, 0);
  absl::Cleanup close_iter = [&] {
    if (iter_id >= 0) {
      H5Ssel_iter_close(iter_id);
    }
  };

  hsize_t seq_off[4] = {};
  size_t seq_len[4] = {};
  size_t nseq = 0;
  size_t nelem = 0;
  ASSERT_GE(H5Ssel_iter_get_seq_list(iter_id, 4, 5, &nseq, &nelem, seq_off,
                                     seq_len),
            0);
  EXPECT_EQ(nseq, 1U);
  EXPECT_EQ(nelem, 5U);
  EXPECT_EQ(seq_off[0], 0U);
  EXPECT_EQ(seq_len[0], 5U * sizeof(int32_t));

  ASSERT_GE(H5Ssel_iter_get_seq_list(iter_id, 4, 32, &nseq, &nelem, seq_off,
                                     seq_len),
            0);
  EXPECT_EQ(nseq, 1U);
  EXPECT_EQ(nelem, 7U);
  EXPECT_EQ(seq_off[0], 5U * sizeof(int32_t));
  EXPECT_EQ(seq_len[0], 7U * sizeof(int32_t));

  ASSERT_GE(H5Ssel_iter_reset(iter_id, space.get()), 0);
  ASSERT_GE(H5Ssel_iter_get_seq_list(iter_id, 4, 12, &nseq, &nelem, seq_off,
                                     seq_len),
            0);
  EXPECT_EQ(nelem, 12U);
  ASSERT_GE(H5Ssel_iter_close(iter_id), 0);
  iter_id = H5I_INVALID_HID;

  // Internal iterator callbacks for coords, block, nelmts, has_next_block,
  // iter_next, and iter_next_block.
  H5S_t* space_ptr = UnwrapSpace(space.get());
  ASSERT_NE(space_ptr, nullptr);

  H5S_sel_iter_t iter{};
  ASSERT_GE(H5S_select_iter_init(&iter, space_ptr, sizeof(int32_t), 0), 0);
  absl::Cleanup release_iter = [&] { H5S_select_iter_release(&iter); };

  EXPECT_EQ(H5S_select_iter_nelmts(&iter), 12U);
  EXPECT_EQ(iter.type->iter_has_next_block(&iter), 0);

  hsize_t coords[2] = {99, 99};
  ASSERT_GE(H5S_select_iter_coords(&iter, coords), 0);
  EXPECT_THAT(coords, ElementsAre(0, 0));

  hsize_t blk_start[2] = {99, 99};
  hsize_t blk_end[2] = {99, 99};
  ASSERT_GE(iter.type->iter_block(&iter, blk_start, blk_end), 0);
  EXPECT_THAT(blk_start, ElementsAre(0, 0));
  EXPECT_THAT(blk_end, ElementsAre(2, 3));

  ASSERT_GE(H5S_select_iter_next(&iter, 5), 0);
  EXPECT_EQ(H5S_select_iter_nelmts(&iter), 7U);
  ASSERT_GE(H5S_select_iter_coords(&iter, coords), 0);
  EXPECT_THAT(coords, ElementsAre(1, 1));

  EXPECT_LT(iter.type->iter_next_block(&iter), 0);

  ASSERT_GE(H5S_select_iter_get_seq_list(&iter, 2, 12, &nseq, &nelem, seq_off,
                                         seq_len),
            0);
  EXPECT_EQ(nseq, 1U);
  EXPECT_EQ(nelem, 7U);
  EXPECT_EQ(seq_off[0], 5U * sizeof(int32_t));
  EXPECT_EQ(seq_len[0], 7U * sizeof(int32_t));

  // Verify scalar iterator behavior as well (`rank == 0`).
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  H5S_t* scalar_ptr = UnwrapSpace(scalar.get());
  ASSERT_NE(scalar_ptr, nullptr);
  H5S_sel_iter_t scalar_iter{};
  ASSERT_GE(H5S_select_iter_init(&scalar_iter, scalar_ptr, sizeof(int32_t), 0),
            0);
  absl::Cleanup release_scalar_iter = [&] {
    H5S_select_iter_release(&scalar_iter);
  };
  hsize_t s_start = 0;
  hsize_t s_end = 0;
  EXPECT_GE(scalar_iter.type->iter_block(&scalar_iter, &s_start, &s_end), 0);
  EXPECT_EQ(H5S_select_iter_nelmts(&scalar_iter), 1U);
}

}  // namespace
}  // namespace hdf5
