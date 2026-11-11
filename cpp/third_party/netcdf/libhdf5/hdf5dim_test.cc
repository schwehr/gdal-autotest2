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

// Clean-room unit tests for //third_party/netcdf/libhdf5/hdf5dim.c.
//
// Covers HDF5_def_dim(), HDF5_inq_dim(), and HDF5_rename_dim() across fixed
// and unlimited dimensions, child groups, classic-model constraints,
// coordinate-variable breaking/reforming, dimscale-only dataset deletion, and
// all parameter validation error paths.

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
extern "C" {
#include "third_party/netcdf/include/nc4internal.h"
}  // extern "C"
// clang-format on

namespace netcdf {
namespace {

TEST(Hdf5DimTest, DefDimValidationAndClassicModelConstraints) {
  int bad_dimid = -1;
  EXPECT_EQ(NC_EBADID, nc_def_dim(-1, "x", 4, &bad_dimid));

  // Classic-model NetCDF-4 rules: 1 unlimited dim max, must be in define mode,
  // length must fit uint32_t.
  TempFile classic_file("dim_classic.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(classic_file.c_str(),
                                  NC_NETCDF4 | NC_CLASSIC_MODEL, nc.ptr()));

    // Bad dimension names before any dim exists.
    EXPECT_EQ(NC_EINVAL, nc_def_dim(nc, nullptr, 4, &bad_dimid));
    EXPECT_EQ(NC_EBADNAME, nc_def_dim(nc, "", 4, &bad_dimid));
    EXPECT_EQ(NC_EBADNAME, nc_def_dim(nc, "bad/slash", 4, &bad_dimid));
    const std::string too_long(NC_MAX_NAME + 1, 'd');
    EXPECT_EQ(NC_EMAXNAME, nc_def_dim(nc, too_long.c_str(), 4, &bad_dimid));

    // Define a fixed dim first, then an unlimited dim, then reject a second
    // unlimited dim (tests iterating over both non-unlimited and unlimited).
    int fixed_id = -1;
    int unlim_id = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "fixed", 5, &fixed_id));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "rec", NC_UNLIMITED, &unlim_id));
    EXPECT_EQ(NC_EUNLIMIT, nc_def_dim(nc, "rec2", NC_UNLIMITED, &bad_dimid));

    // Duplicate name and oversized length under NC_CLASSIC_MODEL.
    EXPECT_EQ(NC_ENAMEINUSE, nc_def_dim(nc, "fixed", 5, &bad_dimid));
    const size_t over_uint32 =
        static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1ULL;
    EXPECT_EQ(NC_EDIMSIZE, nc_def_dim(nc, "too_big", over_uint32, &bad_dimid));

    ASSERT_EQ(NC_NOERR, nc_enddef(nc));
    // Defining a dimension in data mode under NC_CLASSIC_MODEL must fail.
    EXPECT_EQ(NC_ENOTINDEFINE, nc_def_dim(nc, "late", 3, &bad_dimid));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  // Reopen read-only: defining a dimension fails with NC_EPERM.
  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(classic_file.c_str(), NC_NOWRITE, ro.ptr()));
    EXPECT_EQ(NC_EPERM, nc_def_dim(ro, "ro_dim", 2, &bad_dimid));
  }
}

TEST(Hdf5DimTest, DefDimStandardNetcdf4AutoRedefNullIdpAndLargeLen) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR, nc_create_mem("dim_std.nc", NC_NETCDF4, 4096, nc.ptr()));

  // Null idp is permitted.
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "anon_out", 7, nullptr));
  int anon_id = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(nc, "anon_out", &anon_id));
  EXPECT_GE(anon_id, 0);

  // Standard NetCDF-4 allows multiple unlimited dimensions and huge 64-bit
  // fixed lengths.
  int u1 = -1;
  int u2 = -1;
  int huge = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "u1", NC_UNLIMITED, &u1));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "u2", NC_UNLIMITED, &u2));
  const size_t large_len =
      static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 16ULL;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "large", large_len, &huge));

  size_t got_len = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_dimlen(nc, huge, &got_len));
  EXPECT_EQ(large_len, got_len);

  // Leaving define mode and defining another dim auto-reenters define mode.
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));
  int after_id = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "after_enddef", 9, &after_id));

  NC_memio raw_mem{};
  ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), &raw_mem));
  ScopedMemio mem(raw_mem);
  EXPECT_GT(mem.size(), 0U);
}

