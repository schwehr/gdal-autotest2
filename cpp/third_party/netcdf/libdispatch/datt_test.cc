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

// Unit tests for //third_party/netcdf/libdispatch/datt.c.
//
// Covers nc_rename_att() and nc_del_att() dispatch validation, define/data mode
// rules across classic and netCDF-4 formats, subgroup attributes, in-memory
// persistence round-trips, and error handling.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/datt.c

#include <array>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

namespace netcdf {
namespace {

TEST(DattTest, RejectsInvalidAndClosedNcid) {
  EXPECT_EQ(NC_EBADID, nc_rename_att(-1, NC_GLOBAL, "old", "new"));
  EXPECT_EQ(NC_EBADID, nc_rename_att(999999, 0, "old", "new"));
  EXPECT_EQ(NC_EBADID, nc_del_att(-1, NC_GLOBAL, "old"));
  EXPECT_EQ(NC_EBADID, nc_del_att(999999, 0, "old"));

  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR, nc_create("datt_closed_id.nc", NC_DISKLESS, nc.ptr()));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(nc.get(), NC_GLOBAL, "a", 3, "xyz"));
  const int closed_ncid = nc.get();
  ASSERT_EQ(NC_NOERR, nc.close());

  EXPECT_EQ(NC_EBADID, nc_rename_att(closed_ncid, NC_GLOBAL, "a", "b"));
  EXPECT_EQ(NC_EBADID, nc_del_att(closed_ncid, NC_GLOBAL, "a"));
}

TEST(DattTest, RenamesAndDeletesAttributesAcrossClassicAndNetcdf4Formats) {
  struct FormatCase {
    const char* label;
    int cmode;
    bool strict_classic_rules;
  };
  constexpr FormatCase kCases[] = {
      {"classic", NC_DISKLESS, true},
      {"offset64", NC_64BIT_OFFSET | NC_DISKLESS, true},
      {"cdf5", NC_CDF5 | NC_DISKLESS, true},
      {"nc4_classic", NC_NETCDF4 | NC_CLASSIC_MODEL | NC_DISKLESS, true},
      {"netcdf4", NC_NETCDF4 | NC_DISKLESS, false},
  };

  for (const FormatCase& tc : kCases) {
    SCOPED_TRACE(tc.label);
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create("datt_modes.nc", tc.cmode, nc.ptr()));

    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "v", NC_INT, 1, &dimid, &varid));

    constexpr char kTitle[] = "initial_title";
    constexpr int kExtraVal = 42;
    constexpr double kScale = 1.5;
    constexpr char kNote[] = "abc";

    ASSERT_EQ(NC_NOERR, nc_put_att_text(nc.get(), NC_GLOBAL, "title",
                                        sizeof(kTitle) - 1, kTitle));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), NC_GLOBAL, "drop_me", NC_INT,
                                       1, &kExtraVal));
    ASSERT_EQ(NC_NOERR, nc_put_att_double(nc.get(), varid, "scale_factor",
                                          NC_DOUBLE, 1, &kScale));
    ASSERT_EQ(NC_NOERR, nc_put_att_text(nc.get(), varid, "var_note",
                                        sizeof(kNote) - 1, kNote));

    // In define mode, renaming to a longer name and deleting both succeed.
    ASSERT_EQ(NC_NOERR, nc_rename_att(nc.get(), NC_GLOBAL, "title",
                                      "dataset_long_title"));
    ASSERT_EQ(NC_NOERR, nc_del_att(nc.get(), NC_GLOBAL, "drop_me"));

    EXPECT_EQ(NC_ENOTATT,
              nc_inq_att(nc.get(), NC_GLOBAL, "title", nullptr, nullptr));
    EXPECT_EQ(NC_ENOTATT,
              nc_inq_att(nc.get(), NC_GLOBAL, "drop_me", nullptr, nullptr));

    size_t len = 0;
    ASSERT_EQ(NC_NOERR,
              nc_inq_attlen(nc.get(), NC_GLOBAL, "dataset_long_title", &len));
    ASSERT_EQ(sizeof(kTitle) - 1, len);
    std::string got_title(len, '\0');
    ASSERT_EQ(NC_NOERR,
              nc_get_att_text(nc.get(), NC_GLOBAL, "dataset_long_title",
                              got_title.data()));
    EXPECT_EQ(kTitle, got_title);

    // Swap two variable attributes via a temporary name while in define mode.
    constexpr int kFirstVal = 11;
    constexpr int kSecondVal = 22;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc.get(), varid, "first", NC_INT, 1, &kFirstVal));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), varid, "second", NC_INT, 1,
                                       &kSecondVal));
    ASSERT_EQ(NC_NOERR, nc_rename_att(nc.get(), varid, "first", "_tmp"));
    ASSERT_EQ(NC_NOERR, nc_rename_att(nc.get(), varid, "second", "first"));
    ASSERT_EQ(NC_NOERR, nc_rename_att(nc.get(), varid, "_tmp", "second"));

    int got_first = 0;
    int got_second = 0;
    ASSERT_EQ(NC_NOERR, nc_get_att_int(nc.get(), varid, "first", &got_first));
    ASSERT_EQ(NC_NOERR, nc_get_att_int(nc.get(), varid, "second", &got_second));
    EXPECT_EQ(kSecondVal, got_first);
    EXPECT_EQ(kFirstVal, got_second);

    // Leave define mode and test data-mode rename/delete behavior.
    ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));

    // Shorter rename in data mode is permitted in every format.
    ASSERT_EQ(NC_NOERR,
              nc_rename_att(nc.get(), varid, "scale_factor", "scale"));
    double got_scale = 0.0;
    ASSERT_EQ(NC_NOERR,
              nc_get_att_double(nc.get(), varid, "scale", &got_scale));
    EXPECT_DOUBLE_EQ(kScale, got_scale);

    // Expanding rename or deleting in data mode depends on strict classic
    // rules.
    const int grow_stat =
        nc_rename_att(nc.get(), varid, "scale", "scale_factor_expanded");
    const int del_stat = nc_del_att(nc.get(), varid, "var_note");
    if (tc.strict_classic_rules) {
      EXPECT_EQ(NC_ENOTINDEFINE, grow_stat);
      EXPECT_EQ(NC_ENOTINDEFINE, del_stat);
      ASSERT_EQ(NC_NOERR, nc_redef(nc.get()));
      ASSERT_EQ(NC_NOERR, nc_rename_att(nc.get(), varid, "scale",
                                        "scale_factor_expanded"));
      ASSERT_EQ(NC_NOERR, nc_del_att(nc.get(), varid, "var_note"));
      ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));
    } else {
      EXPECT_EQ(NC_NOERR, grow_stat);
      EXPECT_EQ(NC_NOERR, del_stat);
    }

    EXPECT_EQ(NC_ENOTATT,
              nc_inq_att(nc.get(), varid, "var_note", nullptr, nullptr));
    EXPECT_EQ(NC_NOERR, nc_inq_att(nc.get(), varid, "scale_factor_expanded",
                                   nullptr, nullptr));
  }
}

