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
// Tests dataset object-header class callbacks (`H5O_OBJ_DATASET`): open with
// LAPL/DAPL variants, isa predicate, copy udata alloc/free, create, get_oloc,
// bh_info (chunked, VDS, EFL), flush, and error paths (H5Doh.c).
//
// Google3 build caveats:
//   - Compiles `:hdf5` with `-DNDEBUG` (internal structs omit debug fields).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Doh.c

#include "third_party/hdf5/src/hdf5.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields in internal
// metadata structures. Define NDEBUG around both the prelude and internal
// headers so internal struct layouts match the compiled library ABI.
#ifndef NDEBUG
#define H5DOH_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#pragma push_macro("try")
#undef try
#define try hdf5_try_param
#define H5D_FRIEND
#define H5G_FRIEND
#define H5O_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Dpkg.h"
#include "third_party/hdf5/src/H5Gprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5Sprivate.h"
#include "third_party/hdf5/src/H5Tprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#undef try
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5DOH_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5DOH_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

// Pushes an HDF5 API context and optional LAPL/metadata tag for internal calls.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(hid_t lapl_id = H5P_DEFAULT,
                            haddr_t tag = 0x10000) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      if (lapl_id != H5P_DEFAULT) {
        H5CX_set_lapl(lapl_id);
      }
      H5AC_tag(tag, &prev_tag_);
      tagged_ = true;
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
  void set_dapl(hid_t dapl_id) { node_.ctx.dapl_id = dapl_id; }

 private:
  H5CX_node_t node_{};
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool tagged_ = false;
};

struct FailingVfdEnv {
  bool fail_read = false;
  bool fail_write = false;
  bool persist_single_file = false;
  std::vector<uint8_t> persisted_data;
  unsigned long next_fileno = 1;
};

FailingVfdEnv* g_fail_vfd_env = nullptr;

struct MemVfdFile {
  H5FD_t pub{};
  std::vector<uint8_t> data;
  haddr_t eoa = 0;
};

H5FD_t* MemOpen(const char* name, unsigned flags, hid_t /*fapl_id*/,
                haddr_t maxaddr) {
  if (g_fail_vfd_env == nullptr || name == nullptr || *name == '\0') {
    return nullptr;
  }
  auto* file = new MemVfdFile();
  file->pub.fileno = g_fail_vfd_env->next_fileno++;
  file->pub.maxaddr = maxaddr;
  if (g_fail_vfd_env->persist_single_file && (flags & H5F_ACC_TRUNC) == 0 &&
      !g_fail_vfd_env->persisted_data.empty()) {
    file->data = g_fail_vfd_env->persisted_data;
    file->eoa = static_cast<haddr_t>(file->data.size());
  }
  return reinterpret_cast<H5FD_t*>(file);
}

herr_t MemClose(H5FD_t* _file) {
  auto* file = reinterpret_cast<MemVfdFile*>(_file);
  if (g_fail_vfd_env != nullptr && g_fail_vfd_env->persist_single_file) {
    g_fail_vfd_env->persisted_data = file->data;
  }
  delete file;
  return 0;
}

int MemCmp(const H5FD_t* f1, const H5FD_t* f2) {
  if (f1->fileno < f2->fileno) return -1;
  if (f1->fileno > f2->fileno) return 1;
  return 0;
}

herr_t MemQuery(const H5FD_t* /*file*/, unsigned long* flags) {
  *flags = H5FD_FEAT_DATA_SIEVE;
  return 0;
}

haddr_t MemGetEoa(const H5FD_t* _file, H5FD_mem_t /*type*/) {
  return reinterpret_cast<const MemVfdFile*>(_file)->eoa;
}

herr_t MemSetEoa(H5FD_t* _file, H5FD_mem_t /*type*/, haddr_t addr) {
  auto* file = reinterpret_cast<MemVfdFile*>(_file);
  file->eoa = addr;
  if (file->data.size() < static_cast<size_t>(addr)) {
    file->data.resize(static_cast<size_t>(addr), 0);
  }
  return 0;
}

haddr_t MemGetEof(const H5FD_t* _file, H5FD_mem_t /*type*/) {
  const auto* file = reinterpret_cast<const MemVfdFile*>(_file);
  return static_cast<haddr_t>(file->data.size());
}

