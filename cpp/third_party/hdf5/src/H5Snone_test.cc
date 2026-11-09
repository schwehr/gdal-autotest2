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
// Tests HDF5 "none" dataspace selections (`H5S_sel_none`, `H5Sselect_none`,
// `H5S_select_none`), serialization/deserialization, query/shape/projection
// callbacks, selection iterator callbacks, release-error propagation, and Core
// VFD dataset I/O behavior (third_party/hdf5/src/H5Snone.c).
//
// Google3 build caveats:
//   - Compiles `:hdf5` with `-DNDEBUG` (internal structs omit debug fields).
//   - Parallel MPI (H5_HAVE_PARALLEL) is disabled.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Snone.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with `-DNDEBUG`, which omits `#ifndef NDEBUG` struct
// fields inside internal HDF5 headers. Match that macro around internal headers
// so internal struct layouts match the compiled library ABI.
#ifndef NDEBUG
#define H5SNONE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5S_FRIEND
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Spkg.h"
#include "third_party/hdf5/src/H5Sprivate.h"
}  // extern "C"
#ifdef H5SNONE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5SNONE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

H5S_t* UnwrapSpace(hid_t space_id) {
  return static_cast<H5S_t*>(H5I_object_verify(space_id, H5I_DATASPACE));
}

herr_t FailingSelectionRelease(H5S_t* /*space*/) { return -1; }

uint32_t ReadUint32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void WriteUint32LE(uint8_t* p, uint32_t val) {
  p[0] = static_cast<uint8_t>(val & 0xFFu);
  p[1] = static_cast<uint8_t>((val >> 8) & 0xFFu);
  p[2] = static_cast<uint8_t>((val >> 16) & 0xFFu);
  p[3] = static_cast<uint8_t>((val >> 24) & 0xFFu);
}

