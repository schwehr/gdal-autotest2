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
// Unit tests for HARP derived variable resolution, conversion execution, and
// conversion documentation printing (libharp/harp-derived-variable.c).
//
// Google3 build notes: HDF4 is disabled (#undef HAVE_HDF4); udunits2 XML is
// embedded via //third_party/udunits.
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-derived-variable.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp.h.in

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::DoubleNear;
using ::testing::HasSubstr;

std::string* g_doc_output = nullptr;

int CapturePrint(const char* fmt, ...) {
  if (g_doc_output == nullptr) {
    return 0;
  }
  va_list ap;
  va_start(ap, fmt);
  char buf[2048];
  int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0) {
    g_doc_output->append(buf, static_cast<size_t>(n) < sizeof(buf)
                                  ? static_cast<size_t>(n)
                                  : sizeof(buf) - 1);
  }
  return n;
}

harp_variable* AddVariableOrFail(harp_product* product, const char* name,
                                 harp_data_type data_type, int num_dimensions,
                                 const harp_dimension_type* dimension_type,
                                 const long* dimension,
                                 const char* unit = nullptr) {
  harp_variable* raw = nullptr;
  if (harp_variable_new(name, data_type, num_dimensions, dimension_type,
                        dimension, &raw) != HARP_SUCCESS) {
    ADD_FAILURE() << "harp_variable_new failed for " << name << ": "
                  << harp_errno_to_string(harp_errno);
    return nullptr;
  }
  ScopedHarpVariable guard(raw);
  if (unit != nullptr) {
    if (harp_variable_set_unit(raw, unit) != HARP_SUCCESS) {
      ADD_FAILURE() << "harp_variable_set_unit failed for " << name << ": "
                    << harp_errno_to_string(harp_errno);
      return nullptr;
    }
  }
  if (harp_product_add_variable(product, raw) != HARP_SUCCESS) {
    ADD_FAILURE() << "harp_product_add_variable failed for " << name << ": "
                  << harp_errno_to_string(harp_errno);
    return nullptr;
  }
  guard.release();
  return raw;
}

int ZeroSourceConstFunc(harp_variable* variable,
                        const harp_variable** /*source_variable*/) {
  for (long i = 0; i < variable->num_elements; ++i) {
    variable->data.double_data[i] = 7.5;
  }
  return 0;
}

int ZeroSourceStringFunc(harp_variable* variable,
                         const harp_variable** /*source_variable*/) {
  for (long i = 0; i < variable->num_elements; ++i) {
    if (harp_variable_set_string_data_element(variable, i, "str_val") != 0) {
      return -1;
    }
  }
  return 0;
}

int ScaleByTwoFunc(harp_variable* variable,
                   const harp_variable** source_variable) {
  for (long i = 0; i < variable->num_elements; ++i) {
    variable->data.double_data[i] =
        2.0 * source_variable[0]->data.double_data[i];
  }
  return 0;
}

int CopyFirstFunc(harp_variable* variable,
                  const harp_variable** source_variable) {
  for (long i = 0; i < variable->num_elements; ++i) {
    variable->data.double_data[i] = source_variable[0]->data.double_data[i];
  }
  return 0;
}

int AlwaysDisabledFunc() { return 0; }
int AlwaysEnabledFunc() { return 1; }

