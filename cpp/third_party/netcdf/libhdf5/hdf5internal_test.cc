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

// Unit tests for //third_party/netcdf/libhdf5/hdf5internal.c.

#include <array>
#include <cstddef>
#include <cstdlib>
#include <string>

#include "testing/base/public/gunit.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on
#include "third_party/netcdf/google/libhdf5/hdf5internal_shim.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"

namespace netcdf {
namespace {

TEST(Hdf5InternalTest, Hdf5InitAndFinalizeLifecycleInIsolatedChild) {
  RunInExecveChild([]() -> bool {
    if (NetcdfTestHdf5IsInitialized() != 0) {
      return false;
    }
    if (NetcdfTestHdf5InitializeDispatch() != NC_NOERR) {
      return false;
    }
    if (NetcdfTestHdf5IsInitialized() != 1) {
      return false;
    }
    // Second initialization hits the already-initialized fast path.
    if (NetcdfTestHdf5InitializeDispatch() != NC_NOERR) {
      return false;
    }
    if (NetcdfTestHdf5FinalizeDispatch() != NC_NOERR) {
      return false;
    }
    if (NetcdfTestHdf5IsInitialized() != 0) {
      return false;
    }

    if (nc_initialize() != NC_NOERR || NetcdfTestHdf5IsInitialized() != 1) {
      return false;
    }
    {
      int ncid = -1;
      if (nc_create("hdf5_init_child.nc", NC_NETCDF4 | NC_DISKLESS, &ncid) !=
          NC_NOERR) {
        return false;
      }
      int dimid = -1;
      int varid = -1;
      if (nc_def_dim(ncid, "x", 4, &dimid) != NC_NOERR ||
          nc_def_var(ncid, "v", NC_INT, 1, &dimid, &varid) != NC_NOERR ||
          nc_def_var_deflate(ncid, varid, 1, 1, 2) != NC_NOERR ||
          nc_enddef(ncid) != NC_NOERR) {
        nc_close(ncid);
        return false;
      }
      if (nc_close(ncid) != NC_NOERR) {
        return false;
      }
    }
    if (nc_finalize() != NC_NOERR || NetcdfTestHdf5IsInitialized() != 0) {
      return false;
    }
    if (nc_initialize() != NC_NOERR || NetcdfTestHdf5IsInitialized() != 1) {
      return false;
    }
    return nc_finalize() == NC_NOERR && NetcdfTestHdf5IsInitialized() == 0;
  });
}

TEST(Hdf5InternalTest, FindDimLenAndMaxLengthAcrossGroupsAndVarShapes) {
  ScopedMemio memio;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("dimlen_tree.nc", NC_NETCDF4, 8192, nc.ptr()));

