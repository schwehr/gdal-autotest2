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
// Tests contiguous dataset storage allocation, fill values (multi-buffer and
// VLEN), bounds checking, selection I/O (single and multi-dataset), data sieve
// buffer read/write state machines, non-sieve VFD I/O, corrupt file images, and
// contiguous copying in third_party/hdf5/src/H5Dcontig.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Dcontig.c

#include "third_party/hdf5/src/hdf5.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

// `:hdf5` is built with -DNDEBUG, which omits debug fields from metadata
// structures pulled by H5private.h and H5Dpkg.h.
#ifndef NDEBUG
#define H5DCONTIG_TEST_RESTORE_UNDEF_NDEBUG 1
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
#ifdef H5DCONTIG_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5DCONTIG_TEST_RESTORE_UNDEF_NDEBUG
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

// Configurable in-memory VFD for exercising both sieve-disabled VFD paths and
// injected read/write/get_eoa I/O failures in H5Dcontig.c.
struct ConfigurableVfdEnv {
  std::map<std::string, std::vector<uint8_t>> files;
  bool advertise_sieve = false;
  bool fail_read = false;
  bool fail_write = false;
  bool fail_get_eoa = false;
  int read_calls = 0;
  int write_calls = 0;
  unsigned long next_fileno = 1;
};

ConfigurableVfdEnv* g_cfg_vfd_env = nullptr;

struct ConfigurableVfdFile {
  H5FD_t pub{};
  std::string name;
  haddr_t eoa = 0;
};

H5FD_t* CfgOpen(const char* name, unsigned flags, hid_t /*fapl_id*/,
                haddr_t maxaddr) {
  if (g_cfg_vfd_env == nullptr || name == nullptr || *name == '\0') {
    return nullptr;
  }
  auto& files = g_cfg_vfd_env->files;
  if (flags & H5F_ACC_TRUNC) {
    files[name].clear();
  } else if (!(flags & H5F_ACC_CREAT) && !files.contains(name)) {
    return nullptr;
  } else {
    files[name];
  }
  auto* file = new ConfigurableVfdFile();
  file->pub.fileno = g_cfg_vfd_env->next_fileno++;
  file->pub.maxaddr = maxaddr;
  file->name = name;
  file->eoa = 0;
  return reinterpret_cast<H5FD_t*>(file);
}

herr_t CfgClose(H5FD_t* _file) {
  delete reinterpret_cast<ConfigurableVfdFile*>(_file);
  return 0;
}

int CfgCmp(const H5FD_t* f1, const H5FD_t* f2) {
  const auto* a = reinterpret_cast<const ConfigurableVfdFile*>(f1);
  const auto* b = reinterpret_cast<const ConfigurableVfdFile*>(f2);
  if (a->name < b->name) return -1;
  if (a->name > b->name) return 1;
  return 0;
}

herr_t CfgQuery(const H5FD_t* /*file*/, unsigned long* flags) {
  *flags = (g_cfg_vfd_env != nullptr && g_cfg_vfd_env->advertise_sieve)
               ? H5FD_FEAT_DATA_SIEVE
               : 0UL;
  return 0;
}

haddr_t CfgGetEoa(const H5FD_t* _file, H5FD_mem_t /*type*/) {
  if (g_cfg_vfd_env != nullptr && g_cfg_vfd_env->fail_get_eoa) {
    return HADDR_UNDEF;
  }
  return reinterpret_cast<const ConfigurableVfdFile*>(_file)->eoa;
}

herr_t CfgSetEoa(H5FD_t* _file, H5FD_mem_t /*type*/, haddr_t addr) {
  auto* file = reinterpret_cast<ConfigurableVfdFile*>(_file);
  file->eoa = addr;
  auto& buf = g_cfg_vfd_env->files[file->name];
  if (buf.size() < static_cast<size_t>(addr)) {
    buf.resize(static_cast<size_t>(addr), 0);
  }
  return 0;
}

haddr_t CfgGetEof(const H5FD_t* _file, H5FD_mem_t /*type*/) {
  const auto* file = reinterpret_cast<const ConfigurableVfdFile*>(_file);
  const auto it = g_cfg_vfd_env->files.find(file->name);
  const haddr_t size = it == g_cfg_vfd_env->files.end()
                           ? 0
                           : static_cast<haddr_t>(it->second.size());
  return std::max(file->eoa, size);
}

herr_t CfgRead(H5FD_t* _file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
               haddr_t addr, size_t size, void* buf) {
  if (g_cfg_vfd_env == nullptr) {
    return -1;
  }
  ++g_cfg_vfd_env->read_calls;
  if (g_cfg_vfd_env->fail_read) {
    return -1;
  }
  auto* file = reinterpret_cast<ConfigurableVfdFile*>(_file);
  const auto& data = g_cfg_vfd_env->files[file->name];
  std::memset(buf, 0, size);
  const size_t offset = static_cast<size_t>(addr);
  if (offset < data.size()) {
    const size_t avail = std::min(size, data.size() - offset);
    std::memcpy(buf, data.data() + offset, avail);
  }
  return 0;
}

herr_t CfgWrite(H5FD_t* _file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                haddr_t addr, size_t size, const void* buf) {
  if (g_cfg_vfd_env == nullptr) {
    return -1;
  }
  ++g_cfg_vfd_env->write_calls;
  if (g_cfg_vfd_env->fail_write) {
    return -1;
  }
  auto* file = reinterpret_cast<ConfigurableVfdFile*>(_file);
  auto& data = g_cfg_vfd_env->files[file->name];
  const size_t offset = static_cast<size_t>(addr);
  if (offset + size > data.size()) {
    data.resize(offset + size, 0);
  }
  std::memcpy(data.data() + offset, buf, size);
  return 0;
}

H5FD_class_t MakeConfigurableVfdClass() {
  H5FD_class_t cls{};
  cls.version = H5FD_CLASS_VERSION;
  cls.value = static_cast<H5FD_class_value_t>(302);
  cls.name = "test_configurable_contig_vfd";
  cls.maxaddr = (static_cast<haddr_t>(1) << 30) - 1;
  cls.fc_degree = H5F_CLOSE_WEAK;
  cls.open = CfgOpen;
  cls.close = CfgClose;
  cls.cmp = CfgCmp;
  cls.query = CfgQuery;
  cls.get_eoa = CfgGetEoa;
  cls.set_eoa = CfgSetEoa;
  cls.get_eof = CfgGetEof;
  cls.read = CfgRead;
  cls.write = CfgWrite;
  const H5FD_mem_t fl_map[H5FD_MEM_NTYPES] = H5FD_FLMAP_SINGLE;
  std::memcpy(cls.fl_map, fl_map, sizeof(cls.fl_map));
  return cls;
}

std::vector<uint8_t> EncodeU64Le(uint64_t val) {
  std::vector<uint8_t> out(8, 0);
  for (size_t i = 0; i < 8; ++i) {
    out[i] = static_cast<uint8_t>((val >> (8 * i)) & 0xFFu);
  }
  return out;
}

// Helper to write a hyperslab slice into a 1D dataset of int32_t using the
// specified DXPL.
herr_t WriteIntSlice(hid_t dset_id, hid_t dxpl_id, hsize_t start, hsize_t count,
                     const int32_t* data) {
  ScopedSpace fspace(H5Dget_space(dset_id));
  if (!fspace.valid()) return -1;
  if (H5Sselect_hyperslab(fspace, H5S_SELECT_SET, &start, nullptr, &count,
                          nullptr) < 0) {
    return -1;
  }
  ScopedSpace mspace(H5Screate_simple(1, &count, nullptr));
  if (!mspace.valid()) return -1;
  return H5Dwrite(dset_id, H5T_NATIVE_INT32, mspace, fspace, dxpl_id, data);
}

herr_t ReadIntSlice(hid_t dset_id, hid_t dxpl_id, hsize_t start, hsize_t count,
                    int32_t* data) {
  ScopedSpace fspace(H5Dget_space(dset_id));
  if (!fspace.valid()) return -1;
  if (H5Sselect_hyperslab(fspace, H5S_SELECT_SET, &start, nullptr, &count,
                          nullptr) < 0) {
    return -1;
  }
  ScopedSpace mspace(H5Screate_simple(1, &count, nullptr));
  if (!mspace.valid()) return -1;
  return H5Dread(dset_id, H5T_NATIVE_INT32, mspace, fspace, dxpl_id, data);
}

