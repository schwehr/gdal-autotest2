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

#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/libhdf5/nc4hdf_shim.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"

// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_filter.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

namespace netcdf {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

constexpr unsigned int kCustomXorFilterId = 32768u;
constexpr size_t kChunkCacheSize = 41943040u;
constexpr size_t kDefaultChunkCacheSize = 67108864u;
constexpr size_t kDefaultChunksInCache = 1000u;

struct CompoundRecord {
  int key;
  double matrix[2][2];
  nc_vlen_t seq;
};

// Defines a compound type matching CompoundRecord in ncid/grpid.
void DefineCompoundRecordType(int grpid, nc_type vlen_type, nc_type* cmp_type) {
  ASSERT_EQ(NC_NOERR, nc_def_compound(grpid, sizeof(CompoundRecord),
                                      "compound_record_t", cmp_type));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(grpid, *cmp_type, "key",
                               offsetof(CompoundRecord, key), NC_INT));
  const int dims[2] = {2, 2};
  ASSERT_EQ(NC_NOERR, nc_insert_array_compound(grpid, *cmp_type, "matrix",
                                               offsetof(CompoundRecord, matrix),
                                               NC_DOUBLE, 2, dims));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(grpid, *cmp_type, "seq",
                               offsetof(CompoundRecord, seq), vlen_type));
}

TEST(Nc4HdfTest, AtomicTypesAndEndiannessRoundTrip) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("atomic_types.nc", NC_NETCDF4, 0, file.ptr()));

  int dim2 = -1;
  int dim6 = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d2", 2, &dim2));
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d6", 6, &dim6));

  struct TypeSpec {
    nc_type type;
    const char* prefix;
  };
  constexpr TypeSpec kNumericTypes[] = {
      {NC_BYTE, "i8"},    {NC_SHORT, "i16"},  {NC_INT, "i32"},
      {NC_UBYTE, "u8"},   {NC_USHORT, "u16"}, {NC_UINT, "u32"},
      {NC_INT64, "i64"},  {NC_UINT64, "u64"}, {NC_FLOAT, "f32"},
      {NC_DOUBLE, "f64"},
  };
  constexpr int kEndians[] = {NC_ENDIAN_NATIVE, NC_ENDIAN_LITTLE,
                              NC_ENDIAN_BIG};
  constexpr const char* kEndianSuffix[] = {"nat", "le", "be"};

  int varids[10][3] = {};
  for (size_t t = 0; t < 10; ++t) {
    for (size_t e = 0; e < 3; ++e) {
      std::string name =
          std::string(kNumericTypes[t].prefix) + "_" + kEndianSuffix[e];
      ASSERT_EQ(NC_NOERR, nc_def_var(file, name.c_str(), kNumericTypes[t].type,
                                     1, &dim2, &varids[t][e]));
      ASSERT_EQ(NC_NOERR, nc_def_var_endian(file, varids[t][e], kEndians[e]));
    }
  }

  // Non-native big-endian variable with explicit custom fill value exercises
  // the fill_typeid native conversion path in var_create_dataset().
  int v_be_fill = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(file, "i32_be_fill", NC_INT, 1, &dim2, &v_be_fill));
  ASSERT_EQ(NC_NOERR, nc_def_var_endian(file, v_be_fill, NC_ENDIAN_BIG));
  const int custom_be_fill = 0x12345678;
  ASSERT_EQ(NC_NOERR,
            nc_def_var_fill(file, v_be_fill, NC_FILL, &custom_be_fill));

  int v_char = -1;
  int v_str = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "text", NC_CHAR, 1, &dim6, &v_char));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "strs", NC_STRING, 1, &dim2, &v_str));

  nc_type opq_type = 0;
  ASSERT_EQ(NC_NOERR, nc_def_opaque(file, 4, "opq4_t", &opq_type));
  ASSERT_EQ(NC_NOERR, nc_enddef(file));

  // Direct type lookup checks via shim.
  EXPECT_EQ(NC_EBADTYPE,
            Nc4HdfTestGetHdfTypeid(file, NC_NAT, NC_ENDIAN_NATIVE));
  EXPECT_EQ(NC_EBADTYPE, Nc4HdfTestGetHdfTypeid(file, 99999, NC_ENDIAN_NATIVE));
  EXPECT_EQ(NC_EBADTYPE, Nc4HdfTestGetHdfTypeid(file, -1, NC_ENDIAN_NATIVE));
  EXPECT_EQ(NC_NOERR, Nc4HdfTestGetHdfTypeid(file, NC_CHAR, NC_ENDIAN_NATIVE));
  EXPECT_EQ(NC_NOERR,
            Nc4HdfTestGetHdfTypeid(file, NC_STRING, NC_ENDIAN_NATIVE));
  EXPECT_EQ(NC_NOERR, Nc4HdfTestGetHdfTypeid(file, opq_type, NC_ENDIAN_NATIVE));
  for (const auto& spec : kNumericTypes) {
    for (int endian : kEndians) {
      EXPECT_EQ(NC_NOERR, Nc4HdfTestGetHdfTypeid(file, spec.type, endian));
    }
  }

  for (size_t e = 0; e < 3; ++e) {
    const signed char i8_vals[2] = {-12, 34};
    const short i16_vals[2] = {-1234, 5678};
    const int i32_vals[2] = {-123456, 789012};
    const unsigned char u8_vals[2] = {10u, 240u};
    const unsigned short u16_vals[2] = {1000u, 50000u};
    const unsigned int u32_vals[2] = {100000u, 3000000000u};
    const long long i64_vals[2] = {-900000000000LL, 900000000000LL};
    const unsigned long long u64_vals[2] = {42ULL, 18000000000000ULL};
    const float f32_vals[2] = {-1.25f, 3.5f};
    const double f64_vals[2] = {-2.75, 6.125};

    ASSERT_EQ(NC_NOERR, nc_put_var_schar(file, varids[0][e], i8_vals));
    ASSERT_EQ(NC_NOERR, nc_put_var_short(file, varids[1][e], i16_vals));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(file, varids[2][e], i32_vals));
    ASSERT_EQ(NC_NOERR, nc_put_var_uchar(file, varids[3][e], u8_vals));
    ASSERT_EQ(NC_NOERR, nc_put_var_ushort(file, varids[4][e], u16_vals));
    ASSERT_EQ(NC_NOERR, nc_put_var_uint(file, varids[5][e], u32_vals));
    ASSERT_EQ(NC_NOERR, nc_put_var_longlong(file, varids[6][e], i64_vals));
    ASSERT_EQ(NC_NOERR, nc_put_var_ulonglong(file, varids[7][e], u64_vals));
    ASSERT_EQ(NC_NOERR, nc_put_var_float(file, varids[8][e], f32_vals));
    ASSERT_EQ(NC_NOERR, nc_put_var_double(file, varids[9][e], f64_vals));
  }

  // Write only index 1 of i32_be_fill so index 0 retains big-endian fill.
  const size_t idx1 = 1;
  const int written_be = 424242;
  ASSERT_EQ(NC_NOERR, nc_put_var1_int(file, v_be_fill, &idx1, &written_be));

  const char text_in[7] = "netcdf";
  ASSERT_EQ(NC_NOERR, nc_put_var_text(file, v_char, text_in));
  const char* str_in[2] = {"alpha", "beta"};
  ASSERT_EQ(NC_NOERR, nc_put_var_string(file, v_str, str_in));

  ScopedMemio memio;
  ASSERT_EQ(NC_NOERR, nc_close_memio(file.release(), memio.get()));

  ScopedNcFile reopened;
  ASSERT_EQ(NC_NOERR, nc_open_mem("atomic_types.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, reopened.ptr()));

  // In-memory HDF5 files use H5_INDEX_NAME order on reopen, so look up varids
  // by name instead of reusing creation-time varids.
  int re_varids[10][3] = {};
  for (size_t t = 0; t < 10; ++t) {
    for (size_t e = 0; e < 3; ++e) {
      std::string name =
          std::string(kNumericTypes[t].prefix) + "_" + kEndianSuffix[e];
      ASSERT_EQ(NC_NOERR,
                nc_inq_varid(reopened, name.c_str(), &re_varids[t][e]));
    }
  }
  int re_be_fill = -1;
  int re_char = -1;
  int re_str = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(reopened, "i32_be_fill", &re_be_fill));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(reopened, "text", &re_char));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(reopened, "strs", &re_str));

  for (size_t e = 0; e < 3; ++e) {
    signed char i8_out[2] = {};
    short i16_out[2] = {};
    int i32_out[2] = {};
    unsigned char u8_out[2] = {};
    unsigned short u16_out[2] = {};
    unsigned int u32_out[2] = {};
    long long i64_out[2] = {};
    unsigned long long u64_out[2] = {};
    float f32_out[2] = {};
    double f64_out[2] = {};

    ASSERT_EQ(NC_NOERR, nc_get_var_schar(reopened, re_varids[0][e], i8_out));
    ASSERT_EQ(NC_NOERR, nc_get_var_short(reopened, re_varids[1][e], i16_out));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(reopened, re_varids[2][e], i32_out));
    ASSERT_EQ(NC_NOERR, nc_get_var_uchar(reopened, re_varids[3][e], u8_out));
    ASSERT_EQ(NC_NOERR, nc_get_var_ushort(reopened, re_varids[4][e], u16_out));
    ASSERT_EQ(NC_NOERR, nc_get_var_uint(reopened, re_varids[5][e], u32_out));
    ASSERT_EQ(NC_NOERR,
              nc_get_var_longlong(reopened, re_varids[6][e], i64_out));
    ASSERT_EQ(NC_NOERR,
              nc_get_var_ulonglong(reopened, re_varids[7][e], u64_out));
    ASSERT_EQ(NC_NOERR, nc_get_var_float(reopened, re_varids[8][e], f32_out));
    ASSERT_EQ(NC_NOERR, nc_get_var_double(reopened, re_varids[9][e], f64_out));

    EXPECT_THAT(i8_out, ElementsAre(-12, 34));
    EXPECT_THAT(i16_out, ElementsAre(-1234, 5678));
    EXPECT_THAT(i32_out, ElementsAre(-123456, 789012));
    EXPECT_THAT(u8_out, ElementsAre(10u, 240u));
    EXPECT_THAT(u16_out, ElementsAre(1000u, 50000u));
    EXPECT_THAT(u32_out, ElementsAre(100000u, 3000000000u));
    EXPECT_THAT(i64_out, ElementsAre(-900000000000LL, 900000000000LL));
    EXPECT_THAT(u64_out, ElementsAre(42ULL, 18000000000000ULL));
    EXPECT_THAT(f32_out, ElementsAre(-1.25f, 3.5f));
    EXPECT_THAT(f64_out, ElementsAre(-2.75, 6.125));
  }

  int be_fill_out[2] = {};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(reopened, re_be_fill, be_fill_out));
  EXPECT_THAT(be_fill_out, ElementsAre(custom_be_fill, written_be));

  char text_out[7] = {};
  ASSERT_EQ(NC_NOERR, nc_get_var_text(reopened, re_char, text_out));
  EXPECT_EQ(std::string(text_out, 6), "netcdf");

  ScopedNcStrings str_out(2);
  ASSERT_EQ(NC_NOERR, nc_get_var_string(reopened, re_str, str_out.data()));
  EXPECT_STREQ(str_out[0], "alpha");
  EXPECT_STREQ(str_out[1], "beta");
  str_out.clear();
  ASSERT_EQ(NC_NOERR, reopened.close());
}