TEST(Hdf5DimTest, InqDimFixedAndUnlimitedAcrossGroupsAndNullOutputs) {
  char name_buf[NC_MAX_NAME + 1] = {};
  size_t len = 0;
  EXPECT_EQ(NC_EBADID, nc_inq_dim(-1, 0, name_buf, &len));

  TempFile file("dim_inq.nc");
  int rec_id = -1;
  int fix_id = -1;
  int unused_unlim_id = -1;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(file.c_str(), NC_NETCDF4, nc.ptr()));
    EXPECT_EQ(NC_EBADDIM, nc_inq_dim(nc, 0, name_buf, &len));
    EXPECT_EQ(NC_EBADDIM, nc_inq_dim(nc, -1, name_buf, &len));

    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "fix", 4, &fix_id));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "rec", NC_UNLIMITED, &rec_id));
    ASSERT_EQ(NC_NOERR,
              nc_def_dim(nc, "unused_rec", NC_UNLIMITED, &unused_unlim_id));

    int v_root = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_root", NC_INT, 1, &rec_id, &v_root));

    int grp_id = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc, "sub", &grp_id));
    int v_sub = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(grp_id, "v_sub", NC_INT, 1, &rec_id, &v_sub));

    // Before enddef/data, unlimited dimension lengths are 0.
    EXPECT_EQ(NC_NOERR, nc_inq_dimlen(nc, unused_unlim_id, &len));
    EXPECT_EQ(0U, len);
    EXPECT_EQ(NC_NOERR, nc_inq_dimlen(nc, rec_id, &len));
    EXPECT_EQ(0U, len);

    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    // Write 2 records in root var and 5 records in child group var.
    const std::array<int, 2> root_vals = {1, 2};
    const size_t start0 = 0;
    const size_t count2 = root_vals.size();
    ASSERT_EQ(NC_NOERR,
              nc_put_vara_int(nc, v_root, &start0, &count2, root_vals.data()));

    const std::array<int, 5> sub_vals = {10, 20, 30, 40, 50};
    const size_t count5 = sub_vals.size();
    ASSERT_EQ(NC_NOERR, nc_put_vara_int(grp_id, v_sub, &start0, &count5,
                                        sub_vals.data()));

    // Query with both pointers, name only, len only, and both NULL.
    ASSERT_EQ(NC_NOERR, nc_inq_dim(nc, fix_id, name_buf, &len));
    EXPECT_STREQ("fix", name_buf);
    EXPECT_EQ(4U, len);
    ASSERT_EQ(NC_NOERR, nc_inq_dim(nc, fix_id, nullptr, nullptr));

    ASSERT_EQ(NC_NOERR, nc_inq_dim(grp_id, rec_id, name_buf, &len));
    EXPECT_STREQ("rec", name_buf);
    EXPECT_EQ(5U, len);
    ASSERT_EQ(NC_NOERR, nc_inq_dimname(nc, rec_id, name_buf));
    EXPECT_STREQ("rec", name_buf);
    ASSERT_EQ(NC_NOERR, nc_inq_dimlen(nc, unused_unlim_id, &len));
    EXPECT_EQ(0U, len);

    ASSERT_EQ(NC_NOERR, nc.close());
  }

  // Reopen from disk and confirm fixed and unlimited lengths persist.
  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro.ptr()));
    ASSERT_EQ(NC_NOERR, nc_inq_dim(ro, fix_id, name_buf, &len));
    EXPECT_STREQ("fix", name_buf);
    EXPECT_EQ(4U, len);
    ASSERT_EQ(NC_NOERR, nc_inq_dim(ro, rec_id, name_buf, &len));
    EXPECT_STREQ("rec", name_buf);
    EXPECT_EQ(5U, len);
    ASSERT_EQ(NC_NOERR, nc_inq_dim(ro, unused_unlim_id, name_buf, &len));
    EXPECT_STREQ("unused_rec", name_buf);
    EXPECT_EQ(0U, len);
  }
}

