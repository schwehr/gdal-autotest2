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
// Tests complex number datatype creation, base-type validation, serialization,
// in-memory persistence, and internal size-boundary guards
// (third_party/hdf5/src/H5Tcomplex.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tcomplex.c

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5TCOMPLEX_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5T_FRIEND
#if defined(H5_HAVE__FLOAT16) && defined(__cplusplus)
using H5__Float16 = uint16_t;
#endif
#include "third_party/hdf5/src/H5Tpkg.h"
}  // extern "C"
#ifdef H5TCOMPLEX_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TCOMPLEX_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

TEST(H5TcomplexTest, CreateFromFloatBaseTypes) {
  ScopedType cfloat(H5Tcomplex_create(H5T_NATIVE_FLOAT));
  ASSERT_TRUE(cfloat.valid());
  EXPECT_EQ(H5Tget_class(cfloat.get()), H5T_COMPLEX);
  EXPECT_EQ(H5Tget_size(cfloat.get()), 2 * H5Tget_size(H5T_NATIVE_FLOAT));
  ScopedType cfloat_super(H5Tget_super(cfloat.get()));
  ASSERT_TRUE(cfloat_super.valid());
  EXPECT_GT(H5Tequal(cfloat_super.get(), H5T_NATIVE_FLOAT), 0);

  ScopedType cdouble(H5Tcomplex_create(H5T_NATIVE_DOUBLE));
  ASSERT_TRUE(cdouble.valid());
  EXPECT_EQ(H5Tget_size(cdouble.get()), 2 * sizeof(double));
  EXPECT_GT(H5Tequal(cdouble.get(), H5T_NATIVE_DOUBLE_COMPLEX), 0);

  ScopedType cldouble(H5Tcomplex_create(H5T_NATIVE_LDOUBLE));
  ASSERT_TRUE(cldouble.valid());
  EXPECT_EQ(H5Tget_size(cldouble.get()), 2 * sizeof(long double));

  ScopedType cbe32(H5Tcomplex_create(H5T_IEEE_F32BE));
  ASSERT_TRUE(cbe32.valid());
  EXPECT_EQ(H5Tget_size(cbe32.get()), 8);
  EXPECT_EQ(H5Tget_order(cbe32.get()), H5T_ORDER_BE);

#ifdef H5_HAVE__FLOAT16
  ScopedType cf16(H5Tcomplex_create(H5T_NATIVE_FLOAT16));
  ASSERT_TRUE(cf16.valid());
  EXPECT_EQ(H5Tget_size(cf16.get()), 2 * H5Tget_size(H5T_NATIVE_FLOAT16));
#endif
}

