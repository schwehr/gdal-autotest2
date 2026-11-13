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
// Unit tests for HARP variables (libharp/harp-variable.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-variable.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-internal.h
//   https://github.com/stcorp/harp/blob/master/libharp/harp.h.in

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

std::string* g_print_buffer = nullptr;

int CapturePrint(const char* fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (g_print_buffer != nullptr && n > 0) {
    g_print_buffer->append(buf, static_cast<size_t>(n) < sizeof(buf)
                                    ? static_cast<size_t>(n)
                                    : sizeof(buf) - 1);
  }
  return n;
}

class HarpVariableTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_EQ(HARP_SUCCESS, init_.status()); }
  void TearDown() override { harp_set_error(HARP_SUCCESS, nullptr); }

 private:
  ScopedHarpInit init_;
};

ScopedHarpVariable MakeVar(const char* name, harp_data_type data_type,
                           int num_dims, const harp_dimension_type* dim_types,
                           const long* dims) {
  harp_variable* raw = nullptr;
  EXPECT_EQ(HARP_SUCCESS, harp_variable_new(name, data_type, num_dims,
                                            dim_types, dims, &raw));
  return ScopedHarpVariable(raw);
}

void FillNumericSequence(harp_variable* var, double start, double step) {
  for (long i = 0; i < var->num_elements; ++i) {
    const double val = start + static_cast<double>(i) * step;
    switch (var->data_type) {
      case harp_type_int8:
        var->data.int8_data[i] = static_cast<int8_t>(val);
        break;
      case harp_type_int16:
        var->data.int16_data[i] = static_cast<int16_t>(val);
        break;
      case harp_type_int32:
        var->data.int32_data[i] = static_cast<int32_t>(val);
        break;
      case harp_type_float:
        var->data.float_data[i] = static_cast<float>(val);
        break;
      case harp_type_double:
        var->data.double_data[i] = val;
        break;
      case harp_type_string:
        break;
    }
  }
}

double GetNumericElement(const harp_variable* var, long index) {
  switch (var->data_type) {
    case harp_type_int8:
      return static_cast<double>(var->data.int8_data[index]);
    case harp_type_int16:
      return static_cast<double>(var->data.int16_data[index]);
    case harp_type_int32:
      return static_cast<double>(var->data.int32_data[index]);
    case harp_type_float:
      return static_cast<double>(var->data.float_data[index]);
    case harp_type_double:
      return var->data.double_data[index];
    case harp_type_string:
      return 0.0;
  }
  return 0.0;
}

void SetScalarBounds(harp_variable* var, double min_val, double max_val) {
  switch (var->data_type) {
    case harp_type_int8:
      var->valid_min.int8_data = static_cast<int8_t>(min_val);
      var->valid_max.int8_data = static_cast<int8_t>(max_val);
      break;
    case harp_type_int16:
      var->valid_min.int16_data = static_cast<int16_t>(min_val);
      var->valid_max.int16_data = static_cast<int16_t>(max_val);
      break;
    case harp_type_int32:
      var->valid_min.int32_data = static_cast<int32_t>(min_val);
      var->valid_max.int32_data = static_cast<int32_t>(max_val);
      break;
    case harp_type_float:
      var->valid_min.float_data = static_cast<float>(min_val);
      var->valid_max.float_data = static_cast<float>(max_val);
      break;
    case harp_type_double:
      var->valid_min.double_data = min_val;
      var->valid_max.double_data = max_val;
      break;
    case harp_type_string:
      break;
  }
}

double GetScalarMin(const harp_variable* var) {
  switch (var->data_type) {
    case harp_type_int8:
      return static_cast<double>(var->valid_min.int8_data);
    case harp_type_int16:
      return static_cast<double>(var->valid_min.int16_data);
    case harp_type_int32:
      return static_cast<double>(var->valid_min.int32_data);
    case harp_type_float:
      return static_cast<double>(var->valid_min.float_data);
    case harp_type_double:
      return var->valid_min.double_data;
    case harp_type_string:
      return 0.0;
  }
  return 0.0;
}

double GetScalarMax(const harp_variable* var) {
  switch (var->data_type) {
    case harp_type_int8:
      return static_cast<double>(var->valid_max.int8_data);
    case harp_type_int16:
      return static_cast<double>(var->valid_max.int16_data);
    case harp_type_int32:
      return static_cast<double>(var->valid_max.int32_data);
    case harp_type_float:
      return static_cast<double>(var->valid_max.float_data);
    case harp_type_double:
      return var->valid_max.double_data;
    case harp_type_string:
      return 0.0;
  }
  return 0.0;
}

TEST_F(HarpVariableTest, NewAndDeleteAllTypesAndDimensions) {
  harp_variable_delete(nullptr);

  constexpr harp_data_type kAllTypes[] = {
      harp_type_int8,  harp_type_int16,  harp_type_int32,
      harp_type_float, harp_type_double, harp_type_string,
  };

  for (harp_data_type dt : kAllTypes) {
    // Scalar variable (num_dimensions = 0).
    ScopedHarpVariable scalar = MakeVar("scalar_v", dt, 0, nullptr, nullptr);
    ASSERT_NE(nullptr, scalar);
    EXPECT_STREQ("scalar_v", scalar->name);
    EXPECT_EQ(dt, scalar->data_type);
    EXPECT_EQ(0, scalar->num_dimensions);
    EXPECT_EQ(1L, scalar->num_elements);
    EXPECT_NE(nullptr, scalar->data.ptr);
    if (dt != harp_type_string) {
      EXPECT_EQ(1, harp_is_valid_min_for_type(dt, scalar->valid_min));
      EXPECT_EQ(1, harp_is_valid_max_for_type(dt, scalar->valid_max));
    }

    // Multi-dimensional with NULL dimension_type -> defaults to independent.
    constexpr long kDims[] = {2, 3};
    ScopedHarpVariable arr = MakeVar("arr_v", dt, 2, nullptr, kDims);
    ASSERT_NE(nullptr, arr);
    EXPECT_EQ(2, arr->num_dimensions);
    EXPECT_EQ(6L, arr->num_elements);
    EXPECT_EQ(harp_dimension_independent, arr->dimension_type[0]);
    EXPECT_EQ(harp_dimension_independent, arr->dimension_type[1]);
  }

  // Explicit dimension types including repeated latitude with matching lengths.
  constexpr harp_dimension_type kDimTypes[] = {
      harp_dimension_time,
      harp_dimension_latitude,
      harp_dimension_independent,
      harp_dimension_latitude,
  };
  constexpr long kDimLens[] = {2, 3, 4, 3};
  ScopedHarpVariable v =
      MakeVar("valid_dims", harp_type_double, 4, kDimTypes, kDimLens);
  ASSERT_NE(nullptr, v);
  EXPECT_EQ(72L, v->num_elements);

  // Full cleanup of string data, description, unit, and enum_name on delete.
  ScopedHarpVariable str_var =
      MakeVar("str_v", harp_type_string, 1, kDimTypes, kDimLens);
  ASSERT_NE(nullptr, str_var);
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(str_var.get(), 0, "hello"));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_description(str_var.get(), "desc"));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(str_var.get(), "m"));
}

