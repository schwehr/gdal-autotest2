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
// Tests CODA CDF dynamic type constructors, destructors, data type mappings,
// array ordering, sparse/compression metadata, and attribute attachment
// (libcoda/coda-cdf-type.c).
//
// google3 build caveats:
//   - Internal headers lack extern "C" guards and require
//   coda_internal_prelude.h
//     before coda-cdf-internal.h.
//   - All 35 missed lines in libcoda/coda-cdf-type.c (271/306 = 88.6%) are
//     OOM checks/propagation (26 lines: 170, 190, 192, 216, 218, 238-239,
//     244-246, 273, 275, 341-342, 349-350, 392-395, 411-412, 451, 457, 499-500,
//     502), 1 assert(0)/exit(1) (165), or 8 structurally unreachable defensive
//     error checks on freshly constructed CDF types (174, 250-251, 354-355,
//     384-385).
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-cdf-type.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-cdf-internal.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-cdf.h

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_coda/google/util/cdf_builder.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-cdf-internal.h"
}  // extern "C"

namespace stcorp_coda {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

template <typename T>
struct CodaTypeReleaser {
  void operator()(T* type) const {
    if (type != nullptr) {
      coda_type_release(reinterpret_cast<coda_type*>(type));
    }
  }
};

template <typename T>
using UniqueCodaType = std::unique_ptr<T, CodaTypeReleaser<T>>;

template <typename T>
struct CdfTypeDeleter {
  void operator()(T* type) const {
    if (type != nullptr) {
      coda_cdf_type_delete(reinterpret_cast<coda_dynamic_type*>(type));
    }
  }
};

template <typename T>
using UniqueCdfType = std::unique_ptr<T, CdfTypeDeleter<T>>;

template <typename T>
struct MemTypeDeleter {
  void operator()(T* type) const {
    if (type != nullptr) {
      coda_mem_type_delete(reinterpret_cast<coda_dynamic_type*>(type));
    }
  }
};

template <typename T>
using UniqueMemType = std::unique_ptr<T, MemTypeDeleter<T>>;

UniqueCodaType<coda_type_text> MakeTextDef(coda_format format) {
  UniqueCodaType<coda_type_text> def(coda_type_text_new(format));
  if (def == nullptr) {
    return nullptr;
  }
  if (coda_type_set_read_type(reinterpret_cast<coda_type*>(def.get()),
                              coda_native_type_string) != 0) {
    return nullptr;
  }
  return def;
}

UniqueCodaType<coda_type_number> MakeInt32Def(coda_format format) {
  UniqueCodaType<coda_type_number> def(
      coda_type_number_new(format, coda_integer_class));
  if (def == nullptr) {
    return nullptr;
  }
  if (coda_type_set_read_type(reinterpret_cast<coda_type*>(def.get()),
                              coda_native_type_int32) != 0) {
    return nullptr;
  }
  if (coda_type_set_byte_size(reinterpret_cast<coda_type*>(def.get()), 4) !=
      0) {
    return nullptr;
  }
  return def;
}

struct BasicTypeMappingCase {
  int32_t cdf_data_type;
  int32_t num_elements;
  coda_type_class expected_class;
  coda_native_type expected_read_type;
  int expected_byte_size;
};

constexpr BasicTypeMappingCase kBasicTypeCases[] = {
    {.cdf_data_type = 1,
     .num_elements = 1,
     .expected_class = coda_integer_class,
     .expected_read_type = coda_native_type_int8,
     .expected_byte_size = 1},
    {.cdf_data_type = 41,
     .num_elements = 1,
     .expected_class = coda_integer_class,
     .expected_read_type = coda_native_type_int8,
     .expected_byte_size = 1},
    {.cdf_data_type = 2,
     .num_elements = 1,
     .expected_class = coda_integer_class,
     .expected_read_type = coda_native_type_int16,
     .expected_byte_size = 2},
    {.cdf_data_type = 4,
     .num_elements = 1,
     .expected_class = coda_integer_class,
     .expected_read_type = coda_native_type_int32,
     .expected_byte_size = 4},
    {.cdf_data_type = 8,
     .num_elements = 1,
     .expected_class = coda_integer_class,
     .expected_read_type = coda_native_type_int64,
     .expected_byte_size = 8},
    {.cdf_data_type = 11,
     .num_elements = 1,
     .expected_class = coda_integer_class,
     .expected_read_type = coda_native_type_uint8,
     .expected_byte_size = 1},
    {.cdf_data_type = 12,
     .num_elements = 1,
     .expected_class = coda_integer_class,
     .expected_read_type = coda_native_type_uint16,
     .expected_byte_size = 2},
    {.cdf_data_type = 14,
     .num_elements = 1,
     .expected_class = coda_integer_class,
     .expected_read_type = coda_native_type_uint32,
     .expected_byte_size = 4},
    {.cdf_data_type = 21,
     .num_elements = 1,
     .expected_class = coda_real_class,
     .expected_read_type = coda_native_type_float,
     .expected_byte_size = 4},
    {.cdf_data_type = 44,
     .num_elements = 1,
     .expected_class = coda_real_class,
     .expected_read_type = coda_native_type_float,
     .expected_byte_size = 4},
    {.cdf_data_type = 22,
     .num_elements = 1,
     .expected_class = coda_real_class,
     .expected_read_type = coda_native_type_double,
     .expected_byte_size = 8},
    {.cdf_data_type = 45,
     .num_elements = 1,
     .expected_class = coda_real_class,
     .expected_read_type = coda_native_type_double,
     .expected_byte_size = 8},
    {.cdf_data_type = 51,
     .num_elements = 1,
     .expected_class = coda_text_class,
     .expected_read_type = coda_native_type_char,
     .expected_byte_size = 1},
    {.cdf_data_type = 51,
     .num_elements = 12,
     .expected_class = coda_text_class,
     .expected_read_type = coda_native_type_string,
     .expected_byte_size = 12},
    {.cdf_data_type = 52,
     .num_elements = 1,
     .expected_class = coda_text_class,
     .expected_read_type = coda_native_type_char,
     .expected_byte_size = 1},
    {.cdf_data_type = 52,
     .num_elements = 7,
     .expected_class = coda_text_class,
     .expected_read_type = coda_native_type_string,
     .expected_byte_size = 7},
    {.cdf_data_type = 51,
     .num_elements = 0,
     .expected_class = coda_text_class,
     .expected_read_type = coda_native_type_string,
     .expected_byte_size = 0},
};

TEST(CodaCdfTypeTest, VariableNewScalarMapsAllBasicCdfTypes) {
  int32_t dim[CODA_MAX_NUM_DIMS] = {};
  int32_t dim_varys[CODA_MAX_NUM_DIMS] = {};

  for (const BasicTypeMappingCase& tc : kBasicTypeCases) {
    SCOPED_TRACE(tc.cdf_data_type);
    coda_cdf_variable* variable = nullptr;
    UniqueCdfType<coda_dynamic_type> dyn(coda_cdf_variable_new(
        tc.cdf_data_type, /*max_rec=*/0, /*rec_varys=*/0, /*num_dims=*/0, dim,
        dim_varys, coda_array_ordering_c, tc.num_elements,
        /*sparse_rec_method=*/0, /*has_compression=*/0, &variable));
    ASSERT_NE(nullptr, dyn);
    ASSERT_NE(nullptr, variable);
    EXPECT_EQ(dyn.get(), reinterpret_cast<coda_dynamic_type*>(variable));
    EXPECT_EQ(coda_backend_cdf, variable->backend);
    EXPECT_EQ(tag_cdf_variable, variable->tag);
    EXPECT_EQ(nullptr, variable->attributes);
    EXPECT_EQ(nullptr, variable->base_type);
    EXPECT_EQ(1, variable->num_records);
    EXPECT_EQ(1, variable->num_values_per_record);
    EXPECT_EQ(tc.expected_byte_size, variable->value_size);
    ASSERT_NE(nullptr, variable->offset);
    EXPECT_EQ(-1, variable->offset[0]);
    EXPECT_EQ(nullptr, variable->data);

    coda_type* def = reinterpret_cast<coda_type*>(variable->definition);
    ASSERT_NE(nullptr, def);

    coda_format format = coda_format_ascii;
    ASSERT_EQ(CODA_SUCCESS, coda_type_get_format(def, &format));
    EXPECT_EQ(coda_format_cdf, format);

    coda_type_class type_class = coda_record_class;
    ASSERT_EQ(CODA_SUCCESS, coda_type_get_class(def, &type_class));
    EXPECT_EQ(tc.expected_class, type_class);

    coda_native_type read_type = coda_native_type_not_available;
    ASSERT_EQ(CODA_SUCCESS, coda_type_get_read_type(def, &read_type));
    EXPECT_EQ(tc.expected_read_type, read_type);

    int64_t bit_size = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_type_get_bit_size(def, &bit_size));
    EXPECT_EQ(static_cast<int64_t>(tc.expected_byte_size) * 8, bit_size);

    if (tc.expected_class == coda_text_class) {
      long length = -1;
      ASSERT_EQ(CODA_SUCCESS, coda_type_get_string_length(def, &length));
      EXPECT_EQ(tc.expected_byte_size, length);
    }
  }
}

