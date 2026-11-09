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
// Tests scatter/gather dataset I/O (`H5D__scatgath_read`, `H5D__scatgath_write`,
// `H5D__scatgath_read_select`, `H5D__scatgath_write_select`,
// `H5D__scatter_mem`, and `H5D__gather_mem`), including strip-mining,
// multi-sequence vector batching (>1024 sequences), in-place type conversion,
// data transforms, compound subset optimizations, custom converters with
// background buffers (`H5T_BKG_TEMP`, `H5T_BKG_YES`), multi-dataset selection
// I/O, and error paths (H5Dscatgath.c).
//
// Google3 build caveats:
//   - Compiles `:hdf5` with `-DNDEBUG` (internal structs omit debug fields).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Dscatgath.c

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
#define H5DSCATGATH_TEST_RESTORE_UNDEF_NDEBUG 1
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
#include "third_party/hdf5/src/H5Tprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#undef try
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5DSCATGATH_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5DSCATGATH_TEST_RESTORE_UNDEF_NDEBUG
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

  void CacheSelectIoProps() {
    node_.ctx.no_selection_io_cause = 0;
    node_.ctx.no_selection_io_cause_valid = true;
    node_.ctx.actual_selection_io_mode = 0;
    node_.ctx.actual_selection_io_mode_valid = true;
    node_.ctx.dt_conv_cb = H5T_conv_cb_t{};
    node_.ctx.dt_conv_cb_valid = true;
  }

  void CacheVecSize(size_t vec_size) {
    node_.ctx.vec_size = vec_size;
    node_.ctx.vec_size_valid = true;
  }

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

