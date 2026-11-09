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
// Tests dataset transfer property lists (DXPL): buffer sizing, preserve mode,
// EDC checks, filter and type-conversion callbacks, B-tree split ratios, VL
// memory management, hyperslab vector sizes, data transforms, dataset I/O
// hyperslab selections, selection I/O modes, write-buffer modification, and
// property list encode/decode callbacks (third_party/hdf5/src/H5Pdxpl.c).
//
// Google3 note: H5_HAVE_PARALLEL is disabled, so H5Pget_mpio_actual_* and
// H5Pget_mpio_no_collective_cause are compiled out.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Pdxpl.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#include "third_party/hdf5/src/H5Dprivate.h"
#include "third_party/hdf5/src/H5Zprivate.h"
}  // extern "C"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

struct ConvExceptState {
  int count = 0;
};

H5T_conv_ret_t ClampOverflowCb(H5T_conv_except_t except_type,
                               hid_t /*src_id*/, hid_t /*dst_id*/,
                               void* /*src_buf*/, void* dst_buf,
                               void* user_data) {
  auto* state = static_cast<ConvExceptState*>(user_data);
  if (state != nullptr) {
    ++state->count;
  }
  if (except_type == H5T_CONV_EXCEPT_RANGE_HI && dst_buf != nullptr) {
    *static_cast<int8_t*>(dst_buf) = 127;
    return H5T_CONV_HANDLED;
  }
  return H5T_CONV_UNHANDLED;
}

H5Z_cb_return_t DummyFilterCb(H5Z_filter_t /*filter*/, void* /*buf*/,
                              size_t /*buf_size*/, void* op_data) {
  auto* count = static_cast<int*>(op_data);
  if (count != nullptr) {
    ++(*count);
  }
  return H5Z_CB_CONT;
}

struct VlenMemStats {
  std::atomic<int> allocs{0};
  std::atomic<int> frees{0};
};

void* CountingVlenAlloc(size_t size, void* alloc_info) {
  if (alloc_info != nullptr) {
    ++static_cast<VlenMemStats*>(alloc_info)->allocs;
  }
  return std::malloc(size);
}

void CountingVlenFree(void* mem, void* free_info) {
  if (free_info != nullptr) {
    ++static_cast<VlenMemStats*>(free_info)->frees;
  }
  std::free(mem);
}

TEST(H5PdxplTest, BufferGetSetDefaultsErrorsAndStripMining) {
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());

  void* tconv = reinterpret_cast<void*>(0x1);
  void* bkg = reinterpret_cast<void*>(0x1);
  EXPECT_EQ(H5Pget_buffer(H5P_DATASET_XFER_DEFAULT, &tconv, &bkg),
            1024u * 1024u);
  EXPECT_EQ(tconv, nullptr);
  EXPECT_EQ(bkg, nullptr);
  EXPECT_EQ(H5Pget_buffer(dxpl.get(), nullptr, nullptr), 1024u * 1024u);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pset_buffer(dxpl.get(), 0, nullptr, nullptr), 0);
    EXPECT_LT(H5Pset_buffer(H5P_DEFAULT, 64, nullptr, nullptr), 0);
    EXPECT_LT(H5Pset_buffer(H5P_DATASET_XFER_DEFAULT, 64, nullptr, nullptr), 0);
    EXPECT_LT(H5Pset_buffer(fcpl.get(), 64, nullptr, nullptr), 0);
    EXPECT_LT(H5Pset_buffer(H5I_INVALID_HID, 64, nullptr, nullptr), 0);
    EXPECT_EQ(H5Pget_buffer(H5I_INVALID_HID, &tconv, &bkg), 0u);
    EXPECT_EQ(H5Pget_buffer(H5P_DEFAULT, &tconv, &bkg), 0u);
    EXPECT_EQ(H5Pget_buffer(fcpl.get(), &tconv, &bkg), 0u);
  }

  std::array<uint8_t, 64> tconv_buf{};
  std::array<uint8_t, 64> bkg_buf{};
  ASSERT_GE(H5Pset_buffer(dxpl.get(), tconv_buf.size(), tconv_buf.data(),
                          bkg_buf.data()),
            0);
  EXPECT_EQ(H5Pget_buffer(dxpl.get(), &tconv, &bkg), tconv_buf.size());
  EXPECT_EQ(tconv, tconv_buf.data());
  EXPECT_EQ(bkg, bkg_buf.data());

  // Strip-mined type conversion I/O (128 bytes total with a 16-byte buffer).
  ScopedFile file = CreateInMemoryFile("dxpl_strip_mine.h5");
  ASSERT_TRUE(file.valid());
  const hsize_t dims[1] = {16};
  ScopedSpace space(H5Screate_simple(1, dims, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "floats", H5T_IEEE_F32LE,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  std::array<double, 16> input{};
  for (size_t i = 0; i < input.size(); ++i) {
    input[i] = static_cast<double>(i) * 1.5;
  }
  ASSERT_GE(H5Pset_buffer(dxpl.get(), 16, tconv_buf.data(), bkg_buf.data()), 0);
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL,
                     dxpl.get(), input.data()),
            0);

  std::array<double, 16> output{};
  ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, dxpl.get(),
                    output.data()),
            0);
  EXPECT_EQ(output, input);
}