TEST(DattTest, RenamesAndDeletesInNetcdf4SubgroupsAndPersistsViaMemio) {
  ScopedMemio memio;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("datt_nc4_mem.nc", NC_NETCDF4, 4096, nc.ptr()));

    int grp_id = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc.get(), "subgrp", &grp_id));

    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(grp_id, "dim0", 3, &dimid));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(grp_id, "signal", NC_INT, 1, &dimid, &varid));

    constexpr int kG0 = 100;
    constexpr int kG1 = 200;
    constexpr int kG2 = 300;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(grp_id, NC_GLOBAL, "g0", NC_INT, 1, &kG0));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(grp_id, NC_GLOBAL, "g1", NC_INT, 1, &kG1));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(grp_id, NC_GLOBAL, "g2", NC_INT, 1, &kG2));

    constexpr float kA0 = 1.0F;
    constexpr float kA1 = 2.5F;
    constexpr float kA2 = 3.75F;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_float(grp_id, varid, "a0", NC_FLOAT, 1, &kA0));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_float(grp_id, varid, "a1", NC_FLOAT, 1, &kA1));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_float(grp_id, varid, "a2", NC_FLOAT, 1, &kA2));

    // Flush to HDF5 first so subsequent rename/delete exercise created
    // attributes.
    ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));

    ASSERT_EQ(NC_NOERR, nc_del_att(grp_id, NC_GLOBAL, "g0"));
    ASSERT_EQ(NC_NOERR,
              nc_rename_att(grp_id, NC_GLOBAL, "g1", "g1_renamed_longer"));

    ASSERT_EQ(NC_NOERR, nc_del_att(grp_id, varid, "a0"));
    ASSERT_EQ(NC_NOERR,
              nc_rename_att(grp_id, varid, "a1", "a1_renamed_longer"));

    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }

  ScopedNcFile reopened;
  ASSERT_EQ(NC_NOERR, nc_open_mem("datt_nc4_mem.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, reopened.ptr()));

  int grp_id = -1;
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_grp_ncid(reopened.get(), "subgrp", &grp_id));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(grp_id, "signal", &varid));

  int g_natts = -1;
  int v_natts = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_natts(grp_id, &g_natts));
  ASSERT_EQ(NC_NOERR, nc_inq_varnatts(grp_id, varid, &v_natts));
  EXPECT_EQ(2, g_natts);
  EXPECT_EQ(2, v_natts);

  EXPECT_EQ(NC_ENOTATT, nc_inq_att(grp_id, NC_GLOBAL, "g0", nullptr, nullptr));
  EXPECT_EQ(NC_ENOTATT, nc_inq_att(grp_id, NC_GLOBAL, "g1", nullptr, nullptr));
  EXPECT_EQ(NC_ENOTATT, nc_inq_att(grp_id, varid, "a0", nullptr, nullptr));
  EXPECT_EQ(NC_ENOTATT, nc_inq_att(grp_id, varid, "a1", nullptr, nullptr));

  int got_g1 = 0;
  int got_g2 = 0;
  float got_a1 = 0.0F;
  float got_a2 = 0.0F;
  ASSERT_EQ(NC_NOERR,
            nc_get_att_int(grp_id, NC_GLOBAL, "g1_renamed_longer", &got_g1));
  ASSERT_EQ(NC_NOERR, nc_get_att_int(grp_id, NC_GLOBAL, "g2", &got_g2));
  ASSERT_EQ(NC_NOERR,
            nc_get_att_float(grp_id, varid, "a1_renamed_longer", &got_a1));
  ASSERT_EQ(NC_NOERR, nc_get_att_float(grp_id, varid, "a2", &got_a2));
  EXPECT_EQ(200, got_g1);
  EXPECT_EQ(300, got_g2);
  EXPECT_FLOAT_EQ(2.5F, got_a1);
  EXPECT_FLOAT_EQ(3.75F, got_a2);

  std::vector<std::string> var_att_names;
  for (int i = 0; i < v_natts; ++i) {
    char name[NC_MAX_NAME + 1]{};
    ASSERT_EQ(NC_NOERR, nc_inq_attname(grp_id, varid, i, name));
    var_att_names.emplace_back(name);
  }
  // NetCDF-4 deletes and recreates flushed HDF5 attributes on rename, so
  // reopening in H5_INDEX_CRT_ORDER places untouched "a2" before the renamed
  // attribute.
  EXPECT_THAT(var_att_names, ::testing::ElementsAre("a2", "a1_renamed_longer"));

  // Read-only netCDF-4 files reject both rename and delete with NC_EPERM.
  EXPECT_EQ(NC_EPERM, nc_rename_att(grp_id, NC_GLOBAL, "g2", "g2_new"));
  EXPECT_EQ(NC_EPERM, nc_del_att(grp_id, NC_GLOBAL, "g2"));
  EXPECT_EQ(NC_EPERM, nc_rename_att(grp_id, varid, "a2", "a2_new"));
  EXPECT_EQ(NC_EPERM, nc_del_att(grp_id, varid, "a2"));
}

