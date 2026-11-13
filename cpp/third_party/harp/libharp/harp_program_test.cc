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
// Unit tests for HARP program creation and execution (libharp/harp-program.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-program.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-program.h

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/google/util/temp_file.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::ElementsAre;

// Helpers to build and attach variables cleanly with RAII.
void AddDoubleVar(harp_product* prod, const char* name, int num_dims,
                  const harp_dimension_type* dim_types, const long* dims,
                  const char* unit, const std::vector<double>& values) {
  harp_variable* raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_new(name, harp_type_double, num_dims,
                                            dim_types, dims, &raw));
  ScopedHarpVariable var(raw);
  if (unit != nullptr) {
    ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(var.get(), unit));
  }
  for (long i = 0; i < var->num_elements; ++i) {
    var->data.double_data[i] = values[static_cast<size_t>(i)];
  }
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod, var.get()));
  (void)var.release();
}

void AddInt32Var(harp_product* prod, const char* name, int num_dims,
                 const harp_dimension_type* dim_types, const long* dims,
                 const char* unit, const std::vector<int32_t>& values) {
  harp_variable* raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_new(name, harp_type_int32, num_dims,
                                            dim_types, dims, &raw));
  ScopedHarpVariable var(raw);
  if (unit != nullptr) {
    ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(var.get(), unit));
  }
  for (long i = 0; i < var->num_elements; ++i) {
    var->data.int32_data[i] = values[static_cast<size_t>(i)];
  }
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod, var.get()));
  (void)var.release();
}

void AddStringVar(harp_product* prod, const char* name, int num_dims,
                  const harp_dimension_type* dim_types, const long* dims,
                  const std::vector<const char*>& values) {
  harp_variable* raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_new(name, harp_type_string, num_dims,
                                            dim_types, dims, &raw));
  ScopedHarpVariable var(raw);
  for (long i = 0; i < var->num_elements; ++i) {
    ASSERT_EQ(HARP_SUCCESS, harp_variable_set_string_data_element(
                                var.get(), i, values[static_cast<size_t>(i)]));
  }
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod, var.get()));
  (void)var.release();
}

ScopedHarpProduct MakeNewProduct() {
  harp_product* raw = nullptr;
  EXPECT_EQ(HARP_SUCCESS, harp_product_new(&raw));
  return ScopedHarpProduct(raw);
}

// Builds a standard 1D/2D atmospheric product suitable for filtering, binning,
// regridding, smoothing, sorting, squashing, and wrapping tests.
ScopedHarpProduct MakeSampleAtmosphericProduct(long num_times = 3,
                                               long num_levels = 2) {
  ScopedHarpProduct prod = MakeNewProduct();
  EXPECT_EQ(HARP_SUCCESS,
            harp_product_set_source_product(prod.get(), "prod_a"));

  const harp_dimension_type t_dim[] = {harp_dimension_time};
  const long t_len[] = {num_times};
  std::vector<double> dt_vals;
  std::vector<double> lat_vals;
  std::vector<double> lon_vals;
  std::vector<int32_t> idx_vals;
  for (long i = 0; i < num_times; ++i) {
    dt_vals.push_back(static_cast<double>(num_times - i) * 100.0);
    lat_vals.push_back(10.0 + static_cast<double>(i) * 5.0);
    lon_vals.push_back(20.0 + static_cast<double>(i) * 5.0);
    idx_vals.push_back(static_cast<int32_t>(i));
  }
  AddDoubleVar(prod.get(), "datetime", 1, t_dim, t_len,
               "seconds since 2020-01-01", dt_vals);
  AddDoubleVar(prod.get(), "latitude", 1, t_dim, t_len, "degree_north",
               lat_vals);
  AddDoubleVar(prod.get(), "longitude", 1, t_dim, t_len, "degree_east",
               lon_vals);
  AddInt32Var(prod.get(), "collocation_index", 1, t_dim, t_len, nullptr,
              idx_vals);

  const harp_dimension_type b_dim[] = {harp_dimension_time,
                                       harp_dimension_independent};
  const long b_len[] = {num_times, 4};
  std::vector<double> lat_b;
  std::vector<double> lon_b;
  for (long i = 0; i < num_times; ++i) {
    double lat0 = 10.0 + static_cast<double>(i) * 5.0;
    double lon0 = 20.0 + static_cast<double>(i) * 5.0;
    lat_b.insert(lat_b.end(), {lat0 - 1.0, lat0 - 1.0, lat0 + 1.0, lat0 + 1.0});
    lon_b.insert(lon_b.end(), {lon0 - 1.0, lon0 + 1.0, lon0 + 1.0, lon0 - 1.0});
  }
  AddDoubleVar(prod.get(), "latitude_bounds", 2, b_dim, b_len, "degree_north",
               lat_b);
  AddDoubleVar(prod.get(), "longitude_bounds", 2, b_dim, b_len, "degree_east",
               lon_b);

  if (num_levels > 0) {
    const harp_dimension_type p_dim[] = {harp_dimension_time,
                                         harp_dimension_vertical};
    const long p_len[] = {num_times, num_levels};
    const harp_dimension_type pb_dim[] = {harp_dimension_time,
                                          harp_dimension_vertical,
                                          harp_dimension_independent};
    const long pb_len[] = {num_times, num_levels, 2};
    std::vector<double> press;
    std::vector<double> press_b;
    std::vector<double> o3;
    for (long i = 0; i < num_times; ++i) {
      for (long k = 0; k < num_levels; ++k) {
        double p_hi = 1000.0 - static_cast<double>(k) * 400.0;
        double p_lo = p_hi - 400.0;
        press.push_back(0.5 * (p_hi + p_lo));
        press_b.push_back(p_hi);
        press_b.push_back(p_lo);
        o3.push_back(10.0 + static_cast<double>(i + k));
      }
    }
    AddDoubleVar(prod.get(), "pressure", 2, p_dim, p_len, "hPa", press);
    AddDoubleVar(prod.get(), "pressure_bounds", 3, pb_dim, pb_len, "hPa",
                 press_b);
    AddDoubleVar(prod.get(), "O3_column_number_density", 2, p_dim, p_len,
                 "molec/cm2", o3);
  }
  return prod;
}

