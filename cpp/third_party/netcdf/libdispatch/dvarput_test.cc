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

// Unit tests for //third_party/netcdf/libdispatch/dvarput.c.
//
// Tests cover all 75 public nc_put_var* API functions (untyped, _text, _schar,
// _uchar, _short, _int, _long, _float, _double, _ubyte, _ushort, _uint,
// _longlong, _ulonglong, and _string across vara, var1, var, vars, and varm),
// NetCDF-4/HDF5 and Classic/64-bit-offset/CDF-5 format dispatch, default
// NCDEFAULT_put_vars and NCDEFAULT_put_varm odometer/fast-path/coalescing
// logic, NULL coordinate/count/stride handling, unlimited record extension, and
// every validation and error-priority branch.

#include <array>
#include <cstddef>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
// clang-format off
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
// clang-format on

extern "C" {
#include "third_party/netcdf/include/ncdispatch.h"
}  // extern "C"

namespace netcdf {
namespace {

using ::testing::ElementsAre;

TEST(DvarputTest, PutVaraAll15VariantsNetcdf4AndReopenMem) {
  ScopedMemio memio;
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dvarput_vara_all.nc", NC_NETCDF4, 4096, nc.ptr()));

  int dim_id = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "len", 4, &dim_id));

  int v_untyped = -1;
  int v_text = -1;
  int v_schar = -1;
  int v_uchar = -1;
  int v_short = -1;
  int v_int = -1;
  int v_long = -1;
  int v_float = -1;
  int v_double = -1;
  int v_ubyte = -1;
  int v_ushort = -1;
  int v_uint = -1;
  int v_int64 = -1;
  int v_uint64 = -1;
  int v_string = -1;

  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_untyped", NC_INT, 1, &dim_id, &v_untyped));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_text", NC_CHAR, 1, &dim_id, &v_text));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_schar", NC_BYTE, 1, &dim_id, &v_schar));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_uchar", NC_UBYTE, 1, &dim_id, &v_uchar));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_short", NC_SHORT, 1, &dim_id, &v_short));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_int", NC_INT, 1, &dim_id, &v_int));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_long", NC_INT64, 1, &dim_id, &v_long));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_float", NC_FLOAT, 1, &dim_id, &v_float));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_double", NC_DOUBLE, 1, &dim_id, &v_double));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_ubyte", NC_UBYTE, 1, &dim_id, &v_ubyte));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_ushort", NC_USHORT, 1, &dim_id, &v_ushort));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_uint", NC_UINT, 1, &dim_id, &v_uint));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_int64", NC_INT64, 1, &dim_id, &v_int64));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_uint64", NC_UINT64, 1, &dim_id, &v_uint64));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_string", NC_STRING, 1, &dim_id, &v_string));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));

  const std::array<size_t, 1> start = {1};
  const std::array<size_t, 1> count = {3};

  const std::array<int, 3> untyped_vals = {10, 20, 30};
  const std::array<char, 3> text_vals = {'x', 'y', 'z'};
  const std::array<signed char, 3> schar_vals = {-10, 0, 11};
  const std::array<unsigned char, 3> uchar_vals = {200, 201, 202};
  const std::array<short, 3> short_vals = {-1000, 250, 1000};
  const std::array<int, 3> int_vals = {-50000, 42, 50000};
  const std::array<long, 3> long_vals = {-70000L, 123L, 70000L};
  const std::array<float, 3> float_vals = {1.5F, -2.25F, 3.75F};
  const std::array<double, 3> double_vals = {1.125, -2.5, 99.75};
  const std::array<unsigned char, 3> ubyte_vals = {10, 150, 250};
  const std::array<unsigned short, 3> ushort_vals = {100, 30000, 65000};
  const std::array<unsigned int, 3> uint_vals = {7U, 80000U, 4000000000U};
  const std::array<long long, 3> ll_vals = {-9000000000LL, 0LL, 9000000000LL};
  const std::array<unsigned long long, 3> ull_vals = {1ULL, 100000000000ULL,
                                                      18000000000000000000ULL};
  std::array<const char*, 3> str_vals = {"one", "two", "three"};

  ASSERT_EQ(NC_NOERR, nc_put_vara(nc, v_untyped, start.data(), count.data(),
                                  untyped_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vara_text(nc, v_text, start.data(), count.data(),
                                       text_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vara_schar(nc, v_schar, start.data(), count.data(),
                                        schar_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vara_uchar(nc, v_uchar, start.data(), count.data(),
                                        uchar_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vara_short(nc, v_short, start.data(), count.data(),
                                        short_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vara_int(nc, v_int, start.data(), count.data(),
                                      int_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vara_long(nc, v_long, start.data(), count.data(),
                                       long_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vara_float(nc, v_float, start.data(), count.data(),
                                        float_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vara_double(nc, v_double, start.data(),
                                         count.data(), double_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vara_ubyte(nc, v_ubyte, start.data(), count.data(),
                                        ubyte_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vara_ushort(nc, v_ushort, start.data(),
                                         count.data(), ushort_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vara_uint(nc, v_uint, start.data(), count.data(),
                                       uint_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vara_longlong(nc, v_int64, start.data(),
                                           count.data(), ll_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vara_ulonglong(nc, v_uint64, start.data(),
                                            count.data(), ull_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vara_string(nc, v_string, start.data(),
                                         count.data(), str_vals.data()));

  ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));

  ScopedNcFile ro_nc;
  ASSERT_EQ(NC_NOERR,
            nc_open_mem("dvarput_vara_all_ro.nc", NC_NOWRITE, memio.size(),
                        memio.get()->memory, ro_nc.ptr()));

  // NetCDF-4 in-memory creation uses H5P_DEFAULT fcpl without link
  // creation-order tracking, so reopened varids follow alphabetical order
  // rather than creation order. Resolve varids by name on ro_nc.
  int ro_untyped = -1;
  int ro_text = -1;
  int ro_schar = -1;
  int ro_uchar = -1;
  int ro_short = -1;
  int ro_int = -1;
  int ro_long = -1;
  int ro_float = -1;
  int ro_double = -1;
  int ro_ubyte = -1;
  int ro_ushort = -1;
  int ro_uint = -1;
  int ro_int64 = -1;
  int ro_uint64 = -1;
  int ro_string = -1;

  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "v_untyped", &ro_untyped));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "v_text", &ro_text));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "v_schar", &ro_schar));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "v_uchar", &ro_uchar));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "v_short", &ro_short));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "v_int", &ro_int));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "v_long", &ro_long));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "v_float", &ro_float));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "v_double", &ro_double));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "v_ubyte", &ro_ubyte));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "v_ushort", &ro_ushort));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "v_uint", &ro_uint));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "v_int64", &ro_int64));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "v_uint64", &ro_uint64));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "v_string", &ro_string));

  std::array<int, 3> got_untyped{};
  std::array<char, 3> got_text{};
  std::array<signed char, 3> got_schar{};
  std::array<unsigned char, 3> got_uchar{};
  std::array<short, 3> got_short{};
  std::array<int, 3> got_int{};
  std::array<long, 3> got_long{};
  std::array<float, 3> got_float{};
  std::array<double, 3> got_double{};
  std::array<unsigned char, 3> got_ubyte{};
  std::array<unsigned short, 3> got_ushort{};
  std::array<unsigned int, 3> got_uint{};
  std::array<long long, 3> got_ll{};
  std::array<unsigned long long, 3> got_ull{};
  ScopedNcStrings got_strings(3);

  ASSERT_EQ(NC_NOERR, nc_get_vara(ro_nc, ro_untyped, start.data(), count.data(),
                                  got_untyped.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vara_text(ro_nc, ro_text, start.data(),
                                       count.data(), got_text.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vara_schar(ro_nc, ro_schar, start.data(),
                                        count.data(), got_schar.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vara_uchar(ro_nc, ro_uchar, start.data(),
                                        count.data(), got_uchar.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vara_short(ro_nc, ro_short, start.data(),
                                        count.data(), got_short.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vara_int(ro_nc, ro_int, start.data(), count.data(),
                                      got_int.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vara_long(ro_nc, ro_long, start.data(),
                                       count.data(), got_long.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vara_float(ro_nc, ro_float, start.data(),
                                        count.data(), got_float.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vara_double(ro_nc, ro_double, start.data(),
                                         count.data(), got_double.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vara_ubyte(ro_nc, ro_ubyte, start.data(),
                                        count.data(), got_ubyte.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vara_ushort(ro_nc, ro_ushort, start.data(),
                                         count.data(), got_ushort.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vara_uint(ro_nc, ro_uint, start.data(),
                                       count.data(), got_uint.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vara_longlong(ro_nc, ro_int64, start.data(),
                                           count.data(), got_ll.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vara_ulonglong(ro_nc, ro_uint64, start.data(),
                                            count.data(), got_ull.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vara_string(ro_nc, ro_string, start.data(),
                                         count.data(), got_strings.data()));

  EXPECT_EQ(got_untyped, untyped_vals);
  EXPECT_EQ(got_text, text_vals);
  EXPECT_EQ(got_schar, schar_vals);
  EXPECT_EQ(got_uchar, uchar_vals);
  EXPECT_EQ(got_short, short_vals);
  EXPECT_EQ(got_int, int_vals);
  EXPECT_EQ(got_long, long_vals);
  EXPECT_EQ(got_float, float_vals);
  EXPECT_EQ(got_double, double_vals);
  EXPECT_EQ(got_ubyte, ubyte_vals);
  EXPECT_EQ(got_ushort, ushort_vals);
  EXPECT_EQ(got_uint, uint_vals);
  EXPECT_EQ(got_ll, ll_vals);
  EXPECT_EQ(got_ull, ull_vals);
  EXPECT_STREQ(got_strings[0], "one");
  EXPECT_STREQ(got_strings[1], "two");
  EXPECT_STREQ(got_strings[2], "three");
  got_strings.clear();
}

TEST(DvarputTest, PutVar1All15VariantsNetcdf4) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dvarput_var1_all.nc", NC_NETCDF4, 4096, nc.ptr()));

  int dim_id = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "len", 3, &dim_id));

  int v_untyped = -1;
  int v_text = -1;
  int v_schar = -1;
  int v_uchar = -1;
  int v_short = -1;
  int v_int = -1;
  int v_long = -1;
  int v_float = -1;
  int v_double = -1;
  int v_ubyte = -1;
  int v_ushort = -1;
  int v_uint = -1;
  int v_int64 = -1;
  int v_uint64 = -1;
  int v_string = -1;
  int v_scalar = -1;

  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_untyped", NC_INT, 1, &dim_id, &v_untyped));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_text", NC_CHAR, 1, &dim_id, &v_text));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_schar", NC_BYTE, 1, &dim_id, &v_schar));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_uchar", NC_UBYTE, 1, &dim_id, &v_uchar));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_short", NC_SHORT, 1, &dim_id, &v_short));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_int", NC_INT, 1, &dim_id, &v_int));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_long", NC_INT64, 1, &dim_id, &v_long));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_float", NC_FLOAT, 1, &dim_id, &v_float));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_double", NC_DOUBLE, 1, &dim_id, &v_double));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_ubyte", NC_UBYTE, 1, &dim_id, &v_ubyte));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_ushort", NC_USHORT, 1, &dim_id, &v_ushort));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_uint", NC_UINT, 1, &dim_id, &v_uint));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_int64", NC_INT64, 1, &dim_id, &v_int64));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_uint64", NC_UINT64, 1, &dim_id, &v_uint64));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_string", NC_STRING, 1, &dim_id, &v_string));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_scalar", NC_INT, 0, nullptr, &v_scalar));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));

  const size_t idx = 2;
  const int untyped_val = 77;
  const char text_val = 'K';
  const signed char schar_val = -19;
  const unsigned char uchar_val = 211;
  const short short_val = -321;
  const int int_val = 98765;
  const long long_val = -123456L;
  const float float_val = 6.25F;
  const double double_val = -12.5;
  const unsigned char ubyte_val = 244;
  const unsigned short ushort_val = 54321;
  const unsigned int uint_val = 3000000001U;
  const long long ll_val = -800000000000LL;
  const unsigned long long ull_val = 900000000000ULL;
  const char* str_val = "single_string";
  const int scalar_val = 404;

  ASSERT_EQ(NC_NOERR, nc_put_var1(nc, v_untyped, &idx, &untyped_val));
  ASSERT_EQ(NC_NOERR, nc_put_var1_text(nc, v_text, &idx, &text_val));
  ASSERT_EQ(NC_NOERR, nc_put_var1_schar(nc, v_schar, &idx, &schar_val));
  ASSERT_EQ(NC_NOERR, nc_put_var1_uchar(nc, v_uchar, &idx, &uchar_val));
  ASSERT_EQ(NC_NOERR, nc_put_var1_short(nc, v_short, &idx, &short_val));
  ASSERT_EQ(NC_NOERR, nc_put_var1_int(nc, v_int, &idx, &int_val));
  ASSERT_EQ(NC_NOERR, nc_put_var1_long(nc, v_long, &idx, &long_val));
  ASSERT_EQ(NC_NOERR, nc_put_var1_float(nc, v_float, &idx, &float_val));
  ASSERT_EQ(NC_NOERR, nc_put_var1_double(nc, v_double, &idx, &double_val));
  ASSERT_EQ(NC_NOERR, nc_put_var1_ubyte(nc, v_ubyte, &idx, &ubyte_val));
  ASSERT_EQ(NC_NOERR, nc_put_var1_ushort(nc, v_ushort, &idx, &ushort_val));
  ASSERT_EQ(NC_NOERR, nc_put_var1_uint(nc, v_uint, &idx, &uint_val));
  ASSERT_EQ(NC_NOERR, nc_put_var1_longlong(nc, v_int64, &idx, &ll_val));
  ASSERT_EQ(NC_NOERR, nc_put_var1_ulonglong(nc, v_uint64, &idx, &ull_val));
  ASSERT_EQ(NC_NOERR, nc_put_var1_string(nc, v_string, &idx, &str_val));
  ASSERT_EQ(NC_NOERR, nc_put_var1_int(nc, v_scalar, nullptr, &scalar_val));

  int got_untyped = 0;
  char got_text = '\0';
  signed char got_schar = 0;
  unsigned char got_uchar = 0;
  short got_short = 0;
  int got_int = 0;
  long got_long = 0;
  float got_float = 0.0F;
  double got_double = 0.0;
  unsigned char got_ubyte = 0;
  unsigned short got_ushort = 0;
  unsigned int got_uint = 0;
  long long got_ll = 0;
  unsigned long long got_ull = 0;
  ScopedNcStrings got_string(1);
  int got_scalar = 0;

  ASSERT_EQ(NC_NOERR, nc_get_var1(nc, v_untyped, &idx, &got_untyped));
  ASSERT_EQ(NC_NOERR, nc_get_var1_text(nc, v_text, &idx, &got_text));
  ASSERT_EQ(NC_NOERR, nc_get_var1_schar(nc, v_schar, &idx, &got_schar));
  ASSERT_EQ(NC_NOERR, nc_get_var1_uchar(nc, v_uchar, &idx, &got_uchar));
  ASSERT_EQ(NC_NOERR, nc_get_var1_short(nc, v_short, &idx, &got_short));
  ASSERT_EQ(NC_NOERR, nc_get_var1_int(nc, v_int, &idx, &got_int));
  ASSERT_EQ(NC_NOERR, nc_get_var1_long(nc, v_long, &idx, &got_long));
  ASSERT_EQ(NC_NOERR, nc_get_var1_float(nc, v_float, &idx, &got_float));
  ASSERT_EQ(NC_NOERR, nc_get_var1_double(nc, v_double, &idx, &got_double));
  ASSERT_EQ(NC_NOERR, nc_get_var1_ubyte(nc, v_ubyte, &idx, &got_ubyte));
  ASSERT_EQ(NC_NOERR, nc_get_var1_ushort(nc, v_ushort, &idx, &got_ushort));
  ASSERT_EQ(NC_NOERR, nc_get_var1_uint(nc, v_uint, &idx, &got_uint));
  ASSERT_EQ(NC_NOERR, nc_get_var1_longlong(nc, v_int64, &idx, &got_ll));
  ASSERT_EQ(NC_NOERR, nc_get_var1_ulonglong(nc, v_uint64, &idx, &got_ull));
  ASSERT_EQ(NC_NOERR,
            nc_get_var1_string(nc, v_string, &idx, got_string.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var1_int(nc, v_scalar, nullptr, &got_scalar));

  EXPECT_EQ(got_untyped, untyped_val);
  EXPECT_EQ(got_text, text_val);
  EXPECT_EQ(got_schar, schar_val);
  EXPECT_EQ(got_uchar, uchar_val);
  EXPECT_EQ(got_short, short_val);
  EXPECT_EQ(got_int, int_val);
  EXPECT_EQ(got_long, long_val);
  EXPECT_EQ(got_float, float_val);
  EXPECT_EQ(got_double, double_val);
  EXPECT_EQ(got_ubyte, ubyte_val);
  EXPECT_EQ(got_ushort, ushort_val);
  EXPECT_EQ(got_uint, uint_val);
  EXPECT_EQ(got_ll, ll_val);
  EXPECT_EQ(got_ull, ull_val);
  EXPECT_STREQ(got_string[0], "single_string");
  EXPECT_EQ(got_scalar, scalar_val);
  got_string.clear();
}