    int unlim_dim = -1;
    int fixed_dim = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "time", NC_UNLIMITED, &unlim_dim));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 2, &fixed_dim));

    int scalar_var = -1;
    int fixed_var = -1;
    int root_unlim_var = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "scalar_v", NC_INT, 0, nullptr, &scalar_var));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "fixed_v", NC_INT, 1, &fixed_dim, &fixed_var));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "root_t", NC_INT, 1, &unlim_dim, &root_unlim_var));

    int sub_ncid = -1;
    int leaf_ncid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc, "sub", &sub_ncid));
    ASSERT_EQ(NC_NOERR, nc_def_grp(sub_ncid, "leaf", &leaf_ncid));

    int sub_small = -1;
    int sub_large = -1;
    int sub_grid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(sub_ncid, "t_small", NC_INT, 1, &unlim_dim,
                                   &sub_small));
    ASSERT_EQ(NC_NOERR, nc_def_var(sub_ncid, "t_large", NC_INT, 1, &unlim_dim,
                                   &sub_large));
    const std::array<int, 2> kGridDims = {fixed_dim, unlim_dim};
    ASSERT_EQ(NC_NOERR, nc_def_var(sub_ncid, "grid", NC_INT, 2,
                                   kGridDims.data(), &sub_grid));

    // Before nc_enddef, uncreated variables contribute length 0.
    size_t unlim_len = 999;
    ASSERT_EQ(NC_NOERR, nc_inq_dimlen(nc, unlim_dim, &unlim_len));
    EXPECT_EQ(unlim_len, 0U);

    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    const int kScalarVal = 7;
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, scalar_var, &kScalarVal));
    const std::array<int, 2> kFixedVals = {1, 2};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, fixed_var, kFixedVals.data()));

    const std::array<int, 2> kRootRecs = {10, 20};
    constexpr std::array<size_t, 1> kStart0 = {0};
    constexpr std::array<size_t, 1> kCountRoot = {2};
    ASSERT_EQ(NC_NOERR, nc_put_vara_int(nc, root_unlim_var, kStart0.data(),
                                        kCountRoot.data(), kRootRecs.data()));

    const int kSmallRec = 3;
    constexpr std::array<size_t, 1> kCount1 = {1};
    ASSERT_EQ(NC_NOERR, nc_put_vara_int(sub_ncid, sub_small, kStart0.data(),
                                        kCount1.data(), &kSmallRec));

    const std::array<int, 5> kLargeRecs = {1, 2, 3, 4, 5};
    constexpr std::array<size_t, 1> kCount5 = {5};
    ASSERT_EQ(NC_NOERR, nc_put_vara_int(sub_ncid, sub_large, kStart0.data(),
                                        kCount5.data(), kLargeRecs.data()));

    const std::array<int, 6> kGridRecs = {11, 12, 13, 14, 15, 16};
    constexpr std::array<size_t, 2> kGridStart = {0, 0};
    constexpr std::array<size_t, 2> kGridCount = {2, 3};
    ASSERT_EQ(NC_NOERR, nc_put_vara_int(sub_ncid, sub_grid, kGridStart.data(),
                                        kGridCount.data(), kGridRecs.data()));

    // Querying while open discovers the max across all subgroups/vars.
    ASSERT_EQ(NC_NOERR, nc_inq_dimlen(nc, unlim_dim, &unlim_len));
    EXPECT_EQ(unlim_len, 5U);

    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }

  // Reopen read-only with closed HDF5 dataset handles so
  // find_var_dim_max_length opens datasets on demand via nc4_open_var_grp2().
  ScopedNcFile ro_nc;
  ASSERT_EQ(NC_NOERR, nc_open_mem("dimlen_tree_ro.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, ro_nc.ptr()));
  int ro_unlim = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(ro_nc, "time", &ro_unlim));
  size_t ro_len = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_dimlen(ro_nc, ro_unlim, &ro_len));
  EXPECT_EQ(ro_len, 5U);
}

TEST(Hdf5InternalTest, RecFindHdfTypeMatchesCommittedAndHandlesMisses) {
  struct SamplePair {
    int first;
    double second;
  };

  ScopedMemio memio;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("find_type.nc", NC_NETCDF4, 4096, nc.ptr()));

    nc_type opq_tid = NC_NAT;
    nc_type cmp_tid = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_opaque(nc, 4, "opq4", &opq_tid));
    ASSERT_EQ(NC_NOERR,
              nc_def_compound(nc, sizeof(SamplePair), "pair_t", &cmp_tid));
    ASSERT_EQ(NC_NOERR, nc_insert_compound(
                            nc, cmp_tid, "first",
                            NC_COMPOUND_OFFSET(SamplePair, first), NC_INT));
    ASSERT_EQ(NC_NOERR, nc_insert_compound(
                            nc, cmp_tid, "second",
                            NC_COMPOUND_OFFSET(SamplePair, second), NC_DOUBLE));

    // Before nc_enddef, native_hdf_typeid == 0 -> nc4_rec_find_hdf_type falls
    // back to hdf_typeid (0) and H5Tequal returns < 0 -> NULL.
    int found = -1;
    int matched = -1;
    ASSERT_EQ(NC_NOERR,
              NetcdfTestFindHdfTypeForNcType(nc, opq_tid, &found, &matched));
    EXPECT_EQ(found, 0);
    EXPECT_EQ(matched, NC_NAT);

    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "n", 1, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "pair_var", cmp_tid, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    const SamplePair kPair = {99, 3.5};
    ASSERT_EQ(NC_NOERR, nc_put_var(nc, varid, &kPair));
    ASSERT_EQ(NC_NOERR,
              nc_put_att(nc, NC_GLOBAL, "pair_att", cmp_tid, 1, &kPair));

    // Match second user type in alltypes (skips NULL entries 0..31 and
    // non-matching opq4).
    ASSERT_EQ(NC_NOERR,
              NetcdfTestFindHdfTypeForNcType(nc, cmp_tid, &found, &matched));
    EXPECT_EQ(found, 1);
    EXPECT_EQ(matched, cmp_tid);

    // Search for an atomic HDF5 type not present in user-type list -> NULL.
    ASSERT_EQ(NC_NOERR, NetcdfTestFindHdfTypeBuiltinInt(nc, &found));
    EXPECT_EQ(found, 0);

    // Search with invalid hid_t -> H5Tequal < 0 -> NULL.
    ASSERT_EQ(NC_NOERR, NetcdfTestFindHdfTypeInvalidHid(nc, &found));
    EXPECT_EQ(found, 0);

    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }

  ScopedNcFile ro_nc;
  ASSERT_EQ(NC_NOERR, nc_open_mem("find_type_ro.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, ro_nc.ptr()));
  int ro_varid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "pair_var", &ro_varid));
  SamplePair got_pair{};
  ASSERT_EQ(NC_NOERR, nc_get_att(ro_nc, NC_GLOBAL, "pair_att", &got_pair));
  EXPECT_EQ(got_pair.first, 99);
  EXPECT_DOUBLE_EQ(got_pair.second, 3.5);
}