TEST(H5PdxplTest, PreserveCompoundMembersAndErrors) {
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());

  EXPECT_EQ(H5Pget_preserve(H5P_DATASET_XFER_DEFAULT), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pset_preserve(H5P_DEFAULT, true), 0);
    EXPECT_LT(H5Pset_preserve(H5P_DATASET_XFER_DEFAULT, true), 0);
    EXPECT_LT(H5Pset_preserve(fcpl.get(), true), 0);
    EXPECT_LT(H5Pset_preserve(H5I_INVALID_HID, true), 0);
    EXPECT_LT(H5Pget_preserve(H5P_DEFAULT), 0);
    EXPECT_LT(H5Pget_preserve(fcpl.get()), 0);
    EXPECT_LT(H5Pget_preserve(H5I_INVALID_HID), 0);
  }

  ASSERT_GE(H5Pset_preserve(dxpl.get(), true), 0);
  EXPECT_EQ(H5Pget_preserve(dxpl.get()), 1);
  ASSERT_GE(H5Pset_preserve(dxpl.get(), false), 0);
  EXPECT_EQ(H5Pget_preserve(dxpl.get()), 0);

  // Verify H5Pset_preserve preserves untouched compound fields during read.
  struct Pair {
    int32_t a;
    int32_t b;
  };
  ScopedType full_type(H5Tcreate(H5T_COMPOUND, sizeof(Pair)));
  ASSERT_TRUE(full_type.valid());
  ASSERT_GE(H5Tinsert(full_type.get(), "a", offsetof(Pair, a), H5T_NATIVE_INT32),
            0);
  ASSERT_GE(H5Tinsert(full_type.get(), "b", offsetof(Pair, b), H5T_NATIVE_INT32),
            0);

  ScopedType partial_type(H5Tcreate(H5T_COMPOUND, sizeof(Pair)));
  ASSERT_TRUE(partial_type.valid());
  ASSERT_GE(
      H5Tinsert(partial_type.get(), "a", offsetof(Pair, a), H5T_NATIVE_INT32),
      0);

  ScopedFile file = CreateInMemoryFile("dxpl_preserve.h5");
  ASSERT_TRUE(file.valid());
  const hsize_t dim = 2;
  ScopedSpace space(H5Screate_simple(1, &dim, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "pairs", full_type.get(),
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  Pair written[2] = {{10, 20}, {30, 40}};
  ASSERT_GE(H5Dwrite(dset.get(), full_type.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     written),
            0);

  Pair read_buf[2] = {{-1, 777}, {-1, 888}};
  ASSERT_GE(H5Pset_preserve(dxpl.get(), true), 0);
  ASSERT_GE(H5Dread(dset.get(), partial_type.get(), H5S_ALL, H5S_ALL,
                    dxpl.get(), read_buf),
            0);
  EXPECT_EQ(read_buf[0].a, 10);
  EXPECT_EQ(read_buf[0].b, 777);
  EXPECT_EQ(read_buf[1].a, 30);
  EXPECT_EQ(read_buf[1].b, 888);
}

TEST(H5PdxplTest, EdcCheckAndFletcher32Read) {
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());

  EXPECT_EQ(H5Pget_edc_check(H5P_DATASET_XFER_DEFAULT), H5Z_ENABLE_EDC);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pset_edc_check(dxpl.get(), H5Z_ERROR_EDC), 0);
    EXPECT_LT(H5Pset_edc_check(dxpl.get(), H5Z_NO_EDC), 0);
    EXPECT_LT(H5Pset_edc_check(H5P_DEFAULT, H5Z_DISABLE_EDC), 0);
    EXPECT_LT(H5Pset_edc_check(H5P_DATASET_XFER_DEFAULT, H5Z_DISABLE_EDC), 0);
    EXPECT_LT(H5Pset_edc_check(fcpl.get(), H5Z_DISABLE_EDC), 0);
    EXPECT_LT(H5Pset_edc_check(H5I_INVALID_HID, H5Z_DISABLE_EDC), 0);
    EXPECT_EQ(H5Pget_edc_check(H5P_DEFAULT), H5Z_ERROR_EDC);
    EXPECT_EQ(H5Pget_edc_check(fcpl.get()), H5Z_ERROR_EDC);
    EXPECT_EQ(H5Pget_edc_check(H5I_INVALID_HID), H5Z_ERROR_EDC);
  }

  ASSERT_GE(H5Pset_edc_check(dxpl.get(), H5Z_DISABLE_EDC), 0);
  EXPECT_EQ(H5Pget_edc_check(dxpl.get()), H5Z_DISABLE_EDC);
  ASSERT_GE(H5Pset_edc_check(dxpl.get(), H5Z_ENABLE_EDC), 0);
  EXPECT_EQ(H5Pget_edc_check(dxpl.get()), H5Z_ENABLE_EDC);

  ScopedFile file = CreateInMemoryFile("dxpl_edc.h5");
  ASSERT_TRUE(file.valid());
  const hsize_t dim = 8;
  const hsize_t chunk_dim = 4;
  ScopedSpace space(H5Screate_simple(1, &dim, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, &chunk_dim), 0);
  ASSERT_GE(H5Pset_fletcher32(dcpl.get()), 0);

  ScopedDataset dset(H5Dcreate2(file.get(), "checked", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, dcpl.get(),
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  const std::array<int32_t, 8> data{1, 2, 3, 4, 5, 6, 7, 8};
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, data.data()),
            0);

  std::array<int32_t, 8> out{};
  ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl.get(),
                    out.data()),
            0);
  EXPECT_EQ(out, data);

  ASSERT_GE(H5Pset_edc_check(dxpl.get(), H5Z_DISABLE_EDC), 0);
  out.fill(0);
  ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl.get(),
                    out.data()),
            0);
  EXPECT_EQ(out, data);
}

TEST(H5PdxplTest, FilterAndTypeConversionCallbacks) {
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());

  H5T_conv_except_func_t conv_op = nullptr;
  void* conv_data = reinterpret_cast<void*>(0x1);
  ASSERT_GE(H5Pget_type_conv_cb(H5P_DATASET_XFER_DEFAULT, &conv_op, &conv_data),
            0);
  EXPECT_EQ(conv_op, nullptr);
  EXPECT_EQ(conv_data, nullptr);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pset_filter_callback(H5P_DEFAULT, DummyFilterCb, nullptr), 0);
    EXPECT_LT(H5Pset_filter_callback(H5P_DATASET_XFER_DEFAULT, DummyFilterCb,
                                     nullptr),
              0);
    EXPECT_LT(H5Pset_filter_callback(fcpl.get(), DummyFilterCb, nullptr), 0);
    EXPECT_LT(H5Pset_filter_callback(H5I_INVALID_HID, DummyFilterCb, nullptr),
              0);

    EXPECT_LT(H5Pset_type_conv_cb(H5P_DEFAULT, ClampOverflowCb, nullptr), 0);
    EXPECT_LT(H5Pset_type_conv_cb(H5P_DATASET_XFER_DEFAULT, ClampOverflowCb,
                                  nullptr),
              0);
    EXPECT_LT(H5Pset_type_conv_cb(fcpl.get(), ClampOverflowCb, nullptr), 0);
    EXPECT_LT(H5Pset_type_conv_cb(H5I_INVALID_HID, ClampOverflowCb, nullptr),
              0);
    EXPECT_LT(H5Pget_type_conv_cb(H5P_DEFAULT, &conv_op, &conv_data), 0);
    EXPECT_LT(H5Pget_type_conv_cb(fcpl.get(), &conv_op, &conv_data), 0);
    EXPECT_LT(H5Pget_type_conv_cb(H5I_INVALID_HID, &conv_op, &conv_data), 0);
  }

  int filter_count = 0;
  ASSERT_GE(
      H5Pset_filter_callback(dxpl.get(), DummyFilterCb, &filter_count), 0);
  ASSERT_GE(H5Pset_filter_callback(dxpl.get(), nullptr, nullptr), 0);

  ConvExceptState conv_state;
  ASSERT_GE(H5Pset_type_conv_cb(dxpl.get(), ClampOverflowCb, &conv_state), 0);
  ASSERT_GE(H5Pget_type_conv_cb(dxpl.get(), &conv_op, &conv_data), 0);
  EXPECT_EQ(conv_op, ClampOverflowCb);
  EXPECT_EQ(conv_data, &conv_state);

  // Trigger conversion exception callback on int32 -> int8 overflow write.
  ScopedFile file = CreateInMemoryFile("dxpl_conv_cb.h5");
  ASSERT_TRUE(file.valid());
  const hsize_t dim = 2;
  ScopedSpace space(H5Screate_simple(1, &dim, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "i8", H5T_STD_I8LE, space.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  const int32_t src[2] = {5, 1000};
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl.get(),
                     src),
            0);
  EXPECT_GE(conv_state.count, 1);

  int8_t dst[2] = {0, 0};
  ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT8, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    dst),
            0);
  EXPECT_EQ(dst[0], 5);
  EXPECT_EQ(dst[1], 127);
}

