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
// Tests variable-length datatype conversion (H5T__conv_vlen and
// H5T__conv_vlen_nested_free) including VL sequences, VL strings, large
// buffer growth, zero-length/nil sequences, nested VLEN overwriting,
// base reference types, exception callbacks, custom app converters, strided
// compounds, and error unwinding (third_party/hdf5/src/H5Tconv_vlen.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tconv_vlen.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5TCONV_VLEN_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5T_FRIEND
// H5private.h omits H5__Float16 under __cplusplus, while H5Tpkg.h still
// declares unreferenced extern INF globals with that type.
using H5__Float16 = uint16_t;
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Tconv.h"
#include "third_party/hdf5/src/H5Tconv_vlen.h"
#include "third_party/hdf5/src/H5Tpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5TCONV_VLEN_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TCONV_VLEN_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

struct RangeAbortState {
  int exceptions_seen = 0;
  int abort_after = 1;
};

H5T_conv_ret_t AbortOnNthOverflow(H5T_conv_except_t except_type,
                                  hid_t /*src_id*/, hid_t /*dst_id*/,
                                  void* /*src_buf*/, void* dst_buf,
                                  void* user_data) {
  if (except_type == H5T_CONV_EXCEPT_RANGE_HI ||
      except_type == H5T_CONV_EXCEPT_RANGE_LOW) {
    auto* state = static_cast<RangeAbortState*>(user_data);
    ++state->exceptions_seen;
    if (state->exceptions_seen >= state->abort_after) {
      return H5T_CONV_ABORT;
    }
    int8_t clamped = 127;
    std::memcpy(dst_buf, &clamped, sizeof(clamped));
    return H5T_CONV_HANDLED;
  }
  return H5T_CONV_UNHANDLED;
}