TEST_F(HarpVariableTest, NewErrorPaths) {
  harp_variable* raw = nullptr;

  EXPECT_EQ(-1,
            harp_variable_new("", harp_type_double, 0, nullptr, nullptr, &raw));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(nullptr, raw);

  EXPECT_EQ(-1, harp_variable_new("1bad", harp_type_double, 0, nullptr, nullptr,
                                  &raw));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  // Time dimension not at index 0.
  constexpr harp_dimension_type kBadTimeTypes[] = {
      harp_dimension_latitude,
      harp_dimension_time,
  };
  constexpr long kDims[] = {2, 3};
  EXPECT_EQ(-1, harp_variable_new("bad_time", harp_type_double, 2,
                                  kBadTimeTypes, kDims, &raw));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  // Inconsistent lengths for repeated non-independent dimension type.
  constexpr harp_dimension_type kDupLatTypes[] = {
      harp_dimension_latitude,
      harp_dimension_latitude,
  };
  EXPECT_EQ(-1, harp_variable_new("bad_lat", harp_type_double, 2, kDupLatTypes,
                                  kDims, &raw));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
}

TEST_F(HarpVariableTest, CopyAndCopyAttributes) {
  constexpr harp_dimension_type kDimTypes[] = {harp_dimension_time};
  constexpr long kDims[] = {3};
  ScopedHarpVariable src =
      MakeVar("flag_var", harp_type_int8, 1, kDimTypes, kDims);
  ASSERT_NE(nullptr, src);
  FillNumericSequence(src.get(), 0.0, 1.0);
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_description(src.get(), "quality flag"));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(src.get(), "1"));
  const char* kEnumNames[] = {"good", "warn", "bad"};
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_enumeration_values(src.get(), 3, kEnumNames));

  harp_variable* copy_raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_copy(src.get(), &copy_raw));
  ScopedHarpVariable copy(copy_raw);
  EXPECT_STREQ("flag_var", copy->name);
  EXPECT_STREQ("quality flag", copy->description);
  EXPECT_STREQ("1", copy->unit);
  ASSERT_EQ(3, copy->num_enum_values);
  EXPECT_STREQ("good", copy->enum_name[0]);
  EXPECT_STREQ("bad", copy->enum_name[2]);
  EXPECT_EQ(0, copy->valid_min.int8_data);
  EXPECT_EQ(2, copy->valid_max.int8_data);
  EXPECT_EQ(2, copy->data.int8_data[2]);

  // Copy variable with NULL optional attributes.
  ScopedHarpVariable bare =
      MakeVar("bare", harp_type_float, 1, kDimTypes, kDims);
  ASSERT_NE(nullptr, bare);
  harp_variable* bare_copy_raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_copy(bare.get(), &bare_copy_raw));
  ScopedHarpVariable bare_copy(bare_copy_raw);
  EXPECT_EQ(nullptr, bare_copy->description);
  EXPECT_EQ(nullptr, bare_copy->unit);
  EXPECT_EQ(0, bare_copy->num_enum_values);

  // Copy string variable (verifies deep copy of strings).
  ScopedHarpVariable str_src =
      MakeVar("labels", harp_type_string, 1, kDimTypes, kDims);
  ASSERT_NE(nullptr, str_src);
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(str_src.get(), 0, "alpha"));
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(str_src.get(), 1, "beta"));
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(str_src.get(), 2, "gamma"));
  harp_variable* str_copy_raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_copy(str_src.get(), &str_copy_raw));
  ScopedHarpVariable str_copy(str_copy_raw);
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(str_src.get(), 0, "changed"));
  EXPECT_STREQ("alpha", str_copy->data.string_data[0]);
  EXPECT_STREQ("beta", str_copy->data.string_data[1]);
  EXPECT_STREQ("gamma", str_copy->data.string_data[2]);

  // harp_variable_copy_attributes success & error paths.
  ScopedHarpVariable int_target =
      MakeVar("target_i", harp_type_int16, 1, kDimTypes, kDims);
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_copy_attributes(src.get(), int_target.get()));
  EXPECT_STREQ("quality flag", int_target->description);
  EXPECT_STREQ("1", int_target->unit);
  EXPECT_EQ(3, int_target->num_enum_values);

  ScopedHarpVariable dbl_target =
      MakeVar("target_d", harp_type_double, 1, kDimTypes, kDims);
  EXPECT_EQ(-1, harp_variable_copy_attributes(src.get(), dbl_target.get()));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  // Invalid unit on manually constructed source triggers copy_attributes error.
  char* saved_unit = src->unit;
  char bad_unit[] = "not_a_real_unit_xyz";
  src->unit = bad_unit;
  auto restore_unit = absl::MakeCleanup([&] { src->unit = saved_unit; });
  EXPECT_EQ(-1, harp_variable_copy_attributes(src.get(), int_target.get()));
  EXPECT_EQ(HARP_ERROR_INVALID_VARIABLE, harp_errno);
}

TEST_F(HarpVariableTest, RenameSetDescriptionSetUnitAndHasUnit) {
  ScopedHarpVariable v = MakeVar("orig", harp_type_double, 0, nullptr, nullptr);
  ASSERT_NE(nullptr, v);

  EXPECT_EQ(-1, harp_variable_rename(nullptr, "x"));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(-1, harp_variable_rename(v.get(), nullptr));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  ASSERT_EQ(HARP_SUCCESS, harp_variable_rename(v.get(), "renamed"));
  EXPECT_STREQ("renamed", v->name);

  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_description(v.get(), "first"));
  EXPECT_STREQ("first", v->description);
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_description(v.get(), "second"));
  EXPECT_STREQ("second", v->description);
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_description(v.get(), nullptr));
  EXPECT_EQ(nullptr, v->description);

  EXPECT_EQ(0, harp_variable_has_unit(v.get(), "m"));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(v.get(), "W"));
  EXPECT_STREQ("W", v->unit);
  EXPECT_EQ(1, harp_variable_has_unit(v.get(), "W"));
  EXPECT_EQ(1, harp_variable_has_unit(v.get(), "J/s"));
  EXPECT_EQ(0, harp_variable_has_unit(v.get(), "m"));

  EXPECT_EQ(-1, harp_variable_set_unit(v.get(), "bogus_unit_name"));
  EXPECT_EQ(HARP_ERROR_INVALID_VARIABLE, harp_errno);
  EXPECT_STREQ("W", v->unit);

  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(v.get(), "m"));
  EXPECT_STREQ("m", v->unit);
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(v.get(), nullptr));
  EXPECT_EQ(nullptr, v->unit);
}

