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

// Clean-room unit tests for //third_party/netcdf/libhdf5/hdf5grp.c.
//
// Exercises NC4_def_grp and NC4_rename_grp error handling, name checks,
// duplicate checks across variables/groups/types, define-mode transitions,
// nullptr new_ncid, pre-enddef vs post-enddef renames, nested group
// hierarchies, HDF5 close/move/open error handling, and persistence across
// close/reopen cycles.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"

extern "C" {
#include "third_party/netcdf/include/hdf5internal.h"
}  // extern "C"

namespace netcdf {
namespace {

void CloseMemio(ScopedNcFile* file, ScopedMemio* memio) {
  const int status = nc_close_memio(file->get(), memio->get());
  if (status == NC_NOERR) {
    file->release();
    return;
  }
  static_cast<void>(file->abort());
  ASSERT_EQ(NC_NOERR, status);
}

void OpenSubgroupInRedefMode(const char* filename, ScopedNcFile* file,
                             int* subid, NC_HDF5_GRP_INFO_T** root_h5,
                             NC_HDF5_GRP_INFO_T** child_h5) {
  ASSERT_EQ(NC_NOERR, nc_create_mem(filename, NC_NETCDF4, 4096, file->ptr()));
  ASSERT_EQ(NC_NOERR, nc_def_grp(*file, "sub", subid));
  ASSERT_EQ(NC_NOERR, nc_enddef(*file));
  ASSERT_EQ(NC_NOERR, nc_redef(*file));

  NC_GRP_INFO_T* root_grp = nullptr;
  NC_GRP_INFO_T* child_grp = nullptr;
  ASSERT_EQ(NC_NOERR, nc4_find_grp_h5(*file, &root_grp, nullptr));
  ASSERT_EQ(NC_NOERR, nc4_find_grp_h5(*subid, &child_grp, nullptr));
  ASSERT_NE(root_grp, nullptr);
  ASSERT_NE(child_grp, nullptr);
  ASSERT_NE(root_grp->format_grp_info, nullptr);
  ASSERT_NE(child_grp->format_grp_info, nullptr);

  *root_h5 = static_cast<NC_HDF5_GRP_INFO_T*>(root_grp->format_grp_info);
  *child_h5 = static_cast<NC_HDF5_GRP_INFO_T*>(child_grp->format_grp_info);
  ASSERT_GT((*root_h5)->hdf_grpid, 0);
  ASSERT_GT((*child_h5)->hdf_grpid, 0);
}

TEST(Hdf5GrpTest, DefGrpValidatesIdsNamesDuplicatesAndClassicModel) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("grp_validate.nc", NC_NETCDF4, 4096, file.ptr()));

  int grpid = -1;
  // Valid file ID bits paired with non-existent group ID bits exercise
  // nc4_find_grp_h5 inside NC4_def_grp.
  EXPECT_EQ(NC_EBADID, nc_def_grp(file | 0x7fff, "bad_parent", &grpid));
  EXPECT_EQ(NC_EBADID, nc_def_grp(-1, "invalid_id", &grpid));

  // Invalid names caught by nc4_check_name.
  EXPECT_EQ(NC_EINVAL, nc_def_grp(file, nullptr, &grpid));
  EXPECT_EQ(NC_EBADNAME, nc_def_grp(file, "", &grpid));
  EXPECT_EQ(NC_EBADNAME, nc_def_grp(file, " leading_space", &grpid));
  EXPECT_EQ(NC_EBADNAME, nc_def_grp(file, "bad/slash", &grpid));
  const std::string too_long(NC_MAX_NAME + 5, 'g');
  EXPECT_EQ(NC_EMAXNAME, nc_def_grp(file, too_long.c_str(), &grpid));

