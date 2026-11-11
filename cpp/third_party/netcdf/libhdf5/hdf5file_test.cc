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

// Clean-room unit tests for //third_party/netcdf/libhdf5/hdf5file.c.
//
// Exercises NC4_set_fill, NC4_redef, NC4__enddef/NC4_enddef, NC4_sync,
// NC4_abort, NC4_close, NC4_inq, nc4_enddef_netcdf4_file,
// detect_preserve_dimids, nc4_close_netcdf4_file, and dumpopenobjects.

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
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

using ::testing::ElementsAre;

// Serializes a minimal NetCDF-4/HDF5 file via on-disk creation so
// creation-order tracking is enabled on the root group (required for NC_WRITE
// memio opens).
std::vector<unsigned char> BuildWritableNetcdf4Image(
    std::string_view temp_filename) {
  TempFile path(temp_filename);
  {
    ScopedNcFile nc;
    if (const int status =
            nc_create(path.c_str(), NC_NETCDF4 | NC_CLOBBER, nc.ptr());
        status != NC_NOERR) {
      ADD_FAILURE() << "nc_create failed: " << status;
      return {};
    }
    int dimid = -1;
    if (const int status = nc_def_dim(nc.get(), "x", 3, &dimid);
        status != NC_NOERR) {
      ADD_FAILURE() << "nc_def_dim failed: " << status;
      return {};
    }
    int varid = -1;
    if (const int status = nc_def_var(nc.get(), "v", NC_INT, 1, &dimid, &varid);
        status != NC_NOERR) {
      ADD_FAILURE() << "nc_def_var failed: " << status;
      return {};
    }
    if (const int status = nc_enddef(nc.get()); status != NC_NOERR) {
      ADD_FAILURE() << "nc_enddef failed: " << status;
      return {};
    }
    constexpr std::array<int, 3> kVals = {10, 20, 30};
    if (const int status = nc_put_var_int(nc.get(), varid, kVals.data());
        status != NC_NOERR) {
      ADD_FAILURE() << "nc_put_var_int failed: " << status;
      return {};
    }
    if (const int status = nc.close(); status != NC_NOERR) {
      ADD_FAILURE() << "nc_close failed: " << status;
      return {};
    }
  }

  std::ifstream input(path.c_str(), std::ios::binary | std::ios::ate);
  if (!input.is_open()) {
    ADD_FAILURE() << "Failed to open serialized seed file";
    return {};
  }
  const std::streamoff end_pos = input.tellg();
  if (end_pos <= 0) {
    ADD_FAILURE() << "Serialized seed file is empty";
    return {};
  }
  std::vector<unsigned char> bytes(static_cast<std::size_t>(end_pos));
  input.seekg(0, std::ios::beg);
  if (!input.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()))) {
    ADD_FAILURE() << "Failed to read serialized seed bytes";
    return {};
  }
  return bytes;
}