TEST(DattTest, ClassicPersistsRenamedAndDeletedAttributesAndRejectsReadOnly) {
  ScopedMemio memio;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("datt_classic_mem.nc", NC_CLOBBER, 4096, nc.ptr()));

    constexpr std::array<int, 2> kKeepVals = {7, 8};
    constexpr int kDropVal = 99;
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), NC_GLOBAL, "keep_orig", NC_INT,
                                       kKeepVals.size(), kKeepVals.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), NC_GLOBAL, "drop_orig", NC_INT,
                                       1, &kDropVal));
    ASSERT_EQ(NC_NOERR,
              nc_rename_att(nc.get(), NC_GLOBAL, "keep_orig", "keep_final"));
    ASSERT_EQ(NC_NOERR, nc_del_att(nc.get(), NC_GLOBAL, "drop_orig"));

    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }

  ScopedNcFile reopened;
  ASSERT_EQ(NC_NOERR,
            nc_open_mem("datt_classic_mem.nc", NC_NOWRITE, memio.size(),
                        memio.get()->memory, reopened.ptr()));

  int natts = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_natts(reopened.get(), &natts));
  EXPECT_EQ(1, natts);
  EXPECT_EQ(NC_ENOTATT, nc_inq_att(reopened.get(), NC_GLOBAL, "keep_orig",
                                   nullptr, nullptr));
  EXPECT_EQ(NC_ENOTATT, nc_inq_att(reopened.get(), NC_GLOBAL, "drop_orig",
                                   nullptr, nullptr));

  std::vector<int> got(2, -1);
  ASSERT_EQ(NC_NOERR, nc_get_att_int(reopened.get(), NC_GLOBAL, "keep_final",
                                     got.data()));
  EXPECT_THAT(got, ::testing::ElementsAre(7, 8));

  EXPECT_EQ(NC_EPERM,
            nc_rename_att(reopened.get(), NC_GLOBAL, "keep_final", "kf"));
  EXPECT_EQ(NC_ENOTINDEFINE,
            nc_del_att(reopened.get(), NC_GLOBAL, "keep_final"));
}

