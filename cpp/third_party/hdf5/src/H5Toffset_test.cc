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
// Tests datatype bit-offset querying and mutation (`H5Tget_offset`,
// `H5Tset_offset`) across atomic, string, derived, immutable, and invalid
// datatypes (third_party/hdf5/src/H5Toffset.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Toffset.c

#include "third_party/hdf5/src/hdf5.h"

#include <cstddef>
#include <cstdint>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

TEST(H5ToffsetTest, PredefinedAtomicTypesHaveZeroOffset) {
  EXPECT_EQ(H5Tget_offset(H5T_NATIVE_INT), 0);
  EXPECT_EQ(H5Tget_offset(H5T_STD_I32BE), 0);
  EXPECT_EQ(H5Tget_offset(H5T_IEEE_F64LE), 0);
  EXPECT_EQ(H5Tget_offset(H5T_STD_B16BE), 0);
  EXPECT_EQ(H5Tget_offset(H5T_C_S1), 0);
  EXPECT_EQ(H5Tget_offset(H5T_FORTRAN_S1), 0);
}

TEST(H5ToffsetTest, SetAndGetOffsetOnAtomicTypesAndGrowSize) {
  // Integer: 16 bits of precision inside a 4-byte container.
  ScopedType int_dt(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(int_dt.valid());
  ASSERT_GE(H5Tset_precision(int_dt.get(), 16), 0);
  ASSERT_GE(H5Tset_offset(int_dt.get(), 8), 0);
  EXPECT_EQ(H5Tget_offset(int_dt.get()), 8);
  EXPECT_EQ(H5Tget_size(int_dt.get()), 4U);

  // Pushing offset + precision past 32 bits expands the total datatype size,
  // while shrinking offset later leaves container size unchanged.
  ASSERT_GE(H5Tset_offset(int_dt.get(), 24), 0);
  EXPECT_EQ(H5Tget_offset(int_dt.get()), 24);
  EXPECT_EQ(H5Tget_size(int_dt.get()), 5U);

  ASSERT_GE(H5Tset_offset(int_dt.get(), 4), 0);
  EXPECT_EQ(H5Tget_offset(int_dt.get()), 4);
  EXPECT_EQ(H5Tget_size(int_dt.get()), 5U);

  // Bitfield: 8 bits of precision inside 2 bytes, then expand to 3 bytes.
  ScopedType bf_dt(H5Tcopy(H5T_STD_B16LE));
  ASSERT_TRUE(bf_dt.valid());
  ASSERT_GE(H5Tset_precision(bf_dt.get(), 8), 0);
  ASSERT_GE(H5Tset_offset(bf_dt.get(), 4), 0);
  EXPECT_EQ(H5Tget_offset(bf_dt.get()), 4);
  EXPECT_EQ(H5Tget_size(bf_dt.get()), 2U);

  ASSERT_GE(H5Tset_offset(bf_dt.get(), 12), 0);
  EXPECT_EQ(H5Tget_offset(bf_dt.get()), 12);
  EXPECT_EQ(H5Tget_size(bf_dt.get()), 3U);

  // String: zero offset succeeds; non-zero offset fails.
  ScopedType str_dt(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(str_dt.valid());
  EXPECT_GE(H5Tset_offset(str_dt.get(), 0), 0);
  EXPECT_EQ(H5Tget_offset(str_dt.get()), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tset_offset(str_dt.get(), 1), 0);
  }
}

TEST(H5ToffsetTest, DerivedTypesPropagateOffsetAndUpdateSizes) {
  // Array of 3 16-bit integers with 8-bit precision: expanding parent to 3
  // bytes updates array size to 3 * 3 = 9 bytes.
  ScopedType base_i16(H5Tcopy(H5T_STD_I16LE));
  ASSERT_TRUE(base_i16.valid());
  ASSERT_GE(H5Tset_precision(base_i16.get(), 8), 0);

  constexpr hsize_t kDims[1] = {3};
  ScopedType arr_dt(H5Tarray_create2(base_i16.get(), 1, kDims));
  ASSERT_TRUE(arr_dt.valid());
  ASSERT_EQ(H5Tget_size(arr_dt.get()), 6U);

  ASSERT_GE(H5Tset_offset(arr_dt.get(), 12), 0);
  EXPECT_EQ(H5Tget_offset(arr_dt.get()), 12);
  EXPECT_EQ(H5Tget_size(arr_dt.get()), 9U);

  // Complex of transient 4-byte float adjusted to 16-bit binary16 fields and
  // precision: expanding parent from 4 to 5 bytes updates complex size to 10.
  ScopedType base_f32(H5Tcopy(H5T_IEEE_F32LE));
  ASSERT_TRUE(base_f32.valid());
  ASSERT_GE(H5Tset_fields(base_f32.get(), 15, 10, 5, 0, 10), 0);
  ASSERT_GE(H5Tset_ebias(base_f32.get(), 15), 0);
  ASSERT_GE(H5Tset_precision(base_f32.get(), 16), 0);
  ScopedType cplx_dt(H5Tcomplex_create(base_f32.get()));
  ASSERT_TRUE(cplx_dt.valid());
  ASSERT_EQ(H5Tget_size(cplx_dt.get()), 8U);

  ASSERT_GE(H5Tset_offset(cplx_dt.get(), 24), 0);
  EXPECT_EQ(H5Tget_offset(cplx_dt.get()), 24);
  EXPECT_EQ(H5Tget_size(cplx_dt.get()), 10U);

  // Vlen preserves sizeof(hvl_t) while propagating offset to base type.
  ScopedType vlen_dt(H5Tvlen_create(base_i16.get()));
  ASSERT_TRUE(vlen_dt.valid());
  ASSERT_GE(H5Tset_offset(vlen_dt.get(), 12), 0);
  EXPECT_EQ(H5Tget_offset(vlen_dt.get()), 12);
  EXPECT_EQ(H5Tget_size(vlen_dt.get()), sizeof(hvl_t));

  // Nested derived type walks multiple parent pointers on H5Tget_offset.
  ScopedType arr_vlen(H5Tarray_create2(vlen_dt.get(), 1, kDims));
  ASSERT_TRUE(arr_vlen.valid());
  EXPECT_EQ(H5Tget_offset(arr_vlen.get()), 12);

  // Empty enum (before members are inserted) propagates offset and updates
  // size to match its parent; once a member is added, H5Tset_offset fails.
  ScopedType enum_dt(H5Tenum_create(base_i16.get()));
  ASSERT_TRUE(enum_dt.valid());
  ASSERT_GE(H5Tset_offset(enum_dt.get(), 10), 0);
  EXPECT_EQ(H5Tget_offset(enum_dt.get()), 10);
  EXPECT_EQ(H5Tget_size(enum_dt.get()), 3U);

  // Insert a 3-byte member value matching the expanded enum size.
  uint8_t enum_val[3] = {1, 0, 0};
  ASSERT_GE(H5Tenum_insert(enum_dt.get(), "ONE", enum_val), 0);
  EXPECT_EQ(H5Tget_offset(enum_dt.get()), 10);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tset_offset(enum_dt.get(), 4), 0);
  }
}