  // Define one subgroup, one variable, and one enum type in root to test
  // nc4_check_dup_name against all three object namespaces.
  int subid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_grp(file, "existing_grp", &subid));
  int dimid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "dim", 2, &dimid));
  int varid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(file, "existing_var", NC_INT, 1, &dimid, &varid));
  nc_type enum_type = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_enum(file, NC_INT, "existing_type", &enum_type));
  // HDF5 requires at least one enum member before committing metadata on close.
  const int32_t kEnumMember = 0;
  ASSERT_EQ(NC_NOERR,
            nc_insert_enum(file, enum_type, "ENUM_ZERO", &kEnumMember));

  EXPECT_EQ(NC_ENAMEINUSE, nc_def_grp(file, "existing_grp", &grpid));
  EXPECT_EQ(NC_ENAMEINUSE, nc_def_grp(file, "existing_var", &grpid));
  EXPECT_EQ(NC_ENAMEINUSE, nc_def_grp(file, "existing_type", &grpid));

  // Classic-model NetCDF-4 forbids groups.
  ScopedNcFile classic;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("grp_classic.nc", NC_NETCDF4 | NC_CLASSIC_MODEL, 2048,
                          classic.ptr()));
  EXPECT_EQ(NC_ESTRICTNC3, nc_def_grp(classic, "disallowed", &grpid));

  ASSERT_EQ(NC_NOERR, classic.close());
  ASSERT_EQ(NC_NOERR, file.close());
}

TEST(Hdf5GrpTest, DefGrpHandlesNullOutputIdAutoRedefAndReadOnlyRejection) {
  ScopedMemio memio;
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("grp_autodef.nc", NC_NETCDF4, 4096, file.ptr()));

    // new_ncid == nullptr should still create the group cleanly.
    ASSERT_EQ(NC_NOERR, nc_def_grp(file, "quiet_grp", nullptr));
    int quiet_id = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_ncid(file, "quiet_grp", &quiet_id));
    EXPECT_GE(quiet_id, 0);

    // Exit define mode explicitly; defining a nested subgroup must auto-redef.
    ASSERT_EQ(NC_NOERR, nc_enddef(file));
    int auto_child_id = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(quiet_id, "auto_child", &auto_child_id));
    int grandchild_id = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_grp(auto_child_id, "grandchild", &grandchild_id));

    char name_buf[NC_MAX_NAME + 1] = {};
    ASSERT_EQ(NC_NOERR, nc_inq_grpname(grandchild_id, name_buf));
    EXPECT_STREQ("grandchild", name_buf);

    size_t full_len = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_grpname_full(grandchild_id, &full_len, nullptr));
    std::vector<char> full_name(full_len + 1, '\0');
    ASSERT_EQ(NC_NOERR,
              nc_inq_grpname_full(grandchild_id, &full_len, full_name.data()));
    EXPECT_STREQ("/quiet_grp/auto_child/grandchild", full_name.data());

    ASSERT_NO_FATAL_FAILURE(CloseMemio(&file, &memio));
  }

  // Reopen read-only in data mode: auto-redef inside NC4_def_grp returns
  // NC_EPERM.
  ScopedNcFile ro_file;
  ASSERT_EQ(NC_NOERR, nc_open_mem("grp_autodef.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, ro_file.ptr()));
  int ro_child_id = -1;
  EXPECT_EQ(NC_EPERM, nc_def_grp(ro_file, "blocked_on_ro", &ro_child_id));
  ASSERT_EQ(NC_NOERR, ro_file.close());
}

TEST(Hdf5GrpTest, RenameGrpValidatesErrorsAndPermissions) {
  ScopedMemio memio;
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create_mem("grp_rename_errs.nc", NC_NETCDF4, 4096,
                                      file.ptr()));
    int grp_a = -1;
    int grp_b = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(file, "alpha", &grp_a));
    ASSERT_EQ(NC_NOERR, nc_def_grp(file, "beta", &grp_b));

    int dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "n", 2, &dimid));
    int varid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file, "parent_var", NC_INT, 1, &dimid, &varid));
    nc_type enum_type = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_enum(file, NC_INT, "parent_type", &enum_type));
    // Ensure the enum type commits cleanly when nc_close_memio() syncs.
    const int32_t kEnumMember = 1;
    ASSERT_EQ(NC_NOERR,
              nc_insert_enum(file, enum_type, "ENUM_ONE", &kEnumMember));

    // Bad group IDs.
    EXPECT_EQ(NC_EBADID, nc_rename_grp(-1, "new_name"));
    EXPECT_EQ(NC_EBADID, nc_rename_grp(file | 0x7fff, "new_name"));

    // Root group cannot be renamed.
    EXPECT_EQ(NC_EBADGRPID, nc_rename_grp(file, "root_renamed"));

    // Invalid names rejected by nc4_check_name.
    EXPECT_EQ(NC_EINVAL, nc_rename_grp(grp_a, nullptr));
    EXPECT_EQ(NC_EBADNAME, nc_rename_grp(grp_a, ""));
    EXPECT_EQ(NC_EBADNAME, nc_rename_grp(grp_a, " leading_space"));
    EXPECT_EQ(NC_EBADNAME, nc_rename_grp(grp_a, "bad/child"));
    const std::string too_long(NC_MAX_NAME + 5, 'r');
    EXPECT_EQ(NC_EMAXNAME, nc_rename_grp(grp_a, too_long.c_str()));

    // Collisions against sibling group, parent var, and parent type.
    EXPECT_EQ(NC_ENAMEINUSE, nc_rename_grp(grp_a, "beta"));
    EXPECT_EQ(NC_ENAMEINUSE, nc_rename_grp(grp_a, "parent_var"));
    EXPECT_EQ(NC_ENAMEINUSE, nc_rename_grp(grp_a, "parent_type"));

    ASSERT_NO_FATAL_FAILURE(CloseMemio(&file, &memio));
  }

  ScopedNcFile ro_file;
  ASSERT_EQ(NC_NOERR,
            nc_open_mem("grp_rename_errs.nc", NC_NOWRITE, memio.size(),
                        memio.get()->memory, ro_file.ptr()));
  int ro_grp_a = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_ncid(ro_file, "alpha", &ro_grp_a));
  EXPECT_EQ(NC_EPERM, nc_rename_grp(ro_grp_a, "alpha_ro_fail"));
  ASSERT_EQ(NC_NOERR, ro_file.close());
}

