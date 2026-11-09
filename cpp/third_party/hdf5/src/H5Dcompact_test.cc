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
// Tests compact dataset storage construction, initialization, fill values,
// vectorized/memmanage I/O, flushing, copying (including open datasets, VLEN,
// and reference expansion), and corrupt-image validation in
// third_party/hdf5/src/H5Dcompact.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Dcompact.c

#include "third_party/hdf5/src/hdf5.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

// `:hdf5` is built with -DNDEBUG, which omits debug fields from
// H5C_cache_entry_t / metadata structures pulled by H5private.h and H5Dpkg.h.
#ifndef NDEBUG
#define H5DCOMPACT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5D_FRIEND
#define H5F_FRIEND
#define H5S_FRIEND
#define H5T_FRIEND
// H5B2private.h (pulled by H5Dpkg.h) and H5Fpkg.h use `try` as a parameter
// name, colliding with libc++'s `#define try if (1)` and C++ keyword `try`.
#pragma push_macro("try")
#undef try
#define try hdf5_try_param
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Dpkg.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Spkg.h"
#pragma push_macro("H5_HAVE__FLOAT16")
#undef H5_HAVE__FLOAT16
#include "third_party/hdf5/src/H5Tpkg.h"
#pragma pop_macro("H5_HAVE__FLOAT16")
#include "third_party/hdf5/src/H5VLprivate.h"
#undef try
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5DCOMPACT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5DCOMPACT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

// RAII wrapper for pushing/popping an HDF5 internal API context.
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

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

// State shared with the in-memory MEMMANAGE VFD during a single test.
struct MemManageVfdEnv {
  std::map<std::string, std::vector<uint8_t>> files;
  int mem_copy_calls = 0;
  bool fail_mem_copy = false;
  unsigned long next_fileno = 1;
};

MemManageVfdEnv* g_memmanage_env = nullptr;

struct MemManageFile {
  H5FD_t pub{};
  std::string name;
  haddr_t eoa = 0;
};

H5FD_t* MemManageOpen(const char* name, unsigned flags, hid_t /*fapl_id*/,
                      haddr_t maxaddr) {
  if (g_memmanage_env == nullptr || name == nullptr || *name == '\0') {
    return nullptr;
  }
  auto& files = g_memmanage_env->files;
  if (flags & H5F_ACC_TRUNC) {
    files[name].clear();
  } else if (!(flags & H5F_ACC_CREAT) && !files.contains(name)) {
    return nullptr;
  } else {
    files[name];
  }
  auto* file = new MemManageFile();
  file->pub.fileno = g_memmanage_env->next_fileno++;
  file->pub.maxaddr = maxaddr;
  file->name = name;
  file->eoa = 0;
  return reinterpret_cast<H5FD_t*>(file);
}

herr_t MemManageClose(H5FD_t* _file) {
  delete reinterpret_cast<MemManageFile*>(_file);
  return 0;
}

int MemManageCmp(const H5FD_t* f1, const H5FD_t* f2) {
  const auto* a = reinterpret_cast<const MemManageFile*>(f1);
  const auto* b = reinterpret_cast<const MemManageFile*>(f2);
  if (a->name < b->name) return -1;
  if (a->name > b->name) return 1;
  return 0;
}

herr_t MemManageQuery(const H5FD_t* /*file*/, unsigned long* flags) {
  *flags = H5FD_FEAT_MEMMANAGE | H5FD_FEAT_DATA_SIEVE;
  return 0;
}

haddr_t MemManageGetEoa(const H5FD_t* _file, H5FD_mem_t /*type*/) {
  return reinterpret_cast<const MemManageFile*>(_file)->eoa;
}

herr_t MemManageSetEoa(H5FD_t* _file, H5FD_mem_t /*type*/, haddr_t addr) {
  auto* file = reinterpret_cast<MemManageFile*>(_file);
  file->eoa = addr;
  auto& buf = g_memmanage_env->files[file->name];
  if (buf.size() < static_cast<size_t>(addr)) {
    buf.resize(static_cast<size_t>(addr), 0);
  }
  return 0;
}

haddr_t MemManageGetEof(const H5FD_t* _file, H5FD_mem_t /*type*/) {
  const auto* file = reinterpret_cast<const MemManageFile*>(_file);
  const auto it = g_memmanage_env->files.find(file->name);
  haddr_t size = it == g_memmanage_env->files.end()
                     ? 0
                     : static_cast<haddr_t>(it->second.size());
  return std::max(file->eoa, size);
}

herr_t MemManageRead(H5FD_t* _file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                     haddr_t addr, size_t size, void* buf) {
  auto* file = reinterpret_cast<MemManageFile*>(_file);
  const auto& data = g_memmanage_env->files[file->name];
  std::memset(buf, 0, size);
  const size_t offset = static_cast<size_t>(addr);
  if (offset < data.size()) {
    const size_t avail = std::min(size, data.size() - offset);
    std::memcpy(buf, data.data() + offset, avail);
  }
  return 0;
}

herr_t MemManageWrite(H5FD_t* _file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                      haddr_t addr, size_t size, const void* buf) {
  auto* file = reinterpret_cast<MemManageFile*>(_file);
  auto& data = g_memmanage_env->files[file->name];
  const size_t offset = static_cast<size_t>(addr);
  if (offset + size > data.size()) {
    data.resize(offset + size, 0);
  }
  std::memcpy(data.data() + offset, buf, size);
  return 0;
}

herr_t MemManageCtl(H5FD_t* /*file*/, uint64_t op_code, uint64_t flags,
                    const void* input, void** /*output*/) {
  if (op_code == H5FD_CTL_MEM_COPY && input != nullptr &&
      g_memmanage_env != nullptr) {
    ++g_memmanage_env->mem_copy_calls;
    if (g_memmanage_env->fail_mem_copy) {
      return -1;
    }
    const auto* args = static_cast<const H5FD_ctl_memcpy_args_t*>(input);
    auto* dst = static_cast<uint8_t*>(args->dstbuf) + args->dst_off;
    const auto* src = static_cast<const uint8_t*>(args->srcbuf) + args->src_off;
    std::memcpy(dst, src, args->len);
    return 0;
  }
  return (flags & H5FD_CTL_FAIL_IF_UNKNOWN_FLAG) ? -1 : 0;
}

