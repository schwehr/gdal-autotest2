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
// Tests CODA HDF5 cursor navigation, metadata queries, scalar reads, array
// reads, partial array reads, compound datatypes, enums, variable/fixed
// strings, attributes, conversions, boundary checks, and error paths
// (libcoda/coda-hdf5-cursor.c).
//
// google3 build caveats:
//   - HAVE_HDF4 is undefined in config.h (coda-hdf4* excluded); HAVE_HDF5=1.
//   - config.h defines H5_USE_16_API, so HDF5 file synthesis uses versioned
//     HDF5 1.8+ APIs (H5Gcreate2, H5Dcreate2, H5Acreate2, H5Topen2, etc.).
//   - coda-hdf5.c:146-148 leaks H5Eget_major()/H5Eget_minor() strings when
//     CODA_ERROR_HDF5 is raised; tests triggering HDF5 errors wrap those calls
//     in narrow absl::LeakCheckDisabler scopes.
//   - Upstream coda-hdf5-cursor.c:380,732 omit H5Sclose(mem_space_id) on
//     scalar variable-length string success returns.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-hdf5-cursor.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-hdf5.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-hdf5-internal.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda.h

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/debugging/leak_check.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"
#include "third_party/stcorp_coda/google/util/hdf5_test_util.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-hdf5-internal.h"
}  // extern "C"

namespace stcorp_coda {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::IsNan;

class CodaHdf5CursorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    env_def_.Unset();
    env_leap_.Unset();
    coda_set_error(CODA_SUCCESS, nullptr);
    ASSERT_EQ(CODA_SUCCESS, session_.Init());
  }

  void TearDown() override {
    session_.Reset();
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  ScopedEnvVar env_def_{"CODA_DEFINITION"};
  ScopedEnvVar env_leap_{"CODA_LEAP_SECOND_TABLE"};
  ScopedCodaSession session_;
};

coda_cursor GotoField(const coda_cursor& parent, const char* name) {
  coda_cursor child = parent;
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&child, name));
  return child;
}

// White-box helper reading internal coda_cursor stack entries (unmangled via
// CODA_INTERNAL) so error tests can inject invalid HDF5 handles into live
// coda_hdf5_dataset / coda_hdf5_basic_data_type nodes to cover internal
// H5S/H5D/H5T failure branches.
coda_dynamic_type* CursorStackTypeAt(const coda_cursor& c,
                                     int depth_from_top = 0) {
  return c.stack[c.n - 1 - depth_from_top].type;
}

template <typename T>
void WriteDataset(hid_t loc_id, const char* name, hid_t dtype_id,
                  std::span<const hsize_t> dims, const T* data) {
  ScopedH5Space space;
  if (dims.empty()) {
    space.Reset(H5Screate(H5S_SCALAR));
  } else {
    space.Reset(
        H5Screate_simple(static_cast<int>(dims.size()), dims.data(), nullptr));
  }
  ASSERT_TRUE(space.valid());
  const ScopedH5Dataset dset(H5Dcreate2(loc_id, name, dtype_id, space.get(),
                                        H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  ASSERT_GE(H5Dwrite(dset.get(), dtype_id, H5S_ALL, H5S_ALL, H5P_DEFAULT, data),
            0);
}

template <typename T>
void WriteAttribute(hid_t obj_id, const char* name, hid_t dtype_id,
                    std::span<const hsize_t> dims, const T* data) {
  ScopedH5Space space;
  if (dims.empty()) {
    space.Reset(H5Screate(H5S_SCALAR));
  } else {
    space.Reset(
        H5Screate_simple(static_cast<int>(dims.size()), dims.data(), nullptr));
  }
  ASSERT_TRUE(space.valid());
  const ScopedH5Attr attr(H5Acreate2(obj_id, name, dtype_id, space.get(),
                                     H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());
  ASSERT_GE(H5Awrite(attr.get(), dtype_id, data), 0);
}

TEST_F(CodaHdf5CursorTest, SetProductAndGroupNavigationAndErrors) {
  TempFile h5_file("hdf5_cursor_group_nav.h5");
  {
    const ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    constexpr std::array<hsize_t, 1> kDims2 = {2};
    constexpr std::array<int32_t, 2> kD1 = {10, 20};
    constexpr std::array<int32_t, 2> kD2 = {30, 40};
    ASSERT_NO_FATAL_FAILURE(
        WriteDataset(file.get(), "d1", H5T_NATIVE_INT32, kDims2, kD1.data()));
    ASSERT_NO_FATAL_FAILURE(
        WriteDataset(file.get(), "d2", H5T_NATIVE_INT32, kDims2, kD2.data()));

    const ScopedH5Group grp(
        H5Gcreate2(file.get(), "grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    constexpr std::array<int32_t, 2> kInnerA = {101, 102};
    constexpr std::array<int32_t, 2> kInnerB = {201, 202};
    ASSERT_NO_FATAL_FAILURE(
        WriteDataset(grp.get(), "a", H5T_NATIVE_INT32, kDims2, kInnerA.data()));
    ASSERT_NO_FATAL_FAILURE(
        WriteDataset(grp.get(), "b", H5T_NATIVE_INT32, kDims2, kInnerB.data()));
  }

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  coda_format format = coda_format_ascii;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_format(&root, &format));
  EXPECT_EQ(coda_format_hdf5, format);

  coda_type_class type_class = coda_special_class;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&root, &type_class));
  EXPECT_EQ(coda_record_class, type_class);

  long num_elements = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&root, &num_elements));
  EXPECT_EQ(3, num_elements);

  int depth = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_depth(&root, &depth));
  EXPECT_EQ(0, depth);

  int64_t bit_size = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&root, &bit_size));
  EXPECT_EQ(-1, bit_size);

  // Out-of-bounds group field indices return CODA_ERROR_INVALID_INDEX.
  coda_cursor c = root;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_record_field_by_index(&c, -1));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_record_field_by_index(&c, num_elements));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);

  // Step through all root group fields, then verify past-end error preserves
  // cursor position at the last field.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_record_field(&c));
  long field_idx = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&c, &field_idx));
  EXPECT_EQ(0, field_idx);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&c));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&c, &field_idx));
  EXPECT_EQ(1, field_idx);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&c));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&c, &field_idx));
  EXPECT_EQ(2, field_idx);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_next_record_field(&c));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&c, &field_idx));
  EXPECT_EQ(2, field_idx);

  // Navigate into subgroup via path and back to root.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/grp/b"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&c, 1));
  int32_t val = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c, &val));
  EXPECT_EQ(202, val);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&c));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_parent(&c));
  EXPECT_EQ(CODA_ERROR_NO_PARENT, coda_errno);
}

TEST_F(CodaHdf5CursorTest, ArrayNavigationMultiDimAndBoundaryChecks) {
  ScopedCodaOptions restore_options;
  TempFile h5_file("hdf5_cursor_array_nav.h5");
  {
    const ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    constexpr int32_t kScalar = 77;
    ASSERT_NO_FATAL_FAILURE(
        WriteDataset(file.get(), "scalar", H5T_NATIVE_INT32, {}, &kScalar));

    constexpr std::array<hsize_t, 2> kDims2x3 = {2, 3};
    constexpr std::array<int32_t, 6> kMat = {10, 11, 12, 20, 21, 22};
    ASSERT_NO_FATAL_FAILURE(WriteDataset(file.get(), "mat2x3", H5T_NATIVE_INT32,
                                         kDims2x3, kMat.data()));

    constexpr std::array<hsize_t, 3> kDims2x3x4 = {2, 3, 4};
    std::array<int32_t, 24> cube{};
    for (int i = 0; i < 24; ++i) {
      cube[i] = 100 + i;
    }
    ASSERT_NO_FATAL_FAILURE(WriteDataset(
        file.get(), "cube2x3x4", H5T_NATIVE_INT32, kDims2x3x4, cube.data()));
  }

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));
  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  // 0D scalar dataset navigation via subscript count 0 and index 0.
  coda_cursor scalar_c = GotoField(root, "scalar");
  int num_dims = -1;
  std::array<long, CODA_MAX_NUM_DIMS> dims{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_array_dim(&scalar_c, &num_dims, dims.data()));
  EXPECT_EQ(0, num_dims);
  long num_elems = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&scalar_c, &num_elems));
  EXPECT_EQ(1, num_elems);

  coda_cursor scalar_elem = scalar_c;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_array_element(&scalar_elem, 0, nullptr));
  int32_t scalar_val = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&scalar_elem, &scalar_val));
  EXPECT_EQ(77, scalar_val);

  // 2D subscript navigation and error paths.
  coda_cursor mat_c = GotoField(root, "mat2x3");
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_array_dim(&mat_c, &num_dims, dims.data()));
  EXPECT_EQ(2, num_dims);
  EXPECT_EQ(2, dims[0]);
  EXPECT_EQ(3, dims[1]);

  coda_cursor elem = mat_c;
  constexpr long kWrongSubs[1] = {0};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element(&elem, 1, kWrongSubs));
  EXPECT_EQ(CODA_ERROR_ARRAY_NUM_DIMS_MISMATCH, coda_errno);

  constexpr long kNegSub[2] = {-1, 0};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element(&elem, 2, kNegSub));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  constexpr long kHighSub[2] = {0, 3};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element(&elem, 2, kHighSub));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  constexpr long kValidSub2d[2] = {1, 2};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_array_element(&elem, 2, kValidSub2d));
  long elem_idx = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&elem, &elem_idx));
  EXPECT_EQ(5, elem_idx);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&elem, &num_elems));
  EXPECT_EQ(1, num_elems);  // tag_hdf5_basic_datatype
  int32_t mat_val = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&elem, &mat_val));
  EXPECT_EQ(22, mat_val);

  // 3D subscript navigation: (1, 2, 3) = ((1 * 3) + 2) * 4 + 3 = 23.
  coda_cursor cube_elem = GotoField(root, "cube2x3x4");
  constexpr long kValidSub3d[3] = {1, 2, 3};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_array_element(&cube_elem, 3, kValidSub3d));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&cube_elem, &elem_idx));
  EXPECT_EQ(23, elem_idx);
  int32_t cube_val = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cube_elem, &cube_val));
  EXPECT_EQ(123, cube_val);

  // Index navigation with boundary checks enabled (1) and disabled (0).
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(1));
  elem = mat_c;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(&elem, -1));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(&elem, 6));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_array_element(&elem));
  for (int i = 1; i < 6; ++i) {
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&elem));
  }
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_next_array_element(&elem));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(0));
  elem = mat_c;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&elem, 0));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&elem));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&elem, &mat_val));
  EXPECT_EQ(11, mat_val);
}