TEST(H5TconvVlenTest, VlenStringEmptyNullLargeAndCharsetReject) {
  ScopedType vl_ascii(H5Tcopy(H5T_C_S1));
  ASSERT_GE(vl_ascii, 0);
  ASSERT_GE(H5Tset_size(vl_ascii, H5T_VARIABLE), 0);

  ScopedType vl_utf8(H5Tcopy(H5T_C_S1));
  ASSERT_GE(vl_utf8, 0);
  ASSERT_GE(H5Tset_size(vl_utf8, H5T_VARIABLE), 0);
  ASSERT_GE(H5Tset_cset(vl_utf8, H5T_CSET_UTF8), 0);

  // Mismatched VL string charsets must fail INIT.
  {
    ScopedErrorSilence silence;
    char* s = nullptr;
    EXPECT_LT(H5Tconvert(vl_ascii, vl_utf8, 1, &s, nullptr, H5P_DEFAULT), 0);
    EXPECT_LT(H5Tconvert(vl_utf8, vl_ascii, 1, &s, nullptr, H5P_DEFAULT), 0);
  }

  // Round-trip >= 6 VL strings: starts with empty non-null "" (hits
  // !seq_len && !conv_buf on read), includes nullptr (is_nil), and ends with
  // a >4096-byte string (grows conv_buf past H5T_VLEN_MIN_CONF_BUF_SIZE).
  ScopedFile file = CreateInMemoryFile("vlen_str.h5");
  ASSERT_GE(file, 0);

  std::string long_str(5000, 'z');
  std::array<const char*, 6> write_strs = {
      "", nullptr, "short", "alpha", "beta", long_str.c_str(),
  };

  constexpr hsize_t kCount = 6;
  ScopedSpace space(H5Screate_simple(1, &kCount, nullptr));
  ASSERT_GE(space, 0);
  ScopedDataset dset(H5Dcreate2(file, "strs", vl_ascii, space, H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_GE(dset, 0);
  ASSERT_GE(H5Dwrite(dset, vl_ascii, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     write_strs.data()),
            0);

  std::array<char*, 6> read_strs{};
  ASSERT_GE(
      H5Dread(dset, vl_ascii, H5S_ALL, H5S_ALL, H5P_DEFAULT, read_strs.data()),
      0);
  absl::Cleanup reclaim_read = [&] {
    H5Treclaim(vl_ascii, space, H5P_DEFAULT, read_strs.data());
  };

  ASSERT_NE(read_strs[0], nullptr);
  EXPECT_STREQ(read_strs[0], "");
  EXPECT_EQ(read_strs[1], nullptr);
  ASSERT_NE(read_strs[2], nullptr);
  EXPECT_STREQ(read_strs[2], "short");
  ASSERT_NE(read_strs[5], nullptr);
  EXPECT_EQ(std::strlen(read_strs[5]), 5000u);

  // Also write & read an explicit UTF-8 VL string dataset (both ASCII and UTF8
  // valid branches in INIT).
  ScopedDataset utf_dset(H5Dcreate2(file, "utf8_strs", vl_utf8, space,
                                    H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_GE(utf_dset, 0);
  ASSERT_GE(H5Dwrite(utf_dset, vl_utf8, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     write_strs.data()),
            0);
}

TEST(H5TconvVlenTest, SequenceConversionLargeResizeNilAndRefBase) {
  ScopedFile file = CreateInMemoryFile("vlen_seq.h5");
  ASSERT_GE(file, 0);

  // 1. Sequence of STD_I32BE converted from NATIVE_INT32 + NATIVE_DOUBLE read.
  ScopedType vl_disk_i32(H5Tvlen_create(H5T_STD_I32BE));
  ScopedType vl_mem_i32(H5Tvlen_create(H5T_NATIVE_INT32));
  ScopedType vl_mem_f64(H5Tvlen_create(H5T_NATIVE_DOUBLE));
  ASSERT_GE(vl_disk_i32, 0);
  ASSERT_GE(vl_mem_i32, 0);
  ASSERT_GE(vl_mem_f64, 0);

  // Inconvertible base types fail path lookup in H5T__conv_vlen.
  {
    ScopedType opaque_base(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_GE(opaque_base, 0);
    ScopedType vl_opaque(H5Tvlen_create(opaque_base));
    ASSERT_GE(vl_opaque, 0);
    hvl_t dummy{};
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Tconvert(vl_mem_i32, vl_opaque, 1, &dummy, nullptr, H5P_DEFAULT), 0);
  }

  // Element 0..4 normal, element 5 > 1200 ints (>4800 bytes > 4096), element 6
  // nil.
  std::vector<int32_t> small_vec = {1, 2, 3};
  std::vector<int32_t> big_vec(1300);
  std::iota(big_vec.begin(), big_vec.end(), 10);

  constexpr hsize_t kN = 7;
  std::array<hvl_t, kN> write_vl{};
  for (size_t i = 0; i < 5; ++i) {
    write_vl[i] = {small_vec.size(), small_vec.data()};
  }
  write_vl[5] = {big_vec.size(), big_vec.data()};
  write_vl[6] = {0, nullptr};  // nil

  ScopedSpace space(H5Screate_simple(1, &kN, nullptr));
  ASSERT_GE(space, 0);
  ScopedDataset dset(H5Dcreate2(file, "seq_be", vl_disk_i32, space, H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_GE(dset, 0);
  ASSERT_GE(H5Dwrite(dset, vl_mem_i32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     write_vl.data()),
            0);

  // Read back with int -> double base conversion (covers d_stride > s_stride,
  // safe>=2 and safe<2, conv_buf growth, and nil read).
  std::array<hvl_t, kN> read_f64{};
  ASSERT_GE(H5Dread(dset, vl_mem_f64, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    read_f64.data()),
            0);
  absl::Cleanup reclaim_f64 = [&] {
    H5Treclaim(vl_mem_f64, space, H5P_DEFAULT, read_f64.data());
  };
  EXPECT_EQ(read_f64[0].len, 3u);
  EXPECT_DOUBLE_EQ(static_cast<const double*>(read_f64[0].p)[2], 3.0);
  EXPECT_EQ(read_f64[5].len, 1300u);
  EXPECT_DOUBLE_EQ(static_cast<const double*>(read_f64[5].p)[1299], 1309.0);
  EXPECT_EQ(read_f64[6].len, 0u);
  EXPECT_EQ(read_f64[6].p, nullptr);

  // 2. VL sequence with H5T_REFERENCE base type (hits ts/td ref H5T_set_loc).
  ScopedType vl_ref_type(H5Tvlen_create(H5T_STD_REF));
  ASSERT_GE(vl_ref_type, 0);
  H5R_ref_t obj_ref{};
  ASSERT_GE(H5Rcreate_object(file, "seq_be", H5P_DEFAULT, &obj_ref), 0);
  absl::Cleanup destroy_obj_ref = [&] { H5Rdestroy(&obj_ref); };

  hvl_t w_ref_vl = {1, &obj_ref};
  constexpr hsize_t kOne = 1;
  ScopedSpace one_space(H5Screate_simple(1, &kOne, nullptr));
  ASSERT_GE(one_space, 0);
  ScopedDataset ref_vl_dset(H5Dcreate2(file, "ref_vl", vl_ref_type, one_space,
                                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_GE(ref_vl_dset, 0);
  ASSERT_GE(
      H5Dwrite(ref_vl_dset, vl_ref_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, &w_ref_vl),
      0);

  hvl_t r_ref_vl{};
  ASSERT_GE(H5Dread(ref_vl_dset, vl_ref_type, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    &r_ref_vl),
            0);
  // Note: H5T__vlen_reclaim does not call H5Rdestroy inside VL-of-REF, so
  // destroy each element explicitly before reclaiming the VL buffer.
  absl::Cleanup reclaim_ref_vl = [&] {
    if (r_ref_vl.p != nullptr) {
      auto* refs = static_cast<H5R_ref_t*>(r_ref_vl.p);
      for (size_t i = 0; i < r_ref_vl.len; ++i) {
        H5Rdestroy(&refs[i]);
      }
      std::free(r_ref_vl.p);
      r_ref_vl.p = nullptr;
      r_ref_vl.len = 0;
    }
  };
  ASSERT_EQ(r_ref_vl.len, 1u);
  EXPECT_EQ(H5Rget_type(static_cast<const H5R_ref_t*>(r_ref_vl.p)),
            H5R_OBJECT2);
}

TEST(H5TconvVlenTest, NestedVlenOverwriteExercisesNestedFreeAndTmpBufResize) {
  ScopedFile file = CreateInMemoryFile("vlen_nested.h5");
  ASSERT_GE(file, 0);

  // Build VL-of-compound where compound contains:
  //   - integer (scalar leaf)
  //   - array[2] of VL(int32)
  //   - VL string
  // Overwriting a longer sequence with a shorter one exercises H5T_COMPOUND,
  // H5T_ARRAY, H5T_VLEN, and simple leaf branches in H5T__conv_vlen_nested_free,
  // plus tmp_buf resize when bg_seq_len * base_size > tmp_buf_size.
  ScopedType vl_int(H5Tvlen_create(H5T_NATIVE_INT32));
  ScopedType vl_str(H5Tcopy(H5T_C_S1));
  ASSERT_GE(vl_int, 0);
  ASSERT_GE(vl_str, 0);
  ASSERT_GE(H5Tset_size(vl_str, H5T_VARIABLE), 0);

  constexpr hsize_t kArrDim = 2;
  ScopedType arr_of_vl(H5Tarray_create2(vl_int, 1, &kArrDim));
  ASSERT_GE(arr_of_vl, 0);

  struct InnerElem {
    int32_t tag;
    std::array<hvl_t, 2> seqs;
    const char* name;
  };
  ScopedType cmp(H5Tcreate(H5T_COMPOUND, sizeof(InnerElem)));
  ASSERT_GE(cmp, 0);
  ASSERT_GE(H5Tinsert(cmp, "tag", offsetof(InnerElem, tag), H5T_NATIVE_INT32),
            0);
  ASSERT_GE(H5Tinsert(cmp, "seqs", offsetof(InnerElem, seqs), arr_of_vl), 0);
  ASSERT_GE(H5Tinsert(cmp, "name", offsetof(InnerElem, name), vl_str), 0);

  ScopedType outer_vl(H5Tvlen_create(cmp));
  ASSERT_GE(outer_vl, 0);

  // Use a large initial inner length (e.g. 260) on row 0 so when overwritten by
  // length 1 (initial tmp_buf_size == 4096), bg_seq_len * dst_base_size >
  // tmp_buf_size triggers tmp_buf reallocation!
  constexpr size_t kInitialLen = 260;
  std::vector<int32_t> leaf_nums = {7, 8};
  std::vector<InnerElem> long_inner(kInitialLen);
  for (size_t i = 0; i < kInitialLen; ++i) {
    long_inner[i].tag = static_cast<int32_t>(i);
    long_inner[i].seqs[0] = {leaf_nums.size(), leaf_nums.data()};
    long_inner[i].seqs[1] = {0, nullptr};
    long_inner[i].name = "item";
  }

  std::array<hvl_t, 3> initial_rows = {{
      {long_inner.size(), long_inner.data()},
      {2, long_inner.data()},
      {0, nullptr},
  }};

  constexpr hsize_t kRows = 3;
  ScopedSpace space(H5Screate_simple(1, &kRows, nullptr));
  ASSERT_GE(space, 0);
  ScopedDataset dset(H5Dcreate2(file, "nested", outer_vl, space, H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_GE(dset, 0);
  ASSERT_GE(H5Dwrite(dset, outer_vl, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     initial_rows.data()),
            0);

  // Overwrite row 0 with length 1 (shorter -> frees indices 1..259), row 1 with
  // nil, and row 2 (previously nil / bg_seq_len == 0) with length 2 (hits
  // bg_seq_len < seq_len zero-padding branch).
  std::array<hvl_t, 3> updated_rows = {{
      {1, long_inner.data()},
      {0, nullptr},
      {2, long_inner.data()},
  }};
  ASSERT_GE(H5Dwrite(dset, outer_vl, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     updated_rows.data()),
            0);

  std::array<hvl_t, 3> read_rows{};
  ASSERT_GE(
      H5Dread(dset, outer_vl, H5S_ALL, H5S_ALL, H5P_DEFAULT, read_rows.data()),
      0);
  absl::Cleanup reclaim_nested = [&] {
    H5Treclaim(outer_vl, space, H5P_DEFAULT, read_rows.data());
  };
  EXPECT_EQ(read_rows[0].len, 1u);
  EXPECT_EQ(read_rows[1].len, 0u);
  EXPECT_EQ(read_rows[2].len, 2u);
}

TEST(H5TconvVlenTest, ExceptionCallbackAbortUnwindAndCustomAppConverter) {
  // Use 4-byte file addresses & sizes so disk VLEN size (4 + 8 = 12) is
  // strictly smaller than memory sizeof(hvl_t) (16), forcing backwards
  // conversion (d_stride > s_stride) on read and activating both Pass 1 and
  // Pass 2 unwind branches on abort.
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_GE(fcpl, 0);
  ASSERT_GE(H5Pset_sizes(fcpl, 4, 4), 0);
  ScopedFile file = CreateInMemoryFile("vlen_abort.h5", H5F_ACC_TRUNC, fcpl);
  ASSERT_GE(file, 0);

  ScopedType vl_dbl(H5Tvlen_create(H5T_NATIVE_DOUBLE));
  ScopedType vl_i8(H5Tvlen_create(H5T_NATIVE_INT8));
  ASSERT_GE(vl_dbl, 0);
  ASSERT_GE(vl_i8, 0);

  // With 8 elements and disk(12) -> mem(16):
  // Pass 1 processes safe = 8 - ((8*12 + 15)/16) = 2 elements (indices 6, 7).
  // Pass 2 processes remaining 6 elements in reverse (indices 5..0).
  constexpr hsize_t kN = 8;
  std::array<double, 2> ok_vals = {1.0, 2.0};
  std::array<double, 2> ovf_vals = {500.0, 600.0};

  ScopedSpace space(H5Screate_simple(1, &kN, nullptr));
  ASSERT_GE(space, 0);

  // Case A: fail on Pass 1 element 7 (d_stride > 0, elmtno = 1 < safe = 2).
  {
    std::array<hvl_t, kN> write_rows{};
    for (size_t i = 0; i < 7; ++i) write_rows[i] = {ok_vals.size(), ok_vals.data()};
    write_rows[7] = {ovf_vals.size(), ovf_vals.data()};

    ScopedDataset dset(H5Dcreate2(file, "pass1_fail", vl_dbl, space,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_GE(dset, 0);
    ASSERT_GE(H5Dwrite(dset, vl_dbl, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       write_rows.data()),
              0);
    EXPECT_LT(H5Dget_storage_size(dset), kN * sizeof(hvl_t));

    RangeAbortState state{0, 1};
    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_GE(dxpl, 0);
    ASSERT_GE(H5Pset_type_conv_cb(dxpl, AbortOnNthOverflow, &state), 0);

    std::array<hvl_t, kN> read_rows{};
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Dread(dset, vl_i8, H5S_ALL, H5S_ALL, dxpl, read_rows.data()), 0);
  }

  // Case B: succeed Pass 1 (6, 7), convert 5 in Pass 2, then abort at 4
  // (nelmts < orig_nelmts, d_stride < 0). Also tests HANDLED once then ABORT.
  {
    std::array<hvl_t, kN> write_rows{};
    for (size_t i = 0; i < kN; ++i) write_rows[i] = {ok_vals.size(), ok_vals.data()};
    write_rows[6] = {ovf_vals.size(), ovf_vals.data()};  // handled exception
    write_rows[4] = {ovf_vals.size(), ovf_vals.data()};  // aborts in Pass 2

    ScopedDataset dset(H5Dcreate2(file, "pass2_fail", vl_dbl, space,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_GE(dset, 0);
    ASSERT_GE(H5Dwrite(dset, vl_dbl, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       write_rows.data()),
              0);

    // Elements in write_rows[6] trigger 2 overflows (one for 500, one for 600);
    // abort on the 3rd overflow inside write_rows[4].
    RangeAbortState state{0, 3};
    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_GE(dxpl, 0);
    ASSERT_GE(H5Pset_type_conv_cb(dxpl, AbortOnNthOverflow, &state), 0);

    std::array<hvl_t, kN> read_rows{};
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Dread(dset, vl_i8, H5S_ALL, H5S_ALL, dxpl, read_rows.data()), 0);
  }

  // Case C: Custom app conversion function registered on base opaque types
  // exercises `tpath->conv.is_app`.
  ScopedType opq_src(H5Tcreate(H5T_OPAQUE, sizeof(int32_t)));
  ScopedType opq_dst(H5Tcreate(H5T_OPAQUE, sizeof(int32_t)));
  ASSERT_GE(opq_src, 0);
  ASSERT_GE(opq_dst, 0);
  ASSERT_GE(H5Tset_tag(opq_src, "vlen_app_src"), 0);
  ASSERT_GE(H5Tset_tag(opq_dst, "vlen_app_dst"), 0);

  auto app_conv = +[](hid_t /*src*/, hid_t /*dst*/, H5T_cdata_t* cdata,
                      size_t nelmts, size_t /*buf_stride*/,
                      size_t /*bkg_stride*/, void* buf, void* /*bkg*/,
                      hid_t /*dxpl*/) -> herr_t {
    if (cdata->command == H5T_CONV_INIT) {
      cdata->need_bkg = H5T_BKG_NO;
      return 0;
    }
    if (cdata->command == H5T_CONV_FREE) {
      return 0;
    }
    if (cdata->command == H5T_CONV_CONV) {
      auto* vals = static_cast<int32_t*>(buf);
      for (size_t i = 0; i < nelmts; ++i) {
        vals[i] += 100;
      }
      return 0;
    }
    return -1;
  };
  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "test_vlen_app", opq_src, opq_dst,
                        app_conv),
            0);
  absl::Cleanup unreg = [&] {
    H5Tunregister(H5T_PERS_HARD, "test_vlen_app", opq_src, opq_dst, app_conv);
  };

  ScopedType vl_src(H5Tvlen_create(opq_src));
  ScopedType vl_dst(H5Tvlen_create(opq_dst));
  ASSERT_GE(vl_src, 0);
  ASSERT_GE(vl_dst, 0);

  std::array<int32_t, 2> raw = {5, 6};
  hvl_t item = {raw.size(), raw.data()};
  ASSERT_GE(H5Tconvert(vl_src, vl_dst, 1, &item, nullptr, H5P_DEFAULT), 0);
  absl::Cleanup free_item = [&] { std::free(item.p); };
  ASSERT_EQ(item.len, 2u);
  EXPECT_EQ(static_cast<const int32_t*>(item.p)[0], 105);
  EXPECT_EQ(static_cast<const int32_t*>(item.p)[1], 106);
}

TEST(H5TconvVlenTest, DirectInternalValidationStridesBkgAndForwardUnwind) {
  ScopedType vl_dbl_id(H5Tvlen_create(H5T_NATIVE_DOUBLE));
  ScopedType vl_i8_id(H5Tvlen_create(H5T_NATIVE_INT8));
  ScopedType int_id(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_GE(vl_dbl_id, 0);
  ASSERT_GE(vl_i8_id, 0);
  ASSERT_GE(int_id, 0);

  auto* vl_dbl =
      static_cast<H5T_t*>(H5I_object_verify(vl_dbl_id, H5I_DATATYPE));
  auto* vl_i8 = static_cast<H5T_t*>(H5I_object_verify(vl_i8_id, H5I_DATATYPE));
  auto* int_dt = static_cast<H5T_t*>(H5I_object_verify(int_id, H5I_DATATYPE));

  H5T_cdata_t cdata{};
  H5T_conv_ctx_t ctx{};

  // Validation error paths in INIT, CONV, and unknown command.
  {
    ScopedErrorSilence silence;
    cdata.command = H5T_CONV_INIT;
    EXPECT_LT(
        H5T__conv_vlen(nullptr, vl_i8, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
        0);
    EXPECT_LT(
        H5T__conv_vlen(vl_dbl, nullptr, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
        0);
    EXPECT_LT(
        H5T__conv_vlen(int_dt, vl_i8, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
        0);
    EXPECT_LT(
        H5T__conv_vlen(vl_dbl, int_dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
        0);

    cdata.command = H5T_CONV_CONV;
    EXPECT_LT(
        H5T__conv_vlen(nullptr, vl_i8, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
        0);
    EXPECT_LT(
        H5T__conv_vlen(vl_dbl, nullptr, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
        0);
    EXPECT_LT(
        H5T__conv_vlen(vl_dbl, vl_i8, &cdata, nullptr, 0, 0, 0, nullptr, nullptr),
        0);

    cdata.command = static_cast<H5T_cmd_t>(999);
    EXPECT_LT(
        H5T__conv_vlen(vl_dbl, vl_i8, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
        0);
  }

  cdata.command = H5T_CONV_INIT;
  ASSERT_GE(
      H5T__conv_vlen(vl_dbl, vl_i8, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_EQ(cdata.need_bkg, H5T_BKG_NO);
  cdata.command = H5T_CONV_FREE;
  EXPECT_GE(
      H5T__conv_vlen(vl_dbl, vl_i8, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);

  // Push API context so H5CX_get_vlen_alloc_info works in direct internal call.
  H5CX_node_t cx_node{};
  ASSERT_GE(H5CX_push(&cx_node), 0);
  absl::Cleanup pop_cx = [] { H5CX_pop(false); };

  // Direct strided call with explicit bkg & bkg_stride != 0, plus abort in
  // forward mode (convert_forward == true -> exercises lines 530-533).
  std::array<double, 1> d_ok = {42.0};
  std::array<double, 1> d_bad = {500.0};
  std::array<hvl_t, 2> vls = {{{1, d_ok.data()}, {1, d_bad.data()}}};
  std::array<hvl_t, 2> dummy_bkg{};
  RangeAbortState state{0, 1};
  ctx.u.conv.cb_struct.func = AbortOnNthOverflow;
  ctx.u.conv.cb_struct.user_data = &state;
  ctx.u.conv.dxpl_id = H5P_DATASET_XFER_DEFAULT;

  {
    ScopedErrorSilence silence;
    cdata.command = H5T_CONV_CONV;
    EXPECT_LT(H5T__conv_vlen(vl_dbl, vl_i8, &cdata, &ctx, 2, sizeof(hvl_t),
                             sizeof(hvl_t), vls.data(), dummy_bkg.data()),
              0);
  }
}

TEST(H5TconvVlenTest, VlenClassCallbackFailuresAndNestedHooks) {
  ScopedFile file = CreateInMemoryFile("vlen_hooks.h5");
  ASSERT_GE(file, 0);
  H5VL_object_t* file_vol = H5VL_vol_object(file);
  ASSERT_NE(file_vol, nullptr);

  H5CX_node_t cx_node{};
  ASSERT_GE(H5CX_push(&cx_node), 0);
  absl::Cleanup pop_cx = [] { H5CX_pop(false); };

  ScopedType vl_src_id(H5Tvlen_create(H5T_NATIVE_DOUBLE));
  ScopedType vl_dst_i8_id(H5Tvlen_create(H5T_NATIVE_INT8));
  ScopedType vl_dst_dbl_id(H5Tvlen_create(H5T_NATIVE_DOUBLE));
  ASSERT_GE(vl_src_id, 0);
  ASSERT_GE(vl_dst_i8_id, 0);
  ASSERT_GE(vl_dst_dbl_id, 0);

  auto* src_dt =
      static_cast<H5T_t*>(H5I_object_verify(vl_src_id, H5I_DATATYPE));
  auto* dst_i8_dt =
      static_cast<H5T_t*>(H5I_object_verify(vl_dst_i8_id, H5I_DATATYPE));
  auto* dst_dbl_dt =
      static_cast<H5T_t*>(H5I_object_verify(vl_dst_dbl_id, H5I_DATATYPE));
  ASSERT_NE(src_dt, nullptr);
  ASSERT_NE(dst_i8_dt, nullptr);
  ASSERT_NE(dst_dbl_dt, nullptr);

  const H5T_vlen_class_t* orig_src_cls = src_dt->shared->u.vlen.cls;
  const H5T_vlen_class_t* orig_dst_i8_cls = dst_i8_dt->shared->u.vlen.cls;
  H5T_vlen_class_t hook_src_cls = *orig_src_cls;
  H5T_vlen_class_t hook_dst_i8_cls = *orig_dst_i8_cls;

  absl::Cleanup restore_simple_cls = [&] {
    src_dt->shared->u.vlen.cls = orig_src_cls;
    dst_i8_dt->shared->u.vlen.cls = orig_dst_i8_cls;
  };

  H5T_cdata_t cdata{};
  cdata.command = H5T_CONV_CONV;
  H5T_conv_ctx_t ctx{};
  ctx.u.conv.dxpl_id = H5P_DATASET_XFER_DEFAULT;

  double elem_val = 3.0;
  hvl_t non_nil = {1, &elem_val};
  hvl_t nil_vl = {0, nullptr};

  // Invalid DXPL in nested H5CX fails H5CX_get_vlen_alloc_info (line 304).
  {
    ScopedErrorSilence silence;
    H5CX_node_t bad_cx{};
    ASSERT_GE(H5CX_push(&bad_cx), 0);
    absl::Cleanup pop_bad = [] { H5CX_pop(false); };
    bad_cx.ctx.dxpl_id = H5I_INVALID_HID;
    hvl_t tmp = non_nil;
    EXPECT_LT(H5T__conv_vlen(src_dt, dst_i8_dt, &cdata, &ctx, 1, 0, 0, &tmp,
                             nullptr),
              0);
  }

  // Hook src->cls->isnull failure.
  {
    ScopedErrorSilence silence;
    hook_src_cls = *orig_src_cls;
    hook_src_cls.isnull = +[](const H5VL_object_t*, void*, bool*) -> herr_t {
      return -1;
    };
    src_dt->shared->u.vlen.cls = &hook_src_cls;
    hvl_t tmp = non_nil;
    EXPECT_LT(H5T__conv_vlen(src_dt, dst_i8_dt, &cdata, &ctx, 1, 0, 0, &tmp,
                             nullptr),
              0);
    src_dt->shared->u.vlen.cls = orig_src_cls;
  }

  // Hook dst->cls->setnull failure when source element is nil.
  {
    ScopedErrorSilence silence;
    hook_dst_i8_cls = *orig_dst_i8_cls;
    hook_dst_i8_cls.setnull = +[](H5VL_object_t*, void*, void*) -> herr_t {
      return -1;
    };
    dst_i8_dt->shared->u.vlen.cls = &hook_dst_i8_cls;
    hvl_t tmp = nil_vl;
    EXPECT_LT(H5T__conv_vlen(src_dt, dst_i8_dt, &cdata, &ctx, 1, 0, 0, &tmp,
                             nullptr),
              0);
    dst_i8_dt->shared->u.vlen.cls = orig_dst_i8_cls;
  }

  // Hook src->cls->getlen failure.
  {
    ScopedErrorSilence silence;
    hook_src_cls = *orig_src_cls;
    hook_src_cls.getlen = +[](H5VL_object_t*, const void*, size_t*) -> herr_t {
      return -1;
    };
    src_dt->shared->u.vlen.cls = &hook_src_cls;
    hvl_t tmp = non_nil;
    EXPECT_LT(H5T__conv_vlen(src_dt, dst_i8_dt, &cdata, &ctx, 1, 0, 0, &tmp,
                             nullptr),
              0);
    src_dt->shared->u.vlen.cls = orig_src_cls;
  }

  // Hook src->cls->getptr == NULL during noop write-to-file conversion.
  {
    ASSERT_GE(H5T_set_loc(dst_dbl_dt, file_vol, H5T_LOC_DISK), 0);
    absl::Cleanup reset_dst_dbl = [&] {
      H5T_set_loc(dst_dbl_dt, nullptr, H5T_LOC_MEMORY);
    };
    ScopedErrorSilence silence;
    hook_src_cls = *orig_src_cls;
    hook_src_cls.getptr = +[](void*) -> void* { return nullptr; };
    src_dt->shared->u.vlen.cls = &hook_src_cls;
    hvl_t tmp = non_nil;
    EXPECT_LT(H5T__conv_vlen(src_dt, dst_dbl_dt, &cdata, &ctx, 1, 0, 0, &tmp,
                             nullptr),
              0);
    src_dt->shared->u.vlen.cls = orig_src_cls;
  }

  // Hook src->cls->read failure.
  {
    ScopedErrorSilence silence;
    hook_src_cls = *orig_src_cls;
    hook_src_cls.read = +[](H5VL_object_t*, void*, void*, size_t) -> herr_t {
      return -1;
    };
    src_dt->shared->u.vlen.cls = &hook_src_cls;
    hvl_t tmp = non_nil;
    EXPECT_LT(H5T__conv_vlen(src_dt, dst_i8_dt, &cdata, &ctx, 1, 0, 0, &tmp,
                             nullptr),
              0);
    src_dt->shared->u.vlen.cls = orig_src_cls;
  }

  // Hook dst->cls->write failure.
  {
    ScopedErrorSilence silence;
    hook_dst_i8_cls = *orig_dst_i8_cls;
    hook_dst_i8_cls.write = +[](H5VL_object_t*, const H5T_vlen_alloc_info_t*,
                                void*, void*, void*, size_t, size_t) -> herr_t {
      return -1;
    };
    dst_i8_dt->shared->u.vlen.cls = &hook_dst_i8_cls;
    hvl_t tmp = non_nil;
    EXPECT_LT(H5T__conv_vlen(src_dt, dst_i8_dt, &cdata, &ctx, 1, 0, 0, &tmp,
                             nullptr),
              0);
    dst_i8_dt->shared->u.vlen.cls = orig_dst_i8_cls;
  }

  // Nested overwrite error hooks: exercises dst getlen fail, dst read fail,
  // and H5T__conv_vlen_nested_free failure through COMPOUND -> ARRAY -> VLEN.
  {
    constexpr hsize_t kOne = 1;
    ScopedType vl_i32_mem(H5Tvlen_create(H5T_NATIVE_INT32));
    ScopedType arr_i32_mem(H5Tarray_create2(vl_i32_mem, 1, &kOne));
    ScopedType cmp_i32_mem(H5Tcreate(H5T_COMPOUND, sizeof(hvl_t)));
    ASSERT_GE(cmp_i32_mem, 0);
    ASSERT_GE(H5Tinsert(cmp_i32_mem, "v", 0, arr_i32_mem), 0);
    ScopedType outer_src_id(H5Tvlen_create(cmp_i32_mem));

    ScopedType vl_i16_disk(H5Tvlen_create(H5T_STD_I16LE));
    ScopedType arr_i16_disk(H5Tarray_create2(vl_i16_disk, 1, &kOne));
    ScopedType cmp_i16_disk(H5Tcreate(H5T_COMPOUND, sizeof(hvl_t)));
    ASSERT_GE(cmp_i16_disk, 0);
    ASSERT_GE(H5Tinsert(cmp_i16_disk, "v", 0, arr_i16_disk), 0);
    ScopedType outer_dst_id(H5Tvlen_create(cmp_i16_disk));

    ASSERT_GE(outer_src_id, 0);
    ASSERT_GE(outer_dst_id, 0);

    auto* outer_src_dt =
        static_cast<H5T_t*>(H5I_object_verify(outer_src_id, H5I_DATATYPE));
    auto* outer_dst_dt =
        static_cast<H5T_t*>(H5I_object_verify(outer_dst_id, H5I_DATATYPE));
    ASSERT_NE(outer_src_dt, nullptr);
    ASSERT_NE(outer_dst_dt, nullptr);

    ASSERT_GE(H5T_set_loc(outer_dst_dt, file_vol, H5T_LOC_DISK), 0);
    absl::Cleanup reset_outer_loc = [&] {
      H5T_set_loc(outer_dst_dt, nullptr, H5T_LOC_MEMORY);
    };

    H5T_t* leaf_vl_dt =
        outer_dst_dt->shared->parent->shared->u.compnd.memb[0].type->shared
            ->parent;
    const H5T_vlen_class_t* orig_outer_cls = outer_dst_dt->shared->u.vlen.cls;
    const H5T_vlen_class_t* orig_leaf_cls = leaf_vl_dt->shared->u.vlen.cls;
    H5T_vlen_class_t hook_outer_cls = *orig_outer_cls;
    H5T_vlen_class_t hook_leaf_cls = *orig_leaf_cls;
    absl::Cleanup restore_nested_cls = [&] {
      outer_dst_dt->shared->u.vlen.cls = orig_outer_cls;
      leaf_vl_dt->shared->u.vlen.cls = orig_leaf_cls;
    };

    hvl_t inner_nil = {0, nullptr};
    hvl_t outer_val = {1, &inner_nil};
    std::array<uint8_t, 16> disk_bkg{};

    ScopedErrorSilence silence;

    // 1) nested dst getlen fails (line 438).
    hook_outer_cls = *orig_outer_cls;
    hook_outer_cls.getlen = +[](H5VL_object_t*, const void*, size_t*)
        -> herr_t { return -1; };
    outer_dst_dt->shared->u.vlen.cls = &hook_outer_cls;
    EXPECT_LT(H5T__conv_vlen(outer_src_dt, outer_dst_dt, &cdata, &ctx, 1, 0, 0,
                             &outer_val, disk_bkg.data()),
              0);

    // 2) nested dst getlen -> 2, dst read fails (line 455).
    hook_outer_cls.getlen =
        +[](H5VL_object_t*, const void*, size_t* seq_len) -> herr_t {
      *seq_len = 2;
      return 0;
    };
    hook_outer_cls.read =
        +[](H5VL_object_t*, void*, void*, size_t) -> herr_t { return -1; };
    EXPECT_LT(H5T__conv_vlen(outer_src_dt, outer_dst_dt, &cdata, &ctx, 1, 0, 0,
                             &outer_val, disk_bkg.data()),
              0);

    // 3) nested dst read succeeds (zeros tmp_buf), write succeeds (no-op),
    //    seq_len (1) < bg_seq_len (2) triggers H5T__conv_vlen_nested_free on
    //    COMPOUND -> ARRAY -> VLEN whose del returns -1 (lines 77, 85, 93, 492).
    hook_outer_cls.read =
        +[](H5VL_object_t*, void*, void* out_buf, size_t len) -> herr_t {
      if (len > 0) {
        std::memset(out_buf, 0, len);
      }
      return 0;
    };
    hook_outer_cls.write =
        +[](H5VL_object_t*, const H5T_vlen_alloc_info_t*, void*, void*, void*,
            size_t, size_t) -> herr_t { return 0; };
    hook_leaf_cls.del = +[](H5VL_object_t*, void*) -> herr_t { return -1; };
    leaf_vl_dt->shared->u.vlen.cls = &hook_leaf_cls;

    EXPECT_LT(H5T__conv_vlen(outer_src_dt, outer_dst_dt, &cdata, &ctx, 1, 0, 0,
                             &outer_val, disk_bkg.data()),
              0);
  }
}

}  // namespace
}  // namespace hdf5
