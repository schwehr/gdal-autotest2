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
// Tests native VOL connector dataset callbacks (`H5VL__native_dataset_*`),
// public H5D* APIs (single/multi I/O, H5S_PLIST, H5S_BLOCK, format convert,
// direct chunk I/O, chunk iteration), and internal error branches
// (third_party/hdf5/src/H5VLnative_dataset.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5VLnative_dataset.c

#include <array>
#include <cstdint>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5VLNATIVE_DATASET_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5D_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#pragma push_macro("try")
#undef try
#define try h5_try_param
#include "third_party/hdf5/src/H5Dpkg.h"
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5VLnative_private.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5VLNATIVE_DATASET_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5VLNATIVE_DATASET_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

class ScopedApiContext {
 public:
  ScopedApiContext() { pushed_ = (H5CX_push(&node_) >= 0); }
  ~ScopedApiContext() {
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }
  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

int ChunkIterCountCb(const hsize_t* /*offset*/, unsigned /*filter_mask*/,
                     haddr_t /*addr*/, hsize_t /*size*/, void* op_data) {
  auto* count = static_cast<int*>(op_data);
  (*count)++;
  return H5_ITER_CONT;
}

int ChunkIterFailCb(const hsize_t* /*offset*/, unsigned /*filter_mask*/,
                    haddr_t /*addr*/, hsize_t /*size*/, void* /*op_data*/) {
  return H5_ITER_ERROR;
}

herr_t FailingFlushCb(hid_t /*obj_id*/, void* /*udata*/) { return -1; }

TEST(H5VLNativeDatasetTest, PublicIoMultiPlistBlockAndChunkApis) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V110), 0);

  ScopedFile file(
      H5Fcreate("dset_public.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  // 1. Contiguous dataset in v110 (layout version 4) -> H5Dformat_convert.
  const std::array<hsize_t, 1> dims1d = {8};
  ScopedSpace space1d(H5Screate_simple(1, dims1d.data(), nullptr));
  ASSERT_TRUE(space1d.valid());

  ScopedDataset contig_dset(H5Dcreate2(file.get(), "contig", H5T_NATIVE_INT32,
                                       space1d.get(), H5P_DEFAULT, H5P_DEFAULT,
                                       H5P_DEFAULT));
  ASSERT_TRUE(contig_dset.valid());
  std::array<int32_t, 8> wdata = {10, 20, 30, 40, 50, 60, 70, 80};
  ASSERT_GE(H5Dwrite(contig_dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, wdata.data()),
            0);

  EXPECT_NE(H5Dget_offset(contig_dset.get()), HADDR_UNDEF);
  EXPECT_EQ(H5Dget_storage_size(contig_dset.get()), 8 * sizeof(int32_t));
  H5D_space_status_t status = H5D_SPACE_STATUS_NOT_ALLOCATED;
  EXPECT_GE(H5Dget_space_status(contig_dset.get(), &status), 0);
  EXPECT_EQ(status, H5D_SPACE_STATUS_ALLOCATED);
  EXPECT_GE(H5Dformat_convert(contig_dset.get()), 0);
  // Second call hits version <= H5O_LAYOUT_VERSION_DEFAULT.
  EXPECT_GE(H5Dformat_convert(contig_dset.get()), 0);

  ScopedSpace got_space(H5Dget_space(contig_dset.get()));
  EXPECT_TRUE(got_space.valid());
  ScopedType got_type(H5Dget_type(contig_dset.get()));
  EXPECT_TRUE(got_type.valid());
  ScopedPlist got_dcpl(H5Dget_create_plist(contig_dset.get()));
  EXPECT_TRUE(got_dcpl.valid());
  ScopedPlist got_dapl(H5Dget_access_plist(contig_dset.get()));
  EXPECT_TRUE(got_dapl.valid());
  EXPECT_GE(H5Dflush(contig_dset.get()), 0);
  EXPECT_GE(H5Drefresh(contig_dset.get()), 0);

  // 2. H5S_PLIST file space selection + H5S_BLOCK memory space (non-empty & empty).
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  const std::array<hsize_t, 1> hstart = {2};
  const std::array<hsize_t, 1> hcount = {4};
  ASSERT_GE(H5Pset_dataset_io_hyperslab_selection(
                dxpl.get(), 1, H5S_SELECT_SET, hstart.data(), nullptr,
                hcount.data(), nullptr),
            0);
  std::array<int32_t, 4> sub_rdata{};
  EXPECT_GE(H5Dread(contig_dset.get(), H5T_NATIVE_INT32, H5S_BLOCK, H5S_PLIST,
                    dxpl.get(), sub_rdata.data()),
            0);
  EXPECT_THAT(sub_rdata, ::testing::ElementsAre(30, 40, 50, 60));

  // Empty selection with H5S_BLOCK exercises H5S_create(H5S_NULL) branch.
  ScopedSpace empty_fspace(H5Scopy(space1d.get()));
  ASSERT_TRUE(empty_fspace.valid());
  ASSERT_GE(H5Sselect_none(empty_fspace.get()), 0);
  int32_t dummy = 0;
  EXPECT_GE(H5Dread(contig_dset.get(), H5T_NATIVE_INT32, H5S_BLOCK,
                    empty_fspace.get(), H5P_DEFAULT, &dummy),
            0);

  // 3. Multi-dataset read/write (both same file and different-file error).
  ScopedDataset contig_dset2(H5Dcreate2(file.get(), "contig2", H5T_NATIVE_INT32,
                                        space1d.get(), H5P_DEFAULT, H5P_DEFAULT,
                                        H5P_DEFAULT));
  ASSERT_TRUE(contig_dset2.valid());
  std::array<int32_t, 8> wdata2 = {1, 2, 3, 4, 5, 6, 7, 8};
  std::array<int32_t, 8> rdata1{};
  std::array<int32_t, 8> rdata2{};
  std::array<hid_t, 2> dsets = {contig_dset.get(), contig_dset2.get()};
  std::array<hid_t, 2> mtypes = {H5T_NATIVE_INT32, H5T_NATIVE_INT32};
  std::array<hid_t, 2> mspaces = {H5S_ALL, H5S_ALL};
  std::array<hid_t, 2> fspaces = {H5S_ALL, H5S_ALL};
  std::array<const void*, 2> wbufs = {wdata.data(), wdata2.data()};
  std::array<void*, 2> rbufs = {rdata1.data(), rdata2.data()};
  EXPECT_GE(H5Dwrite_multi(2, dsets.data(), mtypes.data(), mspaces.data(),
                           fspaces.data(), H5P_DEFAULT, wbufs.data()),
            0);
  EXPECT_GE(H5Dread_multi(2, dsets.data(), mtypes.data(), mspaces.data(),
                          fspaces.data(), H5P_DEFAULT, rbufs.data()),
            0);
  EXPECT_EQ(rdata2, wdata2);

  ScopedFile other_file = CreateInMemoryFile("other_multi.h5");
  ASSERT_TRUE(other_file.valid());
  ScopedDataset other_dset(H5Dcreate2(other_file.get(), "other",
                                      H5T_NATIVE_INT32, space1d.get(),
                                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(other_dset.valid());
  {
    ScopedErrorSilence silence;
    std::array<hid_t, 2> diff_dsets = {contig_dset.get(), other_dset.get()};
    EXPECT_LT(H5Dwrite_multi(2, diff_dsets.data(), mtypes.data(),
                             mspaces.data(), fspaces.data(), H5P_DEFAULT,
                             wbufs.data()),
              0);
  }

  // 4. Chunked dataset (fixed-array -> B-tree via H5Dformat_convert,
  //    chunk info / direct chunk read/write / chunk_iter / set_extent).
  const std::array<hsize_t, 1> init_dims = {4};
  const std::array<hsize_t, 1> max_dims = {16};
  const std::array<hsize_t, 1> chunk_dims = {4};
  ScopedSpace ext_space(H5Screate_simple(1, init_dims.data(), max_dims.data()));
  ASSERT_TRUE(ext_space.valid());
  ScopedPlist chunk_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(chunk_dcpl.valid());
  ASSERT_GE(H5Pset_chunk(chunk_dcpl.get(), 1, chunk_dims.data()), 0);

  ScopedDataset chunk_dset(H5Dcreate2(file.get(), "chunked", H5T_NATIVE_INT32,
                                      ext_space.get(), H5P_DEFAULT,
                                      chunk_dcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(chunk_dset.valid());

  const std::array<hsize_t, 1> new_extent = {8};
  EXPECT_GE(H5Dset_extent(chunk_dset.get(), new_extent.data()), 0);

  const std::array<int32_t, 4> chunk0 = {11, 22, 33, 44};
  const std::array<hsize_t, 1> off0 = {0};
  ASSERT_GE(H5Dwrite_chunk(chunk_dset.get(), H5P_DEFAULT, 0, off0.data(),
                           sizeof(chunk0), chunk0.data()),
            0);

  std::array<int32_t, 4> read_chunk0{};
  size_t read_chunk_sz = sizeof(read_chunk0);
  uint32_t filter_mask_u32 = 0;
  EXPECT_GE(H5Dread_chunk2(chunk_dset.get(), H5P_DEFAULT, off0.data(),
                           &filter_mask_u32, read_chunk0.data(),
                           &read_chunk_sz),
            0);
  EXPECT_EQ(read_chunk0, chunk0);

  H5D_chunk_index_t idx_type = H5D_CHUNK_IDX_NONE;
  EXPECT_GE(H5Dget_chunk_index_type(chunk_dset.get(), &idx_type), 0);
  EXPECT_NE(idx_type, H5D_CHUNK_IDX_BTREE);

  hsize_t chk_bytes = 0;
  EXPECT_GE(H5Dget_chunk_storage_size(chunk_dset.get(), off0.data(), &chk_bytes),
            0);
  EXPECT_EQ(chk_bytes, sizeof(chunk0));

  hsize_t nchunks = 0;
  ScopedSpace full_chk_space(H5Dget_space(chunk_dset.get()));
  ASSERT_TRUE(full_chk_space.valid());
  EXPECT_GE(H5Dget_num_chunks(chunk_dset.get(), full_chk_space.get(), &nchunks),
            0);
  EXPECT_EQ(nchunks, 1u);
  nchunks = 0;
  EXPECT_GE(H5Dget_num_chunks(chunk_dset.get(), H5S_ALL, &nchunks), 0);
  EXPECT_EQ(nchunks, 1u);

  hsize_t chk_off = 99;
  unsigned fmask = 99;
  haddr_t chk_addr = HADDR_UNDEF;
  hsize_t chk_sz = 0;
  EXPECT_GE(H5Dget_chunk_info(chunk_dset.get(), H5S_ALL, 0, &chk_off, &fmask,
                              &chk_addr, &chk_sz),
            0);
  EXPECT_EQ(chk_off, 0u);
  EXPECT_GE(H5Dget_chunk_info_by_coord(chunk_dset.get(), off0.data(), &fmask,
                                       &chk_addr, &chk_sz),
            0);

  int iter_count = 0;
  EXPECT_GE(
      H5Dchunk_iter(chunk_dset.get(), H5P_DEFAULT, ChunkIterCountCb, &iter_count),
      0);
  EXPECT_EQ(iter_count, 1);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Dchunk_iter(chunk_dset.get(), H5P_DEFAULT, ChunkIterFailCb, nullptr),
        0);
  }

  // Convert fixed-array chunk index -> v1 B-tree, then verify no-op on 2nd call.
  EXPECT_GE(H5Dformat_convert(chunk_dset.get()), 0);
  EXPECT_GE(H5Dformat_convert(chunk_dset.get()), 0);

  // 5. Anonymous dataset, open existing, virtual dataset format_convert, vlen buf size.
  ScopedDataset anon(H5Dcreate_anon(file.get(), H5T_NATIVE_INT32, space1d.get(),
                                    H5P_DEFAULT, H5P_DEFAULT));
  EXPECT_TRUE(anon.valid());
  ScopedDataset opened(H5Dopen2(file.get(), "contig", H5P_DEFAULT));
  EXPECT_TRUE(opened.valid());

  ScopedPlist vds_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(vds_dcpl.valid());
  ASSERT_GE(H5Pset_virtual(vds_dcpl.get(), space1d.get(), ".", "contig",
                           space1d.get()),
            0);
  ScopedDataset vds(H5Dcreate2(file.get(), "vds", H5T_NATIVE_INT32,
                               space1d.get(), H5P_DEFAULT, vds_dcpl.get(),
                               H5P_DEFAULT));
  ASSERT_TRUE(vds.valid());
  EXPECT_GE(H5Dformat_convert(vds.get()), 0);

  ScopedType vl_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(vl_str.valid());
  ASSERT_GE(H5Tset_size(vl_str.get(), H5T_VARIABLE), 0);
  const std::array<hsize_t, 1> vl_dims = {1};
  ScopedSpace vl_space(H5Screate_simple(1, vl_dims.data(), nullptr));
  ASSERT_TRUE(vl_space.valid());
  ScopedDataset vl_dset(H5Dcreate2(file.get(), "vl_str_ds", vl_str.get(),
                                   vl_space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT));
  ASSERT_TRUE(vl_dset.valid());
  const char* s = "hello_hdf5";
  ASSERT_GE(H5Dwrite(vl_dset.get(), vl_str.get(), H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, &s),
            0);
  hsize_t vl_buf_size = 0;
  EXPECT_GE(H5Dvlen_get_buf_size(vl_dset.get(), vl_str.get(), vl_space.get(),
                                 &vl_buf_size),
            0);
  EXPECT_GT(vl_buf_size, 0u);
}

TEST(H5VLNativeDatasetTest, DirectCallbacksAndDefensiveBranches) {
  TempFile tmp_file("dset_direct.h5");
  ScopedFile file(
      H5Fcreate(tmp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(file.valid());
  void* file_obj = H5VL_object(file.get());
  ASSERT_NE(file_obj, nullptr);

  const std::array<hsize_t, 1> dims = {4};
  ScopedSpace space(H5Screate_simple(1, dims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  ScopedDataset pub_contig(H5Dcreate2(file.get(), "pub_contig",
                                      H5T_NATIVE_INT32, space.get(),
                                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(pub_contig.valid());
  void* contig = H5VL_object(pub_contig.get());
  ASSERT_NE(contig, nullptr);

  H5VL_loc_params_t self_loc{};
  self_loc.type = H5VL_OBJECT_BY_SELF;
  self_loc.obj_type = H5I_FILE;

  ScopedApiContext api_ctx;
  ScopedErrorSilence silence;

  // 1. Direct create & close (both named & anon) + create/open error branches.
  void* direct_d = H5VL__native_dataset_create(
      file_obj, &self_loc, "direct_d", H5P_LINK_CREATE_DEFAULT,
      H5T_NATIVE_INT32, space.get(), H5P_DATASET_CREATE_DEFAULT,
      H5P_DATASET_ACCESS_DEFAULT, H5P_DATASET_XFER_DEFAULT, nullptr);
  ASSERT_NE(direct_d, nullptr);
  EXPECT_GE(H5VL__native_dataset_close(direct_d, H5P_DATASET_XFER_DEFAULT, nullptr),
            0);

  void* direct_anon = H5VL__native_dataset_create(
      file_obj, &self_loc, nullptr, H5P_LINK_CREATE_DEFAULT, H5T_NATIVE_INT32,
      space.get(), H5P_DATASET_CREATE_DEFAULT, H5P_DATASET_ACCESS_DEFAULT,
      H5P_DATASET_XFER_DEFAULT, nullptr);
  ASSERT_NE(direct_anon, nullptr);
  EXPECT_GE(
      H5VL__native_dataset_close(direct_anon, H5P_DATASET_XFER_DEFAULT, nullptr),
      0);

  H5VL_loc_params_t bad_loc{};
  bad_loc.obj_type = H5I_BADID;
  EXPECT_EQ(H5VL__native_dataset_create(
                file_obj, &bad_loc, "bad", H5P_LINK_CREATE_DEFAULT,
                H5T_NATIVE_INT32, space.get(), H5P_DATASET_CREATE_DEFAULT,
                H5P_DATASET_ACCESS_DEFAULT, H5P_DATASET_XFER_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VL__native_dataset_create(
                file_obj, &self_loc, "bad_t", H5P_LINK_CREATE_DEFAULT,
                H5I_INVALID_HID, space.get(), H5P_DATASET_CREATE_DEFAULT,
                H5P_DATASET_ACCESS_DEFAULT, H5P_DATASET_XFER_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VL__native_dataset_create(
                file_obj, &self_loc, "bad_s", H5P_LINK_CREATE_DEFAULT,
                H5T_NATIVE_INT32, H5I_INVALID_HID, H5P_DATASET_CREATE_DEFAULT,
                H5P_DATASET_ACCESS_DEFAULT, H5P_DATASET_XFER_DEFAULT, nullptr),
            nullptr);
  // Duplicate named create fails H5D__create_named.
  EXPECT_EQ(H5VL__native_dataset_create(
                file_obj, &self_loc, "direct_d", H5P_LINK_CREATE_DEFAULT,
                H5T_NATIVE_INT32, space.get(), H5P_DATASET_CREATE_DEFAULT,
                H5P_DATASET_ACCESS_DEFAULT, H5P_DATASET_XFER_DEFAULT, nullptr),
            nullptr);

  EXPECT_EQ(H5VL__native_dataset_open(file_obj, &bad_loc, "direct_d",
                                      H5P_DATASET_ACCESS_DEFAULT,
                                      H5P_DATASET_XFER_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VL__native_dataset_open(file_obj, &self_loc, "missing",
                                      H5P_DATASET_ACCESS_DEFAULT,
                                      H5P_DATASET_XFER_DEFAULT, nullptr),
            nullptr);

  // 2. Read/write io_setup error branches.
  int32_t buf[4] = {};
  void* objs[1] = {contig};
  hid_t types[1] = {H5T_NATIVE_INT32};
  hid_t mspaces[1] = {H5S_ALL};
  hid_t fspaces[1] = {H5S_ALL};
  void* rptrs[1] = {buf};
  const void* wptrs[1] = {buf};

  // Invalid mem_type_id
  types[0] = H5I_INVALID_HID;
  EXPECT_LT(H5VL__native_dataset_read(1, objs, types, mspaces, fspaces,
                                      H5P_DATASET_XFER_DEFAULT, rptrs, nullptr),
            0);
  EXPECT_LT(H5VL__native_dataset_write(1, objs, types, mspaces, fspaces,
                                       H5P_DATASET_XFER_DEFAULT, wptrs, nullptr),
            0);
  types[0] = H5T_NATIVE_INT32;

  // file_space_id == H5S_BLOCK (forbidden)
  fspaces[0] = H5S_BLOCK;
  EXPECT_LT(H5VL__native_dataset_read(1, objs, types, mspaces, fspaces,
                                      H5P_DATASET_XFER_DEFAULT, rptrs, nullptr),
            0);

  // file_space_id invalid hid_t (avoid calling H5S_PLIST with an unset DXPL,
  // which leaves space == NULL in H5S_select_copy and triggers UBSan SIGILL).
  fspaces[0] = H5T_NATIVE_INT32;
  EXPECT_LT(H5VL__native_dataset_read(1, objs, types, mspaces, fspaces,
                                      H5P_DATASET_XFER_DEFAULT, rptrs, nullptr),
            0);
  fspaces[0] = H5S_ALL;

  // mem_space_id == H5S_PLIST (forbidden) & invalid hid_t
  mspaces[0] = H5S_PLIST;
  EXPECT_LT(H5VL__native_dataset_read(1, objs, types, mspaces, fspaces,
                                      H5P_DATASET_XFER_DEFAULT, rptrs, nullptr),
            0);
  mspaces[0] = H5T_NATIVE_INT32;
  EXPECT_LT(H5VL__native_dataset_read(1, objs, types, mspaces, fspaces,
                                      H5P_DATASET_XFER_DEFAULT, rptrs, nullptr),
            0);

  // Out-of-bounds hyperslab selections trigger H5S_SELECT_VALID failure.
  ScopedSpace oob_space(H5Screate_simple(1, dims.data(), nullptr));
  const std::array<hsize_t, 1> oob_start = {10};
  const std::array<hsize_t, 1> oob_count = {1};
  ASSERT_GE(H5Sselect_hyperslab(oob_space.get(), H5S_SELECT_SET,
                                oob_start.data(), nullptr, oob_count.data(),
                                nullptr),
            0);
  mspaces[0] = H5S_ALL;
  fspaces[0] = oob_space.get();
  EXPECT_LT(H5VL__native_dataset_read(1, objs, types, mspaces, fspaces,
                                      H5P_DATASET_XFER_DEFAULT, rptrs, nullptr),
            0);
  fspaces[0] = H5S_ALL;
  mspaces[0] = oob_space.get();
  EXPECT_LT(H5VL__native_dataset_read(1, objs, types, mspaces, fspaces,
                                      H5P_DATASET_XFER_DEFAULT, rptrs, nullptr),
            0);
  mspaces[0] = H5S_ALL;

  // 3. Get / Specific / Optional error branches.
  H5VL_dataset_get_args_t gargs{};
  gargs.op_type = static_cast<H5VL_dataset_get_t>(999);
  EXPECT_LT(H5VL__native_dataset_get(contig, &gargs, H5P_DATASET_XFER_DEFAULT,
                                     nullptr),
            0);

  H5VL_dataset_specific_args_t sargs{};
  const std::array<hsize_t, 1> bad_ext = {8};
  sargs.op_type = H5VL_DATASET_SET_EXTENT;
  sargs.args.set_extent.size = bad_ext.data();
  EXPECT_LT(H5VL__native_dataset_specific(contig, &sargs,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
            0);
  sargs = {};
  sargs.op_type = H5VL_DATASET_FLUSH;
  sargs.args.flush.dset_id = H5I_INVALID_HID;
  EXPECT_GE(H5VL__native_dataset_specific(contig, &sargs,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
            0);
  sargs = {};
  sargs.op_type = H5VL_DATASET_REFRESH;
  sargs.args.refresh.dset_id = H5I_INVALID_HID;
  EXPECT_GE(H5VL__native_dataset_specific(contig, &sargs,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
            0);
  sargs = {};
  sargs.op_type = static_cast<H5VL_dataset_specific_t>(999);
  EXPECT_LT(H5VL__native_dataset_specific(contig, &sargs,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
            0);

  // Trigger H5D__flush failure via failing object flush callback, and
  // H5D__refresh failure via H5I_INVALID_HID on a read-only file.
  {
    ScopedPlist cb_fapl = MakeCoreFapl();
    ASSERT_TRUE(cb_fapl.valid());
    ASSERT_GE(H5Pset_object_flush_cb(cb_fapl.get(), FailingFlushCb, nullptr),
              0);
    ScopedFile cb_file(H5Fcreate("flush_cb_fail.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                                 cb_fapl.get()));
    ASSERT_TRUE(cb_file.valid());
    ScopedDataset cb_ds(H5Dcreate2(cb_file.get(), "ds", H5T_NATIVE_INT32,
                                   space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT));
    ASSERT_TRUE(cb_ds.valid());
    EXPECT_LT(H5Dflush(cb_ds.get()), 0);
  }
  {
    TempFile ro_tmp("dset_ro_refresh.h5");
    {
      ScopedFile wfile(
          H5Fcreate(ro_tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(wfile.valid());
      ScopedDataset wds(H5Dcreate2(wfile.get(), "ds", H5T_NATIVE_INT32,
                                   space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT));
      ASSERT_TRUE(wds.valid());
    }
    ScopedFile ro_file(
        H5Fopen(ro_tmp.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(ro_file.valid());
    ScopedDataset ro_ds(H5Dopen2(ro_file.get(), "ds", H5P_DEFAULT));
    ASSERT_TRUE(ro_ds.valid());
    void* ro_contig = H5VL_object(ro_ds.get());
    ASSERT_NE(ro_contig, nullptr);
    sargs = {};
    sargs.op_type = H5VL_DATASET_REFRESH;
    sargs.args.refresh.dset_id = H5I_INVALID_HID;
    EXPECT_LT(H5VL__native_dataset_specific(ro_contig, &sargs,
                                            H5P_DATASET_XFER_DEFAULT, nullptr),
              0);
  }

  // Optional non-chunked & invalid-op errors + temporary invalid layout type.
  H5VL_optional_args_t oargs{};
  H5VL_native_dataset_optional_args_t dopt{};
  oargs.args = &dopt;

  H5D_chunk_index_t idx_out = H5D_CHUNK_IDX_NONE;
  oargs.op_type = H5VL_NATIVE_DATASET_GET_CHUNK_INDEX_TYPE;
  dopt.get_chunk_idx_type.idx_type = &idx_out;
  EXPECT_LT(H5VL__native_dataset_optional(contig, &oargs,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
            0);

  hsize_t dummy_sz = 0;
  const std::array<hsize_t, 1> off0 = {0};
  oargs.op_type = H5VL_NATIVE_DATASET_GET_CHUNK_STORAGE_SIZE;
  dopt.get_chunk_storage_size.offset = off0.data();
  dopt.get_chunk_storage_size.size = &dummy_sz;
  EXPECT_LT(H5VL__native_dataset_optional(contig, &oargs,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
            0);

  oargs.op_type = H5VL_NATIVE_DATASET_GET_NUM_CHUNKS;
  dopt.get_num_chunks.space_id = H5S_ALL;
  dopt.get_num_chunks.nchunks = &dummy_sz;
  EXPECT_LT(H5VL__native_dataset_optional(contig, &oargs,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
            0);
  dopt.get_num_chunks.space_id = H5I_INVALID_HID;
  EXPECT_LT(H5VL__native_dataset_optional(contig, &oargs,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
            0);

  oargs.op_type = H5VL_NATIVE_DATASET_GET_CHUNK_INFO_BY_IDX;
  dopt.get_chunk_info_by_idx.space_id = H5S_ALL;
  dopt.get_chunk_info_by_idx.chk_index = 0;
  EXPECT_LT(H5VL__native_dataset_optional(contig, &oargs,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
            0);
  dopt.get_chunk_info_by_idx.space_id = H5I_INVALID_HID;
  EXPECT_LT(H5VL__native_dataset_optional(contig, &oargs,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
            0);

  oargs.op_type = H5VL_NATIVE_DATASET_GET_CHUNK_INFO_BY_COORD;
  dopt.get_chunk_info_by_coord.offset = off0.data();
  EXPECT_LT(H5VL__native_dataset_optional(contig, &oargs,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
            0);

  size_t cbuf_sz = sizeof(buf);
  oargs.op_type = H5VL_NATIVE_DATASET_CHUNK_READ;
  dopt.chunk_read.offset = off0.data();
  dopt.chunk_read.filters = 0;
  dopt.chunk_read.buf = buf;
  dopt.chunk_read.buf_size = &cbuf_sz;
  EXPECT_LT(H5VL__native_dataset_optional(contig, &oargs,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
            0);

  oargs.op_type = H5VL_NATIVE_DATASET_CHUNK_WRITE;
  dopt.chunk_write.offset = off0.data();
  dopt.chunk_write.filters = 0;
  dopt.chunk_write.size = sizeof(buf);
  dopt.chunk_write.buf = buf;
  EXPECT_LT(H5VL__native_dataset_optional(contig, &oargs,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
            0);

  oargs.op_type = H5VL_NATIVE_DATASET_GET_VLEN_BUF_SIZE;
  dopt.get_vlen_buf_size.type_id = H5I_INVALID_HID;
  dopt.get_vlen_buf_size.space_id = space.get();
  dopt.get_vlen_buf_size.size = &dummy_sz;
  EXPECT_LT(H5VL__native_dataset_optional(contig, &oargs,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
            0);

  oargs.op_type = H5VL_NATIVE_DATASET_CHUNK_ITER;
  dopt.chunk_iter.op = ChunkIterCountCb;
  dopt.chunk_iter.op_data = nullptr;
  EXPECT_LT(H5VL__native_dataset_optional(contig, &oargs,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
            0);

  oargs.op_type = 999;
  EXPECT_LT(H5VL__native_dataset_optional(contig, &oargs,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
            0);

  // 4. Chunked & read-only & conversion failure branches via public APIs.
  {
    ScopedType opq_type(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(opq_type.valid());
    ScopedDataset opq_ds(H5Dcreate2(file.get(), "opaque_ds", opq_type.get(),
                                    space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                    H5P_DEFAULT));
    ASSERT_TRUE(opq_ds.valid());
    EXPECT_LT(H5Dread(opq_ds.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, buf),
              0);
  }
  {
    const std::array<hsize_t, 1> ext_dims = {8};
    const std::array<hsize_t, 1> chk_dims = {4};
    ScopedSpace chk_space(H5Screate_simple(1, ext_dims.data(), nullptr));
    ASSERT_TRUE(chk_space.valid());
    ScopedPlist chk_dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(chk_dcpl.valid());
    ASSERT_GE(H5Pset_chunk(chk_dcpl.get(), 1, chk_dims.data()), 0);
    ScopedPlist chk_fapl = MakeCoreFapl();
    ASSERT_TRUE(chk_fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(chk_fapl.get(), H5F_LIBVER_V110,
                                   H5F_LIBVER_V110),
              0);
    ScopedFile chk_file(H5Fcreate("dset_chk_err.h5", H5F_ACC_TRUNC,
                                  H5P_DEFAULT, chk_fapl.get()));
    ASSERT_TRUE(chk_file.valid());
    ScopedDataset chk_ds(H5Dcreate2(chk_file.get(), "chk", H5T_NATIVE_INT32,
                                    chk_space.get(), H5P_DEFAULT,
                                    chk_dcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(chk_ds.valid());
    const std::array<hsize_t, 1> off4 = {4};
    const std::array<hsize_t, 1> unaligned = {1};
    // Write chunk 0 so index exists; chunk {4} stays unallocated.
    ASSERT_GE(H5Dwrite_chunk(chk_ds.get(), H5P_DEFAULT, 0, off0.data(),
                             sizeof(buf), buf),
              0);
    hsize_t sz_out = 0;
    EXPECT_LT(H5Dget_chunk_storage_size(chk_ds.get(), off4.data(), &sz_out),
              0);
    uint32_t fmask = 0;
    size_t rsz = sizeof(buf);
    EXPECT_LT(H5Dread_chunk2(chk_ds.get(), H5P_DEFAULT, unaligned.data(),
                             &fmask, buf, &rsz),
              0);
    EXPECT_LT(
        H5Dread_chunk2(chk_ds.get(), H5P_DEFAULT, off4.data(), &fmask, buf, &rsz),
        0);
    EXPECT_LT(H5Dwrite_chunk(chk_ds.get(), H5P_DEFAULT, 0, unaligned.data(),
                             sizeof(buf), buf),
              0);
  }
  {
    TempFile ro_err_tmp("dset_ro_errs.h5");
    {
      const std::array<hsize_t, 1> ext_dims = {8};
      const std::array<hsize_t, 1> chk_dims = {4};
      ScopedSpace chk_space(H5Screate_simple(1, ext_dims.data(), nullptr));
      ASSERT_TRUE(chk_space.valid());
      ScopedPlist chk_dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(chk_dcpl.valid());
      ASSERT_GE(H5Pset_chunk(chk_dcpl.get(), 1, chk_dims.data()), 0);
      ScopedPlist chk_fapl(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_TRUE(chk_fapl.valid());
      ASSERT_GE(H5Pset_libver_bounds(chk_fapl.get(), H5F_LIBVER_V110,
                                     H5F_LIBVER_V110),
                0);
      ScopedFile wfile(H5Fcreate(ro_err_tmp.c_str(), H5F_ACC_TRUNC,
                                 H5P_DEFAULT, chk_fapl.get()));
      ASSERT_TRUE(wfile.valid());
      ScopedDataset wchk(H5Dcreate2(wfile.get(), "chk", H5T_NATIVE_INT32,
                                    chk_space.get(), H5P_DEFAULT,
                                    chk_dcpl.get(), H5P_DEFAULT));
      ASSERT_TRUE(wchk.valid());
      ASSERT_GE(H5Dwrite_chunk(wchk.get(), H5P_DEFAULT, 0, off0.data(),
                               sizeof(buf), buf),
                0);
    }
    ScopedFile ro_f(
        H5Fopen(ro_err_tmp.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(ro_f.valid());
    EXPECT_LT(H5Dcreate_anon(ro_f.get(), H5T_NATIVE_INT32, space.get(),
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
    ScopedDataset ro_chk(H5Dopen2(ro_f.get(), "chk", H5P_DEFAULT));
    ASSERT_TRUE(ro_chk.valid());
    EXPECT_LT(H5Dwrite_chunk(ro_chk.get(), H5P_DEFAULT, 0, off0.data(),
                             sizeof(buf), buf),
              0);
    EXPECT_LT(H5Dwrite(ro_chk.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, buf),
              0);
    EXPECT_LT(H5Dformat_convert(ro_chk.get()), 0);
  }

  // 5. Synthetic structs for orphan oloc.file, invalid layout type, and
  // failing chunk index iteration guards.
  H5D_t orphan_d{};
  orphan_d.oloc.file = nullptr;
  std::array<void*, 2> two_objs = {contig, &orphan_d};
  std::array<hid_t, 2> two_types = {H5T_NATIVE_INT32, H5T_NATIVE_INT32};
  std::array<hid_t, 2> two_spaces = {H5S_ALL, H5S_ALL};
  std::array<void*, 2> two_bufs = {buf, buf};
  EXPECT_LT(H5VL__native_dataset_read(2, two_objs.data(), two_types.data(),
                                      two_spaces.data(), two_spaces.data(),
                                      H5P_DATASET_XFER_DEFAULT, two_bufs.data(),
                                      nullptr),
            0);

  oargs.op_type = H5VL_NATIVE_DATASET_CHUNK_READ;
  dopt.chunk_read.offset = off0.data();
  dopt.chunk_read.filters = 0;
  dopt.chunk_read.buf = buf;
  dopt.chunk_read.buf_size = &cbuf_sz;
  EXPECT_LT(H5VL__native_dataset_optional(&orphan_d, &oargs,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
            0);

  oargs.op_type = H5VL_NATIVE_DATASET_CHUNK_WRITE;
  dopt.chunk_write.offset = off0.data();
  dopt.chunk_write.filters = 0;
  dopt.chunk_write.size = sizeof(buf);
  dopt.chunk_write.buf = buf;
  EXPECT_LT(H5VL__native_dataset_optional(&orphan_d, &oargs,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
            0);

  H5D_shared_t fake_sh{};
  H5D_t fake_d{};
  fake_d.shared = &fake_sh;
  fake_sh.layout.type = H5D_NLAYOUTS;
  hsize_t fake_storage_sz = 0;
  gargs = {};
  gargs.op_type = H5VL_DATASET_GET_STORAGE_SIZE;
  gargs.args.get_storage_size.storage_size = &fake_storage_sz;
  EXPECT_LT(
      H5VL__native_dataset_get(&fake_d, &gargs, H5P_DATASET_XFER_DEFAULT, nullptr),
      0);
  oargs.op_type = H5VL_NATIVE_DATASET_FORMAT_CONVERT;
  EXPECT_LT(H5VL__native_dataset_optional(&fake_d, &oargs,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
            0);

  H5D_chunk_ops_t fail_cops{};
  fail_cops.iterate = [](const H5D_chk_idx_info_t*, H5D_chunk_cb_func_t,
                         void*) -> int { return -1; };
  fake_sh.layout.type = H5D_CHUNKED;
  fake_sh.ndims = 1;
  fake_sh.layout.u.chunk.dim[0] = 4;
  fake_sh.layout.storage.u.chunk.idx_addr = 1024;
  fake_sh.layout.storage.u.chunk.ops = &fail_cops;
  fake_sh.space = static_cast<H5S_t*>(H5I_object(space.get()));
  ASSERT_NE(fake_sh.space, nullptr);

  oargs.op_type = H5VL_NATIVE_DATASET_GET_NUM_CHUNKS;
  dopt.get_num_chunks.space_id = H5S_ALL;
  dopt.get_num_chunks.nchunks = &dummy_sz;
  EXPECT_LT(H5VL__native_dataset_optional(&fake_d, &oargs,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
            0);

  oargs.op_type = H5VL_NATIVE_DATASET_GET_CHUNK_INFO_BY_IDX;
  dopt.get_chunk_info_by_idx.space_id = H5S_ALL;
  dopt.get_chunk_info_by_idx.chk_index = 0;
  dopt.get_chunk_info_by_idx.offset = nullptr;
  dopt.get_chunk_info_by_idx.filter_mask = nullptr;
  dopt.get_chunk_info_by_idx.addr = nullptr;
  dopt.get_chunk_info_by_idx.size = nullptr;
  EXPECT_LT(H5VL__native_dataset_optional(&fake_d, &oargs,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
            0);

  oargs.op_type = H5VL_NATIVE_DATASET_GET_CHUNK_INFO_BY_COORD;
  dopt.get_chunk_info_by_coord.offset = off0.data();
  dopt.get_chunk_info_by_coord.filter_mask = nullptr;
  dopt.get_chunk_info_by_coord.addr = nullptr;
  dopt.get_chunk_info_by_coord.size = nullptr;
  EXPECT_LT(H5VL__native_dataset_optional(&fake_d, &oargs,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
            0);
}

}  // namespace
}  // namespace hdf5
