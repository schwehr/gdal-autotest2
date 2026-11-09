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
// Tests datatype byte-order query and mutation APIs (H5Tget_order and
// H5Tset_order) across atomic, derived, compound, opaque, string, and
// reference datatypes (third_party/hdf5/src/H5Torder.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Torder.c

#include "third_party/hdf5/src/hdf5.h"

#include <array>
#include <cstdint>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

TEST(H5TorderTest, GetOrderPredefinedAtomicAndNonAtomicTypes) {
  struct PredefinedOrderCase {
    hid_t type_id;
    H5T_order_t expected;
    std::string_view label;
  };
  const auto cases = std::to_array<PredefinedOrderCase>({
      {.type_id = H5T_STD_I32LE, .expected = H5T_ORDER_LE, .label = "I32LE"},
      {.type_id = H5T_STD_I32BE, .expected = H5T_ORDER_BE, .label = "I32BE"},
      {.type_id = H5T_IEEE_F32LE, .expected = H5T_ORDER_LE, .label = "F32LE"},
      {.type_id = H5T_IEEE_F64BE, .expected = H5T_ORDER_BE, .label = "F64BE"},
      {.type_id = H5T_VAX_F32, .expected = H5T_ORDER_VAX, .label = "VAX_F32"},
      {.type_id = H5T_STD_B16LE, .expected = H5T_ORDER_LE, .label = "B16LE"},
      {.type_id = H5T_STD_B16BE, .expected = H5T_ORDER_BE, .label = "B16BE"},
      {.type_id = H5T_UNIX_D32LE, .expected = H5T_ORDER_LE, .label = "D32LE"},
      {.type_id = H5T_UNIX_D32BE, .expected = H5T_ORDER_BE, .label = "D32BE"},
      {.type_id = H5T_C_S1, .expected = H5T_ORDER_NONE, .label = "C_S1"},
      {.type_id = H5T_FORTRAN_S1, .expected = H5T_ORDER_NONE, .label = "FORTRAN_S1"},
      {.type_id = H5T_STD_REF, .expected = H5T_ORDER_NONE, .label = "STD_REF"},
      {.type_id = H5T_STD_REF_OBJ, .expected = H5T_ORDER_NONE, .label = "STD_REF_OBJ"},
      {.type_id = H5T_STD_REF_DSETREG, .expected = H5T_ORDER_NONE, .label = "STD_REF_DSETREG"},
  });

  for (const auto& tc : cases) {
    EXPECT_EQ(H5Tget_order(tc.type_id), tc.expected) << tc.label;
  }

  // Opaque datatypes report H5T_ORDER_NONE; variable-length strings wrap an
  // unsigned char parent type and defer to its byte order.
  ScopedType opaque(H5Tcreate(H5T_OPAQUE, 8));
  ASSERT_TRUE(opaque.valid());
  EXPECT_EQ(H5Tget_order(opaque), H5T_ORDER_NONE);

  ScopedType vl_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(vl_str.valid());
  ASSERT_GE(H5Tset_size(vl_str, H5T_VARIABLE), 0);
  EXPECT_EQ(H5Tget_order(vl_str), H5Tget_order(H5T_NATIVE_UCHAR));
}

TEST(H5TorderTest, GetOrderFollowsDerivedParentChains) {
  ScopedType enum_be(H5Tenum_create(H5T_STD_I32BE));
  ASSERT_TRUE(enum_be.valid());
  EXPECT_EQ(H5Tget_order(enum_be), H5T_ORDER_BE);

  constexpr std::array<hsize_t, 1> kDims = {4};
  ScopedType arr_le(H5Tarray_create2(H5T_STD_U16LE, 1, kDims.data()));
  ASSERT_TRUE(arr_le.valid());
  EXPECT_EQ(H5Tget_order(arr_le), H5T_ORDER_LE);

  ScopedType vlen_be(H5Tvlen_create(H5T_IEEE_F32BE));
  ASSERT_TRUE(vlen_be.valid());
  EXPECT_EQ(H5Tget_order(vlen_be), H5T_ORDER_BE);

  ScopedType cplx_be(H5Tcomplex_create(H5T_IEEE_F64BE));
  ASSERT_TRUE(cplx_be.valid());
  EXPECT_EQ(H5Tget_order(cplx_be), H5T_ORDER_BE);

  // Nested derived type (array of enum of BE int) walks parent chain to atomic.
  ScopedType arr_of_enum(H5Tarray_create2(enum_be, 1, kDims.data()));
  ASSERT_TRUE(arr_of_enum.valid());
  EXPECT_EQ(H5Tget_order(arr_of_enum), H5T_ORDER_BE);
}