TEST(Hdf5FileTest, SetFillRedefEnddefAndSyncStateTransitionsAndErrors) {
  ScopedMemio memio;
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("file_state.nc", NC_NETCDF4, 4096, file.ptr()));

    // Bad group ID bits exercise nc4_find_grp_h5 failure inside each function.
    const int bad_grp_id = file | 0x7fff;
    int old_fill = -1;
    EXPECT_EQ(NC_EBADID, nc_set_fill(bad_grp_id, NC_NOFILL, &old_fill));
    EXPECT_EQ(NC_EBADID, nc_redef(bad_grp_id));
    EXPECT_EQ(NC_EBADID, nc__enddef(bad_grp_id, 0, 0, 0, 0));
    EXPECT_EQ(NC_EBADID, nc_sync(bad_grp_id));

    // Set fill validations and old_modep NULL / non-NULL branches.
    EXPECT_EQ(NC_EINVAL, nc_set_fill(file, 999, &old_fill));
    ASSERT_EQ(NC_NOERR, nc_set_fill(file, NC_NOFILL, &old_fill));
    EXPECT_EQ(old_fill, NC_FILL);
    ASSERT_EQ(NC_NOERR, nc_set_fill(file, NC_FILL, nullptr));

    // Already in define mode on standard NC_NETCDF4 succeeds cleanly.
    EXPECT_EQ(NC_NOERR, nc_redef(file));

    int dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "x", 3, &dimid));
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "v", NC_INT, 1, &dimid, &varid));

    // Explicit nc__enddef with non-zero alignment hints (ignored by NetCDF-4)
    // followed by repeated nc_enddef triggering NC_ENOTINDEFINE.
    ASSERT_EQ(NC_NOERR, nc__enddef(file, 1024, 4, 2048, 8));
    EXPECT_EQ(NC_ENOTINDEFINE, nc_enddef(file));

    // Data mode nc_sync succeeds.
    ASSERT_EQ(NC_NOERR, nc_sync(file));

    // Reenter define mode and verify nc_sync automatically leaves define mode.
    ASSERT_EQ(NC_NOERR, nc_redef(file));
    int dim2 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "y", 2, &dim2));
    ASSERT_EQ(NC_NOERR, nc_sync(file));
    EXPECT_EQ(NC_ENOTINDEFINE, nc_enddef(file));

    ASSERT_EQ(NC_NOERR, nc_close_memio(file.release(), memio.get()));
  }

  // Reopen read-only and verify permission checks on set_fill, redef, and sync.
  ScopedNcFile ro;
  ASSERT_EQ(NC_NOERR,
            nc_open_mem("file_state.nc", NC_NOWRITE, memio.size(),
                        const_cast<unsigned char*>(memio.bytes()), ro.ptr()));
  int old_fill = -1;
  EXPECT_EQ(NC_EPERM, nc_set_fill(ro, NC_NOFILL, &old_fill));
  EXPECT_EQ(NC_EPERM, nc_redef(ro));
  // Sync on read-only skips metadata writes (h5->no_write) and flushes.
  EXPECT_EQ(NC_NOERR, nc_sync(ro));

  // Classic model stays strict on redef and sync while in define mode.
  ScopedNcFile classic;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("file_classic_state.nc",
                          NC_NETCDF4 | NC_CLASSIC_MODEL, 2048, classic.ptr()));
  EXPECT_EQ(NC_EINDEFINE, nc_redef(classic));
  EXPECT_EQ(NC_EINDEFINE, nc_sync(classic));
  ASSERT_EQ(NC_NOERR, nc_enddef(classic));
  EXPECT_EQ(NC_NOERR, nc_redef(classic));
}

