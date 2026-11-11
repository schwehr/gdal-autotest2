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

// Clean-room unit tests for third_party/netcdf/libdispatch/dvarinq.c.

#include <cstddef>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
// clang-format on
#include "third_party/netcdf/google/libdispatch/dvarinq_shim.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"

namespace netcdf {
namespace {

using ::testing::ElementsAre;

TEST(DvarinqTest, InqVarBasicAndFieldHelpersClassicAndNetcdf4) {
  int varid = -1;
  char name[NC_MAX_NAME + 1]{};
  nc_type xtype = NC_NAT;
  int ndims = -1;
  int dimids[NC_MAX_VAR_DIMS]{};
  int natts = -1;

  EXPECT_EQ(NC_EBADID, nc_inq_varid(-1, "v", &varid));
  EXPECT_EQ(NC_EBADID, nc_inq_var(-1, 0, name, &xtype, &ndims, dimids, &natts));
  EXPECT_EQ(NC_EBADID, nc_inq_varname(-1, 0, name));
  EXPECT_EQ(NC_EBADID, nc_inq_vartype(-1, 0, &xtype));
  EXPECT_EQ(NC_EBADID, nc_inq_varndims(-1, 0, &ndims));
  EXPECT_EQ(NC_EBADID, nc_inq_vardimid(-1, 0, dimids));
  EXPECT_EQ(NC_EBADID, nc_inq_varnatts(-1, 0, &natts));
  EXPECT_EQ(NC_EBADID, nc_inq_varnatts(-1, NC_GLOBAL, &natts));

  // Classic in-memory dataset with a 2D var, a scalar var, and global atts.
  {
    ScopedNcFile classic;
    ASSERT_EQ(NC_NOERR, nc_create("dvarinq_basic_nc3.nc",
                                  NC_CLOBBER | NC_DISKLESS, classic.ptr()));

    int d_lat = -1;
    int d_lon = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(classic.get(), "lat", 3, &d_lat));
    ASSERT_EQ(NC_NOERR, nc_def_dim(classic.get(), "lon", 4, &d_lon));
    const int dpair[2] = {d_lat, d_lon};

    int v_temp = -1;
    int v_scalar = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(classic.get(), "temp", NC_FLOAT, 2, dpair, &v_temp));
    ASSERT_EQ(NC_NOERR, nc_def_var(classic.get(), "offset", NC_DOUBLE, 0,
                                   nullptr, &v_scalar));

    const float scale = 1.5F;
    ASSERT_EQ(NC_NOERR, nc_put_att_float(classic.get(), v_temp, "scale",
                                         NC_FLOAT, 1, &scale));
    const char title[] = "global";
    ASSERT_EQ(NC_NOERR, nc_put_att_text(classic.get(), NC_GLOBAL, "title",
                                        sizeof(title) - 1, title));
    ASSERT_EQ(NC_NOERR, nc_enddef(classic.get()));

    // NC3_inq_varid passes name directly to nc_utf8_normalize without a NULL
    // check when variables exist, so test empty/missing names on classic and
    // reserve NULL name validation for NetCDF-4 below.
    EXPECT_EQ(NC_ENOTVAR, nc_inq_varid(classic.get(), "", &varid));
    EXPECT_EQ(NC_ENOTVAR, nc_inq_varid(classic.get(), "missing", &varid));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(classic.get(), "temp", &varid));
    EXPECT_EQ(varid, v_temp);

    EXPECT_EQ(NC_ENOTVAR, nc_inq_var(classic.get(), 99, name, &xtype, &ndims,
                                     dimids, &natts));
    std::memset(name, 0, sizeof(name));
    ASSERT_EQ(NC_NOERR, nc_inq_var(classic.get(), v_temp, name, &xtype, &ndims,
                                   dimids, &natts));
    EXPECT_STREQ(name, "temp");
    EXPECT_EQ(xtype, NC_FLOAT);
    EXPECT_EQ(ndims, 2);
    EXPECT_EQ(dimids[0], d_lat);
    EXPECT_EQ(dimids[1], d_lon);
    EXPECT_EQ(natts, 1);

