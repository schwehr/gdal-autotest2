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
// Unit tests for HARP products (libharp/harp-product.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-product.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp.h.in

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

// Not declared in any public or private header; signatures mirror
// harp-product.c.
extern "C" {
int harp_product_make_time_dependent(harp_product* product);
int harp_product_resize_dimension(harp_product* product,
                                  harp_dimension_type dimension_type,
                                  long length);
}  // extern "C"

namespace stcorp_harp {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

std::string* g_print_capture = nullptr;

int CapturePrint(const char* fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (g_print_capture != nullptr && n > 0) {
    g_print_capture->append(buf);
  }
  return n;
}

ScopedHarpProduct MakeProduct() {
  harp_product* raw = nullptr;
  EXPECT_EQ(HARP_SUCCESS, harp_product_new(&raw));
  return ScopedHarpProduct(raw);
}

ScopedHarpVariable MakeVar(const char* name, harp_data_type data_type,
                           std::span<const harp_dimension_type> dim_types,
                           std::span<const long> dims) {
  harp_variable* raw = nullptr;
  EXPECT_EQ(
      HARP_SUCCESS,
      harp_variable_new(name, data_type, static_cast<int>(dim_types.size()),
                        dim_types.data(), dims.data(), &raw));
  return ScopedHarpVariable(raw);
}

void AddDoubleVar(harp_product* product, const char* name,
                  std::span<const harp_dimension_type> dim_types,
                  std::span<const long> dims, std::span<const double> values,
                  const char* unit = nullptr) {
  ScopedHarpVariable var = MakeVar(name, harp_type_double, dim_types, dims);
  ASSERT_NE(nullptr, var.get());
  ASSERT_EQ(static_cast<long>(values.size()), var->num_elements);
  for (size_t i = 0; i < values.size(); ++i) {
    var->data.double_data[i] = values[i];
  }
  if (unit != nullptr) {
    ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(var.get(), unit));
  }
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(product, var.get()));
  var.release();
}

void AddInt32Var(harp_product* product, const char* name,
                 std::span<const harp_dimension_type> dim_types,
                 std::span<const long> dims, std::span<const int32_t> values) {
  ScopedHarpVariable var = MakeVar(name, harp_type_int32, dim_types, dims);
  ASSERT_NE(nullptr, var.get());
  ASSERT_EQ(static_cast<long>(values.size()), var->num_elements);
  for (size_t i = 0; i < values.size(); ++i) {
    var->data.int32_data[i] = values[i];
  }
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(product, var.get()));
  var.release();
}

TEST(HarpProductTest, LifecycleCopyAndMetadata) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_product_delete(nullptr);

  ScopedHarpProduct prod = MakeProduct();
  ASSERT_NE(nullptr, prod.get());
  EXPECT_EQ(1, harp_product_is_empty(prod.get()));
  EXPECT_EQ(0, prod->num_variables);

  // Set, overwrite, and clear source_product and history.
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_set_source_product(prod.get(), "/a/b/source1.nc"));
  EXPECT_STREQ("source1.nc", prod->source_product);
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_set_source_product(prod.get(), "source2.nc"));
  EXPECT_STREQ("source2.nc", prod->source_product);
  ASSERT_EQ(HARP_SUCCESS, harp_product_set_source_product(prod.get(), nullptr));
  EXPECT_EQ(nullptr, prod->source_product);
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_set_source_product(prod.get(), "/dir/final.h5"));

  ASSERT_EQ(HARP_SUCCESS, harp_product_set_history(prod.get(), "line1"));
  EXPECT_STREQ("line1", prod->history);
  ASSERT_EQ(HARP_SUCCESS, harp_product_set_history(prod.get(), nullptr));
  EXPECT_EQ(nullptr, prod->history);
  ASSERT_EQ(HARP_SUCCESS, harp_product_set_history(prod.get(), "line2"));

  // Add > BLOCK_SIZE (16) variables to exercise realloc growth.
  const harp_dimension_type t_dim[] = {harp_dimension_time};
  const long t_len[] = {2};
  const double t_vals[] = {1.0, 2.0};
  for (int i = 0; i < 20; ++i) {
    const std::string vname = "v_" + std::to_string(i);
    AddDoubleVar(prod.get(), vname.c_str(), t_dim, t_len, t_vals, "m");
  }
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_description(prod->variable[0], "test desc"));
  EXPECT_EQ(0, harp_product_is_empty(prod.get()));
  EXPECT_EQ(2, prod->dimension[harp_dimension_time]);

  // Storage size with and without attributes.
  int64_t size_no_attr = 0;
  int64_t size_with_attr = 0;
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_get_storage_size(prod.get(), 0, &size_no_attr));
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_get_storage_size(prod.get(), 1, &size_with_attr));
  EXPECT_EQ(20 * 2 * 8, size_no_attr);
  EXPECT_GT(size_with_attr, size_no_attr);

  // Copy product (both with metadata and without metadata).
  harp_product* raw_copy = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_copy(prod.get(), &raw_copy));
  ScopedHarpProduct copy(raw_copy);
  EXPECT_EQ(20, copy->num_variables);
  EXPECT_STREQ("final.h5", copy->source_product);
  EXPECT_STREQ("line2", copy->history);

  // Empty variable makes product empty.
  const long zero_len[] = {0};
  ScopedHarpVariable empty_var =
      MakeVar("empty_v", harp_type_int32, t_dim, zero_len);
  // Make a separate product with 0-length variable.
  ScopedHarpProduct empty_prod = MakeProduct();
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_add_variable(empty_prod.get(), empty_var.get()));
  empty_var.release();
  EXPECT_EQ(1, harp_product_is_empty(empty_prod.get()));
  harp_product* raw_empty_copy = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_copy(empty_prod.get(), &raw_empty_copy));
  ScopedHarpProduct empty_copy(raw_empty_copy);
  EXPECT_EQ(nullptr, empty_copy->source_product);
  EXPECT_EQ(nullptr, empty_copy->history);

  // Remove all variables (and repeat on empty).
  harp_product_remove_all_variables(copy.get());
  EXPECT_EQ(0, copy->num_variables);
  EXPECT_EQ(0, copy->dimension[harp_dimension_time]);
  harp_product_remove_all_variables(copy.get());
}

