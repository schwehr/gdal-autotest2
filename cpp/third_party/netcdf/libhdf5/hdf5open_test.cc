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

// Unit tests for //third_party/netcdf/libhdf5/hdf5open.c.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_format.h"
// clang-format off
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
#include "third_party/netcdf/include/netcdf_filter.h"
// clang-format on
#include "third_party/netcdf/google/libhdf5/hdf5open_shim.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"

namespace netcdf {
namespace {

using ::testing::ElementsAre;

// Restores the global HDF5 metadata block size override on scope exit.
class ScopedMetaBlockSize {
 public:
  ScopedMetaBlockSize() {
    saved_ = (nc_get_meta_block_size(&prev_size_) == NC_NOERR);
  }
  ~ScopedMetaBlockSize() {
    if (saved_) {
      nc_set_meta_block_size(prev_size_);
    }
  }
  ScopedMetaBlockSize(const ScopedMetaBlockSize&) = delete;
  ScopedMetaBlockSize& operator=(const ScopedMetaBlockSize&) = delete;

 private:
  size_t prev_size_ = 0;
  bool saved_ = false;
};

struct SampleCompound {
  int32_t id;
  double score;
};

std::string ReadGlobalTextAttr(int ncid, const char* name) {
  size_t len = 0;
  nc_type xtype = NC_NAT;
  if (nc_inq_att(ncid, NC_GLOBAL, name, &xtype, &len) != NC_NOERR) {
    return "";
  }
  std::string out(len, '\0');
  if (len > 0 &&
      nc_get_att_text(ncid, NC_GLOBAL, name, out.data()) != NC_NOERR) {
    return "";
  }
  return out;
}

bool RunColdInitProbeInChild() {
  ScopedNcFile sc;
  if (nc_create_mem("hdf5open_cold_init.nc", NC_NETCDF4, 4096, sc.ptr()) !=
      NC_NOERR) {
    return false;
  }
  int dimid = -1;
  int varid = -1;
  const int val = 42;
  if (nc_def_dim(sc.get(), "n", 1, &dimid) != NC_NOERR ||
      nc_def_var(sc.get(), "v", NC_INT, 1, &dimid, &varid) != NC_NOERR ||
      nc_put_var_int(sc.get(), varid, &val) != NC_NOERR) {
    return false;
  }
  NC_memio raw_memio{};
  if (nc_close_memio(sc.release(), &raw_memio) != NC_NOERR) {
    return false;
  }
  ScopedMemio memio(raw_memio);
  memio.get()->flags = NC_MEMIO_LOCKED;
  return H5OpenShimProbeDirectOpen(kH5OpenShimDirectColdInit,
                                   "hdf5open_cold_init.nc", NC_NOWRITE,
                                   memio.get()) == NC_NOERR;
}

bool RunAlignmentProbeInChild() {
  TempFile tf("hdf5open_alignment_child.nc");
  {
    ScopedNcFile sc;
    if (nc_create(tf.c_str(), NC_NETCDF4 | NC_CLOBBER, sc.ptr()) != NC_NOERR) {
      return false;
    }
    int dimid = -1;
    int varid = -1;
    const int val = 91;
    if (nc_def_dim(sc.get(), "d", 1, &dimid) != NC_NOERR ||
        nc_def_var(sc.get(), "x", NC_INT, 1, &dimid, &varid) != NC_NOERR ||
        nc_put_var_int(sc.get(), varid, &val) != NC_NOERR ||
        sc.close() != NC_NOERR) {
      return false;
    }
  }

  if (nc_set_alignment(1024, 4096) != NC_NOERR) {
    return false;
  }
  int thresh = 0;
  int align = 0;
  if (nc_get_alignment(&thresh, &align) != NC_NOERR || thresh != 1024 ||
      align != 4096) {
    return false;
  }

  {
    ScopedNcFile sc;
    int varid = -1;
    int got = 0;
    if (nc_open(tf.c_str(), NC_NOWRITE, sc.ptr()) != NC_NOERR ||
        nc_inq_varid(sc.get(), "x", &varid) != NC_NOERR ||
        nc_get_var_int(sc.get(), varid, &got) != NC_NOERR || got != 91) {
      return false;
    }
  }

  // Trigger H5Pset_alignment failure branch with (threshold=0, alignment=0).
  if (nc_set_alignment(0, 0) != NC_NOERR) {
    return false;
  }
  ScopedNcFile bad_sc;
  return nc_open(tf.c_str(), NC_NOWRITE, bad_sc.ptr()) == NC_EHDFERR;
}

TEST(Hdf5OpenTest, OpenAllAtomicTypesFiltersQuantizeChunkingAndClassicFlags) {
  TempFile tf("hdf5open_all_atomics.nc");
  {
    ScopedNcFile sc;
    ASSERT_EQ(NC_NOERR,
              nc_create(tf.c_str(), NC_NETCDF4 | NC_CLOBBER, sc.ptr()));
    const int ncid = sc.get();

    int d2 = -1;
    int d16 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "d2", 2, &d2));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "d16", 16, &d16));

    const signed char g_byte = -12;
    const short g_short = -345;
    const int g_int = -12;
    const int g_int_arr[2] = {100, 200};
    const float g_float = 1.5F;
    const double g_double = -2.75;
    const unsigned char g_ubyte = 200U;
    const unsigned short g_ushort = 45000U;
    const unsigned int g_uint = 3000000000U;
    const long long g_int64 = -5000000000LL;
    const unsigned long long g_uint64 = 9000000000ULL;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_schar(ncid, NC_GLOBAL, "g_byte", NC_BYTE, 1, &g_byte));
    ASSERT_EQ(NC_NOERR, nc_put_att_short(ncid, NC_GLOBAL, "g_short", NC_SHORT,
                                         1, &g_short));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(ncid, NC_GLOBAL, "g_int", NC_INT, 1, &g_int));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(ncid, NC_GLOBAL, "g_int_arr", NC_INT, 2,
                                       g_int_arr));
    ASSERT_EQ(NC_NOERR, nc_put_att_float(ncid, NC_GLOBAL, "g_float", NC_FLOAT,
                                         1, &g_float));
    ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, NC_GLOBAL, "g_double",
                                          NC_DOUBLE, 1, &g_double));
    ASSERT_EQ(NC_NOERR, nc_put_att_uchar(ncid, NC_GLOBAL, "g_ubyte", NC_UBYTE,
                                         1, &g_ubyte));
    ASSERT_EQ(NC_NOERR, nc_put_att_ushort(ncid, NC_GLOBAL, "g_ushort",
                                          NC_USHORT, 1, &g_ushort));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_uint(ncid, NC_GLOBAL, "g_uint", NC_UINT, 1, &g_uint));
    ASSERT_EQ(NC_NOERR, nc_put_att_longlong(ncid, NC_GLOBAL, "g_int64",
                                            NC_INT64, 1, &g_int64));
    ASSERT_EQ(NC_NOERR, nc_put_att_ulonglong(ncid, NC_GLOBAL, "g_uint64",
                                             NC_UINT64, 1, &g_uint64));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_text(ncid, NC_GLOBAL, "g_text", 6, "netcdf"));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(ncid, NC_GLOBAL, "g_empty", NC_INT, 0, nullptr));
    const char* g_strs[2] = {"first", "second"};
    ASSERT_EQ(NC_NOERR,
              nc_put_att_string(ncid, NC_GLOBAL, "g_strs", 2, g_strs));

    int v_char = -1, v_byte = -1, v_short = -1, v_int = -1;
    int v_float = -1, v_double = -1, v_ubyte = -1, v_ushort = -1;
    int v_uint = -1, v_int64 = -1, v_uint64 = -1, v_string = -1;
    int v_compact = -1, v_contig = -1, v_nofill = -1, v_szip = -1;
    int v_bg = -1, v_gbr = -1, v_br = -1, v_noq = -1;

    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_char", NC_CHAR, 1, &d2, &v_char));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_byte", NC_BYTE, 1, &d2, &v_byte));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_short", NC_SHORT, 1, &d2, &v_short));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_int", NC_INT, 1, &d2, &v_int));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_float", NC_FLOAT, 1, &d2, &v_float));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_double", NC_DOUBLE, 1, &d2, &v_double));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_ubyte", NC_UBYTE, 1, &d2, &v_ubyte));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_ushort", NC_USHORT, 1, &d2, &v_ushort));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_uint", NC_UINT, 1, &d2, &v_uint));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_int64", NC_INT64, 1, &d2, &v_int64));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_uint64", NC_UINT64, 1, &d2, &v_uint64));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_string", NC_STRING, 1, &d2, &v_string));

    const size_t chunk2 = 2;
    const int fill_i = -777;
    ASSERT_EQ(NC_NOERR, nc_def_var_endian(ncid, v_int, NC_ENDIAN_BIG));
    ASSERT_EQ(NC_NOERR, nc_def_var_chunking(ncid, v_int, NC_CHUNKED, &chunk2));
    ASSERT_EQ(NC_NOERR, nc_def_var_deflate(ncid, v_int, NC_SHUFFLE, 1, 4));
    ASSERT_EQ(NC_NOERR, nc_def_var_fletcher32(ncid, v_int, NC_FLETCHER32));
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(ncid, v_int, NC_FILL, &fill_i));
    ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_int, "units", 5, "meter"));

    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_compact", NC_INT, 0, nullptr, &v_compact));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_chunking(ncid, v_compact, NC_COMPACT, nullptr));

    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_contig", NC_INT, 1, &d2, &v_contig));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_chunking(ncid, v_contig, NC_CONTIGUOUS, nullptr));

    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_nofill", NC_INT, 1, &d2, &v_nofill));
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(ncid, v_nofill, NC_NOFILL, nullptr));

    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_szip", NC_INT, 1, &d16, &v_szip));
    const size_t chunk16 = 16;
    ASSERT_EQ(NC_NOERR,
              nc_def_var_chunking(ncid, v_szip, NC_CHUNKED, &chunk16));
    ASSERT_EQ(NC_NOERR, nc_def_var_szip(ncid, v_szip, NC_SZIP_EC, 8));

    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_bg", NC_FLOAT, 1, &d2, &v_bg));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_quantize(ncid, v_bg, NC_QUANTIZE_BITGROOM, 3));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_gbr", NC_FLOAT, 1, &d2, &v_gbr));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_quantize(ncid, v_gbr, NC_QUANTIZE_GRANULARBR, 3));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_br", NC_FLOAT, 1, &d2, &v_br));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_quantize(ncid, v_br, NC_QUANTIZE_BITROUND, 8));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_noq", NC_FLOAT, 1, &d2, &v_noq));
    ASSERT_EQ(NC_NOERR, nc_def_var_quantize(ncid, v_noq, NC_NOQUANTIZE, 0));

    ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

    const char chars[2] = {'x', 'y'};
    const signed char bytes[2] = {-42, 17};
    const short shorts[2] = {-100, 200};
    const int ints[2] = {10, 20};
    const float floats[2] = {1.25F, 2.5F};
    const double doubles[2] = {3.125, -4.5};
    const unsigned char ubytes[2] = {10, 250};
    const unsigned short ushorts[2] = {300, 60000};
    const unsigned int uints[2] = {70000U, 90000U};
    const long long int64s[2] = {-5000000000LL, 5000000000LL};
    const unsigned long long uint64s[2] = {10ULL, 9000000000ULL};
    const char* str_vals[2] = {"s0", "s1"};
    const int compact_val = 55;
    std::vector<int> szip_vals(16);
    for (int i = 0; i < 16; ++i) szip_vals[i] = i * 2;
    const float q_vals[2] = {3.14159F, 2.71828F};

    ASSERT_EQ(NC_NOERR, nc_put_var_text(ncid, v_char, chars));
    ASSERT_EQ(NC_NOERR, nc_put_var_schar(ncid, v_byte, bytes));
    ASSERT_EQ(NC_NOERR, nc_put_var_short(ncid, v_short, shorts));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, v_int, ints));
    ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, v_float, floats));
    ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, v_double, doubles));
    ASSERT_EQ(NC_NOERR, nc_put_var_uchar(ncid, v_ubyte, ubytes));
    ASSERT_EQ(NC_NOERR, nc_put_var_ushort(ncid, v_ushort, ushorts));
    ASSERT_EQ(NC_NOERR, nc_put_var_uint(ncid, v_uint, uints));
    ASSERT_EQ(NC_NOERR, nc_put_var_longlong(ncid, v_int64, int64s));
    ASSERT_EQ(NC_NOERR, nc_put_var_ulonglong(ncid, v_uint64, uint64s));
    ASSERT_EQ(NC_NOERR, nc_put_var_string(ncid, v_string, str_vals));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, v_compact, &compact_val));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, v_contig, ints));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, v_nofill, ints));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, v_szip, szip_vals.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, v_bg, q_vals));
    ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, v_gbr, q_vals));
    ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, v_br, q_vals));
    ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, v_noq, q_vals));
    ASSERT_EQ(NC_NOERR, sc.close());
  }

  // Reopen read-only and verify all metadata and values.
  {
    ScopedNcFile sc;
    ASSERT_EQ(NC_NOERR, nc_open(tf.c_str(), NC_NOWRITE, sc.ptr()));
    const int ncid = sc.get();

    int fmt = 0;
    int ext_fmt = 0;
    int ext_mode = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_format(ncid, &fmt));
    ASSERT_EQ(NC_FORMAT_NETCDF4, fmt);
    ASSERT_EQ(NC_NOERR, nc_inq_format_extended(ncid, &ext_fmt, &ext_mode));
    EXPECT_EQ(NC_FORMATX_NC_HDF5, ext_fmt);
    EXPECT_EQ(0, ext_mode & NC_WRITE);

    signed char g_byte = 0;
    short g_short = 0;
    int g_int = 0;
    int g_int_arr[2] = {0, 0};
    float g_float = 0.0F;
    double g_double = 0.0;
    unsigned char g_ubyte = 0;
    unsigned short g_ushort = 0;
    unsigned int g_uint = 0;
    long long g_int64 = 0;
    unsigned long long g_uint64 = 0;
    nc_type att_type = NC_NAT;
    size_t att_len = 0;

    ASSERT_EQ(NC_NOERR, nc_inq_atttype(ncid, NC_GLOBAL, "g_byte", &att_type));
    EXPECT_EQ(NC_BYTE, att_type);
    ASSERT_EQ(NC_NOERR, nc_get_att_schar(ncid, NC_GLOBAL, "g_byte", &g_byte));
    EXPECT_EQ(-12, g_byte);

    ASSERT_EQ(NC_NOERR, nc_inq_atttype(ncid, NC_GLOBAL, "g_short", &att_type));
    EXPECT_EQ(NC_SHORT, att_type);
    ASSERT_EQ(NC_NOERR, nc_get_att_short(ncid, NC_GLOBAL, "g_short", &g_short));
    EXPECT_EQ(-345, g_short);

    ASSERT_EQ(NC_NOERR, nc_get_att_int(ncid, NC_GLOBAL, "g_int", &g_int));
    EXPECT_EQ(-12, g_int);

    ASSERT_EQ(NC_NOERR,
              nc_inq_att(ncid, NC_GLOBAL, "g_int_arr", &att_type, &att_len));
    EXPECT_EQ(NC_INT, att_type);
    EXPECT_EQ(2U, att_len);
    ASSERT_EQ(NC_NOERR,
              nc_get_att_int(ncid, NC_GLOBAL, "g_int_arr", g_int_arr));
    EXPECT_THAT(g_int_arr, ElementsAre(100, 200));

    ASSERT_EQ(NC_NOERR, nc_inq_atttype(ncid, NC_GLOBAL, "g_float", &att_type));
    EXPECT_EQ(NC_FLOAT, att_type);
    ASSERT_EQ(NC_NOERR, nc_get_att_float(ncid, NC_GLOBAL, "g_float", &g_float));
    EXPECT_FLOAT_EQ(1.5F, g_float);

    ASSERT_EQ(NC_NOERR, nc_inq_atttype(ncid, NC_GLOBAL, "g_double", &att_type));
    EXPECT_EQ(NC_DOUBLE, att_type);
    ASSERT_EQ(NC_NOERR,
              nc_get_att_double(ncid, NC_GLOBAL, "g_double", &g_double));
    EXPECT_DOUBLE_EQ(-2.75, g_double);

    ASSERT_EQ(NC_NOERR, nc_inq_atttype(ncid, NC_GLOBAL, "g_ubyte", &att_type));
    EXPECT_EQ(NC_UBYTE, att_type);
    ASSERT_EQ(NC_NOERR, nc_get_att_uchar(ncid, NC_GLOBAL, "g_ubyte", &g_ubyte));
    EXPECT_EQ(200U, g_ubyte);

    ASSERT_EQ(NC_NOERR, nc_inq_atttype(ncid, NC_GLOBAL, "g_ushort", &att_type));
    EXPECT_EQ(NC_USHORT, att_type);
    ASSERT_EQ(NC_NOERR,
              nc_get_att_ushort(ncid, NC_GLOBAL, "g_ushort", &g_ushort));
    EXPECT_EQ(45000U, g_ushort);

    ASSERT_EQ(NC_NOERR, nc_inq_atttype(ncid, NC_GLOBAL, "g_uint", &att_type));
    EXPECT_EQ(NC_UINT, att_type);
    ASSERT_EQ(NC_NOERR, nc_get_att_uint(ncid, NC_GLOBAL, "g_uint", &g_uint));
    EXPECT_EQ(3000000000U, g_uint);

    ASSERT_EQ(NC_NOERR, nc_inq_atttype(ncid, NC_GLOBAL, "g_int64", &att_type));
    EXPECT_EQ(NC_INT64, att_type);
    ASSERT_EQ(NC_NOERR,
              nc_get_att_longlong(ncid, NC_GLOBAL, "g_int64", &g_int64));
    EXPECT_EQ(-5000000000LL, g_int64);

    ASSERT_EQ(NC_NOERR, nc_inq_atttype(ncid, NC_GLOBAL, "g_uint64", &att_type));
    EXPECT_EQ(NC_UINT64, att_type);
    ASSERT_EQ(NC_NOERR,
              nc_get_att_ulonglong(ncid, NC_GLOBAL, "g_uint64", &g_uint64));
    EXPECT_EQ(9000000000ULL, g_uint64);

    EXPECT_EQ("netcdf", ReadGlobalTextAttr(ncid, "g_text"));

    nc_type empty_t = NC_NAT;
    size_t empty_len = 99;
    ASSERT_EQ(NC_NOERR,
              nc_inq_att(ncid, NC_GLOBAL, "g_empty", &empty_t, &empty_len));
    EXPECT_EQ(NC_INT, empty_t);
    EXPECT_EQ(0U, empty_len);

    ScopedNcStrings g_strs(2);
    ASSERT_EQ(NC_NOERR,
              nc_get_att_string(ncid, NC_GLOBAL, "g_strs", g_strs.data()));
    EXPECT_STREQ("first", g_strs[0]);
    EXPECT_STREQ("second", g_strs[1]);

    const char* var_names[12] = {"v_char",  "v_byte",   "v_short",  "v_int",
                                 "v_float", "v_double", "v_ubyte",  "v_ushort",
                                 "v_uint",  "v_int64",  "v_uint64", "v_string"};
    const nc_type expected_types[12] = {
        NC_CHAR,  NC_BYTE,   NC_SHORT, NC_INT,   NC_FLOAT,  NC_DOUBLE,
        NC_UBYTE, NC_USHORT, NC_UINT,  NC_INT64, NC_UINT64, NC_STRING};
    for (int i = 0; i < 12; ++i) {
      int vid = -1;
      nc_type vt = NC_NAT;
      ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, var_names[i], &vid));
      ASSERT_EQ(NC_NOERR, nc_inq_vartype(ncid, vid, &vt));
      EXPECT_EQ(expected_types[i], vt);
    }

    int v_int = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "v_int", &v_int));
    int endian = -1, shuffle = -1, deflate = -1, level = -1, fletcher = -1;
    int no_fill = -1, fill_v = 0, storage = -1;
    size_t chunk_out = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_var_endian(ncid, v_int, &endian));
    EXPECT_EQ(NC_ENDIAN_BIG, endian);
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_deflate(ncid, v_int, &shuffle, &deflate, &level));
    EXPECT_EQ(NC_SHUFFLE, shuffle);
    EXPECT_EQ(1, deflate);
    EXPECT_EQ(4, level);
    ASSERT_EQ(NC_NOERR, nc_inq_var_fletcher32(ncid, v_int, &fletcher));
    EXPECT_EQ(NC_FLETCHER32, fletcher);
    ASSERT_EQ(NC_NOERR, nc_inq_var_fill(ncid, v_int, &no_fill, &fill_v));
    EXPECT_EQ(0, no_fill);
    EXPECT_EQ(-777, fill_v);
    ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(ncid, v_int, &storage, &chunk_out));
    EXPECT_EQ(NC_CHUNKED, storage);
    EXPECT_EQ(2U, chunk_out);

    char units[16]{};
    ASSERT_EQ(NC_NOERR, nc_get_att_text(ncid, v_int, "units", units));
    EXPECT_EQ("meter", std::string_view(units, 5));

    int vid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "v_compact", &vid));
    ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(ncid, vid, &storage, nullptr));
    EXPECT_EQ(NC_COMPACT, storage);
    int compact_val = 0;
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ncid, vid, &compact_val));
    EXPECT_EQ(55, compact_val);

    ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "v_contig", &vid));
    ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(ncid, vid, &storage, nullptr));
    EXPECT_EQ(NC_CONTIGUOUS, storage);

    ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "v_nofill", &vid));
    ASSERT_EQ(NC_NOERR, nc_inq_var_fill(ncid, vid, &no_fill, nullptr));
    EXPECT_EQ(1, no_fill);

    ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "v_szip", &vid));
    int opts = 0, ppb = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_var_szip(ncid, vid, &opts, &ppb));
    EXPECT_EQ(NC_SZIP_EC, opts);
    EXPECT_EQ(8, ppb);

    int qmode = -1, nsd = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "v_bg", &vid));
    ASSERT_EQ(NC_NOERR, nc_inq_var_quantize(ncid, vid, &qmode, &nsd));
    EXPECT_EQ(NC_QUANTIZE_BITGROOM, qmode);
    EXPECT_EQ(3, nsd);

    ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "v_gbr", &vid));
    ASSERT_EQ(NC_NOERR, nc_inq_var_quantize(ncid, vid, &qmode, &nsd));
    EXPECT_EQ(NC_QUANTIZE_GRANULARBR, qmode);
    EXPECT_EQ(3, nsd);

    ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "v_br", &vid));
    ASSERT_EQ(NC_NOERR, nc_inq_var_quantize(ncid, vid, &qmode, &nsd));
    EXPECT_EQ(NC_QUANTIZE_BITROUND, qmode);
    EXPECT_EQ(8, nsd);

    ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "v_noq", &vid));
    ASSERT_EQ(NC_NOERR, nc_inq_var_quantize(ncid, vid, &qmode, &nsd));
    EXPECT_EQ(NC_NOQUANTIZE, qmode);
    EXPECT_EQ(0, nsd);

    signed char bytes_out[2] = {0, 0};
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "v_byte", &vid));
    ASSERT_EQ(NC_NOERR, nc_get_var_schar(ncid, vid, bytes_out));
    EXPECT_EQ(-42, bytes_out[0]);
    EXPECT_EQ(17, bytes_out[1]);

    ScopedNcStrings v_strs(2);
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "v_string", &vid));
    ASSERT_EQ(NC_NOERR, nc_get_var_string(ncid, vid, v_strs.data()));
    EXPECT_STREQ("s0", v_strs[0]);
    EXPECT_STREQ("s1", v_strs[1]);
  }

  // Reopen with NC_WRITE | NC_NOATTCREORD to cover no_attr_create_order mode.
  {
    ScopedNcFile sc;
    ASSERT_EQ(NC_NOERR,
              nc_open(tf.c_str(), NC_WRITE | NC_NOATTCREORD, sc.ptr()));
    int vid = -1;
    int compact_val = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(sc.get(), "v_compact", &vid));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(sc.get(), vid, &compact_val));
    EXPECT_EQ(55, compact_val);
  }

  // NC_CLASSIC_MODEL file creation & reopen.
  TempFile classic_tf("hdf5open_classic_model.nc");
  {
    ScopedNcFile sc;
    ASSERT_EQ(NC_NOERR,
              nc_create(classic_tf.c_str(),
                        NC_NETCDF4 | NC_CLASSIC_MODEL | NC_CLOBBER, sc.ptr()));
    int d = -1, v = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(sc.get(), "x", 2, &d));
    ASSERT_EQ(NC_NOERR, nc_def_var(sc.get(), "y", NC_INT, 1, &d, &v));
    const int vals[2] = {7, 8};
    EXPECT_EQ(NC_EINDEFINE, nc_put_var_int(sc.get(), v, vals));
    ASSERT_EQ(NC_NOERR, nc_enddef(sc.get()));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(sc.get(), v, vals));
    ASSERT_EQ(NC_NOERR, sc.close());
  }
  {
    ScopedNcFile sc;
    ASSERT_EQ(NC_NOERR, nc_open(classic_tf.c_str(), NC_NOWRITE, sc.ptr()));
    int fmt = 0;
    int ext_fmt = 0;
    int ext_mode = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_format(sc.get(), &fmt));
    EXPECT_EQ(NC_FORMAT_NETCDF4_CLASSIC, fmt);
    ASSERT_EQ(NC_NOERR, nc_inq_format_extended(sc.get(), &ext_fmt, &ext_mode));
    EXPECT_EQ(NC_FORMATX_NC_HDF5, ext_fmt);
    EXPECT_NE(0, ext_mode & NC_NETCDF4);
    EXPECT_EQ(0, ext_mode & NC_CLASSIC_MODEL);
    int vid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(sc.get(), "y", &vid));
    int got[2]{};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(sc.get(), vid, got));
    EXPECT_THAT(got, ElementsAre(7, 8));
  }
}

