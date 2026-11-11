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

// Clean-room unit tests for //third_party/netcdf/libhdf5/hdf5attr.c.
//
// Covers NC4_HDF5_put_att() / nc4_put_att(), NC4_HDF5_inq_att(),
// NC4_HDF5_inq_attid(), NC4_HDF5_inq_attname(), NC4_HDF5_get_att(),
// nc4_get_att_special(), NC4_HDF5_rename_att(), and NC4_HDF5_del_att(),
// including lazy attribute loading, classic-model restrictions, reserved
// provenance attributes, NC_FillValue semantics, rollbacks, type conversions,
// and user-defined/variable-length types.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/libhdf5/hdf5attr_shim.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"
#include "third_party/netcdf/include/netcdf.h"

namespace netcdf {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

struct PackedLabeled {
  int32_t tag;
  char* label;
};

TEST(Hdf5AttrTest, PutAttValidationErrorsReservedNamesAndClassicRules) {
  int ival = 1;
  EXPECT_EQ(NC_EBADID, nc_put_att_int(-1, NC_GLOBAL, "a", NC_INT, 1, &ival));

  // Classic-model NetCDF-4 rules and define-mode checks.
  TempFile classic_file("attr_classic_rules.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(classic_file.c_str(),
                                  NC_NETCDF4 | NC_CLASSIC_MODEL, nc.ptr()));
    const int bad_grpid = nc.get() | 0x00FF;
    // nc4_find_nc_grp_h5() returns NC_EBADID for an invalid group index.
    EXPECT_EQ(NC_EBADID,
              nc_put_att_int(bad_grpid, NC_GLOBAL, "a", NC_INT, 1, &ival));

    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v", NC_INT, 1, &dimid, &varid));

    // Parameter validation in nc4_put_att().
    EXPECT_EQ(NC_ENOTVAR, nc_put_att_int(nc, 99, "a", NC_INT, 1, &ival));
    EXPECT_EQ(NC_ENOTVAR, nc_put_att_int(nc, -5, "a", NC_INT, 1, &ival));
    const size_t huge_len =
        static_cast<size_t>(std::numeric_limits<int>::max()) + 1ULL;
    EXPECT_EQ(NC_EINVAL,
              nc_put_att_int(nc, NC_GLOBAL, "huge", NC_INT, huge_len, &ival));
    EXPECT_EQ(NC_EBADNAME,
              nc_put_att_int(nc, NC_GLOBAL, nullptr, NC_INT, 1, &ival));
    const std::string too_long(NC_MAX_NAME + 1, 'a');
    EXPECT_EQ(NC_EBADNAME, nc_put_att_int(nc, NC_GLOBAL, too_long.c_str(),
                                          NC_INT, 1, &ival));
    EXPECT_EQ(NC_EINVAL,
              nc_put_att_int(nc, NC_GLOBAL, "nodata", NC_INT, 1, nullptr));
    EXPECT_EQ(NC_EBADNAME, nc_put_att_int(nc, NC_GLOBAL, "", NC_INT, 1, &ival));
    EXPECT_EQ(NC_EBADNAME,
              nc_put_att_int(nc, NC_GLOBAL, "bad/slash", NC_INT, 1, &ival));

    // Type errors & character conversion blocks.
    EXPECT_EQ(NC_EBADTYPE,
              nc_put_att(nc, NC_GLOBAL, "nat_f", NC_NAT, 1, &ival));
    EXPECT_EQ(NC_EBADTYPE, Hdf5PutAttDirect(nc, NC_GLOBAL, "nat_m", NC_INT, 1,
                                            &ival, NC_NAT, 0));
    EXPECT_EQ(NC_ECHAR,
              nc_put_att_int(nc, NC_GLOBAL, "c_from_i", NC_CHAR, 1, &ival));
    EXPECT_EQ(NC_ECHAR, Hdf5PutAttDirect(nc, NC_GLOBAL, "i_from_c", NC_INT, 1,
                                         "a", NC_CHAR, 0));
    EXPECT_EQ(NC_ECHAR,
              nc_put_att_int(nc, NC_GLOBAL, "s_from_i", NC_STRING, 1, &ival));
    const char* s_ptr = "hi";
    EXPECT_EQ(NC_ECHAR, Hdf5PutAttDirect(nc, NC_GLOBAL, "i_from_s", NC_INT, 1,
                                         &s_ptr, NC_STRING, 0));

    // Classic-model forbids non-classic atomic types (nc_type > NC_DOUBLE).
    const unsigned short u16 = 5;
    EXPECT_EQ(NC_ESTRICTNC3,
              nc_put_att_ushort(nc, NC_GLOBAL, "u16", NC_USHORT, 1, &u16));

    // Reserved names blocked on root NC_GLOBAL and variables.
    EXPECT_EQ(NC_ENAMEINUSE,
              nc_put_att_text(nc, NC_GLOBAL, "_Format", 4, "HDF5"));
    EXPECT_EQ(NC_ENAMEINUSE, nc_put_att_text(nc, varid, "_Format", 4, "HDF5"));
    EXPECT_EQ(NC_ENAMEINUSE,
              nc_put_att_int(nc, varid, "CLASS", NC_INT, 1, &ival));

    // Define valid classic attributes before leaving define mode.
    const std::array<short, 2> s2 = {10, 20};
    ASSERT_EQ(NC_NOERR, nc_put_att_short(nc, NC_GLOBAL, "s_att", NC_SHORT,
                                         s2.size(), s2.data()));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    // Classic data mode rejects new attributes and size-growing overwrites,
    // while allowing same/smaller byte-size overwrites.
    EXPECT_EQ(NC_ENOTINDEFINE,
              nc_put_att_int(nc, NC_GLOBAL, "late", NC_INT, 1, &ival));
    const std::array<double, 2> d2 = {1.0, 2.0};
    EXPECT_EQ(NC_ENOTINDEFINE,
              nc_put_att_double(nc, NC_GLOBAL, "s_att", NC_DOUBLE, d2.size(),
                                d2.data()));
    const int i_same_size = 777;
    EXPECT_EQ(NC_NOERR,
              nc_put_att_int(nc, NC_GLOBAL, "s_att", NC_INT, 1, &i_same_size));

