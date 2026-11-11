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

// Unit tests for //third_party/netcdf/libdispatch/dvarget.c.
//
// Exercises all public nc_get_{var,var1,vara,vars,varm}* entry points alongside
// NC_get_vara, NCDEFAULT_get_vars, and NCDEFAULT_get_varm across CDF-5 and
// NetCDF-4/HDF5 in-memory datasets, user-defined types, strided odometer carry,
// index mapping permutations, null-parameter fallbacks, and error paths.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dvarget.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

extern "C" {
#include "third_party/netcdf/include/ncdispatch.h"
}

namespace netcdf {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

constexpr size_t kRows = 2;
constexpr size_t kCols = 3;
constexpr size_t kTotal2D = kRows * kCols;
constexpr size_t kVarmPadding = 8;

struct PairRecord {
  std::int64_t id;
  double weight;
};

static_assert(sizeof(PairRecord) == sizeof(std::int64_t) + sizeof(double),
              "PairRecord must have no internal or tail padding for MSan");

template <typename T, typename GetVarFn, typename GetVar1Fn, typename GetVaraFn,
          typename GetVarsFn, typename GetVarmFn>
void VerifyNumericGetFamilies(int ncid, int varid,
                              const std::array<T, kTotal2D>& expected,
                              T sentinel, GetVarFn get_var_fn,
                              GetVar1Fn get_var1_fn, GetVaraFn get_vara_fn,
                              GetVarsFn get_vars_fn, GetVarmFn get_varm_fn) {
  {
    std::vector<T> actual(kTotal2D, sentinel);
    ASSERT_EQ(NC_NOERR, get_var_fn(ncid, varid, actual.data()));
    EXPECT_THAT(actual, ElementsAreArray(expected));
  }

  {
    T actual = sentinel;
    const std::array<size_t, 2> index = {1, 2};
    ASSERT_EQ(NC_NOERR, get_var1_fn(ncid, varid, index.data(), &actual));
    EXPECT_EQ(expected[5], actual);
  }

  {
    std::vector<T> actual(4, sentinel);
    const std::array<size_t, 2> start = {0, 1};
    const std::array<size_t, 2> count = {2, 2};
    ASSERT_EQ(NC_NOERR, get_vara_fn(ncid, varid, start.data(), count.data(),
                                    actual.data()));
    EXPECT_THAT(actual, ElementsAre(expected[1], expected[2], expected[4],
                                    expected[5]));
  }

  {
    std::vector<T> actual(4, sentinel);
    const std::array<size_t, 2> start = {0, 0};
    const std::array<size_t, 2> count = {2, 2};
    const std::array<ptrdiff_t, 2> stride = {1, 2};
    ASSERT_EQ(NC_NOERR, get_vars_fn(ncid, varid, start.data(), count.data(),
                                    stride.data(), actual.data()));
    EXPECT_THAT(actual, ElementsAre(expected[0], expected[2], expected[3],
                                    expected[5]));
  }

  {
    std::vector<T> actual(kTotal2D + kVarmPadding, sentinel);
    const std::array<size_t, 2> start = {0, 0};
    const std::array<size_t, 2> count = {kRows, kCols};
    const std::array<ptrdiff_t, 2> stride = {1, 1};
    const std::array<ptrdiff_t, 2> imap = {1, static_cast<ptrdiff_t>(kRows)};
    ASSERT_EQ(NC_NOERR, get_varm_fn(ncid, varid, start.data(), count.data(),
                                    stride.data(), imap.data(), actual.data()));
    EXPECT_THAT(std::vector<T>(actual.data(), actual.data() + kTotal2D),
                ElementsAre(expected[0], expected[3], expected[1], expected[4],
                            expected[2], expected[5]));
  }
}

TEST(DvargetTest, TypedAtomicGetFamiliesRoundTripCdf5) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create("cdf5_typed_get.nc", NC_CDF5 | NC_DISKLESS, nc.ptr()));

  int dimids[2] = {-1, -1};
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "rows", kRows, &dimids[0]));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "cols", kCols, &dimids[1]));

  int v_text = -1;
  int v_schar = -1;
  int v_ubyte = -1;
  int v_short = -1;
  int v_int = -1;
  int v_ushort = -1;
  int v_uint = -1;
  int v_int64 = -1;
  int v_uint64 = -1;
  int v_float = -1;
  int v_double = -1;

  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_text", NC_CHAR, 2, dimids, &v_text));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_schar", NC_BYTE, 2, dimids, &v_schar));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_ubyte", NC_UBYTE, 2, dimids, &v_ubyte));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_short", NC_SHORT, 2, dimids, &v_short));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_int", NC_INT, 2, dimids, &v_int));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_ushort", NC_USHORT, 2, dimids, &v_ushort));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_uint", NC_UINT, 2, dimids, &v_uint));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_int64", NC_INT64, 2, dimids, &v_int64));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_uint64", NC_UINT64, 2, dimids, &v_uint64));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_float", NC_FLOAT, 2, dimids, &v_float));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_double", NC_DOUBLE, 2, dimids, &v_double));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));

  constexpr std::array<char, kTotal2D> kTextData = {'a', 'b', 'c',
                                                    'd', 'e', 'f'};
  constexpr std::array<signed char, kTotal2D> kScharData = {-10, 20,  -30,
                                                            40,  -50, 60};
  constexpr std::array<unsigned char, kTotal2D> kUbyteData = {10,  120, 200,
                                                              240, 5,   99};
  constexpr std::array<short, kTotal2D> kShortData = {-1000, 2000,  -3000,
                                                      4000,  -5000, 6000};
  constexpr std::array<int, kTotal2D> kIntData = {-100000, 200000,  -300000,
                                                  400000,  -500000, 600000};
  constexpr std::array<long, kTotal2D> kLongData = {
      -100000L, 200000L, -300000L, 400000L, -500000L, 600000L};
  constexpr std::array<unsigned short, kTotal2D> kUshortData = {
      100, 40000, 50000, 65000, 7, 88};
  constexpr std::array<unsigned int, kTotal2D> kUintData = {
      1U, 3000000000U, 17U, 4000000000U, 9U, 123U};
  constexpr std::array<long long, kTotal2D> kInt64Data = {
      -5000000000LL, 6000000000LL, -7LL, 8000000000LL, 9LL, -10LL};
  constexpr std::array<unsigned long long, kTotal2D> kUint64Data = {
      1ULL, 9000000000000ULL, 3ULL, 18000000000000ULL, 5ULL, 6ULL};
  constexpr std::array<float, kTotal2D> kFloatData = {1.5F,  -2.25F, 3.75F,
                                                      -4.5F, 5.125F, -6.5F};
  constexpr std::array<double, kTotal2D> kDoubleData = {1.25,  -2.5, 3.125,
                                                        -4.75, 5.5,  -6.25};

  ASSERT_EQ(NC_NOERR, nc_put_var_text(nc, v_text, kTextData.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_schar(nc, v_schar, kScharData.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_ubyte(nc, v_ubyte, kUbyteData.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_short(nc, v_short, kShortData.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, v_int, kIntData.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_ushort(nc, v_ushort, kUshortData.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_uint(nc, v_uint, kUintData.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_longlong(nc, v_int64, kInt64Data.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_ulonglong(nc, v_uint64, kUint64Data.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_float(nc, v_float, kFloatData.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_double(nc, v_double, kDoubleData.data()));

  VerifyNumericGetFamilies<char>(nc, v_text, kTextData, '?', nc_get_var_text,
                                 nc_get_var1_text, nc_get_vara_text,
                                 nc_get_vars_text, nc_get_varm_text);
  VerifyNumericGetFamilies<signed char>(
      nc, v_schar, kScharData, -99, nc_get_var_schar, nc_get_var1_schar,
      nc_get_vara_schar, nc_get_vars_schar, nc_get_varm_schar);
  VerifyNumericGetFamilies<unsigned char>(
      nc, v_ubyte, kUbyteData, 255, nc_get_var_uchar, nc_get_var1_uchar,
      nc_get_vara_uchar, nc_get_vars_uchar, nc_get_varm_uchar);
  VerifyNumericGetFamilies<unsigned char>(
      nc, v_ubyte, kUbyteData, 254, nc_get_var_ubyte, nc_get_var1_ubyte,
      nc_get_vara_ubyte, nc_get_vars_ubyte, nc_get_varm_ubyte);
  VerifyNumericGetFamilies<short>(
      nc, v_short, kShortData, -9999, nc_get_var_short, nc_get_var1_short,
      nc_get_vara_short, nc_get_vars_short, nc_get_varm_short);
  VerifyNumericGetFamilies<int>(nc, v_int, kIntData, -999999, nc_get_var_int,
                                nc_get_var1_int, nc_get_vara_int,
                                nc_get_vars_int, nc_get_varm_int);
  VerifyNumericGetFamilies<long>(
      nc, v_int, kLongData, -999999L, nc_get_var_long, nc_get_var1_long,
      nc_get_vara_long, nc_get_vars_long, nc_get_varm_long);
  VerifyNumericGetFamilies<float>(
      nc, v_float, kFloatData, -999.0F, nc_get_var_float, nc_get_var1_float,
      nc_get_vara_float, nc_get_vars_float, nc_get_varm_float);
  VerifyNumericGetFamilies<double>(
      nc, v_double, kDoubleData, -999.0, nc_get_var_double, nc_get_var1_double,
      nc_get_vara_double, nc_get_vars_double, nc_get_varm_double);
  VerifyNumericGetFamilies<unsigned short>(
      nc, v_ushort, kUshortData, 0xFFFFU, nc_get_var_ushort, nc_get_var1_ushort,
      nc_get_vara_ushort, nc_get_vars_ushort, nc_get_varm_ushort);
  VerifyNumericGetFamilies<unsigned int>(
      nc, v_uint, kUintData, 0xDEADBEEFU, nc_get_var_uint, nc_get_var1_uint,
      nc_get_vara_uint, nc_get_vars_uint, nc_get_varm_uint);
  VerifyNumericGetFamilies<long long>(
      nc, v_int64, kInt64Data, -999999LL, nc_get_var_longlong,
      nc_get_var1_longlong, nc_get_vara_longlong, nc_get_vars_longlong,
      nc_get_varm_longlong);
  VerifyNumericGetFamilies<unsigned long long>(
      nc, v_uint64, kUint64Data, 999ULL, nc_get_var_ulonglong,
      nc_get_var1_ulonglong, nc_get_vara_ulonglong, nc_get_vars_ulonglong,
      nc_get_varm_ulonglong);

  // Untyped void* entry points use the variable's native external type.
  VerifyNumericGetFamilies<int>(nc, v_int, kIntData, -777777, nc_get_var,
                                nc_get_var1, nc_get_vara, nc_get_vars,
                                nc_get_varm);
}

TEST(DvargetTest, AllStringGetFamiliesRoundTripNetcdf4) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR, nc_create("nc4_strings_get.nc", NC_NETCDF4 | NC_DISKLESS,
                                nc.ptr()));

  int dimids[2] = {-1, -1};
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "rows", kRows, &dimids[0]));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "cols", kCols, &dimids[1]));

  int scalar_varid = -1;
  int arr_varid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "scalar_str", NC_STRING, 0, nullptr, &scalar_varid));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "arr_str", NC_STRING, 2, dimids, &arr_varid));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));

  const char* scalar_in = "scalar_hello";
  std::array<const char*, kTotal2D> kStrings = {"alpha", "beta",    "gamma",
                                                "delta", "epsilon", "zeta"};
  ASSERT_EQ(NC_NOERR, nc_put_var_string(nc, scalar_varid, &scalar_in));
  ASSERT_EQ(NC_NOERR, nc_put_var_string(nc, arr_varid, kStrings.data()));

  {
    ScopedNcStrings scalar_out(1);
    ASSERT_EQ(NC_NOERR, nc_get_var_string(nc, scalar_varid, scalar_out.data()));
    ASSERT_NE(nullptr, scalar_out[0]);
    EXPECT_STREQ("scalar_hello", scalar_out[0]);
  }

  {
    ScopedNcStrings full_out(kTotal2D);
    ASSERT_EQ(NC_NOERR, nc_get_var_string(nc, arr_varid, full_out.data()));
    for (size_t i = 0; i < kTotal2D; ++i) {
      ASSERT_NE(nullptr, full_out[i]);
      EXPECT_STREQ(kStrings[i], full_out[i]);
    }
  }

  {
    ScopedNcStrings one_out(1);
    const std::array<size_t, 2> index = {1, 1};
    ASSERT_EQ(NC_NOERR,
              nc_get_var1_string(nc, arr_varid, index.data(), one_out.data()));
    ASSERT_NE(nullptr, one_out[0]);
    EXPECT_STREQ("epsilon", one_out[0]);
  }

  {
    ScopedNcStrings vara_out(4);
    const std::array<size_t, 2> start = {0, 1};
    const std::array<size_t, 2> count = {2, 2};
    ASSERT_EQ(NC_NOERR, nc_get_vara_string(nc, arr_varid, start.data(),
                                           count.data(), vara_out.data()));
    EXPECT_STREQ("beta", vara_out[0]);
    EXPECT_STREQ("gamma", vara_out[1]);
    EXPECT_STREQ("epsilon", vara_out[2]);
    EXPECT_STREQ("zeta", vara_out[3]);
  }

  {
    ScopedNcStrings vars_out(4);
    const std::array<size_t, 2> start = {0, 0};
    const std::array<size_t, 2> count = {2, 2};
    const std::array<ptrdiff_t, 2> stride = {1, 2};
    ASSERT_EQ(NC_NOERR,
              nc_get_vars_string(nc, arr_varid, start.data(), count.data(),
                                 stride.data(), vars_out.data()));
    EXPECT_STREQ("alpha", vars_out[0]);
    EXPECT_STREQ("gamma", vars_out[1]);
    EXPECT_STREQ("delta", vars_out[2]);
    EXPECT_STREQ("zeta", vars_out[3]);
  }

  {
    ScopedNcStrings varm_out(kTotal2D + kVarmPadding);
    const std::array<size_t, 2> start = {0, 0};
    const std::array<size_t, 2> count = {kRows, kCols};
    const std::array<ptrdiff_t, 2> stride = {1, 1};
    const std::array<ptrdiff_t, 2> imap = {1, static_cast<ptrdiff_t>(kRows)};
    ASSERT_EQ(NC_NOERR,
              nc_get_varm_string(nc, arr_varid, start.data(), count.data(),
                                 stride.data(), imap.data(), varm_out.data()));
    EXPECT_STREQ("alpha", varm_out[0]);
    EXPECT_STREQ("delta", varm_out[1]);
    EXPECT_STREQ("beta", varm_out[2]);
    EXPECT_STREQ("epsilon", varm_out[3]);
    EXPECT_STREQ("gamma", varm_out[4]);
    EXPECT_STREQ("zeta", varm_out[5]);
  }

  // Untyped nc_get_{var,var1,vara,vars} also allocate NC_STRING payloads.
  {
    ScopedNcStrings untyped_var(kTotal2D);
    ASSERT_EQ(NC_NOERR, nc_get_var(nc, arr_varid, untyped_var.data()));
    EXPECT_STREQ("alpha", untyped_var[0]);
    EXPECT_STREQ("zeta", untyped_var[5]);
  }
  {
    ScopedNcStrings untyped_var1(1);
    const std::array<size_t, 2> index = {0, 2};
    ASSERT_EQ(NC_NOERR,
              nc_get_var1(nc, arr_varid, index.data(), untyped_var1.data()));
    EXPECT_STREQ("gamma", untyped_var1[0]);
  }
  {
    ScopedNcStrings untyped_vara(2);
    const std::array<size_t, 2> start = {1, 0};
    const std::array<size_t, 2> count = {1, 2};
    ASSERT_EQ(NC_NOERR, nc_get_vara(nc, arr_varid, start.data(), count.data(),
                                    untyped_vara.data()));
    EXPECT_STREQ("delta", untyped_vara[0]);
    EXPECT_STREQ("epsilon", untyped_vara[1]);
  }
  {
    ScopedNcStrings untyped_vars(2);
    const std::array<size_t, 2> start = {1, 0};
    const std::array<size_t, 2> count = {1, 2};
    const std::array<ptrdiff_t, 2> stride = {1, 2};
    ASSERT_EQ(NC_NOERR, nc_get_vars(nc, arr_varid, start.data(), count.data(),
                                    stride.data(), untyped_vars.data()));
    EXPECT_STREQ("delta", untyped_vars[0]);
    EXPECT_STREQ("zeta", untyped_vars[1]);
  }
}

