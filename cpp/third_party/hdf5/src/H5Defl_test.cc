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
// Tests External File List (EFL) contiguous dataset storage construction,
// initialization, multi-file offset/skip I/O, hyperslab vectorized read/write,
// file prefix resolution, extendible dimensions, truncated zero-fill reads,
// metadata heap info, and error paths in third_party/hdf5/src/H5Defl.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Defl.c

#include "third_party/hdf5/src/hdf5.h"

#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <filesystem>  // NOLINT
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"

// `:hdf5` is built with -DNDEBUG, which omits debug fields from metadata
// structures pulled by H5private.h and H5Dpkg.h.
#ifndef NDEBUG
#define H5DEFL_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5D_FRIEND
#define H5F_FRIEND
#define H5S_FRIEND
#define H5T_FRIEND
// H5B2private.h (pulled by H5Dpkg.h) and H5Fpkg.h use `try` as a parameter
// name, colliding with libc++'s `#define try if (1)` macro and `try` keyword.
#pragma push_macro("try")
#undef try
#define try hdf5_try_param
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Dpkg.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Spkg.h"
#pragma push_macro("H5_HAVE__FLOAT16")
#undef H5_HAVE__FLOAT16
#include "third_party/hdf5/src/H5Tpkg.h"
#pragma pop_macro("H5_HAVE__FLOAT16")
#include "third_party/hdf5/src/H5VLprivate.h"
#undef try
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5DEFL_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5DEFL_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

// RAII wrapper pushing an HDF5 API context.
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

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

TEST(H5DeflTest, ConstructionConstraintsAndValidationErrors) {
  TempDir dir("efl_construct_err");
  ScopedFile file = CreateInMemoryFile("efl_construct_err.h5");
  ASSERT_TRUE(file.valid());

  // 1. Only the first dimension may be extendible for EFL datasets.
  {
    constexpr std::array<hsize_t, 2> kDims = {4, 4};
    constexpr std::array<hsize_t, 2> kBadMax = {4, H5S_UNLIMITED};
    ScopedSpace space(H5Screate_simple(2, kDims.data(), kBadMax.data()));
    ASSERT_TRUE(space.valid());

    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_external(dcpl.get(), dir.child("bad2d.bin").c_str(), 0,
                              H5F_UNLIMITED),
              0);

    ScopedErrorSilence silence;
    EXPECT_EQ(H5Dcreate2(file.get(), "bad2d", H5T_NATIVE_INT32, space.get(),
                         H5P_DEFAULT, dcpl.get(), H5P_DEFAULT),
              H5I_INVALID_HID);
  }

  // 2. Unlimited first dimension requires H5F_UNLIMITED final external slot.
  {
    constexpr std::array<hsize_t, 1> kDims = {4};
    constexpr std::array<hsize_t, 1> kMaxUnlim = {H5S_UNLIMITED};
    ScopedSpace space(H5Screate_simple(1, kDims.data(), kMaxUnlim.data()));
    ASSERT_TRUE(space.valid());

    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(
        H5Pset_external(dcpl.get(), dir.child("finite.bin").c_str(), 0, 64), 0);

    ScopedErrorSilence silence;
    EXPECT_EQ(H5Dcreate2(file.get(), "unlim_finite", H5T_NATIVE_INT32,
                         space.get(), H5P_DEFAULT, dcpl.get(), H5P_DEFAULT),
              H5I_INVALID_HID);
  }

  // 3. Finite dataspace max_points * dt_size exceeding external storage fails.
  {
    constexpr std::array<hsize_t, 1> kDims = {8};
    ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());

    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(
        H5Pset_external(dcpl.get(), dir.child("small.bin").c_str(), 0, 16), 0);

    ScopedErrorSilence silence;
    EXPECT_EQ(H5Dcreate2(file.get(), "too_small", H5T_NATIVE_INT32, space.get(),
                         H5P_DEFAULT, dcpl.get(), H5P_DEFAULT),
              H5I_INVALID_HID);
  }

  // 4. Dataspace * dt_size multiplication overflow fails in construct().
  {
    constexpr std::array<hsize_t, 1> kDims = {4};
    constexpr std::array<hsize_t, 1> kHugeMax = {(uint64_t{1} << 62) + 1};
    ScopedSpace space(H5Screate_simple(1, kDims.data(), kHugeMax.data()));
    ASSERT_TRUE(space.valid());

    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_external(dcpl.get(), dir.child("overflow.bin").c_str(), 0,
                              H5F_UNLIMITED),
              0);

    ScopedErrorSilence silence;
    EXPECT_EQ(H5Dcreate2(file.get(), "overflow_dset", H5T_NATIVE_INT64,
                         space.get(), H5P_DEFAULT, dcpl.get(), H5P_DEFAULT),
              H5I_INVALID_HID);
  }
}

