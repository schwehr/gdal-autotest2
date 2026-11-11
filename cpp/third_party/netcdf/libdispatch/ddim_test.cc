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

// Unit tests for //third_party/netcdf/libdispatch/ddim.c.
//
// Exercises nc_def_dim(), nc_inq_dimid(), nc_inq_dim(), nc_rename_dim(),
// nc_inq_ndims(), nc_inq_unlimdim(), nc_inq_dimname(), and nc_inq_dimlen()
// across invalid handle rejection, null output pointer handling, all classic
// and netCDF-4 format modes, unlimited dimension growth, and subgroup scoping.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

namespace netcdf {
namespace {

using ::testing::ElementsAre;

void ExpectAllDimApisReturnBadId(int bad_ncid) {
  int id = -1;
  int ndims = -1;
  int unlim_id = -1;
  size_t len = 0;
  char name[NC_MAX_NAME + 1]{};

  EXPECT_EQ(NC_EBADID, nc_def_dim(bad_ncid, "dim_bad", 4, &id));
  EXPECT_EQ(NC_EBADID, nc_inq_dimid(bad_ncid, "dim_bad", &id));
  EXPECT_EQ(NC_EBADID, nc_inq_dim(bad_ncid, 0, name, &len));
  EXPECT_EQ(NC_EBADID, nc_rename_dim(bad_ncid, 0, "dim_renamed"));
  EXPECT_EQ(NC_EBADID, nc_inq_ndims(bad_ncid, &ndims));
  EXPECT_EQ(NC_EBADID, nc_inq_unlimdim(bad_ncid, &unlim_id));
  EXPECT_EQ(NC_EBADID, nc_inq_dimname(bad_ncid, 0, name));
  EXPECT_EQ(NC_EBADID, nc_inq_dimlen(bad_ncid, 0, &len));
}

TEST(DdimTest, InvalidOrClosedNcidReturnsBadIdForAllApis) {
  ExpectAllDimApisReturnBadId(-1);
  ExpectAllDimApisReturnBadId(0x7fffffff);

  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create("ddim_closed.nc", NC_NETCDF4 | NC_DISKLESS, nc.ptr()));
  const int closed_id = nc.get();
  ASSERT_EQ(NC_NOERR, nc.close());
  ExpectAllDimApisReturnBadId(closed_id);
}

TEST(DdimTest, NullOutputPointersAreIgnoredAcrossClassicAndNetcdf4) {
  for (const int mode : {0, NC_NETCDF4}) {
    SCOPED_TRACE(mode);
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("ddim_null_ptrs.nc", mode, 4096, nc.ptr()));

    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "d0", 5, nullptr));
    EXPECT_EQ(NC_NOERR, nc_inq_ndims(nc.get(), nullptr));
    EXPECT_EQ(NC_NOERR, nc_inq_unlimdim(nc.get(), nullptr));
    EXPECT_EQ(NC_NOERR, nc_inq_dimid(nc.get(), "d0", nullptr));
    EXPECT_EQ(NC_NOERR, nc_inq_dim(nc.get(), 0, nullptr, nullptr));
    EXPECT_EQ(NC_NOERR, nc_inq_dimname(nc.get(), 0, nullptr));
    EXPECT_EQ(NC_NOERR, nc_inq_dimlen(nc.get(), 0, nullptr));

    // Partial null arguments in nc_inq_dim() leave the requested output intact.
    char name[NC_MAX_NAME + 1]{};
    size_t len = 0;
    EXPECT_EQ(NC_NOERR, nc_inq_dim(nc.get(), 0, name, nullptr));
    EXPECT_STREQ("d0", name);
    EXPECT_EQ(NC_NOERR, nc_inq_dim(nc.get(), 0, nullptr, &len));
    EXPECT_EQ(size_t{5}, len);
  }
}