    ASSERT_EQ(NC_NOERR, nc.close());
  }

  // Reopen read-only: nc_put_att returns NC_EPERM.
  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(classic_file.c_str(), NC_NOWRITE, ro.ptr()));
    EXPECT_EQ(NC_EPERM, nc_put_att_int(ro, NC_GLOBAL, "ro", NC_INT, 1, &ival));
  }

  // Invalid file_type in nc4_get_typelen_mem(): clean up partial attribute.
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create("attr_bad_ftype.nc", NC_NETCDF4 | NC_DISKLESS,
                                  nc.ptr()));
    EXPECT_EQ(NC_EBADTYPID,
              nc_put_att_int(nc, NC_GLOBAL, "bad_type", 9999, 1, &ival));
    EXPECT_EQ(NC_NOERR, nc_del_att(nc, NC_GLOBAL, "bad_type"));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  // Child group NC_GLOBAL allows "_Format", and force=1 allows root "_Format".
  {
    ScopedNcFile mem_nc;
    ASSERT_EQ(NC_NOERR, nc_create("attr_reserved_child.nc",
                                  NC_NETCDF4 | NC_DISKLESS, mem_nc.ptr()));
    int sub_id = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(mem_nc, "sub", &sub_id));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_text(sub_id, NC_GLOBAL, "_Format", 4, "sub4"));
    ASSERT_EQ(NC_NOERR, Hdf5PutAttDirect(mem_nc, NC_GLOBAL, "_Format", NC_CHAR,
                                         4, "HDF5", NC_CHAR, 1));
    // "_Format" is reserved without NAMEONLYFLAG, so querying it on root
    // NC_GLOBAL exercises the non-special reserved path in inq/get_att.
    nc_type fmt_type = NC_NAT;
    size_t fmt_len = 0;
    int fmt_attid = -1;
    std::array<char, 5> fmt_val = {};
    EXPECT_EQ(NC_NOERR,
              nc_inq_att(mem_nc, NC_GLOBAL, "_Format", &fmt_type, &fmt_len));
    EXPECT_EQ(NC_CHAR, fmt_type);
    EXPECT_EQ(4U, fmt_len);
    EXPECT_EQ(NC_NOERR, nc_inq_attid(mem_nc, NC_GLOBAL, "_Format", &fmt_attid));
    EXPECT_GE(fmt_attid, 0);
    EXPECT_EQ(NC_NOERR,
              nc_get_att_text(mem_nc, NC_GLOBAL, "_Format", fmt_val.data()));
    EXPECT_STREQ("HDF5", fmt_val.data());
    ASSERT_EQ(NC_NOERR, mem_nc.close());
  }
}