TEST(CodaCdfTypeTest, VariableNewNonScalarMapsAllBasicCdfTypes) {
  int32_t dim[CODA_MAX_NUM_DIMS] = {};
  int32_t dim_varys[CODA_MAX_NUM_DIMS] = {};

  for (const BasicTypeMappingCase& tc : kBasicTypeCases) {
    SCOPED_TRACE(tc.cdf_data_type);
    coda_cdf_variable* variable = nullptr;
    UniqueCdfType<coda_dynamic_type> dyn(coda_cdf_variable_new(
        tc.cdf_data_type, /*max_rec=*/2, /*rec_varys=*/1, /*num_dims=*/0, dim,
        dim_varys, coda_array_ordering_c, tc.num_elements,
        /*sparse_rec_method=*/0, /*has_compression=*/0, &variable));
    ASSERT_NE(nullptr, dyn);
    ASSERT_NE(nullptr, variable);
    EXPECT_EQ(dyn.get(), reinterpret_cast<coda_dynamic_type*>(variable));
    EXPECT_EQ(tag_cdf_variable, variable->tag);
    EXPECT_EQ(3, variable->num_records);
    EXPECT_EQ(1, variable->num_values_per_record);
    EXPECT_EQ(tc.expected_byte_size, variable->value_size);
    ASSERT_NE(nullptr, variable->offset);
    for (int i = 0; i < 3; ++i) {
      EXPECT_EQ(-1, variable->offset[i]);
    }

    ASSERT_NE(nullptr, variable->base_type);
    EXPECT_EQ(coda_backend_cdf, variable->base_type->backend);
    EXPECT_EQ(tag_cdf_basic_type, variable->base_type->tag);

    coda_type* array_def = reinterpret_cast<coda_type*>(variable->definition);
    coda_type_class array_class = coda_record_class;
    ASSERT_EQ(CODA_SUCCESS, coda_type_get_class(array_def, &array_class));
    EXPECT_EQ(coda_array_class, array_class);

    int num_dims = -1;
    long actual_dims[CODA_MAX_NUM_DIMS] = {};
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_get_array_dim(array_def, &num_dims, actual_dims));
    EXPECT_EQ(1, num_dims);
    EXPECT_EQ(3, actual_dims[0]);

    coda_type* base_def = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_get_array_base_type(array_def, &base_def));
    EXPECT_EQ(variable->base_type->definition, base_def);

    coda_type_class base_class = coda_record_class;
    ASSERT_EQ(CODA_SUCCESS, coda_type_get_class(base_def, &base_class));
    EXPECT_EQ(tc.expected_class, base_class);

    coda_native_type read_type = coda_native_type_not_available;
    ASSERT_EQ(CODA_SUCCESS, coda_type_get_read_type(base_def, &read_type));
    EXPECT_EQ(tc.expected_read_type, read_type);
  }
}

