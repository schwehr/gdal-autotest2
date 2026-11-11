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

// Unit tests for //third_party/netcdf/libsrc4/nc4grp.c.
//
// Exercises group creation/lookup, hierarchy naming and parent traversal,
// full-path lookup, per-group variable ID enumeration, parent-aware sorted
// dimension ID enumeration, UTF-8 NFC/NFD normalization, optional NULL output
// pointers, and error handling via Tier 1 in-memory NetCDF-4 datasets.

#include <cstddef>
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

using ::testing::ElementsAre;
using ::testing::UnorderedElementsAre;

// Constructs an invalid group sub-ID on an otherwise open NetCDF-4 file handle
// so dispatch passes NC_check_id() and exercises nc4_find_grp_h5() in nc4grp.c.
int MakeInvalidGroupSubId(int root_ncid) {
  return (root_ncid & 0x7fff0000) | 0x7fff;
}

TEST(Nc4GrpTest, HierarchyNavigationAndFullPathRoundTrip) {
  ScopedMemio memio;
  int alpha_id = -1;
  int beta_id = -1;
  int gamma_id = -1;
  int delta_id = -1;
  int utf8_id = -1;

  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("nc4grp_hierarchy.nc", NC_NETCDF4, 4096, nc.ptr()));

    ASSERT_EQ(NC_NOERR, nc_def_grp(nc.get(), "alpha", &alpha_id));
    ASSERT_EQ(NC_NOERR, nc_def_grp(alpha_id, "beta", &beta_id));
    ASSERT_EQ(NC_NOERR, nc_def_grp(beta_id, "gamma", &gamma_id));
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc.get(), "delta", &delta_id));
    // UTF-8 NFC "cafe\u00e9" (cafe + acute e).
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc.get(), "caf\xC3\xA9", &utf8_id));

    // In-memory creation order before serialization preserves definition order.
    int created_num_grps = -1;
    std::vector<int> created_grps(3, -1);
    ASSERT_EQ(NC_NOERR,
              nc_inq_grps(nc.get(), &created_num_grps, created_grps.data()));
    EXPECT_EQ(3, created_num_grps);
    EXPECT_THAT(created_grps, ElementsAre(alpha_id, delta_id, utf8_id));

    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }

  ScopedNcFile reader;
  ASSERT_EQ(NC_NOERR,
            nc_open_mem("nc4grp_hierarchy.nc", NC_NOWRITE, memio.size(),
                        memio.get()->memory, reader.ptr()));
  const int root_id = reader.get();

  // Direct child lookup via both nc_inq_ncid and nc_inq_grp_ncid.
  int looked_up_alpha = -1;
  int looked_up_delta = -1;
  int looked_up_utf8 = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_ncid(root_id, "alpha", &looked_up_alpha));
  ASSERT_EQ(NC_NOERR, nc_inq_grp_ncid(root_id, "delta", &looked_up_delta));
  // NFD query ("cafe" + U+0301 combining acute) must normalize to NFC.
  ASSERT_EQ(NC_NOERR, nc_inq_ncid(root_id, "cafe\xCC\x81", &looked_up_utf8));
  EXPECT_EQ(NC_NOERR, nc_inq_ncid(root_id, "alpha", nullptr));

  int looked_up_beta = -1;
  int looked_up_gamma = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_ncid(looked_up_alpha, "beta", &looked_up_beta));
  ASSERT_EQ(NC_NOERR, nc_inq_ncid(looked_up_beta, "gamma", &looked_up_gamma));

  // NULL name short-circuits to the root group ncid, even from deep subgroups.
  int short_circuit_root = -1;
  ASSERT_EQ(NC_NOERR,
            nc_inq_ncid(looked_up_gamma, nullptr, &short_circuit_root));
  EXPECT_EQ(root_id, short_circuit_root);
  EXPECT_EQ(NC_NOERR, nc_inq_ncid(looked_up_gamma, nullptr, nullptr));

  // Child group enumeration (count-only, IDs-only, and both).
  int num_root_grps = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_grps(root_id, &num_root_grps, nullptr));
  ASSERT_EQ(3, num_root_grps);

  std::vector<int> root_grps(3, -1);
  EXPECT_EQ(NC_NOERR, nc_inq_grps(root_id, nullptr, root_grps.data()));
  // nc_create_mem() opens the HDF5 image root with H5P_DEFAULT (without link
  // creation-order tracking), so rec_read_metadata() iterates root children in
  // H5_INDEX_NAME order ("alpha", "caf\xC3\xA9", "delta") rather than creation
  // order.
  EXPECT_THAT(root_grps, UnorderedElementsAre(looked_up_alpha, looked_up_delta,
                                              looked_up_utf8));

  int num_alpha_grps = -1;
  int alpha_child = -1;
  ASSERT_EQ(NC_NOERR,
            nc_inq_grps(looked_up_alpha, &num_alpha_grps, &alpha_child));
  EXPECT_EQ(1, num_alpha_grps);
  EXPECT_EQ(looked_up_beta, alpha_child);
  EXPECT_EQ(NC_NOERR, nc_inq_grps(looked_up_alpha, nullptr, nullptr));

  int num_gamma_grps = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_grps(looked_up_gamma, &num_gamma_grps, nullptr));
  EXPECT_EQ(0, num_gamma_grps);

  // Empty dimension/variable enumeration checks on leaf group delta.
  int empty_nvars = -1;
  EXPECT_EQ(NC_NOERR, nc_inq_varids(looked_up_delta, &empty_nvars, nullptr));
  EXPECT_EQ(0, empty_nvars);
  EXPECT_EQ(NC_NOERR, nc_inq_varids(looked_up_delta, nullptr, nullptr));
  int empty_ndims = -1;
  int untouched_dimid = -99;
  EXPECT_EQ(NC_NOERR,
            nc_inq_dimids(looked_up_delta, &empty_ndims, &untouched_dimid, 0));
  EXPECT_EQ(0, empty_ndims);
  EXPECT_EQ(-99, untouched_dimid);

  // Short group name queries.
  char short_name[NC_MAX_NAME + 1]{};
  ASSERT_EQ(NC_NOERR, nc_inq_grpname(root_id, short_name));
  EXPECT_STREQ("/", short_name);
  ASSERT_EQ(NC_NOERR, nc_inq_grpname(looked_up_gamma, short_name));
  EXPECT_STREQ("gamma", short_name);
  EXPECT_EQ(NC_NOERR, nc_inq_grpname(looked_up_gamma, nullptr));

  // Full group path queries (root, depth 1, depth 3, NULL lenp/full_name).
  std::size_t full_len = 0;
  char full_name[4 * (NC_MAX_NAME + 1)]{};
  ASSERT_EQ(NC_NOERR, nc_inq_grpname_full(root_id, &full_len, full_name));
  EXPECT_EQ(1u, full_len);
  EXPECT_STREQ("/", full_name);
  EXPECT_EQ(NC_NOERR, nc_inq_grpname_full(root_id, nullptr, nullptr));

  ASSERT_EQ(NC_NOERR,
            nc_inq_grpname_full(looked_up_alpha, &full_len, full_name));
  EXPECT_EQ(6u, full_len);
  EXPECT_STREQ("/alpha", full_name);

  std::size_t gamma_len = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_grpname_len(looked_up_gamma, &gamma_len));
  EXPECT_EQ(17u, gamma_len);
  std::vector<char> gamma_full(gamma_len + 1, '\0');
  ASSERT_EQ(NC_NOERR,
            nc_inq_grpname_full(looked_up_gamma, nullptr, gamma_full.data()));
  EXPECT_STREQ("/alpha/beta/gamma", gamma_full.data());
  EXPECT_EQ(NC_NOERR, nc_inq_grpname_full(looked_up_gamma, nullptr, nullptr));

  // Parent group traversal.
  int parent_id = -1;
  EXPECT_EQ(NC_ENOGRP, nc_inq_grp_parent(root_id, &parent_id));
  EXPECT_EQ(NC_ENOGRP, nc_inq_grp_parent(root_id, nullptr));
  ASSERT_EQ(NC_NOERR, nc_inq_grp_parent(looked_up_alpha, &parent_id));
  EXPECT_EQ(root_id, parent_id);
  ASSERT_EQ(NC_NOERR, nc_inq_grp_parent(looked_up_gamma, &parent_id));
  EXPECT_EQ(looked_up_beta, parent_id);
  EXPECT_EQ(NC_NOERR, nc_inq_grp_parent(looked_up_gamma, nullptr));

  // Full-path lookup via nc_inq_grp_full_ncid().
  int found_id = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_grp_full_ncid(root_id, "/", &found_id));
  EXPECT_EQ(root_id, found_id);
  ASSERT_EQ(NC_NOERR, nc_inq_grp_full_ncid(root_id, "", &found_id));
  EXPECT_EQ(root_id, found_id);
  EXPECT_EQ(NC_ENOGRP, nc_inq_grp_full_ncid(looked_up_alpha, "/", &found_id));
  EXPECT_EQ(NC_ENOGRP, nc_inq_grp_full_ncid(looked_up_alpha, "", &found_id));

  ASSERT_EQ(NC_NOERR, nc_inq_grp_full_ncid(root_id, "/alpha", &found_id));
  EXPECT_EQ(looked_up_alpha, found_id);
  ASSERT_EQ(NC_NOERR,
            nc_inq_grp_full_ncid(root_id, "/alpha/beta/gamma", &found_id));
  EXPECT_EQ(looked_up_gamma, found_id);
  ASSERT_EQ(NC_NOERR,
            nc_inq_grp_full_ncid(root_id, "alpha//beta/gamma/", &found_id));
  EXPECT_EQ(looked_up_gamma, found_id);
  ASSERT_EQ(NC_NOERR,
            nc_inq_grp_full_ncid(looked_up_alpha, "beta/gamma", &found_id));
  EXPECT_EQ(looked_up_gamma, found_id);
  ASSERT_EQ(NC_NOERR,
            nc_inq_grp_full_ncid(root_id, "/cafe\xCC\x81", &found_id));
  EXPECT_EQ(looked_up_utf8, found_id);
  EXPECT_EQ(NC_NOERR,
            nc_inq_grp_full_ncid(root_id, "/alpha/beta/gamma", nullptr));
}