TEST(DvargetTest, UserDefinedTypesDirectAndPublicDispatchNetcdf4) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create("nc4_udt_get.nc", NC_NETCDF4 | NC_DISKLESS, nc.ptr()));

  int dimid = -1;
  constexpr size_t kLen = 4;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "len", kLen, &dimid));

  nc_type enum_t = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_enum(nc, NC_INT, "color_t", &enum_t));
  const int red_val = 1;
  const int green_val = 2;
  const int blue_val = 7;
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_t, "RED", &red_val));
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_t, "GREEN", &green_val));
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_t, "BLUE", &blue_val));

  nc_type opaque_t = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc, 4, "opaque4_t", &opaque_t));

  nc_type comp_t = NC_NAT;
  ASSERT_EQ(NC_NOERR,
            nc_def_compound(nc, sizeof(PairRecord), "pair_t", &comp_t));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc, comp_t, "id",
                               NC_COMPOUND_OFFSET(PairRecord, id), NC_INT64));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(nc, comp_t, "weight",
                                         NC_COMPOUND_OFFSET(PairRecord, weight),
                                         NC_DOUBLE));

  nc_type vlen_t = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_vlen(nc, "ivec_t", NC_INT, &vlen_t));

  int scalar_enum_id = -1;
  int enum_varid = -1;
  int opaque_varid = -1;
  int comp_varid = -1;
  int vlen_varid = -1;
  int int_varid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "scalar_enum", enum_t, 0, nullptr, &scalar_enum_id));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "enum_arr", enum_t, 1, &dimid, &enum_varid));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "opaque_arr", opaque_t, 1, &dimid, &opaque_varid));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "comp_arr", comp_t, 1, &dimid, &comp_varid));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "vlen_arr", vlen_t, 1, &dimid, &vlen_varid));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "int_arr", NC_INT, 1, &dimid, &int_varid));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));

  const int scalar_enum_in = blue_val;
  const std::array<int, kLen> enum_in = {red_val, green_val, blue_val, red_val};
  const std::array<std::array<unsigned char, 4>, kLen> opaque_in = {{
      {0x01, 0x02, 0x03, 0x04},
      {0x11, 0x12, 0x13, 0x14},
      {0x21, 0x22, 0x23, 0x24},
      {0x31, 0x32, 0x33, 0x34},
  }};
  const std::array<PairRecord, kLen> comp_in = {{
      {10, 1.5},
      {20, 2.5},
      {30, 3.5},
      {40, 4.5},
  }};
  std::array<int, 1> vlen_b0 = {100};
  std::array<int, 2> vlen_b1 = {200, 201};
  std::array<int, 3> vlen_b2 = {300, 301, 302};
  std::array<int, 1> vlen_b3 = {400};
  std::array<nc_vlen_t, kLen> vlen_in = {{
      {vlen_b0.size(), vlen_b0.data()},
      {vlen_b1.size(), vlen_b1.data()},
      {vlen_b2.size(), vlen_b2.data()},
      {vlen_b3.size(), vlen_b3.data()},
  }};
  const std::array<int, kLen> int_in = {1, 2, 3, 4};

  ASSERT_EQ(NC_NOERR, nc_put_var(nc, scalar_enum_id, &scalar_enum_in));
  ASSERT_EQ(NC_NOERR, nc_put_var(nc, enum_varid, enum_in.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var(nc, opaque_varid, opaque_in.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var(nc, comp_varid, comp_in.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var(nc, vlen_varid, vlen_in.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, int_varid, int_in.data()));

  // Public nc_get_{var,var1,vara,vars} on UDTs.
  std::array<std::array<unsigned char, 4>, kLen> opaque_out{};
  ASSERT_EQ(NC_NOERR, nc_get_var(nc, opaque_varid, opaque_out.data()));
  EXPECT_EQ(opaque_in, opaque_out);

  PairRecord comp_one{};
  const size_t comp_idx = 2;
  ASSERT_EQ(NC_NOERR, nc_get_var1(nc, comp_varid, &comp_idx, &comp_one));
  EXPECT_EQ(30, comp_one.id);
  EXPECT_DOUBLE_EQ(3.5, comp_one.weight);

  // VLEN read with nc_reclaim_data ordered strictly before ScopedNcFile closes.
  {
    std::vector<nc_vlen_t> vlen_out(kLen);
    absl::Cleanup reclaim_vlens = [&] {
      EXPECT_EQ(NC_NOERR,
                nc_reclaim_data(nc, vlen_t, vlen_out.data(), vlen_out.size()));
    };
    ASSERT_EQ(NC_NOERR, nc_get_var(nc, vlen_varid, vlen_out.data()));
    EXPECT_EQ(2U, vlen_out[1].len);
    const auto* p1 = static_cast<const int*>(vlen_out[1].p);
    EXPECT_EQ(200, p1[0]);
    EXPECT_EQ(201, p1[1]);
    EXPECT_EQ(3U, vlen_out[2].len);
  }

  // Exercise NCDEFAULT_get_vars directly on NetCDF-4 UDTs to cover
  // memtype > NC_MAX_ATOMIC_TYPE, scalar UDTs, strided UDT odometer loops, and
  // NC_EBADTYPE mismatches (lines 181-191, 210-217 of dvarget.c).
  {
    int scalar_out = -1;
    ASSERT_EQ(NC_NOERR, NCDEFAULT_get_vars(nc, scalar_enum_id, nullptr, nullptr,
                                           nullptr, &scalar_out, NC_NAT));
    EXPECT_EQ(blue_val, scalar_out);
  }

  {
    std::vector<PairRecord> comp_strided(2);
    const size_t start = 1;
    const size_t count = 2;
    const ptrdiff_t stride = 2;
    ASSERT_EQ(NC_NOERR,
              NCDEFAULT_get_vars(nc, comp_varid, &start, &count, &stride,
                                 comp_strided.data(), comp_t));
    EXPECT_EQ(20, comp_strided[0].id);
    EXPECT_DOUBLE_EQ(2.5, comp_strided[0].weight);
    EXPECT_EQ(40, comp_strided[1].id);
    EXPECT_DOUBLE_EQ(4.5, comp_strided[1].weight);
  }

  {
    std::vector<nc_vlen_t> vlen_strided(2);
    absl::Cleanup reclaim_strided = [&] {
      EXPECT_EQ(NC_NOERR, nc_reclaim_data(nc, vlen_t, vlen_strided.data(),
                                          vlen_strided.size()));
    };
    const size_t start = 0;
    const size_t count = 2;
    const ptrdiff_t stride = 2;
    ASSERT_EQ(NC_NOERR,
              NCDEFAULT_get_vars(nc, vlen_varid, &start, &count, &stride,
                                 vlen_strided.data(), NC_NAT));
    EXPECT_EQ(1U, vlen_strided[0].len);
    EXPECT_EQ(100, static_cast<const int*>(vlen_strided[0].p)[0]);
    EXPECT_EQ(3U, vlen_strided[1].len);
    EXPECT_EQ(302, static_cast<const int*>(vlen_strided[1].p)[2]);
  }

  // Non-atomic mismatch in either direction returns NC_EBADTYPE.
  {
    int mismatch_buf[4] = {};
    const size_t start = 0;
    const size_t count = 2;
    const ptrdiff_t stride = 1;
    EXPECT_EQ(NC_EBADTYPE, NCDEFAULT_get_vars(nc, enum_varid, &start, &count,
                                              &stride, mismatch_buf, NC_INT));
    EXPECT_EQ(NC_EBADTYPE, NCDEFAULT_get_vars(nc, int_varid, &start, &count,
                                              &stride, mismatch_buf, enum_t));
  }

  // nc_get_varm rejects user-defined types with NC_EMAPTYPE.
  {
    std::vector<int> varm_buf(kLen + kVarmPadding, -1);
    const size_t start = 0;
    const size_t count = 2;
    const ptrdiff_t stride = 1;
    const ptrdiff_t imap = 1;
    EXPECT_EQ(NC_EMAPTYPE, nc_get_varm(nc, enum_varid, &start, &count, &stride,
                                       &imap, varm_buf.data()));
  }
}

TEST(DvargetTest, NullParametersScalarAndZeroCountHandling) {
  for (const int mode : {NC_CDF5 | NC_DISKLESS, NC_NETCDF4 | NC_DISKLESS}) {
    SCOPED_TRACE(mode);
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create("null_zero_get.nc", mode, nc.ptr()));

    int dimids[2] = {-1, -1};
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "rows", kRows, &dimids[0]));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "cols", kCols, &dimids[1]));

    int scalar_varid = -1;
    int arr_varid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "scalar_i", NC_INT, 0, nullptr, &scalar_varid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "arr_i", NC_INT, 2, dimids, &arr_varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    const int scalar_in = 42;
    constexpr std::array<int, kTotal2D> kArrIn = {1, 2, 3, 4, 5, 6};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, scalar_varid, &scalar_in));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, arr_varid, kArrIn.data()));

    // Scalar variables tolerate NULL start/count/stride/imap across all APIs.
    {
      int out = -1;
      ASSERT_EQ(NC_NOERR,
                nc_get_vara_int(nc, scalar_varid, nullptr, nullptr, &out));
      EXPECT_EQ(42, out);

      out = -1;
      ASSERT_EQ(NC_NOERR, nc_get_vars_int(nc, scalar_varid, nullptr, nullptr,
                                          nullptr, &out));
      EXPECT_EQ(42, out);

      out = -1;
      ASSERT_EQ(NC_NOERR, nc_get_varm_int(nc, scalar_varid, nullptr, nullptr,
                                          nullptr, nullptr, &out));
      EXPECT_EQ(42, out);
    }

    // NULL start on an array variable yields NC_EINVALCOORDS.
    {
      std::vector<int> out(kTotal2D + kVarmPadding, -999);
      const std::array<size_t, 2> count = {1, 1};
      const std::array<ptrdiff_t, 2> stride = {1, 1};
      const std::array<ptrdiff_t, 2> imap = {1, 1};
      EXPECT_EQ(NC_EINVALCOORDS, nc_get_vara_int(nc, arr_varid, nullptr,
                                                 count.data(), out.data()));
      EXPECT_EQ(NC_EINVALCOORDS,
                nc_get_vars_int(nc, arr_varid, nullptr, count.data(),
                                stride.data(), out.data()));
      EXPECT_EQ(NC_EINVALCOORDS,
                nc_get_varm_int(nc, arr_varid, nullptr, count.data(),
                                stride.data(), imap.data(), out.data()));
      EXPECT_EQ(NC_EINVALCOORDS,
                NCDEFAULT_get_vars(nc, arr_varid, nullptr, count.data(),
                                   stride.data(), out.data(), NC_INT));
    }

    // NULL count / stride expand to full dimension extent and unit stride.
    {
      const std::array<size_t, 2> start = {0, 0};
      std::vector<int> vara_out(kTotal2D, -999);
      ASSERT_EQ(NC_NOERR, nc_get_vara_int(nc, arr_varid, start.data(), nullptr,
                                          vara_out.data()));
      EXPECT_THAT(vara_out, ElementsAreArray(kArrIn));

      std::vector<int> vars_out(kTotal2D, -999);
      ASSERT_EQ(NC_NOERR, nc_get_vars_int(nc, arr_varid, start.data(), nullptr,
                                          nullptr, vars_out.data()));
      EXPECT_THAT(vars_out, ElementsAreArray(kArrIn));

      std::vector<int> varm_out(kTotal2D + kVarmPadding, -999);
      const std::array<ptrdiff_t, 2> imap = {1, static_cast<ptrdiff_t>(kRows)};
      ASSERT_EQ(NC_NOERR,
                nc_get_varm_int(nc, arr_varid, start.data(), nullptr, nullptr,
                                imap.data(), varm_out.data()));
      EXPECT_THAT(std::vector<int>(varm_out.data(), varm_out.data() + kTotal2D),
                  ElementsAre(1, 4, 2, 5, 3, 6));
    }

    // Zero-length edges read nothing and leave output untouched.
    // Note: NetCDF-4 validates start + stride * (count - 1) < dimlen on every
    // positive-count dimension even when another dimension has count == 0.
    {
      const std::array<size_t, 2> start = {0, 0};
      const std::array<size_t, 2> zero_dim0 = {0, kCols};
      const std::array<size_t, 2> strided_zero_dim0 = {0, 2};
      const std::array<size_t, 2> strided_zero_dim1 = {1, 0};
      const std::array<ptrdiff_t, 2> stride2 = {2, 2};
      const std::array<ptrdiff_t, 2> imap = {1, 2};
      std::vector<int> untouched(kTotal2D + kVarmPadding, -555);

      EXPECT_EQ(NC_NOERR, nc_get_vara_int(nc, arr_varid, start.data(),
                                          zero_dim0.data(), untouched.data()));
      EXPECT_EQ(NC_NOERR, nc_get_vars_int(nc, arr_varid, start.data(),
                                          strided_zero_dim1.data(),
                                          stride2.data(), untouched.data()));
      EXPECT_EQ(
          NC_NOERR,
          nc_get_varm_int(nc, arr_varid, start.data(), strided_zero_dim0.data(),
                          stride2.data(), imap.data(), untouched.data()));
      // Direct NCDEFAULT_get_varm call with start == NULL and zero-count stops
      // cleanly at edges[idim] == 0 before entering the I/O loop.
      EXPECT_EQ(NC_NOERR, NCDEFAULT_get_varm(
                              nc, arr_varid, nullptr, zero_dim0.data(), nullptr,
                              imap.data(), untouched.data(), NC_INT));
      // start[0] == dimlen is valid iff count[0] == 0.
      const std::array<size_t, 2> start_at_end = {kRows, 0};
      EXPECT_EQ(NC_NOERR, nc_get_vars_int(nc, arr_varid, start_at_end.data(),
                                          strided_zero_dim0.data(),
                                          stride2.data(), untouched.data()));
      EXPECT_THAT(
          std::vector<int>(untouched.data(), untouched.data() + kTotal2D),
          ElementsAre(-555, -555, -555, -555, -555, -555));
    }
  }
}