TEST(DattTest, RejectsInvalidNamesVariablesMissingAttributesAndCollisions) {
  for (const int cmode : {NC_DISKLESS, NC_NETCDF4 | NC_DISKLESS}) {
    SCOPED_TRACE(cmode);
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create("datt_errors.nc", cmode, nc.ptr()));

    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "v", NC_INT, 1, &dimid, &varid));

    constexpr int kVal = 1;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc.get(), NC_GLOBAL, "g_a", NC_INT, 1, &kVal));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc.get(), NC_GLOBAL, "g_b", NC_INT, 1, &kVal));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc.get(), varid, "v_a", NC_INT, 1, &kVal));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc.get(), varid, "v_b", NC_INT, 1, &kVal));

    // Missing attribute name.
    EXPECT_EQ(NC_ENOTATT,
              nc_rename_att(nc.get(), NC_GLOBAL, "missing", "new_missing"));
    EXPECT_EQ(NC_ENOTATT, nc_rename_att(nc.get(), varid, "missing", "new_m"));
    EXPECT_EQ(NC_ENOTATT, nc_del_att(nc.get(), NC_GLOBAL, "missing"));
    EXPECT_EQ(NC_ENOTATT, nc_del_att(nc.get(), varid, "missing"));

    // Invalid variable ID.
    EXPECT_EQ(NC_ENOTVAR, nc_rename_att(nc.get(), 99, "v_a", "v_c"));
    EXPECT_EQ(NC_ENOTVAR, nc_rename_att(nc.get(), -2, "v_a", "v_c"));
    EXPECT_EQ(NC_ENOTVAR, nc_del_att(nc.get(), 99, "v_a"));
    EXPECT_EQ(NC_ENOTVAR, nc_del_att(nc.get(), -2, "v_a"));

    // Collision with an existing attribute name.
    EXPECT_EQ(NC_ENAMEINUSE, nc_rename_att(nc.get(), NC_GLOBAL, "g_a", "g_b"));
    EXPECT_EQ(NC_ENAMEINUSE, nc_rename_att(nc.get(), varid, "v_a", "v_b"));

    // Illegal newname values and overlong newname.
    EXPECT_EQ(NC_EBADNAME, nc_rename_att(nc.get(), NC_GLOBAL, "g_a", ""));
    EXPECT_EQ(NC_EBADNAME,
              nc_rename_att(nc.get(), NC_GLOBAL, "g_a", "bad/name"));
    EXPECT_EQ(NC_EBADNAME, nc_rename_att(nc.get(), NC_GLOBAL, "g_a", " lead"));
    EXPECT_EQ(NC_EBADNAME, nc_rename_att(nc.get(), NC_GLOBAL, "g_a", "trail "));

    const std::string overlong(NC_MAX_NAME + 1, 'z');
    EXPECT_EQ(NC_EMAXNAME,
              nc_rename_att(nc.get(), NC_GLOBAL, "g_a", overlong.c_str()));

    if (cmode & NC_NETCDF4) {
      EXPECT_EQ(NC_EINVAL, nc_rename_att(nc.get(), NC_GLOBAL, nullptr, "x"));
      EXPECT_EQ(NC_EINVAL, nc_rename_att(nc.get(), NC_GLOBAL, "g_a", nullptr));
      EXPECT_EQ(NC_EINVAL, nc_del_att(nc.get(), NC_GLOBAL, nullptr));
    }
  }
}

}  // namespace
}  // namespace netcdf
