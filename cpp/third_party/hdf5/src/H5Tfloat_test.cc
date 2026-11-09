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
// Tests floating-point datatype properties: bit fields (H5Tget_fields /
// H5Tset_fields), exponent bias (H5Tget_ebias / H5Tset_ebias), mantissa
// normalization (H5Tget_norm / H5Tset_norm), and internal bit padding
// (H5Tget_inpad / H5Tset_inpad) (third_party/hdf5/src/H5Tfloat.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tfloat.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

struct ExpectedFloatLayout {
  size_t spos;
  size_t epos;
  size_t esize;
  size_t mpos;
  size_t msize;
  size_t ebias;
};

TEST(H5TfloatTest, GetFieldsBiasNormInpadForPredefinedAndDerivedTypes) {
  struct TypeSpec {
    hid_t type_id;
    ExpectedFloatLayout layout;
  };
  const std::array<TypeSpec, 13> specs = {{
      {H5T_IEEE_F16LE, {15, 10, 5, 0, 10, 15}},
      {H5T_IEEE_F16BE, {15, 10, 5, 0, 10, 15}},
      {H5T_IEEE_F32LE, {31, 23, 8, 0, 23, 127}},
      {H5T_IEEE_F32BE, {31, 23, 8, 0, 23, 127}},
      {H5T_IEEE_F64LE, {63, 52, 11, 0, 52, 1023}},
      {H5T_IEEE_F64BE, {63, 52, 11, 0, 52, 1023}},
      {H5T_FLOAT_BFLOAT16LE, {15, 7, 8, 0, 7, 127}},
      {H5T_FLOAT_BFLOAT16BE, {15, 7, 8, 0, 7, 127}},
      {H5T_FLOAT_F8E4M3, {7, 3, 4, 0, 3, 7}},
      {H5T_FLOAT_F8E5M2, {7, 2, 5, 0, 2, 15}},
      {H5T_FLOAT_F6E2M3, {5, 3, 2, 0, 3, 1}},
      {H5T_FLOAT_F6E3M2, {5, 2, 3, 0, 2, 3}},
      {H5T_FLOAT_F4E2M1, {3, 1, 2, 0, 1, 1}},
  }};

  for (const TypeSpec& spec : specs) {
    size_t spos = 0;
    size_t epos = 0;
    size_t esize = 0;
    size_t mpos = 0;
    size_t msize = 0;
    ASSERT_GE(H5Tget_fields(spec.type_id, &spos, &epos, &esize, &mpos, &msize),
              0);
    EXPECT_EQ(spos, spec.layout.spos);
    EXPECT_EQ(epos, spec.layout.epos);
    EXPECT_EQ(esize, spec.layout.esize);
    EXPECT_EQ(mpos, spec.layout.mpos);
    EXPECT_EQ(msize, spec.layout.msize);
    EXPECT_EQ(H5Tget_ebias(spec.type_id), spec.layout.ebias);
    EXPECT_EQ(H5Tget_norm(spec.type_id), H5T_NORM_IMPLIED);
    EXPECT_EQ(H5Tget_inpad(spec.type_id), H5T_PAD_ZERO);
  }

  // All-null output pointers are permitted by H5Tget_fields.
  EXPECT_GE(H5Tget_fields(H5T_IEEE_F32LE, nullptr, nullptr, nullptr, nullptr,
                          nullptr),
            0);

  // Partial null pointers and derived float types (complex and array).
  const hsize_t dim = 2;
  ScopedType arr_f64(H5Tarray_create2(H5T_IEEE_F64LE, 1, &dim));
  ASSERT_TRUE(arr_f64.valid());
  size_t ebias_check = H5Tget_ebias(arr_f64);
  EXPECT_EQ(ebias_check, 1023u);
  EXPECT_EQ(H5Tget_norm(arr_f64), H5T_NORM_IMPLIED);
  EXPECT_EQ(H5Tget_inpad(arr_f64), H5T_PAD_ZERO);

  size_t spos = 0;
  size_t esize = 0;
  ASSERT_GE(H5Tget_fields(H5T_COMPLEX_IEEE_F32LE, &spos, nullptr, &esize,
                          nullptr, nullptr),
            0);
  EXPECT_EQ(spos, 31u);
  EXPECT_EQ(esize, 8u);
  EXPECT_EQ(H5Tget_ebias(H5T_COMPLEX_IEEE_F32LE), 127u);
  EXPECT_EQ(H5Tget_norm(H5T_COMPLEX_IEEE_F32LE), H5T_NORM_IMPLIED);
  EXPECT_EQ(H5Tget_inpad(H5T_COMPLEX_IEEE_F32LE), H5T_PAD_ZERO);
}