// Writes a collocated NetCDF product for smoothing/regridding/collocation.
void WriteCollocatedProduct(const char* filepath, const char* source_product,
                            long num_times, long num_levels,
                            bool include_collocation_index) {
  ScopedHarpProduct col = MakeNewProduct();
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_set_source_product(col.get(), source_product));

  const harp_dimension_type t_dim[] = {harp_dimension_time};
  const long t_len[] = {num_times};
  std::vector<int32_t> idx;
  for (long i = 0; i < num_times; ++i) {
    idx.push_back(static_cast<int32_t>(i));
  }
  if (include_collocation_index) {
    AddInt32Var(col.get(), "collocation_index", 1, t_dim, t_len, nullptr, idx);
  }
  AddInt32Var(col.get(), "index", 1, t_dim, t_len, nullptr, idx);

  const harp_dimension_type p_dim[] = {harp_dimension_time,
                                       harp_dimension_vertical};
  const long p_len[] = {num_times, num_levels};
  const harp_dimension_type pb_dim[] = {
      harp_dimension_time, harp_dimension_vertical, harp_dimension_independent};
  const long pb_len[] = {num_times, num_levels, 2};
  const harp_dimension_type avk_dim[] = {
      harp_dimension_time, harp_dimension_vertical, harp_dimension_vertical};
  const long avk_len[] = {num_times, num_levels, num_levels};

  std::vector<double> press;
  std::vector<double> press_b;
  std::vector<double> apriori;
  std::vector<double> avk;
  for (long i = 0; i < num_times; ++i) {
    for (long k = 0; k < num_levels; ++k) {
      double p_hi = 900.0 - static_cast<double>(k) * 300.0;
      double p_lo = p_hi - 300.0;
      press.push_back(0.5 * (p_hi + p_lo));
      press_b.push_back(p_hi);
      press_b.push_back(p_lo);
      apriori.push_back(2.0);
      for (long l = 0; l < num_levels; ++l) {
        avk.push_back(k == l ? 1.0 : 0.0);
      }
    }
  }
  AddDoubleVar(col.get(), "pressure", 2, p_dim, p_len, "hPa", press);
  AddDoubleVar(col.get(), "pressure_bounds", 3, pb_dim, pb_len, "hPa", press_b);
  AddDoubleVar(col.get(), "O3_column_number_density_apriori", 2, p_dim, p_len,
               "molec/cm2", apriori);
  AddDoubleVar(col.get(), "O3_column_number_density_avk", 3, avk_dim, avk_len,
               "", avk);

  ASSERT_EQ(HARP_SUCCESS, harp_export(filepath, "netcdf", col.get()));
}

void WriteCollocationCsv(const char* csv_path, const char* src_a,
                         const char* src_b, long num_pairs) {
  harp_collocation_result* raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_new(&raw, 0, nullptr, nullptr));
  ScopedHarpCollocationResult cr(raw);
  for (long i = 0; i < num_pairs; ++i) {
    ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_add_pair(
                                cr.get(), i, src_a, i, src_b, i, 0, nullptr));
  }
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_write(csv_path, cr.get()));
}

TEST(HarpProgramTest, NewDeleteAddOperationsAndOptionRestoration) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  harp_set_error(HARP_SUCCESS, nullptr);

  const int orig_afgl86 = harp_get_option_enable_aux_afgl86();
  const int orig_usstd76 = harp_get_option_enable_aux_usstd76();
  const int orig_oob = harp_get_option_regrid_out_of_bounds();
  auto restore_opts = absl::MakeCleanup([&] {
    harp_set_option_enable_aux_afgl86(orig_afgl86);
    harp_set_option_enable_aux_usstd76(orig_usstd76);
    harp_set_option_regrid_out_of_bounds(orig_oob);
    harp_set_error(HARP_SUCCESS, nullptr);
  });

  harp_set_option_regrid_out_of_bounds(2);
  harp_program* raw_prog = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_program_new(&raw_prog));
  ScopedHarpProgram prog(raw_prog);
  EXPECT_EQ(0, harp_get_option_regrid_out_of_bounds());

  // Add > BLOCK_SIZE (16) operations to trigger multiple realloc blocks.
  for (int i = 0; i < 20; ++i) {
    harp_operation* raw_op = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_operation_set_new("regrid_out_of_bounds", "nan", &raw_op));
    ScopedHarpOperation op(raw_op);
    ASSERT_EQ(HARP_SUCCESS, harp_program_add_operation(prog.get(), op.get()));
    (void)op.release();
  }
  EXPECT_EQ(20, prog->num_operations);

  ScopedHarpProduct prod = MakeSampleAtmosphericProduct(2, 0);
  ASSERT_EQ(HARP_SUCCESS, harp_product_execute_program(prod.get(), prog.get()));

  prog.reset();
  EXPECT_EQ(2, harp_get_option_regrid_out_of_bounds());
  harp_program_delete(nullptr);
}

TEST(HarpProgramTest, ExecuteOperationsNullSyntaxAndEmptyEarlyStop) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  ScopedHarpProduct prod = MakeSampleAtmosphericProduct(2, 0);
  EXPECT_EQ(-1, harp_product_execute_operations(nullptr, "bin()"));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(-1, harp_product_execute_operations(prod.get(), nullptr));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(-1, harp_product_execute_operations(prod.get(), "invalid(("));
  EXPECT_EQ(HARP_ERROR_OPERATION_SYNTAX, harp_errno);

  // Filter empties product; subsequent invalid operation keep(missing) must
  // not execute because harp_product_execute_program returns early.
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_execute_operations(
                prod.get(), "latitude > 1000.0; keep(nonexistent)"));
  EXPECT_EQ(1, harp_product_is_empty(prod.get()));
}