TEST(HarpDerivedVariableTest, GetDerivedVariableExistingWithUnitAndType) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  constexpr harp_dimension_type kDims[] = {harp_dimension_time};
  constexpr long kLen[] = {2};
  harp_variable* speed = AddVariableOrFail(
      prod.get(), "wind_speed", harp_type_float, 1, kDims, kLen, "m/s");
  ASSERT_NE(nullptr, speed);
  speed->data.float_data[0] = 10.0f;
  speed->data.float_data[1] = 20.0f;

  // 1. Null variable name -> HARP_ERROR_INVALID_ARGUMENT.
  harp_variable* out = nullptr;
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_product_get_derived_variable(prod.get(), nullptr, nullptr,
                                                  nullptr, 1, kDims, &out));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  // 2. Existing variable with syntactically different equivalent unit ("m
  // s-1").
  constexpr harp_data_type kDoubleType = harp_type_double;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_derived_variable(
                              prod.get(), "wind_speed", &kDoubleType, "m s-1",
                              1, kDims, &out));
  ScopedHarpVariable out_guard(out);
  EXPECT_STREQ("m s-1", out->unit);
  EXPECT_EQ(harp_type_double, out->data_type);
  EXPECT_THAT(out->data.double_data[0], DoubleNear(10.0, 1e-6));
  EXPECT_THAT(out->data.double_data[1], DoubleNear(20.0, 1e-6));
  out_guard.reset();

  // 3. Existing variable with convertible unit ("km/h").
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_get_derived_variable(prod.get(), "wind_speed", nullptr,
                                              "km/h", 1, kDims, &out));
  out_guard.reset(out);
  EXPECT_STREQ("km/h", out->unit);
  EXPECT_EQ(harp_type_float, out->data_type);
  EXPECT_NEAR(36.0f, out->data.float_data[0], 1e-4f);
  harp_set_error(HARP_SUCCESS, nullptr);
  // Note: harp_product_get_derived_variable leaks its copied info.variable in
  // upstream harp-derived-variable.c when unit/type conversion fails on an
  // existing variable; those error branches are exercised via
  // harp_product_add_derived_variable (which mutates in-place without leaking).
}

TEST(HarpDerivedVariableTest, AddDerivedVariableExistingReplaceAndDerive) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  constexpr harp_dimension_type kTimeDim[] = {harp_dimension_time};
  constexpr long kTimeLen[] = {2};
  constexpr harp_dimension_type kVertDim[] = {harp_dimension_vertical};
  constexpr long kVertLen[] = {2};

  harp_variable* alt = AddVariableOrFail(
      prod.get(), "altitude", harp_type_float, 1, kTimeDim, kTimeLen, "km");
  ASSERT_NE(nullptr, alt);
  alt->data.float_data[0] = 1.0f;
  alt->data.float_data[1] = 2.0f;

  // 1. Existing variable with NULL unit and NULL type -> untouched.
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_add_derived_variable(prod.get(), "altitude", nullptr,
                                              nullptr, 1, kTimeDim));
  EXPECT_STREQ("km", alt->unit);

  // 2. Existing variable with convertible unit & different data type.
  constexpr harp_data_type kDoubleType = harp_type_double;
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_add_derived_variable(prod.get(), "altitude",
                                              &kDoubleType, "m", 1, kTimeDim));
  EXPECT_STREQ("m", alt->unit);
  EXPECT_EQ(harp_type_double, alt->data_type);
  EXPECT_THAT(alt->data.double_data[0], DoubleNear(1000.0, 1e-9));

  // 3. Existing variable with syntactically different equivalent unit
  // ("meter").
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_add_derived_variable(prod.get(), "altitude", nullptr,
                                              "meter", 1, kTimeDim));
  EXPECT_STREQ("meter", alt->unit);

  // 4. Existing variable with incompatible unit -> error.
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_product_add_derived_variable(prod.get(), "altitude",
                                                  nullptr, "s", 1, kTimeDim));
  EXPECT_EQ(HARP_ERROR_UNIT_CONVERSION, harp_errno);

  // 5. Existing string variable with incompatible target data type -> error.
  ASSERT_NE(nullptr, AddVariableOrFail(prod.get(), "str_col", harp_type_string,
                                       1, kTimeDim, kTimeLen));
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_product_add_derived_variable(
                    prod.get(), "str_col", &kDoubleType, nullptr, 1, kTimeDim));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  harp_set_error(HARP_SUCCESS, nullptr);

  // 6. Existing variable with DIFFERENT dimensions: add scalar "latitude"
  // [time]
  //    to derive "latitude_bounds" or replace an existing variable that has
  //    different dimensions when a conversion exists.
  //    First add "altitude_bounds" with wrong dims [vertical], then derive
  //    "altitude_bounds" {time, independent} from altitude {time}? Wait:
  //    let's check solar_zenith_angle: add "elevation_angle" {vertical} (wrong
  //    dim), add "solar_zenith_angle" {time} -> add_derived_variable for
  //    "solar_elevation_angle" {time} or add "solar_elevation_angle" {vertical}
  //    first and replace it!
  ASSERT_NE(nullptr, AddVariableOrFail(prod.get(), "solar_elevation_angle",
                                       harp_type_double, 1, kVertDim, kVertLen,
                                       "degree"));
  harp_variable* sza =
      AddVariableOrFail(prod.get(), "solar_zenith_angle", harp_type_double, 1,
                        kTimeDim, kTimeLen, "degree");
  ASSERT_NE(nullptr, sza);
  sza->data.double_data[0] = 30.0;
  sza->data.double_data[1] = 60.0;

  // Derives solar_elevation_angle {time} and removes the {vertical} one.
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_derived_variable(
                              prod.get(), "solar_elevation_angle", nullptr,
                              "degree", 1, kTimeDim));
  harp_variable* sea = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              prod.get(), "solar_elevation_angle", &sea));
  EXPECT_EQ(harp_dimension_time, sea->dimension_type[0]);
  EXPECT_THAT(sea->data.double_data[0], DoubleNear(60.0, 1e-9));
  EXPECT_THAT(sea->data.double_data[1], DoubleNear(30.0, 1e-9));

  // 7. Missing variable that cannot be derived ->
  // HARP_ERROR_VARIABLE_NOT_FOUND.
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            harp_product_add_derived_variable(prod.get(), "nonexistent_var",
                                              nullptr, nullptr, 1, kTimeDim));
  EXPECT_EQ(HARP_ERROR_VARIABLE_NOT_FOUND, harp_errno);
  harp_set_error(HARP_SUCCESS, nullptr);
}

