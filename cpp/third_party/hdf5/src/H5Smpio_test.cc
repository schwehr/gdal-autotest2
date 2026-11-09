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
// Tests parallel dataspace MPI-datatype selection support constants, disabled
// parallel build sentinels, internal dataspace/hyperslab layout invariants, and
// serial Core VFD fallback behavior for selection patterns handled by
// third_party/hdf5/src/H5Smpio.c.
// (LF=0 in default google3 non-MPI Linux build: all implementations in
// H5Smpio.c are gated inside #ifdef H5_HAVE_PARALLEL.)
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Smpio.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <type_traits>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with `-DNDEBUG`, which omits `#ifndef NDEBUG` struct
// fields inside internal HDF5 headers. Match that macro around internal headers
// so struct layouts match the compiled library ABI.
#ifndef NDEBUG
#define H5SMPIO_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5S_FRIEND
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Spkg.h"
#include "third_party/hdf5/src/H5Sprivate.h"
}  // extern "C"
#ifdef H5SMPIO_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5SMPIO_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

template <typename T>
concept HasOpGen = requires(const T& t) { t.op_gen; };

template <typename T>
concept HasCopied = requires(const T& t) { t.u.copied; };

template <typename T>
concept HasNelmts = requires(const T& t) { t.u.nelmts; };

template <typename T>
concept HasNblocks = requires(const T& t) { t.u.nblocks; };

template <typename T>
concept HasDownType = requires(const T& t) { t.u.down_type; };

H5S_t* UnwrapSpace(hid_t space_id) {
  return static_cast<H5S_t*>(H5I_object_verify(space_id, H5I_DATASPACE));
}

TEST(H5SmpioTest, DefaultBuildDisablesParallelAndExposesSerialHyperOpInfo) {
#ifdef H5_HAVE_PARALLEL
  ADD_FAILURE() << "Expected H5_HAVE_PARALLEL to be disabled in default build.";
#endif

  EXPECT_EQ(H5FD_MPIO, H5I_INVALID_HID);

  // Selection enum discriminants used by H5S_mpio_space_type switch dispatch.
  EXPECT_EQ(H5S_SEL_ERROR, -1);
  EXPECT_EQ(H5S_SEL_NONE, 0);
  EXPECT_EQ(H5S_SEL_POINTS, 1);
  EXPECT_EQ(H5S_SEL_HYPERSLABS, 2);
  EXPECT_EQ(H5S_SEL_ALL, 3);
  EXPECT_GT(H5S_SEL_N, H5S_SEL_ALL);

  // H5S_hyper_op_info_t omits `u.down_type` (`MPI_Datatype`) when parallel is
  // disabled while retaining generation and serial scratch fields.
  static_assert(HasOpGen<H5S_hyper_op_info_t>);
  static_assert(HasCopied<H5S_hyper_op_info_t>);
  static_assert(HasNelmts<H5S_hyper_op_info_t>);
  static_assert(HasNblocks<H5S_hyper_op_info_t>);
  static_assert(!HasDownType<H5S_hyper_op_info_t>);
  static_assert(std::is_standard_layout_v<H5S_hyper_op_info_t>);

  H5S_hyper_op_info_t op_info{};
  op_info.op_gen = 42;
  op_info.u.nblocks = 7;
  EXPECT_EQ(op_info.op_gen, 42u);
  EXPECT_EQ(op_info.u.nblocks, 7u);
}