TEST_F(CodaHdf5CursorTest, ScalarArrayAndPartialArrayReadsAllNumericTypes) {
  TempFile h5_file("hdf5_cursor_numeric.h5");
  constexpr std::array<int8_t, 6> kI8 = {-3, -2, -1, 0, 1, 2};
  constexpr std::array<uint8_t, 6> kU8 = {1, 2, 3, 4, 5, 6};
  constexpr std::array<int16_t, 6> kI16 = {-30, -20, -10, 10, 20, 30};
  constexpr std::array<uint16_t, 6> kU16 = {10, 20, 30, 40, 50, 60};
  constexpr std::array<int32_t, 6> kI32 = {-300, -200, -100, 100, 200, 300};
  constexpr std::array<uint32_t, 6> kU32 = {100, 200, 300, 400, 500, 600};
  constexpr std::array<int64_t, 6> kI64 = {-3000, -2000, -1000,
                                           1000,  2000,  3000};
  constexpr std::array<uint64_t, 6> kU64 = {1000, 2000, 3000, 4000, 5000, 6000};
  constexpr std::array<float, 6> kF32 = {-1.5f, 2.5f, -3.5f, 4.5f, -5.5f, 6.5f};
  constexpr std::array<double, 6> kF64 = {-1.25, 2.25,  -3.25,
                                          4.25,  -5.25, 6.25};

  std::array<int32_t, 24> cube{};
  for (int i = 0; i < 24; ++i) {
    cube[i] = i + 1;
  }
  constexpr std::array<int32_t, 2> kI24Vals = {123456, -5};
  constexpr std::array<uint64_t, 2> kU48Vals = {0x123456789ABCULL, 42ULL};

  {
    const ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    constexpr std::array<hsize_t, 2> kDims2x3 = {2, 3};
    ASSERT_NO_FATAL_FAILURE(
        WriteDataset(file.get(), "i8", H5T_NATIVE_INT8, kDims2x3, kI8.data()));
    ASSERT_NO_FATAL_FAILURE(
        WriteDataset(file.get(), "u8", H5T_NATIVE_UINT8, kDims2x3, kU8.data()));
    ASSERT_NO_FATAL_FAILURE(WriteDataset(file.get(), "i16", H5T_NATIVE_INT16,
                                         kDims2x3, kI16.data()));
    ASSERT_NO_FATAL_FAILURE(WriteDataset(file.get(), "u16", H5T_NATIVE_UINT16,
                                         kDims2x3, kU16.data()));
    ASSERT_NO_FATAL_FAILURE(WriteDataset(file.get(), "i32", H5T_NATIVE_INT32,
                                         kDims2x3, kI32.data()));
    ASSERT_NO_FATAL_FAILURE(WriteDataset(file.get(), "u32", H5T_NATIVE_UINT32,
                                         kDims2x3, kU32.data()));
    ASSERT_NO_FATAL_FAILURE(WriteDataset(file.get(), "i64", H5T_NATIVE_INT64,
                                         kDims2x3, kI64.data()));
    ASSERT_NO_FATAL_FAILURE(WriteDataset(file.get(), "u64", H5T_NATIVE_UINT64,
                                         kDims2x3, kU64.data()));
    ASSERT_NO_FATAL_FAILURE(WriteDataset(file.get(), "f32", H5T_NATIVE_FLOAT,
                                         kDims2x3, kF32.data()));
    ASSERT_NO_FATAL_FAILURE(WriteDataset(file.get(), "f64", H5T_NATIVE_DOUBLE,
                                         kDims2x3, kF64.data()));

    constexpr std::array<hsize_t, 3> kDims2x3x4 = {2, 3, 4};
    ASSERT_NO_FATAL_FAILURE(WriteDataset(file.get(), "cube", H5T_NATIVE_INT32,
                                         kDims2x3x4, cube.data()));

    // Zero-element chunked dataset exercises `num_elements <= 0` in read_array.
    constexpr std::array<hsize_t, 1> kZeroDims = {0};
    constexpr std::array<hsize_t, 1> kMaxDims = {H5S_UNLIMITED};
    constexpr std::array<hsize_t, 1> kChunkDims = {4};
    const ScopedH5Space zero_space(
        H5Screate_simple(1, kZeroDims.data(), kMaxDims.data()));
    ASSERT_TRUE(zero_space.valid());
    const ScopedH5Plist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, kChunkDims.data()), 0);
    const ScopedH5Dataset zero_dset(
        H5Dcreate2(file.get(), "empty_i32", H5T_NATIVE_INT32, zero_space.get(),
                   H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(zero_dset.valid());

    // 3-byte signed and 6-byte unsigned integers exercise `new_size > size`
    // in read_basic_type().
    constexpr std::array<hsize_t, 1> kDims2 = {2};
    const ScopedH5Space space2(H5Screate_simple(1, kDims2.data(), nullptr));
    ASSERT_TRUE(space2.valid());

    const ScopedH5Type i24_type(H5Tcopy(H5T_STD_I32LE));
    ASSERT_TRUE(i24_type.valid());
    ASSERT_GE(H5Tset_size(i24_type.get(), 3), 0);
    const ScopedH5Dataset i24_dset(H5Dcreate2(file.get(), "i24", i24_type.get(),
                                              space2.get(), H5P_DEFAULT,
                                              H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(i24_dset.valid());
    ASSERT_GE(H5Dwrite(i24_dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, kI24Vals.data()),
              0);

    const ScopedH5Type u48_type(H5Tcopy(H5T_STD_U64LE));
    ASSERT_TRUE(u48_type.valid());
    ASSERT_GE(H5Tset_size(u48_type.get(), 6), 0);
    const ScopedH5Dataset u48_dset(H5Dcreate2(file.get(), "u48", u48_type.get(),
                                              space2.get(), H5P_DEFAULT,
                                              H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(u48_dset.valid());
    ASSERT_GE(H5Dwrite(u48_dset.get(), H5T_NATIVE_UINT64, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, kU48Vals.data()),
              0);
  }

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));
  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  auto check_2x3 = [&](const char* name, auto read_fn, auto arr_fn,
                       auto partial_fn, const auto& expected, auto sentinel) {
    SCOPED_TRACE(name);
    using T = typename std::decay_t<decltype(expected)>::value_type;
    const coda_cursor dset = GotoField(root, name);

    coda_cursor elem = dset;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&elem, 4));
    T scalar = sentinel;
    ASSERT_EQ(CODA_SUCCESS, read_fn(&elem, &scalar));
    EXPECT_EQ(expected[4], scalar);

    std::vector<T> c_order(6, sentinel);
    ASSERT_EQ(CODA_SUCCESS,
              arr_fn(&dset, c_order.data(), coda_array_ordering_c));
    EXPECT_THAT(c_order, ElementsAreArray(expected));

    std::vector<T> fortran_order(6, sentinel);
    ASSERT_EQ(CODA_SUCCESS,
              arr_fn(&dset, fortran_order.data(), coda_array_ordering_fortran));
    EXPECT_THAT(fortran_order,
                ElementsAre(expected[0], expected[3], expected[1], expected[4],
                            expected[2], expected[5]));

    // Partial read of row 1 (offset=3, length=3) exercises `while (i > 0)`.
    std::vector<T> part(3, sentinel);
    ASSERT_EQ(CODA_SUCCESS, partial_fn(&dset, 3, 3, part.data()));
    EXPECT_THAT(part, ElementsAre(expected[3], expected[4], expected[5]));
  };

  check_2x3("i8", coda_cursor_read_int8, coda_cursor_read_int8_array,
            coda_cursor_read_int8_partial_array, kI8, int8_t{-99});
  check_2x3("u8", coda_cursor_read_uint8, coda_cursor_read_uint8_array,
            coda_cursor_read_uint8_partial_array, kU8, uint8_t{99});
  check_2x3("i16", coda_cursor_read_int16, coda_cursor_read_int16_array,
            coda_cursor_read_int16_partial_array, kI16, int16_t{-999});
  check_2x3("u16", coda_cursor_read_uint16, coda_cursor_read_uint16_array,
            coda_cursor_read_uint16_partial_array, kU16, uint16_t{999});
  check_2x3("i32", coda_cursor_read_int32, coda_cursor_read_int32_array,
            coda_cursor_read_int32_partial_array, kI32, int32_t{-999});
  check_2x3("u32", coda_cursor_read_uint32, coda_cursor_read_uint32_array,
            coda_cursor_read_uint32_partial_array, kU32, uint32_t{999});
  check_2x3("i64", coda_cursor_read_int64, coda_cursor_read_int64_array,
            coda_cursor_read_int64_partial_array, kI64, int64_t{-999});
  check_2x3("u64", coda_cursor_read_uint64, coda_cursor_read_uint64_array,
            coda_cursor_read_uint64_partial_array, kU64, uint64_t{999});
  check_2x3("f32", coda_cursor_read_float, coda_cursor_read_float_array,
            coda_cursor_read_float_partial_array, kF32, -999.0f);
  check_2x3("f64", coda_cursor_read_double, coda_cursor_read_double_array,
            coda_cursor_read_double_partial_array, kF64, -999.0);

  // 3D partial reads: full plane [1,:,:] and single inner row [1,1,:].
  const coda_cursor cube_c = GotoField(root, "cube");
  std::vector<int32_t> plane(12, -1);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_partial_array(&cube_c, 12, 12,
                                                               plane.data()));
  EXPECT_THAT(plane,
              ElementsAre(13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24));

  std::vector<int32_t> row(4, -1);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_int32_partial_array(&cube_c, 16, 4, row.data()));
  EXPECT_THAT(row, ElementsAre(17, 18, 19, 20));

  // 0-element chunked dataset read returns CODA_SUCCESS without touching dst.
  const coda_cursor empty_c = GotoField(root, "empty_i32");
  int32_t sentinel = -12345;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(&empty_c, &sentinel,
                                                       coda_array_ordering_c));
  EXPECT_EQ(-12345, sentinel);

  // 3-byte signed and 6-byte unsigned scalar reads.
  const coda_cursor i24_c = GotoField(root, "i24");
  for (long i = 0; i < 2; ++i) {
    coda_cursor elem = i24_c;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&elem, i));
    int32_t v = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&elem, &v));
    EXPECT_EQ(kI24Vals[i], v);
  }

  const coda_cursor u48_c = GotoField(root, "u48");
  for (long i = 0; i < 2; ++i) {
    coda_cursor elem = u48_c;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&elem, i));
    uint64_t v = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint64(&elem, &v));
    EXPECT_EQ(kU48Vals[i], v);
  }
}

