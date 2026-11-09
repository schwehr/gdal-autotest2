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
// Tests Object Header Simple Dataspace messages (encoding, decoding, copying,
// sizing, resetting, freeing, pre-copy version bounds, debug formatting, and
// SOHM sharing) in third_party/hdf5/src/H5Osdspace.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Osdspace.c

#include <stdio.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// clang-format off
// `:hdf5` is compiled with -DNDEBUG, omitting debug fields from cache/object
// header structs. Define NDEBUG around internal headers so struct offsets match.
#ifndef NDEBUG
#define H5OSDSPACE_TEST_RESTORE_UNDEF_NDEBUG 1
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
#define H5F_FRIEND
#define H5O_FRIEND
#define H5S_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Dprivate.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5Spkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5OSDSPACE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5OSDSPACE_TEST_RESTORE_UNDEF_NDEBUG
#endif
// clang-format on

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = H5AC__IGNORE_TAG) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      if (tag != H5AC__IGNORE_TAG) {
        H5AC_tag(tag, &prev_tag_);
        tagged_ = true;
      }
    }
  }

  ~ScopedApiContext() {
    if (tagged_) {
      H5AC_tag(prev_tag_, nullptr);
    }
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool tagged_ = false;
};

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

std::string CaptureDebugId(H5F_t* f, const H5S_extent_t* ext) {
  char* buf = nullptr;
  size_t len = 0;
  FILE* stream = open_memstream(&buf, &len);
  if (stream == nullptr) {
    return "";
  }
  absl::Cleanup free_buf = [&] { std::free(buf); };
  const herr_t rc = H5O_debug_id(H5O_SDSPACE_ID, f, ext, stream, 2, 20);
  std::fclose(stream);
  if (rc < 0) {
    return "";
  }
  return std::string(buf != nullptr ? buf : "", len);
}

TEST(H5OsdspaceTest, EncodeDecodeSizeCopyResetFreeV1AndV2AllTypes) {
  for (const size_t sizeof_size : {size_t{4}, size_t{8}}) {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_sizes(fcpl.get(), sizeof_size, sizeof_size), 0);

    ScopedFile file = CreateInMemoryFile("sdspace_roundtrip.h5", H5F_ACC_TRUNC,
                                           fcpl.get());
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    struct Case {
      unsigned version;
      H5S_class_t type;
      std::vector<hsize_t> dims;
      std::vector<hsize_t> max_dims;
      bool has_max;
      hsize_t expected_nelem;
    };

    const std::vector<Case> cases = {
        {H5O_SDSPACE_VERSION_1, H5S_SCALAR, {}, {}, false, 1},
        {H5O_SDSPACE_VERSION_1, H5S_SIMPLE, {3, 4}, {}, false, 12},
        {H5O_SDSPACE_VERSION_1,
         H5S_SIMPLE,
         {2, 5, 3},
         {4, H5S_UNLIMITED, 3},
         true,
         30},
        {H5O_SDSPACE_VERSION_2, H5S_SCALAR, {}, {}, false, 1},
        {H5O_SDSPACE_VERSION_2, H5S_NULL, {}, {}, false, 0},
        {H5O_SDSPACE_VERSION_2, H5S_SIMPLE, {7}, {}, false, 7},
        {H5O_SDSPACE_VERSION_2,
         H5S_SIMPLE,
         {1, 6},
         {10, H5S_UNLIMITED},
         true,
         6},
    };

    for (const Case& c : cases) {
      H5S_extent_t src{};
      src.version = c.version;
      src.type = c.type;
      src.rank = static_cast<unsigned>(c.dims.size());
      src.nelem = c.expected_nelem;
      std::vector<hsize_t> dims_buf = c.dims;
      std::vector<hsize_t> max_buf = c.max_dims;
      src.size = dims_buf.empty() ? nullptr : dims_buf.data();
      src.max = (!c.has_max || max_buf.empty()) ? nullptr : max_buf.data();

      const size_t base_hdr =
          4 + (c.version == H5O_SDSPACE_VERSION_1 ? 4 : 0);
      const size_t expected_sz =
          base_hdr + src.rank * sizeof_size +
          (src.max != nullptr ? src.rank * sizeof_size : 0);
      EXPECT_EQ(
          H5O_msg_raw_size(f, H5O_SDSPACE_ID, /*disable_shared=*/true, &src),
          expected_sz);

      std::vector<uint8_t> encoded(expected_sz, 0xA5u);
      ASSERT_GE(H5O_msg_encode(f, H5O_SDSPACE_ID, /*disable_shared=*/true,
                               encoded.data(), &src),
                0);

      auto* decoded = static_cast<H5S_extent_t*>(H5O_msg_decode(
          f, nullptr, H5O_SDSPACE_ID, encoded.size(), encoded.data()));
      ASSERT_NE(decoded, nullptr);
      absl::Cleanup free_decoded = [&] {
        H5O_msg_free(H5O_SDSPACE_ID, decoded);
      };

      EXPECT_EQ(decoded->version, c.version);
      EXPECT_EQ(decoded->type, c.type);
      EXPECT_EQ(decoded->rank, src.rank);
      EXPECT_EQ(decoded->nelem, c.expected_nelem);
      for (unsigned i = 0; i < src.rank; ++i) {
        ASSERT_NE(decoded->size, nullptr);
        EXPECT_EQ(decoded->size[i], c.dims[i]);
        if (c.has_max) {
          ASSERT_NE(decoded->max, nullptr);
          const hsize_t expected_max =
              (sizeof_size == 4 && c.max_dims[i] == H5S_UNLIMITED)
                  ? 0xffffffffULL
                  : c.max_dims[i];
          EXPECT_EQ(decoded->max[i], expected_max);
        } else {
          EXPECT_EQ(decoded->max, nullptr);
        }
      }

      // Copy into caller-supplied extent and reset via H5O_msg_reset.
      H5S_extent_t copy_inplace{};
      EXPECT_EQ(H5O_msg_copy(H5O_SDSPACE_ID, decoded, &copy_inplace),
                &copy_inplace);
      EXPECT_EQ(copy_inplace.rank, src.rank);
      if (src.rank > 0) {
        EXPECT_NE(copy_inplace.size, decoded->size);
      }
      EXPECT_GE(H5O_msg_reset(H5O_SDSPACE_ID, &copy_inplace), 0);
      EXPECT_EQ(copy_inplace.size, nullptr);
      EXPECT_EQ(copy_inplace.max, nullptr);

      // Copy with null destination allocates and frees cleanly.
      auto* copy_alloc = static_cast<H5S_extent_t*>(
          H5O_msg_copy(H5O_SDSPACE_ID, decoded, nullptr));
      ASSERT_NE(copy_alloc, nullptr);
      H5O_msg_free(H5O_SDSPACE_ID, copy_alloc);
    }
  }
}

