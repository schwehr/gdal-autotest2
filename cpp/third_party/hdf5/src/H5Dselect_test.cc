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
// Tests direct dataspace selection I/O (`H5D__select_read`, `H5D__select_write`,
// and `H5D_select_io_mem`), including single-element paths, multi-sequence
// vector batching (>1024 sequences), custom vector sizes, and error paths
// (H5Dselect.c).
//
// Google3 build caveats:
//   - Compiles `:hdf5` with `-DNDEBUG` (internal structs omit debug fields).
//   - Parallel MPI (H5_HAVE_PARALLEL) is disabled, so `H5D_select_io_mem`
//     (used only by `H5Dmpio.c`) is exercised directly via internal headers.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Dselect.c

#include "third_party/hdf5/src/hdf5.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields in internal
// metadata structures. Define NDEBUG around both the prelude and internal
// headers so internal struct layouts match the compiled library ABI.
#ifndef NDEBUG
#define H5DSELECT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#pragma push_macro("try")
#undef try
#define try hdf5_try_param
#define H5D_FRIEND
#define H5F_FRIEND
#define H5S_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Dpkg.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Spkg.h"
#include "third_party/hdf5/src/H5Sprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#undef try
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5DSELECT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5DSELECT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

// Pushes an HDF5 API context with an optional DXPL for direct internal calls.
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

struct FailVfdEnv {
  bool fail_read = false;
  bool fail_write = false;
  unsigned long next_fileno = 1;
};

FailVfdEnv* g_sel_vfd_env = nullptr;

struct FailVfdFile {
  H5FD_t pub{};
  std::vector<uint8_t> data;
  haddr_t eoa = 0;
};

H5FD_t* SelOpen(const char* name, unsigned /*flags*/, hid_t /*fapl_id*/,
                haddr_t maxaddr) {
  if (g_sel_vfd_env == nullptr || name == nullptr || *name == '\0') {
    return nullptr;
  }
  auto* file = new FailVfdFile();
  file->pub.fileno = g_sel_vfd_env->next_fileno++;
  file->pub.maxaddr = maxaddr;
  return reinterpret_cast<H5FD_t*>(file);
}

herr_t SelClose(H5FD_t* _file) {
  delete reinterpret_cast<FailVfdFile*>(_file);
  return 0;
}

int SelCmp(const H5FD_t* f1, const H5FD_t* f2) {
  if (f1->fileno < f2->fileno) return -1;
  if (f1->fileno > f2->fileno) return 1;
  return 0;
}

herr_t SelQuery(const H5FD_t* /*file*/, unsigned long* flags) {
  *flags = 0;
  return 0;
}

haddr_t SelGetEoa(const H5FD_t* _file, H5FD_mem_t /*type*/) {
  return reinterpret_cast<const FailVfdFile*>(_file)->eoa;
}

herr_t SelSetEoa(H5FD_t* _file, H5FD_mem_t /*type*/, haddr_t addr) {
  auto* file = reinterpret_cast<FailVfdFile*>(_file);
  file->eoa = addr;
  if (file->data.size() < static_cast<size_t>(addr)) {
    file->data.resize(static_cast<size_t>(addr), 0);
  }
  return 0;
}

haddr_t SelGetEof(const H5FD_t* _file, H5FD_mem_t /*type*/) {
  return static_cast<haddr_t>(
      reinterpret_cast<const FailVfdFile*>(_file)->data.size());
}

herr_t SelRead(H5FD_t* _file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
               haddr_t addr, size_t size, void* buf) {
  if (g_sel_vfd_env != nullptr && g_sel_vfd_env->fail_read) {
    return -1;
  }
  const auto* file = reinterpret_cast<const FailVfdFile*>(_file);
  std::memset(buf, 0, size);
  const size_t offset = static_cast<size_t>(addr);
  if (offset < file->data.size()) {
    const size_t avail = std::min(size, file->data.size() - offset);
    std::memcpy(buf, file->data.data() + offset, avail);
  }
  return 0;
}

herr_t SelWrite(H5FD_t* _file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                haddr_t addr, size_t size, const void* buf) {
  if (g_sel_vfd_env != nullptr && g_sel_vfd_env->fail_write) {
    return -1;
  }
  auto* file = reinterpret_cast<FailVfdFile*>(_file);
  const size_t offset = static_cast<size_t>(addr);
  if (offset + size > file->data.size()) {
    file->data.resize(offset + size, 0);
  }
  std::memcpy(file->data.data() + offset, buf, size);
  return 0;
}