TEST_F(CodaHdf5CursorTest, PartialArrayErrorPaths) {
  TempFile h5_file("hdf5_cursor_partial_errors.h5");
  {
    const ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    constexpr int32_t kScalar = 5;
    ASSERT_NO_FATAL_FAILURE(
        WriteDataset(file.get(), "scalar", H5T_NATIVE_INT32, {}, &kScalar));

    constexpr std::array<hsize_t, 2> kDims2x2 = {2, 2};
    constexpr std::array<int32_t, 4> kMat2x2 = {1, 2, 3, 4};
    ASSERT_NO_FATAL_FAILURE(WriteDataset(file.get(), "mat2x2", H5T_NATIVE_INT32,
                                         kDims2x2, kMat2x2.data()));

    constexpr std::array<hsize_t, 2> kDims3x3 = {3, 3};
    constexpr std::array<int32_t, 9> kMat3x3 = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    ASSERT_NO_FATAL_FAILURE(WriteDataset(file.get(), "mat3x3", H5T_NATIVE_INT32,
                                         kDims3x3, kMat3x3.data()));
  }

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));
  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  std::array<int32_t, 9> buf{};

  // 0D scalar dataset rejects partial array reads.
  const coda_cursor scalar_c = GotoField(root, "scalar");
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_cursor_read_int32_partial_array(&scalar_c, 0, 1, buf.data()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // 3x3: length not a multiple of subdimension size (length=4, block_size=3).
  const coda_cursor mat3x3_c = GotoField(root, "mat3x3");
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_cursor_read_int32_partial_array(&mat3x3_c, 0, 4, buf.data()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // 3x3: offset not a multiple of subdimension size (offset=1, length=6).
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_cursor_read_int32_partial_array(&mat3x3_c, 1, 6, buf.data()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // 2x2: start[i] + count[i] > dim[i] (offset=1, length=2 crosses row end).
  const coda_cursor mat2x2_c = GotoField(root, "mat2x2");
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_cursor_read_int32_partial_array(&mat2x2_c, 1, 2, buf.data()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // Out-of-bounds offset/length rejected by
  // coda_cursor_read_int32_partial_array.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_cursor_read_int32_partial_array(&mat2x2_c, -1, 1, buf.data()));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_cursor_read_int32_partial_array(&mat2x2_c, 2, 3, buf.data()));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
}

TEST_F(CodaHdf5CursorTest, StringDatasetsFixedAndVariableLength) {
  TempFile h5_file("hdf5_cursor_strings.h5");
  {
    const ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    // Fixed-length string 1D dataset (size=8).
    const ScopedH5Type fixed_str(H5Tcopy(H5T_C_S1));
    ASSERT_TRUE(fixed_str.valid());
    ASSERT_GE(H5Tset_size(fixed_str.get(), 8), 0);
    ASSERT_GE(H5Tset_strpad(fixed_str.get(), H5T_STR_NULLTERM), 0);
    constexpr std::array<hsize_t, 1> kDims2 = {2};
    const char fixed_vals[2][8] = {"alpha\0\0", "beta123"};
    ASSERT_NO_FATAL_FAILURE(WriteDataset(file.get(), "fixed_1d",
                                         fixed_str.get(), kDims2, fixed_vals));

    // Fixed-length string scalar (0D) dataset.
    const char fixed_scalar[8] = "fix_0d\0";
    ASSERT_NO_FATAL_FAILURE(WriteDataset(file.get(), "fixed_0d",
                                         fixed_str.get(), {}, fixed_scalar));

    // Variable-length string 2x2 dataset.
    const ScopedH5Type vlen_str(H5Tcopy(H5T_C_S1));
    ASSERT_TRUE(vlen_str.valid());
    ASSERT_GE(H5Tset_size(vlen_str.get(), H5T_VARIABLE), 0);
    ASSERT_GE(H5Tset_strpad(vlen_str.get(), H5T_STR_NULLTERM), 0);
    constexpr std::array<hsize_t, 2> kDims2x2 = {2, 2};
    const char* vlen_2d_vals[4] = {"hi", "world!", "", "longer_str"};
    ASSERT_NO_FATAL_FAILURE(WriteDataset(file.get(), "vlen_2d", vlen_str.get(),
                                         kDims2x2, vlen_2d_vals));

    // Variable-length string scalar (0D) dataset.
    const char* vlen_0d_val = "scalar_vlen";
    ASSERT_NO_FATAL_FAILURE(
        WriteDataset(file.get(), "vlen_0d", vlen_str.get(), {}, &vlen_0d_val));
  }

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));
  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  // Fixed-length 1D string: length is H5Tget_size() == 8.
  coda_cursor c = GotoField(root, "fixed_1d");
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&c, 0));
  int has_ascii = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_ascii_content(&c, &has_ascii));
  EXPECT_EQ(1, has_ascii);
  long str_len = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&c, &str_len));
  EXPECT_EQ(8, str_len);

  std::array<char, 16> buf{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&c, buf.data(), 9));
  EXPECT_STREQ("alpha", buf.data());
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&c, buf.data(), 4));
  EXPECT_STREQ("alp", buf.data());

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&c));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&c, buf.data(), 9));
  EXPECT_STREQ("beta123", buf.data());

  // Fixed-length 0D scalar string.
  c = GotoField(root, "fixed_0d");
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&c, 0));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&c, &str_len));
  EXPECT_EQ(8, str_len);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&c, buf.data(), 9));
  EXPECT_STREQ("fix_0d", buf.data());

  // Variable-length 2x2 string dataset: check lengths & reads (with empty).
  const coda_cursor vlen_2d = GotoField(root, "vlen_2d");
  constexpr long kExpectedVlenLens[4] = {2, 6, 0, 10};
  const char* const kExpectedVlenStrs[4] = {"hi", "world!", "", "longer_str"};
  for (long i = 0; i < 4; ++i) {
    coda_cursor elem = vlen_2d;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&elem, i));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&elem, &str_len));
    EXPECT_EQ(kExpectedVlenLens[i], str_len);
    std::fill(buf.begin(), buf.end(), '#');
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_string(&elem, buf.data(), str_len + 1));
    EXPECT_STREQ(kExpectedVlenStrs[i], buf.data());
  }
  // Truncated read on 2D variable-length string element.
  coda_cursor elem_last = vlen_2d;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_array_element_by_index(&elem_last, 3));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&elem_last, buf.data(), 7));
  EXPECT_STREQ("longer", buf.data());

  // Variable-length scalar (0D) string dataset: full & truncated reads.
  coda_cursor vlen_0d = GotoField(root, "vlen_0d");
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&vlen_0d, 0));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&vlen_0d, &str_len));
  EXPECT_EQ(11, str_len);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&vlen_0d, buf.data(), 12));
  EXPECT_STREQ("scalar_vlen", buf.data());
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&vlen_0d, buf.data(), 7));
  EXPECT_STREQ("scalar", buf.data());
}