    std::memset(name, 0, sizeof(name));
    xtype = NC_NAT;
    ndims = -1;
    dimids[0] = dimids[1] = -1;
    natts = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varname(classic.get(), v_scalar, name));
    EXPECT_STREQ(name, "offset");
    ASSERT_EQ(NC_NOERR, nc_inq_vartype(classic.get(), v_scalar, &xtype));
    EXPECT_EQ(xtype, NC_DOUBLE);
    ASSERT_EQ(NC_NOERR, nc_inq_varndims(classic.get(), v_scalar, &ndims));
    EXPECT_EQ(ndims, 0);
    ASSERT_EQ(NC_NOERR, nc_inq_vardimid(classic.get(), v_temp, dimids));
    EXPECT_EQ(dimids[0], d_lat);
    EXPECT_EQ(dimids[1], d_lon);
    ASSERT_EQ(NC_NOERR, nc_inq_varnatts(classic.get(), v_temp, &natts));
    EXPECT_EQ(natts, 1);
    ASSERT_EQ(NC_NOERR, nc_inq_varnatts(classic.get(), NC_GLOBAL, &natts));
    EXPECT_EQ(natts, 1);

    // Null out-parameters must succeed without writing through null pointers.
    EXPECT_EQ(NC_NOERR, nc_inq_var(classic.get(), v_temp, nullptr, nullptr,
                                   nullptr, nullptr, nullptr));
    EXPECT_EQ(NC_NOERR, nc_inq_varnatts(classic.get(), NC_GLOBAL, nullptr));
  }

  // NetCDF-4 group + variable metadata inquiry.
  {
    ScopedNcFile nc4;
    ASSERT_EQ(NC_NOERR,
              nc_create("dvarinq_basic_nc4.nc",
                        NC_CLOBBER | NC_NETCDF4 | NC_DISKLESS, nc4.ptr()));
    int grp_id = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc4.get(), "subgrp", &grp_id));
    int d_x = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc4.get(), "x", 5, &d_x));
    int v_id = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(grp_id, "field", NC_INT64, 1, &d_x, &v_id));
    const int gatt = 7;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(grp_id, NC_GLOBAL, "gflag", NC_INT, 1, &gatt));

    EXPECT_EQ(NC_EINVAL, nc_inq_varid(grp_id, nullptr, &varid));
    EXPECT_EQ(NC_NOERR, nc_inq_varid(grp_id, "field", nullptr));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(grp_id, "field", &varid));
    EXPECT_EQ(varid, v_id);

    std::memset(name, 0, sizeof(name));
    ASSERT_EQ(NC_NOERR,
              nc_inq_var(grp_id, v_id, name, &xtype, &ndims, dimids, &natts));
    EXPECT_STREQ(name, "field");
    EXPECT_EQ(xtype, NC_INT64);
    EXPECT_EQ(ndims, 1);
    EXPECT_EQ(dimids[0], d_x);
    EXPECT_EQ(natts, 0);

    ASSERT_EQ(NC_NOERR, nc_inq_varnatts(grp_id, NC_GLOBAL, &natts));
    EXPECT_EQ(natts, 1);
    EXPECT_EQ(NC_ENOTVAR, nc_inq_varname(grp_id, 42, name));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc4.get()));
  }
}