TEST(Hdf5DimTest, RenameDimValidationErrors) {
  EXPECT_EQ(NC_EBADID, nc_rename_dim(-1, 0, nullptr));
  EXPECT_EQ(NC_EBADID, nc_rename_dim(-1, 0, "newname"));

  TempFile file("dim_rename_err.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(file.c_str(), NC_NETCDF4, nc.ptr()));
    int d0 = -1;
    int d1 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d0", 3, &d0));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d1", 4, &d1));

    EXPECT_EQ(NC_EINVAL, nc_rename_dim(nc, d0, nullptr));
    const int bad_grp_ncid = static_cast<int>(
        (static_cast<unsigned int>(nc.get()) & 0xFFFF0000u) | 0x00FFu);
    EXPECT_EQ(NC_EBADID, nc_rename_dim(bad_grp_ncid, d0, "newname"));
    EXPECT_EQ(NC_EBADNAME, nc_rename_dim(nc, d0, ""));
    EXPECT_EQ(NC_EBADNAME, nc_rename_dim(nc, d0, "a/b"));
    const std::string too_long(NC_MAX_NAME + 1, 'n');
    EXPECT_EQ(NC_EMAXNAME, nc_rename_dim(nc, d0, too_long.c_str()));
    EXPECT_EQ(NC_EBADDIM, nc_rename_dim(nc, 99, "valid"));
    EXPECT_EQ(NC_ENAMEINUSE, nc_rename_dim(nc, d0, "d0"));
    EXPECT_EQ(NC_ENAMEINUSE, nc_rename_dim(nc, d0, "d1"));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  ScopedNcFile ro;
  ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro.ptr()));
  EXPECT_EQ(NC_EPERM, nc_rename_dim(ro, 0, "d0_new"));
}