TEST_F(CodaHdf5CursorTest, EnumDatasetsScalarArrayAndPartialArray) {
  TempFile h5_file("hdf5_cursor_enum.h5");
  constexpr std::array<int32_t, 4> kEnumVals = {10, 20, 20, 10};
  {
    const ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    const ScopedH5Type enum_type(H5Tenum_create(H5T_NATIVE_INT32));
    ASSERT_TRUE(enum_type.valid());
    constexpr int32_t kLow = 10;
    constexpr int32_t kHigh = 20;
    ASSERT_GE(H5Tenum_insert(enum_type.get(), "LOW", &kLow), 0);
    ASSERT_GE(H5Tenum_insert(enum_type.get(), "HIGH", &kHigh), 0);

    constexpr std::array<hsize_t, 2> kDims2x2 = {2, 2};
    ASSERT_NO_FATAL_FAILURE(WriteDataset(file.get(), "enum2x2", enum_type.get(),
                                         kDims2x2, kEnumVals.data()));
  }

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));
  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  const coda_cursor enum_c = GotoField(root, "enum2x2");
  coda_cursor elem = enum_c;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&elem, 1));
  int32_t scalar = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&elem, &scalar));
  EXPECT_EQ(20, scalar);

  std::array<int32_t, 4> full = {-1, -1, -1, -1};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(&enum_c, full.data(),
                                                       coda_array_ordering_c));
  EXPECT_THAT(full, ElementsAreArray(kEnumVals));

  std::array<int32_t, 2> part = {-1, -1};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_int32_partial_array(&enum_c, 2, 2, part.data()));
  EXPECT_THAT(part, ElementsAre(20, 10));
}

TEST_F(CodaHdf5CursorTest, CompoundScalarAndArrayDatasets) {
  struct CompoundRow {
    double score;
    int32_t id;
    int32_t kind;
    char tag[6];
    uint8_t ignored_opaque[2];
  };
  static_assert(sizeof(CompoundRow) == 24);
  const CompoundRow rows[4] = {
      {1.5, 1, 100, "zero\0", {1, 2}},
      {2.5, 2, 200, "one\0\0", {3, 4}},
      {3.5, 3, 100, "two\0\0", {5, 6}},
      {4.5, 4, 200, "three", {7, 8}},
  };

  TempFile h5_file("hdf5_cursor_compound.h5");
  {
    const ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    const ScopedH5Type str6(H5Tcopy(H5T_C_S1));
    ASSERT_TRUE(str6.valid());
    ASSERT_GE(H5Tset_size(str6.get(), 6), 0);
    ASSERT_GE(H5Tset_strpad(str6.get(), H5T_STR_NULLTERM), 0);

    const ScopedH5Type enum_type(H5Tenum_create(H5T_NATIVE_INT32));
    ASSERT_TRUE(enum_type.valid());
    constexpr int32_t kKindA = 100;
    constexpr int32_t kKindB = 200;
    ASSERT_GE(H5Tenum_insert(enum_type.get(), "A", &kKindA), 0);
    ASSERT_GE(H5Tenum_insert(enum_type.get(), "B", &kKindB), 0);

    const ScopedH5Type opaque_type(H5Tcreate(H5T_OPAQUE, 2));
    ASSERT_TRUE(opaque_type.valid());
    ASSERT_GE(H5Tset_tag(opaque_type.get(), "raw2"), 0);

    const ScopedH5Type cmp_type(H5Tcreate(H5T_COMPOUND, sizeof(CompoundRow)));
    ASSERT_TRUE(cmp_type.valid());
    ASSERT_GE(H5Tinsert(cmp_type.get(), "id", offsetof(CompoundRow, id),
                        H5T_NATIVE_INT32),
              0);
    ASSERT_GE(H5Tinsert(cmp_type.get(), "score", offsetof(CompoundRow, score),
                        H5T_NATIVE_DOUBLE),
              0);
    ASSERT_GE(H5Tinsert(cmp_type.get(), "tag", offsetof(CompoundRow, tag),
                        str6.get()),
              0);
    ASSERT_GE(H5Tinsert(cmp_type.get(), "kind", offsetof(CompoundRow, kind),
                        enum_type.get()),
              0);
    ASSERT_GE(
        H5Tinsert(cmp_type.get(), "ignored_opaque",
                  offsetof(CompoundRow, ignored_opaque), opaque_type.get()),
        0);

    constexpr std::array<hsize_t, 2> kDims2x2 = {2, 2};
    ASSERT_NO_FATAL_FAILURE(
        WriteDataset(file.get(), "cmp_2x2", cmp_type.get(), kDims2x2, rows));
    ASSERT_NO_FATAL_FAILURE(
        WriteDataset(file.get(), "cmp_0d", cmp_type.get(), {}, &rows[1]));
  }

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));
  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  // Navigate into 2x2 compound element (1, 0) -> index 2.
  coda_cursor c = GotoField(root, "cmp_2x2");
  constexpr long kSub[2] = {1, 0};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element(&c, 2, kSub));

  coda_type_class type_class = coda_special_class;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&c, &type_class));
  EXPECT_EQ(coda_record_class, type_class);

  // Unsupported opaque member was skipped; 4 supported members remain.
  long num_fields = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&c, &num_fields));
  EXPECT_EQ(4, num_fields);

  // Compound field index bounds checks.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_record_field_by_index(&c, -1));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_record_field_by_index(&c, 4));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);

  // Empty attributes on tag_hdf5_compound_datatype.
  coda_cursor cmp_attrs = c;
  int cmp_has_attrs = -1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_has_attributes(&cmp_attrs, &cmp_has_attrs));
  EXPECT_EQ(0, cmp_has_attrs);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&cmp_attrs));
  long attr_count = -1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_num_elements(&cmp_attrs, &attr_count));
  EXPECT_EQ(0, attr_count);

  // Iterate all 4 compound fields and read each value.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_record_field(&c));
  int32_t id = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c, &id));
  EXPECT_EQ(3, id);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&c));
  double score = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &score));
  EXPECT_DOUBLE_EQ(3.5, score);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&c));
  std::array<char, 8> tag_buf{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&c, tag_buf.data(), 7));
  EXPECT_STREQ("two", tag_buf.data());
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&c, tag_buf.data(), 3));
  EXPECT_STREQ("tw", tag_buf.data());

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&c));
  int32_t kind = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c, &kind));
  EXPECT_EQ(100, kind);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_next_record_field(&c));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);

  // Also read scalar (0D) compound dataset fields.
  coda_cursor c0 = GotoField(root, "cmp_0d");
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_array_element(&c0));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&c0, "id"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c0, &id));
  EXPECT_EQ(2, id);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&c0));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&c0, "tag"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&c0, tag_buf.data(), 7));
  EXPECT_STREQ("one", tag_buf.data());
}

