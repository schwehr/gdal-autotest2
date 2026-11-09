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
// Tests querying and setting string character sets via H5Tget_cset and
// H5Tset_cset, including fixed-length strings, variable-length strings,
// parent-deferring composite types, read-only/committed/locked rejection, and
// invalid arguments (third_party/hdf5/src/H5Tcset.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tcset.c

#include "third_party/hdf5/src/hdf5.h"

#include <array>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

TEST(H5TcsetTest, InvalidTypeIdsReturnError) {
  ScopedErrorSilence silence;

  EXPECT_EQ(H5Tget_cset(H5I_INVALID_HID), H5T_CSET_ERROR);
  EXPECT_LT(H5Tset_cset(H5I_INVALID_HID, H5T_CSET_ASCII), 0);

  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_GE(space, 0);
  EXPECT_EQ(H5Tget_cset(space), H5T_CSET_ERROR);
  EXPECT_LT(H5Tset_cset(space, H5T_CSET_UTF8), 0);
}

TEST(H5TcsetTest, NonStringAndDerivedNonStringTypesFail) {
  ScopedErrorSilence silence;

  ScopedType int_copy(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_GE(int_copy, 0);
  EXPECT_EQ(H5Tget_cset(int_copy), H5T_CSET_ERROR);
  EXPECT_LT(H5Tset_cset(int_copy, H5T_CSET_ASCII), 0);

  // Derived array type whose parent chain terminates at an integer must fail
  // after walking dt->shared->parent.
  constexpr std::array<hsize_t, 1> kDims = {3};
  ScopedType int_array(H5Tarray_create2(int_copy, 1, kDims.data()));
  ASSERT_GE(int_array, 0);
  EXPECT_EQ(H5Tget_cset(int_array), H5T_CSET_ERROR);
  EXPECT_LT(H5Tset_cset(int_array, H5T_CSET_UTF8), 0);

  ScopedType compound(H5Tcreate(H5T_COMPOUND, sizeof(int)));
  ASSERT_GE(compound, 0);
  ASSERT_GE(H5Tinsert(compound, "field", 0, int_copy), 0);
  EXPECT_EQ(H5Tget_cset(compound), H5T_CSET_ERROR);
  EXPECT_LT(H5Tset_cset(compound, H5T_CSET_ASCII), 0);
}

TEST(H5TcsetTest, IllegalCharsetValuesAreRejected) {
  ScopedType str(H5Tcopy(H5T_C_S1));
  ASSERT_GE(str, 0);
  ASSERT_GE(H5Tset_size(str, 8), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Tset_cset(str, H5T_CSET_ERROR), 0);
  EXPECT_LT(H5Tset_cset(str, H5T_NCSET), 0);
  EXPECT_LT(H5Tset_cset(str, static_cast<H5T_cset_t>(H5T_CSET_RESERVED_2)), 0);
  EXPECT_LT(H5Tset_cset(str, static_cast<H5T_cset_t>(99)), 0);
  EXPECT_EQ(H5Tget_cset(str), H5T_CSET_ASCII);
}

TEST(H5TcsetTest, ReadOnlyLockedAndCommittedStringsRejectSet) {
  // Immutable predefined strings can be queried, not modified.
  EXPECT_EQ(H5Tget_cset(H5T_C_S1), H5T_CSET_ASCII);
  EXPECT_EQ(H5Tget_cset(H5T_FORTRAN_S1), H5T_CSET_ASCII);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tset_cset(H5T_C_S1, H5T_CSET_UTF8), 0);
    EXPECT_LT(H5Tset_cset(H5T_FORTRAN_S1, H5T_CSET_UTF8), 0);
  }

  // Locked transient string transitions away from H5T_STATE_TRANSIENT.
  ScopedType locked_str(H5Tcopy(H5T_C_S1));
  ASSERT_GE(locked_str, 0);
  ASSERT_GE(H5Tset_size(locked_str, 12), 0);
  ASSERT_GE(H5Tlock(locked_str), 0);
  EXPECT_EQ(H5Tget_cset(locked_str), H5T_CSET_ASCII);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tset_cset(locked_str, H5T_CSET_UTF8), 0);
  }

  // Committed named datatype preserves its cset when reopened. (Note: under
  // HDF5's VOL architecture, H5Tcommit2 commits a copy on dt->vol_obj and
  // H5Topen2 constructs a decoded transient handle, both retaining UTF-8.)
  ScopedFile file = CreateInMemoryFile("cset_committed.h5");
  ASSERT_GE(file, 0);
  ScopedType committed_str(H5Tcopy(H5T_C_S1));
  ASSERT_GE(committed_str, 0);
  ASSERT_GE(H5Tset_size(committed_str, 16), 0);
  ASSERT_GE(H5Tset_cset(committed_str, H5T_CSET_UTF8), 0);
  ASSERT_GE(
      H5Tcommit2(file, "utf8_type", committed_str, H5P_DEFAULT, H5P_DEFAULT,
                 H5P_DEFAULT),
      0);

  ScopedType reopened(H5Topen2(file, "utf8_type", H5P_DEFAULT));
  ASSERT_GE(reopened, 0);
  EXPECT_EQ(H5Tget_cset(reopened), H5T_CSET_UTF8);
}