TEST(H5SnoneTest, SelectNoneSupportsAllDataspaceClassesAndTransitions) {
  // Simple dataspace: ALL -> NONE -> POINTS -> NONE -> HYPERSLAB -> NONE ->
  // ALL.
  constexpr std::array<hsize_t, 2> kDims = {4, 5};
  ScopedSpace simple(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(simple.valid());

  ASSERT_GE(H5Sselect_none(simple), 0);
  EXPECT_EQ(H5Sget_select_type(simple), H5S_SEL_NONE);
  EXPECT_EQ(H5Sget_select_npoints(simple), 0);
  EXPECT_GT(H5Sselect_valid(simple), 0);

  // Selection validity ignores extent offset for "none" selections.
  constexpr std::array<hssize_t, 2> kLargeOffset = {1000, -1000};
  ASSERT_GE(H5Soffset_simple(simple, kLargeOffset.data()), 0);
  EXPECT_GT(H5Sselect_valid(simple), 0);
  ASSERT_GE(H5Soffset_simple(simple, nullptr), 0);

  // Replace NONE -> POINTS (invokes H5S__none_release) -> NONE.
  constexpr std::array<hsize_t, 4> kPtCoords = {0, 1, 2, 3};
  ASSERT_GE(H5Sselect_elements(simple, H5S_SELECT_SET, 2, kPtCoords.data()), 0);
  EXPECT_EQ(H5Sget_select_type(simple), H5S_SEL_POINTS);
  ASSERT_GE(H5Sselect_none(simple), 0);
  EXPECT_EQ(H5Sget_select_type(simple), H5S_SEL_NONE);

  // Replace NONE -> HYPERSLAB -> NONE -> ALL -> NONE -> NONE.
  constexpr std::array<hsize_t, 2> kStart = {1, 1};
  constexpr std::array<hsize_t, 2> kCount = {2, 2};
  ASSERT_GE(H5Sselect_hyperslab(simple, H5S_SELECT_SET, kStart.data(), nullptr,
                                kCount.data(), nullptr),
            0);
  EXPECT_EQ(H5Sget_select_type(simple), H5S_SEL_HYPERSLABS);
  ASSERT_GE(H5Sselect_none(simple), 0);
  ASSERT_GE(H5Sselect_all(simple), 0);
  EXPECT_EQ(H5Sget_select_type(simple), H5S_SEL_ALL);
  ASSERT_GE(H5Sselect_none(simple), 0);
  ASSERT_GE(H5Sselect_none(simple), 0);
  EXPECT_EQ(H5Sget_select_type(simple), H5S_SEL_NONE);

  // Scalar dataspace with NONE selection.
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ASSERT_GE(H5Sselect_none(scalar), 0);
  EXPECT_EQ(H5Sget_select_type(scalar), H5S_SEL_NONE);
  EXPECT_EQ(H5Sget_select_npoints(scalar), 0);
  EXPECT_GT(H5Sselect_valid(scalar), 0);

  // Null dataspace with NONE selection.
  ScopedSpace null_space(H5Screate(H5S_NULL));
  ASSERT_TRUE(null_space.valid());
  ASSERT_GE(H5Sselect_none(null_space), 0);
  EXPECT_EQ(H5Sget_select_type(null_space), H5S_SEL_NONE);
  EXPECT_EQ(H5Sget_select_npoints(null_space), 0);

  // Invalid or non-dataspace IDs fail cleanly.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Sselect_none(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Sselect_none(H5T_NATIVE_INT), 0);
}

TEST(H5SnoneTest, CopyAndSelectCopyPreserveNoneSelection) {
  constexpr std::array<hsize_t, 2> kDims = {3, 4};
  ScopedSpace src(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(src.valid());
  ASSERT_GE(H5Sselect_none(src), 0);

  ScopedSpace copied(H5Scopy(src));
  ASSERT_TRUE(copied.valid());
  EXPECT_EQ(H5Sget_select_type(copied), H5S_SEL_NONE);
  EXPECT_EQ(H5Sget_select_npoints(copied), 0);

  // Copy NONE selection onto a dataspace with an existing hyperslab selection.
  ScopedSpace dst(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(dst.valid());
  constexpr std::array<hsize_t, 2> kStart = {0, 0};
  constexpr std::array<hsize_t, 2> kCount = {2, 2};
  ASSERT_GE(H5Sselect_hyperslab(dst, H5S_SELECT_SET, kStart.data(), nullptr,
                                kCount.data(), nullptr),
            0);
  ASSERT_GE(H5Sselect_copy(dst, src), 0);
  EXPECT_EQ(H5Sget_select_type(dst), H5S_SEL_NONE);
  EXPECT_EQ(H5Sget_select_npoints(dst), 0);

  // Direct internal copy with both share_selection=false and true.
  H5S_t* src_ptr = UnwrapSpace(src);
  H5S_t* dst_ptr = UnwrapSpace(dst);
  ASSERT_NE(src_ptr, nullptr);
  ASSERT_NE(dst_ptr, nullptr);
  ASSERT_GE(H5S_select_copy(dst_ptr, src_ptr, false), 0);
  EXPECT_EQ(H5S_GET_SELECT_TYPE(dst_ptr), H5S_SEL_NONE);
  ASSERT_GE(H5S_select_copy(dst_ptr, src_ptr, true), 0);
  EXPECT_EQ(H5S_GET_SELECT_TYPE(dst_ptr), H5S_SEL_NONE);
}

TEST(H5SnoneTest, SerializeDeserializeRoundTripsAndErrorPaths) {
  constexpr std::array<hsize_t, 2> kDims = {4, 5};
  ScopedSpace src(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(src.valid());
  ASSERT_GE(H5Sselect_none(src), 0);

  // Public H5Sencode2 size query + encode for simple and scalar NONE spaces.
  // Note: H5Sdecode() passes SIZE_MAX to H5S_select_deserialize() (H5S.c:1619),
  // which triggers upstream UBSan pointer-overflow (*p + SIZE_MAX - 1 in
  // H5Sselect.c:528); verify the selection payload with bounded p_size instead.
  size_t encoded_size = 0;
  ASSERT_GE(H5Sencode2(src, nullptr, &encoded_size, H5P_DEFAULT), 0);
  ASSERT_GT(encoded_size, 16u);
  std::vector<uint8_t> encoded(encoded_size);
  ASSERT_GE(H5Sencode2(src, encoded.data(), &encoded_size, H5P_DEFAULT), 0);
  const uint32_t extent_size = ReadUint32LE(encoded.data() + 3);
  ASSERT_EQ(7u + extent_size + 16u, encoded.size());

  H5S_t* decoded_sel = nullptr;
  absl::Cleanup close_decoded_sel([&] {
    if (decoded_sel != nullptr) {
      H5S_close(decoded_sel);
    }
  });
  const uint8_t* enc_sel_ptr = encoded.data() + 7 + extent_size;
  ASSERT_GE(H5S_select_deserialize(&decoded_sel, &enc_sel_ptr, 16), 0);
  ASSERT_NE(decoded_sel, nullptr);
  EXPECT_EQ(H5S_GET_SELECT_TYPE(decoded_sel), H5S_SEL_NONE);
  EXPECT_EQ(H5S_get_select_npoints(decoded_sel), 0u);

  // Scalar space with NONE selection encodes the same 16-byte NONE selection.
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ASSERT_GE(H5Sselect_none(scalar), 0);
  size_t scalar_size = 0;
  ASSERT_GE(H5Sencode2(scalar, nullptr, &scalar_size, H5P_DEFAULT), 0);
  ASSERT_GT(scalar_size, 16u);
  std::vector<uint8_t> scalar_buf(scalar_size);
  ASSERT_GE(H5Sencode2(scalar, scalar_buf.data(), &scalar_size, H5P_DEFAULT),
            0);
  const uint32_t scalar_extent_size = ReadUint32LE(scalar_buf.data() + 3);
  ASSERT_EQ(7u + scalar_extent_size + 16u, scalar_buf.size());
  H5S_t* scalar_decoded = nullptr;
  absl::Cleanup close_scalar_decoded([&] {
    if (scalar_decoded != nullptr) {
      H5S_close(scalar_decoded);
    }
  });
  const uint8_t* scalar_sel_ptr = scalar_buf.data() + 7 + scalar_extent_size;
  ASSERT_GE(H5S_select_deserialize(&scalar_decoded, &scalar_sel_ptr, 16), 0);
  ASSERT_NE(scalar_decoded, nullptr);
  EXPECT_EQ(H5S_GET_SELECT_TYPE(scalar_decoded), H5S_SEL_NONE);

  // Internal serial_size and wire encoding layout.
  H5S_t* src_ptr = UnwrapSpace(src);
  ASSERT_NE(src_ptr, nullptr);
  EXPECT_EQ(H5S_select_serial_size(src_ptr), 16);

  std::array<uint8_t, 16> wire{};
  uint8_t* write_ptr = wire.data();
  ASSERT_GE(H5S_select_serialize(src_ptr, &write_ptr), 0);
  EXPECT_EQ(write_ptr, wire.data() + wire.size());
  EXPECT_EQ(ReadUint32LE(wire.data()), static_cast<uint32_t>(H5S_SEL_NONE));
  EXPECT_EQ(ReadUint32LE(wire.data() + 4),
            static_cast<uint32_t>(H5S_NONE_VERSION_1));
  EXPECT_EQ(ReadUint32LE(wire.data() + 8), 0u);
  EXPECT_EQ(ReadUint32LE(wire.data() + 12), 0u);

  // Internal deserialize with *space == nullptr allocates a new simple space.
  H5S_t* alloc_space = nullptr;
  absl::Cleanup close_alloc([&] {
    if (alloc_space != nullptr) {
      H5S_close(alloc_space);
    }
  });
  const uint8_t* read_ptr = wire.data();
  ASSERT_GE(H5S_select_deserialize(&alloc_space, &read_ptr, wire.size()), 0);
  ASSERT_NE(alloc_space, nullptr);
  EXPECT_EQ(read_ptr, wire.data() + wire.size());
  EXPECT_EQ(H5S_GET_SELECT_TYPE(alloc_space), H5S_SEL_NONE);

  // Internal deserialize into an already-allocated dataspace (*space != NULL).
  H5S_t* reuse_space = H5S_create_simple(2, kDims.data(), nullptr);
  ASSERT_NE(reuse_space, nullptr);
  absl::Cleanup close_reuse([&] { H5S_close(reuse_space); });
  read_ptr = wire.data();
  ASSERT_GE(H5S_select_deserialize(&reuse_space, &read_ptr, wire.size()), 0);
  EXPECT_EQ(H5S_GET_SELECT_TYPE(reuse_space), H5S_SEL_NONE);

  // Error branches in H5S__none_deserialize (bounded p_size -> skip=false).
  ScopedErrorSilence silence;

  // 1. Buffer overflow before decoding version (4B type + 2B < 4B version).
  //    Also covers cleanup of temporary tmp_space when *space == nullptr.
  {
    H5S_t* tmp_out = nullptr;
    const uint8_t* p = wire.data();
    EXPECT_LT(H5S_select_deserialize(&tmp_out, &p, 6), 0);
    EXPECT_EQ(tmp_out, nullptr);

    p = wire.data();
    EXPECT_LT(H5S_select_deserialize(&reuse_space, &p, 6), 0);
  }

  // 2. Invalid version (< H5S_NONE_VERSION_1 and > H5S_NONE_VERSION_LATEST).
  for (const uint32_t bad_ver : {0u, 2u}) {
    std::array<uint8_t, 16> bad_wire = wire;
    WriteUint32LE(bad_wire.data() + 4, bad_ver);
    H5S_t* tmp_out = nullptr;
    const uint8_t* p = bad_wire.data();
    EXPECT_LT(H5S_select_deserialize(&tmp_out, &p, bad_wire.size()), 0);
    EXPECT_EQ(tmp_out, nullptr);

    p = bad_wire.data();
    EXPECT_LT(H5S_select_deserialize(&reuse_space, &p, bad_wire.size()), 0);
  }

  // 3. Buffer overflow skipping remaining 8 header bytes (4B + 4B + 2B < 16B).
  {
    H5S_t* tmp_out = nullptr;
    const uint8_t* p = wire.data();
    EXPECT_LT(H5S_select_deserialize(&tmp_out, &p, 10), 0);
    EXPECT_EQ(tmp_out, nullptr);

    p = wire.data();
    EXPECT_LT(H5S_select_deserialize(&reuse_space, &p, 10), 0);
  }
}

TEST(H5SnoneTest, QueriesShapeSameIntersectAdjustAndProjection) {
  constexpr std::array<hsize_t, 2> kDims2 = {4, 6};
  constexpr std::array<hsize_t, 3> kDims3 = {2, 3, 4};

  ScopedSpace s2a(H5Screate_simple(2, kDims2.data(), nullptr));
  ScopedSpace s2b(H5Screate_simple(2, kDims2.data(), nullptr));
  ScopedSpace s3(H5Screate_simple(3, kDims3.data(), nullptr));
  ScopedSpace scalar_none(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(s2a.valid());
  ASSERT_TRUE(s2b.valid());
  ASSERT_TRUE(s3.valid());
  ASSERT_TRUE(scalar_none.valid());
  ASSERT_GE(H5Sselect_none(s2a), 0);
  ASSERT_GE(H5Sselect_none(s2b), 0);
  ASSERT_GE(H5Sselect_none(s3), 0);
  ASSERT_GE(H5Sselect_none(scalar_none), 0);

  // Bounds, linear offset, unlimited dim, and non-unlimited element queries.
  H5S_t* s2a_ptr = UnwrapSpace(s2a);
  ASSERT_NE(s2a_ptr, nullptr);
  {
    ScopedErrorSilence silence;
    std::array<hsize_t, 2> start{};
    std::array<hsize_t, 2> end{};
    EXPECT_LT(H5Sget_select_bounds(s2a, start.data(), end.data()), 0);

    hsize_t linear_off = 999;
    EXPECT_LT(H5S_get_select_offset(s2a_ptr, &linear_off), 0);

    hsize_t non_unlim = 0;
    EXPECT_LT(H5S_get_select_num_elem_non_unlim(s2a_ptr, &non_unlim), 0);

    // Public H5Sis_regular_hyperslab errors on NONE, whereas internal
    // H5S__none_is_regular returns true.
    EXPECT_LT(H5Sis_regular_hyperslab(s2a), 0);
  }
  EXPECT_EQ(H5S_get_select_unlim_dim(s2a_ptr), -1);
  EXPECT_EQ(H5S_select_is_contiguous(s2a_ptr), 0);
  EXPECT_EQ(H5S_select_is_single(s2a_ptr), 0);
  EXPECT_GT(H5S_select_is_regular(s2a_ptr), 0);

  // shape_same returns true between any two NONE selections (same/diff rank).
  EXPECT_GT(H5Sselect_shape_same(s2a, s2b), 0);
  EXPECT_GT(H5Sselect_shape_same(s2a, s3), 0);
  EXPECT_GT(H5Sselect_shape_same(scalar_none, s2a), 0);

  // intersect_block returns false.
  constexpr std::array<hsize_t, 2> kBlockStart = {0, 0};
  constexpr std::array<hsize_t, 2> kBlockEnd = {3, 5};
  EXPECT_EQ(
      H5Sselect_intersect_block(s2a, kBlockStart.data(), kBlockEnd.data()), 0);

  // Unsigned and signed adjustments succeed as no-ops.
  constexpr std::array<hsize_t, 2> kUOffset = {1, 2};
  constexpr std::array<hssize_t, 2> kSOffset = {-1, 2};
  EXPECT_GE(H5S_select_adjust_u(s2a_ptr, kUOffset.data()), 0);
  EXPECT_GE(H5S_select_adjust_s(s2a_ptr, kSOffset.data()), 0);

  // Project onto scalar returns FAIL; construct_projection onto simple spaces
  // of smaller/larger/scalar rank produces NONE selection.
  {
    ScopedErrorSilence silence;
    hsize_t scalar_off = 0;
    EXPECT_LT(H5S_select_project_scalar(s2a_ptr, &scalar_off), 0);
  }

  for (const unsigned target_rank : {0u, 1u, 3u}) {
    H5S_t* projected = nullptr;
    absl::Cleanup close_proj([&] {
      if (projected != nullptr) {
        H5S_close(projected);
      }
    });
    ptrdiff_t buf_adj = -1;
    ASSERT_GE(H5S_select_construct_projection(s2a_ptr, &projected, target_rank,
                                              sizeof(int32_t), &buf_adj),
              0);
    ASSERT_NE(projected, nullptr);
    EXPECT_EQ(H5S_GET_SELECT_TYPE(projected), H5S_SEL_NONE);
    EXPECT_EQ(buf_adj, 0);
  }

  // Project intersection when source intersect selection is NONE.
  ScopedSpace projected_inter(H5Sselect_project_intersection(s2a, s2b, s2a));
  ASSERT_TRUE(projected_inter.valid());
  EXPECT_EQ(H5Sget_select_type(projected_inter), H5S_SEL_NONE);
  EXPECT_EQ(H5Sget_select_npoints(projected_inter), 0);
}

TEST(H5SnoneTest, IteratorCallbacksAndSequenceListBehavior) {
  constexpr std::array<hsize_t, 2> kDims = {4, 6};
  ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());
  ASSERT_GE(H5Sselect_none(space), 0);

  // Public iterator lifecycle + reset.
  const hid_t pub_iter = H5Ssel_iter_create(space, sizeof(int32_t), 0);
  ASSERT_GE(pub_iter, 0);
  absl::Cleanup close_pub_iter([&] { H5Ssel_iter_close(pub_iter); });

  std::array<hsize_t, 4> pub_off{};
  std::array<size_t, 4> pub_len{};
  size_t pub_nseq = 99;
  size_t pub_nelmts = 99;
  ASSERT_GE(H5Ssel_iter_get_seq_list(pub_iter, 4, 4, &pub_nseq, &pub_nelmts,
                                     pub_off.data(), pub_len.data()),
            0);
  EXPECT_EQ(pub_nseq, 0u);
  EXPECT_EQ(pub_nelmts, 0u);
  ASSERT_GE(H5Ssel_iter_reset(pub_iter, space), 0);

  // Zero-sized dimension ALL selection vs NONE selection triggers
  // H5S__none_iter_block via H5Sselect_shape_same and fails cleanly.
  constexpr std::array<hsize_t, 2> kZeroDims = {0, 6};
  ScopedSpace zero_all(H5Screate_simple(2, kZeroDims.data(), nullptr));
  ASSERT_TRUE(zero_all.valid());
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Sselect_shape_same(space, zero_all), 0);
  }

  // Direct internal iterator checks covering every H5S_sel_iter_none callback.
  H5S_t* space_ptr = UnwrapSpace(space);
  ASSERT_NE(space_ptr, nullptr);
  H5S_sel_iter_t iter{};
  ASSERT_GE(H5S_select_iter_init(&iter, space_ptr, sizeof(int32_t), 0), 0);
  absl::Cleanup rel_iter([&] { H5S_select_iter_release(&iter); });

  EXPECT_EQ(H5S_select_iter_nelmts(&iter), 0u);

  std::array<hsize_t, 2> coords{};
  EXPECT_LT(H5S_select_iter_coords(&iter, coords.data()), 0);

  ASSERT_NE(iter.type, nullptr);
  std::array<hsize_t, 2> blk_start{};
  std::array<hsize_t, 2> blk_end{};
  EXPECT_LT(iter.type->iter_block(&iter, blk_start.data(), blk_end.data()), 0);
  EXPECT_LT(iter.type->iter_has_next_block(&iter), 0);
  EXPECT_LT(iter.type->iter_next_block(&iter), 0);

  // Advance callback succeeds; seed elmt_left=1 around H5S_select_iter_next so
  // `iter->elmt_left -= nelem` in H5Sselect.c does not underflow.
  EXPECT_GE(iter.type->iter_next(&iter, 1), 0);
  iter.elmt_left = 1;
  EXPECT_GE(H5S_select_iter_next(&iter, 1), 0);
  EXPECT_EQ(iter.elmt_left, 0u);

  std::array<hsize_t, 4> int_off{};
  std::array<size_t, 4> int_len{};
  size_t int_nseq = 99;
  size_t int_nelmts = 99;
  ASSERT_GE(H5S_select_iter_get_seq_list(&iter, 4, 4, &int_nseq, &int_nelmts,
                                         int_off.data(), int_len.data()),
            0);
  EXPECT_EQ(int_nseq, 0u);
  EXPECT_EQ(int_nelmts, 0u);
}

TEST(H5SnoneTest,
     ReleaseFailurePropagatesAcrossSelectNoneDeserializeAndProject) {
  constexpr std::array<hsize_t, 2> kDims = {4, 4};
  ScopedSpace src_none(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(src_none.valid());
  ASSERT_GE(H5Sselect_none(src_none), 0);
  H5S_t* src_ptr = UnwrapSpace(src_none);
  ASSERT_NE(src_ptr, nullptr);

  std::array<uint8_t, 16> wire{};
  uint8_t* wp = wire.data();
  ASSERT_GE(H5S_select_serialize(src_ptr, &wp), 0);

  H5S_select_class_t failing_class = H5S_sel_none[0];
  failing_class.release = FailingSelectionRelease;

  ScopedErrorSilence silence;

  // Internal raw dataspace with failing release callback.
  H5S_t* raw = H5S_create_simple(2, kDims.data(), nullptr);
  ASSERT_NE(raw, nullptr);
  const H5S_select_class_t* const orig_raw_type = raw->select.type;
  absl::Cleanup cleanup_raw([&] {
    raw->select.type = orig_raw_type;
    H5S_close(raw);
  });
  raw->select.type = &failing_class;

  EXPECT_LT(H5S_select_none(raw), 0);
  hsize_t proj_off = 0;
  EXPECT_LT(H5S_select_project_simple(src_ptr, raw, &proj_off), 0);
  const uint8_t* rp = wire.data();
  EXPECT_LT(H5S_select_deserialize(&raw, &rp, wire.size()), 0);

  // Public H5Sselect_none error propagation when underlying release fails.
  ScopedSpace pub(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(pub.valid());
  H5S_t* pub_ptr = UnwrapSpace(pub);
  ASSERT_NE(pub_ptr, nullptr);
  const H5S_select_class_t* const orig_pub_type = pub_ptr->select.type;
  absl::Cleanup restore_pub([&] { pub_ptr->select.type = orig_pub_type; });
  pub_ptr->select.type = &failing_class;

  EXPECT_LT(H5Sselect_none(pub), 0);
}

TEST(H5SnoneTest, DatasetIoAndIterateWithNoneSelectionLeaveBuffersUntouched) {
  ScopedFile file = CreateInMemoryFile("h5s_none_io.h5");
  ASSERT_TRUE(file.valid());

  constexpr std::array<hsize_t, 2> kDims = {3, 4};
  ScopedSpace fspace(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(fspace.valid());

  ScopedDataset dset(H5Dcreate2(file, "matrix", H5T_NATIVE_INT32, fspace,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  std::array<int32_t, 12> initial{};
  std::iota(initial.begin(), initial.end(), 1);
  ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     initial.data()),
            0);

  ScopedSpace none_fspace(H5Scopy(fspace));
  ScopedSpace none_mspace(H5Scopy(fspace));
  ASSERT_TRUE(none_fspace.valid());
  ASSERT_TRUE(none_mspace.valid());
  ASSERT_GE(H5Sselect_none(none_fspace), 0);
  ASSERT_GE(H5Sselect_none(none_mspace), 0);

  // Write with NONE selections must not overwrite stored elements.
  std::array<int32_t, 12> overwrite{};
  overwrite.fill(-777);
  ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, none_mspace, none_fspace,
                     H5P_DEFAULT, overwrite.data()),
            0);

  // Read with NONE selections must not modify caller buffer.
  std::array<int32_t, 12> read_buf{};
  read_buf.fill(999);
  ASSERT_GE(H5Dread(dset, H5T_NATIVE_INT32, none_mspace, none_fspace,
                    H5P_DEFAULT, read_buf.data()),
            0);
  for (const int32_t val : read_buf) {
    EXPECT_EQ(val, 999);
  }

  // Verify stored dataset contents are unchanged.
  std::array<int32_t, 12> verify_buf{};
  ASSERT_GE(H5Dread(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    verify_buf.data()),
            0);
  EXPECT_EQ(verify_buf, initial);

  // H5Diterate over NONE selection invokes callback 0 times.
  int visited = 0;
  auto count_cb = [](void*, hid_t, unsigned, const hsize_t*,
                     void* ctx) -> herr_t {
    ++(*static_cast<int*>(ctx));
    return 0;
  };
  ASSERT_GE(H5Diterate(read_buf.data(), H5T_NATIVE_INT32, none_mspace, count_cb,
                       &visited),
            0);
  EXPECT_EQ(visited, 0);
}

}  // namespace
}  // namespace hdf5