TEST(HarpProductTest, VariableAddDetachRemoveReplaceAndLookup) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  ScopedHarpProduct prod = MakeProduct();
  const harp_dimension_type t_dim[] = {harp_dimension_time};
  const harp_dimension_type v_dim[] = {harp_dimension_vertical};
  const harp_dimension_type tv_dim[] = {harp_dimension_time,
                                        harp_dimension_vertical};
  const harp_dimension_type vv_dim[] = {harp_dimension_vertical,
                                        harp_dimension_vertical};
  const harp_dimension_type i_dim[] = {harp_dimension_independent};
  const long len2[] = {2};
  const long len3[] = {3};
  const long len23[] = {2, 3};
  const long len33[] = {3, 3};

  // Error: NULL variable.
  EXPECT_EQ(-1, harp_product_add_variable(prod.get(), nullptr));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(-1, harp_product_detach_variable(prod.get(), nullptr));
  EXPECT_EQ(-1, harp_product_remove_variable(prod.get(), nullptr));
  EXPECT_EQ(-1, harp_product_replace_variable(prod.get(), nullptr));

  // Add scalar, independent, time, and vertical variables.
  AddDoubleVar(prod.get(), "scalar", {}, {}, std::array<double, 1>{1.0});
  AddDoubleVar(prod.get(), "ind", i_dim, len3,
               std::array<double, 3>{1.0, 2.0, 3.0});
  AddDoubleVar(prod.get(), "t", t_dim, len2, std::array<double, 2>{1.0, 2.0});
  AddDoubleVar(prod.get(), "tv", tv_dim, len23,
               std::array<double, 6>{1, 2, 3, 4, 5, 6});
  AddDoubleVar(prod.get(), "vv", vv_dim, len33,
               std::array<double, 9>{1, 2, 3, 4, 5, 6, 7, 8, 9});
  EXPECT_EQ(2, prod->dimension[harp_dimension_time]);
  EXPECT_EQ(3, prod->dimension[harp_dimension_vertical]);

  // Duplicate name and incompatible dimension on add.
  {
    ScopedHarpVariable dup = MakeVar("t", harp_type_double, t_dim, len2);
    EXPECT_EQ(-1, harp_product_add_variable(prod.get(), dup.get()));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

    ScopedHarpVariable bad_dim = MakeVar("bad", harp_type_double, t_dim, len3);
    EXPECT_EQ(-1, harp_product_add_variable(prod.get(), bad_dim.get()));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }

  // Lookup helpers and error paths.
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), nullptr));
  EXPECT_EQ(0, harp_product_has_variable(prod.get(), "missing"));
  EXPECT_EQ(1, harp_product_has_variable(prod.get(), "tv"));

  harp_variable* found = nullptr;
  int idx = -1;
  EXPECT_EQ(-1, harp_product_get_variable_by_name(prod.get(), nullptr, &found));
  EXPECT_EQ(-1, harp_product_get_variable_by_name(prod.get(), "tv", nullptr));
  EXPECT_EQ(-1,
            harp_product_get_variable_by_name(prod.get(), "missing", &found));
  EXPECT_EQ(HARP_ERROR_VARIABLE_NOT_FOUND, harp_errno);
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_get_variable_by_name(prod.get(), "tv", &found));
  EXPECT_STREQ("tv", found->name);

  EXPECT_EQ(-1,
            harp_product_get_variable_index_by_name(prod.get(), nullptr, &idx));
  EXPECT_EQ(-1,
            harp_product_get_variable_index_by_name(prod.get(), "tv", nullptr));
  EXPECT_EQ(
      -1, harp_product_get_variable_index_by_name(prod.get(), "missing", &idx));
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_get_variable_index_by_name(prod.get(), "tv", &idx));
  EXPECT_EQ(3, idx);

  // Replace variable with itself -> no-op success.
  ASSERT_EQ(HARP_SUCCESS, harp_product_replace_variable(prod.get(), found));

  // Replace missing variable / incompatible dimension.
  {
    ScopedHarpVariable not_there =
        MakeVar("not_there", harp_type_double, t_dim, len2);
    EXPECT_EQ(-1, harp_product_replace_variable(prod.get(), not_there.get()));
    EXPECT_EQ(HARP_ERROR_VARIABLE_NOT_FOUND, harp_errno);

    ScopedHarpVariable bad_repl = MakeVar("tv", harp_type_double, v_dim, len2);
    EXPECT_EQ(-1, harp_product_replace_variable(prod.get(), bad_repl.get()));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }

  // Compatible replace of "vv" with an independent variable clears vertical if
  // no other variable uses it; first remove "tv" so only "vv" has vertical.
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_remove_variable_by_name(prod.get(), "tv"));
  EXPECT_EQ(3, prod->dimension[harp_dimension_vertical]);
  {
    ScopedHarpVariable repl = MakeVar("vv", harp_type_double, i_dim, len2);
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_replace_variable(prod.get(), repl.get()));
    repl.release();
  }
  EXPECT_EQ(0, prod->dimension[harp_dimension_vertical]);

  // Detach & remove variable not in product.
  {
    ScopedHarpVariable foreign =
        MakeVar("foreign", harp_type_double, t_dim, len2);
    EXPECT_EQ(-1, harp_product_detach_variable(prod.get(), foreign.get()));
    EXPECT_EQ(HARP_ERROR_VARIABLE_NOT_FOUND, harp_errno);
    EXPECT_EQ(-1, harp_product_remove_variable(prod.get(), foreign.get()));
    EXPECT_EQ(-1, harp_product_remove_variable_by_name(prod.get(), "foreign"));
  }

  // Detach scalar (no active dims) and remove "t" (clears time dim).
  harp_variable* scalar_ptr = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              prod.get(), "scalar", &scalar_ptr));
  ASSERT_EQ(HARP_SUCCESS, harp_product_detach_variable(prod.get(), scalar_ptr));
  ScopedHarpVariable owned_scalar(scalar_ptr);
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_remove_variable_by_name(prod.get(), "t"));
  EXPECT_EQ(0, prod->dimension[harp_dimension_time]);
}

