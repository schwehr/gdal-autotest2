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
// Tests native datatype resolution (`H5Tget_native_type`) and internal native
// integer/complex initialization across integer, floating-point, string,
// bitfield, opaque, reference, compound, enum, array, vlen, complex, and
// unsupported classes (third_party/hdf5/src/H5Tnative.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tnative.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5TNATIVE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#if defined(H5_HAVE__FLOAT16) && defined(__cplusplus)
__extension__ typedef _Float16 H5__Float16;
#endif
extern "C" {
#define H5T_FRIEND
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Tpkg.h"
}  // extern "C"
#ifdef H5TNATIVE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TNATIVE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

TEST(H5TnativeTest, ReinitPopulatesNativeIntegersAndComplexAlignments) {
  ASSERT_GE(H5close(), 0);
  ASSERT_GE(H5open(), 0);

  EXPECT_EQ(H5T_POINTER_ALIGN_g, alignof(void*));
  EXPECT_EQ(H5T_HVL_ALIGN_g, alignof(hvl_t));
  EXPECT_EQ(H5T_HOBJREF_ALIGN_g, alignof(hobj_ref_t));
  EXPECT_EQ(H5T_HDSETREGREF_ALIGN_g, alignof(hdset_reg_ref_t));
  EXPECT_EQ(H5T_REF_ALIGN_g, alignof(H5R_ref_t));

  EXPECT_EQ(H5Tget_size(H5T_NATIVE_INT16), 2U);
  EXPECT_EQ(H5Tget_size(H5T_NATIVE_UINT32), 4U);
  EXPECT_EQ(H5Tget_size(H5T_NATIVE_INT64), 8U);

#ifdef H5_HAVE_COMPLEX_NUMBERS
  EXPECT_EQ(H5Tget_class(H5T_NATIVE_FLOAT_COMPLEX), H5T_COMPLEX);
  EXPECT_EQ(H5Tget_class(H5T_NATIVE_DOUBLE_COMPLEX), H5T_COMPLEX);
  EXPECT_EQ(H5Tget_class(H5T_NATIVE_LDOUBLE_COMPLEX), H5T_COMPLEX);
  EXPECT_EQ(H5T_NATIVE_FLOAT_COMPLEX_ALIGN_g, alignof(H5_float_complex));
  EXPECT_EQ(H5T_NATIVE_DOUBLE_COMPLEX_ALIGN_g, alignof(H5_double_complex));
  EXPECT_EQ(H5T_NATIVE_LDOUBLE_COMPLEX_ALIGN_g, alignof(H5_ldouble_complex));
#endif
}