TEST_F(HarpVariableTest, EnumerationValuesAndFlagAttributes) {
  ScopedHarpVariable v8 = MakeVar("f8", harp_type_int8, 0, nullptr, nullptr);
  ScopedHarpVariable v16 = MakeVar("f16", harp_type_int16, 0, nullptr, nullptr);
  ScopedHarpVariable v32 = MakeVar("f32", harp_type_int32, 0, nullptr, nullptr);
  ScopedHarpVariable vf = MakeVar("ff", harp_type_float, 0, nullptr, nullptr);

  char* out = nullptr;
  // Error paths before enums are set.
  EXPECT_EQ(-1, harp_variable_get_flag_values_string(v8.get(), &out));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(-1, harp_variable_get_flag_meanings_string(v8.get(), &out));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  const char* kTwo[] = {"clear", "cloudy"};
  EXPECT_EQ(-1, harp_variable_set_enumeration_values(v8.get(), -1, kTwo));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(-1, harp_variable_set_enumeration_values(v8.get(), 2, nullptr));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  const char* kHasNull[] = {"clear", nullptr};
  EXPECT_EQ(-1, harp_variable_set_enumeration_values(v8.get(), 2, kHasNull));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  const char* kBadId[] = {"clear", "bad-name"};
  EXPECT_EQ(-1, harp_variable_set_enumeration_values(v8.get(), 2, kBadId));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(-1, harp_variable_set_enumeration_values(vf.get(), 2, kTwo));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  // Set for int8, int16, int32.
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_enumeration_values(v8.get(), 2, kTwo));
  EXPECT_EQ(0, v8->valid_min.int8_data);
  EXPECT_EQ(1, v8->valid_max.int8_data);

  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_enumeration_values(v16.get(), 2, kTwo));
  EXPECT_EQ(0, v16->valid_min.int16_data);
  EXPECT_EQ(1, v16->valid_max.int16_data);

  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_enumeration_values(v32.get(), 2, kTwo));
  EXPECT_EQ(0, v32->valid_min.int32_data);
  EXPECT_EQ(1, v32->valid_max.int32_data);

  // Non-int8 fails get_flag_values_string / get_flag_meanings_string.
  EXPECT_EQ(-1, harp_variable_get_flag_values_string(v16.get(), &out));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(-1, harp_variable_get_flag_meanings_string(v16.get(), &out));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  // <= 10 enum values.
  {
    char* vals = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_get_flag_values_string(v8.get(), &vals));
    ScopedMallocChar vals_guard(vals);
    EXPECT_STREQ("0b, 1b", vals);

    char* meanings = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_get_flag_meanings_string(v8.get(), &meanings));
    ScopedMallocChar meanings_guard(meanings);
    EXPECT_STREQ("clear cloudy", meanings);
  }

  // > 10 enum values (e.g., 12) and > 100 enum values (e.g., 105 < 128).
  {
    std::vector<std::string> names12;
    names12.reserve(12);
    std::vector<const char*> ptrs12;
    ptrs12.reserve(12);
    for (int i = 0; i < 12; ++i) {
      names12.push_back("v_" + std::to_string(i));
    }
    for (const auto& s : names12) {
      ptrs12.push_back(s.c_str());
    }
    ASSERT_EQ(HARP_SUCCESS, harp_variable_set_enumeration_values(
                                v8.get(), 12, ptrs12.data()));
    char* vals = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_get_flag_values_string(v8.get(), &vals));
    ScopedMallocChar vals_guard(vals);
    EXPECT_THAT(vals, HasSubstr("0b, 1b"));
    EXPECT_THAT(vals, HasSubstr("10b, 11b"));
  }
  {
    std::vector<std::string> names105;
    names105.reserve(105);
    std::vector<const char*> ptrs105;
    ptrs105.reserve(105);
    for (int i = 0; i < 105; ++i) {
      names105.push_back("e_" + std::to_string(i));
    }
    for (const auto& s : names105) {
      ptrs105.push_back(s.c_str());
    }
    ASSERT_EQ(HARP_SUCCESS, harp_variable_set_enumeration_values(
                                v8.get(), 105, ptrs105.data()));
    char* vals = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_get_flag_values_string(v8.get(), &vals));
    ScopedMallocChar vals_guard(vals);
    EXPECT_THAT(vals, HasSubstr("99b, 100b"));
    EXPECT_THAT(vals, HasSubstr("103b, 104b"));
  }

  // Empty enum name triggers error in get_flag_meanings_string.
  {
    char* saved_name = v8->enum_name[0];
    char empty_str[] = "";
    v8->enum_name[0] = empty_str;
    auto restore = absl::MakeCleanup([&] { v8->enum_name[0] = saved_name; });
    EXPECT_EQ(-1, harp_variable_get_flag_meanings_string(v8.get(), &out));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }

  // Clear enumeration values.
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_enumeration_values(v8.get(), 0, nullptr));
  EXPECT_EQ(0, v8->num_enum_values);
  EXPECT_EQ(nullptr, v8->enum_name);

  // Set using flag_meanings with extra spaces, and failure with bad label.
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_enumeration_values_using_flag_meanings(
                v8.get(), "  good   marginal   bad  "));
  ASSERT_EQ(3, v8->num_enum_values);
  EXPECT_STREQ("good", v8->enum_name[0]);
  EXPECT_STREQ("marginal", v8->enum_name[1]);
  EXPECT_STREQ("bad", v8->enum_name[2]);

  EXPECT_EQ(-1, harp_variable_set_enumeration_values_using_flag_meanings(
                    v8.get(), "good 123invalid"));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
}

TEST_F(HarpVariableTest, SetStringDataElement) {
  constexpr long kDims[] = {2};
  ScopedHarpVariable str_var =
      MakeVar("s", harp_type_string, 1, nullptr, kDims);
  ScopedHarpVariable int_var = MakeVar("i", harp_type_int32, 1, nullptr, kDims);

  EXPECT_EQ(-1, harp_variable_set_string_data_element(nullptr, 0, "a"));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(-1, harp_variable_set_string_data_element(int_var.get(), 0, "a"));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(-1, harp_variable_set_string_data_element(str_var.get(), -1, "a"));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(-1,
            harp_variable_set_string_data_element(str_var.get(), 0, nullptr));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(str_var.get(), 0, "first"));
  EXPECT_STREQ("first", str_var->data.string_data[0]);
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(str_var.get(), 0, "updated"));
  EXPECT_STREQ("updated", str_var->data.string_data[0]);
}