TEST(H5OsdspaceTest, DecodeRejectsInvalidVersionsRanksFlagsAndTruncations) {
  ScopedFile file = CreateInMemoryFile("sdspace_decode_errors.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);
  const size_t size_sz = H5F_SIZEOF_SIZE(f);
  ASSERT_GT(size_sz, 1u);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  ScopedErrorSilence silence;

  // Empty buffer (p_size == 0) triggers version overflow.
  std::array<uint8_t, 2> dummy{0u, 0u};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_SDSPACE_ID, 0, dummy.data() + 1),
            nullptr);

  // Bad versions (< V1 or > V2).
  for (const uint8_t bad_ver : {uint8_t{0}, uint8_t{3}}) {
    const std::array<uint8_t, 8> bad = {bad_ver, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(
        H5O_msg_decode(f, nullptr, H5O_SDSPACE_ID, bad.size(), bad.data()),
        nullptr);
  }

  // Overflow before rank byte (p_size == 1).
  const std::array<uint8_t, 1> only_ver = {H5O_SDSPACE_VERSION_2};
  EXPECT_EQ(
      H5O_msg_decode(f, nullptr, H5O_SDSPACE_ID, 1, only_ver.data()), nullptr);

  // Rank > H5S_MAX_RANK (32).
  const std::array<uint8_t, 8> too_many_dims = {
      H5O_SDSPACE_VERSION_2, H5S_MAX_RANK + 1, 0, H5S_SIMPLE, 0, 0, 0, 0};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_SDSPACE_ID, too_many_dims.size(),
                           too_many_dims.data()),
            nullptr);

  // Overflow before flags byte (p_size == 2).
  const std::array<uint8_t, 2> no_flags = {H5O_SDSPACE_VERSION_2, 0};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_SDSPACE_ID, 2, no_flags.data()),
            nullptr);

  // V2: overflow before type byte (p_size == 3).
  const std::array<uint8_t, 3> v2_no_type = {H5O_SDSPACE_VERSION_2, 0, 0};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_SDSPACE_ID, 3, v2_no_type.data()),
            nullptr);

  // V2: invalid rank > 0 for H5S_SCALAR or H5S_NULL.
  for (const H5S_class_t cls : {H5S_SCALAR, H5S_NULL}) {
    std::vector<uint8_t> bad_rank(4 + size_sz, 0);
    bad_rank[0] = H5O_SDSPACE_VERSION_2;
    bad_rank[1] = 1;
    bad_rank[2] = 0;
    bad_rank[3] = static_cast<uint8_t>(cls);
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_SDSPACE_ID, bad_rank.size(),
                             bad_rank.data()),
              nullptr);
  }

  // V1: overflow in first reserved byte (p_size == 3) and 4-byte reserved block
  // (p_size == 4..7).
  const std::array<uint8_t, 8> v1_hdr = {H5O_SDSPACE_VERSION_1, 0, 0, 0, 0, 0,
                                         0, 0};
  for (const size_t len : {size_t{3}, size_t{4}, size_t{7}}) {
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_SDSPACE_ID, len, v1_hdr.data()),
              nullptr);
  }

  // Encode a valid V2 rank-2 extent with H5S_VALID_MAX, then test truncations
  // inside size[] and max[] plus size[i] > max[i] invariant violation.
  H5S_extent_t valid_ext{};
  valid_ext.version = H5O_SDSPACE_VERSION_2;
  valid_ext.type = H5S_SIMPLE;
  valid_ext.rank = 2;
  std::array<hsize_t, 2> cur_dims = {4, 5};
  std::array<hsize_t, 2> max_dims = {10, 10};
  valid_ext.size = cur_dims.data();
  valid_ext.max = max_dims.data();

  const size_t full_sz =
      H5O_msg_raw_size(f, H5O_SDSPACE_ID, /*disable_shared=*/true, &valid_ext);
  std::vector<uint8_t> encoded(full_sz, 0);
  ASSERT_GE(H5O_msg_encode(f, H5O_SDSPACE_ID, /*disable_shared=*/true,
                           encoded.data(), &valid_ext),
            0);

  // Truncated in size[] (header is 4 bytes, size[] needs 2*size_sz).
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_SDSPACE_ID, 4 + size_sz,
                           encoded.data()),
            nullptr);
  // Truncated in max[] (allocates size[] and fails before/during max[]).
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_SDSPACE_ID, 4 + 2 * size_sz,
                           encoded.data()),
            nullptr);
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_SDSPACE_ID, full_sz - 1,
                           encoded.data()),
            nullptr);

  // Violate size[1] > max[1]: triggers cleanup via H5S__extent_release + free.
  std::array<hsize_t, 2> invalid_max = {10, 4};
  valid_ext.max = invalid_max.data();
  std::vector<uint8_t> bad_max_buf(full_sz, 0);
  ASSERT_GE(H5O_msg_encode(f, H5O_SDSPACE_ID, /*disable_shared=*/true,
                           bad_max_buf.data(), &valid_ext),
            0);
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_SDSPACE_ID, bad_max_buf.size(),
                           bad_max_buf.data()),
            nullptr);
}