H5FD_class_t MakeSelVfdClass() {
  H5FD_class_t cls{};
  cls.version = H5FD_CLASS_VERSION;
  cls.value = static_cast<H5FD_class_value_t>(332);
  cls.name = "test_dselect_fail_vfd";
  cls.maxaddr = (static_cast<haddr_t>(1) << 28) - 1;
  cls.fc_degree = H5F_CLOSE_WEAK;
  cls.open = SelOpen;
  cls.close = SelClose;
  cls.cmp = SelCmp;
  cls.query = SelQuery;
  cls.get_eoa = SelGetEoa;
  cls.set_eoa = SelSetEoa;
  cls.get_eof = SelGetEof;
  cls.read = SelRead;
  cls.write = SelWrite;
  const H5FD_mem_t fl_map[H5FD_MEM_NTYPES] = H5FD_FLMAP_SINGLE;
  std::memcpy(cls.fl_map, fl_map, sizeof(cls.fl_map));
  return cls;
}

H5S_t* UnwrapSpace(hid_t space_id) {
  return static_cast<H5S_t*>(H5I_object_verify(space_id, H5I_DATASPACE));
}

herr_t FailIterInit(H5S_t* /*space*/, H5S_sel_iter_t* sel_iter) {
  static const H5S_sel_iter_class_t kDummyIterClass = {};
  sel_iter->type = &kDummyIterClass;
  return -1;
}

herr_t FailGetSeqList(H5S_sel_iter_t* /*iter*/, size_t /*maxseq*/,
                      size_t /*maxbytes*/, size_t* /*nseq*/, size_t* /*nbytes*/,
                      hsize_t* /*off*/, size_t* /*len*/) {
  return -1;
}

herr_t FailingSeqListIterInit(H5S_t* space, H5S_sel_iter_t* sel_iter) {
  if (H5S_sel_all[0].iter_init(space, sel_iter) < 0) {
    return -1;
  }
  static const H5S_sel_iter_class_t kFailSeqIterClass = [&]() {
    H5S_sel_iter_class_t cls = *sel_iter->type;
    cls.iter_get_seq_list = FailGetSeqList;
    return cls;
  }();
  sel_iter->type = &kFailSeqIterClass;
  return 0;
}

void MakeFailInitSpace(const H5S_t* proto, H5S_t* out_space,
                       H5S_select_class_t* out_cls) {
  *out_space = *proto;
  *out_cls = *proto->select.type;
  out_cls->iter_init = FailIterInit;
  out_space->select.type = out_cls;
}

void MakeFailSeqSpace(const H5S_t* all_proto, H5S_t* out_space,
                      H5S_select_class_t* out_cls) {
  *out_space = *all_proto;
  *out_cls = H5S_sel_all[0];
  out_cls->iter_init = FailingSeqListIterInit;
  out_space->select.type = out_cls;
}