ScopedFile CreateInMemoryZeroSieveFile(const char* name) {
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid() || H5Pset_sieve_buf_size(fapl.get(), 0) < 0) {
    return ScopedFile();
  }
  return ScopedFile(H5Fcreate(name, H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
}

struct SgFailVfdEnv {
  bool fail_read = false;
  bool fail_write = false;
  unsigned long next_fileno = 1;
};

SgFailVfdEnv* g_sg_vfd_env = nullptr;

struct SgVfdFile {
  H5FD_t pub{};
  std::vector<uint8_t> data;
  haddr_t eoa = 0;
};

H5FD_t* SgOpen(const char* name, unsigned /*flags*/, hid_t /*fapl_id*/,
               haddr_t maxaddr) {
  if (g_sg_vfd_env == nullptr || name == nullptr || *name == '\0') {
    return nullptr;
  }
  auto* file = new SgVfdFile();
  file->pub.fileno = g_sg_vfd_env->next_fileno++;
  file->pub.maxaddr = maxaddr;
  return reinterpret_cast<H5FD_t*>(file);
}

herr_t SgClose(H5FD_t* _file) {
  delete reinterpret_cast<SgVfdFile*>(_file);
  return 0;
}

int SgCmp(const H5FD_t* f1, const H5FD_t* f2) {
  if (f1->fileno < f2->fileno) return -1;
  if (f1->fileno > f2->fileno) return 1;
  return 0;
}

herr_t SgQuery(const H5FD_t* /*file*/, unsigned long* flags) {
  *flags = 0;
  return 0;
}

haddr_t SgGetEoa(const H5FD_t* _file, H5FD_mem_t /*type*/) {
  return reinterpret_cast<const SgVfdFile*>(_file)->eoa;
}

herr_t SgSetEoa(H5FD_t* _file, H5FD_mem_t /*type*/, haddr_t addr) {
  auto* file = reinterpret_cast<SgVfdFile*>(_file);
  file->eoa = addr;
  if (file->data.size() < static_cast<size_t>(addr)) {
    file->data.resize(static_cast<size_t>(addr), 0);
  }
  return 0;
}

haddr_t SgGetEof(const H5FD_t* _file, H5FD_mem_t /*type*/) {
  return static_cast<haddr_t>(
      reinterpret_cast<const SgVfdFile*>(_file)->data.size());
}

herr_t SgRead(H5FD_t* _file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
              haddr_t addr, size_t size, void* buf) {
  if (g_sg_vfd_env != nullptr && g_sg_vfd_env->fail_read) {
    return -1;
  }
  const auto* file = reinterpret_cast<const SgVfdFile*>(_file);
  std::memset(buf, 0, size);
  const size_t offset = static_cast<size_t>(addr);
  if (offset < file->data.size()) {
    const size_t avail = std::min(size, file->data.size() - offset);
    std::memcpy(buf, file->data.data() + offset, avail);
  }
  return 0;
}

herr_t SgWrite(H5FD_t* _file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
               haddr_t addr, size_t size, const void* buf) {
  if (g_sg_vfd_env != nullptr && g_sg_vfd_env->fail_write) {
    return -1;
  }
  auto* file = reinterpret_cast<SgVfdFile*>(_file);
  const size_t offset = static_cast<size_t>(addr);
  if (offset + size > file->data.size()) {
    file->data.resize(offset + size, 0);
  }
  std::memcpy(file->data.data() + offset, buf, size);
  return 0;
}

H5FD_class_t MakeSgVfdClass() {
  H5FD_class_t cls{};
  cls.version = H5FD_CLASS_VERSION;
  cls.value = static_cast<H5FD_class_value_t>(334);
  cls.name = "test_dscatgath_fail_vfd";
  cls.maxaddr = (static_cast<haddr_t>(1) << 28) - 1;
  cls.fc_degree = H5F_CLOSE_WEAK;
  cls.open = SgOpen;
  cls.close = SgClose;
  cls.cmp = SgCmp;
  cls.query = SgQuery;
  cls.get_eoa = SgGetEoa;
  cls.set_eoa = SgSetEoa;
  cls.get_eof = SgGetEof;
  cls.read = SgRead;
  cls.write = SgWrite;
  const H5FD_mem_t fl_map[H5FD_MEM_NTYPES] = H5FD_FLMAP_SINGLE;
  std::memcpy(cls.fl_map, fl_map, sizeof(cls.fl_map));
  return cls;
}

herr_t FailSgIterInit(H5S_t* /*space*/, H5S_sel_iter_t* sel_iter) {
  static const H5S_sel_iter_class_t kDummyIterClass = {};
  sel_iter->type = &kDummyIterClass;
  return -1;
}

bool g_fail_on_second_init = false;
int g_init_call_count = 0;

herr_t FailSecondSgIterInit(H5S_t* space, H5S_sel_iter_t* sel_iter) {
  ++g_init_call_count;
  if (g_fail_on_second_init && g_init_call_count == 2) {
    static const H5S_sel_iter_class_t kDummyIterClass = {};
    sel_iter->type = &kDummyIterClass;
    return -1;
  }
  return H5S_sel_all[0].iter_init(space, sel_iter);
}

herr_t FailSgGetSeqList(H5S_sel_iter_t* /*iter*/, size_t /*maxseq*/,
                        size_t /*maxbytes*/, size_t* /*nseq*/,
                        size_t* /*nbytes*/, hsize_t* /*off*/, size_t* /*len*/) {
  return -1;
}

herr_t FailingSeqSgIterInit(H5S_t* space, H5S_sel_iter_t* sel_iter) {
  if (H5S_sel_all[0].iter_init(space, sel_iter) < 0) {
    return -1;
  }
  static const H5S_sel_iter_class_t kFailSeqIterClass = [&]() {
    H5S_sel_iter_class_t cls = *sel_iter->type;
    cls.iter_get_seq_list = FailSgGetSeqList;
    return cls;
  }();
  sel_iter->type = &kFailSeqIterClass;
  return 0;
}

H5S_sel_iter_release_func_t g_orig_all_iter_release = nullptr;

herr_t FailSgIterRel(H5S_sel_iter_t* iter) {
  if (g_orig_all_iter_release != nullptr) {
    g_orig_all_iter_release(iter);
  }
  return -1;
}

herr_t FailingRelSgIterInit(H5S_t* space, H5S_sel_iter_t* sel_iter) {
  if (H5S_sel_all[0].iter_init(space, sel_iter) < 0) {
    return -1;
  }
  if (g_orig_all_iter_release == nullptr) {
    g_orig_all_iter_release = sel_iter->type->iter_release;
  }
  static const H5S_sel_iter_class_t kFailRelIterClass = [&]() {
    H5S_sel_iter_class_t cls = *sel_iter->type;
    cls.iter_release = FailSgIterRel;
    return cls;
  }();
  sel_iter->type = &kFailRelIterClass;
  return 0;
}

// Converts between 4-byte and 8-byte opaque values while exercising H5T_BKG_YES.
// dst = orig_dst_first4 (from bkg) concatenated with inverted/copied src, or
// on reverse compresses back to 4 bytes while verifying bkg is passed cleanly.
herr_t Opaque4To8BkgYesConv(hid_t /*src_id*/, hid_t /*dst_id*/,
                           H5T_cdata_t* cdata, size_t nelmts,
                           size_t /*buf_stride*/, size_t /*bkg_stride*/,
                           void* buf, void* bkg, hid_t /*dxpl_id*/) {
  switch (cdata->command) {
    case H5T_CONV_INIT:
      cdata->need_bkg = H5T_BKG_YES;
      return 0;
    case H5T_CONV_CONV: {
      if (buf == nullptr || bkg == nullptr) {
        return -1;
      }
      auto* bytes = static_cast<uint8_t*>(buf);
      const auto* bkg_bytes = static_cast<const uint8_t*>(bkg);
      // Iterate backward because 4->8 grows elements in buf cleanly.
      for (size_t idx = nelmts; idx > 0; --idx) {
        const size_t i = idx - 1;
        uint32_t src_val = 0;
        std::memcpy(&src_val, bytes + i * 4, sizeof(src_val));
        uint64_t bkg_val = 0;
        std::memcpy(&bkg_val, bkg_bytes + i * 8, sizeof(bkg_val));
        const uint32_t hi = static_cast<uint32_t>(bkg_val >> 32);
        const uint64_t dst_val =
            (static_cast<uint64_t>(hi) << 32) | static_cast<uint64_t>(src_val);
        std::memcpy(bytes + i * 8, &dst_val, sizeof(dst_val));
      }
      return 0;
    }
    case H5T_CONV_FREE:
      return 0;
    default:
      return -1;
  }
}

herr_t Opaque8To4BkgYesConv(hid_t /*src_id*/, hid_t /*dst_id*/,
                           H5T_cdata_t* cdata, size_t nelmts,
                           size_t /*buf_stride*/, size_t /*bkg_stride*/,
                           void* buf, void* bkg, hid_t /*dxpl_id*/) {
  switch (cdata->command) {
    case H5T_CONV_INIT:
      cdata->need_bkg = H5T_BKG_YES;
      return 0;
    case H5T_CONV_CONV: {
      if (buf == nullptr || bkg == nullptr) {
        return -1;
      }
      auto* bytes = static_cast<uint8_t*>(buf);
      const auto* bkg_bytes = static_cast<const uint8_t*>(bkg);
      for (size_t i = 0; i < nelmts; ++i) {
        uint64_t src_val = 0;
        std::memcpy(&src_val, bytes + i * 8, sizeof(src_val));
        uint32_t bkg_val = 0;
        std::memcpy(&bkg_val, bkg_bytes + i * 4, sizeof(bkg_val));
        const uint32_t combined =
            static_cast<uint32_t>(src_val) ^ (bkg_val & 0u);
        std::memcpy(bytes + i * 4, &combined, sizeof(combined));
      }
      return 0;
    }
    case H5T_CONV_FREE:
      return 0;
    default:
      return -1;
  }
}

// 4-byte to 4-byte converter requesting H5T_BKG_TEMP scratch storage.
herr_t Opaque4To4BkgTempConv(hid_t /*src_id*/, hid_t /*dst_id*/,
                            H5T_cdata_t* cdata, size_t nelmts,
                            size_t /*buf_stride*/, size_t /*bkg_stride*/,
                            void* buf, void* bkg, hid_t /*dxpl_id*/) {
  switch (cdata->command) {
    case H5T_CONV_INIT:
      cdata->need_bkg = H5T_BKG_TEMP;
      return 0;
    case H5T_CONV_CONV: {
      if (buf == nullptr || bkg == nullptr) {
        return -1;
      }
      std::memcpy(bkg, buf, nelmts * 4);
      auto* dst = static_cast<uint8_t*>(buf);
      const auto* tmp = static_cast<const uint8_t*>(bkg);
      for (size_t i = 0; i < nelmts * 4; ++i) {
        dst[i] = static_cast<uint8_t>(tmp[i] ^ 0x5Au);
      }
      return 0;
    }
    case H5T_CONV_FREE:
      return 0;
    default:
      return -1;
  }
}

// Converter between non-native 24-bit-in-32-bit int and native int32 with
// H5T_BKG_YES so data transform evaluation runs alongside must_fill_bkg.
herr_t Int24AndInt32BkgYesConv(hid_t /*src_id*/, hid_t /*dst_id*/,
                              H5T_cdata_t* cdata, size_t nelmts,
                              size_t /*buf_stride*/, size_t /*bkg_stride*/,
                              void* buf, void* bkg, hid_t /*dxpl_id*/) {
  switch (cdata->command) {
    case H5T_CONV_INIT:
      cdata->need_bkg = H5T_BKG_YES;
      return 0;
    case H5T_CONV_CONV: {
      if (buf == nullptr || bkg == nullptr) {
        return -1;
      }
      // 4-byte to 4-byte identity transform while still consuming bkg.
      const auto* bkg_vals = static_cast<const int32_t*>(bkg);
      auto* vals = static_cast<int32_t*>(buf);
      for (size_t i = 0; i < nelmts; ++i) {
        vals[i] = vals[i] + (bkg_vals[i] & 0);
      }
      return 0;
    }
    case H5T_CONV_FREE:
      return 0;
    default:
      return -1;
  }
}

// Converter that succeeds INIT and always fails CONV.
herr_t FailingConv(hid_t /*src_id*/, hid_t /*dst_id*/, H5T_cdata_t* cdata,
                   size_t /*nelmts*/, size_t /*buf_stride*/,
                   size_t /*bkg_stride*/, void* /*buf*/, void* /*bkg*/,
                   hid_t /*dxpl_id*/) {
  switch (cdata->command) {
    case H5T_CONV_INIT:
      cdata->need_bkg = H5T_BKG_NO;
      return 0;
    case H5T_CONV_CONV:
      return -1;
    case H5T_CONV_FREE:
      return 0;
    default:
      return -1;
  }
}

herr_t FailingBkgYesConv(hid_t /*src_id*/, hid_t /*dst_id*/, H5T_cdata_t* cdata,
                         size_t /*nelmts*/, size_t /*buf_stride*/,
                         size_t /*bkg_stride*/, void* /*buf*/, void* /*bkg*/,
                         hid_t /*dxpl_id*/) {
  switch (cdata->command) {
    case H5T_CONV_INIT:
      cdata->need_bkg = H5T_BKG_YES;
      return 0;
    case H5T_CONV_CONV:
      return -1;
    case H5T_CONV_FREE:
      return 0;
    default:
      return -1;
  }
}

struct FullRecord {
  int32_t a;
  double b;
  int32_t c;
};

struct SubRecord {
  int32_t a;
  double b;
};

ScopedType MakeFullCompoundType() {
  ScopedType t(H5Tcreate(H5T_COMPOUND, sizeof(FullRecord)));
  if (!t.valid()) return t;
  if (H5Tinsert(t, "a", HOFFSET(FullRecord, a), H5T_NATIVE_INT32) < 0 ||
      H5Tinsert(t, "b", HOFFSET(FullRecord, b), H5T_NATIVE_DOUBLE) < 0 ||
      H5Tinsert(t, "c", HOFFSET(FullRecord, c), H5T_NATIVE_INT32) < 0) {
    t.reset();
  }
  return t;
}

ScopedType MakeSubCompoundType() {
  ScopedType t(H5Tcreate(H5T_COMPOUND, sizeof(SubRecord)));
  if (!t.valid()) return t;
  if (H5Tinsert(t, "a", HOFFSET(SubRecord, a), H5T_NATIVE_INT32) < 0 ||
      H5Tinsert(t, "b", HOFFSET(SubRecord, b), H5T_NATIVE_DOUBLE) < 0) {
    t.reset();
  }
  return t;
}

TEST(H5DscatgathTest, StripMiningAndMultiVectorBatchingWithTypeConversion) {
  ScopedFile file = CreateInMemoryFile("sg_strip_vec.h5");
  ASSERT_TRUE(file.valid());

  constexpr hsize_t kTotal = 3000;
  constexpr hsize_t kHalf = 1500;
  constexpr hsize_t kStart0 = 0;
  constexpr hsize_t kStride2 = 2;
  constexpr hsize_t kBlock1 = 1;

  ScopedSpace space(H5Screate_simple(1, &kTotal, nullptr));
  ASSERT_TRUE(space.valid());

  ScopedDataset dset(H5Dcreate2(file, "ints", H5T_STD_I32LE, space, H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ASSERT_GE(H5Pset_selection_io(dxpl, H5D_SELECTION_IO_MODE_OFF), 0);
  // Force multi-chunk strip mining when converting 4B <-> 8B integers.
  ASSERT_GE(H5Pset_buffer(dxpl, 32, nullptr, nullptr), 0);

  // 1. Contiguous strip-mined write and read with int64 -> int32 conversion.
  std::vector<int64_t> full_w(kTotal);
  std::iota(full_w.begin(), full_w.end(), 100);
  ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT64, H5S_ALL, H5S_ALL, dxpl,
                     full_w.data()),
            0);

  std::vector<int64_t> full_r(kTotal, -1);
  ASSERT_GE(
      H5Dread(dset, H5T_NATIVE_INT64, H5S_ALL, H5S_ALL, dxpl, full_r.data()),
      0);
  EXPECT_EQ(full_r, full_w);

  // 2. Strided file space (>1024 sequences) with contiguous memory space:
  //    hits multi-loop H5D__scatter_file and H5D__gather_file.
  ScopedPlist big_buf_dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(big_buf_dxpl.valid());
  ASSERT_GE(H5Pset_selection_io(big_buf_dxpl, H5D_SELECTION_IO_MODE_OFF), 0);

  ScopedSpace mspace_contig(H5Screate_simple(1, &kHalf, nullptr));
  ASSERT_TRUE(mspace_contig.valid());
  ScopedSpace fspace_strided(H5Dget_space(dset));
  ASSERT_TRUE(fspace_strided.valid());
  ASSERT_GE(H5Sselect_hyperslab(fspace_strided, H5S_SELECT_SET, &kStart0,
                                &kStride2, &kHalf, &kBlock1),
            0);

  std::vector<int64_t> even_w(kHalf);
  for (size_t i = 0; i < kHalf; ++i) {
    even_w[i] = static_cast<int64_t>(10000 + i);
  }
  ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT64, mspace_contig, fspace_strided,
                     big_buf_dxpl, even_w.data()),
            0);

  std::vector<int64_t> even_r(kHalf, -1);
  ASSERT_GE(H5Dread(dset, H5T_NATIVE_INT64, mspace_contig, fspace_strided,
                    big_buf_dxpl, even_r.data()),
            0);
  EXPECT_EQ(even_r, even_w);

  // 3. Strided memory space (>1024 sequences) with contiguous file space:
  //    hits multi-loop H5D__gather_mem and H5D__scatter_mem.
  ScopedSpace mspace_strided(H5Screate_simple(1, &kTotal, nullptr));
  ASSERT_TRUE(mspace_strided.valid());
  ASSERT_GE(H5Sselect_hyperslab(mspace_strided, H5S_SELECT_SET, &kStart0,
                                &kStride2, &kHalf, &kBlock1),
            0);
  ScopedSpace fspace_prefix(H5Dget_space(dset));
  ASSERT_TRUE(fspace_prefix.valid());
  ASSERT_GE(H5Sselect_hyperslab(fspace_prefix, H5S_SELECT_SET, &kStart0, nullptr,
                                &kHalf, nullptr),
            0);

  std::vector<int64_t> mem_strided_w(kTotal, -1);
  for (size_t i = 0; i < kHalf; ++i) {
    mem_strided_w[2 * i] = static_cast<int64_t>(20000 + i);
  }
  ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT64, mspace_strided, fspace_prefix,
                     big_buf_dxpl, mem_strided_w.data()),
            0);

  std::vector<int64_t> mem_strided_r(kTotal, -99);
  ASSERT_GE(H5Dread(dset, H5T_NATIVE_INT64, mspace_strided, fspace_prefix,
                    big_buf_dxpl, mem_strided_r.data()),
            0);
  EXPECT_EQ(mem_strided_r[0], 20000);
  EXPECT_EQ(mem_strided_r[1], -99);
  EXPECT_EQ(mem_strided_r[2998], 21499);

  // 4. Custom hyper_vector_size > H5D_IO_VECTOR_SIZE across all helpers.
  ASSERT_GE(H5Pset_hyper_vector_size(big_buf_dxpl, 2048), 0);
  ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT64, mspace_strided, fspace_strided,
                     big_buf_dxpl, mem_strided_w.data()),
            0);
  std::fill(mem_strided_r.begin(), mem_strided_r.end(), -77);
  ASSERT_GE(H5Dread(dset, H5T_NATIVE_INT64, mspace_strided, fspace_strided,
                    big_buf_dxpl, mem_strided_r.data()),
            0);
  EXPECT_EQ(mem_strided_r[0], 20000);
  EXPECT_EQ(mem_strided_r[1], -77);
  EXPECT_EQ(mem_strided_r[2998], 21499);
}