TEST_F(HarpVariableTest, AppendVariables) {
  constexpr harp_dimension_type kDimTypes[] = {
      harp_dimension_time,
      harp_dimension_independent,
  };
  constexpr long kDimsA[] = {2, 2};
  constexpr long kDimsB[] = {1, 2};

  ScopedHarpVariable a = MakeVar("x", harp_type_double, 2, kDimTypes, kDimsA);
  ScopedHarpVariable b = MakeVar("x", harp_type_double, 2, kDimTypes, kDimsB);
  FillNumericSequence(a.get(), 1.0, 1.0);
  FillNumericSequence(b.get(), 5.0, 1.0);
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(a.get(), "m"));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(b.get(), "meter"));

  ASSERT_EQ(HARP_SUCCESS, harp_variable_append(a.get(), b.get()));
  EXPECT_EQ(3L, a->dimension[0]);
  EXPECT_EQ(6L, a->num_elements);
  EXPECT_DOUBLE_EQ(1.0, a->data.double_data[0]);
  EXPECT_DOUBLE_EQ(4.0, a->data.double_data[3]);
  EXPECT_DOUBLE_EQ(5.0, a->data.double_data[4]);
  EXPECT_DOUBLE_EQ(6.0, a->data.double_data[5]);

  // Append string variables (both with NULL units).
  ScopedHarpVariable sa = MakeVar("s", harp_type_string, 1, kDimTypes, kDimsA);
  ScopedHarpVariable sb = MakeVar("s", harp_type_string, 1, kDimTypes, kDimsB);
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(sa.get(), 0, "a0"));
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(sa.get(), 1, "a1"));
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(sb.get(), 0, "b0"));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_append(sa.get(), sb.get()));
  EXPECT_EQ(3L, sa->num_elements);
  EXPECT_STREQ("a0", sa->data.string_data[0]);
  EXPECT_STREQ("a1", sa->data.string_data[1]);
  EXPECT_STREQ("b0", sa->data.string_data[2]);

  // Error cases:
  ScopedHarpVariable diff_name =
      MakeVar("y", harp_type_double, 2, kDimTypes, kDimsB);
  EXPECT_EQ(-1, harp_variable_append(a.get(), diff_name.get()));

  ScopedHarpVariable diff_type =
      MakeVar("x", harp_type_float, 2, kDimTypes, kDimsB);
  EXPECT_EQ(-1, harp_variable_append(a.get(), diff_type.get()));

  // Unit mismatches: non-NULL vs NULL, NULL vs non-NULL, incompatible units.
  ScopedHarpVariable no_unit =
      MakeVar("x", harp_type_double, 2, kDimTypes, kDimsB);
  EXPECT_EQ(-1, harp_variable_append(a.get(), no_unit.get()));
  EXPECT_EQ(-1, harp_variable_append(no_unit.get(), b.get()));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(no_unit.get(), "s"));
  EXPECT_EQ(-1, harp_variable_append(a.get(), no_unit.get()));

  // Dimension count mismatch.
  ScopedHarpVariable diff_ndims =
      MakeVar("x", harp_type_double, 1, kDimTypes, kDimsB);
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(diff_ndims.get(), "m"));
  EXPECT_EQ(-1, harp_variable_append(a.get(), diff_ndims.get()));

  // Enum count mismatch.
  const char* kEnums[] = {"e0"};
  ScopedHarpVariable ea = MakeVar("e", harp_type_int8, 1, kDimTypes, kDimsB);
  ScopedHarpVariable eb = MakeVar("e", harp_type_int8, 1, kDimTypes, kDimsB);
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_enumeration_values(eb.get(), 1, kEnums));
  EXPECT_EQ(-1, harp_variable_append(ea.get(), eb.get()));

  // Non-time-dependent (scalar or latitude first).
  ScopedHarpVariable scalar =
      MakeVar("sc", harp_type_double, 0, nullptr, nullptr);
  EXPECT_EQ(-1, harp_variable_append(scalar.get(), scalar.get()));
  constexpr harp_dimension_type kLatFirst[] = {harp_dimension_latitude};
  ScopedHarpVariable lat_v =
      MakeVar("lv", harp_type_double, 1, kLatFirst, kDimsB);
  EXPECT_EQ(-1, harp_variable_append(lat_v.get(), lat_v.get()));

  // Inner dimension type mismatch and inner dimension length mismatch.
  constexpr harp_dimension_type kDiffInnerType[] = {
      harp_dimension_time,
      harp_dimension_spectral,
  };
  ScopedHarpVariable inner_type =
      MakeVar("x", harp_type_double, 2, kDiffInnerType, kDimsB);
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(inner_type.get(), "m"));
  EXPECT_EQ(-1, harp_variable_append(a.get(), inner_type.get()));

  constexpr long kDiffInnerLen[] = {1, 3};
  ScopedHarpVariable inner_len =
      MakeVar("x", harp_type_double, 2, kDimTypes, kDiffInnerLen);
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(inner_len.get(), "m"));
  EXPECT_EQ(-1, harp_variable_append(a.get(), inner_len.get()));
}

TEST_F(HarpVariableTest, ConvertDataTypeAllPairsAndClamping) {
  constexpr harp_data_type kNumericTypes[] = {
      harp_type_int8,  harp_type_int16,  harp_type_int32,
      harp_type_float, harp_type_double,
  };

  // Error cases and no-op / 0-element cases.
  EXPECT_EQ(-1, harp_variable_convert_data_type(nullptr, harp_type_double));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  ScopedHarpVariable sv = MakeVar("sv", harp_type_string, 0, nullptr, nullptr);
  EXPECT_EQ(-1, harp_variable_convert_data_type(sv.get(), harp_type_double));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  ScopedHarpVariable dv = MakeVar("dv", harp_type_double, 0, nullptr, nullptr);
  EXPECT_EQ(HARP_SUCCESS,
            harp_variable_convert_data_type(dv.get(), harp_type_double));
  EXPECT_EQ(-1, harp_variable_convert_data_type(dv.get(), harp_type_string));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  // Zero-element variable converts type without allocating data.
  {
    ScopedHarpVariable empty =
        MakeVar("empty", harp_type_int16, 0, nullptr, nullptr);
    empty->num_elements = 0;
    std::free(empty->data.ptr);
    empty->data.ptr = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_convert_data_type(empty.get(), harp_type_double));
    EXPECT_EQ(harp_type_double, empty->data_type);
  }

  // Test all 20 (src != dst) numeric conversion pairs in both clamping modes:
  // default valid_min/valid_max (wide -> clamped when narrowing) and narrow
  // valid_min=-5, valid_max=5 (preserved when narrowing).
  constexpr long kDims[] = {3};
  for (harp_data_type src_type : kNumericTypes) {
    for (harp_data_type dst_type : kNumericTypes) {
      if (src_type == dst_type) continue;
      for (bool narrow_bounds : {false, true}) {
        ScopedHarpVariable v = MakeVar("conv", src_type, 1, nullptr, kDims);
        ASSERT_NE(nullptr, v);
        FillNumericSequence(v.get(), -3.0, 3.0);  // -3, 0, 3 (fits int8).
        if (narrow_bounds) {
          SetScalarBounds(v.get(), -5.0, 5.0);
        }
        ASSERT_EQ(HARP_SUCCESS,
                  harp_variable_convert_data_type(v.get(), dst_type));
        EXPECT_EQ(dst_type, v->data_type);
        EXPECT_DOUBLE_EQ(-3.0, GetNumericElement(v.get(), 0));
        EXPECT_DOUBLE_EQ(0.0, GetNumericElement(v.get(), 1));
        EXPECT_DOUBLE_EQ(3.0, GetNumericElement(v.get(), 2));
        if (narrow_bounds) {
          EXPECT_DOUBLE_EQ(-5.0, GetScalarMin(v.get()));
          EXPECT_DOUBLE_EQ(5.0, GetScalarMax(v.get()));
        }
      }
    }
  }
}