TEST(H5TfloatTest, SetFieldsBiasNormInpadAndOffsetConversions) {
  // 1. Configure a custom 16-bit float embedded inside a 32-bit storage type
  // with a non-zero bit offset (8), then verify round-trip getters.
  ScopedType dt(H5Tcopy(H5T_IEEE_F32LE));
  ASSERT_TRUE(dt.valid());

  // Set fields first before reducing precision (per H5Tset_precision rules).
  ASSERT_GE(H5Tset_fields(dt, 15, 10, 5, 0, 10), 0);
  ASSERT_GE(H5Tset_precision(dt, 16), 0);
  ASSERT_GE(H5Tset_offset(dt, 8), 0);
  // Shift fields into the [8, 23] bit range after setting offset=8.
  ASSERT_GE(H5Tset_fields(dt, 23, 18, 5, 8, 10), 0);
  ASSERT_GE(H5Tset_ebias(dt, 15), 0);
  ASSERT_GE(H5Tset_norm(dt, H5T_NORM_IMPLIED), 0);
  ASSERT_GE(H5Tset_inpad(dt, H5T_PAD_ZERO), 0);

  size_t spos = 0;
  size_t epos = 0;
  size_t esize = 0;
  size_t mpos = 0;
  size_t msize = 0;
  ASSERT_GE(H5Tget_fields(dt, &spos, &epos, &esize, &mpos, &msize), 0);
  EXPECT_EQ(spos, 23u);
  EXPECT_EQ(epos, 18u);
  EXPECT_EQ(esize, 5u);
  EXPECT_EQ(mpos, 8u);
  EXPECT_EQ(msize, 10u);
  EXPECT_EQ(H5Tget_ebias(dt), 15u);

  // Convert IEEE half 1.5 (0x3E00) stored at bit offset 8 (0x003E0000 in LE)
  // to native float.
  std::array<uint8_t, 4> raw_buf = {0x00, 0x00, 0x3E, 0x00};
  ASSERT_GE(
      H5Tconvert(dt, H5T_NATIVE_FLOAT, 1, raw_buf.data(), nullptr, H5P_DEFAULT),
      0);
  float decoded = 0.0f;
  std::memcpy(&decoded, raw_buf.data(), sizeof(decoded));
  EXPECT_FLOAT_EQ(decoded, 1.5f);

  // 2. Verify all valid normalization and internal pad enum settings.
  ScopedType custom(H5Tcopy(H5T_IEEE_F32LE));
  ASSERT_TRUE(custom.valid());
  for (H5T_norm_t norm : {H5T_NORM_IMPLIED, H5T_NORM_MSBSET, H5T_NORM_NONE}) {
    EXPECT_GE(H5Tset_norm(custom, norm), 0);
    EXPECT_EQ(H5Tget_norm(custom), norm);
  }
  for (H5T_pad_t pad : {H5T_PAD_ZERO, H5T_PAD_ONE, H5T_PAD_BACKGROUND}) {
    EXPECT_GE(H5Tset_inpad(custom, pad), 0);
    EXPECT_EQ(H5Tget_inpad(custom), pad);
  }
  EXPECT_GE(H5Tset_ebias(custom, 0), 0);
  EXPECT_EQ(H5Tget_ebias(custom), 0u);

  // 3. Derived transient float (complex / array) forwards setters to base.
  ScopedType cplx(H5Tcomplex_create(custom));
  ASSERT_TRUE(cplx.valid());
  EXPECT_GE(H5Tset_fields(cplx, 31, 20, 11, 0, 20), 0);
  EXPECT_GE(H5Tset_ebias(cplx, 1023), 0);
  EXPECT_GE(H5Tset_norm(cplx, H5T_NORM_IMPLIED), 0);
  EXPECT_GE(H5Tset_inpad(cplx, H5T_PAD_ONE), 0);

  EXPECT_EQ(H5Tget_ebias(cplx), 1023u);
  EXPECT_EQ(H5Tget_norm(cplx), H5T_NORM_IMPLIED);
  EXPECT_EQ(H5Tget_inpad(cplx), H5T_PAD_ONE);

  // 4. Verify long double conversion buffer hygiene (zero padding bytes).
  long double ld;
  std::memset(&ld, 0, sizeof(ld));
  ld = 2.5L;
  double d = 0.0;
  uint8_t conv_buf[sizeof(long double)]{};
  std::memcpy(conv_buf, &ld, sizeof(ld));
  ASSERT_GE(H5Tconvert(H5T_NATIVE_LDOUBLE, H5T_NATIVE_DOUBLE, 1, conv_buf,
                       nullptr, H5P_DEFAULT),
            0);
  std::memcpy(&d, conv_buf, sizeof(d));
  EXPECT_DOUBLE_EQ(d, 2.5);
}

