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
// Unit tests for HDF5 v1 B-tree chunked dataset indexing, out-of-order
// insertion, node splits, lookup, iteration, chunk reallocation, extent
// pruning, object copying, deletion, key encoding/decoding, and debug dumping
// (third_party/hdf5/src/H5Dbtree.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Dbtree.c

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug/stats fields from
// H5C_cache_entry_t / H5AC_info_t inside internal structs. Match `:hdf5`
// struct layouts when calling internal H5D_COPS_BTREE / H5D_btree_debug APIs.
#ifndef NDEBUG
#define H5DBTREE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
// Google3 builds with -fno-exceptions, where wrapper_internal_exception_macros.h
// defines `#define try if (1)`. H5B2private.h (pulled by H5Dpkg.h) uses `try`
// as a C parameter identifier.
#pragma push_macro("try")
#undef try
#define try h5_try_param
extern "C" {
#define H5B_FRIEND
#define H5D_FRIEND
#include "third_party/hdf5/src/H5Bpkg.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Dpkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5UCprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#undef try
#pragma pop_macro("try")
#ifdef H5DBTREE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5DBTREE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

// RAII wrapper for pushing/popping an HDF5 API context when invoking package
// or private HDF5 functions directly.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(hid_t dxpl_id = H5P_DEFAULT) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      if (dxpl_id != H5P_DEFAULT) {
        H5CX_set_dxpl(dxpl_id);
      }
    }
  }

  ~ScopedApiContext() {
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

ScopedPlist MakeV1BtreeFcpl(unsigned istore_k = 2) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  if (fcpl.valid() && H5Pset_istore_k(fcpl.get(), istore_k) < 0) {
    fcpl.reset();
  }
  return fcpl;
}

ScopedPlist MakeV1BtreeCoreFapl() {
  ScopedPlist fapl = MakeCoreFapl();
  if (fapl.valid() &&
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18) <
          0) {
    fapl.reset();
  }
  return fapl;
}

std::string ReadStreamToString(FILE* stream) {
  if (stream == nullptr) {
    return {};
  }
  std::fflush(stream);
  std::rewind(stream);
  std::string out;
  char buf[512];
  while (size_t n = std::fread(buf, 1, sizeof(buf), stream)) {
    out.append(buf, n);
  }
  return out;
}

herr_t WriteHyperslab1D(hid_t dset_id, hsize_t start, hsize_t count,
                       const int* data) {
  ScopedSpace fspace(H5Dget_space(dset_id));
  ScopedSpace mspace(H5Screate_simple(1, &count, nullptr));
  if (!fspace.valid() || !mspace.valid()) {
    return -1;
  }
  if (H5Sselect_hyperslab(fspace.get(), H5S_SELECT_SET, &start, nullptr, &count,
                          nullptr) < 0) {
    return -1;
  }
  return H5Dwrite(dset_id, H5T_NATIVE_INT, mspace.get(), fspace.get(),
                  H5P_DEFAULT, data);
}

herr_t ReadHyperslab1D(hid_t dset_id, hsize_t start, hsize_t count, int* out) {
  ScopedSpace fspace(H5Dget_space(dset_id));
  ScopedSpace mspace(H5Screate_simple(1, &count, nullptr));
  if (!fspace.valid() || !mspace.valid()) {
    return -1;
  }
  if (H5Sselect_hyperslab(fspace.get(), H5S_SELECT_SET, &start, nullptr, &count,
                          nullptr) < 0) {
    return -1;
  }
  return H5Dread(dset_id, H5T_NATIVE_INT, mspace.get(), fspace.get(),
                 H5P_DEFAULT, out);
}