TEST_F(HarpVariableTest, HasDimensionTypeAndHasDimensionTypes) {
  constexpr harp_dimension_type kDimTypes[] = {
      harp_dimension_time,
      harp_dimension_vertical,
  };
  constexpr long kDims[] = {2, 4};
  ScopedHarpVariable v =
      MakeVar("profile", harp_type_double, 2, kDimTypes, kDims);
  ASSERT_NE(nullptr, v);

  EXPECT_EQ(1, harp_variable_has_dimension_type(v.get(), harp_dimension_time));
  EXPECT_EQ(1,
            harp_variable_has_dimension_type(v.get(), harp_dimension_vertical));
  EXPECT_EQ(0,
            harp_variable_has_dimension_type(v.get(), harp_dimension_spectral));

  EXPECT_EQ(1, harp_variable_has_dimension_types(v.get(), 2, kDimTypes));
  EXPECT_EQ(0, harp_variable_has_dimension_types(v.get(), 1, kDimTypes));
  constexpr harp_dimension_type kWrongTypes[] = {
      harp_dimension_time,
      harp_dimension_latitude,
  };
  EXPECT_EQ(0, harp_variable_has_dimension_types(v.get(), 2, kWrongTypes));

  ScopedHarpVariable scalar = MakeVar("s", harp_type_int8, 0, nullptr, nullptr);
  EXPECT_EQ(1, harp_variable_has_dimension_types(scalar.get(), 0, nullptr));
}

TEST_F(HarpVariableTest, RearrangeDimension) {
  constexpr long kDims[] = {2, 4, 2};
  ScopedHarpVariable v = MakeVar("r", harp_type_int32, 3, nullptr, kDims);
  ASSERT_NE(nullptr, v);
  FillNumericSequence(v.get(), 0.0, 1.0);

  // Error cases.
  constexpr long kIdentity4[] = {0, 1, 2, 3};
  EXPECT_EQ(-1, harp_variable_rearrange_dimension(nullptr, 0, 1, kIdentity4));
  EXPECT_EQ(-1, harp_variable_rearrange_dimension(v.get(), 1, 0, kIdentity4));
  EXPECT_EQ(-1, harp_variable_rearrange_dimension(v.get(), 1, 1, nullptr));
  EXPECT_EQ(-1, harp_variable_rearrange_dimension(v.get(), -1, 1, kIdentity4));
  EXPECT_EQ(-1, harp_variable_rearrange_dimension(v.get(), 3, 1, kIdentity4));
  constexpr long kNegId[] = {-1};
  constexpr long kTooBigId[] = {4};
  EXPECT_EQ(-1, harp_variable_rearrange_dimension(v.get(), 1, 1, kNegId));
  EXPECT_EQ(-1, harp_variable_rearrange_dimension(v.get(), 1, 1, kTooBigId));

  // Zero-element variable error.
  {
    ScopedHarpVariable empty =
        MakeVar("empty", harp_type_int32, 1, nullptr, kDims);
    empty->num_elements = 0;
    EXPECT_EQ(-1,
              harp_variable_rearrange_dimension(empty.get(), 0, 1, kIdentity4));
  }

  // Identity no-op.
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_rearrange_dimension(v.get(), 1, 4, kIdentity4));
  EXPECT_EQ(4L, v->dimension[1]);

  // 4-cycle [1, 2, 3, 0] with num_groups=2, num_block_elements=2.
  constexpr long kCycle4[] = {1, 2, 3, 0};
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_rearrange_dimension(v.get(), 1, 4, kCycle4));
  // Group 0 originally [0,1 | 2,3 | 4,5 | 6,7] -> [2,3 | 4,5 | 6,7 | 0,1].
  EXPECT_EQ(2, v->data.int32_data[0]);
  EXPECT_EQ(0, v->data.int32_data[6]);
  // Group 1 originally [8,9 | 10,11 | 12,13 | 14,15] -> [10,11 | ... | 8,9].
  EXPECT_EQ(10, v->data.int32_data[8]);
  EXPECT_EQ(8, v->data.int32_data[14]);

  // Fixed point + swap [0, 2, 1, 3].
  constexpr long kSwapMiddle[] = {0, 2, 1, 3};
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_rearrange_dimension(v.get(), 1, 4, kSwapMiddle));
  EXPECT_EQ(2, v->data.int32_data[0]);
  EXPECT_EQ(6, v->data.int32_data[2]);
  EXPECT_EQ(4, v->data.int32_data[4]);

  // Duplicate-after-move [1, 1, 0, 2] (tests moved[from_id] branches).
  constexpr long kDupAfterMove[] = {1, 1, 0, 2};
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_rearrange_dimension(v.get(), 1, 4, kDupAfterMove));
  EXPECT_EQ(6, v->data.int32_data[0]);
  EXPECT_EQ(6, v->data.int32_data[2]);
  EXPECT_EQ(2, v->data.int32_data[4]);
  EXPECT_EQ(4, v->data.int32_data[6]);

  // Growing with duplicates on string variable (non-NULL and NULL strings).
  constexpr long kStrDims[] = {2, 3};
  ScopedHarpVariable sv = MakeVar("sv", harp_type_string, 2, nullptr, kStrDims);
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(sv.get(), 0, "g0_0"));
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(sv.get(), 1, "g0_1"));
  // Leave index 2 as nullptr; group 1 has non-NULL strings.
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(sv.get(), 3, "g1_0"));
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(sv.get(), 4, "g1_1"));
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(sv.get(), 5, "g1_2"));

  constexpr long kGrowIds[] = {1, 1, 0, 2};
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_rearrange_dimension(sv.get(), 1, 4, kGrowIds));
  EXPECT_EQ(4L, sv->dimension[1]);
  EXPECT_STREQ("g0_1", sv->data.string_data[0]);
  EXPECT_STREQ("g0_1", sv->data.string_data[1]);
  EXPECT_STREQ("g0_0", sv->data.string_data[2]);
  EXPECT_EQ(nullptr, sv->data.string_data[3]);
  EXPECT_STREQ("g1_1", sv->data.string_data[4]);
  EXPECT_STREQ("g1_1", sv->data.string_data[5]);

  // Shrinking string variable frees discarded strings cleanly.
  constexpr long kShrinkIds[] = {2, 0};
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_rearrange_dimension(sv.get(), 1, 2, kShrinkIds));
  EXPECT_EQ(2L, sv->dimension[1]);
  EXPECT_EQ(4L, sv->num_elements);
  EXPECT_STREQ("g0_0", sv->data.string_data[0]);
  EXPECT_STREQ("g0_1", sv->data.string_data[1]);
  EXPECT_STREQ("g1_0", sv->data.string_data[2]);
  EXPECT_STREQ("g1_1", sv->data.string_data[3]);
}