TEST(DvarputTest, PutVarAll15VariantsAndOpaqueNetcdf4) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dvarput_var_all.nc", NC_NETCDF4, 4096, nc.ptr()));

  int dim_id = -1;
  nc_type opaque_type = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "n", 2, &dim_id));
  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc, 4, "op4_t", &opaque_type));

  int v_untyped = -1;
  int v_text = -1;
  int v_schar = -1;
  int v_uchar = -1;
  int v_short = -1;
  int v_int = -1;
  int v_long = -1;
  int v_float = -1;
  int v_double = -1;
  int v_ubyte = -1;
  int v_ushort = -1;
  int v_uint = -1;
  int v_int64 = -1;
  int v_uint64 = -1;
  int v_string = -1;
  int v_opaque = -1;

  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_untyped", NC_DOUBLE, 1, &dim_id, &v_untyped));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_text", NC_CHAR, 1, &dim_id, &v_text));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_schar", NC_BYTE, 1, &dim_id, &v_schar));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_uchar", NC_UBYTE, 1, &dim_id, &v_uchar));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_short", NC_SHORT, 1, &dim_id, &v_short));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_int", NC_INT, 1, &dim_id, &v_int));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_long", NC_INT64, 1, &dim_id, &v_long));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_float", NC_FLOAT, 1, &dim_id, &v_float));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_double", NC_DOUBLE, 1, &dim_id, &v_double));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_ubyte", NC_UBYTE, 1, &dim_id, &v_ubyte));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_ushort", NC_USHORT, 1, &dim_id, &v_ushort));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_uint", NC_UINT, 1, &dim_id, &v_uint));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_int64", NC_INT64, 1, &dim_id, &v_int64));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_uint64", NC_UINT64, 1, &dim_id, &v_uint64));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_string", NC_STRING, 1, &dim_id, &v_string));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_opaque", opaque_type, 1, &dim_id, &v_opaque));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));

  const std::array<double, 2> untyped_vals = {4.25, -8.5};
  const std::array<char, 2> text_vals = {'A', 'B'};
  const std::array<signed char, 2> schar_vals = {-120, 120};
  const std::array<unsigned char, 2> uchar_vals = {11, 222};
  const std::array<short, 2> short_vals = {-12345, 12345};
  const std::array<int, 2> int_vals = {-999999, 999999};
  const std::array<long, 2> long_vals = {-100001L, 100001L};
  const std::array<float, 2> float_vals = {0.5F, -0.75F};
  const std::array<double, 2> double_vals = {100.25, -200.5};
  const std::array<unsigned char, 2> ubyte_vals = {5, 250};
  const std::array<unsigned short, 2> ushort_vals = {1001, 60001};
  const std::array<unsigned int, 2> uint_vals = {42U, 4294967000U};
  const std::array<long long, 2> ll_vals = {-5000000000LL, 5000000000LL};
  const std::array<unsigned long long, 2> ull_vals = {7ULL, 70000000000ULL};
  std::array<const char*, 2> str_vals = {"first", "second"};
  const std::array<unsigned char, 8> opaque_vals = {1, 2, 3, 4, 5, 6, 7, 8};

  ASSERT_EQ(NC_NOERR, nc_put_var(nc, v_untyped, untyped_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_text(nc, v_text, text_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_schar(nc, v_schar, schar_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_uchar(nc, v_uchar, uchar_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_short(nc, v_short, short_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, v_int, int_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_long(nc, v_long, long_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_float(nc, v_float, float_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_double(nc, v_double, double_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_ubyte(nc, v_ubyte, ubyte_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_ushort(nc, v_ushort, ushort_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_uint(nc, v_uint, uint_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_longlong(nc, v_int64, ll_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_ulonglong(nc, v_uint64, ull_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_string(nc, v_string, str_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var(nc, v_opaque, opaque_vals.data()));

  std::array<double, 2> got_untyped{};
  std::array<char, 2> got_text{};
  std::array<signed char, 2> got_schar{};
  std::array<unsigned char, 2> got_uchar{};
  std::array<short, 2> got_short{};
  std::array<int, 2> got_int{};
  std::array<long, 2> got_long{};
  std::array<float, 2> got_float{};
  std::array<double, 2> got_double{};
  std::array<unsigned char, 2> got_ubyte{};
  std::array<unsigned short, 2> got_ushort{};
  std::array<unsigned int, 2> got_uint{};
  std::array<long long, 2> got_ll{};
  std::array<unsigned long long, 2> got_ull{};
  ScopedNcStrings got_strings(2);
  std::array<unsigned char, 8> got_opaque{};

  ASSERT_EQ(NC_NOERR, nc_get_var(nc, v_untyped, got_untyped.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_text(nc, v_text, got_text.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_schar(nc, v_schar, got_schar.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_uchar(nc, v_uchar, got_uchar.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_short(nc, v_short, got_short.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_int(nc, v_int, got_int.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_long(nc, v_long, got_long.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_float(nc, v_float, got_float.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_double(nc, v_double, got_double.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_ubyte(nc, v_ubyte, got_ubyte.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_ushort(nc, v_ushort, got_ushort.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_uint(nc, v_uint, got_uint.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_longlong(nc, v_int64, got_ll.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_ulonglong(nc, v_uint64, got_ull.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_string(nc, v_string, got_strings.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var(nc, v_opaque, got_opaque.data()));

  EXPECT_EQ(got_untyped, untyped_vals);
  EXPECT_EQ(got_text, text_vals);
  EXPECT_EQ(got_schar, schar_vals);
  EXPECT_EQ(got_uchar, uchar_vals);
  EXPECT_EQ(got_short, short_vals);
  EXPECT_EQ(got_int, int_vals);
  EXPECT_EQ(got_long, long_vals);
  EXPECT_EQ(got_float, float_vals);
  EXPECT_EQ(got_double, double_vals);
  EXPECT_EQ(got_ubyte, ubyte_vals);
  EXPECT_EQ(got_ushort, ushort_vals);
  EXPECT_EQ(got_uint, uint_vals);
  EXPECT_EQ(got_ll, ll_vals);
  EXPECT_EQ(got_ull, ull_vals);
  EXPECT_STREQ(got_strings[0], "first");
  EXPECT_STREQ(got_strings[1], "second");
  EXPECT_EQ(got_opaque, opaque_vals);
  got_strings.clear();
}

TEST(DvarputTest, PutVarsAll15VariantsNetcdf4) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dvarput_vars_all.nc", NC_NETCDF4, 4096, nc.ptr()));

  int dim_id = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "n", 6, &dim_id));

  int v_untyped = -1;
  int v_text = -1;
  int v_schar = -1;
  int v_uchar = -1;
  int v_short = -1;
  int v_int = -1;
  int v_long = -1;
  int v_float = -1;
  int v_double = -1;
  int v_ubyte = -1;
  int v_ushort = -1;
  int v_uint = -1;
  int v_int64 = -1;
  int v_uint64 = -1;
  int v_string = -1;

  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_untyped", NC_INT, 1, &dim_id, &v_untyped));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_text", NC_CHAR, 1, &dim_id, &v_text));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_schar", NC_BYTE, 1, &dim_id, &v_schar));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_uchar", NC_UBYTE, 1, &dim_id, &v_uchar));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_short", NC_SHORT, 1, &dim_id, &v_short));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_int", NC_INT, 1, &dim_id, &v_int));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_long", NC_INT64, 1, &dim_id, &v_long));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_float", NC_FLOAT, 1, &dim_id, &v_float));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_double", NC_DOUBLE, 1, &dim_id, &v_double));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_ubyte", NC_UBYTE, 1, &dim_id, &v_ubyte));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_ushort", NC_USHORT, 1, &dim_id, &v_ushort));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_uint", NC_UINT, 1, &dim_id, &v_uint));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_int64", NC_INT64, 1, &dim_id, &v_int64));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_uint64", NC_UINT64, 1, &dim_id, &v_uint64));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_string", NC_STRING, 1, &dim_id, &v_string));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));

  const size_t start = 1;
  const size_t count = 3;
  const ptrdiff_t stride = 2;

  const std::array<int, 3> untyped_vals = {11, 22, 33};
  const std::array<char, 3> text_vals = {'p', 'q', 'r'};
  const std::array<signed char, 3> schar_vals = {-5, 5, 15};
  const std::array<unsigned char, 3> uchar_vals = {10, 20, 30};
  const std::array<short, 3> short_vals = {-2, 4, -6};
  const std::array<int, 3> int_vals = {100, 200, 300};
  const std::array<long, 3> long_vals = {101L, 202L, 303L};
  const std::array<float, 3> float_vals = {1.0F, 2.0F, 3.0F};
  const std::array<double, 3> double_vals = {4.0, 5.0, 6.0};
  const std::array<unsigned char, 3> ubyte_vals = {1, 2, 3};
  const std::array<unsigned short, 3> ushort_vals = {111, 222, 333};
  const std::array<unsigned int, 3> uint_vals = {1000U, 2000U, 3000U};
  const std::array<long long, 3> ll_vals = {10LL, 20LL, 30LL};
  const std::array<unsigned long long, 3> ull_vals = {100ULL, 200ULL, 300ULL};
  std::array<const char*, 3> str_vals = {"s1", "s3", "s5"};

  ASSERT_EQ(NC_NOERR, nc_put_vars(nc, v_untyped, &start, &count, &stride,
                                  untyped_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vars_text(nc, v_text, &start, &count, &stride,
                                       text_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vars_schar(nc, v_schar, &start, &count, &stride,
                                        schar_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vars_uchar(nc, v_uchar, &start, &count, &stride,
                                        uchar_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vars_short(nc, v_short, &start, &count, &stride,
                                        short_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vars_int(nc, v_int, &start, &count, &stride,
                                      int_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vars_long(nc, v_long, &start, &count, &stride,
                                       long_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vars_float(nc, v_float, &start, &count, &stride,
                                        float_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vars_double(nc, v_double, &start, &count, &stride,
                                         double_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vars_ubyte(nc, v_ubyte, &start, &count, &stride,
                                        ubyte_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vars_ushort(nc, v_ushort, &start, &count, &stride,
                                         ushort_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vars_uint(nc, v_uint, &start, &count, &stride,
                                       uint_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vars_longlong(nc, v_int64, &start, &count, &stride,
                                           ll_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vars_ulonglong(nc, v_uint64, &start, &count,
                                            &stride, ull_vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_vars_string(nc, v_string, &start, &count, &stride,
                                         str_vals.data()));

  std::array<int, 3> got_untyped{};
  std::array<char, 3> got_text{};
  std::array<signed char, 3> got_schar{};
  std::array<unsigned char, 3> got_uchar{};
  std::array<short, 3> got_short{};
  std::array<int, 3> got_int{};
  std::array<long, 3> got_long{};
  std::array<float, 3> got_float{};
  std::array<double, 3> got_double{};
  std::array<unsigned char, 3> got_ubyte{};
  std::array<unsigned short, 3> got_ushort{};
  std::array<unsigned int, 3> got_uint{};
  std::array<long long, 3> got_ll{};
  std::array<unsigned long long, 3> got_ull{};
  ScopedNcStrings got_strings(3);

  ASSERT_EQ(NC_NOERR, nc_get_vars(nc, v_untyped, &start, &count, &stride,
                                  got_untyped.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vars_text(nc, v_text, &start, &count, &stride,
                                       got_text.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vars_schar(nc, v_schar, &start, &count, &stride,
                                        got_schar.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vars_uchar(nc, v_uchar, &start, &count, &stride,
                                        got_uchar.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vars_short(nc, v_short, &start, &count, &stride,
                                        got_short.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vars_int(nc, v_int, &start, &count, &stride,
                                      got_int.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vars_long(nc, v_long, &start, &count, &stride,
                                       got_long.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vars_float(nc, v_float, &start, &count, &stride,
                                        got_float.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vars_double(nc, v_double, &start, &count, &stride,
                                         got_double.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vars_ubyte(nc, v_ubyte, &start, &count, &stride,
                                        got_ubyte.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vars_ushort(nc, v_ushort, &start, &count, &stride,
                                         got_ushort.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vars_uint(nc, v_uint, &start, &count, &stride,
                                       got_uint.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vars_longlong(nc, v_int64, &start, &count, &stride,
                                           got_ll.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vars_ulonglong(nc, v_uint64, &start, &count,
                                            &stride, got_ull.data()));
  ASSERT_EQ(NC_NOERR, nc_get_vars_string(nc, v_string, &start, &count, &stride,
                                         got_strings.data()));

  EXPECT_EQ(got_untyped, untyped_vals);
  EXPECT_EQ(got_text, text_vals);
  EXPECT_EQ(got_schar, schar_vals);
  EXPECT_EQ(got_uchar, uchar_vals);
  EXPECT_EQ(got_short, short_vals);
  EXPECT_EQ(got_int, int_vals);
  EXPECT_EQ(got_long, long_vals);
  EXPECT_EQ(got_float, float_vals);
  EXPECT_EQ(got_double, double_vals);
  EXPECT_EQ(got_ubyte, ubyte_vals);
  EXPECT_EQ(got_ushort, ushort_vals);
  EXPECT_EQ(got_uint, uint_vals);
  EXPECT_EQ(got_ll, ll_vals);
  EXPECT_EQ(got_ull, ull_vals);
  EXPECT_STREQ(got_strings[0], "s1");
  EXPECT_STREQ(got_strings[1], "s3");
  EXPECT_STREQ(got_strings[2], "s5");
  got_strings.clear();
}

TEST(DvarputTest, PutVarmAll15VariantsNetcdf4Transposed) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dvarput_varm_all.nc", NC_NETCDF4, 4096, nc.ptr()));

  std::array<int, 2> dim_ids = {-1, -1};
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "r", 2, &dim_ids[0]));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "c", 3, &dim_ids[1]));

  int v_untyped = -1;
  int v_text = -1;
  int v_schar = -1;
  int v_uchar = -1;
  int v_short = -1;
  int v_int = -1;
  int v_long = -1;
  int v_float = -1;
  int v_double = -1;
  int v_ubyte = -1;
  int v_ushort = -1;
  int v_uint = -1;
  int v_int64 = -1;
  int v_uint64 = -1;
  int v_string = -1;

  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_untyped", NC_INT, 2, dim_ids.data(), &v_untyped));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_text", NC_CHAR, 2, dim_ids.data(), &v_text));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_schar", NC_BYTE, 2, dim_ids.data(), &v_schar));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_uchar", NC_UBYTE, 2, dim_ids.data(), &v_uchar));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_short", NC_SHORT, 2, dim_ids.data(), &v_short));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_int", NC_INT, 2, dim_ids.data(), &v_int));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_long", NC_INT64, 2, dim_ids.data(), &v_long));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_float", NC_FLOAT, 2, dim_ids.data(), &v_float));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_double", NC_DOUBLE, 2, dim_ids.data(),
                                 &v_double));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_ubyte", NC_UBYTE, 2, dim_ids.data(), &v_ubyte));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_ushort", NC_USHORT, 2, dim_ids.data(),
                                 &v_ushort));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_uint", NC_UINT, 2, dim_ids.data(), &v_uint));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "v_int64", NC_INT64, 2, dim_ids.data(), &v_int64));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_uint64", NC_UINT64, 2, dim_ids.data(),
                                 &v_uint64));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_string", NC_STRING, 2, dim_ids.data(),
                                 &v_string));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));

  // Column-major 3x2 memory layout written into row-major 2x3 netCDF variable
  // via imap = {1, 2}. One trailing initialized slot keeps internal odometer
  // pointer arithmetic within bounds before carry rewind.
  const std::array<size_t, 2> start = {0, 0};
  const std::array<size_t, 2> count = {2, 3};
  const std::array<ptrdiff_t, 2> stride = {1, 1};
  const std::array<ptrdiff_t, 2> imap = {1, 2};

  const std::array<int, 7> src_untyped = {1, 4, 2, 5, 3, 6, 0};
  const std::array<char, 7> src_text = {'a', 'd', 'b', 'e', 'c', 'f', '\0'};
  const std::array<signed char, 7> src_schar = {1, -4, 2, -5, 3, -6, 0};
  const std::array<unsigned char, 7> src_uchar = {10, 40, 20, 50, 30, 60, 0};
  const std::array<short, 7> src_short = {1, 4, 2, 5, 3, 6, 0};
  const std::array<int, 7> src_int = {10, 40, 20, 50, 30, 60, 0};
  const std::array<long, 7> src_long = {10L, 40L, 20L, 50L, 30L, 60L, 0L};
  const std::array<float, 7> src_float = {1.0F, 4.0F, 2.0F, 5.0F,
                                          3.0F, 6.0F, 0.0F};
  const std::array<double, 7> src_double = {1.0, 4.0, 2.0, 5.0, 3.0, 6.0, 0.0};
  const std::array<unsigned char, 7> src_ubyte = {1, 4, 2, 5, 3, 6, 0};
  const std::array<unsigned short, 7> src_ushort = {1, 4, 2, 5, 3, 6, 0};
  const std::array<unsigned int, 7> src_uint = {1U, 4U, 2U, 5U, 3U, 6U, 0U};
  const std::array<long long, 7> src_ll = {1LL, 4LL, 2LL, 5LL, 3LL, 6LL, 0LL};
  const std::array<unsigned long long, 7> src_ull = {1ULL, 4ULL, 2ULL, 5ULL,
                                                     3ULL, 6ULL, 0ULL};
  std::array<const char*, 7> src_str = {"r0c0", "r1c0", "r0c1", "r1c1",
                                        "r0c2", "r1c2", nullptr};

  ASSERT_EQ(NC_NOERR,
            nc_put_varm(nc, v_untyped, start.data(), count.data(),
                        stride.data(), imap.data(), src_untyped.data()));
  ASSERT_EQ(NC_NOERR,
            nc_put_varm_text(nc, v_text, start.data(), count.data(),
                             stride.data(), imap.data(), src_text.data()));
  ASSERT_EQ(NC_NOERR,
            nc_put_varm_schar(nc, v_schar, start.data(), count.data(),
                              stride.data(), imap.data(), src_schar.data()));
  ASSERT_EQ(NC_NOERR,
            nc_put_varm_uchar(nc, v_uchar, start.data(), count.data(),
                              stride.data(), imap.data(), src_uchar.data()));
  ASSERT_EQ(NC_NOERR,
            nc_put_varm_short(nc, v_short, start.data(), count.data(),
                              stride.data(), imap.data(), src_short.data()));
  ASSERT_EQ(NC_NOERR,
            nc_put_varm_int(nc, v_int, start.data(), count.data(),
                            stride.data(), imap.data(), src_int.data()));
  ASSERT_EQ(NC_NOERR,
            nc_put_varm_long(nc, v_long, start.data(), count.data(),
                             stride.data(), imap.data(), src_long.data()));
  ASSERT_EQ(NC_NOERR,
            nc_put_varm_float(nc, v_float, start.data(), count.data(),
                              stride.data(), imap.data(), src_float.data()));
  ASSERT_EQ(NC_NOERR,
            nc_put_varm_double(nc, v_double, start.data(), count.data(),
                               stride.data(), imap.data(), src_double.data()));
  ASSERT_EQ(NC_NOERR,
            nc_put_varm_ubyte(nc, v_ubyte, start.data(), count.data(),
                              stride.data(), imap.data(), src_ubyte.data()));
  ASSERT_EQ(NC_NOERR,
            nc_put_varm_ushort(nc, v_ushort, start.data(), count.data(),
                               stride.data(), imap.data(), src_ushort.data()));
  ASSERT_EQ(NC_NOERR,
            nc_put_varm_uint(nc, v_uint, start.data(), count.data(),
                             stride.data(), imap.data(), src_uint.data()));
  ASSERT_EQ(NC_NOERR,
            nc_put_varm_longlong(nc, v_int64, start.data(), count.data(),
                                 stride.data(), imap.data(), src_ll.data()));
  ASSERT_EQ(NC_NOERR,
            nc_put_varm_ulonglong(nc, v_uint64, start.data(), count.data(),
                                  stride.data(), imap.data(), src_ull.data()));
  ASSERT_EQ(NC_NOERR,
            nc_put_varm_string(nc, v_string, start.data(), count.data(),
                               stride.data(), imap.data(), src_str.data()));

  std::array<int, 6> got_untyped{};
  std::array<char, 6> got_text{};
  std::array<signed char, 6> got_schar{};
  std::array<unsigned char, 6> got_uchar{};
  std::array<short, 6> got_short{};
  std::array<int, 6> got_int{};
  std::array<long, 6> got_long{};
  std::array<float, 6> got_float{};
  std::array<double, 6> got_double{};
  std::array<unsigned char, 6> got_ubyte{};
  std::array<unsigned short, 6> got_ushort{};
  std::array<unsigned int, 6> got_uint{};
  std::array<long long, 6> got_ll{};
  std::array<unsigned long long, 6> got_ull{};
  ScopedNcStrings got_strings(6);

  ASSERT_EQ(NC_NOERR, nc_get_var(nc, v_untyped, got_untyped.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_text(nc, v_text, got_text.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_schar(nc, v_schar, got_schar.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_uchar(nc, v_uchar, got_uchar.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_short(nc, v_short, got_short.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_int(nc, v_int, got_int.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_long(nc, v_long, got_long.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_float(nc, v_float, got_float.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_double(nc, v_double, got_double.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_ubyte(nc, v_ubyte, got_ubyte.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_ushort(nc, v_ushort, got_ushort.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_uint(nc, v_uint, got_uint.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_longlong(nc, v_int64, got_ll.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_ulonglong(nc, v_uint64, got_ull.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_string(nc, v_string, got_strings.data()));

  EXPECT_THAT(got_untyped, ElementsAre(1, 2, 3, 4, 5, 6));
  EXPECT_THAT(got_text, ElementsAre('a', 'b', 'c', 'd', 'e', 'f'));
  EXPECT_THAT(got_schar, ElementsAre(1, 2, 3, -4, -5, -6));
  EXPECT_THAT(got_uchar, ElementsAre(10, 20, 30, 40, 50, 60));
  EXPECT_THAT(got_short, ElementsAre(1, 2, 3, 4, 5, 6));
  EXPECT_THAT(got_int, ElementsAre(10, 20, 30, 40, 50, 60));
  EXPECT_THAT(got_long, ElementsAre(10L, 20L, 30L, 40L, 50L, 60L));
  EXPECT_THAT(got_float, ElementsAre(1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F));
  EXPECT_THAT(got_double, ElementsAre(1.0, 2.0, 3.0, 4.0, 5.0, 6.0));
  EXPECT_THAT(got_ubyte, ElementsAre(1, 2, 3, 4, 5, 6));
  EXPECT_THAT(got_ushort, ElementsAre(1, 2, 3, 4, 5, 6));
  EXPECT_THAT(got_uint, ElementsAre(1U, 2U, 3U, 4U, 5U, 6U));
  EXPECT_THAT(got_ll, ElementsAre(1LL, 2LL, 3LL, 4LL, 5LL, 6LL));
  EXPECT_THAT(got_ull, ElementsAre(1ULL, 2ULL, 3ULL, 4ULL, 5ULL, 6ULL));
  EXPECT_STREQ(got_strings[0], "r0c0");
  EXPECT_STREQ(got_strings[1], "r0c1");
  EXPECT_STREQ(got_strings[2], "r0c2");
  EXPECT_STREQ(got_strings[3], "r1c0");
  EXPECT_STREQ(got_strings[4], "r1c1");
  EXPECT_STREQ(got_strings[5], "r1c2");
  got_strings.clear();
}

TEST(DvarputTest, PutVarsDefaultOdometerClassicAndCdf5Formats) {
  // Exercise NCDEFAULT_put_vars odometer carries, scalar fast path, stride==1
  // fast path, zero-count early return, record extension, and CDF5 types.
  for (const int mode : {0, NC_64BIT_OFFSET, NC_CDF5}) {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("dvarput_vars_nc3.nc", mode, 4096, nc.ptr()));

    int d_time = -1;
    int d_x = -1;
    int d_y = -1;
    int d_z = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "time", NC_UNLIMITED, &d_time));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 4, &d_x));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "y", 4, &d_y));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "z", 4, &d_z));

    const std::array<int, 3> dims3 = {d_x, d_y, d_z};
    const std::array<int, 2> rec_dims = {d_time, d_x};
    int v_cube = -1;
    int v_scalar = -1;
    int v_rec = -1;
    int v_dbl = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "cube", NC_INT, 3, dims3.data(), &v_cube));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "scalar", NC_INT, 0, nullptr, &v_scalar));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "rec", NC_INT, 2, rec_dims.data(), &v_rec));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "dbl", NC_DOUBLE, 1, &d_x, &v_dbl));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    // Pre-fill 4x4x4 cube with sentinel -1.
    std::array<int, 64> cube_init;
    cube_init.fill(-1);
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, v_cube, cube_init.data()));

    // 3D non-unit stride odometer: exercises inner resets and dim-0 overflow.
    const std::array<size_t, 3> s3 = {1, 0, 1};
    const std::array<size_t, 3> c3 = {2, 2, 2};
    const std::array<ptrdiff_t, 3> st3 = {2, 2, 2};
    const std::array<int, 8> put3 = {10, 11, 12, 13, 14, 15, 16, 17};
    ASSERT_EQ(NC_NOERR, nc_put_vars_int(nc, v_cube, s3.data(), c3.data(),
                                        st3.data(), put3.data()));

    std::array<int, 64> got_cube{};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(nc, v_cube, got_cube.data()));
    size_t idx3 = 0;
    for (size_t i = 0; i < 4; ++i) {
      for (size_t j = 0; j < 4; ++j) {
        for (size_t k = 0; k < 4; ++k) {
          const size_t flat = (i * 16) + (j * 4) + k;
          const bool selected =
              (i == 1 || i == 3) && (j == 0 || j == 2) && (k == 1 || k == 3);
          if (selected) {
            EXPECT_EQ(got_cube[flat], put3[idx3++]);
          } else {
            EXPECT_EQ(got_cube[flat], -1);
          }
        }
      }
    }

    // Non-unit stride with nels == 0 returns NC_NOERR and modifies nothing.
    const std::array<size_t, 3> c3_zero = {2, 0, 2};
    const int dummy_val = 999;
    ASSERT_EQ(NC_NOERR, nc_put_vars_int(nc, v_cube, s3.data(), c3_zero.data(),
                                        st3.data(), &dummy_val));
    std::array<int, 64> got_cube_after_zero{};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(nc, v_cube, got_cube_after_zero.data()));
    EXPECT_EQ(got_cube_after_zero, got_cube);

    // Classic scalar variable with all NULL vectors hits rank == 0 branch.
    const int scalar_v = 55;
    ASSERT_EQ(NC_NOERR, nc_put_vars_int(nc, v_scalar, nullptr, nullptr, nullptr,
                                        &scalar_v));
    int got_scalar = 0;
    ASSERT_EQ(NC_NOERR, nc_get_var_int(nc, v_scalar, &got_scalar));
    EXPECT_EQ(got_scalar, scalar_v);

    // Unit stride fast path + NC_NAT untyped call + numeric conversion.
    const size_t s1 = 0;
    const size_t c1 = 4;
    const ptrdiff_t st1 = 1;
    const std::array<double, 4> dvals = {1.25, 2.5, 3.75, 4.5};
    ASSERT_EQ(NC_NOERR, nc_put_vars(nc, v_dbl, &s1, &c1, &st1, dvals.data()));
    std::array<double, 4> got_dvals{};
    ASSERT_EQ(NC_NOERR, nc_get_var_double(nc, v_dbl, got_dvals.data()));
    EXPECT_EQ(got_dvals, dvals);

    // Unlimited record extension via strided nc_put_vars_int.
    const std::array<size_t, 2> rstart = {0, 1};
    const std::array<size_t, 2> rcount = {3, 2};
    const std::array<ptrdiff_t, 2> rstride = {2, 2};
    const std::array<int, 6> rvals = {101, 102, 201, 202, 301, 302};
    ASSERT_EQ(NC_NOERR, nc_put_vars_int(nc, v_rec, rstart.data(), rcount.data(),
                                        rstride.data(), rvals.data()));
    size_t numrecs = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_dimlen(nc, d_time, &numrecs));
    EXPECT_EQ(numrecs, size_t{5});
    std::array<int, 6> got_rvals{};
    ASSERT_EQ(NC_NOERR, nc_get_vars_int(nc, v_rec, rstart.data(), rcount.data(),
                                        rstride.data(), got_rvals.data()));
    EXPECT_EQ(got_rvals, rvals);
  }

  // Native unsigned & 64-bit CDF-5 variable types through NCDEFAULT_put_vars.
  ScopedNcFile cdf5;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dvarput_vars_cdf5.nc", NC_CDF5, 4096, cdf5.ptr()));
  int d_n = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(cdf5, "n", 4, &d_n));
  int vu8 = -1;
  int vu16 = -1;
  int vu32 = -1;
  int vi64 = -1;
  int vu64 = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(cdf5, "u8", NC_UBYTE, 1, &d_n, &vu8));
  ASSERT_EQ(NC_NOERR, nc_def_var(cdf5, "u16", NC_USHORT, 1, &d_n, &vu16));
  ASSERT_EQ(NC_NOERR, nc_def_var(cdf5, "u32", NC_UINT, 1, &d_n, &vu32));
  ASSERT_EQ(NC_NOERR, nc_def_var(cdf5, "i64", NC_INT64, 1, &d_n, &vi64));
  ASSERT_EQ(NC_NOERR, nc_def_var(cdf5, "u64", NC_UINT64, 1, &d_n, &vu64));
  ASSERT_EQ(NC_NOERR, nc_enddef(cdf5));

  const size_t s0 = 0;
  const size_t c2 = 2;
  const ptrdiff_t st2 = 2;
  const std::array<unsigned char, 2> u8v = {11, 22};
  const std::array<unsigned short, 2> u16v = {111, 222};
  const std::array<unsigned int, 2> u32v = {1111U, 2222U};
  const std::array<long long, 2> i64v = {-11111LL, 22222LL};
  const std::array<unsigned long long, 2> u64v = {111111ULL, 222222ULL};
  ASSERT_EQ(NC_NOERR, nc_put_vars_ubyte(cdf5, vu8, &s0, &c2, &st2, u8v.data()));
  ASSERT_EQ(NC_NOERR,
            nc_put_vars_ushort(cdf5, vu16, &s0, &c2, &st2, u16v.data()));
  ASSERT_EQ(NC_NOERR,
            nc_put_vars_uint(cdf5, vu32, &s0, &c2, &st2, u32v.data()));
  ASSERT_EQ(NC_NOERR,
            nc_put_vars_longlong(cdf5, vi64, &s0, &c2, &st2, i64v.data()));
  ASSERT_EQ(NC_NOERR,
            nc_put_vars_ulonglong(cdf5, vu64, &s0, &c2, &st2, u64v.data()));

  std::array<unsigned char, 2> gu8{};
  std::array<unsigned short, 2> gu16{};
  std::array<unsigned int, 2> gu32{};
  std::array<long long, 2> gi64{};
  std::array<unsigned long long, 2> gu64{};
  ASSERT_EQ(NC_NOERR, nc_get_vars_ubyte(cdf5, vu8, &s0, &c2, &st2, gu8.data()));
  ASSERT_EQ(NC_NOERR,
            nc_get_vars_ushort(cdf5, vu16, &s0, &c2, &st2, gu16.data()));
  ASSERT_EQ(NC_NOERR,
            nc_get_vars_uint(cdf5, vu32, &s0, &c2, &st2, gu32.data()));
  ASSERT_EQ(NC_NOERR,
            nc_get_vars_longlong(cdf5, vi64, &s0, &c2, &st2, gi64.data()));
  ASSERT_EQ(NC_NOERR,
            nc_get_vars_ulonglong(cdf5, vu64, &s0, &c2, &st2, gu64.data()));
  EXPECT_EQ(gu8, u8v);
  EXPECT_EQ(gu16, u16v);
  EXPECT_EQ(gu32, u32v);
  EXPECT_EQ(gi64, i64v);
  EXPECT_EQ(gu64, u64v);
}

TEST(DvarputTest, PutVarmDefaultOdometerFastPathsAndFormats) {
  for (const int mode : {0, NC_64BIT_OFFSET, NC_CDF5}) {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("dvarput_varm_nc3.nc", mode, 4096, nc.ptr()));

    int d_time = -1;
    int d0 = -1;
    int d1 = -1;
    int d2 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "time", NC_UNLIMITED, &d_time));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d0", 4, &d0));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d1", 3, &d1));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d2", 2, &d2));

    const std::array<int, 2> dims2 = {d2, d1};
    const std::array<int, 3> dims3 = {d2, d1, d2};
    const std::array<int, 2> dims_strided = {d0, d0};
    const std::array<int, 2> dims_rec = {d_time, d1};
    int v2 = -1;
    int v3 = -1;
    int v_strided = -1;
    int v_scalar = -1;
    int v_rec = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v2", NC_INT, 2, dims2.data(), &v2));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v3", NC_INT, 3, dims3.data(), &v3));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_strided", NC_INT, 2,
                                   dims_strided.data(), &v_strided));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "v_scalar", NC_INT, 0, nullptr, &v_scalar));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "v_rec", NC_INT, 2, dims_rec.data(), &v_rec));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    // 1. stride == 1 && imap == nullptr hits fast path to NC_put_vara.
    const std::array<size_t, 2> s2 = {0, 0};
    const std::array<size_t, 2> c2 = {2, 3};
    const std::array<ptrdiff_t, 2> st1 = {1, 1};
    const std::array<int, 6> fast_vals = {10, 20, 30, 40, 50, 60};
    ASSERT_EQ(NC_NOERR, nc_put_varm(nc, v2, s2.data(), c2.data(), st1.data(),
                                    nullptr, fast_vals.data()));
    std::array<int, 6> got2{};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(nc, v2, got2.data()));
    EXPECT_EQ(got2, fast_vals);

    // 2. edges[idim] == 0 with non-NULL imap returns NC_NOERR early via done.
    const std::array<size_t, 2> c2_zero = {2, 0};
    const std::array<ptrdiff_t, 2> imap_t = {1, 2};
    const int untouched_dummy = 999;
    ASSERT_EQ(NC_NOERR,
              nc_put_varm_int(nc, v2, s2.data(), c2_zero.data(), st1.data(),
                              imap_t.data(), &untouched_dummy));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(nc, v2, got2.data()));
    EXPECT_EQ(got2, fast_vals);

    // 3. start[idim] == varshape[idim] && edges[idim] == 0 succeeds cleanly.
    const std::array<size_t, 2> s2_at_end = {2, 0};
    const std::array<size_t, 2> c2_at_end = {0, 3};
    ASSERT_EQ(NC_NOERR,
              nc_put_varm_int(nc, v2, s2_at_end.data(), c2_at_end.data(),
                              st1.data(), imap_t.data(), &untouched_dummy));

    // 4. Non-unit stride + imapp == nullptr triggers fallback mymap & coalesced
    // fastest-dimension optimization (mystride[maxidim] == 1 && mymap == 1).
    std::array<int, 16> grid_init;
    grid_init.fill(-1);
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, v_strided, grid_init.data()));
    const std::array<size_t, 2> cs = {2, 3};
    const std::array<ptrdiff_t, 2> sts = {2, 1};
    const std::array<int, 6> row_vals = {1, 2, 3, 4, 5, 6};
    ASSERT_EQ(NC_NOERR, nc_put_varm_int(nc, v_strided, s2.data(), cs.data(),
                                        sts.data(), nullptr, row_vals.data()));
    std::array<int, 16> got_grid{};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(nc, v_strided, got_grid.data()));
    EXPECT_THAT(got_grid, ElementsAre(1, 2, 3, -1, -1, -1, -1, -1,  //
                                      4, 5, 6, -1, -1, -1, -1, -1));

    // 5. Explicitly coalesced imap = {3, 1} with stride = {1, 1}.
    const std::array<ptrdiff_t, 2> imap_row_major = {3, 1};
    const std::array<int, 6> row_vals2 = {7, 8, 9, 10, 11, 12};
    ASSERT_EQ(NC_NOERR,
              nc_put_varm_int(nc, v2, s2.data(), c2.data(), st1.data(),
                              imap_row_major.data(), row_vals2.data()));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(nc, v2, got2.data()));
    EXPECT_EQ(got2, row_vals2);

    // 6. 3D transposed odometer (carry loop across multiple dimensions).
    const std::array<size_t, 3> s3 = {0, 0, 0};
    const std::array<size_t, 3> c3 = {2, 3, 2};
    const std::array<ptrdiff_t, 3> st3 = {1, 1, 1};
    const std::array<ptrdiff_t, 3> imap3 = {1, 2, 6};
    // Memory layout indexed by [k][j][i] for i<2, j<3, k<2 (+6 pad slots so
    // `value + mymap[2]` = `data + 17` stays within [data, data + size]).
    const std::array<int, 17> src3 = {
        100, 200,  // k=0,j=0,i=0..1
        110, 210,  // k=0,j=1,i=0..1
        120, 220,  // k=0,j=2,i=0..1
        101, 201,  // k=1,j=0,i=0..1
        111, 211,  // k=1,j=1,i=0..1
        121, 221,  // k=1,j=2,i=0..1
        0,   0,   0, 0, 0,
    };
    ASSERT_EQ(NC_NOERR, nc_put_varm_int(nc, v3, s3.data(), c3.data(),
                                        st3.data(), imap3.data(), src3.data()));
    std::array<int, 12> got3{};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(nc, v3, got3.data()));
    EXPECT_THAT(got3, ElementsAre(100, 101, 110, 111, 120, 121,  //
                                  200, 201, 210, 211, 220, 221));

    // 7. Classic scalar var via nc_put_varm_int with NULLs.
    const int sv = 88;
    ASSERT_EQ(NC_NOERR, nc_put_varm_int(nc, v_scalar, nullptr, nullptr, nullptr,
                                        nullptr, &sv));
    int got_sv = 0;
    ASSERT_EQ(NC_NOERR, nc_get_var_int(nc, v_scalar, &got_sv));
    EXPECT_EQ(got_sv, sv);

    // 8. Record variable write extending unlimited dimension through varm.
    const std::array<size_t, 2> sr = {2, 0};
    const std::array<size_t, 2> cr = {1, 3};
    const std::array<int, 3> rdata = {71, 72, 73};
    ASSERT_EQ(NC_NOERR,
              nc_put_varm_int(nc, v_rec, sr.data(), cr.data(), st1.data(),
                              imap_row_major.data(), rdata.data()));
    size_t nrecs = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_dimlen(nc, d_time, &nrecs));
    EXPECT_EQ(nrecs, size_t{3});
    std::array<int, 3> got_r{};
    ASSERT_EQ(NC_NOERR,
              nc_get_vara_int(nc, v_rec, sr.data(), cr.data(), got_r.data()));
    EXPECT_EQ(got_r, rdata);
  }
}