TEST(Hdf5FileTest, DetectPreserveDimidsCoversAllCoordinateEdgeCases) {
  // Case 1: Clean ordered coord vars in root + subgroup (initial enddef sets
  // is_new_var=false; subsequent redef+sync evaluates all false branches and
  // recurses into clean child groups).
  {
    TempFile clean_nc("preserve_dimids_clean.nc");
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create(clean_nc.c_str(), NC_NETCDF4 | NC_CLOBBER, file.ptr()));
    int d0 = -1;
    int d1 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d0", 2, &d0));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d1", 3, &d1));
    int c0 = -1;
    int c1 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "d0", NC_INT, 1, &d0, &c0));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "d1", NC_INT, 1, &d1, &c1));

    int subgrp = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(file, "sub", &subgrp));
    int sd0 = -1;
    int sc0 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(subgrp, "sd0", 2, &sd0));
    ASSERT_EQ(NC_NOERR, nc_def_var(subgrp, "sd0", NC_INT, 1, &sd0, &sc0));
    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    // Reenter define mode and add only a global attribute so is_new_var and
    // became_coord_var remain false across root and subgroup.
    ASSERT_EQ(NC_NOERR, nc_redef(file));
    const int kTag = 42;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(file, NC_GLOBAL, "tag", NC_INT, 1, &kTag));
    ASSERT_EQ(NC_NOERR, nc_enddef(file));
    ASSERT_EQ(NC_NOERR, file.close());
  }

  // Case 2: Out-of-order 1D coordinate variables preserve dimids on reopen.
  {
    ScopedMemio memio;
    {
      ScopedNcFile file;
      ASSERT_EQ(NC_NOERR,
                nc_create_mem("preserve_ooo.nc", NC_NETCDF4, 4096, file.ptr()));
      int d0 = -1;
      int d1 = -1;
      ASSERT_EQ(NC_NOERR, nc_def_dim(file, "lat", 2, &d0));
      ASSERT_EQ(NC_NOERR, nc_def_dim(file, "lon", 3, &d1));
      int lon_v = -1;
      int lat_v = -1;
      // Define lon coord var before lat coord var -> dimids[0] decreases.
      ASSERT_EQ(NC_NOERR, nc_def_var(file, "lon", NC_INT, 1, &d1, &lon_v));
      ASSERT_EQ(NC_NOERR, nc_def_var(file, "lat", NC_INT, 1, &d0, &lat_v));
      ASSERT_EQ(NC_NOERR, nc_enddef(file));
      ASSERT_EQ(NC_NOERR, nc_close_memio(file.release(), memio.get()));
    }
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR,
              nc_open_mem("preserve_ooo.nc", NC_NOWRITE, memio.size(),
                          const_cast<unsigned char*>(memio.bytes()), ro.ptr()));
    char dim0_name[NC_MAX_NAME + 1] = {};
    char dim1_name[NC_MAX_NAME + 1] = {};
    ASSERT_EQ(NC_NOERR, nc_inq_dimname(ro, 0, dim0_name));
    ASSERT_EQ(NC_NOERR, nc_inq_dimname(ro, 1, dim1_name));
    EXPECT_STREQ(dim0_name, "lat");
    EXPECT_STREQ(dim1_name, "lon");
  }

  // Case 3: Multidimensional coordinate variable inside a subgroup triggers
  // child-group recursion and preserves dimids.
  {
    ScopedMemio memio;
    {
      ScopedNcFile file;
      ASSERT_EQ(NC_NOERR, nc_create_mem("preserve_multidim.nc", NC_NETCDF4,
                                        4096, file.ptr()));
      int grp = -1;
      ASSERT_EQ(NC_NOERR, nc_def_grp(file, "nested", &grp));
      int dx = -1;
      int dy = -1;
      ASSERT_EQ(NC_NOERR, nc_def_dim(grp, "x", 2, &dx));
      ASSERT_EQ(NC_NOERR, nc_def_dim(grp, "y", 3, &dy));
      const int dims[2] = {dx, dy};
      int cv = -1;
      ASSERT_EQ(NC_NOERR, nc_def_var(grp, "x", NC_INT, 2, dims, &cv));
      ASSERT_EQ(NC_NOERR, nc_enddef(file));
      ASSERT_EQ(NC_NOERR, nc_close_memio(file.release(), memio.get()));
    }
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR,
              nc_open_mem("preserve_multidim.nc", NC_NOWRITE, memio.size(),
                          const_cast<unsigned char*>(memio.bytes()), ro.ptr()));
    int grp = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_ncid(ro, "nested", &grp));
    int ndims = 0;
    int dimids[2] = {-1, -1};
    ASSERT_EQ(NC_NOERR, nc_inq_dimids(grp, &ndims, dimids, 0));
    EXPECT_EQ(ndims, 2);
  }

  // Case 4: Renamed variable becomes a coordinate variable after enddef+redef
  // (var->is_new_var == false && var->became_coord_var == true).
  {
    TempFile renamed_coord_nc("preserve_became_coord.nc");
    {
      ScopedNcFile file;
      ASSERT_EQ(NC_NOERR, nc_create(renamed_coord_nc.c_str(),
                                    NC_NETCDF4 | NC_CLOBBER, file.ptr()));
      int d0 = -1;
      ASSERT_EQ(NC_NOERR, nc_def_dim(file, "time", 2, &d0));
      int v0 = -1;
      ASSERT_EQ(NC_NOERR, nc_def_var(file, "t_raw", NC_INT, 1, &d0, &v0));
      ASSERT_EQ(NC_NOERR, nc_enddef(file));

      ASSERT_EQ(NC_NOERR, nc_redef(file));
      ASSERT_EQ(NC_NOERR, nc_rename_var(file, v0, "time"));
      ASSERT_EQ(NC_NOERR, nc_enddef(file));
      ASSERT_EQ(NC_NOERR, file.close());
    }
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR,
              nc_open(renamed_coord_nc.c_str(), NC_NOWRITE, ro.ptr()));
    int vid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "time", &vid));
    EXPECT_GE(vid, 0);
  }
}