TEST(CodaCdfTypeTest, VariableNewTimeTypesScalarAndArray) {
  struct TimeCase {
    int32_t cdf_data_type;
    coda_type_class expected_base_class;
    coda_native_type expected_base_read_type;
    const char* expected_expr;
  };
  constexpr TimeCase kTimeCases[] = {
      {.cdf_data_type = 31,
       .expected_base_class = coda_real_class,
       .expected_base_read_type = coda_native_type_double,
       .expected_expr = "float(.) * 1e-3 - 63113904000.0"},
      {.cdf_data_type = 33,
       .expected_base_class = coda_integer_class,
       .expected_base_read_type = coda_native_type_int64,
       .expected_expr = "float(.) * 1e-9 - 43200.0"},
  };

  for (const TimeCase& tc : kTimeCases) {
    SCOPED_TRACE(tc.cdf_data_type);
    int32_t dim[CODA_MAX_NUM_DIMS] = {4, 0, 0, 0, 0, 0, 0, 0};
    int32_t dim_varys_zero[CODA_MAX_NUM_DIMS] = {0, 0, 0, 0, 0, 0, 0, 0};
    int32_t dim_varys_one[CODA_MAX_NUM_DIMS] = {1, 0, 0, 0, 0, 0, 0, 0};

    // 1. Scalar time variable returns coda_cdf_time* wrapping
    // coda_cdf_variable.
    coda_cdf_variable* scalar_var = nullptr;
    UniqueCdfType<coda_dynamic_type> scalar_dyn(coda_cdf_variable_new(
        tc.cdf_data_type, /*max_rec=*/0, /*rec_varys=*/0, /*num_dims=*/0, dim,
        dim_varys_zero, coda_array_ordering_c, /*num_elements=*/1,
        /*sparse_rec_method=*/0, /*has_compression=*/0, &scalar_var));
    ASSERT_NE(nullptr, scalar_dyn);
    ASSERT_NE(nullptr, scalar_var);
    EXPECT_NE(scalar_dyn.get(),
              reinterpret_cast<coda_dynamic_type*>(scalar_var));

    auto* scalar_time = reinterpret_cast<coda_cdf_time*>(scalar_dyn.get());
    EXPECT_EQ(coda_backend_cdf, scalar_time->backend);
    EXPECT_EQ(tag_cdf_time, scalar_time->tag);
    EXPECT_EQ(tc.cdf_data_type, scalar_time->data_type);
    EXPECT_EQ(reinterpret_cast<coda_dynamic_type*>(scalar_var),
              scalar_time->base_type);
    EXPECT_EQ(8, scalar_var->value_size);

    coda_type* time_def = reinterpret_cast<coda_type*>(scalar_time->definition);
    coda_type_class time_class = coda_record_class;
    ASSERT_EQ(CODA_SUCCESS, coda_type_get_class(time_def, &time_class));
    EXPECT_EQ(coda_special_class, time_class);

    coda_special_type special_type = coda_special_no_data;
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_get_special_type(time_def, &special_type));
    EXPECT_EQ(coda_special_time, special_type);

    coda_type* base_def = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_get_special_base_type(time_def, &base_def));
    EXPECT_EQ(reinterpret_cast<coda_type*>(scalar_var->definition), base_def);

    coda_type_class base_class = coda_record_class;
    ASSERT_EQ(CODA_SUCCESS, coda_type_get_class(base_def, &base_class));
    EXPECT_EQ(tc.expected_base_class, base_class);

    coda_native_type base_read_type = coda_native_type_not_available;
    ASSERT_EQ(CODA_SUCCESS, coda_type_get_read_type(base_def, &base_read_type));
    EXPECT_EQ(tc.expected_base_read_type, base_read_type);

    ASSERT_NE(nullptr, scalar_time->definition->value_expr);
    ScopedExpr expected_expr;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string(
                                tc.expected_expr, expected_expr.OutParam()));
    EXPECT_EQ(1, coda_expression_is_equal(scalar_time->definition->value_expr,
                                          expected_expr.get()));

    // 2. Non-scalar time variable returns coda_cdf_variable whose base_type is
    //    coda_cdf_time wrapping a tag_cdf_basic_type.
    coda_cdf_variable* array_var = nullptr;
    UniqueCdfType<coda_dynamic_type> array_dyn(coda_cdf_variable_new(
        tc.cdf_data_type, /*max_rec=*/1, /*rec_varys=*/1, /*num_dims=*/1, dim,
        dim_varys_one, coda_array_ordering_c, /*num_elements=*/1,
        /*sparse_rec_method=*/0, /*has_compression=*/0, &array_var));
    ASSERT_NE(nullptr, array_dyn);
    ASSERT_NE(nullptr, array_var);
    EXPECT_EQ(array_dyn.get(), reinterpret_cast<coda_dynamic_type*>(array_var));
    EXPECT_EQ(2, array_var->num_records);
    EXPECT_EQ(4, array_var->num_values_per_record);
    EXPECT_EQ(8, array_var->value_size);

    ASSERT_NE(nullptr, array_var->base_type);
    EXPECT_EQ(tag_cdf_time, array_var->base_type->tag);
    auto* elem_time = reinterpret_cast<coda_cdf_time*>(array_var->base_type);
    EXPECT_EQ(tc.cdf_data_type, elem_time->data_type);
    ASSERT_NE(nullptr, elem_time->base_type);
    EXPECT_EQ(tag_cdf_basic_type,
              reinterpret_cast<coda_cdf_type*>(elem_time->base_type)->tag);
  }
}

