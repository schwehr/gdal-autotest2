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
// Unit tests for HARP operation constructors, destructors, predicates, and
// filter evaluators (libharp/harp-operation.c).
//
// Google3 build notes: HDF4 is disabled (#undef HAVE_HDF4); udunits2 XML is
// embedded via //third_party/udunits.
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-operation.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-operation.h

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/oom_fault_inject.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/google/util/temp_file.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

class HarpOperationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(HARP_SUCCESS, init_.status());
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  void TearDown() override { harp_set_error(HARP_SUCCESS, nullptr); }

  ScopedHarpInit init_;
};

ScopedHarpSphericalPolygon MakeBoxPolygonDeg(double lat_min, double lat_max,
                                             double lon_min, double lon_max) {
  harp_spherical_polygon* raw = nullptr;
  double lat_bounds[2] = {lat_min, lat_max};
  double lon_bounds[2] = {lon_min, lon_max};
  EXPECT_EQ(HARP_SUCCESS, harp_spherical_polygon_from_latitude_longitude_bounds(
                              0, 2, lat_bounds, lon_bounds, 1, &raw));
  return ScopedHarpSphericalPolygon(raw);
}

harp_spherical_point MakePointRad(double lat_deg, double lon_deg) {
  harp_spherical_point point;
  point.lat = lat_deg;
  point.lon = lon_deg;
  harp_spherical_point_rad_from_deg(&point);
  harp_spherical_point_check(&point);
  return point;
}

TEST_F(HarpOperationTest, AreaFilterConstructorsAndValidation) {
  TempFile valid_mask("harp_op_area_valid.csv");
  ASSERT_TRUE(WriteFileText(valid_mask.path(),
                            "polygon\n"
                            "0.0,0.0, 0.0,10.0, 10.0,10.0, 10.0,0.0\n"));

  TempFile bad_mask("harp_op_area_bad.csv");
  ASSERT_TRUE(WriteFileText(bad_mask.path(),
                            "polygon\n"
                            "not_a_number\n"));

  TempFile missing_mask("harp_op_area_missing.csv");

  double lat_deg[] = {0.0, 10.0};
  double lon_deg[] = {0.0, 10.0};
  double lat_rad[] = {0.0, std::numbers::pi / 18.0};
  double lon_rad[] = {0.0, std::numbers::pi / 18.0};
  double one_pt[] = {0.0};
  double same_lat[] = {5.0, 5.0};
  double wide_lat[] = {-90.0, 90.0};
  double wide_lon[] = {0.0, 90.0};
  double min_fraction = 0.5;

  // 1. Validation errors shared across area_covers/inside/intersects/point_in.
  harp_operation* raw_op = nullptr;
  EXPECT_EQ(-1,
            harp_operation_area_covers_area_filter_new(
                nullptr, 2, lat_deg, nullptr, 1, lon_deg, nullptr, &raw_op));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(-1, harp_operation_area_covers_area_filter_new(
                    valid_mask.c_str(), 2, lat_deg, nullptr, 2, lon_deg,
                    nullptr, &raw_op));
  EXPECT_EQ(-1,
            harp_operation_area_covers_area_filter_new(
                nullptr, 0, nullptr, nullptr, 0, nullptr, nullptr, &raw_op));

  EXPECT_EQ(-1,
            harp_operation_area_inside_area_filter_new(
                nullptr, 2, lat_deg, nullptr, 1, lon_deg, nullptr, &raw_op));
  EXPECT_EQ(-1, harp_operation_area_inside_area_filter_new(
                    valid_mask.c_str(), 2, lat_deg, nullptr, 2, lon_deg,
                    nullptr, &raw_op));
  EXPECT_EQ(-1,
            harp_operation_area_inside_area_filter_new(
                nullptr, 0, nullptr, nullptr, 0, nullptr, nullptr, &raw_op));

  EXPECT_EQ(-1, harp_operation_area_intersects_area_filter_new(
                    nullptr, 2, lat_deg, nullptr, 1, lon_deg, nullptr, nullptr,
                    &raw_op));
  EXPECT_EQ(-1, harp_operation_area_intersects_area_filter_new(
                    valid_mask.c_str(), 2, lat_deg, nullptr, 2, lon_deg,
                    nullptr, nullptr, &raw_op));
  EXPECT_EQ(-1, harp_operation_area_intersects_area_filter_new(
                    nullptr, 0, nullptr, nullptr, 0, nullptr, nullptr, nullptr,
                    &raw_op));

  EXPECT_EQ(-1,
            harp_operation_point_in_area_filter_new(
                nullptr, 2, lat_deg, nullptr, 1, lon_deg, nullptr, &raw_op));
  EXPECT_EQ(-1, harp_operation_point_in_area_filter_new(
                    valid_mask.c_str(), 2, lat_deg, nullptr, 2, lon_deg,
                    nullptr, &raw_op));
  EXPECT_EQ(-1,
            harp_operation_point_in_area_filter_new(
                nullptr, 0, nullptr, nullptr, 0, nullptr, nullptr, &raw_op));

  // 2. Missing and malformed area mask files.
  EXPECT_EQ(-1, harp_operation_area_covers_area_filter_new(
                    missing_mask.c_str(), 0, nullptr, nullptr, 0, nullptr,
                    nullptr, &raw_op));
  EXPECT_EQ(-1, harp_operation_area_covers_area_filter_new(
                    bad_mask.c_str(), 0, nullptr, nullptr, 0, nullptr, nullptr,
                    &raw_op));
  EXPECT_EQ(-1, harp_operation_area_inside_area_filter_new(
                    missing_mask.c_str(), 0, nullptr, nullptr, 0, nullptr,
                    nullptr, &raw_op));
  EXPECT_EQ(-1, harp_operation_area_intersects_area_filter_new(
                    missing_mask.c_str(), 0, nullptr, nullptr, 0, nullptr,
                    nullptr, nullptr, &raw_op));
  EXPECT_EQ(-1, harp_operation_point_in_area_filter_new(
                    missing_mask.c_str(), 0, nullptr, nullptr, 0, nullptr,
                    nullptr, &raw_op));

  // 3. Unit conversion failures and polygon geometry failures.
  EXPECT_EQ(-1,
            harp_operation_area_covers_area_filter_new(
                nullptr, 2, lat_deg, "m", 2, lon_deg, "degree_east", &raw_op));
  EXPECT_EQ(-1,
            harp_operation_area_covers_area_filter_new(
                nullptr, 2, lat_deg, "degree_north", 2, lon_deg, "m", &raw_op));
  EXPECT_EQ(-1, harp_operation_area_covers_area_filter_new(
                    nullptr, 2, same_lat, "degree_north", 2, lon_deg,
                    "degree_east", &raw_op));
  EXPECT_EQ(-1, harp_operation_area_inside_area_filter_new(
                    nullptr, 2, same_lat, "degree_north", 2, lon_deg,
                    "degree_east", &raw_op));
  EXPECT_EQ(-1, harp_operation_area_intersects_area_filter_new(
                    nullptr, 2, same_lat, "degree_north", 2, lon_deg,
                    "degree_east", nullptr, &raw_op));
  EXPECT_EQ(-1, harp_operation_point_in_area_filter_new(
                    nullptr, 2, same_lat, "degree_north", 2, lon_deg,
                    "degree_east", &raw_op));
  EXPECT_EQ(-1, harp_operation_area_covers_area_filter_new(
                    nullptr, 1, one_pt, nullptr, 1, one_pt, nullptr, &raw_op));
  // 2-point bounds spanning > 180 deg longitude pass
  // harp_spherical_polygon_from_latitude_longitude_bounds (which skips
  // check_polygon for num_vertices == 2) and then fail
  // harp_area_mask_add_polygon
  // -> harp_spherical_polygon_check.
  raw_op = nullptr;
  EXPECT_EQ(-1,
            harp_operation_area_covers_area_filter_new(
                nullptr, 2, wide_lat, nullptr, 2, wide_lon, nullptr, &raw_op));
  ScopedHarpOperation op_wide_guard(raw_op);
  raw_op = nullptr;

  // 4. Valid constructions from file, degree bounds, and radian bounds.
  ASSERT_EQ(HARP_SUCCESS, harp_operation_area_covers_area_filter_new(
                              valid_mask.c_str(), 0, nullptr, nullptr, 0,
                              nullptr, nullptr, &raw_op));
  ScopedHarpOperation op_covers_file(raw_op);

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_area_covers_area_filter_new(
                nullptr, 2, lat_rad, "rad", 2, lon_rad, "rad", &raw_op));
  ScopedHarpOperation op_covers_rad(raw_op);

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_area_inside_area_filter_new(
                              valid_mask.c_str(), 0, nullptr, nullptr, 0,
                              nullptr, nullptr, &raw_op));
  ScopedHarpOperation op_inside_file(raw_op);

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_area_inside_area_filter_new(
                              nullptr, 2, lat_deg, "degree_north", 2, lon_deg,
                              "degree_east", &raw_op));
  ScopedHarpOperation op_inside_deg(raw_op);

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_area_intersects_area_filter_new(
                              valid_mask.c_str(), 0, nullptr, nullptr, 0,
                              nullptr, nullptr, &min_fraction, &raw_op));
  ScopedHarpOperation op_intersects_file(raw_op);

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_area_intersects_area_filter_new(
                              nullptr, 2, lat_deg, nullptr, 2, lon_deg, nullptr,
                              nullptr, &raw_op));
  ScopedHarpOperation op_intersects_deg(raw_op);

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_point_in_area_filter_new(
                              valid_mask.c_str(), 0, nullptr, nullptr, 0,
                              nullptr, nullptr, &raw_op));
  ScopedHarpOperation op_point_file(raw_op);

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_point_in_area_filter_new(
                nullptr, 2, lat_deg, nullptr, 2, lon_deg, nullptr, &raw_op));
  ScopedHarpOperation op_point_deg(raw_op);
}