TEST(H5ToffsetTest, ErrorPathsRejectInvalidTypesStatesAndNonAtomicClasses) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedType comp_dt(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(comp_dt.valid());
  ASSERT_GE(H5Tinsert(comp_dt.get(), "a", 0, H5T_NATIVE_INT32), 0);

  ScopedType opaque_dt(H5Tcreate(H5T_OPAQUE, 8));
  ASSERT_TRUE(opaque_dt.valid());

  ScopedType ref_dt(H5Tcopy(H5T_STD_REF_OBJ));
  ASSERT_TRUE(ref_dt.valid());

  constexpr hsize_t kDims[1] = {2};
  ScopedType comp_arr(H5Tarray_create2(comp_dt.get(), 1, kDims));
  ASSERT_TRUE(comp_arr.valid());

  // Declare silence before locked_dt so ~ScopedType on locked_dt is silent.
  ScopedErrorSilence silence;

  ScopedType locked_dt(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(locked_dt.valid());
  ASSERT_GE(H5Tlock(locked_dt.get()), 0);

  // Invalid / non-datatype handles.
  EXPECT_LT(H5Tget_offset(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Tget_offset(file.get()), 0);
  EXPECT_LT(H5Tset_offset(H5I_INVALID_HID, 0), 0);
  EXPECT_LT(H5Tset_offset(file.get(), 0), 0);

  // Read-only (predefined and locked) datatypes.
  EXPECT_LT(H5Tset_offset(H5T_NATIVE_INT, 0), 0);
  EXPECT_LT(H5Tset_offset(locked_dt.get(), 0), 0);

  // Non-atomic classes.
  EXPECT_LT(H5Tget_offset(comp_dt.get()), 0);
  EXPECT_LT(H5Tget_offset(opaque_dt.get()), 0);
  EXPECT_LT(H5Tget_offset(H5T_STD_REF_OBJ), 0);
  EXPECT_LT(H5Tget_offset(comp_arr.get()), 0);

  EXPECT_LT(H5Tset_offset(comp_dt.get(), 0), 0);
  EXPECT_LT(H5Tset_offset(opaque_dt.get(), 0), 0);
  EXPECT_LT(H5Tset_offset(ref_dt.get(), 0), 0);
}

}  // namespace
}  // namespace hdf5
