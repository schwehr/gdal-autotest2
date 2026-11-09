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
// Unit tests for HDF5 dataset raw I/O orchestration, type conversion and
// background buffer sizing, dataspace rank projection, selection I/O routing,
// filter re-validation on write, and error handling
// (third_party/hdf5/src/H5Dio.c).
//
// Coverage notes in default non-MPI Linux `-DNDEBUG` Google3 builds:
//   - 35 lines under `if (io_info.md_io_ops.multi_read_md)` / `multi_write_md`
//     (lines 326-360, 764-793) are only reachable when `H5_HAVE_PARALLEL`
//     is enabled (`H5D__ioinfo_adjust` assigns `md_io_ops`).
//   - 20 lines are heap/free-list OOM error checks (`H5MM_malloc/calloc` and
//     `H5FL_BLK_MALLOC`), unreachable under ASan/MSan
//     `allocator_may_return_null=0`.
//   - Additional lines are statically unreachable under `-DNDEBUG` / LP64:
//     `_NOERR` return checks (lines 121, 233, 459, 548, 688, 888, 1070),
//     valid dataspace `H5S_GET_EXTENT_NPOINTS(...) < 0` (line 715), LP64
//     `size_t == hsize_t` (line 1204), post-`HGOTO_ERROR` closing brace
//     (line 403), and unrecoverable NULL `H5S_close` cleanup paths on failed
//     projection (lines 223, 466, 675, 895).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Dio.c

#include "third_party/hdf5/src/hdf5.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

// `:hdf5` is built with -DNDEBUG, which omits debug fields from metadata
// structures pulled by H5private.h and H5Dpkg.h.
#ifndef NDEBUG
#define H5DIO_TEST_RESTORE_UNDEF_NDEBUG 1
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
#ifdef H5DIO_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5DIO_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

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

  H5CX_t* mutable_ctx() { return &node_.ctx; }

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

// In-memory VFD supporting read_vector/write_vector (so H5F_has_vector_select_io
// returns true) and configurable read/write failure injection.
struct VectorVfdEnv {
  std::map<std::string, std::vector<uint8_t>> files;
  bool advertise_vector = true;
  bool fail_read = false;
  bool fail_write = false;
  int read_vector_calls = 0;
  int write_vector_calls = 0;
  unsigned long next_fileno = 1;
};

VectorVfdEnv* g_vec_env = nullptr;

struct VectorVfdFile {
  H5FD_t pub{};
  std::string name;
  haddr_t eoa = 0;
};

H5FD_t* VecOpen(const char* name, unsigned flags, hid_t /*fapl_id*/,
                haddr_t maxaddr) {
  if (g_vec_env == nullptr || name == nullptr || *name == '\0') {
    return nullptr;
  }
  auto& files = g_vec_env->files;
  if (flags & H5F_ACC_TRUNC) {
    files[name].clear();
  } else if (!(flags & H5F_ACC_CREAT) && !files.contains(name)) {
    return nullptr;
  } else {
    files[name];
  }
  auto* file = new VectorVfdFile();
  file->pub.fileno = g_vec_env->next_fileno++;
  file->pub.maxaddr = maxaddr;
  file->name = name;
  return reinterpret_cast<H5FD_t*>(file);
}

herr_t VecClose(H5FD_t* _file) {
  delete reinterpret_cast<VectorVfdFile*>(_file);
  return 0;
}

int VecCmp(const H5FD_t* f1, const H5FD_t* f2) {
  const auto* a = reinterpret_cast<const VectorVfdFile*>(f1);
  const auto* b = reinterpret_cast<const VectorVfdFile*>(f2);
  if (a->name < b->name) return -1;
  if (a->name > b->name) return 1;
  return 0;
}

herr_t VecQuery(const H5FD_t* /*file*/, unsigned long* flags) {
  *flags = 0;
  return 0;
}

haddr_t VecGetEoa(const H5FD_t* _file, H5FD_mem_t /*type*/) {
  return reinterpret_cast<const VectorVfdFile*>(_file)->eoa;
}

herr_t VecSetEoa(H5FD_t* _file, H5FD_mem_t /*type*/, haddr_t addr) {
  auto* file = reinterpret_cast<VectorVfdFile*>(_file);
  file->eoa = addr;
  auto& buf = g_vec_env->files[file->name];
  if (buf.size() < static_cast<size_t>(addr)) {
    buf.resize(static_cast<size_t>(addr), 0);
  }
  return 0;
}

haddr_t VecGetEof(const H5FD_t* _file, H5FD_mem_t /*type*/) {
  const auto* file = reinterpret_cast<const VectorVfdFile*>(_file);
  const auto it = g_vec_env->files.find(file->name);
  const haddr_t size = it == g_vec_env->files.end()
                           ? 0
                           : static_cast<haddr_t>(it->second.size());
  return std::max(file->eoa, size);
}

herr_t VecRead(H5FD_t* _file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
               haddr_t addr, size_t size, void* buf) {
  if (g_vec_env == nullptr || g_vec_env->fail_read) {
    return -1;
  }
  auto* file = reinterpret_cast<VectorVfdFile*>(_file);
  const auto& data = g_vec_env->files[file->name];
  std::memset(buf, 0, size);
  const size_t offset = static_cast<size_t>(addr);
  if (offset < data.size()) {
    const size_t avail = std::min(size, data.size() - offset);
    std::memcpy(buf, data.data() + offset, avail);
  }
  return 0;
}

herr_t VecWrite(H5FD_t* _file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                haddr_t addr, size_t size, const void* buf) {
  if (g_vec_env == nullptr || g_vec_env->fail_write) {
    return -1;
  }
  auto* file = reinterpret_cast<VectorVfdFile*>(_file);
  auto& data = g_vec_env->files[file->name];
  const size_t offset = static_cast<size_t>(addr);
  if (offset + size > data.size()) {
    data.resize(offset + size, 0);
  }
  std::memcpy(data.data() + offset, buf, size);
  return 0;
}

herr_t VecReadVector(H5FD_t* file, hid_t dxpl_id, uint32_t count,
                     H5FD_mem_t types[], haddr_t addrs[], size_t sizes[],
                     void* bufs[]) {
  if (g_vec_env == nullptr) return -1;
  ++g_vec_env->read_vector_calls;
  for (uint32_t i = 0; i < count; ++i) {
    if (VecRead(file, types[i], dxpl_id, addrs[i], sizes[i], bufs[i]) < 0) {
      return -1;
    }
  }
  return 0;
}

herr_t VecWriteVector(H5FD_t* file, hid_t dxpl_id, uint32_t count,
                      H5FD_mem_t types[], haddr_t addrs[], size_t sizes[],
                      const void* bufs[]) {
  if (g_vec_env == nullptr) return -1;
  ++g_vec_env->write_vector_calls;
  for (uint32_t i = 0; i < count; ++i) {
    if (VecWrite(file, types[i], dxpl_id, addrs[i], sizes[i], bufs[i]) < 0) {
      return -1;
    }
  }
  return 0;
}