TEST(H5DcontigTest, CreateLateEarlyFillMultiBufferVlenAndDelete) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  // Small sieve buffer so we hit both dset_size < sieve_buf_size and >= paths.
  ASSERT_GE(H5Pset_sieve_buf_size(fapl, 64), 0);

  ScopedFile file(
      H5Fcreate("contig_alloc_fill.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(file.valid());

  // 1. Late-allocated contiguous dataset with a custom fill value and a small
  //    temp buffer DXPL so H5D__contig_fill() executes multiple loop chunks.
  ScopedPlist dcpl_late(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl_late.valid());
  ASSERT_GE(H5Pset_alloc_time(dcpl_late, H5D_ALLOC_TIME_LATE), 0);
  ASSERT_GE(H5Pset_fill_time(dcpl_late, H5D_FILL_TIME_ALLOC), 0);
  const int32_t kFill = 555;
  ASSERT_GE(H5Pset_fill_value(dcpl_late, H5T_NATIVE_INT32, &kFill), 0);

  constexpr std::array<hsize_t, 1> kDimsLarge = {128};  // 512B >= 64B sieve
  ScopedSpace space_large(H5Screate_simple(1, kDimsLarge.data(), nullptr));
  ASSERT_TRUE(space_large.valid());

  {
    ScopedDataset dset_late(H5Dcreate2(file, "late_fill", H5T_NATIVE_INT32,
                                       space_large, H5P_DEFAULT, dcpl_late,
                                       H5P_DEFAULT));
    ASSERT_TRUE(dset_late.valid());

    H5D_space_status_t status = H5D_SPACE_STATUS_ALLOCATED;
    ASSERT_GE(H5Dget_space_status(dset_late, &status), 0);
    EXPECT_EQ(status, H5D_SPACE_STATUS_NOT_ALLOCATED);
    EXPECT_EQ(H5Dget_offset(dset_late), HADDR_UNDEF);

    // Partial write with a 128-byte temp buffer triggers H5D__contig_alloc()
    // and multi-iteration H5D__contig_fill().
    ScopedPlist dxpl_small(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl_small.valid());
    ASSERT_GE(H5Pset_buffer(dxpl_small, 128, nullptr, nullptr), 0);
    const int32_t kOne = 999;
    ASSERT_GE(WriteIntSlice(dset_late, dxpl_small, 10, 1, &kOne), 0);

    ASSERT_GE(H5Dget_space_status(dset_late, &status), 0);
    EXPECT_EQ(status, H5D_SPACE_STATUS_ALLOCATED);
    EXPECT_NE(H5Dget_offset(dset_late), HADDR_UNDEF);

    std::vector<int32_t> got(128, 0);
    ASSERT_GE(H5Dread(dset_late, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, got.data()),
              0);
    EXPECT_EQ(got[0], 555);
    EXPECT_EQ(got[10], 999);
    EXPECT_EQ(got[127], 555);
  }

  // 2. Small early-allocated dataset (< 64B sieve_buf_size) deleted later.
  constexpr std::array<hsize_t, 1> kDimsSmall = {4};
  ScopedSpace space_small(H5Screate_simple(1, kDimsSmall.data(), nullptr));
  ASSERT_TRUE(space_small.valid());
  ScopedPlist dcpl_early(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl_early.valid());
  ASSERT_GE(H5Pset_alloc_time(dcpl_early, H5D_ALLOC_TIME_EARLY), 0);
  {
    ScopedDataset small(H5Dcreate2(file, "small_del", H5T_NATIVE_INT32,
                                   space_small, H5P_DEFAULT, dcpl_early,
                                   H5P_DEFAULT));
    ASSERT_TRUE(small.valid());
    H5D_space_status_t status = H5D_SPACE_STATUS_NOT_ALLOCATED;
    ASSERT_GE(H5Dget_space_status(small, &status), 0);
    EXPECT_EQ(status, H5D_SPACE_STATUS_ALLOCATED);
  }
  ASSERT_GE(H5Ldelete(file, "small_del", H5P_DEFAULT), 0);

  // 3. VLEN dataset with a non-default VL fill value and ALLOC fill time hits
  //    H5D__fill_refill_vl inside H5D__contig_fill().
  ScopedType vl_int(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vl_int.valid());
  std::array<int32_t, 2> kVlFillElts = {21, 22};
  hvl_t kVlFill{kVlFillElts.size(), kVlFillElts.data()};
  ScopedPlist dcpl_vl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl_vl.valid());
  ASSERT_GE(H5Pset_alloc_time(dcpl_vl, H5D_ALLOC_TIME_EARLY), 0);
  ASSERT_GE(H5Pset_fill_value(dcpl_vl, vl_int, &kVlFill), 0);
  ASSERT_GE(H5Pset_fill_time(dcpl_vl, H5D_FILL_TIME_ALLOC), 0);

  ScopedDataset vl_dset(H5Dcreate2(file, "vl_contig", vl_int, space_small,
                                   H5P_DEFAULT, dcpl_vl, H5P_DEFAULT));
  ASSERT_TRUE(vl_dset.valid());
  std::array<hvl_t, 4> vl_got{};
  ASSERT_GE(H5Dread(vl_dset, vl_int, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    vl_got.data()),
            0);
  absl::Cleanup reclaim = [&] {
    H5Treclaim(vl_int, space_small, H5P_DEFAULT, vl_got.data());
  };
  ASSERT_EQ(vl_got[0].len, 2u);
  EXPECT_EQ(static_cast<const int32_t*>(vl_got[0].p)[1], 22);

  // 4. Extendible contiguous non-external dataset fails construction.
  constexpr std::array<hsize_t, 1> kMaxDims = {10};
  ScopedSpace ext_space(
      H5Screate_simple(1, kDimsSmall.data(), kMaxDims.data()));
  ASSERT_TRUE(ext_space.valid());
  ScopedErrorSilence silence;
  EXPECT_EQ(H5Dcreate2(file, "ext_err", H5T_NATIVE_INT32, ext_space,
                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);

  // 5. Reopen persisted image to exercise H5D__contig_init() on both sizes.
  ASSERT_GE(H5Fflush(file, H5F_SCOPE_GLOBAL), 0);
  const ssize_t image_len = H5Fget_file_image(file, nullptr, 0);
  ASSERT_GT(image_len, 0);
  std::vector<uint8_t> image(static_cast<size_t>(image_len), 0);
  ASSERT_EQ(H5Fget_file_image(file, image.data(), image.size()), image_len);
  vl_dset.reset();
  file.reset();

  ScopedPlist reopen_fapl = MakeCoreFapl();
  ASSERT_TRUE(reopen_fapl.valid());
  ASSERT_GE(H5Pset_sieve_buf_size(reopen_fapl, 64), 0);
  ASSERT_GE(H5Pset_file_image(reopen_fapl, image.data(), image.size()), 0);
  ScopedFile reopened(H5Fopen("contig_reopen.h5", H5F_ACC_RDONLY, reopen_fapl));
  ASSERT_TRUE(reopened.valid());

  ScopedDataset reopened_large(H5Dopen2(reopened, "late_fill", H5P_DEFAULT));
  ASSERT_TRUE(reopened_large.valid());
  ScopedDataset reopened_vl(H5Dopen2(reopened, "vl_contig", H5P_DEFAULT));
  ASSERT_TRUE(reopened_vl.valid());
}

TEST(H5DcontigTest, SelectionIoSingleMultiTypeConvAndDisableCauses) {
  // 1. Standard Core VFD file: test selection I/O ON with direct I/O, zero-
  //    element selection, type conversion + data transform, and multi-dataset
  //    I/O.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_sieve_buf_size(fapl, 64), 0);
    ScopedFile file(
        H5Fcreate("contig_sel_io.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(file.valid());

    constexpr std::array<hsize_t, 1> kDims = {8};
    ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());

    ScopedDataset d1(H5Dcreate2(file, "d1", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d1.valid());
    ScopedDataset d2(H5Dcreate2(file, "d2", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d2.valid());

    ScopedPlist dxpl_on(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl_on.valid());
    ASSERT_GE(H5Pset_selection_io(dxpl_on, H5D_SELECTION_IO_MODE_ON), 0);

    // Direct single-dataset selection write & read (no type conversion).
    std::array<int32_t, 8> vals1 = {1, 2, 3, 4, 5, 6, 7, 8};
    std::array<int32_t, 8> read1 = {};
    ASSERT_GE(H5Dwrite(d1, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl_on,
                       vals1.data()),
              0);
    ASSERT_GE(
        H5Dread(d1, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl_on, read1.data()),
        0);
    EXPECT_THAT(read1, ElementsAre(1, 2, 3, 4, 5, 6, 7, 8));

    // Empty (H5Sselect_none) write & read exercises nelmts == 0 path.
    ScopedSpace empty_space(H5Scopy(space));
    ASSERT_TRUE(empty_space.valid());
    ASSERT_GE(H5Sselect_none(empty_space), 0);
    ASSERT_GE(H5Dwrite(d1, H5T_NATIVE_INT32, empty_space, empty_space, dxpl_on,
                       vals1.data()),
              0);
    ASSERT_GE(H5Dread(d1, H5T_NATIVE_INT32, empty_space, empty_space, dxpl_on,
                      read1.data()),
              0);

    // Single-dataset selection I/O with type conversion and data transform
    // hits H5D_INIT_PIECE_TCONV and deferred selection I/O else branches.
    ScopedPlist dxpl_xform(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl_xform.valid());
    ASSERT_GE(H5Pset_selection_io(dxpl_xform, H5D_SELECTION_IO_MODE_ON), 0);
    ASSERT_GE(H5Pset_data_transform(dxpl_xform, "x + 10"), 0);

    std::array<double, 8> d_in = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
    std::array<double, 8> d_out = {};
    ASSERT_GE(H5Dwrite(d2, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, dxpl_xform,
                       d_in.data()),
              0);
    ASSERT_GE(
        H5Dread(d2, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, dxpl_xform, d_out.data()),
        0);
    EXPECT_DOUBLE_EQ(d_out[0], 21.0);
    EXPECT_DOUBLE_EQ(d_out[7], 28.0);

    // Multi-dataset H5Dwrite_multi / H5Dread_multi with selection I/O OFF
    // exercises H5D__contig_mdio_init(), and with ON exercises mdset selection
    // I/O arrays in H5D__contig_read() / H5D__contig_write().
    ScopedPlist dxpl_off(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl_off.valid());
    ASSERT_GE(H5Pset_selection_io(dxpl_off, H5D_SELECTION_IO_MODE_OFF), 0);

    std::array<int32_t, 8> multi_w1 = {10, 20, 30, 40, 50, 60, 70, 80};
    std::array<int32_t, 8> multi_w2 = {11, 22, 33, 44, 55, 66, 77, 88};
    std::array<hid_t, 2> dsets = {d1.get(), d2.get()};
    std::array<hid_t, 2> types = {H5T_NATIVE_INT32, H5T_NATIVE_INT32};
    std::array<hid_t, 2> mspaces = {H5S_ALL, H5S_ALL};
    std::array<hid_t, 2> fspaces = {H5S_ALL, H5S_ALL};
    std::array<const void*, 2> wbufs = {multi_w1.data(), multi_w2.data()};
    ASSERT_GE(H5Dwrite_multi(2, dsets.data(), types.data(), mspaces.data(),
                             fspaces.data(), dxpl_off, wbufs.data()),
              0);

    std::array<int32_t, 8> multi_r1 = {};
    std::array<int32_t, 8> multi_r2 = {};
    std::array<void*, 2> rbufs = {multi_r1.data(), multi_r2.data()};
    ASSERT_GE(H5Dread_multi(2, dsets.data(), types.data(), mspaces.data(),
                            fspaces.data(), dxpl_off, rbufs.data()),
              0);
    EXPECT_THAT(multi_r1, ElementsAre(10, 20, 30, 40, 50, 60, 70, 80));
    EXPECT_THAT(multi_r2, ElementsAre(11, 22, 33, 44, 55, 66, 77, 88));

    ASSERT_GE(H5Dwrite_multi(2, dsets.data(), types.data(), mspaces.data(),
                             fspaces.data(), dxpl_on, wbufs.data()),
              0);
    std::fill(multi_r1.begin(), multi_r1.end(), 0);
    std::fill(multi_r2.begin(), multi_r2.end(), 0);
    ASSERT_GE(H5Dread_multi(2, dsets.data(), types.data(), mspaces.data(),
                            fspaces.data(), dxpl_on, rbufs.data()),
              0);
    EXPECT_THAT(multi_r1, ElementsAre(10, 20, 30, 40, 50, 60, 70, 80));
    EXPECT_THAT(multi_r2, ElementsAre(11, 22, 33, 44, 55, 66, 77, 88));

    // Sieve buffer conflict: perform a partial write with selection I/O OFF
    // so sieve_buf is allocated and dirty, then verify selection I/O ON falls
    // back with H5D_SEL_IO_CONTIGUOUS_SIEVE_BUFFER for both read and write.
    const int32_t kPatch = 900;
    ASSERT_GE(WriteIntSlice(d1, dxpl_off, 0, 1, &kPatch), 0);

    ASSERT_GE(H5Dread(d1, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl_on,
                      multi_r1.data()),
              0);
    uint32_t cause = 0;
    ASSERT_GE(H5Pget_no_selection_io_cause(dxpl_on, &cause), 0);
    EXPECT_NE(cause & H5D_SEL_IO_CONTIGUOUS_SIEVE_BUFFER, 0u);

    ASSERT_GE(H5Dwrite(d1, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl_on,
                       multi_w1.data()),
              0);
    cause = 0;
    ASSERT_GE(H5Pget_no_selection_io_cause(dxpl_on, &cause), 0);
    EXPECT_NE(cause & H5D_SEL_IO_CONTIGUOUS_SIEVE_BUFFER, 0u);
    ASSERT_GE(H5Dflush(d1), 0);
  }

  // 2. Page buffer conflict disables selection I/O with
  //    H5D_SEL_IO_PAGE_BUFFER.
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(
        H5Pset_file_space_strategy(fcpl, H5F_FSPACE_STRATEGY_PAGE, false, 1),
        0);

    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_page_buffer_size(fapl, 4096, 50, 50), 0);

    ScopedFile file(
        H5Fcreate("contig_pagebuf.h5", H5F_ACC_TRUNC, fcpl, fapl));
    ASSERT_TRUE(file.valid());

    constexpr std::array<hsize_t, 1> kDims = {4};
    ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(file, "pb", H5T_NATIVE_INT32, space,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    ScopedPlist dxpl_on(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl_on.valid());
    ASSERT_GE(H5Pset_selection_io(dxpl_on, H5D_SELECTION_IO_MODE_ON), 0);

    constexpr std::array<int32_t, 4> kInput = {5, 6, 7, 8};
    ASSERT_GE(
        H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl_on,
                 kInput.data()),
        0);
    uint32_t cause = 0;
    ASSERT_GE(H5Pget_no_selection_io_cause(dxpl_on, &cause), 0);
    EXPECT_NE(cause & H5D_SEL_IO_PAGE_BUFFER, 0u);
  }
}

TEST(H5DcontigTest, SieveBufferReadAndWriteStateMachine) {
  // Use a 32-byte (8 x int32_t) sieve buffer on a 128-byte (32 x int32_t)
  // contiguous dataset with selection I/O disabled so H5D__contig_readvv and
  // writevv drive the full sieve buffer state machine.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_sieve_buf_size(fapl, 32), 0);

  ScopedFile file(
      H5Fcreate("contig_sieve_sm.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(file.valid());

  ScopedPlist dxpl_off(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl_off.valid());
  ASSERT_GE(H5Pset_selection_io(dxpl_off, H5D_SELECTION_IO_MODE_OFF), 0);

  constexpr std::array<hsize_t, 1> kDims = {32};  // 128 bytes
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  // Prepare baseline pattern on disk and close to start with clean sieve_buf.
  std::vector<int32_t> pattern(32, 0);
  for (int32_t i = 0; i < 32; ++i) {
    pattern[static_cast<size_t>(i)] = i * 10;
  }
  {
    ScopedDataset init_dset(H5Dcreate2(file, "sm_read", H5T_NATIVE_INT32, space,
                                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(init_dset.valid());
    ASSERT_GE(H5Dwrite(init_dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl_off,
                       pattern.data()),
              0);
  }

  // 1. Exercise every readvv_sieve_cb branch via a freshly opened handle.
  {
    ScopedDataset rd(H5Dopen2(file, "sm_read", H5P_DEFAULT));
    ASSERT_TRUE(rd.valid());

    // 1a. Initial read > sieve_buf_size (16 ints = 64B > 32B) with sieve_buf
    //     == NULL -> direct block_read.
    std::vector<int32_t> big_buf(16, -1);
    ASSERT_GE(ReadIntSlice(rd, dxpl_off, 0, 16, big_buf.data()), 0);
    EXPECT_EQ(big_buf[15], 150);

    // 1b. Initial read <= sieve_buf_size (2 ints = 8B at elmt 0) allocates
    //     sieve_buf covering [0..8).
    std::array<int32_t, 2> pair = {};
    ASSERT_GE(ReadIntSlice(rd, dxpl_off, 0, 2, pair.data()), 0);
    EXPECT_THAT(pair, ElementsAre(0, 10));

    // 1c. Subsequent read entirely inside [0..8) hits sieve_buf directly.
    ASSERT_GE(ReadIntSlice(rd, dxpl_off, 3, 2, pair.data()), 0);
    EXPECT_THAT(pair, ElementsAre(30, 40));

    // 1d. Write 1 int inside [0..8) -> sieve_dirty = true.
    const int32_t kDirtyA = 777;
    ASSERT_GE(WriteIntSlice(rd, dxpl_off, 2, 1, &kDirtyA), 0);

    // 1e. Large read (40B > 32B) disjoint from [0..8) -> skips dirty flush.
    std::vector<int32_t> disjoint_big(10, -1);
    ASSERT_GE(ReadIntSlice(rd, dxpl_off, 16, 10, disjoint_big.data()), 0);
    EXPECT_EQ(disjoint_big[0], 160);

    // 1f. Large read (40B > 32B) overlapping dirty [0..8) -> flushes sieve
    //     before direct read.
    ASSERT_GE(ReadIntSlice(rd, dxpl_off, 4, 10, disjoint_big.data()), 0);
    EXPECT_EQ(disjoint_big[0], 40);

    // 1g. Make [0..8) dirty again, then read small slice (8B) at elmt 24 ->
    //     flushes dirty sieve and reloads sieve at [24..32).
    const int32_t kDirtyB = 888;
    ASSERT_GE(WriteIntSlice(rd, dxpl_off, 1, 1, &kDirtyB), 0);
    ASSERT_GE(ReadIntSlice(rd, dxpl_off, 24, 2, pair.data()), 0);
    EXPECT_THAT(pair, ElementsAre(240, 250));
  }

  // 2. Exercise writevv_sieve_cb branches on a separate dataset.
  {
    ScopedDataset wr(H5Dcreate2(file, "sm_write", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(wr.valid());

    // 2a. Initial write > sieve_buf_size (16 ints = 64B > 32B) with
    //     sieve_buf == NULL -> direct block_write.
    std::vector<int32_t> big_w(16, 5);
    ASSERT_GE(WriteIntSlice(wr, dxpl_off, 0, 16, big_w.data()), 0);

    // 2b. Initial write <= sieve_buf_size (2 ints = 8B at elmt 10) allocates
    //     sieve_buf covering [10..18) and marks dirty.
    std::array<int32_t, 2> two = {100, 101};
    ASSERT_GE(WriteIntSlice(wr, dxpl_off, 10, 2, two.data()), 0);

    // 2c. Large write (40B > 32B) overlapping dirty [10..18) flushes dirty
    //     sieve, resets sieve_loc = HADDR_UNDEF / sieve_size = 0, and writes.
    std::vector<int32_t> ten(10, 42);
    ASSERT_GE(WriteIntSlice(wr, dxpl_off, 12, 10, ten.data()), 0);

    // 2d. Write at EOF (2 ints at elmt 30 -> sieve_size == len == 8B) skips
    //     pre-read, then prepend adjacent writes at elmt 28 (8B) and elmt 24
    //     (16B) while dirty (hits memmove prepend path).
    std::array<int32_t, 2> tail = {300, 301};
    ASSERT_GE(WriteIntSlice(wr, dxpl_off, 30, 2, tail.data()), 0);
    std::array<int32_t, 2> pre1 = {280, 281};
    ASSERT_GE(WriteIntSlice(wr, dxpl_off, 28, 2, pre1.data()), 0);
    std::array<int32_t, 4> pre2 = {240, 241, 242, 243};
    ASSERT_GE(WriteIntSlice(wr, dxpl_off, 24, 4, pre2.data()), 0);

    // 2e. Disjoint write (4 ints at elmt 2) flushes dirty sieve [24..32) and
    //     reloads [2..10).
    std::array<int32_t, 4> mid = {20, 21, 22, 23};
    ASSERT_GE(WriteIntSlice(wr, dxpl_off, 2, 4, mid.data()), 0);

    // Flush dirty sieve and flush clean sieve.
    ASSERT_GE(H5Dflush(wr), 0);
    ASSERT_GE(H5Dflush(wr), 0);

    // Verify full round-trip values across all segments.
    std::vector<int32_t> got(32, -1);
    ASSERT_GE(H5Dread(wr, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl_off,
                      got.data()),
              0);
    EXPECT_EQ(got[2], 20);
    EXPECT_EQ(got[10], 100);
    EXPECT_EQ(got[12], 42);
    EXPECT_EQ(got[24], 240);
    EXPECT_EQ(got[28], 280);
    EXPECT_EQ(got[31], 301);
  }
}

TEST(H5DcontigTest, CustomNoSieveAndSieveVfdReadWriteAndErrorPaths) {
  ConfigurableVfdEnv env;
  g_cfg_vfd_env = &env;
  absl::Cleanup reset_env = [] { g_cfg_vfd_env = nullptr; };

  const H5FD_class_t cls = MakeConfigurableVfdClass();
  const hid_t drv_id = H5FDregister(&cls);
  ASSERT_NE(drv_id, H5I_INVALID_HID);
  absl::Cleanup unregister_drv = [drv_id] { H5FDunregister(drv_id); };

  ScopedPlist dxpl_off(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl_off.valid());
  ASSERT_GE(H5Pset_selection_io(dxpl_off, H5D_SELECTION_IO_MODE_OFF), 0);

  ScopedPlist dxpl_on(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl_on.valid());
  ASSERT_GE(H5Pset_selection_io(dxpl_on, H5D_SELECTION_IO_MODE_ON), 0);

  // 1. Plain non-sieve VFD path: exercises H5D__contig_readvv_cb,
  //    H5D__contig_writevv_cb, and selection I/O read/write failure paths.
  {
    env.advertise_sieve = false;
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_driver(fapl, drv_id, nullptr), 0);

    ScopedFile file(
        H5Fcreate("nosieve_contig.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(file.valid());

    constexpr std::array<hsize_t, 1> kDims = {8};
    ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(file, "plain", H5T_NATIVE_INT32, space,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    std::array<int32_t, 8> wdata = {10, 11, 12, 13, 14, 15, 16, 17};
    ASSERT_GE(
        H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl_off,
                 wdata.data()),
        0);

    std::array<int32_t, 8> rdata = {};
    ASSERT_GE(
        H5Dread(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl_off,
                rdata.data()),
        0);
    EXPECT_THAT(rdata, ElementsAre(10, 11, 12, 13, 14, 15, 16, 17));

    ScopedErrorSilence silence;
    env.fail_write = true;
    EXPECT_LT(
        H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl_off,
                 wdata.data()),
        0);
    EXPECT_LT(
        H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl_on,
                 wdata.data()),
        0);
    env.fail_write = false;

    env.fail_read = true;
    EXPECT_LT(
        H5Dread(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl_off,
                rdata.data()),
        0);
    EXPECT_LT(
        H5Dread(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl_on,
                rdata.data()),
        0);
    env.fail_read = false;
  }

  // 2. Sieve-enabled VFD error injection across readvv_sieve_cb,
  //    writevv_sieve_cb, and H5D__contig_flush().
  {
    env.advertise_sieve = true;
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_driver(fapl, drv_id, nullptr), 0);
    ASSERT_GE(H5Pset_sieve_buf_size(fapl, 32), 0);

    ScopedFile file(
        H5Fcreate("sieve_err_contig.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(file.valid());

    constexpr std::array<hsize_t, 1> kDims = {32};
    ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());

    std::vector<int32_t> full_data(32, 3);
    {
      ScopedDataset init_ds(H5Dcreate2(file, "ds", H5T_NATIVE_INT32, space,
                                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(init_ds.valid());
      ASSERT_GE(H5Dwrite(init_ds, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl_off,
                         full_data.data()),
                0);
    }

    // Reopen fresh so sieve_buf is initially NULL, then inject errors cleanly.
    ASSERT_GE(H5Fflush(file, H5F_SCOPE_GLOBAL), 0);
    ScopedDataset ds(H5Dopen2(file, "ds", H5P_DEFAULT));
    ASSERT_TRUE(ds.valid());

    ScopedErrorSilence silence;
    std::vector<int32_t> tmp(16, 0);

    // 2a. Fail read on initial len > sieve_buf_size and initial len <= sieve.
    env.fail_read = true;
    EXPECT_LT(ReadIntSlice(ds, dxpl_off, 0, 16, tmp.data()), 0);
    EXPECT_LT(ReadIntSlice(ds, dxpl_off, 0, 2, tmp.data()), 0);
    env.fail_read = false;

    // 2b. Fail get_eoa when allocating initial sieve_buf on already-allocated
    //     datasets whose sieve buffers are still unallocated (FILL_TIME_NEVER).
    {
      ScopedPlist dcpl_early_nofill(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl_early_nofill.valid());
      ASSERT_GE(H5Pset_alloc_time(dcpl_early_nofill, H5D_ALLOC_TIME_EARLY), 0);
      ASSERT_GE(H5Pset_fill_time(dcpl_early_nofill, H5D_FILL_TIME_NEVER), 0);

      ScopedDataset ds_eoa_r(H5Dcreate2(file, "ds_eoa_r", H5T_NATIVE_INT32,
                                        space, H5P_DEFAULT, dcpl_early_nofill,
                                        H5P_DEFAULT));
      ASSERT_TRUE(ds_eoa_r.valid());
      ScopedDataset ds_eoa_w(H5Dcreate2(file, "ds_eoa_w", H5T_NATIVE_INT32,
                                        space, H5P_DEFAULT, dcpl_early_nofill,
                                        H5P_DEFAULT));
      ASSERT_TRUE(ds_eoa_w.valid());
      env.fail_get_eoa = true;
      EXPECT_LT(ReadIntSlice(ds_eoa_r, dxpl_off, 0, 2, tmp.data()), 0);
      EXPECT_LT(WriteIntSlice(ds_eoa_w, dxpl_off, 0, 2, tmp.data()), 0);
      env.fail_get_eoa = false;
    }

    // 2c. Fail direct write and initial small sieve pre-read on fresh dataset.
    {
      ScopedPlist dcpl_early_nofill(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl_early_nofill.valid());
      ASSERT_GE(H5Pset_alloc_time(dcpl_early_nofill, H5D_ALLOC_TIME_EARLY), 0);
      ASSERT_GE(H5Pset_fill_time(dcpl_early_nofill, H5D_FILL_TIME_NEVER), 0);

      ScopedDataset ds_w(H5Dcreate2(file, "ds_wfail", H5T_NATIVE_INT32, space,
                                    H5P_DEFAULT, dcpl_early_nofill,
                                    H5P_DEFAULT));
      ASSERT_TRUE(ds_w.valid());
      env.fail_write = true;
      EXPECT_LT(WriteIntSlice(ds_w, dxpl_off, 0, 16, tmp.data()), 0);
      env.fail_write = false;

      env.fail_read = true;
      EXPECT_LT(WriteIntSlice(ds_w, dxpl_off, 0, 2, tmp.data()), 0);
      env.fail_read = false;
    }

    // 2d. Drive allocated clean and dirty sieve miss / large overlap errors on
    //     both readvv_sieve_cb and writevv_sieve_cb.
    ASSERT_GE(ReadIntSlice(ds, dxpl_off, 0, 2, tmp.data()), 0);

    env.fail_read = true;
    EXPECT_LT(ReadIntSlice(ds, dxpl_off, 16, 16, tmp.data()), 0);
    EXPECT_LT(ReadIntSlice(ds, dxpl_off, 16, 2, tmp.data()), 0);
    env.fail_read = false;
    // Note: the previous miss read moved sieve_loc to element 16 even though
    // its block read failed, so reload element 0 before forcing another miss.
    ASSERT_GE(ReadIntSlice(ds, dxpl_off, 0, 2, tmp.data()), 0);

    env.fail_get_eoa = true;
    EXPECT_LT(ReadIntSlice(ds, dxpl_off, 16, 2, tmp.data()), 0);
    env.fail_get_eoa = false;
    ASSERT_GE(ReadIntSlice(ds, dxpl_off, 0, 2, tmp.data()), 0);

    env.fail_write = true;
    EXPECT_LT(WriteIntSlice(ds, dxpl_off, 16, 16, tmp.data()), 0);
    env.fail_write = false;

    env.fail_get_eoa = true;
    EXPECT_LT(WriteIntSlice(ds, dxpl_off, 16, 2, tmp.data()), 0);
    env.fail_get_eoa = false;
    ASSERT_GE(ReadIntSlice(ds, dxpl_off, 0, 2, tmp.data()), 0);

    env.fail_read = true;
    EXPECT_LT(WriteIntSlice(ds, dxpl_off, 16, 2, tmp.data()), 0);
    env.fail_read = false;
    ASSERT_GE(ReadIntSlice(ds, dxpl_off, 0, 2, tmp.data()), 0);

    // Dirty sieve at [0..32B) and test every dirty flush failure branch.
    const int32_t kVal = 11;
    ASSERT_GE(WriteIntSlice(ds, dxpl_off, 0, 1, &kVal), 0);
    env.fail_write = true;
    EXPECT_LT(H5Dflush(ds), 0);
    EXPECT_LT(ReadIntSlice(ds, dxpl_off, 4, 16, tmp.data()), 0);
    EXPECT_LT(ReadIntSlice(ds, dxpl_off, 16, 2, tmp.data()), 0);
    EXPECT_LT(WriteIntSlice(ds, dxpl_off, 4, 16, tmp.data()), 0);
    EXPECT_LT(WriteIntSlice(ds, dxpl_off, 16, 2, tmp.data()), 0);
    env.fail_write = false;
    ASSERT_GE(H5Dflush(ds), 0);
  }
}

TEST(H5DcontigTest, CorruptFileImageContigCheckFailsOpen) {
  // 1. Create a v1 OHDR file containing a 4 x uint32 contiguous dataset and
  //    record its on-disk storage address before flushing to a file image.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());

  ScopedFile file(
      H5Fcreate("contig_corrupt.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(file.valid());

  constexpr std::array<hsize_t, 1> kDims = {4};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  haddr_t dset_addr = HADDR_UNDEF;
  {
    ScopedDataset dset(H5Dcreate2(file, "bad_contig", H5T_STD_U32LE, space,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    constexpr std::array<uint32_t, 4> kData = {1, 2, 3, 4};
    ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_UINT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kData.data()),
              0);
    dset_addr = H5Dget_offset(dset);
  }
  ASSERT_NE(dset_addr, HADDR_UNDEF);
  ASSERT_GE(H5Fflush(file, H5F_SCOPE_GLOBAL), 0);

  const ssize_t img_len = H5Fget_file_image(file, nullptr, 0);
  ASSERT_GT(img_len, 0);
  std::vector<uint8_t> base_image(static_cast<size_t>(img_len), 0);
  ASSERT_EQ(H5Fget_file_image(file, base_image.data(), base_image.size()),
            img_len);
  file.reset();

  // 2. Locate v3 contiguous layout message {0x03, 0x01, le64(addr), le64(16)}.
  std::vector<uint8_t> needle = {0x03, 0x01};
  const std::vector<uint8_t> addr_bytes = EncodeU64Le(dset_addr);
  const std::vector<uint8_t> size_bytes = EncodeU64Le(16);
  needle.insert(needle.end(), addr_bytes.begin(), addr_bytes.end());
  needle.insert(needle.end(), size_bytes.begin(), size_bytes.end());

  const auto pos_it = std::search(base_image.begin(), base_image.end(),
                                  needle.begin(), needle.end());
  ASSERT_NE(pos_it, base_image.end());
  const size_t addr_offset =
      static_cast<size_t>(pos_it - base_image.begin()) + 2;

  auto open_and_expect_fail = [&](uint64_t corrupt_addr) {
    std::vector<uint8_t> patched = base_image;
    const std::vector<uint8_t> bad_bytes = EncodeU64Le(corrupt_addr);
    std::memcpy(patched.data() + addr_offset, bad_bytes.data(), 8);

    ScopedPlist cfapl = MakeCoreFapl();
    ASSERT_TRUE(cfapl.valid());
    ASSERT_GE(H5Pset_file_image(cfapl, patched.data(), patched.size()), 0);
    ScopedFile cf(H5Fopen("contig_bad_open.h5", H5F_ACC_RDONLY, cfapl));
    ASSERT_TRUE(cf.valid());

    ScopedErrorSilence silence;
    EXPECT_EQ(H5Dopen2(cf, "bad_contig", H5P_DEFAULT), H5I_INVALID_HID);
  };

  // Off-end-of-file address (addr + 16 > rel_eoa) and uint64 wraparound
  // (addr + 16 <= addr).
  open_and_expect_fail(base_image.size() - 4);
  open_and_expect_fail(std::numeric_limits<uint64_t>::max() - 8);
}

TEST(H5DcontigTest, ContigCopyCoversOpenSieveOver1MbVlenAndReferences) {
  ScopedFile src = CreateInMemoryFile("contig_copy_src.h5");
  ASSERT_TRUE(src.valid());
  ScopedFile dst = CreateInMemoryFile("contig_copy_dst.h5");
  ASSERT_TRUE(dst.valid());

  ScopedPlist dxpl_off(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl_off.valid());
  ASSERT_GE(H5Pset_selection_io(dxpl_off, H5D_SELECTION_IO_MODE_OFF), 0);

  // 1. Copy an open dataset while its dirty sieve buffer covers the dataset,
  //    exercising H5D__contig_is_data_cached() and try_sieve in contig_copy().
  {
    constexpr std::array<hsize_t, 1> kDims = {4};
    ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset open_ds(H5Dcreate2(src, "open_sieve", H5T_NATIVE_INT32, space,
                                     H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(open_ds.valid());
    constexpr std::array<int32_t, 4> kInit = {1, 2, 3, 4};
    ASSERT_GE(H5Dwrite(open_ds, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, kInit.data()),
              0);
    // Partial write with selection I/O OFF keeps updated values in sieve_buf.
    const int32_t kUpdated = 99;
    ASSERT_GE(WriteIntSlice(open_ds, dxpl_off, 0, 1, &kUpdated), 0);

    ASSERT_GE(
        H5Ocopy(src, "open_sieve", dst, "open_sieve", H5P_DEFAULT, H5P_DEFAULT),
        0);

    ScopedDataset copied(H5Dopen2(dst, "open_sieve", H5P_DEFAULT));
    ASSERT_TRUE(copied.valid());
    std::array<int32_t, 4> got = {};
    ASSERT_GE(H5Dread(copied, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      got.data()),
              0);
    EXPECT_THAT(got, ElementsAre(99, 2, 3, 4));
  }

  // 2. Copy >1 MiB contiguous dataset so H5D__contig_copy() loops multiple
  //    times and reduces total_src_nbytes < src_nbytes on the final chunk.
  {
    constexpr hsize_t kOver1MbElmts = (1024 * 1024) / sizeof(int32_t) + 128;
    const std::array<hsize_t, 1> dims = {kOver1MbElmts};
    ScopedSpace space(H5Screate_simple(1, dims.data(), nullptr));
    ASSERT_TRUE(space.valid());
    {
      ScopedDataset big(H5Dcreate2(src, "big_1mb", H5T_NATIVE_INT32, space,
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(big.valid());
      std::vector<int32_t> vals(static_cast<size_t>(kOver1MbElmts), 0);
      vals.front() = 111;
      vals.back() = 222;
      ASSERT_GE(H5Dwrite(big, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                         vals.data()),
                0);
    }
    ASSERT_GE(
        H5Ocopy(src, "big_1mb", dst, "big_1mb", H5P_DEFAULT, H5P_DEFAULT), 0);
  }

  // 3. Copy >1 MiB VLEN contiguous dataset to hit both VLEN conversion setup
  //    and VLEN remainder H5S_set_extent_real() adjustment in H5D__contig_copy.
  {
    ScopedType vl_int(H5Tvlen_create(H5T_NATIVE_INT32));
    ASSERT_TRUE(vl_int.valid());
    constexpr hsize_t kVlenOver1MbCount = 70000;
    const std::array<hsize_t, 1> dims = {kVlenOver1MbCount};
    ScopedSpace space(H5Screate_simple(1, dims.data(), nullptr));
    ASSERT_TRUE(space.valid());

    {
      int32_t dummy_val = 42;
      hvl_t fill_vl{1, &dummy_val};
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl.valid());
      ASSERT_GE(H5Pset_alloc_time(dcpl, H5D_ALLOC_TIME_EARLY), 0);
      ASSERT_GE(H5Pset_fill_value(dcpl, vl_int, &fill_vl), 0);
      ScopedDataset vl_big(H5Dcreate2(src, "vl_1mb", vl_int, space, H5P_DEFAULT,
                                      dcpl, H5P_DEFAULT));
      ASSERT_TRUE(vl_big.valid());
      std::vector<hvl_t> w_vl(static_cast<size_t>(kVlenOver1MbCount), fill_vl);
      ASSERT_GE(H5Dwrite(vl_big, vl_int, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                         w_vl.data()),
                0);
    }
    ASSERT_GE(H5Ocopy(src, "vl_1mb", dst, "vl_1mb", H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  // 4. Copy object-reference contiguous dataset: same file, cross-file zeroed
  //    (expand_ref=false), and cross-file H5O_COPY_EXPAND_REFERENCE_FLAG.
  {
    constexpr std::array<hsize_t, 1> kTwo = {2};
    ScopedSpace tspace(H5Screate_simple(1, kTwo.data(), nullptr));
    ASSERT_TRUE(tspace.valid());
    {
      ScopedDataset target(H5Dcreate2(src, "ref_target", H5T_NATIVE_INT32,
                                      tspace, H5P_DEFAULT, H5P_DEFAULT,
                                      H5P_DEFAULT));
      ASSERT_TRUE(target.valid());
      constexpr std::array<int32_t, 2> kTData = {61, 62};
      ASSERT_GE(H5Dwrite(target, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                         H5P_DEFAULT, kTData.data()),
                0);
    }

    constexpr std::array<hsize_t, 1> kOne = {1};
    ScopedSpace rspace(H5Screate_simple(1, kOne.data(), nullptr));
    ASSERT_TRUE(rspace.valid());
    H5R_ref_t src_ref{};
    ASSERT_GE(H5Rcreate_object(src, "/ref_target", H5P_DEFAULT, &src_ref), 0);
    absl::Cleanup destroy_src_ref = [&] { H5Rdestroy(&src_ref); };

    {
      ScopedDataset rds(H5Dcreate2(src, "refs", H5T_STD_REF, rspace,
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(rds.valid());
      ASSERT_GE(H5Dwrite(rds, H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                         &src_ref),
                0);
    }

    ASSERT_GE(
        H5Ocopy(src, "refs", src, "refs_same", H5P_DEFAULT, H5P_DEFAULT), 0);
    ASSERT_GE(
        H5Ocopy(src, "refs", dst, "refs_zeroed", H5P_DEFAULT, H5P_DEFAULT), 0);

    ScopedPlist ocpypl(H5Pcreate(H5P_OBJECT_COPY));
    ASSERT_TRUE(ocpypl.valid());
    ASSERT_GE(H5Pset_copy_object(ocpypl, H5O_COPY_EXPAND_REFERENCE_FLAG), 0);
    ASSERT_GE(H5Ocopy(src, "refs", dst, "refs_expanded", ocpypl, H5P_DEFAULT),
              0);

    ScopedDataset exp_ds(H5Dopen2(dst, "refs_expanded", H5P_DEFAULT));
    ASSERT_TRUE(exp_ds.valid());
    H5R_ref_t dst_ref{};
    ASSERT_GE(H5Dread(exp_ds, H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      &dst_ref),
              0);
    absl::Cleanup destroy_dst_ref = [&] { H5Rdestroy(&dst_ref); };

    ScopedDataset deref(H5Ropen_object(&dst_ref, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(deref.valid());
    std::array<int32_t, 2> deref_vals = {};
    ASSERT_GE(H5Dread(deref, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      deref_vals.data()),
              0);
    EXPECT_THAT(deref_vals, ElementsAre(61, 62));
  }
}

TEST(H5DcontigTest, InternalVersionBoundsAllocDeleteAndSieveAppend) {
  ScopedFile file = CreateInMemoryFile("contig_internal.h5");
  ASSERT_TRUE(file.valid());

  constexpr std::array<hsize_t, 1> kDims = {16};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  {
    ScopedDataset dset_id(H5Dcreate2(file, "ds", H5T_NATIVE_INT32, space,
                                     H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(dset_id.valid());

    auto* f = static_cast<H5F_t*>(H5VL_object_verify(file, H5I_FILE));
    auto* dset = static_cast<H5D_t*>(H5VL_object_verify(dset_id, H5I_DATASET));
    ASSERT_NE(f, nullptr);
    ASSERT_NE(dset, nullptr);

    // 1. Exercise H5D__contig_is_space_alloc and is_data_cached.
    EXPECT_FALSE(H5D__contig_is_space_alloc(&dset->shared->layout.storage));
    EXPECT_FALSE(H5D__contig_is_data_cached(dset->shared));

    // 2. Exercise legacy layout version construct/init and out-of-bounds error.
    const H5F_libver_t orig_low = f->shared->low_bound;
    const H5F_libver_t orig_high = f->shared->high_bound;
    const unsigned orig_ver = dset->shared->layout.version;
    const hsize_t orig_size = dset->shared->layout.storage.u.contig.size;
    absl::Cleanup restore = [&] {
      f->shared->low_bound = orig_low;
      f->shared->high_bound = orig_high;
      dset->shared->layout.version = orig_ver;
      dset->shared->layout.storage.u.contig.size = orig_size;
    };

    {
      ScopedApiContext ctx;
      dset->shared->layout.version = H5O_LAYOUT_VERSION_1;
      f->shared->low_bound = H5F_LIBVER_V18;
      f->shared->high_bound = H5F_LIBVER_LATEST;
      ASSERT_GE(H5D_LOPS_CONTIG->construct(f, dset), 0);
      EXPECT_EQ(dset->shared->layout.version, H5O_LAYOUT_VERSION_3);

      dset->shared->layout.version = H5O_LAYOUT_VERSION_1;
      dset->shared->layout.storage.u.contig.size = 0;
      ASSERT_GE(H5D_LOPS_CONTIG->init(f, dset, H5P_DEFAULT, true), 0);
      EXPECT_EQ(dset->shared->layout.storage.u.contig.size, 64u);

      // Out-of-bounds check in construct().
      dset->shared->layout.version = H5O_LAYOUT_VERSION_1;
      f->shared->low_bound = H5F_LIBVER_V110;
      f->shared->high_bound = H5F_LIBVER_EARLIEST;
      ScopedErrorSilence silence;
      EXPECT_LT(H5D_LOPS_CONTIG->construct(f, dset), 0);
    }
    std::move(restore).Invoke();

    // 3. Exercise H5D__contig_alloc() failure with overflowed allocation size.
    {
      H5O_storage_contig_t bad_storage{};
      bad_storage.addr = HADDR_UNDEF;
      bad_storage.size = HADDR_MAX;
      ScopedApiContext ctx;
      ScopedErrorSilence silence;
      EXPECT_LT(H5D__contig_alloc(f, &bad_storage), 0);
    }

    // 4. Exercise H5D__contig_mdio_init() when contig_piece_info == nullptr.
    {
      H5D_io_info_t io_info{};
      H5D_dset_io_info_t dinfo{};
      EXPECT_GE(H5D_LOPS_CONTIG->mdio_init(&io_info, &dinfo), 0);
    }

    // 5. Force dirty sieve append branch in H5D__contig_writevv_sieve_cb
    //    (addr == sieve_end && len + sieve_size <= sieve_buf_size).
    std::array<int32_t, 16> zeros = {};
    ASSERT_GE(H5Dwrite(dset_id, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, zeros.data()),
              0);

    ScopedPlist dxpl_off(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl_off.valid());
    ASSERT_GE(H5Pset_selection_io(dxpl_off, H5D_SELECTION_IO_MODE_OFF), 0);
    const std::array<int32_t, 2> head = {11, 22};
    ASSERT_GE(WriteIntSlice(dset_id, dxpl_off, 0, 2, head.data()), 0);
    ASSERT_TRUE(H5D__contig_is_data_cached(dset->shared));

    // Trim cached sieve_size to the 8-byte dirty prefix so the contiguous
    // write starting at byte offset 8 hits `addr == sieve_end` cleanly.
    ASSERT_TRUE(dset->shared->cache.sieve.sieve_dirty);
    ASSERT_GE(dset->shared->cache.sieve.sieve_buf_size, 16u);
    dset->shared->cache.sieve.sieve_size = 8;

    H5D_io_info_t io_info{};
    io_info.f_sh = f->shared;
    io_info.op_type = H5D_IO_OP_WRITE;
    H5D_storage_t store{};
    store.contig.dset_addr = dset->shared->layout.storage.u.contig.addr;
    store.contig.dset_size = dset->shared->layout.storage.u.contig.size;
    const std::array<int32_t, 2> append_vals = {33, 44};
    H5D_dset_io_info_t dset_info{};
    dset_info.dset = dset;
    dset_info.store = &store;
    dset_info.buf.cvp = append_vals.data();

    hsize_t dset_off = 8;
    size_t dset_len = 8;
    size_t dset_curr_seq = 0;
    hsize_t mem_off = 0;
    size_t mem_len = 8;
    size_t mem_curr_seq = 0;
    {
      ScopedApiContext ctx(dxpl_off);
      EXPECT_EQ(
          H5D_LOPS_CONTIG->writevv(&io_info, &dset_info, 1, &dset_curr_seq,
                                   &dset_len, &dset_off, 1, &mem_curr_seq,
                                   &mem_len, &mem_off),
          8);
    }
    EXPECT_EQ(dset->shared->cache.sieve.sieve_size, 16u);
    ASSERT_GE(H5Dflush(dset_id), 0);

    std::array<int32_t, 4> check_vals = {};
    ASSERT_GE(ReadIntSlice(dset_id, dxpl_off, 0, 4, check_vals.data()), 0);
    EXPECT_THAT(check_vals, ElementsAre(11, 22, 33, 44));

    // 6. Exercise H5D__contig_delete() error path when H5MF_xfree rejects a
    //    temporary-address range (>= f->shared->tmp_addr).
    {
      H5O_storage_t bad_del{};
      bad_del.type = H5D_CONTIGUOUS;
      bad_del.u.contig.addr = f->shared->tmp_addr;
      bad_del.u.contig.size = 16;
      ScopedApiContext ctx;
      ScopedErrorSilence silence;
      EXPECT_LT(H5D__contig_delete(f, &bad_del), 0);
    }
  }
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

TEST(H5DcontigTest, InternalCheckConstructFillIoInitAndCopyErrors) {
  ScopedFile src_file = CreateInMemoryFile("contig_int_src.h5");
  ASSERT_TRUE(src_file.valid());
  ScopedFile dst_file = CreateInMemoryFile("contig_int_dst.h5");
  ASSERT_TRUE(dst_file.valid());

  auto* f_src = static_cast<H5F_t*>(H5VL_object_verify(src_file, H5I_FILE));
  auto* f_dst = static_cast<H5F_t*>(H5VL_object_verify(dst_file, H5I_FILE));
  ASSERT_NE(f_src, nullptr);
  ASSERT_NE(f_dst, nullptr);

  ScopedPlist dcpl_early(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl_early.valid());
  ASSERT_GE(H5Pset_alloc_time(dcpl_early, H5D_ALLOC_TIME_EARLY), 0);
  ASSERT_GE(H5Pset_fill_time(dcpl_early, H5D_FILL_TIME_ALLOC), 0);

  constexpr std::array<hsize_t, 1> kDims16 = {16};
  ScopedSpace space16(H5Screate_simple(1, kDims16.data(), nullptr));
  ASSERT_TRUE(space16.valid());

  // 1. Exercise H5D__contig_check(), H5D__contig_construct(),
  //    H5D__contig_init(), H5D__contig_fill() nelem error, and
  //    H5D__contig_may_use_select_io() non-contig layout_ops fallback.
  {
    ScopedDataset u8_id(H5Dcreate2(src_file, "u8_err", H5T_NATIVE_UINT8,
                                   space16, H5P_DEFAULT, dcpl_early,
                                   H5P_DEFAULT));
    ASSERT_TRUE(u8_id.valid());
    auto* dset = static_cast<H5D_t*>(H5VL_object_verify(u8_id, H5I_DATASET));
    ASSERT_NE(dset, nullptr);

    const size_t orig_dt_size = dset->shared->type->shared->size;
    const hsize_t orig_nelem = dset->shared->space->extent.nelem;
    const haddr_t orig_addr = dset->shared->layout.storage.u.contig.addr;
    const unsigned orig_ver = dset->shared->layout.version;
    absl::Cleanup restore_u8 = [&] {
      dset->shared->type->shared->size = orig_dt_size;
      dset->shared->space->extent.nelem = orig_nelem;
      dset->shared->layout.storage.u.contig.addr = orig_addr;
      dset->shared->layout.version = orig_ver;
    };

    ScopedApiContext ctx;
    ScopedErrorSilence silence;

    // dt_size == 0 in check() and construct().
    dset->shared->type->shared->size = 0;
    EXPECT_LT(H5D__contig_check(f_src, &dset->shared->layout,
                                H5S_GET_EXTENT(dset->shared->space),
                                dset->shared->type),
              0);
    EXPECT_LT(H5D_LOPS_CONTIG->construct(f_src, dset), 0);

    // Multiplication overflow in check() and construct().
    dset->shared->type->shared->size = (static_cast<size_t>(1) << 62) + 1;
    EXPECT_LT(H5D__contig_check(f_src, &dset->shared->layout,
                                H5S_GET_EXTENT(dset->shared->space),
                                dset->shared->type),
              0);
    EXPECT_LT(H5D_LOPS_CONTIG->construct(f_src, dset), 0);
    dset->shared->type->shared->size = orig_dt_size;

    // Overflowing addr + data_size and past-EOA addr in check() and init().
    dset->shared->layout.storage.u.contig.addr = HADDR_MAX - 8;
    EXPECT_LT(H5D_LOPS_CONTIG->init(f_src, dset, H5P_DEFAULT, true), 0);
    const haddr_t rel_eoa = H5F_get_eoa(f_src, H5FD_MEM_DRAW);
    ASSERT_NE(rel_eoa, HADDR_UNDEF);
    dset->shared->layout.storage.u.contig.addr = rel_eoa;
    EXPECT_LT(H5D__contig_check(f_src, &dset->shared->layout,
                                H5S_GET_EXTENT(dset->shared->space),
                                dset->shared->type),
              0);

    // Negative dataspace nelem in construct(), fill(), and legacy init().
    dset->shared->layout.storage.u.contig.addr = orig_addr;
    dset->shared->space->extent.nelem = static_cast<hsize_t>(-1);
    EXPECT_LT(H5D_LOPS_CONTIG->construct(f_src, dset), 0);
    EXPECT_LT(H5D__contig_fill(dset), 0);
    dset->shared->layout.storage.u.contig.addr = HADDR_UNDEF;
    dset->shared->layout.version = H5O_LAYOUT_VERSION_1;
    EXPECT_LT(H5D_LOPS_CONTIG->init(f_src, dset, H5P_DEFAULT, true), 0);
    dset->shared->layout.storage.u.contig.addr = orig_addr;
    dset->shared->layout.version = orig_ver;
    dset->shared->space->extent.nelem = orig_nelem;

    // Non-contiguous layout_ops inside H5D__contig_io_init disables select_io.
    H5D_io_info_t io_info{};
    io_info.f_sh = f_src->shared;
    io_info.op_type = H5D_IO_OP_READ;
    io_info.use_select_io = H5D_SELECTION_IO_MODE_ON;
    H5D_storage_t store{};
    H5D_dset_io_info_t dinfo{};
    dinfo.dset = dset;
    dinfo.file_space = dset->shared->space;
    dinfo.mem_space = dset->shared->space;
    dinfo.store = &store;
    dinfo.layout_ops.readvv = nullptr;
    dinfo.nelmts = 0;
    EXPECT_GE(H5D_LOPS_CONTIG->io_init(&io_info, &dinfo), 0);
    EXPECT_EQ(io_info.use_select_io, H5D_SELECTION_IO_MODE_OFF);
    EXPECT_NE(io_info.no_selection_io_cause &
                  H5D_SEL_IO_NOT_CONTIGUOUS_OR_CHUNKED_DATASET,
              0u);
  }

  // 2. ConfigurableVfd tests: H5D__contig_check get_eoa fail, H5D__contig_fill
  //    write fail, and H5D__contig_copy read / write / corrupt-VLEN errors.
  {
    ConfigurableVfdEnv cfg_env;
    g_cfg_vfd_env = &cfg_env;
    absl::Cleanup reset_env = [] { g_cfg_vfd_env = nullptr; };

    const H5FD_class_t cls = MakeConfigurableVfdClass();
    const hid_t drv_id = H5FDregister(&cls);
    ASSERT_NE(drv_id, H5I_INVALID_HID);
    absl::Cleanup unregister_drv = [drv_id] { H5FDunregister(drv_id); };

    ScopedPlist cfg_fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(cfg_fapl.valid());
    ASSERT_GE(H5Pset_driver(cfg_fapl, drv_id, nullptr), 0);

    ScopedFile cfg_src(H5Fcreate("contig_cfg_copy_src.h5", H5F_ACC_TRUNC,
                                 H5P_DEFAULT, cfg_fapl));
    ASSERT_TRUE(cfg_src.valid());
    ScopedFile cfg_dst(H5Fcreate("contig_cfg_copy_dst.h5", H5F_ACC_TRUNC,
                                 H5P_DEFAULT, cfg_fapl));
    ASSERT_TRUE(cfg_dst.valid());
    auto* f_cfg_src =
        static_cast<H5F_t*>(H5VL_object_verify(cfg_src, H5I_FILE));
    auto* f_cfg_dst =
        static_cast<H5F_t*>(H5VL_object_verify(cfg_dst, H5I_FILE));
    ASSERT_NE(f_cfg_src, nullptr);
    ASSERT_NE(f_cfg_dst, nullptr);

    ScopedDataset cfg_ds_id(H5Dcreate2(cfg_src, "cfg_plain", H5T_NATIVE_INT32,
                                       space16, H5P_DEFAULT, dcpl_early,
                                       H5P_DEFAULT));
    ASSERT_TRUE(cfg_ds_id.valid());
    auto* cfg_dset =
        static_cast<H5D_t*>(H5VL_object_verify(cfg_ds_id, H5I_DATASET));
    ASSERT_NE(cfg_dset, nullptr);

    ScopedApiContext ctx;
    ScopedErrorSilence silence;

    cfg_env.fail_get_eoa = true;
    EXPECT_LT(H5D__contig_check(f_cfg_src, &cfg_dset->shared->layout,
                                H5S_GET_EXTENT(cfg_dset->shared->space),
                                cfg_dset->shared->type),
              0);
    cfg_env.fail_get_eoa = false;

    cfg_env.fail_write = true;
    EXPECT_LT(H5D__contig_fill(cfg_dset), 0);

    H5O_storage_contig_t dst_store{};
    dst_store.addr = HADDR_UNDEF;
    dst_store.size = cfg_dset->shared->layout.storage.u.contig.size;
    H5O_copy_t cpy_info{};
    EXPECT_LT(
        H5D__contig_copy(f_src, &cfg_dset->shared->layout.storage.u.contig,
                         f_cfg_dst, &dst_store, cfg_dset->shared->type,
                         &cpy_info),
        0);
    cfg_env.fail_write = false;

    cfg_env.fail_read = true;
    dst_store.addr = HADDR_UNDEF;
    EXPECT_LT(
        H5D__contig_copy(f_cfg_src, &cfg_dset->shared->layout.storage.u.contig,
                         f_dst, &dst_store, cfg_dset->shared->type, &cpy_info),
        0);
    cfg_env.fail_read = false;

    // Write a 1-element VLEN dataset, flush, overwrite its on-disk heap ref
    // (preserving small seq_len=1, corrupting bytes [4..16)) in cfg_env's file
    // vector, and verify H5D__contig_copy detects the src->mem error cleanly.
    constexpr std::array<hsize_t, 1> kDims1 = {1};
    ScopedSpace space1(H5Screate_simple(1, kDims1.data(), nullptr));
    ASSERT_TRUE(space1.valid());
    ScopedType vl_int(H5Tvlen_create(H5T_NATIVE_INT32));
    ASSERT_TRUE(vl_int.valid());
    std::array<int32_t, 2> vl_elts = {7, 8};
    const hvl_t vl_val{vl_elts.size(), vl_elts.data()};
    ScopedPlist dcpl_vl(H5Pcopy(dcpl_early));
    ASSERT_TRUE(dcpl_vl.valid());
    ASSERT_GE(H5Pset_fill_value(dcpl_vl, vl_int, &vl_val), 0);

    ScopedDataset cfg_vl_id(H5Dcreate2(cfg_src, "cfg_vl", vl_int, space1,
                                       H5P_DEFAULT, dcpl_vl, H5P_DEFAULT));
    ASSERT_TRUE(cfg_vl_id.valid());
    auto* cfg_vl_dset =
        static_cast<H5D_t*>(H5VL_object_verify(cfg_vl_id, H5I_DATASET));
    ASSERT_NE(cfg_vl_dset, nullptr);
    const haddr_t vl_addr = cfg_vl_dset->shared->layout.storage.u.contig.addr;
    const haddr_t vl_size = cfg_vl_dset->shared->layout.storage.u.contig.size;
    ASSERT_NE(vl_addr, HADDR_UNDEF);
    ASSERT_EQ(vl_size, 16u);
    ASSERT_GE(H5Fflush(cfg_src, H5F_SCOPE_GLOBAL), 0);

    auto& raw_bytes = cfg_env.files["contig_cfg_copy_src.h5"];
    ASSERT_GE(raw_bytes.size(), static_cast<size_t>(vl_addr + 16));
    raw_bytes[static_cast<size_t>(vl_addr) + 0] = 1;
    raw_bytes[static_cast<size_t>(vl_addr) + 1] = 0;
    raw_bytes[static_cast<size_t>(vl_addr) + 2] = 0;
    raw_bytes[static_cast<size_t>(vl_addr) + 3] = 0;
    std::memset(raw_bytes.data() + static_cast<size_t>(vl_addr) + 4, 0xFE, 12);

    H5O_storage_contig_t src_vl_store =
        cfg_vl_dset->shared->layout.storage.u.contig;
    dst_store.addr = HADDR_UNDEF;
    dst_store.size = 16;
    EXPECT_LT(H5D__contig_copy(f_cfg_src, &src_vl_store, f_dst, &dst_store,
                               cfg_vl_dset->shared->type, &cpy_info),
              0);
  }

  // 3. Remaining H5D__contig_fill() and H5D__contig_copy() error paths:
  //    alloc failure, VLEN fill corruption, dt_size == 0, nelmts == 0, missing
  //    compound tpath, mem->dst conversion fail, and invalid ref expansion.
  constexpr std::array<hsize_t, 1> kDims1 = {1};
  ScopedSpace space1(H5Screate_simple(1, kDims1.data(), nullptr));
  ASSERT_TRUE(space1.valid());

  {
    ScopedType vl_int(H5Tvlen_create(H5T_NATIVE_INT32));
    ASSERT_TRUE(vl_int.valid());
    std::array<int32_t, 2> fill_elts = {12, 34};
    const hvl_t fill_vl{fill_elts.size(), fill_elts.data()};
    ScopedPlist vl_dcpl(H5Pcopy(dcpl_early));
    ASSERT_TRUE(vl_dcpl.valid());
    ASSERT_GE(H5Pset_fill_value(vl_dcpl, vl_int, &fill_vl), 0);

    ScopedDataset vl_id(H5Dcreate2(src_file, "vl_err", vl_int, space1,
                                   H5P_DEFAULT, vl_dcpl, H5P_DEFAULT));
    ASSERT_TRUE(vl_id.valid());
    auto* vl_dset = static_cast<H5D_t*>(H5VL_object_verify(vl_id, H5I_DATASET));
    ASSERT_NE(vl_dset, nullptr);

    // 3a. Corrupt VLEN global heap ref in fill cache (keeping seq_len=2).
    ASSERT_NE(vl_dset->shared->dcpl_cache.fill.buf, nullptr);
    const auto fill_len =
        static_cast<size_t>(vl_dset->shared->dcpl_cache.fill.size);
    ASSERT_GT(fill_len, 4u);
    std::vector<uint8_t> saved_fill(fill_len, 0);
    std::memcpy(saved_fill.data(), vl_dset->shared->dcpl_cache.fill.buf,
                fill_len);
    {
      absl::Cleanup restore_fill = [&] {
        std::memcpy(vl_dset->shared->dcpl_cache.fill.buf, saved_fill.data(),
                    fill_len);
      };
      std::memset(
          static_cast<uint8_t*>(vl_dset->shared->dcpl_cache.fill.buf) + 4,
          0xFF, fill_len - 4);
      ScopedApiContext ctx;
      ScopedErrorSilence silence;
      EXPECT_LT(H5D__contig_fill(vl_dset), 0);
    }

    // 3b. H5D__contig_copy() alloc fail (HADDR_MAX), dt_size == 0, nelmts == 0.
    H5O_copy_t cpy_info{};
    H5O_storage_contig_t dst_store{};
    ScopedApiContext ctx;
    ScopedErrorSilence silence;

    dst_store.addr = HADDR_UNDEF;
    dst_store.size = HADDR_MAX;
    EXPECT_LT(H5D__contig_copy(f_src,
                               &vl_dset->shared->layout.storage.u.contig, f_dst,
                               &dst_store, vl_dset->shared->type, &cpy_info),
              0);

    const size_t orig_vl_size = vl_dset->shared->type->shared->size;
    {
      absl::Cleanup restore_vl_size = [&] {
        vl_dset->shared->type->shared->size = orig_vl_size;
      };
      vl_dset->shared->type->shared->size = 0;
      dst_store.addr = HADDR_UNDEF;
      dst_store.size = 16;
      EXPECT_LT(
          H5D__contig_copy(f_src, &vl_dset->shared->layout.storage.u.contig,
                           f_dst, &dst_store, vl_dset->shared->type, &cpy_info),
          0);
    }

    dst_store.addr = HADDR_UNDEF;
    dst_store.size = 4;  // < 16 bytes -> nelmts == 0
    EXPECT_LT(H5D__contig_copy(f_src,
                               &vl_dset->shared->layout.storage.u.contig, f_dst,
                               &dst_store, vl_dset->shared->type, &cpy_info),
              0);
  }

  // 3c. Compound{VLEN, OPAQUE(force_conv=true)} hits H5D__fill_init and
  //     H5D__contig_copy tpath failure cleanly.
  {
    ScopedType vl_int(H5Tvlen_create(H5T_NATIVE_INT32));
    ASSERT_TRUE(vl_int.valid());
    ScopedType opq(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(opq.valid());
    ASSERT_GE(H5Tset_tag(opq, "contig_unconv_opaque"), 0);

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

    std::array<int32_t, 1> elem = {55};
    VlAndOpaque fill_cmp{{elem.size(), elem.data()}, {1, 2, 3, 4}};
    ScopedPlist cmp_dcpl(H5Pcopy(dcpl_early));
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
    const bool orig_top_fc = cmp_dset->shared->type->shared->force_conv;
    const bool orig_fc = memb1_type->shared->force_conv;
    char* const orig_tag = memb1_type->shared->u.opaque.tag;
    char alt_tag[] = "contig_unconv_opaque_miss";
    absl::Cleanup restore_fc = [&] {
      cmp_dset->shared->type->shared->force_conv = orig_top_fc;
      memb1_type->shared->force_conv = orig_fc;
      memb1_type->shared->u.opaque.tag = orig_tag;
    };
    cmp_dset->shared->type->shared->force_conv = true;
    memb1_type->shared->force_conv = true;
    memb1_type->shared->u.opaque.tag = alt_tag;

    H5O_storage_contig_t dst_store{};
    dst_store.addr = HADDR_UNDEF;
    dst_store.size = cmp_dset->shared->layout.storage.u.contig.size;
    H5O_copy_t cpy_info{};

    ScopedApiContext ctx;
    ScopedErrorSilence silence;
    EXPECT_LT(H5D__contig_fill(cmp_dset), 0);
    EXPECT_LT(
        H5D__contig_copy(f_src, &cmp_dset->shared->layout.storage.u.contig,
                         f_dst, &dst_store, cmp_dset->shared->type, &cpy_info),
        0);
  }

  // 3d. Register hard converter failing Pass 2 (mem->dst to 4B-addr dst4_file).
  {
    ScopedPlist dst4_fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(dst4_fcpl.valid());
    ASSERT_GE(H5Pset_sizes(dst4_fcpl, 4, 4), 0);
    ScopedPlist dst4_fapl = MakeCoreFapl();
    ASSERT_TRUE(dst4_fapl.valid());
    ScopedFile dst4_file(
        H5Fcreate("contig_err_dst4.h5", H5F_ACC_TRUNC, dst4_fcpl, dst4_fapl));
    ASSERT_TRUE(dst4_file.valid());
    auto* f_dst4 = static_cast<H5F_t*>(H5VL_object_verify(dst4_file, H5I_FILE));
    ASSERT_NE(f_dst4, nullptr);

    ScopedType opq(H5Tcreate(H5T_OPAQUE, 1));
    ASSERT_TRUE(opq.valid());
    ASSERT_GE(H5Tset_tag(opq, "contig_fail_mem_dst_vl"), 0);
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
    ScopedPlist nil_dcpl(H5Pcopy(dcpl_early));
    ASSERT_TRUE(nil_dcpl.valid());
    ASSERT_GE(H5Pset_fill_value(nil_dcpl, vl_opq, &nil_fill), 0);

    ScopedDataset nil_id(H5Dcreate2(src_file, "nil_vl", vl_opq, space1,
                                    H5P_DEFAULT, nil_dcpl, H5P_DEFAULT));
    ASSERT_TRUE(nil_id.valid());
    auto* nil_dset =
        static_cast<H5D_t*>(H5VL_object_verify(nil_id, H5I_DATASET));
    ASSERT_NE(nil_dset, nullptr);

    ASSERT_GE(H5Tregister(H5T_PERS_HARD, "contig_fail_mem_dst_conv", vl_opq,
                          disk4_vl, FailMemToDstVlenConv),
              0);
    absl::Cleanup unregister_conv = [&] {
      H5Tunregister(H5T_PERS_HARD, "contig_fail_mem_dst_conv", vl_opq, disk4_vl,
                    FailMemToDstVlenConv);
    };

    H5O_storage_contig_t dst_store{};
    dst_store.addr = HADDR_UNDEF;
    dst_store.size = nil_dset->shared->layout.storage.u.contig.size;
    H5O_copy_t cpy_info{};

    ScopedApiContext ctx;
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5D__contig_copy(f_src, &nil_dset->shared->layout.storage.u.contig,
                         f_dst4, &dst_store, nil_dset->shared->type, &cpy_info),
        0);
  }

  // 3e. Invalid deprecated object reference fails H5O_copy_expand_ref inside
  //     H5D__contig_copy() cleanly without invoking H5O__layout_copy_file().
  {
    ScopedDataset bad_ref_id(H5Dcreate2(src_file, "bad_ref", H5T_STD_REF_OBJ,
                                        space1, H5P_DEFAULT, dcpl_early,
                                        H5P_DEFAULT));
    ASSERT_TRUE(bad_ref_id.valid());
    // HADDR_UNDEF fails inside H5O__copy_expand_ref_object1 before touching
    // cpy_info->map_list; flush so H5D__contig_copy reads it from disk.
    const hobj_ref_t bogus_ref = static_cast<hobj_ref_t>(HADDR_UNDEF);
    ASSERT_GE(H5Dwrite(bad_ref_id, H5T_STD_REF_OBJ, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, &bogus_ref),
              0);
    ASSERT_GE(H5Fflush(src_file, H5F_SCOPE_GLOBAL), 0);
    auto* bad_ref_dset =
        static_cast<H5D_t*>(H5VL_object_verify(bad_ref_id, H5I_DATASET));
    ASSERT_NE(bad_ref_dset, nullptr);

    H5O_storage_contig_t dst_store{};
    dst_store.addr = HADDR_UNDEF;
    dst_store.size = bad_ref_dset->shared->layout.storage.u.contig.size;
    H5O_copy_t cpy_info{};
    cpy_info.expand_ref = true;

    ScopedApiContext ctx;
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5D__contig_copy(f_src, &bad_ref_dset->shared->layout.storage.u.contig,
                         f_dst, &dst_store, bad_ref_dset->shared->type,
                         &cpy_info),
        0);
  }
}

}  // namespace
}  // namespace hdf5