TEST(H5TnativeTest, IntegerTypesAcrossPrecisionsSignsAndDirections) {
  struct IntCase {
    hid_t base_id;
    size_t custom_size;
    size_t custom_prec;
    H5T_direction_t dir;
    hid_t expected_native;
  };

  const std::vector<IntCase> cases = {
      // Signed across DEFAULT, ASCEND, and DESCEND.
      {H5T_STD_I8BE, 0, 0, H5T_DIR_DEFAULT, H5T_NATIVE_SCHAR},
      {H5T_STD_I8BE, 0, 0, H5T_DIR_ASCEND, H5T_NATIVE_SCHAR},
      {H5T_STD_I8BE, 1, 5, H5T_DIR_DESCEND, H5T_NATIVE_SCHAR},
      {H5T_STD_I16BE, 0, 0, H5T_DIR_ASCEND, H5T_NATIVE_SHORT},
      {H5T_STD_I16BE, 0, 0, H5T_DIR_DESCEND, H5T_NATIVE_SHORT},
      {H5T_STD_I32BE, 0, 0, H5T_DIR_ASCEND, H5T_NATIVE_INT},
      {H5T_STD_I32BE, 0, 0, H5T_DIR_DESCEND, H5T_NATIVE_INT},
      {H5T_STD_I64BE, 0, 0, H5T_DIR_ASCEND, H5T_NATIVE_LONG},
      {H5T_STD_I64BE, 0, 0, H5T_DIR_DESCEND, H5T_NATIVE_LONG},
      {H5T_STD_I64BE, 16, 128, H5T_DIR_ASCEND, H5T_NATIVE_LLONG},
      {H5T_STD_I64BE, 16, 128, H5T_DIR_DESCEND, H5T_NATIVE_LLONG},
      // Unsigned across ASCEND and DESCEND.
      {H5T_STD_U8BE, 0, 0, H5T_DIR_ASCEND, H5T_NATIVE_UCHAR},
      {H5T_STD_U8BE, 1, 5, H5T_DIR_DESCEND, H5T_NATIVE_UCHAR},
      {H5T_STD_U16BE, 0, 0, H5T_DIR_ASCEND, H5T_NATIVE_USHORT},
      {H5T_STD_U16BE, 0, 0, H5T_DIR_DESCEND, H5T_NATIVE_USHORT},
      {H5T_STD_U32BE, 0, 0, H5T_DIR_ASCEND, H5T_NATIVE_UINT},
      {H5T_STD_U32BE, 0, 0, H5T_DIR_DESCEND, H5T_NATIVE_UINT},
      {H5T_STD_U64BE, 0, 0, H5T_DIR_ASCEND, H5T_NATIVE_ULONG},
      {H5T_STD_U64BE, 0, 0, H5T_DIR_DESCEND, H5T_NATIVE_ULONG},
      {H5T_STD_U64BE, 16, 128, H5T_DIR_ASCEND, H5T_NATIVE_ULLONG},
      {H5T_STD_U64BE, 16, 128, H5T_DIR_DESCEND, H5T_NATIVE_ULLONG},
  };

  for (const auto& tc : cases) {
    ScopedType query(H5Tcopy(tc.base_id));
    ASSERT_TRUE(query.valid());
    if (tc.custom_size > 0) {
      ASSERT_GE(H5Tset_size(query.get(), tc.custom_size), 0);
      ASSERT_GE(H5Tset_precision(query.get(), tc.custom_prec), 0);
    }
    ScopedType nat(H5Tget_native_type(query.get(), tc.dir));
    ASSERT_TRUE(nat.valid());
    EXPECT_GT(H5Tequal(nat.get(), tc.expected_native), 0);
  }
}