TEST(HarpProductTest, DimensionOperationsRearrangeFilterResizeRemoveFlatten) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  const harp_dimension_type t_dim[] = {harp_dimension_time};
  const harp_dimension_type v_dim[] = {harp_dimension_vertical};
  const harp_dimension_type tv_dim[] = {harp_dimension_time,
                                        harp_dimension_vertical};
  const harp_dimension_type lat_dim[] = {harp_dimension_latitude};
  const harp_dimension_type tlv_dim[] = {
      harp_dimension_time, harp_dimension_latitude, harp_dimension_vertical};
  const harp_dimension_type tvv_dim[] = {
      harp_dimension_time, harp_dimension_vertical, harp_dimension_vertical};

  // 1. make_time_dependent on time-independent product.
  {
    ScopedHarpProduct prod = MakeProduct();
    AddDoubleVar(prod.get(), "scalar", {}, {}, std::array<double, 1>{7.0});
    AddDoubleVar(prod.get(), "alt", v_dim, std::array<long, 1>{2},
                 std::array<double, 2>{10.0, 20.0});
    ASSERT_EQ(HARP_SUCCESS, harp_product_make_time_dependent(prod.get()));
    EXPECT_EQ(1, prod->dimension[harp_dimension_time]);
    harp_variable* scalar = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_get_variable_by_name(prod.get(), "scalar", &scalar));
    EXPECT_EQ(1, scalar->num_dimensions);
    EXPECT_EQ(harp_dimension_time, scalar->dimension_type[0]);
  }

  // 2. rearrange_dimension, filter_by_index, resize_dimension,
  // filter_dimension, remove_dimension.
  {
    ScopedHarpProduct prod = MakeProduct();
    AddInt32Var(prod.get(), "index", t_dim, std::array<long, 1>{3},
                std::array<int32_t, 3>{10, 20, 30});
    AddDoubleVar(prod.get(), "val", tv_dim, std::array<long, 2>{3, 2},
                 std::array<double, 6>{1, 2, 3, 4, 5, 6});
    AddDoubleVar(prod.get(), "lat", lat_dim, std::array<long, 1>{2},
                 std::array<double, 2>{-10.0, 10.0});

    // Error paths on rearrange_dimension and filter_dimension.
    const long dummy_ids[] = {0};
    const uint8_t dummy_mask[] = {1, 0, 1};
    EXPECT_EQ(-1, harp_product_rearrange_dimension(
                      prod.get(), harp_dimension_independent, 1, dummy_ids));
    EXPECT_EQ(-1, harp_product_rearrange_dimension(
                      prod.get(), harp_dimension_spectral, 1, dummy_ids));
    EXPECT_EQ(-1, harp_product_rearrange_dimension(
                      prod.get(), harp_dimension_time, 1, nullptr));
    EXPECT_EQ(-1, harp_product_filter_dimension(
                      prod.get(), harp_dimension_independent, dummy_mask));
    EXPECT_EQ(-1, harp_product_filter_dimension(
                      prod.get(), harp_dimension_spectral, dummy_mask));
    EXPECT_EQ(-1, harp_product_filter_dimension(prod.get(), harp_dimension_time,
                                                nullptr));

    // filter_by_index errors and success.
    int32_t bad_idx[] = {99};
    EXPECT_EQ(-1,
              harp_product_filter_by_index(prod.get(), "missing", 1, bad_idx));
    EXPECT_EQ(-1,
              harp_product_filter_by_index(prod.get(), "index", 1, bad_idx));

    int32_t keep_idx[] = {30, 10};
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_filter_by_index(prod.get(), "index", 2, keep_idx));
    EXPECT_EQ(2, prod->dimension[harp_dimension_time]);
    harp_variable* val = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_get_variable_by_name(prod.get(), "val", &val));
    EXPECT_THAT(std::span<const double>(val->data.double_data, 4),
                ElementsAre(5.0, 6.0, 1.0, 2.0));

    // rearrange_dimension / filter_by_index error when element id is out of
    // bounds, and resize_dimension error on negative length.
    const long oob_ids[] = {99};
    EXPECT_EQ(-1, harp_product_rearrange_dimension(
                      prod.get(), harp_dimension_time, 1, oob_ids));
    EXPECT_EQ(-1, harp_product_resize_dimension(prod.get(),
                                                harp_dimension_vertical, -1));

    // resize_dimension.
    ASSERT_EQ(HARP_SUCCESS, harp_product_resize_dimension(
                                prod.get(), harp_dimension_vertical, 3));
    EXPECT_EQ(3, prod->dimension[harp_dimension_vertical]);

    // filter_dimension keeping 1 element.
    const uint8_t keep_mask[] = {0, 1};
    ASSERT_EQ(HARP_SUCCESS, harp_product_filter_dimension(
                                prod.get(), harp_dimension_time, keep_mask));
    EXPECT_EQ(1, prod->dimension[harp_dimension_time]);

    // remove_dimension (active, inactive, and independent error).
    EXPECT_EQ(-1, harp_product_remove_dimension(prod.get(),
                                                harp_dimension_independent));
    ASSERT_EQ(HARP_SUCCESS, harp_product_remove_dimension(
                                prod.get(), harp_dimension_spectral));
    ASSERT_EQ(HARP_SUCCESS, harp_product_remove_dimension(
                                prod.get(), harp_dimension_latitude));
    EXPECT_EQ(0, prod->dimension[harp_dimension_latitude]);
    EXPECT_EQ(0, harp_product_has_variable(prod.get(), "lat"));

    // rearrange with 0 elements and filter with all-zero mask empty the
    // product.
    const uint8_t zero_mask[] = {0};
    ASSERT_EQ(HARP_SUCCESS, harp_product_filter_dimension(
                                prod.get(), harp_dimension_time, zero_mask));
    EXPECT_EQ(1, harp_product_is_empty(prod.get()));

    AddDoubleVar(prod.get(), "x", t_dim, std::array<long, 1>{1},
                 std::array<double, 1>{42.0});
    ASSERT_EQ(HARP_SUCCESS, harp_product_rearrange_dimension(
                                prod.get(), harp_dimension_time, 0, dummy_ids));
    EXPECT_EQ(1, harp_product_is_empty(prod.get()));
  }

  // 3. flatten_dimension: errors, dim_length == 0, dim_length == 1,
  // dim_length > 1 (with index, collocation_index, repeated dim, non-time-first
  // dim, dim_index > 1 transpose, and time-only var).
  // Note: harp_product_flatten_dimension evaluates product->dimension[dim]
  // before checking harp_dimension_independent (-1), so passing
  // harp_dimension_independent is UB/OOB-read in upstream harp-product.c:1909.
  {
    ScopedHarpProduct prod = MakeProduct();
    EXPECT_EQ(-1,
              harp_product_flatten_dimension(prod.get(), harp_dimension_time));
    EXPECT_EQ(HARP_SUCCESS, harp_product_flatten_dimension(
                                prod.get(), harp_dimension_vertical));

    // dim_length == 1 path.
    AddDoubleVar(prod.get(), "tv1", tv_dim, std::array<long, 2>{2, 1},
                 std::array<double, 2>{10.0, 20.0});
    ASSERT_EQ(HARP_SUCCESS, harp_product_flatten_dimension(
                                prod.get(), harp_dimension_vertical));
    EXPECT_EQ(0, prod->dimension[harp_dimension_vertical]);
    EXPECT_EQ(2, prod->dimension[harp_dimension_time]);
    harp_variable* tv1 = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_get_variable_by_name(prod.get(), "tv1", &tv1));
    EXPECT_EQ(1, tv1->num_dimensions);
  }

  {
    ScopedHarpProduct prod = MakeProduct();
    AddInt32Var(prod.get(), "index", t_dim, std::array<long, 1>{2},
                std::array<int32_t, 2>{0, 1});
    AddInt32Var(prod.get(), "collocation_index", t_dim, std::array<long, 1>{2},
                std::array<int32_t, 2>{5, 6});
    AddDoubleVar(prod.get(), "t_only", t_dim, std::array<long, 1>{2},
                 std::array<double, 2>{1.0, 2.0});
    AddDoubleVar(prod.get(), "lat_only", lat_dim, std::array<long, 1>{2},
                 std::array<double, 2>{10.0, 20.0});
    AddDoubleVar(prod.get(), "v_only", v_dim, std::array<long, 1>{2},
                 std::array<double, 2>{100.0, 200.0});
    AddDoubleVar(prod.get(), "tvv", tvv_dim, std::array<long, 3>{2, 2, 2},
                 std::array<double, 8>{1, 2, 3, 4, 5, 6, 7, 8});
    AddDoubleVar(prod.get(), "tlv", tlv_dim, std::array<long, 3>{2, 2, 2},
                 std::array<double, 8>{1, 2, 3, 4, 5, 6, 7, 8});

    ASSERT_EQ(HARP_SUCCESS, harp_product_flatten_dimension(
                                prod.get(), harp_dimension_vertical));
    EXPECT_EQ(4, prod->dimension[harp_dimension_time]);
    EXPECT_EQ(0, prod->dimension[harp_dimension_vertical]);
    EXPECT_EQ(0, harp_product_has_variable(prod.get(), "index"));
    EXPECT_EQ(0, harp_product_has_variable(prod.get(), "collocation_index"));
    EXPECT_EQ(0, harp_product_has_variable(prod.get(), "tvv"));
    EXPECT_EQ(1, harp_product_has_variable(prod.get(), "lat_only"));
    EXPECT_EQ(1, harp_product_has_variable(prod.get(), "t_only"));
    EXPECT_EQ(1, harp_product_has_variable(prod.get(), "v_only"));
    EXPECT_EQ(1, harp_product_has_variable(prod.get(), "tlv"));

    harp_variable* tlv = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_get_variable_by_name(prod.get(), "tlv", &tlv));
    EXPECT_EQ(2, tlv->num_dimensions);
    EXPECT_EQ(4, tlv->dimension[0]);
    EXPECT_EQ(2, tlv->dimension[1]);
  }

  // Flattening vertical when product->dimension[time] == 0 initially, plus 4D
  // variables to cover dim_index > 1 with trailing dims and shifting dims
  // after dim_index == 1.
  {
    const harp_dimension_type lon_dim[] = {harp_dimension_longitude};
    const harp_dimension_type tvll_dim[] = {
        harp_dimension_time, harp_dimension_vertical, harp_dimension_latitude,
        harp_dimension_longitude};
    const harp_dimension_type tlvl_dim[] = {
        harp_dimension_time, harp_dimension_latitude, harp_dimension_vertical,
        harp_dimension_longitude};
    ScopedHarpProduct prod = MakeProduct();
    AddDoubleVar(prod.get(), "v_only", v_dim, std::array<long, 1>{2},
                 std::array<double, 2>{100.0, 200.0});
    ASSERT_EQ(HARP_SUCCESS, harp_product_flatten_dimension(
                                prod.get(), harp_dimension_vertical));
    EXPECT_EQ(2, prod->dimension[harp_dimension_time]);
    EXPECT_EQ(0, prod->dimension[harp_dimension_vertical]);

    AddDoubleVar(prod.get(), "lon_only", lon_dim, std::array<long, 1>{2},
                 std::array<double, 2>{1.0, 2.0});
    AddDoubleVar(prod.get(), "tvll", tvll_dim, std::array<long, 4>{2, 2, 2, 2},
                 std::array<double, 16>{});
    AddDoubleVar(prod.get(), "tlvl", tlvl_dim, std::array<long, 4>{2, 2, 2, 2},
                 std::array<double, 16>{});
    ASSERT_EQ(HARP_SUCCESS, harp_product_flatten_dimension(
                                prod.get(), harp_dimension_vertical));
    EXPECT_EQ(4, prod->dimension[harp_dimension_time]);
    EXPECT_EQ(0, prod->dimension[harp_dimension_vertical]);
  }
}