TEST(Hdf5InternalTest, CoordVarBreakDeleteDimscaleAndReformPublicWorkflows) {
  ScopedMemio memio;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("coord_ops.nc", NC_NETCDF4, 8192, nc.ptr()));

    // Root dimensions: a coordinate dimension, a coord dim left broken at
    // close, and a non-coord dimscale renamed later.
    int rdim = -1;
    int rdim_var = -1;
    int bdim = -1;
    int bdim_var = -1;
    int pdim = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "rdim", 2, &rdim));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "rdim", NC_INT, 1, &rdim, &rdim_var));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "bdim", 2, &bdim));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "bdim", NC_INT, 1, &bdim, &bdim_var));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "pdim", 2, &pdim));

    // Subgroup has two local dims and variables referencing both parent coord
    // dim and local non-coord dims.
    int sub_ncid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc, "sub", &sub_ncid));
    int sdim = -1;
    int sdim2 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(sub_ncid, "sdim", 2, &sdim));
    ASSERT_EQ(NC_NOERR, nc_def_dim(sub_ncid, "sdim2", 2, &sdim2));

    const std::array<int, 3> kVarDims = {sdim, rdim, sdim2};
    int var_to_reform = -1;
    int sibling_var = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(sub_ncid, "future_coord", NC_INT, 3,
                                   kVarDims.data(), &var_to_reform));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(sub_ncid, "observer", NC_INT, 1, &sdim, &sibling_var));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    const std::array<int, 2> kCoordVals = {1, 2};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, rdim_var, kCoordVals.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, bdim_var, kCoordVals.data()));
    ASSERT_EQ(NC_NOERR,
              nc_put_var_int(sub_ncid, sibling_var, kCoordVals.data()));
    const std::array<int, 8> kCubeVals = {10, 11, 12, 13, 14, 15, 16, 17};
    ASSERT_EQ(NC_NOERR,
              nc_put_var_int(sub_ncid, var_to_reform, kCubeVals.data()));

    // Break bdim's coord var via dim rename and leave dimscale_attached open
    // through nc_close_memio() so close_vars frees dimscale_attached.
    ASSERT_EQ(NC_NOERR, nc_rename_dim(nc, bdim, "bdim_only"));

    // Rename non-coord dim pdim -> triggers delete_dimscale_dataset().
    ASSERT_EQ(NC_NOERR, nc_rename_dim(nc, pdim, "pdim_renamed"));

    // Reform var_to_reform into coord var for sdim while detaching rdim (parent
    // group coord var) and sdim2 (local non-coord dimscale) and reattaching
    // observer.
    ASSERT_EQ(NC_NOERR, nc_rename_var(sub_ncid, var_to_reform, "sdim"));

    // Syncing writes the recreated dimension-scale dataset (initialized to
    // NC_FILL_INT) and clears became_coord_var; rewriting data after the sync
    // persists across reopen.
    ASSERT_EQ(NC_NOERR, nc_sync(nc));
    std::array<int, 8> post_sync_cube{};
    ASSERT_EQ(NC_NOERR,
              nc_get_var_int(sub_ncid, var_to_reform, post_sync_cube.data()));
    EXPECT_EQ(post_sync_cube[7], NC_FILL_INT);
    ASSERT_EQ(NC_NOERR,
              nc_put_var_int(sub_ncid, var_to_reform, kCubeVals.data()));

    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }

  ScopedNcFile ro_nc;
  ASSERT_EQ(NC_NOERR, nc_open_mem("coord_ops_ro.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, ro_nc.ptr()));
  int ro_sub = -1;
  int ro_sdim_var = -1;
  int ro_observer_var = -1;
  int ro_bdim_var = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_grp_ncid(ro_nc, "sub", &ro_sub));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_sub, "sdim", &ro_sdim_var));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_sub, "observer", &ro_observer_var));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "bdim", &ro_bdim_var));

  std::array<int, 8> read_cube{};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(ro_sub, ro_sdim_var, read_cube.data()));
  EXPECT_EQ(read_cube, (std::array<int, 8>{10, 11, 12, 13, 14, 15, 16, 17}));

  std::array<int, 2> read_obs{};
  std::array<int, 2> read_bdim{};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(ro_sub, ro_observer_var, read_obs.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_int(ro_nc, ro_bdim_var, read_bdim.data()));
  EXPECT_EQ(read_obs, (std::array<int, 2>{1, 2}));
  EXPECT_EQ(read_bdim, (std::array<int, 2>{1, 2}));
}