TEST(H5TnativeTest, FloatTypesAcrossSizesSpecialFormatsAndDirections) {
  ScopedType custom_1b(H5Tcopy(H5T_IEEE_F32LE));
  ASSERT_TRUE(custom_1b.valid());
  ASSERT_GE(H5Tset_fields(custom_1b.get(), 7, 4, 3, 0, 4), 0);
  ASSERT_GE(H5Tset_precision(custom_1b.get(), 8), 0);
  ASSERT_GE(H5Tset_size(custom_1b.get(), 1), 0);

  ScopedType custom_3b(H5Tcopy(H5T_IEEE_F32LE));
  ASSERT_TRUE(custom_3b.valid());
  ASSERT_GE(H5Tset_fields(custom_3b.get(), 23, 15, 8, 0, 15), 0);
  ASSERT_GE(H5Tset_precision(custom_3b.get(), 24), 0);
  ASSERT_GE(H5Tset_size(custom_3b.get(), 3), 0);

  ScopedType custom_6b(H5Tcopy(H5T_IEEE_F64LE));
  ASSERT_TRUE(custom_6b.valid());
  ASSERT_GE(H5Tset_fields(custom_6b.get(), 47, 36, 11, 0, 36), 0);
  ASSERT_GE(H5Tset_precision(custom_6b.get(), 48), 0);
  ASSERT_GE(H5Tset_size(custom_6b.get(), 6), 0);

  ScopedType custom_24b(H5Tcopy(H5T_NATIVE_LDOUBLE));
  ASSERT_TRUE(custom_24b.valid());
  ASSERT_GE(H5Tset_size(custom_24b.get(), 24), 0);

  struct FloatCase {
    hid_t src_id;
    H5T_direction_t dir;
    hid_t expected_native;
  };

  const std::vector<FloatCase> cases = {
      // 1-byte ML floats -> _Float16; non-carveout 1-byte -> float.
      {H5T_FLOAT_F8E4M3, H5T_DIR_DEFAULT, H5T_NATIVE_FLOAT16},
      {H5T_FLOAT_F8E5M2, H5T_DIR_ASCEND, H5T_NATIVE_FLOAT16},
      {H5T_FLOAT_F6E2M3, H5T_DIR_ASCEND, H5T_NATIVE_FLOAT16},
      {H5T_FLOAT_F6E3M2, H5T_DIR_ASCEND, H5T_NATIVE_FLOAT16},
      {H5T_FLOAT_F4E2M1, H5T_DIR_ASCEND, H5T_NATIVE_FLOAT16},
      {H5T_FLOAT_F8E4M3, H5T_DIR_DESCEND, H5T_NATIVE_FLOAT16},
      {H5T_FLOAT_F8E5M2, H5T_DIR_DESCEND, H5T_NATIVE_FLOAT16},
      {H5T_FLOAT_F6E2M3, H5T_DIR_DESCEND, H5T_NATIVE_FLOAT16},
      {H5T_FLOAT_F6E3M2, H5T_DIR_DESCEND, H5T_NATIVE_FLOAT16},
      {H5T_FLOAT_F4E2M1, H5T_DIR_DESCEND, H5T_NATIVE_FLOAT16},
      {custom_1b.get(), H5T_DIR_ASCEND, H5T_NATIVE_FLOAT},
      {custom_1b.get(), H5T_DIR_DESCEND, H5T_NATIVE_FLOAT},
      // 2-byte IEEE F16 -> _Float16; bfloat16 -> float.
      {H5T_IEEE_F16LE, H5T_DIR_ASCEND, H5T_NATIVE_FLOAT16},
      {H5T_IEEE_F16BE, H5T_DIR_ASCEND, H5T_NATIVE_FLOAT16},
      {H5T_IEEE_F16LE, H5T_DIR_DESCEND, H5T_NATIVE_FLOAT16},
      {H5T_IEEE_F16BE, H5T_DIR_DESCEND, H5T_NATIVE_FLOAT16},
      {H5T_FLOAT_BFLOAT16LE, H5T_DIR_ASCEND, H5T_NATIVE_FLOAT},
      {H5T_FLOAT_BFLOAT16BE, H5T_DIR_ASCEND, H5T_NATIVE_FLOAT},
      {H5T_FLOAT_BFLOAT16LE, H5T_DIR_DESCEND, H5T_NATIVE_FLOAT},
      {H5T_FLOAT_BFLOAT16BE, H5T_DIR_DESCEND, H5T_NATIVE_FLOAT},
      // 3-byte and 4-byte -> float.
      {custom_3b.get(), H5T_DIR_ASCEND, H5T_NATIVE_FLOAT},
      {custom_3b.get(), H5T_DIR_DESCEND, H5T_NATIVE_FLOAT},
      {H5T_IEEE_F32BE, H5T_DIR_ASCEND, H5T_NATIVE_FLOAT},
      {H5T_IEEE_F32BE, H5T_DIR_DESCEND, H5T_NATIVE_FLOAT},
      {H5T_VAX_F32, H5T_DIR_ASCEND, H5T_NATIVE_FLOAT},
      // 6-byte and 8-byte -> double.
      {custom_6b.get(), H5T_DIR_ASCEND, H5T_NATIVE_DOUBLE},
      {custom_6b.get(), H5T_DIR_DESCEND, H5T_NATIVE_DOUBLE},
      {H5T_IEEE_F64BE, H5T_DIR_ASCEND, H5T_NATIVE_DOUBLE},
      {H5T_IEEE_F64BE, H5T_DIR_DESCEND, H5T_NATIVE_DOUBLE},
      {H5T_VAX_F64, H5T_DIR_DESCEND, H5T_NATIVE_DOUBLE},
      // long double and oversized 24-byte -> long double.
      {H5T_NATIVE_LDOUBLE, H5T_DIR_ASCEND, H5T_NATIVE_LDOUBLE},
      {H5T_NATIVE_LDOUBLE, H5T_DIR_DESCEND, H5T_NATIVE_LDOUBLE},
      {custom_24b.get(), H5T_DIR_ASCEND, H5T_NATIVE_LDOUBLE},
      {custom_24b.get(), H5T_DIR_DESCEND, H5T_NATIVE_LDOUBLE},
  };

  for (const auto& tc : cases) {
    ScopedType nat(H5Tget_native_type(tc.src_id, tc.dir));
    ASSERT_TRUE(nat.valid());
    EXPECT_GT(H5Tequal(nat.get(), tc.expected_native), 0);
  }
}