TEST(DvarinqTest, InqVarDeflateAllBranches) {
  int shuffle = -1;
  int deflate = -1;
  int level = -1;
  EXPECT_EQ(NC_EBADID, nc_inq_var_deflate(-1, 0, &shuffle, &deflate, &level));

  // Classic dataset: filter_info returns NC_ENOFILTER, inq_var_all reports 0.
  {
    ScopedNcFile classic;
    ASSERT_EQ(NC_NOERR, nc_create("dvarinq_deflate_nc3.nc",
                                  NC_CLOBBER | NC_DISKLESS, classic.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(classic.get(), "n", 4, &dimid));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(classic.get(), "v", NC_INT, 1, &dimid, &varid));

    EXPECT_EQ(NC_ENOTVAR, nc_inq_var_deflate(classic.get(), 99, &shuffle,
                                             &deflate, &level));

    ASSERT_EQ(NC_NOERR, nc_inq_var_deflate(classic.get(), varid, &shuffle,
                                           &deflate, &level));
    EXPECT_EQ(shuffle, 0);
    EXPECT_EQ(deflate, 0);
    EXPECT_EQ(level, 0);

    // Null shufflep hits early-return branch before inq_var_all.
    deflate = -1;
    level = -1;
    EXPECT_EQ(NC_NOERR, nc_inq_var_deflate(classic.get(), varid, nullptr,
                                           &deflate, &level));
    EXPECT_EQ(deflate, 0);
    EXPECT_EQ(level, 0);
  }

  // NetCDF-4 dataset: default (uncompressed), shuffle-only, and
  // deflate+shuffle.
  {
    ScopedNcFile nc4;
    ASSERT_EQ(NC_NOERR,
              nc_create("dvarinq_deflate_nc4.nc",
                        NC_CLOBBER | NC_NETCDF4 | NC_DISKLESS, nc4.ptr()));
    int dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc4.get(), "n", 8, &dimid));

    int v_none = -1;
    int v_shuf = -1;
    int v_both = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc4.get(), "v_none", NC_INT, 1, &dimid, &v_none));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc4.get(), "v_shuf", NC_INT, 1, &dimid, &v_shuf));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc4.get(), "v_both", NC_INT, 1, &dimid, &v_both));
    ASSERT_EQ(NC_NOERR, nc_def_var_deflate(nc4.get(), v_shuf, 1, 0, 0));
    ASSERT_EQ(NC_NOERR, nc_def_var_deflate(nc4.get(), v_both, 1, 1, 5));

    // Invalid varid hits default switch return in nc_inq_var_deflate.
    EXPECT_EQ(NC_ENOTVAR,
              nc_inq_var_deflate(nc4.get(), 55, &shuffle, &deflate, &level));

    shuffle = -1;
    deflate = -1;
    level = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_var_deflate(nc4.get(), v_none, &shuffle,
                                           &deflate, &level));
    EXPECT_EQ(shuffle, 0);
    EXPECT_EQ(deflate, 0);
    EXPECT_EQ(level, 0);

    shuffle = -1;
    deflate = -1;
    level = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_var_deflate(nc4.get(), v_shuf, &shuffle,
                                           &deflate, &level));
    EXPECT_EQ(shuffle, 1);
    EXPECT_EQ(deflate, 0);
    EXPECT_EQ(level, 0);

    shuffle = -1;
    deflate = -1;
    level = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_var_deflate(nc4.get(), v_both, &shuffle,
                                           &deflate, &level));
    EXPECT_EQ(shuffle, 1);
    EXPECT_EQ(deflate, 1);
    EXPECT_EQ(level, 5);

    // Verify NULL out-pointers both without and with deflate enabled.
    EXPECT_EQ(NC_NOERR,
              nc_inq_var_deflate(nc4.get(), v_none, nullptr, nullptr, nullptr));
    EXPECT_EQ(NC_NOERR,
              nc_inq_var_deflate(nc4.get(), v_both, nullptr, nullptr, nullptr));
    deflate = -1;
    EXPECT_EQ(NC_NOERR, nc_inq_var_deflate(nc4.get(), v_both, nullptr, &deflate,
                                           nullptr));
    EXPECT_EQ(deflate, 1);
    ASSERT_EQ(NC_NOERR, nc_enddef(nc4.get()));
  }
}