TEST(CodaCdfTypeTest, VariableNewNonVaryingDimsBecomeScalarOrFilteredArray) {
  int32_t dim[CODA_MAX_NUM_DIMS] = {2, 3, 4, 0, 0, 0, 0, 0};

  // 1. rec_varys == 0 and all dim_varys == 0 -> scalar variable.
  int32_t all_zero_varys[CODA_MAX_NUM_DIMS] = {0, 0, 0, 0, 0, 0, 0, 0};
  coda_cdf_variable* scalar_var = nullptr;
  UniqueCdfType<coda_dynamic_type> scalar_dyn(coda_cdf_variable_new(
      /*data_type=*/4, /*max_rec=*/0, /*rec_varys=*/0, /*num_dims=*/3, dim,
      all_zero_varys, coda_array_ordering_c, /*num_elements=*/1,
      /*sparse_rec_method=*/0, /*has_compression=*/0, &scalar_var));
  ASSERT_NE(nullptr, scalar_dyn);
  ASSERT_NE(nullptr, scalar_var);
  EXPECT_EQ(nullptr, scalar_var->base_type);
  EXPECT_EQ(1, scalar_var->num_records);
  EXPECT_EQ(1, scalar_var->num_values_per_record);
  EXPECT_EQ(coda_integer_class, scalar_var->definition->type_class);

  // 2. rec_varys == 0 with one varying dimension -> 1D array of size 3.
  int32_t middle_varys[CODA_MAX_NUM_DIMS] = {0, 1, 0, 0, 0, 0, 0, 0};
  coda_cdf_variable* mid_var = nullptr;
  UniqueCdfType<coda_dynamic_type> mid_dyn(coda_cdf_variable_new(
      /*data_type=*/4, /*max_rec=*/0, /*rec_varys=*/0, /*num_dims=*/3, dim,
      middle_varys, coda_array_ordering_c, /*num_elements=*/1,
      /*sparse_rec_method=*/0, /*has_compression=*/0, &mid_var));
  ASSERT_NE(nullptr, mid_dyn);
  ASSERT_NE(nullptr, mid_var);
  EXPECT_EQ(1, mid_var->num_records);
  EXPECT_EQ(3, mid_var->num_values_per_record);

  int num_dims = -1;
  long actual_dims[CODA_MAX_NUM_DIMS] = {};
  ASSERT_EQ(CODA_SUCCESS, coda_type_get_array_dim(
                              reinterpret_cast<coda_type*>(mid_var->definition),
                              &num_dims, actual_dims));
  EXPECT_EQ(1, num_dims);
  EXPECT_EQ(3, actual_dims[0]);

  // 3. rec_varys == 1 with all dim_varys == 0 -> 1D array of num_records.
  coda_cdf_variable* rec_only_var = nullptr;
  UniqueCdfType<coda_dynamic_type> rec_only_dyn(coda_cdf_variable_new(
      /*data_type=*/4, /*max_rec=*/2, /*rec_varys=*/1, /*num_dims=*/3, dim,
      all_zero_varys, coda_array_ordering_c, /*num_elements=*/1,
      /*sparse_rec_method=*/0, /*has_compression=*/0, &rec_only_var));
  ASSERT_NE(nullptr, rec_only_dyn);
  ASSERT_NE(nullptr, rec_only_var);
  EXPECT_EQ(3, rec_only_var->num_records);
  EXPECT_EQ(1, rec_only_var->num_values_per_record);
  ASSERT_EQ(CODA_SUCCESS, coda_type_get_array_dim(reinterpret_cast<coda_type*>(
                                                      rec_only_var->definition),
                                                  &num_dims, actual_dims));
  EXPECT_EQ(1, num_dims);
  EXPECT_EQ(3, actual_dims[0]);
}

TEST(CodaCdfTypeTest, VariableNewRespectsCAndFortranArrayOrdering) {
  int32_t dim[CODA_MAX_NUM_DIMS] = {2, 3, 5, 0, 0, 0, 0, 0};
  int32_t dim_varys[CODA_MAX_NUM_DIMS] = {1, 0, 1, 0, 0, 0, 0, 0};

  // 1. C ordering preserves dimension order after record dimension: [5, 2, 5].
  coda_cdf_variable* c_var = nullptr;
  UniqueCdfType<coda_dynamic_type> c_dyn(coda_cdf_variable_new(
      /*data_type=*/22, /*max_rec=*/4, /*rec_varys=*/1, /*num_dims=*/3, dim,
      dim_varys, coda_array_ordering_c, /*num_elements=*/1,
      /*sparse_rec_method=*/0, /*has_compression=*/0, &c_var));
  ASSERT_NE(nullptr, c_dyn);
  EXPECT_EQ(5, c_var->num_records);
  EXPECT_EQ(10, c_var->num_values_per_record);

  int num_dims = -1;
  long c_dims[CODA_MAX_NUM_DIMS] = {};
  ASSERT_EQ(CODA_SUCCESS, coda_type_get_array_dim(
                              reinterpret_cast<coda_type*>(c_var->definition),
                              &num_dims, c_dims));
  ASSERT_EQ(3, num_dims);
  EXPECT_THAT(std::span<const long>(c_dims, 3), ElementsAre(5, 2, 5));

  // 2. Fortran ordering reverses non-record dimensions: [5, 5, 2].
  coda_cdf_variable* f_var = nullptr;
  UniqueCdfType<coda_dynamic_type> f_dyn(coda_cdf_variable_new(
      /*data_type=*/22, /*max_rec=*/4, /*rec_varys=*/1, /*num_dims=*/3, dim,
      dim_varys, coda_array_ordering_fortran, /*num_elements=*/1,
      /*sparse_rec_method=*/0, /*has_compression=*/0, &f_var));
  ASSERT_NE(nullptr, f_dyn);
  EXPECT_EQ(5, f_var->num_records);
  EXPECT_EQ(10, f_var->num_values_per_record);

  long f_dims[CODA_MAX_NUM_DIMS] = {};
  ASSERT_EQ(CODA_SUCCESS, coda_type_get_array_dim(
                              reinterpret_cast<coda_type*>(f_var->definition),
                              &num_dims, f_dims));
  ASSERT_EQ(3, num_dims);
  EXPECT_THAT(std::span<const long>(f_dims, 3), ElementsAre(5, 5, 2));
}

