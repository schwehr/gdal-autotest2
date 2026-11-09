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
// Tests array datatype creation, dimensionality/extent queries, force-conversion
// propagation, nested arrays, serialization, deprecated v1 wrappers, and
// in-memory Core VFD persistence (third_party/hdf5/src/H5Tarray.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tarray.c

#include <array>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

TEST(H5TarrayTest, Create2AndQueryDimensions) {
  const std::array<hsize_t, 3> expected_dims = {2, 3, 4};
  ScopedType array_type(H5Tarray_create2(H5T_NATIVE_INT32, 3, expected_dims.data()));
  ASSERT_TRUE(array_type.valid());

  EXPECT_EQ(H5Tget_class(array_type.get()), H5T_ARRAY);
  EXPECT_EQ(H5Tget_size(array_type.get()), 24 * sizeof(int32_t));
  EXPECT_EQ(H5Tget_array_ndims(array_type.get()), 3);

  std::array<hsize_t, 3> actual_dims{};
  EXPECT_EQ(H5Tget_array_dims2(array_type.get(), actual_dims.data()), 3);
  EXPECT_THAT(actual_dims, ElementsAre(2, 3, 4));

  // Querying with nullptr dims still returns the rank.
  EXPECT_EQ(H5Tget_array_dims2(array_type.get(), nullptr), 3);

  ScopedType super_type(H5Tget_super(array_type.get()));
  ASSERT_TRUE(super_type.valid());
  EXPECT_GT(H5Tequal(super_type.get(), H5T_NATIVE_INT32), 0);
}

TEST(H5TarrayTest, Create2SingleAndMaxRank) {
  const hsize_t single_dim = 5;
  ScopedType vec_type(H5Tarray_create2(H5T_NATIVE_DOUBLE, 1, &single_dim));
  ASSERT_TRUE(vec_type.valid());
  EXPECT_EQ(H5Tget_array_ndims(vec_type.get()), 1);
  hsize_t out_dim = 0;
  EXPECT_EQ(H5Tget_array_dims2(vec_type.get(), &out_dim), 1);
  EXPECT_EQ(out_dim, 5);

  std::array<hsize_t, H5S_MAX_RANK> max_dims;
  max_dims.fill(1);
  max_dims[0] = 2;
  max_dims[H5S_MAX_RANK - 1] = 3;
  ScopedType max_rank_type(
      H5Tarray_create2(H5T_NATIVE_UCHAR, H5S_MAX_RANK, max_dims.data()));
  ASSERT_TRUE(max_rank_type.valid());
  EXPECT_EQ(H5Tget_array_ndims(max_rank_type.get()), H5S_MAX_RANK);
  EXPECT_EQ(H5Tget_size(max_rank_type.get()), 6);

  std::array<hsize_t, H5S_MAX_RANK> got_dims{};
  EXPECT_EQ(H5Tget_array_dims2(max_rank_type.get(), got_dims.data()),
            H5S_MAX_RANK);
  EXPECT_EQ(got_dims, max_dims);
}

TEST(H5TarrayTest, NestedAndCompositeBaseTypes) {
  const std::array<hsize_t, 2> inner_dims = {2, 2};
  ScopedType inner(H5Tarray_create2(H5T_NATIVE_INT16, 2, inner_dims.data()));
  ASSERT_TRUE(inner.valid());

  const hsize_t outer_dim = 3;
  ScopedType outer(H5Tarray_create2(inner.get(), 1, &outer_dim));
  ASSERT_TRUE(outer.valid());
  EXPECT_EQ(H5Tget_size(outer.get()), 12 * sizeof(int16_t));
  EXPECT_EQ(H5Tget_array_ndims(outer.get()), 1);

  // VLEN and reference base types propagate force_conv == true.
  ScopedType vlen_base(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vlen_base.valid());
  const hsize_t vlen_arr_dim = 2;
  ScopedType vlen_arr(H5Tarray_create2(vlen_base.get(), 1, &vlen_arr_dim));
  ASSERT_TRUE(vlen_arr.valid());
  EXPECT_EQ(H5Tget_size(vlen_arr.get()), 2 * sizeof(hvl_t));

  ScopedType ref_arr(H5Tarray_create2(H5T_STD_REF, 1, &vlen_arr_dim));
  ASSERT_TRUE(ref_arr.valid());

  // Compound base containing an array member.
  ScopedType comp(H5Tcreate(H5T_COMPOUND, sizeof(int32_t) * 5));
  ASSERT_TRUE(comp.valid());
  ASSERT_GE(H5Tinsert(comp.get(), "arr", 0, inner.get()), 0);
  ASSERT_GE(H5Tinsert(comp.get(), "tag", sizeof(int32_t) * 4, H5T_NATIVE_INT32), 0);
  const hsize_t comp_dim = 2;
  ScopedType comp_arr(H5Tarray_create2(comp.get(), 1, &comp_dim));
  ASSERT_TRUE(comp_arr.valid());
  EXPECT_EQ(H5Tget_size(comp_arr.get()), 2 * H5Tget_size(comp.get()));
}

