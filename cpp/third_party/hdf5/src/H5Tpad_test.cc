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
// Tests datatype bit-padding query and mutation APIs (H5Tget_pad and
// H5Tset_pad) across atomic, derived, and non-atomic datatypes
// (third_party/hdf5/src/H5Tpad.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tpad.c

#include "third_party/hdf5/src/hdf5.h"

#include <array>
#include <cstdint>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

TEST(H5TpadTest, GetAndSetPadOnAtomicAndDerivedTypes) {
  ScopedType int_t(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(int_t.valid());

  H5T_pad_t lsb = H5T_PAD_ERROR;
  H5T_pad_t msb = H5T_PAD_ERROR;
  ASSERT_GE(H5Tget_pad(int_t, &lsb, &msb), 0);
  EXPECT_EQ(lsb, H5T_PAD_ZERO);
  EXPECT_EQ(msb, H5T_PAD_ZERO);

  // Either output pointer may be null.
  lsb = H5T_PAD_ERROR;
  msb = H5T_PAD_ERROR;
  EXPECT_GE(H5Tget_pad(int_t, &lsb, nullptr), 0);
  EXPECT_EQ(lsb, H5T_PAD_ZERO);
  EXPECT_GE(H5Tget_pad(int_t, nullptr, &msb), 0);
  EXPECT_EQ(msb, H5T_PAD_ZERO);
  EXPECT_GE(H5Tget_pad(int_t, nullptr, nullptr), 0);

  // Exercise all valid H5T_pad_t combinations.
  constexpr auto kPads = std::to_array<H5T_pad_t>({
      H5T_PAD_ZERO,
      H5T_PAD_ONE,
      H5T_PAD_BACKGROUND,
  });
  for (H5T_pad_t l : kPads) {
    for (H5T_pad_t m : kPads) {
      ASSERT_GE(H5Tset_pad(int_t, l, m), 0);
      lsb = H5T_PAD_ERROR;
      msb = H5T_PAD_ERROR;
      ASSERT_GE(H5Tget_pad(int_t, &lsb, &msb), 0);
      EXPECT_EQ(lsb, l);
      EXPECT_EQ(msb, m);
    }
  }

  // Derived types (empty enum, array, complex) defer to atomic parent.
  ScopedType enum_t(H5Tenum_create(H5T_STD_I32LE));
  ASSERT_TRUE(enum_t.valid());
  ASSERT_GE(H5Tset_pad(enum_t, H5T_PAD_ONE, H5T_PAD_BACKGROUND), 0);
  ASSERT_GE(H5Tget_pad(enum_t, &lsb, &msb), 0);
  EXPECT_EQ(lsb, H5T_PAD_ONE);
  EXPECT_EQ(msb, H5T_PAD_BACKGROUND);

  constexpr std::array<hsize_t, 1> kDims = {2};
  ScopedType arr_t(H5Tarray_create2(H5T_IEEE_F32LE, 1, kDims.data()));
  ASSERT_TRUE(arr_t.valid());
  ASSERT_GE(H5Tset_pad(arr_t, H5T_PAD_BACKGROUND, H5T_PAD_ZERO), 0);
  ASSERT_GE(H5Tget_pad(arr_t, &lsb, &msb), 0);
  EXPECT_EQ(lsb, H5T_PAD_BACKGROUND);
  EXPECT_EQ(msb, H5T_PAD_ZERO);

  ScopedType cplx_t(H5Tcomplex_create(H5T_IEEE_F64LE));
  ASSERT_TRUE(cplx_t.valid());
  ASSERT_GE(H5Tset_pad(cplx_t, H5T_PAD_ONE, H5T_PAD_ONE), 0);
  ASSERT_GE(H5Tget_pad(cplx_t, &lsb, &msb), 0);
  EXPECT_EQ(lsb, H5T_PAD_ONE);
  EXPECT_EQ(msb, H5T_PAD_ONE);
}

TEST(H5TpadTest, GetAndSetPadErrorPaths) {
  ScopedErrorSilence silence;
  H5T_pad_t lsb = H5T_PAD_ERROR;
  H5T_pad_t msb = H5T_PAD_ERROR;

  // Invalid IDs and non-datatype IDs fail.
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  EXPECT_LT(H5Tget_pad(H5I_INVALID_HID, &lsb, &msb), 0);
  EXPECT_LT(H5Tget_pad(space, &lsb, &msb), 0);
  EXPECT_LT(H5Tset_pad(H5I_INVALID_HID, H5T_PAD_ZERO, H5T_PAD_ZERO), 0);
  EXPECT_LT(H5Tset_pad(space, H5T_PAD_ZERO, H5T_PAD_ZERO), 0);

  // Immutable predefined datatypes fail with read-only error.
  EXPECT_LT(H5Tset_pad(H5T_STD_I32LE, H5T_PAD_ONE, H5T_PAD_ONE), 0);
  EXPECT_LT(H5Tset_pad(H5T_IEEE_F32LE, H5T_PAD_ONE, H5T_PAD_ONE), 0);

  // Invalid pad enum values fail for both lsb and msb.
  ScopedType int_t(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(int_t.valid());
  EXPECT_LT(H5Tset_pad(int_t, H5T_PAD_ERROR, H5T_PAD_ZERO), 0);
  EXPECT_LT(H5Tset_pad(int_t, H5T_NPAD, H5T_PAD_ZERO), 0);
  EXPECT_LT(H5Tset_pad(int_t, H5T_PAD_ZERO, H5T_PAD_ERROR), 0);
  EXPECT_LT(H5Tset_pad(int_t, H5T_PAD_ZERO, H5T_NPAD), 0);

  // Enum with defined members cannot change padding.
  ScopedType enum_t(H5Tenum_create(H5T_STD_I32LE));
  ASSERT_TRUE(enum_t.valid());
  int32_t val = 7;
  ASSERT_GE(H5Tenum_insert(enum_t, "SEVEN", &val), 0);
  EXPECT_LT(H5Tset_pad(enum_t, H5T_PAD_ONE, H5T_PAD_ONE), 0);

  // Non-atomic datatypes (compound, opaque, reference) are rejected.
  ScopedType comp(H5Tcreate(H5T_COMPOUND, 4));
  ASSERT_TRUE(comp.valid());
  EXPECT_LT(H5Tget_pad(comp, &lsb, &msb), 0);
  EXPECT_LT(H5Tset_pad(comp, H5T_PAD_ZERO, H5T_PAD_ZERO), 0);

  ScopedType opaque(H5Tcreate(H5T_OPAQUE, 4));
  ASSERT_TRUE(opaque.valid());
  EXPECT_LT(H5Tget_pad(opaque, &lsb, &msb), 0);
  EXPECT_LT(H5Tset_pad(opaque, H5T_PAD_ZERO, H5T_PAD_ZERO), 0);

  ScopedType ref_copy(H5Tcopy(H5T_STD_REF));
  ASSERT_TRUE(ref_copy.valid());
  EXPECT_LT(H5Tget_pad(ref_copy, &lsb, &msb), 0);
  EXPECT_LT(H5Tset_pad(ref_copy, H5T_PAD_ZERO, H5T_PAD_ZERO), 0);
}

}  // namespace
}  // namespace hdf5