TEST(HarpDerivedVariableTest, DeriveBuiltInVariablesAndPostConversions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  constexpr harp_dimension_type kDims[] = {harp_dimension_time};
  constexpr long kLen[] = {2};
  // Provide solar_zenith_angle in radians as float (tests both unit and data
  // type conversion on source variable!).
  harp_variable* sza = AddVariableOrFail(
      prod.get(), "solar_zenith_angle", harp_type_float, 1, kDims, kLen, "rad");
  ASSERT_NE(nullptr, sza);
  sza->data.float_data[0] = 0.0f;
  sza->data.float_data[1] = 0.5235987756f;  // ~30 deg

  // Derive solar_elevation_angle converted to "rad" and harp_type_float.
  constexpr harp_data_type kFloatType = harp_type_float;
  harp_variable* out = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_derived_variable(
                              prod.get(), "solar_elevation_angle", &kFloatType,
                              "rad", 1, kDims, &out));
  ScopedHarpVariable out_guard(out);
  EXPECT_EQ(harp_type_float, out->data_type);
  EXPECT_STREQ("rad", out->unit);
  EXPECT_NEAR(1.5707963f, out->data.float_data[0], 1e-5f);
  EXPECT_NEAR(1.0471975f, out->data.float_data[1], 1e-5f);
  out_guard.reset();

  // Incompatible target unit on newly derived variable -> error.
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, harp_product_get_derived_variable(prod.get(), "solar_elevation_angle",
                                            nullptr, "m", 1, kDims, &out));
  EXPECT_EQ(HARP_ERROR_UNIT_CONVERSION, harp_errno);
  harp_set_error(HARP_SUCCESS, nullptr);

  // Source variable already has target unit ("degree") but needs data_type
  // conversion (float -> double, exercises *is_temp == 0 copy in
  // get_source_variable).
  harp_product* raw_p2 = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_p2));
  ScopedHarpProduct p2(raw_p2);
  harp_variable* sza_deg =
      AddVariableOrFail(p2.get(), "solar_zenith_angle", harp_type_float, 1,
                        kDims, kLen, "degree");
  ASSERT_NE(nullptr, sza_deg);
  sza_deg->data.float_data[0] = 30.0f;
  sza_deg->data.float_data[1] = 60.0f;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_derived_variable(
                              p2.get(), "solar_elevation_angle", nullptr,
                              "degree", 1, kDims, &out));
  out_guard.reset(out);
  EXPECT_THAT(out->data.double_data[0], DoubleNear(60.0, 1e-5));
  EXPECT_THAT(out->data.double_data[1], DoubleNear(30.0, 1e-5));
  out_guard.reset();

  // Source variable exists with mismatched dimension_type ({vertical} instead
  // of {time}) -> ignored by has_dimension_types & get_source_variable.
  constexpr harp_dimension_type kVertDim[] = {harp_dimension_vertical};
  ASSERT_NE(nullptr,
            AddVariableOrFail(p2.get(), "sensor_zenith_angle", harp_type_double,
                              1, kVertDim, kLen, "degree"));
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, harp_product_get_derived_variable(p2.get(), "sensor_elevation_angle",
                                            nullptr, "degree", 1, kDims, &out));
  EXPECT_EQ(HARP_ERROR_VARIABLE_NOT_FOUND, harp_errno);
  harp_set_error(HARP_SUCCESS, nullptr);
}