TEST(Hdf5AttrTest, PutAttAllAtomicTypesDataModeAutoRedefZeroLenAndConvert) {
  TempFile file("attr_all_types.nc");
  int varid = -1;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(file.c_str(), NC_NETCDF4, nc.ptr()));
    int dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v", NC_INT, 1, &dimid, &varid));

    // Seed an attribute of every NetCDF-4 atomic type before nc_enddef so
    // overwriting them in data mode exercises every case of nc4typelen().
    const signed char b1 = -5;
    const char ch1 = 'Z';
    const unsigned char ub1 = 200;
    const short s1 = -1234;
    const unsigned short us1 = 43210;
    const int i1 = -100000;
    const unsigned int ui1 = 3000000000U;
    const float f1 = 1.25F;
    const double d1 = 2.5;
    const long long i64_1 = -5000000000LL;
    const unsigned long long u64_1 = 9000000000ULL;
    const char* str1 = "first";

    ASSERT_EQ(NC_NOERR,
              nc_put_att_schar(nc, NC_GLOBAL, "t_byte", NC_BYTE, 1, &b1));
    ASSERT_EQ(NC_NOERR, nc_put_att_text(nc, NC_GLOBAL, "t_char", 1, &ch1));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_uchar(nc, NC_GLOBAL, "t_ubyte", NC_UBYTE, 1, &ub1));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_short(nc, NC_GLOBAL, "t_short", NC_SHORT, 1, &s1));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_ushort(nc, NC_GLOBAL, "t_ushort", NC_USHORT, 1, &us1));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, NC_GLOBAL, "t_int", NC_INT, 1, &i1));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_uint(nc, NC_GLOBAL, "t_uint", NC_UINT, 1, &ui1));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_float(nc, NC_GLOBAL, "t_float", NC_FLOAT, 1, &f1));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_double(nc, NC_GLOBAL, "t_double", NC_DOUBLE, 1, &d1));
    ASSERT_EQ(NC_NOERR, nc_put_att_longlong(nc, NC_GLOBAL, "t_int64", NC_INT64,
                                            1, &i64_1));
    ASSERT_EQ(NC_NOERR, nc_put_att_ulonglong(nc, NC_GLOBAL, "t_uint64",
                                             NC_UINT64, 1, &u64_1));
    ASSERT_EQ(NC_NOERR, nc_put_att_string(nc, NC_GLOBAL, "t_string", 1, &str1));

    // Zero-length attribute -> non-zero -> back to zero-length.
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc, varid, "empty_to_full", NC_INT, 0, nullptr));
    const int filled = 88;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc, varid, "empty_to_full", NC_INT, 1, &filled));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc, varid, "full_to_empty", NC_INT, 1, &filled));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc, varid, "full_to_empty", NC_INT, 0, nullptr));

    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    // Overwrite all seeded attributes in data mode (same size first, then
    // larger size triggering automatic NC4_redef).
    const signed char b2 = -9;
    const char ch2 = 'Q';
    const unsigned char ub2 = 250;
    const short s2 = -2000;
    const unsigned short us2 = 60000;
    const int i2 = -200000;
    const unsigned int ui2 = 3100000000U;
    const float f2 = 3.5F;
    const double d2 = 4.75;
    const long long i64_2 = -6000000000LL;
    const unsigned long long u64_2 = 12000000000ULL;
    std::array<const char*, 2> strs2 = {"alpha", "beta"};

    ASSERT_EQ(NC_NOERR,
              nc_put_att_schar(nc, NC_GLOBAL, "t_byte", NC_BYTE, 1, &b2));
    ASSERT_EQ(NC_NOERR, nc_put_att_text(nc, NC_GLOBAL, "t_char", 1, &ch2));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_uchar(nc, NC_GLOBAL, "t_ubyte", NC_UBYTE, 1, &ub2));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_short(nc, NC_GLOBAL, "t_short", NC_SHORT, 1, &s2));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_ushort(nc, NC_GLOBAL, "t_ushort", NC_USHORT, 1, &us2));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, NC_GLOBAL, "t_int", NC_INT, 1, &i2));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_uint(nc, NC_GLOBAL, "t_uint", NC_UINT, 1, &ui2));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_float(nc, NC_GLOBAL, "t_float", NC_FLOAT, 1, &f2));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_double(nc, NC_GLOBAL, "t_double", NC_DOUBLE, 1, &d2));
    ASSERT_EQ(NC_NOERR, nc_put_att_longlong(nc, NC_GLOBAL, "t_int64", NC_INT64,
                                            1, &i64_2));
    ASSERT_EQ(NC_NOERR, nc_put_att_ulonglong(nc, NC_GLOBAL, "t_uint64",
                                             NC_UINT64, 1, &u64_2));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_string(nc, NC_GLOBAL, "t_string", 1, &strs2[0]));

    // Trigger NC4_redef() on an existing attribute when size grows, and on a
    // brand new attribute written in data mode.
    const std::array<int, 2> grown = {11, 22};
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, NC_GLOBAL, "t_int", NC_INT,
                                       grown.size(), grown.data()));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc, NC_GLOBAL, "late_new", NC_INT, 1, &filled));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    // Implicit type conversions (in-range and NC_ERANGE).
    const int int_as_dbl = 42;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc, varid, "conv_dbl", NC_DOUBLE, 1, &int_as_dbl));
    const int oob_for_byte = 1000;
    EXPECT_EQ(NC_ERANGE,
              nc_put_att_int(nc, varid, "conv_oob", NC_BYTE, 1, &oob_for_byte));

    // Grow NC_STRING attribute in data mode (reclaims previous string).
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));
    ASSERT_EQ(NC_NOERR, nc_put_att_string(nc, NC_GLOBAL, "t_string",
                                          strs2.size(), strs2.data()));

    ASSERT_EQ(NC_NOERR, nc.close());
  }

  // Reopen read-only and verify converted/redefined values.
  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro.ptr()));
    ScopedNcStrings strings_in(2);

    std::array<int, 2> grown_in = {};
    ASSERT_EQ(NC_NOERR,
              nc_get_att_int(ro, NC_GLOBAL, "t_int", grown_in.data()));
    EXPECT_THAT(grown_in, ElementsAre(11, 22));

    ASSERT_EQ(NC_NOERR,
              nc_get_att_string(ro, NC_GLOBAL, "t_string", strings_in.data()));
    EXPECT_STREQ("alpha", strings_in[0]);
    EXPECT_STREQ("beta", strings_in[1]);

    double d_in = 0.0;
    ASSERT_EQ(NC_NOERR, nc_get_att_double(ro, varid, "conv_dbl", &d_in));
    EXPECT_DOUBLE_EQ(42.0, d_in);

    size_t empty_len = 99;
    ASSERT_EQ(NC_NOERR, nc_inq_attlen(ro, varid, "full_to_empty", &empty_len));
    EXPECT_EQ(0U, empty_len);
  }
}