TEST(H5TcomplexTest, PersistenceAndSerializationInMemory) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedType cdouble(H5Tcomplex_create(H5T_NATIVE_DOUBLE));
  ASSERT_TRUE(cdouble.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "/cplx_d", cdouble.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);

  ScopedType reopened(H5Topen2(file.get(), "/cplx_d", H5P_DEFAULT));
  ASSERT_TRUE(reopened.valid());
  EXPECT_EQ(H5Tget_class(reopened.get()), H5T_COMPLEX);
  EXPECT_GT(H5Tequal(reopened.get(), cdouble.get()), 0);

  // Write and read a complex<double> dataset using a packed 2-double element layout.
  const hsize_t count = 2;
  ScopedSpace space(H5Screate_simple(1, &count, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "/cplx_data", reopened.get(),
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  const std::array<double, 4> write_vals = {1.5, -2.25, 3.0, 4.75};
  ASSERT_GE(H5Dwrite(dset.get(), cdouble.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     write_vals.data()),
            0);

  std::array<double, 4> read_vals{};
  ASSERT_GE(H5Dread(dset.get(), cdouble.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    read_vals.data()),
            0);
  EXPECT_EQ(read_vals, write_vals);

  // Also check long double zero-initialized padding before any conversion.
  ScopedType cldouble(H5Tcomplex_create(H5T_NATIVE_LDOUBLE));
  ASSERT_TRUE(cldouble.valid());
  std::array<long double, 4> ld_buf;
  std::memset(ld_buf.data(), 0, ld_buf.size() * sizeof(long double));
  ld_buf[0] = 2.0L;
  ld_buf[1] = -3.0L;
  ld_buf[2] = 4.0L;
  ld_buf[3] = 5.0L;
  ASSERT_GE(H5Tconvert(cldouble.get(), cdouble.get(), 2, ld_buf.data(), nullptr,
                       H5P_DEFAULT),
            0);

  // Encode / decode roundtrip.
  size_t enc_size = 0;
  ASSERT_GE(H5Tencode(cdouble.get(), nullptr, &enc_size), 0);
  std::vector<unsigned char> enc(enc_size);
  ASSERT_GE(H5Tencode(cdouble.get(), enc.data(), &enc_size), 0);
  ScopedType decoded(H5Tdecode(enc.data(), enc_size));
  ASSERT_TRUE(decoded.valid());
  EXPECT_EQ(H5Tget_class(decoded.get()), H5T_COMPLEX);
  EXPECT_GT(H5Tequal(decoded.get(), cdouble.get()), 0);
}

TEST(H5TcomplexTest, PublicErrorCases) {
  ScopedErrorSilence silence;

  EXPECT_EQ(H5Tcomplex_create(H5I_INVALID_HID), H5I_INVALID_HID);

  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  EXPECT_EQ(H5Tcomplex_create(space.get()), H5I_INVALID_HID);

  // Non-floating-point datatypes must be rejected.
  EXPECT_EQ(H5Tcomplex_create(H5T_NATIVE_INT), H5I_INVALID_HID);

  ScopedType str_type(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(str_type.valid());
  EXPECT_EQ(H5Tcomplex_create(str_type.get()), H5I_INVALID_HID);

  ScopedType comp_type(H5Tcreate(H5T_COMPOUND, sizeof(float) * 2));
  ASSERT_TRUE(comp_type.valid());
  ASSERT_GE(H5Tinsert(comp_type.get(), "re", 0, H5T_NATIVE_FLOAT), 0);
  ASSERT_GE(H5Tinsert(comp_type.get(), "im", sizeof(float), H5T_NATIVE_FLOAT), 0);
  EXPECT_EQ(H5Tcomplex_create(comp_type.get()), H5I_INVALID_HID);
}

TEST(H5TcomplexTest, InternalInvalidFloatSizes) {
  ASSERT_GE(H5open(), 0);
  ScopedErrorSilence silence;

  H5T_t zero_base{};
  H5T_shared_t zero_shared{};
  zero_shared.type = H5T_FLOAT;
  zero_shared.size = 0;
  zero_base.shared = &zero_shared;
  EXPECT_EQ(H5T__complex_create(&zero_base), nullptr);

  H5T_t huge_base{};
  H5T_shared_t huge_shared{};
  huge_shared.type = H5T_FLOAT;
  huge_shared.size = (std::numeric_limits<size_t>::max() / 2) + 1;
  huge_base.shared = &huge_shared;
  EXPECT_EQ(H5T__complex_create(&huge_base), nullptr);

  // Force H5T_copy(base, H5T_COPY_ALL) inside H5T__complex_create to fail via
  // a parent datatype whose zero-allocated compound member has a nullptr name
  // (avoids any heap allocations before failure so zero memory leaks).
  H5T_cmemb_t bad_memb{};
  bad_memb.name = nullptr;

  H5T_t bad_parent{};
  H5T_shared_t bad_parent_shared{};
  bad_parent_shared.type = H5T_COMPOUND;
  bad_parent_shared.u.compnd.nalloc = 0;
  bad_parent_shared.u.compnd.nmembs = 1;
  bad_parent_shared.u.compnd.memb = &bad_memb;
  bad_parent.shared = &bad_parent_shared;

  H5T_t bad_base{};
  H5T_shared_t bad_base_shared{};
  bad_base_shared.type = H5T_FLOAT;
  bad_base_shared.size = sizeof(float);
  bad_base_shared.parent = &bad_parent;
  bad_base.shared = &bad_base_shared;
  EXPECT_EQ(H5T__complex_create(&bad_base), nullptr);
}

}  // namespace
}  // namespace hdf5