TEST(DvargetTest, DefaultGetVarsMultiDimRecordAndErrorPaths) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create("default_vars_cdf5.nc", NC_CDF5 | NC_DISKLESS, nc.ptr()));

  int dim3d[3] = {-1, -1, -1};
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d0", 3, &dim3d[0]));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d1", 4, &dim3d[1]));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d2", 5, &dim3d[2]));

  int rec_dimid = -1;
  int fixed_col_dimid = -1;
  int d4_dimid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "rec", NC_UNLIMITED, &rec_dimid));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "rcol", 3, &fixed_col_dimid));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "len4", 4, &d4_dimid));

  int var3d_id = -1;
  int rec_varid = -1;
  int short_varid = -1;
  int char_varid = -1;
  const int rec_dims[2] = {rec_dimid, fixed_col_dimid};
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v3d", NC_INT, 3, dim3d, &var3d_id));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "recvar", NC_INT, 2, rec_dims, &rec_varid));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "vshort", NC_SHORT, 1, &d4_dimid, &short_varid));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "vchar", NC_CHAR, 1, &d4_dimid, &char_varid));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));

  std::array<int, 60> data3d{};
  for (int i = 0; i < 60; ++i) {
    data3d[static_cast<size_t>(i)] = i;
  }
  ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, var3d_id, data3d.data()));

  // Write 3 records (3 x 3).
  constexpr std::array<int, 9> kRecData = {100, 101, 102, 103, 104,
                                           105, 106, 107, 108};
  const std::array<size_t, 2> rec_start = {0, 0};
  const std::array<size_t, 2> rec_count = {3, 3};
  ASSERT_EQ(NC_NOERR, nc_put_vara_int(nc, rec_varid, rec_start.data(),
                                      rec_count.data(), kRecData.data()));

  constexpr std::array<short, 4> kShortData = {300, 10, 400, -300};
  ASSERT_EQ(NC_NOERR, nc_put_var_short(nc, short_varid, kShortData.data()));
  constexpr std::array<char, 4> kCharData = {'w', 'x', 'y', 'z'};
  ASSERT_EQ(NC_NOERR, nc_put_var_text(nc, char_varid, kCharData.data()));

  // 3D strided read hits odom_init / odom_more / multi-dim carry in odom_next.
  {
    const std::array<size_t, 3> start = {0, 1, 0};
    const std::array<size_t, 3> count = {2, 2, 2};
    const std::array<ptrdiff_t, 3> stride = {2, 2, 3};
    std::vector<int> out(8, -1);
    ASSERT_EQ(NC_NOERR,
              nc_get_vars_int(nc, var3d_id, start.data(), count.data(),
                              stride.data(), out.data()));
    // Indices: (0,1,0)=5, (0,1,3)=8, (0,3,0)=15, (0,3,3)=18,
    //          (2,1,0)=45, (2,1,3)=48, (2,3,0)=55, (2,3,3)=58.
    EXPECT_THAT(out, ElementsAre(5, 8, 15, 18, 45, 48, 55, 58));
  }

  // Unit-stride 3D call hits simplestride delegation to NC_get_vara.
  {
    const std::array<size_t, 3> start = {1, 2, 3};
    const std::array<size_t, 3> count = {1, 2, 2};
    const std::array<ptrdiff_t, 3> stride1 = {1, 1, 1};
    std::vector<int> out(4, -1);
    ASSERT_EQ(NC_NOERR,
              nc_get_vars_int(nc, var3d_id, start.data(), count.data(),
                              stride1.data(), out.data()));
    // Indices: (1,2,3)=33, (1,2,4)=34, (1,3,3)=38, (1,3,4)=39.
    EXPECT_THAT(out, ElementsAre(33, 34, 38, 39));
  }

  // Record variable reads: explicit count with non-unit stride, direct NULL
  // edges + NULL stride, and direct NULL edges + non-unit stride where the
  // remaining extent (dimlen - start) stays in bounds.
  {
    const std::array<size_t, 2> start0 = {0, 0};
    const std::array<size_t, 2> count2 = {2, 2};
    const std::array<ptrdiff_t, 2> stride2 = {2, 2};
    std::vector<int> explicit_strided_out(4, -1);
    ASSERT_EQ(NC_NOERR,
              NCDEFAULT_get_vars(nc, rec_varid, start0.data(), count2.data(),
                                 stride2.data(), explicit_strided_out.data(),
                                 NC_INT));
    EXPECT_THAT(explicit_strided_out, ElementsAre(100, 102, 106, 108));

    const std::array<size_t, 2> start = {1, 0};
    std::vector<int> out(6, -1);
    ASSERT_EQ(NC_NOERR, NCDEFAULT_get_vars(nc, rec_varid, start.data(), nullptr,
                                           nullptr, out.data(), NC_INT));
    EXPECT_THAT(out, ElementsAre(103, 104, 105, 106, 107, 108));

    const std::array<size_t, 2> start_tail = {1, 2};
    const std::array<ptrdiff_t, 2> stride_tail = {1, 2};
    std::vector<int> tail_out(2, -1);
    ASSERT_EQ(NC_NOERR,
              NCDEFAULT_get_vars(nc, rec_varid, start_tail.data(), nullptr,
                                 stride_tail.data(), tail_out.data(), NC_INT));
    EXPECT_THAT(tail_out, ElementsAre(105, 108));
  }

  // Multiple NC_ERANGE elements in strided loop preserve sticky NC_ERANGE.
  {
    const size_t start = 0;
    const size_t count = 2;
    const ptrdiff_t stride = 2;
    std::vector<signed char> out(2, 0);
    EXPECT_EQ(NC_ERANGE, nc_get_vars_schar(nc, short_varid, &start, &count,
                                           &stride, out.data()));
  }

  // Error checks across NC_get_vars and NCDEFAULT_get_vars.
  {
    const size_t start = 0;
    const size_t count = 1;
    const ptrdiff_t stride_ok = 1;
    int buf[4] = {};
    short sbuf[4] = {};
    char cbuf[4] = {};

    EXPECT_EQ(NC_EBADID,
              nc_get_vars_int(-1, 0, &start, &count, &stride_ok, buf));
    EXPECT_EQ(NC_EBADID, NCDEFAULT_get_vars(-1, 0, &start, &count, &stride_ok,
                                            buf, NC_INT));
    EXPECT_EQ(NC_ENOTVAR,
              nc_get_vars_int(nc, 999, nullptr, nullptr, nullptr, buf));
    EXPECT_EQ(NC_ENOTVAR,
              nc_get_vars_int(nc, 999, &start, &count, &stride_ok, buf));
    EXPECT_EQ(NC_ENOTVAR, NCDEFAULT_get_vars(nc, 999, &start, &count,
                                             &stride_ok, buf, NC_INT));

    // NC_ECHAR in both directions.
    EXPECT_EQ(NC_ECHAR, nc_get_vars_text(nc, short_varid, &start, &count,
                                         &stride_ok, cbuf));
    EXPECT_EQ(NC_ECHAR,
              nc_get_vars_int(nc, char_varid, &start, &count, &stride_ok, buf));

    // Start out of bounds and start == dimlen with count > 0 ->
    // NC_EINVALCOORDS.
    const size_t start_too_big = 5;
    const size_t start_at_dimlen = 4;
    EXPECT_EQ(NC_EINVALCOORDS,
              nc_get_vars_short(nc, short_varid, &start_too_big, &count,
                                &stride_ok, sbuf));
    EXPECT_EQ(NC_EINVALCOORDS,
              nc_get_vars_short(nc, short_varid, &start_at_dimlen, &count,
                                &stride_ok, sbuf));

    // start + count > dimlen -> NC_EEDGE.
    const size_t start2 = 2;
    const size_t count3 = 3;
    EXPECT_EQ(NC_EEDGE, nc_get_vars_short(nc, short_varid, &start2, &count3,
                                          &stride_ok, sbuf));

    // Invalid strides: 0, negative, and >= X_INT_MAX.
    for (const ptrdiff_t bad_stride :
         {ptrdiff_t{0}, ptrdiff_t{-1}, static_cast<ptrdiff_t>(X_INT_MAX)}) {
      EXPECT_EQ(NC_ESTRIDE, nc_get_vars_short(nc, short_varid, &start, &count,
                                              &bad_stride, sbuf));
    }
  }

  // Non-ERANGE error inside odometer loop (NC_EINDEFINE when back in def mode).
  ASSERT_EQ(NC_NOERR, nc_redef(nc));
  {
    const size_t start = 0;
    const size_t count = 2;
    const ptrdiff_t stride = 2;
    std::vector<short> out(2, -1);
    EXPECT_EQ(NC_EINDEFINE, nc_get_vars_short(nc, short_varid, &start, &count,
                                              &stride, out.data()));
  }
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));
}