TEST(H5TnativeTest, BitfieldTypesAcrossPrecisionsAndDirections) {
  ScopedType bf_24(H5Tcopy(H5T_STD_B32BE));
  ASSERT_TRUE(bf_24.valid());
  ASSERT_GE(H5Tset_precision(bf_24.get(), 24), 0);

  ScopedType bf_128(H5Tcopy(H5T_STD_B64BE));
  ASSERT_TRUE(bf_128.valid());
  ASSERT_GE(H5Tset_size(bf_128.get(), 16), 0);
  ASSERT_GE(H5Tset_precision(bf_128.get(), 128), 0);

  struct BitfieldCase {
    hid_t src_id;
    H5T_direction_t dir;
    hid_t expected_native;
  };

  const std::vector<BitfieldCase> cases = {
      {H5T_STD_B8BE, H5T_DIR_DEFAULT, H5T_NATIVE_B8},
      {H5T_STD_B8BE, H5T_DIR_ASCEND, H5T_NATIVE_B8},
      {H5T_STD_B16BE, H5T_DIR_ASCEND, H5T_NATIVE_B16},
      {bf_24.get(), H5T_DIR_ASCEND, H5T_NATIVE_B32},
      {H5T_STD_B32BE, H5T_DIR_ASCEND, H5T_NATIVE_B32},
      {H5T_STD_B64BE, H5T_DIR_ASCEND, H5T_NATIVE_B64},
      {bf_128.get(), H5T_DIR_ASCEND, H5T_NATIVE_B64},
      {H5T_STD_B8BE, H5T_DIR_DESCEND, H5T_NATIVE_B8},
      {H5T_STD_B16BE, H5T_DIR_DESCEND, H5T_NATIVE_B16},
      {H5T_STD_B32BE, H5T_DIR_DESCEND, H5T_NATIVE_B32},
      {H5T_STD_B64BE, H5T_DIR_DESCEND, H5T_NATIVE_B64},
      {bf_128.get(), H5T_DIR_DESCEND, H5T_NATIVE_B64},
  };

  for (const auto& tc : cases) {
    ScopedType nat(H5Tget_native_type(tc.src_id, tc.dir));
    ASSERT_TRUE(nat.valid());
    EXPECT_GT(H5Tequal(nat.get(), tc.expected_native), 0);
  }
}