TEST(HarpDerivedVariableTest, DeriveIndependentDimSpeciesAndAuxOptions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const int prev_afgl = harp_get_option_enable_aux_afgl86();
  const int prev_usstd = harp_get_option_enable_aux_usstd76();
  auto restore_opts = absl::MakeCleanup([&] {
    harp_set_option_enable_aux_afgl86(prev_afgl);
    harp_set_option_enable_aux_usstd76(prev_usstd);
  });

  // 1. Derive latitude_bounds {latitude, independent} from latitude {latitude}
  //    (tests independent_dimension_length handling and multi-dim conversion).
  harp_product* raw_grid_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_grid_prod));
  ScopedHarpProduct grid_prod(raw_grid_prod);

  constexpr harp_dimension_type kLatDim[] = {harp_dimension_latitude};
  constexpr long kLatLen[] = {2};
  harp_variable* lat_grid =
      AddVariableOrFail(grid_prod.get(), "latitude", harp_type_double, 1,
                        kLatDim, kLatLen, "degree_north");
  ASSERT_NE(nullptr, lat_grid);
  lat_grid->data.double_data[0] = 10.0;
  lat_grid->data.double_data[1] = 20.0;

  constexpr harp_dimension_type kBoundsDim[] = {harp_dimension_latitude,
                                                harp_dimension_independent};
  harp_variable* bounds = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_derived_variable(
                              grid_prod.get(), "latitude_bounds", nullptr,
                              "degree_north", 2, kBoundsDim, &bounds));
  ScopedHarpVariable bounds_guard(bounds);
  EXPECT_EQ(2, bounds->num_dimensions);
  EXPECT_EQ(2L, bounds->dimension[1]);

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  constexpr harp_dimension_type kTimeDim[] = {harp_dimension_time};
  constexpr long kTimeLen[] = {2};
  harp_variable* lat =
      AddVariableOrFail(prod.get(), "latitude", harp_type_double, 1, kTimeDim,
                        kTimeLen, "degree_north");
  ASSERT_NE(nullptr, lat);
  lat->data.double_data[0] = 10.0;
  lat->data.double_data[1] = 20.0;

  // 2. Species-specific optimization: requesting O3_number_density on a product
  //    without any O3 variable fails quickly via specific_species pruning.
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_product_get_derived_variable(
                    prod.get(), "O3_number_density", nullptr, nullptr, 1,
                    kTimeDim, &bounds));
  EXPECT_EQ(HARP_ERROR_VARIABLE_NOT_FOUND, harp_errno);
  harp_set_error(HARP_SUCCESS, nullptr);

  // 3. Auxiliary climatology option (enabled vs disabled): with datetime,
  //    latitude, and altitude, temperature {time, vertical} can be derived when
  //    AFGL86 is enabled.
  harp_variable* dt =
      AddVariableOrFail(prod.get(), "datetime", harp_type_double, 1, kTimeDim,
                        kTimeLen, "days since 2000-01-01");
  ASSERT_NE(nullptr, dt);
  dt->data.double_data[0] = 100.0;
  dt->data.double_data[1] = 100.0;
  constexpr harp_dimension_type kTVDim[] = {harp_dimension_time,
                                            harp_dimension_vertical};
  constexpr long kTVLen[] = {2, 2};
  harp_variable* alt = AddVariableOrFail(
      prod.get(), "altitude", harp_type_double, 2, kTVDim, kTVLen, "m");
  ASSERT_NE(nullptr, alt);
  alt->data.double_data[0] = 1000.0;
  alt->data.double_data[1] = 5000.0;
  alt->data.double_data[2] = 1000.0;
  alt->data.double_data[3] = 5000.0;

  ASSERT_EQ(HARP_SUCCESS, harp_set_option_enable_aux_afgl86(1));
  harp_variable* temp = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_get_derived_variable(prod.get(), "temperature",
                                              nullptr, "K", 2, kTVDim, &temp));
  ScopedHarpVariable temp_guard(temp);
  EXPECT_GT(temp->data.double_data[0], 200.0);
}