TEST(DvarputTest, NullCoordinatesCountAndStrideHandlingPublicApi) {
  for (const int mode : {0, NC_NETCDF4}) {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("dvarput_nulls.nc", mode, 4096, nc.ptr()));

    std::array<int, 2> dims = {-1, -1};
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "r", 2, &dims[0]));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "c", 3, &dims[1]));

    int v1d = -1;
    int v2d = -1;
    int vscalar = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v1d", NC_INT, 1, &dims[0], &v1d));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v2d", NC_INT, 2, dims.data(), &v2d));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "vscalar", NC_INT, 0, nullptr, &vscalar));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    // start == nullptr on non-scalar fails in NC_check_nulls.
    const std::array<size_t, 2> cnt2 = {2, 3};
    const std::array<ptrdiff_t, 2> st2 = {1, 1};
    const std::array<int, 6> src6 = {1, 2, 3, 4, 5, 6};
    EXPECT_EQ(NC_EINVALCOORDS,
              nc_put_vara_int(nc, v1d, nullptr, cnt2.data(), src6.data()));
    EXPECT_EQ(NC_EINVALCOORDS, nc_put_var1_int(nc, v1d, nullptr, src6.data()));
    EXPECT_EQ(NC_EINVALCOORDS, nc_put_vars_int(nc, v1d, nullptr, cnt2.data(),
                                               st2.data(), src6.data()));
    EXPECT_EQ(NC_EINVALCOORDS,
              nc_put_varm_int(nc, v1d, nullptr, cnt2.data(), st2.data(),
                              nullptr, src6.data()));

    // Invalid varid caught inside NC_check_nulls when edges/stride is nullptr.
    const std::array<size_t, 2> start0 = {0, 0};
    EXPECT_EQ(NC_ENOTVAR,
              nc_put_vara_int(nc, 999, start0.data(), nullptr, src6.data()));
    EXPECT_EQ(NC_ENOTVAR, nc_put_vars_int(nc, 999, start0.data(), nullptr,
                                          nullptr, src6.data()));
    EXPECT_EQ(NC_ENOTVAR, nc_put_varm_int(nc, 999, start0.data(), nullptr,
                                          nullptr, nullptr, src6.data()));

    // edges == nullptr allocates my_count via NC_check_nulls and frees it.
    ASSERT_EQ(NC_NOERR,
              nc_put_vara_int(nc, v2d, start0.data(), nullptr, src6.data()));
    std::array<int, 6> got6{};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(nc, v2d, got6.data()));
    EXPECT_EQ(got6, src6);

    // edges == nullptr & stride == nullptr in nc_put_vars_int and nc_put_varm.
    const std::array<int, 6> src6_b = {11, 22, 33, 44, 55, 66};
    ASSERT_EQ(NC_NOERR, nc_put_vars_int(nc, v2d, start0.data(), nullptr,
                                        nullptr, src6_b.data()));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(nc, v2d, got6.data()));
    EXPECT_EQ(got6, src6_b);

    const std::array<ptrdiff_t, 2> imap_t = {1, 2};
    const std::array<int, 7> src6_t = {1, 4, 2, 5, 3, 6, 0};
    ASSERT_EQ(NC_NOERR, nc_put_varm_int(nc, v2d, start0.data(), nullptr,
                                        nullptr, imap_t.data(), src6_t.data()));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(nc, v2d, got6.data()));
    EXPECT_THAT(got6, ElementsAre(1, 2, 3, 4, 5, 6));

    // Non-NULL edges with stride == nullptr only allocates/frees my_stride.
    const std::array<int, 6> src6_c = {21, 22, 23, 24, 25, 26};
    ASSERT_EQ(NC_NOERR, nc_put_vars_int(nc, v2d, start0.data(), cnt2.data(),
                                        nullptr, src6_c.data()));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(nc, v2d, got6.data()));
    EXPECT_EQ(got6, src6_c);

    const std::array<int, 7> src6_t2 = {31, 34, 32, 35, 33, 36, 0};
    ASSERT_EQ(NC_NOERR,
              nc_put_varm_int(nc, v2d, start0.data(), cnt2.data(), nullptr,
                              imap_t.data(), src6_t2.data()));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(nc, v2d, got6.data()));
    EXPECT_THAT(got6, ElementsAre(31, 32, 33, 34, 35, 36));

    // Scalar variable accepts nullptr for start/edges/stride/imap.
    const int sv = 707;
    ASSERT_EQ(NC_NOERR, nc_put_vara_int(nc, vscalar, nullptr, nullptr, &sv));
    int got_sv = 0;
    ASSERT_EQ(NC_NOERR, nc_get_var_int(nc, vscalar, &got_sv));
    EXPECT_EQ(got_sv, sv);
  }
}