TEST(H5DselectTest, SingleAndMultiSequenceIoWithSelectionIoDisabled) {
  ScopedFile file = CreateInMemoryFile("select_io.h5");
  ASSERT_TRUE(file.valid());

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ASSERT_GE(H5Pset_selection_io(dxpl, H5D_SELECTION_IO_MODE_OFF), 0);

  // 1. Single-element path (nelmts == 1) on scalar and 1-element selections.
  {
    ScopedSpace scalar(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar.valid());
    ScopedDataset ds(H5Dcreate2(file, "scalar", H5T_NATIVE_INT32, scalar,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ds.valid());

    const int32_t wval = 4242;
    int32_t rval = -1;
    ASSERT_GE(
        H5Dwrite(ds, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl, &wval), 0);
    ASSERT_GE(
        H5Dread(ds, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl, &rval), 0);
    EXPECT_EQ(rval, 4242);
  }

  // 2. Multi-sequence (> H5D_IO_VECTOR_SIZE = 1024) file and memory strided
  //    selections, plus dxpl_vec_size > H5D_IO_VECTOR_SIZE (2048).
  constexpr hsize_t kTotal = 3000;
  constexpr hsize_t kHalf = 1500;
  ScopedSpace fspace(H5Screate_simple(1, &kTotal, nullptr));
  ASSERT_TRUE(fspace.valid());
  ScopedDataset dset(H5Dcreate2(file, "vec1d", H5T_NATIVE_INT32, fspace,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  ScopedSpace mspace_contig(H5Screate_simple(1, &kHalf, nullptr));
  ASSERT_TRUE(mspace_contig.valid());

  ScopedSpace fspace_strided(H5Dget_space(dset));
  ASSERT_TRUE(fspace_strided.valid());
  constexpr hsize_t kStart0 = 0;
  constexpr hsize_t kStride2 = 2;
  constexpr hsize_t kBlock1 = 1;
  ASSERT_GE(H5Sselect_hyperslab(fspace_strided, H5S_SELECT_SET, &kStart0,
                                &kStride2, &kHalf, &kBlock1),
            0);

  ScopedSpace mspace_strided(H5Screate_simple(1, &kTotal, nullptr));
  ASSERT_TRUE(mspace_strided.valid());
  ASSERT_GE(H5Sselect_hyperslab(mspace_strided, H5S_SELECT_SET, &kStart0,
                                &kStride2, &kHalf, &kBlock1),
            0);

  std::vector<int32_t> even_vals(kHalf);
  std::iota(even_vals.begin(), even_vals.end(), 10);

  // 2a. >1024 sequences in file space with contiguous memory space.
  ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, mspace_contig, fspace_strided,
                     dxpl, even_vals.data()),
            0);
  std::vector<int32_t> read_even(kHalf, -1);
  ASSERT_GE(H5Dread(dset, H5T_NATIVE_INT32, mspace_contig, fspace_strided, dxpl,
                    read_even.data()),
            0);
  EXPECT_EQ(read_even, even_vals);

  // 2b. Contiguous file space prefix with >1024 sequences in memory space.
  std::vector<int32_t> mem_strided_buf(kTotal, -1);
  for (size_t i = 0; i < kHalf; ++i) {
    mem_strided_buf[2 * i] = static_cast<int32_t>(i + 500);
  }
  ScopedSpace fspace_prefix(H5Dget_space(dset));
  ASSERT_TRUE(fspace_prefix.valid());
  ASSERT_GE(H5Sselect_hyperslab(fspace_prefix, H5S_SELECT_SET, &kStart0, nullptr,
                                &kHalf, nullptr),
            0);
  ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, mspace_strided, fspace_prefix,
                     dxpl, mem_strided_buf.data()),
            0);
  std::vector<int32_t> read_strided_buf(kTotal, -99);
  ASSERT_GE(H5Dread(dset, H5T_NATIVE_INT32, mspace_strided, fspace_prefix, dxpl,
                    read_strided_buf.data()),
            0);
  EXPECT_EQ(read_strided_buf[0], 500);
  EXPECT_EQ(read_strided_buf[1], -99);
  EXPECT_EQ(read_strided_buf[2998], 500 + 1499);

  // 2c. Custom hyper_vector_size > H5D_IO_VECTOR_SIZE.
  ASSERT_GE(H5Pset_hyper_vector_size(dxpl, 2048), 0);
  std::fill(read_even.begin(), read_even.end(), -1);
  ASSERT_GE(H5Dread(dset, H5T_NATIVE_INT32, mspace_contig, fspace_prefix, dxpl,
                    read_even.data()),
            0);
  EXPECT_EQ(read_even[0], 500);
  EXPECT_EQ(read_even[1499], 500 + 1499);
}