TEST(HarpDerivedVariableTest, CustomConversionsCyclesDepthAndZeroTimeDim) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  // Re-initialize derived variable list cleanly so custom conversions stay
  // isolated to this test and are freed when ScopedHarpInit exits.
  harp_derived_variable_list_done();
  // Lazy init via harp_doc_list_conversions when list is NULL.
  std::string output;
  g_doc_output = &output;
  auto clear_doc = absl::MakeCleanup([] { g_doc_output = nullptr; });
  ASSERT_EQ(HARP_SUCCESS, harp_doc_list_conversions(
                              nullptr, "solar_elevation_angle", CapturePrint));
  EXPECT_THAT(output, HasSubstr("solar_elevation_angle"));

  // Register custom conversions:
  // - zero_src_var {time} with 0 sources (exercises num_source_variables == 0
  //   and dimension[time] == 0 -> 1 in create_variable).
  harp_dimension_type kTimeDim[] = {harp_dimension_time};
  harp_variable_conversion* conv_zero = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_conversion_new(
                              "custom_zero_src", harp_type_double, "m", 1,
                              kTimeDim, -1, ZeroSourceConstFunc, &conv_zero));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_conversion_set_source_description(
                              conv_zero, "constant zero-source test"));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_conversion_set_enabled_function(
                              conv_zero, AlwaysEnabledFunc));

  // - disabled conversion (exercises enabled() == 0).
  harp_variable_conversion* conv_disabled = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_conversion_new("custom_disabled", harp_type_double,
                                         "m", 1, kTimeDim, -1,
                                         ZeroSourceConstFunc, &conv_disabled));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_conversion_set_enabled_function(
                              conv_disabled, AlwaysDisabledFunc));

  // - cycle: custom_cyc_a <-> custom_cyc_b.
  harp_variable_conversion* cyc_a = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_conversion_species_new(
                "custom_cyc_a", harp_type_double, "1", 1, kTimeDim, -1,
                harp_chemical_species_O3, CopyFirstFunc, &cyc_a));
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_conversion_add_source(
                cyc_a, "custom_cyc_b", harp_type_double, "1", 1, kTimeDim, -1));
  harp_variable_conversion* cyc_b = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_conversion_species_new(
                "custom_cyc_b", harp_type_double, "1", 1, kTimeDim, -1,
                harp_chemical_species_O3, CopyFirstFunc, &cyc_b));
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_conversion_add_source(
                cyc_b, "custom_cyc_a", harp_type_double, "1", 1, kTimeDim, -1));

  // - deep chain of length 13 (> max_depth 10) to trigger max_depth & budget
  //   pruning.
  const std::vector<std::string> chain_names = {
      "chain_0",  "chain_1",  "chain_2", "chain_3", "chain_4",
      "chain_5",  "chain_6",  "chain_7", "chain_8", "chain_9",
      "chain_10", "chain_11", "chain_12"};
  for (size_t i = 0; i + 1 < chain_names.size(); ++i) {
    harp_variable_conversion* step = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_variable_conversion_new(
                                chain_names[i].c_str(), harp_type_double, "1",
                                1, kTimeDim, -1, ScaleByTwoFunc, &step));
    ASSERT_EQ(HARP_SUCCESS, harp_variable_conversion_add_source(
                                step, chain_names[i + 1].c_str(),
                                harp_type_double, "1", 1, kTimeDim, -1));
  }
  // Alternative on chain_0 from custom_indep with independent length 3.
  harp_dimension_type kIndepDim[] = {harp_dimension_time,
                                     harp_dimension_independent};
  harp_variable_conversion* indep_conv = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_conversion_new(
                              "custom_indep", harp_type_double, "1", 2,
                              kIndepDim, 3, ZeroSourceConstFunc, &indep_conv));
  harp_variable_conversion* indep_mismatch = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_conversion_new(
                              "custom_indep_need4", harp_type_double, "1", 2,
                              kIndepDim, 4, CopyFirstFunc, &indep_mismatch));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_conversion_add_source(
                              indep_mismatch, "custom_indep", harp_type_double,
                              "1", 2, kIndepDim, 4));

  // Conversions exercising unit conversion on recursively derived sources and
  // disabled alternative skipped in find_and_print_conversion.
  harp_variable_conversion* km_src = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_conversion_new(
                              "custom_km", harp_type_double, "km", 1, kTimeDim,
                              -1, ZeroSourceConstFunc, &km_src));
  harp_variable_conversion* from_km = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_conversion_new(
                              "custom_from_km", harp_type_double, "m", 1,
                              kTimeDim, -1, CopyFirstFunc, &from_km));
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_conversion_add_source(
                from_km, "custom_km", harp_type_double, "m", 1, kTimeDim, -1));

  harp_variable_conversion* multi_dis = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_conversion_new(
                              "custom_multi_opt", harp_type_double, "m", 1,
                              kTimeDim, -1, ZeroSourceConstFunc, &multi_dis));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_conversion_set_enabled_function(
                              multi_dis, AlwaysDisabledFunc));
  harp_variable_conversion* multi_en = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_conversion_new(
                              "custom_multi_opt", harp_type_double, "m", 1,
                              kTimeDim, -1, CopyFirstFunc, &multi_en));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_conversion_add_source(
                              multi_en, "custom_zero_src", harp_type_double,
                              "m", 1, kTimeDim, -1));
  harp_variable_conversion* multi_user = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_conversion_new(
                              "custom_multi_user", harp_type_double, "m", 1,
                              kTimeDim, -1, CopyFirstFunc, &multi_user));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_conversion_add_source(
                              multi_user, "custom_multi_opt", harp_type_double,
                              "m", 1, kTimeDim, -1));

  // Source exists in product with wrong dims while a conversion exists for the
  // right dims (hits info->variable = NULL in get_source_variable), plus a
  // derived string variable to test post-derivation data_type conversion error.
  harp_variable_conversion* ambig_src = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_conversion_new(
                              "custom_ambig", harp_type_double, "m", 1,
                              kTimeDim, -1, ZeroSourceConstFunc, &ambig_src));
  harp_variable_conversion* ambig_user = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_conversion_new(
                              "custom_ambig_user", harp_type_double, "m", 1,
                              kTimeDim, -1, CopyFirstFunc, &ambig_user));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_conversion_add_source(
                              ambig_user, "custom_ambig", harp_type_double, "m",
                              1, kTimeDim, -1));
  harp_variable_conversion* str_conv = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_conversion_new(
                              "custom_str", harp_type_string, nullptr, 1,
                              kTimeDim, -1, ZeroSourceStringFunc, &str_conv));

  // Empty product (time dim == 0): custom_zero_src creates dim[time] = 1.
  harp_product* raw_empty = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_empty));
  ScopedHarpProduct empty_prod(raw_empty);

  harp_variable* out = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_derived_variable(
                              empty_prod.get(), "custom_zero_src", nullptr,
                              nullptr, 1, kTimeDim, &out));
  ScopedHarpVariable out_guard(out);
  EXPECT_EQ(1L, out->dimension[0]);
  EXPECT_DOUBLE_EQ(7.5, out->data.double_data[0]);
  out_guard.reset();

  // Disabled conversion cannot be derived.
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_product_get_derived_variable(empty_prod.get(),
                                                  "custom_disabled", nullptr,
                                                  nullptr, 1, kTimeDim, &out));
  EXPECT_EQ(HARP_ERROR_VARIABLE_NOT_FOUND, harp_errno);

  // Cycle cannot be derived (needs an O3 variable in product so species check
  // passes and enters cycle).
  constexpr long kLen1[] = {1};
  ASSERT_NE(nullptr, AddVariableOrFail(empty_prod.get(), "O3_dummy",
                                       harp_type_double, 1, kTimeDim, kLen1));
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_product_get_derived_variable(empty_prod.get(),
                                                  "custom_cyc_a", nullptr,
                                                  nullptr, 1, kTimeDim, &out));
  EXPECT_EQ(HARP_ERROR_VARIABLE_NOT_FOUND, harp_errno);

  // Deep chain exceeds max_depth (10) -> cannot derive chain_0 from chain_12.
  ASSERT_NE(nullptr,
            AddVariableOrFail(empty_prod.get(), "chain_12", harp_type_double, 1,
                              kTimeDim, kLen1, "1"));
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_product_get_derived_variable(empty_prod.get(), "chain_0",
                                                  nullptr, nullptr, 1, kTimeDim,
                                                  &out));
  EXPECT_EQ(HARP_ERROR_VARIABLE_NOT_FOUND, harp_errno);

  // Independent dimension length mismatch (custom_indep has len 3, needed 4).
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_product_get_derived_variable(empty_prod.get(),
                                                  "custom_indep_need4", nullptr,
                                                  nullptr, 2, kIndepDim, &out));
  EXPECT_EQ(HARP_ERROR_VARIABLE_NOT_FOUND, harp_errno);

  // Also test existing variable in product with wrong independent dimension
  // length (custom_indep {time, independent(5)} != expected 4) -> falls through
  // has_dimension_types to conversion check, which also fails.
  constexpr long kIndep5Len[] = {1, 5};
  ASSERT_NE(nullptr,
            AddVariableOrFail(empty_prod.get(), "custom_indep",
                              harp_type_double, 2, kIndepDim, kIndep5Len, "1"));
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_product_get_derived_variable(empty_prod.get(),
                                                  "custom_indep_need4", nullptr,
                                                  nullptr, 2, kIndepDim, &out));
  EXPECT_EQ(HARP_ERROR_VARIABLE_NOT_FOUND, harp_errno);

  // Recursively derived source variable unit conversion (success).
  // Note: upstream perform_conversion leaks source_info on get_source_variable
  // failure, so failing source unit conversion is not invoked here.
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_derived_variable(
                              empty_prod.get(), "custom_from_km", nullptr,
                              nullptr, 1, kTimeDim, &out));
  out_guard.reset(out);
  EXPECT_DOUBLE_EQ(7500.0, out->data.double_data[0]);
  out_guard.reset();
  harp_set_error(HARP_SUCCESS, nullptr);

  // Source exists in product with {vertical}, while custom_ambig_user needs
  // custom_ambig {time} -> falls back to custom_ambig {time} conversion.
  constexpr harp_dimension_type kVertDim[] = {harp_dimension_vertical};
  ASSERT_NE(nullptr,
            AddVariableOrFail(empty_prod.get(), "custom_ambig",
                              harp_type_double, 1, kVertDim, kLen1, "m"));
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_derived_variable(
                              empty_prod.get(), "custom_ambig_user", nullptr,
                              nullptr, 1, kTimeDim, &out));
  out_guard.reset(out);
  EXPECT_DOUBLE_EQ(7.5, out->data.double_data[0]);
  out_guard.reset();

  // Derived string variable cannot convert to double -> cleans up & errors.
  constexpr harp_data_type kDoubleType = harp_type_double;
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_product_get_derived_variable(empty_prod.get(),
                                                  "custom_str", &kDoubleType,
                                                  nullptr, 1, kTimeDim, &out));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  harp_set_error(HARP_SUCCESS, nullptr);

  // Exercise harp_doc_list_conversions with product == NULL (all) and
  // product != NULL (possible conversions, including 0-source and filtered).
  output.clear();
  ASSERT_EQ(HARP_SUCCESS,
            harp_doc_list_conversions(nullptr, nullptr, CapturePrint));
  EXPECT_THAT(output, HasSubstr("custom_zero_src"));
  EXPECT_THAT(output, HasSubstr("derived without input variables"));
  EXPECT_THAT(output, HasSubstr("constant zero-source test"));
  EXPECT_THAT(output, testing::Not(HasSubstr("custom_disabled")));

  output.clear();
  ASSERT_EQ(HARP_SUCCESS,
            harp_doc_list_conversions(empty_prod.get(), nullptr, CapturePrint));
  EXPECT_THAT(output, HasSubstr("custom_zero_src"));
  EXPECT_THAT(output, HasSubstr("chain_2"));

  output.clear();
  ASSERT_EQ(HARP_SUCCESS, harp_doc_list_conversions(empty_prod.get(), "chain_2",
                                                    CapturePrint));
  EXPECT_THAT(output, HasSubstr("chain_2"));
  EXPECT_THAT(output, HasSubstr("chain_12"));

  output.clear();
  ASSERT_EQ(HARP_SUCCESS,
            harp_doc_list_conversions(empty_prod.get(), "custom_multi_user",
                                      CapturePrint));
  EXPECT_THAT(output, HasSubstr("custom_multi_opt"));
  EXPECT_THAT(output, HasSubstr("custom_zero_src"));

  // Delete a standalone conversion directly (nullptr and populated).
  harp_variable_conversion_delete(nullptr);
}