TEST(H5SmpioTest, SelectionPrimitivesForMpioDispatchBranchesBehaveCorrectly) {
  constexpr std::array<hsize_t, 2> kDims = {4, 6};

  // 1. ALL and NONE selections.
  ScopedSpace all_space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(all_space.valid());
  EXPECT_EQ(H5Sget_select_type(all_space), H5S_SEL_ALL);
  EXPECT_EQ(H5Sget_select_npoints(all_space), 24);

  ScopedSpace none_space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(none_space.valid());
  ASSERT_GE(H5Sselect_none(none_space), 0);
  EXPECT_EQ(H5Sget_select_type(none_space), H5S_SEL_NONE);
  EXPECT_EQ(H5Sget_select_npoints(none_space), 0);

  // 2. POINT selection with out-of-order coordinates and sorted sequence check.
  ScopedSpace point_space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(point_space.valid());
  constexpr std::array<hsize_t, 6> kCoords = {2, 4, 0, 2, 1, 0};
  ASSERT_GE(H5Sselect_elements(point_space, H5S_SELECT_SET, 3, kCoords.data()),
            0);
  EXPECT_EQ(H5Sget_select_type(point_space), H5S_SEL_POINTS);

  const hid_t unsorted_iter =
      H5Ssel_iter_create(point_space, sizeof(int32_t), 0);
  ASSERT_GE(unsorted_iter, 0);
  absl::Cleanup close_unsorted([&] { H5Ssel_iter_close(unsorted_iter); });

  const hid_t sorted_iter = H5Ssel_iter_create(
      point_space, sizeof(int32_t), H5S_SEL_ITER_GET_SEQ_LIST_SORTED);
  ASSERT_GE(sorted_iter, 0);
  absl::Cleanup close_sorted([&] { H5Ssel_iter_close(sorted_iter); });

  std::array<hsize_t, 4> unsorted_off{};
  std::array<size_t, 4> unsorted_len{};
  size_t unsorted_nseq = 0;
  size_t unsorted_nelmts = 0;
  ASSERT_GE(H5Ssel_iter_get_seq_list(unsorted_iter, 4, 4, &unsorted_nseq,
                                     &unsorted_nelmts, unsorted_off.data(),
                                     unsorted_len.data()),
            0);
  EXPECT_EQ(unsorted_nseq, 3u);
  EXPECT_EQ(unsorted_nelmts, 3u);
  EXPECT_EQ(unsorted_off[0], 16u * sizeof(int32_t));
  EXPECT_EQ(unsorted_off[1], 2u * sizeof(int32_t));
  EXPECT_EQ(unsorted_off[2], 6u * sizeof(int32_t));

  std::array<hsize_t, 4> sorted_off{};
  std::array<size_t, 4> sorted_len{};
  size_t sorted_nseq = 0;
  size_t sorted_nelmts = 0;
  // Sorted iterator splits batches at decreasing offsets (loc=8 < 64):
  // batch 1 yields {off=64}, batch 2 yields {off=8, off=24}.
  ASSERT_GE(
      H5Ssel_iter_get_seq_list(sorted_iter, 4, 4, &sorted_nseq, &sorted_nelmts,
                               sorted_off.data(), sorted_len.data()),
      0);
  EXPECT_EQ(sorted_nseq, 1u);
  EXPECT_EQ(sorted_nelmts, 1u);
  EXPECT_EQ(sorted_off[0], 16u * sizeof(int32_t));

  ASSERT_GE(
      H5Ssel_iter_get_seq_list(sorted_iter, 4, 4, &sorted_nseq, &sorted_nelmts,
                               sorted_off.data(), sorted_len.data()),
      0);
  EXPECT_EQ(sorted_nseq, 2u);
  EXPECT_EQ(sorted_nelmts, 2u);
  EXPECT_EQ(sorted_off[0], 2u * sizeof(int32_t));
  EXPECT_EQ(sorted_off[1], 6u * sizeof(int32_t));

  // 3. Regular vs irregular hyperslab classification and op_gen progression.
  ScopedSpace reg_space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(reg_space.valid());
  constexpr std::array<hsize_t, 2> kStart0 = {0, 0};
  constexpr std::array<hsize_t, 2> kStride0 = {2, 3};
  constexpr std::array<hsize_t, 2> kCount0 = {2, 2};
  constexpr std::array<hsize_t, 2> kBlock0 = {1, 2};
  ASSERT_GE(
      H5Sselect_hyperslab(reg_space, H5S_SELECT_SET, kStart0.data(),
                          kStride0.data(), kCount0.data(), kBlock0.data()),
      0);
  H5S_t* reg_ptr = UnwrapSpace(reg_space);
  ASSERT_NE(reg_ptr, nullptr);
  EXPECT_GT(H5S_SELECT_IS_REGULAR(reg_ptr), 0);

  // Union of disjoint hyperslab yields an irregular span-tree selection.
  constexpr std::array<hsize_t, 2> kStart1 = {1, 1};
  constexpr std::array<hsize_t, 2> kCount1 = {1, 1};
  constexpr std::array<hsize_t, 2> kBlock1 = {1, 3};
  ASSERT_GE(H5Sselect_hyperslab(reg_space, H5S_SELECT_OR, kStart1.data(),
                                nullptr, kCount1.data(), kBlock1.data()),
            0);
  EXPECT_EQ(H5S_SELECT_IS_REGULAR(reg_ptr), 0);
  EXPECT_NE(reg_ptr->select.sel_info.hslab->span_lst, nullptr);

  const uint64_t gen1 = H5S__hyper_get_op_gen();
  const uint64_t gen2 = H5S__hyper_get_op_gen();
  EXPECT_LT(gen1, gen2);
}