TEST(Hdf5FileTest, AbortDeletesInitialCreateRestoresRedefAndRejectsMissing) {
  // Invalid group ID bits. Call NC4_abort() directly because nc_abort()
  // unconditionally frees the root NC struct in libdispatch/dfile.c even when
  // NC4_abort() returns NC_EBADID before freeing hdf5/nc4 metadata.
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("abort_badid.nc", NC_NETCDF4, 2048, file.ptr()));
    EXPECT_EQ(NC_EBADID, NC4_abort(file | 0x7fff));
    EXPECT_EQ(NC_EBADID, nc_abort(-1));
  }

  // Initial define mode abort removes on-disk file.
  {
    TempFile initial_nc("abort_initial_remove.nc");
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create(initial_nc.c_str(), NC_NETCDF4 | NC_CLOBBER,
                                  file.ptr()));
    EXPECT_TRUE(std::filesystem::exists(initial_nc.path()));
    int dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d", 2, &dimid));
    ASSERT_EQ(NC_NOERR, file.abort());
    EXPECT_FALSE(std::filesystem::exists(initial_nc.path()));
  }

  // Redef mode abort preserves previously committed file and rolls back
  // uncommitted redef definitions.
  {
    TempFile redef_nc("abort_redef_preserve.nc");
    {
      ScopedNcFile file;
      ASSERT_EQ(NC_NOERR, nc_create(redef_nc.c_str(), NC_NETCDF4 | NC_CLOBBER,
                                    file.ptr()));
      int dimid = -1;
      int kept_var = -1;
      ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d", 2, &dimid));
      ASSERT_EQ(NC_NOERR,
                nc_def_var(file, "kept", NC_INT, 1, &dimid, &kept_var));
      ASSERT_EQ(NC_NOERR, nc_enddef(file));
      const int kVals[2] = {101, 202};
      ASSERT_EQ(NC_NOERR, nc_put_var_int(file, kept_var, kVals));
      ASSERT_EQ(NC_NOERR, nc_sync(file));

      ASSERT_EQ(NC_NOERR, nc_redef(file));
      int dropped_var = -1;
      ASSERT_EQ(NC_NOERR,
                nc_def_var(file, "dropped", NC_INT, 1, &dimid, &dropped_var));
      ASSERT_EQ(NC_NOERR, file.abort());
    }

    EXPECT_TRUE(std::filesystem::exists(redef_nc.path()));
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(redef_nc.c_str(), NC_NOWRITE, ro.ptr()));
    int kept_id = -1;
    int dropped_id = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "kept", &kept_id));
    EXPECT_EQ(NC_ENOTVAR, nc_inq_varid(ro, "dropped", &dropped_id));
    int read_vals[2] = {};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, kept_id, read_vals));
    EXPECT_THAT(read_vals, ElementsAre(101, 202));

    // Aborting a read-only open file also succeeds cleanly.
    ASSERT_EQ(NC_NOERR, ro.abort());
  }

  // Aborting an in-memory file after enddef + redef exercises the abort=1
  // inmemory cleanup path without invoking remove().
  {
    ScopedNcFile mem_file;
    ASSERT_EQ(NC_NOERR, nc_create_mem("abort_inmem_redef.nc", NC_NETCDF4, 4096,
                                      mem_file.ptr()));
    int dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(mem_file, "x", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_enddef(mem_file));
    ASSERT_EQ(NC_NOERR, nc_redef(mem_file));
    ASSERT_EQ(NC_NOERR, mem_file.abort());
  }

  // Aborting an ephemeral NC_DISKLESS create still in initial define mode tries
  // to remove a path that never existed on disk, yielding NC_ECANTREMOVE.
  {
    TempDir dir("d");
    const std::string phantom = dir.child("x.nc").native();
    ScopedNcFile diskless;
    ASSERT_EQ(NC_NOERR, nc_create(phantom.c_str(), NC_NETCDF4 | NC_DISKLESS,
                                  diskless.ptr()));
    EXPECT_EQ(NC_ECANTREMOVE, diskless.abort());
  }
}