TEST(DdimTest, DefineInquireRenameAndReopenLifecycleAcrossFormats) {
  struct FormatCase {
    const char* label;
    int create_mode;
    bool classic_model;
    bool is_nc4;
  };
  constexpr FormatCase kFormats[] = {
      {"classic", 0, true, false},
      {"offset64", NC_64BIT_OFFSET, true, false},
      {"cdf5", NC_CDF5, true, false},
      {"nc4_classic", NC_NETCDF4 | NC_CLASSIC_MODEL, true, true},
      {"nc4", NC_NETCDF4, false, true},
  };

  for (const FormatCase& tc : kFormats) {
    SCOPED_TRACE(tc.label);
    ScopedMemio mem;
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR,
                nc_create_mem(absl::StrFormat("ddim_%s.nc", tc.label).c_str(),
                              tc.create_mode, 8192, nc.ptr()));

      int ndims = -1;
      int unlim_id = -99;
      ASSERT_EQ(NC_NOERR, nc_inq_ndims(nc.get(), &ndims));
      EXPECT_EQ(0, ndims);
      ASSERT_EQ(NC_NOERR, nc_inq_unlimdim(nc.get(), &unlim_id));
      EXPECT_EQ(-1, unlim_id);

      int x_id = -1;
      int y_id = -1;
      int time_id = -1;
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 4, &x_id));
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "y", 7, &y_id));
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "time", NC_UNLIMITED, &time_id));

      ASSERT_EQ(NC_NOERR, nc_inq_ndims(nc.get(), &ndims));
      EXPECT_EQ(3, ndims);
      ASSERT_EQ(NC_NOERR, nc_inq_unlimdim(nc.get(), &unlim_id));
      EXPECT_EQ(time_id, unlim_id);

      // Name and length queries agree across combined and split inquiry APIs.
      int looked_up_id = -1;
      ASSERT_EQ(NC_NOERR, nc_inq_dimid(nc.get(), "y", &looked_up_id));
      EXPECT_EQ(y_id, looked_up_id);

      char dim_name[NC_MAX_NAME + 1]{};
      size_t dim_len = 0;
      ASSERT_EQ(NC_NOERR, nc_inq_dim(nc.get(), y_id, dim_name, &dim_len));
      EXPECT_STREQ("y", dim_name);
      EXPECT_EQ(size_t{7}, dim_len);

      std::array<char, NC_MAX_NAME + 1> split_name{};
      size_t split_len = 0;
      ASSERT_EQ(NC_NOERR, nc_inq_dimname(nc.get(), time_id, split_name.data()));
      ASSERT_EQ(NC_NOERR, nc_inq_dimlen(nc.get(), time_id, &split_len));
      EXPECT_STREQ("time", split_name.data());
      EXPECT_EQ(size_t{0}, split_len);

      // Invalid names, duplicates, and missing dimension IDs are rejected.
      int scratch_id = -1;
      EXPECT_EQ(NC_ENAMEINUSE, nc_def_dim(nc.get(), "x", 10, &scratch_id));
      EXPECT_EQ(NC_EBADNAME, nc_def_dim(nc.get(), "", 4, &scratch_id));
      EXPECT_EQ(NC_EBADNAME, nc_def_dim(nc.get(), "bad/name", 4, &scratch_id));
      EXPECT_EQ(NC_EBADNAME, nc_def_dim(nc.get(), ".dot", 4, &scratch_id));
      const std::string overlong_name(NC_MAX_NAME + 1, 'a');
      EXPECT_EQ(NC_EMAXNAME,
                nc_def_dim(nc.get(), overlong_name.c_str(), 4, &scratch_id));
      EXPECT_EQ(NC_EBADDIM, nc_inq_dimid(nc.get(), "no_such_dim", &scratch_id));
      EXPECT_EQ(NC_EBADDIM, nc_inq_dim(nc.get(), -1, dim_name, &dim_len));
      EXPECT_EQ(NC_EBADDIM, nc_inq_dim(nc.get(), 99, dim_name, &dim_len));
      EXPECT_EQ(NC_EBADDIM, nc_inq_dimname(nc.get(), 99, dim_name));
      EXPECT_EQ(NC_EBADDIM, nc_inq_dimlen(nc.get(), 99, &dim_len));

      if (tc.classic_model) {
        EXPECT_EQ(NC_EUNLIMIT, nc_def_dim(nc.get(), "second_unlim",
                                          NC_UNLIMITED, &scratch_id));
      }

      // Rename while in define mode (equal length and longer replacement).
      ASSERT_EQ(NC_NOERR, nc_rename_dim(nc.get(), x_id, "z"));
      ASSERT_EQ(NC_NOERR, nc_rename_dim(nc.get(), x_id, "latitude_axis"));
      EXPECT_EQ(NC_EBADDIM, nc_inq_dimid(nc.get(), "x", &scratch_id));
      ASSERT_EQ(NC_NOERR,
                nc_inq_dimid(nc.get(), "latitude_axis", &looked_up_id));
      EXPECT_EQ(x_id, looked_up_id);
      EXPECT_EQ(NC_ENAMEINUSE, nc_rename_dim(nc.get(), x_id, "y"));
      EXPECT_EQ(NC_EBADDIM, nc_rename_dim(nc.get(), 99, "valid_unused"));
      EXPECT_EQ(NC_EBADNAME, nc_rename_dim(nc.get(), x_id, ""));
      EXPECT_EQ(NC_EMAXNAME,
                nc_rename_dim(nc.get(), x_id, overlong_name.c_str()));

      // Define a record variable and write 3 records to advance time length.
      int var_id = -1;
      ASSERT_EQ(NC_NOERR,
                nc_def_var(nc.get(), "series", NC_INT, 1, &time_id, &var_id));
      ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));

      constexpr std::array<int32_t, 3> kRecords{10, 20, 30};
      constexpr size_t kStart[1] = {0};
      constexpr size_t kCount[1] = {kRecords.size()};
      ASSERT_EQ(NC_NOERR, nc_put_vara_int(nc.get(), var_id, kStart, kCount,
                                          kRecords.data()));
      ASSERT_EQ(NC_NOERR, nc_sync(nc.get()));

      size_t rec_len = 0;
      ASSERT_EQ(NC_NOERR, nc_inq_dimlen(nc.get(), time_id, &rec_len));
      EXPECT_EQ(kRecords.size(), rec_len);

      // Verify data-mode constraints for defining dims and renaming.
      if (tc.classic_model) {
        EXPECT_EQ(NC_ENOTINDEFINE,
                  nc_def_dim(nc.get(), "late_dim", 2, &scratch_id));
      } else {
        ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "late_dim", 2, &scratch_id));
        int second_unlim = -1;
        ASSERT_EQ(NC_NOERR,
                  nc_def_dim(nc.get(), "unlim2", NC_UNLIMITED, &second_unlim));
        // Classic nc_inq_unlimdim() continues to return the first unlimited id.
        ASSERT_EQ(NC_NOERR, nc_inq_unlimdim(nc.get(), &unlim_id));
        EXPECT_EQ(time_id, unlim_id);
      }

      if (!tc.is_nc4) {
        EXPECT_EQ(NC_ENOTINDEFINE,
                  nc_rename_dim(nc.get(), x_id,
                                "longer_than_latitude_axis_allocated_slot"));
        ASSERT_EQ(NC_NOERR, nc_rename_dim(nc.get(), x_id, "xa"));
      } else {
        ASSERT_EQ(NC_NOERR, nc_rename_dim(nc.get(), x_id, "xa"));
      }

      ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), mem.get()));
    }

    // Reopen read-only and verify persistence plus write rejection.
    ScopedNcFile ro_nc;
    ASSERT_EQ(NC_NOERR, nc_open_mem("ddim_ro.nc", NC_NOWRITE, mem.size(),
                                    mem.get()->memory, ro_nc.ptr()));
    int ro_ndims = -1;
    int ro_unlim = -1;
    int ro_x_id = -1;
    size_t ro_len = 0;
    char ro_name[NC_MAX_NAME + 1]{};
    ASSERT_EQ(NC_NOERR, nc_inq_ndims(ro_nc.get(), &ro_ndims));
    EXPECT_EQ(tc.is_nc4 && !tc.classic_model ? 5 : 3, ro_ndims);
    ASSERT_EQ(NC_NOERR, nc_inq_dimid(ro_nc.get(), "xa", &ro_x_id));
    ASSERT_EQ(NC_NOERR, nc_inq_dim(ro_nc.get(), ro_x_id, ro_name, &ro_len));
    EXPECT_STREQ("xa", ro_name);
    EXPECT_EQ(size_t{4}, ro_len);

    ASSERT_EQ(NC_NOERR, nc_inq_unlimdim(ro_nc.get(), &ro_unlim));
    ASSERT_GE(ro_unlim, 0);
    ASSERT_EQ(NC_NOERR, nc_inq_dimlen(ro_nc.get(), ro_unlim, &ro_len));
    EXPECT_EQ(size_t{3}, ro_len);

    std::vector<int32_t> read_records(3, -1);
    int ro_var_id = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc.get(), "series", &ro_var_id));
    ASSERT_EQ(NC_NOERR,
              nc_get_var_int(ro_nc.get(), ro_var_id, read_records.data()));
    EXPECT_THAT(read_records, ElementsAre(10, 20, 30));

    int dummy_id = -1;
    EXPECT_EQ(NC_EPERM, nc_rename_dim(ro_nc.get(), ro_x_id, "xb"));
    EXPECT_EQ(tc.is_nc4 ? NC_EPERM : NC_ENOTINDEFINE,
              nc_def_dim(ro_nc.get(), "ro_new", 2, &dummy_id));
  }
}