TEST_F(HarpVariableTest, FilterDimension) {
  constexpr long kDims[] = {2, 3, 2};
  ScopedHarpVariable v = MakeVar("f", harp_type_double, 3, nullptr, kDims);
  FillNumericSequence(v.get(), 0.0, 1.0);

  constexpr uint8_t kAllOnes[] = {1, 1, 1};
  constexpr uint8_t kAllZeros[] = {0, 0, 0};
  constexpr uint8_t kMask[] = {0, 1, 0};

  EXPECT_EQ(-1, harp_variable_filter_dimension(nullptr, 1, kMask));
  EXPECT_EQ(-1, harp_variable_filter_dimension(v.get(), 1, nullptr));
  EXPECT_EQ(-1, harp_variable_filter_dimension(v.get(), -1, kMask));
  EXPECT_EQ(-1, harp_variable_filter_dimension(v.get(), 3, kMask));
  EXPECT_EQ(-1, harp_variable_filter_dimension(v.get(), 1, kAllZeros));

  {
    ScopedHarpVariable empty =
        MakeVar("empty", harp_type_double, 1, nullptr, kDims);
    empty->num_elements = 0;
    EXPECT_EQ(-1, harp_variable_filter_dimension(empty.get(), 0, kMask));
  }

  // All-ones mask is a no-op.
  ASSERT_EQ(HARP_SUCCESS, harp_variable_filter_dimension(v.get(), 1, kAllOnes));
  EXPECT_EQ(3L, v->dimension[1]);

  // Filter middle dimension with num_groups = 2, num_block_elements = 2.
  ASSERT_EQ(HARP_SUCCESS, harp_variable_filter_dimension(v.get(), 1, kMask));
  EXPECT_EQ(1L, v->dimension[1]);
  EXPECT_EQ(4L, v->num_elements);
  EXPECT_DOUBLE_EQ(2.0, v->data.double_data[0]);
  EXPECT_DOUBLE_EQ(3.0, v->data.double_data[1]);
  EXPECT_DOUBLE_EQ(8.0, v->data.double_data[2]);
  EXPECT_DOUBLE_EQ(9.0, v->data.double_data[3]);

  // Filter string variable frees discarded strings.
  constexpr long kStrDims[] = {2, 2};
  ScopedHarpVariable sv = MakeVar("sf", harp_type_string, 2, nullptr, kStrDims);
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(sv.get(), 0, "drop0"));
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(sv.get(), 1, "keep0"));
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(sv.get(), 2, "drop1"));
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(sv.get(), 3, "keep1"));
  constexpr uint8_t kMask2[] = {0, 1};
  ASSERT_EQ(HARP_SUCCESS, harp_variable_filter_dimension(sv.get(), 1, kMask2));
  EXPECT_EQ(2L, sv->num_elements);
  EXPECT_STREQ("keep0", sv->data.string_data[0]);
  EXPECT_STREQ("keep1", sv->data.string_data[1]);
}

TEST_F(HarpVariableTest, ResizeDimensionAllTypes) {
  constexpr long kErrorDims[] = {2, 3};
  ScopedHarpVariable err_v =
      MakeVar("err", harp_type_int32, 2, nullptr, kErrorDims);
  EXPECT_EQ(-1, harp_variable_resize_dimension(nullptr, 0, 1));
  EXPECT_EQ(-1, harp_variable_resize_dimension(err_v.get(), -1, 1));
  EXPECT_EQ(-1, harp_variable_resize_dimension(err_v.get(), 2, 1));
  EXPECT_EQ(-1, harp_variable_resize_dimension(err_v.get(), 0, 0));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_resize_dimension(err_v.get(), 0, 2));

  // Expand [2, 1, 2] -> [2, 3, 2] for all 6 types (num_blocks=2,
  // block_elems=2).
  constexpr harp_data_type kAllTypes[] = {
      harp_type_int8,  harp_type_int16,  harp_type_int32,
      harp_type_float, harp_type_double, harp_type_string,
  };
  constexpr long kInitDims[] = {2, 1, 2};
  for (harp_data_type dt : kAllTypes) {
    ScopedHarpVariable v = MakeVar("rz", dt, 3, nullptr, kInitDims);
    ASSERT_NE(nullptr, v);
    if (dt == harp_type_string) {
      ASSERT_EQ(HARP_SUCCESS,
                harp_variable_set_string_data_element(v.get(), 0, "a"));
      ASSERT_EQ(HARP_SUCCESS,
                harp_variable_set_string_data_element(v.get(), 1, "b"));
      ASSERT_EQ(HARP_SUCCESS,
                harp_variable_set_string_data_element(v.get(), 2, "c"));
      ASSERT_EQ(HARP_SUCCESS,
                harp_variable_set_string_data_element(v.get(), 3, "d"));
    } else {
      FillNumericSequence(v.get(), 1.0, 1.0);
    }

    ASSERT_EQ(HARP_SUCCESS, harp_variable_resize_dimension(v.get(), 1, 3));
    EXPECT_EQ(3L, v->dimension[1]);
    EXPECT_EQ(12L, v->num_elements);

    if (dt == harp_type_string) {
      EXPECT_STREQ("a", v->data.string_data[0]);
      EXPECT_STREQ("b", v->data.string_data[1]);
      EXPECT_EQ(nullptr, v->data.string_data[2]);
      EXPECT_STREQ("c", v->data.string_data[6]);
      EXPECT_STREQ("d", v->data.string_data[7]);
      EXPECT_EQ(nullptr, v->data.string_data[8]);

      // Put strings in trailing slots then shrink back to 1 to test freeing.
      ASSERT_EQ(HARP_SUCCESS,
                harp_variable_set_string_data_element(v.get(), 3, "drop1"));
      ASSERT_EQ(HARP_SUCCESS,
                harp_variable_set_string_data_element(v.get(), 9, "drop2"));
      ASSERT_EQ(HARP_SUCCESS, harp_variable_resize_dimension(v.get(), 1, 1));
      EXPECT_EQ(4L, v->num_elements);
      EXPECT_STREQ("a", v->data.string_data[0]);
      EXPECT_STREQ("c", v->data.string_data[2]);
    } else {
      EXPECT_DOUBLE_EQ(1.0, GetNumericElement(v.get(), 0));
      EXPECT_DOUBLE_EQ(2.0, GetNumericElement(v.get(), 1));
      EXPECT_DOUBLE_EQ(3.0, GetNumericElement(v.get(), 6));
      EXPECT_DOUBLE_EQ(4.0, GetNumericElement(v.get(), 7));
      if (dt == harp_type_float || dt == harp_type_double) {
        EXPECT_EQ(1, harp_isnan(GetNumericElement(v.get(), 2)));
        EXPECT_EQ(1, harp_isnan(GetNumericElement(v.get(), 8)));
      } else {
        EXPECT_DOUBLE_EQ(0.0, GetNumericElement(v.get(), 2));
        EXPECT_DOUBLE_EQ(0.0, GetNumericElement(v.get(), 8));
      }

      ASSERT_EQ(HARP_SUCCESS, harp_variable_resize_dimension(v.get(), 1, 1));
      EXPECT_EQ(4L, v->num_elements);
      EXPECT_DOUBLE_EQ(3.0, GetNumericElement(v.get(), 2));
    }
  }
}