TEST(Hdf5OpenTest, OpenUserTypesOutOfOrderCoordsMultiDimCoordsAndSubgroup) {
  TempFile tf("hdf5open_utypes_coords.nc");
  {
    ScopedNcFile sc;
    ASSERT_EQ(NC_NOERR,
              nc_create(tf.c_str(), NC_NETCDF4 | NC_CLOBBER, sc.ptr()));
    const int ncid = sc.get();

    int dim_b = -1, dim_a = -1, dim_lat = -1, dim_lon = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "b", 2, &dim_b));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "a", 3, &dim_a));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "lat", 2, &dim_lat));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "lon", 2, &dim_lon));

    // Define coordinate vars out of dimension order: a(a) then b(b), plus 2D
    // lat(lat, lon).
    int var_a = -1, var_b = -1, var_lat = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "a", NC_INT, 1, &dim_a, &var_a));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "b", NC_INT, 1, &dim_b, &var_b));
    const int lat_dims[2] = {dim_lat, dim_lon};
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "lat", NC_FLOAT, 2, lat_dims, &var_lat));

    // Parent user types: enum, vlen, opaque, compound.
    nc_type enum_t = NC_NAT, vlen_t = NC_NAT, opaque_t = NC_NAT, cmp_t = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_enum(ncid, NC_INT, "color_t", &enum_t));
    const int c0 = 0, c1 = 1;
    ASSERT_EQ(NC_NOERR, nc_insert_enum(ncid, enum_t, "LOW", &c0));
    ASSERT_EQ(NC_NOERR, nc_insert_enum(ncid, enum_t, "HIGH", &c1));

    ASSERT_EQ(NC_NOERR, nc_def_vlen(ncid, "vlen_i", NC_INT, &vlen_t));
    ASSERT_EQ(NC_NOERR, nc_def_opaque(ncid, 3, "opq3", &opaque_t));
    ASSERT_EQ(NC_NOERR,
              nc_def_compound(ncid, sizeof(SampleCompound), "rec_t", &cmp_t));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(ncid, cmp_t, "id",
                                 offsetof(SampleCompound, id), NC_INT));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(ncid, cmp_t, "score",
                                 offsetof(SampleCompound, score), NC_DOUBLE));

    int subgid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(ncid, "sub", &subgid));
    int v_enum = -1, v_vlen = -1, v_opq = -1, v_cmp = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(subgid, "ve", enum_t, 1, &dim_b, &v_enum));
    ASSERT_EQ(NC_NOERR, nc_def_var(subgid, "vv", vlen_t, 1, &dim_b, &v_vlen));
    ASSERT_EQ(NC_NOERR, nc_def_var(subgid, "vo", opaque_t, 0, nullptr, &v_opq));
    ASSERT_EQ(NC_NOERR, nc_def_var(subgid, "vc", cmp_t, 0, nullptr, &v_cmp));

    const int b_vals[2] = {10, 20};
    const int a_vals[3] = {1, 2, 3};
    const float lat_vals[4] = {10.0F, 11.0F, 20.0F, 21.0F};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, var_b, b_vals));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, var_a, a_vals));
    ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, var_lat, lat_vals));

    const int evals[2] = {c1, c0};
    int vl0[2] = {3, 4};
    int vl1[3] = {5, 6, 7};
    nc_vlen_t vlen_vals[2] = {{2, vl0}, {3, vl1}};
    const unsigned char opq_val[3] = {0xAA, 0xBB, 0xCC};
    SampleCompound cmp_val{};
    cmp_val.id = 42;
    cmp_val.score = 9.5;
    ASSERT_EQ(NC_NOERR, nc_put_var(subgid, v_enum, evals));
    ASSERT_EQ(NC_NOERR, nc_put_var(subgid, v_vlen, vlen_vals));
    ASSERT_EQ(NC_NOERR, nc_put_var(subgid, v_opq, opq_val));
    ASSERT_EQ(NC_NOERR, nc_put_var(subgid, v_cmp, &cmp_val));
    ASSERT_EQ(NC_NOERR, sc.close());
  }

  ScopedNcFile sc;
  ASSERT_EQ(NC_NOERR, nc_open(tf.c_str(), NC_NOWRITE, sc.ptr()));
  const int ncid = sc.get();

  int dim_b = -1, dim_a = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(ncid, "b", &dim_b));
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(ncid, "a", &dim_a));
  EXPECT_EQ(0, dim_b);
  EXPECT_EQ(1, dim_a);
  int var_b = -1, var_a = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "b", &var_b));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "a", &var_a));
  int b_out[2]{}, a_out[3]{};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(ncid, var_b, b_out));
  ASSERT_EQ(NC_NOERR, nc_get_var_int(ncid, var_a, a_out));
  EXPECT_THAT(b_out, ElementsAre(10, 20));
  EXPECT_THAT(a_out, ElementsAre(1, 2, 3));

  int var_lat = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "lat", &var_lat));
  int ndims = 0;
  int lat_dimids[2] = {-1, -1};
  ASSERT_EQ(NC_NOERR, nc_inq_varndims(ncid, var_lat, &ndims));
  EXPECT_EQ(2, ndims);
  ASSERT_EQ(NC_NOERR, nc_inq_vardimid(ncid, var_lat, lat_dimids));
  char dname0[NC_MAX_NAME + 1]{};
  char dname1[NC_MAX_NAME + 1]{};
  ASSERT_EQ(NC_NOERR, nc_inq_dimname(ncid, lat_dimids[0], dname0));
  ASSERT_EQ(NC_NOERR, nc_inq_dimname(ncid, lat_dimids[1], dname1));
  EXPECT_STREQ("lat", dname0);
  EXPECT_STREQ("lon", dname1);

  int subgid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_ncid(ncid, "sub", &subgid));
  int v_enum = -1, v_vlen = -1, v_opq = -1, v_cmp = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(subgid, "ve", &v_enum));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(subgid, "vv", &v_vlen));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(subgid, "vo", &v_opq));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(subgid, "vc", &v_cmp));

  int evals_out[2] = {-1, -1};
  ASSERT_EQ(NC_NOERR, nc_get_var(subgid, v_enum, evals_out));
  EXPECT_THAT(evals_out, ElementsAre(1, 0));

  ScopedNcVlens vlens_out(2);
  ASSERT_EQ(NC_NOERR, nc_get_var(subgid, v_vlen, vlens_out.data()));
  EXPECT_EQ(2U, vlens_out[0].len);
  EXPECT_EQ(3U, vlens_out[1].len);

  unsigned char opq_out[3]{};
  ASSERT_EQ(NC_NOERR, nc_get_var(subgid, v_opq, opq_out));
  EXPECT_THAT(opq_out, ElementsAre(0xAA, 0xBB, 0xCC));

  SampleCompound cmp_out{};
  ASSERT_EQ(NC_NOERR, nc_get_var(subgid, v_cmp, &cmp_out));
  EXPECT_EQ(42, cmp_out.id);
  EXPECT_DOUBLE_EQ(9.5, cmp_out.score);
}