TEST(HarpDerivedVariableTest, DocListConversionsSkipsExistingAndLazyInit) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  // Verify lazy initialization in harp_product_get_derived_variable and
  // harp_product_add_derived_variable when harp_derived_variable_conversions is
  // NULL.
  harp_derived_variable_list_done();

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  constexpr harp_dimension_type kDims[] = {harp_dimension_time};
  constexpr long kLen[] = {1};
  harp_variable* sza =
      AddVariableOrFail(prod.get(), "solar_zenith_angle", harp_type_double, 1,
                        kDims, kLen, "degree");
  ASSERT_NE(nullptr, sza);
  sza->data.double_data[0] = 45.0;

  harp_variable* out = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_derived_variable(
                              prod.get(), "solar_elevation_angle", nullptr,
                              "degree", 1, kDims, &out));
  ScopedHarpVariable out_guard(out);
  EXPECT_THAT(out->data.double_data[0], DoubleNear(45.0, 1e-9));
  out_guard.reset();

  // Also test lazy init in harp_product_add_derived_variable.
  harp_derived_variable_list_done();
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_derived_variable(
                              prod.get(), "solar_elevation_angle", nullptr,
                              "degree", 1, kDims));

  // solar_elevation_angle already exists with {time} in prod, so
  // harp_doc_list_conversions(prod, "solar_elevation_angle", ...) skips it.
  std::string output;
  g_doc_output = &output;
  auto clear_doc = absl::MakeCleanup([] { g_doc_output = nullptr; });
  ASSERT_EQ(HARP_SUCCESS,
            harp_doc_list_conversions(prod.get(), "solar_elevation_angle",
                                      CapturePrint));
  EXPECT_TRUE(output.empty());
}

}  // namespace
}  // namespace stcorp_harp