TEST(Hdf5GrpTest, RenameGrpPreAndPostEnddefAndAcrossReopen) {
  TempFile disk_nc("hdf5grp_rename_persist.nc");
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create(disk_nc.c_str(), NC_NETCDF4 | NC_CLOBBER, file.ptr()));

    int top_grp = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(file, "draft_top", &top_grp));
    // Rename before initial nc_enddef (hdf_grpid == 0 branch).
    ASSERT_EQ(NC_NOERR, nc_rename_grp(top_grp, "initial_top"));

    int old_lookup = -1;
    EXPECT_EQ(NC_ENOGRP, nc_inq_ncid(file, "draft_top", &old_lookup));
    int resolved_top = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_ncid(file, "initial_top", &resolved_top));
    EXPECT_EQ(top_grp, resolved_top);

    int child_grp = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(top_grp, "draft_child", &child_grp));
    int dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(child_grp, "x", 3, &dimid));
    int varid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(child_grp, "vec", NC_INT, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    const int kPayload[3] = {10, 20, 30};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(child_grp, varid, kPayload));

    // Rename while out of define mode (auto NC4_redef + H5Gclose/H5Lmove/
    // H5Gopen2 branch on materialized HDF5 group).
    ASSERT_EQ(NC_NOERR, nc_rename_grp(child_grp, "renamed_child_datamode"));

    // Enter define mode explicitly and rename the top-level group while it
    // contains an open child group and HDF5 handle.
    ASSERT_EQ(NC_NOERR, nc_enddef(file));
    ASSERT_EQ(NC_NOERR, nc_redef(file));
    ASSERT_EQ(NC_NOERR, nc_rename_grp(top_grp, "renamed_top_defmode"));

    ASSERT_EQ(NC_NOERR, file.close());
  }

  // Reopen existing file read/write, verify renamed hierarchy & data, and
  // perform another rename on both parent and child groups after reopen.
  {
    ScopedNcFile rw_file;
    ASSERT_EQ(NC_NOERR, nc_open(disk_nc.c_str(), NC_WRITE, rw_file.ptr()));

    int top_grp = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_ncid(rw_file, "renamed_top_defmode", &top_grp));
    int child_grp = -1;
    ASSERT_EQ(NC_NOERR,
              nc_inq_ncid(top_grp, "renamed_child_datamode", &child_grp));

    int parent_id = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_grp_parent(child_grp, &parent_id));
    EXPECT_EQ(top_grp, parent_id);

    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(child_grp, "vec", &varid));
    int read_back[3] = {-1, -1, -1};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(child_grp, varid, read_back));
    EXPECT_THAT(read_back, ::testing::ElementsAre(10, 20, 30));

    ASSERT_EQ(NC_NOERR, nc_rename_grp(child_grp, "final_child"));
    ASSERT_EQ(NC_NOERR, nc_rename_grp(top_grp, "final_top"));
    ASSERT_EQ(NC_NOERR, rw_file.close());
  }

  // Final read-only verification of persisted group names and hierarchy.
  {
    ScopedNcFile ro_file;
    ASSERT_EQ(NC_NOERR, nc_open(disk_nc.c_str(), NC_NOWRITE, ro_file.ptr()));
    int num_root_grps = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_grps(ro_file, &num_root_grps, nullptr));
    ASSERT_EQ(1, num_root_grps);

    int top_grp = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_grps(ro_file, nullptr, &top_grp));
    char top_name[NC_MAX_NAME + 1] = {};
    ASSERT_EQ(NC_NOERR, nc_inq_grpname(top_grp, top_name));
    EXPECT_STREQ("final_top", top_name);

    int child_grp = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_grp_full_ncid(ro_file, "/final_top/final_child",
                                             &child_grp));
    char child_name[NC_MAX_NAME + 1] = {};
    ASSERT_EQ(NC_NOERR, nc_inq_grpname(child_grp, child_name));
    EXPECT_STREQ("final_child", child_name);

    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(child_grp, "vec", &varid));
    std::vector<int> values(3, -1);
    ASSERT_EQ(NC_NOERR, nc_get_var_int(child_grp, varid, values.data()));
    EXPECT_THAT(values, ::testing::ElementsAre(10, 20, 30));
    ASSERT_EQ(NC_NOERR, ro_file.close());
  }
}