TEST(H5DscatgathTest, InPlaceConversionAndDataTransformsInBothIoModes) {
  ScopedFile file = CreateInMemoryZeroSieveFile("sg_inplace_xform.h5");
  ASSERT_TRUE(file.valid());

  constexpr hsize_t kDim = 16;
  ScopedSpace fspace(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(fspace.valid());
  ScopedDataset dset(H5Dcreate2(file, "vals", H5T_STD_I32LE, fspace,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  // Non-zero memory slice offset exercises contig_piece_info->buf_off > 0.
  constexpr hsize_t kCount = 8;
  constexpr hsize_t kMemOffset = 4;
  constexpr hsize_t kFileStart = 2;
  ScopedSpace mspace_slice(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(mspace_slice.valid());
  ASSERT_GE(H5Sselect_hyperslab(mspace_slice, H5S_SELECT_SET, &kMemOffset,
                                nullptr, &kCount, nullptr),
            0);
  ScopedSpace fspace_slice(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(fspace_slice.valid());
  ASSERT_GE(H5Sselect_hyperslab(fspace_slice, H5S_SELECT_SET, &kFileStart,
                                nullptr, &kCount, nullptr),
            0);

  for (H5D_selection_io_mode_t mode :
       {H5D_SELECTION_IO_MODE_OFF, H5D_SELECTION_IO_MODE_ON}) {
    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl.valid());
    ASSERT_GE(H5Pset_selection_io(dxpl, mode), 0);
    ASSERT_GE(H5Pset_modify_write_buf(dxpl, true), 0);
    ASSERT_GE(H5Pset_data_transform(dxpl, "x + 5"), 0);

    std::vector<int64_t> wbuf(kDim, -1);
    for (size_t i = 0; i < kCount; ++i) {
      wbuf[kMemOffset + i] = static_cast<int64_t>(10 + i);
    }
    ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT64, mspace_slice, fspace_slice, dxpl,
                       wbuf.data()),
              0);
    if (mode == H5D_SELECTION_IO_MODE_ON) {
      uint32_t cause = 0;
      EXPECT_GE(H5Pget_no_selection_io_cause(dxpl, &cause), 0);
      EXPECT_EQ(cause & ~H5D_SEL_IO_NO_VECTOR_OR_SELECTION_IO_CB, 0u);
    }

    // Clear transform for readback, then also test transform during read.
    ScopedPlist read_xform(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(read_xform.valid());
    ASSERT_GE(H5Pset_selection_io(read_xform, mode), 0);
    ASSERT_GE(H5Pset_data_transform(read_xform, "x * 2"), 0);

    std::vector<int64_t> rbuf(kDim, -999);
    ASSERT_GE(H5Dread(dset, H5T_NATIVE_INT64, mspace_slice, fspace_slice,
                      read_xform, rbuf.data()),
              0);
    if (mode == H5D_SELECTION_IO_MODE_ON) {
      uint32_t cause = 0;
      EXPECT_GE(H5Pget_no_selection_io_cause(read_xform, &cause), 0);
      EXPECT_EQ(cause & ~H5D_SEL_IO_NO_VECTOR_OR_SELECTION_IO_CB, 0u);
    }
    EXPECT_EQ(rbuf[0], -999);
    EXPECT_EQ(rbuf[kMemOffset], (10 + 5) * 2);
    EXPECT_EQ(rbuf[kMemOffset + 7], (17 + 5) * 2);
  }

  // Test invalid data transform evaluation on H5T_COMPOUND (both modes).
  ScopedType cmpd_type = MakeFullCompoundType();
  ASSERT_TRUE(cmpd_type.valid());
  constexpr hsize_t kCmpdDim = 4;
  ScopedSpace cmpd_space(H5Screate_simple(1, &kCmpdDim, nullptr));
  ASSERT_TRUE(cmpd_space.valid());
  ScopedDataset cmpd_ds(H5Dcreate2(file, "cmpd", cmpd_type, cmpd_space,
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(cmpd_ds.valid());
  std::vector<FullRecord> recs(kCmpdDim, FullRecord{1, 2.5, 3});
  ASSERT_GE(H5Dwrite(cmpd_ds, cmpd_type, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     recs.data()),
            0);

  ScopedErrorSilence silence;
  for (H5D_selection_io_mode_t mode :
       {H5D_SELECTION_IO_MODE_OFF, H5D_SELECTION_IO_MODE_ON}) {
    ScopedPlist bad_xform(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(bad_xform.valid());
    ASSERT_GE(H5Pset_selection_io(bad_xform, mode), 0);
    ASSERT_GE(H5Pset_data_transform(bad_xform, "x + 1"), 0);
    EXPECT_LT(H5Dwrite(cmpd_ds, cmpd_type, H5S_ALL, H5S_ALL, bad_xform,
                       recs.data()),
              0);
    EXPECT_LT(
        H5Dread(cmpd_ds, cmpd_type, H5S_ALL, H5S_ALL, bad_xform, recs.data()),
        0);
  }
}

TEST(H5DscatgathTest, CompoundSubsetOptimizationBothModesAndMultiSequence) {
  ScopedFile file = CreateInMemoryZeroSieveFile("sg_cmpd_opt.h5");
  ASSERT_TRUE(file.valid());

  ScopedType full_t = MakeFullCompoundType();
  ScopedType sub_t = MakeSubCompoundType();
  ASSERT_TRUE(full_t.valid());
  ASSERT_TRUE(sub_t.valid());

  // 1. Dataset with FullRecord; memory with SubRecord (H5T_SUBSET_DST on read,
  //    H5T_SUBSET_SRC on write -> exercises H5D__compound_opt_read + bkg YES).
  constexpr hsize_t kTotal = 3000;
  constexpr hsize_t kHalf = 1500;
  constexpr hsize_t kStart0 = 0;
  constexpr hsize_t kStride2 = 2;
  constexpr hsize_t kBlock1 = 1;

  ScopedSpace full_space(H5Screate_simple(1, &kTotal, nullptr));
  ASSERT_TRUE(full_space.valid());
  ScopedDataset full_ds(H5Dcreate2(file, "full_cmpd", full_t, full_space,
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(full_ds.valid());

  std::vector<FullRecord> init_full(kTotal);
  for (size_t i = 0; i < kTotal; ++i) {
    init_full[i] = FullRecord{static_cast<int32_t>(i),
                              static_cast<double>(i) * 0.5,
                              static_cast<int32_t>(i + 10000)};
  }
  ASSERT_GE(H5Dwrite(full_ds, full_t, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     init_full.data()),
            0);

  ScopedSpace strided_mem(H5Screate_simple(1, &kTotal, nullptr));
  ASSERT_TRUE(strided_mem.valid());
  ASSERT_GE(H5Sselect_hyperslab(strided_mem, H5S_SELECT_SET, &kStart0,
                                &kStride2, &kHalf, &kBlock1),
            0);
  ScopedSpace prefix_file(H5Screate_simple(1, &kTotal, nullptr));
  ASSERT_TRUE(prefix_file.valid());
  ASSERT_GE(H5Sselect_hyperslab(prefix_file, H5S_SELECT_SET, &kStart0, nullptr,
                                &kHalf, nullptr),
            0);
  ScopedSpace contig_sub(H5Screate_simple(1, &kHalf, nullptr));
  ASSERT_TRUE(contig_sub.valid());

  for (H5D_selection_io_mode_t mode :
       {H5D_SELECTION_IO_MODE_OFF, H5D_SELECTION_IO_MODE_ON}) {
    ASSERT_GE(H5Dwrite(full_ds, full_t, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       init_full.data()),
              0);

    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl.valid());
    ASSERT_GE(H5Pset_selection_io(dxpl, mode), 0);
    ASSERT_GE(H5Pset_hyper_vector_size(dxpl, 2048), 0);

    // Contiguous subset read (hits in_place_tconv=false downgrade in OFF).
    std::vector<SubRecord> contig_r(kHalf, SubRecord{-1, -1.0});
    ASSERT_GE(H5Dread(full_ds, sub_t, contig_sub, prefix_file, dxpl,
                      contig_r.data()),
              0);
    EXPECT_EQ(contig_r[0].a, 0);
    EXPECT_DOUBLE_EQ(contig_r[10].b, 5.0);

    // Strided >1024 sequence subset read (hits H5D__compound_opt_read loop).
    std::vector<SubRecord> strided_r(kTotal, SubRecord{-1, -1.0});
    ASSERT_GE(H5Dread(full_ds, sub_t, strided_mem, prefix_file, dxpl,
                      strided_r.data()),
              0);
    EXPECT_EQ(strided_r[0].a, 0);
    EXPECT_EQ(strided_r[1].a, -1);
    EXPECT_EQ(strided_r[2998].a, 1499);

    // Subset write (SubRecord mem -> FullRecord file): preserves field `c`
    // via H5T_BKG_YES in H5D__scatgath_write / H5D__scatgath_write_select!
    std::vector<SubRecord> sub_w(kHalf);
    for (size_t i = 0; i < kHalf; ++i) {
      sub_w[i] = SubRecord{static_cast<int32_t>(5000 + i),
                           static_cast<double>(i) * 2.0};
    }
    ASSERT_GE(H5Dwrite(full_ds, sub_t, contig_sub, prefix_file, dxpl,
                       sub_w.data()),
              0);
  }

  std::vector<FullRecord> check_full(kTotal);
  ASSERT_GE(H5Dread(full_ds, full_t, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    check_full.data()),
            0);
  EXPECT_EQ(check_full[7].a, 5007);
  EXPECT_DOUBLE_EQ(check_full[7].b, 14.0);
  EXPECT_EQ(check_full[7].c, 10007);

  // 2. Dataset with SubRecord; memory with FullRecord (H5T_SUBSET_DST on write
  //    -> exercises H5D__compound_opt_write in both OFF and ON modes).
  constexpr hsize_t kSmall = 8;
  ScopedSpace small_space(H5Screate_simple(1, &kSmall, nullptr));
  ASSERT_TRUE(small_space.valid());
  ScopedDataset sub_ds(H5Dcreate2(file, "sub_cmpd", sub_t, small_space,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(sub_ds.valid());

  std::vector<FullRecord> small_w(kSmall);
  for (size_t i = 0; i < kSmall; ++i) {
    small_w[i] = FullRecord{static_cast<int32_t>(900 + i),
                            static_cast<double>(i) * 1.5, 77};
  }
  for (H5D_selection_io_mode_t mode :
       {H5D_SELECTION_IO_MODE_OFF, H5D_SELECTION_IO_MODE_ON}) {
    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl.valid());
    ASSERT_GE(H5Pset_selection_io(dxpl, mode), 0);
    // Avoid upstream H5Dscatgath.c:1271 nullptr+offset UBSan when in-place
    // write conversion combines with MODE_ON and H5T_BKG_YES (`tconv_buf==NULL`).
    ASSERT_GE(
        H5Pset_modify_write_buf(dxpl, mode == H5D_SELECTION_IO_MODE_OFF), 0);
    // Reinitialize small_w each pass because H5Pset_modify_write_buf allows
    // in-place / compound write mutation.
    std::vector<FullRecord> pass_w = small_w;
    ASSERT_GE(H5Dwrite(sub_ds, full_t, H5S_ALL, H5S_ALL, dxpl, pass_w.data()),
              0);
  }
  std::vector<SubRecord> small_r(kSmall);
  ASSERT_GE(H5Dread(sub_ds, sub_t, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    small_r.data()),
            0);
  EXPECT_EQ(small_r[3].a, 903);
  EXPECT_DOUBLE_EQ(small_r[3].b, 4.5);
}

TEST(H5DscatgathTest, CustomConvertersBkgModesTransformsAndFailures) {
  ScopedFile file = CreateInMemoryZeroSieveFile("sg_custom_conv.h5");
  ASSERT_TRUE(file.valid());

  // 1. Opaque 4B <-> 8B converter requesting H5T_BKG_YES.
  ScopedType opq4(H5Tcreate(H5T_OPAQUE, 4));
  ScopedType opq8(H5Tcreate(H5T_OPAQUE, 8));
  ScopedType opq4_alt(H5Tcreate(H5T_OPAQUE, 4));
  ASSERT_TRUE(opq4.valid());
  ASSERT_TRUE(opq8.valid());
  ASSERT_TRUE(opq4_alt.valid());
  ASSERT_GE(H5Tset_tag(opq4, "sg_opq4"), 0);
  ASSERT_GE(H5Tset_tag(opq8, "sg_opq8"), 0);
  ASSERT_GE(H5Tset_tag(opq4_alt, "sg_opq4_alt"), 0);

  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "opq4to8_bkg", opq4, opq8,
                        Opaque4To8BkgYesConv),
            0);
  absl::Cleanup unreg_4to8 = [&] {
    H5Tunregister(H5T_PERS_HARD, "opq4to8_bkg", opq4, opq8,
                  Opaque4To8BkgYesConv);
  };
  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "opq8to4_bkg", opq8, opq4,
                        Opaque8To4BkgYesConv),
            0);
  absl::Cleanup unreg_8to4 = [&] {
    H5Tunregister(H5T_PERS_HARD, "opq8to4_bkg", opq8, opq4,
                  Opaque8To4BkgYesConv);
  };
  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "opq4to4_temp", opq4, opq4_alt,
                        Opaque4To4BkgTempConv),
            0);
  absl::Cleanup unreg_temp = [&] {
    H5Tunregister(H5T_PERS_HARD, "opq4to4_temp", opq4, opq4_alt,
                  Opaque4To4BkgTempConv);
  };
  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "opq4alt_to_4", opq4_alt, opq4,
                        Opaque4To4BkgTempConv),
            0);
  absl::Cleanup unreg_temp_rev = [&] {
    H5Tunregister(H5T_PERS_HARD, "opq4alt_to_4", opq4_alt, opq4,
                  Opaque4To4BkgTempConv);
  };

  constexpr hsize_t kDim = 8;
  ScopedSpace space(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset ds4(H5Dcreate2(file, "opq4_ds", opq4, space, H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ds4.valid());

  // Exercise H5T_BKG_YES + in_place_tconv + small tconv buffer in OFF & ON.
  for (H5D_selection_io_mode_t mode :
       {H5D_SELECTION_IO_MODE_OFF, H5D_SELECTION_IO_MODE_ON}) {
    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl.valid());
    ASSERT_GE(H5Pset_selection_io(dxpl, mode), 0);
    // Avoid upstream H5Dscatgath.c:1271 nullptr+offset UBSan when in-place
    // write conversion combines with MODE_ON and H5T_BKG_YES (`tconv_buf==NULL`).
    ASSERT_GE(
        H5Pset_modify_write_buf(dxpl, mode == H5D_SELECTION_IO_MODE_OFF), 0);
    if (mode == H5D_SELECTION_IO_MODE_OFF) {
      ASSERT_GE(H5Pset_buffer(dxpl, 16, nullptr, nullptr), 0);
    }

    std::vector<uint64_t> w8(kDim);
    for (size_t i = 0; i < kDim; ++i) {
      w8[i] = 0x11110000ULL + i;
    }
    ASSERT_GE(H5Dwrite(ds4, opq8, H5S_ALL, H5S_ALL, dxpl, w8.data()), 0);

    std::vector<uint64_t> r8(kDim);
    for (size_t i = 0; i < kDim; ++i) {
      // High 32 bits come from background buffer inside Opaque4To8BkgYesConv.
      r8[i] = (0xCAFE0000ULL + i) << 32;
    }
    ASSERT_GE(H5Dread(ds4, opq8, H5S_ALL, H5S_ALL, dxpl, r8.data()), 0);
    EXPECT_EQ(r8[3], ((0xCAFE0003ULL << 32) | 0x11110003ULL));

    // Also test non-in-place strided mem space with H5T_BKG_YES in ON/OFF.
    constexpr hsize_t kHalf = 4;
    constexpr hsize_t kStart0 = 0;
    constexpr hsize_t kStride2 = 2;
    constexpr hsize_t kBlock1 = 1;
    ScopedSpace mstr(H5Screate_simple(1, &kDim, nullptr));
    ScopedSpace fsub(H5Screate_simple(1, &kDim, nullptr));
    ASSERT_GE(H5Sselect_hyperslab(mstr, H5S_SELECT_SET, &kStart0, &kStride2,
                                  &kHalf, &kBlock1),
              0);
    ASSERT_GE(H5Sselect_hyperslab(fsub, H5S_SELECT_SET, &kStart0, nullptr,
                                  &kHalf, nullptr),
              0);

    std::vector<uint64_t> r8_str(kDim, 0);
    for (size_t i = 0; i < kHalf; ++i) {
      r8_str[2 * i] = (0xBEEF0000ULL + i) << 32;
    }
    ASSERT_GE(H5Dread(ds4, opq8, mstr, fsub, dxpl, r8_str.data()), 0);
    EXPECT_EQ(r8_str[2], ((0xBEEF0001ULL << 32) | 0x11110001ULL));

    // Exercise H5T_BKG_TEMP under both selection I/O modes. Keep expected
    // separate because H5Pset_modify_write_buf allows in-place XOR on write.
    const std::vector<uint32_t> kExpectedTmp = {1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<uint32_t> tmp_vals = kExpectedTmp;
    ASSERT_GE(H5Dwrite(ds4, opq4_alt, H5S_ALL, H5S_ALL, dxpl, tmp_vals.data()),
              0);
    std::vector<uint32_t> tmp_out(kDim, 0);
    ASSERT_GE(H5Dread(ds4, opq4_alt, H5S_ALL, H5S_ALL, dxpl, tmp_out.data()),
              0);
    EXPECT_EQ(tmp_out, kExpectedTmp);
  }

  // 2. Integer pair with H5T_BKG_YES + valid data transform during selection
  //    write (`must_fill_bkg` data transform lines 1276-1287) and invalid
  //    data transform with H5T_BKG_TEMP (`else` data transform lines 1217-1228)
  //    and H5T_BKG_YES.
  ScopedType i24(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(i24.valid());
  ASSERT_GE(H5Tset_precision(i24, 24), 0);
  ASSERT_GE(
      H5Tregister(H5T_PERS_HARD, "i32_to_i24_bkg", H5T_NATIVE_INT32, i24,
                  Int24AndInt32BkgYesConv),
      0);
  absl::Cleanup unreg_i24 = [&] {
    H5Tunregister(H5T_PERS_HARD, "i32_to_i24_bkg", H5T_NATIVE_INT32, i24,
                  Int24AndInt32BkgYesConv);
  };
  ScopedDataset ds_i24(H5Dcreate2(file, "i24_ds", i24, space, H5P_DEFAULT,
                                  H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ds_i24.valid());
  {
    ScopedPlist dxpl_xform(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl_xform.valid());
    ASSERT_GE(H5Pset_selection_io(dxpl_xform, H5D_SELECTION_IO_MODE_ON), 0);
    ASSERT_GE(H5Pset_data_transform(dxpl_xform, "x + 10"), 0);
    std::vector<int32_t> vals = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_GE(H5Dwrite(ds_i24, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, dxpl_xform,
                       vals.data()),
              0);
    std::vector<int32_t> read_raw(kDim, 0);
    ASSERT_GE(H5Dread(ds_i24, i24, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      read_raw.data()),
              0);
    EXPECT_THAT(read_raw, ElementsAre(11, 12, 13, 14, 15, 16, 17, 18));
  }

  // 3. Failing H5T_convert (H5T_BKG_NO and H5T_BKG_YES) in OFF and ON modes.
  ScopedType opq_fail_a(H5Tcreate(H5T_OPAQUE, 4));
  ScopedType opq_fail_b(H5Tcreate(H5T_OPAQUE, 4));
  ScopedType opq_fail_bkg(H5Tcreate(H5T_OPAQUE, 8));
  ASSERT_TRUE(opq_fail_a.valid());
  ASSERT_TRUE(opq_fail_b.valid());
  ASSERT_TRUE(opq_fail_bkg.valid());
  ASSERT_GE(H5Tset_tag(opq_fail_a, "fail_a"), 0);
  ASSERT_GE(H5Tset_tag(opq_fail_b, "fail_b"), 0);
  ASSERT_GE(H5Tset_tag(opq_fail_bkg, "fail_bkg"), 0);

  ASSERT_GE(
      H5Tregister(H5T_PERS_HARD, "fail_ab", opq_fail_a, opq_fail_b, FailingConv),
      0);
  absl::Cleanup unreg_fa = [&] {
    H5Tunregister(H5T_PERS_HARD, "fail_ab", opq_fail_a, opq_fail_b,
                  FailingConv);
  };
  ASSERT_GE(
      H5Tregister(H5T_PERS_HARD, "fail_ba", opq_fail_b, opq_fail_a, FailingConv),
      0);
  absl::Cleanup unreg_fb = [&] {
    H5Tunregister(H5T_PERS_HARD, "fail_ba", opq_fail_b, opq_fail_a,
                  FailingConv);
  };
  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "fail_abkg", opq_fail_a, opq_fail_bkg,
                        FailingBkgYesConv),
            0);
  absl::Cleanup unreg_fbkg1 = [&] {
    H5Tunregister(H5T_PERS_HARD, "fail_abkg", opq_fail_a, opq_fail_bkg,
                  FailingBkgYesConv);
  };
  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "fail_bkga", opq_fail_bkg, opq_fail_a,
                        FailingBkgYesConv),
            0);
  absl::Cleanup unreg_fbkg2 = [&] {
    H5Tunregister(H5T_PERS_HARD, "fail_bkga", opq_fail_bkg, opq_fail_a,
                  FailingBkgYesConv);
  };

  ScopedDataset ds_fail(H5Dcreate2(file, "fail_ds", opq_fail_a, space,
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ds_fail.valid());

  ScopedErrorSilence silence;
  std::vector<uint64_t> buf64(kDim, 0);
  for (H5D_selection_io_mode_t mode :
       {H5D_SELECTION_IO_MODE_OFF, H5D_SELECTION_IO_MODE_ON}) {
    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl.valid());
    ASSERT_GE(H5Pset_selection_io(dxpl, mode), 0);

    // H5T_convert failure without background buffer.
    EXPECT_LT(
        H5Dwrite(ds_fail, opq_fail_b, H5S_ALL, H5S_ALL, dxpl, buf64.data()), 0);
    EXPECT_LT(
        H5Dread(ds_fail, opq_fail_b, H5S_ALL, H5S_ALL, dxpl, buf64.data()), 0);

    // H5T_convert failure WITH H5T_BKG_YES (covers must_fill_bkg convert err).
    EXPECT_LT(
        H5Dwrite(ds_fail, opq_fail_bkg, H5S_ALL, H5S_ALL, dxpl, buf64.data()),
        0);

    // Non-numeric opaque transform evaluation error (covers xform_eval err
    // both in else branch and must_fill_bkg branch of write_select / read_sel).
    ScopedPlist bad_xform(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(bad_xform.valid());
    ASSERT_GE(H5Pset_selection_io(bad_xform, mode), 0);
    ASSERT_GE(H5Pset_data_transform(bad_xform, "x + 1"), 0);
    EXPECT_LT(H5Dwrite(ds4, opq8, H5S_ALL, H5S_ALL, bad_xform, buf64.data()),
              0);
    EXPECT_LT(
        H5Dwrite(ds4, opq4_alt, H5S_ALL, H5S_ALL, bad_xform, buf64.data()), 0);
    EXPECT_LT(
        H5Dread(ds4, opq4_alt, H5S_ALL, H5S_ALL, bad_xform, buf64.data()), 0);
    if (mode == H5D_SELECTION_IO_MODE_ON) {
      uint32_t cause = 0;
      EXPECT_GE(H5Pget_no_selection_io_cause(bad_xform, &cause), 0);
      EXPECT_EQ(cause & ~H5D_SEL_IO_NO_VECTOR_OR_SELECTION_IO_CB, 0u);
    }
  }
}

TEST(H5DscatgathTest, MultiDatasetMixedNoopTconvAndBkgSelectionIo) {
  ScopedFile file = CreateInMemoryZeroSieveFile("sg_multi.h5");
  ASSERT_TRUE(file.valid());

  constexpr hsize_t kDim = 8;
  ScopedSpace sp0(H5Screate_simple(1, &kDim, nullptr));
  ScopedSpace sp1(H5Screate_simple(1, &kDim, nullptr));
  ScopedSpace sp2(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(sp0.valid() && sp1.valid() && sp2.valid());

  ScopedType full_t = MakeFullCompoundType();
  ScopedType sub_t = MakeSubCompoundType();
  ASSERT_TRUE(full_t.valid() && sub_t.valid());

  // ds0: NOOP piece (int32 -> int32).
  ScopedDataset ds0(H5Dcreate2(file, "ds0_noop", H5T_NATIVE_INT32, sp0,
                               H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  // ds1: Non-bkg type conversion piece (int32 -> int64).
  ScopedDataset ds1(H5Dcreate2(file, "ds1_tconv", H5T_STD_I32LE, sp1,
                               H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  // ds2: Background-buffer type conversion piece (FullRecord <-> SubRecord).
  ScopedDataset ds2(H5Dcreate2(file, "ds2_bkg", full_t, sp2, H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ds0.valid() && ds1.valid() && ds2.valid());

  std::vector<FullRecord> init_recs(kDim);
  for (size_t i = 0; i < kDim; ++i) {
    init_recs[i] = FullRecord{static_cast<int32_t>(i),
                              static_cast<double>(i) * 0.25,
                              static_cast<int32_t>(800 + i)};
  }
  ASSERT_GE(H5Dwrite(ds2, full_t, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     init_recs.data()),
            0);

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ASSERT_GE(H5Pset_selection_io(dxpl, H5D_SELECTION_IO_MODE_ON), 0);

  std::array<hid_t, 3> dsets = {ds0.get(), ds1.get(), ds2.get()};
  std::array<hid_t, 3> mem_types = {H5T_NATIVE_INT32, H5T_NATIVE_INT64,
                                    sub_t.get()};
  std::array<hid_t, 3> spaces = {H5S_ALL, H5S_ALL, H5S_ALL};

  std::array<int32_t, kDim> w0 = {10, 20, 30, 40, 50, 60, 70, 80};
  std::array<int64_t, kDim> w1 = {100, 200, 300, 400, 500, 600, 700, 800};
  std::array<SubRecord, kDim> w2{};
  for (size_t i = 0; i < kDim; ++i) {
    w2[i] = SubRecord{static_cast<int32_t>(300 + i),
                      static_cast<double>(i) * 3.0};
  }
  std::array<const void*, 3> wbufs = {w0.data(), w1.data(), w2.data()};
  ASSERT_GE(H5Dwrite_multi(3, dsets.data(), mem_types.data(), spaces.data(),
                           spaces.data(), dxpl, wbufs.data()),
            0);
  {
    uint32_t cause = 0;
    EXPECT_GE(H5Pget_no_selection_io_cause(dxpl, &cause), 0);
    EXPECT_EQ(cause & ~H5D_SEL_IO_NO_VECTOR_OR_SELECTION_IO_CB, 0u);
  }

  std::array<int32_t, kDim> r0{};
  std::array<int64_t, kDim> r1{};
  std::array<SubRecord, kDim> r2{};
  std::array<void*, 3> rbufs = {r0.data(), r1.data(), r2.data()};
  ASSERT_GE(H5Dread_multi(3, dsets.data(), mem_types.data(), spaces.data(),
                          spaces.data(), dxpl, rbufs.data()),
            0);
  {
    uint32_t cause = 0;
    EXPECT_GE(H5Pget_no_selection_io_cause(dxpl, &cause), 0);
    EXPECT_EQ(cause & ~H5D_SEL_IO_NO_VECTOR_OR_SELECTION_IO_CB, 0u);
  }
  EXPECT_EQ(r0, w0);
  EXPECT_EQ(r1, w1);
  EXPECT_EQ(r2[4].a, 304);
  EXPECT_DOUBLE_EQ(r2[4].b, 12.0);

  // Confirm field `c` was preserved by mixed multi-dset background write.
  std::vector<FullRecord> check2(kDim);
  ASSERT_GE(H5Dread(ds2, full_t, H5S_ALL, H5S_ALL, H5P_DEFAULT, check2.data()),
            0);
  EXPECT_EQ(check2[4].c, 804);
}

TEST(H5DscatgathTest, VfdIoFailuresAndDirectInternalZeroAndErrorPaths) {
  SgFailVfdEnv env;
  g_sg_vfd_env = &env;
  absl::Cleanup reset_env = [] { g_sg_vfd_env = nullptr; };

  const H5FD_class_t cls = MakeSgVfdClass();
  const hid_t drv_id = H5FDregister(&cls);
  ASSERT_NE(drv_id, H5I_INVALID_HID);
  absl::Cleanup unreg_drv = [drv_id] { H5FDunregister(drv_id); };

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_sieve_buf_size(fapl, 0), 0);
  ASSERT_GE(H5Pset_driver(fapl, drv_id, nullptr), 0);

  ScopedFile file(H5Fcreate("sg_err.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(file.valid());

  ScopedType full_t = MakeFullCompoundType();
  ScopedType sub_t = MakeSubCompoundType();
  ASSERT_TRUE(full_t.valid() && sub_t.valid());

  constexpr hsize_t kDim = 8;
  ScopedSpace space(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(space.valid());

  ScopedDataset ds_int(H5Dcreate2(file, "int_ds", H5T_STD_I32LE, space,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedDataset ds_full(H5Dcreate2(file, "full_ds", full_t, space, H5P_DEFAULT,
                                   H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ds_int.valid() && ds_full.valid());

  std::array<int64_t, kDim> ints_w = {1, 2, 3, 4, 5, 6, 7, 8};
  ASSERT_GE(H5Dwrite(ds_int, H5T_NATIVE_INT64, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     ints_w.data()),
            0);
  std::vector<FullRecord> full_w(kDim, FullRecord{1, 2.0, 3});
  ASSERT_GE(H5Dwrite(ds_full, full_t, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     full_w.data()),
            0);

  ScopedErrorSilence silence;

  // 1. VFD failures in both SELECTION_IO_MODE_OFF and SELECTION_IO_MODE_ON,
  //    covering gather_file, scatter_file, scatgath_read_select, bkg read
  //    failure in scatgath_write_select, and final select_write failure.
  for (H5D_selection_io_mode_t mode :
       {H5D_SELECTION_IO_MODE_OFF, H5D_SELECTION_IO_MODE_ON}) {
    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl.valid());
    ASSERT_GE(H5Pset_selection_io(dxpl, mode), 0);

    std::array<int64_t, kDim> ints_r{};
    std::vector<SubRecord> sub_w(kDim, SubRecord{5, 6.0});

    env.fail_read = true;
    EXPECT_LT(H5Dread(ds_int, H5T_NATIVE_INT64, H5S_ALL, H5S_ALL, dxpl,
                      ints_r.data()),
              0);
    // Triggers file gather / selection read to background buffer failure.
    EXPECT_LT(
        H5Dwrite(ds_full, sub_t, H5S_ALL, H5S_ALL, dxpl, sub_w.data()), 0);
    env.fail_read = false;

    env.fail_write = true;
    EXPECT_LT(H5Dwrite(ds_int, H5T_NATIVE_INT64, H5S_ALL, H5S_ALL, dxpl,
                       ints_w.data()),
              0);
    env.fail_write = false;
  }

  // 2. Direct internal calls for zero-element paths (`nelmts == 0`) and
  //    selection-iterator initialization/sequence errors in H5Dscatgath.c.
  auto* ds_ptr = static_cast<H5D_t*>(H5VL_object_verify(ds_int, H5I_DATASET));
  ASSERT_NE(ds_ptr, nullptr);
  auto* raw_space = static_cast<H5S_t*>(H5I_object_verify(space, H5I_DATASPACE));
  ASSERT_NE(raw_space, nullptr);

  H5D_storage_t store{};
  store.contig.dset_addr = ds_ptr->shared->layout.storage.u.contig.addr;
  store.contig.dset_size = ds_ptr->shared->layout.storage.u.contig.size;

  H5D_dset_io_info_t dinfo{};
  dinfo.dset = ds_ptr;
  dinfo.store = &store;
  dinfo.layout_ops = *ds_ptr->shared->layout.ops;
  dinfo.buf.vp = ints_w.data();
  dinfo.nelmts = 0;
  dinfo.file_space = raw_space;
  dinfo.mem_space = raw_space;
  dinfo.type_info.src_type_size = sizeof(int64_t);
  dinfo.type_info.dst_type_size = sizeof(int64_t);

  H5D_io_info_t io_info{};
  io_info.op_type = H5D_IO_OP_READ;
  io_info.f_sh = ds_ptr->oloc.file->shared;
  io_info.count = 1;
  io_info.dsets_info = &dinfo;

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  EXPECT_GE(H5D__scatgath_read(&io_info, &dinfo), 0);
  io_info.op_type = H5D_IO_OP_WRITE;
  EXPECT_GE(H5D__scatgath_write(&io_info, &dinfo), 0);

  // Selection iterator initialization failures on file_iter, mem_iter, and
  // bkg_iter in H5D__scatgath_read and H5D__scatgath_write.
  H5S_t bad_init_space = *raw_space;
  H5S_select_class_t bad_init_cls = *raw_space->select.type;
  bad_init_cls.iter_init = FailSgIterInit;
  bad_init_space.select.type = &bad_init_cls;

  H5S_t second_fail_space = *raw_space;
  H5S_select_class_t second_fail_cls = H5S_sel_all[0];
  second_fail_cls.iter_init = FailSecondSgIterInit;
  second_fail_space.select.type = &second_fail_cls;
  absl::Cleanup reset_second = [] {
    g_fail_on_second_init = false;
    g_init_call_count = 0;
  };

  dinfo.nelmts = 1;
  io_info.op_type = H5D_IO_OP_READ;
  dinfo.file_space = &bad_init_space;
  dinfo.mem_space = raw_space;
  EXPECT_LT(H5D__scatgath_read(&io_info, &dinfo), 0);
  dinfo.file_space = raw_space;
  dinfo.mem_space = &bad_init_space;
  EXPECT_LT(H5D__scatgath_read(&io_info, &dinfo), 0);
  g_fail_on_second_init = true;
  g_init_call_count = 0;
  dinfo.file_space = raw_space;
  dinfo.mem_space = &second_fail_space;
  EXPECT_LT(H5D__scatgath_read(&io_info, &dinfo), 0);

  io_info.op_type = H5D_IO_OP_WRITE;
  g_fail_on_second_init = false;
  dinfo.file_space = &bad_init_space;
  dinfo.mem_space = raw_space;
  EXPECT_LT(H5D__scatgath_write(&io_info, &dinfo), 0);
  dinfo.file_space = raw_space;
  dinfo.mem_space = &bad_init_space;
  EXPECT_LT(H5D__scatgath_write(&io_info, &dinfo), 0);
  g_fail_on_second_init = true;
  g_init_call_count = 0;
  dinfo.file_space = &second_fail_space;
  dinfo.mem_space = raw_space;
  EXPECT_LT(H5D__scatgath_write(&io_info, &dinfo), 0);
  g_fail_on_second_init = false;

  // Sequence iterator GET_SEQ_LIST failures in H5D__gather_mem and
  // H5D__scatter_mem (plus H5D__gather_file via failing file sequence list).
  H5S_t bad_seq_space = *raw_space;
  H5S_select_class_t bad_seq_cls = H5S_sel_all[0];
  bad_seq_cls.iter_init = FailingSeqSgIterInit;
  bad_seq_space.select.type = &bad_seq_cls;

  H5S_sel_iter_t iter{};
  std::array<int64_t, 4> tmp_buf{};
  ASSERT_GE(H5S_select_iter_init(&iter, &bad_seq_space, sizeof(int64_t), 0), 0);
  EXPECT_EQ(H5D__gather_mem(ints_w.data(), &iter, 1, tmp_buf.data()), 0u);
  H5S_SELECT_ITER_RELEASE(&iter);

  ASSERT_GE(H5S_select_iter_init(&iter, &bad_seq_space, sizeof(int64_t), 0), 0);
  // Note: H5D__scatter_mem calls HGOTO_ERROR(..., 0, ...) on sequence error,
  // so verify call returns cleanly without leaking iterators.
  EXPECT_EQ(H5D__scatter_mem(tmp_buf.data(), &iter, 1, ints_w.data()), 0);
  H5S_SELECT_ITER_RELEASE(&iter);
}

TEST(H5DscatgathTest, CompoundSubsetOptimizationDirectScatgathAndSelectIo) {
  ScopedFile file = CreateInMemoryZeroSieveFile("sg_direct_cmpd.h5");
  ASSERT_TRUE(file.valid());

  ScopedType full_t = MakeFullCompoundType();
  ScopedType sub_t = MakeSubCompoundType();
  ASSERT_TRUE(full_t.valid() && sub_t.valid());

  auto* raw_full_t =
      static_cast<const H5T_t*>(H5I_object_verify(full_t, H5I_DATATYPE));
  auto* raw_sub_t =
      static_cast<const H5T_t*>(H5I_object_verify(sub_t, H5I_DATATYPE));
  ASSERT_NE(raw_full_t, nullptr);
  ASSERT_NE(raw_sub_t, nullptr);
  H5T_path_t* full_to_sub_path = H5T_path_find(raw_full_t, raw_sub_t);
  ASSERT_NE(full_to_sub_path, nullptr);

  constexpr hsize_t kTotal = 3000;
  constexpr hsize_t kHalf = 1500;
  constexpr hsize_t kSmall = 8;
  constexpr hsize_t kStart0 = 0;
  constexpr hsize_t kStride2 = 2;
  constexpr hsize_t kBlock1 = 1;

  ScopedSpace full_space(H5Screate_simple(1, &kTotal, nullptr));
  ScopedDataset full_ds(H5Dcreate2(file, "full_ds", full_t, full_space,
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(full_ds.valid());

  std::vector<FullRecord> full_init(kTotal);
  for (size_t i = 0; i < kTotal; ++i) {
    full_init[i] = FullRecord{static_cast<int32_t>(i),
                                static_cast<double>(i) * 0.5,
                                static_cast<int32_t>(10000 + i)};
  }
  ASSERT_GE(H5Dwrite(full_ds, full_t, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     full_init.data()),
            0);

  ScopedSpace prefix_half(H5Screate_simple(1, &kTotal, nullptr));
  ASSERT_GE(H5Sselect_hyperslab(prefix_half, H5S_SELECT_SET, &kStart0, nullptr,
                                &kHalf, nullptr),
            0);
  ScopedSpace strided_mem(H5Screate_simple(1, &kTotal, nullptr));
  ASSERT_GE(H5Sselect_hyperslab(strided_mem, H5S_SELECT_SET, &kStart0,
                                &kStride2, &kHalf, &kBlock1),
            0);
  ScopedSpace prefix_small(H5Screate_simple(1, &kTotal, nullptr));
  ASSERT_GE(H5Sselect_hyperslab(prefix_small, H5S_SELECT_SET, &kStart0, nullptr,
                                &kSmall, nullptr),
            0);
  ScopedSpace small_space(H5Screate_simple(1, &kSmall, nullptr));
  ASSERT_TRUE(prefix_half.valid() && strided_mem.valid() &&
              prefix_small.valid() && small_space.valid());

  auto* full_ptr =
      static_cast<H5D_t*>(H5VL_object_verify(full_ds, H5I_DATASET));
  auto* raw_prefix_half =
      static_cast<H5S_t*>(H5I_object_verify(prefix_half, H5I_DATASPACE));
  auto* raw_strided_mem =
      static_cast<H5S_t*>(H5I_object_verify(strided_mem, H5I_DATASPACE));
  auto* raw_prefix_small =
      static_cast<H5S_t*>(H5I_object_verify(prefix_small, H5I_DATASPACE));
  auto* raw_small_space =
      static_cast<H5S_t*>(H5I_object_verify(small_space, H5I_DATASPACE));
  ASSERT_NE(full_ptr, nullptr);
  ASSERT_NE(raw_prefix_half, nullptr);
  ASSERT_NE(raw_strided_mem, nullptr);
  ASSERT_NE(raw_prefix_small, nullptr);
  ASSERT_NE(raw_small_space, nullptr);

  H5D_storage_t full_store{};
  full_store.contig.dset_addr = full_ptr->shared->layout.storage.u.contig.addr;
  full_store.contig.dset_size = full_ptr->shared->layout.storage.u.contig.size;

  const H5T_subset_info_t subset_dst{H5T_SUBSET_DST, sizeof(SubRecord)};

  // 1. Direct H5D__scatgath_read hitting in_place_tconv downgrade and
  //    H5D__compound_opt_read with hyper_vector_size > 1024 (>1024 sequences).
  {
    ScopedPlist dxpl_vec(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl_vec.valid());
    ASSERT_GE(H5Pset_hyper_vector_size(dxpl_vec, 2048), 0);
    ScopedApiContext ctx(dxpl_vec);
    ASSERT_TRUE(ctx.valid());

    std::vector<uint8_t> tconv_buf(kHalf * sizeof(FullRecord), 0);
    std::vector<SubRecord> strided_out(kTotal, SubRecord{-1, -1.0});
    H5D_piece_info_t piece{};
    piece.in_place_tconv = true;

    H5D_dset_io_info_t dinfo{};
    dinfo.dset = full_ptr;
    dinfo.store = &full_store;
    dinfo.layout_ops = *full_ptr->shared->layout.ops;
    dinfo.buf.vp = strided_out.data();
    dinfo.nelmts = kHalf;
    dinfo.file_space = raw_prefix_half;
    dinfo.mem_space = raw_strided_mem;
    dinfo.layout_io_info.contig_piece_info = &piece;
    dinfo.type_info.mem_type = raw_sub_t;
    dinfo.type_info.dset_type = raw_full_t;
    dinfo.type_info.src_type = raw_full_t;
    dinfo.type_info.dst_type = raw_sub_t;
    dinfo.type_info.tpath = full_to_sub_path;
    dinfo.type_info.src_type_size = sizeof(FullRecord);
    dinfo.type_info.dst_type_size = sizeof(SubRecord);
    dinfo.type_info.is_conv_noop = false;
    dinfo.type_info.is_xform_noop = true;
    dinfo.type_info.cmpd_subset = &subset_dst;
    dinfo.type_info.need_bkg = H5T_BKG_NO;
    dinfo.type_info.request_nelmts = kHalf;

    H5D_io_info_t io_info{};
    io_info.op_type = H5D_IO_OP_READ;
    io_info.f_sh = full_ptr->oloc.file->shared;
    io_info.count = 1;
    io_info.dsets_info = &dinfo;
    io_info.use_select_io = H5D_SELECTION_IO_MODE_OFF;
    io_info.tconv_buf = tconv_buf.data();
    io_info.tconv_buf_size = tconv_buf.size();

    ASSERT_GE(H5D__scatgath_read(&io_info, &dinfo), 0);
    EXPECT_EQ(strided_out[0].a, 0);
    EXPECT_DOUBLE_EQ(strided_out[0].b, 0.0);
    EXPECT_EQ(strided_out[1].a, -1);
    EXPECT_EQ(strided_out[2998].a, 1499);
    EXPECT_DOUBLE_EQ(strided_out[2998].b, 749.5);

    // Sequence generation failure inside H5D__compound_opt_read.
    ScopedErrorSilence silence;
    H5S_t bad_seq_space = *raw_small_space;
    H5S_select_class_t bad_seq_cls = H5S_sel_all[0];
    bad_seq_cls.iter_init = FailingSeqSgIterInit;
    bad_seq_space.select.type = &bad_seq_cls;

    dinfo.nelmts = kSmall;
    dinfo.file_space = raw_prefix_small;
    dinfo.mem_space = &bad_seq_space;
    dinfo.type_info.request_nelmts = kSmall;
    // Upstream H5Dscatgath.c:1433 passes 0 to HGOTO_ERROR on sequence failure.
    EXPECT_GE(H5D__scatgath_read(&io_info, &dinfo), 0);
  }

  // 2. Direct H5D__scatgath_read_select hitting H5D__compound_opt_read and
  //    H5CX_get_vec_size failure.
  {
    std::vector<uint8_t> tconv_buf(kSmall * sizeof(FullRecord), 0);
    std::vector<SubRecord> sel_out(kSmall, SubRecord{-1, -1.0});

    H5D_dset_io_info_t dinfo{};
    dinfo.dset = full_ptr;
    dinfo.store = &full_store;
    dinfo.layout_ops = *full_ptr->shared->layout.ops;
    dinfo.buf.vp = sel_out.data();
    dinfo.nelmts = kSmall;
    dinfo.file_space = raw_prefix_small;
    dinfo.mem_space = raw_small_space;
    dinfo.type_info.mem_type = raw_sub_t;
    dinfo.type_info.dset_type = raw_full_t;
    dinfo.type_info.src_type = raw_full_t;
    dinfo.type_info.dst_type = raw_sub_t;
    dinfo.type_info.tpath = full_to_sub_path;
    dinfo.type_info.src_type_size = sizeof(FullRecord);
    dinfo.type_info.dst_type_size = sizeof(SubRecord);
    dinfo.type_info.is_conv_noop = false;
    dinfo.type_info.is_xform_noop = true;
    dinfo.type_info.cmpd_subset = &subset_dst;
    dinfo.type_info.need_bkg = H5T_BKG_NO;
    dinfo.type_info.request_nelmts = kSmall;

    H5D_piece_info_t sel_piece{};
    sel_piece.piece_points = kSmall;
    sel_piece.in_place_tconv = false;
    sel_piece.dset_info = &dinfo;

    H5D_piece_info_t* sel_pieces[1] = {&sel_piece};
    H5S_t* mem_spaces[1] = {raw_small_space};
    H5S_t* file_spaces[1] = {raw_prefix_small};
    haddr_t addrs[1] = {full_store.contig.dset_addr};
    size_t element_sizes[1] = {sizeof(FullRecord)};
    void* rbufs[1] = {sel_out.data()};

    H5D_io_info_t io_info{};
    io_info.op_type = H5D_IO_OP_READ;
    io_info.f_sh = full_ptr->oloc.file->shared;
    io_info.count = 1;
    io_info.dsets_info = &dinfo;
    io_info.piece_count = 1;
    io_info.pieces_added = 1;
    io_info.sel_pieces = sel_pieces;
    io_info.mem_spaces = mem_spaces;
    io_info.file_spaces = file_spaces;
    io_info.addrs = addrs;
    io_info.element_sizes = element_sizes;
    io_info.rbufs = rbufs;
    io_info.use_select_io = H5D_SELECTION_IO_MODE_ON;
    io_info.tconv_buf = tconv_buf.data();
    io_info.tconv_buf_size = tconv_buf.size();

    {
      ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
      ASSERT_TRUE(dxpl.valid());
      ScopedApiContext ctx(dxpl);
      ASSERT_TRUE(ctx.valid());
      ASSERT_GE(H5D__scatgath_read_select(&io_info), 0);
      EXPECT_EQ(sel_out[5].a, 5);
      EXPECT_DOUBLE_EQ(sel_out[5].b, 2.5);
    }

    // Wrong plist class causes H5CX_get_vec_size failure in compound_opt_read.
    {
      ScopedErrorSilence silence;
      ScopedPlist bad_dxpl(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_TRUE(bad_dxpl.valid());
      ScopedApiContext ctx(bad_dxpl);
      ASSERT_TRUE(ctx.valid());
      ctx.CacheSelectIoProps();
      EXPECT_LT(H5D__scatgath_read_select(&io_info), 0);
    }
  }

  // 3. Direct H5D__scatgath_write and H5D__scatgath_write_select hitting
  //    in_place_tconv downgrade and H5D__compound_opt_write.
  ScopedDataset sub_ds(H5Dcreate2(file, "sub_ds", sub_t, small_space,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(sub_ds.valid());
  std::vector<SubRecord> sub_init(kSmall, SubRecord{0, 0.0});
  ASSERT_GE(
      H5Dwrite(sub_ds, sub_t, H5S_ALL, H5S_ALL, H5P_DEFAULT, sub_init.data()),
      0);
  auto* sub_ptr = static_cast<H5D_t*>(H5VL_object_verify(sub_ds, H5I_DATASET));
  ASSERT_NE(sub_ptr, nullptr);

  H5D_storage_t sub_store{};
  sub_store.contig.dset_addr = sub_ptr->shared->layout.storage.u.contig.addr;
  sub_store.contig.dset_size = sub_ptr->shared->layout.storage.u.contig.size;

  std::vector<FullRecord> full_w(kSmall);
  for (size_t i = 0; i < kSmall; ++i) {
    full_w[i] = FullRecord{static_cast<int32_t>(900 + i),
                           static_cast<double>(i) * 1.5, 77};
  }
  {
    std::vector<uint8_t> tconv_buf(kSmall * sizeof(FullRecord), 0);
    H5D_piece_info_t wpiece{};
    wpiece.in_place_tconv = true;

    H5D_dset_io_info_t dinfo{};
    dinfo.dset = sub_ptr;
    dinfo.store = &sub_store;
    dinfo.layout_ops = *sub_ptr->shared->layout.ops;
    dinfo.buf.cvp = full_w.data();
    dinfo.nelmts = kSmall;
    dinfo.file_space = raw_small_space;
    dinfo.mem_space = raw_small_space;
    dinfo.layout_io_info.contig_piece_info = &wpiece;
    dinfo.type_info.mem_type = raw_full_t;
    dinfo.type_info.dset_type = raw_sub_t;
    dinfo.type_info.src_type = raw_full_t;
    dinfo.type_info.dst_type = raw_sub_t;
    dinfo.type_info.tpath = full_to_sub_path;
    dinfo.type_info.src_type_size = sizeof(FullRecord);
    dinfo.type_info.dst_type_size = sizeof(SubRecord);
    dinfo.type_info.is_conv_noop = false;
    dinfo.type_info.is_xform_noop = true;
    dinfo.type_info.cmpd_subset = &subset_dst;
    dinfo.type_info.need_bkg = H5T_BKG_NO;
    dinfo.type_info.request_nelmts = kSmall;

    H5D_io_info_t io_info{};
    io_info.op_type = H5D_IO_OP_WRITE;
    io_info.f_sh = sub_ptr->oloc.file->shared;
    io_info.count = 1;
    io_info.dsets_info = &dinfo;
    io_info.use_select_io = H5D_SELECTION_IO_MODE_OFF;
    io_info.tconv_buf = tconv_buf.data();
    io_info.tconv_buf_size = tconv_buf.size();

    {
      ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
      ASSERT_TRUE(dxpl.valid());
      ScopedApiContext ctx(dxpl);
      ASSERT_TRUE(ctx.valid());
      ASSERT_GE(H5D__scatgath_write(&io_info, &dinfo), 0);
    }

    std::vector<SubRecord> read_check(kSmall);
    ASSERT_GE(H5Dread(sub_ds, sub_t, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      read_check.data()),
              0);
    EXPECT_EQ(read_check[3].a, 903);
    EXPECT_DOUBLE_EQ(read_check[3].b, 4.5);

    std::vector<FullRecord> full_w2(kSmall);
    for (size_t i = 0; i < kSmall; ++i) {
      full_w2[i] = FullRecord{static_cast<int32_t>(1200 + i),
                              static_cast<double>(i) * 2.5, 99};
    }
    H5D_piece_info_t w_sel_piece{};
    w_sel_piece.piece_points = kSmall;
    w_sel_piece.in_place_tconv = false;
    w_sel_piece.dset_info = &dinfo;

    H5D_piece_info_t* sel_pieces[1] = {&w_sel_piece};
    H5S_t* mem_spaces[1] = {raw_small_space};
    H5S_t* file_spaces[1] = {raw_small_space};
    haddr_t addrs[1] = {sub_store.contig.dset_addr};
    size_t element_sizes[1] = {sizeof(SubRecord)};
    const void* wbufs[1] = {full_w2.data()};

    io_info.piece_count = 1;
    io_info.pieces_added = 1;
    io_info.sel_pieces = sel_pieces;
    io_info.mem_spaces = mem_spaces;
    io_info.file_spaces = file_spaces;
    io_info.addrs = addrs;
    io_info.element_sizes = element_sizes;
    io_info.wbufs = wbufs;
    io_info.use_select_io = H5D_SELECTION_IO_MODE_ON;
    {
      ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
      ASSERT_TRUE(dxpl.valid());
      ScopedApiContext ctx(dxpl);
      ASSERT_TRUE(ctx.valid());
      ASSERT_GE(H5D__scatgath_write_select(&io_info), 0);
    }

    ASSERT_GE(H5Dread(sub_ds, sub_t, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      read_check.data()),
              0);
    EXPECT_EQ(read_check[6].a, 1206);
    EXPECT_DOUBLE_EQ(read_check[6].b, 15.0);
  }
}

TEST(H5DscatgathTest, DirectScatgathAndSelectErrorPaths) {
  ScopedFile file = CreateInMemoryZeroSieveFile("sg_direct_err.h5");
  ASSERT_TRUE(file.valid());

  constexpr hsize_t kDim = 8;
  ScopedSpace space(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset ds_int(H5Dcreate2(file, "int_ds", H5T_STD_I32LE, space,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ds_int.valid());

  std::array<int32_t, kDim> ints_i32 = {1, 2, 3, 4, 5, 6, 7, 8};
  ASSERT_GE(H5Dwrite(ds_int, H5T_STD_I32LE, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     ints_i32.data()),
            0);

  auto* ds_ptr = static_cast<H5D_t*>(H5VL_object_verify(ds_int, H5I_DATASET));
  auto* raw_space = static_cast<H5S_t*>(H5I_object_verify(space, H5I_DATASPACE));
  auto* raw_i32_t =
      static_cast<const H5T_t*>(H5I_object_verify(H5T_STD_I32LE, H5I_DATATYPE));
  auto* raw_i64_t = static_cast<const H5T_t*>(
      H5I_object_verify(H5T_NATIVE_INT64, H5I_DATATYPE));
  ASSERT_NE(ds_ptr, nullptr);
  ASSERT_NE(raw_space, nullptr);
  ASSERT_NE(raw_i32_t, nullptr);
  ASSERT_NE(raw_i64_t, nullptr);

  H5T_path_t* noop_path = H5T_path_find(raw_i32_t, raw_i32_t);
  H5T_path_t* i64_to_i32 = H5T_path_find(raw_i64_t, raw_i32_t);
  H5T_path_t* i32_to_i64 = H5T_path_find(raw_i32_t, raw_i64_t);
  ASSERT_NE(noop_path, nullptr);
  ASSERT_NE(i64_to_i32, nullptr);
  ASSERT_NE(i32_to_i64, nullptr);

  H5D_storage_t store{};
  store.contig.dset_addr = ds_ptr->shared->layout.storage.u.contig.addr;
  store.contig.dset_size = ds_ptr->shared->layout.storage.u.contig.size;

  H5S_t bad_init_space = *raw_space;
  H5S_select_class_t bad_init_cls = *raw_space->select.type;
  bad_init_cls.iter_init = FailSgIterInit;
  bad_init_space.select.type = &bad_init_cls;

  H5S_t bad_seq_space = *raw_space;
  H5S_select_class_t bad_seq_cls = H5S_sel_all[0];
  bad_seq_cls.iter_init = FailingSeqSgIterInit;
  bad_seq_space.select.type = &bad_seq_cls;

  H5S_t fail_rel_space = *raw_space;
  H5S_select_class_t fail_rel_cls = H5S_sel_all[0];
  fail_rel_cls.iter_init = FailingRelSgIterInit;
  fail_rel_space.select.type = &fail_rel_cls;

  ScopedErrorSilence silence;
  std::vector<uint8_t> tconv_buf(kDim * sizeof(int64_t), 0);
  std::vector<uint8_t> bkg_buf(kDim * sizeof(int64_t), 0);
  std::array<int64_t, kDim> ints_i64 = {10, 20, 30, 40, 50, 60, 70, 80};

  // 1. Sequence-list, iterator-init, and iterator-release errors with a valid
  //    DXPL.
  {
    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl.valid());
    ScopedApiContext ctx(dxpl);
    ASSERT_TRUE(ctx.valid());

    H5D_piece_info_t piece{};
    H5D_dset_io_info_t dinfo{};
    dinfo.dset = ds_ptr;
    dinfo.store = &store;
    dinfo.layout_ops = *ds_ptr->shared->layout.ops;
    dinfo.buf.vp = ints_i64.data();
    dinfo.nelmts = kDim;
    dinfo.layout_io_info.contig_piece_info = &piece;
    dinfo.type_info.src_type = raw_i32_t;
    dinfo.type_info.dst_type = raw_i64_t;
    dinfo.type_info.mem_type = raw_i64_t;
    dinfo.type_info.dset_type = raw_i32_t;
    dinfo.type_info.tpath = i32_to_i64;
    dinfo.type_info.src_type_size = sizeof(int32_t);
    dinfo.type_info.dst_type_size = sizeof(int64_t);
    dinfo.type_info.is_conv_noop = false;
    dinfo.type_info.is_xform_noop = true;
    dinfo.type_info.request_nelmts = kDim;

    H5D_io_info_t io_info{};
    io_info.f_sh = ds_ptr->oloc.file->shared;
    io_info.count = 1;
    io_info.dsets_info = &dinfo;
    io_info.tconv_buf = tconv_buf.data();
    io_info.tconv_buf_size = tconv_buf.size();
    io_info.bkg_buf = bkg_buf.data();
    io_info.bkg_buf_size = bkg_buf.size();

    // H5D__scatgath_read sequence / scatter / release failures.
    io_info.op_type = H5D_IO_OP_READ;
    piece.in_place_tconv = true;
    dinfo.file_space = &bad_seq_space;
    dinfo.mem_space = raw_space;
    dinfo.type_info.need_bkg = H5T_BKG_NO;
    EXPECT_LT(H5D__scatgath_read(&io_info, &dinfo), 0);

    piece.in_place_tconv = false;
    dinfo.file_space = raw_space;
    dinfo.mem_space = &bad_seq_space;
    dinfo.type_info.need_bkg = H5T_BKG_YES;
    EXPECT_LT(H5D__scatgath_read(&io_info, &dinfo), 0);

    dinfo.type_info.need_bkg = H5T_BKG_NO;
    // Note: H5D__scatter_mem returns 0 (SUCCEED) on sequence error cleanly.
    EXPECT_GE(H5D__scatgath_read(&io_info, &dinfo), 0);

    piece.in_place_tconv = true;
    dinfo.file_space = &fail_rel_space;
    dinfo.mem_space = raw_space;
    EXPECT_LT(H5D__scatgath_read(&io_info, &dinfo), 0);

    dinfo.file_space = raw_space;
    dinfo.mem_space = &fail_rel_space;
    EXPECT_LT(H5D__scatgath_read(&io_info, &dinfo), 0);

    // H5D__scatgath_write sequence / mem-gather / release failures.
    io_info.op_type = H5D_IO_OP_WRITE;
    dinfo.buf.cvp = ints_i64.data();
    dinfo.type_info.src_type = raw_i64_t;
    dinfo.type_info.dst_type = raw_i32_t;
    dinfo.type_info.tpath = i64_to_i32;
    dinfo.type_info.src_type_size = sizeof(int64_t);
    dinfo.type_info.dst_type_size = sizeof(int32_t);
    dinfo.type_info.need_bkg = H5T_BKG_NO;

    piece.in_place_tconv = true;
    dinfo.file_space = &bad_seq_space;
    dinfo.mem_space = raw_space;
    EXPECT_LT(H5D__scatgath_write(&io_info, &dinfo), 0);

    piece.in_place_tconv = false;
    dinfo.file_space = raw_space;
    dinfo.mem_space = &bad_seq_space;
    EXPECT_LT(H5D__scatgath_write(&io_info, &dinfo), 0);

    piece.in_place_tconv = true;
    dinfo.file_space = &fail_rel_space;
    dinfo.mem_space = raw_space;
    EXPECT_LT(H5D__scatgath_write(&io_info, &dinfo), 0);

    dinfo.file_space = raw_space;
    dinfo.mem_space = &fail_rel_space;
    EXPECT_LT(H5D__scatgath_write(&io_info, &dinfo), 0);

    // H5D__scatgath_read_select / write_select iterator errors.
    H5D_piece_info_t sel_piece{};
    sel_piece.piece_points = kDim;
    sel_piece.dset_info = &dinfo;
    H5D_piece_info_t* sel_pieces[1] = {&sel_piece};
    H5S_t* mem_spaces[1] = {&bad_init_space};
    H5S_t* file_spaces[1] = {raw_space};
    haddr_t addrs[1] = {store.contig.dset_addr};
    size_t element_sizes[1] = {sizeof(int32_t)};
    void* rbufs[1] = {ints_i64.data()};
    const void* wbufs[1] = {ints_i64.data()};

    io_info.piece_count = 1;
    io_info.pieces_added = 1;
    io_info.sel_pieces = sel_pieces;
    io_info.mem_spaces = mem_spaces;
    io_info.file_spaces = file_spaces;
    io_info.addrs = addrs;
    io_info.element_sizes = element_sizes;
    io_info.rbufs = rbufs;
    io_info.wbufs = wbufs;
    io_info.use_select_io = H5D_SELECTION_IO_MODE_ON;

    // read_select errors.
    io_info.op_type = H5D_IO_OP_READ;
    dinfo.buf.vp = ints_i64.data();
    dinfo.type_info.src_type = raw_i32_t;
    dinfo.type_info.dst_type = raw_i64_t;
    dinfo.type_info.tpath = i32_to_i64;
    dinfo.type_info.src_type_size = sizeof(int32_t);
    dinfo.type_info.dst_type_size = sizeof(int64_t);

    sel_piece.in_place_tconv = false;
    dinfo.type_info.need_bkg = H5T_BKG_YES;
    mem_spaces[0] = &bad_init_space;
    EXPECT_LT(H5D__scatgath_read_select(&io_info), 0);
    mem_spaces[0] = &bad_seq_space;
    EXPECT_LT(H5D__scatgath_read_select(&io_info), 0);
    mem_spaces[0] = &fail_rel_space;
    EXPECT_LT(H5D__scatgath_read_select(&io_info), 0);

    dinfo.type_info.need_bkg = H5T_BKG_NO;
    mem_spaces[0] = &bad_init_space;
    EXPECT_LT(H5D__scatgath_read_select(&io_info), 0);
    mem_spaces[0] = &bad_seq_space;
    // scatter_mem returns 0 on sequence failure cleanly; next test release err.
    EXPECT_GE(H5D__scatgath_read_select(&io_info), 0);
    mem_spaces[0] = &fail_rel_space;
    EXPECT_LT(H5D__scatgath_read_select(&io_info), 0);

    // write_select errors.
    io_info.op_type = H5D_IO_OP_WRITE;
    dinfo.buf.cvp = ints_i64.data();
    dinfo.type_info.src_type = raw_i64_t;
    dinfo.type_info.dst_type = raw_i32_t;
    dinfo.type_info.tpath = i64_to_i32;
    dinfo.type_info.src_type_size = sizeof(int64_t);
    dinfo.type_info.dst_type_size = sizeof(int32_t);
    dinfo.type_info.need_bkg = H5T_BKG_NO;
    sel_piece.in_place_tconv = false;

    mem_spaces[0] = &bad_init_space;
    EXPECT_LT(H5D__scatgath_write_select(&io_info), 0);
    mem_spaces[0] = &bad_seq_space;
    EXPECT_LT(H5D__scatgath_write_select(&io_info), 0);
    sel_piece.in_place_tconv = true;
    mem_spaces[0] = &fail_rel_space;
    EXPECT_LT(H5D__scatgath_write_select(&io_info), 0);
  }

  // 2. Wrong property-list class on API context triggers H5CX_get_vec_size and
  //    H5CX_get_data_transform failures cleanly.
  ScopedPlist bad_dxpl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(bad_dxpl.valid());

  // 2a. H5CX_get_vec_size failures (vec_size_valid == false).
  {
    ScopedApiContext ctx(bad_dxpl);
    ASSERT_TRUE(ctx.valid());
    ctx.CacheSelectIoProps();

    H5S_sel_iter_t iter{};
    std::array<int32_t, kDim> tmp{};
    ASSERT_GE(H5S_select_iter_init(&iter, raw_space, sizeof(int32_t), 0), 0);
    EXPECT_LT(H5D__scatter_mem(tmp.data(), &iter, 1, ints_i32.data()), 0);
    EXPECT_EQ(H5D__gather_mem(ints_i32.data(), &iter, 1, tmp.data()), 0u);
    EXPECT_GE(H5S_SELECT_ITER_RELEASE(&iter), 0);

    H5D_piece_info_t piece{};
    piece.in_place_tconv = true;
    H5D_dset_io_info_t dinfo{};
    dinfo.dset = ds_ptr;
    dinfo.store = &store;
    dinfo.layout_ops = *ds_ptr->shared->layout.ops;
    dinfo.buf.vp = ints_i32.data();
    dinfo.nelmts = 1;
    dinfo.file_space = raw_space;
    dinfo.mem_space = raw_space;
    dinfo.layout_io_info.contig_piece_info = &piece;
    dinfo.type_info.mem_type = raw_i32_t;
    dinfo.type_info.dset_type = raw_i32_t;
    dinfo.type_info.src_type = raw_i32_t;
    dinfo.type_info.dst_type = raw_i32_t;
    dinfo.type_info.tpath = noop_path;
    dinfo.type_info.src_type_size = sizeof(int32_t);
    dinfo.type_info.dst_type_size = sizeof(int32_t);
    dinfo.type_info.is_conv_noop = true;
    dinfo.type_info.is_xform_noop = true;
    dinfo.type_info.need_bkg = H5T_BKG_NO;
    dinfo.type_info.request_nelmts = 1;

    H5D_io_info_t io_info{};
    io_info.f_sh = ds_ptr->oloc.file->shared;
    io_info.count = 1;
    io_info.dsets_info = &dinfo;

    io_info.op_type = H5D_IO_OP_READ;
    EXPECT_LT(H5D__scatgath_read(&io_info, &dinfo), 0);
    io_info.op_type = H5D_IO_OP_WRITE;
    EXPECT_LT(H5D__scatgath_write(&io_info, &dinfo), 0);
  }

  // 2b. H5CX_get_data_transform failures (vec_size cached, xform not cached).
  {
    ScopedApiContext ctx(bad_dxpl);
    ASSERT_TRUE(ctx.valid());
    ctx.CacheSelectIoProps();
    ctx.CacheVecSize(1024);

    H5D_piece_info_t piece{};
    piece.in_place_tconv = true;
    H5D_dset_io_info_t dinfo{};
    dinfo.dset = ds_ptr;
    dinfo.store = &store;
    dinfo.layout_ops = *ds_ptr->shared->layout.ops;
    dinfo.buf.vp = ints_i32.data();
    dinfo.nelmts = kDim;
    dinfo.file_space = raw_space;
    dinfo.mem_space = raw_space;
    dinfo.layout_io_info.contig_piece_info = &piece;
    dinfo.type_info.mem_type = raw_i32_t;
    dinfo.type_info.dset_type = raw_i32_t;
    dinfo.type_info.src_type = raw_i32_t;
    dinfo.type_info.dst_type = raw_i32_t;
    dinfo.type_info.tpath = noop_path;
    dinfo.type_info.src_type_size = sizeof(int32_t);
    dinfo.type_info.dst_type_size = sizeof(int32_t);
    dinfo.type_info.is_conv_noop = false;
    dinfo.type_info.is_xform_noop = false;
    dinfo.type_info.need_bkg = H5T_BKG_NO;
    dinfo.type_info.request_nelmts = kDim;

    H5D_piece_info_t sel_piece{};
    sel_piece.piece_points = kDim;
    sel_piece.in_place_tconv = true;
    sel_piece.dset_info = &dinfo;
    H5D_piece_info_t* sel_pieces[1] = {&sel_piece};
    H5S_t* mem_spaces[1] = {raw_space};
    H5S_t* file_spaces[1] = {raw_space};
    haddr_t addrs[1] = {store.contig.dset_addr};
    size_t element_sizes[1] = {sizeof(int32_t)};
    void* rbufs[1] = {ints_i32.data()};
    const void* wbufs[1] = {ints_i32.data()};

    H5D_io_info_t io_info{};
    io_info.f_sh = ds_ptr->oloc.file->shared;
    io_info.count = 1;
    io_info.dsets_info = &dinfo;
    io_info.piece_count = 1;
    io_info.pieces_added = 1;
    io_info.sel_pieces = sel_pieces;
    io_info.mem_spaces = mem_spaces;
    io_info.file_spaces = file_spaces;
    io_info.addrs = addrs;
    io_info.element_sizes = element_sizes;
    io_info.rbufs = rbufs;
    io_info.wbufs = wbufs;
    io_info.tconv_buf = tconv_buf.data();
    io_info.tconv_buf_size = tconv_buf.size();
    io_info.bkg_buf = bkg_buf.data();
    io_info.bkg_buf_size = bkg_buf.size();

    io_info.op_type = H5D_IO_OP_READ;
    EXPECT_LT(H5D__scatgath_read(&io_info, &dinfo), 0);
    io_info.op_type = H5D_IO_OP_WRITE;
    EXPECT_LT(H5D__scatgath_write(&io_info, &dinfo), 0);

    io_info.use_select_io = H5D_SELECTION_IO_MODE_ON;
    io_info.op_type = H5D_IO_OP_READ;
    EXPECT_LT(H5D__scatgath_read_select(&io_info), 0);
    io_info.op_type = H5D_IO_OP_WRITE;
    EXPECT_LT(H5D__scatgath_write_select(&io_info), 0);

    // must_fill_bkg data transform retrieval failure in write_select.
    sel_piece.in_place_tconv = false;
    dinfo.type_info.need_bkg = H5T_BKG_YES;
    io_info.must_fill_bkg = true;
    EXPECT_LT(H5D__scatgath_write_select(&io_info), 0);
  }
}

}  // namespace
}  // namespace hdf5