TEST(Nc4HdfTest, AttributesCreateReuseReplaceAndVariableDirtyAttrs) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create_mem("attrs.nc", NC_NETCDF4, 0, file.ptr()));

  int sub_grpid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_grp(file, "sub", &sub_grpid));

  int dimid = -1;
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d", 3, &dimid));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v", NC_INT, 1, &dimid, &varid));

  nc_type opq_type = 0;
  ASSERT_EQ(NC_NOERR, nc_def_opaque(file, 4, "opq_t", &opq_type));

  // Initial empty & non-empty attributes across root, child group, and var.
  ASSERT_EQ(NC_NOERR, nc_put_att_text(file, NC_GLOBAL, "g_empty_str", 0, ""));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(file, NC_GLOBAL, "g_reuse_str", 4, "abcd"));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(file, NC_GLOBAL, "g_resize_str", 3, "xyz"));

  ASSERT_EQ(NC_NOERR,
            nc_put_att_int(file, NC_GLOBAL, "g_empty_int", NC_INT, 0, nullptr));
  const int init_ints[2] = {10, 20};
  ASSERT_EQ(NC_NOERR, nc_put_att_int(file, NC_GLOBAL, "g_reuse_int", NC_INT, 2,
                                     init_ints));
  ASSERT_EQ(NC_NOERR, nc_put_att_int(file, NC_GLOBAL, "g_retype_num", NC_INT, 2,
                                     init_ints));

  ASSERT_EQ(NC_NOERR,
            nc_put_att_string(file, NC_GLOBAL, "g_empty_strvec", 0, nullptr));
  const char* init_strvec[2] = {"s0", "s1"};
  ASSERT_EQ(NC_NOERR,
            nc_put_att_string(file, NC_GLOBAL, "g_strvec", 2, init_strvec));

  const unsigned char opq_val[4] = {1u, 2u, 3u, 4u};
  ASSERT_EQ(NC_NOERR, nc_put_att(file, varid, "v_opq", opq_type, 1, opq_val));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(sub_grpid, NC_GLOBAL, "sub_title", 5, "hello"));

  ASSERT_EQ(NC_NOERR, nc_enddef(file));

  // Re-enter define mode to exercise existing-attribute reuse vs
  // delete/recreate paths inside put_att_grpa() and post-creation variable
  // attr_dirty flushing.
  ASSERT_EQ(NC_NOERR, nc_redef(file));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(file, NC_GLOBAL, "g_empty_str", 5, "12345"));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(file, NC_GLOBAL, "g_reuse_str", 4, "wxyz"));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(file, NC_GLOBAL, "g_resize_str", 6, "abcdef"));

  const int empty_to_three[3] = {7, 8, 9};
  const int reuse_ints[2] = {30, 40};
  const double retype_doubles[2] = {1.5, 2.5};
  ASSERT_EQ(NC_NOERR, nc_put_att_int(file, NC_GLOBAL, "g_empty_int", NC_INT, 3,
                                     empty_to_three));
  ASSERT_EQ(NC_NOERR, nc_put_att_int(file, NC_GLOBAL, "g_reuse_int", NC_INT, 2,
                                     reuse_ints));
  ASSERT_EQ(NC_NOERR, nc_put_att_double(file, NC_GLOBAL, "g_retype_num",
                                        NC_DOUBLE, 2, retype_doubles));

  const char* updated_strvec[2] = {"u0", "updated_longer"};
  ASSERT_EQ(NC_NOERR,
            nc_put_att_string(file, NC_GLOBAL, "g_strvec", 2, updated_strvec));
  const char* shrunk_strvec[1] = {"only_one"};
  ASSERT_EQ(NC_NOERR, nc_put_att_string(file, NC_GLOBAL, "g_empty_strvec", 1,
                                        shrunk_strvec));

  // Dirty attribute on an already-created variable hits write_var() attr_dirty.
  ASSERT_EQ(NC_NOERR, nc_put_att_text(file, varid, "v_dirty", 5, "dirty"));
  ASSERT_EQ(NC_NOERR, nc_enddef(file));

  // Shrink g_resize_str back down to 0 length to hit H5S_NULL transition.
  ASSERT_EQ(NC_NOERR, nc_redef(file));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(file, NC_GLOBAL, "g_resize_str", 0, ""));
  ASSERT_EQ(NC_NOERR, nc_enddef(file));

  ScopedMemio memio;
  ASSERT_EQ(NC_NOERR, nc_close_memio(file.release(), memio.get()));

  ScopedNcFile reopened;
  ASSERT_EQ(NC_NOERR, nc_open_mem("attrs.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, reopened.ptr()));

  size_t len = 99;
  nc_type xtype = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_inq_attlen(reopened, NC_GLOBAL, "g_resize_str", &len));
  EXPECT_EQ(len, 0u);

  char buf5[6] = {};
  ASSERT_EQ(NC_NOERR,
            nc_get_att_text(reopened, NC_GLOBAL, "g_empty_str", buf5));
  EXPECT_EQ(std::string(buf5, 5), "12345");

  char buf4[5] = {};
  ASSERT_EQ(NC_NOERR,
            nc_get_att_text(reopened, NC_GLOBAL, "g_reuse_str", buf4));
  EXPECT_EQ(std::string(buf4, 4), "wxyz");

  int out_ints[3] = {};
  ASSERT_EQ(NC_NOERR,
            nc_get_att_int(reopened, NC_GLOBAL, "g_empty_int", out_ints));
  EXPECT_THAT(out_ints, ElementsAre(7, 8, 9));

  int out_reuse[2] = {};
  ASSERT_EQ(NC_NOERR,
            nc_get_att_int(reopened, NC_GLOBAL, "g_reuse_int", out_reuse));
  EXPECT_THAT(out_reuse, ElementsAre(30, 40));

  ASSERT_EQ(NC_NOERR,
            nc_inq_att(reopened, NC_GLOBAL, "g_retype_num", &xtype, &len));
  EXPECT_EQ(xtype, NC_DOUBLE);
  EXPECT_EQ(len, 2u);
  double out_doubles[2] = {};
  ASSERT_EQ(NC_NOERR, nc_get_att_double(reopened, NC_GLOBAL, "g_retype_num",
                                        out_doubles));
  EXPECT_THAT(out_doubles, ElementsAre(1.5, 2.5));

  ScopedNcStrings out_strvec(2);
  ASSERT_EQ(NC_NOERR, nc_get_att_string(reopened, NC_GLOBAL, "g_strvec",
                                        out_strvec.data()));
  EXPECT_STREQ(out_strvec[0], "u0");
  EXPECT_STREQ(out_strvec[1], "updated_longer");
  out_strvec.clear();

  ScopedNcStrings out_one(1);
  ASSERT_EQ(NC_NOERR, nc_get_att_string(reopened, NC_GLOBAL, "g_empty_strvec",
                                        out_one.data()));
  EXPECT_STREQ(out_one[0], "only_one");
  out_one.clear();

  int re_sub = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_grp_ncid(reopened, "sub", &re_sub));
  char sub_buf[6] = {};
  ASSERT_EQ(NC_NOERR, nc_get_att_text(re_sub, NC_GLOBAL, "sub_title", sub_buf));
  EXPECT_EQ(std::string(sub_buf, 5), "hello");

  char dirty_buf[6] = {};
  ASSERT_EQ(NC_NOERR, nc_get_att_text(reopened, varid, "v_dirty", dirty_buf));
  EXPECT_EQ(std::string(dirty_buf, 5), "dirty");

  unsigned char opq_out[4] = {};
  ASSERT_EQ(NC_NOERR, nc_get_att(reopened, varid, "v_opq", opq_out));
  EXPECT_THAT(opq_out, ElementsAre(1u, 2u, 3u, 4u));
  ASSERT_EQ(NC_NOERR, reopened.close());
}