TEST(H5TcsetTest, FixedStringGetAndSetRoundTrip) {
  ScopedType str(H5Tcopy(H5T_C_S1));
  ASSERT_GE(str, 0);
  ASSERT_GE(H5Tset_size(str, 32), 0);

  EXPECT_EQ(H5Tget_cset(str), H5T_CSET_ASCII);
  EXPECT_GE(H5Tset_cset(str, H5T_CSET_UTF8), 0);
  EXPECT_EQ(H5Tget_cset(str), H5T_CSET_UTF8);
  EXPECT_GE(H5Tset_cset(str, H5T_CSET_ASCII), 0);
  EXPECT_EQ(H5Tget_cset(str), H5T_CSET_ASCII);
}

TEST(H5TcsetTest, VariableLengthStringGetAndSetRoundTrip) {
  ScopedType vlen_str(H5Tcopy(H5T_C_S1));
  ASSERT_GE(vlen_str, 0);
  ASSERT_GE(H5Tset_size(vlen_str, H5T_VARIABLE), 0);
  ASSERT_GT(H5Tis_variable_str(vlen_str), 0);

  EXPECT_EQ(H5Tget_cset(vlen_str), H5T_CSET_ASCII);
  EXPECT_GE(H5Tset_cset(vlen_str, H5T_CSET_UTF8), 0);
  EXPECT_EQ(H5Tget_cset(vlen_str), H5T_CSET_UTF8);
  EXPECT_GE(H5Tset_cset(vlen_str, H5T_CSET_ASCII), 0);
  EXPECT_EQ(H5Tget_cset(vlen_str), H5T_CSET_ASCII);
}

TEST(H5TcsetTest, CompositeTypesDeferToParentString) {
  // Fixed string wrapped in two levels of array types.
  ScopedType fixed_str(H5Tcopy(H5T_C_S1));
  ASSERT_GE(fixed_str, 0);
  ASSERT_GE(H5Tset_size(fixed_str, 8), 0);

  constexpr std::array<hsize_t, 1> kInnerDims = {2};
  ScopedType inner_arr(H5Tarray_create2(fixed_str, 1, kInnerDims.data()));
  ASSERT_GE(inner_arr, 0);

  constexpr std::array<hsize_t, 1> kOuterDims = {3};
  ScopedType outer_arr(H5Tarray_create2(inner_arr, 1, kOuterDims.data()));
  ASSERT_GE(outer_arr, 0);

  EXPECT_EQ(H5Tget_cset(outer_arr), H5T_CSET_ASCII);
  EXPECT_GE(H5Tset_cset(outer_arr, H5T_CSET_UTF8), 0);
  EXPECT_EQ(H5Tget_cset(outer_arr), H5T_CSET_UTF8);
  // H5Tarray_create2 copies its base type, so setting outer_arr updates its
  // internal parent chain without mutating the caller's original handles.
  ScopedType outer_super(H5Tget_super(outer_arr));
  ASSERT_GE(outer_super, 0);
  EXPECT_EQ(H5Tget_cset(outer_super), H5T_CSET_UTF8);
  EXPECT_EQ(H5Tget_cset(inner_arr), H5T_CSET_ASCII);
  EXPECT_EQ(H5Tget_cset(fixed_str), H5T_CSET_ASCII);

  // VL string wrapped inside a VL sequence.
  ScopedType vl_str(H5Tcopy(H5T_C_S1));
  ASSERT_GE(vl_str, 0);
  ASSERT_GE(H5Tset_size(vl_str, H5T_VARIABLE), 0);

  ScopedType vl_seq_of_str(H5Tvlen_create(vl_str));
  ASSERT_GE(vl_seq_of_str, 0);
  EXPECT_EQ(H5Tget_cset(vl_seq_of_str), H5T_CSET_ASCII);
  EXPECT_GE(H5Tset_cset(vl_seq_of_str, H5T_CSET_UTF8), 0);
  EXPECT_EQ(H5Tget_cset(vl_seq_of_str), H5T_CSET_UTF8);
  ScopedType vl_super(H5Tget_super(vl_seq_of_str));
  ASSERT_GE(vl_super, 0);
  EXPECT_EQ(H5Tget_cset(vl_super), H5T_CSET_UTF8);
  EXPECT_EQ(H5Tget_cset(vl_str), H5T_CSET_ASCII);
}

}  // namespace
}  // namespace hdf5