TEST(HarpProductTest, AppendWithCountWeightAndDimensionAlignment) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  const harp_dimension_type t_dim[] = {harp_dimension_time};
  const harp_dimension_type v_dim[] = {harp_dimension_vertical};
  const harp_dimension_type tv_dim[] = {harp_dimension_time,
                                        harp_dimension_vertical};
  const harp_dimension_type tvi_dim[] = {
      harp_dimension_time, harp_dimension_vertical, harp_dimension_independent};
  const harp_dimension_type tvlat_dim[] = {
      harp_dimension_time, harp_dimension_vertical, harp_dimension_latitude};
  const harp_dimension_type tlat_dim[] = {harp_dimension_time,
                                          harp_dimension_latitude};

  // 1. other_product == NULL removes index, makes time-dependent, clears
  // source_product.
  {
    ScopedHarpProduct p = MakeProduct();
    ASSERT_EQ(HARP_SUCCESS, harp_product_set_source_product(p.get(), "a.nc"));
    AddInt32Var(p.get(), "index", t_dim, std::array<long, 1>{1},
                std::array<int32_t, 1>{0});
    AddDoubleVar(p.get(), "alt", v_dim, std::array<long, 1>{2},
                 std::array<double, 2>{1.0, 2.0});
    ASSERT_EQ(HARP_SUCCESS, harp_product_append(p.get(), nullptr));
    EXPECT_EQ(0, harp_product_has_variable(p.get(), "index"));
    EXPECT_EQ(nullptr, p->source_product);
    EXPECT_EQ(1, p->dimension[harp_dimension_time]);
  }

  // 2. Append two products with count, *_count, weight, *_weight, and different
  // vertical / latitude dimension sizes.
  {
    ScopedHarpProduct p1 = MakeProduct();
    ScopedHarpProduct p2 = MakeProduct();
    AddInt32Var(p1.get(), "index", t_dim, std::array<long, 1>{1},
                std::array<int32_t, 1>{0});
    AddInt32Var(p2.get(), "index", t_dim, std::array<long, 1>{1},
                std::array<int32_t, 1>{1});

    AddInt32Var(p1.get(), "count", t_dim, std::array<long, 1>{1},
                std::array<int32_t, 1>{3});
    AddInt32Var(p2.get(), "count", t_dim, std::array<long, 1>{1},
                std::array<int32_t, 1>{5});

    // p1 has weight [time, vertical(2)] and O3_weight; p2 has CO_count and
    // CO_weight.
    AddDoubleVar(p1.get(), "weight", tv_dim, std::array<long, 2>{1, 2},
                 std::array<double, 2>{1.0, 1.0});
    AddDoubleVar(p2.get(), "weight", tv_dim, std::array<long, 2>{1, 3},
                 std::array<double, 3>{2.0, 2.0, 2.0});
    AddDoubleVar(p1.get(), "O3_weight", tvi_dim, std::array<long, 3>{1, 2, 2},
                 std::array<double, 4>{1.0, 1.0, 1.0, 1.0});
    AddDoubleVar(p2.get(), "NO2_weight", tvlat_dim,
                 std::array<long, 3>{1, 3, 1}, std::array<double, 3>{1, 1, 1});
    AddInt32Var(p2.get(), "CO_count", tv_dim, std::array<long, 2>{1, 3},
                std::array<int32_t, 3>{4, 4, 4});
    AddDoubleVar(p2.get(), "CO_weight", tv_dim, std::array<long, 2>{1, 3},
                 std::array<double, 3>{3.0, 3.0, 3.0});

    // Also include latitude with p1 > p2 to test both resize directions.
    AddDoubleVar(p1.get(), "lat_data", tlat_dim, std::array<long, 2>{1, 3},
                 std::array<double, 3>{1.0, 2.0, 3.0});
    AddDoubleVar(p2.get(), "lat_data", tlat_dim, std::array<long, 2>{1, 1},
                 std::array<double, 1>{9.0});

    ASSERT_EQ(HARP_SUCCESS, harp_product_append(p1.get(), p2.get()));
    EXPECT_EQ(2, p1->dimension[harp_dimension_time]);
    EXPECT_EQ(3, p1->dimension[harp_dimension_vertical]);
    EXPECT_EQ(3, p1->dimension[harp_dimension_latitude]);
    EXPECT_EQ(1, harp_product_has_variable(p1.get(), "CO_count"));
    EXPECT_EQ(1, harp_product_has_variable(p1.get(), "CO_weight"));
    EXPECT_EQ(1, harp_product_has_variable(p1.get(), "NO2_weight"));
    EXPECT_EQ(1, harp_product_has_variable(p1.get(), "O3_weight"));
  }

  // 3. Error cases: invalid *_count without time first dim (both directions),
  // mismatched *_weight dimensions (same rank different dim type & fewer dims),
  // and variable mismatch in either direction.
  {
    ScopedHarpProduct p1 = MakeProduct();
    ScopedHarpProduct p2 = MakeProduct();
    AddInt32Var(p1.get(), "count", t_dim, std::array<long, 1>{1},
                std::array<int32_t, 1>{1});
    AddInt32Var(p2.get(), "count", t_dim, std::array<long, 1>{1},
                std::array<int32_t, 1>{2});
    AddInt32Var(p2.get(), "bad_count", v_dim, std::array<long, 1>{1},
                std::array<int32_t, 1>{2});
    EXPECT_EQ(-1, harp_product_append(p1.get(), p2.get()));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }
  {
    ScopedHarpProduct p1 = MakeProduct();
    ScopedHarpProduct p2 = MakeProduct();
    AddInt32Var(p1.get(), "count", t_dim, std::array<long, 1>{1},
                std::array<int32_t, 1>{1});
    AddInt32Var(p2.get(), "count", t_dim, std::array<long, 1>{1},
                std::array<int32_t, 1>{2});
    AddInt32Var(p1.get(), "bad_count", v_dim, std::array<long, 1>{1},
                std::array<int32_t, 1>{2});
    EXPECT_EQ(-1, harp_product_append(p1.get(), p2.get()));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }
  {
    ScopedHarpProduct p1 = MakeProduct();
    ScopedHarpProduct p2 = MakeProduct();
    AddDoubleVar(p1.get(), "weight", tv_dim, std::array<long, 2>{1, 2},
                 std::array<double, 2>{1.0, 1.0});
    AddDoubleVar(p2.get(), "weight", tv_dim, std::array<long, 2>{1, 2},
                 std::array<double, 2>{1.0, 1.0});
    // angle_weight has fewer dims and lat_weight has different dim type -> both
    // ignored by add_missing_count_and_weight_variables, failing match check.
    AddDoubleVar(p2.get(), "angle_weight", t_dim, std::array<long, 1>{1},
                 std::array<double, 1>{1.0});
    AddDoubleVar(p2.get(), "lat_weight", tlat_dim, std::array<long, 2>{1, 1},
                 std::array<double, 1>{1.0});
    EXPECT_EQ(-1, harp_product_append(p1.get(), p2.get()));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }
  {
    ScopedHarpProduct p1 = MakeProduct();
    ScopedHarpProduct p2 = MakeProduct();
    AddDoubleVar(p1.get(), "only_in_p1", t_dim, std::array<long, 1>{1},
                 std::array<double, 1>{1.0});
    EXPECT_EQ(-1, harp_product_append(p1.get(), p2.get()));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }
}