TEST(Nc4HdfTest, StorageLayoutsFillValuesAndFilters) {
  ASSERT_EQ(NC_NOERR, Nc4HdfEnsureCustomFilterRegistered(kCustomXorFilterId));

  TempFile temp_file("nc4hdf_storage_filters.nc");
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR,
            nc_create(temp_file.c_str(), NC_NETCDF4 | NC_CLOBBER, file.ptr()));

  int d_unlim = -1;
  int d_small = -1;
  int d_32 = -1;
  int d_huge = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "unlim", NC_UNLIMITED, &d_unlim));
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "small", 4, &d_small));
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d32", 32, &d_32));
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "huge", 2000000, &d_huge));

  int v_scalar_contig = -1;
  int v_scalar_compact = -1;
  int v_compact = -1;
  int v_nofill_fletcher_deflate = -1;
  int v_xor = -1;
  int v_str_fill = -1;
  int v_vlen_fill = -1;
  int v_zero_contig = -1;
  int v_zero_str = -1;
  int v_zero_unclamped = -1;

  ASSERT_EQ(NC_NOERR, nc_def_var(file, "scalar_contig", NC_INT, 0, nullptr,
                                 &v_scalar_contig));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "scalar_compact", NC_INT, 0, nullptr,
                                 &v_scalar_compact));
  ASSERT_EQ(NC_NOERR,
            nc_def_var_chunking(file, v_scalar_compact, NC_COMPACT, nullptr));

  ASSERT_EQ(NC_NOERR, nc_def_var(file, "arr_compact", NC_DOUBLE, 1, &d_small,
                                 &v_compact));
  ASSERT_EQ(NC_NOERR,
            nc_def_var_chunking(file, v_compact, NC_COMPACT, nullptr));

  ASSERT_EQ(NC_NOERR, nc_def_var(file, "filtered", NC_INT, 1, &d_32,
                                 &v_nofill_fletcher_deflate));
  const size_t chunk16[1] = {16};
  ASSERT_EQ(NC_NOERR, nc_def_var_chunking(file, v_nofill_fletcher_deflate,
                                          NC_CHUNKED, chunk16));
  ASSERT_EQ(NC_NOERR, nc_def_var_fill(file, v_nofill_fletcher_deflate,
                                      NC_NOFILL, nullptr));
  ASSERT_EQ(NC_NOERR, nc_def_var_fletcher32(file, v_nofill_fletcher_deflate,
                                            NC_FLETCHER32));
  ASSERT_EQ(NC_NOERR,
            nc_def_var_deflate(file, v_nofill_fletcher_deflate, 1, 1, 4));
  ASSERT_EQ(NC_NOERR, nc_set_var_chunk_cache(file, v_nofill_fletcher_deflate,
                                             1024u * 1024u, 256u, 0.5f));

  int v_szip = -1;
  const bool szip_avail =
      (nc_inq_filter_avail(file, H5Z_FILTER_SZIP) == NC_NOERR);
  if (szip_avail) {
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file, "szip_var", NC_INT, 1, &d_32, &v_szip));
    ASSERT_EQ(NC_NOERR, nc_def_var_chunking(file, v_szip, NC_CHUNKED, chunk16));
    ASSERT_EQ(NC_NOERR, nc_def_var_szip(file, v_szip, NC_SZIP_EC, 8));
  }

  ASSERT_EQ(NC_NOERR, nc_def_var(file, "xor_var", NC_INT, 1, &d_32, &v_xor));
  ASSERT_EQ(NC_NOERR, nc_def_var_chunking(file, v_xor, NC_CHUNKED, chunk16));
  const unsigned int xor_param = 0x5au;
  ASSERT_EQ(NC_NOERR,
            nc_def_var_filter(file, v_xor, kCustomXorFilterId, 1, &xor_param));

  // NC_STRING variable with explicit custom fill value.
  ASSERT_EQ(NC_NOERR,
            nc_def_var(file, "str_fill", NC_STRING, 1, &d_small, &v_str_fill));
  const char* fill_str = "custom_string_fill";
  ASSERT_EQ(NC_NOERR, nc_def_var_fill(file, v_str_fill, NC_FILL, &fill_str));

  // NC_VLEN variable with explicit custom fill value (exercises nc_free_vlen
  // cleanup in var_create_dataset()).
  nc_type vlen_int_t = 0;
  ASSERT_EQ(NC_NOERR, nc_def_vlen(file, "vlen_int_t", NC_INT, &vlen_int_t));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "vlen_fill", vlen_int_t, 1, &d_small,
                                 &v_vlen_fill));
  int vlen_fill_elems[2] = {77, 88};
  nc_vlen_t vlen_fill{};
  vlen_fill.len = 2;
  vlen_fill.p = vlen_fill_elems;
  ASSERT_EQ(NC_NOERR, nc_def_var_fill(file, v_vlen_fill, NC_FILL, &vlen_fill));

  // Variables whose precomputed chunksizes are cleared so var_create_dataset()
  // exercises its inline default chunksize/contiguous auto-selection logic.
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "zero_contig", NC_INT, 1, &d_small,
                                 &v_zero_contig));
  const int unlim_small_dims[2] = {d_unlim, d_small};
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "zero_str", NC_STRING, 2,
                                 unlim_small_dims, &v_zero_str));
  const int unlim_huge_dims[2] = {d_unlim, d_huge};
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "zero_unclamped", NC_INT, 2,
                                 unlim_huge_dims, &v_zero_unclamped));
  ASSERT_EQ(NC_NOERR,
            nc_def_var_fill(file, v_zero_unclamped, NC_NOFILL, nullptr));
  ASSERT_EQ(NC_NOERR, Nc4HdfZeroVarChunksizes(file, v_zero_contig));
  ASSERT_EQ(NC_NOERR, Nc4HdfZeroVarChunksizes(file, v_zero_str));
  ASSERT_EQ(NC_NOERR, Nc4HdfZeroVarChunksizes(file, v_zero_unclamped));

  ASSERT_EQ(NC_NOERR, nc_enddef(file));

  int zero_contig_storage = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(file, v_zero_contig,
                                          &zero_contig_storage, nullptr));
  EXPECT_EQ(zero_contig_storage, NC_CONTIGUOUS);

  size_t str_chunks[2] = {};
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_chunking(file, v_zero_str, nullptr, str_chunks));
  EXPECT_EQ(str_chunks[0], 1u);
  EXPECT_EQ(str_chunks[1], 4u);

  size_t huge_chunks[2] = {};
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_chunking(file, v_zero_unclamped, nullptr, huge_chunks));
  EXPECT_EQ(huge_chunks[0], 1u);
  EXPECT_GT(huge_chunks[1], 1u);
  EXPECT_LT(huge_chunks[1], 2000000u);

  const int scalar_contig_val = 42;
  ASSERT_EQ(NC_NOERR,
            nc_put_var_int(file, v_scalar_contig, &scalar_contig_val));
  const int scalar_val = 919;
  ASSERT_EQ(NC_NOERR, nc_put_var_int(file, v_scalar_compact, &scalar_val));
  const double compact_vals[4] = {1.0, 2.0, 3.0, 4.0};
  ASSERT_EQ(NC_NOERR, nc_put_var_double(file, v_compact, compact_vals));

  std::array<int, 32> vals{};
  for (size_t i = 0; i < vals.size(); ++i) {
    vals[i] = static_cast<int>(i * 3 + 1);
  }
  ASSERT_EQ(NC_NOERR,
            nc_put_var_int(file, v_nofill_fletcher_deflate, vals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_int(file, v_xor, vals.data()));
  if (szip_avail) {
    ASSERT_EQ(NC_NOERR, nc_put_var_int(file, v_szip, vals.data()));
  }

  // Write index 3 of str_fill/vlen_fill so earlier indices return custom fills.
  const size_t idx3 = 3;
  const char* str3 = "written_3";
  ASSERT_EQ(NC_NOERR, nc_put_var1_string(file, v_str_fill, &idx3, &str3));
  int v3_elems[3] = {11, 22, 33};
  nc_vlen_t v3{};
  v3.len = 3;
  v3.p = v3_elems;
  ASSERT_EQ(NC_NOERR, nc_put_var1(file, v_vlen_fill, &idx3, &v3));

  ASSERT_EQ(NC_NOERR, file.close());

  // Reopen and verify storage layouts, filters, and custom string/vlen fills.
  ScopedNcFile reopened;
  ASSERT_EQ(NC_NOERR, nc_open(temp_file.c_str(), NC_NOWRITE, reopened.ptr()));

  int storage_mode = -1;
  int scalar_contig_out = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(reopened, v_scalar_contig,
                                          &storage_mode, nullptr));
  EXPECT_EQ(storage_mode, NC_CONTIGUOUS);
  ASSERT_EQ(NC_NOERR,
            nc_get_var_int(reopened, v_scalar_contig, &scalar_contig_out));
  EXPECT_EQ(scalar_contig_out, 42);

  int scalar_compact_out = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(reopened, v_scalar_compact,
                                          &storage_mode, nullptr));
  EXPECT_EQ(storage_mode, NC_COMPACT);
  ASSERT_EQ(NC_NOERR,
            nc_get_var_int(reopened, v_scalar_compact, &scalar_compact_out));
  EXPECT_EQ(scalar_compact_out, 919);

  double compact_out[4] = {};
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_chunking(reopened, v_compact, &storage_mode, nullptr));
  EXPECT_EQ(storage_mode, NC_COMPACT);
  ASSERT_EQ(NC_NOERR, nc_get_var_double(reopened, v_compact, compact_out));
  EXPECT_THAT(compact_out, ElementsAre(1.0, 2.0, 3.0, 4.0));

  int no_fill = 0;
  int fletcher = 0;
  int shuffle = 0;
  int deflate = 0;
  int deflate_level = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_var_fill(reopened, v_nofill_fletcher_deflate,
                                      &no_fill, nullptr));
  EXPECT_EQ(no_fill, 1);
  ASSERT_EQ(NC_NOERR, nc_inq_var_fletcher32(reopened, v_nofill_fletcher_deflate,
                                            &fletcher));
  EXPECT_EQ(fletcher, NC_FLETCHER32);
  ASSERT_EQ(NC_NOERR, nc_inq_var_deflate(reopened, v_nofill_fletcher_deflate,
                                         &shuffle, &deflate, &deflate_level));
  EXPECT_EQ(shuffle, 1);
  EXPECT_EQ(deflate, 1);
  EXPECT_EQ(deflate_level, 4);

  std::array<int, 32> xor_out{};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(reopened, v_xor, xor_out.data()));
  EXPECT_EQ(xor_out, vals);

  std::array<int, 32> def_out{};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(reopened, v_nofill_fletcher_deflate,
                                     def_out.data()));
  EXPECT_EQ(def_out, vals);
  if (szip_avail) {
    std::array<int, 32> szip_out{};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(reopened, v_szip, szip_out.data()));
    EXPECT_EQ(szip_out, vals);
  }

  ScopedNcStrings str_out(4);
  ASSERT_EQ(NC_NOERR, nc_get_var_string(reopened, v_str_fill, str_out.data()));
  EXPECT_STREQ(str_out[0], "custom_string_fill");
  EXPECT_STREQ(str_out[3], "written_3");
  str_out.clear();

  ScopedNcVlens vlen_out(4);
  ASSERT_EQ(NC_NOERR, nc_get_var(reopened, v_vlen_fill, vlen_out.data()));
  ASSERT_EQ(vlen_out[0].len, 2u);
  const int* fv_ptr = static_cast<const int*>(vlen_out[0].p);
  EXPECT_EQ(fv_ptr[0], 77);
  EXPECT_EQ(fv_ptr[1], 88);
  ASSERT_EQ(vlen_out[3].len, 3u);
  const int* wv_ptr = static_cast<const int*>(vlen_out[3].p);
  EXPECT_EQ(wv_ptr[0], 11);
  EXPECT_EQ(wv_ptr[1], 22);
  EXPECT_EQ(wv_ptr[2], 33);
  vlen_out.clear();
  ASSERT_EQ(NC_NOERR, reopened.close());
}