TEST(H5PdxplTest, BtreeSplitRatiosAndHyperVectorSize) {
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());

  double left = -1.0;
  double middle = -1.0;
  double right = -1.0;
  ASSERT_GE(H5Pget_btree_ratios(H5P_DATASET_XFER_DEFAULT, &left, &middle,
                                &right),
            0);
  EXPECT_DOUBLE_EQ(left, 0.1);
  EXPECT_DOUBLE_EQ(middle, 0.5);
  EXPECT_DOUBLE_EQ(right, 0.9);
  EXPECT_GE(H5Pget_btree_ratios(dxpl.get(), nullptr, nullptr, nullptr), 0);

  size_t vec_size = 0;
  ASSERT_GE(H5Pget_hyper_vector_size(H5P_DATASET_XFER_DEFAULT, &vec_size), 0);
  EXPECT_EQ(vec_size, 1024u);
  EXPECT_GE(H5Pget_hyper_vector_size(dxpl.get(), nullptr), 0);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pset_btree_ratios(dxpl.get(), -0.01, 0.5, 0.5), 0);
    EXPECT_LT(H5Pset_btree_ratios(dxpl.get(), 1.01, 0.5, 0.5), 0);
    EXPECT_LT(H5Pset_btree_ratios(dxpl.get(), 0.5, -0.01, 0.5), 0);
    EXPECT_LT(H5Pset_btree_ratios(dxpl.get(), 0.5, 1.01, 0.5), 0);
    EXPECT_LT(H5Pset_btree_ratios(dxpl.get(), 0.5, 0.5, -0.01), 0);
    EXPECT_LT(H5Pset_btree_ratios(dxpl.get(), 0.5, 0.5, 1.01), 0);
    EXPECT_LT(H5Pset_btree_ratios(H5P_DEFAULT, 0.2, 0.5, 0.8), 0);
    EXPECT_LT(H5Pset_btree_ratios(H5P_DATASET_XFER_DEFAULT, 0.2, 0.5, 0.8), 0);
    EXPECT_LT(H5Pset_btree_ratios(fcpl.get(), 0.2, 0.5, 0.8), 0);
    EXPECT_LT(H5Pset_btree_ratios(H5I_INVALID_HID, 0.2, 0.5, 0.8), 0);
    EXPECT_LT(H5Pget_btree_ratios(H5P_DEFAULT, &left, &middle, &right), 0);
    EXPECT_LT(H5Pget_btree_ratios(fcpl.get(), &left, &middle, &right), 0);
    EXPECT_LT(H5Pget_btree_ratios(H5I_INVALID_HID, &left, &middle, &right), 0);

    EXPECT_LT(H5Pset_hyper_vector_size(dxpl.get(), 0), 0);
    EXPECT_LT(H5Pset_hyper_vector_size(H5P_DEFAULT, 16), 0);
    EXPECT_LT(H5Pset_hyper_vector_size(H5P_DATASET_XFER_DEFAULT, 16), 0);
    EXPECT_LT(H5Pset_hyper_vector_size(fcpl.get(), 16), 0);
    EXPECT_LT(H5Pset_hyper_vector_size(H5I_INVALID_HID, 16), 0);
    EXPECT_LT(H5Pget_hyper_vector_size(H5P_DEFAULT, &vec_size), 0);
    EXPECT_LT(H5Pget_hyper_vector_size(fcpl.get(), &vec_size), 0);
    EXPECT_LT(H5Pget_hyper_vector_size(H5I_INVALID_HID, &vec_size), 0);
  }

  ASSERT_GE(H5Pset_btree_ratios(dxpl.get(), 0.0, 0.5, 1.0), 0);
  ASSERT_GE(H5Pget_btree_ratios(dxpl.get(), &left, &middle, &right), 0);
  EXPECT_DOUBLE_EQ(left, 0.0);
  EXPECT_DOUBLE_EQ(middle, 0.5);
  EXPECT_DOUBLE_EQ(right, 1.0);

  ASSERT_GE(H5Pset_hyper_vector_size(dxpl.get(), 1), 0);
  ASSERT_GE(H5Pget_hyper_vector_size(dxpl.get(), &vec_size), 0);
  EXPECT_EQ(vec_size, 1u);
}