TEST(Hdf5DimTest, RenameDimWithoutCoordVarDeletesDimscaleDatasetAndRebinds) {
  TempFile file("dim_rename_dimscale.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(file.c_str(), NC_NETCDF4, nc.ptr()));

    int d_def = -1;
    int d_used = -1;
    int d_other = -1;
    int d_unused = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d_def", 2, &d_def));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d_used", 3, &d_used));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d_other", 2, &d_other));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d_unused", 2, &d_unused));

    // Rename a dimension still in define mode (no HDF5 dimscale dataset yet).
    ASSERT_EQ(NC_NOERR, nc_rename_dim(nc, d_def, "d_def_renamed"));

    int v_used = -1;
    int v_target = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_used", NC_INT, 1, &d_used, &v_used));
    // Define non-coord 1D variable "target" using d_other, NOT d_used.
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "target", NC_INT, 1, &d_other, &v_target));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    const std::array<int, 3> used_vals = {11, 22, 33};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, v_used, used_vals.data()));
    const std::array<int, 2> target_vals = {44, 55};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, v_target, target_vals.data()));

    // Renaming to non-coord variable name "target" (which uses d_other, not
    // d_used) deletes d_used's dimscale dataset without reforming a coord var.
    // Rename again to "target_dim" before close() so write_dim() does not
    // collide with dataset "target" in HDF5.
    ASSERT_EQ(NC_NOERR, nc_rename_dim(nc, d_used, "target"));
    char renamed_buf[NC_MAX_NAME + 1] = {};
    ASSERT_EQ(NC_NOERR, nc_inq_dimname(nc, d_used, renamed_buf));
    EXPECT_STREQ("target", renamed_buf);
    ASSERT_EQ(NC_NOERR, nc_rename_dim(nc, d_used, "target_dim"));
    ASSERT_EQ(NC_NOERR, nc_rename_dim(nc, d_unused, "d_unused_renamed"));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  // Reopen NC_WRITE so dimscale datasets opened from disk are also tested.
  {
    ScopedNcFile rw;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_WRITE, rw.ptr()));
    // Populate dimscale_attached metadata on all opened variables before
    // renaming dimensions on disk.
    int rw_v_used = -1;
    int rw_v_target = -1;
    int ndims = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(rw, "v_used", &rw_v_used));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(rw, "target", &rw_v_target));
    ASSERT_EQ(NC_NOERR, nc_inq_varndims(rw, rw_v_used, &ndims));
    EXPECT_EQ(1, ndims);
    ASSERT_EQ(NC_NOERR, nc_inq_varndims(rw, rw_v_target, &ndims));
    EXPECT_EQ(1, ndims);

    int id_def = -1;
    int id_target_dim = -1;
    int id_unused = -1;
    int missing_dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_dimid(rw, "d_def_renamed", &id_def));
    ASSERT_EQ(NC_NOERR, nc_inq_dimid(rw, "target_dim", &id_target_dim));
    ASSERT_EQ(NC_NOERR, nc_inq_dimid(rw, "d_unused_renamed", &id_unused));
    EXPECT_EQ(NC_EBADDIM, nc_inq_dimid(rw, "d_used", &missing_dimid));

    // Rename dimension opened from disk with active dimscale dataset.
    ASSERT_EQ(NC_NOERR, nc_rename_dim(rw, id_target_dim, "target_dim2"));
    ASSERT_EQ(NC_NOERR, rw.close());
  }

  // Verify variable data and renamed dimensions persist on disk.
  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro.ptr()));
    int id_t2 = -1;
    int ro_v_used = -1;
    int ro_v_target = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_dimid(ro, "target_dim2", &id_t2));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "v_used", &ro_v_used));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "target", &ro_v_target));
    std::array<int, 3> used_in = {};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, ro_v_used, used_in.data()));
    EXPECT_EQ((std::array<int, 3>{11, 22, 33}), used_in);
    std::array<int, 2> target_in = {};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, ro_v_target, target_in.data()));
    EXPECT_EQ((std::array<int, 2>{44, 55}), target_in);
  }
}