TEST(DvarputTest, ErrorPathsValidationDefineReadOnlyAndErangePriority) {
  const size_t z = 0;
  const size_t o = 1;
  const ptrdiff_t p = 1;
  int dummy = 0;

  // 1. Bad ncid across public entry points.
  EXPECT_EQ(NC_EBADID, nc_put_vara(-1, 0, &z, &o, &dummy));
  EXPECT_EQ(NC_EBADID, nc_put_vara_int(-1, 0, &z, &o, &dummy));
  EXPECT_EQ(NC_EBADID, nc_put_var1_int(-1, 0, &z, &dummy));
  EXPECT_EQ(NC_EBADID, nc_put_var_int(-1, 0, &dummy));
  EXPECT_EQ(NC_EBADID, nc_put_vars_int(-1, 0, &z, &o, &p, &dummy));
  EXPECT_EQ(NC_EBADID, nc_put_varm_int(-1, 0, &z, &o, &p, &p, &dummy));

  // 2. Classic define mode (NC_EINDEFINE), invalid varid (NC_ENOTVAR), bounds,
  //    strides, ECHAR, and ERANGE checks.
  ScopedMemio memio;
  ScopedNcFile nc3;
  ASSERT_EQ(NC_NOERR, nc_create_mem("dvarput_err3.nc", 0, 4096, nc3.ptr()));

  int dim_id = -1;
  int v_byte4 = -1;
  int v_byte6 = -1;
  int v_int = -1;
  int v_char = -1;
  int dim6_id = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc3, "d4", 4, &dim_id));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc3, "d6", 6, &dim6_id));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc3, "vb4", NC_BYTE, 1, &dim_id, &v_byte4));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc3, "vb6", NC_BYTE, 1, &dim6_id, &v_byte6));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc3, "vi", NC_INT, 1, &dim_id, &v_int));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc3, "vc", NC_CHAR, 1, &dim_id, &v_char));

  // Invalid varid while non-null start/count/stride bypasses NC_check_nulls.
  EXPECT_EQ(NC_ENOTVAR, nc_put_vara(nc3, 999, &z, &o, &dummy));
  EXPECT_EQ(NC_ENOTVAR, nc_put_var_int(nc3, 999, &dummy));
  EXPECT_EQ(NC_ENOTVAR, nc_put_vars_int(nc3, 999, &z, &o, &p, &dummy));
  EXPECT_EQ(NC_ENOTVAR, nc_put_varm_int(nc3, 999, &z, &o, &p, &p, &dummy));

  // In define mode: NC_EINDEFINE.
  EXPECT_EQ(NC_EINDEFINE, nc_put_vara_int(nc3, v_int, &z, &o, &dummy));
  EXPECT_EQ(NC_EINDEFINE, nc_put_var1_int(nc3, v_int, &z, &dummy));
  EXPECT_EQ(NC_EINDEFINE, nc_put_var_int(nc3, v_int, &dummy));
  EXPECT_EQ(NC_EINDEFINE, nc_put_vars_int(nc3, v_int, &z, &o, &p, &dummy));
  EXPECT_EQ(NC_EINDEFINE, nc_put_varm_int(nc3, v_int, &z, &o, &p, &p, &dummy));

  ASSERT_EQ(NC_NOERR, nc_enddef(nc3));

  // ECHAR checks in NCDEFAULT_put_vars & NCDEFAULT_put_varm.
  const char ch = 'A';
  EXPECT_EQ(NC_ECHAR, nc_put_vars_text(nc3, v_int, &z, &o, &p, &ch));
  EXPECT_EQ(NC_ECHAR, nc_put_vars_int(nc3, v_char, &z, &o, &p, &dummy));
  EXPECT_EQ(NC_ECHAR, nc_put_varm_text(nc3, v_int, &z, &o, &p, &p, &ch));
  EXPECT_EQ(NC_ECHAR, nc_put_varm_int(nc3, v_char, &z, &o, &p, &p, &dummy));

  // NC_EINVALCOORDS and NC_EEDGE in NCDEFAULT_put_vars.
  const size_t start_gt = 5;
  const size_t start_eq = 4;
  const size_t count_one = 1;
  const size_t count_zero = 0;
  const size_t count_over = 5;
  EXPECT_EQ(NC_EINVALCOORDS,
            nc_put_vars_int(nc3, v_int, &start_gt, &o, &p, &dummy));
  EXPECT_EQ(NC_EINVALCOORDS,
            nc_put_vars_int(nc3, v_int, &start_eq, &count_one, &p, &dummy));
  EXPECT_EQ(NC_NOERR,
            nc_put_vars_int(nc3, v_int, &start_eq, &count_zero, &p, &dummy));
  EXPECT_EQ(NC_EEDGE, nc_put_vars_int(nc3, v_int, &z, &count_over, &p, &dummy));

  // NC_EINVALCOORDS and NC_EEDGE in NCDEFAULT_put_varm (non-NULL imap).
  EXPECT_EQ(NC_EINVALCOORDS,
            nc_put_varm_int(nc3, v_int, &start_gt, &o, &p, &p, &dummy));
  EXPECT_EQ(NC_EINVALCOORDS,
            nc_put_varm_int(nc3, v_int, &start_eq, &count_one, &p, &p, &dummy));
  EXPECT_EQ(NC_EEDGE,
            nc_put_varm_int(nc3, v_int, &z, &count_over, &p, &p, &dummy));

  // NC_ESTRIDE in NCDEFAULT_put_vars and NCDEFAULT_put_varm.
  for (const ptrdiff_t bad_stride :
       {ptrdiff_t{0}, ptrdiff_t{-1}, static_cast<ptrdiff_t>(X_INT_MAX)}) {
    EXPECT_EQ(NC_ESTRIDE,
              nc_put_vars_int(nc3, v_int, &z, &o, &bad_stride, &dummy));
    EXPECT_EQ(NC_ESTRIDE,
              nc_put_varm_int(nc3, v_int, &z, &o, &bad_stride, &p, &dummy));
  }

  // NC_ERANGE preserved across multiple elements (integer->schar avoids float
  // cast overflow UB): out-of-range + out-of-range + in-range => NC_ERANGE.
  const std::array<int, 3> erange_ok_bounds = {300, 400, 5};
  const size_t c3 = 3;
  const ptrdiff_t st2 = 2;
  EXPECT_EQ(NC_ERANGE, nc_put_vars_int(nc3, v_byte6, &z, &c3, &st2,
                                       erange_ok_bounds.data()));
  EXPECT_EQ(NC_ERANGE, nc_put_varm_int(nc3, v_byte6, &z, &c3, &st2, &p,
                                       erange_ok_bounds.data()));

  // NC_ERANGE overwritten by subsequent non-ERANGE error (NC_EINVALCOORDS when
  // strided step hits index 4 >= dimlen 4) in both put_vars and put_varm.
  const std::array<int, 3> erange_then_oob = {300, 400, 500};
  EXPECT_EQ(NC_EINVALCOORDS, nc_put_vars_int(nc3, v_byte4, &z, &c3, &st2,
                                             erange_then_oob.data()));
  EXPECT_EQ(NC_EINVALCOORDS, nc_put_varm_int(nc3, v_byte4, &z, &c3, &st2, &p,
                                             erange_then_oob.data()));

  // Write valid data, close to memio, reopen NC_NOWRITE => NC_EPERM.
  const std::array<int, 4> valid4 = {1, 2, 3, 4};
  ASSERT_EQ(NC_NOERR, nc_put_var_int(nc3, v_int, valid4.data()));
  ASSERT_EQ(NC_NOERR, nc_close_memio(nc3.release(), memio.get()));

  ScopedNcFile ro_nc;
  ASSERT_EQ(NC_NOERR,
            nc_open_mem("dvarput_err3_ro.nc", NC_NOWRITE, memio.size(),
                        memio.get()->memory, ro_nc.ptr()));
  int ro_v_int = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "vi", &ro_v_int));
  EXPECT_EQ(NC_EPERM, nc_put_vara_int(ro_nc, ro_v_int, &z, &o, &dummy));
  EXPECT_EQ(NC_EPERM, nc_put_var1_int(ro_nc, ro_v_int, &z, &dummy));
  EXPECT_EQ(NC_EPERM, nc_put_var_int(ro_nc, ro_v_int, valid4.data()));
  EXPECT_EQ(NC_EPERM, nc_put_vars_int(ro_nc, ro_v_int, &z, &o, &p, &dummy));
  EXPECT_EQ(NC_EPERM, nc_put_varm_int(ro_nc, ro_v_int, &z, &o, &p, &p, &dummy));

  // 3. NetCDF-4 NC_EMAPTYPE (nc_put_varm on opaque & enum), stride <= 0, and
  //    NC_CLASSIC_MODEL define mode NC_EINDEFINE.
  ScopedNcFile nc4;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dvarput_err4.nc", NC_NETCDF4, 4096, nc4.ptr()));
  int d4 = -1;
  nc_type op_t = NC_NAT;
  nc_type en_t = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc4, "d", 4, &d4));
  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc4, 4, "op_t", &op_t));
  ASSERT_EQ(NC_NOERR, nc_def_enum(nc4, NC_INT, "en_t", &en_t));
  const int enum_member = 1;
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc4, en_t, "ONE", &enum_member));
  int v_op = -1;
  int v_en = -1;
  int v_i4 = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(nc4, "v_op", op_t, 1, &d4, &v_op));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc4, "v_en", en_t, 1, &d4, &v_en));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc4, "v_i4", NC_INT, 1, &d4, &v_i4));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc4));

  const std::array<unsigned char, 4> op_datum = {1, 2, 3, 4};
  EXPECT_EQ(NC_EMAPTYPE,
            nc_put_varm(nc4, v_op, &z, &o, &p, &p, op_datum.data()));
  EXPECT_EQ(NC_EMAPTYPE,
            nc_put_varm_int(nc4, v_en, &z, &o, &p, &p, &enum_member));
  const ptrdiff_t zero_stride = 0;
  EXPECT_EQ(NC_ESTRIDE,
            nc_put_vars_int(nc4, v_i4, &z, &o, &zero_stride, &dummy));

  ScopedNcFile nc4_classic;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dvarput_err4c.nc", NC_NETCDF4 | NC_CLASSIC_MODEL,
                          4096, nc4_classic.ptr()));
  int dc = -1;
  int vc = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc4_classic, "d", 2, &dc));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc4_classic, "v", NC_INT, 1, &dc, &vc));
  EXPECT_EQ(NC_EINDEFINE, nc_put_vara_int(nc4_classic, vc, &z, &o, &dummy));
}