TEST_F(HarpOperationTest, AreaAndPointFilterEvaluation) {
  ScopedHarpSphericalPolygon outer = MakeBoxPolygonDeg(0.0, 10.0, 0.0, 10.0);
  ScopedHarpSphericalPolygon inner = MakeBoxPolygonDeg(2.0, 8.0, 2.0, 8.0);
  ScopedHarpSphericalPolygon partial = MakeBoxPolygonDeg(8.0, 18.0, 8.0, 18.0);
  ScopedHarpSphericalPolygon outside =
      MakeBoxPolygonDeg(30.0, 40.0, 30.0, 40.0);
  ASSERT_NE(nullptr, outer);
  ASSERT_NE(nullptr, inner);
  ASSERT_NE(nullptr, partial);
  ASSERT_NE(nullptr, outside);

  double inner_lat[] = {2.0, 8.0};
  double inner_lon[] = {2.0, 8.0};
  double outer_lat[] = {0.0, 10.0};
  double outer_lon[] = {0.0, 10.0};

  // 1. eval_area_covers_area: mask (inner) is inside outer, not inside outside.
  harp_operation* raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_area_covers_area_filter_new(
                              nullptr, 2, inner_lat, nullptr, 2, inner_lon,
                              nullptr, &raw_op));
  ScopedHarpOperation op_covers(raw_op);
  auto* poly_filter =
      reinterpret_cast<harp_operation_polygon_filter*>(op_covers.get());
  EXPECT_EQ(1, poly_filter->eval(poly_filter, outer.get()));
  EXPECT_EQ(0, poly_filter->eval(poly_filter, outside.get()));

  // 2. area_covers_point: unit validation + eval.
  raw_op = nullptr;
  EXPECT_EQ(-1, harp_operation_area_covers_point_filter_new(
                    5.0, "m", 5.0, "degree_east", &raw_op));
  EXPECT_EQ(-1, harp_operation_area_covers_point_filter_new(5.0, "degree_north",
                                                            5.0, "m", &raw_op));
  ASSERT_EQ(HARP_SUCCESS, harp_operation_area_covers_point_filter_new(
                              5.0 * std::numbers::pi / 180.0, "rad",
                              5.0 * std::numbers::pi / 180.0, "rad", &raw_op));
  ScopedHarpOperation op_covers_pt(raw_op);
  poly_filter =
      reinterpret_cast<harp_operation_polygon_filter*>(op_covers_pt.get());
  EXPECT_EQ(1, poly_filter->eval(poly_filter, outer.get()));
  EXPECT_EQ(0, poly_filter->eval(poly_filter, outside.get()));

  // 3. eval_area_inside_area: mask (outer) covers inner, not outside.
  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_area_inside_area_filter_new(
                              nullptr, 2, outer_lat, nullptr, 2, outer_lon,
                              nullptr, &raw_op));
  ScopedHarpOperation op_inside(raw_op);
  poly_filter =
      reinterpret_cast<harp_operation_polygon_filter*>(op_inside.get());
  EXPECT_EQ(1, poly_filter->eval(poly_filter, inner.get()));
  EXPECT_EQ(0, poly_filter->eval(poly_filter, outside.get()));

  // 4. eval_area_intersects_area without and with min_fraction.
  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_area_intersects_area_filter_new(
                              nullptr, 2, outer_lat, nullptr, 2, outer_lon,
                              nullptr, nullptr, &raw_op));
  ScopedHarpOperation op_intersects_nofrac(raw_op);
  poly_filter = reinterpret_cast<harp_operation_polygon_filter*>(
      op_intersects_nofrac.get());
  EXPECT_EQ(1, poly_filter->eval(poly_filter, partial.get()));
  EXPECT_EQ(0, poly_filter->eval(poly_filter, outside.get()));

  double min_fraction = 0.5;
  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_area_intersects_area_filter_new(
                              nullptr, 2, outer_lat, nullptr, 2, outer_lon,
                              nullptr, &min_fraction, &raw_op));
  ScopedHarpOperation op_intersects_frac(raw_op);
  poly_filter = reinterpret_cast<harp_operation_polygon_filter*>(
      op_intersects_frac.get());
  EXPECT_EQ(1, poly_filter->eval(poly_filter, inner.get()));
  EXPECT_EQ(0, poly_filter->eval(poly_filter, partial.get()));

  // 5. point_distance_filter: unit validation + eval.
  raw_op = nullptr;
  EXPECT_EQ(-1, harp_operation_point_distance_filter_new(
                    0.0, "m", 0.0, "degree_east", 100.0, "km", &raw_op));
  EXPECT_EQ(-1, harp_operation_point_distance_filter_new(
                    0.0, "degree_north", 0.0, "m", 100.0, "km", &raw_op));
  EXPECT_EQ(-1,
            harp_operation_point_distance_filter_new(
                0.0, "degree_north", 0.0, "degree_east", 100.0, "s", &raw_op));
  ASSERT_EQ(HARP_SUCCESS, harp_operation_point_distance_filter_new(
                              0.0, "rad", 0.0, "rad", 200.0, "km", &raw_op));
  ScopedHarpOperation op_dist(raw_op);
  auto* pt_filter =
      reinterpret_cast<harp_operation_point_filter*>(op_dist.get());
  harp_spherical_point near_pt = MakePointRad(0.5, 0.5);
  harp_spherical_point far_pt = MakePointRad(20.0, 20.0);
  EXPECT_EQ(1, pt_filter->eval(pt_filter, &near_pt));
  EXPECT_EQ(0, pt_filter->eval(pt_filter, &far_pt));

  // Also cover nullptr units for point_distance_filter_new.
  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_point_distance_filter_new(0.0, nullptr, 0.0, nullptr,
                                                     1000.0, nullptr, &raw_op));
  ScopedHarpOperation op_dist_null_units(raw_op);

  // 6. eval_point_in_area.
  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_point_in_area_filter_new(
                              nullptr, 2, outer_lat, nullptr, 2, outer_lon,
                              nullptr, &raw_op));
  ScopedHarpOperation op_pt_in_area(raw_op);
  pt_filter =
      reinterpret_cast<harp_operation_point_filter*>(op_pt_in_area.get());
  harp_spherical_point inside_pt = MakePointRad(5.0, 5.0);
  EXPECT_EQ(1, pt_filter->eval(pt_filter, &inside_pt));
  EXPECT_EQ(0, pt_filter->eval(pt_filter, &far_pt));
}

