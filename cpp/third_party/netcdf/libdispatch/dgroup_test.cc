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

// Unit tests for //third_party/netcdf/libdispatch/dgroup.c.
//
// Covers all dispatch wrappers for group creation, renaming, hierarchy
// traversal, variable/dimension/type listing, nc_show_metadata(), and format
// / bad-ncid error paths.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dgroup.c

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

namespace netcdf {
namespace {

TEST(DgroupTest, InvalidNcidReturnsBadIdAcrossAllGroupApis) {
  // Include a genuinely closed handle alongside invalid sentinel IDs.
  int closed_ncid = -1;
  {
    ScopedNcFile tmp;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("closed_grp.nc", NC_NETCDF4, 4096, tmp.ptr()));
    closed_ncid = tmp.get();
    ScopedMemio memio;
    ASSERT_EQ(NC_NOERR, nc_close_memio(tmp.release(), memio.get()));
  }

  const std::array<int, 3> bad_ids = {-1, 999999, closed_ncid};
  for (int bad_ncid : bad_ids) {
    SCOPED_TRACE(bad_ncid);
    int out_id = -1;
    int count = 0;
    size_t len = 0;
    char name_buf[NC_MAX_NAME + 1]{};

    EXPECT_EQ(NC_EBADID, nc_inq_ncid(bad_ncid, "grp", &out_id));
    EXPECT_EQ(NC_EBADID, nc_inq_grps(bad_ncid, &count, &out_id));
    EXPECT_EQ(NC_EBADID, nc_inq_grpname(bad_ncid, name_buf));
    EXPECT_EQ(NC_EBADID, nc_inq_grpname_full(bad_ncid, &len, name_buf));
    EXPECT_EQ(NC_EBADID, nc_inq_grpname_len(bad_ncid, &len));
    EXPECT_EQ(NC_EBADID, nc_inq_grp_parent(bad_ncid, &out_id));
    EXPECT_EQ(NC_EBADID, nc_inq_grp_ncid(bad_ncid, "grp", &out_id));
    EXPECT_EQ(NC_EBADID, nc_inq_grp_full_ncid(bad_ncid, "/grp", &out_id));
    EXPECT_EQ(NC_EBADID, nc_inq_varids(bad_ncid, &count, &out_id));
    EXPECT_EQ(NC_EBADID, nc_inq_dimids(bad_ncid, &count, &out_id, 0));
    EXPECT_EQ(NC_EBADID, nc_inq_typeids(bad_ncid, &count, &out_id));
    EXPECT_EQ(NC_EBADID, nc_def_grp(bad_ncid, "sub", &out_id));
    EXPECT_EQ(NC_EBADID, nc_rename_grp(bad_ncid, "renamed"));
    EXPECT_EQ(NC_EBADID, nc_show_metadata(bad_ncid));
  }
}

TEST(DgroupTest, ClassicFormatsExposeRootGroupAndRejectGroupMutations) {
  struct ClassicCase {
    const char* name;
    int cmode;
  };
  constexpr ClassicCase kFormats[] = {
      {"classic", NC_CLOBBER},
      {"offset64", NC_CLOBBER | NC_64BIT_OFFSET},
      {"cdf5", NC_CLOBBER | NC_CDF5},
  };

  for (const ClassicCase& tc : kFormats) {
    SCOPED_TRACE(tc.name);
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create_mem(tc.name, tc.cmode, 2048, nc.ptr()));

    int d0 = -1;
    int d1 = -1;
    int v0 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 3, &d0));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "y", 2, &d1));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "a", NC_INT, 1, &d0, &v0));

    int sub_id = -1;
    EXPECT_EQ(NC_ENOTNC4, nc_def_grp(nc, "child", &sub_id));
    EXPECT_EQ(NC_ENOTNC4, nc_rename_grp(nc, "renamed"));

    int num_grps = -1;
    EXPECT_EQ(NC_NOERR, nc_inq_grps(nc, &num_grps, nullptr));
    EXPECT_EQ(0, num_grps);

    char name[NC_MAX_NAME + 1]{};
    EXPECT_EQ(NC_NOERR, nc_inq_grpname(nc, name));
    EXPECT_STREQ("/", name);

    size_t full_len = 0;
    char full_name[NC_MAX_NAME + 1]{};
    EXPECT_EQ(NC_NOERR, nc_inq_grpname_len(nc, &full_len));
    EXPECT_EQ(size_t{1}, full_len);
    full_len = 0;
    EXPECT_EQ(NC_NOERR, nc_inq_grpname_full(nc, &full_len, full_name));
    EXPECT_EQ(size_t{1}, full_len);
    EXPECT_STREQ("/", full_name);

    int queried_id = -1;
    EXPECT_EQ(NC_NOERR, nc_inq_ncid(nc, "/", &queried_id));
    EXPECT_EQ(nc.get(), queried_id);
    queried_id = -1;
    EXPECT_EQ(NC_NOERR, nc_inq_grp_ncid(nc, nullptr, &queried_id));
    EXPECT_EQ(nc.get(), queried_id);

    int parent_id = -1;
    EXPECT_EQ(NC_ENOGRP, nc_inq_grp_parent(nc, &parent_id));
    EXPECT_EQ(NC_ENOGRP, nc_inq_grp_full_ncid(nc, "/", &queried_id));

    int ndims = 0;
    std::array<int, 2> dimids{-1, -1};
    EXPECT_EQ(NC_NOERR, nc_inq_dimids(nc, &ndims, dimids.data(), 1));
    EXPECT_EQ(2, ndims);
    EXPECT_THAT(dimids, ::testing::ElementsAre(d0, d1));

    int nvars = 0;
    int varid = -1;
    EXPECT_EQ(NC_NOERR, nc_inq_varids(nc, &nvars, &varid));
    EXPECT_EQ(1, nvars);
    EXPECT_EQ(v0, varid);

    int ntypes = -1;
    EXPECT_EQ(NC_NOERR, nc_inq_typeids(nc, &ntypes, nullptr));
    EXPECT_EQ(0, ntypes);

    EXPECT_EQ(NC_NOERR, nc_show_metadata(nc));

    ScopedMemio memio;
    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }
}