TEST(DvarputTest, DirectInternalPutVarsAndPutVarmContracts) {
  // Directly exercise internal branches in NCDEFAULT_put_vars and
  // NCDEFAULT_put_varm that NC_put_vars / NC_put_varm otherwise intercept.
  const size_t z = 0;
  const size_t o = 1;
  const ptrdiff_t p = 1;
  int dummy = 0;

  EXPECT_EQ(NC_EBADID, NCDEFAULT_put_vars(-1, 0, &z, &o, &p, &dummy, NC_INT));
  EXPECT_EQ(NC_EBADID,
            NCDEFAULT_put_varm(-1, 0, &z, &o, &p, &p, &dummy, NC_INT));

  // NetCDF-4 opaque var for user-defined type path in NCDEFAULT_put_vars.
  ScopedNcFile nc4;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dvarput_direct4.nc", NC_NETCDF4, 4096, nc4.ptr()));
  int d4 = -1;
  nc_type op_t = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc4, "n", 4, &d4));
  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc4, 4, "op4", &op_t));
  int v_op = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(nc4, "v_op", op_t, 1, &d4, &v_op));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc4));

  const std::array<unsigned char, 16> op_init = {
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  };
  ASSERT_EQ(NC_NOERR, nc_put_var(nc4, v_op, op_init.data()));

  // Non-atomic matching type + non-unit stride hits memtype >
  // NC_MAX_ATOMIC_TYPE and advances memptr by vartypelen inside odometer.
  const size_t op_start = 1;
  const size_t op_count = 2;
  const ptrdiff_t op_stride = 2;
  const std::array<unsigned char, 8> op_write = {10, 11, 12, 13,
                                                 20, 21, 22, 23};
  ASSERT_EQ(NC_NOERR, NCDEFAULT_put_vars(nc4, v_op, &op_start, &op_count,
                                         &op_stride, op_write.data(), op_t));
  std::array<unsigned char, 16> got_op{};
  ASSERT_EQ(NC_NOERR, nc_get_var(nc4, v_op, got_op.data()));
  EXPECT_THAT(got_op, ElementsAre(0, 0, 0, 0, 10, 11, 12, 13,  //
                                  0, 0, 0, 0, 20, 21, 22, 23));

  // Mismatched user-defined type returns NC_EBADTYPE.
  EXPECT_EQ(NC_EBADTYPE,
            NCDEFAULT_put_vars(nc4, v_op, &z, &o, &p, &dummy, NC_INT));

  // Classic dataset for rank>0 start==NULL, edges==NULL, stride==NULL, and
  // record/fixed dim edges==NULL branches in both NCDEFAULT_put_vars/put_varm.
  ScopedNcFile nc3;
  ASSERT_EQ(NC_NOERR, nc_create_mem("dvarput_direct3.nc", 0, 4096, nc3.ptr()));
  int dt = -1;
  int dc = -1;
  int df = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc3, "t", NC_UNLIMITED, &dt));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc3, "c", 3, &dc));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc3, "f", 4, &df));
  const std::array<int, 2> rec_dims = {dt, dc};
  const std::array<int, 2> fix_dims = {dc, df};
  int v_rec = -1;
  int v_fix = -1;
  int v_1d = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc3, "v_rec", NC_INT, 2, rec_dims.data(), &v_rec));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc3, "v_fix", NC_INT, 2, fix_dims.data(), &v_fix));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc3, "v_1d", NC_INT, 1, &df, &v_1d));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc3));

  // NCDEFAULT_put_vars: rank > 0 && start == nullptr -> NC_EINVALCOORDS.
  EXPECT_EQ(NC_EINVALCOORDS,
            NCDEFAULT_put_vars(nc3, v_1d, nullptr, &o, &p, &dummy, NC_INT));
  // NCDEFAULT_put_vars: bogus memtype > NC_MAX_ATOMIC_TYPE -> NC_EBADTYPE.
  EXPECT_EQ(NC_EBADTYPE,
            NCDEFAULT_put_vars(nc3, v_1d, &z, &o, &p, &dummy, 999));

  // NCDEFAULT_put_vars: edges == nullptr & stride == nullptr on fixed 1D var.
  const std::array<int, 4> vals4 = {1, 2, 3, 4};
  ASSERT_EQ(NC_NOERR, NCDEFAULT_put_vars(nc3, v_1d, &z, nullptr, nullptr,
                                         vals4.data(), NC_INT));
  std::array<int, 4> got4{};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(nc3, v_1d, got4.data()));
  EXPECT_EQ(got4, vals4);

  // Seed 3 records in v_rec so numrecs == 3.
  const std::array<size_t, 2> rec_s0 = {0, 0};
  const std::array<size_t, 2> rec_c3 = {3, 3};
  std::array<int, 9> rec_init;
  rec_init.fill(0);
  ASSERT_EQ(NC_NOERR, nc_put_vara_int(nc3, v_rec, rec_s0.data(), rec_c3.data(),
                                      rec_init.data()));

  // NCDEFAULT_put_vars with start = {1, 0}, edges = nullptr on 2D [rec, fixed]
  // covers both `is_recdim[i] && isrecvar` and `!is_recdim[i]` branches.
  const std::array<size_t, 2> rec_s1 = {1, 0};
  const std::array<int, 6> rec_patch = {11, 12, 13, 21, 22, 23};
  ASSERT_EQ(NC_NOERR, NCDEFAULT_put_vars(nc3, v_rec, rec_s1.data(), nullptr,
                                         nullptr, rec_patch.data(), NC_INT));
  const std::array<size_t, 2> rec_c2 = {2, 3};
  std::array<int, 6> got_rec{};
  ASSERT_EQ(NC_NOERR, nc_get_vara_int(nc3, v_rec, rec_s1.data(), rec_c2.data(),
                                      got_rec.data()));
  EXPECT_EQ(got_rec, rec_patch);

  // NCDEFAULT_put_varm: start == nullptr safely paired with edges[0] == 0
  // exits via `goto done` without dereferencing start in the carry loop.
  const size_t edge_zero = 0;
  ASSERT_EQ(NC_NOERR, NCDEFAULT_put_varm(nc3, v_1d, nullptr, &edge_zero, &p, &p,
                                         &dummy, NC_INT));

  // NCDEFAULT_put_varm: edges == nullptr on 2D [rec, fixed] var with transposed
  // imap = {1, 2} covers `idim == 0 && isrecvar ? numrecs - mystart[idim]` and
  // `varshape[idim] - mystart[idim]`.
  const std::array<ptrdiff_t, 2> imap_rec_t = {1, 2};
  const std::array<int, 7> rec_patch_t = {31, 41, 32, 42, 33, 43, 0};
  ASSERT_EQ(NC_NOERR,
            NCDEFAULT_put_varm(nc3, v_rec, rec_s1.data(), nullptr, nullptr,
                               imap_rec_t.data(), rec_patch_t.data(), NC_INT));
  ASSERT_EQ(NC_NOERR, nc_get_vara_int(nc3, v_rec, rec_s1.data(), rec_c2.data(),
                                      got_rec.data()));
  EXPECT_THAT(got_rec, ElementsAre(31, 32, 33, 41, 42, 43));

  // NCDEFAULT_put_varm: edges == nullptr, stride == nullptr on fixed 2D var
  // [3, 4] covers `varshape[0] - mystart[0]` and `stride == NULL` (+2 pad slots
  // keep `value + mymap[1]` = `data + 14` within [data, data + size]).
  const std::array<ptrdiff_t, 2> imap_fix_t = {1, 3};
  const std::array<int, 14> fix_t = {
      1, 5, 9,   //
      2, 6, 10,  //
      3, 7, 11,  //
      4, 8, 12,  //
      0, 0,
  };
  ASSERT_EQ(NC_NOERR,
            NCDEFAULT_put_varm(nc3, v_fix, rec_s0.data(), nullptr, nullptr,
                               imap_fix_t.data(), fix_t.data(), NC_INT));
  std::array<int, 12> got_fix{};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(nc3, v_fix, got_fix.data()));
  EXPECT_THAT(got_fix, ElementsAre(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12));
}

}  // namespace
}  // namespace netcdf