TEST(Hdf5OpenTest, OpenDisklessPersistAndInMemoryOwnershipModes) {
  TempFile tf("hdf5open_diskless.nc");
  {
    ScopedNcFile sc;
    ASSERT_EQ(NC_NOERR,
              nc_create(tf.c_str(), NC_NETCDF4 | NC_CLOBBER, sc.ptr()));
    int d = -1, v = -1;
    const int init_v = 11;
    ASSERT_EQ(NC_NOERR, nc_def_dim(sc.get(), "n", 1, &d));
    ASSERT_EQ(NC_NOERR, nc_def_var(sc.get(), "k", NC_INT, 1, &d, &v));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(sc.get(), v, &init_v));
    ASSERT_EQ(NC_NOERR, sc.close());
  }

  // NC_DISKLESS read-only open.
  {
    ScopedNcFile sc;
    ASSERT_EQ(NC_NOERR,
              nc_open(tf.c_str(), NC_NOWRITE | NC_DISKLESS, sc.ptr()));
    int ext_fmt = 0, ext_mode = 0, vid = -1, got = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_format_extended(sc.get(), &ext_fmt, &ext_mode));
    EXPECT_NE(0, ext_mode & NC_DISKLESS);
    ASSERT_EQ(NC_NOERR, nc_inq_varid(sc.get(), "k", &vid));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(sc.get(), vid, &got));
    EXPECT_EQ(11, got);
  }

  // NC_DISKLESS | NC_PERSIST | NC_WRITE updates file on close.
  {
    ScopedNcFile sc;
    ASSERT_EQ(NC_NOERR, nc_open(tf.c_str(), NC_WRITE | NC_DISKLESS | NC_PERSIST,
                                sc.ptr()));
    int vid = -1;
    const int updated = 99;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(sc.get(), "k", &vid));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(sc.get(), vid, &updated));
    ASSERT_EQ(NC_NOERR, sc.close());
  }
  {
    ScopedNcFile sc;
    ASSERT_EQ(NC_NOERR, nc_open(tf.c_str(), NC_NOWRITE, sc.ptr()));
    int vid = -1, got = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(sc.get(), "k", &vid));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(sc.get(), vid, &got));
    EXPECT_EQ(99, got);
  }

  // Build an in-memory NetCDF-4 image for nc_open_mem and NC_ECANTWRITE probes.
  ScopedMemio base_mem;
  {
    ScopedNcFile sc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("mem_base.nc", NC_NETCDF4, 4096, sc.ptr()));
    int d = -1, v = -1;
    const int val = 123;
    ASSERT_EQ(NC_NOERR, nc_def_dim(sc.get(), "n", 1, &d));
    ASSERT_EQ(NC_NOERR, nc_def_var(sc.get(), "m", NC_INT, 1, &d, &v));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(sc.get(), v, &val));
    NC_memio raw{};
    ASSERT_EQ(NC_NOERR, nc_close_memio(sc.release(), &raw));
    base_mem = ScopedMemio(raw);
  }

  // Read-only nc_open_mem.
  {
    ScopedNcFile sc;
    ASSERT_EQ(NC_NOERR, nc_open_mem("mem_ro.nc", NC_NOWRITE, base_mem.size(),
                                    base_mem.get()->memory, sc.ptr()));
    int vid = -1, got = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(sc.get(), "m", &vid));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(sc.get(), vid, &got));
    EXPECT_EQ(123, got);
  }

  // In-memory files created via nc_create_mem use H5P_DEFAULT for the root
  // group (no H5P_CRT_ORDER_TRACKED), so NC_WRITE reopen fails cleanly.
  {
    std::vector<unsigned char> mem_copy(base_mem.size());
    std::memcpy(mem_copy.data(), base_mem.bytes(), base_mem.size());
    NC_memio ro_locked{mem_copy.size(), mem_copy.data(), NC_MEMIO_LOCKED};
    ScopedNcFile sc;
    EXPECT_EQ(NC_ECANTWRITE, nc_open_memio("mem_base_cantwrite.nc", NC_WRITE,
                                           &ro_locked, sc.ptr()));
  }

  // Build a disk NetCDF-4 image (which sets H5P_CRT_ORDER_TRACKED on '/') for
  // writable nc_open_memio ownership tests.
  std::vector<unsigned char> disk_bytes;
  {
    TempFile disk_tf("hdf5open_mem_src.nc");
    {
      ScopedNcFile sc;
      ASSERT_EQ(NC_NOERR,
                nc_create(disk_tf.c_str(), NC_NETCDF4 | NC_CLOBBER, sc.ptr()));
      int d = -1, v = -1;
      const int val = 123;
      ASSERT_EQ(NC_NOERR, nc_def_dim(sc.get(), "n", 1, &d));
      ASSERT_EQ(NC_NOERR, nc_def_var(sc.get(), "m", NC_INT, 1, &d, &v));
      ASSERT_EQ(NC_NOERR, nc_put_var_int(sc.get(), v, &val));
      ASSERT_EQ(NC_NOERR, sc.close());
    }
    std::ifstream ifs(disk_tf.c_str(), std::ios::binary | std::ios::ate);
    ASSERT_TRUE(ifs.is_open());
    const std::streamsize sz = ifs.tellg();
    ASSERT_GT(sz, 0);
    ifs.seekg(0, std::ios::beg);
    disk_bytes.resize(static_cast<size_t>(sz));
    ASSERT_TRUE(
        ifs.read(reinterpret_cast<char*>(disk_bytes.data()), sz).good());
  }

  // Locked NC_WRITE nc_open_memio keeps caller buffer intact.
  // NC4_image_init sets H5Pset_fapl_core increment = max(65536, 0.1 * size), so
  // HDF5 rounds EOA up to 65536 on flush/close; pre-size the locked RW image
  // to kMinCoreIncrement because NC_MEMIO_LOCKED forbids reallocation growth.
  {
    constexpr size_t kMinCoreIncrement = 65536;
    ASSERT_LE(disk_bytes.size(), kMinCoreIncrement);
    std::vector<unsigned char> locked_copy(kMinCoreIncrement, 0);
    std::memcpy(locked_copy.data(), disk_bytes.data(), disk_bytes.size());
    NC_memio locked_mem{locked_copy.size(), locked_copy.data(),
                        NC_MEMIO_LOCKED};
    ScopedNcFile sc;
    ASSERT_EQ(NC_NOERR,
              nc_open_memio("mem_locked.nc", NC_WRITE, &locked_mem, sc.ptr()));
    EXPECT_EQ(locked_copy.data(), locked_mem.memory);
    int vid = -1;
    int got = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(sc.get(), "m", &vid));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(sc.get(), vid, &got));
    EXPECT_EQ(123, got);
    const int updated = 321;
    ASSERT_EQ(NC_NOERR, nc_put_var_int(sc.get(), vid, &updated));
    ASSERT_EQ(NC_NOERR, sc.close());

    ScopedNcFile sc_verify;
    ASSERT_EQ(NC_NOERR, nc_open_mem("mem_locked_verify.nc", NC_NOWRITE,
                                    locked_copy.size(), locked_copy.data(),
                                    sc_verify.ptr()));
    int verified = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(sc_verify.get(), "m", &vid));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(sc_verify.get(), vid, &verified));
    EXPECT_EQ(321, verified);
  }

  // Unlocked NC_WRITE nc_open_memio takes ownership of malloc'd memory.
  {
    void* owned_buf = std::malloc(disk_bytes.size());
    ASSERT_NE(nullptr, owned_buf);
    const auto free_owned_buf =
        absl::MakeCleanup([&owned_buf] { std::free(owned_buf); });
    std::memcpy(owned_buf, disk_bytes.data(), disk_bytes.size());
    NC_memio write_mem{disk_bytes.size(), owned_buf, 0};

    ScopedNcFile sc;
    const int open_status =
        nc_open_memio("mem_write.nc", NC_WRITE, &write_mem, sc.ptr());
    if (write_mem.memory == nullptr) {
      owned_buf = nullptr;
    }
    ASSERT_EQ(NC_NOERR, open_status);
    EXPECT_EQ(nullptr, write_mem.memory);
    EXPECT_EQ(0U, write_mem.size);

    int vid = -1;
    const int updated = 456;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(sc.get(), "m", &vid));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(sc.get(), vid, &updated));
    NC_memio closed_raw{};
    ASSERT_EQ(NC_NOERR, nc_close_memio(sc.release(), &closed_raw));
    ScopedMemio closed_mem(closed_raw);

    ScopedNcFile sc_reopen;
    ASSERT_EQ(NC_NOERR,
              nc_open_mem("mem_verify.nc", NC_NOWRITE, closed_mem.size(),
                          closed_mem.get()->memory, sc_reopen.ptr()));
    int got = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(sc_reopen.get(), "m", &vid));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(sc_reopen.get(), vid, &got));
    EXPECT_EQ(456, got);
  }
}