TEST(CodaCdfTypeTest, VariableNewPreservesSparseAndCompressionFlags) {
  int32_t dim[CODA_MAX_NUM_DIMS] = {};
  int32_t dim_varys[CODA_MAX_NUM_DIMS] = {};

  for (int sparse : {0, 1, 2}) {
    for (int compressed : {0, 1}) {
      coda_cdf_variable* variable = nullptr;
      UniqueCdfType<coda_dynamic_type> dyn(coda_cdf_variable_new(
          /*data_type=*/4, /*max_rec=*/1, /*rec_varys=*/1, /*num_dims=*/0, dim,
          dim_varys, coda_array_ordering_c, /*num_elements=*/1, sparse,
          compressed, &variable));
      ASSERT_NE(nullptr, dyn);
      ASSERT_NE(nullptr, variable);
      EXPECT_EQ(sparse, variable->sparse_rec_method);
      EXPECT_EQ(compressed, variable->has_compression);
      EXPECT_EQ(nullptr, variable->data);
    }
  }
}

TEST(CodaCdfTypeTest, VariableNewRejectsInvalidTypesAndDimensions) {
  const auto reset_err =
      absl::MakeCleanup([] { coda_set_error(CODA_SUCCESS, nullptr); });
  int32_t dim[CODA_MAX_NUM_DIMS] = {};
  int32_t dim_varys[CODA_MAX_NUM_DIMS] = {};

  // 1. Invalid data_type (including EPOCH16 = 32) on scalar variable.
  for (int32_t invalid_type : {0, 32, 999}) {
    coda_set_error(CODA_SUCCESS, nullptr);
    coda_cdf_variable* variable = nullptr;
    EXPECT_EQ(nullptr,
              coda_cdf_variable_new(
                  invalid_type, /*max_rec=*/0, /*rec_varys=*/0, /*num_dims=*/0,
                  dim, dim_varys, coda_array_ordering_c, /*num_elements=*/1,
                  /*sparse_rec_method=*/0, /*has_compression=*/0, &variable));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
    EXPECT_EQ(nullptr, variable);
  }

  // 2. Invalid data_type on non-scalar variable (exercises basic_type_new
  // fail).
  coda_set_error(CODA_SUCCESS, nullptr);
  coda_cdf_variable* variable = nullptr;
  EXPECT_EQ(nullptr, coda_cdf_variable_new(
                         /*data_type=*/32, /*max_rec=*/1, /*rec_varys=*/1,
                         /*num_dims=*/0, dim, dim_varys, coda_array_ordering_c,
                         /*num_elements=*/1, /*sparse_rec_method=*/0,
                         /*has_compression=*/0, &variable));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // 3. Negative num_elements on CHAR triggers coda_type_set_byte_size() error.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr, coda_cdf_variable_new(
                         /*data_type=*/51, /*max_rec=*/0, /*rec_varys=*/0,
                         /*num_dims=*/0, dim, dim_varys, coda_array_ordering_c,
                         /*num_elements=*/-1, /*sparse_rec_method=*/0,
                         /*has_compression=*/0, &variable));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // 4. rec_varys=1 with max_rec=-2 fails when adding record dimension (0-1=-1).
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr, coda_cdf_variable_new(
                         /*data_type=*/4, /*max_rec=*/-2, /*rec_varys=*/1,
                         /*num_dims=*/0, dim, dim_varys, coda_array_ordering_c,
                         /*num_elements=*/1, /*sparse_rec_method=*/0,
                         /*has_compression=*/0, &variable));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // 5. Negative varying array dimension fails in dimension addition loop.
  coda_set_error(CODA_SUCCESS, nullptr);
  dim[0] = -1;
  dim_varys[0] = 1;
  EXPECT_EQ(nullptr, coda_cdf_variable_new(
                         /*data_type=*/4, /*max_rec=*/0, /*rec_varys=*/0,
                         /*num_dims=*/1, dim, dim_varys, coda_array_ordering_c,
                         /*num_elements=*/1, /*sparse_rec_method=*/0,
                         /*has_compression=*/0, &variable));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // 6. Exceeding CODA_MAX_NUM_DIMS (record dim + 8 varying dims = 9 total).
  coda_set_error(CODA_SUCCESS, nullptr);
  for (int i = 0; i < CODA_MAX_NUM_DIMS; ++i) {
    dim[i] = 2;
    dim_varys[i] = 1;
  }
  EXPECT_EQ(nullptr,
            coda_cdf_variable_new(
                /*data_type=*/4, /*max_rec=*/0, /*rec_varys=*/1,
                CODA_MAX_NUM_DIMS, dim, dim_varys, coda_array_ordering_c,
                /*num_elements=*/1, /*sparse_rec_method=*/0,
                /*has_compression=*/0, &variable));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
}