TEST(H5TarrayTest, DatasetAndCommittedRoundtripInMemory) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  const std::array<hsize_t, 2> arr_dims = {2, 3};
  ScopedType arr_type(H5Tarray_create2(H5T_NATIVE_INT32, 2, arr_dims.data()));
  ASSERT_TRUE(arr_type.valid());

  // Commit the array type and reopen it.
  ASSERT_GE(H5Tcommit2(file.get(), "/mat2x3", arr_type.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  ScopedType reopened(H5Topen2(file.get(), "/mat2x3", H5P_DEFAULT));
  ASSERT_TRUE(reopened.valid());
  std::array<hsize_t, 2> got_dims{};
  EXPECT_EQ(H5Tget_array_dims2(reopened.get(), got_dims.data()), 2);
  EXPECT_THAT(got_dims, ElementsAre(2, 3));

  // Dataset write/read roundtrip with 2D array of ints.
  const hsize_t dset_dim = 4;
  ScopedSpace space(H5Screate_simple(1, &dset_dim, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "/dset", reopened.get(), space.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  std::vector<int32_t> write_buf(4 * 2 * 3);
  std::iota(write_buf.begin(), write_buf.end(), 100);
  ASSERT_GE(H5Dwrite(dset.get(), arr_type.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     write_buf.data()),
            0);

  std::vector<int32_t> read_buf(write_buf.size(), -1);
  ASSERT_GE(H5Dread(dset.get(), arr_type.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    read_buf.data()),
            0);
  EXPECT_EQ(read_buf, write_buf);

  // Dataset write/read with array of VLEN ints (exercises force_conv).
  ScopedType vlen_int(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vlen_int.valid());
  const hsize_t vl_arr_dim = 2;
  ScopedType vl_arr(H5Tarray_create2(vlen_int.get(), 1, &vl_arr_dim));
  ASSERT_TRUE(vl_arr.valid());
  const hsize_t scalar_dim = 2;
  ScopedSpace vl_space(H5Screate_simple(1, &scalar_dim, nullptr));
  ASSERT_TRUE(vl_space.valid());
  ScopedDataset vl_dset(H5Dcreate2(file.get(), "/vl_dset", vl_arr.get(),
                                   vl_space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT));
  ASSERT_TRUE(vl_dset.valid());

  int32_t seq0[] = {1, 2};
  int32_t seq1[] = {3, 4, 5};
  int32_t seq2[] = {6};
  int32_t seq3[] = {7, 8, 9, 10};
  std::array<hvl_t, 4> vl_write = {
      hvl_t{2, seq0},
      hvl_t{3, seq1},
      hvl_t{1, seq2},
      hvl_t{4, seq3},
  };
  ASSERT_GE(H5Dwrite(vl_dset.get(), vl_arr.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     vl_write.data()),
            0);

  std::array<hvl_t, 4> vl_read{};
  auto reclaim = absl::MakeCleanup([&]() {
    H5Treclaim(vl_arr.get(), vl_space.get(), H5P_DEFAULT, vl_read.data());
  });
  ASSERT_GE(H5Dread(vl_dset.get(), vl_arr.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    vl_read.data()),
            0);
  EXPECT_EQ(vl_read[0].len, 2);
  EXPECT_EQ(vl_read[1].len, 3);
  EXPECT_EQ(vl_read[2].len, 1);
  EXPECT_EQ(vl_read[3].len, 4);
  EXPECT_EQ(static_cast<const int32_t*>(vl_read[1].p)[2], 5);

  // Encode / decode roundtrip.
  size_t encoded_size = 0;
  ASSERT_GE(H5Tencode(arr_type.get(), nullptr, &encoded_size), 0);
  std::vector<unsigned char> encoded(encoded_size);
  ASSERT_GE(H5Tencode(arr_type.get(), encoded.data(), &encoded_size), 0);
  ScopedType decoded(H5Tdecode(encoded.data(), encoded_size));
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Tequal(decoded.get(), arr_type.get()), 0);
}

TEST(H5TarrayTest, Create2AndQueryErrorCases) {
  ScopedErrorSilence silence;

  const hsize_t good_dim = 4;
  EXPECT_LT(H5Tarray_create2(H5T_NATIVE_INT, 0, &good_dim), 0);
  EXPECT_LT(H5Tarray_create2(H5T_NATIVE_INT, H5S_MAX_RANK + 1, &good_dim), 0);
  EXPECT_LT(H5Tarray_create2(H5T_NATIVE_INT, 1, nullptr), 0);

  const std::array<hsize_t, 3> zero_first = {0, 2, 3};
  EXPECT_LT(H5Tarray_create2(H5T_NATIVE_INT, 3, zero_first.data()), 0);
  const std::array<hsize_t, 3> zero_mid = {2, 0, 3};
  EXPECT_LT(H5Tarray_create2(H5T_NATIVE_INT, 3, zero_mid.data()), 0);

  EXPECT_LT(H5Tarray_create2(H5I_INVALID_HID, 1, &good_dim), 0);
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  EXPECT_LT(H5Tarray_create2(space.get(), 1, &good_dim), 0);

  // H5Tget_array_ndims error cases.
  EXPECT_LT(H5Tget_array_ndims(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Tget_array_ndims(space.get()), 0);
  EXPECT_LT(H5Tget_array_ndims(H5T_NATIVE_INT), 0);

  // H5Tget_array_dims2 error cases.
  hsize_t out_dim = 0;
  EXPECT_LT(H5Tget_array_dims2(H5I_INVALID_HID, &out_dim), 0);
  EXPECT_LT(H5Tget_array_dims2(space.get(), &out_dim), 0);
  EXPECT_LT(H5Tget_array_dims2(H5T_NATIVE_INT, &out_dim), 0);
}

#ifndef H5_NO_DEPRECATED_SYMBOLS
TEST(H5TarrayTest, DeprecatedCreate1AndGetDims1) {
  const std::array<hsize_t, 2> dims = {3, 5};
  const std::array<int, 2> perm = {1, 0};
  ScopedType arr_with_perm(H5Tarray_create1(H5T_NATIVE_FLOAT, 2, dims.data(),
                                            perm.data()));
  ASSERT_TRUE(arr_with_perm.valid());

  ScopedType arr_null_perm(
      H5Tarray_create1(H5T_NATIVE_FLOAT, 2, dims.data(), nullptr));
  ASSERT_TRUE(arr_null_perm.valid());
  EXPECT_GT(H5Tequal(arr_with_perm.get(), arr_null_perm.get()), 0);

  std::array<hsize_t, 2> out_dims{};
  std::array<int, 2> out_perm{};
  EXPECT_EQ(H5Tget_array_dims1(arr_with_perm.get(), out_dims.data(),
                               out_perm.data()),
            2);
  EXPECT_THAT(out_dims, ElementsAre(3, 5));
  EXPECT_EQ(H5Tget_array_dims1(arr_with_perm.get(), nullptr, nullptr), 2);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Tarray_create1(H5T_NATIVE_INT, -1, dims.data(), nullptr), 0);
  EXPECT_LT(H5Tarray_create1(H5T_NATIVE_INT, 0, dims.data(), nullptr), 0);
  EXPECT_LT(H5Tarray_create1(H5T_NATIVE_INT, H5S_MAX_RANK + 1, dims.data(),
                             nullptr),
            0);
  EXPECT_LT(H5Tarray_create1(H5T_NATIVE_INT, 1, nullptr, nullptr), 0);

  const std::array<hsize_t, 2> bad_dims = {2, 0};
  EXPECT_LT(H5Tarray_create1(H5T_NATIVE_INT, 2, bad_dims.data(), nullptr), 0);
  EXPECT_LT(H5Tarray_create1(H5I_INVALID_HID, 2, dims.data(), nullptr), 0);

  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  EXPECT_LT(H5Tarray_create1(space.get(), 2, dims.data(), nullptr), 0);

  EXPECT_LT(H5Tget_array_dims1(H5I_INVALID_HID, out_dims.data(), nullptr), 0);
  EXPECT_LT(H5Tget_array_dims1(space.get(), out_dims.data(), nullptr), 0);
  EXPECT_LT(H5Tget_array_dims1(H5T_NATIVE_DOUBLE, out_dims.data(), nullptr), 0);
}
#endif  // H5_NO_DEPRECATED_SYMBOLS

}  // namespace
}  // namespace hdf5