TEST(HarpProgramTest, ValueFiltersScalar1D2DEnumAndErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  // 1. Scalar numeric & string value filters: pass, fail (empties), eval err.
  {
    ScopedHarpProduct prod = MakeNewProduct();
    AddDoubleVar(prod.get(), "s_num", 0, nullptr, nullptr, "m", {5.0});
    AddStringVar(prod.get(), "s_str", 0, nullptr, nullptr, {"ok"});
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_execute_operations(
                  prod.get(), "s_num > 400 [cm]; s_str == \"ok\""));
    EXPECT_EQ(0, harp_product_is_empty(prod.get()));

    EXPECT_EQ(-1,
              harp_product_execute_operations(prod.get(), "s_num == \"bad\""));
    EXPECT_EQ(-1, harp_product_execute_operations(prod.get(), "s_str > 1"));

    // Failing scalar string filter empties product.
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_execute_operations(prod.get(), "s_str == \"no\""));
    EXPECT_EQ(1, harp_product_is_empty(prod.get()));
  }
  {
    ScopedHarpProduct prod = MakeNewProduct();
    AddDoubleVar(prod.get(), "s_num", 0, nullptr, nullptr, nullptr, {5.0});
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_execute_operations(prod.get(), "s_num > 10"));
    EXPECT_EQ(1, harp_product_is_empty(prod.get()));
  }

  // 2. 1D value filters: coalesced same-variable filters, interrupted by
  // different variable or non-filter op, enum string filter, and error path.
  {
    ScopedHarpProduct prod = MakeNewProduct();
    const harp_dimension_type t_dim[] = {harp_dimension_time};
    const long t_len[] = {4};
    AddDoubleVar(prod.get(), "x", 1, t_dim, t_len, "m", {1.0, 2.0, 3.0, 4.0});
    AddDoubleVar(prod.get(), "y", 1, t_dim, t_len, nullptr,
                 {10.0, 20.0, 30.0, 40.0});
    AddStringVar(prod.get(), "tag", 1, t_dim, t_len, {"a", "b", "c", "d"});

    harp_variable* raw_enum = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_variable_new("mode", harp_type_int8, 1, t_dim,
                                              t_len, &raw_enum));
    ScopedHarpVariable enum_var(raw_enum);
    const char* enum_names[] = {"low", "high"};
    ASSERT_EQ(HARP_SUCCESS, harp_variable_set_enumeration_values(
                                enum_var.get(), 2, enum_names));
    enum_var->data.int8_data[0] = 0;
    enum_var->data.int8_data[1] = 1;
    enum_var->data.int8_data[2] = 1;
    enum_var->data.int8_data[3] = 0;
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_add_variable(prod.get(), enum_var.get()));
    (void)enum_var.release();

    ASSERT_EQ(HARP_SUCCESS,
              harp_product_execute_operations(
                  prod.get(),
                  "x >= 200 [cm]; x <= 3.5 [m]; y > 15; set(\"afgl86\", "
                  "\"disabled\"); mode == \"high\"; tag in (\"b\", \"c\")"));
    EXPECT_EQ(2L, prod->dimension[harp_dimension_time]);

    EXPECT_EQ(-1, harp_product_execute_operations(prod.get(), "x == \"bad\""));
    EXPECT_EQ(-1, harp_product_execute_operations(prod.get(), "x > 1.0 [s]"));
    EXPECT_EQ(-1, harp_product_execute_operations(prod.get(), "tag > 1"));
    EXPECT_EQ(-1, harp_product_execute_operations(prod.get(), "missing > 1"));
  }

  // 3. 2D {time, vertical} value filters (numeric & string) and invalid dims.
  {
    ScopedHarpProduct prod = MakeNewProduct();
    const harp_dimension_type tv_dim[] = {harp_dimension_time,
                                          harp_dimension_vertical};
    const long tv_len[] = {2, 2};
    // Time 0 has {1, 5} (partial keep), Time 1 has {0, 0} (fully masked out).
    AddDoubleVar(prod.get(), "profile", 2, tv_dim, tv_len, nullptr,
                 {1.0, 5.0, 0.0, 0.0});
    AddStringVar(prod.get(), "label", 2, tv_dim, tv_len,
                 {"keep", "keep", "drop", "drop"});
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_execute_operations(
                  prod.get(), "profile > 2.0; label == \"keep\""));
    EXPECT_EQ(1L, prod->dimension[harp_dimension_time]);
    EXPECT_EQ(1L, prod->dimension[harp_dimension_vertical]);

    EXPECT_EQ(
        -1, harp_product_execute_operations(prod.get(), "profile == \"bad\""));
    EXPECT_EQ(-1, harp_product_execute_operations(prod.get(), "label > 1"));
  }

  // 4. Unsupported dimensions for value filter (1D independent, 2D non-time).
  {
    ScopedHarpProduct prod = MakeNewProduct();
    const harp_dimension_type ind_dim[] = {harp_dimension_independent};
    const long ind_len[] = {2};
    AddDoubleVar(prod.get(), "ind", 1, ind_dim, ind_len, nullptr, {1.0, 2.0});
    EXPECT_EQ(-1, harp_product_execute_operations(prod.get(), "ind > 0"));
    EXPECT_EQ(HARP_ERROR_OPERATION, harp_errno);
  }
}