TEST(Hdf5DimTest, RenameDimBreaksAndReformsCoordinateVarsInDefineAndDataModes) {
  // Part 1: Break and reform coordinate variables while still in define mode
  // (no HDF5 datasets created yet), inside an in-memory dataset verified after
  // serialization.
  {
    ScopedNcFile mem_nc;
    ASSERT_EQ(NC_NOERR, nc_create_mem("dim_coord_define.nc", NC_NETCDF4, 4096,
                                      mem_nc.ptr()));
    int d_lat = -1;
    int v_lat = -1;
    int d_x = -1;
    int v_x_axis = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(mem_nc, "lat", 3, &d_lat));
    ASSERT_EQ(NC_NOERR, nc_def_var(mem_nc, "lat", NC_INT, 1, &d_lat, &v_lat));
    ASSERT_EQ(NC_NOERR, nc_def_dim(mem_nc, "x", 2, &d_x));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(mem_nc, "x_axis", NC_DOUBLE, 1, &d_x, &v_x_axis));

    // Break "lat" coordinate var, then reform it back to "lat".
    ASSERT_EQ(NC_NOERR, nc_rename_dim(mem_nc, d_lat, "lat_broken"));
    ASSERT_EQ(NC_NOERR, nc_rename_dim(mem_nc, d_lat, "lat"));

    // Reform non-coordinate variable "x_axis" into a coordinate variable.
    ASSERT_EQ(NC_NOERR, nc_rename_dim(mem_nc, d_x, "x_axis"));

    ASSERT_EQ(NC_NOERR, nc_enddef(mem_nc));
    const std::array<int, 3> lat_vals = {-10, 0, 10};
    const std::array<double, 2> x_vals = {1.5, 2.5};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(mem_nc, v_lat, lat_vals.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_double(mem_nc, v_x_axis, x_vals.data()));

    NC_memio raw_mem{};
    ASSERT_EQ(NC_NOERR, nc_close_memio(mem_nc.release(), &raw_mem));
    ScopedMemio mem(raw_mem);
    EXPECT_GT(mem.size(), 0U);

    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open_mem("dim_coord_define.nc", NC_NOWRITE,
                                    mem.size(), mem.get()->memory, ro.ptr()));
    int ro_d_lat = -1;
    int ro_v_lat = -1;
    int ro_d_x = -1;
    int ro_v_x = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_dimid(ro, "lat", &ro_d_lat));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "lat", &ro_v_lat));
    ASSERT_EQ(NC_NOERR, nc_inq_dimid(ro, "x_axis", &ro_d_x));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "x_axis", &ro_v_x));
    std::array<int, 3> lat_in = {};
    std::array<double, 2> x_in = {};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, ro_v_lat, lat_in.data()));
    ASSERT_EQ(NC_NOERR, nc_get_var_double(ro, ro_v_x, x_in.data()));
    EXPECT_EQ(lat_vals, lat_in);
    EXPECT_DOUBLE_EQ(1.5, x_in[0]);
    EXPECT_DOUBLE_EQ(2.5, x_in[1]);
  }

  // Part 2: Break and reform persisted coordinate variables after nc_enddef
  // and across NC_WRITE reopens.
  TempFile file("dim_coord_persist.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(file.c_str(), NC_NETCDF4, nc.ptr()));
    int d_lon = -1;
    int d_plain = -1;
    int v_lon = -1;
    int v_temp = -1;
    int v_plain_coord = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "lon", 3, &d_lon));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "lon", NC_INT, 1, &d_lon, &v_lon));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "temp", NC_INT, 1, &d_lon, &v_temp));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d_plain", 2, &d_plain));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "plain_coord", NC_INT, 1, &d_plain,
                                   &v_plain_coord));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    const std::array<int, 3> lon_vals = {10, 20, 30};
    const std::array<int, 3> temp_vals = {1, 2, 3};
    const std::array<int, 2> plain_vals = {100, 200};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, v_lon, lon_vals.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, v_temp, temp_vals.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, v_plain_coord, plain_vals.data()));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  // Break the coordinate variable "lon" -> dim "lon_broken".
  {
    ScopedNcFile rw;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_WRITE, rw.ptr()));
    // Populate dimscale_attached metadata on all opened variables before
    // breaking/reforming coordinate variables on disk.
    int v_lon = -1;
    int v_temp = -1;
    int v_plain = -1;
    int ndims = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(rw, "lon", &v_lon));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(rw, "temp", &v_temp));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(rw, "plain_coord", &v_plain));
    ASSERT_EQ(NC_NOERR, nc_inq_varndims(rw, v_lon, &ndims));
    EXPECT_EQ(1, ndims);
    ASSERT_EQ(NC_NOERR, nc_inq_varndims(rw, v_temp, &ndims));
    EXPECT_EQ(1, ndims);
    ASSERT_EQ(NC_NOERR, nc_inq_varndims(rw, v_plain, &ndims));
    EXPECT_EQ(1, ndims);

    int d_lon = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_dimid(rw, "lon", &d_lon));
    ASSERT_EQ(NC_NOERR, nc_rename_dim(rw, d_lon, "lon_broken"));
    ASSERT_EQ(NC_NOERR, rw.close());
  }

  // Verify broken coordinate metadata and preserved data after Reopen 1.
  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro.ptr()));
    int d_lon_broken = -1;
    int v_lon = -1;
    int v_temp = -1;
    int v_plain_coord = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_dimid(ro, "lon_broken", &d_lon_broken));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "lon", &v_lon));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "temp", &v_temp));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "plain_coord", &v_plain_coord));

    std::array<int, 3> lon_in = {};
    std::array<int, 3> temp_in = {};
    std::array<int, 2> plain_in = {};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, v_lon, lon_in.data()));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, v_temp, temp_in.data()));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, v_plain_coord, plain_in.data()));
    EXPECT_EQ((std::array<int, 3>{10, 20, 30}), lon_in);
    EXPECT_EQ((std::array<int, 3>{1, 2, 3}), temp_in);
    EXPECT_EQ((std::array<int, 2>{100, 200}), plain_in);
  }

  // Reopen, reform "lon_broken" back into coordinate variable "lon", and
  // also promote dimscale-only "d_plain" to coordinate variable "plain_coord".
  // Sync first so write_var() replaces and recreates the newly formed
  // coordinate datasets cleanly before repopulating coordinate values.
  {
    ScopedNcFile rw;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_WRITE, rw.ptr()));
    // Query variable metadata to populate dimscale_attached before
    // rec_reattach_scales() runs during nc_sync().
    int d_lon_broken = -1;
    int d_plain = -1;
    int v_lon = -1;
    int v_temp = -1;
    int v_plain_coord = -1;
    int ndims = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(rw, "lon", &v_lon));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(rw, "temp", &v_temp));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(rw, "plain_coord", &v_plain_coord));
    ASSERT_EQ(NC_NOERR, nc_inq_varndims(rw, v_lon, &ndims));
    EXPECT_EQ(1, ndims);
    ASSERT_EQ(NC_NOERR, nc_inq_varndims(rw, v_temp, &ndims));
    EXPECT_EQ(1, ndims);
    ASSERT_EQ(NC_NOERR, nc_inq_varndims(rw, v_plain_coord, &ndims));
    EXPECT_EQ(1, ndims);

    ASSERT_EQ(NC_NOERR, nc_inq_dimid(rw, "lon_broken", &d_lon_broken));
    ASSERT_EQ(NC_NOERR, nc_inq_dimid(rw, "d_plain", &d_plain));
    ASSERT_EQ(NC_NOERR, nc_rename_dim(rw, d_lon_broken, "lon"));
    ASSERT_EQ(NC_NOERR, nc_rename_dim(rw, d_plain, "plain_coord"));
    ASSERT_EQ(NC_NOERR, nc_sync(rw));

    ASSERT_EQ(NC_NOERR, nc_inq_varid(rw, "lon", &v_lon));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(rw, "plain_coord", &v_plain_coord));
    const std::array<int, 3> updated_lon = {15, 25, 35};
    const std::array<int, 2> updated_plain = {150, 250};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(rw, v_lon, updated_lon.data()));
    ASSERT_EQ(NC_NOERR,
              nc_put_var_int(rw, v_plain_coord, updated_plain.data()));
    ASSERT_EQ(NC_NOERR, rw.close());
  }

  // Final read-only verification of all dimensions, variables, and data.
  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro.ptr()));
    int d_lon = -1;
    int d_plain_coord = -1;
    int v_lon = -1;
    int v_temp = -1;
    int v_plain_coord = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_dimid(ro, "lon", &d_lon));
    ASSERT_EQ(NC_NOERR, nc_inq_dimid(ro, "plain_coord", &d_plain_coord));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "lon", &v_lon));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "temp", &v_temp));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "plain_coord", &v_plain_coord));

    std::array<int, 3> lon_in = {};
    std::array<int, 3> temp_in = {};
    std::array<int, 2> plain_in = {};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, v_lon, lon_in.data()));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, v_temp, temp_in.data()));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, v_plain_coord, plain_in.data()));
    EXPECT_EQ((std::array<int, 3>{15, 25, 35}), lon_in);
    EXPECT_EQ((std::array<int, 3>{1, 2, 3}), temp_in);
    EXPECT_EQ((std::array<int, 2>{150, 250}), plain_in);
  }
}