TEST(CodaCdfTypeTest, VariableAddAttributeSuccessAndDuplicateHandling) {
  UniqueCodaType<coda_type_text> attr_str_def(MakeTextDef(coda_format_cdf));
  ASSERT_NE(nullptr, attr_str_def);
  UniqueCodaType<coda_type_number> attr_int_def(MakeInt32Def(coda_format_cdf));
  ASSERT_NE(nullptr, attr_int_def);

  coda_product dummy_product{};
  const auto free_dummy_mem =
      absl::MakeCleanup([&] { std::free(dummy_product.mem_ptr); });

  int32_t dim[CODA_MAX_NUM_DIMS] = {};
  int32_t dim_varys[CODA_MAX_NUM_DIMS] = {};
  coda_cdf_variable* variable = nullptr;
  UniqueCdfType<coda_dynamic_type> var_dyn(coda_cdf_variable_new(
      /*data_type=*/4, /*max_rec=*/0, /*rec_varys=*/0, /*num_dims=*/0, dim,
      dim_varys, coda_array_ordering_c, /*num_elements=*/1,
      /*sparse_rec_method=*/0, /*has_compression=*/0, &variable));
  ASSERT_NE(nullptr, var_dyn);

  // 1. Add first attribute with update_definition=1 (creates definition
  // record).
  UniqueMemType<coda_mem_data> units_attr(
      coda_mem_string_new(attr_str_def.get(), nullptr, &dummy_product, "m/s"));
  ASSERT_NE(nullptr, units_attr);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cdf_variable_add_attribute(
                variable, "units",
                reinterpret_cast<coda_dynamic_type*>(units_attr.get()),
                /*update_definition=*/1));
  units_attr.release();

  // 2. Add second distinct attribute with update_definition=1.
  UniqueMemType<coda_mem_data> scale_attr(
      coda_mem_int32_new(attr_int_def.get(), nullptr, &dummy_product, 10));
  ASSERT_NE(nullptr, scale_attr);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cdf_variable_add_attribute(
                variable, "scale",
                reinterpret_cast<coda_dynamic_type*>(scale_attr.get()),
                /*update_definition=*/1));
  scale_attr.release();

  // 3. Add duplicate real_name with update_definition=1 -> disambiguated field.
  UniqueMemType<coda_mem_data> dup_units_attr(
      coda_mem_string_new(attr_str_def.get(), nullptr, &dummy_product, "km/s"));
  ASSERT_NE(nullptr, dup_units_attr);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cdf_variable_add_attribute(
                variable, "units",
                reinterpret_cast<coda_dynamic_type*>(dup_units_attr.get()),
                /*update_definition=*/1));
  dup_units_attr.release();

  ASSERT_NE(nullptr, variable->attributes);
  EXPECT_EQ(3, variable->attributes->num_fields);
  EXPECT_EQ(3, variable->definition->attributes->num_fields);
  EXPECT_STREQ("units", variable->definition->attributes->field[0]->name);
  EXPECT_STREQ("scale", variable->definition->attributes->field[1]->name);
  EXPECT_STREQ("units", variable->definition->attributes->field[2]->real_name);
}

TEST(CodaCdfTypeTest, VariableAddAttributePrepopulatedDefinitionBranches) {
  UniqueCodaType<coda_type_text> slot1_def(MakeTextDef(coda_format_cdf));
  UniqueCodaType<coda_type_number> slot2_def(MakeInt32Def(coda_format_cdf));
  UniqueCodaType<coda_type_text> wrong_def(MakeTextDef(coda_format_cdf));
  ASSERT_NE(nullptr, slot1_def);
  ASSERT_NE(nullptr, slot2_def);
  ASSERT_NE(nullptr, wrong_def);

  UniqueCodaType<coda_type_record> attr_rec_def(
      coda_type_record_new(coda_format_cdf));
  ASSERT_NE(nullptr, attr_rec_def);
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_create_field(
                              attr_rec_def.get(), "pre1",
                              reinterpret_cast<coda_type*>(slot1_def.get())));
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_create_field(
                              attr_rec_def.get(), "pre2",
                              reinterpret_cast<coda_type*>(slot2_def.get())));

  coda_product dummy_product{};
  const auto free_dummy_mem =
      absl::MakeCleanup([&] { std::free(dummy_product.mem_ptr); });

  int32_t dim[CODA_MAX_NUM_DIMS] = {};
  int32_t dim_varys[CODA_MAX_NUM_DIMS] = {};
  coda_cdf_variable* variable = nullptr;
  UniqueCdfType<coda_dynamic_type> var_dyn(coda_cdf_variable_new(
      /*data_type=*/4, /*max_rec=*/0, /*rec_varys=*/0, /*num_dims=*/0, dim,
      dim_varys, coda_array_ordering_c, /*num_elements=*/1,
      /*sparse_rec_method=*/0, /*has_compression=*/0, &variable));
  ASSERT_NE(nullptr, var_dyn);

  ASSERT_EQ(CODA_SUCCESS, coda_type_set_attributes(reinterpret_cast<coda_type*>(
                                                       variable->definition),
                                                   attr_rec_def.get()));

  const auto reset_err =
      absl::MakeCleanup([] { coda_set_error(CODA_SUCCESS, nullptr); });

  // 1. type->attributes == NULL with update_definition=0 fails even when
  //    definition->attributes is populated.
  UniqueMemType<coda_mem_data> attr1(
      coda_mem_string_new(slot1_def.get(), nullptr, &dummy_product, "v1"));
  ASSERT_NE(nullptr, attr1);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cdf_variable_add_attribute(
                    variable, "pre1",
                    reinterpret_cast<coda_dynamic_type*>(attr1.get()),
                    /*update_definition=*/0));
  EXPECT_EQ(CODA_ERROR_INVALID_NAME, coda_errno);

  // 2. update_definition=1 with existing definition->attributes reuses the
  //    existing definition record and fills empty slot "pre1" without realloc.
  ASSERT_EQ(CODA_SUCCESS, coda_cdf_variable_add_attribute(
                              variable, "pre1",
                              reinterpret_cast<coda_dynamic_type*>(attr1.get()),
                              /*update_definition=*/1));
  attr1.release();
  ASSERT_NE(nullptr, variable->attributes);
  EXPECT_EQ(2, variable->attributes->num_fields);

  // 3. Now type->attributes != NULL: test update_definition=0 error paths.
  UniqueMemType<coda_mem_data> wrong_attr(
      coda_mem_string_new(wrong_def.get(), nullptr, &dummy_product, "bad"));
  ASSERT_NE(nullptr, wrong_attr);

  // 3a. Missing attribute name.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cdf_variable_add_attribute(
                    variable, "missing_attr",
                    reinterpret_cast<coda_dynamic_type*>(wrong_attr.get()),
                    /*update_definition=*/0));
  EXPECT_EQ(CODA_ERROR_INVALID_NAME, coda_errno);

  // 3b. Attribute already set ("pre1").
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cdf_variable_add_attribute(
                    variable, "pre1",
                    reinterpret_cast<coda_dynamic_type*>(wrong_attr.get()),
                    /*update_definition=*/0));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // 3c. Incompatible type for unfilled slot ("pre2" expects slot2_def).
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cdf_variable_add_attribute(
                    variable, "pre2",
                    reinterpret_cast<coda_dynamic_type*>(wrong_attr.get()),
                    /*update_definition=*/0));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // 4. Matching type with update_definition=0 fills "pre2".
  UniqueMemType<coda_mem_data> attr2(
      coda_mem_int32_new(slot2_def.get(), nullptr, &dummy_product, 99));
  ASSERT_NE(nullptr, attr2);
  ASSERT_EQ(CODA_SUCCESS, coda_cdf_variable_add_attribute(
                              variable, "pre2",
                              reinterpret_cast<coda_dynamic_type*>(attr2.get()),
                              /*update_definition=*/0));
  attr2.release();
  EXPECT_NE(nullptr, variable->attributes->field_type[1]);
}