TEST_F(HarpVariableTest, AddRemoveAndSquashDimension) {
  // Error paths for add_dimension.
  ScopedHarpVariable v = MakeVar("x", harp_type_double, 0, nullptr, nullptr);
  v->data.double_data[0] = 42.0;

  EXPECT_EQ(-1,
            harp_variable_add_dimension(nullptr, 0, harp_dimension_time, 1));
  EXPECT_EQ(-1,
            harp_variable_add_dimension(v.get(), -1, harp_dimension_time, 1));
  EXPECT_EQ(-1,
            harp_variable_add_dimension(v.get(), 1, harp_dimension_time, 1));
  EXPECT_EQ(-1,
            harp_variable_add_dimension(v.get(), 0, harp_dimension_time, 0));

  // Zero-element variable cannot add dimension.
  {
    ScopedHarpVariable empty =
        MakeVar("empty", harp_type_double, 0, nullptr, nullptr);
    empty->num_elements = 0;
    EXPECT_EQ(-1, harp_variable_add_dimension(empty.get(), 0,
                                              harp_dimension_independent, 2));
  }

  // Build up to HARP_MAX_NUM_DIMS (8) and check limit error + time rules.
  ASSERT_EQ(HARP_SUCCESS, harp_variable_add_dimension(
                              v.get(), 0, harp_dimension_latitude, 2));
  EXPECT_EQ(-1,
            harp_variable_add_dimension(v.get(), 1, harp_dimension_time, 2));
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_add_dimension(v.get(), 0, harp_dimension_time, 2));
  EXPECT_EQ(-1,
            harp_variable_add_dimension(v.get(), 0, harp_dimension_time, 2));
  EXPECT_EQ(
      -1, harp_variable_add_dimension(v.get(), 2, harp_dimension_latitude, 5));
  // Matching length for existing non-independent dimension succeeds.
  ASSERT_EQ(HARP_SUCCESS, harp_variable_add_dimension(
                              v.get(), 2, harp_dimension_latitude, 2));

  while (v->num_dimensions < HARP_MAX_NUM_DIMS) {
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_add_dimension(v.get(), v->num_dimensions,
                                          harp_dimension_independent, 1));
  }
  EXPECT_EQ(-1, harp_variable_add_dimension(v.get(), 0,
                                            harp_dimension_independent, 1));

  // String variable add_dimension (with non-NULL and NULL), squash, remove.
  constexpr long kStrDims[] = {2};
  ScopedHarpVariable sv = MakeVar("sv", harp_type_string, 1, nullptr, kStrDims);
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(sv.get(), 0, "hi"));
  // Leave index 1 as nullptr.
  ASSERT_EQ(HARP_SUCCESS, harp_variable_add_dimension(
                              sv.get(), 1, harp_dimension_independent, 2));
  EXPECT_EQ(4L, sv->num_elements);
  EXPECT_STREQ("hi", sv->data.string_data[0]);
  EXPECT_STREQ("hi", sv->data.string_data[1]);
  EXPECT_EQ(nullptr, sv->data.string_data[2]);
  EXPECT_EQ(nullptr, sv->data.string_data[3]);

  // Make all elements non-NULL for squash_dimension (uses strcmp).
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(sv.get(), 2, "lo"));
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(sv.get(), 3, "lo"));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_squash_dimension(sv.get(), 1));
  EXPECT_EQ(1, sv->num_dimensions);
  EXPECT_STREQ("hi", sv->data.string_data[0]);
  EXPECT_STREQ("lo", sv->data.string_data[1]);

  // Differing values fail squash for both string and numeric.
  EXPECT_EQ(-1, harp_variable_squash_dimension(sv.get(), 0));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  // Remove dimension error propagation & success (both non-last and last dim).
  EXPECT_EQ(-1, harp_variable_remove_dimension(sv.get(), 5, 0));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_add_dimension(
                              sv.get(), 1, harp_dimension_independent, 2));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_remove_dimension(sv.get(), 0, 1));
  EXPECT_EQ(1, sv->num_dimensions);
  EXPECT_STREQ("lo", sv->data.string_data[0]);
  ASSERT_EQ(HARP_SUCCESS, harp_variable_remove_dimension(sv.get(), 0, 0));
  EXPECT_EQ(0, sv->num_dimensions);
  EXPECT_STREQ("lo", sv->data.string_data[0]);

  // Numeric squash: error cases, length-1 dimension, identical multi-group.
  EXPECT_EQ(-1, harp_variable_squash_dimension(nullptr, 0));
  ScopedHarpVariable nv = MakeVar("nv", harp_type_int32, 0, nullptr, nullptr);
  nv->data.int32_data[0] = 7;
  EXPECT_EQ(-1, harp_variable_squash_dimension(nv.get(), 0));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_add_dimension(
                              nv.get(), 0, harp_dimension_independent, 1));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_squash_dimension(nv.get(), 0));
  EXPECT_EQ(0, nv->num_dimensions);

  ASSERT_EQ(HARP_SUCCESS, harp_variable_add_dimension(
                              nv.get(), 0, harp_dimension_independent, 2));
  nv->data.int32_data[1] = 8;
  EXPECT_EQ(-1, harp_variable_squash_dimension(nv.get(), 0));
  nv->data.int32_data[1] = 7;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_squash_dimension(nv.get(), 0));
  EXPECT_EQ(0, nv->num_dimensions);
  EXPECT_EQ(7, nv->data.int32_data[0]);
}