TEST(Nc4HdfTest, NoAttrCreateOrderModeWorksCleanly) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("noattcreord.nc", NC_NETCDF4 | NC_NOATTCREORD, 0,
                          file.ptr()));

  int sub = -1;
  int d_wo = -1;
  int v = -1;
  ASSERT_EQ(NC_NOERR, nc_def_grp(file, "sub", &sub));
  ASSERT_EQ(NC_NOERR, nc_def_dim(sub, "dim_wo_var", 4, &d_wo));
  ASSERT_EQ(NC_NOERR, nc_def_var(sub, "v", NC_INT, 1, &d_wo, &v));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(sub, v, "note", 4, "test"));
  ASSERT_EQ(NC_NOERR, nc_enddef(file));

  const int in_vals[4] = {1, 2, 3, 4};
  ASSERT_EQ(NC_NOERR, nc_put_var_int(sub, v, in_vals));
  ScopedMemio memio;
  ASSERT_EQ(NC_NOERR, nc_close_memio(file.release(), memio.get()));

  ScopedNcFile reopened;
  ASSERT_EQ(NC_NOERR, nc_open_mem("noattcreord.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, reopened.ptr()));
  int re_sub = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_grp_ncid(reopened, "sub", &re_sub));
  int out_vals[4] = {};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(re_sub, v, out_vals));
  EXPECT_THAT(out_vals, ElementsAre(1, 2, 3, 4));
  ASSERT_EQ(NC_NOERR, reopened.close());
}

TEST(Nc4HdfTest, VarCreateDatasetFilterErrorsAbortCleanly) {
  enum class VarLayout {
    kChunked1D,
    kScalar,
    kContiguous1D,
  };
  struct FilterFailureCase {
    VarLayout layout;
    unsigned int filter_id;
    size_t nparams;
    unsigned int params[2];
    int expected_err;
  };
  constexpr FilterFailureCase kCases[] = {
      {VarLayout::kChunked1D, H5Z_FILTER_DEFLATE, 0, {0, 0}, NC_EFILTER},
      {VarLayout::kChunked1D, H5Z_FILTER_DEFLATE, 1, {99, 0}, NC_EFILTER},
      {VarLayout::kChunked1D, H5Z_FILTER_SZIP, 1, {4, 0}, NC_EFILTER},
      {VarLayout::kChunked1D, H5Z_FILTER_SZIP, 2, {0, 1}, NC_EFILTER},
      {VarLayout::kChunked1D, 65536u, 0, {0, 0}, NC_EFILTER},
      // Fletcher32 uses H5Z_FLAG_MANDATORY, so H5Dcreate2() rejects it even
      // on a scalar (H5S_SCALAR) dataspace.
      {VarLayout::kScalar, H5Z_FILTER_FLETCHER32, 0, {0, 0}, NC_EHDFERR},
      // Shuffle uses H5Z_FLAG_OPTIONAL and is ignored by HDF5 on H5S_SCALAR,
      // so use a 1D contiguous (H5S_SIMPLE + H5D_CONTIGUOUS) variable so
      // H5Dcreate2() rejects filters on a non-chunked dataset.
      {VarLayout::kContiguous1D, H5Z_FILTER_SHUFFLE, 0, {0, 0}, NC_EHDFERR},
  };

  for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i) {
    TempFile bad_file("nc4hdf_bad_filter_" + std::to_string(i) + ".nc");
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create(bad_file.c_str(), NC_NETCDF4 | NC_CLOBBER, file.ptr()));
    int dimid = -1;
    int varid = -1;
    if (kCases[i].layout == VarLayout::kScalar) {
      ASSERT_EQ(NC_NOERR, nc_def_var(file, "v", NC_INT, 0, nullptr, &varid));
    } else {
      ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d", 16, &dimid));
      ASSERT_EQ(NC_NOERR, nc_def_var(file, "v", NC_INT, 1, &dimid, &varid));
      if (kCases[i].layout == VarLayout::kChunked1D) {
        const size_t chunk[1] = {8};
        ASSERT_EQ(NC_NOERR,
                  nc_def_var_chunking(file, varid, NC_CHUNKED, chunk));
      } else {
        ASSERT_EQ(NC_NOERR,
                  nc_def_var_chunking(file, varid, NC_CONTIGUOUS, nullptr));
      }
    }
    ASSERT_EQ(NC_NOERR,
              Nc4HdfInjectBadFilterParams(
                  file, varid, kCases[i].filter_id, kCases[i].nparams,
                  kCases[i].nparams > 0 ? kCases[i].params : nullptr));
    EXPECT_EQ(kCases[i].expected_err, nc_enddef(file)) << "case " << i;
    // Upstream var_create_dataset() stores the -1 return of failed H5Dcreate2
    // into hdf_datasetid, which makes close_vars() invoke H5Dclose(-1) and
    // leak file state on nc_abort(). Clear -1 back to 0 before aborting.
    ASSERT_EQ(NC_NOERR, Nc4HdfClearInvalidDatasetId(file, varid))
        << "case " << i;
    ASSERT_EQ(NC_NOERR, file.abort()) << "case " << i;
  }
}

TEST(Nc4HdfTest, QuantizationAttributesAndInvalidMode) {
  TempFile temp_file("nc4hdf_quantize.nc");
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR,
            nc_create(temp_file.c_str(), NC_NETCDF4 | NC_CLOBBER, file.ptr()));

  int dimid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d", 8, &dimid));
  const size_t chunk[1] = {4};

  int v_bg = -1;
  int v_gbr = -1;
  int v_br = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "bg", NC_FLOAT, 1, &dimid, &v_bg));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "gbr", NC_DOUBLE, 1, &dimid, &v_gbr));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "br", NC_FLOAT, 1, &dimid, &v_br));

  ASSERT_EQ(NC_NOERR, nc_def_var_chunking(file, v_bg, NC_CHUNKED, chunk));
  ASSERT_EQ(NC_NOERR, nc_def_var_chunking(file, v_gbr, NC_CHUNKED, chunk));
  ASSERT_EQ(NC_NOERR, nc_def_var_chunking(file, v_br, NC_CHUNKED, chunk));

  ASSERT_EQ(NC_NOERR, nc_def_var_quantize(file, v_bg, NC_QUANTIZE_BITGROOM, 3));
  ASSERT_EQ(NC_NOERR,
            nc_def_var_quantize(file, v_gbr, NC_QUANTIZE_GRANULARBR, 3));
  ASSERT_EQ(NC_NOERR,
            nc_def_var_quantize(file, v_br, NC_QUANTIZE_BITROUND, 10));
  ASSERT_EQ(NC_NOERR, nc_enddef(file));

  int has_bg = 0;
  int has_gbr = 0;
  int has_br = 0;
  ASSERT_EQ(NC_NOERR, Nc4HdfVarHasAttr(file, v_bg,
                                       NC_QUANTIZE_BITGROOM_ATT_NAME, &has_bg));
  ASSERT_EQ(
      NC_NOERR,
      Nc4HdfVarHasAttr(file, v_gbr, NC_QUANTIZE_GRANULARBR_ATT_NAME, &has_gbr));
  ASSERT_EQ(NC_NOERR, Nc4HdfVarHasAttr(file, v_br,
                                       NC_QUANTIZE_BITROUND_ATT_NAME, &has_br));
  EXPECT_EQ(has_bg, 1);
  EXPECT_EQ(has_gbr, 1);
  EXPECT_EQ(has_br, 1);

  const std::array<float, 8> fin = {1.23456f, 2.34567f, 3.45678f, 4.56789f,
                                    5.67891f, 6.78912f, 7.89123f, 8.91234f};
  const std::array<double, 8> din = {1.23456, 2.34567, 3.45678, 4.56789,
                                     5.67891, 6.78912, 7.89123, 8.91234};
  ASSERT_EQ(NC_NOERR, nc_put_var_float(file, v_bg, fin.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_double(file, v_gbr, din.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_float(file, v_br, fin.data()));
  ASSERT_EQ(NC_NOERR, file.close());

  ScopedNcFile reopened;
  ASSERT_EQ(NC_NOERR, nc_open(temp_file.c_str(), NC_NOWRITE, reopened.ptr()));
  int qmode = -1;
  int qnsd = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_var_quantize(reopened, v_bg, &qmode, &qnsd));
  EXPECT_EQ(qmode, NC_QUANTIZE_BITGROOM);
  EXPECT_EQ(qnsd, 3);
  ASSERT_EQ(NC_NOERR, nc_inq_var_quantize(reopened, v_gbr, &qmode, &qnsd));
  EXPECT_EQ(qmode, NC_QUANTIZE_GRANULARBR);
  EXPECT_EQ(qnsd, 3);
  ASSERT_EQ(NC_NOERR, nc_inq_var_quantize(reopened, v_br, &qmode, &qnsd));
  EXPECT_EQ(qmode, NC_QUANTIZE_BITROUND);
  EXPECT_EQ(qnsd, 10);
  ASSERT_EQ(NC_NOERR, reopened.close());

  // Invalid quantize_mode injected directly hits default case -> NC_EINVAL.
  TempFile bad_qtemp("nc4hdf_bad_quantize.nc");
  ScopedNcFile bad_qfile;
  ASSERT_EQ(NC_NOERR, nc_create(bad_qtemp.c_str(), NC_NETCDF4 | NC_CLOBBER,
                                bad_qfile.ptr()));
  int bdim = -1;
  int bvar = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(bad_qfile, "d", 4, &bdim));
  ASSERT_EQ(NC_NOERR, nc_def_var(bad_qfile, "qf", NC_FLOAT, 1, &bdim, &bvar));
  ASSERT_EQ(NC_NOERR, Nc4HdfSetVarQuantizeMode(bad_qfile, bvar, 99, 3));
  EXPECT_EQ(NC_EINVAL, nc_enddef(bad_qfile));
  ASSERT_EQ(NC_NOERR, bad_qfile.abort());
}

