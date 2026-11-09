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
// Tests datatype precision query and mutation APIs (H5Tget_precision and
// H5Tset_precision) across atomic, floating-point, and derived datatypes
// (third_party/hdf5/src/H5Tprecis.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tprecis.c

#include "third_party/hdf5/src/hdf5.h"

#include <array>
#include <cstdint>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

TEST(H5TprecisTest, GetPrecisionAtomicAndDerivedTypes) {
  struct PrecisionCase {
    hid_t type_id;
    size_t expected_bits;
    std::string_view label;
  };
  const auto cases = std::to_array<PrecisionCase>({
      {.type_id = H5T_STD_I8LE, .expected_bits = 8, .label = "I8LE"},
      {.type_id = H5T_STD_I32LE, .expected_bits = 32, .label = "I32LE"},
      {.type_id = H5T_IEEE_F64LE, .expected_bits = 64, .label = "F64LE"},
      {.type_id = H5T_C_S1, .expected_bits = 8, .label = "C_S1"},
      {.type_id = H5T_STD_B16LE, .expected_bits = 16, .label = "B16LE"},
      {.type_id = H5T_UNIX_D32LE, .expected_bits = 32, .label = "D32LE"},
  });

  for (const auto& tc : cases) {
    EXPECT_EQ(H5Tget_precision(tc.type_id), tc.expected_bits) << tc.label;
  }

  ScopedType enum_t(H5Tenum_create(H5T_STD_I16LE));
  ASSERT_TRUE(enum_t.valid());
  EXPECT_EQ(H5Tget_precision(enum_t), 16u);

  constexpr std::array<hsize_t, 1> kDims = {4};
  ScopedType arr_t(H5Tarray_create2(H5T_STD_I32LE, 1, kDims.data()));
  ASSERT_TRUE(arr_t.valid());
  EXPECT_EQ(H5Tget_precision(arr_t), 32u);

  ScopedType vlen_t(H5Tvlen_create(H5T_IEEE_F32LE));
  ASSERT_TRUE(vlen_t.valid());
  EXPECT_EQ(H5Tget_precision(vlen_t), 32u);
}

