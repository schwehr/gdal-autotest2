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
// Tests datatype string-padding query and mutation APIs (H5Tget_strpad and
// H5Tset_strpad) across fixed-length, variable-length, and derived string
// datatypes (third_party/hdf5/src/H5Tstrpad.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tstrpad.c

#include "third_party/hdf5/src/hdf5.h"

#include <array>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

TEST(H5TstrpadTest, GetAndSetStrpadFixedVariableAndDerivedStrings) {
  EXPECT_EQ(H5Tget_strpad(H5T_C_S1), H5T_STR_NULLTERM);
  EXPECT_EQ(H5Tget_strpad(H5T_FORTRAN_S1), H5T_STR_SPACEPAD);

  constexpr auto kStrPads = std::to_array<H5T_str_t>({
      H5T_STR_NULLTERM,
      H5T_STR_NULLPAD,
      H5T_STR_SPACEPAD,
  });

  // Fixed-length string supports all padding modes.
  ScopedType fixed_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(fixed_str.valid());
  ASSERT_GE(H5Tset_size(fixed_str, 16), 0);
  for (H5T_str_t pad : kStrPads) {
    ASSERT_GE(H5Tset_strpad(fixed_str, pad), 0);
    EXPECT_EQ(H5Tget_strpad(fixed_str), pad);
  }

  // Variable-length string supports all padding modes.
  ScopedType vl_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(vl_str.valid());
  ASSERT_GE(H5Tset_size(vl_str, H5T_VARIABLE), 0);
  for (H5T_str_t pad : kStrPads) {
    ASSERT_GE(H5Tset_strpad(vl_str, pad), 0);
    EXPECT_EQ(H5Tget_strpad(vl_str), pad);
  }

  // Derived array wrapping a string defers to its parent string type.
  constexpr std::array<hsize_t, 1> kDims = {3};
  ScopedType arr_fixed(H5Tarray_create2(fixed_str, 1, kDims.data()));
  ASSERT_TRUE(arr_fixed.valid());
  ASSERT_GE(H5Tset_strpad(arr_fixed, H5T_STR_NULLPAD), 0);
  EXPECT_EQ(H5Tget_strpad(arr_fixed), H5T_STR_NULLPAD);

  ScopedType arr_vl(H5Tarray_create2(vl_str, 1, kDims.data()));
  ASSERT_TRUE(arr_vl.valid());
  ASSERT_GE(H5Tset_strpad(arr_vl, H5T_STR_NULLTERM), 0);
  EXPECT_EQ(H5Tget_strpad(arr_vl), H5T_STR_NULLTERM);
}

TEST(H5TstrpadTest, GetAndSetStrpadErrorPaths) {
  ScopedErrorSilence silence;

  // Invalid IDs and non-datatype IDs fail.
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  EXPECT_EQ(H5Tget_strpad(H5I_INVALID_HID), H5T_STR_ERROR);
  EXPECT_EQ(H5Tget_strpad(space), H5T_STR_ERROR);
  EXPECT_LT(H5Tset_strpad(H5I_INVALID_HID, H5T_STR_NULLTERM), 0);
  EXPECT_LT(H5Tset_strpad(space, H5T_STR_NULLTERM), 0);

  // Immutable predefined string datatypes fail with read-only error.
  EXPECT_LT(H5Tset_strpad(H5T_C_S1, H5T_STR_NULLPAD), 0);
  EXPECT_LT(H5Tset_strpad(H5T_FORTRAN_S1, H5T_STR_NULLTERM), 0);

  // Illegal padding values fail.
  ScopedType str_t(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(str_t.valid());
  EXPECT_LT(H5Tset_strpad(str_t, H5T_STR_ERROR), 0);
  EXPECT_LT(H5Tset_strpad(str_t, H5T_STR_RESERVED_3), 0);

  // Non-string atomic and composite types are rejected.
  ScopedType int_t(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(int_t.valid());
  EXPECT_EQ(H5Tget_strpad(int_t), H5T_STR_ERROR);
  EXPECT_LT(H5Tset_strpad(int_t, H5T_STR_NULLTERM), 0);

  constexpr std::array<hsize_t, 1> kDims = {2};
  ScopedType arr_int(H5Tarray_create2(int_t, 1, kDims.data()));
  ASSERT_TRUE(arr_int.valid());
  EXPECT_EQ(H5Tget_strpad(arr_int), H5T_STR_ERROR);
  EXPECT_LT(H5Tset_strpad(arr_int, H5T_STR_NULLTERM), 0);

  ScopedType comp(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(comp.valid());
  EXPECT_EQ(H5Tget_strpad(comp), H5T_STR_ERROR);
  EXPECT_LT(H5Tset_strpad(comp, H5T_STR_NULLTERM), 0);
}

}  // namespace
}  // namespace hdf5