TEST(H5DselectTest, SelectIoReadWriteErrorPathsViaCustomVfdAndDirectCalls) {
  FailVfdEnv env;
  g_sel_vfd_env = &env;
  absl::Cleanup reset_env = [] { g_sel_vfd_env = nullptr; };

  const H5FD_class_t cls = MakeSelVfdClass();
  const hid_t drv_id = H5FDregister(&cls);
  ASSERT_NE(drv_id, H5I_INVALID_HID);
  absl::Cleanup unreg_drv = [drv_id] { H5FDunregister(drv_id); };

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_sieve_buf_size(fapl, 0), 0);
  ASSERT_GE(H5Pset_driver(fapl, drv_id, nullptr), 0);

  ScopedFile file(
      H5Fcreate("select_err.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(file.valid());

  ScopedPlist dxpl_off(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl_off.valid());
  ASSERT_GE(H5Pset_selection_io(dxpl_off, H5D_SELECTION_IO_MODE_OFF), 0);

  constexpr hsize_t kDim = 8;
  ScopedSpace space(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file, "ds", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  std::array<int32_t, 8> buf = {1, 2, 3, 4, 5, 6, 7, 8};
  ASSERT_GE(
      H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl_off, buf.data()),
      0);

  ScopedErrorSilence silence;

  // 1. Inject VFD read/write errors for both nelmts == 1 and nelmts > 1.
  ScopedSpace one_pt(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(one_pt.valid());
  constexpr hsize_t kCoord = 2;
  ASSERT_GE(H5Sselect_elements(one_pt, H5S_SELECT_SET, 1, &kCoord), 0);
  ScopedSpace m_one(H5Screate_simple(1, &kCoord, nullptr));  // 2 ints
  constexpr hsize_t kOne = 1;
  ASSERT_GE(H5Sset_extent_simple(m_one, 1, &kOne, nullptr), 0);

  env.fail_read = true;
  EXPECT_LT(
      H5Dread(dset, H5T_NATIVE_INT32, m_one, one_pt, dxpl_off, buf.data()), 0);
  EXPECT_LT(
      H5Dread(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl_off, buf.data()),
      0);
  env.fail_read = false;

  env.fail_write = true;
  EXPECT_LT(
      H5Dwrite(dset, H5T_NATIVE_INT32, m_one, one_pt, dxpl_off, buf.data()), 0);
  EXPECT_LT(
      H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl_off, buf.data()),
      0);
  env.fail_write = false;

  // 2. Direct internal H5D__select_read / H5D__select_write error paths:
  //    elmt_size == 0, H5S_SELECT_OFFSET errors on nelmts == 1, vec_size
  //    retrieval error, and sequence iterator errors on nelmts == 2.
  auto* ds = static_cast<H5D_t*>(H5VL_object_verify(dset, H5I_DATASET));
  ASSERT_NE(ds, nullptr);
  H5S_t* raw_space = UnwrapSpace(space);
  ASSERT_NE(raw_space, nullptr);

  ScopedSpace empty_space(H5Scopy(space));
  ASSERT_TRUE(empty_space.valid());
  ASSERT_GE(H5Sselect_none(empty_space), 0);
  H5S_t* raw_empty = UnwrapSpace(empty_space);
  ASSERT_NE(raw_empty, nullptr);

  ScopedSpace one_sel(H5Scopy(one_pt));
  ASSERT_TRUE(one_sel.valid());
  H5S_t* raw_one = UnwrapSpace(one_sel);
  ASSERT_NE(raw_one, nullptr);

  H5D_storage_t store{};
  store.contig.dset_addr = ds->shared->layout.storage.u.contig.addr;
  store.contig.dset_size = ds->shared->layout.storage.u.contig.size;

  H5D_dset_io_info_t dinfo{};
  dinfo.dset = ds;
  dinfo.store = &store;
  dinfo.layout_ops = *ds->shared->layout.ops;
  dinfo.buf.vp = buf.data();
  dinfo.nelmts = 1;
  dinfo.file_space = raw_space;
  dinfo.mem_space = raw_space;
  dinfo.type_info.src_type_size = 0;
  dinfo.type_info.dst_type_size = 0;

  H5D_io_info_t io_info{};
  io_info.op_type = H5D_IO_OP_READ;
  io_info.f_sh = ds->oloc.file->shared;
  io_info.count = 1;
  io_info.dsets_info = &dinfo;

  ScopedApiContext ctx(dxpl_off);
  ASSERT_TRUE(ctx.valid());

  // elmt_size == 0 on both read and write.
  EXPECT_LT(H5D__select_read(&io_info, &dinfo), 0);
  io_info.op_type = H5D_IO_OP_WRITE;
  EXPECT_LT(H5D__select_write(&io_info, &dinfo), 0);

  // H5S_SELECT_OFFSET failures when nelmts == 1.
  io_info.op_type = H5D_IO_OP_READ;
  dinfo.type_info.src_type_size = sizeof(int32_t);
  dinfo.type_info.dst_type_size = sizeof(int32_t);
  dinfo.file_space = raw_empty;
  dinfo.mem_space = raw_space;
  EXPECT_LT(H5D__select_read(&io_info, &dinfo), 0);
  dinfo.file_space = raw_space;
  dinfo.mem_space = raw_empty;
  EXPECT_LT(H5D__select_read(&io_info, &dinfo), 0);

  // H5S_select_iter_init and H5S_SELECT_ITER_GET_SEQ_LIST failures when
  // nelmts == 2 using copied stack H5S_t wrappers with failing callbacks.
  H5S_t bad_init_space{};
  H5S_select_class_t bad_init_cls{};
  MakeFailInitSpace(raw_space, &bad_init_space, &bad_init_cls);

  H5S_t bad_seq_space{};
  H5S_select_class_t bad_seq_cls{};
  MakeFailSeqSpace(raw_space, &bad_seq_space, &bad_seq_cls);

  dinfo.nelmts = 2;
  dinfo.file_space = &bad_init_space;
  dinfo.mem_space = raw_space;
  EXPECT_LT(H5D__select_read(&io_info, &dinfo), 0);
  dinfo.file_space = raw_space;
  dinfo.mem_space = &bad_init_space;
  EXPECT_LT(H5D__select_read(&io_info, &dinfo), 0);

  dinfo.file_space = &bad_seq_space;
  dinfo.mem_space = raw_space;
  EXPECT_LT(H5D__select_read(&io_info, &dinfo), 0);
  dinfo.file_space = raw_space;
  dinfo.mem_space = &bad_seq_space;
  EXPECT_LT(H5D__select_read(&io_info, &dinfo), 0);

  // Also cover writev path on 1-point selection cleanly with f_sh set.
  io_info.op_type = H5D_IO_OP_WRITE;
  dinfo.nelmts = 1;
  dinfo.file_space = raw_one;
  dinfo.mem_space = raw_one;
  EXPECT_GE(H5D__select_write(&io_info, &dinfo), 0);
}

TEST(H5DselectTest, SelectIoMemSingleMultiSequenceAndErrorPaths) {
  ASSERT_GE(H5open(), 0);

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());

  // 1. Single-element and zero-element memory copies.
  constexpr hsize_t kDimSmall = 4;
  ScopedSpace s_space(H5Screate_simple(1, &kDimSmall, nullptr));
  ASSERT_TRUE(s_space.valid());
  ScopedSpace d_space(H5Screate_simple(1, &kDimSmall, nullptr));
  ASSERT_TRUE(d_space.valid());
  constexpr hsize_t kSrcIdx = 2;
  constexpr hsize_t kDstIdx = 1;
  ASSERT_GE(H5Sselect_elements(s_space, H5S_SELECT_SET, 1, &kSrcIdx), 0);
  ASSERT_GE(H5Sselect_elements(d_space, H5S_SELECT_SET, 1, &kDstIdx), 0);

  std::array<int32_t, 4> src_small = {10, 20, 30, 40};
  std::array<int32_t, 4> dst_small = {-1, -1, -1, -1};
  {
    ScopedApiContext ctx(dxpl);
    ASSERT_TRUE(ctx.valid());
    ASSERT_GE(H5D_select_io_mem(dst_small.data(), UnwrapSpace(d_space),
                                src_small.data(), UnwrapSpace(s_space),
                                sizeof(int32_t), /*nelmts=*/0),
              0);
    EXPECT_THAT(dst_small, ElementsAre(-1, -1, -1, -1));

    ASSERT_GE(H5D_select_io_mem(dst_small.data(), UnwrapSpace(d_space),
                                src_small.data(), UnwrapSpace(s_space),
                                sizeof(int32_t), /*nelmts=*/1),
              0);
    EXPECT_THAT(dst_small, ElementsAre(-1, 30, -1, -1));
  }

  // 2. Multi-sequence (> 1024) strided memory-to-memory copies in both
  //    directions plus custom hyper_vector_size > 1024.
  constexpr hsize_t kTotal = 3000;
  constexpr hsize_t kHalf = 1500;
  constexpr hsize_t kStart0 = 0;
  constexpr hsize_t kStride2 = 2;
  constexpr hsize_t kBlock1 = 1;

  ScopedSpace contig_space(H5Screate_simple(1, &kHalf, nullptr));
  ASSERT_TRUE(contig_space.valid());
  ScopedSpace strided_dst(H5Screate_simple(1, &kTotal, nullptr));
  ASSERT_TRUE(strided_dst.valid());
  ASSERT_GE(H5Sselect_hyperslab(strided_dst, H5S_SELECT_SET, &kStart0,
                                &kStride2, &kHalf, &kBlock1),
            0);
  ScopedSpace strided_src(H5Scopy(strided_dst));
  ASSERT_TRUE(strided_src.valid());

  std::vector<int32_t> src_vec(kHalf);
  std::iota(src_vec.begin(), src_vec.end(), 100);
  std::vector<int32_t> dst_vec(kTotal, -7);

  {
    ScopedApiContext ctx(dxpl);
    ASSERT_TRUE(ctx.valid());
    ASSERT_GE(H5D_select_io_mem(dst_vec.data(), UnwrapSpace(strided_dst),
                                src_vec.data(), UnwrapSpace(contig_space),
                                sizeof(int32_t), kHalf),
              0);
    EXPECT_EQ(dst_vec[0], 100);
    EXPECT_EQ(dst_vec[1], -7);
    EXPECT_EQ(dst_vec[2998], 100 + 1499);
  }

  ASSERT_GE(H5Pset_hyper_vector_size(dxpl, 2048), 0);
  std::vector<int32_t> gathered_vec(kHalf, -1);
  {
    ScopedApiContext ctx(dxpl);
    ASSERT_TRUE(ctx.valid());
    ASSERT_GE(H5D_select_io_mem(gathered_vec.data(), UnwrapSpace(contig_space),
                                dst_vec.data(), UnwrapSpace(strided_src),
                                sizeof(int32_t), kHalf),
              0);
    EXPECT_EQ(gathered_vec, src_vec);
  }

  // 3. Error paths in H5D_select_io_mem.
  ScopedSpace empty_space(H5Scopy(contig_space));
  ASSERT_TRUE(empty_space.valid());
  ASSERT_GE(H5Sselect_none(empty_space), 0);
  ScopedSpace null_space(H5Screate(H5S_NULL));
  ASSERT_TRUE(null_space.valid());

  ScopedErrorSilence silence;
  ScopedApiContext ctx(dxpl);
  ASSERT_TRUE(ctx.valid());

  EXPECT_LT(H5D_select_io_mem(dst_small.data(), UnwrapSpace(d_space),
                              src_small.data(), UnwrapSpace(s_space),
                              /*elmt_size=*/0, /*nelmts=*/1),
            0);

  // H5S_SELECT_OFFSET failures when nelmts == 1.
  EXPECT_LT(H5D_select_io_mem(dst_small.data(), UnwrapSpace(empty_space),
                              src_small.data(), UnwrapSpace(s_space),
                              sizeof(int32_t), /*nelmts=*/1),
            0);
  EXPECT_LT(H5D_select_io_mem(dst_small.data(), UnwrapSpace(d_space),
                              src_small.data(), UnwrapSpace(empty_space),
                              sizeof(int32_t), /*nelmts=*/1),
            0);

  // H5S_select_iter_init and GET_SEQ_LIST failures when nelmts == 2.
  H5S_t* raw_contig = UnwrapSpace(contig_space);
  ASSERT_NE(raw_contig, nullptr);
  H5S_t bad_init_space{};
  H5S_select_class_t bad_init_cls{};
  MakeFailInitSpace(raw_contig, &bad_init_space, &bad_init_cls);

  H5S_t bad_seq_space{};
  H5S_select_class_t bad_seq_cls{};
  MakeFailSeqSpace(raw_contig, &bad_seq_space, &bad_seq_cls);

  EXPECT_LT(H5D_select_io_mem(dst_small.data(), &bad_init_space,
                              src_small.data(), raw_contig,
                              sizeof(int32_t), /*nelmts=*/2),
            0);
  EXPECT_LT(H5D_select_io_mem(dst_small.data(), raw_contig,
                              src_small.data(), &bad_init_space,
                              sizeof(int32_t), /*nelmts=*/2),
            0);
  EXPECT_LT(H5D_select_io_mem(dst_small.data(), &bad_seq_space,
                              src_small.data(), raw_contig,
                              sizeof(int32_t), /*nelmts=*/2),
            0);
  EXPECT_LT(H5D_select_io_mem(dst_small.data(), raw_contig,
                              src_small.data(), &bad_seq_space,
                              sizeof(int32_t), /*nelmts=*/2),
            0);
}

}  // namespace
}  // namespace hdf5