TEST(H5OsdspaceTest, PreCopyFileSupportsUdataNullAndVersionBoundsFailure) {
  ScopedPlist latest_fapl = MakeCoreFapl();
  ASSERT_TRUE(latest_fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(latest_fapl.get(), H5F_LIBVER_LATEST,
                                 H5F_LIBVER_LATEST),
            0);

  ScopedPlist earliest_fapl = MakeCoreFapl();
  ASSERT_TRUE(earliest_fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(earliest_fapl.get(), H5F_LIBVER_EARLIEST,
                                 H5F_LIBVER_V18),
            0);

  ScopedFile src_file(H5Fcreate("sdspace_copy_src.h5", H5F_ACC_TRUNC,
                                H5P_DEFAULT, latest_fapl.get()));
  ScopedFile dst_latest(H5Fcreate("sdspace_copy_dst_l.h5", H5F_ACC_TRUNC,
                                  H5P_DEFAULT, latest_fapl.get()));
  ScopedFile dst_earliest(H5Fcreate("sdspace_copy_dst_e.h5", H5F_ACC_TRUNC,
                                    H5P_DEFAULT, earliest_fapl.get()));
  ASSERT_TRUE(src_file.valid());
  ASSERT_TRUE(dst_latest.valid());
  ASSERT_TRUE(dst_earliest.valid());

  // Dataset with V2 H5S_NULL dataspace and an attribute with V2 simple space.
  ScopedSpace null_space(H5Screate(H5S_NULL));
  ASSERT_TRUE(null_space.valid());
  ScopedDataset null_dset(H5Dcreate2(src_file.get(), "/null_ds",
                                     H5T_NATIVE_INT, null_space.get(),
                                     H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(null_dset.valid());
  const std::array<hsize_t, 2> attr_dims = {2, 3};
  ScopedSpace attr_space(H5Screate_simple(2, attr_dims.data(), nullptr));
  ASSERT_TRUE(attr_space.valid());
  ScopedAttr attr(H5Acreate2(null_dset.get(), "a", H5T_NATIVE_INT,
                             attr_space.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());
  attr.reset();
  null_dset.reset();

  // Dataset copy into LATEST destination exercises both dataset (udata !=
  // NULL) and attribute (udata == NULL) pre_copy_file paths end-to-end.
  ASSERT_GE(H5Ocopy(src_file.get(), "/null_ds", dst_latest.get(),
                    "/null_ds_copy", H5P_DEFAULT, H5P_DEFAULT),
            0);
  ScopedDataset copied_ds(
      H5Dopen2(dst_latest.get(), "/null_ds_copy", H5P_DEFAULT));
  ASSERT_TRUE(copied_ds.valid());
  ScopedSpace copied_sp(H5Dget_space(copied_ds.get()));
  ASSERT_TRUE(copied_sp.valid());
  EXPECT_EQ(H5Sget_simple_extent_type(copied_sp.get()), H5S_NULL);

  // Also test direct pre_copy_file callback with udata == nullptr, non-null
  // udata, and out-of-bounds version check (temporarily setting destination
  // high_bound = H5F_LIBVER_EARLIEST and restoring via Cleanup).
  H5F_t* f_src = UnwrapFile(src_file.get());
  H5F_t* f_dst_l = UnwrapFile(dst_latest.get());
  H5F_t* f_dst_e = UnwrapFile(dst_earliest.get());
  ASSERT_NE(f_src, nullptr);
  ASSERT_NE(f_dst_l, nullptr);
  ASSERT_NE(f_dst_e, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5S_extent_t ext_v2{};
  ext_v2.version = H5O_SDSPACE_VERSION_2;
  ext_v2.type = H5S_SIMPLE;
  ext_v2.rank = 2;
  ext_v2.nelem = 6;
  std::array<hsize_t, 2> sz = {2, 3};
  std::array<hsize_t, 2> mx = {2, H5S_UNLIMITED};
  ext_v2.size = sz.data();
  ext_v2.max = mx.data();

  bool deleted = false;
  H5O_copy_t cpy_ok{};
  cpy_ok.file_dst = f_dst_l;
  ASSERT_NE(H5O_MSG_SDSPACE[0].pre_copy_file, nullptr);

  // udata == nullptr succeeds.
  EXPECT_GE(H5O_MSG_SDSPACE[0].pre_copy_file(f_src, &ext_v2, &deleted, &cpy_ok,
                                             nullptr),
            0);

  // udata != nullptr allocates & copies src_space_extent.
  H5D_copy_file_ud_t dset_udata{};
  EXPECT_GE(H5O_MSG_SDSPACE[0].pre_copy_file(f_src, &ext_v2, &deleted, &cpy_ok,
                                             &dset_udata),
            0);
  ASSERT_NE(dset_udata.src_space_extent, nullptr);
  EXPECT_EQ(dset_udata.src_space_extent->rank, 2u);
  H5O_msg_free(H5O_SDSPACE_ID, dset_udata.src_space_extent);

  // V2 > H5O_sdspace_ver_bounds[H5F_LIBVER_EARLIEST] fails cleanly via both
  // H5Ocopy and direct pre_copy_file.
  const H5F_libver_t saved_high = f_dst_e->shared->high_bound;
  f_dst_e->shared->high_bound = H5F_LIBVER_EARLIEST;
  absl::Cleanup restore_high = [&] {
    f_dst_e->shared->high_bound = saved_high;
  };
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Ocopy(src_file.get(), "/null_ds", dst_earliest.get(),
                      "/should_fail", H5P_DEFAULT, H5P_DEFAULT),
              0);
    H5O_copy_t cpy_bad{};
    cpy_bad.file_dst = f_dst_e;
    EXPECT_LT(H5O_MSG_SDSPACE[0].pre_copy_file(f_src, &ext_v2, &deleted,
                                               &cpy_bad, nullptr),
              0);
  }
}

TEST(H5OsdspaceTest, DebugFormattingAndSohmSharedDataspaces) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 1), 0);
  ASSERT_GE(
      H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_SDSPACE_FLAG, 1), 0);

  ScopedFile file =
      CreateInMemoryFile("sdspace_sohm.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    // Scalar debug (rank == 0).
    H5S_extent_t scalar{};
    scalar.version = H5O_SDSPACE_VERSION_2;
    scalar.type = H5S_SCALAR;
    const std::string out_scalar = CaptureDebugId(f, &scalar);
    EXPECT_THAT(out_scalar, HasSubstr("Rank:"));
    EXPECT_THAT(out_scalar, HasSubstr("0"));

    // Fixed dims debug (max == nullptr -> CONSTANT).
    std::array<hsize_t, 2> fixed_dims = {3, 8};
    H5S_extent_t fixed{};
    fixed.version = H5O_SDSPACE_VERSION_1;
    fixed.type = H5S_SIMPLE;
    fixed.rank = 2;
    fixed.size = fixed_dims.data();
    const std::string out_fixed = CaptureDebugId(f, &fixed);
    EXPECT_THAT(out_fixed, HasSubstr("Rank:"));
    EXPECT_THAT(out_fixed, HasSubstr("{3, 8}"));
    EXPECT_THAT(out_fixed, HasSubstr("CONSTANT"));

    // Mix of finite and H5S_UNLIMITED max dims -> includes UNLIM.
    std::array<hsize_t, 3> unlim_cur = {2, 4, 6};
    std::array<hsize_t, 3> unlim_max = {10, H5S_UNLIMITED, 6};
    H5S_extent_t unlim{};
    unlim.version = H5O_SDSPACE_VERSION_2;
    unlim.type = H5S_SIMPLE;
    unlim.rank = 3;
    unlim.size = unlim_cur.data();
    unlim.max = unlim_max.data();
    const std::string out_unlim = CaptureDebugId(f, &unlim);
    EXPECT_THAT(out_unlim, HasSubstr("{2, 4, 6}"));
    EXPECT_THAT(out_unlim, HasSubstr("{10, UNLIM, 6}"));
  }

  // End-to-end SOHM sharing for dataspaces across multiple datasets and reopen.
  const std::array<hsize_t, 3> dims = {2, 3, 4};
  ScopedSpace space(H5Screate_simple(3, dims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  ScopedDataset ds1(H5Dcreate2(file.get(), "/ds1", H5T_NATIVE_INT, space.get(),
                               H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedDataset ds2(H5Dcreate2(file.get(), "/ds2", H5T_NATIVE_INT, space.get(),
                               H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ds1.valid());
  ASSERT_TRUE(ds2.valid());
  ds1.reset();
  ds2.reset();

  H5F_info2_t finfo{};
  ASSERT_GE(H5Fget_info2(file.get(), &finfo), 0);
  EXPECT_GT(finfo.sohm.msgs_info.index_size, 0u);

  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  const ssize_t img_len = H5Fget_file_image(file.get(), nullptr, 0);
  ASSERT_GT(img_len, 0);
  std::vector<uint8_t> img(static_cast<size_t>(img_len));
  ASSERT_EQ(H5Fget_file_image(file.get(), img.data(), img.size()), img_len);

  ScopedPlist ro_fapl = MakeCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl.get(), img.data(), img.size()), 0);
  ScopedFile ro(H5Fopen("sdspace_sohm_ro.h5", H5F_ACC_RDONLY, ro_fapl.get()));
  ASSERT_TRUE(ro.valid());
  ScopedDataset ro_ds2(H5Dopen2(ro.get(), "/ds2", H5P_DEFAULT));
  ASSERT_TRUE(ro_ds2.valid());
  ScopedSpace ro_sp(H5Dget_space(ro_ds2.get()));
  ASSERT_TRUE(ro_sp.valid());
  std::array<hsize_t, 3> got_dims{};
  ASSERT_EQ(H5Sget_simple_extent_dims(ro_sp.get(), got_dims.data(), nullptr),
            3);
  EXPECT_EQ(got_dims, dims);

  // Delete shared dataset references to exercise shared_delete path.
  ASSERT_GE(H5Ldelete(file.get(), "/ds1", H5P_DEFAULT), 0);
  ASSERT_GE(H5Ldelete(file.get(), "/ds2", H5P_DEFAULT), 0);
}

}  // namespace
}  // namespace hdf5