TEST(H5PdxplTest, VlenMemManagerRoundTripAndDatasetReclaim) {
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());

  H5MM_allocate_t alloc_fn = CountingVlenAlloc;
  void* alloc_info = reinterpret_cast<void*>(0x1);
  H5MM_free_t free_fn = CountingVlenFree;
  void* free_info = reinterpret_cast<void*>(0x1);
  ASSERT_GE(H5Pget_vlen_mem_manager(H5P_DATASET_XFER_DEFAULT, &alloc_fn,
                                    &alloc_info, &free_fn, &free_info),
            0);
  EXPECT_EQ(alloc_fn, nullptr);
  EXPECT_EQ(alloc_info, nullptr);
  EXPECT_EQ(free_fn, nullptr);
  EXPECT_EQ(free_info, nullptr);
  EXPECT_GE(H5Pget_vlen_mem_manager(dxpl.get(), nullptr, nullptr, nullptr,
                                    nullptr),
            0);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pset_vlen_mem_manager(H5P_DEFAULT, CountingVlenAlloc, nullptr,
                                      CountingVlenFree, nullptr),
              0);
    EXPECT_LT(H5Pset_vlen_mem_manager(H5P_DATASET_XFER_DEFAULT,
                                      CountingVlenAlloc, nullptr,
                                      CountingVlenFree, nullptr),
              0);
    EXPECT_LT(H5Pset_vlen_mem_manager(fcpl.get(), CountingVlenAlloc, nullptr,
                                      CountingVlenFree, nullptr),
              0);
    EXPECT_LT(H5Pset_vlen_mem_manager(H5I_INVALID_HID, CountingVlenAlloc,
                                      nullptr, CountingVlenFree, nullptr),
              0);
    EXPECT_LT(H5Pget_vlen_mem_manager(H5P_DEFAULT, &alloc_fn, &alloc_info,
                                      &free_fn, &free_info),
              0);
    EXPECT_LT(H5Pget_vlen_mem_manager(fcpl.get(), &alloc_fn, &alloc_info,
                                      &free_fn, &free_info),
              0);
    EXPECT_LT(H5Pget_vlen_mem_manager(H5I_INVALID_HID, &alloc_fn, &alloc_info,
                                      &free_fn, &free_info),
              0);
  }

  VlenMemStats stats;
  ASSERT_GE(H5Pset_vlen_mem_manager(dxpl.get(), CountingVlenAlloc, &stats,
                                    CountingVlenFree, &stats),
            0);
  ASSERT_GE(H5Pget_vlen_mem_manager(dxpl.get(), &alloc_fn, &alloc_info,
                                    &free_fn, &free_info),
            0);
  EXPECT_EQ(alloc_fn, CountingVlenAlloc);
  EXPECT_EQ(alloc_info, &stats);
  EXPECT_EQ(free_fn, CountingVlenFree);
  EXPECT_EQ(free_info, &stats);

  ScopedFile file = CreateInMemoryFile("dxpl_vlen_mgr.h5");
  ASSERT_TRUE(file.valid());
  ScopedType vlen_type(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vlen_type.valid());
  const hsize_t dim = 2;
  ScopedSpace space(H5Screate_simple(1, &dim, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "vlen", vlen_type.get(),
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  std::array<int32_t, 2> row0{11, 22};
  std::array<int32_t, 3> row1{33, 44, 55};
  std::array<hvl_t, 2> write_vl{{{row0.size(), row0.data()},
                                 {row1.size(), row1.data()}}};
  ASSERT_GE(H5Dwrite(dset.get(), vlen_type.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     write_vl.data()),
            0);

  std::array<hvl_t, 2> read_vl{};
  absl::Cleanup reclaim_guard = [&]() {
    H5Treclaim(vlen_type.get(), space.get(), dxpl.get(), read_vl.data());
  };
  ASSERT_GE(H5Dread(dset.get(), vlen_type.get(), H5S_ALL, H5S_ALL, dxpl.get(),
                    read_vl.data()),
            0);
  EXPECT_EQ(stats.allocs.load(), 2);
  EXPECT_EQ(read_vl[0].len, 2u);
  EXPECT_EQ(read_vl[1].len, 3u);

  std::move(reclaim_guard).Invoke();
  EXPECT_EQ(stats.frees.load(), 2);

  ASSERT_GE(H5Pset_vlen_mem_manager(dxpl.get(), nullptr, nullptr, nullptr,
                                    nullptr),
            0);
  ASSERT_GE(H5Pget_vlen_mem_manager(dxpl.get(), &alloc_fn, &alloc_info,
                                    &free_fn, &free_info),
            0);
  EXPECT_EQ(alloc_fn, nullptr);
  EXPECT_EQ(free_fn, nullptr);
}

TEST(H5PdxplTest, DataTransformApiCallbacksComparisonAndDatasetIo) {
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ScopedPlist empty_dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(empty_dxpl.valid());
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());

  // Error checks must run on fresh DXPLs because H5Pset_data_transform destroys
  // an existing transform before attempting to parse the new expression.
  {
    ScopedErrorSilence silence;
    ScopedPlist fresh_bad_expr(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(fresh_bad_expr.valid());
    char buf[32] = {};

    EXPECT_LT(H5Pset_data_transform(dxpl.get(), nullptr), 0);
    EXPECT_LT(H5Pset_data_transform(H5P_DEFAULT, "2*x"), 0);
    EXPECT_LT(H5Pset_data_transform(fcpl.get(), "2*x"), 0);
    EXPECT_LT(H5Pset_data_transform(H5I_INVALID_HID, "2*x"), 0);
    EXPECT_LT(H5Pset_data_transform(fresh_bad_expr.get(), "x + *"), 0);

    EXPECT_LT(H5Pget_data_transform(H5I_INVALID_HID, buf, sizeof(buf)), 0);
    EXPECT_LT(H5Pget_data_transform(fcpl.get(), buf, sizeof(buf)), 0);
    EXPECT_LT(H5Pget_data_transform(empty_dxpl.get(), buf, sizeof(buf)), 0);
  }

  ASSERT_GE(H5Pset_data_transform(dxpl.get(), "2*x+1"), 0);
  EXPECT_EQ(H5Pget_data_transform(dxpl.get(), nullptr, 0), 5);

  char full_buf[16] = {};
  EXPECT_EQ(H5Pget_data_transform(dxpl.get(), full_buf, sizeof(full_buf)), 5);
  EXPECT_STREQ(full_buf, "2*x+1");

  char short_buf[4] = {};
  EXPECT_EQ(H5Pget_data_transform(dxpl.get(), short_buf, sizeof(short_buf)), 5);
  EXPECT_STREQ(short_buf, "2*x");

  // Exercise H5P__dxfr_xform_cmp branches via H5Pequal.
  EXPECT_GT(H5Pequal(empty_dxpl.get(), H5P_DATASET_XFER_DEFAULT), 0);
  EXPECT_EQ(H5Pequal(empty_dxpl.get(), dxpl.get()), 0);
  EXPECT_EQ(H5Pequal(dxpl.get(), empty_dxpl.get()), 0);

  ScopedPlist copy_dxpl(H5Pcopy(dxpl.get()));
  ASSERT_TRUE(copy_dxpl.valid());
  EXPECT_GT(H5Pequal(dxpl.get(), copy_dxpl.get()), 0);

  // Overwriting a valid transform frees the previous parse tree cleanly.
  ASSERT_GE(H5Pset_data_transform(copy_dxpl.get(), "x-3"), 0);
  EXPECT_EQ(H5Pequal(dxpl.get(), copy_dxpl.get()), 0);

  // Exercise H5P__dxfr_xform_get / _set / _del via generic H5P APIs.
  H5Z_data_xform_t* extracted = nullptr;
  absl::Cleanup destroy_extracted = [&]() {
    if (extracted != nullptr) {
      H5Z_xform_destroy(extracted);
      extracted = nullptr;
    }
  };
  ASSERT_GE(H5Pget(empty_dxpl.get(), "data_transform", &extracted), 0);
  EXPECT_EQ(extracted, nullptr);

  ASSERT_GE(H5Pget(dxpl.get(), "data_transform", &extracted), 0);
  ASSERT_NE(extracted, nullptr);
  ASSERT_GE(H5Pset(empty_dxpl.get(), "data_transform", &extracted), 0);
  EXPECT_GT(H5Pequal(dxpl.get(), empty_dxpl.get()), 0);
  ASSERT_GE(H5Premove(empty_dxpl.get(), "data_transform"), 0);
  EXPECT_EQ(H5Pequal(dxpl.get(), empty_dxpl.get()), 0);

  // Real dataset write with "2*x+1" transform.
  ScopedFile file = CreateInMemoryFile("dxpl_xform.h5");
  ASSERT_TRUE(file.valid());
  const hsize_t dim = 4;
  ScopedSpace space(H5Screate_simple(1, &dim, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "xformed", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  const std::array<int32_t, 4> raw{1, 2, 3, 4};
  ASSERT_GE(
      H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl.get(),
               raw.data()),
      0);

  std::array<int32_t, 4> stored{};
  ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    stored.data()),
            0);
  EXPECT_THAT(stored, ElementsAre(3, 5, 7, 9));
}