TEST(Hdf5FileTest, CloseHandlesErrorsLockedAndUnlockedMemioAndInq) {
  ScopedMemio memio;
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("close_and_inq.nc", NC_NETCDF4, 4096, file.ptr()));

    // Empty root inquiry before defining any dims/vars/atts, plus all-NULL.
    int ndims = -1;
    int nvars = -1;
    int natts = -1;
    int unlim = 99;
    ASSERT_EQ(NC_NOERR, nc_inq(file, nullptr, nullptr, nullptr, nullptr));
    ASSERT_EQ(NC_NOERR, nc_inq(file, &ndims, &nvars, &natts, &unlim));
    EXPECT_EQ(ndims, 0);
    EXPECT_EQ(nvars, 0);
    EXPECT_EQ(natts, 0);
    EXPECT_EQ(unlim, -1);

    // Invalid group ID bits & closing a non-root subgroup.
    int subgrp = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(file, "sub", &subgrp));
    EXPECT_EQ(NC_EBADID, nc_close(file | 0x7fff));
    EXPECT_EQ(NC_EBADGRPID, nc_close(subgrp));
    EXPECT_EQ(NC_EBADID,
              nc_inq(file | 0x7fff, &ndims, nullptr, nullptr, nullptr));

    // Fixed dimension first, unlimited dimension second -> exercises loop
    // skipping fixed dim before locating unlimited dim.
    int d_fixed = -1;
    int d_unlim = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "fixed", 4, &d_fixed));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "unlim", NC_UNLIMITED, &d_unlim));
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "arr", NC_INT, 1, &d_fixed, &varid));
    const int kAttr = 7;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(file, NC_GLOBAL, "gattr", NC_INT, 1, &kAttr));

    ASSERT_EQ(NC_NOERR, nc_inq(file, &ndims, &nvars, &natts, &unlim));
    EXPECT_EQ(ndims, 2);
    EXPECT_EQ(nvars, 1);
    EXPECT_EQ(natts, 1);
    EXPECT_EQ(unlim, d_unlim);

    ASSERT_EQ(NC_NOERR, nc_close_memio(file.release(), memio.get()));
  }

  // Closing an nc_create_mem dataset via nc_close() (memio == NULL) frees
  // internal memory cleanly without leaks.
  {
    ScopedNcFile disc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("close_discard.nc", NC_NETCDF4, 2048, disc.ptr()));
    ASSERT_EQ(NC_NOERR, disc.close());
  }

  // Reopen with NC_MEMIO_LOCKED read-only: calling nc_inq immediately checks
  // lazy grp->atts_read == 0 attribute loading; nc_close preserves caller
  // memory.
  {
    NC_memio locked{};
    locked.memory = const_cast<unsigned char*>(memio.bytes());
    locked.size = memio.size();
    locked.flags = NC_MEMIO_LOCKED;
    ScopedNcFile ro_locked;
    ASSERT_EQ(NC_NOERR, nc_open_memio("locked_inq.nc", NC_NOWRITE, &locked,
                                      ro_locked.ptr()));
    int ndims = 0;
    int nvars = 0;
    int natts = 0;
    int unlim = -1;
    ASSERT_EQ(NC_NOERR, nc_inq(ro_locked, &ndims, &nvars, &natts, &unlim));
    EXPECT_EQ(ndims, 2);
    EXPECT_EQ(nvars, 1);
    EXPECT_EQ(natts, 1);
    EXPECT_EQ(unlim, 1);
    ASSERT_EQ(NC_NOERR, ro_locked.close());
  }

  // Reopen with unlocked NC_WRITE: seed from an on-disk NetCDF-4 image (so
  // creation-order tracking is enabled on the root group); netCDF takes
  // ownership of the malloc()'d buffer and frees it inside
  // nc4_close_netcdf4_file().
  {
    const std::vector<unsigned char> writable =
        BuildWritableNetcdf4Image("hdf5file_seed.nc");
    ASSERT_FALSE(writable.empty());

    void* owned_buf = std::malloc(writable.size());
    ASSERT_NE(owned_buf, nullptr);
    std::memcpy(owned_buf, writable.data(), writable.size());

    NC_memio unlocked{};
    unlocked.memory = owned_buf;
    unlocked.size = writable.size();
    unlocked.flags = 0;
    auto free_unclaimed =
        absl::MakeCleanup([&unlocked]() { std::free(unlocked.memory); });

    ScopedNcFile rw_unlocked;
    ASSERT_EQ(NC_NOERR, nc_open_memio("unlocked_rw.nc", NC_WRITE, &unlocked,
                                      rw_unlocked.ptr()));
    // Ownership transferred on open; nc4_close_netcdf4_file() frees the buffer.
    EXPECT_EQ(unlocked.memory, nullptr);
    EXPECT_EQ(unlocked.size, 0U);

    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(rw_unlocked.get(), "v", &varid));
    std::array<int, 3> vals = {};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(rw_unlocked.get(), varid, vals.data()));
    EXPECT_THAT(vals, ElementsAre(10, 20, 30));

    constexpr std::array<int, 3> kUpdated = {40, 50, 60};
    ASSERT_EQ(NC_NOERR,
              nc_put_var_int(rw_unlocked.get(), varid, kUpdated.data()));
    ASSERT_EQ(NC_NOERR, rw_unlocked.close());
  }
}