TEST(H5DeflTest, MultiSlotReadWriteHyperslabExtendPrefixAndTruncatedZeroFill) {
  TempDir dir("efl_multi");
  const std::filesystem::path h5_path = dir.child("host.h5");
  const std::filesystem::path raw0 = dir.child("part0.bin");
  const std::filesystem::path raw1 = dir.child("part1.bin");
  const std::filesystem::path raw2 = dir.child("part2.bin");

  // Pre-seed raw0 with sentinel bytes before offset 8 so we verify offset
  // preservation.
  {
    std::ofstream out(raw0, std::ios::binary);
    ASSERT_TRUE(out.good());
    const std::array<uint8_t, 8> header_bytes = {1, 2, 3, 4, 5, 6, 7, 8};
    out.write(reinterpret_cast<const char*>(header_bytes.data()),
              header_bytes.size());
  }

  // Three-slot EFL covering 2x4 int32 matrix (initially 32 bytes) and
  // extendable on dimension 0:
  //   slot 0: "part0.bin", offset 8, size 12 (3 int32s)
  //   slot 1: "part1.bin", offset 4, size 16 (4 int32s)
  //   slot 2: "part2.bin", offset 0, size H5F_UNLIMITED
  {
    ScopedFile file(
        H5Fcreate(h5_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    constexpr std::array<hsize_t, 2> kDims = {2, 4};
    constexpr std::array<hsize_t, 2> kMaxDims = {H5S_UNLIMITED, 4};
    ScopedSpace space(H5Screate_simple(2, kDims.data(), kMaxDims.data()));
    ASSERT_TRUE(space.valid());

    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_external(dcpl.get(), "part0.bin", 8, 12), 0);
    ASSERT_GE(H5Pset_external(dcpl.get(), "part1.bin", 4, 16), 0);
    ASSERT_GE(H5Pset_external(dcpl.get(), "part2.bin", 0, H5F_UNLIMITED), 0);

    ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_TRUE(dapl.valid());
    ASSERT_GE(H5Pset_efile_prefix(dapl.get(), "${ORIGIN}"), 0);

    ScopedDataset dset(H5Dcreate2(file.get(), "matrix", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  dapl.get()));
    ASSERT_TRUE(dset.valid());

    // Query space status and object metadata info (H5D__efl_is_space_alloc and
    // H5D__efl_bh_info via H5Oget_native_info_by_name).
    H5D_space_status_t status = H5D_SPACE_STATUS_ERROR;
    ASSERT_GE(H5Dget_space_status(dset.get(), &status), 0);
    EXPECT_EQ(status, H5D_SPACE_STATUS_ALLOCATED);

    H5O_native_info_t ninfo{};
    ASSERT_GE(H5Oget_native_info_by_name(file.get(), "matrix", &ninfo,
                                          H5O_NATIVE_INFO_META_SIZE,
                                          H5P_DEFAULT),
              0);
    EXPECT_GT(ninfo.meta_size.obj.heap_size, 0U);

    const std::array<int32_t, 8> wdata = {10, 20, 30, 40, 50, 60, 70, 80};
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, wdata.data()),
              0);
    ASSERT_GE(H5Dflush(dset.get()), 0);

    // Object copy within file duplicates EFL metadata cleanly while layout size
    // matches initial 2x4 extent.
    ASSERT_GE(H5Ocopy(file.get(), "matrix", file.get(), "matrix_copy",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);

    // Extend along dim 0 to 4x4 (16 ints = 64 bytes -> exercises slot 1 skip
    // and unlimited slot 2).
    constexpr std::array<hsize_t, 2> kExtDims = {4, 4};
    ASSERT_GE(H5Dset_extent(dset.get(), kExtDims.data()), 0);

    // Hyperslab strided write (row 2, odd columns {1, 3}) and row 3 full.
    {
      ScopedSpace fspace(H5Dget_space(dset.get()));
      ASSERT_TRUE(fspace.valid());
      constexpr std::array<hsize_t, 2> kStart = {2, 1};
      constexpr std::array<hsize_t, 2> kStride = {1, 2};
      constexpr std::array<hsize_t, 2> kCount = {2, 2};
      ASSERT_GE(H5Sselect_hyperslab(fspace.get(), H5S_SELECT_SET, kStart.data(),
                                    kStride.data(), kCount.data(), nullptr),
                0);
      constexpr std::array<hsize_t, 1> kMdims = {4};
      ScopedSpace mspace(H5Screate_simple(1, kMdims.data(), nullptr));
      ASSERT_TRUE(mspace.valid());
      const std::array<int32_t, 4> patch = {201, 203, 301, 303};
      ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, mspace.get(),
                         fspace.get(), H5P_DEFAULT, patch.data()),
                0);
    }

    // Read directly starting from slot 1 offset via hyperslab (exercises
    // slot skipping in H5D__efl_read).
    {
      ScopedSpace fspace(H5Dget_space(dset.get()));
      ASSERT_TRUE(fspace.valid());
      constexpr std::array<hsize_t, 2> kStart = {1, 0};
      constexpr std::array<hsize_t, 2> kCount = {1, 4};
      ASSERT_GE(H5Sselect_hyperslab(fspace.get(), H5S_SELECT_SET, kStart.data(),
                                    nullptr, kCount.data(), nullptr),
                0);
      ScopedSpace mspace(H5Screate_simple(1, &kCount[1], nullptr));
      ASSERT_TRUE(mspace.valid());
      std::array<int32_t, 4> row1 = {};
      ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, mspace.get(),
                        fspace.get(), H5P_DEFAULT, row1.data()),
                0);
      EXPECT_THAT(row1, ElementsAre(50, 60, 70, 80));
    }
  }

  // Verify sentinel bytes before offset 8 in part0.bin were untouched.
  {
    std::ifstream in(raw0, std::ios::binary);
    ASSERT_TRUE(in.good());
    std::array<uint8_t, 8> header_bytes = {};
    in.read(reinterpret_cast<char*>(header_bytes.data()), header_bytes.size());
    EXPECT_THAT(header_bytes, ElementsAre(1, 2, 3, 4, 5, 6, 7, 8));
  }

  // Truncate part0.bin to 16 bytes (leaving room for offset 8 + 2 int32s,
  // cutting off the 3rd int32), and truncate part2.bin to 0 bytes, then reopen
  // and read: H5D__efl_read zero-fills past physical EOF of each member file.
  ASSERT_EQ(truncate(raw0.c_str(), 16), 0);
  ASSERT_EQ(truncate(raw2.c_str(), 0), 0);
  {
    ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_TRUE(dapl.valid());
    ASSERT_GE(H5Pset_efile_prefix(dapl.get(), dir.c_str()), 0);

    ScopedFile file(H5Fopen(h5_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    ScopedDataset dset(H5Dopen2(file.get(), "matrix", dapl.get()));
    ASSERT_TRUE(dset.valid());

    std::array<int32_t, 16> out = {};
    out.fill(-999);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, out.data()),
              0);
    // Element 0, 1 came from raw0; element 2 was truncated -> zero-filled;
    // elements 3..6 came from raw1; remaining came from truncated raw2 -> 0.
    EXPECT_EQ(out[0], 10);
    EXPECT_EQ(out[1], 20);
    EXPECT_EQ(out[2], 0);
    EXPECT_EQ(out[3], 40);
    EXPECT_EQ(out[4], 50);
    EXPECT_EQ(out[5], 60);
    EXPECT_EQ(out[6], 70);
    EXPECT_EQ(out[7], 0);
    EXPECT_EQ(out[15], 0);
  }

  EXPECT_TRUE(std::filesystem::exists(raw1));
  EXPECT_TRUE(std::filesystem::exists(raw2));
}