TEST(DvargetTest, DefaultGetVarmOptimizationsMappingRecordAndErrorPaths) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create("default_varm_cdf5.nc", NC_CDF5 | NC_DISKLESS, nc.ptr()));

  int dim3d[3] = {-1, -1, -1};
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "z", 2, &dim3d[0]));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "y", 3, &dim3d[1]));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 4, &dim3d[2]));

  int sq_dims[2] = {-1, -1};
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "r4", 4, &sq_dims[0]));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "c4", 4, &sq_dims[1]));

  int rec_dimid = -1;
  int rcol_dimid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "rec", NC_UNLIMITED, &rec_dimid));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "rcol", 3, &rcol_dimid));
  const int rec_dims[2] = {rec_dimid, rcol_dimid};

  int erange_dims[2] = {-1, -1};
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "e0", 2, &erange_dims[0]));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "e1", 2, &erange_dims[1]));

  int var3d_id = -1;
  int sq_varid = -1;
  int rec_varid = -1;
  int short2d_id = -1;
  int char2d_id = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v3d", NC_INT, 3, dim3d, &var3d_id));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "vsq", NC_INT, 2, sq_dims, &sq_varid));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "vrec", NC_INT, 2, rec_dims, &rec_varid));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "vshort2d", NC_SHORT, 2, erange_dims, &short2d_id));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "vchar2d", NC_CHAR, 2, erange_dims, &char2d_id));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));

  std::array<int, 24> data3d{};
  for (int i = 0; i < 24; ++i) {
    data3d[static_cast<size_t>(i)] = i;
  }
  ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, var3d_id, data3d.data()));

  std::array<int, 16> data_sq{};
  for (int i = 0; i < 16; ++i) {
    data_sq[static_cast<size_t>(i)] = i;
  }
  ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, sq_varid, data_sq.data()));

  constexpr std::array<int, 9> kRecData = {100, 101, 102, 103, 104,
                                           105, 106, 107, 108};
  const std::array<size_t, 2> rec_start = {0, 0};
  const std::array<size_t, 2> rec_count = {3, 3};
  ASSERT_EQ(NC_NOERR, nc_put_vara_int(nc, rec_varid, rec_start.data(),
                                      rec_count.data(), kRecData.data()));

  constexpr std::array<short, 4> kShort2d = {300, 1, 400, 2};
  ASSERT_EQ(NC_NOERR, nc_put_var_short(nc, short2d_id, kShort2d.data()));
  constexpr std::array<char, 4> kChar2d = {'a', 'b', 'c', 'd'};
  ASSERT_EQ(NC_NOERR, nc_put_var_text(nc, char2d_id, kChar2d.data()));

  // 3D permuted imap + stride hits multi-level carry and break.
  {
    const std::array<size_t, 3> start = {0, 0, 0};
    const std::array<size_t, 3> count = {2, 2, 2};
    const std::array<ptrdiff_t, 3> stride = {1, 2, 2};
    const std::array<ptrdiff_t, 3> imap = {1, 4, 2};
    std::vector<int> out(8 + kVarmPadding, -1);
    ASSERT_EQ(NC_NOERR,
              nc_get_varm_int(nc, var3d_id, start.data(), count.data(),
                              stride.data(), imap.data(), out.data()));
    // Input coords (z in {0,1}, y in {0,2}, x in {0,2}):
    // output[z + 4*(y/2) + 2*(x/2)] = value:
    // (0,0,0)=0->0, (0,0,2)=2->2, (0,2,0)=8->4, (0,2,2)=10->6,
    // (1,0,0)=12->1, (1,0,2)=14->3, (1,2,0)=20->5, (1,2,2)=22->7.
    EXPECT_THAT(std::vector<int>(out.data(), out.data() + 8),
                ElementsAre(0, 12, 2, 14, 8, 20, 10, 22));
  }

  // stride == all 1s && imap == nullptr hits fast path to NC_get_vara.
  {
    const std::array<size_t, 2> start = {1, 1};
    const std::array<size_t, 2> count = {2, 2};
    const std::array<ptrdiff_t, 2> stride1 = {1, 1};
    std::vector<int> out(4, -1);
    ASSERT_EQ(NC_NOERR,
              nc_get_varm_int(nc, sq_varid, start.data(), count.data(),
                              stride1.data(), nullptr, out.data()));
    EXPECT_THAT(out, ElementsAre(5, 6, 9, 10));
  }

  // Non-unit stride with imap == nullptr exercises default mymap computation.
  {
    const std::array<size_t, 2> start = {0, 0};
    const std::array<size_t, 2> count = {2, 2};
    const std::array<ptrdiff_t, 2> stride = {2, 2};
    std::vector<int> out(4 + kVarmPadding, -1);
    ASSERT_EQ(NC_NOERR,
              nc_get_varm_int(nc, sq_varid, start.data(), count.data(),
                              stride.data(), nullptr, out.data()));
    EXPECT_THAT(std::vector<int>(out.data(), out.data() + 4),
                ElementsAre(0, 2, 8, 10));
  }

  // mystride[maxidim] == 1 && mymap[maxidim] == 1 optimization branch vs
  // non-optimized transposed imap branch.
  {
    const std::array<size_t, 2> start = {0, 0};
    const std::array<size_t, 2> count = {2, 3};
    const std::array<ptrdiff_t, 2> stride = {2, 1};
    const std::array<ptrdiff_t, 2> opt_imap = {3, 1};
    std::vector<int> opt_out(6 + kVarmPadding, -1);
    ASSERT_EQ(NC_NOERR,
              nc_get_varm_int(nc, sq_varid, start.data(), count.data(),
                              stride.data(), opt_imap.data(), opt_out.data()));
    EXPECT_THAT(std::vector<int>(opt_out.data(), opt_out.data() + 6),
                ElementsAre(0, 1, 2, 8, 9, 10));

    const std::array<ptrdiff_t, 2> trans_imap = {1, 2};
    std::vector<int> trans_out(6 + kVarmPadding, -1);
    ASSERT_EQ(NC_NOERR, nc_get_varm_int(nc, sq_varid, start.data(),
                                        count.data(), stride.data(),
                                        trans_imap.data(), trans_out.data()));
    EXPECT_THAT(std::vector<int>(trans_out.data(), trans_out.data() + 6),
                ElementsAre(0, 8, 1, 9, 2, 10));
  }

  // Direct NCDEFAULT_get_varm with NULL edges and NULL stride on a 2D record
  // variable hits isrecvar edge computation and stride == NULL paths.
  {
    const std::array<size_t, 2> start = {0, 0};
    const std::array<ptrdiff_t, 2> imap = {1, 3};
    std::vector<int> out(9 + kVarmPadding, -1);
    ASSERT_EQ(NC_NOERR,
              NCDEFAULT_get_varm(nc, rec_varid, start.data(), nullptr, nullptr,
                                 imap.data(), out.data(), NC_INT));
    EXPECT_THAT(std::vector<int>(out.data(), out.data() + 9),
                ElementsAre(100, 103, 106, 101, 104, 107, 102, 105, 108));
  }

  // Multiple NC_ERANGE elements inside varm loop.
  {
    const std::array<size_t, 2> start = {0, 0};
    const std::array<size_t, 2> count = {2, 2};
    const std::array<ptrdiff_t, 2> stride = {1, 1};
    const std::array<ptrdiff_t, 2> imap = {1, 2};
    std::vector<signed char> out(4 + kVarmPadding, 0);
    EXPECT_EQ(NC_ERANGE,
              nc_get_varm_schar(nc, short2d_id, start.data(), count.data(),
                                stride.data(), imap.data(), out.data()));
  }

  // Error checks for NC_get_varm and NCDEFAULT_get_varm.
  {
    const std::array<size_t, 2> start = {0, 0};
    const std::array<size_t, 2> count = {1, 1};
    const std::array<ptrdiff_t, 2> stride_ok = {1, 1};
    const std::array<ptrdiff_t, 2> imap = {1, 1};
    int buf[8] = {};
    char cbuf[8] = {};

    EXPECT_EQ(NC_EBADID, nc_get_varm_int(-1, 0, start.data(), count.data(),
                                         stride_ok.data(), imap.data(), buf));
    EXPECT_EQ(NC_EBADID,
              NCDEFAULT_get_varm(-1, 0, start.data(), count.data(),
                                 stride_ok.data(), imap.data(), buf, NC_INT));
    EXPECT_EQ(NC_ENOTVAR, nc_get_varm_int(nc, 999, nullptr, nullptr, nullptr,
                                          imap.data(), buf));
    EXPECT_EQ(NC_ENOTVAR, nc_get_varm_int(nc, 999, start.data(), count.data(),
                                          stride_ok.data(), imap.data(), buf));
    EXPECT_EQ(NC_ENOTVAR,
              NCDEFAULT_get_varm(nc, 999, start.data(), count.data(),
                                 stride_ok.data(), imap.data(), buf, NC_INT));

    EXPECT_EQ(NC_ECHAR,
              nc_get_varm_text(nc, short2d_id, start.data(), count.data(),
                               stride_ok.data(), imap.data(), cbuf));
    EXPECT_EQ(NC_ECHAR,
              nc_get_varm_int(nc, char2d_id, start.data(), count.data(),
                              stride_ok.data(), imap.data(), buf));

    for (const ptrdiff_t bad :
         {ptrdiff_t{0}, ptrdiff_t{-1}, static_cast<ptrdiff_t>(X_INT_MAX)}) {
      const std::array<ptrdiff_t, 2> bad_stride = {bad, 1};
      EXPECT_EQ(NC_ESTRIDE,
                nc_get_varm_int(nc, sq_varid, start.data(), count.data(),
                                bad_stride.data(), imap.data(), buf));
    }

    const std::array<size_t, 2> start_gt = {5, 0};
    const std::array<size_t, 2> start_eq = {4, 0};
    const std::array<size_t, 2> start_edge = {2, 0};
    const std::array<size_t, 2> count_edge = {3, 1};
    EXPECT_EQ(NC_EINVALCOORDS,
              nc_get_varm_int(nc, sq_varid, start_gt.data(), count.data(),
                              stride_ok.data(), imap.data(), buf));
    EXPECT_EQ(NC_EINVALCOORDS,
              nc_get_varm_int(nc, sq_varid, start_eq.data(), count.data(),
                              stride_ok.data(), imap.data(), buf));
    EXPECT_EQ(NC_EEDGE, nc_get_varm_int(nc, sq_varid, start_edge.data(),
                                        count_edge.data(), stride_ok.data(),
                                        imap.data(), buf));
  }

  ASSERT_EQ(NC_NOERR, nc_redef(nc));
  {
    const std::array<size_t, 2> start = {0, 0};
    const std::array<size_t, 2> count = {1, 2};
    const std::array<ptrdiff_t, 2> stride = {1, 1};
    const std::array<ptrdiff_t, 2> imap = {1, 1};
    std::vector<int> out(2 + kVarmPadding, -1);
    EXPECT_EQ(NC_EINDEFINE,
              nc_get_varm_int(nc, sq_varid, start.data(), count.data(),
                              stride.data(), imap.data(), out.data()));
  }
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));
}

