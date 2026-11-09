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
// Tests native floating-point datatype initialization and detected hardware
// representations (`H5T__init_native_float_types`, `H5T_NATIVE_FLOAT*`)
// (third_party/hdf5/src/H5Tinit_float.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tinit_float.c

#include <cstddef>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5TINIT_FLOAT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#if defined(H5_HAVE__FLOAT16) && defined(__cplusplus)
__extension__ typedef _Float16 H5__Float16;
#endif
extern "C" {
#define H5T_FRIEND
#include "third_party/hdf5/src/H5Tpkg.h"
}  // extern "C"
#ifdef H5TINIT_FLOAT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TINIT_FLOAT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

TEST(H5TinitFloatTest, ReinitPopulatesAllNativeFloatingPointTypes) {
  // Close and re-open so H5T__init_native_float_types() runs cleanly inside
  // this test target's execution as well.
  ASSERT_GE(H5close(), 0);
  ASSERT_GE(H5open(), 0);

  // float: IEEE 754 binary32
  EXPECT_EQ(H5Tget_class(H5T_NATIVE_FLOAT), H5T_FLOAT);
  EXPECT_EQ(H5Tget_size(H5T_NATIVE_FLOAT), sizeof(float));
  EXPECT_EQ(H5Tget_precision(H5T_NATIVE_FLOAT), 32U);
  EXPECT_EQ(H5Tget_offset(H5T_NATIVE_FLOAT), 0);
  EXPECT_EQ(H5Tget_ebias(H5T_NATIVE_FLOAT), 127U);
  EXPECT_EQ(H5Tget_norm(H5T_NATIVE_FLOAT), H5T_NORM_IMPLIED);
  EXPECT_EQ(H5Tget_inpad(H5T_NATIVE_FLOAT), H5T_PAD_ZERO);
  {
    size_t spos = 0, epos = 0, esize = 0, mpos = 0, msize = 0;
    ASSERT_GE(H5Tget_fields(H5T_NATIVE_FLOAT, &spos, &epos, &esize, &mpos,
                            &msize),
              0);
    EXPECT_EQ(spos, 31U);
    EXPECT_EQ(epos, 23U);
    EXPECT_EQ(esize, 8U);
    EXPECT_EQ(mpos, 0U);
    EXPECT_EQ(msize, 23U);
  }
  EXPECT_EQ(H5T_NATIVE_FLOAT_ALIGN_g, alignof(float));

  // double: IEEE 754 binary64
  EXPECT_EQ(H5Tget_class(H5T_NATIVE_DOUBLE), H5T_FLOAT);
  EXPECT_EQ(H5Tget_size(H5T_NATIVE_DOUBLE), sizeof(double));
  EXPECT_EQ(H5Tget_precision(H5T_NATIVE_DOUBLE), 64U);
  EXPECT_EQ(H5Tget_offset(H5T_NATIVE_DOUBLE), 0);
  EXPECT_EQ(H5Tget_ebias(H5T_NATIVE_DOUBLE), 1023U);
  EXPECT_EQ(H5Tget_norm(H5T_NATIVE_DOUBLE), H5T_NORM_IMPLIED);
  {
    size_t spos = 0, epos = 0, esize = 0, mpos = 0, msize = 0;
    ASSERT_GE(H5Tget_fields(H5T_NATIVE_DOUBLE, &spos, &epos, &esize, &mpos,
                            &msize),
              0);
    EXPECT_EQ(spos, 63U);
    EXPECT_EQ(epos, 52U);
    EXPECT_EQ(esize, 11U);
    EXPECT_EQ(mpos, 0U);
    EXPECT_EQ(msize, 52U);
  }
  EXPECT_EQ(H5T_NATIVE_DOUBLE_ALIGN_g, alignof(double));

  // long double: 80-bit extended on x86_64 or IEEE binary128 on aarch64.
  EXPECT_EQ(H5Tget_class(H5T_NATIVE_LDOUBLE), H5T_FLOAT);
  EXPECT_EQ(H5Tget_size(H5T_NATIVE_LDOUBLE), sizeof(long double));
  EXPECT_EQ(H5Tget_ebias(H5T_NATIVE_LDOUBLE), 16383U);
  EXPECT_EQ(H5T_NATIVE_LDOUBLE_ALIGN_g, alignof(long double));
  EXPECT_EQ(H5T_native_order_g, H5Tget_order(H5T_NATIVE_LDOUBLE));
#if defined(__x86_64__)
  EXPECT_EQ(H5Tget_precision(H5T_NATIVE_LDOUBLE), 80U);
  EXPECT_EQ(H5Tget_norm(H5T_NATIVE_LDOUBLE), H5T_NORM_NONE);
#elif defined(__aarch64__)
  EXPECT_EQ(H5Tget_precision(H5T_NATIVE_LDOUBLE), 128U);
  EXPECT_EQ(H5Tget_norm(H5T_NATIVE_LDOUBLE), H5T_NORM_IMPLIED);
#endif

#ifdef H5_HAVE__FLOAT16
  // _Float16: IEEE 754 binary16
  EXPECT_EQ(H5Tget_class(H5T_NATIVE_FLOAT16), H5T_FLOAT);
  EXPECT_EQ(H5Tget_size(H5T_NATIVE_FLOAT16), 2U);
  EXPECT_EQ(H5Tget_precision(H5T_NATIVE_FLOAT16), 16U);
  EXPECT_EQ(H5Tget_offset(H5T_NATIVE_FLOAT16), 0);
  EXPECT_EQ(H5Tget_ebias(H5T_NATIVE_FLOAT16), 15U);
  EXPECT_EQ(H5Tget_norm(H5T_NATIVE_FLOAT16), H5T_NORM_IMPLIED);
  EXPECT_EQ(H5T_NATIVE_FLOAT16_ALIGN_g, 2U);
#endif
}

}  // namespace
}  // namespace hdf5