TEST(DdimTest, Netcdf4SubgroupScopingAndCoordVarRename) {
  ScopedNcFile root;
  ASSERT_EQ(NC_NOERR, nc_create("ddim_subgroups.nc", NC_NETCDF4 | NC_DISKLESS,
                                root.ptr()));

  int lat_dim = -1;
  int lat_var = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(root.get(), "lat", 3, &lat_dim));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(root.get(), "lat", NC_DOUBLE, 1, &lat_dim, &lat_var));
  ASSERT_EQ(NC_NOERR, nc_enddef(root.get()));
  constexpr std::array<double, 3> kLats{-45.0, 0.0, 45.0};
  ASSERT_EQ(NC_NOERR, nc_put_var_double(root.get(), lat_var, kLats.data()));

  int child_grp = -1;
  int lon_dim = -1;
  ASSERT_EQ(NC_NOERR, nc_def_grp(root.get(), "child", &child_grp));
  ASSERT_EQ(NC_NOERR, nc_def_dim(child_grp, "lon", 6, &lon_dim));

  int child_ndims = -1;
  int child_unlim = -99;
  ASSERT_EQ(NC_NOERR, nc_inq_ndims(child_grp, &child_ndims));
  EXPECT_EQ(1, child_ndims);
  ASSERT_EQ(NC_NOERR, nc_inq_unlimdim(child_grp, &child_unlim));
  EXPECT_EQ(-1, child_unlim);

  // Ancestor dimensions are visible from subgroups via nc_inq_dimid() and
  // dimension inquiry APIs.
  int visible_lat = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(child_grp, "lat", &visible_lat));
  EXPECT_EQ(lat_dim, visible_lat);
  char child_dim_name[NC_MAX_NAME + 1]{};
  size_t child_dim_len = 0;
  ASSERT_EQ(NC_NOERR,
            nc_inq_dim(child_grp, lat_dim, child_dim_name, &child_dim_len));
  EXPECT_STREQ("lat", child_dim_name);
  EXPECT_EQ(size_t{3}, child_dim_len);

  // Renaming the root coordinate dimension breaks and can reform its link.
  ASSERT_EQ(NC_NOERR, nc_rename_dim(root.get(), lat_dim, "latitude"));
  char renamed[NC_MAX_NAME + 1]{};
  ASSERT_EQ(NC_NOERR, nc_inq_dimname(root.get(), lat_dim, renamed));
  EXPECT_STREQ("latitude", renamed);

  // Syncing with a broken coordinate variable preserves the variable's data.
  ASSERT_EQ(NC_NOERR, nc_sync(root.get()));
  std::vector<double> read_lats(3, -999.0);
  ASSERT_EQ(NC_NOERR, nc_get_var_double(root.get(), lat_var, read_lats.data()));
  EXPECT_THAT(read_lats, ElementsAre(-45.0, 0.0, 45.0));

  // Renaming back reforms the coordinate variable link. In netCDF-4/HDF5,
  // syncing an already-created variable that becomes a coordinate variable
  // replaces its HDF5 dataset (resetting values to NC_FILL_DOUBLE), after
  // which coordinate values can be rewritten and read back.
  ASSERT_EQ(NC_NOERR, nc_rename_dim(root.get(), lat_dim, "lat"));
  visible_lat = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(root.get(), "lat", &visible_lat));
  EXPECT_EQ(lat_dim, visible_lat);
  visible_lat = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(child_grp, "lat", &visible_lat));
  EXPECT_EQ(lat_dim, visible_lat);

  ASSERT_EQ(NC_NOERR, nc_sync(root.get()));
  read_lats.assign(3, -999.0);
  ASSERT_EQ(NC_NOERR, nc_get_var_double(root.get(), lat_var, read_lats.data()));
  EXPECT_THAT(read_lats,
              ElementsAre(NC_FILL_DOUBLE, NC_FILL_DOUBLE, NC_FILL_DOUBLE));

  ASSERT_EQ(NC_NOERR, nc_put_var_double(root.get(), lat_var, kLats.data()));
  read_lats.assign(3, -999.0);
  ASSERT_EQ(NC_NOERR, nc_get_var_double(root.get(), lat_var, read_lats.data()));
  EXPECT_THAT(read_lats, ElementsAre(-45.0, 0.0, 45.0));
}

}  // namespace
}  // namespace netcdf