TEST(Nc4GrpTest, GroupRenameUpdatesMetadataAndLookup) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("nc4grp_rename.nc", NC_NETCDF4, 4096, nc.ptr()));
  int child_id = -1;
  int grand_id = -1;
  ASSERT_EQ(NC_NOERR, nc_def_grp(nc.get(), "before", &child_id));
  ASSERT_EQ(NC_NOERR, nc_def_grp(child_id, "leaf", &grand_id));

  ASSERT_EQ(NC_NOERR, nc_rename_grp(child_id, "after"));

  char name[NC_MAX_NAME + 1]{};
  ASSERT_EQ(NC_NOERR, nc_inq_grpname(child_id, name));
  EXPECT_STREQ("after", name);

  std::size_t len = 0;
  char full_name[2 * (NC_MAX_NAME + 1)]{};
  ASSERT_EQ(NC_NOERR, nc_inq_grpname_full(grand_id, &len, full_name));
  EXPECT_EQ(11u, len);
  EXPECT_STREQ("/after/leaf", full_name);

  int found_id = -1;
  EXPECT_EQ(NC_ENOGRP, nc_inq_ncid(nc.get(), "before", &found_id));
  ASSERT_EQ(NC_NOERR, nc_inq_ncid(nc.get(), "after", &found_id));
  EXPECT_EQ(child_id, found_id);
  ASSERT_EQ(NC_NOERR, nc_inq_grp_full_ncid(nc.get(), "/after/leaf", &found_id));
  EXPECT_EQ(grand_id, found_id);

  ScopedMemio memio;
  ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
}