herr_t MemRead(H5FD_t* _file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
               haddr_t addr, size_t size, void* buf) {
  if (g_fail_vfd_env != nullptr && g_fail_vfd_env->fail_read) {
    return -1;
  }
  const auto* file = reinterpret_cast<const MemVfdFile*>(_file);
  std::memset(buf, 0, size);
  const size_t offset = static_cast<size_t>(addr);
  if (offset < file->data.size()) {
    const size_t avail = std::min(size, file->data.size() - offset);
    std::memcpy(buf, file->data.data() + offset, avail);
  }
  return 0;
}

herr_t MemWrite(H5FD_t* _file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                haddr_t addr, size_t size, const void* buf) {
  if (g_fail_vfd_env != nullptr && g_fail_vfd_env->fail_write) {
    return -1;
  }
  auto* file = reinterpret_cast<MemVfdFile*>(_file);
  const size_t offset = static_cast<size_t>(addr);
  if (offset + size > file->data.size()) {
    file->data.resize(offset + size, 0);
  }
  std::memcpy(file->data.data() + offset, buf, size);
  return 0;
}

H5FD_class_t MakeFailingVfdClass() {
  H5FD_class_t cls{};
  cls.version = H5FD_CLASS_VERSION;
  cls.value = static_cast<H5FD_class_value_t>(331);
  cls.name = "test_doh_failing_vfd";
  cls.maxaddr = (static_cast<haddr_t>(1) << 28) - 1;
  cls.fc_degree = H5F_CLOSE_WEAK;
  cls.open = MemOpen;
  cls.close = MemClose;
  cls.cmp = MemCmp;
  cls.query = MemQuery;
  cls.get_eoa = MemGetEoa;
  cls.set_eoa = MemSetEoa;
  cls.get_eof = MemGetEof;
  cls.read = MemRead;
  cls.write = MemWrite;
  const H5FD_mem_t fl_map[H5FD_MEM_NTYPES] = H5FD_FLMAP_SINGLE;
  std::memcpy(cls.fl_map, fl_map, sizeof(cls.fl_map));
  return cls;
}