TEST(H5TnativeTest, StringsOpaqueReferencesEnumsArraysVlenAndComplex) {
  ScopedType fixed_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(fixed_str.valid());
  ASSERT_GE(H5Tset_size(fixed_str.get(), 12), 0);
  ScopedType nat_fixed_str(H5Tget_native_type(fixed_str.get(), H5T_DIR_DEFAULT));
  ASSERT_TRUE(nat_fixed_str.valid());
  EXPECT_GT(H5Tequal(nat_fixed_str.get(), fixed_str.get()), 0);

  ScopedType vl_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(vl_str.valid());
  ASSERT_GE(H5Tset_size(vl_str.get(), H5T_VARIABLE), 0);
  ScopedType nat_vl_str(H5Tget_native_type(vl_str.get(), H5T_DIR_DEFAULT));
  ASSERT_TRUE(nat_vl_str.valid());
  EXPECT_GT(H5Tequal(nat_vl_str.get(), vl_str.get()), 0);

  ScopedType opaque(H5Tcreate(H5T_OPAQUE, 7));
  ASSERT_TRUE(opaque.valid());
  ASSERT_GE(H5Tset_tag(opaque.get(), "raw7"), 0);
  ScopedType nat_opaque(H5Tget_native_type(opaque.get(), H5T_DIR_DEFAULT));
  ASSERT_TRUE(nat_opaque.valid());
  EXPECT_GT(H5Tequal(nat_opaque.get(), opaque.get()), 0);

  for (hid_t ref_id : {H5T_STD_REF_OBJ, H5T_STD_REF_DSETREG, H5T_STD_REF}) {
    ScopedType nat_ref(H5Tget_native_type(ref_id, H5T_DIR_DEFAULT));
    ASSERT_TRUE(nat_ref.valid());
    EXPECT_GT(H5Tequal(nat_ref.get(), ref_id), 0);
  }

  // Non-native big-endian enum converts member values to native byte order.
  ScopedType enum_be(H5Tenum_create(H5T_STD_I32BE));
  ASSERT_TRUE(enum_be.valid());
  const std::array<uint8_t, 4> be_one = {0x00, 0x00, 0x00, 0x01};
  const std::array<uint8_t, 4> be_two = {0x00, 0x00, 0x01, 0x00};
  ASSERT_GE(H5Tenum_insert(enum_be.get(), "ONE", be_one.data()), 0);
  ASSERT_GE(H5Tenum_insert(enum_be.get(), "TWO_FIFTY_SIX", be_two.data()), 0);
  ScopedType nat_enum(H5Tget_native_type(enum_be.get(), H5T_DIR_DEFAULT));
  ASSERT_TRUE(nat_enum.valid());
  int32_t decoded = 0;
  ASSERT_GE(H5Tenum_valueof(nat_enum.get(), "TWO_FIFTY_SIX", &decoded), 0);
  EXPECT_EQ(decoded, 256);

  // 2D Array of non-native float.
  constexpr hsize_t kDims[2] = {2, 3};
  ScopedType arr_be(H5Tarray_create2(H5T_IEEE_F64BE, 2, kDims));
  ASSERT_TRUE(arr_be.valid());
  ScopedType nat_arr(H5Tget_native_type(arr_be.get(), H5T_DIR_DEFAULT));
  ASSERT_TRUE(nat_arr.valid());
  EXPECT_EQ(H5Tget_size(nat_arr.get()), 6U * sizeof(double));

  // VL sequence of non-native int.
  ScopedType vlen_be(H5Tvlen_create(H5T_STD_I16BE));
  ASSERT_TRUE(vlen_be.valid());
  ScopedType nat_vlen(H5Tget_native_type(vlen_be.get(), H5T_DIR_DEFAULT));
  ASSERT_TRUE(nat_vlen.valid());
  EXPECT_EQ(H5Tget_size(nat_vlen.get()), sizeof(hvl_t));

  // Complex types across float precisions.
  ScopedType cplx_f32(H5Tcomplex_create(H5T_IEEE_F32BE));
  ASSERT_TRUE(cplx_f32.valid());
  ScopedType nat_cplx_f32(H5Tget_native_type(cplx_f32.get(), H5T_DIR_DEFAULT));
  ASSERT_TRUE(nat_cplx_f32.valid());
  EXPECT_GT(H5Tequal(nat_cplx_f32.get(), H5T_NATIVE_FLOAT_COMPLEX), 0);

  ScopedType cplx_ldbl(H5Tcomplex_create(H5T_NATIVE_LDOUBLE));
  ASSERT_TRUE(cplx_ldbl.valid());
  ScopedType nat_cplx_ldbl(
      H5Tget_native_type(cplx_ldbl.get(), H5T_DIR_DEFAULT));
  ASSERT_TRUE(nat_cplx_ldbl.valid());
  EXPECT_GT(H5Tequal(nat_cplx_ldbl.get(), H5T_NATIVE_LDOUBLE_COMPLEX), 0);
}