TEST_F(HarpOperationTest, BitMaskFilterNewAndEval) {
  struct BitMaskCase {
    harp_bit_mask_operator_type op_type;
    uint32_t mask;
    uint32_t val;
    int expected;
  };
  constexpr BitMaskCase kCases[] = {
      {.op_type = operator_bit_mask_all,
       .mask = 0x05,
       .val = 0x07,
       .expected = 1},
      {.op_type = operator_bit_mask_all,
       .mask = 0x05,
       .val = 0x04,
       .expected = 0},
      {.op_type = operator_bit_mask_any,
       .mask = 0x05,
       .val = 0x04,
       .expected = 1},
      {.op_type = operator_bit_mask_any,
       .mask = 0x05,
       .val = 0x02,
       .expected = 0},
      {.op_type = operator_bit_mask_none,
       .mask = 0x05,
       .val = 0x02,
       .expected = 1},
      {.op_type = operator_bit_mask_none,
       .mask = 0x05,
       .val = 0x01,
       .expected = 0},
  };

  for (const auto& tc : kCases) {
    harp_operation* raw_op = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_operation_bit_mask_filter_new(
                                "flags", tc.op_type, tc.mask, &raw_op));
    ScopedHarpOperation op(raw_op);
    auto* filter =
        reinterpret_cast<harp_operation_numeric_value_filter*>(op.get());

    int8_t v8 = static_cast<int8_t>(tc.val);
    int16_t v16 = static_cast<int16_t>(tc.val);
    int32_t v32 = static_cast<int32_t>(tc.val);
    EXPECT_EQ(tc.expected, filter->eval(filter, harp_type_int8, &v8));
    EXPECT_EQ(tc.expected, filter->eval(filter, harp_type_int16, &v16));
    EXPECT_EQ(tc.expected, filter->eval(filter, harp_type_int32, &v32));

    double dval = 1.0;
    EXPECT_EQ(-1, filter->eval(filter, harp_type_double, &dval));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }
}

TEST_F(HarpOperationTest, CollocationFilterPrepareAndEval) {
  TempFile coll_csv("harp_op_collocation.csv");
  // Deliberately unsorted with a duplicate index_a=30 to exercise qsort
  // compare_int32 (<, >, ==) and deduplication.
  ASSERT_TRUE(WriteFileText(
      coll_csv.path(),
      "collocation_index,source_product_a,index_a,source_product_b,index_b,"
      "datetime_diff [s]\n"
      "0,prod_a.nc,30,prod_b.nc,1,0.0\n"
      "1,prod_a.nc,10,prod_b.nc,2,0.0\n"
      "2,prod_a.nc,30,prod_b.nc,3,0.0\n"
      "3,prod_a.nc,20,prod_b.nc,4,0.0\n"
      "4,prod_a.nc,40,prod_b.nc,5,0.0\n"));

  TempFile missing_csv("harp_op_collocation_missing.csv");

  harp_operation* raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_collocation_filter_new(
                missing_csv.c_str(), harp_collocation_left, -1, -1, &raw_op));
  ScopedHarpOperation op_missing(raw_op);
  EXPECT_EQ(-1, harp_operation_prepare_collocation_filter(op_missing.get(),
                                                          "prod_a.nc"));

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_collocation_filter_new(
                coll_csv.c_str(), harp_collocation_left, -1, -1, &raw_op));
  ScopedHarpOperation op(raw_op);
  auto* filter =
      reinterpret_cast<harp_operation_numeric_value_filter*>(op.get());

  // Unsupported data type and empty-list evaluation before prepare.
  double dval = 10.0;
  EXPECT_EQ(-1, filter->eval(filter, harp_type_double, &dval));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  int32_t idx = 10;
  EXPECT_EQ(0, filter->eval(filter, harp_type_int32, &idx));

  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_prepare_collocation_filter(op.get(), "prod_a.nc"));
  for (int32_t present : {10, 20, 30, 40}) {
    EXPECT_EQ(1, filter->eval(filter, harp_type_int32, &present));
  }
  for (int32_t absent : {5, 15, 25, 35, 50}) {
    EXPECT_EQ(0, filter->eval(filter, harp_type_int32, &absent));
  }

  // Re-preparing replaces the previous collocation_mask and value buffer.
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_prepare_collocation_filter(op.get(), "prod_a.nc"));
  idx = 20;
  EXPECT_EQ(1, filter->eval(filter, harp_type_int32, &idx));
}

TEST_F(HarpOperationTest, ComparisonFilterNewUnitAndEval) {
  struct CmpCase {
    harp_comparison_operator_type op_type;
    double rhs;
    double lhs;
    int expected;
  };
  constexpr CmpCase kCases[] = {
      {.op_type = operator_eq, .rhs = 10.0, .lhs = 10.0, .expected = 1},
      {.op_type = operator_eq, .rhs = 10.0, .lhs = 9.0, .expected = 0},
      {.op_type = operator_ne, .rhs = 10.0, .lhs = 9.0, .expected = 1},
      {.op_type = operator_ne, .rhs = 10.0, .lhs = 10.0, .expected = 0},
      {.op_type = operator_lt, .rhs = 10.0, .lhs = 9.0, .expected = 1},
      {.op_type = operator_lt, .rhs = 10.0, .lhs = 10.0, .expected = 0},
      {.op_type = operator_le, .rhs = 10.0, .lhs = 10.0, .expected = 1},
      {.op_type = operator_le, .rhs = 10.0, .lhs = 11.0, .expected = 0},
      {.op_type = operator_gt, .rhs = 10.0, .lhs = 11.0, .expected = 1},
      {.op_type = operator_gt, .rhs = 10.0, .lhs = 10.0, .expected = 0},
      {.op_type = operator_ge, .rhs = 10.0, .lhs = 10.0, .expected = 1},
      {.op_type = operator_ge, .rhs = 10.0, .lhs = 9.0, .expected = 0},
  };

  for (const auto& tc : kCases) {
    harp_operation* raw_op = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_operation_comparison_filter_new("altitude", tc.op_type,
                                                   tc.rhs, nullptr, &raw_op));
    ScopedHarpOperation op(raw_op);
    EXPECT_EQ(HARP_SUCCESS, harp_operation_set_value_unit(op.get(), "m"));
    auto* filter =
        reinterpret_cast<harp_operation_numeric_value_filter*>(op.get());

    int8_t v8 = static_cast<int8_t>(tc.lhs);
    int16_t v16 = static_cast<int16_t>(tc.lhs);
    int32_t v32 = static_cast<int32_t>(tc.lhs);
    float vf = static_cast<float>(tc.lhs);
    double vd = tc.lhs;
    EXPECT_EQ(tc.expected, filter->eval(filter, harp_type_int8, &v8));
    EXPECT_EQ(tc.expected, filter->eval(filter, harp_type_int16, &v16));
    EXPECT_EQ(tc.expected, filter->eval(filter, harp_type_int32, &v32));
    EXPECT_EQ(tc.expected, filter->eval(filter, harp_type_float, &vf));
    EXPECT_EQ(tc.expected, filter->eval(filter, harp_type_double, &vd));

    char s[] = "10";
    char* sp = s;
    EXPECT_EQ(-1, filter->eval(filter, harp_type_string, &sp));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }

  // Unit conversion path: target unit "km", source unit "m", then "cm", then
  // incompatible "s".
  harp_operation* raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_comparison_filter_new(
                              "altitude", operator_ge, 2.0, "km", &raw_op));
  ScopedHarpOperation op_unit(raw_op);
  EXPECT_EQ(HARP_SUCCESS, harp_operation_set_value_unit(op_unit.get(), "km"));
  ASSERT_EQ(HARP_SUCCESS, harp_operation_set_value_unit(op_unit.get(), "m"));
  auto* filter =
      reinterpret_cast<harp_operation_numeric_value_filter*>(op_unit.get());
  double meters_yes = 2500.0;
  double meters_no = 1500.0;
  EXPECT_EQ(1, filter->eval(filter, harp_type_double, &meters_yes));
  EXPECT_EQ(0, filter->eval(filter, harp_type_double, &meters_no));

  // Second set_value_unit deletes the prior unit_converter.
  ASSERT_EQ(HARP_SUCCESS, harp_operation_set_value_unit(op_unit.get(), "cm"));
  double cm_yes = 250000.0;
  EXPECT_EQ(1, filter->eval(filter, harp_type_double, &cm_yes));

  EXPECT_EQ(-1, harp_operation_set_value_unit(op_unit.get(), "s"));
  EXPECT_EQ(HARP_ERROR_UNIT_CONVERSION, harp_errno);

  // Restore a non-null unit_converter so comparison_filter_delete frees it.
  ASSERT_EQ(HARP_SUCCESS, harp_operation_set_value_unit(op_unit.get(), "m"));
}