H5FD_class_t MakeMemManageVfdClass() {
  H5FD_class_t cls{};
  cls.version = H5FD_CLASS_VERSION;
  cls.value = static_cast<H5FD_class_value_t>(301);
  cls.name = "test_memmanage_vfd";
  cls.maxaddr = (static_cast<haddr_t>(1) << 30) - 1;
  cls.fc_degree = H5F_CLOSE_WEAK;
  cls.open = MemManageOpen;
  cls.close = MemManageClose;
  cls.cmp = MemManageCmp;
  cls.query = MemManageQuery;
  cls.get_eoa = MemManageGetEoa;
  cls.set_eoa = MemManageSetEoa;
  cls.get_eof = MemManageGetEof;
  cls.read = MemManageRead;
  cls.write = MemManageWrite;
  cls.ctl = MemManageCtl;
  const H5FD_mem_t fl_map[H5FD_MEM_NTYPES] = H5FD_FLMAP_SINGLE;
  std::memcpy(cls.fl_map, fl_map, sizeof(cls.fl_map));
  return cls;
}

TEST(H5DcompactTest, CreateFillWriteHyperslabFlushAndReopen) {
  // 1. Create an in-memory file with 1D, 2D, and scalar compact datasets,
  //    configuring a custom fill value on the 1D dataset.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());

  ScopedFile file(
      H5Fcreate("compact_basic.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(file.valid());

  ScopedPlist dcpl_fill(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl_fill.valid());
  ASSERT_GE(H5Pset_layout(dcpl_fill, H5D_COMPACT), 0);
  const int32_t kFillVal = 777;
  ASSERT_GE(H5Pset_fill_value(dcpl_fill, H5T_NATIVE_INT32, &kFillVal), 0);
  ASSERT_GE(H5Pset_fill_time(dcpl_fill, H5D_FILL_TIME_ALLOC), 0);

  constexpr std::array<hsize_t, 1> kDims1D = {6};
  ScopedSpace space1d(H5Screate_simple(1, kDims1D.data(), nullptr));
  ASSERT_TRUE(space1d.valid());

  {
    ScopedDataset dset1d(H5Dcreate2(file, "vec", H5T_NATIVE_INT32, space1d,
                                    H5P_DEFAULT, dcpl_fill, H5P_DEFAULT));
    ASSERT_TRUE(dset1d.valid());

    // Verify H5D__compact_is_space_alloc reports allocated immediately.
    H5D_space_status_t space_status = H5D_SPACE_STATUS_NOT_ALLOCATED;
    ASSERT_GE(H5Dget_space_status(dset1d, &space_status), 0);
    EXPECT_EQ(space_status, H5D_SPACE_STATUS_ALLOCATED);

    // 2. Read before writing to verify H5D__compact_fill populated all slots.
    std::array<int32_t, 6> got = {};
    ASSERT_GE(H5Dread(dset1d, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      got.data()),
              0);
    EXPECT_THAT(got, ElementsAre(777, 777, 777, 777, 777, 777));

    // 3. Perform a strided hyperslab write and point selection read via Core
    //    VFD (H5VM_memcpyvv branches in writevv/readvv).
    constexpr std::array<hsize_t, 1> kStart = {1};
    constexpr std::array<hsize_t, 1> kStride = {2};
    constexpr std::array<hsize_t, 1> kCount = {3};
    ScopedSpace fspace_sel(H5Scopy(space1d));
    ASSERT_TRUE(fspace_sel.valid());
    ASSERT_GE(H5Sselect_hyperslab(fspace_sel, H5S_SELECT_SET, kStart.data(),
                                  kStride.data(), kCount.data(), nullptr),
              0);

    constexpr std::array<hsize_t, 1> kMemDim = {3};
    ScopedSpace mspace(H5Screate_simple(1, kMemDim.data(), nullptr));
    ASSERT_TRUE(mspace.valid());
    constexpr std::array<int32_t, 3> kPatch = {11, 22, 33};
    ASSERT_GE(H5Dwrite(dset1d, H5T_NATIVE_INT32, mspace, fspace_sel,
                       H5P_DEFAULT, kPatch.data()),
              0);

    // Flush dirty compact buffer, then call H5Dflush again while clean.
    ASSERT_GE(H5Dflush(dset1d), 0);
    ASSERT_GE(H5Dflush(dset1d), 0);

    // 4. Scalar (0D) and 2D compact datasets with type conversion.
    ScopedSpace scalar_space(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar_space.valid());
    ScopedPlist dcpl_plain(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_plain.valid());
    ASSERT_GE(H5Pset_layout(dcpl_plain, H5D_COMPACT), 0);
    ScopedDataset dset_scalar(H5Dcreate2(file, "scalar", H5T_NATIVE_DOUBLE,
                                         scalar_space, H5P_DEFAULT, dcpl_plain,
                                         H5P_DEFAULT));
    ASSERT_TRUE(dset_scalar.valid());
    const int32_t kScalarIn = 42;
    ASSERT_GE(H5Dwrite(dset_scalar, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, &kScalarIn),
              0);

    // Unlink an unused compact dataset to exercise close + object deletion.
    ScopedDataset dset_del(H5Dcreate2(file, "temp_del", H5T_NATIVE_INT32,
                                      space1d, H5P_DEFAULT, dcpl_plain,
                                      H5P_DEFAULT));
    ASSERT_TRUE(dset_del.valid());
  }

  ASSERT_GE(H5Ldelete(file, "temp_del", H5P_DEFAULT), 0);

  // 5. Retrieve file image and reopen via a new Core FAPL to trigger
  //    H5D__compact_init on disk open.
  ASSERT_GE(H5Fflush(file, H5F_SCOPE_GLOBAL), 0);
  const ssize_t image_size = H5Fget_file_image(file, nullptr, 0);
  ASSERT_GT(image_size, 0);
  std::vector<uint8_t> image(static_cast<size_t>(image_size), 0);
  ASSERT_EQ(H5Fget_file_image(file, image.data(), image.size()), image_size);
  file.reset();

  ScopedPlist reopen_fapl = MakeCoreFapl();
  ASSERT_TRUE(reopen_fapl.valid());
  ASSERT_GE(H5Pset_file_image(reopen_fapl, image.data(), image.size()), 0);
  ScopedFile reopened(H5Fopen("compact_reopen.h5", H5F_ACC_RDONLY, reopen_fapl));
  ASSERT_TRUE(reopened.valid());

  ScopedDataset reopened_vec(H5Dopen2(reopened, "vec", H5P_DEFAULT));
  ASSERT_TRUE(reopened_vec.valid());
  std::array<int32_t, 6> got = {};
  ASSERT_GE(H5Dread(reopened_vec, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, got.data()),
            0);
  EXPECT_THAT(got, ElementsAre(777, 11, 777, 22, 777, 33));

  ScopedDataset reopened_scalar(H5Dopen2(reopened, "scalar", H5P_DEFAULT));
  ASSERT_TRUE(reopened_scalar.valid());
  double got_scalar = -1.0;
  ASSERT_GE(H5Dread(reopened_scalar, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, &got_scalar),
            0);
  EXPECT_DOUBLE_EQ(got_scalar, 42.0);
}

TEST(H5DcompactTest, CompactFillSupportsVariableLengthFillValues) {
  ScopedFile file = CreateInMemoryFile("compact_vlen_fill.h5");
  ASSERT_TRUE(file.valid());

  // 1. VLEN integer compact dataset with a custom fill value triggers
  //    H5D__fill_refill_vl inside H5D__compact_fill().
  ScopedType vl_int(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vl_int.valid());

  std::array<int32_t, 3> fill_elts = {5, 6, 7};
  hvl_t fill_vl{fill_elts.size(), fill_elts.data()};

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_layout(dcpl, H5D_COMPACT), 0);
  ASSERT_GE(H5Pset_fill_value(dcpl, vl_int, &fill_vl), 0);
  ASSERT_GE(H5Pset_fill_time(dcpl, H5D_FILL_TIME_ALLOC), 0);

  constexpr std::array<hsize_t, 1> kDims = {3};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  ScopedDataset dset(H5Dcreate2(file, "vlen_fill", vl_int, space, H5P_DEFAULT,
                                dcpl, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  std::array<hvl_t, 3> read_vl{};
  ASSERT_GE(H5Dread(dset, vl_int, H5S_ALL, H5S_ALL, H5P_DEFAULT, read_vl.data()),
            0);
  absl::Cleanup reclaim = [&] {
    H5Treclaim(vl_int, space, H5P_DEFAULT, read_vl.data());
  };

  for (size_t i = 0; i < read_vl.size(); ++i) {
    ASSERT_EQ(read_vl[i].len, 3u);
    const auto* vals = static_cast<const int32_t*>(read_vl[i].p);
    EXPECT_EQ(vals[0], 5);
    EXPECT_EQ(vals[1], 6);
    EXPECT_EQ(vals[2], 7);
  }
}

TEST(H5DcompactTest, ConstructRejectsExtendibleAndOversizedCompactDatasets) {
  ScopedFile file = CreateInMemoryFile("compact_construct_err.h5");
  ASSERT_TRUE(file.valid());

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_layout(dcpl, H5D_COMPACT), 0);

  ScopedErrorSilence silence;

  // 1. Extendible dataspace (max_dims > curr_dims) must fail in construct().
  constexpr std::array<hsize_t, 1> kDims = {4};
  constexpr std::array<hsize_t, 1> kMaxDims = {8};
  ScopedSpace ext_space(H5Screate_simple(1, kDims.data(), kMaxDims.data()));
  ASSERT_TRUE(ext_space.valid());
  EXPECT_EQ(H5Dcreate2(file, "extendible", H5T_NATIVE_INT32, ext_space,
                       H5P_DEFAULT, dcpl, H5P_DEFAULT),
            H5I_INVALID_HID);

  // 2. Non-early allocation time is invalid for compact datasets.
  ScopedPlist late_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(late_dcpl.valid());
  ASSERT_GE(H5Pset_layout(late_dcpl, H5D_COMPACT), 0);
  ASSERT_GE(H5Pset_alloc_time(late_dcpl, H5D_ALLOC_TIME_LATE), 0);
  ScopedSpace small_space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(small_space.valid());
  EXPECT_EQ(H5Dcreate2(file, "late_alloc", H5T_NATIVE_INT32, small_space,
                       H5P_DEFAULT, late_dcpl, H5P_DEFAULT),
            H5I_INVALID_HID);

  // 3. Compact payload exceeding the 64 KiB object header message limit fails.
  constexpr std::array<hsize_t, 1> kTooBigDims = {20000};
  ScopedSpace big_space(H5Screate_simple(1, kTooBigDims.data(), nullptr));
  ASSERT_TRUE(big_space.valid());
  EXPECT_EQ(H5Dcreate2(file, "too_big", H5T_NATIVE_INT32, big_space,
                       H5P_DEFAULT, dcpl, H5P_DEFAULT),
            H5I_INVALID_HID);
}

TEST(H5DcompactTest, IoInitReportsSelectionIoDisabledForCompact) {
  ScopedFile file = CreateInMemoryFile("compact_sel_io.h5");
  ASSERT_TRUE(file.valid());

  constexpr std::array<hsize_t, 1> kDims = {4};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_layout(dcpl, H5D_COMPACT), 0);

  ScopedDataset dset(H5Dcreate2(file, "compact", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, dcpl, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  // Request selection I/O on a DXPL; H5D__compact_io_init must disable it and
  // record H5D_SEL_IO_NOT_CONTIGUOUS_OR_CHUNKED_DATASET.
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ASSERT_GE(H5Pset_selection_io(dxpl, H5D_SELECTION_IO_MODE_ON), 0);

  constexpr std::array<int32_t, 4> kInput = {10, 20, 30, 40};
  ASSERT_GE(
      H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl, kInput.data()),
      0);

  uint32_t write_cause = 0;
  ASSERT_GE(H5Pget_no_selection_io_cause(dxpl, &write_cause), 0);
  EXPECT_NE(write_cause & H5D_SEL_IO_NOT_CONTIGUOUS_OR_CHUNKED_DATASET, 0u);

  std::array<int32_t, 4> output = {-1, -1, -1, -1};
  ASSERT_GE(
      H5Dread(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl, output.data()),
      0);
  uint32_t read_cause = 0;
  ASSERT_GE(H5Pget_no_selection_io_cause(dxpl, &read_cause), 0);
  EXPECT_NE(read_cause & H5D_SEL_IO_NOT_CONTIGUOUS_OR_CHUNKED_DATASET, 0u);
  EXPECT_THAT(output, ElementsAre(10, 20, 30, 40));
}

TEST(H5DcompactTest, CustomMemManageVfdExercisesCtlMemCopyAndFailurePaths) {
  MemManageVfdEnv env;
  g_memmanage_env = &env;
  absl::Cleanup reset_env = [] { g_memmanage_env = nullptr; };

  const H5FD_class_t cls = MakeMemManageVfdClass();
  const hid_t drv_id = H5FDregister(&cls);
  ASSERT_NE(drv_id, H5I_INVALID_HID);
  // Keep H5FDunregister outer to all file/fapl/dataset guards so driver refs
  // drop to zero before unregistering.
  absl::Cleanup unregister_drv = [drv_id] { H5FDunregister(drv_id); };

  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_driver(fapl, drv_id, nullptr), 0);

    ScopedFile file(
        H5Fcreate("memmanage_compact.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(file.valid());

    constexpr std::array<hsize_t, 1> kDims = {4};
    ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());

    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_layout(dcpl, H5D_COMPACT), 0);

    ScopedDataset dset(H5Dcreate2(file, "mm", H5T_NATIVE_INT32, space,
                                  H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    // 1. Successful write and read via H5D__compact_iovv_memmanage_cb.
    constexpr std::array<int32_t, 4> kWriteData = {3, 6, 9, 12};
    ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kWriteData.data()),
              0);
    EXPECT_GT(env.mem_copy_calls, 0);
    const int calls_after_write = env.mem_copy_calls;

    std::array<int32_t, 4> read_data = {-1, -1, -1, -1};
    ASSERT_GE(H5Dread(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      read_data.data()),
              0);
    EXPECT_GT(env.mem_copy_calls, calls_after_write);
    EXPECT_THAT(read_data, ElementsAre(3, 6, 9, 12));

    // 2. Inject H5FD_ctl failures to cover error propagation in
    //    H5D__compact_iovv_memmanage_cb, writevv, and readvv.
    env.fail_mem_copy = true;
    ScopedErrorSilence silence;
    EXPECT_LT(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kWriteData.data()),
              0);
    EXPECT_LT(H5Dread(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      read_data.data()),
              0);
  }
}