TEST(Nc4HdfTest, AdjustVarChunkCacheBehavior) {
  ScopedChunkCache cache_guard;
  // Upstream global default chunk cache is 64 MiB (DEFAULT_CHUNK_CACHE_SIZE),
  // while nc4_adjust_var_cache() triggers when chunkcache.size == 40 MiB
  // (CHUNK_CACHE_SIZE). Set the default to CHUNK_CACHE_SIZE for this test.
  ASSERT_EQ(NC_NOERR, nc_set_chunk_cache(kChunkCacheSize, 1009u, 0.75f));
  TempFile temp_file("nc4hdf_cache.nc");
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR,
            nc_create(temp_file.c_str(), NC_NETCDF4 | NC_CLOBBER, file.ptr()));

  int d_small = -1;
  int d_med = -1;
  int d_large = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d_small", 1000, &d_small));
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d_med", 30000, &d_med));
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d_large", 40000, &d_large));

  int v_contig = -1;
  int v_small = -1;
  int v_med = -1;
  int v_large = -1;
  int v_custom = -1;
  int v_str = -1;

  ASSERT_EQ(NC_NOERR,
            nc_def_var(file, "contig", NC_INT, 1, &d_small, &v_contig));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(file, "small_c", NC_INT, 1, &d_small, &v_small));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "med_c", NC_INT, 1, &d_med, &v_med));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(file, "large_c", NC_INT, 1, &d_large, &v_large));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(file, "custom_c", NC_INT, 1, &d_med, &v_custom));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "str_c", NC_STRING, 1, &d_med, &v_str));

  const size_t c_small[1] = {100};    // 400 bytes -> stays 41943040
  const size_t c_med[1] = {15000};    // 60000 bytes -> 60000000
  const size_t c_large[1] = {20000};  // 80000 bytes -> capped at 67108864
  const size_t c_str[1] = {11000};    // >= 44 MB on 32-bit, >= 64 MB on 64-bit
  ASSERT_EQ(NC_NOERR, nc_def_var_chunking(file, v_small, NC_CHUNKED, c_small));
  ASSERT_EQ(NC_NOERR, nc_def_var_chunking(file, v_med, NC_CHUNKED, c_med));
  ASSERT_EQ(NC_NOERR, nc_def_var_chunking(file, v_large, NC_CHUNKED, c_large));
  ASSERT_EQ(NC_NOERR, nc_def_var_chunking(file, v_custom, NC_CHUNKED, c_med));
  ASSERT_EQ(NC_NOERR,
            nc_set_var_chunk_cache(file, v_custom, 1048576u, 500u, 0.5f));
  ASSERT_EQ(NC_NOERR, nc_def_var_chunking(file, v_str, NC_CHUNKED, c_str));

  ASSERT_EQ(NC_NOERR, nc_enddef(file));

  // Verify zero type_info->size branch in nc4_adjust_var_cache() and
  // nc4_reopen_dataset() when hdf_datasetid != 0.
  size_t adjusted_cache_size = 0;
  ASSERT_EQ(NC_NOERR, Nc4HdfTestAdjustVarCacheWithZeroTypeSize(
                          file, v_str, &adjusted_cache_size));
  const size_t str_chunk_bytes = size_t{11000} * sizeof(char*);
  const size_t expected_str_cache =
      (str_chunk_bytes > (kDefaultChunkCacheSize / kDefaultChunksInCache))
          ? kDefaultChunkCacheSize
          : (str_chunk_bytes * kDefaultChunksInCache);
  EXPECT_EQ(adjusted_cache_size, expected_str_cache);

  size_t cache_sz = 0;
  ASSERT_EQ(NC_NOERR, nc_get_var_chunk_cache(file, v_contig, &cache_sz, nullptr,
                                             nullptr));
  EXPECT_EQ(cache_sz, kChunkCacheSize);
  ASSERT_EQ(NC_NOERR,
            nc_get_var_chunk_cache(file, v_small, &cache_sz, nullptr, nullptr));
  EXPECT_EQ(cache_sz, kChunkCacheSize);
  ASSERT_EQ(NC_NOERR,
            nc_get_var_chunk_cache(file, v_med, &cache_sz, nullptr, nullptr));
  EXPECT_EQ(cache_sz, size_t{60000} * kDefaultChunksInCache);
  ASSERT_EQ(NC_NOERR,
            nc_get_var_chunk_cache(file, v_large, &cache_sz, nullptr, nullptr));
  EXPECT_EQ(cache_sz, kDefaultChunkCacheSize);
  ASSERT_EQ(NC_NOERR, nc_get_var_chunk_cache(file, v_custom, &cache_sz, nullptr,
                                             nullptr));
  EXPECT_EQ(cache_sz, 1048576u);

  ASSERT_EQ(NC_NOERR, file.close());

  ScopedNcFile reopened;
  ASSERT_EQ(NC_NOERR, nc_open(temp_file.c_str(), NC_NOWRITE, reopened.ptr()));
  int re_small = -1;
  int re_med = -1;
  int re_large = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(reopened, "small_c", &re_small));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(reopened, "med_c", &re_med));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(reopened, "large_c", &re_large));

  // NC4_get_var_chunk_cache() does not load lazy var metadata; query chunking
  // first to trigger nc4_get_var_meta() -> nc4_adjust_var_cache().
  int storage = -1;
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_chunking(reopened, re_small, &storage, nullptr));
  ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(reopened, re_med, &storage, nullptr));
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_chunking(reopened, re_large, &storage, nullptr));

  ASSERT_EQ(NC_NOERR, nc_get_var_chunk_cache(reopened, re_small, &cache_sz,
                                             nullptr, nullptr));
  EXPECT_EQ(cache_sz, kChunkCacheSize);
  ASSERT_EQ(NC_NOERR, nc_get_var_chunk_cache(reopened, re_med, &cache_sz,
                                             nullptr, nullptr));
  EXPECT_EQ(cache_sz, size_t{60000} * kDefaultChunksInCache);
  ASSERT_EQ(NC_NOERR, nc_get_var_chunk_cache(reopened, re_large, &cache_sz,
                                             nullptr, nullptr));
  EXPECT_EQ(cache_sz, kDefaultChunkCacheSize);
  ASSERT_EQ(NC_NOERR, reopened.close());
}