TEST(Hdf5AttrTest, FillValueRulesChildGroupCreatedVarAndUserTypes) {
  // Mismatched NC_FillValue type on a fresh variable: clean up partial attr.
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create("attr_bad_fill.nc", NC_NETCDF4 | NC_DISKLESS,
                                  nc.ptr()));
    int dimid = -1;
    int vi = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "vi", NC_INT, 1, &dimid, &vi));
    const double df = 1.5;
    EXPECT_EQ(NC_EBADTYPE,
              nc_put_att_double(nc, vi, NC_FillValue, NC_DOUBLE, 1, &df));
    EXPECT_EQ(NC_NOERR, nc_del_att(nc, vi, NC_FillValue));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  TempFile file("attr_fill_and_udt.nc");
  int sub_id = -1;
  int v_sub = -1;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(file.c_str(), NC_NETCDF4, nc.ptr()));

    // Global NC_FillValue may have len > 1 without triggering variable rules.
    const std::array<int, 3> global_fill = {1, 2, 3};
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, NC_GLOBAL, NC_FillValue, NC_INT,
                                       global_fill.size(), global_fill.data()));

    int dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "n", 2, &dimid));
    int v_short = -1;
    int v_str = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "vs", NC_SHORT, 1, &dimid, &v_short));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "vstr", NC_STRING, 1, &dimid, &v_str));

    // Variable NC_FillValue must have len == 1.
    const std::array<short, 2> bad_len = {1, 2};
    EXPECT_EQ(NC_EINVAL, nc_put_att_short(nc, v_short, NC_FillValue, NC_SHORT,
                                          bad_len.size(), bad_len.data()));

    // Fill value conversion from int -> NC_SHORT, overwrite, and NC_ERANGE.
    const int good_fill_1 = -12;
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, v_short, NC_FillValue, NC_SHORT, 1,
                                       &good_fill_1));
    const int good_fill_2 = -24;
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, v_short, NC_FillValue, NC_SHORT, 1,
                                       &good_fill_2));
    const int bad_fill_range = 100000;
    EXPECT_EQ(NC_ERANGE, nc_put_att_int(nc, v_short, NC_FillValue, NC_SHORT, 1,
                                        &bad_fill_range));
    const short clean_short_fill = -77;
    ASSERT_EQ(NC_NOERR, nc_put_att_short(nc, v_short, NC_FillValue, NC_SHORT, 1,
                                         &clean_short_fill));

    // Non-convertible NC_STRING NC_FillValue + overwrite (reclaiming old copy).
    const char* s_fill_1 = "unset";
    const char* s_fill_2 = "missing";
    ASSERT_EQ(NC_NOERR,
              nc_put_att_string(nc, v_str, NC_FillValue, 1, &s_fill_1));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_string(nc, v_str, NC_FillValue, 1, &s_fill_2));

    // User-defined attributes: NC_ENUM, NC_OPAQUE, NC_VLEN, NC_COMPOUND.
    nc_type enum_t = NC_NAT;
    nc_type opaque_t = NC_NAT;
    nc_type vlen_t = NC_NAT;
    nc_type cmp_t = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc, NC_INT, "state_t", &enum_t));
    const int m0 = 0;
    const int m1 = 1;
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_t, "OFF", &m0));
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_t, "ON", &m1));
    const int eval = 1;
    ASSERT_EQ(NC_NOERR, nc_put_att(nc, NC_GLOBAL, "a_enum", enum_t, 1, &eval));

    ASSERT_EQ(NC_NOERR, nc_def_opaque(nc, 4, "op4_t", &opaque_t));
    const std::array<unsigned char, 4> opq = {0x12, 0x34, 0x56, 0x78};
    ASSERT_EQ(NC_NOERR,
              nc_put_att(nc, NC_GLOBAL, "a_opaque", opaque_t, 1, opq.data()));

    ASSERT_EQ(NC_NOERR, nc_def_vlen(nc, "ivlen_t", NC_INT, &vlen_t));
    std::array<int, 2> vl_a = {10, 20};
    std::array<int, 3> vl_b = {30, 40, 50};
    std::array<nc_vlen_t, 2> vl_arr = {
        nc_vlen_t{vl_a.size(), vl_a.data()},
        nc_vlen_t{vl_b.size(), vl_b.data()},
    };
    ASSERT_EQ(NC_NOERR, nc_put_att(nc, NC_GLOBAL, "a_vlen", vlen_t,
                                   vl_arr.size(), vl_arr.data()));

    ASSERT_EQ(NC_NOERR,
              nc_def_compound(nc, sizeof(PackedLabeled), "labeled_t", &cmp_t));
    ASSERT_EQ(NC_NOERR, nc_insert_compound(
                            nc, cmp_t, "tag",
                            NC_COMPOUND_OFFSET(PackedLabeled, tag), NC_INT));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc, cmp_t, "label",
                                 NC_COMPOUND_OFFSET(PackedLabeled, label),
                                 NC_STRING));
    char cmp_str[] = "node0";
    PackedLabeled cmp_in{42, cmp_str};
    ASSERT_EQ(NC_NOERR, nc_put_att(nc, NC_GLOBAL, "a_cmp", cmp_t, 1, &cmp_in));

    // Child group var: nc_enddef(root) creates HDF5 datasets everywhere while
    // leaving sub_var->written_to == NC_FALSE, hitting var->created &&
    // !var->written_to when updating NC_FillValue on v_sub afterward.
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc, "sub", &sub_id));
    ASSERT_EQ(NC_NOERR, nc_def_var(sub_id, "v_sub", NC_INT, 1, &dimid, &v_sub));
    const int initial_sub_fill = -1;
    ASSERT_EQ(NC_NOERR, nc_put_att_int(sub_id, v_sub, NC_FillValue, NC_INT, 1,
                                       &initial_sub_fill));

    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    // Root variable was marked written_to by NC4_enddef(root).
    EXPECT_EQ(NC_ELATEFILL, nc_put_att_short(nc, v_short, NC_FillValue,
                                             NC_SHORT, 1, &clean_short_fill));

    // Child variable is created in HDF5, not yet written_to -> updates fill
    // value and sets fill_val_changed = NC_TRUE. Sync before writing so the
    // recreated HDF5 dataset receives the subsequent element write.
    const int updated_sub_fill = 555;
    ASSERT_EQ(NC_NOERR, nc_put_att_int(sub_id, v_sub, NC_FillValue, NC_INT, 1,
                                       &updated_sub_fill));
    ASSERT_EQ(NC_NOERR, nc_sync(nc));

    const size_t idx1 = 1;
    const int written_val = 999;
    ASSERT_EQ(NC_NOERR, nc_put_var1_int(sub_id, v_sub, &idx1, &written_val));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  // Reopen read-only and read back user-defined attributes and child var fill.
  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro.ptr()));

    int ro_sub = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_ncid(ro, "sub", &ro_sub));
    int ro_v_sub = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_sub, "v_sub", &ro_v_sub));
    std::array<int, 2> sub_vals = {0, 0};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro_sub, ro_v_sub, sub_vals.data()));
    EXPECT_THAT(sub_vals, ElementsAre(555, 999));

    int eval_out = -1;
    ASSERT_EQ(NC_NOERR, nc_get_att(ro, NC_GLOBAL, "a_enum", &eval_out));
    EXPECT_EQ(1, eval_out);

    std::array<unsigned char, 4> opq_out = {};
    ASSERT_EQ(NC_NOERR, nc_get_att(ro, NC_GLOBAL, "a_opaque", opq_out.data()));
    EXPECT_THAT(opq_out, ElementsAre(0x12, 0x34, 0x56, 0x78));

    {
      ScopedNcVlens vl_out(2);
      ASSERT_EQ(NC_NOERR, nc_get_att(ro, NC_GLOBAL, "a_vlen", vl_out.data()));
      ASSERT_EQ(2U, vl_out[0].len);
      ASSERT_EQ(3U, vl_out[1].len);
      const auto* p0 = static_cast<const int*>(vl_out[0].p);
      const auto* p1 = static_cast<const int*>(vl_out[1].p);
      EXPECT_EQ(10, p0[0]);
      EXPECT_EQ(20, p0[1]);
      EXPECT_EQ(30, p1[0]);
      EXPECT_EQ(50, p1[2]);
    }

    {
      nc_type cmp_t_in = NC_NAT;
      ASSERT_EQ(NC_NOERR, nc_inq_atttype(ro, NC_GLOBAL, "a_cmp", &cmp_t_in));
      PackedLabeled cmp_out{};
      absl::Cleanup reclaim_cmp = [&ro, cmp_t_in, &cmp_out] {
        nc_reclaim_data(ro, cmp_t_in, &cmp_out, 1);
      };
      ASSERT_EQ(NC_NOERR, nc_get_att(ro, NC_GLOBAL, "a_cmp", &cmp_out));
      EXPECT_EQ(42, cmp_out.tag);
      EXPECT_STREQ("node0", cmp_out.label);
    }
  }
}