TEST(Hdf5InternalTest, ReformCoordVarEdgeCasesViaShim) {
  // Edge case 1: hdf_dimscaleid != 0 closed and unlinked by
  // nc4_reform_coord_var.
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("reform_dimscale.nc", NC_NETCDF4, 4096, nc.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "d_var", NC_INT, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));
    EXPECT_EQ(NC_NOERR, NetcdfTestReformCoordVarWithDimscale(nc, dimid, varid));
    ASSERT_EQ(NC_NOERR, nc.abort());
  }

  // Edge case 2: var->was_coord_var == NC_TRUE with dimscale_attached == NULL.
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("reform_wascv.nc", NC_NETCDF4, 4096, nc.ptr()));
    int dimid = -1;
    int coord_varid = -1;
    int obs_varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "d", NC_INT, 1, &dimid, &coord_varid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "obs", NC_INT, 1, &dimid, &obs_varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));
    ASSERT_EQ(NC_NOERR, nc_rename_var(nc, coord_varid, "d_broken"));
    EXPECT_EQ(NC_NOERR, NetcdfTestReformCoordVarWasCoordNoAttached(
                            nc, dimid, coord_varid));
    ASSERT_EQ(NC_NOERR, nc.abort());
  }

  // Edge case 3: dim_datasetid == 0 guard when dimscale_attached[d] is set.
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("reform_zero_ds.nc", NC_NETCDF4, 4096, nc.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v", NC_INT, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));
    EXPECT_EQ(NC_NOERR,
              NetcdfTestReformCoordVarZeroDimDataset(nc, dimid, varid));
    ASSERT_EQ(NC_NOERR, nc.abort());
  }
}

TEST(Hdf5InternalTest, CloseAndAbortReclaimAllHdf5ResourcesCleanly) {
  ScopedMemio memio;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("close_full.nc", NC_NETCDF4, 8192, nc.ptr()));

    int g1 = -1;
    int g2 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc, "g1", &g1));
    ASSERT_EQ(NC_NOERR, nc_def_grp(g1, "g2", &g2));

    int dim_only = -1;
    int dim_data = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "dim_only", 3, &dim_only));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "dim_data", 2, &dim_data));

    nc_type opq_tid = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_opaque(nc, 4, "tag4", &opq_tid));

    int var_int = -1;
    int var_str = -1;
    int var_opq = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "v_int", NC_INT, 1, &dim_data, &var_int));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(g1, "v_str", NC_STRING, 0, nullptr, &var_str));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(g2, "v_opq", opq_tid, 1, &dim_only, &var_opq));

    // Exercises var->filters and var->fill_value reclamation during close.
    ASSERT_EQ(NC_NOERR, nc_def_var_deflate(nc, var_int, 1, 1, 3));
    const int kIntFill = -999;
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(nc, var_int, 0, &kIntFill));
    const char* str_fill = "missing";
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(g1, var_str, 0, &str_fill));

    const int kGattVal = 123;
    const char* gatt_str = "root_meta";
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc, NC_GLOBAL, "gi", NC_INT, 1, &kGattVal));
    ASSERT_EQ(NC_NOERR, nc_put_att_string(nc, NC_GLOBAL, "gs", 1, &gatt_str));
    ASSERT_EQ(NC_NOERR, nc_put_att_string(g1, var_str, "vs_att", 1, &gatt_str));

    ASSERT_EQ(NC_NOERR, nc_enddef(nc));
    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }

  // Reopen populated file: dimscale_hdf5_objids and att typeids are allocated
  // and cleaned on close.
  {
    ScopedNcFile ro_nc;
    ASSERT_EQ(NC_NOERR,
              nc_open_mem("close_full_ro.nc", NC_NOWRITE, memio.size(),
                          memio.get()->memory, ro_nc.ptr()));
    int ro_g1 = -1;
    int ro_var_str = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_grp_ncid(ro_nc, "g1", &ro_g1));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_g1, "v_str", &ro_var_str));

    ScopedNcStrings strs(1);
    ASSERT_EQ(NC_NOERR, nc_get_var_string(ro_g1, ro_var_str, strs.data()));
    EXPECT_STREQ(strs[0], "missing");

    int ro_var_int = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "v_int", &ro_var_int));
    std::array<int, 2> got_ints{};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro_nc, ro_var_int, got_ints.data()));
    EXPECT_EQ(got_ints, (std::array<int, 2>{-999, -999}));
  }

  // Abort a newly created file before nc_enddef so hdf_grpid, hdf_datasetid,
  // hdf_dimscaleid, and hdf_typeid remain 0 in close_*().
  {
    TempFile abort_file("uncommitted_abort.nc");
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create(abort_file.c_str(), NC_NETCDF4 | NC_CLOBBER, nc.ptr()));
    if (abort_file.string().size() >= NC_MAX_NAME) {
      ASSERT_EQ(NC_NOERR, nc_enddef(nc));
      ASSERT_EQ(NC_NOERR, nc_redef(nc));
    }
    int sub = -1;
    int dimid = -1;
    int varid = -1;
    nc_type cmp_tid = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc, "sub_abort", &sub));
    ASSERT_EQ(NC_NOERR, nc_def_dim(sub, "d", 3, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_compound(sub, sizeof(int), "c", &cmp_tid));
    ASSERT_EQ(NC_NOERR, nc_insert_compound(sub, cmp_tid, "x", 0, NC_INT));
    ASSERT_EQ(NC_NOERR, nc_def_var(sub, "v", NC_STRING, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc_def_var_deflate(sub, varid, 0, 1, 1));
    const char* fill_str = "unwritten";
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(sub, varid, 0, &fill_str));
    ASSERT_EQ(NC_NOERR, nc.abort());
  }
}