TEST(Hdf5OpenTest, OpenRawHdf5WithCreationOrderAndClassicAttr) {
  TempFile tf("hdf5open_raw_crt.h5");
  ASSERT_EQ(0, H5OpenShimCreateRawHdf5File(tf.c_str(), /*use_crt_order=*/1,
                                           /*include_classic_attr=*/1));

  ScopedNcFile sc;
  ASSERT_EQ(NC_NOERR, nc_open(tf.c_str(), NC_NOWRITE, sc.ptr()));
  const int ncid = sc.get();

  int fmt = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_format(ncid, &fmt));
  EXPECT_EQ(NC_FORMAT_NETCDF4_CLASSIC, fmt);

  int ext_fmt = 0, ext_mode = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_format_extended(ncid, &ext_fmt, &ext_mode));
  EXPECT_EQ(NC_FORMATX_NC_HDF5, ext_fmt);
  EXPECT_NE(0, ext_mode & NC_NETCDF4);
  EXPECT_EQ(0, ext_mode & NC_CLASSIC_MODEL);

  // Reference and bitfield variables and reference attributes are skipped.
  int unused_id = -1;
  EXPECT_EQ(NC_ENOTVAR, nc_inq_varid(ncid, "ignored_ref_var", &unused_id));
  EXPECT_EQ(NC_ENOTVAR, nc_inq_varid(ncid, "ignored_bit_var", &unused_id));
  EXPECT_EQ(NC_ENOTVAR, nc_inq_varid(ncid, "unlim_no_var", &unused_id));
  EXPECT_EQ(NC_ENOTATT,
            nc_inq_attid(ncid, NC_GLOBAL, "global_ref_ignored", &unused_id));

  // Global attributes (scalar, null, fixed-char, fixed-str-array, vlen-str,
  // enum).
  int g_scalar = 0;
  ASSERT_EQ(NC_NOERR,
            nc_get_att_int(ncid, NC_GLOBAL, "global_scalar_i32", &g_scalar));
  EXPECT_EQ(123, g_scalar);

  size_t zero_len = 99;
  ASSERT_EQ(NC_NOERR,
            nc_inq_attlen(ncid, NC_GLOBAL, "global_zero_len", &zero_len));
  EXPECT_EQ(0U, zero_len);

  EXPECT_EQ("hello", ReadGlobalTextAttr(ncid, "global_fixed_char"));

  {
    ScopedNcStrings fixed_strs(2);
    ASSERT_EQ(NC_NOERR,
              nc_get_att_string(ncid, NC_GLOBAL, "global_fixed_str_arr",
                                fixed_strs.data()));
    EXPECT_STREQ("alpha", fixed_strs[0]);
    EXPECT_STREQ("beta", fixed_strs[1]);
  }
  {
    ScopedNcStrings vlen_strs(2);
    ASSERT_EQ(NC_NOERR, nc_get_att_string(ncid, NC_GLOBAL, "global_vlen_str",
                                          vlen_strs.data()));
    EXPECT_STREQ("one", vlen_strs[0]);
    EXPECT_STREQ("two", vlen_strs[1]);
  }

  nc_type enum_att_type = NC_NAT;
  int enum_att_val = -1;
  ASSERT_EQ(NC_NOERR,
            nc_inq_atttype(ncid, NC_GLOBAL, "global_enum_att", &enum_att_type));
  ASSERT_EQ(NC_NOERR,
            nc_get_att(ncid, NC_GLOBAL, "global_enum_att", &enum_att_val));
  EXPECT_EQ(2, enum_att_val);

  // Dimension scales, non-coord variable name unwrapping, and coord hint.
  int coord_x_dimid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(ncid, "coord_x", &coord_x_dimid));
  EXPECT_EQ(5, coord_x_dimid);
  int coord_x_vid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "coord_x", &coord_x_vid));
  int coord_x_vals[3]{};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(ncid, coord_x_vid, coord_x_vals));
  EXPECT_THAT(coord_x_vals, ElementsAre(10, 20, 30));

  int unlim_dimid = -1;
  size_t unlim_len = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(ncid, "unlim_no_var", &unlim_dimid));
  ASSERT_EQ(NC_NOERR, nc_inq_dimlen(ncid, unlim_dimid, &unlim_len));
  EXPECT_EQ(3U, unlim_len);
  int unlim_data_vid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "unlim_data", &unlim_data_vid));
  int unlim_data_vals[3]{};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(ncid, unlim_data_vid, unlim_data_vals));
  EXPECT_THAT(unlim_data_vals, ElementsAre(100, 200, 300));

  int pure_dimid = -1;
  size_t pure_len = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(ncid, "pure_dim", &pure_dimid));
  ASSERT_EQ(NC_NOERR, nc_inq_dimlen(ncid, pure_dimid, &pure_len));
  EXPECT_EQ(2U, pure_len);
  int non_coord_vid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "pure_dim", &non_coord_vid));
  int non_coord_dimid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_vardimid(ncid, non_coord_vid, &non_coord_dimid));
  EXPECT_EQ(5, non_coord_dimid);
  int non_coord_vals[3]{};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(ncid, non_coord_vid, non_coord_vals));
  EXPECT_THAT(non_coord_vals, ElementsAre(7, 8, 9));

  int coord_hint_vid = -1;
  int coord_hint_dimid = -1;
  int hint_storage = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "coord_hint_var", &coord_hint_vid));
  ASSERT_EQ(NC_NOERR, nc_inq_vardimid(ncid, coord_hint_vid, &coord_hint_dimid));
  EXPECT_EQ(5, coord_hint_dimid);
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_chunking(ncid, coord_hint_vid, &hint_storage, nullptr));

  // Big-endian chunked deflated var, variable attrs, and second
  // nc4_get_var_meta probe.
  int be_vid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "be_deflate_var", &be_vid));
  int endian = -1, shuffle = -1, deflate = -1, level = -1, no_fill = -1,
      fill_v = 0, storage = -1;
  size_t chunk_out = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_var_endian(ncid, be_vid, &endian));
  EXPECT_EQ(NC_ENDIAN_BIG, endian);
  ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(ncid, be_vid, &storage, &chunk_out));
  EXPECT_EQ(NC_CHUNKED, storage);
  EXPECT_EQ(3U, chunk_out);
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_deflate(ncid, be_vid, &shuffle, &deflate, &level));
  EXPECT_EQ(1, deflate);
  EXPECT_EQ(3, level);
  ASSERT_EQ(NC_NOERR, nc_inq_var_fill(ncid, be_vid, &no_fill, &fill_v));
  EXPECT_EQ(0, no_fill);
  EXPECT_EQ(-99, fill_v);
  ASSERT_EQ(NC_NOERR, H5OpenShimProbeGetVarMetaTwice(ncid, be_vid));
  int be_vals[3]{};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(ncid, be_vid, be_vals));
  EXPECT_THAT(be_vals, ElementsAre(11, 22, 33));
  EXPECT_EQ(NC_ENOTATT,
            nc_inq_attid(ncid, be_vid, "v_ref_ignored", &unused_id));
  int v_int_att = 0;
  ASSERT_EQ(NC_NOERR, nc_get_att_int(ncid, be_vid, "v_int", &v_int_att));
  EXPECT_EQ(42, v_int_att);
  {
    ScopedNcStrings v_fstrs(2);
    ASSERT_EQ(NC_NOERR, nc_get_att_string(ncid, be_vid, "v_fixed_str_arr",
                                          v_fstrs.data()));
    EXPECT_STREQ("vx", v_fstrs[0]);
    EXPECT_STREQ("vy", v_fstrs[1]);
  }

  // SZIP, compact, virtual (NC_UNKNOWN_STORAGE), contiguous, and phony dims.
  int vid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "szip_var", &vid));
  int szip_mask = 0, szip_ppb = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_var_szip(ncid, vid, &szip_mask, &szip_ppb));
  EXPECT_EQ(NC_SZIP_NN, szip_mask);
  EXPECT_EQ(8, szip_ppb);
  std::vector<int> szip_vals(16, 0);
  ASSERT_EQ(NC_NOERR, nc_get_var_int(ncid, vid, szip_vals.data()));
  for (int i = 0; i < 16; ++i) EXPECT_EQ(i, szip_vals[i]);

  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "compact_i32", &vid));
  ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(ncid, vid, &storage, nullptr));
  EXPECT_EQ(NC_COMPACT, storage);
  int compact_val = 0;
  ASSERT_EQ(NC_NOERR, nc_get_var_int(ncid, vid, &compact_val));
  EXPECT_EQ(77, compact_val);

  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "virtual_i32", &vid));
  ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(ncid, vid, &storage, nullptr));
  EXPECT_EQ(NC_UNKNOWN_STORAGE, storage);

  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "contig_i32", &vid));
  ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(ncid, vid, &storage, nullptr));
  EXPECT_EQ(NC_CONTIGUOUS, storage);

  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "square_no_scale", &vid));
  int sq_dims[2] = {-1, -1};
  ASSERT_EQ(NC_NOERR, nc_inq_vardimid(ncid, vid, sq_dims));
  EXPECT_NE(sq_dims[0], sq_dims[1]);

  int uphony1 = -1, uphony2 = -1, fphony = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "unlim_phony_var", &uphony1));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "unlim_phony_var2", &uphony2));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "fixed_len5_var", &fphony));
  int udim1 = -1, udim2 = -1, fdim = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_vardimid(ncid, uphony1, &udim1));
  ASSERT_EQ(NC_NOERR, nc_inq_vardimid(ncid, uphony2, &udim2));
  ASSERT_EQ(NC_NOERR, nc_inq_vardimid(ncid, fphony, &fdim));
  EXPECT_EQ(udim1, udim2);
  EXPECT_NE(udim1, fdim);

  // Fixed-length string dataset (>1 byte -> NC_STRING) & 1-byte NC_CHAR.
  {
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "fixed_str_var", &vid));
    nc_type stype = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_inq_vartype(ncid, vid, &stype));
    EXPECT_EQ(NC_STRING, stype);
    const size_t start = 0;
    const size_t count = 2;
    ScopedNcStrings fstrs(2);
    ASSERT_EQ(NC_NOERR,
              nc_get_vara_string(ncid, vid, &start, &count, fstrs.data()));
    EXPECT_STREQ("alpha", fstrs[0]);
    EXPECT_STREQ("beta", fstrs[1]);
  }
  {
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "fixed_char_var", &vid));
    nc_type ctype = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_inq_vartype(ncid, vid, &ctype));
    EXPECT_EQ(NC_CHAR, ctype);
    char cbuf[4]{};
    ASSERT_EQ(NC_NOERR, nc_get_var_text(ncid, vid, cbuf));
    EXPECT_EQ("abcd", std::string_view(cbuf, 4));
  }

  // Committed named types including committed NC_STRING ("named_str").
  nc_type named_str_tid = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(ncid, "named_str", &named_str_tid));
  int named_str_class = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_user_type(ncid, named_str_tid, nullptr, nullptr,
                                       nullptr, nullptr, &named_str_class));
  EXPECT_EQ(NC_STRING, named_str_class);

  // Transient/anonymous variables.
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "anon_enum_var", &vid));
  uint16_t anon_eval = 0;
  ASSERT_EQ(NC_NOERR, nc_get_var(ncid, vid, &anon_eval));
  EXPECT_EQ(20U, anon_eval);

  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "anon_vlen_var", &vid));
  {
    ScopedNcVlens anon_vlens(1);
    ASSERT_EQ(NC_NOERR, nc_get_var(ncid, vid, anon_vlens.data()));
    EXPECT_EQ(2U, anon_vlens[0].len);
  }

  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "anon_opaque_var", &vid));
  unsigned char anon_opq[8]{};
  ASSERT_EQ(NC_NOERR, nc_get_var(ncid, vid, anon_opq));
  EXPECT_EQ(1U, anon_opq[0]);
  EXPECT_EQ(8U, anon_opq[7]);

  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "anon_cmp_var", &vid));
  nc_type anon_cmp_tid = NC_NAT;
  size_t anon_cmp_sz = 0, anon_nfields = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_vartype(ncid, vid, &anon_cmp_tid));
  ASSERT_EQ(NC_NOERR, nc_inq_compound(ncid, anon_cmp_tid, nullptr, &anon_cmp_sz,
                                      &anon_nfields));
  EXPECT_EQ(2U, anon_nfields);
  std::vector<unsigned char> anon_cmp_buf(anon_cmp_sz, 0);
  ASSERT_EQ(NC_NOERR, nc_get_var(ncid, vid, anon_cmp_buf.data()));
  size_t off_x = 0, off_y = 0;
  ASSERT_EQ(NC_NOERR,
            nc_inq_compound_fieldoffset(ncid, anon_cmp_tid, 0, &off_x));
  ASSERT_EQ(NC_NOERR,
            nc_inq_compound_fieldoffset(ncid, anon_cmp_tid, 1, &off_y));
  float fx = 0.0F;
  int64_t iy = 0;
  std::memcpy(&fx, anon_cmp_buf.data() + off_x, sizeof(fx));
  std::memcpy(&iy, anon_cmp_buf.data() + off_y, sizeof(iy));
  EXPECT_FLOAT_EQ(1.5F, fx);
  EXPECT_EQ(999, iy);

  // Subgroup reusing parent dimscale and parent committed compound type.
  int subgid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_ncid(ncid, "subgrp", &subgid));
  int sub_dim_vid = -1, sub_dimid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(subgid, "sub_parent_dim_var", &sub_dim_vid));
  ASSERT_EQ(NC_NOERR, nc_inq_vardimid(subgid, sub_dim_vid, &sub_dimid));
  EXPECT_EQ(5, sub_dimid);
  int sub_vals[3]{};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(subgid, sub_dim_vid, sub_vals));
  EXPECT_THAT(sub_vals, ElementsAre(101, 102, 103));

  int sub_cmp_vid = -1;
  nc_type sub_cmp_tid = NC_NAT;
  size_t sub_cmp_sz = 0, sub_nfields = 0;
  ASSERT_EQ(NC_NOERR,
            nc_inq_varid(subgid, "sub_parent_type_var", &sub_cmp_vid));
  ASSERT_EQ(NC_NOERR, nc_inq_vartype(subgid, sub_cmp_vid, &sub_cmp_tid));
  ASSERT_EQ(NC_NOERR, nc_inq_compound(subgid, sub_cmp_tid, nullptr, &sub_cmp_sz,
                                      &sub_nfields));
  EXPECT_EQ(4U, sub_nfields);
  std::vector<unsigned char> sub_cmp_buf(sub_cmp_sz, 0);
  ASSERT_EQ(NC_NOERR, nc_get_var(subgid, sub_cmp_vid, sub_cmp_buf.data()));
  absl::Cleanup reclaim_sub_cmp = [&]() {
    EXPECT_EQ(NC_NOERR,
              nc_reclaim_data(ncid, sub_cmp_tid, sub_cmp_buf.data(), 1));
  };
  size_t off_a = 0, off_b = 0, off_arr = 0, off_str = 0;
  ASSERT_EQ(NC_NOERR,
            nc_inq_compound_fieldoffset(subgid, sub_cmp_tid, 0, &off_a));
  ASSERT_EQ(NC_NOERR,
            nc_inq_compound_fieldoffset(subgid, sub_cmp_tid, 1, &off_b));
  ASSERT_EQ(NC_NOERR,
            nc_inq_compound_fieldoffset(subgid, sub_cmp_tid, 2, &off_arr));
  ASSERT_EQ(NC_NOERR,
            nc_inq_compound_fieldoffset(subgid, sub_cmp_tid, 3, &off_str));
  int32_t fa = 0;
  double fb = 0.0;
  int32_t farr[6]{};
  const char* fstr = nullptr;
  std::memcpy(&fa, sub_cmp_buf.data() + off_a, sizeof(fa));
  std::memcpy(&fb, sub_cmp_buf.data() + off_b, sizeof(fb));
  std::memcpy(farr, sub_cmp_buf.data() + off_arr, sizeof(farr));
  std::memcpy(&fstr, sub_cmp_buf.data() + off_str, sizeof(fstr));
  EXPECT_EQ(7, fa);
  EXPECT_DOUBLE_EQ(2.25, fb);
  EXPECT_THAT(farr, ElementsAre(1, 2, 3, 4, 5, 6));
  EXPECT_STREQ("sub_hi", fstr);
}