H5FD_class_t MakeVectorVfdClass(bool with_vector_ops) {
  H5FD_class_t cls{};
  cls.version = H5FD_CLASS_VERSION;
  cls.value = static_cast<H5FD_class_value_t>(with_vector_ops ? 309 : 310);
  cls.name = with_vector_ops ? "test_h5dio_vec_vfd" : "test_h5dio_novec_vfd";
  cls.maxaddr = (static_cast<haddr_t>(1) << 30) - 1;
  cls.fc_degree = H5F_CLOSE_WEAK;
  cls.open = VecOpen;
  cls.close = VecClose;
  cls.cmp = VecCmp;
  cls.query = VecQuery;
  cls.get_eoa = VecGetEoa;
  cls.set_eoa = VecSetEoa;
  cls.get_eof = VecGetEof;
  cls.read = VecRead;
  cls.write = VecWrite;
  if (with_vector_ops) {
    cls.read_vector = VecReadVector;
    cls.write_vector = VecWriteVector;
  }
  const H5FD_mem_t fl_map[H5FD_MEM_NTYPES] = H5FD_FLMAP_SINGLE;
  std::memcpy(cls.fl_map, fl_map, sizeof(cls.fl_map));
  return cls;
}

// Custom filter with toggleable can_apply callback to test H5D__write's
// `!checked_filters` re-check when a dataset is reopened via H5Dopen2.
constexpr H5Z_filter_t kToggleFilterId = 315;
bool g_allow_can_apply = true;

htri_t ToggleFilterCanApply(hid_t /*dcpl_id*/, hid_t /*type_id*/,
                            hid_t /*space_id*/) {
  return g_allow_can_apply ? 1 : 0;
}

size_t ToggleFilterFunc(unsigned int /*flags*/, size_t /*cd_nelmts*/,
                        const unsigned int /*cd_values*/[], size_t nbytes,
                        size_t* buf_size, void** /*buf*/) {
  *buf_size = nbytes;
  return nbytes;
}

// Custom datatype converter setting H5T_BKG_TEMP on H5T_CONV_INIT so we test
// `need_bkg == H5T_BKG_TEMP` (bkg_buf allocated, must_fill_bkg == false).
herr_t OpaqueTempBkgConv(hid_t /*src_id*/, hid_t /*dst_id*/, H5T_cdata_t* cdata,
                         size_t nelmts, size_t buf_stride,
                         size_t /*bkg_stride*/, void* buf, void* bkg,
                         hid_t /*dxpl_id*/) {
  switch (cdata->command) {
    case H5T_CONV_INIT:
      cdata->need_bkg = H5T_BKG_TEMP;
      return 0;
    case H5T_CONV_CONV: {
      if (buf_stride == 0) {
        auto* bytes = static_cast<uint8_t*>(buf);
        if (bkg != nullptr) {
          std::memcpy(bkg, bytes, nelmts * 4);
          for (size_t i = 0; i < nelmts * 4; ++i) {
            bytes[i] = static_cast<uint8_t>(static_cast<uint8_t*>(bkg)[i] ^ 0x5Au);
          }
        }
      }
      return 0;
    }
    case H5T_CONV_FREE:
      return 0;
    default:
      return -1;
  }
}

TEST(H5DioTest, UnallocatedReadsArgValidationAndReadOnlyErrors) {
  ScopedFile file = CreateInMemoryFile("dio_basic.h5");
  ASSERT_TRUE(file.valid());

  constexpr std::array<hsize_t, 1> kDims4 = {4};
  constexpr std::array<hsize_t, 1> kDims2 = {2};
  ScopedSpace space4(H5Screate_simple(1, kDims4.data(), nullptr));
  ScopedSpace space2(H5Screate_simple(1, kDims2.data(), nullptr));
  ScopedSpace space_none(H5Scopy(space4));
  ScopedSpace space_no_extent(H5Screate(H5S_SIMPLE));
  ASSERT_TRUE(space4.valid());
  ASSERT_TRUE(space2.valid());
  ASSERT_TRUE(space_none.valid());
  ASSERT_TRUE(space_no_extent.valid());
  ASSERT_GE(H5Sselect_none(space_none), 0);

  // 1. Unallocated dataset read with fill_time=ALLOC fills user buffer.
  ScopedPlist dcpl_late_fill(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl_late_fill.valid());
  ASSERT_GE(H5Pset_alloc_time(dcpl_late_fill, H5D_ALLOC_TIME_LATE), 0);
  ASSERT_GE(H5Pset_fill_time(dcpl_late_fill, H5D_FILL_TIME_ALLOC), 0);
  const int32_t kFillVal = 42;
  ASSERT_GE(H5Pset_fill_value(dcpl_late_fill, H5T_NATIVE_INT32, &kFillVal), 0);

  ScopedDataset d_fill(H5Dcreate2(file, "unalloc_fill", H5T_NATIVE_INT32,
                                  space4, H5P_DEFAULT, dcpl_late_fill,
                                  H5P_DEFAULT));
  ASSERT_TRUE(d_fill.valid());
  std::array<int32_t, 4> buf = {-1, -1, -1, -1};
  ASSERT_GE(H5Dread(d_fill, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    buf.data()),
            0);
  EXPECT_THAT(buf, ElementsAre(42, 42, 42, 42));

  // 2. Unallocated dataset read with fill_time=NEVER skips filling user buffer.
  ScopedPlist dcpl_never(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl_never.valid());
  ASSERT_GE(H5Pset_alloc_time(dcpl_never, H5D_ALLOC_TIME_LATE), 0);
  ASSERT_GE(H5Pset_fill_time(dcpl_never, H5D_FILL_TIME_NEVER), 0);
  ScopedDataset d_never(H5Dcreate2(file, "unalloc_never", H5T_NATIVE_INT32,
                                   space4, H5P_DEFAULT, dcpl_never,
                                   H5P_DEFAULT));
  ASSERT_TRUE(d_never.valid());
  buf = {7, 8, 9, 10};
  ASSERT_GE(H5Dread(d_never, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    buf.data()),
            0);
  EXPECT_THAT(buf, ElementsAre(7, 8, 9, 10));

  // 3. Unallocated dataset with undefined fill value + FILL_TIME_IFSET fails
  //    on read.
  ScopedPlist dcpl_undef(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl_undef.valid());
  ASSERT_GE(H5Pset_alloc_time(dcpl_undef, H5D_ALLOC_TIME_LATE), 0);
  ASSERT_GE(H5Pset_fill_value(dcpl_undef, H5T_NATIVE_INT32, nullptr), 0);
  ASSERT_GE(H5Pset_fill_time(dcpl_undef, H5D_FILL_TIME_IFSET), 0);
  ScopedDataset d_undef(H5Dcreate2(file, "unalloc_undef", H5T_NATIVE_INT32,
                                   space4, H5P_DEFAULT, dcpl_undef,
                                   H5P_DEFAULT));
  ASSERT_TRUE(d_undef.valid());

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Dread(d_undef, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      buf.data()),
              0);
  }

  // 4. Multi-dataset read mixing skipped (unallocated) and allocated datasets
  //    exercises `if (dset_info[i].skip_io) continue`.
  ScopedDataset d_alloc(H5Dcreate2(file, "allocated", H5T_NATIVE_INT32, space4,
                                   H5P_DEFAULT, dcpl_late_fill, H5P_DEFAULT));
  ASSERT_TRUE(d_alloc.valid());
  constexpr std::array<int32_t, 4> kWriteData = {100, 200, 300, 400};
  ASSERT_GE(H5Dwrite(d_alloc, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     kWriteData.data()),
            0);

  std::array<int32_t, 4> r0 = {};
  std::array<int32_t, 4> r1 = {};
  std::array<hid_t, 2> dsets = {d_fill.get(), d_alloc.get()};
  std::array<hid_t, 2> types = {H5T_NATIVE_INT32, H5T_NATIVE_INT32};
  std::array<hid_t, 2> spaces = {H5S_ALL, H5S_ALL};
  std::array<void*, 2> rbufs = {r0.data(), r1.data()};
  ASSERT_GE(H5Dread_multi(2, dsets.data(), types.data(), spaces.data(),
                          spaces.data(), H5P_DEFAULT, rbufs.data()),
            0);
  EXPECT_THAT(r0, ElementsAre(42, 42, 42, 42));
  EXPECT_THAT(r1, ElementsAre(100, 200, 300, 400));

  // 5. Write with 0 elements selected on unallocated dataset skips storage
  //    allocation; NULL buffer with 0 elements succeeds on both read and write;
  //    NULL buffer with >0 elements fails on both.
  EXPECT_GE(H5Dwrite(d_never, H5T_NATIVE_INT32, space_none, space_none,
                     H5P_DEFAULT, nullptr),
            0);
  H5D_space_status_t status = H5D_SPACE_STATUS_ALLOCATED;
  ASSERT_GE(H5Dget_space_status(d_never, &status), 0);
  EXPECT_EQ(status, H5D_SPACE_STATUS_NOT_ALLOCATED);
  EXPECT_GE(H5Dread(d_alloc, H5T_NATIVE_INT32, space_none, space_none,
                    H5P_DEFAULT, nullptr),
            0);

  {
    ScopedErrorSilence silence;
    // NULL buffer with nelmts > 0.
    EXPECT_LT(H5Dread(d_alloc, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      nullptr),
              0);
    EXPECT_LT(H5Dwrite(d_alloc, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       nullptr),
              0);

    // Mismatched selection counts.
    EXPECT_LT(H5Dread(d_alloc, H5T_NATIVE_INT32, space2, space4, H5P_DEFAULT,
                      buf.data()),
              0);
    EXPECT_LT(H5Dwrite(d_alloc, H5T_NATIVE_INT32, space2, space4, H5P_DEFAULT,
                       kWriteData.data()),
              0);

    // File or memory space without extent set (0 points matching space_none).
    EXPECT_LT(H5Dread(d_alloc, H5T_NATIVE_INT32, space_none, space_no_extent,
                      H5P_DEFAULT, nullptr),
              0);
    EXPECT_LT(H5Dread(d_alloc, H5T_NATIVE_INT32, space_no_extent, space_none,
                      H5P_DEFAULT, nullptr),
              0);
    EXPECT_LT(H5Dwrite(d_alloc, H5T_NATIVE_INT32, space_none, space_no_extent,
                       H5P_DEFAULT, nullptr),
              0);
    EXPECT_LT(H5Dwrite(d_alloc, H5T_NATIVE_INT32, space_no_extent, space_none,
                       H5P_DEFAULT, nullptr),
              0);

    // Incompatible datatype conversion fails H5T_path_find.
    ScopedType opaque(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(opaque.valid());
    EXPECT_LT(H5Dread(d_alloc, opaque, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      buf.data()),
              0);
    EXPECT_LT(H5Dwrite(d_alloc, opaque, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kWriteData.data()),
              0);
  }

  // 6. Read-only file fails write with no write intent error.
  ASSERT_GE(H5Fflush(file, H5F_SCOPE_GLOBAL), 0);
  const ssize_t image_len = H5Fget_file_image(file, nullptr, 0);
  ASSERT_GT(image_len, 0);
  std::vector<uint8_t> image(static_cast<size_t>(image_len), 0);
  ASSERT_EQ(H5Fget_file_image(file, image.data(), image.size()), image_len);

  ScopedPlist ro_fapl = MakeCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl, image.data(), image.size()), 0);
  ScopedFile ro_file(H5Fopen("ro_dio.h5", H5F_ACC_RDONLY, ro_fapl));
  ASSERT_TRUE(ro_file.valid());
  ScopedDataset ro_dset(H5Dopen2(ro_file, "allocated", H5P_DEFAULT));
  ASSERT_TRUE(ro_dset.valid());
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Dwrite(ro_dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kWriteData.data()),
              0);
  }
}