TEST(Hdf5InternalTest, FindGrpH5VarAndAttLazyReadsNullOutputsAndErrors) {
  ScopedMemio memio;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("find_var_att.nc", NC_NETCDF4, 4096, nc.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "temp", NC_INT, 1, &dimid, &varid));

    // Uncommitted variable before nc_enddef (!my_var->created).
    int out_varid = -1;
    EXPECT_EQ(NC_NOERR, NetcdfTestFindGrpH5Var(nc, varid, 0, &out_varid));
    EXPECT_EQ(out_varid, varid);

    const int kVal = 10;
    const char* str_val = "hello";
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, NC_GLOBAL, "g0", NC_INT, 1, &kVal));
    ASSERT_EQ(NC_NOERR, nc_put_att_string(nc, NC_GLOBAL, "g1", 1, &str_val));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc, varid, "units_tag", NC_INT, 1, &kVal));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    const std::array<int, 2> kData = {1, 2};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, varid, kData.data()));
    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }

  // Reopen once to exercise nc4_hdf5_find_grp_h5_var lazy meta_read and NULL
  // optional output pointers.
  {
    ScopedNcFile ro_nc;
    ASSERT_EQ(NC_NOERR,
              nc_open_mem("find_var_att_ro1.nc", NC_NOWRITE, memio.size(),
                          memio.get()->memory, ro_nc.ptr()));
    int out_varid = -1;
    EXPECT_EQ(NC_EBADID, NetcdfTestFindGrpH5Var(-1, 0, 0, &out_varid));
    EXPECT_EQ(NC_EBADID,
              NetcdfTestFindGrpH5Var(ro_nc | 0x7f, 0, 0, &out_varid));
    EXPECT_EQ(NC_ENOTVAR, NetcdfTestFindGrpH5Var(ro_nc, 999, 0, &out_varid));

    // First lookup triggers !my_var->meta_read && my_var->created.
    ASSERT_EQ(NC_NOERR, NetcdfTestFindGrpH5Var(ro_nc, 0, 0, &out_varid));
    EXPECT_EQ(out_varid, 0);
    // Second lookup skips meta_read and ignores NULL outputs.
    ASSERT_EQ(NC_NOERR, NetcdfTestFindGrpH5Var(ro_nc, 0, 1, nullptr));
  }

  // Reopen a fresh handle so !my_grp->atts_read, !my_var->atts_read, and
  // !my_var->meta_read are all triggered in nc4_hdf5_find_grp_var_att.
  {
    ScopedNcFile ro_nc;
    ASSERT_EQ(NC_NOERR,
              nc_open_mem("find_var_att_ro2.nc", NC_NOWRITE, memio.size(),
                          memio.get()->memory, ro_nc.ptr()));

    std::array<char, NC_MAX_NAME + 1> norm_name{};
    int matched_attid = -1;

    EXPECT_EQ(NC_EBADID,
              NetcdfTestFindGrpVarAtt(-1, NC_GLOBAL, "g0", 0, 1, 0,
                                      norm_name.data(), &matched_attid));
    EXPECT_EQ(NC_EBADID,
              NetcdfTestFindGrpVarAtt(ro_nc | 0x7f, NC_GLOBAL, "g0", 0, 1, 0,
                                      norm_name.data(), &matched_attid));

    // First global lookup triggers !my_grp->atts_read.
    ASSERT_EQ(NC_NOERR,
              NetcdfTestFindGrpVarAtt(ro_nc, NC_GLOBAL, "g0", 0, 1, 0,
                                      norm_name.data(), &matched_attid));
    EXPECT_STREQ(norm_name.data(), "g0");
    EXPECT_EQ(matched_attid, 0);

    // Second global lookup by index with all optional outputs NULL.
    ASSERT_EQ(NC_NOERR, NetcdfTestFindGrpVarAtt(ro_nc, NC_GLOBAL, nullptr, 1, 0,
                                                1, nullptr, nullptr));

    // use_name == 1 with name == NULL and att == NULL -> NC_EBADNAME.
    EXPECT_EQ(NC_EBADNAME, NetcdfTestFindGrpVarAtt(ro_nc, NC_GLOBAL, nullptr, 0,
                                                   1, 1, nullptr, nullptr));

    // Oversized name triggers nc4_normalize_name -> NC_EMAXNAME.
    const std::string too_long(NC_MAX_NAME + 1, 'a');
    EXPECT_EQ(NC_EMAXNAME,
              NetcdfTestFindGrpVarAtt(ro_nc, NC_GLOBAL, too_long.c_str(), 0, 1,
                                      0, norm_name.data(), &matched_attid));

    // Missing attribute name and index on global group -> NC_ENOTATT.
    EXPECT_EQ(NC_ENOTATT,
              NetcdfTestFindGrpVarAtt(ro_nc, NC_GLOBAL, "missing_att", 0, 1, 0,
                                      norm_name.data(), &matched_attid));
    EXPECT_EQ(NC_ENOTATT,
              NetcdfTestFindGrpVarAtt(ro_nc, NC_GLOBAL, nullptr, 99, 0, 0,
                                      norm_name.data(), &matched_attid));

    // Invalid varid -> NC_ENOTVAR.
    EXPECT_EQ(NC_ENOTVAR,
              NetcdfTestFindGrpVarAtt(ro_nc, 999, "units_tag", 0, 1, 0,
                                      norm_name.data(), &matched_attid));

    // First variable attribute lookup triggers !my_var->atts_read and
    // !my_var->meta_read && my_var->created.
    ASSERT_EQ(NC_NOERR,
              NetcdfTestFindGrpVarAtt(ro_nc, 0, "units_tag", 0, 1, 0,
                                      norm_name.data(), &matched_attid));
    EXPECT_STREQ(norm_name.data(), "units_tag");
    EXPECT_EQ(matched_attid, 0);

    // Second variable attribute lookup by attnum uses cached metadata.
    matched_attid = -1;
    ASSERT_EQ(NC_NOERR,
              NetcdfTestFindGrpVarAtt(ro_nc, 0, nullptr, 0, 0, 0,
                                      norm_name.data(), &matched_attid));
    EXPECT_EQ(matched_attid, 0);
  }
}