TEST(Hdf5OpenTest,
     OpenRawHdf5WithoutCreationOrderWorksReadOnlyAndRejectsWrite) {
  TempFile tf("hdf5open_raw_nocrt.h5");
  ASSERT_EQ(0, H5OpenShimCreateRawHdf5File(tf.c_str(), /*use_crt_order=*/0,
                                           /*include_classic_attr=*/0));

  ScopedNcFile rw_sc;
  EXPECT_EQ(NC_ECANTWRITE, nc_open(tf.c_str(), NC_WRITE, rw_sc.ptr()));

  ScopedNcFile sc;
  ASSERT_EQ(NC_NOERR, nc_open(tf.c_str(), NC_NOWRITE, sc.ptr()));
  const int ncid = sc.get();

  int ext_fmt = 0, ext_mode = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_format_extended(ncid, &ext_fmt, &ext_mode));
  EXPECT_EQ(NC_FORMATX_NC_HDF5, ext_fmt);
  EXPECT_EQ(0, ext_mode & NC_CLASSIC_MODEL);

  int uphony1 = -1, uphony2 = -1, fphony = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "unlim_phony_var", &uphony1));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "unlim_phony_var2", &uphony2));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "fixed_len5_var", &fphony));
  int udim1 = -1, udim2 = -1, fdim = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_vardimid(ncid, uphony1, &udim1));
  ASSERT_EQ(NC_NOERR, nc_inq_vardimid(ncid, uphony2, &udim2));
  ASSERT_EQ(NC_NOERR, nc_inq_vardimid(ncid, fphony, &fdim));
  EXPECT_EQ(udim1, udim2);
  EXPECT_NE(udim1, fdim);

  int subgid = -1, sub_dim_vid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_ncid(ncid, "subgrp", &subgid));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(subgid, "sub_parent_dim_var", &sub_dim_vid));
  int sub_dimid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_vardimid(subgid, sub_dim_vid, &sub_dimid));
  EXPECT_EQ(5, sub_dimid);
}