TEST_F(CodaHdf5CursorTest, AttributesOnGroupSubgroupDatasetAndBasicElements) {
  TempFile h5_file("hdf5_cursor_attrs.h5");
  {
    const ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    constexpr int32_t kRootInt = 42;
    ASSERT_NO_FATAL_FAILURE(WriteAttribute(file.get(), "root_int",
                                           H5T_NATIVE_INT32, {}, &kRootInt));

    const ScopedH5Type vlen_str(H5Tcopy(H5T_C_S1));
    ASSERT_TRUE(vlen_str.valid());
    ASSERT_GE(H5Tset_size(vlen_str.get(), H5T_VARIABLE), 0);
    const char* title = "hdf5_title";
    ASSERT_NO_FATAL_FAILURE(
        WriteAttribute(file.get(), "root_vlen", vlen_str.get(), {}, &title));

    const ScopedH5Space null_space(H5Screate(H5S_NULL));
    ASSERT_TRUE(null_space.valid());
    const ScopedH5Attr null_str_attr(
        H5Acreate2(file.get(), "empty_str", vlen_str.get(), null_space.get(),
                   H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(null_str_attr.valid());

    constexpr std::array<hsize_t, 1> kDims3 = {3};
    constexpr std::array<int16_t, 3> kAttrArr = {7, 8, 9};
    ASSERT_NO_FATAL_FAILURE(WriteAttribute(
        file.get(), "root_arr", H5T_NATIVE_INT16, kDims3, kAttrArr.data()));

    const ScopedH5Group grp_with_attr(H5Gcreate2(
        file.get(), "grp_attr", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp_with_attr.valid());
    constexpr double kVersion = 2.5;
    ASSERT_NO_FATAL_FAILURE(WriteAttribute(grp_with_attr.get(), "ver",
                                           H5T_NATIVE_DOUBLE, {}, &kVersion));

    const ScopedH5Group grp_no_attr(H5Gcreate2(
        file.get(), "grp_plain", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp_no_attr.valid());

    constexpr std::array<hsize_t, 1> kDims2 = {2};
    constexpr std::array<int32_t, 2> kData = {11, 22};
    ASSERT_NO_FATAL_FAILURE(WriteDataset(
        grp_with_attr.get(), "data", H5T_NATIVE_INT32, kDims2, kData.data()));
    const ScopedH5Dataset dset(
        H5Dopen2(grp_with_attr.get(), "data", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    const ScopedH5Type fixed_str(H5Tcopy(H5T_C_S1));
    ASSERT_TRUE(fixed_str.valid());
    ASSERT_GE(H5Tset_size(fixed_str.get(), 4), 0);
    const char units[4] = "m/s";
    ASSERT_NO_FATAL_FAILURE(
        WriteAttribute(dset.get(), "units", fixed_str.get(), {}, units));
  }

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));
  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  // Root group attributes (tag_hdf5_group).
  int has_attrs = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&root, &has_attrs));
  EXPECT_EQ(1, has_attrs);

  coda_cursor root_attrs = root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&root_attrs));
  long num_attrs = -1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_num_elements(&root_attrs, &num_attrs));
  EXPECT_EQ(4, num_attrs);

  coda_cursor attr = GotoField(root_attrs, "root_int");
  int32_t root_int = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&attr, &root_int));
  EXPECT_EQ(42, root_int);

  attr = GotoField(root_attrs, "root_vlen");
  std::array<char, 32> buf{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&attr, buf.data(), 32));
  EXPECT_STREQ("hdf5_title", buf.data());

  attr = GotoField(root_attrs, "empty_str");
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&attr, buf.data(), 32));
  EXPECT_STREQ("", buf.data());

  attr = GotoField(root_attrs, "root_arr");
  std::array<int16_t, 3> arr_vals{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16_array(&attr, arr_vals.data(),
                                                       coda_array_ordering_c));
  EXPECT_THAT(arr_vals, ElementsAre(7, 8, 9));

  // Subgroup without HDF5 attributes: coda_hdf5_create_tree() unconditionally
  // attaches a non-NULL empty coda_mem_record to every group/dataset definition
  // (coda-hdf5-type.c:1220-1230), so coda_cursor_has_attributes() reports 1
  // while the attribute record has 0 fields.
  coda_cursor plain_grp = GotoField(root, "grp_plain");
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&plain_grp, &has_attrs));
  EXPECT_EQ(1, has_attrs);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&plain_grp));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&plain_grp, &num_attrs));
  EXPECT_EQ(0, num_attrs);

  // Subgroup and dataset attributes (tag_hdf5_group & tag_hdf5_dataset).
  coda_cursor grp_attr_c = GotoField(root, "grp_attr");
  coda_cursor ver_c = grp_attr_c;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&ver_c));
  ver_c = GotoField(ver_c, "ver");
  double ver = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&ver_c, &ver));
  EXPECT_DOUBLE_EQ(2.5, ver);

  coda_cursor dset_c = GotoField(grp_attr_c, "data");
  coda_cursor dset_attrs = dset_c;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&dset_attrs));
  dset_attrs = GotoField(dset_attrs, "units");
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&dset_attrs, buf.data(), 16));
  EXPECT_STREQ("m/s", buf.data());

  // Basic element inside dataset has has_attributes == 0 and empty attributes
  // (default case in coda_hdf5_cursor_goto_attributes).
  coda_cursor elem_attrs = dset_c;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_array_element(&elem_attrs));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&elem_attrs, &has_attrs));
  EXPECT_EQ(0, has_attrs);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&elem_attrs));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_num_elements(&elem_attrs, &num_attrs));
  EXPECT_EQ(0, num_attrs);
}

TEST_F(CodaHdf5CursorTest, ConversionsEnabledAndDisabled) {
  ScopedCodaOptions restore_options;
  TempFile h5_file("hdf5_cursor_conversions.h5");
  {
    const ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    constexpr std::array<hsize_t, 2> kDims2x2 = {2, 2};
    constexpr std::array<int16_t, 4> kScaledVals = {10, 20, -999, 40};
    ASSERT_NO_FATAL_FAILURE(WriteDataset(file.get(), "scaled_i16",
                                         H5T_NATIVE_INT16, kDims2x2,
                                         kScaledVals.data()));
    const ScopedH5Dataset dset(H5Dopen2(file.get(), "scaled_i16", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    constexpr double kScale = 0.5;
    constexpr double kOffset = 100.0;
    constexpr int16_t kFill = -999;
    ASSERT_NO_FATAL_FAILURE(WriteAttribute(dset.get(), "scale_factor",
                                           H5T_NATIVE_DOUBLE, {}, &kScale));
    ASSERT_NO_FATAL_FAILURE(WriteAttribute(dset.get(), "add_offset",
                                           H5T_NATIVE_DOUBLE, {}, &kOffset));
    ASSERT_NO_FATAL_FAILURE(
        WriteAttribute(dset.get(), "_FillValue", H5T_NATIVE_INT16, {}, &kFill));
  }

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));
  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  const coda_cursor dset = GotoField(root, "scaled_i16");
  coda_cursor elem = dset;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&elem, 2));

  // With coda_option_perform_conversions == 1: read_type is double and
  // fill values become NaN.
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_conversions(1));
  coda_native_type read_type = coda_native_type_not_available;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_read_type(&elem, &read_type));
  EXPECT_EQ(coda_native_type_double, read_type);

  int16_t raw = 0;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_int16(&elem, &raw));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  double conv_scalar = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&elem, &conv_scalar));
  EXPECT_THAT(conv_scalar, IsNan());

  std::array<double, 4> conv_arr{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_array(&dset, conv_arr.data(),
                                                        coda_array_ordering_c));
  EXPECT_DOUBLE_EQ(105.0, conv_arr[0]);
  EXPECT_DOUBLE_EQ(110.0, conv_arr[1]);
  EXPECT_THAT(conv_arr[2], IsNan());
  EXPECT_DOUBLE_EQ(120.0, conv_arr[3]);

  std::array<double, 2> conv_part{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_partial_array(
                              &dset, 2, 2, conv_part.data()));
  EXPECT_THAT(conv_part[0], IsNan());
  EXPECT_DOUBLE_EQ(120.0, conv_part[1]);

  // With coda_option_perform_conversions == 0: raw int16 values are returned.
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_conversions(0));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_read_type(&elem, &read_type));
  EXPECT_EQ(coda_native_type_int16, read_type);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&elem, &raw));
  EXPECT_EQ(-999, raw);

  std::array<int16_t, 4> raw_arr{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16_array(&dset, raw_arr.data(),
                                                       coda_array_ordering_c));
  EXPECT_THAT(raw_arr, ElementsAre(10, 20, -999, 40));

  std::array<int16_t, 2> raw_part{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_int16_partial_array(&dset, 0, 2, raw_part.data()));
  EXPECT_THAT(raw_part, ElementsAre(10, 20));
}