TEST(H5DeflTest, IoOpenErrorsAndInternalBoundsAndOverflowChecks) {
  TempDir dir("efl_errors");
  ScopedFile file = CreateInMemoryFile("efl_errors.h5");
  ASSERT_TRUE(file.valid());

  constexpr std::array<hsize_t, 1> kDims = {4};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  // 1. Read from non-existent external file fails (HDopen O_RDONLY).
  {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(
        H5Pset_external(dcpl.get(), dir.child("missing.bin").c_str(), 0, 16),
        0);
    ScopedDataset dset(H5Dcreate2(file.get(), "missing_read", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    std::array<int32_t, 4> buf = {};
    ScopedErrorSilence silence;
    EXPECT_LT(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, buf.data()),
              0);
  }

  // 2. Write to non-existent subdirectory hits HDaccess(..., F_OK) < 0 error.
  {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_external(
                  dcpl.get(),
                  dir.child("no_such_subdir").string().append("/raw.bin").c_str(),
                  0, 16),
              0);
    ScopedDataset dset(H5Dcreate2(file.get(), "missing_dir", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    const std::array<int32_t, 4> buf = {1, 2, 3, 4};
    ScopedErrorSilence silence;
    EXPECT_LT(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, buf.data()),
              0);
  }

  // 3. Write to read-only file (mode 0444) hits HDaccess(..., F_OK) == 0 error
  //    branch when running unprivileged.
  if (geteuid() != 0) {
    const std::filesystem::path ro_path = dir.child("readonly.bin");
    {
      std::ofstream out(ro_path, std::ios::binary);
      ASSERT_TRUE(out.good());
      out << "abcd";
    }
    ASSERT_EQ(chmod(ro_path.c_str(), 0444), 0);
    absl::Cleanup restore_perms = [&ro_path]() {
      chmod(ro_path.c_str(), 0644);
    };

    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_external(dcpl.get(), ro_path.c_str(), 0, 16), 0);
    ScopedDataset dset(H5Dcreate2(file.get(), "ro_write", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    const std::array<int32_t, 4> buf = {1, 2, 3, 4};
    ScopedErrorSilence silence;
    EXPECT_LT(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, buf.data()),
              0);
  }

  // 4. Internal checks on H5D_LOPS_EFL[0] construct, init, readvv, writevv.
  const std::filesystem::path ok_path = dir.child("ok_internal.bin");
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_external(dcpl.get(), ok_path.c_str(), 0, 16), 0);
  ScopedDataset dset_id(H5Dcreate2(file.get(), "internal_dset",
                                   H5T_NATIVE_INT32, space.get(), H5P_DEFAULT,
                                   dcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(dset_id.valid());

  const std::array<int32_t, 4> init_vals = {11, 22, 33, 44};
  ASSERT_GE(H5Dwrite(dset_id.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, init_vals.data()),
            0);

  auto* f = static_cast<H5F_t*>(H5VL_object_verify(file.get(), H5I_FILE));
  auto* dset =
      static_cast<H5D_t*>(H5VL_object_verify(dset_id.get(), H5I_DATASET));
  ASSERT_NE(f, nullptr);
  ASSERT_NE(dset, nullptr);

  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    ScopedErrorSilence silence;

    // 4a. construct() / init() error paths with synthetic mutations restored on
    // exit.
    const size_t orig_dt_size = dset->shared->type->shared->size;
    const hsize_t orig_nelem = dset->shared->space->extent.nelem;
    const hsize_t orig_slot0_size = dset->shared->dcpl_cache.efl.slot[0].size;
    absl::Cleanup restore_dset = [&] {
      dset->shared->type->shared->size = orig_dt_size;
      dset->shared->space->extent.nelem = orig_nelem;
      dset->shared->dcpl_cache.efl.slot[0].size = orig_slot0_size;
    };

    // Valid init() call.
    EXPECT_GE(H5D_LOPS_EFL[0].init(f, dset, H5P_DEFAULT, true), 0);

    // dt_size == 0 in construct() and init().
    dset->shared->type->shared->size = 0;
    EXPECT_LT(H5D_LOPS_EFL[0].construct(f, dset), 0);
    EXPECT_LT(H5D_LOPS_EFL[0].init(f, dset, H5P_DEFAULT, true), 0);
    dset->shared->type->shared->size = orig_dt_size;

    // stmp_size / snelmts < 0 in construct() and init().
    dset->shared->space->extent.nelem = static_cast<hsize_t>(-1);
    EXPECT_LT(H5D_LOPS_EFL[0].construct(f, dset), 0);
    EXPECT_LT(H5D_LOPS_EFL[0].init(f, dset, H5P_DEFAULT, true), 0);
    dset->shared->space->extent.nelem = orig_nelem;

    // nelmts * dt_size overflow in init().
    dset->shared->type->shared->size = (size_t{1} << 62) + 1;
    EXPECT_LT(H5D_LOPS_EFL[0].init(f, dset, H5P_DEFAULT, true), 0);
    dset->shared->type->shared->size = orig_dt_size;

    // data_size > max_storage in init().
    dset->shared->dcpl_cache.efl.slot[0].size = 8;
    EXPECT_LT(H5D_LOPS_EFL[0].init(f, dset, H5P_DEFAULT, true), 0);
    dset->shared->dcpl_cache.efl.slot[0].size = orig_slot0_size;

    // 4b. Direct readvv/writevv past logical end of EFL (u >= efl->nused) and
    // external file address overflow (slot[0].offset + skip).
    H5D_io_info_t io_info{};
    io_info.f_sh = f->shared;
    H5D_storage_t store{};
    store.efl = dset->shared->dcpl_cache.efl;

    std::array<uint8_t, 4> io_buf{};
    H5D_dset_io_info_t dinfo{};
    dinfo.dset = dset;
    dinfo.store = &store;
    dinfo.buf.vp = io_buf.data();

    // Past logical end (offset == 16 == slot[0].size -> u == efl->nused).
    hsize_t dset_off = 16;
    size_t dset_len = 4;
    size_t dset_seq = 0;
    hsize_t mem_off = 0;
    size_t mem_len = 4;
    size_t mem_seq = 0;
    io_info.op_type = H5D_IO_OP_READ;
    EXPECT_LT(H5D_LOPS_EFL[0].readvv(&io_info, &dinfo, 1, &dset_seq, &dset_len,
                                     &dset_off, 1, &mem_seq, &mem_len,
                                     &mem_off),
              0);

    dset_seq = 0;
    mem_seq = 0;
    io_info.op_type = H5D_IO_OP_WRITE;
    EXPECT_LT(H5D_LOPS_EFL[0].writevv(&io_info, &dinfo, 1, &dset_seq,
                                      &dset_len, &dset_off, 1, &mem_seq,
                                      &mem_len, &mem_off),
              0);

    // Offset + skip overflow (H5F_OVERFLOW_HSIZET2OFFT).
    store.efl.slot[0].offset = std::numeric_limits<HDoff_t>::max();
    dset_off = 4;
    dset_seq = 0;
    mem_seq = 0;
    io_info.op_type = H5D_IO_OP_READ;
    EXPECT_LT(H5D_LOPS_EFL[0].readvv(&io_info, &dinfo, 1, &dset_seq, &dset_len,
                                     &dset_off, 1, &mem_seq, &mem_len,
                                     &mem_off),
              0);

    dset_seq = 0;
    mem_seq = 0;
    io_info.op_type = H5D_IO_OP_WRITE;
    EXPECT_LT(H5D_LOPS_EFL[0].writevv(&io_info, &dinfo, 1, &dset_seq,
                                      &dset_len, &dset_off, 1, &mem_seq,
                                      &mem_len, &mem_off),
              0);
  }
}

}  // namespace
}  // namespace hdf5