TEST(Hdf5AttrTest, SpecialProvenanceAttributesPublicAndShimPaths) {
  TempFile nc4_file("attr_special_nc4.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(nc4_file.c_str(), NC_NETCDF4, nc.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d", 1, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v", NC_INT, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  // Public inquiries/reads on reopened NetCDF-4 file.
  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(nc4_file.c_str(), NC_NOWRITE, ro.ptr()));

    nc_type type = NC_NAT;
    size_t len = 0;
    int attid = -1;

    ASSERT_EQ(NC_NOERR, nc_inq_att(ro, NC_GLOBAL, "_IsNetcdf4", &type, &len));
    EXPECT_EQ(NC_INT, type);
    EXPECT_EQ(1U, len);
    int is_nc4 = 0;
    ASSERT_EQ(NC_NOERR, nc_get_att_int(ro, NC_GLOBAL, "_IsNetcdf4", &is_nc4));
    EXPECT_EQ(1, is_nc4);

    ASSERT_EQ(NC_NOERR,
              nc_inq_att(ro, NC_GLOBAL, "_SuperblockVersion", &type, &len));
    EXPECT_EQ(NC_INT, type);
    EXPECT_EQ(1U, len);
    int sb_ver = -1;
    ASSERT_EQ(NC_NOERR,
              nc_get_att_int(ro, NC_GLOBAL, "_SuperblockVersion", &sb_ver));
    EXPECT_GE(sb_ver, 0);

    ASSERT_EQ(NC_NOERR,
              nc_inq_att(ro, NC_GLOBAL, "_NCProperties", &type, &len));
    EXPECT_EQ(NC_CHAR, type);
    ASSERT_GT(len, 0U);
    std::string props(len + 1, '\0');
    ASSERT_EQ(NC_NOERR,
              nc_get_att_text(ro, NC_GLOBAL, "_NCProperties", props.data()));
    EXPECT_THAT(props.c_str(), HasSubstr("version="));

    // Special virtual attributes do not have attribute index numbers.
    EXPECT_EQ(NC_EATTMETA, nc_inq_attid(ro, NC_GLOBAL, "_IsNetcdf4", &attid));
    EXPECT_EQ(NC_EATTMETA,
              nc_inq_attid(ro, NC_GLOBAL, "_SuperblockVersion", &attid));
    EXPECT_EQ(NC_EATTMETA,
              nc_inq_attid(ro, NC_GLOBAL, "_NCProperties", &attid));

    // On a variable (not NC_GLOBAL), special names are not virtual attributes.
    EXPECT_EQ(NC_ENOTATT, nc_inq_att(ro, 0, "_IsNetcdf4", &type, &len));
    EXPECT_EQ(NC_ENOTATT, nc_inq_attid(ro, 0, "_SuperblockVersion", &attid));
    int var_is_nc4 = -1;
    EXPECT_EQ(NC_ENOTATT, nc_get_att_int(ro, 0, "_IsNetcdf4", &var_is_nc4));

    // Direct shim calls to exercise every switch arm in nc4_get_att_special().
    EXPECT_EQ(NC_EATTMETA, Hdf5CallGetAttSpecial(ro, "_IsNetcdf4", NC_INT, 1, 0,
                                                 &type, &len, nullptr));
    EXPECT_EQ(NC_ECHAR, Hdf5CallGetAttSpecial(ro, "_NCProperties", NC_INT, 0, 0,
                                              &type, &len, nullptr));
    EXPECT_EQ(NC_NOERR, Hdf5CallGetAttSpecial(ro, "_NCProperties", NC_NAT, 0, 0,
                                              &type, &len, props.data()));
    EXPECT_EQ(NC_NOERR, Hdf5CallGetAttSpecial(ro, "_NCProperties", NC_CHAR, 0,
                                              1, nullptr, nullptr, nullptr));

    signed char sb_byte = -1;
    short sb_short = -1;
    int sb_int = -1;
    unsigned char sb_ubyte = 0;
    unsigned short sb_ushort = 0;
    unsigned int sb_uint = 0;
    long long sb_i64 = -1;
    unsigned long long sb_u64 = 0;
    int nat_is4 = 0;

    EXPECT_EQ(NC_NOERR, Hdf5CallGetAttSpecial(ro, "_SuperblockVersion", NC_BYTE,
                                              0, 0, &type, &len, &sb_byte));
    EXPECT_EQ(NC_NOERR,
              Hdf5CallGetAttSpecial(ro, "_SuperblockVersion", NC_SHORT, 0, 0,
                                    &type, &len, &sb_short));
    EXPECT_EQ(NC_NOERR, Hdf5CallGetAttSpecial(ro, "_SuperblockVersion", NC_INT,
                                              0, 0, &type, &len, &sb_int));
    EXPECT_EQ(NC_NOERR,
              Hdf5CallGetAttSpecial(ro, "_SuperblockVersion", NC_UBYTE, 0, 0,
                                    &type, &len, &sb_ubyte));
    EXPECT_EQ(NC_NOERR,
              Hdf5CallGetAttSpecial(ro, "_SuperblockVersion", NC_USHORT, 0, 0,
                                    &type, &len, &sb_ushort));
    EXPECT_EQ(NC_NOERR, Hdf5CallGetAttSpecial(ro, "_SuperblockVersion", NC_UINT,
                                              0, 0, &type, &len, &sb_uint));
    EXPECT_EQ(NC_NOERR,
              Hdf5CallGetAttSpecial(ro, "_SuperblockVersion", NC_INT64, 0, 0,
                                    &type, &len, &sb_i64));
    EXPECT_EQ(NC_NOERR,
              Hdf5CallGetAttSpecial(ro, "_SuperblockVersion", NC_UINT64, 0, 0,
                                    &type, &len, &sb_u64));
    EXPECT_EQ(NC_NOERR, Hdf5CallGetAttSpecial(ro, "_IsNetcdf4", NC_NAT, 0, 1,
                                              nullptr, nullptr, &nat_is4));
    EXPECT_EQ(1, nat_is4);
    EXPECT_EQ(sb_ver, sb_int);
    EXPECT_EQ(sb_ver, static_cast<int>(sb_i64));
    EXPECT_EQ(static_cast<unsigned long long>(sb_ver), sb_u64);

    double dbl_dummy = 0.0;
    EXPECT_EQ(NC_ERANGE,
              Hdf5CallGetAttSpecial(ro, "_SuperblockVersion", NC_DOUBLE, 0, 0,
                                    &type, &len, &dbl_dummy));
    EXPECT_EQ(NC_NOERR, Hdf5CallGetAttSpecial(ro, "_IsNetcdf4", NC_DOUBLE, 0, 1,
                                              nullptr, nullptr, nullptr));
    EXPECT_EQ(NC_NOERR, Hdf5CallGetAttSpecial(ro, "_OtherSpecial", NC_NAT, 0, 1,
                                              nullptr, nullptr, nullptr));
  }

  // Raw HDF5 file created without `_NCProperties`: NC_ENOTATT when querying it.
  TempFile raw_hdf5("attr_raw.h5");
  ASSERT_EQ(0, Hdf5CreateRawHdf5File(raw_hdf5.c_str()));
  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(raw_hdf5.c_str(), NC_NOWRITE, ro.ptr()));
    nc_type type = NC_NAT;
    size_t len = 0;
    EXPECT_EQ(NC_ENOTATT,
              nc_inq_att(ro, NC_GLOBAL, "_NCProperties", &type, &len));
    int is_nc4 = -1;
    ASSERT_EQ(NC_NOERR, nc_get_att_int(ro, NC_GLOBAL, "_IsNetcdf4", &is_nc4));
    EXPECT_GE(is_nc4, 0);
  }
}