TEST(HarpProductTest, GetDatetimeRange) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  const harp_dimension_type t_dim[] = {harp_dimension_time};
  const long len3[] = {3};
  double start = 0.0;
  double stop = 0.0;

  // 1. Explicit datetime_start and datetime_stop (with NaN skipped).
  {
    ScopedHarpProduct prod = MakeProduct();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    AddDoubleVar(prod.get(), "datetime_start", t_dim, len3,
                 std::array<double, 3>{nan, 10.0, 5.0},
                 "days since 2000-01-01");
    AddDoubleVar(prod.get(), "datetime_stop", t_dim, len3,
                 std::array<double, 3>{6.0, nan, 12.0},
                 "days since 2000-01-01");
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_get_datetime_range(prod.get(), &start, &stop));
    EXPECT_DOUBLE_EQ(5.0, start);
    EXPECT_DOUBLE_EQ(12.0, stop);
  }

  // 2. Fallback to datetime (stored as float in seconds) for both start &
  // stop, plus stop-only call.
  {
    ScopedHarpProduct prod = MakeProduct();
    ScopedHarpVariable dt = MakeVar("datetime", harp_type_float, t_dim, len3);
    dt->data.float_data[0] = 86400.0f;
    dt->data.float_data[1] = 2.0f * 86400.0f;
    dt->data.float_data[2] = 3.0f * 86400.0f;
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_set_unit(dt.get(), "seconds since 2000-01-01"));
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), dt.get()));
    dt.release();

    ASSERT_EQ(HARP_SUCCESS,
              harp_product_get_datetime_range(prod.get(), &start, &stop));
    EXPECT_DOUBLE_EQ(1.0, start);
    EXPECT_DOUBLE_EQ(3.0, stop);

    double stop_only = 0.0;
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_get_datetime_range(prod.get(), nullptr, &stop_only));
    EXPECT_DOUBLE_EQ(3.0, stop_only);
  }

  // 3. Error cases: missing datetime, all-NaN start, all-NaN stop.
  {
    ScopedHarpProduct empty = MakeProduct();
    EXPECT_EQ(-1, harp_product_get_datetime_range(empty.get(), &start, &stop));
    EXPECT_EQ(-1, harp_product_get_datetime_range(empty.get(), nullptr, &stop));

    const double nan = std::numeric_limits<double>::quiet_NaN();
    AddDoubleVar(empty.get(), "datetime_start", t_dim, std::array<long, 1>{1},
                 std::array<double, 1>{nan}, "days since 2000-01-01");
    EXPECT_EQ(-1,
              harp_product_get_datetime_range(empty.get(), &start, nullptr));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

    AddDoubleVar(empty.get(), "datetime_stop", t_dim, std::array<long, 1>{1},
                 std::array<double, 1>{nan}, "days since 2000-01-01");
    EXPECT_EQ(-1, harp_product_get_datetime_range(empty.get(), nullptr, &stop));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }
}