TEST(CodaCdfTypeTest, VariableAddAttributeRejectsInvalidArguments) {
  const auto reset_err =
      absl::MakeCleanup([] { coda_set_error(CODA_SUCCESS, nullptr); });

  UniqueCodaType<coda_type_text> cdf_str_def(MakeTextDef(coda_format_cdf));
  UniqueCodaType<coda_type_text> netcdf_str_def(
      MakeTextDef(coda_format_netcdf));
  ASSERT_NE(nullptr, cdf_str_def);
  ASSERT_NE(nullptr, netcdf_str_def);

  UniqueMemType<coda_mem_data> cdf_attr(
      coda_mem_string_new(cdf_str_def.get(), nullptr, nullptr, nullptr));
  UniqueMemType<coda_mem_data> netcdf_attr(
      coda_mem_string_new(netcdf_str_def.get(), nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, cdf_attr);
  ASSERT_NE(nullptr, netcdf_attr);

  int32_t dim[CODA_MAX_NUM_DIMS] = {};
  int32_t dim_varys[CODA_MAX_NUM_DIMS] = {};
  coda_cdf_variable* variable = nullptr;
  UniqueCdfType<coda_dynamic_type> var_dyn(coda_cdf_variable_new(
      /*data_type=*/4, /*max_rec=*/0, /*rec_varys=*/0, /*num_dims=*/0, dim,
      dim_varys, coda_array_ordering_c, /*num_elements=*/1,
      /*sparse_rec_method=*/0, /*has_compression=*/0, &variable));
  ASSERT_NE(nullptr, var_dyn);

  // 1. NULL type argument.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cdf_variable_add_attribute(
                    nullptr, "attr",
                    reinterpret_cast<coda_dynamic_type*>(cdf_attr.get()), 1));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // 2. NULL real_name argument.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cdf_variable_add_attribute(
                    variable, nullptr,
                    reinterpret_cast<coda_dynamic_type*>(cdf_attr.get()), 1));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // 3. NULL attribute_type argument.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cdf_variable_add_attribute(variable, "attr", nullptr, 1));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // 4. Non-memory backend on variable->attributes is rejected.
  coda_dynamic_type non_mem_attrs{};
  non_mem_attrs.backend = coda_backend_cdf;
  variable->attributes = reinterpret_cast<coda_mem_record*>(&non_mem_attrs);
  {
    const auto restore_attrs =
        absl::MakeCleanup([&] { variable->attributes = nullptr; });
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cdf_variable_add_attribute(
                      variable, "attr",
                      reinterpret_cast<coda_dynamic_type*>(cdf_attr.get()), 1));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  }

  // 5. Mismatched format in attribute_type causes coda_type_record_create_field
  //    to fail inside coda_cdf_variable_add_attribute.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cdf_variable_add_attribute(
                    variable, "bad_fmt",
                    reinterpret_cast<coda_dynamic_type*>(netcdf_attr.get()),
                    /*update_definition=*/1));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
}

TEST(CodaCdfTypeTest, TypeDeleteHandlesEmptyTimeAndVariableStructs) {
  // 1. tag_cdf_time with base_type == NULL and definition == NULL.
  auto* empty_time =
      static_cast<coda_cdf_time*>(std::malloc(sizeof(coda_cdf_time)));
  ASSERT_NE(nullptr, empty_time);
  empty_time->backend = coda_backend_cdf;
  empty_time->definition = nullptr;
  empty_time->tag = tag_cdf_time;
  empty_time->base_type = nullptr;
  empty_time->data_type = 31;
  coda_cdf_type_delete(reinterpret_cast<coda_dynamic_type*>(empty_time));

  // 2. tag_cdf_variable with all optional pointers NULL.
  auto* empty_var =
      static_cast<coda_cdf_variable*>(std::malloc(sizeof(coda_cdf_variable)));
  ASSERT_NE(nullptr, empty_var);
  empty_var->backend = coda_backend_cdf;
  empty_var->definition = nullptr;
  empty_var->tag = tag_cdf_variable;
  empty_var->attributes = nullptr;
  empty_var->base_type = nullptr;
  empty_var->num_records = 0;
  empty_var->num_values_per_record = 0;
  empty_var->value_size = 0;
  empty_var->sparse_rec_method = 0;
  empty_var->has_compression = 0;
  empty_var->offset = nullptr;
  empty_var->data = nullptr;
  coda_cdf_type_delete(reinterpret_cast<coda_dynamic_type*>(empty_var));
}