TEST(Hdf5FileTest, CloseNetcdf4FileReportsOpenHdf5ObjectsAndInvalidHandle) {
  TempDir dir("dump_open_objs");
  const std::string diskless_path = dir.child("open_objs.nc").native();

  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create(diskless_path.c_str(), NC_NETCDF4 | NC_DISKLESS,
                                file.ptr()));
  ASSERT_EQ(NC_NOERR, nc_enddef(file));

  NC_FILE_INFO_T* h5 = nullptr;
  ASSERT_EQ(NC_NOERR, nc4_find_grp_h5(file, nullptr, &h5));
  ASSERT_NE(h5, nullptr);
  ASSERT_NE(h5->format_file_info, nullptr);

  auto* hdf5_info = static_cast<NC_HDF5_FILE_INFO_T*>(h5->format_file_info);
  ASSERT_EQ(hdf5_info->uri, nullptr);
  const hid_t orig_hdfid = hdf5_info->hdfid;
  ASSERT_GT(orig_hdfid, 0);

  // Create an isolated in-memory HDF5 file with H5F_CLOSE_SEMI and hold a root
  // group handle open so H5Fclose() refuses to close and triggers
  // dumpopenobjects() with nobjs > 0.
  hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
  ASSERT_GE(fapl, 0);
  absl::Cleanup close_fapl = [&]() { H5Pclose(fapl); };
  ASSERT_GE(H5Pset_fapl_core(fapl, 65536, 0), 0);
  ASSERT_GE(H5Pset_fclose_degree(fapl, H5F_CLOSE_SEMI), 0);

  hid_t semi_fid = H5Fcreate("hdf5file_dump_open_probe.h5", H5F_ACC_TRUNC,
                             H5P_DEFAULT, fapl);
  ASSERT_GT(semi_fid, 0);
  hid_t open_grp = H5Gopen2(semi_fid, "/", H5P_DEFAULT);
  ASSERT_GE(open_grp, 0);

  absl::Cleanup restore_and_close = [&]() {
    hdf5_info->hdfid = orig_hdfid;
    if (open_grp >= 0) {
      H5Gclose(open_grp);
      open_grp = -1;
    }
    if (semi_fid > 0) {
      H5Fclose(semi_fid);
      semi_fid = -1;
    }
  };

  hdf5_info->hdfid = semi_fid;
  EXPECT_EQ(NC_EHDFERR, nc4_close_netcdf4_file(h5, /*abort=*/1, nullptr));

  // Close the dangling group and semi file, then pass the closed positive hid_t
  // so H5Fclose() fails and dumpopenobjects() hits nobjs < 0.
  ASSERT_GE(H5Gclose(open_grp), 0);
  open_grp = -1;
  ASSERT_GE(H5Fclose(semi_fid), 0);
  const hid_t stale_fid = semi_fid;
  semi_fid = -1;

  hdf5_info->hdfid = stale_fid;
  EXPECT_EQ(NC_EHDFERR, nc4_close_netcdf4_file(h5, /*abort=*/1, nullptr));

  // Restore original HDF5 handle and close normally via ScopedNcFile.
  hdf5_info->hdfid = orig_hdfid;
  std::move(restore_and_close).Invoke();
  ASSERT_EQ(NC_NOERR, file.close());
}

}  // namespace
}  // namespace netcdf