TEST_F(HarpOperationTest, LongitudeRangeFilterNewUnitAndEval) {
  harp_operation* raw_op = nullptr;
  EXPECT_EQ(-1, harp_operation_longitude_range_filter_new(
                    0.0, "m", 90.0, "degree_east", &raw_op));
  EXPECT_EQ(-1, harp_operation_longitude_range_filter_new(0.0, "degree_east",
                                                          90.0, "m", &raw_op));

  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_longitude_range_filter_new(
                0.0, "rad", std::numbers::pi / 2.0, "rad", &raw_op));
  ScopedHarpOperation op(raw_op);
  auto* filter =
      reinterpret_cast<harp_operation_numeric_value_filter*>(op.get());

  // Evaluate across all numeric types + invalid type without unit converter.
  int8_t v8 = 45;
  int16_t v16 = 45;
  int32_t v32 = -315;  // wraps to +45 deg
  float vf = 45.0f;
  double vd_in = 405.0;  // wraps to +45 deg
  double vd_out = 120.0;
  EXPECT_EQ(1, filter->eval(filter, harp_type_int8, &v8));
  EXPECT_EQ(1, filter->eval(filter, harp_type_int16, &v16));
  EXPECT_EQ(1, filter->eval(filter, harp_type_int32, &v32));
  EXPECT_EQ(1, filter->eval(filter, harp_type_float, &vf));
  EXPECT_EQ(1, filter->eval(filter, harp_type_double, &vd_in));
  EXPECT_EQ(0, filter->eval(filter, harp_type_double, &vd_out));

  char s[] = "45";
  char* sp = s;
  EXPECT_EQ(-1, filter->eval(filter, harp_type_string, &sp));

  // Now attach a unit converter from radians to degree_east.
  ASSERT_EQ(HARP_SUCCESS, harp_operation_set_value_unit(op.get(), "rad"));
  double rad_in = std::numbers::pi / 4.0;
  double rad_out = std::numbers::pi;
  EXPECT_EQ(1, filter->eval(filter, harp_type_double, &rad_in));
  EXPECT_EQ(0, filter->eval(filter, harp_type_double, &rad_out));

  // Also cover nullptr units in constructor.
  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_longitude_range_filter_new(
                              -10.0, nullptr, 10.0, nullptr, &raw_op));
  ScopedHarpOperation op_null_units(raw_op);
}

TEST_F(HarpOperationTest, MembershipFilterNewUnitAndEval) {
  harp_operation* raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_membership_filter_new(
                              "x", operator_in, 0, nullptr, nullptr, &raw_op));
  ScopedHarpOperation op_empty(raw_op);
  auto* filter_empty =
      reinterpret_cast<harp_operation_numeric_value_filter*>(op_empty.get());
  int32_t v_zero = 1;
  EXPECT_EQ(0, filter_empty->eval(filter_empty, harp_type_int32, &v_zero));

  constexpr double kVals[] = {1.0, 2.0, 3.0};
  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_membership_filter_new("altitude", operator_in, 3,
                                                 kVals, "km", &raw_op));
  ScopedHarpOperation op_in(raw_op);
  ASSERT_EQ(HARP_SUCCESS, harp_operation_set_value_unit(op_in.get(), "m"));
  auto* filter_in =
      reinterpret_cast<harp_operation_numeric_value_filter*>(op_in.get());

  double m_yes = 2000.0;
  double m_no = 4000.0;
  EXPECT_EQ(1, filter_in->eval(filter_in, harp_type_double, &m_yes));
  EXPECT_EQ(0, filter_in->eval(filter_in, harp_type_double, &m_no));

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_membership_filter_new("count", operator_not_in, 3,
                                                 kVals, nullptr, &raw_op));
  ScopedHarpOperation op_not_in(raw_op);
  auto* filter_not_in =
      reinterpret_cast<harp_operation_numeric_value_filter*>(op_not_in.get());
  int8_t v8 = 2;
  int16_t v16 = 5;
  int32_t v32 = 3;
  float vf = 1.0f;
  double vd = 9.0;
  EXPECT_EQ(0, filter_not_in->eval(filter_not_in, harp_type_int8, &v8));
  EXPECT_EQ(1, filter_not_in->eval(filter_not_in, harp_type_int16, &v16));
  EXPECT_EQ(0, filter_not_in->eval(filter_not_in, harp_type_int32, &v32));
  EXPECT_EQ(0, filter_not_in->eval(filter_not_in, harp_type_float, &vf));
  EXPECT_EQ(1, filter_not_in->eval(filter_not_in, harp_type_double, &vd));

  char s[] = "1";
  char* sp = s;
  EXPECT_EQ(-1, filter_not_in->eval(filter_not_in, harp_type_string, &sp));
}