TEST(Hdf5DimTest, DefInqBadGroupIdAndTooLongDim) {
  TempFile file("dim_edge_cov.nc");
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR, nc_create(file.c_str(), NC_NETCDF4, nc.ptr()));

  int d_fixed = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "fixed", 5, &d_fixed));

  const int bad_grp_ncid = static_cast<int>(
      (static_cast<unsigned int>(nc.get()) & 0xFFFF0000u) | 0x00FFu);
  int bad_dimid = -1;
  EXPECT_EQ(NC_EBADID, nc_def_dim(bad_grp_ncid, "bad_grp", 4, &bad_dimid));

  char name_buf[NC_MAX_NAME + 1] = {};
  size_t len = 0;
  EXPECT_EQ(NC_EBADID, nc_inq_dim(bad_grp_ncid, d_fixed, name_buf, &len));

  // Defining a dimension in data mode triggers HDF5_def_dim() -> NC4_redef().
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));
  int d_redef = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "after_enddef", 7, &d_redef));

  // Cover dim->too_long handling in HDF5_inq_dim().
  NC* nc_ptr = nullptr;
  NC_GRP_INFO_T* grp = nullptr;
  NC_FILE_INFO_T* h5_ptr = nullptr;
  ASSERT_EQ(NC_NOERR, nc4_find_nc_grp_h5(nc.get(), &nc_ptr, &grp, &h5_ptr));
  ASSERT_NE(nc_ptr, nullptr);
  ASSERT_NE(grp, nullptr);
  ASSERT_NE(h5_ptr, nullptr);
  NC_DIM_INFO_T* dim = nullptr;
  NC_GRP_INFO_T* dim_grp = nullptr;
  ASSERT_EQ(NC_NOERR, nc4_find_dim(grp, d_fixed, &dim, &dim_grp));
  ASSERT_NE(dim, nullptr);
  ASSERT_EQ(grp, dim_grp);

  dim->too_long = NC_TRUE;
  EXPECT_EQ(NC_EDIMSIZE, nc_inq_dim(nc, d_fixed, name_buf, &len));
  EXPECT_STREQ("fixed", name_buf);
  EXPECT_EQ(static_cast<size_t>(NC_MAX_UINT), len);
  len = 0;
  EXPECT_EQ(NC_EDIMSIZE, nc_inq_dimlen(nc, d_fixed, &len));
  EXPECT_EQ(static_cast<size_t>(NC_MAX_UINT), len);
  dim->too_long = NC_FALSE;

  ASSERT_EQ(NC_NOERR, nc.close());
}