TEST(Nc4HdfTest, GroupsUserTypesClassicModelAndEmptyEnum) {
  // Part A: Classic model _nc3_strict written on first enddef and reused on
  // subsequent redef/enddef.
  {
    TempFile classic_file("nc4hdf_classic_strict.nc");
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create(classic_file.c_str(),
                                  NC_NETCDF4 | NC_CLASSIC_MODEL | NC_CLOBBER,
                                  file.ptr()));
    int d = -1;
    int v1 = -1;
    int v2 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d", 2, &d));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "v1", NC_INT, 1, &d, &v1));
    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    ASSERT_EQ(NC_NOERR, nc_redef(file));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "v2", NC_INT, 1, &d, &v2));
    ASSERT_EQ(NC_NOERR, nc_enddef(file));
    int fmt = 0;
    int mode = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_format_extended(file, &fmt, &mode));
    EXPECT_EQ(fmt, NC_FORMATX_NC_HDF5);
    EXPECT_NE(mode & NC_CLASSIC_MODEL, 0);
    ASSERT_EQ(NC_NOERR, file.close());

    ScopedNcFile reopened;
    ASSERT_EQ(NC_NOERR,
              nc_open(classic_file.c_str(), NC_NOWRITE, reopened.ptr()));
    // NC4_inq_format() reflects the persisted _nc3_strict classic model flag,
    // while NC4_inq_format_extended() returns nc->mode | NC_NETCDF4 on open.
    int reopened_fmt = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_format(reopened, &reopened_fmt));
    EXPECT_EQ(reopened_fmt, NC_FORMAT_NETCDF4_CLASSIC);
    int is_nc4 = -1;
    ASSERT_EQ(NC_NOERR,
              nc_get_att_int(reopened, NC_GLOBAL, "_IsNetcdf4", &is_nc4));
    EXPECT_EQ(is_nc4, 1);
    ASSERT_EQ(NC_NOERR, nc_inq_format_extended(reopened, &fmt, &mode));
    EXPECT_EQ(fmt, NC_FORMATX_NC_HDF5);
    EXPECT_EQ(mode & NC_NETCDF4, NC_NETCDF4);
    ASSERT_EQ(NC_NOERR, reopened.close());
  }

  // Part B: Nested groups (created across initial enddef and redef) and all
  // committed user types (enum, opaque, vlen, compound with 2D array + vlen).
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("groups_types.nc", NC_NETCDF4, 0, file.ptr()));

    int g1 = -1;
    int g2 = -1;
    int g3 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(file, "g1", &g1));
    ASSERT_EQ(NC_NOERR, nc_def_grp(g1, "g2", &g2));

    nc_type enum_t = 0;
    nc_type opq_t = 0;
    nc_type vlen_t = 0;
    nc_type cmp_t = 0;
    ASSERT_EQ(NC_NOERR, nc_def_enum(file, NC_INT, "color_t", &enum_t));
    const int red_v = 0;
    const int green_v = 1;
    ASSERT_EQ(NC_NOERR, nc_insert_enum(file, enum_t, "RED", &red_v));
    ASSERT_EQ(NC_NOERR, nc_insert_enum(file, enum_t, "GREEN", &green_v));

    ASSERT_EQ(NC_NOERR, nc_def_opaque(file, 4, "tag_t", &opq_t));
    // Name vlen alphabetically before "compound_record_t" because in-memory
    // HDF5 files iterate root links with H5_INDEX_NAME when reopening.
    ASSERT_EQ(NC_NOERR, nc_def_vlen(file, "a_ivec_t", NC_INT, &vlen_t));
    DefineCompoundRecordType(file, vlen_t, &cmp_t);

    int d2 = -1;
    int v_enum = -1;
    int v_opq = -1;
    int v_cmp = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d2", 2, &d2));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "v_enum", enum_t, 1, &d2, &v_enum));
    ASSERT_EQ(NC_NOERR, nc_def_var(g2, "v_opq", opq_t, 1, &d2, &v_opq));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "v_cmp", cmp_t, 1, &d2, &v_cmp));
    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    // Re-enter define mode to trigger already-committed skip in commit_type()
    // and dynamic subgroup creation in create_group().
    ASSERT_EQ(NC_NOERR, nc_redef(file));
    ASSERT_EQ(NC_NOERR, nc_def_grp(g2, "g3", &g3));
    int v_cmp_sub = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(g3, "v_cmp_sub", cmp_t, 1, &d2, &v_cmp_sub));
    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    int seq0_data[2] = {100, 200};
    int seq1_data[3] = {300, 400, 500};
    CompoundRecord recs[2];
    std::memset(recs, 0, sizeof(recs));
    recs[0].key = 11;
    recs[0].matrix[0][0] = 1.0;
    recs[0].matrix[0][1] = 2.0;
    recs[0].matrix[1][0] = 3.0;
    recs[0].matrix[1][1] = 4.0;
    recs[0].seq.len = 2;
    recs[0].seq.p = seq0_data;
    recs[1].key = 22;
    recs[1].matrix[0][0] = 5.0;
    recs[1].matrix[0][1] = 6.0;
    recs[1].matrix[1][0] = 7.0;
    recs[1].matrix[1][1] = 8.0;
    recs[1].seq.len = 3;
    recs[1].seq.p = seq1_data;

    ASSERT_EQ(NC_NOERR, nc_put_var(file, v_cmp, recs));
    const int enums_in[2] = {red_v, green_v};
    ASSERT_EQ(NC_NOERR, nc_put_var(file, v_enum, enums_in));
    const unsigned char opq_in[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_EQ(NC_NOERR, nc_put_var(g2, v_opq, opq_in));

    ScopedMemio memio;
    ASSERT_EQ(NC_NOERR, nc_close_memio(file.release(), memio.get()));

    ScopedNcFile reopened;
    ASSERT_EQ(NC_NOERR, nc_open_mem("groups_types.nc", NC_NOWRITE, memio.size(),
                                    memio.get()->memory, reopened.ptr()));
    nc_type re_cmp_t = 0;
    int re_v_cmp = -1;
    int re_v_enum = -1;
    ASSERT_EQ(NC_NOERR,
              nc_inq_typeid(reopened, "compound_record_t", &re_cmp_t));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(reopened, "v_cmp", &re_v_cmp));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(reopened, "v_enum", &re_v_enum));

    std::vector<CompoundRecord> recs_out(2);
    std::memset(recs_out.data(), 0, recs_out.size() * sizeof(CompoundRecord));
    struct ScopedNcReclaim {
      int ncid;
      nc_type xtype;
      void* data;
      size_t count;
      bool active = true;
      int reclaim() {
        if (!active) return NC_NOERR;
        active = false;
        return nc_reclaim_data(ncid, xtype, data, count);
      }
      ~ScopedNcReclaim() { static_cast<void>(reclaim()); }
    } recs_guard{reopened.get(), re_cmp_t, recs_out.data(), recs_out.size()};

    ASSERT_EQ(NC_NOERR, nc_get_var(reopened, re_v_cmp, recs_out.data()));
    EXPECT_EQ(recs_out[0].key, 11);
    EXPECT_DOUBLE_EQ(recs_out[0].matrix[1][1], 4.0);
    ASSERT_EQ(recs_out[0].seq.len, 2u);
    EXPECT_EQ(static_cast<const int*>(recs_out[0].seq.p)[1], 200);
    EXPECT_EQ(recs_out[1].key, 22);
    ASSERT_EQ(recs_out[1].seq.len, 3u);
    EXPECT_EQ(static_cast<const int*>(recs_out[1].seq.p)[2], 500);
    ASSERT_EQ(NC_NOERR, recs_guard.reclaim());

    int enum_out[2] = {};
    ASSERT_EQ(NC_NOERR, nc_get_var(reopened, re_v_enum, enum_out));
    EXPECT_THAT(enum_out, ElementsAre(red_v, green_v));

    int re_g1 = -1;
    int re_g2 = -1;
    int re_g3 = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_grp_ncid(reopened, "g1", &re_g1));
    ASSERT_EQ(NC_NOERR, nc_inq_grp_ncid(re_g1, "g2", &re_g2));
    ASSERT_EQ(NC_NOERR, nc_inq_grp_ncid(re_g2, "g3", &re_g3));

    int re_v_opq = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(re_g2, "v_opq", &re_v_opq));
    unsigned char opq_out[8] = {};
    ASSERT_EQ(NC_NOERR, nc_get_var(re_g2, re_v_opq, opq_out));
    EXPECT_THAT(opq_out, ElementsAre(1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u));

    int re_cmp_sub = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(re_g3, "v_cmp_sub", &re_cmp_sub));
    EXPECT_EQ(re_cmp_sub, v_cmp_sub);
    ASSERT_EQ(NC_NOERR, reopened.close());
  }

  // Part C: Empty NC_ENUM triggers NC_EINVAL in commit_type().
  {
    TempFile empty_enum_temp("nc4hdf_empty_enum.nc");
    ScopedNcFile empty_enum_file;
    ASSERT_EQ(NC_NOERR,
              nc_create(empty_enum_temp.c_str(), NC_NETCDF4 | NC_CLOBBER,
                        empty_enum_file.ptr()));
    nc_type empty_enum = 0;
    ASSERT_EQ(NC_NOERR,
              nc_def_enum(empty_enum_file, NC_INT, "empty_e", &empty_enum));
    EXPECT_EQ(NC_EINVAL, nc_enddef(empty_enum_file));
    ASSERT_EQ(NC_NOERR, empty_enum_file.abort());
  }
}