TEST(H5DcompactTest, CorruptFileImageCompactSizeMismatchFailsInit) {
  // 1. Create a v1 object header file (default EARLIEST low bound) containing a
  //    4 x uint32 compact dataset with distinctive sentinel payload bytes.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());

  ScopedFile file(
      H5Fcreate("compact_corrupt.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(file.valid());

  constexpr std::array<hsize_t, 1> kDims = {4};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_layout(dcpl, H5D_COMPACT), 0);

  constexpr std::array<uint32_t, 4> kMagic = {
      0xDEADBEEFu, 0xCAFEBABEu, 0x12345678u, 0x9ABCDEF0u};
  {
    ScopedDataset dset(H5Dcreate2(file, "bad", H5T_STD_U32LE, space,
                                  H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_UINT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kMagic.data()),
              0);
  }
  ASSERT_GE(H5Fflush(file, H5F_SCOPE_GLOBAL), 0);

  const ssize_t image_len = H5Fget_file_image(file, nullptr, 0);
  ASSERT_GT(image_len, 0);
  std::vector<uint8_t> image(static_cast<size_t>(image_len), 0);
  ASSERT_EQ(H5Fget_file_image(file, image.data(), image.size()), image_len);
  file.reset();

  // 2. Locate the 16-byte compact payload in the v3 layout message
  //    ({0x03, 0x00, 0x10, 0x00, <16 bytes>}) and corrupt the stored compact
  //    size from 16 to 8 so H5D__compact_init detects compact.size != data_size.
  constexpr std::array<uint8_t, 20> kExpectedNeedle = {
      0x03, 0x00, 0x10, 0x00,
      0xEF, 0xBE, 0xAD, 0xDE, 0xBE, 0xBA, 0xFE, 0xCA,
      0x78, 0x56, 0x34, 0x12, 0xF0, 0xDE, 0xBC, 0x9A};
  auto it = std::search(image.begin(), image.end(), kExpectedNeedle.begin(),
                        kExpectedNeedle.end());
  ASSERT_NE(it, image.end());
  it[2] = 0x08;
  it[3] = 0x00;

  ScopedPlist corrupt_fapl = MakeCoreFapl();
  ASSERT_TRUE(corrupt_fapl.valid());
  ASSERT_GE(H5Pset_file_image(corrupt_fapl, image.data(), image.size()), 0);
  ScopedFile corrupt_file(
      H5Fopen("compact_corrupt_open.h5", H5F_ACC_RDONLY, corrupt_fapl));
  ASSERT_TRUE(corrupt_file.valid());

  ScopedErrorSilence silence;
  EXPECT_EQ(H5Dopen2(corrupt_file, "bad", H5P_DEFAULT), H5I_INVALID_HID);
}

TEST(H5DcompactTest, CompactCopyHandlesClosedOpenVlenAndReferences) {
  ScopedFile src = CreateInMemoryFile("compact_copy_src.h5");
  ASSERT_TRUE(src.valid());
  ScopedFile dst = CreateInMemoryFile("compact_copy_dst.h5");
  ASSERT_TRUE(dst.valid());

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_layout(dcpl, H5D_COMPACT), 0);

  constexpr std::array<hsize_t, 1> kDims = {2};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  // 1. Copy a closed plain compact dataset.
  constexpr std::array<int32_t, 2> kPlain = {100, 200};
  {
    ScopedDataset dset(H5Dcreate2(src, "closed_plain", H5T_NATIVE_INT32, space,
                                  H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kPlain.data()),
              0);
  }
  ASSERT_GE(
      H5Ocopy(src, "closed_plain", dst, "closed_plain", H5P_DEFAULT, H5P_DEFAULT),
      0);

  // 2. Copy an open modified compact dataset while shared_fo != nullptr.
  {
    ScopedDataset open_dset(H5Dcreate2(src, "open_plain", H5T_NATIVE_INT32,
                                       space, H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(open_dset.valid());
    constexpr std::array<int32_t, 2> kUpdated = {333, 444};
    ASSERT_GE(H5Dwrite(open_dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, kUpdated.data()),
              0);
    ASSERT_GE(
        H5Ocopy(src, "open_plain", dst, "open_plain", H5P_DEFAULT, H5P_DEFAULT),
        0);

    ScopedDataset dst_dset(H5Dopen2(dst, "open_plain", H5P_DEFAULT));
    ASSERT_TRUE(dst_dset.valid());
    std::array<int32_t, 2> got = {};
    ASSERT_GE(H5Dread(dst_dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      got.data()),
              0);
    EXPECT_THAT(got, ElementsAre(333, 444));
  }

  // 3. Copy a VLEN compact dataset across files.
  {
    ScopedType vl_int(H5Tvlen_create(H5T_NATIVE_INT32));
    ASSERT_TRUE(vl_int.valid());
    std::array<int32_t, 2> a = {7, 8};
    std::array<int32_t, 3> b = {9, 10, 11};
    std::array<hvl_t, 2> fill_val = {
        hvl_t{a.size(), a.data()},
        hvl_t{b.size(), b.data()},
    };
    ScopedPlist vl_dcpl(H5Pcopy(dcpl));
    ASSERT_TRUE(vl_dcpl.valid());
    ASSERT_GE(H5Pset_fill_value(vl_dcpl, vl_int, &fill_val[0]), 0);

    ScopedDataset vl_dset(H5Dcreate2(src, "vlen_dset", vl_int, space,
                                     H5P_DEFAULT, vl_dcpl, H5P_DEFAULT));
    ASSERT_TRUE(vl_dset.valid());
    ASSERT_GE(H5Dwrite(vl_dset, vl_int, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       fill_val.data()),
              0);
  }
  ASSERT_GE(
      H5Ocopy(src, "vlen_dset", dst, "vlen_dset", H5P_DEFAULT, H5P_DEFAULT),
      0);

  // 4. Copy an object-reference compact dataset: within the same file, across
  //    files with expand_ref=false (zeroed), and across files with
  //    H5O_COPY_EXPAND_REFERENCE_FLAG.
  constexpr std::array<hsize_t, 1> kRefTargetDims = {2};
  ScopedSpace target_space(H5Screate_simple(1, kRefTargetDims.data(), nullptr));
  ASSERT_TRUE(target_space.valid());
  {
    ScopedDataset target(H5Dcreate2(src, "ref_target", H5T_NATIVE_INT32,
                                    target_space, H5P_DEFAULT, dcpl,
                                    H5P_DEFAULT));
    ASSERT_TRUE(target.valid());
    constexpr std::array<int32_t, 2> kTargetVals = {91, 92};
    ASSERT_GE(H5Dwrite(target, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kTargetVals.data()),
              0);
  }

  constexpr std::array<hsize_t, 1> kOne = {1};
  ScopedSpace ref_space(H5Screate_simple(1, kOne.data(), nullptr));
  ASSERT_TRUE(ref_space.valid());
  H5R_ref_t src_ref{};
  ASSERT_GE(H5Rcreate_object(src, "/ref_target", H5P_DEFAULT, &src_ref), 0);
  absl::Cleanup destroy_src_ref = [&] { H5Rdestroy(&src_ref); };

  {
    ScopedDataset ref_dset(H5Dcreate2(src, "refs", H5T_STD_REF, ref_space,
                                      H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(ref_dset.valid());
    ASSERT_GE(
        H5Dwrite(ref_dset, H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT, &src_ref),
        0);
  }

  // Same-file copy preserves reference verbatim.
  ASSERT_GE(
      H5Ocopy(src, "refs", src, "refs_same_file", H5P_DEFAULT, H5P_DEFAULT), 0);

  // Cross-file copy without expand flag resets reference to zeros.
  ASSERT_GE(
      H5Ocopy(src, "refs", dst, "refs_zeroed", H5P_DEFAULT, H5P_DEFAULT), 0);
  {
    ScopedDataset zero_dset(H5Dopen2(dst, "refs_zeroed", H5P_DEFAULT));
    ASSERT_TRUE(zero_dset.valid());
    H5R_ref_t zero_ref{};
    ASSERT_GE(H5Dread(zero_dset, H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      &zero_ref),
              0);
    absl::Cleanup destroy_zero_ref = [&] { H5Rdestroy(&zero_ref); };
    ScopedErrorSilence silence;
    EXPECT_EQ(H5Ropen_object(&zero_ref, H5P_DEFAULT, H5P_DEFAULT),
              H5I_INVALID_HID);
  }

  // Cross-file copy with H5O_COPY_EXPAND_REFERENCE_FLAG copies target object.
  ScopedPlist ocpypl(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpypl.valid());
  ASSERT_GE(H5Pset_copy_object(ocpypl, H5O_COPY_EXPAND_REFERENCE_FLAG), 0);
  ASSERT_GE(H5Ocopy(src, "refs", dst, "refs_expanded", ocpypl, H5P_DEFAULT), 0);

  ScopedDataset exp_dset(H5Dopen2(dst, "refs_expanded", H5P_DEFAULT));
  ASSERT_TRUE(exp_dset.valid());
  H5R_ref_t dst_ref{};
  ASSERT_GE(
      H5Dread(exp_dset, H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT, &dst_ref),
      0);
  absl::Cleanup destroy_dst_ref = [&] { H5Rdestroy(&dst_ref); };

  ScopedDataset deref_target(
      H5Ropen_object(&dst_ref, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(deref_target.valid());
  std::array<int32_t, 2> deref_vals = {};
  ASSERT_GE(H5Dread(deref_target, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, deref_vals.data()),
            0);
  EXPECT_THAT(deref_vals, ElementsAre(91, 92));
}

TEST(H5DcompactTest, InternalConstructVersionBoundsAndReadOnlyFlushError) {
  ScopedFile rw_file = CreateInMemoryFile("compact_internal.h5");
  ASSERT_TRUE(rw_file.valid());

  constexpr std::array<hsize_t, 1> kDims = {4};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_layout(dcpl, H5D_COMPACT), 0);

  // 1. Exercise legacy layout version (< H5O_LAYOUT_VERSION_3) upgrade and
  //    out-of-bounds error branches inside H5D__compact_construct().
  {
    ScopedDataset dset_id(H5Dcreate2(rw_file, "dset", H5T_NATIVE_INT32, space,
                                     H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(dset_id.valid());

    auto* f = static_cast<H5F_t*>(H5VL_object_verify(rw_file, H5I_FILE));
    auto* dset = static_cast<H5D_t*>(H5VL_object_verify(dset_id, H5I_DATASET));
    ASSERT_NE(f, nullptr);
    ASSERT_NE(dset, nullptr);

    const H5F_libver_t orig_low = f->shared->low_bound;
    const H5F_libver_t orig_high = f->shared->high_bound;
    const unsigned orig_ver = dset->shared->layout.version;
    absl::Cleanup restore_bounds = [&] {
      f->shared->low_bound = orig_low;
      f->shared->high_bound = orig_high;
      dset->shared->layout.version = orig_ver;
    };

    ScopedApiContext ctx;
    dset->shared->layout.version = H5O_LAYOUT_VERSION_1;
    f->shared->low_bound = H5F_LIBVER_V18;
    f->shared->high_bound = H5F_LIBVER_LATEST;
    ASSERT_GE(H5D_LOPS_COMPACT->construct(f, dset), 0);
    EXPECT_EQ(dset->shared->layout.version, H5O_LAYOUT_VERSION_3);

    // Trigger version > H5O_layout_ver_bounds[high_bound] check.
    dset->shared->layout.version = H5O_LAYOUT_VERSION_1;
    f->shared->low_bound = H5F_LIBVER_V110;
    f->shared->high_bound = H5F_LIBVER_EARLIEST;
    ScopedErrorSilence silence;
    EXPECT_LT(H5D_LOPS_COMPACT->construct(f, dset), 0);
  }

  // 2. Reopen read-only and force a dirty compact flush failure to verify
  //    H5D__compact_flush() restores the dirty flag on error.
  ASSERT_GE(H5Fflush(rw_file, H5F_SCOPE_GLOBAL), 0);
  const ssize_t img_len = H5Fget_file_image(rw_file, nullptr, 0);
  ASSERT_GT(img_len, 0);
  std::vector<uint8_t> image(static_cast<size_t>(img_len), 0);
  ASSERT_EQ(H5Fget_file_image(rw_file, image.data(), image.size()), img_len);
  rw_file.reset();

  ScopedPlist ro_fapl = MakeCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl, image.data(), image.size()), 0);
  ScopedFile ro_file(H5Fopen("compact_ro.h5", H5F_ACC_RDONLY, ro_fapl));
  ASSERT_TRUE(ro_file.valid());

  ScopedDataset ro_dset_id(H5Dopen2(ro_file, "dset", H5P_DEFAULT));
  ASSERT_TRUE(ro_dset_id.valid());
  auto* ro_dset =
      static_cast<H5D_t*>(H5VL_object_verify(ro_dset_id, H5I_DATASET));
  ASSERT_NE(ro_dset, nullptr);

  ro_dset->shared->layout.storage.u.compact.dirty = true;
  absl::Cleanup clear_dirty = [&] {
    ro_dset->shared->layout.storage.u.compact.dirty = false;
  };

  ScopedApiContext ctx;
  ScopedErrorSilence silence;
  EXPECT_LT(H5D_LOPS_COMPACT->flush(ro_dset), 0);
  EXPECT_TRUE(ro_dset->shared->layout.storage.u.compact.dirty);
}

herr_t FailMemToDstVlenConv(hid_t /*src_id*/, hid_t /*dst_id*/, H5T_cdata_t* cdata,
                            size_t /*nelmts*/, size_t /*buf_stride*/,
                            size_t /*bkg_stride*/, void* /*buf*/, void* /*bkg*/,
                            hid_t /*dxpl_id*/) {
  if (cdata->command == H5T_CONV_INIT) {
    cdata->need_bkg = H5T_BKG_NO;
    return 0;
  }
  if (cdata->command == H5T_CONV_FREE) {
    return 0;
  }
  if (cdata->command == H5T_CONV_CONV) {
    return -1;
  }
  return 0;
}

TEST(H5DcompactTest, InternalInitFillAndCopyErrorPaths) {
  ScopedFile src_file = CreateInMemoryFile("compact_err_src.h5");
  ASSERT_TRUE(src_file.valid());
  ScopedFile dst_file = CreateInMemoryFile("compact_err_dst.h5");
  ASSERT_TRUE(dst_file.valid());

  auto* f_src = static_cast<H5F_t*>(H5VL_object_verify(src_file, H5I_FILE));
  auto* f_dst = static_cast<H5F_t*>(H5VL_object_verify(dst_file, H5I_FILE));
  ASSERT_NE(f_src, nullptr);
  ASSERT_NE(f_dst, nullptr);

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_layout(dcpl, H5D_COMPACT), 0);

  constexpr std::array<hsize_t, 1> kDims4 = {4};
  ScopedSpace space4(H5Screate_simple(1, kDims4.data(), nullptr));
  ASSERT_TRUE(space4.valid());

  // 1. Exercise H5D__compact_init() error branches: zero dt_size, negative
  //    dataspace nelem, multiplication overflow, and compact.size mismatch.
  {
    ScopedDataset dset_id(H5Dcreate2(src_file, "init_err", H5T_NATIVE_INT32,
                                     space4, H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(dset_id.valid());
    auto* dset =
        static_cast<H5D_t*>(H5VL_object_verify(dset_id, H5I_DATASET));
    ASSERT_NE(dset, nullptr);

    const size_t orig_dt_size = dset->shared->type->shared->size;
    const hsize_t orig_nelem = dset->shared->space->extent.nelem;
    const size_t orig_compact_size =
        dset->shared->layout.storage.u.compact.size;
    absl::Cleanup restore_init = [&] {
      dset->shared->type->shared->size = orig_dt_size;
      dset->shared->space->extent.nelem = orig_nelem;
      dset->shared->layout.storage.u.compact.size = orig_compact_size;
    };

    ScopedApiContext ctx;
    ScopedErrorSilence silence;

    dset->shared->layout.storage.u.compact.size = 4;
    EXPECT_LT(H5D_LOPS_COMPACT->init(f_src, dset, H5P_DEFAULT, true), 0);
    dset->shared->layout.storage.u.compact.size = orig_compact_size;

    dset->shared->type->shared->size = 0;
    EXPECT_LT(H5D_LOPS_COMPACT->init(f_src, dset, H5P_DEFAULT, true), 0);

    dset->shared->type->shared->size = (static_cast<size_t>(1) << 62) + 1;
    EXPECT_LT(H5D_LOPS_COMPACT->init(f_src, dset, H5P_DEFAULT, true), 0);
    dset->shared->type->shared->size = orig_dt_size;

    dset->shared->space->extent.nelem = static_cast<hsize_t>(-1);
    EXPECT_LT(H5D_LOPS_COMPACT->init(f_src, dset, H5P_DEFAULT, true), 0);
    dset->shared->space->extent.nelem = orig_nelem;
  }

  // 2. Exercise H5D__compact_fill() error paths (corrupt VLEN fill value in
  //    H5D__fill_refill_vl and missing tpath in H5D__fill_init) plus VLEN
  //    H5D__compact_copy() error paths (dt_src size=0, nelmts=0, corrupt
  //    src->mem conversion, and unconvertible compound opaque member).
  constexpr std::array<hsize_t, 1> kDims1 = {1};
  ScopedSpace space1(H5Screate_simple(1, kDims1.data(), nullptr));
  ASSERT_TRUE(space1.valid());

  {
    ScopedType vl_int(H5Tvlen_create(H5T_NATIVE_INT32));
    ASSERT_TRUE(vl_int.valid());
    std::array<int32_t, 2> fill_elems = {12, 34};
    const hvl_t fill_vl = {fill_elems.size(), fill_elems.data()};
    ScopedPlist vl_dcpl(H5Pcopy(dcpl));
    ASSERT_TRUE(vl_dcpl.valid());
    ASSERT_GE(H5Pset_fill_value(vl_dcpl, vl_int, &fill_vl), 0);

    ScopedDataset vl_id(H5Dcreate2(src_file, "vl_fill_copy", vl_int, space1,
                                   H5P_DEFAULT, vl_dcpl, H5P_DEFAULT));
    ASSERT_TRUE(vl_id.valid());
    auto* vl_dset = static_cast<H5D_t*>(H5VL_object_verify(vl_id, H5I_DATASET));
    ASSERT_NE(vl_dset, nullptr);

    // 2a. Corrupt VLEN global-heap reference bytes (keeping the small
    //     4-byte sequence length intact so HDF5 doesn't allocate gigabytes) so
    //     H5D__fill_refill_vl fails cleanly.
    ASSERT_NE(vl_dset->shared->dcpl_cache.fill.buf, nullptr);
    ASSERT_GT(vl_dset->shared->dcpl_cache.fill.size, 4);
    const auto fill_len =
        static_cast<size_t>(vl_dset->shared->dcpl_cache.fill.size);
    std::vector<uint8_t> saved_fill(fill_len, 0);
    std::memcpy(saved_fill.data(), vl_dset->shared->dcpl_cache.fill.buf,
                fill_len);
    {
      absl::Cleanup restore_fill = [&] {
        std::memcpy(vl_dset->shared->dcpl_cache.fill.buf, saved_fill.data(),
                    fill_len);
      };
      std::memset(static_cast<uint8_t*>(vl_dset->shared->dcpl_cache.fill.buf) +
                      4,
                  0xFF, fill_len - 4);
      ScopedApiContext ctx;
      ScopedErrorSilence silence;
      EXPECT_LT(H5D__compact_fill(vl_dset), 0);
    }

    // 2b. H5D__compact_copy() with src_dt_size == 0, nelmts == 0, and corrupt
    //     on-disk VLEN heap reference (seq_len=1, bogus heap addr/index).
    std::array<uint8_t, 64> dst_buf = {};
    H5O_storage_compact_t dst_store{};
    dst_store.buf = dst_buf.data();
    dst_store.size = 16;

    std::array<uint8_t, 64> bad_src_buf{};
    bad_src_buf[0] = 1;  // Small valid seq_len (little-endian uint32).
    std::memset(bad_src_buf.data() + 4, 0xFE, bad_src_buf.size() - 4);
    H5O_storage_compact_t src_store{};
    src_store.buf = bad_src_buf.data();
    src_store.size = 16;

    H5O_copy_t cpy_info{};
    ScopedApiContext ctx;
    ScopedErrorSilence silence;

    // Force src_dt_size == 0 temporarily.
    const size_t orig_vl_size = vl_dset->shared->type->shared->size;
    {
      absl::Cleanup restore_vl_size = [&] {
        vl_dset->shared->type->shared->size = orig_vl_size;
      };
      vl_dset->shared->type->shared->size = 0;
      EXPECT_LT(H5D__compact_copy(f_src, &src_store, f_dst, &dst_store,
                                  vl_dset->shared->type, &cpy_info),
                0);
    }

    // Force nelmts == 0 (storage_src->size < src_dt_size).
    src_store.size = 4;
    EXPECT_LT(H5D__compact_copy(f_src, &src_store, f_dst, &dst_store,
                                vl_dset->shared->type, &cpy_info),
              0);

    // Corrupt VLEN global heap address triggers H5T_convert(src->mem) error.
    src_store.size = 16;
    EXPECT_LT(H5D__compact_copy(f_src, &src_store, f_dst, &dst_store,
                                vl_dset->shared->type, &cpy_info),
              0);
  }

  // 3. Compound with VLEN + OPAQUE(force_conv=true) triggers tpath lookup
  //    failure inside both H5D__fill_init (via H5D__compact_fill) and
  //    H5D__compact_copy().
  {
    ScopedType vl_int(H5Tvlen_create(H5T_NATIVE_INT32));
    ASSERT_TRUE(vl_int.valid());
    ScopedType opq(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(opq.valid());
    ASSERT_GE(H5Tset_tag(opq, "compact_unconv_opaque"), 0);

    struct VlAndOpaque {
      hvl_t vl;
      std::array<uint8_t, 4> opq;
    };
    ScopedType cmp_dt(H5Tcreate(H5T_COMPOUND, sizeof(VlAndOpaque)));
    ASSERT_TRUE(cmp_dt.valid());
    ASSERT_GE(
        H5Tinsert(cmp_dt, "vl", HOFFSET(VlAndOpaque, vl), vl_int), 0);
    ASSERT_GE(
        H5Tinsert(cmp_dt, "opq", HOFFSET(VlAndOpaque, opq), opq), 0);

    std::array<int32_t, 1> elem = {77};
    VlAndOpaque fill_cmp{{elem.size(), elem.data()}, {1, 2, 3, 4}};
    ScopedPlist cmp_dcpl(H5Pcopy(dcpl));
    ASSERT_TRUE(cmp_dcpl.valid());
    ASSERT_GE(H5Pset_fill_value(cmp_dcpl, cmp_dt, &fill_cmp), 0);

    ScopedDataset cmp_id(H5Dcreate2(src_file, "cmp_vl", cmp_dt, space1,
                                    H5P_DEFAULT, cmp_dcpl, H5P_DEFAULT));
    ASSERT_TRUE(cmp_id.valid());
    auto* cmp_dset =
        static_cast<H5D_t*>(H5VL_object_verify(cmp_id, H5I_DATASET));
    ASSERT_NE(cmp_dset, nullptr);
    ASSERT_EQ(cmp_dset->shared->type->shared->u.compnd.nmembs, 2u);

    H5T_t* memb1_type = cmp_dset->shared->type->shared->u.compnd.memb[1].type;
    ASSERT_NE(memb1_type, nullptr);
    const bool orig_top_fc = cmp_dset->shared->type->shared->force_conv;
    const bool orig_fc = memb1_type->shared->force_conv;
    char* const orig_tag = memb1_type->shared->u.opaque.tag;
    char alt_tag[] = "compact_unconv_opaque_miss";
    absl::Cleanup restore_fc = [&] {
      cmp_dset->shared->type->shared->force_conv = orig_top_fc;
      memb1_type->shared->force_conv = orig_fc;
      memb1_type->shared->u.opaque.tag = orig_tag;
    };
    cmp_dset->shared->type->shared->force_conv = true;
    memb1_type->shared->force_conv = true;
    memb1_type->shared->u.opaque.tag = alt_tag;

    std::array<uint8_t, 64> dst_buf = {};
    H5O_storage_compact_t dst_store{};
    dst_store.buf = dst_buf.data();
    dst_store.size = cmp_dset->shared->layout.storage.u.compact.size;
    H5O_copy_t cpy_info{};

    ScopedApiContext ctx;
    ScopedErrorSilence silence;
    EXPECT_LT(H5D__compact_fill(cmp_dset), 0);
    EXPECT_LT(
        H5D__compact_copy(f_src, &cmp_dset->shared->layout.storage.u.compact,
                          f_dst, &dst_store, cmp_dset->shared->type, &cpy_info),
        0);
  }

  // 4. Register temporary hard VLEN converter (mem [16B] -> 4B-addr dst [12B])
  //    to hit H5D__compact_copy()'s mem->dst error branch cleanly with 0 leaks.
  {
    ScopedPlist dst4_fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(dst4_fcpl.valid());
    ASSERT_GE(H5Pset_sizes(dst4_fcpl, 4, 4), 0);
    ScopedPlist dst4_fapl = MakeCoreFapl();
    ASSERT_TRUE(dst4_fapl.valid());
    ScopedFile dst4_file(
        H5Fcreate("compact_err_dst4.h5", H5F_ACC_TRUNC, dst4_fcpl, dst4_fapl));
    ASSERT_TRUE(dst4_file.valid());
    auto* f_dst4 = static_cast<H5F_t*>(H5VL_object_verify(dst4_file, H5I_FILE));
    ASSERT_NE(f_dst4, nullptr);

    ScopedType opq(H5Tcreate(H5T_OPAQUE, 1));
    ASSERT_TRUE(opq.valid());
    ASSERT_GE(H5Tset_tag(opq, "fail_mem_dst_vlen"), 0);
    ScopedType vl_opq(H5Tvlen_create(opq));
    ASSERT_TRUE(vl_opq.valid());
    ScopedType disk4_vl(H5Tcopy(vl_opq));
    ASSERT_TRUE(disk4_vl.valid());
    auto* disk4_dt =
        static_cast<H5T_t*>(H5I_object_verify(disk4_vl, H5I_DATATYPE));
    ASSERT_NE(disk4_dt, nullptr);
    {
      ScopedApiContext ctx;
      ASSERT_GE(H5T_set_loc(disk4_dt, H5F_VOL_OBJ(f_dst4), H5T_LOC_DISK), 0);
    }

    const hvl_t nil_fill = {0, nullptr};
    ScopedPlist nil_dcpl(H5Pcopy(dcpl));
    ASSERT_TRUE(nil_dcpl.valid());
    ASSERT_GE(H5Pset_fill_value(nil_dcpl, vl_opq, &nil_fill), 0);

    ScopedDataset nil_id(H5Dcreate2(src_file, "nil_vl", vl_opq, space1,
                                    H5P_DEFAULT, nil_dcpl, H5P_DEFAULT));
    ASSERT_TRUE(nil_id.valid());
    auto* nil_dset =
        static_cast<H5D_t*>(H5VL_object_verify(nil_id, H5I_DATASET));
    ASSERT_NE(nil_dset, nullptr);

    ASSERT_GE(H5Tregister(H5T_PERS_HARD, "fail_mem_dst_vlen_conv", vl_opq,
                          disk4_vl, FailMemToDstVlenConv),
              0);
    absl::Cleanup unregister_conv = [&] {
      H5Tunregister(H5T_PERS_HARD, "fail_mem_dst_vlen_conv", vl_opq, disk4_vl,
                    FailMemToDstVlenConv);
    };

    std::array<uint8_t, 64> dst_buf = {};
    H5O_storage_compact_t dst_store{};
    dst_store.buf = dst_buf.data();
    dst_store.size = nil_dset->shared->layout.storage.u.compact.size;
    H5O_copy_t cpy_info{};

    ScopedApiContext ctx;
    ScopedErrorSilence silence;
    EXPECT_LT(H5D__compact_copy(f_src, &nil_dset->shared->layout.storage.u.compact,
                                f_dst4, &dst_store, nil_dset->shared->type,
                                &cpy_info),
              0);
  }

  // 5. Invalid deprecated object reference triggers H5O_copy_expand_ref error
  //    inside H5D__compact_copy() without invoking H5O__layout_copy_file().
  {
    ScopedDataset bad_ref_id(H5Dcreate2(src_file, "bad_obj_ref",
                                        H5T_STD_REF_OBJ, space1, H5P_DEFAULT,
                                        dcpl, H5P_DEFAULT));
    ASSERT_TRUE(bad_ref_id.valid());
    // HADDR_UNDEF fails inside H5O__copy_expand_ref_object1 before touching
    // cpy_info->map_list.
    const hobj_ref_t bogus_ref = static_cast<hobj_ref_t>(HADDR_UNDEF);
    ASSERT_GE(H5Dwrite(bad_ref_id, H5T_STD_REF_OBJ, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, &bogus_ref),
              0);
    auto* bad_ref_dset =
        static_cast<H5D_t*>(H5VL_object_verify(bad_ref_id, H5I_DATASET));
    ASSERT_NE(bad_ref_dset, nullptr);

    std::array<uint8_t, 64> dst_buf = {};
    H5O_storage_compact_t dst_store{};
    dst_store.buf = dst_buf.data();
    dst_store.size = bad_ref_dset->shared->layout.storage.u.compact.size;
    H5O_copy_t cpy_info{};
    cpy_info.expand_ref = true;

    ScopedApiContext ctx;
    ScopedErrorSilence silence;
    EXPECT_LT(H5D__compact_copy(f_src,
                                &bad_ref_dset->shared->layout.storage.u.compact,
                                f_dst, &dst_store, bad_ref_dset->shared->type,
                                &cpy_info),
              0);
  }
}

}  // namespace
}  // namespace hdf5
