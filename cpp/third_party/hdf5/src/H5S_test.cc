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
// Tests dataspace lifecycle, extent management, encoding/decoding, dataset
// integration, and package termination in third_party/hdf5/src/H5S.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5S.c

#include "third_party/hdf5/src/hdf5.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define H5S_TEST_HAS_ASAN 1
#endif
#endif
#if !defined(H5S_TEST_HAS_ASAN) && \
    (defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__))
#define H5S_TEST_HAS_ASAN 1
#endif

// clang-format off
// `:hdf5` compiles with -DNDEBUG; match internal struct layouts before
// including package/private headers.
#ifndef NDEBUG
#define H5S_TEST_RESTORE_UNDEF_NDEBUG 1
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
#define H5S_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Gprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5Spkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5S_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5S_TEST_RESTORE_UNDEF_NDEBUG
#endif
// clang-format on

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

H5S_t* UnwrapSpace(hid_t space_id) {
  return static_cast<H5S_t*>(H5I_object_verify(space_id, H5I_DATASPACE));
}

TEST(H5STest, CreateCloseAndCopyValidAndInvalidArguments) {
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  EXPECT_EQ(H5Sget_simple_extent_type(scalar.get()), H5S_SCALAR);
  EXPECT_EQ(H5Sget_simple_extent_npoints(scalar.get()), 1);
  EXPECT_EQ(H5Sget_simple_extent_ndims(scalar.get()), 0);
  EXPECT_GT(H5Sis_simple(scalar.get()), 0);

  ScopedSpace null_space(H5Screate(H5S_NULL));
  ASSERT_TRUE(null_space.valid());
  EXPECT_EQ(H5Sget_simple_extent_type(null_space.get()), H5S_NULL);
  EXPECT_EQ(H5Sget_simple_extent_npoints(null_space.get()), 0);
  EXPECT_EQ(H5Sget_simple_extent_ndims(null_space.get()), 0);
  EXPECT_EQ(H5Sis_simple(null_space.get()), 0);

  // Uninitialized simple dataspace has rank 0, nelem 0, and H5S_has_extent ==
  // false.
  ScopedSpace uninit_simple(H5Screate(H5S_SIMPLE));
  ASSERT_TRUE(uninit_simple.valid());
  EXPECT_EQ(H5Sget_simple_extent_type(uninit_simple.get()), H5S_SIMPLE);
  EXPECT_EQ(H5Sget_simple_extent_npoints(uninit_simple.get()), 0);
  EXPECT_EQ(H5Sget_simple_extent_ndims(uninit_simple.get()), 0);
  EXPECT_GT(H5Sis_simple(uninit_simple.get()), 0);

  const H5S_t* uninit_ptr = UnwrapSpace(uninit_simple.get());
  ASSERT_NE(uninit_ptr, nullptr);
  EXPECT_FALSE(H5S_has_extent(uninit_ptr));

  // Copying uninitialized H5S_SIMPLE covers `src->size == NULL` in
  // H5S__extent_copy_real.
  ScopedSpace copy_uninit(H5Scopy(uninit_simple.get()));
  ASSERT_TRUE(copy_uninit.valid());
  EXPECT_GT(H5Sextent_equal(uninit_simple.get(), copy_uninit.get()), 0);

  // Simple dataspace with rank 0 becomes scalar.
  ScopedSpace rank0(H5Screate_simple(0, nullptr, nullptr));
  ASSERT_TRUE(rank0.valid());
  EXPECT_EQ(H5Sget_simple_extent_type(rank0.get()), H5S_SCALAR);

  // Simple dataspace with zero-length dim and unlimited maxdim.
  const hsize_t zero_dims[2] = {0, 4};
  const hsize_t unlim_max[2] = {H5S_UNLIMITED, 8};
  ScopedSpace zero_space(H5Screate_simple(2, zero_dims, unlim_max));
  ASSERT_TRUE(zero_space.valid());
  EXPECT_EQ(H5Sget_simple_extent_npoints(zero_space.get()), 0);
  EXPECT_EQ(H5Sget_simple_extent_ndims(zero_space.get()), 2);

  hsize_t out_dims[2] = {99, 99};
  hsize_t out_max[2] = {99, 99};
  EXPECT_EQ(
      H5Sget_simple_extent_dims(zero_space.get(), out_dims, out_max), 2);
  EXPECT_THAT(out_dims, ElementsAre(0, 4));
  EXPECT_THAT(out_max, ElementsAre(H5S_UNLIMITED, 8));
  EXPECT_EQ(H5Sget_simple_extent_dims(zero_space.get(), nullptr, nullptr), 2);

  // Copy with selection and maxdims.
  const hsize_t s_dims[2] = {3, 5};
  const hsize_t s_start[2] = {1, 1};
  const hsize_t s_count[2] = {2, 2};
  ScopedSpace src_sel(H5Screate_simple(2, s_dims, nullptr));
  ASSERT_TRUE(src_sel.valid());
  ASSERT_GE(H5Sselect_hyperslab(src_sel.get(), H5S_SELECT_SET, s_start, nullptr,
                                s_count, nullptr),
            0);
  ScopedSpace copied_sel(H5Scopy(src_sel.get()));
  ASSERT_TRUE(copied_sel.valid());
  EXPECT_EQ(H5Sget_select_npoints(copied_sel.get()), 4);
  EXPECT_GT(H5Sextent_equal(src_sel.get(), copied_sel.get()), 0);

  // Invalid inputs.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Screate(H5S_NO_CLASS), 0);
  EXPECT_LT(H5Screate(static_cast<H5S_class_t>(99)), 0);
  EXPECT_LT(H5Screate_simple(-1, s_dims, nullptr), 0);
  EXPECT_LT(H5Screate_simple(H5S_MAX_RANK + 1, s_dims, nullptr), 0);
  EXPECT_LT(H5Screate_simple(2, nullptr, nullptr), 0);
  const hsize_t bad_dims[2] = {H5S_UNLIMITED, 2};
  EXPECT_LT(H5Screate_simple(2, bad_dims, nullptr), 0);
  const hsize_t bad_max[2] = {3, 4};
  EXPECT_LT(H5Screate_simple(2, s_dims, bad_max), 0);
  EXPECT_LT(H5Sclose(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Scopy(H5I_INVALID_HID), 0);
  EXPECT_EQ(H5Sget_simple_extent_type(H5I_INVALID_HID), H5S_NO_CLASS);
  EXPECT_LT(H5Sget_simple_extent_npoints(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Sget_simple_extent_ndims(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Sget_simple_extent_dims(H5I_INVALID_HID, out_dims, out_max), 0);
  EXPECT_LT(H5Sis_simple(H5I_INVALID_HID), 0);
}

TEST(H5STest, SetExtentSimpleNoneAndExtentCopy) {
  ScopedSpace space(H5Screate(H5S_NULL));
  ASSERT_TRUE(space.valid());

  // Reset NULL -> SIMPLE with rank 0 (scalar) and then 2D.
  ASSERT_GE(H5Sset_extent_simple(space.get(), 0, nullptr, nullptr), 0);
  EXPECT_EQ(H5Sget_simple_extent_type(space.get()), H5S_SCALAR);

  const hsize_t dims[2] = {3, 4};
  const hsize_t maxdims[2] = {6, 8};
  ASSERT_GE(H5Sset_extent_simple(space.get(), 2, dims, maxdims), 0);
  EXPECT_EQ(H5Sget_simple_extent_type(space.get()), H5S_SIMPLE);
  EXPECT_EQ(H5Sget_select_npoints(space.get()), 12);

  // When selection is H5S_SEL_NONE, resizing preserves H5S_SEL_NONE.
  ASSERT_GE(H5Sselect_none(space.get()), 0);
  const hsize_t dims2[2] = {2, 5};
  ASSERT_GE(H5Sset_extent_simple(space.get(), 2, dims2, nullptr), 0);
  EXPECT_EQ(H5Sget_select_type(space.get()), H5S_SEL_NONE);
  EXPECT_EQ(H5Sget_simple_extent_npoints(space.get()), 10);

  // H5Sextent_copy updates destination selection when selection is ALL.
  ScopedSpace dst_all(H5Screate(H5S_NULL));
  ASSERT_TRUE(dst_all.valid());
  ASSERT_GE(H5Sextent_copy(dst_all.get(), space.get()), 0);
  EXPECT_EQ(H5Sget_simple_extent_type(dst_all.get()), H5S_SIMPLE);
  EXPECT_EQ(H5Sget_select_npoints(dst_all.get()), 10);

  // H5Sextent_copy leaves non-ALL selection type unchanged.
  ASSERT_GE(H5Sselect_none(dst_all.get()), 0);
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ASSERT_GE(H5Sextent_copy(dst_all.get(), scalar.get()), 0);
  EXPECT_EQ(H5Sget_simple_extent_type(dst_all.get()), H5S_SCALAR);
  EXPECT_EQ(H5Sget_select_type(dst_all.get()), H5S_SEL_NONE);

  ASSERT_GE(H5Sset_extent_none(dst_all.get()), 0);
  EXPECT_EQ(H5Sget_simple_extent_type(dst_all.get()), H5S_NULL);

  // Error cases for H5Sset_extent_simple, H5Sset_extent_none, H5Sextent_copy.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Sset_extent_simple(H5I_INVALID_HID, 2, dims, nullptr), 0);
  EXPECT_LT(H5Sset_extent_simple(space.get(), 2, nullptr, nullptr), 0);
  EXPECT_LT(H5Sset_extent_simple(space.get(), -1, dims, nullptr), 0);
  EXPECT_LT(H5Sset_extent_simple(space.get(), H5S_MAX_RANK + 1, dims, nullptr),
            0);
  const hsize_t unlim_dims[2] = {H5S_UNLIMITED, 4};
  EXPECT_LT(H5Sset_extent_simple(space.get(), 2, unlim_dims, nullptr), 0);
  EXPECT_LT(H5Sset_extent_simple(space.get(), 0, nullptr, maxdims), 0);
  const hsize_t small_max[2] = {1, 5};
  EXPECT_LT(H5Sset_extent_simple(space.get(), 2, dims, small_max), 0);
  EXPECT_LT(H5Sset_extent_none(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Sextent_copy(H5I_INVALID_HID, space.get()), 0);
  EXPECT_LT(H5Sextent_copy(space.get(), H5I_INVALID_HID), 0);
}

TEST(H5STest, ExtentEqualAndInternalHelpersCoverAllBranches) {
  const hsize_t d1[2] = {3, 4};
  const hsize_t d2[2] = {3, 5};
  const hsize_t d3[1] = {12};
  const hsize_t m1[2] = {6, 8};
  const hsize_t m2[2] = {6, 9};

  ScopedSpace s_null(H5Screate(H5S_NULL));
  ScopedSpace s_scalar(H5Screate(H5S_SCALAR));
  ScopedSpace s_a(H5Screate_simple(2, d1, m1));
  ScopedSpace s_a2(H5Screate_simple(2, d1, m1));
  ScopedSpace s_diff_size(H5Screate_simple(2, d2, nullptr));
  ScopedSpace s_diff_rank(H5Screate_simple(1, d3, nullptr));
  ScopedSpace s_diff_max(H5Screate_simple(2, d1, m2));
  ASSERT_TRUE(s_null.valid() && s_scalar.valid() && s_a.valid() &&
              s_a2.valid() && s_diff_size.valid() && s_diff_rank.valid() &&
              s_diff_max.valid());

  EXPECT_GT(H5Sextent_equal(s_null.get(), s_null.get()), 0);
  EXPECT_GT(H5Sextent_equal(s_scalar.get(), s_scalar.get()), 0);
  EXPECT_GT(H5Sextent_equal(s_a.get(), s_a2.get()), 0);
  EXPECT_EQ(H5Sextent_equal(s_null.get(), s_scalar.get()), 0);
  EXPECT_EQ(H5Sextent_equal(s_a.get(), s_scalar.get()), 0);
  EXPECT_EQ(H5Sextent_equal(s_a.get(), s_diff_rank.get()), 0);
  EXPECT_EQ(H5Sextent_equal(s_a.get(), s_diff_size.get()), 0);
  EXPECT_EQ(H5Sextent_equal(s_a.get(), s_diff_max.get()), 0);

  // Internal H5S_copy with copy_max = false creates an extent with max == NULL,
  // exercising H5S_get_npoints_max, H5S_extent_get_dims, and H5S_extent_equal.
  H5S_t* pa = UnwrapSpace(s_a.get());
  ASSERT_NE(pa, nullptr);
  H5S_t* no_max = H5S_copy(pa, /*share_selection=*/false, /*copy_max=*/false);
  ASSERT_NE(no_max, nullptr);
  absl::Cleanup close_no_max = [&] { H5S_close(no_max); };
  H5S_t* no_max2 = H5S_copy(pa, /*share_selection=*/false, /*copy_max=*/false);
  ASSERT_NE(no_max2, nullptr);
  absl::Cleanup close_no_max2 = [&] { H5S_close(no_max2); };

  EXPECT_EQ(H5S_extent_equal(pa, no_max), 0);
  EXPECT_EQ(H5S_extent_equal(no_max, pa), 0);
  EXPECT_GT(H5S_extent_equal(no_max, no_max2), 0);

  EXPECT_EQ(H5S_get_npoints_max(UnwrapSpace(s_null.get())), 0U);
  EXPECT_EQ(H5S_get_npoints_max(UnwrapSpace(s_scalar.get())), 1U);
  EXPECT_EQ(H5S_get_npoints_max(pa), 48U);
  EXPECT_EQ(H5S_get_npoints_max(no_max), 12U);

  const hsize_t unlim[2] = {H5S_UNLIMITED, 4};
  ScopedSpace s_unlim(H5Screate_simple(2, d1, unlim));
  ASSERT_TRUE(s_unlim.valid());
  EXPECT_EQ(H5S_get_npoints_max(UnwrapSpace(s_unlim.get())), HSIZET_MAX);

  // Direct internal getters/checks.
  EXPECT_NE(H5S_get_simple_extent(pa), nullptr);
  EXPECT_EQ(H5S_extent_nelem(&pa->extent), 12U);
  EXPECT_EQ(H5S_get_simple_extent_type(pa), H5S_SIMPLE);
  EXPECT_EQ(H5S_get_simple_extent_npoints(pa), 12);
  EXPECT_EQ(H5S_get_simple_extent_ndims(pa), 2);
  EXPECT_TRUE(H5S_has_extent(pa));
  EXPECT_TRUE(H5S_has_extent(UnwrapSpace(s_null.get())));
  EXPECT_TRUE(H5S_has_extent(UnwrapSpace(s_scalar.get())));

  hsize_t gd[2] = {};
  hsize_t gm[2] = {};
  EXPECT_EQ(H5S_extent_get_dims(&no_max->extent, gd, gm), 2);
  EXPECT_THAT(gd, ElementsAre(3, 4));
  EXPECT_THAT(gm, ElementsAre(3, 4));
  EXPECT_EQ(H5S_extent_get_dims(&UnwrapSpace(s_null.get())->extent, gd, gm), 0);

  // Internal H5S_set_extent paths: unchanged, changed (ALL vs NONE), overflow.
  EXPECT_EQ(H5S_set_extent(pa, d1), 0);
  const hsize_t shrunk[2] = {2, 3};
  EXPECT_GT(H5S_set_extent(pa, shrunk), 0);
  EXPECT_EQ(H5S_get_select_npoints(pa), 6U);

  ScopedSpace s_none(H5Screate_simple(2, d1, m1));
  ASSERT_TRUE(s_none.valid());
  ASSERT_GE(H5Sselect_none(s_none.get()), 0);
  EXPECT_GE(H5S_set_extent_real(UnwrapSpace(s_none.get()), d2), 0);
  EXPECT_EQ(H5Sget_select_type(s_none.get()), H5S_SEL_NONE);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Sextent_equal(H5I_INVALID_HID, s_a.get()), 0);
  EXPECT_LT(H5Sextent_equal(s_a.get(), H5I_INVALID_HID), 0);
  const hsize_t too_big[2] = {10, 4};
  EXPECT_LT(H5S_set_extent(pa, too_big), 0);
  EXPECT_LT(H5S_set_extent_simple(pa, H5S_MAX_RANK + 1, d1, nullptr), 0);

  // Exercise internal H5S_NO_CLASS paths in H5S_create, H5S__extent_copy_real,
  // H5S_get_npoints_max, H5S_get_simple_extent_ndims, H5S_extent_get_dims, and
  // H5S_get_simple_extent_dims (libhdf5 is built with -DNDEBUG).
  H5S_t* no_class = H5S_create(H5S_NO_CLASS);
  ASSERT_NE(no_class, nullptr);
  absl::Cleanup close_no_class = [&] { H5S_close(no_class); };
  H5S_t* no_class_copy =
      H5S_copy(no_class, /*share_selection=*/false, /*copy_max=*/false);
  ASSERT_NE(no_class_copy, nullptr);
  absl::Cleanup close_no_class_copy = [&] { H5S_close(no_class_copy); };
  EXPECT_EQ(H5S_get_npoints_max(no_class), 0U);
  EXPECT_LT(H5S_get_simple_extent_ndims(no_class), 0);
  EXPECT_LT(H5S_get_simple_extent_dims(no_class, nullptr, nullptr), 0);
}

TEST(H5STest, DatasetPersistenceAndSetVersionAcrossLibverBounds) {
  // Chunked extensible dataset exercises H5S_set_version, H5S_append, H5S_read,
  // H5S_set_extent, H5S_set_extent_real, and H5S_write.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);
  ScopedFile file(
      H5Fcreate("h5s_dset.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  const hsize_t dims[2] = {2, 3};
  const hsize_t maxdims[2] = {6, H5S_UNLIMITED};
  const hsize_t chunk[2] = {2, 3};
  ScopedSpace space(H5Screate_simple(2, dims, maxdims));
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(space.valid() && dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 2, chunk), 0);

  {
    ScopedDataset dset(H5Dcreate2(file.get(), "/ext", H5T_NATIVE_INT,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    const std::array<int, 6> data = {1, 2, 3, 4, 5, 6};
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, data.data()),
              0);

    const hsize_t new_dims[2] = {4, 5};
    ASSERT_GE(H5Dset_extent(dset.get(), new_dims), 0);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    ScopedErrorSilence silence;
    const hsize_t over_max[2] = {8, 5};
    EXPECT_LT(H5Dset_extent(dset.get(), over_max), 0);
  }

  ScopedDataset reopened(H5Dopen2(file.get(), "/ext", H5P_DEFAULT));
  ASSERT_TRUE(reopened.valid());
  ScopedSpace got_space(H5Dget_space(reopened.get()));
  ASSERT_TRUE(got_space.valid());
  hsize_t got_dims[2] = {};
  ASSERT_EQ(H5Sget_simple_extent_dims(got_space.get(), got_dims, nullptr), 2);
  EXPECT_THAT(got_dims, ElementsAre(4, 5));

  // H5Pset_libver_bounds requires high >= H5F_LIBVER_V18; verify H5S_NULL
  // dataset persistence succeeds at v18 and H5S_set_version rejects it when
  // high_bound is H5F_LIBVER_EARLIEST.
  ScopedPlist early_fapl = MakeCoreFapl();
  ASSERT_TRUE(early_fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(early_fapl.get(), H5F_LIBVER_EARLIEST,
                                 H5F_LIBVER_V18),
            0);
  ScopedFile early_file(
      H5Fcreate("h5s_early.h5", H5F_ACC_TRUNC, H5P_DEFAULT, early_fapl.get()));
  ASSERT_TRUE(early_file.valid());
  ScopedSpace null_space(H5Screate(H5S_NULL));
  ASSERT_TRUE(null_space.valid());

  ScopedDataset null_dset(H5Dcreate2(early_file.get(), "/null_ds",
                                     H5T_NATIVE_INT, null_space.get(),
                                     H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(null_dset.valid());

  H5F_t* early_f = UnwrapFile(early_file.get());
  ASSERT_NE(early_f, nullptr);
  {
    const H5F_libver_t orig_high = early_f->shared->high_bound;
    early_f->shared->high_bound = H5F_LIBVER_EARLIEST;
    const absl::Cleanup restore_high = [early_f, orig_high] {
      early_f->shared->high_bound = orig_high;
    };
    ScopedErrorSilence silence;
    EXPECT_LT(H5S_set_version(early_f, UnwrapSpace(null_space.get())), 0);
  }

  // Root group lacks a dataspace message: H5S_read and H5S_write fail cleanly
  // inside an active HDF5 API context.
  H5O_loc_t* root_oloc = H5G_oloc(early_f->shared->root_grp);
  ASSERT_NE(root_oloc, nullptr);
  {
    H5CX_node_t api_ctx = {};
    ASSERT_GE(H5CX_push(&api_ctx), 0);
    const absl::Cleanup pop_ctx = [] { H5CX_pop(/*update_dxpl_props=*/false); };
    ScopedErrorSilence silence;
    EXPECT_EQ(H5S_read(root_oloc), nullptr);
    H5O_t* root_oh = H5O_pin(root_oloc);
    ASSERT_NE(root_oh, nullptr);
    const absl::Cleanup unpin_root = [root_oh] { H5O_unpin(root_oh); };
    EXPECT_LT(
        H5S_write(early_f, root_oh, 0, UnwrapSpace(null_space.get())), 0);
  }
}

TEST(H5STest, Encode2AndDecodeRoundTripsAndCorruptionHandling) {
  const hsize_t dims[2] = {4, 5};
  const hsize_t start[2] = {1, 2};
  const hsize_t count[2] = {2, 2};

  ScopedSpace simple(H5Screate_simple(2, dims, nullptr));
  ASSERT_TRUE(simple.valid());
  ASSERT_GE(H5Sselect_hyperslab(simple.get(), H5S_SELECT_SET, start, nullptr,
                                count, nullptr),
            0);

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST), 0);

  size_t nalloc = 0;
  ASSERT_GE(H5Sencode2(simple.get(), nullptr, &nalloc, fapl.get()), 0);
  ASSERT_GT(nalloc, 10U);

  // Small buffer returns required size without failing or overrunning.
  std::vector<unsigned char> enc(nalloc, 0);
  size_t probe = 4;
  ASSERT_GE(H5Sencode2(simple.get(), enc.data(), &probe, H5P_DEFAULT), 0);
  EXPECT_EQ(probe, nalloc);

  size_t write_sz = nalloc;
  ASSERT_GE(H5Sencode2(simple.get(), enc.data(), &write_sz, fapl.get()), 0);

#if !defined(H5S_TEST_HAS_ASAN)
  // Upstream H5S_decode calls H5S_select_deserialize(..., SIZE_MAX), where
  // `*p + SIZE_MAX - 1` in H5Sselect.c triggers UBSan pointer-overflow under
  // `--config=asan-chlor`.
  ScopedSpace decoded(H5Sdecode(enc.data()));
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Sextent_equal(simple.get(), decoded.get()), 0);
  EXPECT_EQ(H5Sget_select_npoints(decoded.get()), 4);
#endif

  // Corruption and invalid argument paths.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Sencode2(H5I_INVALID_HID, nullptr, &nalloc, H5P_DEFAULT), 0);
  EXPECT_LT(H5Sdecode(nullptr), 0);

  // Negative selection offset makes H5S_SELECT_SERIAL_SIZE fail.
  ScopedSpace neg_pt(H5Screate_simple(2, dims, nullptr));
  ASSERT_TRUE(neg_pt.valid());
  const hsize_t coord[2] = {0, 0};
  const hssize_t neg_off[2] = {-1, 0};
  ASSERT_GE(H5Sselect_elements(neg_pt.get(), H5S_SELECT_SET, 1, coord), 0);
  ASSERT_GE(H5Soffset_simple(neg_pt.get(), neg_off), 0);
  EXPECT_LT(H5Sencode2(neg_pt.get(), nullptr, &nalloc, H5P_DEFAULT), 0);

  // Bad message ID at byte 0.
  std::vector<unsigned char> bad_type = enc;
  bad_type[0] = 0xFF;
  EXPECT_LT(H5Sdecode(bad_type.data()), 0);

  // Bad encode version at byte 1.
  std::vector<unsigned char> bad_ver = enc;
  bad_ver[1] = 99;
  EXPECT_LT(H5Sdecode(bad_ver.data()), 0);

  // Extent size 0 fails H5O_msg_decode.
  std::vector<unsigned char> bad_ext_sz = enc;
  bad_ext_sz[3] = 0;
  bad_ext_sz[4] = 0;
  bad_ext_sz[5] = 0;
  bad_ext_sz[6] = 0;
  EXPECT_LT(H5Sdecode(bad_ext_sz.data()), 0);
}

TEST(H5STest, PackageInitAndTopTermClearsOpenIdsCleanly) {
  ASSERT_GE(H5open(), 0);
  absl::Cleanup reopen = [] { H5open(); };

  // Call H5S_init cleanly while initialized.
  EXPECT_GE(H5S_init(), 0);

  // Leave one dataspace ID and one selection iterator ID open across H5close()
  // so H5S_top_term_package() clears both ID classes and H5S_term_package()
  // shuts down cleanly; H5open() re-initializes the package afterward.
  const hsize_t dims[1] = {4};
  const hid_t open_space = H5Screate_simple(1, dims, nullptr);
  ASSERT_GE(open_space, 0);
  const hid_t open_iter = H5Ssel_iter_create(open_space, sizeof(int), 0);
  ASSERT_GE(open_iter, 0);

  ASSERT_GE(H5close(), 0);
  ASSERT_GE(H5open(), 0);

  // Verify the dataspace interface works normally after reopen.
  ScopedSpace post_reopen(H5Screate_simple(1, dims, nullptr));
  ASSERT_TRUE(post_reopen.valid());
  EXPECT_EQ(H5Sget_simple_extent_npoints(post_reopen.get()), 4);
}

}  // namespace
}  // namespace hdf5