TEST(HarpProductTest, UpdateHistoryVerifyAndPrint) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  ScopedHarpProduct prod = MakeProduct();

  // executable == NULL error.
  char arg0[] = "prog";
  char* empty_argv[] = {arg0};
  EXPECT_EQ(-1,
            harp_product_update_history(prod.get(), nullptr, 1, empty_argv));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  // Update history with plain and quoted arguments, twice to hit append '\n'.
  char arg1[] = "plain";
  char arg2[] = "a b;c[d]";
  char arg3[] = "x>y";
  char* argv[] = {arg0, arg1, arg2, arg3};
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_update_history(prod.get(), "harpconvert", 4, argv));
  EXPECT_THAT(prod->history, HasSubstr("harpconvert plain 'a b;c[d]' 'x>y'"));
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_update_history(prod.get(), "harpcheck", 2, argv));
  EXPECT_THAT(prod->history, HasSubstr("\n"));
  EXPECT_THAT(prod->history, HasSubstr("harpcheck plain"));

  // Print NULL and non-NULL product (with and without data/attributes).
  std::string captured;
  g_print_capture = &captured;
  auto clear_capture = absl::MakeCleanup([] { g_print_capture = nullptr; });

  harp_product_print(nullptr, 0, 0, CapturePrint);
  EXPECT_EQ("NULL\n", captured);

  captured.clear();
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_set_source_product(prod.get(), "src.nc"));
  const harp_dimension_type t_dim[] = {harp_dimension_time};
  AddDoubleVar(prod.get(), "v", t_dim, std::array<long, 1>{1},
               std::array<double, 1>{3.5}, "m");
  harp_product_print(prod.get(), 1, 1, CapturePrint);
  EXPECT_THAT(captured, HasSubstr("time = 1"));
  EXPECT_THAT(captured, HasSubstr("source_product = \"src.nc\""));
  EXPECT_THAT(captured, HasSubstr("history = "));
  EXPECT_THAT(captured, HasSubstr("data:"));

  // Verify valid product.
  const harp_dimension_type i_dim[] = {harp_dimension_independent};
  AddDoubleVar(prod.get(), "ind", i_dim, std::array<long, 1>{2},
               std::array<double, 2>{1.0, 2.0});
  EXPECT_EQ(HARP_SUCCESS, harp_product_verify(prod.get()));

  // Verify error paths via direct struct inspection/mutation.
  EXPECT_EQ(-1, harp_product_verify(nullptr));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  prod->dimension[harp_dimension_time] = -1;
  EXPECT_EQ(-1, harp_product_verify(prod.get()));
  EXPECT_EQ(HARP_ERROR_INVALID_PRODUCT, harp_errno);
  prod->dimension[harp_dimension_time] = 1;

  const int saved_num_vars = prod->num_variables;
  harp_variable** saved_vars = prod->variable;
  prod->num_variables = -1;
  EXPECT_EQ(-1, harp_product_verify(prod.get()));
  prod->num_variables = 1;
  prod->variable = nullptr;
  EXPECT_EQ(-1, harp_product_verify(prod.get()));
  prod->num_variables = saved_num_vars;
  prod->variable = saved_vars;

  // Variable mismatch with product dimension (variable itself stays internally
  // consistent while product->dimension[time] disagrees).
  prod->dimension[harp_dimension_time] = 99;
  EXPECT_EQ(-1, harp_product_verify(prod.get()));
  EXPECT_EQ(HARP_ERROR_INVALID_PRODUCT, harp_errno);
  prod->dimension[harp_dimension_time] = 1;

  // Invalid variable (with non-NULL name and with NULL name).
  char* saved_v0_name = prod->variable[0]->name;
  prod->variable[0]->name = nullptr;
  EXPECT_EQ(-1, harp_product_verify(prod.get()));
  prod->variable[0]->name = saved_v0_name;

  prod->variable[0]->num_elements = -1;
  EXPECT_EQ(-1, harp_product_verify(prod.get()));
  prod->variable[0]->num_elements = 1;

  // Variable[i] == NULL.
  harp_variable* saved_v0 = prod->variable[0];
  prod->variable[0] = nullptr;
  EXPECT_EQ(-1, harp_product_verify(prod.get()));
  prod->variable[0] = saved_v0;

  // Duplicate variable name.
  char* saved_v1_name = prod->variable[1]->name;
  prod->variable[1]->name = saved_v0_name;
  EXPECT_EQ(-1, harp_product_verify(prod.get()));
  EXPECT_EQ(HARP_ERROR_INVALID_PRODUCT, harp_errno);
  prod->variable[1]->name = saved_v1_name;
}