TEST(Hdf5OpenTest, OpenBadFiltersAndMissingFilters) {
  TempFile tf("hdf5open_bad_filters.h5");
  ASSERT_EQ(0, H5OpenShimCreateBadFilterFile(tf.c_str()));

  ScopedNcFile sc;
  ASSERT_EQ(NC_NOERR, nc_open(tf.c_str(), NC_NOWRITE, sc.ptr()));
  const int ncid = sc.get();

  int v_noparams = -1, v_params = -1, v_bad_nelems = -1, v_bad_level = -1;
  int v_mand_vlen = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "unreg_noparams", &v_noparams));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "unreg_withparams", &v_params));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "bad_deflate_nelems", &v_bad_nelems));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "bad_deflate_level", &v_bad_level));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "mandatory_vlen_str", &v_mand_vlen));

  size_t nfilters = 0;
  unsigned int fid = 0;
  size_t nparams = 99;
  ASSERT_EQ(NC_NOERR, nc_inq_var_filter_ids(ncid, v_noparams, &nfilters, &fid));
  EXPECT_EQ(1U, nfilters);
  EXPECT_EQ(400U, fid);
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_filter_info(ncid, v_noparams, 400U, &nparams, nullptr));
  EXPECT_EQ(0U, nparams);
  EXPECT_EQ(NC_ENOFILTER, nc_inq_filter_avail(ncid, 400U));
  int buf[4]{};
  EXPECT_EQ(NC_ENOFILTER, nc_get_var_int(ncid, v_noparams, buf));

  unsigned int fparams[2]{};
  ASSERT_EQ(NC_NOERR, nc_inq_var_filter_ids(ncid, v_params, &nfilters, &fid));
  EXPECT_EQ(1U, nfilters);
  EXPECT_EQ(401U, fid);
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_filter_info(ncid, v_params, 401U, &nparams, fparams));
  EXPECT_EQ(2U, nparams);
  EXPECT_THAT(fparams, ElementsAre(7U, 9U));
  EXPECT_EQ(NC_ENOFILTER, nc_inq_filter_avail(ncid, 401U));
  EXPECT_EQ(NC_ENOFILTER, nc_get_var_int(ncid, v_params, buf));

  int shuf = 0, defl = 0, lvl = 0;
  EXPECT_EQ(NC_EHDFERR,
            nc_inq_var_deflate(ncid, v_bad_nelems, &shuf, &defl, &lvl));
  EXPECT_EQ(NC_EHDFERR,
            nc_inq_var_deflate(ncid, v_bad_level, &shuf, &defl, &lvl));

  int fletcher = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_var_fletcher32(ncid, v_mand_vlen, &fletcher));
  EXPECT_EQ(NC_FLETCHER32, fletcher);
  ScopedNcStrings mand_strs(4);
  // HDF5 defines H5Z_FLAG_MANDATORY as 0x0000, so hdf5open.c's
  // `(flags & H5Z_FLAG_MANDATORY) != 0` guard does not mark Fletcher32 on
  // vlen string vars as missing; reading succeeds cleanly.
  EXPECT_EQ(NC_NOERR, nc_get_var_string(ncid, v_mand_vlen, mand_strs.data()));
}