TEST(H5DioTest, RankMismatchProjectionSingleAndMultiDataset) {
  ScopedFile file = CreateInMemoryFile("dio_proj.h5");
  ASSERT_TRUE(file.valid());

  // 2D dataset [4, 4] with 1x4 hyperslab slice vs 1D [8] mem space with
  // offset=3, count=4 (exercises buf_adj != 0 and projection cleanup).
  constexpr std::array<hsize_t, 2> kFileDims = {4, 4};
  constexpr std::array<hsize_t, 1> kMemDims = {8};
  ScopedSpace fspace_base(H5Screate_simple(2, kFileDims.data(), nullptr));
  ASSERT_TRUE(fspace_base.valid());

  ScopedDataset d1(H5Dcreate2(file, "d1", H5T_NATIVE_INT32, fspace_base,
                              H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedDataset d2(H5Dcreate2(file, "d2", H5T_NATIVE_INT32, fspace_base,
                              H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(d1.valid());
  ASSERT_TRUE(d2.valid());

  ScopedSpace fspace1(H5Dget_space(d1));
  ScopedSpace fspace2(H5Dget_space(d2));
  ScopedSpace mspace1(H5Screate_simple(1, kMemDims.data(), nullptr));
  ScopedSpace mspace2(H5Screate_simple(1, kMemDims.data(), nullptr));
  ASSERT_TRUE(fspace1.valid());
  ASSERT_TRUE(fspace2.valid());
  ASSERT_TRUE(mspace1.valid());
  ASSERT_TRUE(mspace2.valid());

  constexpr std::array<hsize_t, 2> kFStart = {2, 0};
  constexpr std::array<hsize_t, 2> kFCount = {1, 4};
  constexpr std::array<hsize_t, 1> kMStart = {3};
  constexpr std::array<hsize_t, 1> kMCount = {4};
  ASSERT_GE(H5Sselect_hyperslab(fspace1, H5S_SELECT_SET, kFStart.data(),
                                nullptr, kFCount.data(), nullptr),
            0);
  ASSERT_GE(H5Sselect_hyperslab(fspace2, H5S_SELECT_SET, kFStart.data(),
                                nullptr, kFCount.data(), nullptr),
            0);
  ASSERT_GE(H5Sselect_hyperslab(mspace1, H5S_SELECT_SET, kMStart.data(),
                                nullptr, kMCount.data(), nullptr),
            0);
  ASSERT_GE(H5Sselect_hyperslab(mspace2, H5S_SELECT_SET, kMStart.data(),
                                nullptr, kMCount.data(), nullptr),
            0);

  // Single-dataset projection write & read.
  constexpr std::array<int32_t, 8> kSingleIn = {0, 0, 0, 11, 22, 33, 44, 0};
  std::array<int32_t, 8> single_out = {};
  ASSERT_GE(H5Dwrite(d1, H5T_NATIVE_INT32, mspace1, fspace1, H5P_DEFAULT,
                     kSingleIn.data()),
            0);
  ASSERT_GE(H5Dread(d1, H5T_NATIVE_INT32, mspace1, fspace1, H5P_DEFAULT,
                    single_out.data()),
            0);
  EXPECT_EQ(single_out[3], 11);
  EXPECT_EQ(single_out[6], 44);

  // Multi-dataset projection write & read (allocates orig_mem_space array).
  constexpr std::array<int32_t, 8> kMultiIn2 = {0, 0, 0, 55, 66, 77, 88, 0};
  std::array<int32_t, 8> multi_out1 = {};
  std::array<int32_t, 8> multi_out2 = {};
  std::array<hid_t, 2> dsets = {d1.get(), d2.get()};
  std::array<hid_t, 2> types = {H5T_NATIVE_INT32, H5T_NATIVE_INT32};
  std::array<hid_t, 2> mspaces = {mspace1.get(), mspace2.get()};
  std::array<hid_t, 2> fspaces = {fspace1.get(), fspace2.get()};
  std::array<const void*, 2> wbufs = {kSingleIn.data(), kMultiIn2.data()};
  std::array<void*, 2> rbufs = {multi_out1.data(), multi_out2.data()};

  ASSERT_GE(H5Dwrite_multi(2, dsets.data(), types.data(), mspaces.data(),
                           fspaces.data(), H5P_DEFAULT, wbufs.data()),
            0);
  ASSERT_GE(H5Dread_multi(2, dsets.data(), types.data(), mspaces.data(),
                          fspaces.data(), H5P_DEFAULT, rbufs.data()),
            0);
  EXPECT_EQ(multi_out1[4], 22);
  EXPECT_EQ(multi_out2[5], 77);
}

TEST(H5DioTest, FilteredWriteValidationVlenAndCompoundConversion) {
  // 1. Custom filter whose can_apply succeeds on create and fails after reopen.
  g_allow_can_apply = true;
  const H5Z_class2_t filter_cls = {
      H5Z_CLASS_T_VERS, kToggleFilterId, 1, 1, "test_toggle_filter",
      ToggleFilterCanApply, nullptr, ToggleFilterFunc,
  };
  ASSERT_GE(H5Zregister(&filter_cls), 0);
  absl::Cleanup unregister_filter = [] { H5Zunregister(kToggleFilterId); };

  ScopedFile file = CreateInMemoryFile("dio_filters_types.h5");
  ASSERT_TRUE(file.valid());

  constexpr std::array<hsize_t, 1> kDims4 = {4};
  constexpr std::array<hsize_t, 1> kChunk2 = {2};
  ScopedSpace space4(H5Screate_simple(1, kDims4.data(), nullptr));
  ASSERT_TRUE(space4.valid());

  {
    ScopedPlist dcpl_filt(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_filt.valid());
    ASSERT_GE(H5Pset_chunk(dcpl_filt, 1, kChunk2.data()), 0);
    ASSERT_GE(H5Pset_filter(dcpl_filt, kToggleFilterId, H5Z_FLAG_MANDATORY, 0,
                            nullptr),
              0);

    ScopedDataset d_filt(H5Dcreate2(file, "filt", H5T_NATIVE_INT32, space4,
                                    H5P_DEFAULT, dcpl_filt, H5P_DEFAULT));
    ASSERT_TRUE(d_filt.valid());
    constexpr std::array<int32_t, 4> kVals = {1, 2, 3, 4};
    ASSERT_GE(H5Dwrite(d_filt, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kVals.data()),
              0);
    std::array<int32_t, 4> read_vals = {};
    ASSERT_GE(H5Dread(d_filt, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      read_vals.data()),
              0);
    EXPECT_THAT(read_vals, ElementsAre(1, 2, 3, 4));
  }

  // Reopen so checked_filters == false; flip g_allow_can_apply to false so
  // H5D__write hits H5Z_can_apply failure.
  {
    ScopedDataset reopened(H5Dopen2(file, "filt", H5P_DEFAULT));
    ASSERT_TRUE(reopened.valid());
    g_allow_can_apply = false;
    absl::Cleanup restore_flag = [] { g_allow_can_apply = true; };
    constexpr std::array<int32_t, 4> kVals = {5, 6, 7, 8};
    ScopedErrorSilence silence;
    EXPECT_LT(H5Dwrite(reopened, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, kVals.data()),
              0);
  }

  // 2. Late-allocated VLEN dataset write sets full_overwrite=false and
  //    VLEN type conversion sets need_bkg = H5T_BKG_YES.
  {
    ScopedType vl_i32(H5Tvlen_create(H5T_NATIVE_INT32));
    ScopedType vl_i64(H5Tvlen_create(H5T_NATIVE_INT64));
    ASSERT_TRUE(vl_i32.valid());
    ASSERT_TRUE(vl_i64.valid());

    ScopedPlist dcpl_late(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_late.valid());
    ASSERT_GE(H5Pset_alloc_time(dcpl_late, H5D_ALLOC_TIME_LATE), 0);

    ScopedDataset d_vl(H5Dcreate2(file, "vl_late", vl_i32, space4, H5P_DEFAULT,
                                  dcpl_late, H5P_DEFAULT));
    ASSERT_TRUE(d_vl.valid());

    std::array<int64_t, 2> e0 = {10, 20};
    std::array<int64_t, 1> e1 = {30};
    std::array<hvl_t, 4> w_vl = {{
        {e0.size(), e0.data()},
        {e1.size(), e1.data()},
        {e0.size(), e0.data()},
        {e1.size(), e1.data()},
    }};
    ASSERT_GE(
        H5Dwrite(d_vl, vl_i64, H5S_ALL, H5S_ALL, H5P_DEFAULT, w_vl.data()), 0);
  }

  // 3. Partial write on late-allocated non-VLEN dataset (full_overwrite=false)
  //    followed by compound subset write/read with H5Pset_preserve(true/false).
  {
    struct FullRec {
      int32_t a;
      int32_t b;
    };
    struct SubRec {
      int32_t b;
    };
    ScopedType dt_full(H5Tcreate(H5T_COMPOUND, sizeof(FullRec)));
    ScopedType dt_sub(H5Tcreate(H5T_COMPOUND, sizeof(SubRec)));
    ASSERT_TRUE(dt_full.valid());
    ASSERT_TRUE(dt_sub.valid());
    ASSERT_GE(H5Tinsert(dt_full, "a", HOFFSET(FullRec, a), H5T_NATIVE_INT32),
              0);
    ASSERT_GE(H5Tinsert(dt_full, "b", HOFFSET(FullRec, b), H5T_NATIVE_INT32),
              0);
    ASSERT_GE(H5Tinsert(dt_sub, "b", HOFFSET(SubRec, b), H5T_NATIVE_INT32), 0);

    ScopedPlist dcpl_late(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_late.valid());
    ASSERT_GE(H5Pset_alloc_time(dcpl_late, H5D_ALLOC_TIME_LATE), 0);
    ScopedDataset d_cmp(H5Dcreate2(file, "cmp_late", dt_full, space4,
                                   H5P_DEFAULT, dcpl_late, H5P_DEFAULT));
    ASSERT_TRUE(d_cmp.valid());

    // Partial write of 2 elements (full_overwrite = false).
    constexpr std::array<hsize_t, 1> kStart0 = {0};
    constexpr std::array<hsize_t, 1> kCount2 = {2};
    ScopedSpace fsub(H5Dget_space(d_cmp));
    ScopedSpace msub(H5Screate_simple(1, kCount2.data(), nullptr));
    ASSERT_TRUE(fsub.valid());
    ASSERT_TRUE(msub.valid());
    ASSERT_GE(H5Sselect_hyperslab(fsub, H5S_SELECT_SET, kStart0.data(), nullptr,
                                  kCount2.data(), nullptr),
              0);

    constexpr std::array<FullRec, 2> kInitSub = {{{1, 10}, {2, 20}}};
    ASSERT_GE(
        H5Dwrite(d_cmp, dt_full, msub, fsub, H5P_DEFAULT, kInitSub.data()), 0);

    // Write subset field `b` with preserve=true (H5T_BKG_YES) and selection I/O
    // ON so Phase 2/3 allocate background buffer and must_fill_bkg=true.
    ScopedPlist dxpl_sel_bkg(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl_sel_bkg.valid());
    ASSERT_GE(H5Pset_selection_io(dxpl_sel_bkg, H5D_SELECTION_IO_MODE_ON), 0);
    ASSERT_GE(H5Pset_preserve(dxpl_sel_bkg, true), 0);
    ASSERT_GE(H5Pset_modify_write_buf(dxpl_sel_bkg, true), 0);

    std::array<SubRec, 2> patch = {{{99}, {88}}};
    ASSERT_GE(
        H5Dwrite(d_cmp, dt_sub, msub, fsub, dxpl_sel_bkg, patch.data()), 0);

    std::array<FullRec, 2> read_back = {};
    ASSERT_GE(
        H5Dread(d_cmp, dt_full, msub, fsub, H5P_DEFAULT, read_back.data()), 0);
    EXPECT_EQ(read_back[0].a, 1);
    EXPECT_EQ(read_back[0].b, 99);
    EXPECT_EQ(read_back[1].a, 2);
    EXPECT_EQ(read_back[1].b, 88);
  }
}

TEST(H5DioTest, SelectionIoModesBufferSizingAndCustomVectorVfd) {
  VectorVfdEnv env;
  g_vec_env = &env;
  absl::Cleanup reset_env = [] { g_vec_env = nullptr; };

  const H5FD_class_t vec_cls = MakeVectorVfdClass(/*with_vector_ops=*/true);
  const hid_t vec_drv = H5FDregister(&vec_cls);
  ASSERT_NE(vec_drv, H5I_INVALID_HID);
  absl::Cleanup unreg_vec = [vec_drv] { H5FDunregister(vec_drv); };

  const H5FD_class_t novec_cls = MakeVectorVfdClass(/*with_vector_ops=*/false);
  const hid_t novec_drv = H5FDregister(&novec_cls);
  ASSERT_NE(novec_drv, H5I_INVALID_HID);
  absl::Cleanup unreg_novec = [novec_drv] { H5FDunregister(novec_drv); };

  // 1. Non-vector VFD with SELECTION_IO_MODE_DEFAULT disables selection I/O
  //    with H5D_SEL_IO_DEFAULT_OFF.
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_driver(fapl, novec_drv, nullptr), 0);
    ScopedFile file(H5Fcreate("novec.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(file.valid());

    constexpr std::array<hsize_t, 1> kDims = {4};
    ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(file, "d", H5T_NATIVE_INT32, space,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    ScopedPlist dxpl_def(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl_def.valid());
    constexpr std::array<int32_t, 4> kIn = {1, 2, 3, 4};
    ASSERT_GE(
        H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl_def, kIn.data()),
        0);
    uint32_t cause = 0;
    ASSERT_GE(H5Pget_no_selection_io_cause(dxpl_def, &cause), 0);
    EXPECT_NE(cause & H5D_SEL_IO_DEFAULT_OFF, 0u);
  }

  // 2. Vector-capable VFD exercises H5F_has_vector_select_io == true,
  //    selection I/O multi-dset with and without type conversion, tconv/bkg
  //    overflow fallback, H5T_BKG_TEMP converter, and VFD read/write errors.
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_driver(fapl, vec_drv, nullptr), 0);
    ScopedFile file(H5Fcreate("vec.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(file.valid());

    constexpr std::array<hsize_t, 1> kDims8 = {8};
    ScopedSpace space8(H5Screate_simple(1, kDims8.data(), nullptr));
    ASSERT_TRUE(space8.valid());
    ScopedDataset d1(H5Dcreate2(file, "d1", H5T_NATIVE_INT32, space8,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ScopedDataset d2(H5Dcreate2(file, "d2", H5T_NATIVE_INT32, space8,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d1.valid());
    ASSERT_TRUE(d2.valid());

    // 2a. Default DXPL automatically turns on selection I/O for multi-dset.
    std::array<int32_t, 8> w1 = {1, 2, 3, 4, 5, 6, 7, 8};
    std::array<int32_t, 8> w2 = {10, 20, 30, 40, 50, 60, 70, 80};
    std::array<hid_t, 2> dsets = {d1.get(), d2.get()};
    std::array<hid_t, 2> types = {H5T_NATIVE_INT32, H5T_NATIVE_INT32};
    std::array<hid_t, 2> spaces = {H5S_ALL, H5S_ALL};
    std::array<const void*, 2> wbufs = {w1.data(), w2.data()};
    ASSERT_GE(H5Dwrite_multi(2, dsets.data(), types.data(), spaces.data(),
                             spaces.data(), H5P_DEFAULT, wbufs.data()),
              0);
    EXPECT_GT(env.write_vector_calls, 0);

    std::array<int32_t, 8> r1 = {};
    std::array<int32_t, 8> r2 = {};
    std::array<void*, 2> rbufs = {r1.data(), r2.data()};
    ASSERT_GE(H5Dread_multi(2, dsets.data(), types.data(), spaces.data(),
                            spaces.data(), H5P_DEFAULT, rbufs.data()),
              0);
    EXPECT_GT(env.read_vector_calls, 0);
    EXPECT_THAT(r1, ElementsAre(1, 2, 3, 4, 5, 6, 7, 8));
    EXPECT_THAT(r2, ElementsAre(10, 20, 30, 40, 50, 60, 70, 80));

    // 2b. Selection I/O ON with type conversion exceeding max_temp_buf sets
    //     H5D_SEL_IO_TCONV_BUF_TOO_SMALL.
    ScopedPlist dxpl_tiny(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl_tiny.valid());
    ASSERT_GE(H5Pset_selection_io(dxpl_tiny, H5D_SELECTION_IO_MODE_ON), 0);
    ASSERT_GE(H5Pset_buffer(dxpl_tiny, 16, nullptr, nullptr), 0);
    std::array<int64_t, 8> w64 = {1, 2, 3, 4, 5, 6, 7, 8};
    std::array<int64_t, 8> r64 = {};
    // Non-in-place write (modify_write_buf=false) needs 8*8=64B > 16B.
    ASSERT_GE(H5Dwrite(d1, H5T_NATIVE_INT64, H5S_ALL, H5S_ALL, dxpl_tiny,
                       w64.data()),
              0);
    uint32_t cause = 0;
    ASSERT_GE(H5Pget_no_selection_io_cause(dxpl_tiny, &cause), 0);
    EXPECT_NE(cause & H5D_SEL_IO_TCONV_BUF_TOO_SMALL, 0u);

    // 2c. Selection I/O ON with in-place tconv (tconv_buf_size==0) and
    //     bkg_buf_size > max_temp_buf sets H5D_SEL_IO_BKG_BUF_TOO_SMALL.
    struct FullRec {
      int32_t a;
      int32_t b;
    };
    struct SubRec {
      int32_t b;
    };
    ScopedType dt_full(H5Tcreate(H5T_COMPOUND, sizeof(FullRec)));
    ScopedType dt_sub(H5Tcreate(H5T_COMPOUND, sizeof(SubRec)));
    ASSERT_TRUE(dt_full.valid());
    ASSERT_TRUE(dt_sub.valid());
    ASSERT_GE(H5Tinsert(dt_full, "a", HOFFSET(FullRec, a), H5T_NATIVE_INT32),
              0);
    ASSERT_GE(H5Tinsert(dt_full, "b", HOFFSET(FullRec, b), H5T_NATIVE_INT32),
              0);
    ASSERT_GE(H5Tinsert(dt_sub, "b", HOFFSET(SubRec, b), H5T_NATIVE_INT32), 0);

    ScopedDataset d_cmp(H5Dcreate2(file, "cmp_sel", dt_full, space8,
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_cmp.valid());
    std::array<FullRec, 8> w_cmp{};
    for (int32_t i = 0; i < 8; ++i) {
      w_cmp[static_cast<size_t>(i)] = {i, i * 10};
    }
    ASSERT_GE(H5Dwrite(d_cmp, dt_full, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       w_cmp.data()),
              0);

    // On read of 8 FullRec (64B) from 8 SubRec? No: mem_type=FullRec (8B >=
    // dst/src 4B) with contiguous H5S_ALL -> in_place_tconv=true (tconv=0),
    // while bkg_buf_size = 8 * 8 = 64B > max_temp_buf=16B!
    // Wait: need_bkg=H5T_BKG_YES on compound subset write: set modify_write_buf
    // so write uses in_place_tconv=true (mem=FullRec 8B >= file=FullRec/SubRec)!
    // Specifically: file=dt_full (8B), mem=dt_sub (4B) on read? For read,
    // mem_type_size must be >= file_type_size for in_place_tconv: so file=dt_sub
    // and mem=dt_full with H5Pset_preserve(true)!
    ScopedDataset d_sub(H5Dcreate2(file, "sub_file", dt_sub, space8,
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_sub.valid());
    std::array<SubRec, 8> w_sub{};
    ASSERT_GE(
        H5Dwrite(d_sub, dt_sub, H5S_ALL, H5S_ALL, H5P_DEFAULT, w_sub.data()),
        0);
    ScopedPlist dxpl_bkg_small(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl_bkg_small.valid());
    ASSERT_GE(H5Pset_selection_io(dxpl_bkg_small, H5D_SELECTION_IO_MODE_ON), 0);
    ASSERT_GE(H5Pset_preserve(dxpl_bkg_small, true), 0);
    ASSERT_GE(H5Pset_buffer(dxpl_bkg_small, 16, nullptr, nullptr), 0);
    std::array<FullRec, 8> r_cmp{};
    ASSERT_GE(H5Dread(d_sub, dt_full, H5S_ALL, H5S_ALL, dxpl_bkg_small,
                      r_cmp.data()),
              0);
    cause = 0;
    ASSERT_GE(H5Pget_no_selection_io_cause(dxpl_bkg_small, &cause), 0);
    EXPECT_NE(cause & H5D_SEL_IO_BKG_BUF_TOO_SMALL, 0u);

    // 2d. Register custom H5T_BKG_TEMP converter to hit need_bkg == H5T_BKG_TEMP
    //     (must_fill_bkg == false, bkg_buf allocated in selection I/O ON).
    ScopedType opq_a(H5Tcreate(H5T_OPAQUE, 4));
    ScopedType opq_b(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(opq_a.valid());
    ASSERT_TRUE(opq_b.valid());
    ASSERT_GE(H5Tset_tag(opq_a, "h5dio_bkg_temp_a"), 0);
    ASSERT_GE(H5Tset_tag(opq_b, "h5dio_bkg_temp_b"), 0);
    ASSERT_GE(H5Tregister(H5T_PERS_HARD, "h5dio_bkg_temp_conv", opq_a, opq_b,
                          OpaqueTempBkgConv),
              0);
    absl::Cleanup unreg_conv = [&] {
      H5Tunregister(H5T_PERS_HARD, "h5dio_bkg_temp_conv", opq_a, opq_b,
                    OpaqueTempBkgConv);
    };
    ScopedDataset d_opq(H5Dcreate2(file, "opq_b", opq_b, space8, H5P_DEFAULT,
                                   H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_opq.valid());
    std::array<uint32_t, 8> opq_in = {1, 2, 3, 4, 5, 6, 7, 8};
    ScopedPlist dxpl_sel_on(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl_sel_on.valid());
    ASSERT_GE(H5Pset_selection_io(dxpl_sel_on, H5D_SELECTION_IO_MODE_ON), 0);
    ASSERT_GE(
        H5Dwrite(d_opq, opq_a, H5S_ALL, H5S_ALL, dxpl_sel_on, opq_in.data()),
        0);

    // 2d-bis. Successful selection I/O ON type-converted read covers the
    //         H5D__scatgath_read_select success path.
    std::array<int64_t, 8> r64_sel = {};
    ASSERT_GE(H5Dread(d1, H5T_NATIVE_INT64, H5S_ALL, H5S_ALL, dxpl_sel_on,
                      r64_sel.data()),
              0);
    EXPECT_THAT(r64_sel, ElementsAre(1, 2, 3, 4, 5, 6, 7, 8));

    // 2e. User-supplied tconv & bkg buffers + tiny max_temp_buf error in
    //     SELECTION_IO_MODE_OFF (phase 3).
    std::vector<uint8_t> user_tconv(256, 0);
    std::vector<uint8_t> user_bkg(256, 0);
    ScopedPlist dxpl_user_buf(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl_user_buf.valid());
    ASSERT_GE(H5Pset_selection_io(dxpl_user_buf, H5D_SELECTION_IO_MODE_OFF), 0);
    ASSERT_GE(H5Pset_buffer(dxpl_user_buf, user_tconv.size(), user_tconv.data(),
                            user_bkg.data()),
              0);
    ASSERT_GE(H5Dread(d1, H5T_NATIVE_INT64, H5S_ALL, H5S_ALL, dxpl_user_buf,
                      r64.data()),
              0);

    ScopedPlist dxpl_too_small(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl_too_small.valid());
    ASSERT_GE(H5Pset_selection_io(dxpl_too_small, H5D_SELECTION_IO_MODE_OFF),
              0);
    ASSERT_GE(H5Pset_buffer(dxpl_too_small, 4, nullptr, nullptr), 0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Dread(d1, H5T_NATIVE_INT64, H5S_ALL, H5S_ALL, dxpl_too_small,
                        r64.data()),
                0);
    }

    // 2f. Large element (> 1 MiB H5D_TEMP_BUF_SIZE) with default DXPL and
    //     selection I/O OFF exercises target_size growth for default settings.
    constexpr std::array<hsize_t, 1> kBigArrDims = {300000};
    ScopedType arr_i32(H5Tarray_create2(H5T_NATIVE_INT32, 1,
                                        kBigArrDims.data()));
    ScopedType arr_i64(H5Tarray_create2(H5T_NATIVE_INT64, 1,
                                        kBigArrDims.data()));
    ASSERT_TRUE(arr_i32.valid());
    ASSERT_TRUE(arr_i64.valid());
    constexpr std::array<hsize_t, 1> kDims1 = {1};
    ScopedSpace space1(H5Screate_simple(1, kDims1.data(), nullptr));
    ASSERT_TRUE(space1.valid());
    ScopedDataset d_big(H5Dcreate2(file, "big_arr", arr_i32, space1,
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_big.valid());
    std::vector<int32_t> big_in(300000, 7);
    ASSERT_GE(
        H5Dwrite(d_big, arr_i32, H5S_ALL, H5S_ALL, H5P_DEFAULT, big_in.data()),
        0);
    ScopedPlist dxpl_off(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl_off.valid());
    ASSERT_GE(H5Pset_selection_io(dxpl_off, H5D_SELECTION_IO_MODE_OFF), 0);
    std::vector<int64_t> big_out(300000, 0);
    ASSERT_GE(H5Dread(d_big, arr_i64, H5S_ALL, H5S_ALL, dxpl_off,
                      big_out.data()),
              0);
    EXPECT_EQ(big_out[0], 7);
    EXPECT_EQ(big_out[299999], 7);

    // 2g. VFD failure injection across selection I/O OFF, ON (no tconv), and
    //     ON (type conversion -> H5D__scatgath_{read,write}_select).
    {
      ScopedErrorSilence silence;
      env.fail_write = true;
      EXPECT_LT(H5Dwrite(d1, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl_off,
                         w1.data()),
                0);
      EXPECT_LT(H5Dwrite_multi(2, dsets.data(), types.data(), spaces.data(),
                               spaces.data(), dxpl_sel_on, wbufs.data()),
                0);
      EXPECT_LT(H5Dwrite(d1, H5T_NATIVE_INT64, H5S_ALL, H5S_ALL, dxpl_sel_on,
                         w64.data()),
                0);
      env.fail_write = false;

      env.fail_read = true;
      EXPECT_LT(H5Dread(d1, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl_off,
                        r1.data()),
                0);
      EXPECT_LT(H5Dread_multi(2, dsets.data(), types.data(), spaces.data(),
                              spaces.data(), dxpl_sel_on, rbufs.data()),
                0);
      EXPECT_LT(H5Dread(d1, H5T_NATIVE_INT64, H5S_ALL, H5S_ALL, dxpl_sel_on,
                        r64.data()),
                0);
      env.fail_read = false;
    }
  }
}

TEST(H5DioTest, InternalErrorPathsForInvalidDsetAndLayoutCallbacks) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_sieve_buf_size(fapl, 0), 0);
  ScopedFile file(
      H5Fcreate("dio_internal.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(file.valid());

  constexpr std::array<hsize_t, 1> kDims4 = {4};
  ScopedSpace space4(H5Screate_simple(1, kDims4.data(), nullptr));
  ASSERT_TRUE(space4.valid());
  ScopedDataset dset_id(H5Dcreate2(file, "d", H5T_NATIVE_INT32, space4,
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset_id.valid());
  std::array<int32_t, 4> buf = {1, 2, 3, 4};
  ASSERT_GE(H5Dwrite(dset_id, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     buf.data()),
            0);

  auto* real_dset =
      static_cast<H5D_t*>(H5VL_object_verify(dset_id, H5I_DATASET));
  ASSERT_NE(real_dset, nullptr);
  auto* mem_type = static_cast<H5T_t*>(
      H5I_object_verify(H5T_NATIVE_INT32, H5I_DATATYPE));
  ASSERT_NE(mem_type, nullptr);

  // 1. count=2 with dinfo[1].dset == nullptr and dinfo[1].dset->oloc.file ==
  //    nullptr covers the defensive dataset/file checks in H5D__read/write.
  {
    std::array<H5D_dset_io_info_t, 2> dinfo{};
    dinfo[0].dset = real_dset;
    dinfo[0].file_space = real_dset->shared->space;
    dinfo[0].mem_space = real_dset->shared->space;
    dinfo[0].mem_type = mem_type;
    dinfo[0].buf.vp = buf.data();

    dinfo[1] = dinfo[0];
    dinfo[1].dset = nullptr;

    ScopedApiContext ctx;
    ScopedErrorSilence silence;
    EXPECT_LT(H5D__read(2, dinfo.data()), 0);
    EXPECT_LT(H5D__write(2, dinfo.data()), 0);

    H5D_t no_file_dset = *real_dset;
    no_file_dset.oloc.file = nullptr;
    dinfo[1].dset = &no_file_dset;
    EXPECT_LT(H5D__read(2, dinfo.data()), 0);
    EXPECT_LT(H5D__write(2, dinfo.data()), 0);
  }

  // 2. Custom layout_ops on a stack copy of H5D_t/H5D_shared_t triggers
  //    io_init failure and io_term failure cleanup paths cleanly.
  {
    H5D_shared_t custom_shared = *real_dset->shared;
    H5D_t custom_dset = *real_dset;
    custom_dset.shared = &custom_shared;

    H5D_layout_ops_t fail_init_ops = *H5D_LOPS_CONTIG;
    fail_init_ops.io_init = [](H5D_io_info_t* /*io*/,
                               H5D_dset_io_info_t* /*di*/) -> herr_t {
      return -1;
    };
    custom_shared.layout.ops = &fail_init_ops;

    H5D_dset_io_info_t dinfo{};
    dinfo.dset = &custom_dset;
    dinfo.file_space = custom_shared.space;
    dinfo.mem_space = custom_shared.space;
    dinfo.mem_type = mem_type;
    dinfo.buf.vp = buf.data();

    ScopedApiContext ctx;
    ScopedErrorSilence silence;
    EXPECT_LT(H5D__read(1, &dinfo), 0);
    EXPECT_LT(H5D__write(1, &dinfo), 0);

    // Now test io_term failure after io_init succeeds, delegating to the
    // real io_term so contig_piece_info is freed cleanly and flushing any
    // sieve buffer allocated on custom_shared.
    H5D_layout_ops_t fail_term_ops = *H5D_LOPS_CONTIG;
    fail_term_ops.io_term = [](H5D_io_info_t* io,
                               H5D_dset_io_info_t* di) -> herr_t {
      if (H5D_LOPS_CONTIG->io_term != nullptr) {
        (void)H5D_LOPS_CONTIG->io_term(io, di);
      }
      return -1;
    };
    custom_shared.layout.ops = &fail_term_ops;
    absl::Cleanup flush_custom_dset = [&] {
      if (H5D_LOPS_CONTIG->flush != nullptr) {
        (void)H5D_LOPS_CONTIG->flush(&custom_dset);
      }
    };
    EXPECT_LT(H5D__read(1, &dinfo), 0);
    EXPECT_LT(H5D__write(1, &dinfo), 0);
  }

  // 3. Unallocated read H5D__fill conversion failure (H5Dio.c:269), plus
  //    internal invalid fill metadata (254), invalid dcpl_id flush failure
  //    (573), and storage alloc failure on write (726).
  {
    ScopedType opq_fa(H5Tcreate(H5T_OPAQUE, 4));
    ScopedType opq_fb(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(opq_fa.valid());
    ASSERT_TRUE(opq_fb.valid());
    ASSERT_GE(H5Tset_tag(opq_fa, "h5dio_fill_fail_a"), 0);
    ASSERT_GE(H5Tset_tag(opq_fb, "h5dio_fill_fail_b"), 0);
    const auto fail_conv = [](hid_t /*src_id*/, hid_t /*dst_id*/,
                              H5T_cdata_t* cdata, size_t /*nelmts*/,
                              size_t /*buf_stride*/, size_t /*bkg_stride*/,
                              void* /*buf*/, void* /*bkg*/,
                              hid_t /*dxpl_id*/) -> herr_t {
      switch (cdata->command) {
        case H5T_CONV_INIT:
          return 0;
        case H5T_CONV_CONV:
          return -1;
        case H5T_CONV_FREE:
          return 0;
        default:
          return -1;
      }
    };
    ASSERT_GE(H5Tregister(H5T_PERS_HARD, "h5dio_fill_fail_conv", opq_fa, opq_fb,
                          fail_conv),
              0);
    absl::Cleanup unreg_fail = [&] {
      H5Tunregister(H5T_PERS_HARD, "h5dio_fill_fail_conv", opq_fa, opq_fb,
                    fail_conv);
    };
    ScopedPlist dcpl_fill(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_fill.valid());
    ASSERT_GE(H5Pset_alloc_time(dcpl_fill, H5D_ALLOC_TIME_LATE), 0);
    ASSERT_GE(H5Pset_fill_time(dcpl_fill, H5D_FILL_TIME_ALLOC), 0);
    const uint32_t kFillVal = 0x11223344u;
    ASSERT_GE(H5Pset_fill_value(dcpl_fill, opq_fa, &kFillVal), 0);
    ScopedDataset d_unalloc_fail(H5Dcreate2(file, "d_unalloc_fail", opq_fa,
                                            space4, H5P_DEFAULT, dcpl_fill,
                                            H5P_DEFAULT));
    ASSERT_TRUE(d_unalloc_fail.valid());
    std::array<uint32_t, 4> opq_out = {};
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Dread(d_unalloc_fail, opq_fb, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                        opq_out.data()),
                0);
    }
  }

  {
    H5D_shared_t bad_shared = *real_dset->shared;
    H5D_t bad_dset = *real_dset;
    bad_dset.shared = &bad_shared;
    H5D_dset_io_info_t dinfo{};
    dinfo.dset = &bad_dset;
    dinfo.file_space = bad_shared.space;
    dinfo.mem_space = bad_shared.space;
    dinfo.mem_type = mem_type;
    dinfo.buf.vp = buf.data();

    ScopedApiContext ctx;
    ScopedErrorSilence silence;

    // 3a. Invalid fill combo (size=-1 with non-null buf) on unallocated contig
    //     storage hits H5P_is_fill_value_defined failure in H5D__read (254).
    int32_t dummy_fill = 0;
    bad_shared.layout.storage.u.contig.addr = HADDR_UNDEF;
    bad_shared.dcpl_cache.fill.size = -1;
    bad_shared.dcpl_cache.fill.buf = &dummy_fill;
    EXPECT_LT(H5D__read(1, &dinfo), 0);

    // 3b. checked_filters=false with invalid dcpl_id hits
    //     H5D_flush_layout_to_dcpl failure in H5D__write (573).
    bad_shared.layout.storage.u.contig = real_dset->shared->layout.storage.u.contig;
    bad_shared.dcpl_cache.fill = real_dset->shared->dcpl_cache.fill;
    bad_shared.checked_filters = false;
    bad_shared.dcpl_id = H5I_INVALID_HID;
    EXPECT_LT(H5D__write(1, &dinfo), 0);

    // 3c. Unallocated contig with huge size triggers H5D__alloc_storage failure
    //     in H5D__write (726).
    bad_shared.checked_filters = true;
    bad_shared.dcpl_cache.fill.alloc_time = H5D_ALLOC_TIME_LATE;
    bad_shared.layout.storage.u.contig.addr = HADDR_UNDEF;
    bad_shared.layout.storage.u.contig.size = HADDR_MAX;
    EXPECT_LT(H5D__write(1, &dinfo), 0);
  }

  // 4. Non-DXPL plist bound to API context progressively triggers H5CX getter
  //    failures in typeinfo_init (1100, 1116), phase2 (1214 -> 307, 745), and
  //    phase3 (1392, 1394, 1432 -> 318, 756).
  {
    ScopedPlist non_dxpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(non_dxpl.valid());
    auto* mem_i64 = static_cast<H5T_t*>(
        H5I_object_verify(H5T_NATIVE_INT64, H5I_DATATYPE));
    ASSERT_NE(mem_i64, nullptr);
    std::array<int64_t, 4> buf64 = {1, 2, 3, 4};

    H5D_dset_io_info_t dinfo{};
    dinfo.dset = real_dset;
    dinfo.file_space = real_dset->shared->space;
    dinfo.mem_space = real_dset->shared->space;
    dinfo.mem_type = mem_i64;
    dinfo.buf.vp = buf64.data();

    ScopedApiContext ctx(non_dxpl.get());
    H5CX_t* c = ctx.mutable_ctx();
    c->selection_io_mode = H5D_SELECTION_IO_MODE_ON;
    c->selection_io_mode_valid = true;
    c->modify_write_buf = false;
    c->modify_write_buf_valid = true;
    // Let H5T_path_find get dt_conv_cb cleanly before probing H5Dio.c H5CX calls.
    c->dt_conv_cb_valid = true;

    ScopedErrorSilence silence;
    // Step 1: H5CX_get_data_transform fails (1100).
    EXPECT_LT(H5D__read(1, &dinfo), 0);
    // Step 2: H5CX_get_bkgr_buf_type fails (1116).
    c->data_transform = nullptr;
    c->data_transform_valid = true;
    EXPECT_LT(H5D__read(1, &dinfo), 0);
    // Step 3: H5D__typeinfo_init_phase2 H5CX_get_max_temp_buf fails (1214).
    c->bkgr_buf_type = H5T_BKG_NO;
    c->bkgr_buf_type_valid = true;
    EXPECT_LT(H5D__read(1, &dinfo), 0);
    EXPECT_LT(H5D__write(1, &dinfo), 0);
    // Step 4: H5D__typeinfo_init_phase3 H5CX_get_tconv_buf fails (1392).
    c->max_temp_buf = H5D_TEMP_BUF_SIZE;
    c->max_temp_buf_valid = true;
    EXPECT_LT(H5D__read(1, &dinfo), 0);
    EXPECT_LT(H5D__write(1, &dinfo), 0);
    // Step 5: H5D__typeinfo_init_phase3 H5CX_get_bkgr_buf fails (1394).
    c->tconv_buf = nullptr;
    c->tconv_buf_valid = true;
    EXPECT_LT(H5D__read(1, &dinfo), 0);
    // Step 6: SELECTION_IO_MODE_OFF phase3 H5CX_get_max_temp_buf fails (1432).
    c->bkgr_buf = nullptr;
    c->bkgr_buf_valid = true;
    c->selection_io_mode = H5D_SELECTION_IO_MODE_OFF;
    c->max_temp_buf_valid = false;
    EXPECT_LT(H5D__read(1, &dinfo), 0);
  }
}

}  // namespace
}  // namespace hdf5