TEST(H5TorderTest, GetOrderCompoundHomogeneousNoneAndMixed) {
  // 1. Empty compound reports H5T_ORDER_NONE.
  ScopedType empty_comp(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(empty_comp.valid());
  EXPECT_EQ(H5Tget_order(empty_comp), H5T_ORDER_NONE);

  // 2. Compound with only H5T_ORDER_NONE members reports H5T_ORDER_NONE.
  ScopedType str4(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(str4.valid());
  ASSERT_GE(H5Tset_size(str4, 4), 0);
  ScopedType opaque4(H5Tcreate(H5T_OPAQUE, 4));
  ASSERT_TRUE(opaque4.valid());

  ScopedType none_comp(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(none_comp.valid());
  ASSERT_GE(H5Tinsert(none_comp, "s", 0, str4), 0);
  ASSERT_GE(H5Tinsert(none_comp, "o", 4, opaque4), 0);
  EXPECT_EQ(H5Tget_order(none_comp), H5T_ORDER_NONE);

  // 3. Leading H5T_ORDER_NONE members are ignored in favor of ordered members.
  ScopedType none_then_le(H5Tcreate(H5T_COMPOUND, 12));
  ASSERT_TRUE(none_then_le.valid());
  ASSERT_GE(H5Tinsert(none_then_le, "s", 0, str4), 0);
  ASSERT_GE(H5Tinsert(none_then_le, "i", 4, H5T_STD_I32LE), 0);
  ASSERT_GE(H5Tinsert(none_then_le, "f", 8, H5T_IEEE_F32LE), 0);
  EXPECT_EQ(H5Tget_order(none_then_le), H5T_ORDER_LE);

  // 4. Consistent BE members with trailing NONE member report H5T_ORDER_BE.
  ScopedType be_with_none(H5Tcreate(H5T_COMPOUND, 12));
  ASSERT_TRUE(be_with_none.valid());
  ASSERT_GE(H5Tinsert(be_with_none, "i", 0, H5T_STD_I32BE), 0);
  ASSERT_GE(H5Tinsert(be_with_none, "s", 4, str4), 0);
  ASSERT_GE(H5Tinsert(be_with_none, "f", 8, H5T_IEEE_F32BE), 0);
  EXPECT_EQ(H5Tget_order(be_with_none), H5T_ORDER_BE);

  // 5. Mixed LE and BE members trigger early break and H5T_ORDER_MIXED.
  ScopedType mixed_comp(H5Tcreate(H5T_COMPOUND, 12));
  ASSERT_TRUE(mixed_comp.valid());
  ASSERT_GE(H5Tinsert(mixed_comp, "le", 0, H5T_STD_I32LE), 0);
  ASSERT_GE(H5Tinsert(mixed_comp, "be", 4, H5T_STD_I32BE), 0);
  ASSERT_GE(H5Tinsert(mixed_comp, "trailing", 8, H5T_IEEE_F32LE), 0);
  EXPECT_EQ(H5Tget_order(mixed_comp), H5T_ORDER_MIXED);
}

TEST(H5TorderTest, SetOrderAtomicDerivedAndCompoundTypes) {
  // Atomic types accept LE, BE, and VAX orders.
  ScopedType int_t(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(int_t.valid());
  EXPECT_GE(H5Tset_order(int_t, H5T_ORDER_BE), 0);
  EXPECT_EQ(H5Tget_order(int_t), H5T_ORDER_BE);
  EXPECT_GE(H5Tset_order(int_t, H5T_ORDER_VAX), 0);
  EXPECT_EQ(H5Tget_order(int_t), H5T_ORDER_VAX);
  EXPECT_GE(H5Tset_order(int_t, H5T_ORDER_LE), 0);
  EXPECT_EQ(H5Tget_order(int_t), H5T_ORDER_LE);

  // H5T_ORDER_NONE is permitted on fixed strings, opaque types, and references.
  ScopedType fixed_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(fixed_str.valid());
  EXPECT_GE(H5Tset_order(fixed_str, H5T_ORDER_NONE), 0);
  EXPECT_EQ(H5Tget_order(fixed_str), H5T_ORDER_NONE);

  ScopedType opaque(H5Tcreate(H5T_OPAQUE, 4));
  ASSERT_TRUE(opaque.valid());
  EXPECT_GE(H5Tset_order(opaque, H5T_ORDER_NONE), 0);
  // Setting an endianness order on an opaque type is ignored cleanly.
  EXPECT_GE(H5Tset_order(opaque, H5T_ORDER_BE), 0);
  EXPECT_EQ(H5Tget_order(opaque), H5T_ORDER_NONE);

  ScopedType ref_copy(H5Tcopy(H5T_STD_REF));
  ASSERT_TRUE(ref_copy.valid());
  EXPECT_GE(H5Tset_order(ref_copy, H5T_ORDER_NONE), 0);
  EXPECT_GE(H5Tset_order(ref_copy, H5T_ORDER_LE), 0);
  EXPECT_EQ(H5Tget_order(ref_copy), H5T_ORDER_NONE);

  // Empty enum, array, vlen sequence, and complex types defer to parent.
  ScopedType enum_t(H5Tenum_create(H5T_STD_I32LE));
  ASSERT_TRUE(enum_t.valid());
  EXPECT_GE(H5Tset_order(enum_t, H5T_ORDER_BE), 0);
  EXPECT_EQ(H5Tget_order(enum_t), H5T_ORDER_BE);

  constexpr std::array<hsize_t, 1> kDims = {3};
  ScopedType arr_t(H5Tarray_create2(H5T_STD_I32LE, 1, kDims.data()));
  ASSERT_TRUE(arr_t.valid());
  EXPECT_GE(H5Tset_order(arr_t, H5T_ORDER_BE), 0);
  EXPECT_EQ(H5Tget_order(arr_t), H5T_ORDER_BE);

  // Compound recursively updates all members (while opaque ignores order).
  ScopedType comp(H5Tcreate(H5T_COMPOUND, 12));
  ASSERT_TRUE(comp.valid());
  ASSERT_GE(H5Tinsert(comp, "a", 0, H5T_STD_I32LE), 0);
  ASSERT_GE(H5Tinsert(comp, "b", 4, H5T_IEEE_F32BE), 0);
  ASSERT_GE(H5Tinsert(comp, "o", 8, opaque), 0);
  EXPECT_EQ(H5Tget_order(comp), H5T_ORDER_MIXED);
  EXPECT_GE(H5Tset_order(comp, H5T_ORDER_BE), 0);
  EXPECT_EQ(H5Tget_order(comp), H5T_ORDER_BE);
}

TEST(H5TorderTest, GetAndSetOrderErrorPaths) {
  ScopedErrorSilence silence;

  // Invalid IDs and non-datatype IDs fail.
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  EXPECT_EQ(H5Tget_order(H5I_INVALID_HID), H5T_ORDER_ERROR);
  EXPECT_EQ(H5Tget_order(space), H5T_ORDER_ERROR);
  EXPECT_LT(H5Tset_order(H5I_INVALID_HID, H5T_ORDER_LE), 0);
  EXPECT_LT(H5Tset_order(space, H5T_ORDER_LE), 0);

  // Illegal byte order values fail.
  ScopedType int_t(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(int_t.valid());
  EXPECT_LT(H5Tset_order(int_t, H5T_ORDER_ERROR), 0);
  EXPECT_LT(H5Tset_order(int_t, H5T_ORDER_MIXED), 0);
  EXPECT_LT(H5Tset_order(int_t, static_cast<H5T_order_t>(-2)), 0);
  EXPECT_LT(H5Tset_order(int_t, static_cast<H5T_order_t>(H5T_ORDER_NONE + 1)), 0);

  // Read-only predefined and committed datatypes fail.
  EXPECT_LT(H5Tset_order(H5T_STD_I32LE, H5T_ORDER_BE), 0);
  ScopedFile file = CreateInMemoryFile("order_commit.h5");
  ASSERT_TRUE(file.valid());
  ScopedType committed_t(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(committed_t.valid());
  ASSERT_GE(H5Tcommit2(file, "committed_i32", committed_t, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), 0);
  EXPECT_LT(H5Tset_order(committed_t, H5T_ORDER_BE), 0);

  // Enum with defined members cannot change order.
  ScopedType enum_t(H5Tenum_create(H5T_STD_I32LE));
  ASSERT_TRUE(enum_t.valid());
  int32_t enum_val = 1;
  ASSERT_GE(H5Tenum_insert(enum_t, "ONE", &enum_val), 0);
  EXPECT_LT(H5Tset_order(enum_t, H5T_ORDER_BE), 0);

  // H5T_ORDER_NONE is rejected on integers, floats, and variable-length strings.
  EXPECT_LT(H5Tset_order(int_t, H5T_ORDER_NONE), 0);
  ScopedType float_t(H5Tcopy(H5T_IEEE_F32LE));
  ASSERT_TRUE(float_t.valid());
  EXPECT_LT(H5Tset_order(float_t, H5T_ORDER_NONE), 0);
  ScopedType vl_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(vl_str.valid());
  ASSERT_GE(H5Tset_size(vl_str, H5T_VARIABLE), 0);
  EXPECT_LT(H5Tset_order(vl_str, H5T_ORDER_NONE), 0);

  // Empty compound fails to set order.
  ScopedType empty_comp(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(empty_comp.valid());
  EXPECT_LT(H5Tset_order(empty_comp, H5T_ORDER_LE), 0);

  // Compound containing an invalid-for-order member fails mid-loop.
  ScopedType bad_comp(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(bad_comp.valid());
  ASSERT_GE(H5Tinsert(bad_comp, "ok", 0, H5T_STD_I32LE), 0);
  ASSERT_GE(H5Tinsert(bad_comp, "en", 4, enum_t), 0);
  EXPECT_LT(H5Tset_order(bad_comp, H5T_ORDER_BE), 0);
}

}  // namespace
}  // namespace hdf5