TEST(H5TnativeTest, NestedAndNaturallyAlignedCompoundsAndComposites) {
  // Inner packed compound (char + int64) -> native layout adds 7-byte pad
  // before int64.
  ScopedType inner(H5Tcreate(H5T_COMPOUND, 9));
  ASSERT_TRUE(inner.valid());
  ASSERT_GE(H5Tinsert(inner.get(), "c", 0, H5T_STD_I8BE), 0);
  ASSERT_GE(H5Tinsert(inner.get(), "i64", 1, H5T_STD_I64BE), 0);

  ScopedType enum_be(H5Tenum_create(H5T_STD_I32BE));
  ASSERT_TRUE(enum_be.valid());
  const std::array<uint8_t, 4> be_zero = {0, 0, 0, 0};
  ASSERT_GE(H5Tenum_insert(enum_be.get(), "ZERO", be_zero.data()), 0);

  ScopedType fixed_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(fixed_str.valid());
  ASSERT_GE(H5Tset_size(fixed_str.get(), 3), 0);

  ScopedType vl_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(vl_str.valid());
  ASSERT_GE(H5Tset_size(vl_str.get(), H5T_VARIABLE), 0);

  ScopedType opaque(H5Tcreate(H5T_OPAQUE, 5));
  ASSERT_TRUE(opaque.valid());
  ASSERT_GE(H5Tset_tag(opaque.get(), "opq5"), 0);

  constexpr hsize_t kArrDim[1] = {2};
  ScopedType arr_i16(H5Tarray_create2(H5T_STD_I16BE, 1, kArrDim));
  ASSERT_TRUE(arr_i16.valid());

  ScopedType vlen_i32(H5Tvlen_create(H5T_STD_I32BE));
  ASSERT_TRUE(vlen_i32.valid());

  ScopedType cplx_f64(H5Tcomplex_create(H5T_IEEE_F64BE));
  ASSERT_TRUE(cplx_f64.valid());

  const size_t packed_size = 1 + 9 + 2 + 4 + 3 + sizeof(char*) + 5 +
                             sizeof(hobj_ref_t) + sizeof(hdset_reg_ref_t) +
                             sizeof(H5R_ref_t) + 4 + sizeof(hvl_t) + 16 + 1;
  ScopedType outer(H5Tcreate(H5T_COMPOUND, packed_size));
  ASSERT_TRUE(outer.valid());
  size_t off = 0;
  ASSERT_GE(H5Tinsert(outer.get(), "tag", off, H5T_STD_I8BE), 0);
  off += 1;
  ASSERT_GE(H5Tinsert(outer.get(), "inner", off, inner.get()), 0);
  off += 9;
  ASSERT_GE(H5Tinsert(outer.get(), "bf16", off, H5T_STD_B16BE), 0);
  off += 2;
  ASSERT_GE(H5Tinsert(outer.get(), "kind", off, enum_be.get()), 0);
  off += 4;
  ASSERT_GE(H5Tinsert(outer.get(), "fstr", off, fixed_str.get()), 0);
  off += 3;
  ASSERT_GE(H5Tinsert(outer.get(), "vstr", off, vl_str.get()), 0);
  off += sizeof(char*);
  ASSERT_GE(H5Tinsert(outer.get(), "opq", off, opaque.get()), 0);
  off += 5;
  ASSERT_GE(H5Tinsert(outer.get(), "robj", off, H5T_STD_REF_OBJ), 0);
  off += sizeof(hobj_ref_t);
  ASSERT_GE(H5Tinsert(outer.get(), "rreg", off, H5T_STD_REF_DSETREG), 0);
  off += sizeof(hdset_reg_ref_t);
  ASSERT_GE(H5Tinsert(outer.get(), "rnew", off, H5T_STD_REF), 0);
  off += sizeof(H5R_ref_t);
  ASSERT_GE(H5Tinsert(outer.get(), "arr", off, arr_i16.get()), 0);
  off += 4;
  ASSERT_GE(H5Tinsert(outer.get(), "vlen", off, vlen_i32.get()), 0);
  off += sizeof(hvl_t);
  ASSERT_GE(H5Tinsert(outer.get(), "cplx", off, cplx_f64.get()), 0);
  off += 16;
  ASSERT_GE(H5Tinsert(outer.get(), "tail_b", off, H5T_STD_I8BE), 0);

  ScopedType nat_outer(H5Tget_native_type(outer.get(), H5T_DIR_DEFAULT));
  ASSERT_TRUE(nat_outer.valid());
  EXPECT_EQ(H5Tget_nmembers(nat_outer.get()), 14);
  EXPECT_EQ(H5Tget_member_offset(nat_outer.get(), 1), alignof(int64_t));
  EXPECT_EQ(H5Tget_size(nat_outer.get()) % alignof(int64_t), 0U);

  // Byte-only compound (align == 1) and pre-aligned compound (no tail padding).
  ScopedType bytes_only(H5Tcreate(H5T_COMPOUND, 2));
  ASSERT_TRUE(bytes_only.valid());
  ASSERT_GE(H5Tinsert(bytes_only.get(), "b0", 0, H5T_STD_I8BE), 0);
  ASSERT_GE(H5Tinsert(bytes_only.get(), "b1", 1, H5T_STD_U8BE), 0);
  ScopedType nat_bytes(H5Tget_native_type(bytes_only.get(), H5T_DIR_DEFAULT));
  ASSERT_TRUE(nat_bytes.valid());
  EXPECT_EQ(H5Tget_size(nat_bytes.get()), 2U);

  ScopedType two_i32(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(two_i32.valid());
  ASSERT_GE(H5Tinsert(two_i32.get(), "x", 0, H5T_STD_I32BE), 0);
  ASSERT_GE(H5Tinsert(two_i32.get(), "y", 4, H5T_STD_I32BE), 0);
  ScopedType nat_two_i32(H5Tget_native_type(two_i32.get(), H5T_DIR_DEFAULT));
  ASSERT_TRUE(nat_two_i32.valid());
  EXPECT_EQ(H5Tget_size(nat_two_i32.get()), 8U);

  // Array and vlen wrapping compound exercise composite super_align paths.
  ScopedType arr_comp(H5Tarray_create2(two_i32.get(), 1, kArrDim));
  ASSERT_TRUE(arr_comp.valid());
  ScopedType nat_arr_comp(H5Tget_native_type(arr_comp.get(), H5T_DIR_DEFAULT));
  ASSERT_TRUE(nat_arr_comp.valid());
  EXPECT_EQ(H5Tget_size(nat_arr_comp.get()), 16U);
}

TEST(H5TnativeTest, ErrorPathsRejectBadArgsTimeAndEmptyCompound) {
  ScopedType empty_comp(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(empty_comp.valid());

  // Duplicate member name triggers H5T__insert failure inside
  // H5T__get_native_type and exercises full done: cleanup without leaking.
  ScopedType dup_comp(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(dup_comp.valid());
  ASSERT_GE(H5Tinsert(dup_comp.get(), "a", 0, H5T_STD_I32BE), 0);
  ASSERT_GE(H5Tinsert(dup_comp.get(), "b", 4, H5T_STD_I32BE), 0);
  auto* dup_dt = static_cast<H5T_t*>(H5I_object(dup_comp.get()));
  ASSERT_NE(dup_dt, nullptr);
  ASSERT_GE(dup_dt->shared->u.compnd.nmembs, 2U);
  char* second_name = dup_dt->shared->u.compnd.memb[1].name;
  ASSERT_NE(second_name, nullptr);
  ASSERT_EQ(second_name[0], 'b');

  // Transient integer and array copies for internal class/size/sign/ndims
  // validation checks (restored via absl::Cleanup before H5Tclose).
  ScopedType mod_int(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(mod_int.valid());
  auto* mod_int_dt = static_cast<H5T_t*>(H5I_object(mod_int.get()));
  ASSERT_NE(mod_int_dt, nullptr);

  constexpr hsize_t kOneDim[1] = {2};
  ScopedType mod_arr(H5Tarray_create2(H5T_STD_I32LE, 1, kOneDim));
  ASSERT_TRUE(mod_arr.valid());
  auto* mod_arr_dt = static_cast<H5T_t*>(H5I_object(mod_arr.get()));
  ASSERT_NE(mod_arr_dt, nullptr);

  ScopedErrorSilence silence;
  EXPECT_EQ(H5Tget_native_type(H5I_INVALID_HID, H5T_DIR_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Tget_native_type(H5T_STD_I32LE, static_cast<H5T_direction_t>(99)),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Tget_native_type(H5T_UNIX_D32LE, H5T_DIR_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Tget_native_type(empty_comp.get(), H5T_DIR_DEFAULT),
            H5I_INVALID_HID);

  {
    second_name[0] = 'a';
    absl::Cleanup restore_dup = [second_name] { second_name[0] = 'b'; };
    EXPECT_EQ(H5Tget_native_type(dup_comp.get(), H5T_DIR_DEFAULT),
              H5I_INVALID_HID);
  }
  {
    const H5T_class_t orig_cls = mod_int_dt->shared->type;
    absl::Cleanup restore_cls = [mod_int_dt, orig_cls] {
      mod_int_dt->shared->type = orig_cls;
    };
    mod_int_dt->shared->type = H5T_NO_CLASS;
    EXPECT_EQ(H5Tget_native_type(mod_int.get(), H5T_DIR_DEFAULT),
              H5I_INVALID_HID);
    mod_int_dt->shared->type = H5T_NCLASSES;
    EXPECT_EQ(H5Tget_native_type(mod_int.get(), H5T_DIR_DEFAULT),
              H5I_INVALID_HID);
  }
  {
    const size_t orig_size = mod_int_dt->shared->size;
    absl::Cleanup restore_size = [mod_int_dt, orig_size] {
      mod_int_dt->shared->size = orig_size;
    };
    mod_int_dt->shared->size = 0;
    EXPECT_EQ(H5Tget_native_type(mod_int.get(), H5T_DIR_DEFAULT),
              H5I_INVALID_HID);
  }
  {
    const H5T_sign_t orig_sign = mod_int_dt->shared->u.atomic.u.i.sign;
    absl::Cleanup restore_sign = [mod_int_dt, orig_sign] {
      mod_int_dt->shared->u.atomic.u.i.sign = orig_sign;
    };
    mod_int_dt->shared->u.atomic.u.i.sign = H5T_SGN_ERROR;
    EXPECT_EQ(H5Tget_native_type(mod_int.get(), H5T_DIR_DEFAULT),
              H5I_INVALID_HID);
  }
  {
    const unsigned orig_ndims = mod_arr_dt->shared->u.array.ndims;
    absl::Cleanup restore_ndims = [mod_arr_dt, orig_ndims] {
      mod_arr_dt->shared->u.array.ndims = orig_ndims;
    };
    mod_arr_dt->shared->u.array.ndims = 0;
    EXPECT_EQ(H5Tget_native_type(mod_arr.get(), H5T_DIR_DEFAULT),
              H5I_INVALID_HID);
  }
}

}  // namespace
}  // namespace hdf5