TEST(Hdf5InternalTest, GetChunkCacheRetrievesSetValuesAndHandlesNullAndErrors) {
  ScopedChunkCache chunk_cache_guard;
  constexpr size_t kCacheSize = 524288;
  constexpr size_t kCacheNelems = 1009;
  constexpr float kPreemption = 0.25F;
  ASSERT_EQ(NC_NOERR,
            nc_set_chunk_cache(kCacheSize, kCacheNelems, kPreemption));

  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create("chunk_cache.nc", NC_NETCDF4 | NC_DISKLESS, nc.ptr()));

  size_t got_size = 0;
  size_t got_nelems = 0;
  float got_preemption = -1.0F;
  ASSERT_EQ(NC_NOERR, NetcdfTestHdf5GetChunkCache(nc, 0, &got_size, &got_nelems,
                                                  &got_preemption));
  EXPECT_EQ(got_size, kCacheSize);
  EXPECT_EQ(got_nelems, kCacheNelems);
  EXPECT_FLOAT_EQ(got_preemption, kPreemption);

  // Nullable output pointers return NC_NOERR.
  EXPECT_EQ(NC_NOERR,
            NetcdfTestHdf5GetChunkCache(nc, 1, nullptr, nullptr, nullptr));

  // Invalid ncid / group fails with NC_EBADID.
  EXPECT_EQ(NC_EBADID, NetcdfTestHdf5GetChunkCache(
                           -1, 0, &got_size, &got_nelems, &got_preemption));
  EXPECT_EQ(NC_EBADID,
            NetcdfTestHdf5GetChunkCache(nc | 0x7f, 0, &got_size, &got_nelems,
                                        &got_preemption));
}