TEST(Hdf5GrpTest, RenameGrpReportsHdf5CloseAndMoveErrors) {
  ScopedNcFile file;
  int subid = -1;
  NC_HDF5_GRP_INFO_T* root_h5 = nullptr;
  NC_HDF5_GRP_INFO_T* child_h5 = nullptr;
  ASSERT_NO_FATAL_FAILURE(OpenSubgroupInRedefMode("grp_hdf_errs.nc", &file,
                                                  &subid, &root_h5, &child_h5));
  absl::Cleanup abort_file = [&]() { static_cast<void>(file.abort()); };

  const hid_t orig_root_hid = root_h5->hdf_grpid;
  const hid_t orig_child_hid = child_h5->hdf_grpid;
  absl::Cleanup restore_root = [&]() { root_h5->hdf_grpid = orig_root_hid; };
  {
    absl::Cleanup restore_child = [&]() {
      child_h5->hdf_grpid = orig_child_hid;
    };
    child_h5->hdf_grpid = -1;
    EXPECT_EQ(NC_EHDFERR, nc_rename_grp(subid, "sub_close_fail"));
  }

  // Now child_h5->hdf_grpid is valid again; closing succeeds and zeroes it,
  // then H5Lmove fails because parent's hdf_grpid is invalid.
  root_h5->hdf_grpid = -1;
  EXPECT_EQ(NC_EHDFERR, nc_rename_grp(subid, "sub_move_fail"));
  std::move(restore_root).Invoke();

  // child_h5->hdf_grpid is 0 after H5Gclose and file is in redef mode, so
  // abort() releases resources without calling remove().
  std::move(abort_file).Cancel();
  ASSERT_EQ(NC_NOERR, file.abort());
}