TEST_F(HarpOperationTest, IndexFiltersNewAndEval) {
  harp_operation* raw_op = nullptr;
  EXPECT_EQ(-1, harp_operation_index_comparison_filter_new(
                    harp_dimension_independent, operator_eq, 0, &raw_op));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  struct IdxCmpCase {
    harp_comparison_operator_type op_type;
    int32_t rhs;
    int32_t lhs;
    int expected;
  };
  constexpr IdxCmpCase kCmpCases[] = {
      {.op_type = operator_eq, .rhs = 5, .lhs = 5, .expected = 1},
      {.op_type = operator_ne, .rhs = 5, .lhs = 4, .expected = 1},
      {.op_type = operator_lt, .rhs = 5, .lhs = 4, .expected = 1},
      {.op_type = operator_le, .rhs = 5, .lhs = 5, .expected = 1},
      {.op_type = operator_gt, .rhs = 5, .lhs = 6, .expected = 1},
      {.op_type = operator_ge, .rhs = 5, .lhs = 5, .expected = 1},
      {.op_type = operator_gt, .rhs = 5, .lhs = 5, .expected = 0},
  };
  for (const auto& tc : kCmpCases) {
    raw_op = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_operation_index_comparison_filter_new(
                  harp_dimension_time, tc.op_type, tc.rhs, &raw_op));
    ScopedHarpOperation op(raw_op);
    auto* filter = reinterpret_cast<harp_operation_index_filter*>(op.get());
    EXPECT_EQ(tc.expected, filter->eval(filter, tc.lhs));
  }

  raw_op = nullptr;
  EXPECT_EQ(-1,
            harp_operation_index_membership_filter_new(
                harp_dimension_independent, operator_in, 0, nullptr, &raw_op));

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_index_membership_filter_new(
                harp_dimension_time, operator_in, 0, nullptr, &raw_op));
  ScopedHarpOperation op_idx_empty(raw_op);

  constexpr int32_t kIndices[] = {2, 4, 6};
  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_index_membership_filter_new(
                harp_dimension_time, operator_in, 3, kIndices, &raw_op));
  ScopedHarpOperation op_idx_in(raw_op);
  auto* idx_in =
      reinterpret_cast<harp_operation_index_filter*>(op_idx_in.get());
  EXPECT_EQ(1, idx_in->eval(idx_in, 4));
  EXPECT_EQ(0, idx_in->eval(idx_in, 5));

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_index_membership_filter_new(
                harp_dimension_time, operator_not_in, 3, kIndices, &raw_op));
  ScopedHarpOperation op_idx_not_in(raw_op);
  auto* idx_not_in =
      reinterpret_cast<harp_operation_index_filter*>(op_idx_not_in.get());
  EXPECT_EQ(0, idx_not_in->eval(idx_not_in, 4));
  EXPECT_EQ(1, idx_not_in->eval(idx_not_in, 5));
}

TEST_F(HarpOperationTest, StringFiltersNewAndEval) {
  char enum_a[] = "alpha";
  char enum_b[] = "beta";
  char* enum_names[] = {enum_a, enum_b};

  struct StrCmpCase {
    harp_comparison_operator_type op_type;
    const char* rhs;
    const char* lhs;
    int expected;
  };
  constexpr StrCmpCase kCases[] = {
      {.op_type = operator_eq, .rhs = "beta", .lhs = "beta", .expected = 1},
      {.op_type = operator_ne, .rhs = "beta", .lhs = "alpha", .expected = 1},
      {.op_type = operator_lt, .rhs = "beta", .lhs = "alpha", .expected = 0},
      {.op_type = operator_le, .rhs = "alpha", .lhs = "beta", .expected = 1},
      {.op_type = operator_gt, .rhs = "beta", .lhs = "alpha", .expected = 1},
      {.op_type = operator_ge, .rhs = "alpha", .lhs = "alpha", .expected = 1},
  };

  for (const auto& tc : kCases) {
    harp_operation* raw_op = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_operation_string_comparison_filter_new(
                                "mode", tc.op_type, tc.rhs, &raw_op));
    ScopedHarpOperation op(raw_op);
    auto* filter =
        reinterpret_cast<harp_operation_string_value_filter*>(op.get());

    std::string lhs_buf(tc.lhs);
    char* lhs_ptr = lhs_buf.data();
    EXPECT_EQ(tc.expected,
              filter->eval(filter, 0, nullptr, harp_type_string, &lhs_ptr));
  }

  // Enum-backed int8/int16/int32 and non-string error path.
  harp_operation* raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_string_comparison_filter_new(
                              "mode", operator_eq, "beta", &raw_op));
  ScopedHarpOperation op_cmp(raw_op);
  auto* cmp_filter =
      reinterpret_cast<harp_operation_string_value_filter*>(op_cmp.get());

  int32_t non_str = 0;
  EXPECT_EQ(
      -1, cmp_filter->eval(cmp_filter, 0, nullptr, harp_type_int32, &non_str));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  int8_t e8 = 1;
  int16_t e16 = 0;
  int32_t e32_oob = 99;
  EXPECT_EQ(1,
            cmp_filter->eval(cmp_filter, 2, enum_names, harp_type_int8, &e8));
  EXPECT_EQ(0,
            cmp_filter->eval(cmp_filter, 2, enum_names, harp_type_int16, &e16));
  EXPECT_EQ(0, cmp_filter->eval(cmp_filter, 2, enum_names, harp_type_int32,
                                &e32_oob));

  // String membership filter: empty, null element cleanup, enum & string eval.
  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_string_membership_filter_new(
                              "mode", operator_in, 0, nullptr, &raw_op));
  ScopedHarpOperation op_mem_empty(raw_op);

  const char* vals_with_null[] = {"alpha", nullptr};
  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_string_membership_filter_new(
                              "mode", operator_in, 2, vals_with_null, &raw_op));
  ScopedHarpOperation op_mem_null_elem(raw_op);

  const char* vals[] = {"alpha", "gamma"};
  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_string_membership_filter_new(
                              "mode", operator_in, 2, vals, &raw_op));
  ScopedHarpOperation op_mem_in(raw_op);
  auto* mem_in =
      reinterpret_cast<harp_operation_string_value_filter*>(op_mem_in.get());

  char s_alpha[] = "alpha";
  char s_beta[] = "beta";
  char* p_alpha = s_alpha;
  char* p_beta = s_beta;
  EXPECT_EQ(1, mem_in->eval(mem_in, 0, nullptr, harp_type_string, &p_alpha));
  EXPECT_EQ(0, mem_in->eval(mem_in, 0, nullptr, harp_type_string, &p_beta));
  EXPECT_EQ(-1, mem_in->eval(mem_in, 0, nullptr, harp_type_int32, &non_str));

  int8_t m8 = 0;
  int16_t m16 = 1;
  int32_t m32_oob = -1;
  EXPECT_EQ(1, mem_in->eval(mem_in, 2, enum_names, harp_type_int8, &m8));
  EXPECT_EQ(0, mem_in->eval(mem_in, 2, enum_names, harp_type_int16, &m16));
  EXPECT_EQ(0, mem_in->eval(mem_in, 2, enum_names, harp_type_int32, &m32_oob));

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_string_membership_filter_new(
                              "mode", operator_not_in, 2, vals, &raw_op));
  ScopedHarpOperation op_mem_not_in(raw_op);
  auto* mem_not_in = reinterpret_cast<harp_operation_string_value_filter*>(
      op_mem_not_in.get());
  EXPECT_EQ(
      0, mem_not_in->eval(mem_not_in, 0, nullptr, harp_type_string, &p_alpha));
  EXPECT_EQ(
      1, mem_not_in->eval(mem_not_in, 0, nullptr, harp_type_string, &p_beta));
}