TEST(H5PdxplTest, DatasetIoHyperslabSelectionErrorsComparisonAndIo) {
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ScopedPlist empty_dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(empty_dxpl.valid());
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());

  const hsize_t start2[2] = {0, 0};
  const hsize_t count2[2] = {2, 2};
  const hsize_t bad_stride2[2] = {1, 0};
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pset_dataset_io_hyperslab_selection(
                  dxpl.get(), 0, H5S_SELECT_SET, start2, nullptr, count2,
                  nullptr),
              0);
    EXPECT_LT(H5Pset_dataset_io_hyperslab_selection(
                  dxpl.get(), H5S_MAX_RANK + 1, H5S_SELECT_SET, start2, nullptr,
                  count2, nullptr),
              0);
    EXPECT_LT(H5Pset_dataset_io_hyperslab_selection(
                  dxpl.get(), 2, H5S_SELECT_NOOP, start2, nullptr, count2,
                  nullptr),
              0);
    EXPECT_LT(H5Pset_dataset_io_hyperslab_selection(
                  dxpl.get(), 2, H5S_SELECT_INVALID, start2, nullptr, count2,
                  nullptr),
              0);
    EXPECT_LT(H5Pset_dataset_io_hyperslab_selection(
                  dxpl.get(), 2, H5S_SELECT_SET, nullptr, nullptr, count2,
                  nullptr),
              0);
    EXPECT_LT(H5Pset_dataset_io_hyperslab_selection(
                  dxpl.get(), 2, H5S_SELECT_SET, start2, bad_stride2, count2,
                  nullptr),
              0);
    EXPECT_LT(H5Pset_dataset_io_hyperslab_selection(
                  dxpl.get(), 2, H5S_SELECT_SET, start2, nullptr, nullptr,
                  nullptr),
              0);
    EXPECT_LT(H5Pset_dataset_io_hyperslab_selection(
                  H5P_DEFAULT, 2, H5S_SELECT_SET, start2, nullptr, count2,
                  nullptr),
              0);
    EXPECT_LT(H5Pset_dataset_io_hyperslab_selection(
                  fcpl.get(), 2, H5S_SELECT_SET, start2, nullptr, count2,
                  nullptr),
              0);
    EXPECT_LT(H5Pset_dataset_io_hyperslab_selection(
                  H5I_INVALID_HID, 2, H5S_SELECT_SET, start2, nullptr, count2,
                  nullptr),
              0);

    // Overlapping stride=1, count=2, block=2 fails H5S_select_hyperslab on a
    // fresh DXPL, exercising the space_created cleanup path.
    ScopedPlist fresh_overlap(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(fresh_overlap.valid());
    const hsize_t overlap_s[1] = {0};
    const hsize_t overlap_st[1] = {1};
    const hsize_t overlap_c[1] = {2};
    const hsize_t overlap_b[1] = {2};
    EXPECT_LT(H5Pset_dataset_io_hyperslab_selection(
                  fresh_overlap.get(), 1, H5S_SELECT_SET, overlap_s, overlap_st,
                  overlap_c, overlap_b),
              0);
  }

  // Valid rank-2 SET + same-rank OR.
  const hsize_t s1[2] = {1, 1};
  const hsize_t c1[2] = {2, 2};
  ASSERT_GE(H5Pset_dataset_io_hyperslab_selection(
                dxpl.get(), 2, H5S_SELECT_SET, s1, nullptr, c1, nullptr),
            0);
  const hsize_t s2[2] = {3, 3};
  const hsize_t c2[2] = {1, 1};
  ASSERT_GE(H5Pset_dataset_io_hyperslab_selection(
                dxpl.get(), 2, H5S_SELECT_OR, s2, nullptr, c2, nullptr),
            0);

  // Changing rank with non-SET op fails; changing rank with SET succeeds.
  ScopedPlist rank1_dxpl(H5Pcopy(dxpl.get()));
  ASSERT_TRUE(rank1_dxpl.valid());
  EXPECT_GT(H5Pequal(dxpl.get(), rank1_dxpl.get()), 0);
  {
    ScopedErrorSilence silence;
    const hsize_t r1_s[1] = {0};
    const hsize_t r1_c[1] = {2};
    EXPECT_LT(H5Pset_dataset_io_hyperslab_selection(
                  rank1_dxpl.get(), 1, H5S_SELECT_OR, r1_s, nullptr, r1_c,
                  nullptr),
              0);
  }
  const hsize_t r1_s[1] = {0};
  const hsize_t r1_c[1] = {2};
  ASSERT_GE(H5Pset_dataset_io_hyperslab_selection(
                rank1_dxpl.get(), 1, H5S_SELECT_SET, r1_s, nullptr, r1_c,
                nullptr),
            0);

  // Exercise all comparison branches of H5P__dxfr_dset_io_hyp_sel_cmp.
  ScopedPlist diff_shape_dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(diff_shape_dxpl.valid());
  const hsize_t alt_s[2] = {0, 0};
  const hsize_t alt_c[2] = {1, 3};
  ASSERT_GE(H5Pset_dataset_io_hyperslab_selection(
                diff_shape_dxpl.get(), 2, H5S_SELECT_SET, alt_s, nullptr, alt_c,
                nullptr),
            0);
  EXPECT_EQ(H5Pequal(empty_dxpl.get(), dxpl.get()), 0);
  EXPECT_EQ(H5Pequal(dxpl.get(), empty_dxpl.get()), 0);
  EXPECT_EQ(H5Pequal(dxpl.get(), rank1_dxpl.get()), 0);
  EXPECT_EQ(H5Pequal(dxpl.get(), diff_shape_dxpl.get()), 0);

  // Real I/O with H5S_PLIST using a 4x4 dataset and 2x2 sub-block selection.
  ScopedPlist io_dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(io_dxpl.valid());
  ASSERT_GE(H5Pset_dataset_io_hyperslab_selection(
                io_dxpl.get(), 2, H5S_SELECT_SET, s1, nullptr, c1, nullptr),
            0);

  ScopedFile file = CreateInMemoryFile("dxpl_h5s_plist.h5");
  ASSERT_TRUE(file.valid());
  const hsize_t fdims[2] = {4, 4};
  ScopedSpace fspace(H5Screate_simple(2, fdims, nullptr));
  ASSERT_TRUE(fspace.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "grid", H5T_NATIVE_INT32,
                                fspace.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  std::array<int32_t, 4> patch{10, 20, 30, 40};
  const hsize_t mdims[2] = {2, 2};
  ScopedSpace mspace(H5Screate_simple(2, mdims, nullptr));
  ASSERT_TRUE(mspace.valid());
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, mspace.get(), H5S_PLIST,
                     io_dxpl.get(), patch.data()),
            0);

  std::array<int32_t, 4> read_patch{};
  ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, mspace.get(), H5S_PLIST,
                    io_dxpl.get(), read_patch.data()),
            0);
  EXPECT_EQ(read_patch, patch);
}