TEST(Hdf5AttrTest, EnumEurlAndAttSaveRollbackPreservesExistingData) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create("attr_eurl.nc", NC_NETCDF4 | NC_DISKLESS, nc.ptr()));
  nc_type enum_t = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_enum(nc, NC_INT, "mode_t", &enum_t));
  const int m0 = 0;
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_t, "ZERO", &m0));

  const int orig_val = 123;
  ASSERT_EQ(NC_NOERR,
            nc_put_att_int(nc, NC_GLOBAL, "e_rollback", NC_INT, 1, &orig_val));

  // Passing mem_type=NC_INT and file_type=enum_t triggers NC_EURL and rolls
  // back attsave.data onto the existing attribute cleanly.
  const int bogus = 456;
  EXPECT_EQ(NC_EURL,
            nc_put_att_int(nc, NC_GLOBAL, "e_rollback", enum_t, 1, &bogus));
  EXPECT_EQ(NC_EURL,
            nc_put_att_int(nc, NC_GLOBAL, "e_new_bad", enum_t, 1, &bogus));
  EXPECT_EQ(NC_NOERR, nc_del_att(nc, NC_GLOBAL, "e_new_bad"));

  // nc4_put_att() mutates att->nc_typeid before failing with NC_EURL without
  // restoring attsave.type, so inspect the rolled-back payload via untyped
  // nc_get_att() (memtype = NC_NAT) to avoid a spurious NC_EBADTYPE.
  int restored = -1;
  ASSERT_EQ(NC_NOERR, nc_get_att(nc, NC_GLOBAL, "e_rollback", &restored));
  EXPECT_EQ(123, restored);
  ASSERT_EQ(NC_NOERR, nc.close());
}