TEST(DgroupTest, Netcdf4ClassicModelDisallowsSubgroups) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("nc4_classic_grp.nc", NC_NETCDF4 | NC_CLASSIC_MODEL,
                          4096, nc.ptr()));
  int child_id = -1;
  EXPECT_EQ(NC_ESTRICTNC3, nc_def_grp(nc, "child", &child_id));
  EXPECT_EQ(NC_EBADGRPID, nc_rename_grp(nc, "root_renamed"));

  int num_grps = -1;
  EXPECT_EQ(NC_NOERR, nc_inq_grps(nc, &num_grps, nullptr));
  EXPECT_EQ(0, num_grps);

  char name[NC_MAX_NAME + 1]{};
  EXPECT_EQ(NC_NOERR, nc_inq_grpname(nc, name));
  EXPECT_STREQ("/", name);
  EXPECT_EQ(NC_NOERR, nc_show_metadata(nc));

  ScopedMemio memio;
  ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
}

TEST(DgroupTest, Netcdf4NestedHierarchyTraversalDimensionsVarsTypesAndRename) {
  ScopedMemio memio;
  {
    ScopedNcFile root;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("hierarchy.nc", NC_NETCDF4, 8192, root.ptr()));

    int region_a = -1;
    int region_b = -1;
    int sub_1 = -1;
    int sub_2 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(root, "region_a", &region_a));
    ASSERT_EQ(NC_NOERR, nc_def_grp(root, "region_b", &region_b));
    ASSERT_EQ(NC_NOERR, nc_def_grp(region_a, "sub_1", &sub_1));
    ASSERT_EQ(NC_NOERR, nc_def_grp(region_a, "sub_2", &sub_2));

    // Duplicate group name in same parent fails.
    int dup_id = -1;
    EXPECT_EQ(NC_ENAMEINUSE, nc_def_grp(root, "region_a", &dup_id));

    // Root dimension is visible in child groups only with include_parents != 0.
    int root_dim = -1;
    int sub_dim = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(root, "time", 2, &root_dim));
    ASSERT_EQ(NC_NOERR, nc_def_dim(sub_1, "site", 5, &sub_dim));

    // Variables in root and nested subgroup.
    int root_var = -1;
    int sub_var0 = -1;
    int sub_var1 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(root, "global_temp", NC_FLOAT, 1, &root_dim,
                                   &root_var));
    const std::array<int, 2> sub_dims = {root_dim, sub_dim};
    ASSERT_EQ(NC_NOERR, nc_def_var(sub_1, "local_temp", NC_DOUBLE, 2,
                                   sub_dims.data(), &sub_var0));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(sub_1, "local_count", NC_INT, 1, &sub_dim, &sub_var1));

    // User-defined types in root and subgroup.
    nc_type root_enum = -1;
    nc_type sub_opaque = -1;
    ASSERT_EQ(NC_NOERR, nc_def_enum(root, NC_INT, "status_t", &root_enum));
    int enum_val = 1;
    ASSERT_EQ(NC_NOERR, nc_insert_enum(root, root_enum, "OK", &enum_val));
    ASSERT_EQ(NC_NOERR, nc_def_opaque(sub_1, 4, "tag_t", &sub_opaque));

    // Group name & full path inquiries.
    char grp_name[NC_MAX_NAME + 1]{};
    EXPECT_EQ(NC_NOERR, nc_inq_grpname(sub_1, grp_name));
    EXPECT_STREQ("sub_1", grp_name);

    size_t len = 0;
    EXPECT_EQ(NC_NOERR, nc_inq_grpname_len(sub_1, &len));
    std::string expected_full = "/region_a/sub_1";
    EXPECT_EQ(expected_full.size(), len);
    std::vector<char> full_buf(len + 1, '\0');
    size_t len2 = 0;
    EXPECT_EQ(NC_NOERR, nc_inq_grpname_full(sub_1, &len2, full_buf.data()));
    EXPECT_EQ(expected_full, full_buf.data());
    EXPECT_EQ(len, len2);

    // Parent links.
    int parent_id = -1;
    EXPECT_EQ(NC_NOERR, nc_inq_grp_parent(sub_1, &parent_id));
    EXPECT_EQ(region_a, parent_id);
    EXPECT_EQ(NC_NOERR, nc_inq_grp_parent(region_a, &parent_id));
    EXPECT_EQ(root.get(), parent_id);
    EXPECT_EQ(NC_ENOGRP, nc_inq_grp_parent(root, &parent_id));

    // Relative and full ncid lookups.
    int found = -1;
    EXPECT_EQ(NC_NOERR, nc_inq_ncid(sub_1, nullptr, &found));
    EXPECT_EQ(root.get(), found);
    EXPECT_EQ(NC_NOERR, nc_inq_ncid(root, "region_a", &found));
    EXPECT_EQ(region_a, found);
    EXPECT_EQ(NC_NOERR, nc_inq_grp_ncid(region_a, "sub_2", &found));
    EXPECT_EQ(sub_2, found);
    EXPECT_EQ(NC_ENOGRP, nc_inq_grp_ncid(root, "missing_grp", &found));

    EXPECT_EQ(NC_NOERR, nc_inq_grp_full_ncid(root, "/", &found));
    EXPECT_EQ(root.get(), found);
    EXPECT_EQ(NC_NOERR, nc_inq_grp_full_ncid(root, "/region_a/sub_1", &found));
    EXPECT_EQ(sub_1, found);
    EXPECT_EQ(NC_NOERR, nc_inq_grp_full_ncid(region_a, "sub_2", &found));
    EXPECT_EQ(sub_2, found);
    EXPECT_EQ(NC_ENOGRP, nc_inq_grp_full_ncid(sub_1, "/", &found));
    EXPECT_EQ(NC_ENOGRP, nc_inq_grp_full_ncid(root, "/region_a/nope", &found));
    EXPECT_EQ(NC_EINVAL, nc_inq_grp_full_ncid(root, nullptr, &found));

    // Child group listing (count-only and full list, plus numgrps == nullptr).
    int num_root_grps = 0;
    EXPECT_EQ(NC_NOERR, nc_inq_grps(root, &num_root_grps, nullptr));
    EXPECT_EQ(2, num_root_grps);
    std::array<int, 2> root_children{};
    EXPECT_EQ(NC_NOERR, nc_inq_grps(root, nullptr, root_children.data()));
    EXPECT_THAT(root_children,
                ::testing::UnorderedElementsAre(region_a, region_b));

    // Variable IDs per group.
    int nvars = 0;
    EXPECT_EQ(NC_NOERR, nc_inq_varids(sub_1, &nvars, nullptr));
    EXPECT_EQ(2, nvars);
    std::array<int, 2> varids{};
    EXPECT_EQ(NC_NOERR, nc_inq_varids(sub_1, &nvars, varids.data()));
    EXPECT_THAT(varids, ::testing::ElementsAre(sub_var0, sub_var1));

    // Dimension IDs with include_parents = 0 vs include_parents = 1.
    int ndims_local = 0;
    int local_dim_id = -1;
    EXPECT_EQ(NC_NOERR, nc_inq_dimids(sub_1, &ndims_local, &local_dim_id, 0));
    EXPECT_EQ(1, ndims_local);
    EXPECT_EQ(sub_dim, local_dim_id);

    int ndims_all = 0;
    std::array<int, 2> all_dimids{};
    EXPECT_EQ(NC_NOERR, nc_inq_dimids(sub_1, &ndims_all, all_dimids.data(), 1));
    EXPECT_EQ(2, ndims_all);
    EXPECT_THAT(all_dimids, ::testing::UnorderedElementsAre(root_dim, sub_dim));

    // User type IDs per group.
    int ntypes = 0;
    nc_type queried_type = -1;
    EXPECT_EQ(NC_NOERR, nc_inq_typeids(root, &ntypes, &queried_type));
    EXPECT_EQ(1, ntypes);
    EXPECT_EQ(root_enum, queried_type);
    EXPECT_EQ(NC_NOERR, nc_inq_typeids(sub_1, &ntypes, &queried_type));
    EXPECT_EQ(1, ntypes);
    EXPECT_EQ(sub_opaque, queried_type);
    EXPECT_EQ(NC_NOERR, nc_inq_typeids(region_b, &ntypes, nullptr));
    EXPECT_EQ(0, ntypes);

    // Rename subgroup before close, and verify failure modes.
    EXPECT_EQ(NC_EBADGRPID, nc_rename_grp(root, "bad_root_rename"));
    EXPECT_EQ(NC_ENAMEINUSE, nc_rename_grp(sub_2, "sub_1"));
    EXPECT_EQ(NC_EBADNAME, nc_rename_grp(sub_2, ""));
    EXPECT_EQ(NC_EINVAL, nc_rename_grp(sub_2, nullptr));
    ASSERT_EQ(NC_NOERR, nc_rename_grp(sub_2, "sub_renamed"));
    EXPECT_EQ(NC_NOERR,
              nc_inq_grp_full_ncid(root, "/region_a/sub_renamed", &found));
    EXPECT_EQ(sub_2, found);

    EXPECT_EQ(NC_NOERR, nc_show_metadata(root));
    EXPECT_EQ(NC_NOERR, nc_show_metadata(sub_1));

    ASSERT_EQ(NC_NOERR, nc_close_memio(root.release(), memio.get()));
  }

  // Reopen read-only in memory to verify persisted hierarchy & EPERM on writes.
  ScopedNcFile ro_root;
  ASSERT_EQ(NC_NOERR, nc_open_mem("hierarchy.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, ro_root.ptr()));
  int ro_sub = -1;
  ASSERT_EQ(NC_NOERR,
            nc_inq_grp_full_ncid(ro_root, "/region_a/sub_renamed", &ro_sub));
  char ro_name[NC_MAX_NAME + 1]{};
  EXPECT_EQ(NC_NOERR, nc_inq_grpname(ro_sub, ro_name));
  EXPECT_STREQ("sub_renamed", ro_name);
  EXPECT_EQ(NC_EPERM, nc_rename_grp(ro_sub, "should_fail"));
  EXPECT_EQ(NC_EPERM, nc_def_grp(ro_root, "ro_new", &ro_sub));
}

TEST(DgroupTest, RenamePersistedGroupOnDiskFileSucceeds) {
  TempFile disk_nc("dgroup_rename.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create(disk_nc.c_str(), NC_CLOBBER | NC_NETCDF4, nc.ptr()));
    int gid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc, "before", &gid));
    ASSERT_EQ(NC_NOERR, nc.close());
  }
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_open(disk_nc.c_str(), NC_WRITE, nc.ptr()));
    int gid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_grp_ncid(nc, "before", &gid));
    ASSERT_EQ(NC_NOERR, nc_rename_grp(gid, "after"));
    ASSERT_EQ(NC_NOERR, nc.close());
  }
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_open(disk_nc.c_str(), NC_NOWRITE, nc.ptr()));
    int gid = -1;
    EXPECT_EQ(NC_ENOGRP, nc_inq_grp_ncid(nc, "before", &gid));
    EXPECT_EQ(NC_NOERR, nc_inq_grp_ncid(nc, "after", &gid));
    char name[NC_MAX_NAME + 1]{};
    EXPECT_EQ(NC_NOERR, nc_inq_grpname(gid, name));
    EXPECT_STREQ("after", name);
  }
}

}  // namespace
}  // namespace netcdf