TEST(Nc4HdfTest, FillValueChangeReplacementOnCoordAndDataVars) {
  TempFile temp_file("nc4hdf_fill_replace.nc");
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR,
            nc_create(temp_file.c_str(), NC_NETCDF4 | NC_CLOBBER, file.ptr()));

  // Place coord and data vars inside a child group `work` because NC4_enddef()
  // marks root-group vars as written_to = NC_TRUE, whereas child-group vars
  // remain replaceable during nc_redef() without triggering NC_ELATEFILL.
  int work = -1;
  int sub = -1;
  ASSERT_EQ(NC_NOERR, nc_def_grp(file, "work", &work));
  ASSERT_EQ(NC_NOERR, nc_def_grp(work, "sub", &sub));

  // Define `axis` before `coord` so `axis` gets dimid 0 (_Netcdf4Dimid=0)
  // and `coord` gets dimid 1. During the subsequent nc_redef(),
  // detect_preserve_dimids() sees one ordered coord var and sets
  // bad_coord_order=NC_FALSE, so write_var() recreates `coord` after `axis`
  // without _Netcdf4Dimid; on reopen, `axis` advances next_dimid to 1 so
  // `coord` receives dimid 1, matching `data`'s COORDINATES attribute.
  int d_axis = -1;
  int d_coord = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(work, "axis", 4, &d_axis));
  ASSERT_EQ(NC_NOERR, nc_def_dim(work, "coord", 3, &d_coord));

  int v_coord = -1;
  int v_data = -1;
  int v_sub_user = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(work, "coord", NC_INT, 1, &d_coord, &v_coord));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(work, v_coord, "units", 6, "meters"));

  const int data_dims[2] = {d_coord, d_axis};
  ASSERT_EQ(NC_NOERR, nc_def_var(work, "data", NC_INT, 2, data_dims, &v_data));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(work, v_data, "long_name", 6, "height"));

  ASSERT_EQ(NC_NOERR,
            nc_def_var(sub, "sub_user", NC_INT, 1, &d_coord, &v_sub_user));
  ASSERT_EQ(NC_NOERR, nc_enddef(file));

  // Changing NC_FillValue during nc_redef sets fill_val_changed on both coord
  // and non-coord vars, forcing dataset replacement while flag_atts_dirty()
  // retains existing user attributes. Defining an uncreated var in sub during
  // the same redef also exercises !var->created in rec_detach/reattach_scales.
  ASSERT_EQ(NC_NOERR, nc_redef(file));
  int v_sub_redef_new = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(sub, "sub_redef_new", NC_INT, 1, &d_coord,
                                 &v_sub_redef_new));
  const int new_coord_fill = -777;
  const int new_data_fill = -888;
  ASSERT_EQ(NC_NOERR, nc_put_att_int(work, v_coord, NC_FillValue, NC_INT, 1,
                                     &new_coord_fill));
  ASSERT_EQ(NC_NOERR, nc_put_att_int(work, v_data, NC_FillValue, NC_INT, 1,
                                     &new_data_fill));
  ASSERT_EQ(NC_NOERR, nc_enddef(file));
  ASSERT_EQ(NC_NOERR, file.close());

  ScopedNcFile reopened;
  ASSERT_EQ(NC_NOERR, nc_open(temp_file.c_str(), NC_NOWRITE, reopened.ptr()));
  int re_work = -1;
  int re_coord = -1;
  int re_data = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_grp_ncid(reopened, "work", &re_work));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(re_work, "coord", &re_coord));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(re_work, "data", &re_data));

  char units_buf[7] = {};
  char lnaam_buf[7] = {};
  ASSERT_EQ(NC_NOERR, nc_get_att_text(re_work, re_coord, "units", units_buf));
  ASSERT_EQ(NC_NOERR,
            nc_get_att_text(re_work, re_data, "long_name", lnaam_buf));
  EXPECT_EQ(std::string(units_buf, 6), "meters");
  EXPECT_EQ(std::string(lnaam_buf, 6), "height");

  int coord_out[3] = {};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(re_work, re_coord, coord_out));
  EXPECT_THAT(coord_out, ElementsAre(-777, -777, -777));
  std::array<int, 12> data_out{};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(re_work, re_data, data_out.data()));
  EXPECT_THAT(data_out, ElementsAre(-888, -888, -888, -888, -888, -888, -888,
                                    -888, -888, -888, -888, -888));
  ASSERT_EQ(NC_NOERR, reopened.close());
}

TEST(Nc4HdfTest, RenameBecameAndWasCoordVarTransitions) {
  // Subcase A: Rename variable to dim name BEFORE enddef (var_exists == false)
  // and AFTER enddef (var_exists == true -> dataset replaced, scales detached
  // & reattached across root and subgroup, alt_name non-coord var).
  {
    TempFile temp_file("nc4hdf_became_coord.nc");
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create(temp_file.c_str(), NC_NETCDF4 | NC_CLOBBER,
                                  file.ptr()));
    int sub = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(file, "sub", &sub));

    int d_pre = -1;
    int d_post = -1;
    int d_alt = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "pdim", 3, &d_pre));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "post_dim", 3, &d_post));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "alt_dim", 2, &d_alt));

    int v_pre = -1;
    int v_user = -1;
    int v_alt = -1;
    int v_cand = -1;
    int v_sub_user = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "pre_tmp", NC_INT, 1, &d_pre, &v_pre));
    // Pre-enddef rename sets became_coord_var while no HDF5 dataset exists yet.
    ASSERT_EQ(NC_NOERR, nc_rename_var(file, v_pre, "pdim"));

    ASSERT_EQ(NC_NOERR,
              nc_def_var(file, "user_v", NC_INT, 1, &d_post, &v_user));
    // Same name as alt_dim, but indexed over d_pre -> var->alt_name set.
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "alt_dim", NC_INT, 1, &d_pre, &v_alt));
    // Define candidate last (highest varid) for post-enddef rename.
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "cand", NC_INT, 1, &d_post, &v_cand));
    ASSERT_EQ(NC_NOERR, nc_put_att_text(file, v_cand, "notes", 4, "keep"));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(sub, "sub_user", NC_INT, 1, &d_post, &v_sub_user));
    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    ASSERT_EQ(NC_NOERR, nc_redef(file));
    ASSERT_EQ(NC_NOERR, nc_rename_var(file, v_cand, "post_dim"));
    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    const int post_vals[3] = {10, 20, 30};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(file, v_cand, post_vals));
    ASSERT_EQ(NC_NOERR, file.close());

    ScopedNcFile reopened;
    ASSERT_EQ(NC_NOERR, nc_open(temp_file.c_str(), NC_NOWRITE, reopened.ptr()));
    char notes_buf[5] = {};
    ASSERT_EQ(NC_NOERR, nc_get_att_text(reopened, v_cand, "notes", notes_buf));
    EXPECT_EQ(std::string(notes_buf, 4), "keep");
    int out_vals[3] = {};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(reopened, v_cand, out_vals));
    EXPECT_THAT(out_vals, ElementsAre(10, 20, 30));
    ASSERT_EQ(NC_NOERR, reopened.close());
  }

  // Subcase B: Out-of-order coord vars + second redef/enddef (hits
  // write_netcdf4_dimid attr_exists==1 on both write_var & write_dim), breaking
  // coord vars (remove_coord_atts with & without _Netcdf4Dimid), and
  // Nc4HdfSetVarWasCoordVar on a regular attached var.
  {
    TempFile ooo_file("nc4hdf_ooo_was_coord.nc");
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create(ooo_file.c_str(), NC_NETCDF4 | NC_CLOBBER, file.ptr()));
    int d0 = -1;
    int d1 = -1;
    int d_wovar = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d0", 2, &d0));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d1", 3, &d1));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d_wovar", 2, &d_wovar));

    // Define coord vars out of dim order (d1 before d0) -> bad_coord_order=1.
    int v_c1 = -1;
    int v_c0 = -1;
    int v_u = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "d1", NC_INT, 1, &d1, &v_c1));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "d0", NC_INT, 1, &d0, &v_c0));
    const int u_dims[3] = {d0, d1, d_wovar};
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "u", NC_INT, 3, u_dims, &v_u));
    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    int has_dimid = 0;
    ASSERT_EQ(NC_NOERR,
              Nc4HdfDimHasAttr(file, "d1", "_Netcdf4Dimid", &has_dimid));
    EXPECT_EQ(has_dimid, 1);

    // Second redef/enddef writes existing dimids with attr_exists == 1 on both
    // coord vars (write_var) and dim without var (write_dim).
    ASSERT_EQ(NC_NOERR, nc_redef(file));
    int v_extra = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "extra", NC_INT, 1, &d0, &v_extra));
    ASSERT_EQ(NC_NOERR, Nc4HdfSetVarWasCoordVar(file, v_u, 1));
    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    // Break coordinate variable d0 via rename (highest coord varid here?
    // Rename highest var first or test v_u unaffected; rename v_extra? Wait:
    // keep v_u/v_extra intact; rename v_c0/v_c1 is safe since no alt_name
    // vars exist).
    ASSERT_EQ(NC_NOERR, nc_redef(file));
    ASSERT_EQ(NC_NOERR, nc_rename_var(file, v_c1, "not_d1"));
    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    int has_class = 1;
    int has_dimid_after = 1;
    ASSERT_EQ(NC_NOERR, Nc4HdfVarHasAttr(file, v_c1, "CLASS", &has_class));
    ASSERT_EQ(NC_NOERR,
              Nc4HdfVarHasAttr(file, v_c1, "_Netcdf4Dimid", &has_dimid_after));
    EXPECT_EQ(has_class, 0);
    EXPECT_EQ(has_dimid_after, 0);
    ASSERT_EQ(NC_NOERR, file.close());
  }

  // Subcase C: Initial nc_enddef() writes _Netcdf4Dimid because NC4_def_var()
  // marks var->is_new_var = NC_TRUE. Remove _Netcdf4Dimid before breaking the
  // coord var so remove_coord_atts() exercises the absent NC_DIMID_ATT_NAME
  // path while still removing CLASS and NAME.
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("inorder_break.nc", NC_NETCDF4, 0, file.ptr()));
    int dx = -1;
    int vx = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "x", 2, &dx));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "x", NC_INT, 1, &dx, &vx));
    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    int has_dimid = 0;
    ASSERT_EQ(NC_NOERR,
              Nc4HdfDimHasAttr(file, "x", "_Netcdf4Dimid", &has_dimid));
    EXPECT_EQ(has_dimid, 1);

    ASSERT_EQ(NC_NOERR, Nc4HdfDeleteVarAttr(file, vx, "_Netcdf4Dimid"));
    ASSERT_EQ(NC_NOERR,
              Nc4HdfDimHasAttr(file, "x", "_Netcdf4Dimid", &has_dimid));
    EXPECT_EQ(has_dimid, 0);

    ASSERT_EQ(NC_NOERR, nc_redef(file));
    ASSERT_EQ(NC_NOERR, nc_rename_var(file, vx, "broken_x"));
    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    int has_class = 1;
    int has_name = 1;
    ASSERT_EQ(NC_NOERR, Nc4HdfVarHasAttr(file, vx, "CLASS", &has_class));
    ASSERT_EQ(NC_NOERR, Nc4HdfVarHasAttr(file, vx, "NAME", &has_name));
    EXPECT_EQ(has_class, 0);
    EXPECT_EQ(has_name, 0);
    ASSERT_EQ(NC_NOERR, file.close());
  }
}