TEST(H5DohTest, OpenWithDefaultLaplAndCustomDaplPlusIsaAndCopyUdata) {
  ScopedFile src = CreateInMemoryFile("doh_src.h5");
  ASSERT_TRUE(src.valid());
  ScopedFile dst = CreateInMemoryFile("doh_dst.h5");
  ASSERT_TRUE(dst.valid());

  constexpr std::array<hsize_t, 1> kDims = {16};
  constexpr std::array<hsize_t, 1> kChunk = {4};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl, 1, kChunk.data()), 0);
  ASSERT_GE(H5Pset_deflate(dcpl, 5), 0);

  ScopedGroup grp(
      H5Gcreate2(src, "grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedType committed_type(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(committed_type.valid());
  ASSERT_GE(H5Tcommit2(src, "dtype", committed_type, H5P_DEFAULT, H5P_DEFAULT,
                       H5P_DEFAULT),
            0);

  std::array<int32_t, 16> vals{};
  for (size_t i = 0; i < vals.size(); ++i) {
    vals[i] = static_cast<int32_t>(i + 1);
  }
  {
    ScopedDataset ds(H5Dcreate2(src, "filtered", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(ds.valid());
    ASSERT_GE(H5Dwrite(ds, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       vals.data()),
              0);
  }

  // 1. Open dataset via H5Oopen with H5P_DEFAULT, custom LAPL, and custom DAPL
  //    (DAPL inherits H5P_LINK_ACCESS, exercising all 3 branches in
  //    H5O__dset_open).
  {
    ScopedObject o_def(H5Oopen(src, "filtered", H5P_DEFAULT));
    ASSERT_TRUE(o_def.valid());
  }
  {
    ScopedPlist lapl(H5Pcreate(H5P_LINK_ACCESS));
    ASSERT_TRUE(lapl.valid());
    ScopedObject o_lapl(H5Oopen(src, "filtered", lapl));
    ASSERT_TRUE(o_lapl.valid());
  }
  {
    ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_TRUE(dapl.valid());
    ASSERT_GE(H5Pset_chunk_cache(dapl, 13, 4096, 0.5), 0);
    ScopedObject o_dapl(H5Oopen(src, "filtered", dapl));
    ASSERT_TRUE(o_dapl.valid());
  }

  // 2. Copy filtered dataset via H5Ocopy to populate and free all three fields
  //    in H5D_copy_file_ud_t (`src_space_extent`, `src_dtype`, `src_pline`).
  ASSERT_GE(
      H5Ocopy(src, "filtered", dst, "copied", H5P_DEFAULT, H5P_DEFAULT), 0);
  ScopedDataset copied(H5Dopen2(dst, "copied", H5P_DEFAULT));
  ASSERT_TRUE(copied.valid());
  std::array<int32_t, 16> rvals{};
  ASSERT_GE(H5Dread(copied, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    rvals.data()),
            0);
  EXPECT_EQ(rvals, vals);

  // 3. Direct get/free copy_file_udata with NULL fields + direct isa checks
  //    on dataset (true), committed datatype (dtype only -> false), and group
  //    (no dtype -> false).
  void* empty_udata = H5O_OBJ_DATASET->get_copy_file_udata();
  ASSERT_NE(empty_udata, nullptr);
  H5O_OBJ_DATASET->free_copy_file_udata(empty_udata);

  auto* ds_ptr =
      static_cast<H5D_t*>(H5VL_object_verify(copied, H5I_DATASET));
  ASSERT_NE(ds_ptr, nullptr);
  auto* grp_ptr = static_cast<H5G_t*>(H5VL_object_verify(grp, H5I_GROUP));
  ASSERT_NE(grp_ptr, nullptr);
  auto* dt_ptr =
      static_cast<H5T_t*>(H5VL_object_verify(committed_type, H5I_DATATYPE));
  ASSERT_NE(dt_ptr, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5O_t* oh_ds = H5O_protect(&ds_ptr->oloc, H5AC__READ_ONLY_FLAG, false);
  ASSERT_NE(oh_ds, nullptr);
  EXPECT_GT(H5O_OBJ_DATASET->isa(oh_ds), 0);
  ASSERT_GE(H5O_unprotect(&ds_ptr->oloc, oh_ds, H5AC__NO_FLAGS_SET), 0);

  H5O_loc_t* dt_oloc = H5T_oloc(dt_ptr);
  ASSERT_NE(dt_oloc, nullptr);
  H5O_t* oh_dt = H5O_protect(dt_oloc, H5AC__READ_ONLY_FLAG, false);
  ASSERT_NE(oh_dt, nullptr);
  EXPECT_EQ(H5O_OBJ_DATASET->isa(oh_dt), 0);
  ASSERT_GE(H5O_unprotect(dt_oloc, oh_dt, H5AC__NO_FLAGS_SET), 0);

  H5O_loc_t* grp_oloc = H5G_oloc(grp_ptr);
  ASSERT_NE(grp_oloc, nullptr);
  H5O_t* oh_grp = H5O_protect(grp_oloc, H5AC__READ_ONLY_FLAG, false);
  ASSERT_NE(oh_grp, nullptr);
  EXPECT_EQ(H5O_OBJ_DATASET->isa(oh_grp), 0);
  ASSERT_GE(H5O_unprotect(grp_oloc, oh_grp, H5AC__NO_FLAGS_SET), 0);
}

TEST(H5DohTest, BhInfoCoversContiguousChunkedVirtualAndExternalDatasets) {
  TempDir ext_dir("doh_efl");
  const std::filesystem::path ext_path = ext_dir.child("external.bin");
  {
    std::ofstream ofs(ext_path, std::ios::binary);
    ASSERT_TRUE(ofs.is_open());
    const std::array<uint8_t, 64> zeros{};
    ofs.write(reinterpret_cast<const char*>(zeros.data()),
              static_cast<std::streamsize>(zeros.size()));
    ASSERT_TRUE(ofs.good());
  }

  // Use a Core VFD file for metadata while pointing EFL to TempDir file.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ScopedFile file(
      H5Fcreate("doh_bh_info.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(file.valid());

  constexpr std::array<hsize_t, 1> kDims = {8};
  constexpr std::array<hsize_t, 1> kUnlim = {H5S_UNLIMITED};
  constexpr std::array<hsize_t, 1> kChunk = {4};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());
  ScopedSpace ext_space(H5Screate_simple(1, kDims.data(), kUnlim.data()));
  ASSERT_TRUE(ext_space.valid());

  // 1. Contiguous dataset.
  ScopedDataset contig(H5Dcreate2(file, "contig", H5T_NATIVE_INT32, space,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(contig.valid());
  H5O_native_info_t info{};
  ASSERT_GE(H5Oget_native_info(contig, &info, H5O_NATIVE_INFO_META_SIZE), 0);
  EXPECT_EQ(info.meta_size.obj.index_size, 0u);
  EXPECT_EQ(info.meta_size.obj.heap_size, 0u);

  // 2. Allocated chunked dataset (unlim v1 B-tree index -> index_size > 0) and
  //    unallocated late chunked dataset.
  ScopedPlist dcpl_chk(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl_chk.valid());
  ASSERT_GE(H5Pset_chunk(dcpl_chk, 1, kChunk.data()), 0);
  ScopedDataset chk_alloc(H5Dcreate2(file, "chk_alloc", H5T_NATIVE_INT32,
                                     ext_space, H5P_DEFAULT, dcpl_chk,
                                     H5P_DEFAULT));
  ASSERT_TRUE(chk_alloc.valid());
  std::array<int32_t, 8> wdata = {1, 2, 3, 4, 5, 6, 7, 8};
  ASSERT_GE(H5Dwrite(chk_alloc, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     wdata.data()),
            0);
  ASSERT_GE(H5Fflush(file, H5F_SCOPE_GLOBAL), 0);
  info = {};
  ASSERT_GE(H5Oget_native_info(chk_alloc, &info, H5O_NATIVE_INFO_META_SIZE),
            0);
  EXPECT_GT(info.meta_size.obj.index_size, 0u);

  ScopedPlist dcpl_late(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl_late.valid());
  ASSERT_GE(H5Pset_chunk(dcpl_late, 1, kChunk.data()), 0);
  ASSERT_GE(H5Pset_alloc_time(dcpl_late, H5D_ALLOC_TIME_LATE), 0);
  ScopedDataset chk_unalloc(H5Dcreate2(file, "chk_unalloc", H5T_NATIVE_INT32,
                                       ext_space, H5P_DEFAULT, dcpl_late,
                                       H5P_DEFAULT));
  ASSERT_TRUE(chk_unalloc.valid());
  info = {};
  ASSERT_GE(H5Oget_native_info(chk_unalloc, &info, H5O_NATIVE_INFO_META_SIZE),
            0);
  EXPECT_EQ(info.meta_size.obj.index_size, 0u);

  // 3. Virtual dataset with persisted global heap mapping (heap_size > 0).
  ScopedPlist dcpl_vds(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl_vds.valid());
  ASSERT_GE(H5Pset_virtual(dcpl_vds, space, "doh_bh_info.h5", "contig", space),
            0);
  ScopedDataset vds(H5Dcreate2(file, "vds", H5T_NATIVE_INT32, space,
                               H5P_DEFAULT, dcpl_vds, H5P_DEFAULT));
  ASSERT_TRUE(vds.valid());
  ASSERT_GE(H5Fflush(file, H5F_SCOPE_GLOBAL), 0);
  info = {};
  ASSERT_GE(H5Oget_native_info(vds, &info, H5O_NATIVE_INFO_META_SIZE), 0);
  EXPECT_GT(info.meta_size.obj.heap_size, 0u);

  // 4. External File List (EFL) contiguous dataset (heap_size > 0).
  ScopedPlist dcpl_efl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl_efl.valid());
  ASSERT_GE(H5Pset_external(dcpl_efl, ext_path.c_str(), 0, 32), 0);
  ScopedDataset efl(H5Dcreate2(file, "efl_ds", H5T_NATIVE_INT32, space,
                               H5P_DEFAULT, dcpl_efl, H5P_DEFAULT));
  ASSERT_TRUE(efl.valid());
  ASSERT_GE(H5Fflush(file, H5F_SCOPE_GLOBAL), 0);
  info = {};
  ASSERT_GE(H5Oget_native_info(efl, &info, H5O_NATIVE_INFO_META_SIZE), 0);
  EXPECT_GT(info.meta_size.obj.heap_size, 0u);

  // 5. Direct bh_info error path on a group object header (missing LAYOUT).
  ScopedGroup grp(
      H5Gcreate2(file, "grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  auto* grp_ptr = static_cast<H5G_t*>(H5VL_object_verify(grp, H5I_GROUP));
  ASSERT_NE(grp_ptr, nullptr);
  H5O_loc_t* grp_oloc = H5G_oloc(grp_ptr);
  ASSERT_NE(grp_oloc, nullptr);
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5O_t* oh_grp = H5O_protect(grp_oloc, H5AC__READ_ONLY_FLAG, false);
  ASSERT_NE(oh_grp, nullptr);
  absl::Cleanup unprot = [&] {
    H5O_unprotect(grp_oloc, oh_grp, H5AC__NO_FLAGS_SET);
  };
  H5_ih_info_t bh_out{};
  ScopedErrorSilence silence;
  EXPECT_LT(H5O_OBJ_DATASET->bh_info(grp_oloc, oh_grp, &bh_out), 0);

  // Chunked bh_info failure when loc points to a non-dataset group without
  // dataspace message while oh holds an allocated chunked layout.
  auto* chk_ptr =
      static_cast<H5D_t*>(H5VL_object_verify(chk_alloc, H5I_DATASET));
  ASSERT_NE(chk_ptr, nullptr);
  H5O_t* oh_chk = H5O_protect(&chk_ptr->oloc, H5AC__READ_ONLY_FLAG, false);
  ASSERT_NE(oh_chk, nullptr);
  absl::Cleanup unprot_chk = [&] {
    H5O_unprotect(&chk_ptr->oloc, oh_chk, H5AC__NO_FLAGS_SET);
  };
  EXPECT_LT(H5O_OBJ_DATASET->bh_info(grp_oloc, oh_chk, &bh_out), 0);
}

TEST(H5DohTest, GetOlocCreateFlushAndObjectClassErrorPaths) {
  FailingVfdEnv env;
  g_fail_vfd_env = &env;
  absl::Cleanup reset_env = [] { g_fail_vfd_env = nullptr; };

  const H5FD_class_t cls = MakeFailingVfdClass();
  const hid_t drv_id = H5FDregister(&cls);
  ASSERT_NE(drv_id, H5I_INVALID_HID);
  absl::Cleanup unreg_drv = [drv_id] { H5FDunregister(drv_id); };

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_sieve_buf_size(fapl, 64), 0);
  ASSERT_GE(H5Pset_driver(fapl, drv_id, nullptr), 0);

  ScopedFile file(
      H5Fcreate("doh_flush.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(file.valid());

  constexpr std::array<hsize_t, 1> kDims = {16};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file, "ds", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  // 1. Public H5Oflush / H5Oget_info3 / H5O_OBJ_DATASET->get_oloc & flush.
  std::array<int32_t, 4> partial = {10, 20, 30, 40};
  ScopedSpace mem_sub(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(mem_sub.valid());
  constexpr hsize_t kStart = 0;
  constexpr hsize_t kCount = 4;
  ScopedSpace file_sub(H5Dget_space(dset));
  ASSERT_TRUE(file_sub.valid());
  ASSERT_GE(
      H5Sselect_hyperslab(file_sub, H5S_SELECT_SET, &kStart, nullptr, &kCount,
                          nullptr),
      0);
  ASSERT_GE(
      H5Sselect_hyperslab(mem_sub, H5S_SELECT_SET, &kStart, nullptr, &kCount,
                          nullptr),
      0);
  ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, mem_sub, file_sub, H5P_DEFAULT,
                     partial.data()),
            0);
  ASSERT_GE(H5Oflush(dset), 0);

  H5O_info2_t oinfo{};
  ASSERT_GE(H5Oget_info3(dset, &oinfo, H5O_INFO_BASIC), 0);
  EXPECT_EQ(oinfo.type, H5O_TYPE_DATASET);

  EXPECT_NE(H5O_OBJ_DATASET->get_oloc(dset), nullptr);

  // 2. Flush failure via dirty sieve buffer when VFD write fails.
  ScopedPlist dxpl_off(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl_off.valid());
  ASSERT_GE(H5Pset_selection_io(dxpl_off, H5D_SELECTION_IO_MODE_OFF), 0);
  ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, mem_sub, file_sub, dxpl_off,
                     partial.data()),
            0);

  ScopedErrorSilence silence;
  env.fail_write = true;
  EXPECT_LT(H5Oflush(dset), 0);
  env.fail_write = false;

  // Clean up dirty sieve buffer so file teardown stays error-free.
  ASSERT_GE(H5Oflush(dset), 0);

  // 3. Direct error paths on get_oloc, open, create, and flush.
  EXPECT_EQ(H5O_OBJ_DATASET->get_oloc(H5I_INVALID_HID), nullptr);

  auto* ds_ptr = static_cast<H5D_t*>(H5VL_object_verify(dset, H5I_DATASET));
  ASSERT_NE(ds_ptr, nullptr);

  {
    // Pass H5I_INVALID_HID as LAPL in H5CX so H5P_isa_class fails in
    // H5O__dset_open without mutating `ds_ptr->oloc`.
    ScopedApiContext ctx(H5I_INVALID_HID, ds_ptr->oloc.addr);
    ASSERT_TRUE(ctx.valid());
    H5CX_set_lapl(H5I_INVALID_HID);
    H5G_loc_t obj_loc{};
    obj_loc.oloc = &ds_ptr->oloc;
    obj_loc.path = &ds_ptr->path;
    H5I_type_t opened_type = H5I_BADID;
    EXPECT_EQ(H5O_OBJ_DATASET->open(&obj_loc, &opened_type), nullptr);
  }

  {
    // Already-open dataset reopened with mismatched external file prefix fails
    // inside H5D_open without invoking H5D__open_oid error cleanup. Use a deep
    // copy of `ds_ptr->oloc`/`path` because `H5D_open` takes shallow ownership
    // and resets `open_loc` on entry.
    ScopedPlist diff_dapl(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_TRUE(diff_dapl.valid());
    ASSERT_GE(H5Pset_efile_prefix(diff_dapl, "/doh_mismatch_prefix"), 0);
    ScopedApiContext ctx(diff_dapl, ds_ptr->oloc.addr);
    ASSERT_TRUE(ctx.valid());
    ctx.set_dapl(diff_dapl);

    H5O_loc_t tmp_oloc{};
    H5G_name_t tmp_path{};
    H5O_loc_reset(&tmp_oloc);
    H5G_name_reset(&tmp_path);
    ASSERT_GE(H5O_loc_copy_deep(&tmp_oloc, &ds_ptr->oloc), 0);
    ASSERT_GE(H5G_name_copy(&tmp_path, &ds_ptr->path, H5_COPY_DEEP), 0);
    absl::Cleanup free_tmp_loc = [&] {
      H5O_loc_free(&tmp_oloc);
      H5G_name_free(&tmp_path);
    };

    H5G_loc_t open_loc{};
    open_loc.oloc = &tmp_oloc;
    open_loc.path = &tmp_path;
    H5I_type_t opened_type = H5I_BADID;
    EXPECT_EQ(H5O_OBJ_DATASET->open(&open_loc, &opened_type), nullptr);
  }

  {
    // H5O__dset_create fails when DCPL is chunked for a scalar dataspace.
    ScopedSpace scalar_space(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar_space.valid());
    auto* s_ptr =
        static_cast<H5S_t*>(H5I_object_verify(scalar_space, H5I_DATASPACE));
    ASSERT_NE(s_ptr, nullptr);

    ScopedPlist bad_dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(bad_dcpl.valid());
    constexpr hsize_t kOne = 1;
    ASSERT_GE(H5Pset_chunk(bad_dcpl, 1, &kOne), 0);

    H5D_obj_create_t crt{};
    crt.type_id = H5T_NATIVE_INT32;
    crt.space = s_ptr;
    crt.dcpl_id = bad_dcpl;
    crt.dapl_id = H5P_DATASET_ACCESS_DEFAULT;

    H5O_loc_t dummy_oloc{};
    H5G_name_t dummy_path{};
    H5G_loc_t out_loc{&dummy_oloc, &dummy_path};

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    EXPECT_EQ(H5O_OBJ_DATASET->create(ds_ptr->oloc.file, &crt, &out_loc),
              nullptr);
  }

  {
    // H5O__dset_flush error paths: non-dataset object and unreadable oloc.
    ScopedGroup grp(
        H5Gcreate2(file, "not_a_ds", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    auto* grp_ptr = static_cast<H5G_t*>(H5VL_object_verify(grp, H5I_GROUP));
    ASSERT_NE(grp_ptr, nullptr);

    H5D_t fake_dset = *ds_ptr;
    fake_dset.oloc = *H5G_oloc(grp_ptr);
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    EXPECT_LT(H5O_OBJ_DATASET->flush(&fake_dset), 0);

    fake_dset.oloc.addr = HADDR_UNDEF;
    EXPECT_LT(H5O_OBJ_DATASET->flush(&fake_dset), 0);
  }
}

TEST(H5DohTest, BhInfoVirtualExternalAndCorruptEflErrorPaths) {
  TempDir ext_dir("doh_bh_efl");
  const std::filesystem::path ext_path = ext_dir.child("external_bh.bin");
  {
    std::ofstream ofs(ext_path, std::ios::binary);
    ASSERT_TRUE(ofs.is_open());
    const std::array<uint8_t, 64> zeros{};
    ofs.write(reinterpret_cast<const char*>(zeros.data()),
              static_cast<std::streamsize>(zeros.size()));
    ASSERT_TRUE(ofs.good());
  }

  FailingVfdEnv env;
  env.persist_single_file = true;
  g_fail_vfd_env = &env;
  absl::Cleanup reset_env = [] { g_fail_vfd_env = nullptr; };

  const H5FD_class_t cls = MakeFailingVfdClass();
  const hid_t drv_id = H5FDregister(&cls);
  ASSERT_NE(drv_id, H5I_INVALID_HID);
  absl::Cleanup unreg_drv = [drv_id] { H5FDunregister(drv_id); };

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_driver(fapl, drv_id, nullptr), 0);

  haddr_t vds_addr = HADDR_UNDEF;
  haddr_t efl_addr = HADDR_UNDEF;
  constexpr std::array<hsize_t, 1> kDims = {8};
  {
    ScopedFile wfile(
        H5Fcreate("doh_bh_vfd.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(wfile.valid());
    ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());

    ScopedDataset contig(H5Dcreate2(wfile, "contig", H5T_NATIVE_INT32, space,
                                    H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(contig.valid());

    ScopedPlist dcpl_vds(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_vds.valid());
    ASSERT_GE(
        H5Pset_virtual(dcpl_vds, space, "doh_bh_vfd.h5", "contig", space), 0);
    ScopedDataset vds(H5Dcreate2(wfile, "vds", H5T_NATIVE_INT32, space,
                                 H5P_DEFAULT, dcpl_vds, H5P_DEFAULT));
    ASSERT_TRUE(vds.valid());

    ScopedPlist dcpl_efl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_efl.valid());
    ASSERT_GE(H5Pset_external(dcpl_efl, ext_path.c_str(), 0, 32), 0);
    ScopedDataset efl(H5Dcreate2(wfile, "efl_ds", H5T_NATIVE_INT32, space,
                                 H5P_DEFAULT, dcpl_efl, H5P_DEFAULT));
    ASSERT_TRUE(efl.valid());

    auto* vds_ptr = static_cast<H5D_t*>(H5VL_object_verify(vds, H5I_DATASET));
    auto* efl_ptr = static_cast<H5D_t*>(H5VL_object_verify(efl, H5I_DATASET));
    ASSERT_NE(vds_ptr, nullptr);
    ASSERT_NE(efl_ptr, nullptr);
    vds_addr = vds_ptr->oloc.addr;
    efl_addr = efl_ptr->oloc.addr;
  }

  // Reopen RDONLY without opening `vds`/`efl_ds` so global/local heaps are not
  // cached in memory, then trigger VFD read errors inside H5O__dset_bh_info.
  ScopedFile rfile(H5Fopen("doh_bh_vfd.h5", H5F_ACC_RDONLY, fapl));
  ASSERT_TRUE(rfile.valid());
  ScopedDataset contig_open(H5Dopen2(rfile, "contig", H5P_DEFAULT));
  ASSERT_TRUE(contig_open.valid());
  auto* contig_ptr =
      static_cast<H5D_t*>(H5VL_object_verify(contig_open, H5I_DATASET));
  ASSERT_NE(contig_ptr, nullptr);
  H5F_t* rf = contig_ptr->oloc.file;
  ASSERT_NE(rf, nullptr);

  ScopedErrorSilence silence;
  H5_ih_info_t bh_out{};

  {
    H5O_loc_t vds_loc{};
    vds_loc.file = rf;
    vds_loc.addr = vds_addr;
    ScopedApiContext ctx(H5P_DEFAULT, vds_addr);
    ASSERT_TRUE(ctx.valid());
    H5O_t* oh_vds = H5O_protect(&vds_loc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(oh_vds, nullptr);
    absl::Cleanup unprot_vds = [&] {
      env.fail_read = false;
      H5O_unprotect(&vds_loc, oh_vds, H5AC__NO_FLAGS_SET);
    };
    env.fail_read = true;
    EXPECT_LT(H5O_OBJ_DATASET->bh_info(&vds_loc, oh_vds, &bh_out), 0);
    env.fail_read = false;
  }

  {
    H5O_loc_t efl_loc{};
    efl_loc.file = rf;
    efl_loc.addr = efl_addr;
    ScopedApiContext ctx(H5P_DEFAULT, efl_addr);
    ASSERT_TRUE(ctx.valid());
    H5O_t* oh_efl = H5O_protect(&efl_loc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(oh_efl, nullptr);
    absl::Cleanup unprot_efl = [&] {
      env.fail_read = false;
      H5O_unprotect(&efl_loc, oh_efl, H5AC__NO_FLAGS_SET);
    };
    // First populate oh_efl's cached native H5O_EFL_ID message.
    EXPECT_GE(H5O_OBJ_DATASET->bh_info(&efl_loc, oh_efl, &bh_out), 0);

    // Override only efl_copy.heap_addr so H5O_msg_read_oh succeeds from
    // cached native while H5D__efl_bh_info fails reading the local heap.
    std::vector<H5O_mesg_t> heap_msgs(oh_efl->mesg,
                                      oh_efl->mesg + oh_efl->nmesgs);
    H5O_efl_t efl_copy{};
    bool found_efl_native = false;
    for (auto& m : heap_msgs) {
      if (m.type == H5O_MSG_EFL && m.native != nullptr) {
        efl_copy = *static_cast<const H5O_efl_t*>(m.native);
        efl_copy.heap_addr += 4096;
        m.native = &efl_copy;
        found_efl_native = true;
      }
    }
    ASSERT_TRUE(found_efl_native);
    H5O_t bad_heap_oh = *oh_efl;
    bad_heap_oh.mesg = heap_msgs.data();
    env.fail_read = true;
    EXPECT_LT(H5O_OBJ_DATASET->bh_info(&efl_loc, &bad_heap_oh, &bh_out), 0);
    env.fail_read = false;

    // Also test corrupt/undecodable EFL message in oh (H5O_msg_read_oh ==
    // NULL) via a shallow message-array copy on the stack.
    std::vector<H5O_mesg_t> msgs(oh_efl->mesg, oh_efl->mesg + oh_efl->nmesgs);
    uint8_t bad_efl_raw[4] = {0xFF, 0, 0, 0};
    for (auto& m : msgs) {
      if (m.type == H5O_MSG_EFL) {
        m.native = nullptr;
        m.raw = bad_efl_raw;
        m.raw_size = sizeof(bad_efl_raw);
      }
    }
    H5O_t bad_oh = *oh_efl;
    bad_oh.mesg = msgs.data();
    EXPECT_LT(H5O_OBJ_DATASET->bh_info(&efl_loc, &bad_oh, &bh_out), 0);
  }
}

}  // namespace
}  // namespace hdf5