TEST(H5SmpioTest, SerialCoreVfdDatasetIoCoversSelectionPatterns) {
  ScopedFile file = CreateInMemoryFile("h5s_mpio_serial_fallback.h5");
  ASSERT_TRUE(file.valid());

  constexpr std::array<hsize_t, 2> kDims = {4, 6};
  ScopedSpace fspace(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(fspace.valid());

  ScopedDataset dset(H5Dcreate2(file, "data", H5T_NATIVE_INT32, fspace,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  std::array<int32_t, 24> init{};
  std::iota(init.begin(), init.end(), 100);
  ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     init.data()),
            0);

  // None selection write/read leaves dataset and caller buffer untouched.
  ScopedSpace none_space(H5Scopy(fspace));
  ASSERT_TRUE(none_space.valid());
  ASSERT_GE(H5Sselect_none(none_space), 0);
  std::array<int32_t, 4> sentinel = {-1, -2, -3, -4};
  constexpr hsize_t kFour = 4;
  ScopedSpace mem_none(H5Screate_simple(1, &kFour, nullptr));
  ASSERT_TRUE(mem_none.valid());
  ASSERT_GE(H5Sselect_none(mem_none), 0);
  ASSERT_GE(H5Dread(dset, H5T_NATIVE_INT32, mem_none, none_space, H5P_DEFAULT,
                    sentinel.data()),
            0);
  EXPECT_THAT(sentinel, ElementsAre(-1, -2, -3, -4));

  // Unsorted point selection read preserves requested point order.
  ScopedSpace pt_file(H5Scopy(fspace));
  ASSERT_TRUE(pt_file.valid());
  constexpr std::array<hsize_t, 8> kPts = {3, 5, 0, 0, 1, 2, 0, 5};
  ASSERT_GE(H5Sselect_elements(pt_file, H5S_SELECT_SET, 4, kPts.data()), 0);
  ScopedSpace pt_mem(H5Screate_simple(1, &kFour, nullptr));
  ASSERT_TRUE(pt_mem.valid());
  std::array<int32_t, 4> pt_out{};
  ASSERT_GE(H5Dread(dset, H5T_NATIVE_INT32, pt_mem, pt_file, H5P_DEFAULT,
                    pt_out.data()),
            0);
  EXPECT_THAT(pt_out, ElementsAre(123, 100, 108, 105));

  // Regular strided hyperslab read.
  ScopedSpace reg_file(H5Scopy(fspace));
  ASSERT_TRUE(reg_file.valid());
  constexpr std::array<hsize_t, 2> kStart = {1, 1};
  constexpr std::array<hsize_t, 2> kStride = {2, 2};
  constexpr std::array<hsize_t, 2> kCount = {2, 2};
  constexpr std::array<hsize_t, 2> kBlock = {1, 1};
  ASSERT_GE(H5Sselect_hyperslab(reg_file, H5S_SELECT_SET, kStart.data(),
                                kStride.data(), kCount.data(), kBlock.data()),
            0);
  std::array<int32_t, 4> hyper_out{};
  ASSERT_GE(H5Dread(dset, H5T_NATIVE_INT32, pt_mem, reg_file, H5P_DEFAULT,
                    hyper_out.data()),
            0);
  EXPECT_THAT(hyper_out, ElementsAre(107, 109, 119, 121));
}

}  // namespace
}  // namespace hdf5