TEST_F(HarpOperationTest, ValidRangeFilterNewSetAndEval) {
  // Non-valid-range operation returns 0 from set_valid_range.
  harp_operation* raw_bin = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_bin_full_new(&raw_bin));
  ScopedHarpOperation op_bin(raw_bin);
  harp_scalar dummy_min = {};
  harp_scalar dummy_max = {};
  EXPECT_EQ(HARP_SUCCESS,
            harp_operation_set_valid_range(op_bin.get(), harp_type_double,
                                           dummy_min, dummy_max));

  for (int invalid : {0, 1}) {
    harp_operation* raw_op = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_operation_valid_range_filter_new("val", invalid, &raw_op));
    ScopedHarpOperation op(raw_op);
    auto* filter =
        reinterpret_cast<harp_operation_numeric_value_filter*>(op.get());

    // Exercise set_valid_range for all 5 numeric types.
    harp_scalar min_s = {};
    harp_scalar max_s = {};
    min_s.int8_data = 0;
    max_s.int8_data = 10;
    ASSERT_EQ(HARP_SUCCESS, harp_operation_set_valid_range(
                                op.get(), harp_type_int8, min_s, max_s));
    min_s.int16_data = 0;
    max_s.int16_data = 10;
    ASSERT_EQ(HARP_SUCCESS, harp_operation_set_valid_range(
                                op.get(), harp_type_int16, min_s, max_s));
    min_s.int32_data = 0;
    max_s.int32_data = 10;
    ASSERT_EQ(HARP_SUCCESS, harp_operation_set_valid_range(
                                op.get(), harp_type_int32, min_s, max_s));
    min_s.float_data = 0.0f;
    max_s.float_data = 10.0f;
    ASSERT_EQ(HARP_SUCCESS, harp_operation_set_valid_range(
                                op.get(), harp_type_float, min_s, max_s));
    min_s.double_data = 0.0;
    max_s.double_data = 10.0;
    ASSERT_EQ(HARP_SUCCESS, harp_operation_set_valid_range(
                                op.get(), harp_type_double, min_s, max_s));

    int8_t v8 = 5;
    int16_t v16 = 5;
    int32_t v32 = 5;
    float vf = 5.0f;
    double vd_in = 5.0;
    double vd_out = 15.0;
    double vd_nan = std::numeric_limits<double>::quiet_NaN();

    int expected_in = invalid ? 0 : 1;
    int expected_out = invalid ? 1 : 0;
    EXPECT_EQ(expected_in, filter->eval(filter, harp_type_int8, &v8));
    EXPECT_EQ(expected_in, filter->eval(filter, harp_type_int16, &v16));
    EXPECT_EQ(expected_in, filter->eval(filter, harp_type_int32, &v32));
    EXPECT_EQ(expected_in, filter->eval(filter, harp_type_float, &vf));
    EXPECT_EQ(expected_in, filter->eval(filter, harp_type_double, &vd_in));
    EXPECT_EQ(expected_out, filter->eval(filter, harp_type_double, &vd_out));
    EXPECT_EQ(expected_out, filter->eval(filter, harp_type_double, &vd_nan));

    char s[] = "5";
    char* sp = s;
    EXPECT_EQ(-1, filter->eval(filter, harp_type_string, &sp));
  }
}

TEST_F(HarpOperationTest, RebinAndRegridConstructors) {
  double bounds2[] = {0.0, 10.0};
  double bounds3[] = {0.0, 5.0, 10.0};
  double bounds4[] = {0.0, 5.0, 5.0, 10.0};
  double values2[] = {2.5, 7.5};

  // 1. rebin errors & success.
  harp_operation* raw_op = nullptr;
  EXPECT_EQ(-1, harp_operation_rebin_new(harp_dimension_vertical, "alt_bounds",
                                         "km", 1, bounds2, &raw_op));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(-1, harp_operation_rebin_new(harp_dimension_vertical, "bad-name",
                                         "km", 2, bounds2, &raw_op));
  EXPECT_EQ(-1, harp_operation_rebin_new(harp_dimension_vertical, "alt_bounds",
                                         "bad_unit", 2, bounds2, &raw_op));
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_rebin_new(harp_dimension_vertical, "alt_bounds",
                                     "km", 3, bounds3, &raw_op));
  ScopedHarpOperation op_rebin(raw_op);
  auto* rebin = reinterpret_cast<harp_operation_rebin*>(op_rebin.get());
  EXPECT_EQ(2, rebin->axis_bounds_variable->dimension[0]);
  EXPECT_DOUBLE_EQ(0.0, rebin->axis_bounds_variable->data.double_data[0]);
  EXPECT_DOUBLE_EQ(5.0, rebin->axis_bounds_variable->data.double_data[1]);

  // 2. regrid errors & success (no bounds, n+1 bounds, 2n bounds).
  raw_op = nullptr;
  EXPECT_EQ(-1, harp_operation_regrid_new(harp_dimension_vertical, "alt", "km",
                                          2, values2, 5, bounds4, &raw_op));
  EXPECT_EQ(-1,
            harp_operation_regrid_new(harp_dimension_vertical, "bad-name", "km",
                                      2, values2, 0, nullptr, &raw_op));
  EXPECT_EQ(
      -1, harp_operation_regrid_new(harp_dimension_vertical, "alt", "bad_unit",
                                    2, values2, 0, nullptr, &raw_op));

  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_regrid_new(harp_dimension_vertical, "alt", "km", 2,
                                      values2, 0, nullptr, &raw_op));
  ScopedHarpOperation op_regrid_nobounds(raw_op);

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_regrid_new(harp_dimension_vertical, "alt", "km", 2,
                                      values2, 3, bounds3, &raw_op));
  ScopedHarpOperation op_regrid_n1(raw_op);
  auto* regrid_n1 =
      reinterpret_cast<harp_operation_regrid*>(op_regrid_n1.get());
  EXPECT_DOUBLE_EQ(5.0, regrid_n1->axis_bounds_variable->data.double_data[2]);

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_regrid_new(harp_dimension_vertical, "alt", "km", 2,
                                      values2, 4, bounds4, &raw_op));
  ScopedHarpOperation op_regrid_2n(raw_op);
}