TEST(H5DbtreeTest, OneDimensionalOutOfOrderSplitsLookupsAndIter) {
  // Small istore_k = 2 forces multi-level B-tree splits after >4 chunks.
  ScopedPlist fcpl = MakeV1BtreeFcpl(/*istore_k=*/2);
  ASSERT_TRUE(fcpl.valid());
  ScopedPlist fapl = MakeV1BtreeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ScopedFile file(H5Fcreate("btree_1d.h5", H5F_ACC_TRUNC, fcpl.get(),
                            fapl.get()));
  ASSERT_TRUE(file.valid());

  constexpr hsize_t kChunkLen = 4;
  constexpr hsize_t kNumChunks = 50;
  const hsize_t dims[1] = {kChunkLen * kNumChunks};
  const hsize_t max_dims[1] = {H5S_UNLIMITED};
  ScopedSpace space(H5Screate_simple(1, dims, max_dims));
  ASSERT_TRUE(space.valid());
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, &kChunkLen), 0);
  int fill_val = -999;
  ASSERT_GE(H5Pset_fill_value(dcpl.get(), H5T_NATIVE_INT, &fill_val), 0);

  ScopedDataset dset(H5Dcreate2(file.get(), "/d1", H5T_NATIVE_INT, space.get(),
                                H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  H5D_chunk_index_t idx_type = H5D_CHUNK_IDX_NONE;
  ASSERT_GE(H5Dget_chunk_index_type(dset.get(), &idx_type), 0);
  EXPECT_EQ(idx_type, H5D_CHUNK_IDX_BTREE);

  // Write chunk index 10 first, then index 2 (smaller than leftmost root key)
  // to exercise H5B_INS_LEFT in H5D__btree_new_node(), then descending/sparse
  // chunks to exercise splits (H5D__btree_cmp2) and 1-D cmp3 comparisons.
  std::vector<hsize_t> order = {10, 2, 40, 20};
  for (int i = 36; i >= 0; i -= 2) {
    if (i != 10 && i != 2 && i != 20) {
      order.push_back(static_cast<hsize_t>(i));
    }
  }
  for (hsize_t idx : order) {
    std::array<int, 4> chunk = {static_cast<int>(idx * 10 + 0),
                                static_cast<int>(idx * 10 + 1),
                                static_cast<int>(idx * 10 + 2),
                                static_cast<int>(idx * 10 + 3)};
    ASSERT_GE(WriteHyperslab1D(dset.get(), idx * kChunkLen, kChunkLen,
                               chunk.data()),
              0);
  }
  // Rewrite existing chunk [20] directly to trigger 1-D cmp3 exact-boundary
  // branch (udata->scaled[0] == rt_key->scaled[0]).
  const std::array<int, 4> rewrite20 = {200, 201, 202, 203};
  ASSERT_GE(WriteHyperslab1D(dset.get(), 20 * kChunkLen, kChunkLen,
                             rewrite20.data()),
            0);
  ASSERT_GE(H5Dflush(dset.get()), 0);

  hsize_t nchunks = 0;
  ASSERT_GE(H5Dget_num_chunks(dset.get(), H5S_ALL, &nchunks), 0);
  EXPECT_EQ(nchunks, order.size());

  // Read existing chunk and sparse unwritten chunks (both between written
  // chunks [5] and beyond max written chunk [45]) to hit found=true and
  // found=false paths in H5D__btree_found.
  std::array<int, 4> read_buf{};
  ASSERT_GE(ReadHyperslab1D(dset.get(), 10 * kChunkLen, kChunkLen,
                            read_buf.data()),
            0);
  EXPECT_THAT(read_buf, ::testing::ElementsAre(100, 101, 102, 103));

  ASSERT_GE(
      ReadHyperslab1D(dset.get(), 5 * kChunkLen, kChunkLen, read_buf.data()),
      0);
  EXPECT_THAT(read_buf, ::testing::ElementsAre(-999, -999, -999, -999));

  ASSERT_GE(
      ReadHyperslab1D(dset.get(), 45 * kChunkLen, kChunkLen, read_buf.data()),
      0);
  EXPECT_THAT(read_buf, ::testing::ElementsAre(-999, -999, -999, -999));

  // Verify chunk coordinate lookup succeeds for written chunk and reports
  // unallocated (size == 0, addr == HADDR_UNDEF) for sparse gaps.
  hsize_t chk_size = 0;
  const hsize_t hit_off = 2 * kChunkLen;
  ASSERT_GE(
      H5Dget_chunk_info_by_coord(dset.get(), &hit_off, nullptr, nullptr,
                                 &chk_size),
      0);
  EXPECT_EQ(chk_size, kChunkLen * sizeof(int));
  {
    haddr_t gap_addr = 0;
    const hsize_t gap_off = 5 * kChunkLen;
    EXPECT_GE(H5Dget_chunk_info_by_coord(dset.get(), &gap_off, nullptr,
                                         &gap_addr, &chk_size),
              0);
    EXPECT_EQ(chk_size, 0);
    EXPECT_EQ(gap_addr, HADDR_UNDEF);
  }

  // Exercise H5Dchunk_iter: full iteration, early H5_ITER_STOP, and error.
  hsize_t iter_count = 0;
  auto count_cb = [](const hsize_t* /*offset*/, unsigned /*filter_mask*/,
                     haddr_t /*addr*/, hsize_t nbytes,
                     void* op_data) -> herr_t {
    EXPECT_EQ(nbytes, kChunkLen * sizeof(int));
    ++(*static_cast<hsize_t*>(op_data));
    return H5_ITER_CONT;
  };
  ASSERT_EQ(H5Dchunk_iter(dset.get(), H5P_DEFAULT, count_cb, &iter_count), 0);
  EXPECT_EQ(iter_count, nchunks);

  hsize_t stop_count = 0;
  auto stop_cb = [](const hsize_t*, unsigned, haddr_t, hsize_t,
                    void* op_data) -> herr_t {
    if (++(*static_cast<hsize_t*>(op_data)) >= 3) {
      return H5_ITER_STOP;
    }
    return H5_ITER_CONT;
  };
  EXPECT_GE(H5Dchunk_iter(dset.get(), H5P_DEFAULT, stop_cb, &stop_count), 0);
  EXPECT_EQ(stop_count, 3);

  {
    ScopedErrorSilence silence;
    auto err_cb = [](const hsize_t*, unsigned, haddr_t, hsize_t,
                     void*) -> herr_t { return -1; };
    EXPECT_LT(H5Dchunk_iter(dset.get(), H5P_DEFAULT, err_cb, nullptr), 0);
  }
}

TEST(H5DbtreeTest, MultiDimensionalChunksReallocAndNoopInsert) {
  ScopedPlist fcpl = MakeV1BtreeFcpl(/*istore_k=*/2);
  ASSERT_TRUE(fcpl.valid());
  ScopedPlist fapl = MakeV1BtreeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ScopedFile file(H5Fcreate("btree_nd.h5", H5F_ACC_TRUNC, fcpl.get(),
                            fapl.get()));
  ASSERT_TRUE(file.valid());

  // 2-D compressed chunked dataset with chunk cache disabled so repeated
  // writes immediately enter H5D__btree_insert (exercising CHANGE and NOOP).
  const hsize_t dims2[2] = {32, 32};
  const hsize_t chunk2[2] = {4, 4};
  ScopedSpace space2(H5Screate_simple(2, dims2, nullptr));
  ASSERT_TRUE(space2.valid());
  ScopedPlist dcpl2(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl2.valid());
  ASSERT_GE(H5Pset_chunk(dcpl2.get(), 2, chunk2), 0);
  ASSERT_GE(H5Pset_deflate(dcpl2.get(), 6), 0);
  ScopedPlist dapl2(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_TRUE(dapl2.valid());
  ASSERT_GE(H5Pset_chunk_cache(dapl2.get(), 1, 0, 0.0), 0);

  ScopedDataset dset2(H5Dcreate2(file.get(), "/d2", H5T_NATIVE_INT,
                                 space2.get(), H5P_DEFAULT, dcpl2.get(),
                                 dapl2.get()));
  ASSERT_TRUE(dset2.valid());

  // Write top-right chunk [24, 24] before [0, 0] (H5B_INS_LEFT), then
  // additional sparse 2-D chunks to force node splits and 2-D cmp2 / cmp3.
  const std::array<std::pair<hsize_t, hsize_t>, 12> coords2 = {{
      {24, 24},
      {0, 0},
      {28, 28},
      {12, 16},
      {16, 8},
      {8, 20},
      {4, 12},
      {20, 4},
      {0, 28},
      {28, 0},
      {12, 0},
      {8, 8},
  }};
  ScopedSpace mspace2(H5Screate_simple(2, chunk2, nullptr));
  ASSERT_TRUE(mspace2.valid());
  ScopedSpace fspace2(H5Dget_space(dset2.get()));
  ASSERT_TRUE(fspace2.valid());

  for (size_t idx = 0; idx < coords2.size(); ++idx) {
    std::array<int, 16> zeros{};
    if (idx != 1) {
      for (size_t j = 0; j < zeros.size(); ++j) {
        zeros[j] = static_cast<int>((idx + 1) * 31 + j * 7);
      }
    }
    const hsize_t start[2] = {coords2[idx].first, coords2[idx].second};
    ASSERT_GE(H5Sselect_hyperslab(fspace2.get(), H5S_SELECT_SET, start, nullptr,
                                  chunk2, nullptr),
              0);
    ASSERT_GE(H5Dwrite(dset2.get(), H5T_NATIVE_INT, mspace2.get(),
                       fspace2.get(), H5P_DEFAULT, zeros.data()),
              0);
  }

  // Rewriting [0, 0] with all zeros keeps size identical (H5B_INS_NOOP),
  // whereas incompressible pattern changes compressed size (H5B_INS_CHANGE).
  const hsize_t origin2[2] = {0, 0};
  ASSERT_GE(H5Sselect_hyperslab(fspace2.get(), H5S_SELECT_SET, origin2, nullptr,
                                chunk2, nullptr),
            0);
  std::array<int, 16> still_zeros{};
  ASSERT_GE(H5Dwrite(dset2.get(), H5T_NATIVE_INT, mspace2.get(), fspace2.get(),
                     H5P_DEFAULT, still_zeros.data()),
            0);

  std::array<int, 16> dense_vals{};
  for (int i = 0; i < 16; ++i) {
    uint32_t u = static_cast<uint32_t>(i);
    dense_vals[i] = static_cast<int>((u * 2654435761U) ^ (u << 17));
  }
  ASSERT_GE(H5Dwrite(dset2.get(), H5T_NATIVE_INT, mspace2.get(), fspace2.get(),
                     H5P_DEFAULT, dense_vals.data()),
            0);

  // Also test H5Dwrite_chunk directly on [4, 4] with varying then same nbytes.
  const hsize_t off44[2] = {4, 4};
  const std::array<uint8_t, 12> raw12 = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  const std::array<uint8_t, 24> raw24 = {1, 2, 3, 4, 5, 6, 7, 8};
  ASSERT_GE(H5Dwrite_chunk(dset2.get(), H5P_DEFAULT, 1, off44, raw12.size(),
                           raw12.data()),
            0);
  ASSERT_GE(H5Dwrite_chunk(dset2.get(), H5P_DEFAULT, 1, off44, raw24.size(),
                           raw24.data()),
            0);
  ASSERT_GE(H5Dwrite_chunk(dset2.get(), H5P_DEFAULT, 1, off44, raw24.size(),
                           raw24.data()),
            0);

  // Sparse 2-D read on unwritten gap [4, 0] exercises H5D__btree_found false.
  const hsize_t gap2[2] = {4, 0};
  ASSERT_GE(H5Sselect_hyperslab(fspace2.get(), H5S_SELECT_SET, gap2, nullptr,
                                chunk2, nullptr),
            0);
  std::array<int, 16> out2{};
  std::fill(out2.begin(), out2.end(), 123);
  ASSERT_GE(H5Dread(dset2.get(), H5T_NATIVE_INT, mspace2.get(), fspace2.get(),
                    H5P_DEFAULT, out2.data()),
            0);
  EXPECT_EQ(out2[0], 0);

  // 3-D dataset (ndims == 4 internally) for 3-D key comparison & encoding.
  const hsize_t dims3[3] = {8, 8, 8};
  const hsize_t chunk3[3] = {2, 2, 2};
  ScopedSpace space3(H5Screate_simple(3, dims3, nullptr));
  ASSERT_TRUE(space3.valid());
  ScopedPlist dcpl3(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl3.valid());
  ASSERT_GE(H5Pset_chunk(dcpl3.get(), 3, chunk3), 0);
  ScopedDataset dset3(H5Dcreate2(file.get(), "/d3", H5T_NATIVE_INT,
                                 space3.get(), H5P_DEFAULT, dcpl3.get(),
                                 H5P_DEFAULT));
  ASSERT_TRUE(dset3.valid());
  std::array<int, 8> w3 = {1, 2, 3, 4, 5, 6, 7, 8};
  const hsize_t off3a[3] = {4, 4, 4};
  const hsize_t off3b[3] = {0, 0, 0};
  ASSERT_GE(H5Dwrite_chunk(dset3.get(), H5P_DEFAULT, 0, off3a,
                           w3.size() * sizeof(int), w3.data()),
            0);
  ASSERT_GE(H5Dwrite_chunk(dset3.get(), H5P_DEFAULT, 0, off3b,
                           w3.size() * sizeof(int), w3.data()),
            0);
  hsize_t n3 = 0;
  ASSERT_GE(H5Dget_num_chunks(dset3.get(), H5S_ALL, &n3), 0);
  EXPECT_EQ(n3, 2);
}

TEST(H5DbtreeTest, ExtentPruningCopyDeleteAndNativeInfo) {
  ScopedPlist fcpl = MakeV1BtreeFcpl(/*istore_k=*/2);
  ASSERT_TRUE(fcpl.valid());
  ScopedPlist fapl = MakeV1BtreeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ScopedFile src_file(H5Fcreate("btree_src.h5", H5F_ACC_TRUNC, fcpl.get(),
                                fapl.get()));
  ASSERT_TRUE(src_file.valid());
  ScopedFile dst_file(H5Fcreate("btree_dst.h5", H5F_ACC_TRUNC, fcpl.get(),
                                fapl.get()));
  ASSERT_TRUE(dst_file.valid());

  constexpr hsize_t kChunkLen = 4;
  const hsize_t init_dims[1] = {100};
  const hsize_t max_dims[1] = {H5S_UNLIMITED};
  ScopedSpace space(H5Screate_simple(1, init_dims, max_dims));
  ASSERT_TRUE(space.valid());
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, &kChunkLen), 0);

  ScopedDataset dset(H5Dcreate2(src_file.get(), "/prune", H5T_NATIVE_INT,
                                space.get(), H5P_DEFAULT, dcpl.get(),
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  for (hsize_t idx = 0; idx < 22; ++idx) {
    std::array<int, 4> buf = {static_cast<int>(idx), static_cast<int>(idx + 1),
                              static_cast<int>(idx + 2),
                              static_cast<int>(idx + 3)};
    ASSERT_GE(WriteHyperslab1D(dset.get(), idx * kChunkLen, kChunkLen,
                               buf.data()),
              0);
  }
  ASSERT_GE(H5Dflush(dset.get()), 0);

  // Trigger H5O__dset_bh_info -> H5D__chunk_bh_info -> H5D__btree_idx_size.
  H5O_native_info_t ninfo{};
  ASSERT_GE(
      H5Oget_native_info(dset.get(), &ninfo, H5O_NATIVE_INFO_META_SIZE), 0);
  EXPECT_GT(ninfo.meta_size.obj.index_size, 0);

  // Copy populated v1-btree dataset into same file and separate destination
  // file (triggers copy_setup, iterate, copy_shutdown).
  ASSERT_GE(H5Ocopy(src_file.get(), "/prune", src_file.get(), "/prune_copy",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Ocopy(src_file.get(), "/prune", dst_file.get(), "/prune_remote",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);

  ScopedDataset remote_copy(
      H5Dopen2(dst_file.get(), "/prune_remote", H5P_DEFAULT));
  ASSERT_TRUE(remote_copy.valid());
  hsize_t remote_chunks = 0;
  ASSERT_GE(H5Dget_num_chunks(remote_copy.get(), H5S_ALL, &remote_chunks), 0);
  EXPECT_EQ(remote_chunks, 22);
  remote_copy.reset();

  // Shrink extent across multiple B-tree levels (22 -> 4 chunks -> 0 chunks),
  // invoking H5D__btree_idx_remove and H5D__btree_remove.
  const hsize_t shrink_mid[1] = {16};
  ASSERT_GE(H5Dset_extent(dset.get(), shrink_mid), 0);
  hsize_t nchunks = 0;
  ASSERT_GE(H5Dget_num_chunks(dset.get(), H5S_ALL, &nchunks), 0);
  EXPECT_EQ(nchunks, 4);

  const hsize_t shrink_zero[1] = {0};
  ASSERT_GE(H5Dset_extent(dset.get(), shrink_zero), 0);
  ASSERT_GE(H5Dget_num_chunks(dset.get(), H5S_ALL, &nchunks), 0);
  EXPECT_EQ(nchunks, 0);

  // Re-extend and write so idx_delete cleans up non-empty B-tree via H5Ldelete.
  const hsize_t re_extend[1] = {32};
  ASSERT_GE(H5Dset_extent(dset.get(), re_extend), 0);
  for (hsize_t idx = 0; idx < 8; ++idx) {
    const std::array<int, 4> buf = {1, 2, 3, 4};
    ASSERT_GE(
        WriteHyperslab1D(dset.get(), idx * kChunkLen, kChunkLen, buf.data()),
        0);
  }
  dset.reset();

  // Also create and delete an unallocated v1-btree dataset (idx_addr == UNDEF).
  ScopedDataset empty_dset(H5Dcreate2(src_file.get(), "/empty_chk",
                                      H5T_NATIVE_INT, space.get(), H5P_DEFAULT,
                                      dcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(empty_dset.valid());
  empty_dset.reset();

  ASSERT_GE(H5Ldelete(src_file.get(), "/prune", H5P_DEFAULT), 0);
  ASSERT_GE(H5Ldelete(src_file.get(), "/prune_copy", H5P_DEFAULT), 0);
  ASSERT_GE(H5Ldelete(src_file.get(), "/empty_chk", H5P_DEFAULT), 0);
}

TEST(H5DbtreeTest, OnDiskEncodeDecodeInternalOpsAndDebugPaths) {
  TempFile disk_file("h5dbtree_disk.h5");
  ScopedPlist fcpl = MakeV1BtreeFcpl(/*istore_k=*/2);
  ASSERT_TRUE(fcpl.valid());
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
      0);

  haddr_t saved_idx_addr = HADDR_UNDEF;
  constexpr hsize_t kChunkLen = 4;
  {
    ScopedFile file(H5Fcreate(disk_file.c_str(), H5F_ACC_TRUNC, fcpl.get(),
                              fapl.get()));
    ASSERT_TRUE(file.valid());

    const hsize_t dims[1] = {80};
    ScopedSpace space(H5Screate_simple(1, dims, nullptr));
    ASSERT_TRUE(space.valid());
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, &kChunkLen), 0);
    ScopedDataset dset(H5Dcreate2(file.get(), "/disk_dset", H5T_NATIVE_INT,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    for (hsize_t idx = 0; idx < 18; ++idx) {
      std::array<int, 4> vals = {static_cast<int>(idx * 4 + 0),
                                 static_cast<int>(idx * 4 + 1),
                                 static_cast<int>(idx * 4 + 2),
                                 static_cast<int>(idx * 4 + 3)};
      ASSERT_GE(WriteHyperslab1D(dset.get(), idx * kChunkLen, kChunkLen,
                                 vals.data()),
                0);
    }
    ASSERT_GE(H5Dflush(dset.get()), 0);
    auto* f_ptr =
        static_cast<H5F_t*>(H5VL_object_verify(file.get(), H5I_FILE));
    auto* dset_ptr =
        static_cast<H5D_t*>(H5VL_object_verify(dset.get(), H5I_DATASET));
    ASSERT_NE(f_ptr, nullptr);
    ASSERT_NE(dset_ptr, nullptr);
    saved_idx_addr = dset_ptr->shared->layout.storage.u.chunk.idx_addr;
    ASSERT_TRUE(H5_addr_defined(saved_idx_addr));

    // Re-insert an existing chunk with identical size/offset to exercise
    // H5B_INS_NOOP in H5D__btree_insert, plus bogus-address error paths.
    {
      ScopedApiContext api_ctx;
      ASSERT_TRUE(api_ctx.valid());
      H5D_chk_idx_info_t w_idx{};
      w_idx.f = f_ptr;
      w_idx.pline = &dset_ptr->shared->dcpl_cache.pline;
      w_idx.layout = &dset_ptr->shared->layout;

      H5D_chunk_ud_t noop_ud{};
      std::array<hsize_t, H5O_LAYOUT_NDIMS> scaled{};
      scaled[0] = 2;
      scaled[1] = 0;
      noop_ud.common.layout = &dset_ptr->shared->layout.u.chunk;
      noop_ud.common.storage = &dset_ptr->shared->layout.storage.u.chunk;
      noop_ud.common.scaled = scaled.data();
      noop_ud.chunk_block.offset = HADDR_UNDEF;
      ASSERT_GE(H5D_COPS_BTREE[0].get_addr(&w_idx, &noop_ud), 0);
      ASSERT_TRUE(H5_addr_defined(noop_ud.chunk_block.offset));
      EXPECT_GE(H5D_COPS_BTREE[0].insert(&w_idx, &noop_ud, dset_ptr), 0);

      // Trigger H5B_{insert,find,remove,delete,iterate} failure paths cleanly
      // by pointing idx_addr at offset 0 (file superblock, non-B-tree magic).
      ScopedErrorSilence silence;
      H5O_layout_t bad_addr_layout = dset_ptr->shared->layout;
      bad_addr_layout.storage.u.chunk.idx_addr = 0;
      H5D_chk_idx_info_t bad_idx{};
      bad_idx.f = f_ptr;
      bad_idx.pline = &dset_ptr->shared->dcpl_cache.pline;
      bad_idx.layout = &bad_addr_layout;
      noop_ud.common.storage = &bad_addr_layout.storage.u.chunk;
      EXPECT_LT(H5D_COPS_BTREE[0].insert(&bad_idx, &noop_ud, dset_ptr), 0);
      EXPECT_LT(H5D_COPS_BTREE[0].get_addr(&bad_idx, &noop_ud), 0);
      H5D_chunk_common_ud_t rm_ud = noop_ud.common;
      EXPECT_LT(H5D_COPS_BTREE[0].remove(&bad_idx, &rm_ud), 0);
      auto dummy_cb = [](const H5D_chunk_rec_t*, void*) -> int {
        return H5_ITER_CONT;
      };
      int dummy_ud = 0;
      EXPECT_LT(H5D_COPS_BTREE[0].iterate(&bad_idx, dummy_cb, &dummy_ud), 0);
    }

    // Pad file after /disk_dset so enlarged bogus raw-key reads stay within
    // EOF when testing H5D__btree_decode_key ndims validation.
    const hsize_t pad_dims[1] = {1024};
    ScopedSpace pad_space(H5Screate_simple(1, pad_dims, nullptr));
    ASSERT_TRUE(pad_space.valid());
    ScopedDataset pad_dset(H5Dcreate2(file.get(), "/pad_dset", H5T_NATIVE_INT,
                                      pad_space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                      H5P_DEFAULT));
    ASSERT_TRUE(pad_dset.valid());
    std::vector<int> pad_vals(1024, 7);
    ASSERT_GE(H5Dwrite(pad_dset.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, pad_vals.data()),
              0);
  }

  // Reopen file read-only so H5D__btree_decode_key runs on cold on-disk nodes.
  {
    ScopedFile ro_file(H5Fopen(disk_file.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(ro_file.valid());
    ScopedDataset ro_dset(H5Dopen2(ro_file.get(), "/disk_dset", H5P_DEFAULT));
    ASSERT_TRUE(ro_dset.valid());

    hsize_t nchunks = 0;
    ASSERT_GE(H5Dget_num_chunks(ro_dset.get(), H5S_ALL, &nchunks), 0);
    EXPECT_EQ(nchunks, 18);

    std::array<int, 4> read_vals{};
    ASSERT_GE(ReadHyperslab1D(ro_dset.get(), 15 * kChunkLen, kChunkLen,
                              read_vals.data()),
              0);
    EXPECT_THAT(read_vals, ::testing::ElementsAre(60, 61, 62, 63));

    auto* f_ptr =
        static_cast<H5F_t*>(H5VL_object_verify(ro_file.get(), H5I_FILE));
    auto* dset_ptr =
        static_cast<H5D_t*>(H5VL_object_verify(ro_dset.get(), H5I_DATASET));
    ASSERT_NE(f_ptr, nullptr);
    ASSERT_NE(dset_ptr, nullptr);

    TempFile dump_file("h5dbtree_dump.txt");
    std::FILE* dump_stream = std::fopen(dump_file.c_str(), "w+");
    ASSERT_NE(dump_stream, nullptr);
    auto close_dump = absl::MakeCleanup([&]() { std::fclose(dump_stream); });

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    // Cover H5D__btree_idx_dump via H5D__chunk_dump_index, and H5D_btree_debug
    // + H5D__btree_debug_key.
    std::array<uint32_t, H5O_LAYOUT_NDIMS> dim32{};
    for (unsigned u = 0; u < dset_ptr->shared->layout.u.chunk.ndims; ++u) {
      dim32[u] = static_cast<uint32_t>(dset_ptr->shared->layout.u.chunk.dim[u]);
    }
    ASSERT_GE(H5D__chunk_dump_index(dset_ptr, dump_stream), 0);
    ASSERT_GE(H5D_btree_debug(f_ptr, saved_idx_addr, dump_stream, 0, 12,
                              dset_ptr->shared->layout.u.chunk.ndims,
                              dim32.data()),
              0);
    std::string dump_text = ReadStreamToString(dump_stream);
    EXPECT_THAT(dump_text, HasSubstr("Address:"));
    EXPECT_THAT(dump_text, HasSubstr("Chunk size:"));
    EXPECT_THAT(dump_text, HasSubstr("Logical offset:"));

    // Cover H5D__btree_cmp2 via H5B__verify_structure while all tree nodes
    // loaded by H5Dget_num_chunks are already cached in H5AC.
    auto* shared_bt = static_cast<H5B_shared_t*>(
        H5UC_GET_OBJ(dset_ptr->shared->layout.storage.u.chunk.u.btree.shared));
    ASSERT_NE(shared_bt, nullptr);
    H5D_chunk_common_ud_t verify_ud{};
    verify_ud.layout = &dset_ptr->shared->layout.u.chunk;
    verify_ud.storage = &dset_ptr->shared->layout.storage.u.chunk;
    EXPECT_GE(H5B__verify_structure(f_ptr, saved_idx_addr, shared_bt->type,
                                    &verify_ud),
              0);

    // Cover no-op and query ops on H5D_COPS_BTREE directly.
    H5D_chk_idx_info_t idx_info{};
    idx_info.f = f_ptr;
    idx_info.pline = &dset_ptr->shared->dcpl_cache.pline;
    idx_info.layout = &dset_ptr->shared->layout;

    ASSERT_GE(H5D_COPS_BTREE[0].open(&idx_info), 0);
    bool is_open = false;
    ASSERT_GE(H5D_COPS_BTREE[0].is_open(&idx_info, &is_open), 0);
    EXPECT_TRUE(is_open);
    ASSERT_GE(H5D_COPS_BTREE[0].load_metadata(&idx_info), 0);
    ASSERT_GE(H5D_COPS_BTREE[0].close(&idx_info), 0);

    // Test reset(reset_addr=false/true) and dest(shared==NULL error) on local
    // layout copy.
    H5O_layout_t local_layout = dset_ptr->shared->layout;
    local_layout.storage.u.chunk.u.btree.shared = nullptr;
    H5D_chk_idx_info_t local_idx{};
    local_idx.f = f_ptr;
    local_idx.pline = &dset_ptr->shared->dcpl_cache.pline;
    local_idx.layout = &local_layout;
    ASSERT_GE(H5D_COPS_BTREE[0].init(&local_idx, dset_ptr->shared->space,
                                     dset_ptr->oloc.addr),
              0);
    ASSERT_GE(H5D_COPS_BTREE[0].is_open(&local_idx, &is_open), 0);
    EXPECT_TRUE(is_open);
    ASSERT_GE(H5D_COPS_BTREE[0].dest(&local_idx), 0);

    H5O_storage_chunk_t tmp_storage = dset_ptr->shared->layout.storage.u.chunk;
    ASSERT_GE(H5D_COPS_BTREE[0].reset(&tmp_storage, /*reset_addr=*/false), 0);
    EXPECT_TRUE(H5_addr_defined(tmp_storage.idx_addr));
    EXPECT_EQ(tmp_storage.u.btree.shared, nullptr);

    ASSERT_GE(H5D_COPS_BTREE[0].reset(&tmp_storage, /*reset_addr=*/true), 0);
    EXPECT_FALSE(H5_addr_defined(tmp_storage.idx_addr));

    ScopedErrorSilence silence;
    local_layout.storage.u.chunk.u.btree.shared = nullptr;
    EXPECT_LT(H5D_COPS_BTREE[0].dest(&local_idx), 0);
    EXPECT_GE(H5D_COPS_BTREE[0].is_open(&local_idx, &is_open), 0);
    EXPECT_FALSE(is_open);
  }

  // Trigger H5D__btree_decode_key error paths (ndims > H5O_LAYOUT_NDIMS,
  // dim[u] == 0, and misaligned tmp_offset % dim[u] != 0) via H5B_iterate
  // (which initializes cache_udata.exp_level cleanly) on fresh file opens.
  struct DecodeErrCase {
    unsigned ndims;
    uint32_t dim0;
    uint32_t dim1;
  };
  const std::array<DecodeErrCase, 3> decode_err_cases = {{
      {H5O_LAYOUT_NDIMS + 1, 4, 4},
      {2, 0, 4},
      {2, 3, 4},
  }};
  for (const auto& err_case : decode_err_cases) {
    ScopedFile err_file(H5Fopen(disk_file.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(err_file.valid());
    ScopedDataset err_dset(H5Dopen2(err_file.get(), "/disk_dset", H5P_DEFAULT));
    ASSERT_TRUE(err_dset.valid());
    auto* f_ptr =
        static_cast<H5F_t*>(H5VL_object_verify(err_file.get(), H5I_FILE));
    auto* dset_ptr =
        static_cast<H5D_t*>(H5VL_object_verify(err_dset.get(), H5I_DATASET));
    ASSERT_NE(f_ptr, nullptr);
    ASSERT_NE(dset_ptr, nullptr);

    H5O_layout_t bad_layout = dset_ptr->shared->layout;
    bad_layout.u.chunk.ndims = err_case.ndims;
    bad_layout.u.chunk.dim[0] = err_case.dim0;
    bad_layout.u.chunk.dim[1] = err_case.dim1;
    bad_layout.storage.u.chunk.u.btree.shared = nullptr;
    H5D_chk_idx_info_t bad_idx{};
    bad_idx.f = f_ptr;
    bad_idx.pline = &dset_ptr->shared->dcpl_cache.pline;
    bad_idx.layout = &bad_layout;

    ScopedErrorSilence silence;
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());
    ASSERT_GE(
        H5D_COPS_BTREE[0].init(&bad_idx, dset_ptr->shared->space,
                               dset_ptr->oloc.addr),
        0);
    auto dummy_cb = [](const H5D_chunk_rec_t*, void*) -> int {
      return H5_ITER_CONT;
    };
    int dummy_ud = 0;
    EXPECT_LT(H5D_COPS_BTREE[0].iterate(&bad_idx, dummy_cb, &dummy_ud), 0);
    EXPECT_GE(H5D_COPS_BTREE[0].dest(&bad_idx), 0);
  }
}

}  // namespace
}  // namespace hdf5