TEST(Hdf5GrpTest, RenameGrpReportsHdf5OpenErrorAfterMove) {
  ScopedNcFile file;
  int subid = -1;
  NC_HDF5_GRP_INFO_T* root_h5 = nullptr;
  NC_HDF5_GRP_INFO_T* child_h5 = nullptr;
  ASSERT_NO_FATAL_FAILURE(OpenSubgroupInRedefMode("grp_hdf_open_err.nc", &file,
                                                  &subid, &root_h5, &child_h5));

  absl::Cleanup cleanup_on_exit = [&]() {
    if (child_h5->hdf_grpid < 0) {
      child_h5->hdf_grpid = 0;
    }
    if (file.valid()) {
      static_cast<void>(file.abort());
    }
  };

  // Replace the materialized group link with a dangling soft link so
  // NC4_rename_grp closes child_h5->hdf_grpid, moves the link via H5Lmove,
  // and fails in H5Gopen2 because the soft-link target does not exist.
  ASSERT_GE(H5Gclose(child_h5->hdf_grpid), 0);
  child_h5->hdf_grpid = 0;
  ASSERT_GE(H5Ldelete(root_h5->hdf_grpid, "sub", H5P_DEFAULT), 0);
  ASSERT_GE(H5Lcreate_soft("/missing_target", root_h5->hdf_grpid, "sub",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  child_h5->hdf_grpid = H5Gcreate2(root_h5->hdf_grpid, "ephemeral_grp",
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  ASSERT_GT(child_h5->hdf_grpid, 0);

  EXPECT_EQ(NC_EHDFERR, nc_rename_grp(subid, "sub_open_fail"));
  EXPECT_LT(child_h5->hdf_grpid, hid_t{0});
  child_h5->hdf_grpid = 0;

  std::move(cleanup_on_exit).Cancel();
  ASSERT_EQ(NC_NOERR, file.abort());
}

TEST(Hdf5GrpTest, RenameGrpSkipsMoveWhenParentHandleUnopened) {
  ScopedNcFile file;
  int subid = -1;
  NC_HDF5_GRP_INFO_T* root_h5 = nullptr;
  NC_HDF5_GRP_INFO_T* child_h5 = nullptr;
  ASSERT_NO_FATAL_FAILURE(OpenSubgroupInRedefMode(
      "grp_hdf_no_parent_hid.nc", &file, &subid, &root_h5, &child_h5));
  absl::Cleanup abort_file = [&]() { static_cast<void>(file.abort()); };

  const hid_t orig_root_hid = root_h5->hdf_grpid;
  absl::Cleanup restore_root = [&]() { root_h5->hdf_grpid = orig_root_hid; };

  // When parent's hdf_grpid is 0, NC4_rename_grp closes child's handle, skips
  // H5Lmove/H5Gopen2, updates metadata name, and rebuilds the child index.
  root_h5->hdf_grpid = 0;
  EXPECT_EQ(NC_NOERR, nc_rename_grp(subid, "renamed_without_parent_hid"));
  EXPECT_EQ(hid_t{0}, child_h5->hdf_grpid);

  int looked_up = -1;
  ASSERT_EQ(NC_NOERR,
            nc_inq_ncid(file, "renamed_without_parent_hid", &looked_up));
  EXPECT_EQ(subid, looked_up);

  std::move(restore_root).Invoke();
  std::move(abort_file).Cancel();
  ASSERT_EQ(NC_NOERR, file.abort());
}

}  // namespace
}  // namespace netcdf