TEST(Hdf5AttrTest, RenameAttValidationClassicConstraintsAndPersistence) {
  // Invalid ncid fails in dispatch NC_check_id() before NC4_HDF5_rename_att().
  EXPECT_EQ(NC_EBADID, nc_rename_att(-1, NC_GLOBAL, nullptr, "b"));
  EXPECT_EQ(NC_EBADID, nc_rename_att(-1, NC_GLOBAL, "a", nullptr));
  EXPECT_EQ(NC_EBADID, nc_rename_att(-1, NC_GLOBAL, "a", "b"));

  // Classic-model rename rules.
  TempFile classic_file("attr_rename_classic.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(classic_file.c_str(),
                                  NC_NETCDF4 | NC_CLASSIC_MODEL, nc.ptr()));
    // Valid ncid reaches NC4_HDF5_rename_att()'s null name/newname checks.
    EXPECT_EQ(NC_EINVAL, nc_rename_att(nc, NC_GLOBAL, nullptr, "b"));
    EXPECT_EQ(NC_EINVAL, nc_rename_att(nc, NC_GLOBAL, "a", nullptr));
    const int bad_grpid = nc.get() | 0x00FF;
    EXPECT_EQ(NC_EBADID, nc_rename_att(bad_grpid, NC_GLOBAL, "a", "b"));

    const int val = 9;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc, NC_GLOBAL, "long_name", NC_INT, 1, &val));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    // In classic data mode, shorter/equal rename succeeds; longer fails.
    EXPECT_EQ(NC_ENOTINDEFINE,
              nc_rename_att(nc, NC_GLOBAL, "long_name", "much_longer_name"));
    ASSERT_EQ(NC_NOERR, nc_rename_att(nc, NC_GLOBAL, "long_name", "short"));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  // Standard NetCDF-4 rename validation and created/uncreated attribute paths.
  TempFile file("attr_rename_persist.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(file.c_str(), NC_NETCDF4, nc.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v", NC_INT, 1, &dimid, &varid));

    const int v1 = 101;
    const int v2 = 202;
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, NC_GLOBAL, "g_one", NC_INT, 1, &v1));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, NC_GLOBAL, "g_two", NC_INT, 1, &v2));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, varid, "v_one", NC_INT, 1, &v1));

    const std::string too_long(NC_MAX_NAME + 1, 'r');
    EXPECT_EQ(NC_EMAXNAME,
              nc_rename_att(nc, NC_GLOBAL, "g_one", too_long.c_str()));
    EXPECT_EQ(NC_EBADNAME, nc_rename_att(nc, NC_GLOBAL, "g_one", ""));
    EXPECT_EQ(NC_EBADNAME, nc_rename_att(nc, NC_GLOBAL, "g_one", "bad/name"));
    EXPECT_EQ(NC_ENOTVAR, nc_rename_att(nc, 99, "g_one", "other"));
    EXPECT_EQ(NC_ENAMEINUSE, nc_rename_att(nc, NC_GLOBAL, "g_one", "g_two"));
    EXPECT_EQ(NC_EBADNAME, nc_rename_att(nc, NC_GLOBAL, "\xFF", "valid_new"));
    EXPECT_EQ(NC_ENOTATT, nc_rename_att(nc, NC_GLOBAL, "missing", "valid_new"));

    // Rename uncreated global & var attributes while still in define mode.
    ASSERT_EQ(NC_NOERR, nc_rename_att(nc, NC_GLOBAL, "g_one", "g_def_renamed"));
    ASSERT_EQ(NC_NOERR, nc_rename_att(nc, varid, "v_one", "v_def_renamed"));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  // Reopen read-only: rename must return NC_EPERM.
  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro.ptr()));
    EXPECT_EQ(NC_EPERM,
              nc_rename_att(ro, NC_GLOBAL, "g_def_renamed", "denied"));
  }

  // Reopen NC_WRITE: triggers lazy att read and HDF5 H5Adelete() on both
  // root NC_GLOBAL and variable datasets when renaming longer in data mode.
  {
    ScopedNcFile rw;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_WRITE, rw.ptr()));
    ASSERT_EQ(NC_NOERR, nc_rename_att(rw, NC_GLOBAL, "g_def_renamed",
                                      "g_persisted_much_longer"));
    ASSERT_EQ(NC_NOERR,
              nc_rename_att(rw, 0, "v_def_renamed", "v_persisted_much_longer"));
    ASSERT_EQ(NC_NOERR, rw.close());
  }

  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro.ptr()));
    nc_type t = NC_NAT;
    EXPECT_EQ(NC_ENOTATT, nc_inq_atttype(ro, NC_GLOBAL, "g_def_renamed", &t));
    EXPECT_EQ(NC_ENOTATT, nc_inq_atttype(ro, 0, "v_def_renamed", &t));
    int g_val = 0;
    int v_val = 0;
    ASSERT_EQ(NC_NOERR,
              nc_get_att_int(ro, NC_GLOBAL, "g_persisted_much_longer", &g_val));
    ASSERT_EQ(NC_NOERR,
              nc_get_att_int(ro, 0, "v_persisted_much_longer", &v_val));
    EXPECT_EQ(101, g_val);
    EXPECT_EQ(101, v_val);
  }
}