TEST_F(HarpVariableTest, VerifyAllErrorAndSuccessPaths) {
  EXPECT_EQ(-1, harp_variable_verify(nullptr));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  constexpr harp_dimension_type kDimTypes[] = {
      harp_dimension_time,      harp_dimension_latitude,
      harp_dimension_longitude, harp_dimension_vertical,
      harp_dimension_spectral,  harp_dimension_independent,
  };
  constexpr long kDims[] = {1, 1, 1, 1, 1, 1};
  ScopedHarpVariable v =
      MakeVar("good_var", harp_type_int8, 6, kDimTypes, kDims);
  ASSERT_NE(nullptr, v);
  const char* kEnums[] = {"ok"};
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_enumeration_values(v.get(), 1, kEnums));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(v.get(), "1"));
  EXPECT_EQ(HARP_SUCCESS, harp_variable_verify(v.get()));

  // Verify every invalid field check by temporarily mutating and restoring.
  {
    char* orig = v->name;
    v->name = nullptr;
    EXPECT_EQ(-1, harp_variable_verify(v.get()));
    char bad_name[] = "9bad";
    v->name = bad_name;
    EXPECT_EQ(-1, harp_variable_verify(v.get()));
    v->name = orig;
  }
  {
    harp_data_type orig = v->data_type;
    v->data_type = static_cast<harp_data_type>(99);
    EXPECT_EQ(-1, harp_variable_verify(v.get()));
    v->data_type = orig;
  }
  {
    int orig = v->num_dimensions;
    v->num_dimensions = -1;
    EXPECT_EQ(-1, harp_variable_verify(v.get()));
    v->num_dimensions = HARP_MAX_NUM_DIMS + 1;
    EXPECT_EQ(-1, harp_variable_verify(v.get()));
    v->num_dimensions = orig;
  }
  {
    harp_dimension_type orig = v->dimension_type[1];
    v->dimension_type[1] = static_cast<harp_dimension_type>(99);
    EXPECT_EQ(-1, harp_variable_verify(v.get()));
    v->dimension_type[1] = orig;
  }
  {
    // Inner dimension time when dim 0 is not time.
    harp_dimension_type orig0 = v->dimension_type[0];
    harp_dimension_type orig1 = v->dimension_type[1];
    v->dimension_type[0] = harp_dimension_latitude;
    v->dimension_type[1] = harp_dimension_time;
    EXPECT_EQ(-1, harp_variable_verify(v.get()));
    v->dimension_type[0] = orig0;
    v->dimension_type[1] = orig1;
  }
  {
    long orig = v->dimension[1];
    v->dimension[1] = 0;
    EXPECT_EQ(-1, harp_variable_verify(v.get()));
    v->dimension[1] = orig;
  }
  {
    // Mismatched lengths for duplicate non-independent dimension type.
    harp_dimension_type orig_type = v->dimension_type[2];
    long orig_len = v->dimension[2];
    v->dimension_type[2] = harp_dimension_latitude;
    v->dimension[2] = 2;
    EXPECT_EQ(-1, harp_variable_verify(v.get()));
    v->dimension_type[2] = orig_type;
    v->dimension[2] = orig_len;
  }
  {
    long orig = v->num_elements;
    v->num_elements = -1;
    EXPECT_EQ(-1, harp_variable_verify(v.get()));
    v->num_elements = 99;
    EXPECT_EQ(-1, harp_variable_verify(v.get()));
    v->num_elements = orig;
  }
  {
    void* orig = v->data.ptr;
    v->data.ptr = nullptr;
    EXPECT_EQ(-1, harp_variable_verify(v.get()));
    v->data.ptr = orig;
  }
  {
    char* orig = v->unit;
    char bad_unit[] = "bad_unit_xyz";
    v->unit = bad_unit;
    EXPECT_EQ(-1, harp_variable_verify(v.get()));
    v->unit = orig;
  }
  {
    // Enum errors: NULL enum_name array, NULL element, invalid id.
    char** orig_enums = v->enum_name;
    v->enum_name = nullptr;
    EXPECT_EQ(-1, harp_variable_verify(v.get()));
    v->enum_name = orig_enums;

    char* orig_e0 = v->enum_name[0];
    v->enum_name[0] = nullptr;
    EXPECT_EQ(-1, harp_variable_verify(v.get()));
    char bad_e[] = "1bad";
    v->enum_name[0] = bad_e;
    EXPECT_EQ(-1, harp_variable_verify(v.get()));
    v->enum_name[0] = orig_e0;
  }

  // Float variable with enum values triggers data_type check in verify.
  {
    ScopedHarpVariable fv_enum =
        MakeVar("fv_enum", harp_type_float, 0, nullptr, nullptr);
    char enum_label[] = "ok";
    char* enum_arr[] = {enum_label};
    fv_enum->num_enum_values = 1;
    fv_enum->enum_name = enum_arr;
    auto clear_enum = absl::MakeCleanup([&] {
      fv_enum->num_enum_values = 0;
      fv_enum->enum_name = nullptr;
    });
    EXPECT_EQ(-1, harp_variable_verify(fv_enum.get()));
    EXPECT_EQ(HARP_ERROR_INVALID_VARIABLE, harp_errno);
  }

  // NaN valid_min / valid_max checks for float and double.
  for (harp_data_type dt : {harp_type_float, harp_type_double}) {
    ScopedHarpVariable fv = MakeVar("fv", dt, 0, nullptr, nullptr);
    SetScalarBounds(fv.get(), std::numeric_limits<double>::quiet_NaN(), 1.0);
    EXPECT_EQ(-1, harp_variable_verify(fv.get()));
    SetScalarBounds(fv.get(), 0.0, std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(-1, harp_variable_verify(fv.get()));
  }
}

TEST_F(HarpVariableTest, PrintAndPrintData) {
  std::string out;
  g_print_buffer = &out;
  auto clear_buf = absl::MakeCleanup([&] { g_print_buffer = nullptr; });

  harp_variable_print(nullptr, 1, CapturePrint);
  EXPECT_EQ("    NULL\n", out);

  // All 6 types with show_attributes=1, custom valid_min/valid_max, and
  // print_data in both 1D and 2D modes.
  constexpr harp_dimension_type kDimTypes[] = {
      harp_dimension_time,
      harp_dimension_independent,
  };
  constexpr long kDims2D[] = {2, 2};
  constexpr harp_data_type kAllTypes[] = {
      harp_type_int8,  harp_type_int16,  harp_type_int32,
      harp_type_float, harp_type_double, harp_type_string,
  };

  for (harp_data_type dt : kAllTypes) {
    out.clear();
    ScopedHarpVariable v = MakeVar("var", dt, 2, kDimTypes, kDims2D);
    ASSERT_EQ(HARP_SUCCESS, harp_variable_set_description(v.get(), "desc"));
    ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(v.get(), "m"));
    if (dt == harp_type_string) {
      for (long i = 0; i < 4; ++i) {
        ASSERT_EQ(HARP_SUCCESS,
                  harp_variable_set_string_data_element(v.get(), i, "str"));
      }
    } else {
      FillNumericSequence(v.get(), 1.0, 1.0);
      SetScalarBounds(v.get(), -5.0, 5.0);
    }

    harp_variable_print(v.get(), 0, CapturePrint);
    EXPECT_THAT(out, HasSubstr("var {time = 2, 2} [m]"));
    out.clear();
    harp_variable_print(v.get(), 1, CapturePrint);
    EXPECT_THAT(out, HasSubstr("description = \"desc\""));
    if (dt != harp_type_string) {
      EXPECT_THAT(out, HasSubstr("valid min = -5"));
      EXPECT_THAT(out, HasSubstr("valid max = 5"));
    }

    out.clear();
    harp_variable_print_data(v.get(), CapturePrint);
    EXPECT_THAT(out, HasSubstr("var = \n  "));
  }

  // Scalar print_data (num_dimensions == 0) + enum print branches (int8 all,
  // int16 with 1, 2, and 3 values).
  ScopedHarpVariable s8 = MakeVar("e8", harp_type_int8, 0, nullptr, nullptr);
  const char* kThree[] = {"a", "b", "c"};
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_enumeration_values(s8.get(), 3, kThree));
  out.clear();
  harp_variable_print(s8.get(), 1, CapturePrint);
  EXPECT_THAT(out, HasSubstr("enum = \"a\" (0), \"b\" (1), \"c\" (2)"));
  out.clear();
  harp_variable_print_data(s8.get(), CapturePrint);
  EXPECT_THAT(out, HasSubstr("e8 = 0"));

  ScopedHarpVariable s16 = MakeVar("e16", harp_type_int16, 0, nullptr, nullptr);
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_enumeration_values(s16.get(), 1, kThree));
  out.clear();
  harp_variable_print(s16.get(), 1, CapturePrint);
  EXPECT_THAT(out, HasSubstr("enum = \"a\" (0)\n"));

  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_enumeration_values(s16.get(), 2, kThree));
  out.clear();
  harp_variable_print(s16.get(), 1, CapturePrint);
  EXPECT_THAT(out, HasSubstr("enum = \"a\" (0), \"b\" (1)\n"));

  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_enumeration_values(s16.get(), 3, kThree));
  out.clear();
  harp_variable_print(s16.get(), 1, CapturePrint);
  EXPECT_THAT(out, HasSubstr("enum = \"a\" (0), ..., \"c\" (2)\n"));
}

}  // namespace
}  // namespace stcorp_harp