TEST(DvarinqTest, InqVarFletcher32ChunkingFillEndian) {
  int fletcher = -1;
  int storage = -1;
  size_t chunks[2] = {0, 0};
  int no_fill = -1;
  int fill_val = 0;
  int endian = -1;

  EXPECT_EQ(NC_EBADID, nc_inq_var_fletcher32(-1, 0, &fletcher));
  EXPECT_EQ(NC_EBADID, nc_inq_var_chunking(-1, 0, &storage, chunks));
  EXPECT_EQ(NC_EBADID, nc_inq_var_fill(-1, 0, &no_fill, &fill_val));
  EXPECT_EQ(NC_EBADID, nc_inq_var_endian(-1, 0, &endian));

  // Classic dataset behavior.
  {
    ScopedNcFile classic;
    ASSERT_EQ(NC_NOERR, nc_create("dvarinq_props_nc3.nc",
                                  NC_CLOBBER | NC_DISKLESS, classic.ptr()));
    int dimid = -1;
    int v_def = -1;
    int v_cust = -1;
    int v_nofill = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(classic.get(), "n", 6, &dimid));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(classic.get(), "v_def", NC_INT, 1, &dimid, &v_def));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(classic.get(), "v_cust", NC_INT, 1, &dimid, &v_cust));
    ASSERT_EQ(NC_NOERR, nc_def_var(classic.get(), "v_nofill", NC_INT, 1, &dimid,
                                   &v_nofill));

    const int custom_fill = -999;
    ASSERT_EQ(NC_NOERR,
              nc_def_var_fill(classic.get(), v_cust, NC_FILL, &custom_fill));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_fill(classic.get(), v_nofill, NC_NOFILL, nullptr));

    ASSERT_EQ(NC_NOERR, nc_inq_var_fletcher32(classic.get(), v_def, &fletcher));
    EXPECT_EQ(fletcher, NC_NOCHECKSUM);

    ASSERT_EQ(NC_NOERR,
              nc_inq_var_chunking(classic.get(), v_def, &storage, chunks));
    EXPECT_EQ(storage, NC_CONTIGUOUS);

    no_fill = -1;
    fill_val = 0;
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_fill(classic.get(), v_cust, &no_fill, &fill_val));
    EXPECT_EQ(no_fill, 0);
    EXPECT_EQ(fill_val, custom_fill);

    no_fill = -1;
    fill_val = 123;
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_fill(classic.get(), v_nofill, &no_fill, &fill_val));
    EXPECT_EQ(no_fill, 1);

    EXPECT_EQ(NC_ENOTNC4, nc_inq_var_endian(classic.get(), v_def, &endian));
  }

  // NetCDF-4 dataset behavior across chunking modes, checksum, fill, endian.
  {
    ScopedNcFile nc4;
    ASSERT_EQ(NC_NOERR,
              nc_create("dvarinq_props_nc4.nc",
                        NC_CLOBBER | NC_NETCDF4 | NC_DISKLESS, nc4.ptr()));
    int d0 = -1;
    int d1 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc4.get(), "r", 4, &d0));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc4.get(), "c", 6, &d1));
    const int d2[2] = {d0, d1};

    int v_chunked = -1;
    int v_contig = -1;
    int v_compact = -1;
    int v_endian_big = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc4.get(), "chunked", NC_INT, 2, d2, &v_chunked));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc4.get(), "contig", NC_INT, 2, d2, &v_contig));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc4.get(), "compact", NC_INT, 2, d2, &v_compact));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc4.get(), "big_end", NC_INT, 1, &d0, &v_endian_big));

    const size_t want_chunks[2] = {2, 3};
    ASSERT_EQ(NC_NOERR, nc_def_var_chunking(nc4.get(), v_chunked, NC_CHUNKED,
                                            want_chunks));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_fletcher32(nc4.get(), v_chunked, NC_FLETCHER32));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_chunking(nc4.get(), v_contig, NC_CONTIGUOUS, nullptr));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_chunking(nc4.get(), v_compact, NC_COMPACT, nullptr));
    const int fill_custom = 777;
    ASSERT_EQ(NC_NOERR,
              nc_def_var_fill(nc4.get(), v_contig, NC_FILL, &fill_custom));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_fill(nc4.get(), v_compact, NC_NOFILL, nullptr));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_endian(nc4.get(), v_endian_big, NC_ENDIAN_BIG));

    EXPECT_EQ(NC_ENOTVAR, nc_inq_var_fletcher32(nc4.get(), 88, &fletcher));
    EXPECT_EQ(NC_ENOTVAR, nc_inq_var_chunking(nc4.get(), 88, &storage, chunks));
    EXPECT_EQ(NC_ENOTVAR, nc_inq_var_fill(nc4.get(), 88, &no_fill, &fill_val));
    EXPECT_EQ(NC_ENOTVAR, nc_inq_var_endian(nc4.get(), 88, &endian));

    ASSERT_EQ(NC_NOERR, nc_inq_var_fletcher32(nc4.get(), v_chunked, &fletcher));
    EXPECT_EQ(fletcher, NC_FLETCHER32);
    ASSERT_EQ(NC_NOERR, nc_inq_var_fletcher32(nc4.get(), v_contig, &fletcher));
    EXPECT_EQ(fletcher, NC_NOCHECKSUM);
    EXPECT_EQ(NC_NOERR, nc_inq_var_fletcher32(nc4.get(), v_chunked, nullptr));

    chunks[0] = chunks[1] = 0;
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_chunking(nc4.get(), v_chunked, &storage, chunks));
    EXPECT_EQ(storage, NC_CHUNKED);
    EXPECT_EQ(chunks[0], 2U);
    EXPECT_EQ(chunks[1], 3U);

    ASSERT_EQ(NC_NOERR,
              nc_inq_var_chunking(nc4.get(), v_contig, &storage, nullptr));
    EXPECT_EQ(storage, NC_CONTIGUOUS);
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_chunking(nc4.get(), v_compact, &storage, nullptr));
    EXPECT_EQ(storage, NC_COMPACT);
    EXPECT_EQ(NC_NOERR,
              nc_inq_var_chunking(nc4.get(), v_chunked, nullptr, nullptr));

    no_fill = -1;
    fill_val = 0;
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_fill(nc4.get(), v_contig, &no_fill, &fill_val));
    EXPECT_EQ(no_fill, 0);
    EXPECT_EQ(fill_val, fill_custom);

    no_fill = -1;
    fill_val = 0;
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_fill(nc4.get(), v_chunked, &no_fill, &fill_val));
    EXPECT_EQ(no_fill, 0);
    EXPECT_EQ(fill_val, NC_FILL_INT);

    no_fill = -1;
    fill_val = -1;
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_fill(nc4.get(), v_compact, &no_fill, &fill_val));
    EXPECT_EQ(no_fill, 1);
    EXPECT_EQ(NC_NOERR, nc_inq_var_fill(nc4.get(), v_contig, nullptr, nullptr));

    ASSERT_EQ(NC_NOERR, nc_inq_var_endian(nc4.get(), v_endian_big, &endian));
    EXPECT_EQ(endian, NC_ENDIAN_BIG);
    ASSERT_EQ(NC_NOERR, nc_inq_var_endian(nc4.get(), v_contig, &endian));
    EXPECT_EQ(endian, NC_ENDIAN_NATIVE);
    EXPECT_EQ(NC_NOERR, nc_inq_var_endian(nc4.get(), v_endian_big, nullptr));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc4.get()));
  }
}