TEST(DvargetTest, VaraVar1VarErrorsAndPersistedMemioRoundTrip) {
  // Error checks for nc_get_vara, NC_get_vara, nc_get_var1, and nc_get_var.
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create("vara_errors.nc", NC_CDF5 | NC_DISKLESS, nc.ptr()));

    int dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "n", 4, &dimid));
    int var_int = -1;
    int var_char = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "vi", NC_INT, 1, &dimid, &var_int));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "vc", NC_CHAR, 1, &dimid, &var_char));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    const std::array<int, 4> vi = {11, 22, 33, 44};
    const std::array<char, 4> vc = {'p', 'q', 'r', 's'};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, var_int, vi.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_text(nc, var_char, vc.data()));

    const size_t start = 0;
    const size_t count = 1;
    int ibuf[4] = {};
    char cbuf[4] = {};

    EXPECT_EQ(NC_EBADID, nc_get_vara(-1, 0, &start, &count, ibuf));
    EXPECT_EQ(NC_ENOTVAR, nc_get_vara(nc, 999, &start, &count, ibuf));
    EXPECT_EQ(NC_EBADID, nc_get_vara_int(-1, 0, &start, &count, ibuf));
    EXPECT_EQ(NC_ENOTVAR, nc_get_vara_int(nc, 999, nullptr, nullptr, ibuf));
    EXPECT_EQ(NC_ENOTVAR, nc_get_vara_int(nc, 999, &start, &count, ibuf));
    EXPECT_EQ(NC_EBADID, nc_get_var1_int(-1, 0, &start, ibuf));
    EXPECT_EQ(NC_ENOTVAR, nc_get_var1_int(nc, 999, &start, ibuf));
    EXPECT_EQ(NC_EBADID, nc_get_var_int(-1, 0, ibuf));
    EXPECT_EQ(NC_ENOTVAR, nc_get_var_int(nc, 999, ibuf));

    EXPECT_EQ(NC_ECHAR, nc_get_vara_text(nc, var_int, &start, &count, cbuf));
    EXPECT_EQ(NC_ECHAR, nc_get_vara_int(nc, var_char, &start, &count, ibuf));

    const size_t oob = 4;
    const size_t edge_oob = 3;
    const size_t edge_start = 2;
    EXPECT_EQ(NC_EINVALCOORDS,
              nc_get_vara_int(nc, var_int, &oob, &count, ibuf));
    EXPECT_EQ(NC_EINVALCOORDS, nc_get_var1_int(nc, var_int, &oob, ibuf));
    EXPECT_EQ(NC_EEDGE,
              nc_get_vara_int(nc, var_int, &edge_start, &edge_oob, ibuf));

    ASSERT_EQ(NC_NOERR, nc_redef(nc));
    EXPECT_EQ(NC_EINDEFINE, nc_get_vara_int(nc, var_int, &start, &count, ibuf));
    EXPECT_EQ(NC_EINDEFINE, nc_get_var1_int(nc, var_int, &start, ibuf));
    EXPECT_EQ(NC_EINDEFINE, nc_get_var_int(nc, var_int, ibuf));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));
  }

  // Persisted nc_create_mem -> nc_close_memio -> nc_open_mem readback across
  // CDF-5 and NetCDF-4.
  for (const int mode : {NC_CDF5, NC_NETCDF4}) {
    SCOPED_TRACE(mode);
    ScopedMemio memio;
    {
      ScopedNcFile writer;
      ASSERT_EQ(NC_NOERR,
                nc_create_mem("persisted_get.nc", mode, 4096, writer.ptr()));
      int dimids[2] = {-1, -1};
      ASSERT_EQ(NC_NOERR, nc_def_dim(writer, "r", kRows, &dimids[0]));
      ASSERT_EQ(NC_NOERR, nc_def_dim(writer, "c", kCols, &dimids[1]));
      int varid = -1;
      ASSERT_EQ(NC_NOERR,
                nc_def_var(writer, "m", NC_DOUBLE, 2, dimids, &varid));
      ASSERT_EQ(NC_NOERR, nc_enddef(writer));
      constexpr std::array<double, kTotal2D> kData = {1.5, 2.5, 3.5,
                                                      4.5, 5.5, 6.5};
      ASSERT_EQ(NC_NOERR, nc_put_var_double(writer, varid, kData.data()));
      ASSERT_EQ(NC_NOERR, nc_close_memio(writer.release(), memio.get()));
    }

    ScopedNcFile reader;
    ASSERT_EQ(NC_NOERR,
              nc_open_mem("persisted_get.nc", NC_NOWRITE, memio.size(),
                          memio.get()->memory, reader.ptr()));
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(reader, "m", &varid));

    VerifyNumericGetFamilies<double>(
        reader, varid, {1.5, 2.5, 3.5, 4.5, 5.5, 6.5}, -999.0,
        nc_get_var_double, nc_get_var1_double, nc_get_vara_double,
        nc_get_vars_double, nc_get_varm_double);
  }
}

}  // namespace
}  // namespace netcdf