TEST(H5PdxplTest, SelectionIoAndModifyWriteBufferProperties) {
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());

  H5D_selection_io_mode_t sel_mode = H5D_SELECTION_IO_MODE_ON;
  uint32_t no_cause = 999u;
  uint32_t actual_mode = 999u;
  bool modify_write_buf = true;

  ASSERT_GE(H5Pget_selection_io(H5P_DATASET_XFER_DEFAULT, &sel_mode), 0);
  EXPECT_EQ(sel_mode, H5D_SELECTION_IO_MODE_DEFAULT);
  ASSERT_GE(H5Pget_no_selection_io_cause(H5P_DATASET_XFER_DEFAULT, &no_cause),
            0);
  EXPECT_EQ(no_cause, 0u);
  ASSERT_GE(
      H5Pget_actual_selection_io_mode(H5P_DATASET_XFER_DEFAULT, &actual_mode),
      0);
  EXPECT_EQ(actual_mode, 0u);
  ASSERT_GE(
      H5Pget_modify_write_buf(H5P_DATASET_XFER_DEFAULT, &modify_write_buf), 0);
  EXPECT_FALSE(modify_write_buf);

  // Null outputs are allowed by getters.
  EXPECT_GE(H5Pget_selection_io(dxpl.get(), nullptr), 0);
  EXPECT_GE(H5Pget_no_selection_io_cause(dxpl.get(), nullptr), 0);
  EXPECT_GE(H5Pget_actual_selection_io_mode(dxpl.get(), nullptr), 0);
  EXPECT_GE(H5Pget_modify_write_buf(dxpl.get(), nullptr), 0);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pset_selection_io(H5P_DEFAULT, H5D_SELECTION_IO_MODE_OFF), 0);
    EXPECT_LT(H5Pset_selection_io(H5P_DATASET_XFER_DEFAULT,
                                  H5D_SELECTION_IO_MODE_OFF),
              0);
    EXPECT_LT(H5Pset_selection_io(fcpl.get(), H5D_SELECTION_IO_MODE_OFF), 0);
    EXPECT_LT(H5Pset_selection_io(H5I_INVALID_HID, H5D_SELECTION_IO_MODE_OFF),
              0);
    EXPECT_LT(H5Pget_selection_io(H5P_DEFAULT, &sel_mode), 0);
    EXPECT_LT(H5Pget_selection_io(fcpl.get(), &sel_mode), 0);
    EXPECT_LT(H5Pget_selection_io(H5I_INVALID_HID, &sel_mode), 0);

    EXPECT_LT(H5Pget_no_selection_io_cause(H5P_DEFAULT, &no_cause), 0);
    EXPECT_LT(H5Pget_no_selection_io_cause(fcpl.get(), &no_cause), 0);
    EXPECT_LT(H5Pget_no_selection_io_cause(H5I_INVALID_HID, &no_cause), 0);
    EXPECT_LT(H5Pget_actual_selection_io_mode(H5P_DEFAULT, &actual_mode), 0);
    EXPECT_LT(H5Pget_actual_selection_io_mode(fcpl.get(), &actual_mode), 0);
    EXPECT_LT(H5Pget_actual_selection_io_mode(H5I_INVALID_HID, &actual_mode),
              0);

    EXPECT_LT(H5Pset_modify_write_buf(H5P_DEFAULT, true), 0);
    EXPECT_LT(H5Pset_modify_write_buf(H5P_DATASET_XFER_DEFAULT, true), 0);
    EXPECT_LT(H5Pset_modify_write_buf(fcpl.get(), true), 0);
    EXPECT_LT(H5Pset_modify_write_buf(H5I_INVALID_HID, true), 0);
    EXPECT_LT(H5Pget_modify_write_buf(H5P_DEFAULT, &modify_write_buf), 0);
    EXPECT_LT(H5Pget_modify_write_buf(fcpl.get(), &modify_write_buf), 0);
    EXPECT_LT(H5Pget_modify_write_buf(H5I_INVALID_HID, &modify_write_buf), 0);
  }

  ASSERT_GE(H5Pset_modify_write_buf(dxpl.get(), true), 0);
  ASSERT_GE(H5Pget_modify_write_buf(dxpl.get(), &modify_write_buf), 0);
  EXPECT_TRUE(modify_write_buf);

  ASSERT_GE(H5Pset_selection_io(dxpl.get(), H5D_SELECTION_IO_MODE_OFF), 0);
  ASSERT_GE(H5Pget_selection_io(dxpl.get(), &sel_mode), 0);
  EXPECT_EQ(sel_mode, H5D_SELECTION_IO_MODE_OFF);

  ScopedFile file = CreateInMemoryFile("dxpl_sel_io.h5");
  ASSERT_TRUE(file.valid());
  const hsize_t dim = 8;
  ScopedSpace space(H5Screate_simple(1, &dim, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "sel", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  std::array<int32_t, 8> data{};
  std::iota(data.begin(), data.end(), 1);
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl.get(),
                     data.data()),
            0);

  ASSERT_GE(H5Pget_no_selection_io_cause(dxpl.get(), &no_cause), 0);
  EXPECT_NE(no_cause & H5D_SEL_IO_DISABLE_BY_API, 0u);
  ASSERT_GE(H5Pget_actual_selection_io_mode(dxpl.get(), &actual_mode), 0);
  EXPECT_EQ(actual_mode, 0u);

  ASSERT_GE(H5Pset_selection_io(dxpl.get(), H5D_SELECTION_IO_MODE_ON), 0);
  ASSERT_GE(H5Pget_selection_io(dxpl.get(), &sel_mode), 0);
  EXPECT_EQ(sel_mode, H5D_SELECTION_IO_MODE_ON);

  std::array<int32_t, 8> read_back{};
  ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl.get(),
                    read_back.data()),
            0);
  EXPECT_EQ(read_back, data);
  ASSERT_GE(H5Pget_actual_selection_io_mode(dxpl.get(), &actual_mode), 0);
}