TEST(HarpProductTest, SortAllDataTypesAndErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  const harp_dimension_type t_dim[] = {harp_dimension_time};
  const harp_dimension_type v_dim[] = {harp_dimension_vertical};
  const harp_dimension_type i_dim[] = {harp_dimension_independent};
  const harp_dimension_type tv_dim[] = {harp_dimension_time,
                                        harp_dimension_vertical};
  const long len3[] = {3};
  const long len1[] = {1};
  const long len31[] = {3, 1};

  ScopedHarpProduct prod = MakeProduct();

  // Add one variable of each of the 6 HARP data types along time.
  // Primary key: k_int8 = {2, 1, 2}; tie-breaker: k_str = {"b", "x", "a"}.
  {
    ScopedHarpVariable v = MakeVar("k_int8", harp_type_int8, t_dim, len3);
    v->data.int8_data[0] = 2;
    v->data.int8_data[1] = 1;
    v->data.int8_data[2] = 2;
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), v.get()));
    v.release();
  }
  {
    ScopedHarpVariable v = MakeVar("k_int16", harp_type_int16, t_dim, len3);
    v->data.int16_data[0] = 30;
    v->data.int16_data[1] = 10;
    v->data.int16_data[2] = 20;
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), v.get()));
    v.release();
  }
  {
    ScopedHarpVariable v = MakeVar("k_int32", harp_type_int32, t_dim, len3);
    v->data.int32_data[0] = 100;
    v->data.int32_data[1] = 100;
    v->data.int32_data[2] = 50;
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), v.get()));
    v.release();
  }
  {
    ScopedHarpVariable v = MakeVar("k_float", harp_type_float, t_dim, len3);
    v->data.float_data[0] = 3.0f;
    v->data.float_data[1] = 1.0f;
    v->data.float_data[2] = 2.0f;
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), v.get()));
    v.release();
  }
  {
    ScopedHarpVariable v = MakeVar("k_double", harp_type_double, t_dim, len3);
    v->data.double_data[0] = 5.5;
    v->data.double_data[1] = 2.5;
    v->data.double_data[2] = 2.5;
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), v.get()));
    v.release();
  }
  {
    ScopedHarpVariable v = MakeVar("k_str", harp_type_string, t_dim, len3);
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_set_string_data_element(v.get(), 0, "b"));
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_set_string_data_element(v.get(), 1, "x"));
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_set_string_data_element(v.get(), 2, "a"));
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), v.get()));
    v.release();
  }
  AddDoubleVar(prod.get(), "v2d", tv_dim, len31,
               std::array<double, 3>{0.0, 1.0, 2.0});
  AddDoubleVar(prod.get(), "vert", v_dim, len1, std::array<double, 1>{5.0});
  AddDoubleVar(prod.get(), "ind", i_dim, len1, std::array<double, 1>{9.0});

  // Error paths.
  const char* keys_bad[] = {"missing"};
  EXPECT_EQ(-1, harp_product_sort(prod.get(), 0, keys_bad));
  EXPECT_EQ(-1, harp_product_sort(prod.get(), 1, keys_bad));
  const char* keys_2d[] = {"v2d"};
  EXPECT_EQ(-1, harp_product_sort(prod.get(), 1, keys_2d));
  const char* keys_ind[] = {"ind"};
  EXPECT_EQ(-1, harp_product_sort(prod.get(), 1, keys_ind));
  const char* keys_mismatch[] = {"k_int8", "vert"};
  EXPECT_EQ(-1, harp_product_sort(prod.get(), 2, keys_mismatch));

  // Multi-key sort across all 6 data types:
  // Order of k_int8: {1 (idx 1), 2 (idx 2, "a"), 2 (idx 0, "b")}.
  const char* all_keys[] = {"k_int8",  "k_str",   "k_int32",
                            "k_int16", "k_float", "k_double"};
  ASSERT_EQ(HARP_SUCCESS, harp_product_sort(prod.get(), 6, all_keys));
  harp_variable* k_str = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_get_variable_by_name(prod.get(), "k_str", &k_str));
  EXPECT_STREQ("x", k_str->data.string_data[0]);
  EXPECT_STREQ("a", k_str->data.string_data[1]);
  EXPECT_STREQ("b", k_str->data.string_data[2]);

  // Exercise <, >, == branches for int16, float, double, and string ties.
  const char* k_i16[] = {"k_int16"};
  ASSERT_EQ(HARP_SUCCESS, harp_product_sort(prod.get(), 1, k_i16));
  const char* k_flt[] = {"k_float"};
  ASSERT_EQ(HARP_SUCCESS, harp_product_sort(prod.get(), 1, k_flt));
  const char* k_dbl[] = {"k_double"};
  ASSERT_EQ(HARP_SUCCESS, harp_product_sort(prod.get(), 1, k_dbl));

  // Equal strings fall through to next key; identical rows return 0.
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_string_data_element(k_str, 0, "z"));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_string_data_element(k_str, 1, "z"));
  const char* k_str_tie[] = {"k_str", "k_int32"};
  ASSERT_EQ(HARP_SUCCESS, harp_product_sort(prod.get(), 2, k_str_tie));
  const char* k_i32_same[] = {"k_int32"};
  ASSERT_EQ(HARP_SUCCESS, harp_product_sort(prod.get(), 1, k_i32_same));
}

}  // namespace
}  // namespace stcorp_harp