TEST_F(CodaHdf5CursorTest, UnsupportedHdf5TypesSkippedAndUnsupportedReadsFail) {
  TempFile h5_file("hdf5_cursor_unsupported.h5");
  {
    const ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    constexpr std::array<hsize_t, 1> kDims2 = {2};
    constexpr std::array<int32_t, 2> kValid = {123, 456};
    ASSERT_NO_FATAL_FAILURE(WriteDataset(file.get(), "valid", H5T_NATIVE_INT32,
                                         kDims2, kValid.data()));

    // Fixed 1-byte string to verify read_char fails on HDF5 text types.
    const ScopedH5Type str1(H5Tcopy(H5T_C_S1));
    ASSERT_TRUE(str1.valid());
    ASSERT_GE(H5Tset_size(str1.get(), 1), 0);
    const char ch[2] = {'a', 'b'};
    ASSERT_NO_FATAL_FAILURE(
        WriteDataset(file.get(), "text1", str1.get(), kDims2, ch));

    // Unsupported types skipped by coda_hdf5_create_tree().
    const ScopedH5Type opaque(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(opaque.valid());
    ASSERT_GE(H5Tset_tag(opaque.get(), "opq"), 0);
    const uint8_t opaque_data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_NO_FATAL_FAILURE(WriteDataset(file.get(), "skip_opaque",
                                         opaque.get(), kDims2, opaque_data));

    const uint8_t bit_data[2] = {0xAA, 0x55};
    const ScopedH5Space space2(H5Screate_simple(1, kDims2.data(), nullptr));
    ASSERT_TRUE(space2.valid());
    const ScopedH5Dataset bit_dset(
        H5Dcreate2(file.get(), "skip_bitfield", H5T_STD_B8LE, space2.get(),
                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(bit_dset.valid());
    ASSERT_GE(H5Dwrite(bit_dset.get(), H5T_NATIVE_B8, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, bit_data),
              0);

    hobj_ref_t ref = 0;
    ASSERT_GE(H5Rcreate(&ref, file.get(), "valid", H5R_OBJECT, -1), 0);
    ASSERT_NO_FATAL_FAILURE(
        WriteDataset(file.get(), "skip_ref", H5T_STD_REF_OBJ, {}, &ref));

    const ScopedH5Type vlen_i32(H5Tvlen_create(H5T_NATIVE_INT32));
    ASSERT_TRUE(vlen_i32.valid());
    int32_t seq[2] = {7, 8};
    hvl_t vlen_val = {2, seq};
    ASSERT_NO_FATAL_FAILURE(
        WriteDataset(file.get(), "skip_vlen", vlen_i32.get(), {}, &vlen_val));

    constexpr std::array<hsize_t, 1> kArrTypeDims = {2};
    const ScopedH5Type arr_type(
        H5Tarray_create2(H5T_NATIVE_INT32, 1, kArrTypeDims.data()));
    ASSERT_TRUE(arr_type.valid());
    const int32_t arr_val[2] = {1, 2};
    ASSERT_NO_FATAL_FAILURE(WriteDataset(file.get(), "skip_array_type",
                                         arr_type.get(), {}, arr_val));

    const ScopedH5Space null_space(H5Screate(H5S_NULL));
    ASSERT_TRUE(null_space.valid());
    const ScopedH5Dataset null_dset(
        H5Dcreate2(file.get(), "skip_null", H5T_NATIVE_INT32, null_space.get(),
                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(null_dset.valid());

    ASSERT_GE(H5Lcreate_soft("/valid", file.get(), "skip_softlink", H5P_DEFAULT,
                             H5P_DEFAULT),
              0);

    const ScopedH5Type committed_t(H5Tcopy(H5T_NATIVE_INT32));
    ASSERT_TRUE(committed_t.valid());
    ASSERT_GE(H5Tcommit2(file.get(), "skip_committed_type", committed_t.get(),
                         H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
              0);
    const ScopedH5Type reopened_t(
        H5Topen2(file.get(), "skip_committed_type", H5P_DEFAULT));
    ASSERT_TRUE(reopened_t.valid());
  }

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));
  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  // Only "valid" and "text1" should appear in root group.
  long num_fields = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&root, &num_fields));
  EXPECT_EQ(2, num_fields);

  const coda_cursor valid_dset = GotoField(root, "valid");
  coda_cursor valid_elem = valid_dset;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_array_element_by_index(&valid_elem, 0));

  // Raw bits/bytes and char reads are unsupported for HDF5.
  uint8_t raw_bytes[4] = {};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_bits(&valid_elem, raw_bytes, 0, 8));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_bytes(&valid_elem, raw_bytes, 0, 4));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  const coda_cursor text1_dset = GotoField(root, "text1");
  coda_cursor text1_elem = text1_dset;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_array_element_by_index(&text1_elem, 0));

  char ch_out = 0;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_char(&text1_elem, &ch_out));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  char ch_arr[2] = {};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_char_array(&text1_dset, ch_arr,
                                            coda_array_ordering_c));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  // Type mismatch error paths on scalar & array reads.
  int8_t i8_out = 0;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_int8(&valid_elem, &i8_out));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  uint32_t u32_out = 0;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_uint32(&valid_elem, &u32_out));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  char str_out[8] = {};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_string(&valid_elem, str_out, 8));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  int32_t i32_out = 0;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_int32(&text1_elem, &i32_out));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
}

TEST_F(CodaHdf5CursorTest, CorruptFletcher32DatasetTriggersHdf5ReadErrors) {
  constexpr std::array<uint8_t, 16> kMagicData = {
      0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x17, 0x28,
      0x39, 0x4A, 0x5B, 0x6C, 0x7D, 0x8E, 0x9F, 0x01};
  TempFile h5_file("hdf5_cursor_fletcher32.h5");
  {
    const ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    constexpr std::array<hsize_t, 1> kDims16 = {16};
    const ScopedH5Space space(H5Screate_simple(1, kDims16.data(), nullptr));
    ASSERT_TRUE(space.valid());
    const ScopedH5Plist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, kDims16.data()), 0);
    ASSERT_GE(H5Pset_fletcher32(dcpl.get()), 0);

    const ScopedH5Dataset dset(H5Dcreate2(file.get(), "checked", H5T_STD_U8LE,
                                          space.get(), H5P_DEFAULT, dcpl.get(),
                                          H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_UINT8, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, kMagicData.data()),
              0);
  }

  // Flip one byte inside the Fletcher32-protected chunk so H5Fopen()/coda_open
  // succeeds while H5Dread() fails with CODA_ERROR_HDF5.
  std::vector<uint8_t> bytes;
  {
    std::ifstream in(h5_file.path(), std::ios::binary | std::ios::ate);
    ASSERT_TRUE(in.is_open());
    const auto size = in.tellg();
    ASSERT_GT(size, 0);
    bytes.resize(static_cast<size_t>(size));
    in.seekg(0);
    ASSERT_TRUE(in.read(reinterpret_cast<char*>(bytes.data()), size).good());
  }
  auto it = std::search(bytes.begin(), bytes.end(), kMagicData.begin(),
                        kMagicData.end());
  ASSERT_NE(bytes.end(), it);
  *it ^= 0xFF;
  ASSERT_TRUE(h5_file.WriteBytes(bytes));

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));
  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  const coda_cursor checked = GotoField(root, "checked");
  coda_cursor elem = checked;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&elem, 0));

  // coda-hdf5.c:146-148 leaks H5Eget_major/H5Eget_minor strings on HDF5 error.
  const absl::LeakCheckDisabler leak_check_disabler;

  std::array<uint8_t, 16> arr_dst{};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_uint8_array(&checked, arr_dst.data(),
                                             coda_array_ordering_c));
  EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);

  std::array<uint8_t, 8> part_dst{};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_uint8_partial_array(&checked, 0, 8,
                                                     part_dst.data()));
  EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);

  uint8_t scalar_dst = 0;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_uint8(&elem, &scalar_dst));
  EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);
}

TEST_F(CodaHdf5CursorTest, MissingAndCorruptHdf5FilesFail) {
  const std::filesystem::path missing =
      std::filesystem::path(::testing::TempDir()) /
      "hdf5_cursor_does_not_exist.h5";
  std::error_code ec;
  std::filesystem::remove(missing, ec);

  ScopedProduct product;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(missing.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);
  EXPECT_EQ(nullptr, product.get());

  TempFile truncated("hdf5_cursor_truncated.h5");
  constexpr auto kHdf5Magic =
      std::to_array<uint8_t>({0x89, 'H', 'D', 'F', '\r', '\n', 0x1A, '\n'});
  ASSERT_TRUE(truncated.WriteBytes(kHdf5Magic));

  {
    const absl::LeakCheckDisabler leak_check_disabler;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_open(truncated.c_str(), product.OutParam()));
  }
  EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);
  EXPECT_EQ(nullptr, product.get());
}