TEST(H5PdxplTest, EncodeDecodeRoundTripAndCorruptPayloads) {
  // Round-trip default DXPL (includes NULL data_transform, len == 0 path).
  ScopedPlist def_dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(def_dxpl.valid());
  size_t def_size = 0;
  ASSERT_GE(H5Pencode2(def_dxpl.get(), nullptr, &def_size, H5P_DEFAULT), 0);
  ASSERT_GT(def_size, 0u);
  std::vector<uint8_t> def_buf(def_size, 0);
  ASSERT_GE(
      H5Pencode2(def_dxpl.get(), def_buf.data(), &def_size, H5P_DEFAULT), 0);
  ScopedPlist decoded_def(H5Pdecode(def_buf.data()));
  ASSERT_TRUE(decoded_def.valid());
  EXPECT_GT(H5Pequal(def_dxpl.get(), decoded_def.get()), 0);

  // Round-trip customized DXPL covering all enc/dec callbacks.
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ASSERT_GE(H5Pset_buffer(dxpl.get(), 4096, nullptr, nullptr), 0);
  ASSERT_GE(H5Pset_preserve(dxpl.get(), true), 0);
  ASSERT_GE(H5Pset_btree_ratios(dxpl.get(), 0.2, 0.4, 0.8), 0);
  ASSERT_GE(H5Pset_hyper_vector_size(dxpl.get(), 512), 0);
  ASSERT_GE(H5Pset_edc_check(dxpl.get(), H5Z_DISABLE_EDC), 0);
  ASSERT_GE(H5Pset_data_transform(dxpl.get(), "2*x+1"), 0);
  ASSERT_GE(H5Pset_selection_io(dxpl.get(), H5D_SELECTION_IO_MODE_ON), 0);
  ASSERT_GE(H5Pset_modify_write_buf(dxpl.get(), true), 0);

  size_t enc_size = 0;
  ASSERT_GE(H5Pencode2(dxpl.get(), nullptr, &enc_size, H5P_DEFAULT), 0);
  std::vector<uint8_t> buf(enc_size, 0);
  ASSERT_GE(H5Pencode2(dxpl.get(), buf.data(), &enc_size, H5P_DEFAULT), 0);

  ScopedPlist decoded(H5Pdecode(buf.data()));
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Pequal(dxpl.get(), decoded.get()), 0);

  EXPECT_EQ(H5Pget_preserve(decoded.get()), 1);
  EXPECT_EQ(H5Pget_edc_check(decoded.get()), H5Z_DISABLE_EDC);
  H5D_selection_io_mode_t sel_mode = H5D_SELECTION_IO_MODE_DEFAULT;
  ASSERT_GE(H5Pget_selection_io(decoded.get(), &sel_mode), 0);
  EXPECT_EQ(sel_mode, H5D_SELECTION_IO_MODE_ON);
  bool mod_buf = false;
  ASSERT_GE(H5Pget_modify_write_buf(decoded.get(), &mod_buf), 0);
  EXPECT_TRUE(mod_buf);
  char xform[16] = {};
  ASSERT_GT(H5Pget_data_transform(decoded.get(), xform, sizeof(xform)), 0);
  EXPECT_STREQ(xform, "2*x+1");

  // Corrupt encoded btree_split_ratio double-size byte -> decode must fail.
  {
    std::vector<uint8_t> corrupt_btree = buf;
    const std::string tag("btree_split_ratio\0", 18);
    auto it = std::search(corrupt_btree.begin(), corrupt_btree.end(),
                          tag.begin(), tag.end());
    ASSERT_NE(it, corrupt_btree.end());
    const size_t size_byte_idx =
        static_cast<size_t>(std::distance(corrupt_btree.begin(), it)) +
        tag.size();
    ASSERT_LT(size_byte_idx, corrupt_btree.size());
    corrupt_btree[size_byte_idx] ^= 1u;

    ScopedErrorSilence silence;
    EXPECT_EQ(H5Pdecode(corrupt_btree.data()), H5I_INVALID_HID);
  }

  // Corrupt encoded data_transform expression -> H5Z_xform_create fails.
  {
    std::vector<uint8_t> corrupt_xform = buf;
    const std::string expr("2*x+1", 5);
    auto it = std::search(corrupt_xform.begin(), corrupt_xform.end(),
                          expr.begin(), expr.end());
    ASSERT_NE(it, corrupt_xform.end());
    std::fill_n(it, expr.size(), '?');

    ScopedErrorSilence silence;
    EXPECT_EQ(H5Pdecode(corrupt_xform.data()), H5I_INVALID_HID);
  }
}