TEST(Hdf5OpenTest, OpenCorruptAndNegativeHdf5Files) {
  TempFile corrupt_tf("hdf5open_corrupt.h5");
  {
    std::ofstream ofs(corrupt_tf.c_str(), std::ios::binary);
    ASSERT_TRUE(ofs.is_open());
    const unsigned char bad_hdr[64] = {0x89, 'H',  'D',  'F',  '\r', '\n',
                                       0x1a, '\n', 0xff, 0xff, 0xff, 0xff};
    ofs.write(reinterpret_cast<const char*>(bad_hdr), sizeof(bad_hdr));
    ASSERT_TRUE(ofs.good());
  }

  {
    ScopedNcFile sc;
    EXPECT_EQ(NC_EHDFERR, nc_open(corrupt_tf.c_str(), NC_NOWRITE, sc.ptr()));
    EXPECT_EQ(NC_EHDFERR,
              nc_open(corrupt_tf.c_str(), NC_NOWRITE | NC_DISKLESS, sc.ptr()));
  }
  {
    std::array<unsigned char, 64> bad_buf = {0x89, 'H',  'D',  'F',  '\r',
                                             '\n', 0x1a, '\n', 0xff, 0xff};
    ScopedNcFile sc;
    EXPECT_EQ(NC_EHDFERR,
              nc_open_mem("corrupt_mem.nc", NC_NOWRITE, bad_buf.size(),
                          bad_buf.data(), sc.ptr()));
  }

  const int fail_variants[] = {
      kH5OpenShimNegH5sNullVar,
      kH5OpenShimNegBadCoordsLen,
      kH5OpenShimNegCommittedBitfield,
      kH5OpenShimNegDanglingLink,
  };
  for (int variant : fail_variants) {
    TempFile neg_tf(absl::StrFormat("hdf5open_neg_%d.h5", variant));
    ASSERT_EQ(0, H5OpenShimCreateNegativeFile(neg_tf.c_str(), variant));
    ScopedNcFile sc;
    EXPECT_EQ(NC_EHDFERR, nc_open(neg_tf.c_str(), NC_NOWRITE, sc.ptr()))
        << "variant=" << variant;
  }

  TempFile att_2d_tf("hdf5open_neg_2d_att.h5");
  ASSERT_EQ(
      0, H5OpenShimCreateNegativeFile(att_2d_tf.c_str(), kH5OpenShimNeg2dAttr));
  ScopedNcFile sc;
  ASSERT_EQ(NC_NOERR, nc_open(att_2d_tf.c_str(), NC_NOWRITE, sc.ptr()));
  int aid = -1;
  EXPECT_EQ(NC_EATTMETA,
            nc_inq_attid(sc.get(), NC_GLOBAL, "matrix_attr", &aid));
}