TEST(Hdf5InternalTest, FindDimLenErrorBranchesViaShim) {
  ScopedMemio memio;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("dimlen_err.nc", NC_NETCDF4, 4096, nc.ptr()));
    int time_dim = -1;
    int sub_ncid = -1;
    int sub_var = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "time", NC_UNLIMITED, &time_dim));
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc, "sub", &sub_ncid));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(sub_ncid, "sub_v", NC_INT, 1, &time_dim, &sub_var));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    const std::array<int, 2> kVals = {11, 22};
    constexpr std::array<size_t, 1> kStart = {0};
    constexpr std::array<size_t, 1> kCount = {2};
    ASSERT_EQ(NC_NOERR, nc_put_vara_int(sub_ncid, sub_var, kStart.data(),
                                        kCount.data(), kVals.data()));
    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }

  ScopedNcFile ro_nc;
  ASSERT_EQ(NC_NOERR, nc_open_mem("dimlen_err_ro.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, ro_nc.ptr()));
  int ro_dim = -1;
  int ro_sub = -1;
  int ro_var = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(ro_nc, "time", &ro_dim));
  ASSERT_EQ(NC_NOERR, nc_inq_grp_ncid(ro_nc, "sub", &ro_sub));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_sub, "sub_v", &ro_var));

  // Mode 0: nc4_open_var_grp2() failure propagates through child group/var
  // loops.
  EXPECT_EQ(NC_ENOTVAR,
            NetcdfTestFindDimLenErrorPaths(ro_nc, ro_sub, ro_dim, ro_var, 0));
  // Mode 1: H5Dget_space() failure on invalid dataset ID.
  EXPECT_EQ(NC_EHDFERR,
            NetcdfTestFindDimLenErrorPaths(ro_nc, ro_sub, ro_dim, ro_var, 1));
  // Mode 2: dataset_ndims != var->ndims mismatch.
  EXPECT_EQ(NC_EHDFERR,
            NetcdfTestFindDimLenErrorPaths(ro_nc, ro_sub, ro_dim, ro_var, 2));

  size_t len = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_dimlen(ro_nc, ro_dim, &len));
  EXPECT_EQ(len, 2U);
}

TEST(Hdf5InternalTest, CoordAndDimscaleErrorBranchesViaShim) {
  // nc4_break_coord_var() returns error when rec_detach_scales() fails.
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("break_err.nc", NC_NETCDF4, 4096, nc.ptr()));
    int dimid = -1;
    int coord_varid = -1;
    int obs_varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "d", NC_INT, 1, &dimid, &coord_varid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "obs", NC_INT, 1, &dimid, &obs_varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));
    EXPECT_EQ(NC_EDIMSCALE, NetcdfTestBreakCoordVarDetachError(
                                nc, dimid, coord_varid, obs_varid));
    ASSERT_EQ(NC_NOERR, nc.abort());
  }

  // delete_dimscale_dataset() mode 0: rec_detach_scales() fails.
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("del_detach_err.nc", NC_NETCDF4, 4096, nc.ptr()));
    int dimid = -1;
    int obs_varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "obs", NC_INT, 1, &dimid, &obs_varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));
    EXPECT_EQ(NC_EDIMSCALE,
              NetcdfTestDeleteDimscaleErrorPaths(nc, dimid, obs_varid, 0));
    ASSERT_EQ(NC_NOERR, nc.abort());
  }

  // delete_dimscale_dataset() modes 1 & 2: H5Dclose() and H5Gunlink() failures.
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("del_h5_err.nc", NC_NETCDF4, 4096, nc.ptr()));
    int d1 = -1;
    int d2 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d1", 2, &d1));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d2", 2, &d2));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));
    EXPECT_EQ(NC_EHDFERR, NetcdfTestDeleteDimscaleErrorPaths(nc, d1, -1, 1));
    EXPECT_EQ(NC_EHDFERR, NetcdfTestDeleteDimscaleErrorPaths(nc, d2, -1, 2));
    ASSERT_EQ(NC_NOERR, nc.abort());
  }

  // nc4_reform_coord_var() modes 0, 1, 2: H5DSdetach_scale, H5Dclose,
  // H5Gunlink failures.
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("reform_h5_err.nc", NC_NETCDF4, 4096, nc.ptr()));
    int d0 = -1;
    int d1 = -1;
    int d2 = -1;
    int v0 = -1;
    int v1 = -1;
    int v2 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d0", 2, &d0));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d1", 2, &d1));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d2", 2, &d2));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v0", NC_INT, 1, &d0, &v0));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v1", NC_INT, 1, &d1, &v1));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v2", NC_INT, 1, &d2, &v2));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));
    EXPECT_EQ(NC_EHDFERR,
              NetcdfTestReformCoordVarErrorPaths(nc, d0, v0, -1, 0));
    EXPECT_EQ(NC_EHDFERR,
              NetcdfTestReformCoordVarErrorPaths(nc, d1, v1, -1, 1));
    EXPECT_EQ(NC_EDIMMETA,
              NetcdfTestReformCoordVarErrorPaths(nc, d2, v2, -1, 2));
    ASSERT_EQ(NC_NOERR, nc.abort());
  }

  // nc4_reform_coord_var() mode 3: rec_reattach_scales() failure.
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create_mem("reform_reattach_err.nc", NC_NETCDF4,
                                      4096, nc.ptr()));
    int dimid = -1;
    int cv = -1;
    int obs = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "d", NC_INT, 1, &dimid, &cv));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "obs", NC_INT, 1, &dimid, &obs));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));
    ASSERT_EQ(NC_NOERR, nc_rename_var(nc, cv, "d_broken"));
    EXPECT_EQ(NC_EDIMSCALE,
              NetcdfTestReformCoordVarErrorPaths(nc, dimid, cv, obs, 3));
    ASSERT_EQ(NC_NOERR, nc.abort());
  }
}