TEST(H5PdxplTest, RemovedPropertiesFailInternalGetSetAndPeek) {
  ScopedErrorSilence silence;

  // 1. max_temp_buf / tconv_buf / bkgr_buf removed individually.
  {
    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl.valid());
    ASSERT_GE(H5Premove(dxpl.get(), H5D_XFER_MAX_TEMP_BUF_NAME), 0);
    EXPECT_LT(H5Pset_buffer(dxpl.get(), 64, nullptr, nullptr), 0);
    EXPECT_EQ(H5Pget_buffer(dxpl.get(), nullptr, nullptr), 0u);
  }
  {
    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl.valid());
    void* tconv = nullptr;
    ASSERT_GE(H5Premove(dxpl.get(), H5D_XFER_TCONV_BUF_NAME), 0);
    EXPECT_LT(H5Pset_buffer(dxpl.get(), 64, nullptr, nullptr), 0);
    EXPECT_EQ(H5Pget_buffer(dxpl.get(), &tconv, nullptr), 0u);
  }
  {
    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl.valid());
    void* bkg = nullptr;
    ASSERT_GE(H5Premove(dxpl.get(), H5D_XFER_BKGR_BUF_NAME), 0);
    EXPECT_LT(H5Pset_buffer(dxpl.get(), 64, nullptr, nullptr), 0);
    EXPECT_EQ(H5Pget_buffer(dxpl.get(), nullptr, &bkg), 0u);
  }

  // 2. vlen_alloc / vlen_alloc_info / vlen_free / vlen_free_info removed.
  {
    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl.valid());
    H5MM_allocate_t alloc_fn = nullptr;

    ASSERT_GE(H5Premove(dxpl.get(), H5D_XFER_VLEN_ALLOC_NAME), 0);
    EXPECT_LT(H5Pset_vlen_mem_manager(dxpl.get(), nullptr, nullptr, nullptr,
                                      nullptr),
              0);
    EXPECT_LT(H5Pget_vlen_mem_manager(dxpl.get(), &alloc_fn, nullptr, nullptr,
                                      nullptr),
              0);
  }
  {
    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl.valid());
    void* info = nullptr;
    ASSERT_GE(H5Premove(dxpl.get(), H5D_XFER_VLEN_ALLOC_INFO_NAME), 0);
    EXPECT_LT(H5Pset_vlen_mem_manager(dxpl.get(), nullptr, nullptr, nullptr,
                                      nullptr),
              0);
    EXPECT_LT(H5Pget_vlen_mem_manager(dxpl.get(), nullptr, &info, nullptr,
                                      nullptr),
              0);
  }
  {
    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl.valid());
    H5MM_free_t free_fn = nullptr;
    ASSERT_GE(H5Premove(dxpl.get(), H5D_XFER_VLEN_FREE_NAME), 0);
    EXPECT_LT(H5Pset_vlen_mem_manager(dxpl.get(), nullptr, nullptr, nullptr,
                                      nullptr),
              0);
    EXPECT_LT(H5Pget_vlen_mem_manager(dxpl.get(), nullptr, nullptr, &free_fn,
                                      nullptr),
              0);
  }
  {
    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl.valid());
    void* info = nullptr;
    ASSERT_GE(H5Premove(dxpl.get(), H5D_XFER_VLEN_FREE_INFO_NAME), 0);
    EXPECT_LT(H5Pset_vlen_mem_manager(dxpl.get(), nullptr, nullptr, nullptr,
                                      nullptr),
              0);
    EXPECT_LT(H5Pget_vlen_mem_manager(dxpl.get(), nullptr, nullptr, nullptr,
                                      &info),
              0);
  }

  // 3. Remaining DXPL properties removed on a single DXPL.
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  const char* props[] = {
      H5D_XFER_XFORM_NAME,
      H5D_XFER_BKGR_BUF_TYPE_NAME,
      H5D_XFER_EDC_NAME,
      H5D_XFER_FILTER_CB_NAME,
      H5D_XFER_CONV_CB_NAME,
      H5D_XFER_BTREE_SPLIT_RATIO_NAME,
      H5D_XFER_HYPER_VECTOR_SIZE_NAME,
      H5D_XFER_DSET_IO_SEL_NAME,
      H5D_XFER_SELECTION_IO_MODE_NAME,
      H5D_XFER_NO_SELECTION_IO_CAUSE_NAME,
      H5D_XFER_ACTUAL_SELECTION_IO_MODE_NAME,
      H5D_XFER_MODIFY_WRITE_BUF_NAME,
  };
  for (const char* name : props) {
    ASSERT_GE(H5Premove(dxpl.get(), name), 0);
  }

  char xform_buf[8] = {};
  EXPECT_LT(H5Pset_data_transform(dxpl.get(), "x"), 0);
  EXPECT_LT(H5Pget_data_transform(dxpl.get(), xform_buf, sizeof(xform_buf)), 0);

  EXPECT_LT(H5Pset_preserve(dxpl.get(), true), 0);
  EXPECT_LT(H5Pget_preserve(dxpl.get()), 0);

  EXPECT_LT(H5Pset_edc_check(dxpl.get(), H5Z_ENABLE_EDC), 0);
  EXPECT_EQ(H5Pget_edc_check(dxpl.get()), H5Z_ERROR_EDC);

  EXPECT_LT(H5Pset_filter_callback(dxpl.get(), nullptr, nullptr), 0);

  H5T_conv_except_func_t conv_op = nullptr;
  void* conv_data = nullptr;
  EXPECT_LT(H5Pset_type_conv_cb(dxpl.get(), nullptr, nullptr), 0);
  EXPECT_LT(H5Pget_type_conv_cb(dxpl.get(), &conv_op, &conv_data), 0);

  double left = 0.0;
  double middle = 0.0;
  double right = 0.0;
  EXPECT_LT(H5Pset_btree_ratios(dxpl.get(), 0.1, 0.5, 0.9), 0);
  EXPECT_LT(H5Pget_btree_ratios(dxpl.get(), &left, &middle, &right), 0);

  size_t vec_size = 0;
  EXPECT_LT(H5Pset_hyper_vector_size(dxpl.get(), 16), 0);
  EXPECT_LT(H5Pget_hyper_vector_size(dxpl.get(), &vec_size), 0);

  const hsize_t s[1] = {0};
  const hsize_t c[1] = {1};
  EXPECT_LT(H5Pset_dataset_io_hyperslab_selection(dxpl.get(), 1, H5S_SELECT_SET,
                                                  s, nullptr, c, nullptr),
            0);

  H5D_selection_io_mode_t sel_mode = H5D_SELECTION_IO_MODE_DEFAULT;
  uint32_t u32 = 0;
  bool mod_buf = false;
  EXPECT_LT(H5Pset_selection_io(dxpl.get(), H5D_SELECTION_IO_MODE_ON), 0);
  EXPECT_LT(H5Pget_selection_io(dxpl.get(), &sel_mode), 0);
  EXPECT_LT(H5Pget_no_selection_io_cause(dxpl.get(), &u32), 0);
  EXPECT_LT(H5Pget_actual_selection_io_mode(dxpl.get(), &u32), 0);
  EXPECT_LT(H5Pset_modify_write_buf(dxpl.get(), true), 0);
  EXPECT_LT(H5Pget_modify_write_buf(dxpl.get(), &mod_buf), 0);
}

}  // namespace
}  // namespace hdf5