TEST(DvarinqTest, InqVarQuantizeAndUnlimdims) {
  int qmode = -1;
  int nsd = -1;
  int nunlim = -1;
  int unlimids[4]{-1, -1, -1, -1};

  EXPECT_EQ(NC_EBADID, nc_inq_var_quantize(-1, 0, &qmode, &nsd));
  EXPECT_EQ(NC_EBADID, nc_inq_unlimdims(-1, &nunlim, unlimids));

  // Classic dataset checks.
  {
    ScopedNcFile classic;
    ASSERT_EQ(NC_NOERR, nc_create("dvarinq_qu_nc3.nc", NC_CLOBBER | NC_DISKLESS,
                                  classic.ptr()));
    EXPECT_EQ(NC_EGLOBAL,
              nc_inq_var_quantize(classic.get(), NC_GLOBAL, &qmode, &nsd));

    int d_fix = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(classic.get(), "fix", 3, &d_fix));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(classic.get(), "f", NC_FLOAT, 1, &d_fix, &varid));
    EXPECT_EQ(NC_ENOTNC4,
              nc_inq_var_quantize(classic.get(), varid, &qmode, &nsd));

    nunlim = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_unlimdims(classic.get(), &nunlim, unlimids));
    EXPECT_EQ(nunlim, 0);

    int d_unlim = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_dim(classic.get(), "rec", NC_UNLIMITED, &d_unlim));
    nunlim = -1;
    unlimids[0] = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_unlimdims(classic.get(), &nunlim, unlimids));
    EXPECT_EQ(nunlim, 1);
    EXPECT_EQ(unlimids[0], d_unlim);
    EXPECT_EQ(NC_NOERR, nc_inq_unlimdims(classic.get(), nullptr, nullptr));
  }

  // NetCDF-4 quantize modes and multiple unlimited dimensions.
  {
    ScopedNcFile nc4;
    ASSERT_EQ(NC_NOERR,
              nc_create("dvarinq_qu_nc4.nc",
                        NC_CLOBBER | NC_NETCDF4 | NC_DISKLESS, nc4.ptr()));
    EXPECT_EQ(NC_EGLOBAL,
              nc_inq_var_quantize(nc4.get(), NC_GLOBAL, &qmode, &nsd));

    nunlim = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_unlimdims(nc4.get(), &nunlim, nullptr));
    EXPECT_EQ(nunlim, 0);

    int du0 = -1;
    int du1 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc4.get(), "t0", NC_UNLIMITED, &du0));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc4.get(), "t1", NC_UNLIMITED, &du1));

    nunlim = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_unlimdims(nc4.get(), &nunlim, nullptr));
    ASSERT_EQ(nunlim, 2);
    std::vector<int> got_unlim(static_cast<size_t>(nunlim), -1);
    ASSERT_EQ(NC_NOERR, nc_inq_unlimdims(nc4.get(), nullptr, got_unlim.data()));
    EXPECT_THAT(got_unlim, ElementsAre(du0, du1));

    int v_none = -1;
    int v_bg = -1;
    int v_gbr = -1;
    int v_br = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc4.get(), "v_none", NC_FLOAT, 1, &du0, &v_none));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc4.get(), "v_bg", NC_FLOAT, 1, &du0, &v_bg));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc4.get(), "v_gbr", NC_DOUBLE, 1, &du0, &v_gbr));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc4.get(), "v_br", NC_FLOAT, 1, &du0, &v_br));

    ASSERT_EQ(NC_NOERR,
              nc_def_var_quantize(nc4.get(), v_bg, NC_QUANTIZE_BITGROOM, 3));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_quantize(nc4.get(), v_gbr, NC_QUANTIZE_GRANULARBR, 7));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_quantize(nc4.get(), v_br, NC_QUANTIZE_BITROUND, 10));

    EXPECT_EQ(NC_ENOTVAR, nc_inq_var_quantize(nc4.get(), 77, &qmode, &nsd));

    ASSERT_EQ(NC_NOERR, nc_inq_var_quantize(nc4.get(), v_none, &qmode, &nsd));
    EXPECT_EQ(qmode, NC_NOQUANTIZE);
    EXPECT_EQ(nsd, 0);

    ASSERT_EQ(NC_NOERR, nc_inq_var_quantize(nc4.get(), v_bg, &qmode, &nsd));
    EXPECT_EQ(qmode, NC_QUANTIZE_BITGROOM);
    EXPECT_EQ(nsd, 3);

    ASSERT_EQ(NC_NOERR, nc_inq_var_quantize(nc4.get(), v_gbr, &qmode, &nsd));
    EXPECT_EQ(qmode, NC_QUANTIZE_GRANULARBR);
    EXPECT_EQ(nsd, 7);

    ASSERT_EQ(NC_NOERR, nc_inq_var_quantize(nc4.get(), v_br, &qmode, &nsd));
    EXPECT_EQ(qmode, NC_QUANTIZE_BITROUND);
    EXPECT_EQ(nsd, 10);

    EXPECT_EQ(NC_NOERR, nc_inq_var_quantize(nc4.get(), v_bg, nullptr, nullptr));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc4.get()));
  }
}