TEST(CodaCdfTypeTest, EndToEndSynthesizedCdfExercisesTypesAttributesAndClose) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // Build a CDF v3 file with compressed data, scalar/array time variables,
  // Fortran ordering, text, and variable attributes so coda_open() +
  // coda_close() exercise all coda-cdf-type.c paths end-to-end.
  CdfBuilder builder;
  builder.SetArrayOrdering(CdfArrayOrdering::kColumnMajor);

  builder.AddZVar({
      .name = "compressed_i32",
      .data_type = CdfDataType::kInt4,
      .rec_varys = true,
      .dims = {2},
      .dim_varys = {1},
      .has_compression = true,
      .compress_data = true,
      .data = CdfBuilder::EncodeValues<int32_t>({10, 20, 30, 40},
                                                /*big_endian=*/true),
  });
  builder.AddZVar({
      .name = "epoch_scalar",
      .data_type = CdfDataType::kEpoch,
      .rec_varys = false,
      // 2000-01-01T00:00:00.000 in CDF_EPOCH ms = 63113904000000.0.
      .data = CdfBuilder::EncodeValue<double>(63113904000000.0,
                                              /*big_endian=*/true),
  });
  builder.AddZVar({
      .name = "tt2000_arr",
      .data_type = CdfDataType::kTimeTt2000,
      .rec_varys = true,
      // 43200e9 ns evaluates to 0.0 s in CODA's TT2000 expression
      // (float(.) * 1e-9 - 43200.0).
      .data = CdfBuilder::EncodeValues<int64_t>({43200000000000LL},
                                                /*big_endian=*/true),
  });
  builder.AddZVar({
      .name = "fortran_mat",
      .data_type = CdfDataType::kReal8,
      .rec_varys = false,
      .dims = {2, 3},
      .dim_varys = {1, 1},
      .data = CdfBuilder::EncodeValues<double>({1.0, 2.0, 3.0, 4.0, 5.0, 6.0},
                                               /*big_endian=*/true),
  });
  builder.AddZVar({
      .name = "label_char",
      .data_type = CdfDataType::kChar,
      .num_elems = 1,
      .rec_varys = false,
      .data = {'Z'},
  });
  builder.AddAttr({
      .name = "units",
      .scope = CdfAttrScope::kVariable,
      .entries = {CdfAttrEntrySpec::String(/*entry_num=*/0, "counts",
                                           /*is_z_entry=*/true)},
  });

  TempFile cdf_file("coda_cdf_type_e2e.cdf");
  ASSERT_TRUE(cdf_file.WriteBytes(builder.Build()));

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(cdf_file.c_str(), product.OutParam()));

  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

  // 1. Verify compressed variable decodes (sets variable->data) and has attr.
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "compressed_i32"));
  std::array<int32_t, 4> i32_vals = {};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(&cursor, i32_vals.data(),
                                                       coda_array_ordering_c));
  EXPECT_THAT(i32_vals, ElementsAre(10, 20, 30, 40));

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "units"));
  long unit_len = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &unit_len));
  ASSERT_EQ(6, unit_len);
  std::string unit_buf(static_cast<size_t>(unit_len) + 1, '\0');
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&cursor, unit_buf.data(),
                                    static_cast<long>(unit_buf.size())));
  EXPECT_STREQ("counts", unit_buf.c_str());
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));

  // 2. Verify scalar EPOCH special time converts to 0.0 s since 2000-01-01.
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "epoch_scalar"));
  coda_type_class tclass = coda_record_class;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cursor, &tclass));
  EXPECT_EQ(coda_special_class, tclass);
  double epoch_sec = -1.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &epoch_sec));
  EXPECT_DOUBLE_EQ(0.0, epoch_sec);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));

  // 3. Verify TT2000 array element converts to 86400.0 - 43200.0 = 0.0 s.
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "tt2000_arr"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&cursor, 0));
  double tt2000_sec = -1.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &tt2000_sec));
  EXPECT_DOUBLE_EQ(0.0, tt2000_sec);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));

  // 4. Verify Fortran ordering reverses [2, 3] to [3, 2].
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "fortran_mat"));
  int num_dims = -1;
  long dims[CODA_MAX_NUM_DIMS] = {};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_array_dim(&cursor, &num_dims, dims));
  ASSERT_EQ(2, num_dims);
  EXPECT_THAT(std::span<const long>(dims, 2), ElementsAre(3, 2));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));

  // 5. Verify CHAR(1) maps to coda_native_type_char.
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "label_char"));
  coda_type* char_type = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type(&cursor, &char_type));
  coda_native_type char_read_type = coda_native_type_not_available;
  ASSERT_EQ(CODA_SUCCESS, coda_type_get_read_type(char_type, &char_read_type));
  EXPECT_EQ(coda_native_type_char, char_read_type);
  char ch = '\0';
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char(&cursor, &ch));
  EXPECT_EQ('Z', ch);
}

TEST(CodaCdfTypeTest, OpenMissingTruncatedAndEpoch16CdfFailGracefully) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const auto reset_err =
      absl::MakeCleanup([] { coda_set_error(CODA_SUCCESS, nullptr); });

  // 1. Missing CDF file fails with CODA_ERROR_FILE_NOT_FOUND.
  const std::filesystem::path missing_path =
      std::filesystem::path(::testing::TempDir()) / "nonexistent_cdf_type.cdf";
  ScopedProduct product;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(missing_path.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);

  // 2. Truncated CDF header fails with CODA_ERROR_OUT_OF_BOUNDS_READ.
  TempFile truncated("truncated_cdf_type.cdf");
  constexpr std::array<uint8_t, 12> kTruncatedCdf = {
      0xCD, 0xF3, 0x00, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
  ASSERT_TRUE(truncated.WriteBytes(kTruncatedCdf));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(truncated.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);

  // 3. EPOCH16 variable in CDF file fails with CODA_ERROR_UNSUPPORTED_PRODUCT.
  CdfBuilder epoch16_builder;
  epoch16_builder.AddZVar({
      .name = "unsupported_epoch16",
      .data_type = CdfDataType::kEpoch16,
      .rec_varys = false,
      .data = std::vector<uint8_t>(16, 0U),
  });
  TempFile epoch16_file("epoch16_cdf_type.cdf");
  ASSERT_TRUE(epoch16_file.WriteBytes(epoch16_builder.Build()));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(epoch16_file.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr("EPOCH16"));
}

// TODO(schwehr): 88.6% reachable ceiling in libcoda/coda-cdf-type.c (271/306);
// remaining 35 missed lines are 26 malloc/strdup/calloc OOM branches, 1
// unreachable default assert(0)/exit(1), and 8 unreachable setter guards on
// freshly allocated CODA type objects.

}  // namespace
}  // namespace stcorp_coda