TEST(H5TfloatTest, GettersAndSettersRejectInvalidArgsAndLayouts) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedType int_copy(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(int_copy.valid());
  const hsize_t dim = 2;
  ScopedType arr_int(H5Tarray_create2(int_copy, 1, &dim));
  ASSERT_TRUE(arr_int.valid());

  ScopedType locked_f32(H5Tcopy(H5T_IEEE_F32LE));
  ASSERT_TRUE(locked_f32.valid());
  ASSERT_GE(H5Tlock(locked_f32), 0);

  ScopedType f32(H5Tcopy(H5T_IEEE_F32LE));
  ASSERT_TRUE(f32.valid());

  ScopedErrorSilence silence;

  // Invalid type IDs and non-float classes on all getters.
  const std::array<hid_t, 5> bad_get_ids = {
      H5I_INVALID_HID, file.get(), H5T_NATIVE_INT32, H5T_C_S1, arr_int.get()};
  for (hid_t bad_id : bad_get_ids) {
    size_t s = 0;
    EXPECT_LT(H5Tget_fields(bad_id, &s, &s, &s, &s, &s), 0);
    EXPECT_EQ(H5Tget_ebias(bad_id), 0u);
    EXPECT_EQ(H5Tget_norm(bad_id), H5T_NORM_ERROR);
    EXPECT_EQ(H5Tget_inpad(bad_id), H5T_PAD_ERROR);
  }

  // Invalid IDs, predefined immutable, locked, and non-float on all setters.
  const std::array<hid_t, 6> bad_set_ids = {H5I_INVALID_HID, file.get(),
                                            H5T_IEEE_F32LE,  locked_f32.get(),
                                            int_copy.get(),  arr_int.get()};
  for (hid_t bad_id : bad_set_ids) {
    EXPECT_LT(H5Tset_fields(bad_id, 31, 23, 8, 0, 23), 0);
    EXPECT_LT(H5Tset_ebias(bad_id, 127), 0);
    EXPECT_LT(H5Tset_norm(bad_id, H5T_NORM_IMPLIED), 0);
    EXPECT_LT(H5Tset_inpad(bad_id, H5T_PAD_ZERO), 0);
  }

  // Out-of-bounds field positions/sizes on 32-bit precision float.
  EXPECT_LT(H5Tset_fields(f32, 31, 25, 8, 0, 23), 0);   // exponent > prec
  EXPECT_LT(H5Tset_fields(f32, 31, 23, 8, 10, 23), 0);  // mantissa > prec
  EXPECT_LT(H5Tset_fields(f32, 32, 23, 8, 0, 23), 0);   // sign >= prec

  // Overlapping fields:
  EXPECT_LT(H5Tset_fields(f32, 25, 23, 8, 0, 23), 0);  // sign inside exponent
  EXPECT_LT(H5Tset_fields(f32, 5, 23, 8, 0, 23), 0);   // sign inside mantissa
  EXPECT_LT(H5Tset_fields(f32, 31, 23, 8, 0, 24), 0);  // mantissa overlaps exp
  EXPECT_LT(H5Tset_fields(f32, 31, 0, 9, 8, 23), 0);   // exp overlaps mantissa

  // Invalid norm / inpad enum values.
  EXPECT_LT(H5Tset_norm(f32, H5T_NORM_ERROR), 0);
  EXPECT_LT(H5Tset_norm(f32, static_cast<H5T_norm_t>(3)), 0);
  EXPECT_LT(H5Tset_inpad(f32, H5T_PAD_ERROR), 0);
  EXPECT_LT(H5Tset_inpad(f32, H5T_NPAD), 0);
}

}  // namespace
}  // namespace hdf5