template <typename T>
void WriteDatasetWithMemType(hid_t loc_id, const char* name,
                             hid_t file_dtype_id, hid_t mem_dtype_id,
                             std::span<const hsize_t> dims, const T* data) {
  ScopedH5Space space;
  if (dims.empty()) {
    space.Reset(H5Screate(H5S_SCALAR));
  } else {
    space.Reset(
        H5Screate_simple(static_cast<int>(dims.size()), dims.data(), nullptr));
  }
  ASSERT_TRUE(space.valid());
  const ScopedH5Dataset dset(H5Dcreate2(loc_id, name, file_dtype_id,
                                        space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                        H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  ASSERT_GE(
      H5Dwrite(dset.get(), mem_dtype_id, H5S_ALL, H5S_ALL, H5P_DEFAULT, data),
      0);
}

bool CorruptAllHdf5Magic(const std::filesystem::path& path,
                         std::string_view magic, std::string_view replacement) {
  if (magic.size() != 4 || replacement.size() != 4) {
    return false;
  }
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in.is_open()) {
    return false;
  }
  const std::streamsize file_size = in.tellg();
  if (file_size <= 0) {
    return false;
  }
  in.seekg(0, std::ios::beg);
  std::vector<uint8_t> bytes(static_cast<size_t>(file_size));
  if (!in.read(reinterpret_cast<char*>(bytes.data()), file_size)) {
    return false;
  }
  in.close();

  size_t replaced = 0;
  for (size_t i = 0; i + magic.size() <= bytes.size(); ++i) {
    if (std::memcmp(bytes.data() + i, magic.data(), magic.size()) == 0) {
      std::memcpy(bytes.data() + i, replacement.data(), replacement.size());
      ++replaced;
    }
  }
  if (replaced == 0) {
    return false;
  }
  return WriteFileBytes(path, bytes);
}

TEST_F(CodaHdf5CursorTest, CorruptVlenGlobalHeapTriggersHdf5ReadErrors) {
  TempFile h5_file("hdf5_cursor_corrupt_gcol.h5");
  {
    const ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    const ScopedH5Type vlen_str(H5Tcopy(H5T_C_S1));
    ASSERT_TRUE(vlen_str.valid());
    ASSERT_GE(H5Tset_size(vlen_str.get(), H5T_VARIABLE), 0);
    ASSERT_GE(H5Tset_strpad(vlen_str.get(), H5T_STR_NULLTERM), 0);

    const char* scalar_val = "scalar_vlen_heap";
    ASSERT_NO_FATAL_FAILURE(
        WriteDataset(file.get(), "vlen_0d", vlen_str.get(), {}, &scalar_val));

    constexpr std::array<hsize_t, 1> kDims2 = {2};
    const char* arr_vals[2] = {"first_vlen", "second_vlen"};
    ASSERT_NO_FATAL_FAILURE(
        WriteDataset(file.get(), "vlen_1d", vlen_str.get(), kDims2, arr_vals));
  }

  // Corrupting Global Heap ("GCOL") leaves file open intact while causing
  // H5Dread (0D) and H5Dvlen_get_buf_size (1D) to fail with CODA_ERROR_HDF5.
  ASSERT_TRUE(CorruptAllHdf5Magic(h5_file.path(), "GCOL", "XCOL"));

  const absl::Cleanup clear_h5_errors = [] {
    H5Eclear1();
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));
  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  coda_cursor vlen0d_elem = GotoField(root, "vlen_0d");
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_array_element_by_index(&vlen0d_elem, 0));

  coda_cursor vlen1d_elem = GotoField(root, "vlen_1d");
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_array_element_by_index(&vlen1d_elem, 1));

  const absl::LeakCheckDisabler leak_check_disabler;
  long str_len = -1;
  std::array<char, 32> buf{};

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_string_length(&vlen0d_elem, &str_len));
  EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_string(&vlen0d_elem, buf.data(), buf.size()));
  EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_string_length(&vlen1d_elem, &str_len));
  EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_string(&vlen1d_elem, buf.data(), buf.size()));
  EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);
}

TEST_F(CodaHdf5CursorTest, PartialArrayUnlimitedLengthTriggersMemSpaceError) {
  ScopedCodaOptions restore_options;
  TempFile h5_file("hdf5_cursor_partial_unlimited.h5");
  {
    const ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    constexpr std::array<hsize_t, 1> kDims4 = {4};
    constexpr std::array<int32_t, 4> kVals = {10, 20, 30, 40};
    ASSERT_NO_FATAL_FAILURE(WriteDataset(file.get(), "arr1d", H5T_NATIVE_INT32,
                                         kDims4, kVals.data()));
  }

  const absl::Cleanup clear_h5_errors = [] {
    H5Eclear1();
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));
  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));
  const coda_cursor arr_c = GotoField(root, "arr1d");

  // With boundary checks disabled, offset=1 and length=-1 wraps
  // start[0] + count[0] == 0 <= dim[0], passes H5Sselect_hyperslab with
  // H5S_UNLIMITED, and fails in H5Screate_simple(1, &hlength, NULL).
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(0));
  std::array<int32_t, 4> dst{};
  {
    const absl::LeakCheckDisabler leak_check_disabler;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_cursor_read_int32_partial_array(&arr_c, 1, -1, dst.data()));
  }
  EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);
}

TEST_F(CodaHdf5CursorTest,
       InternalCursorErrorPathsDataspaceArrayDimAndConversions) {
  struct Row {
    int16_t s16;
    int32_t s32;
  };
  static_assert(offsetof(Row, s16) == 0);
  static_assert(offsetof(Row, s32) == 4);
  static_assert(sizeof(Row) == 8);

  TempFile h5_file("hdf5_cursor_internal_errors.h5");
  {
    const ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    constexpr std::array<hsize_t, 1> kDims2 = {2};
    constexpr std::array<int32_t, 2> kI32 = {11, 22};
    ASSERT_NO_FATAL_FAILURE(WriteDataset(file.get(), "i32_1d", H5T_NATIVE_INT32,
                                         kDims2, kI32.data()));

    const ScopedH5Type vlen_str(H5Tcopy(H5T_C_S1));
    ASSERT_TRUE(vlen_str.valid());
    ASSERT_GE(H5Tset_size(vlen_str.get(), H5T_VARIABLE), 0);
    ASSERT_GE(H5Tset_strpad(vlen_str.get(), H5T_STR_NULLTERM), 0);
    const char* vlen_vals[2] = {"hello", "world"};
    ASSERT_NO_FATAL_FAILURE(
        WriteDataset(file.get(), "vlen_1d", vlen_str.get(), kDims2, vlen_vals));

    const ScopedH5Type cmp_type(H5Tcreate(H5T_COMPOUND, sizeof(Row)));
    ASSERT_TRUE(cmp_type.valid());
    ASSERT_GE(
        H5Tinsert(cmp_type.get(), "s16", offsetof(Row, s16), H5T_NATIVE_INT16),
        0);
    ASSERT_GE(
        H5Tinsert(cmp_type.get(), "s32", offsetof(Row, s32), H5T_NATIVE_INT32),
        0);
    const Row rows[2] = {{12, 345}, {67, 890}};
    ASSERT_NO_FATAL_FAILURE(
        WriteDataset(file.get(), "cmp_1d", cmp_type.get(), kDims2, rows));
  }

  const absl::Cleanup clear_h5_errors = [] {
    H5Eclear1();
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));
  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  // 1. Direct read_array on non-empty group fails
  // coda_hdf5_cursor_get_array_dim.
  {
    std::array<int32_t, 3> buf{};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_hdf5_cursor_read_int32_array(&root, buf.data()));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  }

  // 2. Direct read_partial_array with non-array dataset definition fails
  //    coda_hdf5_cursor_get_array_dim.
  {
    const coda_cursor arr_c = GotoField(root, "i32_1d");
    auto* ds = reinterpret_cast<coda_hdf5_dataset*>(CursorStackTypeAt(arr_c));
    coda_type_array* saved_def = ds->definition;
    const absl::Cleanup restore_def = [&] { ds->definition = saved_def; };
    ds->definition =
        reinterpret_cast<coda_type_array*>(ds->base_type->definition);

    int32_t out = 0;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_hdf5_cursor_read_int32_partial_array(&arr_c, 0, 1, &out));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  }

  // 3. Invalid dataspace_id triggers H5Sselect_hyperslab and H5Sselect_elements
  //    error branches in read_partial_array and read_basic_type.
  {
    const coda_cursor arr_c = GotoField(root, "i32_1d");
    coda_cursor elem_c = arr_c;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_array_element_by_index(&elem_c, 0));
    auto* ds = reinterpret_cast<coda_hdf5_dataset*>(CursorStackTypeAt(arr_c));
    const hid_t saved_space = ds->dataspace_id;
    const absl::Cleanup restore_space = [&] { ds->dataspace_id = saved_space; };
    ds->dataspace_id = H5I_INVALID_HID;

    const absl::LeakCheckDisabler leak_check_disabler;
    int32_t out = 0;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int32_partial_array(&arr_c, 0, 1, &out));
    EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int32(&elem_c, &out));
    EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);
  }

  // 4. Invalid dataspace_id on 1D vlen string dataset triggers
  //    H5Sselect_elements error in coda_hdf5_cursor_get_string_length.
  {
    coda_cursor vlen_elem = GotoField(root, "vlen_1d");
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_array_element_by_index(&vlen_elem, 0));
    auto* vlen_ds =
        reinterpret_cast<coda_hdf5_dataset*>(CursorStackTypeAt(vlen_elem, 1));
    const hid_t saved_space = vlen_ds->dataspace_id;
    const absl::Cleanup restore_space = [&] {
      vlen_ds->dataspace_id = saved_space;
    };
    vlen_ds->dataspace_id = H5I_INVALID_HID;

    const absl::LeakCheckDisabler leak_check_disabler;
    long len = -1;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_string_length(&vlen_elem, &len));
    EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);
  }

  // 5. Synthetic compound wrapper over valid 1D vlen dataset lets
  //    H5Dvlen_get_buf_size succeed while H5Dread fails on invalid member_type
  //    (white-box coda_cursor construction using CODA_INTERNAL fields).
  {
    coda_cursor vlen_elem = GotoField(root, "vlen_1d");
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_array_element_by_index(&vlen_elem, 0));
    auto* vlen_ds =
        reinterpret_cast<coda_hdf5_dataset*>(CursorStackTypeAt(vlen_elem, 1));

    coda_hdf5_compound_data_type fake_cmp{};
    fake_cmp.backend = coda_backend_hdf5;
    fake_cmp.definition = nullptr;
    fake_cmp.tag = tag_hdf5_compound_datatype;
    fake_cmp.datatype_id = H5I_INVALID_HID;
    fake_cmp.member = nullptr;
    hid_t bad_member_type = H5I_INVALID_HID;
    fake_cmp.member_type = &bad_member_type;

    coda_cursor synth_cursor{};
    synth_cursor.product = product.get();
    synth_cursor.n = 4;
    synth_cursor.stack[0] = vlen_elem.stack[0];
    synth_cursor.stack[1] = vlen_elem.stack[1];
    synth_cursor.stack[2].type =
        reinterpret_cast<coda_dynamic_type*>(&fake_cmp);
    synth_cursor.stack[2].index = 0;
    synth_cursor.stack[2].bit_offset = -1;
    synth_cursor.stack[3].type = CursorStackTypeAt(vlen_elem);
    synth_cursor.stack[3].index = 0;
    synth_cursor.stack[3].bit_offset = -1;

    const absl::Cleanup reset_sel = [&] {
      H5Sselect_all(vlen_ds->dataspace_id);
    };
    const absl::LeakCheckDisabler leak_check_disabler;
    std::array<char, 16> sbuf{};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_hdf5_cursor_read_string(&synth_cursor, sbuf.data(),
                                               static_cast<long>(sbuf.size())));
    EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);
  }

  // 6. H5Tconvert failures in read_basic_type for both new_size <= size and
  //    new_size > size using compound members (H5Dread reads via member_type,
  //    then H5Tconvert converts from base_type->datatype_id).
  {
    coda_cursor s32_c = GotoField(root, "cmp_1d");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&s32_c, 0));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&s32_c, "s32"));
    auto* s32_type =
        reinterpret_cast<coda_hdf5_basic_data_type*>(CursorStackTypeAt(s32_c));

    const ScopedH5Type opaque4(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(opaque4.valid());
    const hid_t saved_dt = s32_type->datatype_id;
    const absl::Cleanup restore_dt = [&] { s32_type->datatype_id = saved_dt; };
    s32_type->datatype_id = opaque4.get();

    const absl::LeakCheckDisabler leak_check_disabler;
    int32_t out = 0;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int32(&s32_c, &out));
    EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);
  }

  {
    coda_cursor s16_c = GotoField(root, "cmp_1d");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&s16_c, 0));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&s16_c, "s16"));
    auto* s16_type =
        reinterpret_cast<coda_hdf5_basic_data_type*>(CursorStackTypeAt(s16_c));

    const ScopedH5Type opaque2(H5Tcreate(H5T_OPAQUE, 2));
    ASSERT_TRUE(opaque2.valid());
    const hid_t saved_dt = s16_type->datatype_id;
    const coda_native_type saved_rt = s16_type->definition->read_type;
    const absl::Cleanup restore_s16 = [&] {
      s16_type->datatype_id = saved_dt;
      coda_type_set_read_type(s16_type->definition, saved_rt);
    };
    s16_type->datatype_id = opaque2.get();
    ASSERT_EQ(CODA_SUCCESS, coda_type_set_read_type(s16_type->definition,
                                                    coda_native_type_int32));

    const absl::LeakCheckDisabler leak_check_disabler;
    int32_t out = 0;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int32(&s16_c, &out));
    EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);
  }
}