TEST(HarpProgramTest, IndexPointPolygonAndCollocationFilters) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  // 1. Index filters (absent dimension is no-op, present dimension filters).
  {
    ScopedHarpProduct prod = MakeSampleAtmosphericProduct(4, 2);
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_execute_operations(
                  prod.get(),
                  "index(spectral) == 0; index(time) < 3; index(time) in "
                  "(0, 2)"));
    EXPECT_EQ(2L, prod->dimension[harp_dimension_time]);
  }

  // 2. Point filters (coalesced + followed by non-point op) + missing
  // lat/lon errors.
  {
    ScopedHarpProduct prod = MakeSampleAtmosphericProduct(3, 0);
    ASSERT_EQ(HARP_SUCCESS, harp_product_execute_operations(
                                prod.get(),
                                "point_distance(10.0, 20.0, 800 [km]); "
                                "point_in_area((8.0, 8.0, 17.0, 17.0), "
                                "(18.0, 27.0, 27.0, 18.0)); "
                                "sort(datetime)"));
    EXPECT_EQ(2L, prod->dimension[harp_dimension_time]);

    ScopedHarpProduct empty_geo = MakeNewProduct();
    EXPECT_EQ(-1, harp_product_execute_operations(
                      empty_geo.get(), "point_distance(0, 0, 100 [km])"));
    const harp_dimension_type t_dim[] = {harp_dimension_time};
    const long t_len[] = {1};
    AddDoubleVar(empty_geo.get(), "latitude", 1, t_dim, t_len, "degree_north",
                 {10.0});
    EXPECT_EQ(-1, harp_product_execute_operations(
                      empty_geo.get(), "point_distance(0, 0, 100 [km])"));
  }

  // 3. Polygon filters (coalesced + followed by non-polygon op) + error paths.
  {
    ScopedHarpProduct prod = MakeSampleAtmosphericProduct(3, 0);
    ASSERT_EQ(HARP_SUCCESS, harp_product_execute_operations(
                                prod.get(),
                                "area_covers_point(10.0, 20.0); "
                                "area_intersects_area((8.0, 8.0, 12.0, 12.0), "
                                "(18.0, 22.0, 22.0, 18.0)); "
                                "area_inside_area((5.0, 5.0, 25.0, 25.0), "
                                "(15.0, 35.0, 35.0, 15.0)); "
                                "sort(datetime)"));
    EXPECT_EQ(1L, prod->dimension[harp_dimension_time]);
  }
  {
    ScopedHarpProduct prod = MakeSampleAtmosphericProduct(2, 0);
    ASSERT_EQ(HARP_SUCCESS, harp_product_execute_operations(
                                prod.get(),
                                "area_covers_area((9.5, 9.5, 10.5, 10.5), "
                                "(19.5, 20.5, 20.5, 19.5))"));
    EXPECT_EQ(1L, prod->dimension[harp_dimension_time]);
  }
  {
    // Polygon errors: missing latitude_bounds, missing longitude_bounds,
    // mismatched corner counts, < 2 corners, invalid polygon bounds (NaN).
    ScopedHarpProduct p_missing = MakeNewProduct();
    EXPECT_EQ(-1, harp_product_execute_operations(p_missing.get(),
                                                  "area_covers_point(0, 0)"));

    const harp_dimension_type b_dim[] = {harp_dimension_time,
                                         harp_dimension_independent};
    const long b_len1[] = {1, 4};
    AddDoubleVar(p_missing.get(), "latitude_bounds", 2, b_dim, b_len1,
                 "degree_north", {0, 0, 1, 1});
    EXPECT_EQ(-1, harp_product_execute_operations(p_missing.get(),
                                                  "area_covers_point(0, 0)"));

    ScopedHarpProduct p_mismatch = MakeNewProduct();
    const long b_len2[] = {1, 3};
    AddDoubleVar(p_mismatch.get(), "latitude_bounds", 2, b_dim, b_len1,
                 "degree_north", {0, 0, 1, 1});
    AddDoubleVar(p_mismatch.get(), "longitude_bounds", 2, b_dim, b_len2,
                 "degree_east", {0, 1, 1});
    EXPECT_EQ(-1, harp_product_execute_operations(p_mismatch.get(),
                                                  "area_covers_point(0, 0)"));

    ScopedHarpProduct p_small = MakeNewProduct();
    const long b_len_one[] = {1, 1};
    AddDoubleVar(p_small.get(), "latitude_bounds", 2, b_dim, b_len_one,
                 "degree_north", {0});
    AddDoubleVar(p_small.get(), "longitude_bounds", 2, b_dim, b_len_one,
                 "degree_east", {0});
    EXPECT_EQ(-1, harp_product_execute_operations(p_small.get(),
                                                  "area_covers_point(0, 0)"));

    ScopedHarpProduct p_bad_poly = MakeNewProduct();
    const long b_len_two[] = {1, 2};
    AddDoubleVar(p_bad_poly.get(), "latitude_bounds", 2, b_dim, b_len_two,
                 "degree_north", {0.0, 0.0});
    AddDoubleVar(p_bad_poly.get(), "longitude_bounds", 2, b_dim, b_len_two,
                 "degree_east", {0.0, 1.0});
    EXPECT_EQ(-1, harp_product_execute_operations(p_bad_poly.get(),
                                                  "area_covers_point(0, 0)"));
  }

  // 4. Collocation filter: source_product == NULL, bad collocation_index,
  // auto-derive index, and existing collocation_index.
  {
    TempFile csv_file("harp_program_colloc_filter.csv");
    WriteCollocationCsv(csv_file.c_str(), "prod_a", "prod_b", 3);

    ScopedHarpProduct no_src = MakeNewProduct();
    EXPECT_EQ(-1, harp_product_execute_operations(
                      no_src.get(), absl::StrFormat("collocate_left(\"%s\")",
                                                    csv_file.c_str())
                                        .c_str()));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

    // Product with source_product and invalid scalar collocation_index fails.
    ScopedHarpProduct bad_idx = MakeNewProduct();
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_set_source_product(bad_idx.get(), "prod_a"));
    const harp_dimension_type t_dim_1[] = {harp_dimension_time};
    const long t_len_1[] = {1};
    AddStringVar(bad_idx.get(), "collocation_index", 1, t_dim_1, t_len_1,
                 {"bad"});
    EXPECT_EQ(-1, harp_product_execute_operations(
                      bad_idx.get(), absl::StrFormat("collocate_left(\"%s\")",
                                                     csv_file.c_str())
                                         .c_str()));

    // Without collocation_index/index: auto-derives `index`.
    ScopedHarpProduct p_auto = MakeNewProduct();
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_set_source_product(p_auto.get(), "prod_a"));
    const harp_dimension_type t_dim[] = {harp_dimension_time};
    const long t_len[] = {3};
    AddDoubleVar(p_auto.get(), "val", 1, t_dim, t_len, nullptr,
                 {1.0, 2.0, 3.0});
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_execute_operations(
                  p_auto.get(), absl::StrFormat("collocate_left(\"%s\", 1, 2)",
                                                csv_file.c_str())
                                    .c_str()));
    EXPECT_EQ(2L, p_auto->dimension[harp_dimension_time]);

    // Missing CSV file triggers error.
    EXPECT_EQ(-1, harp_product_execute_operations(
                      p_auto.get(), "collocate_left(\"/nonexistent/c.csv\")"));
  }
}

