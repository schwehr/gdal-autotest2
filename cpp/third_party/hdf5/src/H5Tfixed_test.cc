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
// Tests integer datatype sign queries and mutations (H5Tget_sign, H5Tset_sign)
// across predefined, transient, locked, committed, and derived datatypes
// (third_party/hdf5/src/H5Tfixed.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tfixed.c

#include <array>
#include <cstdint>
#include <cstring>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

TEST(H5TfixedTest, GetSignPredefinedAndDerivedIntegerTypes) {
  EXPECT_EQ(H5Tget_sign(H5T_NATIVE_INT8), H5T_SGN_2);
  EXPECT_EQ(H5Tget_sign(H5T_NATIVE_INT32), H5T_SGN_2);
  EXPECT_EQ(H5Tget_sign(H5T_STD_I16BE), H5T_SGN_2);

  EXPECT_EQ(H5Tget_sign(H5T_NATIVE_UINT8), H5T_SGN_NONE);
  EXPECT_EQ(H5Tget_sign(H5T_NATIVE_UINT32), H5T_SGN_NONE);
  EXPECT_EQ(H5Tget_sign(H5T_STD_U64LE), H5T_SGN_NONE);

  // Derived types defer to their base parent datatype.
  ScopedType enum_u16(H5Tenum_create(H5T_NATIVE_UINT16));
  ASSERT_TRUE(enum_u16.valid());
  EXPECT_EQ(H5Tget_sign(enum_u16), H5T_SGN_NONE);

  const hsize_t dim = 3;
  ScopedType arr_i32(H5Tarray_create2(H5T_NATIVE_INT32, 1, &dim));
  ASSERT_TRUE(arr_i32.valid());
  EXPECT_EQ(H5Tget_sign(arr_i32), H5T_SGN_2);

  ScopedType vlen_u32(H5Tvlen_create(H5T_STD_U32LE));
  ASSERT_TRUE(vlen_u32.valid());
  EXPECT_EQ(H5Tget_sign(vlen_u32), H5T_SGN_NONE);
}

TEST(H5TfixedTest, SetSignTogglesSignOnTransientAndEmptyEnumTypes) {
  // 1. Toggle sign on a transient integer datatype and convert -1 -> uint64.
  ScopedType dt(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(dt.valid());
  EXPECT_EQ(H5Tget_sign(dt), H5T_SGN_2);

  ASSERT_GE(H5Tset_sign(dt, H5T_SGN_NONE), 0);
  EXPECT_EQ(H5Tget_sign(dt), H5T_SGN_NONE);

  // Reinterpreting 0xFFFFFFFFu as unsigned produces 4294967295ULL, whereas
  // signed two's complement produces -1LL.
  const uint32_t all_ones = 0xFFFFFFFFu;
  std::array<uint8_t, sizeof(uint64_t)> buf{};
  std::memcpy(buf.data(), &all_ones, sizeof(all_ones));
  ASSERT_GE(
      H5Tconvert(dt, H5T_NATIVE_UINT64, 1, buf.data(), nullptr, H5P_DEFAULT),
      0);
  uint64_t out_u64 = 0;
  std::memcpy(&out_u64, buf.data(), sizeof(out_u64));
  EXPECT_EQ(out_u64, 0xFFFFFFFFULL);

  ASSERT_GE(H5Tset_sign(dt, H5T_SGN_2), 0);
  EXPECT_EQ(H5Tget_sign(dt), H5T_SGN_2);

  buf.fill(0);
  std::memcpy(buf.data(), &all_ones, sizeof(all_ones));
  ASSERT_GE(
      H5Tconvert(dt, H5T_NATIVE_INT64, 1, buf.data(), nullptr, H5P_DEFAULT), 0);
  int64_t out_i64 = 0;
  std::memcpy(&out_i64, buf.data(), sizeof(out_i64));
  EXPECT_EQ(out_i64, -1LL);

  // 2. Empty enum delegates H5Tset_sign to its base integer parent before
  // members are inserted.
  ScopedType empty_enum(H5Tenum_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(empty_enum.valid());
  ASSERT_GE(H5Tset_sign(empty_enum, H5T_SGN_NONE), 0);
  EXPECT_EQ(H5Tget_sign(empty_enum), H5T_SGN_NONE);
}

TEST(H5TfixedTest, GetAndSetSignErrorPaths) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedType float_copy(H5Tcopy(H5T_NATIVE_FLOAT));
  ASSERT_TRUE(float_copy.valid());
  const hsize_t dim = 2;
  ScopedType arr_float(H5Tarray_create2(float_copy, 1, &dim));
  ASSERT_TRUE(arr_float.valid());

  ScopedType locked_int(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(locked_int.valid());
  ASSERT_GE(H5Tlock(locked_int), 0);

  ScopedType pop_enum(H5Tenum_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(pop_enum.valid());
  const int32_t zero = 0;
  ASSERT_GE(H5Tenum_insert(pop_enum, "ZERO", &zero), 0);

  ScopedType int_copy(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(int_copy.valid());

  ScopedErrorSilence silence;

  // H5Tget_sign errors: invalid IDs and non-integer classes (including
  // derived).
  EXPECT_EQ(H5Tget_sign(H5I_INVALID_HID), H5T_SGN_ERROR);
  EXPECT_EQ(H5Tget_sign(file), H5T_SGN_ERROR);
  EXPECT_EQ(H5Tget_sign(H5T_NATIVE_FLOAT), H5T_SGN_ERROR);
  EXPECT_EQ(H5Tget_sign(H5T_C_S1), H5T_SGN_ERROR);
  EXPECT_EQ(H5Tget_sign(arr_float), H5T_SGN_ERROR);

  // H5Tset_sign errors: invalid ID, predefined immutable, and locked types.
  EXPECT_LT(H5Tset_sign(H5I_INVALID_HID, H5T_SGN_NONE), 0);
  EXPECT_LT(H5Tset_sign(file, H5T_SGN_NONE), 0);
  EXPECT_LT(H5Tset_sign(H5T_NATIVE_INT32, H5T_SGN_NONE), 0);
  EXPECT_LT(H5Tset_sign(locked_int, H5T_SGN_NONE), 0);

  // Invalid sign values.
  EXPECT_LT(H5Tset_sign(int_copy, H5T_SGN_ERROR), 0);
  EXPECT_LT(H5Tset_sign(int_copy, H5T_NSGN), 0);
  EXPECT_LT(H5Tset_sign(int_copy, static_cast<H5T_sign_t>(99)), 0);

  // Enum with members already inserted.
  EXPECT_LT(H5Tset_sign(pop_enum, H5T_SGN_NONE), 0);

  // Non-integer transient types (direct and derived).
  EXPECT_LT(H5Tset_sign(float_copy, H5T_SGN_NONE), 0);
  EXPECT_LT(H5Tset_sign(arr_float, H5T_SGN_NONE), 0);
}

}  // namespace
}  // namespace hdf5