TEST_F(CodaHdf5CursorTest, BigEndianAndWideningReads) {
  TempFile h5_file("hdf5_cursor_endian_widen.h5");
  constexpr std::array<hsize_t, 2> kDims2x2 = {2, 2};
  constexpr std::array<int16_t, 4> kI16 = {-1234, 5678, -90, 12};
  constexpr std::array<uint32_t, 4> kU32 = {100000, 200000, 300000, 400000};
  constexpr std::array<float, 4> kF32 = {-1.25f, 2.5f, -3.75f, 4.0f};
  constexpr std::array<double, 4> kF64 = {-10.5, 20.25, -30.125, 40.5};
  constexpr int8_t kI8Scalar = -42;
  constexpr uint8_t kU8Scalar = 200;

  {
    const ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    ASSERT_NO_FATAL_FAILURE(
        WriteDatasetWithMemType(file.get(), "be_i16", H5T_STD_I16BE,
                                H5T_NATIVE_INT16, kDims2x2, kI16.data()));
    ASSERT_NO_FATAL_FAILURE(
        WriteDatasetWithMemType(file.get(), "be_u32", H5T_STD_U32BE,
                                H5T_NATIVE_UINT32, kDims2x2, kU32.data()));
    ASSERT_NO_FATAL_FAILURE(
        WriteDatasetWithMemType(file.get(), "be_f32", H5T_IEEE_F32BE,
                                H5T_NATIVE_FLOAT, kDims2x2, kF32.data()));
    ASSERT_NO_FATAL_FAILURE(
        WriteDatasetWithMemType(file.get(), "be_f64", H5T_IEEE_F64BE,
                                H5T_NATIVE_DOUBLE, kDims2x2, kF64.data()));
    ASSERT_NO_FATAL_FAILURE(
        WriteDataset(file.get(), "i8_0d", H5T_NATIVE_INT8, {}, &kI8Scalar));
    ASSERT_NO_FATAL_FAILURE(
        WriteDataset(file.get(), "u8_0d", H5T_NATIVE_UINT8, {}, &kU8Scalar));
  }

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));
  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  // Big-endian array & partial reads.
  const coda_cursor be_i16_c = GotoField(root, "be_i16");
  std::array<int16_t, 4> i16_out{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_int16_array(&be_i16_c, i16_out.data(),
                                         coda_array_ordering_c));
  EXPECT_THAT(i16_out, ElementsAreArray(kI16));
  std::array<int16_t, 2> i16_part{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16_partial_array(
                              &be_i16_c, 2, 2, i16_part.data()));
  EXPECT_THAT(i16_part, ElementsAre(-90, 12));

  const coda_cursor be_u32_c = GotoField(root, "be_u32");
  std::array<uint32_t, 4> u32_out{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_uint32_array(&be_u32_c, u32_out.data(),
                                          coda_array_ordering_fortran));
  EXPECT_THAT(u32_out, ElementsAre(100000, 300000, 200000, 400000));

  const coda_cursor be_f32_c = GotoField(root, "be_f32");
  std::array<float, 4> f32_out{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_float_array(&be_f32_c, f32_out.data(),
                                         coda_array_ordering_c));
  EXPECT_THAT(f32_out, ElementsAreArray(kF32));

  coda_cursor be_f64_elem = GotoField(root, "be_f64");
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_array_element_by_index(&be_f64_elem, 2));
  double d_val = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&be_f64_elem, &d_val));
  EXPECT_DOUBLE_EQ(-30.125, d_val);
  float f_from_d = 0.0f;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&be_f64_elem, &f_from_d));
  EXPECT_FLOAT_EQ(-30.125f, f_from_d);

  // Signed and unsigned integer widening + float/double reads.
  coda_cursor i8_elem = GotoField(root, "i8_0d");
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&i8_elem, 0));
  int16_t w_i16 = 0;
  int32_t w_i32 = 0;
  int64_t w_i64 = 0;
  float w_f32 = 0.0f;
  double w_f64 = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&i8_elem, &w_i16));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&i8_elem, &w_i32));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&i8_elem, &w_i64));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&i8_elem, &w_f32));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&i8_elem, &w_f64));
  EXPECT_EQ(-42, w_i16);
  EXPECT_EQ(-42, w_i32);
  EXPECT_EQ(-42, w_i64);
  EXPECT_FLOAT_EQ(-42.0f, w_f32);
  EXPECT_DOUBLE_EQ(-42.0, w_f64);

  coda_cursor u8_elem = GotoField(root, "u8_0d");
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&u8_elem, 0));
  uint16_t w_u16 = 0;
  uint32_t w_u32 = 0;
  uint64_t w_u64 = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint16(&u8_elem, &w_u16));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint32(&u8_elem, &w_u32));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint64(&u8_elem, &w_u64));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&u8_elem, &w_f64));
  EXPECT_EQ(200u, w_u16);
  EXPECT_EQ(200u, w_u32);
  EXPECT_EQ(200u, w_u64);
  EXPECT_DOUBLE_EQ(200.0, w_f64);
}

// TODO(schwehr): Remaining uncovered lines in libcoda/coda-hdf5-cursor.c are
// defensive assert(0) defaults (lines 85, 128, 165, 231), dead checks where
// coda_hdf5_cursor_get_num_elements unconditionally returns 0 (215, 255-256,
// 427), OOM branches (363-364, 703-704, 757-759, 765-767), and post-success
// HDF5 invariant error checks (376-378, 391-392, 474-475, 481-483, 602-603,
// 615-616, 622-624, 728-730, 783-786, 802-804, 838-840).

}  // namespace
}  // namespace stcorp_coda