TEST(HarpProgramTest, BinClampDeriveExcludeKeepFlattenRenameSortSquashWrap) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  // 1. bin(), bin(var), bin((v1, v2)), bin_spatial(), plus error paths.
  {
    ScopedHarpProduct prod = MakeSampleAtmosphericProduct(3, 2);
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_execute_operations(
                  prod.get(), "sort(datetime); bin(collocation_index)"));
    EXPECT_EQ(3L, prod->dimension[harp_dimension_time]);

    ASSERT_EQ(HARP_SUCCESS,
              harp_product_execute_operations(
                  prod.get(), "bin_spatial((0.0, 30.0), (0.0, 30.0)); bin()"));
    EXPECT_EQ(1L, prod->dimension[harp_dimension_time]);

    EXPECT_EQ(-1, harp_product_execute_operations(prod.get(), "sort(missing)"));
    EXPECT_EQ(-1, harp_product_execute_operations(prod.get(), "bin(missing)"));
    ScopedHarpProduct no_geo = MakeNewProduct();
    const harp_dimension_type t_dim[] = {harp_dimension_time};
    const long t_len[] = {1};
    AddDoubleVar(no_geo.get(), "x", 1, t_dim, t_len, nullptr, {1.0});
    EXPECT_EQ(-1, harp_product_execute_operations(
                      no_geo.get(), "bin_spatial((0.0, 10.0), (0.0, 10.0))"));
  }

  // 2. clamp(), rebin(), regrid(), and error paths.
  {
    ScopedHarpProduct prod = MakeSampleAtmosphericProduct(2, 2);
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_execute_operations(
                  prod.get(),
                  "clamp(vertical, pressure [hPa], (300.0, 900.0)); "
                  "regrid(vertical, pressure [hPa], (700.0, 400.0)); "
                  "rebin(vertical, pressure_bounds [hPa], (900.0, 500.0, "
                  "200.0))"));
    EXPECT_EQ(2L, prod->dimension[harp_dimension_vertical]);

    EXPECT_EQ(-1, harp_product_execute_operations(
                      prod.get(),
                      "clamp(vertical, missing_axis [hPa], (300.0, 900.0))"));
    EXPECT_EQ(-1, harp_product_execute_operations(
                      prod.get(),
                      "rebin(vertical, missing_bounds [hPa], (900.0, 500.0))"));
    EXPECT_EQ(-1, harp_product_execute_operations(
                      prod.get(),
                      "regrid(vertical, missing_axis [hPa], (800.0, 400.0))"));
    EXPECT_EQ(-1, harp_product_execute_operations(
                      prod.get(),
                      "rebin(independent, pressure_bounds [hPa], "
                      "(900.0, 500.0))"));
    EXPECT_EQ(-1, harp_product_execute_operations(
                      prod.get(),
                      "regrid(independent, pressure [hPa], (800.0, 400.0))"));
  }

  // 3. derive() on existing variable (unit conv, set_unit, type conv) + new
  // derived variable with/without explicit data type.
  {
    ScopedHarpProduct prod = MakeSampleAtmosphericProduct(2, 2);
    AddDoubleVar(prod.get(), "height", 1,
                 std::array<harp_dimension_type, 1>{harp_dimension_time}.data(),
                 std::array<long, 1>{2}.data(), "m", {1000.0, 2000.0});
    AddStringVar(prod.get(), "label", 1,
                 std::array<harp_dimension_type, 1>{harp_dimension_time}.data(),
                 std::array<long, 1>{2}.data(), {"a", "b"});
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_execute_operations(
                  prod.get(),
                  "derive(height); derive(height [meter]); "
                  "derive(height [km]); derive(height float); "
                  "derive(latitude_bounds {time, independent} [degree_north]); "
                  "derive(collocation_index int32 {time})"));
    harp_variable* h = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_get_variable_by_name(prod.get(), "height", &h));
    EXPECT_EQ(harp_type_float, h->data_type);
    EXPECT_STREQ("km", h->unit);
    EXPECT_FLOAT_EQ(1.0f, h->data.float_data[0]);

    EXPECT_EQ(
        -1, harp_product_execute_operations(prod.get(), "derive(missing_var)"));
    EXPECT_EQ(
        -1, harp_product_execute_operations(prod.get(), "derive(height [s])"));
    EXPECT_EQ(-1, harp_product_execute_operations(prod.get(),
                                                  "derive(label double)"));
  }

  // 4. exclude(), keep(), flatten(), rename(), squash(), wrap().
  {
    ScopedHarpProduct prod = MakeSampleAtmosphericProduct(2, 2);
    AddDoubleVar(prod.get(), "angle", 1,
                 std::array<harp_dimension_type, 1>{harp_dimension_time}.data(),
                 std::array<long, 1>{2}.data(), nullptr, {370.0, -10.0});
    AddStringVar(prod.get(), "str_var", 1,
                 std::array<harp_dimension_type, 1>{harp_dimension_time}.data(),
                 std::array<long, 1>{2}.data(), {"x", "y"});
    EXPECT_EQ(-1, harp_product_execute_operations(
                      prod.get(), "squash(vertical, pressure)"));
    EXPECT_EQ(-1, harp_product_execute_operations(prod.get(), "flatten(time)"));
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_execute_operations(
                  prod.get(),
                  "rename(angle, azimuth); rename(angle, azimuth); "
                  "wrap(azimuth, 0.0, 360.0); "
                  "wrap(longitude [degree_east], -180.0, 180.0); "
                  "exclude(latitude_bounds, longitude_bounds, str_var); "
                  "keep(datetime, azimuth, longitude, pressure*); "
                  "squash(vertical, datetime); "
                  "flatten(vertical)"));
    harp_variable* az = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_get_variable_by_name(prod.get(), "azimuth", &az));
    EXPECT_THAT(std::vector<double>(az->data.double_data,
                                    az->data.double_data + az->num_elements),
                ElementsAre(10.0, 10.0, 350.0, 350.0));

    EXPECT_EQ(-1,
              harp_product_execute_operations(prod.get(), "keep(nonexistent)"));
    EXPECT_EQ(-1, harp_product_execute_operations(
                      prod.get(), "rename(missing_src, new_dst)"));
    EXPECT_EQ(-1, harp_product_execute_operations(
                      prod.get(), "wrap(missing_var, 0.0, 1.0)"));
    EXPECT_EQ(-1, harp_product_execute_operations(
                      prod.get(), "wrap(longitude [m], 0.0, 1.0)"));
    AddStringVar(prod.get(), "bad_wrap", 1,
                 std::array<harp_dimension_type, 1>{harp_dimension_time}.data(),
                 std::array<long, 1>{4}.data(), {"a", "b", "c", "d"});
    EXPECT_EQ(-1, harp_product_execute_operations(prod.get(),
                                                  "wrap(bad_wrap, 0.0, 1.0)"));
    EXPECT_EQ(-1, harp_product_execute_operations(prod.get(),
                                                  "squash(time, missing_var)"));
  }
}