TEST(Hdf5AttrTest, DelAttErrorsLazyLoadRenumberingAndStringReclaim) {
  // Invalid ncid fails in dispatch NC_check_id() before NC4_HDF5_del_att().
  EXPECT_EQ(NC_EBADID, nc_del_att(-1, NC_GLOBAL, nullptr));
  EXPECT_EQ(NC_EBADID, nc_del_att(-1, NC_GLOBAL, "a"));

  // Classic-model data-mode restriction and read-only NC_EPERM.
  TempFile classic_file("attr_del_classic.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(classic_file.c_str(),
                                  NC_NETCDF4 | NC_CLASSIC_MODEL, nc.ptr()));
    // Valid ncid reaches NC4_HDF5_del_att()'s null name check.
    EXPECT_EQ(NC_EINVAL, nc_del_att(nc, NC_GLOBAL, nullptr));
    const int bad_grpid = nc.get() | 0x00FF;
    EXPECT_EQ(NC_EBADID, nc_del_att(bad_grpid, NC_GLOBAL, "a"));
    const int v = 1;
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, NC_GLOBAL, "g", NC_INT, 1, &v));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));
    EXPECT_EQ(NC_ENOTINDEFINE, nc_del_att(nc, NC_GLOBAL, "g"));
    ASSERT_EQ(NC_NOERR, nc.close());
  }
  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(classic_file.c_str(), NC_NOWRITE, ro.ptr()));
    EXPECT_EQ(NC_EPERM, nc_del_att(ro, NC_GLOBAL, "g"));
  }

  // Standard NetCDF-4: uncreated, zero-length, string-reclaim, created on
  // disk, auto-redef, lazy read, and attribute-number renumbering.
  TempFile file("attr_del_persist.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(file.c_str(), NC_NETCDF4, nc.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v", NC_INT, 1, &dimid, &varid));

    EXPECT_EQ(NC_ENOTVAR, nc_del_att(nc, 99, "g"));
    EXPECT_EQ(NC_ENOTATT, nc_del_att(nc, NC_GLOBAL, "nonexistent"));

    // Uncreated empty, non-empty, and string attributes deleted in define mode.
    const int val = 42;
    const char* str_temp = "ephemeral";
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc, varid, "u_empty", NC_INT, 0, nullptr));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, varid, "u_int", NC_INT, 1, &val));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_string(nc, NC_GLOBAL, "u_str", 1, &str_temp));
    ASSERT_EQ(NC_NOERR, nc_del_att(nc, varid, "u_empty"));
    ASSERT_EQ(NC_NOERR, nc_del_att(nc, varid, "u_int"));
    ASSERT_EQ(NC_NOERR, nc_del_att(nc, NC_GLOBAL, "u_str"));

    // Persist 3 global and 3 variable attributes (one NC_STRING).
    const int a0 = 10;
    const int a1 = 20;
    const int a2 = 30;
    const char* vs0 = "delete_me";
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, NC_GLOBAL, "g0", NC_INT, 1, &a0));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, NC_GLOBAL, "g1", NC_INT, 1, &a1));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, NC_GLOBAL, "g2", NC_INT, 1, &a2));
    ASSERT_EQ(NC_NOERR, nc_put_att_string(nc, varid, "v0", 1, &vs0));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, varid, "v1", NC_INT, 1, &a1));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, varid, "v2", NC_INT, 1, &a2));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  // Reopen NC_WRITE in data mode and delete g1 (middle global attribute,
  // hitting both false (g0) and true (g2) branches of id renumbering, plus
  // lazy grp att read, NC4_redef, and H5Adelete).
  {
    ScopedNcFile rw;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_WRITE, rw.ptr()));
    ASSERT_EQ(NC_NOERR, nc_del_att(rw, NC_GLOBAL, "g1"));
    ASSERT_EQ(NC_NOERR, rw.close());
  }

  // Reopen NC_WRITE again and delete v0 on variable (triggers lazy var att
  // read, var->created HDF5 delete, NC_STRING reclaim, and renumbering).
  {
    ScopedNcFile rw;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_WRITE, rw.ptr()));
    ASSERT_EQ(NC_NOERR, nc_del_att(rw, 0, "v0"));
    ASSERT_EQ(NC_NOERR, rw.close());
  }

  // Reopen read-only and verify remaining attributes and renumbered attnums.
  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro.ptr()));
    int attid = -1;
    EXPECT_EQ(NC_ENOTATT, nc_inq_attid(ro, NC_GLOBAL, "g1", &attid));
    EXPECT_EQ(NC_ENOTATT, nc_inq_attid(ro, 0, "v0", &attid));
    ASSERT_EQ(NC_NOERR, nc_inq_attid(ro, NC_GLOBAL, "g0", &attid));
    EXPECT_EQ(0, attid);
    ASSERT_EQ(NC_NOERR, nc_inq_attid(ro, NC_GLOBAL, "g2", &attid));
    EXPECT_EQ(1, attid);
    ASSERT_EQ(NC_NOERR, nc_inq_attid(ro, 0, "v1", &attid));
    EXPECT_EQ(0, attid);
    ASSERT_EQ(NC_NOERR, nc_inq_attid(ro, 0, "v2", &attid));
    EXPECT_EQ(1, attid);
  }
}

TEST(Hdf5AttrTest, LazyInqAndGetAttAcrossFreshHandlesAndNullOutputs) {
  TempFile file("attr_lazy_inq.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(file.c_str(), NC_NETCDF4, nc.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d", 1, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v", NC_INT, 1, &dimid, &varid));
    const int gv = 11;
    const int vv = 22;
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, NC_GLOBAL, "ga", NC_INT, 1, &gv));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, varid, "va", NC_INT, 1, &vv));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  // Handle 1: nc_inq_att triggers lazy att read with NULL output pointers.
  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro.ptr()));
    EXPECT_EQ(NC_EBADID,
              nc_inq_att(ro.get() | 0x00FF, NC_GLOBAL, "ga", nullptr, nullptr));
    ASSERT_EQ(NC_NOERR, nc_inq_att(ro, NC_GLOBAL, "ga", nullptr, nullptr));
    ASSERT_EQ(NC_NOERR, nc_inq_att(ro, 0, "va", nullptr, nullptr));
  }

  // Handle 2: nc_inq_attid triggers lazy att read.
  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro.ptr()));
    int id = -1;
    EXPECT_EQ(NC_EBADID, nc_inq_attid(ro.get() | 0x00FF, NC_GLOBAL, "ga", &id));
    ASSERT_EQ(NC_NOERR, nc_inq_attid(ro, NC_GLOBAL, "ga", &id));
    EXPECT_EQ(0, id);
    ASSERT_EQ(NC_NOERR, nc_inq_attid(ro, 0, "va", &id));
    EXPECT_EQ(0, id);
  }

  // Handle 3: nc_inq_attname triggers lazy att read and handles name ==
  // nullptr.
  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro.ptr()));
    char name_buf[NC_MAX_NAME + 1] = {};
    EXPECT_EQ(NC_EBADID,
              nc_inq_attname(ro.get() | 0x00FF, NC_GLOBAL, 0, name_buf));
    ASSERT_EQ(NC_NOERR, nc_inq_attname(ro, NC_GLOBAL, 0, nullptr));
    ASSERT_EQ(NC_NOERR, nc_inq_attname(ro, 0, 0, name_buf));
    EXPECT_STREQ("va", name_buf);
  }

  // Handle 4: nc_get_att_int triggers lazy att read directly.
  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro.ptr()));
    int gv_in = 0;
    int vv_in = 0;
    EXPECT_EQ(NC_EBADID,
              nc_get_att_int(ro.get() | 0x00FF, NC_GLOBAL, "ga", &gv_in));
    ASSERT_EQ(NC_NOERR, nc_get_att_int(ro, NC_GLOBAL, "ga", &gv_in));
    ASSERT_EQ(NC_NOERR, nc_get_att_int(ro, 0, "va", &vv_in));
    EXPECT_EQ(11, gv_in);
    EXPECT_EQ(22, vv_in);
  }
}

}  // namespace
}  // namespace netcdf