TEST_F(HarpOperationTest, AllRemainingConstructorsAndDeletes) {
  const char* vars[] = {"O3_column_number_density", "latitude"};
  constexpr harp_dimension_type kDims[] = {harp_dimension_time,
                                           harp_dimension_vertical};
  constexpr harp_dimension_type kTooManyDims[HARP_MAX_NUM_DIMS + 1] = {};
  harp_data_type dt = harp_type_float;

  harp_operation* raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_bin_collocated_new("coll.csv", 'a', &raw_op));
  ScopedHarpOperation op_bin_coll(raw_op);

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_bin_full_new(&raw_op));
  ScopedHarpOperation op_bin_full(raw_op);

  // Include values within EPSILON of -90, 90, and lon[0] + 360 to test
  // snapping in harp_operation_bin_spatial_new.
  double lat_edges[] = {-90.0 + 1e-12, 0.0, 90.0 - 1e-12};
  double lon_edges[] = {-180.0, 0.0, 180.0 - 1e-12};
  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_bin_spatial_new(3, lat_edges, 3,
                                                         lon_edges, &raw_op));
  ScopedHarpOperation op_bin_spatial(raw_op);
  auto* bin_sp =
      reinterpret_cast<harp_operation_bin_spatial*>(op_bin_spatial.get());
  EXPECT_EQ(-90.0, bin_sp->latitude_edges[0]);
  EXPECT_EQ(90.0, bin_sp->latitude_edges[2]);
  EXPECT_EQ(180.0, bin_sp->longitude_edges[2]);

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_bin_with_variables_new(0, vars, &raw_op));
  ScopedHarpOperation op_bin_vars0(raw_op);
  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_bin_with_variables_new(2, vars, &raw_op));
  ScopedHarpOperation op_bin_vars2(raw_op);

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_clamp_new(harp_dimension_vertical, "pressure", "hPa",
                                     10.0, 1000.0, &raw_op));
  ScopedHarpOperation op_clamp(raw_op);

  // derive_variable_new: too many dims, NULL data_type/unit, non-NULL.
  raw_op = nullptr;
  EXPECT_EQ(-1, harp_operation_derive_variable_new(
                    "x", nullptr, HARP_MAX_NUM_DIMS + 1, kTooManyDims, nullptr,
                    &raw_op));
  ASSERT_EQ(HARP_SUCCESS, harp_operation_derive_variable_new(
                              "x", nullptr, -1, nullptr, nullptr, &raw_op));
  ScopedHarpOperation op_derive1(raw_op);
  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_derive_variable_new("x", &dt, 2, kDims,
                                                             "DU", &raw_op));
  ScopedHarpOperation op_derive2(raw_op);

  // derive_smoothed_column_collocated_dataset_new.
  raw_op = nullptr;
  EXPECT_EQ(-1, harp_operation_derive_smoothed_column_collocated_dataset_new(
                    "x", HARP_MAX_NUM_DIMS + 1, kTooManyDims, nullptr, "alt",
                    "km", "coll.csv", 'b', "/dir", &raw_op));
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_derive_smoothed_column_collocated_dataset_new(
                "x", 2, kDims, nullptr, "alt", "km", "coll.csv", 'b', "/dir",
                &raw_op));
  ScopedHarpOperation op_dscd1(raw_op);
  raw_op = nullptr;
  ASSERT_EQ(
      HARP_SUCCESS,
      harp_operation_derive_smoothed_column_collocated_dataset_new(
          "x", 2, kDims, "DU", "alt", "km", "coll.csv", 'b', "/dir", &raw_op));
  ScopedHarpOperation op_dscd2(raw_op);

  // derive_smoothed_column_collocated_product_new.
  raw_op = nullptr;
  EXPECT_EQ(-1, harp_operation_derive_smoothed_column_collocated_product_new(
                    "x", HARP_MAX_NUM_DIMS + 1, kTooManyDims, nullptr, "alt",
                    "km", "ref.nc", &raw_op));
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_derive_smoothed_column_collocated_product_new(
                "x", 2, kDims, nullptr, "alt", "km", "ref.nc", &raw_op));
  ScopedHarpOperation op_dscp1(raw_op);
  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_derive_smoothed_column_collocated_product_new(
                "x", 2, kDims, "DU", "alt", "km", "ref.nc", &raw_op));
  ScopedHarpOperation op_dscp2(raw_op);

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_exclude_variable_new(0, vars, &raw_op));
  ScopedHarpOperation op_excl0(raw_op);
  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_exclude_variable_new(2, vars, &raw_op));
  ScopedHarpOperation op_excl2(raw_op);

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_flatten_new(harp_dimension_time, &raw_op));
  ScopedHarpOperation op_flatten(raw_op);

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_keep_variable_new(0, vars, &raw_op));
  ScopedHarpOperation op_keep0(raw_op);
  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_keep_variable_new(2, vars, &raw_op));
  ScopedHarpOperation op_keep2(raw_op);

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_regrid_collocated_dataset_new(
                              harp_dimension_vertical, "alt", "km", "coll.csv",
                              'b', "/dir", &raw_op));
  ScopedHarpOperation op_rcd(raw_op);

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_regrid_collocated_product_new(
                harp_dimension_vertical, "alt", "km", "ref.nc", &raw_op));
  ScopedHarpOperation op_rcp(raw_op);

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_rename_new("old", "new", &raw_op));
  ScopedHarpOperation op_rename(raw_op);

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_set_new("afgl86", "enabled", &raw_op));
  ScopedHarpOperation op_set(raw_op);

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_smooth_collocated_dataset_new(
                              0, vars, harp_dimension_vertical, "alt", "km",
                              "coll.csv", 'b', "/dir", &raw_op));
  ScopedHarpOperation op_scd0(raw_op);
  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_smooth_collocated_dataset_new(
                              2, vars, harp_dimension_vertical, "alt", "km",
                              "coll.csv", 'b', "/dir", &raw_op));
  ScopedHarpOperation op_scd2(raw_op);

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_smooth_collocated_product_new(
                              0, vars, harp_dimension_vertical, "alt", "km",
                              "ref.nc", &raw_op));
  ScopedHarpOperation op_scp0(raw_op);
  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_smooth_collocated_product_new(
                              2, vars, harp_dimension_vertical, "alt", "km",
                              "ref.nc", &raw_op));
  ScopedHarpOperation op_scp2(raw_op);

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_sort_new(0, vars, &raw_op));
  ScopedHarpOperation op_sort0(raw_op);
  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_sort_new(2, vars, &raw_op));
  ScopedHarpOperation op_sort2(raw_op);

  // Note: upstream harp_operation_squash_new leaves operation->variable_name
  // uninitialized when num_variables <= 0; only pass num_variables > 0.
  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_squash_new(harp_dimension_time, 2, vars, &raw_op));
  ScopedHarpOperation op_squash(raw_op);

  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_wrap_new("longitude", nullptr, -180.0,
                                                  180.0, &raw_op));
  ScopedHarpOperation op_wrap1(raw_op);
  raw_op = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_wrap_new("longitude", "degree_east",
                                                  -180.0, 180.0, &raw_op));
  ScopedHarpOperation op_wrap2(raw_op);
}

TEST_F(HarpOperationTest, PredicatesAndMetadataQueries) {
  double bounds[] = {0.0, 10.0};
  double vals[] = {1.0};
  const char* svals[] = {"a"};

  harp_operation* raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_bit_mask_filter_new(
                              "qa", operator_bit_mask_any, 1, &raw));
  ScopedHarpOperation op_bitmask(raw);

  raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_collocation_filter_new(
                              "c.csv", harp_collocation_left, -1, -1, &raw));
  ScopedHarpOperation op_collocation(raw);

  raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_comparison_filter_new(
                              "alt", operator_eq, 1.0, nullptr, &raw));
  ScopedHarpOperation op_cmp(raw);

  raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_longitude_range_filter_new(
                              0.0, nullptr, 10.0, nullptr, &raw));
  ScopedHarpOperation op_lon(raw);

  raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_membership_filter_new(
                              "mem", operator_in, 1, vals, nullptr, &raw));
  ScopedHarpOperation op_mem(raw);

  raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_string_comparison_filter_new(
                              "scmp", operator_eq, "a", &raw));
  ScopedHarpOperation op_scmp(raw);

  raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_string_membership_filter_new(
                              "smem", operator_in, 1, svals, &raw));
  ScopedHarpOperation op_smem(raw);

  raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_valid_range_filter_new("vr", 0, &raw));
  ScopedHarpOperation op_vr(raw);

  raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_point_distance_filter_new(0.0, nullptr, 0.0, nullptr,
                                                     100.0, nullptr, &raw));
  ScopedHarpOperation op_pt_dist(raw);

  raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_point_in_area_filter_new(nullptr, 2, bounds, nullptr,
                                                    2, bounds, nullptr, &raw));
  ScopedHarpOperation op_pt_area(raw);

  raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_area_covers_area_filter_new(
                nullptr, 2, bounds, nullptr, 2, bounds, nullptr, &raw));
  ScopedHarpOperation op_poly_ca(raw);

  raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_area_covers_point_filter_new(
                              5.0, nullptr, 5.0, nullptr, &raw));
  ScopedHarpOperation op_poly_cp(raw);

  raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_operation_area_inside_area_filter_new(
                nullptr, 2, bounds, nullptr, 2, bounds, nullptr, &raw));
  ScopedHarpOperation op_poly_ia(raw);

  raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_area_intersects_area_filter_new(
                              nullptr, 2, bounds, nullptr, 2, bounds, nullptr,
                              nullptr, &raw));
  ScopedHarpOperation op_poly_xa(raw);

  raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_operation_bin_full_new(&raw));
  ScopedHarpOperation op_other(raw);

  struct VarNameCase {
    const harp_operation* op;
    std::string_view expected_name;
  };
  const VarNameCase kVarNameCases[] = {
      {.op = op_bitmask.get(), .expected_name = "qa"},
      {.op = op_collocation.get(), .expected_name = "index"},
      {.op = op_cmp.get(), .expected_name = "alt"},
      {.op = op_lon.get(), .expected_name = "longitude"},
      {.op = op_mem.get(), .expected_name = "mem"},
      {.op = op_scmp.get(), .expected_name = "scmp"},
      {.op = op_smem.get(), .expected_name = "smem"},
      {.op = op_vr.get(), .expected_name = "vr"},
  };
  for (const auto& tc : kVarNameCases) {
    const char* name = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_operation_get_variable_name(tc.op, &name));
    EXPECT_EQ(tc.expected_name, name);
  }
  const char* missing_name = nullptr;
  EXPECT_EQ(-1,
            harp_operation_get_variable_name(op_other.get(), &missing_name));
  EXPECT_EQ(HARP_ERROR_OPERATION, harp_errno);

  EXPECT_EQ(1, harp_operation_is_point_filter(op_pt_dist.get()));
  EXPECT_EQ(1, harp_operation_is_point_filter(op_pt_area.get()));
  EXPECT_EQ(0, harp_operation_is_point_filter(op_other.get()));

  EXPECT_EQ(1, harp_operation_is_polygon_filter(op_poly_ca.get()));
  EXPECT_EQ(1, harp_operation_is_polygon_filter(op_poly_cp.get()));
  EXPECT_EQ(1, harp_operation_is_polygon_filter(op_poly_ia.get()));
  EXPECT_EQ(1, harp_operation_is_polygon_filter(op_poly_xa.get()));
  EXPECT_EQ(0, harp_operation_is_polygon_filter(op_other.get()));

  EXPECT_EQ(1, harp_operation_is_string_value_filter(op_scmp.get()));
  EXPECT_EQ(1, harp_operation_is_string_value_filter(op_smem.get()));
  EXPECT_EQ(0, harp_operation_is_string_value_filter(op_cmp.get()));

  EXPECT_EQ(1, harp_operation_is_value_filter(op_bitmask.get()));
  EXPECT_EQ(1, harp_operation_is_value_filter(op_cmp.get()));
  EXPECT_EQ(1, harp_operation_is_value_filter(op_lon.get()));
  EXPECT_EQ(1, harp_operation_is_value_filter(op_mem.get()));
  EXPECT_EQ(1, harp_operation_is_value_filter(op_scmp.get()));
  EXPECT_EQ(1, harp_operation_is_value_filter(op_smem.get()));
  EXPECT_EQ(1, harp_operation_is_value_filter(op_vr.get()));
  EXPECT_EQ(0, harp_operation_is_value_filter(op_other.get()));

  EXPECT_EQ(HARP_SUCCESS, harp_operation_set_value_unit(op_other.get(), "m"));
}