TEST(Nc4GrpTest, RejectsInvalidIdsMissingGroupsAndInvalidNames) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("nc4grp_errors.nc", NC_NETCDF4, 4096, nc.ptr()));

  int sub_id = -1;
  int nested_id = -1;
  ASSERT_EQ(NC_NOERR, nc_def_grp(nc.get(), "sub", &sub_id));
  ASSERT_EQ(NC_NOERR, nc_def_grp(sub_id, "nested", &nested_id));

  const int bad_grp_ncid = MakeInvalidGroupSubId(nc.get());
  int out_id = -1;
  int count = -1;
  char name[NC_MAX_NAME + 1]{};

  // Invalid group sub-IDs enter nc4grp.c handlers and fail nc4_find_grp_h5().
  EXPECT_EQ(NC_EBADID, nc_inq_ncid(bad_grp_ncid, "sub", &out_id));
  EXPECT_EQ(NC_EBADID, nc_inq_grps(bad_grp_ncid, &count, nullptr));
  EXPECT_EQ(NC_EBADID, nc_inq_grpname(bad_grp_ncid, name));
  EXPECT_EQ(NC_EBADID, nc_inq_grp_parent(bad_grp_ncid, &out_id));
  EXPECT_EQ(NC_EBADID, nc_inq_grp_full_ncid(bad_grp_ncid, "/sub", &out_id));
  EXPECT_EQ(NC_EBADID, nc_inq_varids(bad_grp_ncid, &count, nullptr));
  EXPECT_EQ(NC_EBADID, nc_inq_dimids(bad_grp_ncid, &count, nullptr, 0));
  EXPECT_EQ(NC_EBADID, nc_inq_dimids(bad_grp_ncid, &count, nullptr, 1));

  // Completely bogus file ID is rejected before group lookup.
  EXPECT_EQ(NC_EBADID, nc_inq_ncid(-1, "sub", &out_id));

  // Name validation and missing direct children.
  const std::string too_long(NC_MAX_NAME + 1, 'a');
  EXPECT_EQ(NC_EBADNAME, nc_inq_ncid(nc.get(), "", &out_id));
  EXPECT_EQ(NC_EBADNAME, nc_inq_ncid(nc.get(), "sub/nested", &out_id));
  EXPECT_EQ(NC_EBADNAME, nc_inq_ncid(nc.get(), "bad ", &out_id));
  EXPECT_EQ(NC_EMAXNAME, nc_inq_ncid(nc.get(), too_long.c_str(), &out_id));
  EXPECT_EQ(NC_ENOGRP, nc_inq_ncid(nc.get(), "no_such_group", &out_id));
  // Grandchildren are not direct children of root.
  EXPECT_EQ(NC_ENOGRP, nc_inq_ncid(nc.get(), "nested", &out_id));

  // Full path validation errors.
  EXPECT_EQ(NC_EINVAL, nc_inq_grp_full_ncid(nc.get(), nullptr, &out_id));
  EXPECT_EQ(NC_ENOGRP,
            nc_inq_grp_full_ncid(nc.get(), "/sub/missing/nested", &out_id));
  EXPECT_EQ(NC_EBADNAME, nc_inq_grp_full_ncid(nc.get(), "/sub/bad ", &out_id));
  const std::string long_full = "/sub/" + too_long;
  EXPECT_EQ(NC_EMAXNAME,
            nc_inq_grp_full_ncid(nc.get(), long_full.c_str(), &out_id));
  // Lookup starts from the supplied group, not necessarily root.
  EXPECT_EQ(NC_ENOGRP, nc_inq_grp_full_ncid(sub_id, "/sub/nested", &out_id));

  // Classic (NetCDF-3) files dispatch to NC3_* handlers: nc_def_grp() fails
  // with NC_ENOTNC4 and nc_inq_grp_full_ncid() returns NC_ENOGRP, whereas
  // NC3_inq_ncid() succeeds and returns the root ncid. NetCDF-4 files created
  // with NC_CLASSIC_MODEL dispatch to nc4grp.c: nc_def_grp() returns
  // NC_ESTRICTNC3 and nc_inq_ncid() returns NC_ENOGRP for missing groups.
  ScopedNcFile classic_nc;
  ASSERT_EQ(NC_NOERR, nc_create_mem("classic_nogrp.nc", NC_CLASSIC_MODEL, 1024,
                                    classic_nc.ptr()));
  EXPECT_EQ(NC_ENOTNC4, nc_def_grp(classic_nc.get(), "sub", &out_id));
  EXPECT_EQ(NC_ENOGRP, nc_inq_grp_full_ncid(classic_nc.get(), "/sub", &out_id));
  out_id = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_ncid(classic_nc.get(), "sub", &out_id));
  EXPECT_EQ(classic_nc.get(), out_id);

  ScopedNcFile strict_nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("strict_nc4_nogrp.nc", NC_NETCDF4 | NC_CLASSIC_MODEL,
                          4096, strict_nc.ptr()));
  EXPECT_EQ(NC_ESTRICTNC3, nc_def_grp(strict_nc.get(), "sub", &out_id));
  EXPECT_EQ(NC_ENOGRP, nc_inq_ncid(strict_nc.get(), "sub", &out_id));
  out_id = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_ncid(strict_nc.get(), nullptr, &out_id));
  EXPECT_EQ(strict_nc.get(), out_id);

  ASSERT_EQ(NC_NOERR, strict_nc.close());
  ASSERT_EQ(NC_NOERR, classic_nc.close());
  ASSERT_EQ(NC_NOERR, nc.close());
}