TEST(Hdf5DimTest, RenameDimDeleteDimscaleFailureInChildGroup) {
  struct AbortOnExit {
    int ncid = -1;
    ~AbortOnExit() {
      if (ncid >= 0) {
        nc_abort(ncid);
      }
    }
  };

  TempFile file("dim_delete_dimscale_err.nc");
  AbortOnExit guard;
  ASSERT_EQ(NC_NOERR, nc_create(file.c_str(), NC_NETCDF4, &guard.ncid));

  int d_root = -1;
  int sub = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(guard.ncid, "d_root", 4, &d_root));
  ASSERT_EQ(NC_NOERR, nc_def_grp(guard.ncid, "sub", &sub));
  ASSERT_EQ(NC_NOERR, nc_enddef(guard.ncid));

  // Calling nc_rename_dim() via child group `sub` on parent dimscale-only
  // `d_root` causes delete_dimscale_dataset(sub, ...) to fail H5Gunlink().
  EXPECT_EQ(NC_EHDFERR, nc_rename_dim(sub, d_root, "renamed_from_sub"));

  char name_buf[NC_MAX_NAME + 1] = {};
  size_t len = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_dim(guard.ncid, d_root, name_buf, &len));
  EXPECT_STREQ("d_root", name_buf);
  EXPECT_EQ(size_t{4}, len);

  ASSERT_EQ(NC_NOERR, nc_abort(guard.ncid));
  guard.ncid = -1;
}

}  // namespace
}  // namespace netcdf