TEST(HarpProgramTest, CollocatedBinRegridAndSmoothOperations) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  TempDir dir("harp_program_colloc_ds");
  TempDir dir2d("harp_program_colloc_ds2d");
  TempFile col_prod_file("harp_program_colloc_prod.nc");
  TempFile csv_file("harp_program_colloc.csv");
  TempFile csv2d_file("harp_program_colloc2d.csv");
  const std::string col_nc = col_prod_file.string();
  const std::string ds_nc = (dir.path() / "prod_b.nc").native();
  const std::string csv_path = csv_file.string();
  WriteCollocatedProduct(col_nc.c_str(), "prod_b", 2, 2,
                         /*include_collocation_index=*/true);
  // Dataset products omit `collocation_index` (providing `index` instead) to
  // avoid an upstream leak in harp_product_apply_collocation_mask when
  // `collocation_index` already exists.
  WriteCollocatedProduct(ds_nc.c_str(), "prod_b", 2, 2,
                         /*include_collocation_index=*/false);
  WriteCollocationCsv(csv_path.c_str(), "prod_a", "prod_b", 2);

  // 1. regrid with collocated product and collocated dataset (a & b).
  {
    ScopedHarpProduct p1 = MakeSampleAtmosphericProduct(2, 2);
    ASSERT_EQ(
        HARP_SUCCESS,
        harp_product_execute_operations(
            p1.get(),
            absl::StrFormat("regrid(vertical, pressure [hPa], \"%s\")", col_nc)
                .c_str()));
    EXPECT_EQ(2L, p1->dimension[harp_dimension_vertical]);

    ScopedHarpProduct p2 = MakeSampleAtmosphericProduct(2, 2);
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_execute_operations(
                  p2.get(), absl::StrFormat("regrid(vertical, pressure [hPa], "
                                            "\"%s\", b, \"%s\")",
                                            csv_path, dir.string())
                                .c_str()))
        << harp_errno_to_string(harp_errno);

    // Error paths for regrid collocated product/dataset (missing file, bad
    // dataset_dir after csv read succeeds, target_dataset=='a', and missing
    // vertical dimension after import succeeds).
    EXPECT_EQ(
        -1, harp_product_execute_operations(
                p1.get(), "regrid(vertical, pressure [hPa], \"/no/such.nc\")"));
    EXPECT_EQ(-1,
              harp_product_execute_operations(
                  p1.get(),
                  absl::StrFormat("regrid(vertical, pressure [hPa], \"%s\", a, "
                                  "\"/no/dir\")",
                                  csv_path)
                      .c_str()));
    ScopedHarpProduct p_novert = MakeSampleAtmosphericProduct(2, 0);
    EXPECT_EQ(-1, harp_product_execute_operations(
                      p_novert.get(),
                      absl::StrFormat(
                          "regrid(vertical, pressure [hPa], \"%s\")", col_nc)
                          .c_str()));
    EXPECT_EQ(-1,
              harp_product_execute_operations(
                  p_novert.get(),
                  absl::StrFormat("regrid(vertical, pressure [hPa], \"%s\", b, "
                                  "\"%s\")",
                                  csv_path, dir.string())
                      .c_str()));
  }

  // 2. smooth with collocated product and collocated dataset (a & b) + errors.
  {
    ScopedHarpProduct p1 = MakeSampleAtmosphericProduct(2, 2);
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_execute_operations(
                  p1.get(), absl::StrFormat("smooth(O3_column_number_density, "
                                            "vertical, pressure [hPa], \"%s\")",
                                            col_nc)
                                .c_str()))
        << harp_errno_to_string(harp_errno);

    ScopedHarpProduct p2 = MakeSampleAtmosphericProduct(2, 2);
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_execute_operations(
                  p2.get(), absl::StrFormat("smooth(O3_column_number_density, "
                                            "vertical, pressure [hPa], \"%s\", "
                                            "b, \"%s\")",
                                            csv_path, dir.string())
                                .c_str()))
        << harp_errno_to_string(harp_errno);

    // Non-vertical dimension error + missing file / bad dir / missing vertical
    // dim errors.
    EXPECT_EQ(-1, harp_product_execute_operations(
                      p1.get(),
                      absl::StrFormat("smooth(O3_column_number_density, time, "
                                      "pressure [hPa], \"%s\")",
                                      col_nc)
                          .c_str()));
    EXPECT_EQ(-1, harp_product_execute_operations(
                      p1.get(),
                      absl::StrFormat("smooth(O3_column_number_density, time, "
                                      "pressure [hPa], \"%s\", a, \"%s\")",
                                      csv_path, dir.string())
                          .c_str()));
    EXPECT_EQ(-1, harp_product_execute_operations(
                      p1.get(),
                      "smooth(O3_column_number_density, vertical, "
                      "pressure [hPa], \"/no/such.nc\")"));
    EXPECT_EQ(-1,
              harp_product_execute_operations(
                  p1.get(),
                  absl::StrFormat("smooth(O3_column_number_density, vertical, "
                                  "pressure [hPa], \"%s\", a, \"/no/dir\")",
                                  csv_path)
                      .c_str()));
    ScopedHarpProduct p_novert = MakeSampleAtmosphericProduct(2, 0);
    EXPECT_EQ(-1, harp_product_execute_operations(
                      p_novert.get(),
                      absl::StrFormat("smooth(O3_column_number_density, "
                                      "vertical, pressure [hPa], \"%s\")",
                                      col_nc)
                          .c_str()));
    EXPECT_EQ(-1, harp_product_execute_operations(
                      p_novert.get(),
                      absl::StrFormat("smooth(O3_column_number_density, "
                                      "vertical, pressure [hPa], \"%s\", b, "
                                      "\"%s\")",
                                      csv_path, dir.string())
                          .c_str()));
  }

  // 3. derive_smoothed_column with collocated product and dataset + errors.
  {
    // derive_smoothed_column(var {time} ...) expects 2D {time, vertical} avk.
    TempFile col2d_prod_file("harp_program_colloc_prod2d.nc");
    const std::string col2d_nc = col2d_prod_file.string();
    const std::string ds2d_nc = (dir2d.path() / "prod_b2d.nc").native();
    const std::string csv2d_path = csv2d_file.string();
    auto write_2d = [&](const char* path, bool include_collocation_index) {
      ScopedHarpProduct col2d = MakeNewProduct();
      ASSERT_EQ(HARP_SUCCESS,
                harp_product_set_source_product(col2d.get(), "prod_b2d"));
      const harp_dimension_type t_dim[] = {harp_dimension_time};
      const long t_len[] = {2};
      if (include_collocation_index) {
        AddInt32Var(col2d.get(), "collocation_index", 1, t_dim, t_len, nullptr,
                    {0, 1});
      }
      AddInt32Var(col2d.get(), "index", 1, t_dim, t_len, nullptr, {0, 1});
      const harp_dimension_type p_dim[] = {harp_dimension_time,
                                           harp_dimension_vertical};
      const long p_len[] = {2, 2};
      const harp_dimension_type pb_dim[] = {harp_dimension_time,
                                            harp_dimension_vertical,
                                            harp_dimension_independent};
      const long pb_len[] = {2, 2, 2};
      AddDoubleVar(col2d.get(), "pressure", 2, p_dim, p_len, "hPa",
                   {750.0, 450.0, 750.0, 450.0});
      AddDoubleVar(col2d.get(), "pressure_bounds", 3, pb_dim, pb_len, "hPa",
                   {900.0, 600.0, 600.0, 300.0, 900.0, 600.0, 600.0, 300.0});
      AddDoubleVar(col2d.get(), "O3_column_number_density_avk", 2, p_dim, p_len,
                   "", {1.0, 1.0, 1.0, 1.0});
      AddDoubleVar(col2d.get(), "O3_column_number_density_apriori", 2, p_dim,
                   p_len, "molec/cm2", {2.0, 2.0, 2.0, 2.0});
      ASSERT_EQ(HARP_SUCCESS, harp_export(path, "netcdf", col2d.get()));
    };
    write_2d(col2d_nc.c_str(), /*include_collocation_index=*/true);
    write_2d(ds2d_nc.c_str(), /*include_collocation_index=*/false);
    WriteCollocationCsv(csv2d_path.c_str(), "prod_a", "prod_b2d", 2);

    ScopedHarpProduct p1 = MakeSampleAtmosphericProduct(2, 2);
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_execute_operations(
                  p1.get(),
                  absl::StrFormat(
                      "derive_smoothed_column(O3_column_number_density {time} "
                      "[molec/cm2], pressure [hPa], \"%s\")",
                      col2d_nc)
                      .c_str()));

    ScopedHarpProduct p2 = MakeSampleAtmosphericProduct(2, 2);
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_execute_operations(
                  p2.get(),
                  absl::StrFormat(
                      "derive_smoothed_column(O3_column_number_density {time} "
                      "[molec/cm2], pressure [hPa], \"%s\", b, \"%s\")",
                      csv2d_path, dir2d.string())
                      .c_str()))
        << harp_errno_to_string(harp_errno);

    // Also cover the `add_variable` branch (target variable not yet present
    // in product, derived via pressure_bounds + O3_number_density).
    auto add_number_density_product = [&]() {
      ScopedHarpProduct p = MakeSampleAtmosphericProduct(2, 2);
      harp_variable* o3_col = nullptr;
      EXPECT_EQ(HARP_SUCCESS,
                harp_product_get_variable_by_name(
                    p.get(), "O3_column_number_density", &o3_col));
      EXPECT_EQ(HARP_SUCCESS, harp_product_remove_variable(p.get(), o3_col));
      const harp_dimension_type tv_dim[] = {harp_dimension_time,
                                            harp_dimension_vertical};
      const long tv_len[] = {2, 2};
      AddDoubleVar(p.get(), "O3_number_density", 2, tv_dim, tv_len, "molec/cm3",
                   {1e11, 1e11, 1e11, 1e11});
      // Add altitude_bounds so O3_column_number_density {time, vertical}
      // can be derived from O3_number_density.
      const harp_dimension_type ab_dim[] = {harp_dimension_time,
                                            harp_dimension_vertical,
                                            harp_dimension_independent};
      const long ab_len[] = {2, 2, 2};
      AddDoubleVar(p.get(), "altitude_bounds", 3, ab_dim, ab_len, "m",
                   {0.0, 1000.0, 1000.0, 2000.0, 0.0, 1000.0, 1000.0, 2000.0});
      return p;
    };
    {
      ScopedHarpProduct p_add1 = add_number_density_product();
      ASSERT_EQ(
          HARP_SUCCESS,
          harp_product_execute_operations(
              p_add1.get(),
              absl::StrFormat("derive_smoothed_column(O3_column_number_density "
                              "{time} [molec/cm2], pressure [hPa], \"%s\")",
                              col2d_nc)
                  .c_str()))
          << harp_errno_to_string(harp_errno);
    }
    {
      ScopedHarpProduct p_add2 = add_number_density_product();
      ASSERT_EQ(
          HARP_SUCCESS,
          harp_product_execute_operations(
              p_add2.get(),
              absl::StrFormat("derive_smoothed_column(O3_column_number_density "
                              "{time} [molec/cm2], pressure [hPa], \"%s\", b, "
                              "\"%s\")",
                              csv2d_path, dir2d.string())
                  .c_str()))
          << harp_errno_to_string(harp_errno);
    }

    EXPECT_EQ(-1, harp_product_execute_operations(
                      p1.get(),
                      "derive_smoothed_column(O3_column_number_density {time} "
                      "[molec/cm2], pressure [hPa], \"/no/such.nc\")"));
    EXPECT_EQ(
        -1, harp_product_execute_operations(
                p1.get(), absl::StrFormat(
                              "derive_smoothed_column(O3_column_number_density "
                              "{time} [molec/cm2], pressure [hPa], \"%s\", a, "
                              "\"/no/dir\")",
                              csv2d_path)
                              .c_str()));
    ScopedHarpProduct p_novert = MakeSampleAtmosphericProduct(2, 0);
    EXPECT_EQ(-1, harp_product_execute_operations(
                      p_novert.get(),
                      absl::StrFormat(
                          "derive_smoothed_column(O3_column_number_density "
                          "{time} [molec/cm2], pressure [hPa], \"%s\")",
                          col2d_nc)
                          .c_str()));
    EXPECT_EQ(-1, harp_product_execute_operations(
                      p_novert.get(),
                      absl::StrFormat(
                          "derive_smoothed_column(O3_column_number_density "
                          "{time} [molec/cm2], pressure [hPa], \"%s\", b, "
                          "\"%s\")",
                          csv2d_path, dir2d.string())
                          .c_str()));
  }

  // 4. bin_collocated error paths (missing CSV, inconsistent product).
  // Note: upstream execute_bin_collocated() in harp-program.c omits
  // harp_collocation_result_delete(collocation_result) on the success path,
  // so only error paths (which free collocation_result cleanly) are exercised.
  {
    ScopedHarpProduct p_err = MakeNewProduct();
    EXPECT_EQ(-1, harp_product_execute_operations(
                      p_err.get(), "bin(\"/no/such/colloc.csv\", a)"));
    EXPECT_EQ(-1, harp_product_execute_operations(
                      p_err.get(),
                      absl::StrFormat("bin(\"%s\", a)", csv_path).c_str()));
  }
}