TEST(Nc4HdfTest, NoDimscaleAttachExtendedUnlimScalesAndOpenVarHelpers) {
  // Part A: NC_NODIMSCALE_ATTACH skips attach_dimscales().
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("nodimscale.nc", NC_NETCDF4 | NC_NODIMSCALE_ATTACH,
                            0, file.ptr()));
    int d = -1;
    int v = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d", 3, &d));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "v", NC_INT, 1, &d, &v));
    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    int has_dimlist = 1;
    ASSERT_EQ(NC_NOERR,
              Nc4HdfVarHasAttr(file, v, "DIMENSION_LIST", &has_dimlist));
    EXPECT_EQ(has_dimlist, 0);
    ASSERT_EQ(NC_NOERR, file.close());
  }

  // Part B: Extended unlimited dim, rec_detach/reattach NC_EDIMSCALE, and
  // nc4_open_var_grp2 paths.
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("var_helpers.nc", NC_NETCDF4, 0, file.ptr()));
    int sub = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(file, "sub", &sub));

    int d_unlim = -1;
    int v_created = -1;
    int v_sub_created = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "t", NC_UNLIMITED, &d_unlim));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file, "created_v", NC_INT, 1, &d_unlim, &v_created));
    ASSERT_EQ(NC_NOERR, nc_def_var(sub, "sub_created", NC_INT, 1, &d_unlim,
                                   &v_sub_created));
    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    const size_t start[1] = {0};
    const size_t count[1] = {3};
    const int tvals[3] = {1, 2, 3};
    ASSERT_EQ(NC_NOERR, nc_put_vara_int(file, v_created, start, count, tvals));
    ASSERT_EQ(NC_NOERR, nc_sync(file));

    // Define uncreated variables in root and child groups during nc_redef, then
    // test nc4_open_var_grp2 and rec_detach/reattach error paths BEFORE enddef.
    ASSERT_EQ(NC_NOERR, nc_redef(file));
    int v_uncreated = -1;
    int v_sub_uncreated = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "uncreated_v", NC_INT, 1, &d_unlim,
                                   &v_uncreated));
    ASSERT_EQ(NC_NOERR, nc_def_var(sub, "sub_uncreated", NC_INT, 1, &d_unlim,
                                   &v_sub_uncreated));

    EXPECT_EQ(NC_NOERR,
              Nc4HdfTestOpenVarGrp2(file, v_created, v_uncreated, 999));
    EXPECT_EQ(NC_NOERR, Nc4HdfTestRecScalesError(file, d_unlim));

    ASSERT_EQ(NC_NOERR, nc_enddef(file));
    ASSERT_EQ(NC_NOERR, file.close());
  }
}

TEST(Nc4HdfTest, RecMatchDimscalesOnRawHdf5) {
  TempFile raw_file("nc4hdf_raw_dimscales.h5");
  ASSERT_EQ(NC_NOERR, Nc4HdfCreateRawHdf5ForDimscaleMatch(raw_file.c_str()));

  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_open(raw_file.c_str(), NC_NOWRITE, file.ptr()));

  int phony_ndims = 0;
  int sub_matched = 0;
  ASSERT_EQ(NC_NOERR,
            Nc4HdfTestRecMatchDimscales(file, &phony_ndims, &sub_matched));
  EXPECT_EQ(phony_ndims, 3);
  EXPECT_EQ(sub_matched, 1);

  ASSERT_EQ(NC_NOERR, file.close());
}

TEST(Nc4HdfTest, ShowOpenObjectsAndProvenance) {
  // Part A: NetCDF-4 file with child group, dataset, committed enum, and attr.
  {
    TempFile nc_file("nc4hdf_open_objs.nc");
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create(nc_file.c_str(), NC_NETCDF4 | NC_CLOBBER, file.ptr()));
    int child = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(file, "child", &child));

    nc_type enum_t = 0;
    ASSERT_EQ(NC_NOERR, nc_def_enum(file, NC_INT, "state_t", &enum_t));
    const int ok_val = 1;
    ASSERT_EQ(NC_NOERR, nc_insert_enum(file, enum_t, "OK", &ok_val));

    int d = -1;
    int v = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d", 2, &d));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "v", enum_t, 1, &d, &v));
    ASSERT_EQ(NC_NOERR, nc_put_att_text(file, NC_GLOBAL, "meta", 4, "info"));
    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    const int data[2] = {ok_val, ok_val};
    ASSERT_EQ(NC_NOERR, nc_put_var(file, v, data));

    ASSERT_EQ(NC_NOERR, Nc4HdfTestShowOpenObjects(file, file, "meta"));

    unsigned major = 0;
    unsigned minor = 0;
    unsigned release = 0;
    int superblock = -1;
    int is_nc4 = -1;
    int bad_super_err = NC_NOERR;
    ASSERT_EQ(NC_NOERR,
              Nc4HdfGetProvenanceDirect(file, &major, &minor, &release,
                                        &superblock, &is_nc4, &bad_super_err));
    EXPECT_GE(major, 1u);
    EXPECT_GE(superblock, 0);
    EXPECT_EQ(is_nc4, 1);
    EXPECT_EQ(bad_super_err, NC_EHDFERR);
    ASSERT_EQ(NC_NOERR, file.close());

    // Public virtual provenance attributes queried after reopening.
    ScopedNcFile reopened;
    ASSERT_EQ(NC_NOERR, nc_open(nc_file.c_str(), NC_NOWRITE, reopened.ptr()));
    int is_nc4_att = -1;
    int super_att = -1;
    ASSERT_EQ(NC_NOERR,
              nc_get_att_int(reopened, NC_GLOBAL, "_IsNetcdf4", &is_nc4_att));
    ASSERT_EQ(NC_NOERR, nc_get_att_int(reopened, NC_GLOBAL,
                                       "_SuperblockVersion", &super_att));
    EXPECT_EQ(is_nc4_att, 1);
    EXPECT_GE(super_att, 0);

    size_t props_len = 0;
    ASSERT_EQ(NC_NOERR,
              nc_inq_attlen(reopened, NC_GLOBAL, "_NCProperties", &props_len));
    std::string props(props_len, '\0');
    ASSERT_EQ(NC_NOERR, nc_get_att_text(reopened, NC_GLOBAL, "_NCProperties",
                                        props.data()));
    EXPECT_THAT(props, HasSubstr("version="));
    ASSERT_EQ(NC_NOERR, reopened.close());
  }

  // Part B: Raw HDF5 without _NCProperties/_nc3_strict -> NC4_walk heuristic.
  {
    TempFile raw_below("nc4hdf_raw_below_thresh.h5");
    ASSERT_EQ(NC_NOERR, Nc4HdfCreateRawHdf5File(raw_below.c_str(), 0));
    ScopedNcFile f0;
    ASSERT_EQ(NC_NOERR, nc_open(raw_below.c_str(), NC_NOWRITE, f0.ptr()));
    int is_nc4 = -1;
    ASSERT_EQ(NC_NOERR, Nc4HdfGetProvenanceDirect(f0, nullptr, nullptr, nullptr,
                                                  nullptr, &is_nc4, nullptr));
    EXPECT_EQ(is_nc4, 0);
    int att_val = -1;
    ASSERT_EQ(NC_NOERR, nc_get_att_int(f0, NC_GLOBAL, "_IsNetcdf4", &att_val));
    EXPECT_EQ(att_val, 0);
    ASSERT_EQ(NC_NOERR, f0.close());

    TempFile raw_above("nc4hdf_raw_above_thresh.h5");
    ASSERT_EQ(NC_NOERR, Nc4HdfCreateRawHdf5File(raw_above.c_str(), 1));
    ScopedNcFile f1;
    ASSERT_EQ(NC_NOERR, nc_open(raw_above.c_str(), NC_NOWRITE, f1.ptr()));
    is_nc4 = -1;
    ASSERT_EQ(NC_NOERR, Nc4HdfGetProvenanceDirect(f1, nullptr, nullptr, nullptr,
                                                  nullptr, &is_nc4, nullptr));
    EXPECT_EQ(is_nc4, 1);
    att_val = -1;
    ASSERT_EQ(NC_NOERR, nc_get_att_int(f1, NC_GLOBAL, "_IsNetcdf4", &att_val));
    EXPECT_EQ(att_val, 1);
    ASSERT_EQ(NC_NOERR, f1.close());
  }
}

TEST(Nc4HdfTest, CommitTypeAndGroupWriteErrors) {
  TempFile ro_plain_file("nc4hdf_ro_plain.nc");
  EXPECT_EQ(NC_NOERR,
            Nc4HdfTestCommitTypeAndGroupErrors(ro_plain_file.c_str()));

  for (const int subcase : {1, 2, 3}) {
    EXPECT_TRUE(RunInForkedChild([subcase]() {
      return Nc4HdfTestForkedErrorSubcase(subcase) == NC_NOERR;
    })) << "subcase="
        << subcase;
  }
}

TEST(Nc4HdfTest, WriteMetadataAndVarErrorPaths) {
  TempFile ro_attrs_file("nc4hdf_ro_attrs.nc");
  EXPECT_EQ(NC_NOERR,
            Nc4HdfTestWriteMetadataAndVarErrors(ro_attrs_file.c_str()));

  EXPECT_TRUE(RunInForkedChild(
      []() { return Nc4HdfTestForkedErrorSubcase(0) == NC_NOERR; }));
}

TEST(Nc4HdfTest, MatchDimscalesAndIsNetcdf4ErrorPaths) {
  TempFile raw_file("nc4hdf_raw_isnc4.h5");
  EXPECT_EQ(NC_NOERR, Nc4HdfTestMatchDimscalesAndIsNc4Errors(raw_file.c_str()));

  for (const int subcase : {5, 6, 7}) {
    EXPECT_TRUE(RunInForkedChild([subcase]() {
      return Nc4HdfTestForkedErrorSubcase(subcase) == NC_NOERR;
    })) << "subcase="
        << subcase;
  }
}

}  // namespace
}  // namespace netcdf