TEST(Nc4GrpTest, InqVaridsAndSortedInqDimidsAcrossHierarchy) {
  ScopedMemio memio;
  int d_root0 = -1;
  int d_child0 = -1;
  int d_parent0 = -1;
  int d_root1 = -1;
  int d_parent1 = -1;
  int d_child1 = -1;
  int d_sibling = -1;

  int var_root0 = -1;
  int var_parent0 = -1;
  int var_parent1 = -1;
  int var_child0 = -1;
  int var_child1 = -1;
  int var_child2 = -1;

  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("nc4grp_vars_dims.nc", NC_NETCDF4, 4096, nc.ptr()));

    int parent_id = -1;
    int child_id = -1;
    int sibling_id = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc.get(), "parent", &parent_id));
    ASSERT_EQ(NC_NOERR, nc_def_grp(parent_id, "child", &child_id));
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc.get(), "sibling", &sibling_id));

    // Define dimensions out of hierarchical order so parent-inclusive dimid
    // arrays are unsorted prior to qsort(..., int_cmp).
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "r0", 2, &d_root0));
    ASSERT_EQ(NC_NOERR, nc_def_dim(child_id, "c0", 3, &d_child0));
    ASSERT_EQ(NC_NOERR, nc_def_dim(parent_id, "p0", 4, &d_parent0));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "r1", 5, &d_root1));
    ASSERT_EQ(NC_NOERR, nc_def_dim(parent_id, "p1", 6, &d_parent1));
    ASSERT_EQ(NC_NOERR, nc_def_dim(child_id, "c1", 7, &d_child1));
    ASSERT_EQ(NC_NOERR, nc_def_dim(sibling_id, "s0", 8, &d_sibling));

    // Define variables in each group (leaving sibling empty).
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc.get(), "vr0", NC_INT, 1, &d_root0, &var_root0));
    ASSERT_EQ(NC_NOERR, nc_def_var(parent_id, "vp0", NC_FLOAT, 1, &d_parent0,
                                   &var_parent0));
    ASSERT_EQ(NC_NOERR, nc_def_var(parent_id, "vp1", NC_DOUBLE, 1, &d_root1,
                                   &var_parent1));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(child_id, "vc0", NC_INT, 1, &d_child0, &var_child0));
    ASSERT_EQ(NC_NOERR, nc_def_var(child_id, "vc1", NC_SHORT, 1, &d_parent1,
                                   &var_child1));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(child_id, "vc2", NC_BYTE, 1, &d_child1, &var_child2));

    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }

  ScopedNcFile reader;
  ASSERT_EQ(NC_NOERR,
            nc_open_mem("nc4grp_vars_dims.nc", NC_NOWRITE, memio.size(),
                        memio.get()->memory, reader.ptr()));
  const int root_id = reader.get();
  int parent_id = -1;
  int child_id = -1;
  int sibling_id = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_grp_full_ncid(root_id, "/parent", &parent_id));
  ASSERT_EQ(NC_NOERR,
            nc_inq_grp_full_ncid(root_id, "/parent/child", &child_id));
  ASSERT_EQ(NC_NOERR, nc_inq_grp_full_ncid(root_id, "/sibling", &sibling_id));

  // nc_inq_varids(): empty group, count-only, ids-only, and both.
  int nvars = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varids(sibling_id, &nvars, nullptr));
  EXPECT_EQ(0, nvars);

  ASSERT_EQ(NC_NOERR, nc_inq_varids(child_id, &nvars, nullptr));
  ASSERT_EQ(3, nvars);
  std::vector<int> child_varids(3, -1);
  EXPECT_EQ(NC_NOERR, nc_inq_varids(child_id, nullptr, child_varids.data()));
  EXPECT_THAT(child_varids, ElementsAre(var_child0, var_child1, var_child2));

  int parent_nvars = -1;
  std::vector<int> parent_varids(2, -1);
  ASSERT_EQ(NC_NOERR,
            nc_inq_varids(parent_id, &parent_nvars, parent_varids.data()));
  EXPECT_EQ(2, parent_nvars);
  EXPECT_THAT(parent_varids, ElementsAre(var_parent0, var_parent1));

  // nc_inq_dimids(): include_parents == 0 vs non-zero (sorted ascending).
  int ndims = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_dimids(child_id, &ndims, nullptr, 0));
  ASSERT_EQ(2, ndims);
  std::vector<int> child_dimids_local(2, -1);
  EXPECT_EQ(NC_NOERR,
            nc_inq_dimids(child_id, nullptr, child_dimids_local.data(), 0));
  EXPECT_THAT(child_dimids_local, ElementsAre(d_child0, d_child1));

  int ndims_all = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_dimids(child_id, &ndims_all, nullptr, 1));
  ASSERT_EQ(6, ndims_all);
  std::vector<int> child_dimids_all(6, -1);
  ASSERT_EQ(NC_NOERR, nc_inq_dimids(child_id, nullptr, child_dimids_all.data(),
                                    /*include_parents=*/1));
  EXPECT_THAT(child_dimids_all, ElementsAre(d_root0, d_child0, d_parent0,
                                            d_root1, d_parent1, d_child1));

  // Non-1 non-zero include_parents value also includes ancestors.
  std::vector<int> parent_dimids_all(4, -1);
  int parent_ndims = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_dimids(parent_id, &parent_ndims,
                                    parent_dimids_all.data(), 7));
  EXPECT_EQ(4, parent_ndims);
  EXPECT_THAT(parent_dimids_all,
              ElementsAre(d_root0, d_parent0, d_root1, d_parent1));

  // Optional NULL output pointers for nc_inq_dimids().
  EXPECT_EQ(NC_NOERR, nc_inq_dimids(child_id, nullptr, nullptr, 0));
  EXPECT_EQ(NC_NOERR, nc_inq_dimids(child_id, nullptr, nullptr, 1));
}

}  // namespace
}  // namespace netcdf