TEST(HarpProgramTest, SetOperationValidAndInvalidValues) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const int orig_afgl86 = harp_get_option_enable_aux_afgl86();
  const int orig_usstd76 = harp_get_option_enable_aux_usstd76();
  const int orig_colloc_dt = harp_get_option_create_collocation_datetime();
  const int orig_prop_unc = harp_get_option_propagate_uncertainty();
  const int orig_oob = harp_get_option_regrid_out_of_bounds();
  auto restore = absl::MakeCleanup([&] {
    harp_set_option_enable_aux_afgl86(orig_afgl86);
    harp_set_option_enable_aux_usstd76(orig_usstd76);
    harp_set_option_create_collocation_datetime(orig_colloc_dt);
    harp_set_option_propagate_uncertainty(orig_prop_unc);
    harp_set_option_regrid_out_of_bounds(orig_oob);
    harp_set_error(HARP_SUCCESS, nullptr);
  });

  ScopedHarpProduct prod = MakeSampleAtmosphericProduct(2, 0);

  ASSERT_EQ(HARP_SUCCESS,
            harp_product_execute_operations(
                prod.get(),
                "set(\"afgl86\", \"enabled\"); "
                "set(\"afgl86\", \"usstd76\"); "
                "set(\"afgl86\", \"disabled\"); "
                "set(\"collocation_datetime\", \"enabled\"); "
                "set(\"collocation_datetime\", \"disabled\"); "
                "set(\"propagate_uncertainty\", \"correlated\"); "
                "set(\"propagate_uncertainty\", \"uncorrelated\"); "
                "set(\"regrid_out_of_bounds\", \"edge\"); "
                "set(\"regrid_out_of_bounds\", \"extrapolate\"); "
                "set(\"regrid_out_of_bounds\", \"nan\")"));

  constexpr const char* kInvalidSetOps[] = {
      "set(\"afgl86\", \"bad\")",
      "set(\"collocation_datetime\", \"bad\")",
      "set(\"propagate_uncertainty\", \"bad\")",
      "set(\"regrid_out_of_bounds\", \"bad\")",
      "set(\"unknown_option\", \"val\")",
  };
  for (const char* op_str : kInvalidSetOps) {
    EXPECT_EQ(-1, harp_product_execute_operations(prod.get(), op_str));
    EXPECT_EQ(HARP_ERROR_OPERATION, harp_errno);
  }
}

}  // namespace
}  // namespace stcorp_harp