TEST(Hdf5OpenTest, DirectOpenValidationAndColdInit) {
  EXPECT_EQ(NC_EBADID, H5OpenShimProbeDirectOpen(kH5OpenShimDirectBadNcid,
                                                 "hdf5open_bad_ncid.nc",
                                                 NC_NOWRITE, nullptr));
  EXPECT_EQ(NC_EINVAL, H5OpenShimProbeDirectOpen(kH5OpenShimDirectIllegalMmap,
                                                 "hdf5open_bad_mmap.nc",
                                                 NC_NOWRITE, nullptr));
  EXPECT_EQ(NC_EINVAL,
            H5OpenShimProbeDirectOpen(kH5OpenShimDirectDisklessInmem,
                                      "hdf5open_bad_diskless_inmem.nc",
                                      NC_NOWRITE, nullptr));
  EXPECT_EQ(NC_EINMEMORY, H5OpenShimProbeDirectOpen(
                              kH5OpenShimDirectInmemNullParams,
                              "hdf5open_null_params.nc", NC_NOWRITE, nullptr));
  EXPECT_EQ(NC_EINMEMORY, H5OpenShimProbeDirectOpen(
                              kH5OpenShimDirectInmemEmptyMem,
                              "hdf5open_empty_mem.nc", NC_NOWRITE, nullptr));

  RunInExecveChild(&RunColdInitProbeInChild);
}

TEST(Hdf5OpenTest, GlobalChunkCacheMetaBlockSizeAndIsolatedAlignment) {
  TempFile tf("hdf5open_global_fapl.nc");
  {
    ScopedNcFile sc;
    ASSERT_EQ(NC_NOERR,
              nc_create(tf.c_str(), NC_NETCDF4 | NC_CLOBBER, sc.ptr()));
    int d = -1, v = -1;
    const int val = 33;
    ASSERT_EQ(NC_NOERR, nc_def_dim(sc.get(), "d", 1, &d));
    ASSERT_EQ(NC_NOERR, nc_def_var(sc.get(), "v", NC_INT, 1, &d, &v));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(sc.get(), v, &val));
    ASSERT_EQ(NC_NOERR, sc.close());
  }

  {
    ScopedChunkCache cache_guard;
    ScopedMetaBlockSize meta_guard;
    ASSERT_EQ(NC_NOERR, nc_set_chunk_cache(8U * 1024U * 1024U, 521U, 0.5F));
    ASSERT_EQ(NC_NOERR, nc_set_meta_block_size(8192U));

    ScopedNcFile sc;
    ASSERT_EQ(NC_NOERR, nc_open(tf.c_str(), NC_NOWRITE, sc.ptr()));
    int vid = -1, got = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(sc.get(), "v", &vid));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(sc.get(), vid, &got));
    EXPECT_EQ(33, got);
  }

  RunInExecveChild(&RunAlignmentProbeInChild);
}

void RunLeakyRawSuiteInChild() {
  TempDir td("hdf5open_leaky_raw");
  EXPECT_EQ(0, H5OpenShimRunLeakyRawSuite(td.c_str()));
}

void RunH5pClassFailureSuiteInChild() {
  TempFile tf("hdf5open_h5p_fail.nc");
  {
    ScopedNcFile sc;
    ASSERT_EQ(NC_NOERR,
              nc_create(tf.c_str(), NC_NETCDF4 | NC_CLOBBER, sc.ptr()));
    int dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(sc.get(), "d", 2, &dimid));
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(sc.get(), "v", NC_INT, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, sc.close());
  }
  EXPECT_EQ(0, H5OpenShimRunH5pClassFailureSuite(tf.c_str()));
}

void RunOpenFaultVolSuiteInChild() {
  TempDir td("hdf5open_fault_vol");
  EXPECT_EQ(0, H5OpenShimRunOpenFaultVolSuite(td.c_str()));
}

TEST(Hdf5OpenTest, ExtraFixturesAndInvalidPreemption) {
  {
    TempFile tf("hdf5open_extra_comm_vstr.h5");
    ASSERT_EQ(0, H5OpenShimCreateExtraFixtureFile(
                     tf.c_str(), kH5OpenShimExtraCommittedVarStr));
    ScopedNcFile sc;
    ASSERT_EQ(NC_NOERR, nc_open(tf.c_str(), NC_NOWRITE, sc.ptr()));
    nc_type typeid_vstr = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_inq_typeid(sc.get(), "c_vstr", &typeid_vstr));
    int type_class = NC_NAT;
    ASSERT_EQ(NC_NOERR,
              nc_inq_user_type(sc.get(), typeid_vstr, nullptr, nullptr, nullptr,
                               nullptr, &type_class));
    EXPECT_EQ(NC_STRING, type_class);
    int vid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(sc.get(), "sv", &vid));
    nc_type vtype = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_inq_vartype(sc.get(), vid, &vtype));
    EXPECT_EQ(NC_STRING, vtype);
  }

  {
    TempFile tf("hdf5open_extra_ldbl_att.h5");
    ASSERT_EQ(0, H5OpenShimCreateExtraFixtureFile(
                     tf.c_str(), kH5OpenShimExtraLdblGlobalAttr));
    ScopedNcFile sc;
    ASSERT_EQ(NC_NOERR, nc_open(tf.c_str(), NC_NOWRITE, sc.ptr()));
    int natts = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_natts(sc.get(), &natts));
    EXPECT_EQ(0, natts);
    int vid = -1;
    EXPECT_EQ(NC_NOERR, nc_inq_varid(sc.get(), "v", &vid));
  }

  {
    TempFile tf("hdf5open_extra_vstr_deflate.h5");
    ASSERT_EQ(0, H5OpenShimCreateExtraFixtureFile(
                     tf.c_str(), kH5OpenShimExtraChunkedVarStrDeflate));
    ScopedNcFile sc;
    ASSERT_EQ(NC_NOERR, nc_open(tf.c_str(), NC_NOWRITE, sc.ptr()));
    int vid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(sc.get(), "vstr_def", &vid));
    int shuf = 0;
    int defl = 0;
    int lvl = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_var_deflate(sc.get(), vid, &shuf, &defl, &lvl));
    EXPECT_EQ(1, defl);
    EXPECT_EQ(1, lvl);
  }

  {
    TempFile tf("hdf5open_extra_untracked_subgrp.h5");
    ASSERT_EQ(0, H5OpenShimCreateExtraFixtureFile(
                     tf.c_str(), kH5OpenShimExtraUntrackedSubgroup));
    {
      ScopedNcFile sc_ro;
      ASSERT_EQ(NC_NOERR, nc_open(tf.c_str(), NC_NOWRITE, sc_ro.ptr()));
      int subgid = -1;
      EXPECT_EQ(NC_NOERR, nc_inq_ncid(sc_ro.get(), "sub", &subgid));
    }
    {
      ScopedNcFile sc_rw;
      EXPECT_EQ(NC_ECANTWRITE, nc_open(tf.c_str(), NC_WRITE, sc_rw.ptr()));
    }
  }

  {
    TempFile tf("hdf5open_extra_2d_scale_nocoords.h5");
    ASSERT_EQ(0, H5OpenShimCreateExtraFixtureFile(
                     tf.c_str(), kH5OpenShimExtra2dScaleNoCoords));
    ScopedNcFile sc;
    EXPECT_EQ(NC_EHDFERR, nc_open(tf.c_str(), NC_NOWRITE, sc.ptr()));
  }

  {
    TempFile tf("hdf5open_invalid_preemption.nc");
    {
      ScopedNcFile sc;
      ASSERT_EQ(NC_NOERR,
                nc_create(tf.c_str(), NC_NETCDF4 | NC_CLOBBER, sc.ptr()));
      ASSERT_EQ(NC_NOERR, sc.close());
    }
    ScopedChunkCache chunk_cache_guard;
    EXPECT_EQ(NC_EHDFERR, H5OpenShimProbeInvalidPreemption(tf.c_str()));
  }
}

TEST(Hdf5OpenTest, IsolatedRawAndVolFaultSuites) {
  RunInExecveChild([]() {
    RunLeakyRawSuiteInChild();
    return !::testing::Test::HasFailure();
  });
  RunInExecveChild([]() {
    RunH5pClassFailureSuiteInChild();
    return !::testing::Test::HasFailure();
  });
  RunInExecveChild([]() {
    RunOpenFaultVolSuiteInChild();
    return !::testing::Test::HasFailure();
  });
}

}  // namespace
}  // namespace netcdf