TEST(DvarinqTest, InqVarSzipClassicAndNetcdf4) {
  int options_mask = -1;
  int pixels_per_block = -1;
  EXPECT_EQ(NC_EBADID,
            nc_inq_var_szip(-1, 0, &options_mask, &pixels_per_block));

  // Classic dataset: filter_info returns NC_ENOFILTER -> 0, 0.
  {
    ScopedNcFile classic;
    ASSERT_EQ(NC_NOERR, nc_create("dvarinq_szip_nc3.nc",
                                  NC_CLOBBER | NC_DISKLESS, classic.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(classic.get(), "n", 16, &dimid));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(classic.get(), "v", NC_INT, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc_inq_var_szip(classic.get(), varid, &options_mask,
                                        &pixels_per_block));
    EXPECT_EQ(options_mask, 0);
    EXPECT_EQ(pixels_per_block, 0);
  }

  // NetCDF-4 dataset: bad varid, uncompressed var, and szip-compressed var.
  {
    ScopedNcFile nc4;
    ASSERT_EQ(NC_NOERR,
              nc_create("dvarinq_szip_nc4.nc",
                        NC_CLOBBER | NC_NETCDF4 | NC_DISKLESS, nc4.ptr()));
    int d0 = -1;
    int d1 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc4.get(), "y", 4, &d0));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc4.get(), "x", 8, &d1));
    const int d2[2] = {d0, d1};

    int v_noszip = -1;
    int v_szip = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc4.get(), "noszip", NC_INT, 2, d2, &v_noszip));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc4.get(), "with_szip", NC_INT, 2, d2, &v_szip));

    EXPECT_EQ(NC_ENOTVAR,
              nc_inq_var_szip(nc4.get(), 99, &options_mask, &pixels_per_block));

    ASSERT_EQ(NC_NOERR, nc_inq_var_szip(nc4.get(), v_noszip, &options_mask,
                                        &pixels_per_block));
    EXPECT_EQ(options_mask, 0);
    EXPECT_EQ(pixels_per_block, 0);

    ASSERT_EQ(NC_NOERR, nc_def_var_szip(nc4.get(), v_szip, NC_SZIP_EC, 8));
    options_mask = 0;
    pixels_per_block = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_var_szip(nc4.get(), v_szip, &options_mask,
                                        &pixels_per_block));
    EXPECT_NE(options_mask & NC_SZIP_EC, 0);
    EXPECT_EQ(pixels_per_block, 8);

    // Null optional output pointers.
    EXPECT_EQ(NC_NOERR, nc_inq_var_szip(nc4.get(), v_szip, nullptr, nullptr));
    EXPECT_EQ(NC_NOERR,
              nc_inq_var_szip(nc4.get(), v_szip, nullptr, &pixels_per_block));
    EXPECT_EQ(NC_NOERR,
              nc_inq_var_szip(nc4.get(), v_szip, &options_mask, nullptr));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc4.get()));
  }
}