TEST(Hdf5InternalTest, CloseAttTypeFindAndCacheErrorBranchesViaShim) {
  // nc4_HDF5_close_att() and nc4_HDF5_close_type() H5Tclose() error branches.
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("close_err.nc", NC_NETCDF4, 4096, nc.ptr()));
    nc_type opq_tid = NC_NAT;
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_opaque(nc, 4, "opq4", &opq_tid));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "n", 1, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_opq", opq_tid, 1, &dimid, &varid));
    const int kAttr = 7;
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, NC_GLOBAL, "ga", NC_INT, 1, &kAttr));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    EXPECT_EQ(NC_EHDFERR, NetcdfTestCloseAttError(nc, NC_GLOBAL, 0));
    EXPECT_EQ(NC_EHDFERR, NetcdfTestCloseTypeError(nc, opq_tid, 0));
    EXPECT_EQ(NC_EHDFERR, NetcdfTestCloseTypeError(nc, opq_tid, 1));
    ASSERT_EQ(NC_NOERR, nc.abort());
  }

  // Lazy-read and chunk-cache error paths on a reopened file.
  ScopedMemio memio;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("lazy_err.nc", NC_NETCDF4, 4096, nc.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v", NC_INT, 1, &dimid, &varid));
    const int kTag = 42;
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, NC_GLOBAL, "ga", NC_INT, 1, &kTag));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, varid, "va", NC_INT, 1, &kTag));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));
    const std::array<int, 2> kData = {4, 5};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, varid, kData.data()));
    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }

  ScopedNcFile ro_nc;
  ASSERT_EQ(NC_NOERR, nc_open_mem("lazy_err_ro.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, ro_nc.ptr()));
  EXPECT_EQ(NC_EHDFERR, NetcdfTestFindGrpH5VarMetaError(ro_nc, 0));
  EXPECT_EQ(NC_EATTMETA, NetcdfTestFindGrpVarAttErrorPaths(ro_nc, 0, 0));
  EXPECT_EQ(NC_EATTMETA, NetcdfTestFindGrpVarAttErrorPaths(ro_nc, 0, 1));
  EXPECT_EQ(NC_EHDFERR, NetcdfTestFindGrpVarAttErrorPaths(ro_nc, 0, 2));
  EXPECT_EQ(NC_EHDFERR, NetcdfTestHdf5GetChunkCacheInvalidHdfid(ro_nc));

  // Restored handles remain valid for subsequent public reads and close.
  int got_att = 0;
  ASSERT_EQ(NC_NOERR, nc_get_att_int(ro_nc, NC_GLOBAL, "ga", &got_att));
  EXPECT_EQ(got_att, 42);
  std::array<int, 2> got_vals{};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(ro_nc, 0, got_vals.data()));
  EXPECT_EQ(got_vals, (std::array<int, 2>{4, 5}));
}

}  // namespace
}  // namespace netcdf