TEST(H5TprecisTest, SetPrecisionAdjustsOffsetAndSizeForIntegersBitsAndTime) {
  ScopedType int_t(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(int_t.valid());

  // 1. Decreasing precision keeps size unchanged.
  ASSERT_GE(H5Tset_precision(int_t, 16), 0);
  EXPECT_EQ(H5Tget_precision(int_t), 16u);
  EXPECT_EQ(H5Tget_size(int_t), 4u);

  // 2. Precision + offset > 8 * size shifts offset down to fit inside size.
  ASSERT_GE(H5Tset_offset(int_t, 8), 0);
  ASSERT_GE(H5Tset_precision(int_t, 28), 0);
  EXPECT_EQ(H5Tget_precision(int_t), 28u);
  EXPECT_EQ(H5Tget_offset(int_t), 4);
  EXPECT_EQ(H5Tget_size(int_t), 4u);

  // 3. Precision > 8 * size resets offset to 0 and rounds byte size up.
  ASSERT_GE(H5Tset_precision(int_t, 37), 0);
  EXPECT_EQ(H5Tget_precision(int_t), 37u);
  EXPECT_EQ(H5Tget_offset(int_t), 0);
  EXPECT_EQ(H5Tget_size(int_t), 5u);

  // Bitfield and time classes also support H5Tset_precision.
  ScopedType bit_t(H5Tcopy(H5T_STD_B16LE));
  ASSERT_TRUE(bit_t.valid());
  ASSERT_GE(H5Tset_precision(bit_t, 24), 0);
  EXPECT_EQ(H5Tget_precision(bit_t), 24u);
  EXPECT_EQ(H5Tget_size(bit_t), 3u);

  ScopedType time_t(H5Tcopy(H5T_UNIX_D32LE));
  ASSERT_TRUE(time_t.valid());
  ASSERT_GE(H5Tset_precision(time_t, 64), 0);
  EXPECT_EQ(H5Tget_precision(time_t), 64u);
  EXPECT_EQ(H5Tget_size(time_t), 8u);
}

TEST(H5TprecisTest, SetPrecisionFloatingPointFieldValidationAndSuccess) {
  // Decreasing float precision without adjusting sign/exponent/mantissa fails.
  {
    ScopedErrorSilence silence;
    // 1. sign >= prec + offset (IEEE F32 has sign=31).
    ScopedType f1(H5Tcopy(H5T_IEEE_F32LE));
    ASSERT_TRUE(f1.valid());
    EXPECT_LT(H5Tset_precision(f1, 31), 0);

    // 2. epos + esize > prec + offset while sign < prec + offset.
    ScopedType f2(H5Tcopy(H5T_IEEE_F32LE));
    ASSERT_TRUE(f2.valid());
    ASSERT_GE(H5Tset_fields(f2, /*spos=*/15, /*epos=*/16, /*esize=*/8, /*mpos=*/0, /*msize=*/10), 0);
    EXPECT_LT(H5Tset_precision(f2, 20), 0);

    // 3. mpos + msize > prec + offset while sign and exponent fit.
    ScopedType f3(H5Tcopy(H5T_IEEE_F32LE));
    ASSERT_TRUE(f3.valid());
    ASSERT_GE(H5Tset_fields(f3, /*spos=*/15, /*epos=*/10, /*esize=*/5, /*mpos=*/16, /*msize=*/10), 0);
    EXPECT_LT(H5Tset_precision(f3, 20), 0);
  }

  // Adjusting sign, exponent, and mantissa fields first allows reducing precision.
  ScopedType float_t(H5Tcopy(H5T_IEEE_F32LE));
  ASSERT_TRUE(float_t.valid());
  ASSERT_GE(H5Tset_fields(float_t, /*spos=*/15, /*epos=*/10, /*esize=*/5, /*mpos=*/0, /*msize=*/10), 0);
  ASSERT_GE(H5Tset_precision(float_t, 16), 0);
  EXPECT_EQ(H5Tget_precision(float_t), 16u);
}

TEST(H5TprecisTest, SetPrecisionOnDerivedTypesUpdatesWrapperSizes) {
  // Array size scales by nelem * parent_size.
  constexpr std::array<hsize_t, 1> kDims = {3};
  ScopedType arr_t(H5Tarray_create2(H5T_STD_I32LE, 1, kDims.data()));
  ASSERT_TRUE(arr_t.valid());
  EXPECT_EQ(H5Tget_size(arr_t), 12u);
  ASSERT_GE(H5Tset_precision(arr_t, 64), 0);
  EXPECT_EQ(H5Tget_precision(arr_t), 64u);
  EXPECT_EQ(H5Tget_size(arr_t), 24u);

  // Complex size scales by 2 * parent_size.
  ScopedType cplx_t(H5Tcomplex_create(H5T_IEEE_F32LE));
  ASSERT_TRUE(cplx_t.valid());
  EXPECT_EQ(H5Tget_size(cplx_t), 8u);
  ASSERT_GE(H5Tset_precision(cplx_t, 64), 0);
  EXPECT_EQ(H5Tget_precision(cplx_t), 64u);
  EXPECT_EQ(H5Tget_size(cplx_t), 16u);

  // VLEN size stays sizeof(hvl_t) while parent precision changes.
  ScopedType vlen_t(H5Tvlen_create(H5T_STD_I32LE));
  ASSERT_TRUE(vlen_t.valid());
  const size_t vlen_sz = H5Tget_size(vlen_t);
  ASSERT_GE(H5Tset_precision(vlen_t, 64), 0);
  EXPECT_EQ(H5Tget_precision(vlen_t), 64u);
  EXPECT_EQ(H5Tget_size(vlen_t), vlen_sz);

  // Empty enum size tracks parent size.
  ScopedType enum_t(H5Tenum_create(H5T_STD_I32LE));
  ASSERT_TRUE(enum_t.valid());
  ASSERT_GE(H5Tset_precision(enum_t, 64), 0);
  EXPECT_EQ(H5Tget_precision(enum_t), 64u);
  EXPECT_EQ(H5Tget_size(enum_t), 8u);

  // Parent error propagates through recursive H5T__set_precision call.
  ScopedErrorSilence silence;
  ScopedType arr_float(H5Tarray_create2(H5T_IEEE_F32LE, 1, kDims.data()));
  ASSERT_TRUE(arr_float.valid());
  EXPECT_LT(H5Tset_precision(arr_float, 16), 0);
}

TEST(H5TprecisTest, GetAndSetPrecisionErrorPaths) {
  ScopedErrorSilence silence;

  // Invalid IDs and non-datatype IDs fail.
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  EXPECT_EQ(H5Tget_precision(H5I_INVALID_HID), 0u);
  EXPECT_EQ(H5Tget_precision(space), 0u);
  EXPECT_LT(H5Tset_precision(H5I_INVALID_HID, 16), 0);
  EXPECT_LT(H5Tset_precision(space, 16), 0);

  // Read-only predefined and committed types fail.
  EXPECT_LT(H5Tset_precision(H5T_STD_I32LE, 16), 0);
  ScopedFile file = CreateInMemoryFile("precis_commit.h5");
  ASSERT_TRUE(file.valid());
  ScopedType committed_t(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(committed_t.valid());
  ASSERT_GE(H5Tcommit2(file, "committed_prec", committed_t, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), 0);
  EXPECT_LT(H5Tset_precision(committed_t, 16), 0);

  // Zero precision fails.
  ScopedType int_t(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(int_t.valid());
  EXPECT_LT(H5Tset_precision(int_t, 0), 0);

  // Populated enum cannot change precision.
  ScopedType enum_t(H5Tenum_create(H5T_STD_I32LE));
  ASSERT_TRUE(enum_t.valid());
  int32_t val = 1;
  ASSERT_GE(H5Tenum_insert(enum_t, "ONE", &val), 0);
  EXPECT_LT(H5Tset_precision(enum_t, 16), 0);

  // Fixed strings have read-only precision.
  ScopedType str_t(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(str_t.valid());
  EXPECT_LT(H5Tset_precision(str_t, 16), 0);

  // Compound and opaque reject both get and set.
  ScopedType comp_t(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(comp_t.valid());
  EXPECT_EQ(H5Tget_precision(comp_t), 0u);
  EXPECT_LT(H5Tset_precision(comp_t, 16), 0);

  ScopedType opaque_t(H5Tcreate(H5T_OPAQUE, 8));
  ASSERT_TRUE(opaque_t.valid());
  EXPECT_EQ(H5Tget_precision(opaque_t), 0u);
  EXPECT_LT(H5Tset_precision(opaque_t, 16), 0);

  // Reference datatypes hit non-atomic rejection in H5T__set_precision.
  ScopedType ref_t(H5Tcopy(H5T_STD_REF));
  ASSERT_TRUE(ref_t.valid());
  EXPECT_EQ(H5Tget_precision(ref_t), 0u);
  EXPECT_LT(H5Tset_precision(ref_t, 16), 0);

  // Variable-length string delegates precision to its unsigned char parent.
  ScopedType vl_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(vl_str.valid());
  ASSERT_GE(H5Tset_size(vl_str, H5T_VARIABLE), 0);
  EXPECT_GE(H5Tset_precision(vl_str, 16), 0);
  EXPECT_EQ(H5Tget_precision(vl_str), 16u);
}

}  // namespace
}  // namespace hdf5