TEST_F(HarpOperationTest, OutOfMemoryInConstructors) {
  TempFile valid_mask("harp_op_oom_area.csv");
  ASSERT_TRUE(WriteFileText(valid_mask.path(),
                            "polygon\n"
                            "0.0,0.0, 0.0,10.0, 10.0,10.0, 10.0,0.0\n"));

  const absl::Cleanup clear_err = [] { harp_set_error(HARP_SUCCESS, nullptr); };

  double bounds[2] = {0.0, 10.0};
  double lat_edges[3] = {-90.0, 0.0, 90.0};
  double lon_edges[3] = {-180.0, 0.0, 180.0};
  double min_frac = 0.5;
  const char* var_names[2] = {"O3", "temperature"};
  const harp_data_type dt_double = harp_type_double;
  const harp_dimension_type dim_time = harp_dimension_time;
  const harp_dimension_type dims_tv[2] = {harp_dimension_time,
                                          harp_dimension_vertical};
  const int32_t idx_vals[2] = {0, 1};
  const double d_vals[2] = {1.0, 2.0};

  auto sweep_op = [](auto fn) {
    return SweepOomUntilSuccess([&]() {
      harp_operation* op = nullptr;
      if (fn(&op) == HARP_SUCCESS) {
        ScopedSuppressOom suppress;
        harp_operation_delete(op);
      }
    });
  };

  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_area_covers_area_filter_new(
                  valid_mask.c_str(), 0, nullptr, nullptr, 0, nullptr, nullptr,
                  op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_area_covers_area_filter_new(
                  nullptr, 2, bounds, "degree_north", 2, bounds, "degree_east",
                  op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_area_covers_point_filter_new(
                  5.0, "degree_north", 5.0, "degree_east", op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_area_inside_area_filter_new(
                  nullptr, 2, bounds, "degree_north", 2, bounds, "degree_east",
                  op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_area_intersects_area_filter_new(
                  nullptr, 2, bounds, "degree_north", 2, bounds, "degree_east",
                  &min_frac, op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_bin_collocated_new("colloc.csv", 'a', op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_bin_full_new(op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_bin_spatial_new(3, lat_edges, 3, lon_edges,
                                                    op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_bin_with_variables_new(1, var_names, op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_bit_mask_filter_new(
                  "qa", operator_bit_mask_any, 3, op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_clamp_new(harp_dimension_vertical,
                                              "altitude", "m", 0.0, 1000.0, op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_collocation_filter_new(
                  "colloc.csv", harp_collocation_left, 0, 10, op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_comparison_filter_new("alt", operator_gt,
                                                          10.0, "m", op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_derive_variable_new("O3", &dt_double, 1,
                                                        &dim_time, "DU", op);
            }),
            0);
  EXPECT_GT(
      sweep_op([&](harp_operation** op) {
        return harp_operation_derive_smoothed_column_collocated_dataset_new(
            "O3", 2, dims_tv, "DU", "altitude", "m", "colloc.csv", 'a', "./dir",
            op);
      }),
      0);
  EXPECT_GT(
      sweep_op([&](harp_operation** op) {
        return harp_operation_derive_smoothed_column_collocated_product_new(
            "O3", 2, dims_tv, "DU", "altitude", "m", "prod.nc", op);
      }),
      0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_exclude_variable_new(1, var_names, op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_flatten_new(harp_dimension_vertical, op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_index_comparison_filter_new(
                  harp_dimension_time, operator_eq, 0, op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_index_membership_filter_new(
                  harp_dimension_time, operator_in, 1, idx_vals, op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_keep_variable_new(1, var_names, op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_longitude_range_filter_new(
                  -10.0, "degree_east", 10.0, "degree_east", op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_membership_filter_new("alt", operator_in, 2,
                                                          d_vals, "m", op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_point_distance_filter_new(
                  0.0, "degree_north", 0.0, "degree_east", 100.0, "km", op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_point_in_area_filter_new(
                  nullptr, 2, bounds, "degree_north", 2, bounds, "degree_east",
                  op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_rebin_new(harp_dimension_vertical,
                                              "altitude_bounds", "m", 3,
                                              lat_edges, op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_regrid_new(harp_dimension_vertical, "alt",
                                               "m", 2, bounds, 3, lat_edges,
                                               op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_regrid_collocated_dataset_new(
                  harp_dimension_vertical, "altitude", "m", "colloc.csv", 'a',
                  "./dir", op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_regrid_collocated_product_new(
                  harp_dimension_vertical, "altitude", "m", "prod.nc", op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_rename_new("O3", "ozone", op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_set_new("source_product", "val", op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_smooth_collocated_dataset_new(
                  1, var_names, harp_dimension_vertical, "altitude", "m",
                  "colloc.csv", 'a', "./dir", op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_smooth_collocated_product_new(
                  1, var_names, harp_dimension_vertical, "altitude", "m",
                  "prod.nc", op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_sort_new(1, var_names, op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_squash_new(harp_dimension_time, 1,
                                               var_names, op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_string_comparison_filter_new(
                  "flag", operator_eq, "ok", op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_string_membership_filter_new(
                  "flag", operator_in, 1, var_names, op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_valid_range_filter_new("O3", 0, op);
            }),
            0);
  EXPECT_GT(sweep_op([&](harp_operation** op) {
              return harp_operation_wrap_new("longitude", "degree_east", -180.0,
                                             180.0, op);
            }),
            0);
}

}  // namespace
}  // namespace stcorp_harp