TEST(DvarinqTest, UdfDispatchCoversFilterEdgeAndErrorBranches) {
  ASSERT_EQ(NC_NOERR, netcdf_dvarinq_register_udf());

  ScopedNcFile udf;
  ASSERT_EQ(NC_NOERR, nc_create("dvarinq_udf.nc",
                                NC_CLOBBER | NC_DISKLESS | NC_UDF9, udf.ptr()));
  int dimid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(udf.get(), "n", 4, &dimid));

  int v0 = -1;
  int v1 = -1;
  int v2 = -1;
  int v3 = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(udf.get(), "v_notnc4", NC_INT, 1, &dimid, &v0));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(udf.get(), "v_bad_deflate", NC_INT, 1, &dimid, &v1));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(udf.get(), "v_short_szip", NC_INT, 1, &dimid, &v2));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(udf.get(), "v_long_szip", NC_INT, 1, &dimid, &v3));
  ASSERT_EQ(0, v0);
  ASSERT_EQ(1, v1);
  ASSERT_EQ(2, v2);
  ASSERT_EQ(3, v3);

  // varid 0: filter_info returns NC_ENOTNC4 -> nc_inq_var_deflate zero-fills.
  int shuffle = -1;
  int deflate = -1;
  int level = -1;
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_deflate(udf.get(), v0, &shuffle, &deflate, &level));
  EXPECT_EQ(shuffle, 0);
  EXPECT_EQ(deflate, 0);
  EXPECT_EQ(level, 0);
  EXPECT_EQ(NC_NOERR,
            nc_inq_var_deflate(udf.get(), v0, nullptr, nullptr, nullptr));

  // varid 0 with nc_inq_var_szip also hits case NC_ENOTNC4.
  int options_mask = -1;
  int pixels_per_block = -1;
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_szip(udf.get(), v0, &options_mask, &pixels_per_block));
  EXPECT_EQ(options_mask, 0);
  EXPECT_EQ(pixels_per_block, 0);

  // varid 1: nparams != 1 on deflate -> NC_EFILTER.
  EXPECT_EQ(NC_EFILTER,
            nc_inq_var_deflate(udf.get(), v1, &shuffle, &deflate, &level));

  // varid 2: nparams < 2 on szip -> NC_EFILTER.
  EXPECT_EQ(NC_EFILTER,
            nc_inq_var_szip(udf.get(), v2, &options_mask, &pixels_per_block));

  // varid 3: nparams > 2 on szip -> clamped to 2 params and succeeds.
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_szip(udf.get(), v3, &options_mask, &pixels_per_block));
  EXPECT_EQ(options_mask, NC_SZIP_EC);
  EXPECT_EQ(pixels_per_block, 8);
}

}  // namespace
}  // namespace netcdf
