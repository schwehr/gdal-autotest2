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

// Clean-room unit tests for //third_party/netcdf/libhdf5/hdf5create.c.
//
// Covers NC4_create(), nc4_create_file(), and nc4_H5Fcreate() across disk,
// diskless, persist, in-memory, clobber/noclobber, option flags, global FAPL
// alignment/meta_block_size/chunkcache hooks, and failure cleanup paths.

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <fstream>
#include <optional>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/libhdf5/hdf5create_shim.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

namespace netcdf {
namespace {

TEST(Hdf5CreateTest, InitializesHdf5AndAppliesAlignmentAndMetaBlockInChild) {
  RunInExecveChild([]() -> bool {
    // First NetCDF-4 creation in this fresh execve child triggers
    // !nc4_hdf5_initialized -> nc4_hdf5_initialize() in NC4_create().
    int ncid = -1;
    if (nc_create("child_init.nc", NC_NETCDF4 | NC_DISKLESS, &ncid) !=
        NC_NOERR) {
      return false;
    }
    if (nc_close(ncid) != NC_NOERR) {
      return false;
    }

    // Configure global HDF5 alignment and meta block size before creation.
    if (nc_set_alignment(4096, 4096) != NC_NOERR) {
      return false;
    }
    if (nc_set_meta_block_size(8192) != NC_NOERR) {
      return false;
    }

    int threshold = -1;
    int alignment = -1;
    if (nc_get_alignment(&threshold, &alignment) != NC_NOERR ||
        threshold != 4096 || alignment != 4096) {
      return false;
    }
    size_t meta_block = 0;
    if (nc_get_meta_block_size(&meta_block) != NC_NOERR || meta_block != 8192) {
      return false;
    }

    ncid = -1;
    if (nc_create("child_aligned.nc", NC_NETCDF4 | NC_DISKLESS, &ncid) !=
        NC_NOERR) {
      return false;
    }
    int dimid = -1;
    int varid = -1;
    const int val = 42;
    if (nc_def_dim(ncid, "d", 1, &dimid) != NC_NOERR ||
        nc_def_var(ncid, "v", NC_INT, 1, &dimid, &varid) != NC_NOERR ||
        nc_enddef(ncid) != NC_NOERR ||
        nc_put_var_int(ncid, varid, &val) != NC_NOERR ||
        nc_close(ncid) != NC_NOERR) {
      return false;
    }

    // Alignment < 1 fails H5Pset_alignment() inside nc4_create_file().
    if (nc_set_meta_block_size(0) != NC_NOERR ||
        nc_set_alignment(0, 0) != NC_NOERR) {
      return false;
    }
    ncid = -1;
    return nc_create("child_bad_align.nc", NC_NETCDF4 | NC_DISKLESS, &ncid) ==
           NC_EHDFERR;
  });
}

TEST(Hdf5CreateTest, CreatesNormalAndClobberNoClobberDiskFiles) {
  TempFile file("create_basic.nc");
  ASSERT_FALSE(std::filesystem::exists(file.path()));

  // NC_NOCLOBBER succeeds when destination does not yet exist.
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create(file.c_str(), NC_NETCDF4 | NC_NOCLOBBER, nc.ptr()));
    int format = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_format(nc, &format));
    EXPECT_EQ(NC_FORMAT_NETCDF4, format);
    const int attr = 7;
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, NC_GLOBAL, "g", NC_INT, 1, &attr));
    ASSERT_EQ(NC_NOERR, nc.close());
  }
  EXPECT_TRUE(std::filesystem::exists(file.path()));

  // NC_NOCLOBBER fails with NC_EEXIST when file already exists on disk.
  {
    int dup_ncid = -1;
    EXPECT_EQ(NC_EEXIST,
              nc_create(file.c_str(), NC_NETCDF4 | NC_NOCLOBBER, &dup_ncid));
  }

  // NC_CLOBBER replaces the existing file cleanly.
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create(file.c_str(), NC_NETCDF4 | NC_CLOBBER, nc.ptr()));
    int natts = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_natts(nc, &natts));
    EXPECT_EQ(0, natts);
  }
}

TEST(Hdf5CreateTest, DisklessNonPersistAndPersistAndInitialSizeIncrements) {
  // Small initialsz (<= 655360) selects min_incr=65536 and does not persist.
  TempFile ephemeral("ephemeral_diskless.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc__create(ephemeral.c_str(), NC_NETCDF4 | NC_DISKLESS,
                                   1024, nullptr, nc.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v", NC_INT, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc.close());
  }
  EXPECT_FALSE(std::filesystem::exists(ephemeral.path()));

  // Large initialsz (> 655360) triggers fractional 10% core alloc increment,
  // and NC_DISKLESS | NC_NOCLOBBER ignores an existing file on disk.
  TempFile existing("existing_ignored_by_diskless.nc");
  {
    std::ofstream ofs(existing.path(), std::ios::binary);
    ofs << "placeholder";
  }
  ASSERT_TRUE(std::filesystem::exists(existing.path()));
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc__create(existing.c_str(),
                                   NC_NETCDF4 | NC_DISKLESS | NC_NOCLOBBER,
                                   1024 * 1024, nullptr, nc.ptr()));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  // NC_DISKLESS | NC_PERSIST writes the core HDF5 image to disk upon close.
  TempFile persisted("persisted_diskless.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc__create(persisted.c_str(),
                                   NC_NETCDF4 | NC_DISKLESS | NC_PERSIST, 2048,
                                   nullptr, nc.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 3, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "vals", NC_INT, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));
    const int vals[3] = {10, 20, 30};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, varid, vals));
    ASSERT_EQ(NC_NOERR, nc.close());
  }
  EXPECT_TRUE(std::filesystem::exists(persisted.path()));

  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(persisted.c_str(), NC_NOWRITE, ro.ptr()));
    int vals_in[3] = {0, 0, 0};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, 0, vals_in));
    EXPECT_EQ(10, vals_in[0]);
    EXPECT_EQ(20, vals_in[1]);
    EXPECT_EQ(30, vals_in[2]);
  }
}

TEST(Hdf5CreateTest, SupportsInMemoryAndCustomChunkCacheAndOptionalFlags) {
  ScopedChunkCache cache_guard;
  ASSERT_EQ(NC_NOERR, nc_set_chunk_cache(1024 * 1024, 1009, 0.5F));

  // Create in-memory dataset with NC_NOATTCREORD and NC_NODIMSCALE_ATTACH.
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("inmem_flags.nc",
                          NC_NETCDF4 | NC_NOATTCREORD | NC_NODIMSCALE_ATTACH,
                          4096, nc.ptr()));
  int dimid = -1;
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "n", 2, &dimid));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v", NC_DOUBLE, 1, &dimid, &varid));
  const double attr_val = 3.25;
  ASSERT_EQ(NC_NOERR,
            nc_put_att_double(nc, varid, "scale", NC_DOUBLE, 1, &attr_val));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));
  const double data[2] = {1.5, 2.5};
  ASSERT_EQ(NC_NOERR, nc_put_var_double(nc, varid, data));

  NC_memio raw_mem{};
  ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), &raw_mem));
  ScopedMemio mem(raw_mem);
  EXPECT_GT(mem.size(), 0U);

  ScopedNcFile ro;
  ASSERT_EQ(NC_NOERR, nc_open_mem("inmem_flags.nc", NC_NOWRITE, mem.size(),
                                  mem.get()->memory, ro.ptr()));
  double data_in[2] = {0.0, 0.0};
  ASSERT_EQ(NC_NOERR, nc_get_var_double(ro, 0, data_in));
  EXPECT_DOUBLE_EQ(1.5, data_in[0]);
  EXPECT_DOUBLE_EQ(2.5, data_in[1]);
  double attr_in = 0.0;
  ASSERT_EQ(NC_NOERR, nc_get_att_double(ro, 0, "scale", &attr_in));
  EXPECT_DOUBLE_EQ(3.25, attr_in);
}

TEST(Hdf5CreateTest, RejectsIllegalModesAndUnwritableCreatePaths) {
  // Missing parent directory triggers nc4_H5Fcreate() < 0 -> BAIL(EACCES).
  TempDir dir("create_fail_dir");
  const std::filesystem::path bad_path = dir.child("nonexistent/file.nc");
  int ncid = -1;
  EXPECT_EQ(EACCES, nc_create(bad_path.c_str(), NC_NETCDF4, &ncid));

  // Direct NC4_create() checks for ILLEGAL_CREATE_FLAGS.
  EXPECT_EQ(NC_EINVAL,
            Hdf5CreateWithIllegalCmode("illegal.nc", NC_NETCDF4 | NC_MMAP));
  EXPECT_EQ(NC_EINVAL, Hdf5CreateWithIllegalCmode(
                           "illegal.nc", NC_NETCDF4 | NC_64BIT_OFFSET));
  EXPECT_EQ(NC_EINVAL,
            Hdf5CreateWithIllegalCmode("illegal.nc", NC_NETCDF4 | NC_CDF5));

  // Mutual exclusion in nc4_create_file(): NC_INMEMORY | NC_DISKLESS.
  EXPECT_EQ(NC_EINTERNAL,
            Hdf5CreateBypassDispatch(
                "both.nc", NC_NETCDF4 | NC_INMEMORY | NC_DISKLESS, 0, 0));

  // Non-NULL NC_memio parameters on NC4_create().
  EXPECT_EQ(NC_NOERR, Hdf5CreateBypassDispatch("memio_param.nc",
                                               NC_NETCDF4 | NC_INMEMORY, 0, 1));
}

class ScopedFailRootVol {
 public:
  ScopedFailRootVol() = default;
  ScopedFailRootVol(const ScopedFailRootVol&) = delete;
  ScopedFailRootVol& operator=(const ScopedFailRootVol&) = delete;

  bool Init() {
    const char* prev = std::getenv("HDF5_VOL_CONNECTOR");
    if (prev != nullptr) {
      saved_env_ = prev;
    }
    vol_id_ = Hdf5CreateRegisterFailRootVol();
    if (vol_id_ < 0) {
      return false;
    }
    if (::setenv("HDF5_VOL_CONNECTOR",
                 "hdf5create_fail_root_vol under_vol=0;under_info={}",
                 1) != 0) {
      Cleanup();
      return false;
    }
    env_modified_ = true;
    if (Hdf5CreateReparseDefaultVol() != NC_NOERR) {
      Cleanup();
      return false;
    }
    return true;
  }

  ~ScopedFailRootVol() { Cleanup(); }

 private:
  void Cleanup() {
    if (env_modified_) {
      if (saved_env_.has_value()) {
        ::setenv("HDF5_VOL_CONNECTOR", saved_env_->c_str(), 1);
      } else {
        ::unsetenv("HDF5_VOL_CONNECTOR");
      }
      env_modified_ = false;
    }
    if (vol_id_ >= 0) {
      Hdf5CreateCleanupFailRootVol(vol_id_);
      vol_id_ = -1;
    }
  }

  long long vol_id_ = -1;
  bool env_modified_ = false;
  std::optional<std::string> saved_env_;
};

TEST(Hdf5CreateTest, RejectsInvalidMemioAndChunkCachePreemption) {
  EXPECT_EQ(NC_EHDFERR, Hdf5CreateWithNonNullZeroSizeMemio());
  EXPECT_EQ(NC_EHDFERR, Hdf5CreateWithInvalidChunkCachePreemption(
                            "bad_cache_preemption.nc"));

  TempDir dir("post_bad_preemption");
  int ncid = -1;
  ASSERT_EQ(NC_NOERR, nc_create(dir.child("ok.nc").c_str(), NC_NETCDF4, &ncid));
  ScopedNcFile file(ncid);
  EXPECT_EQ(NC_NOERR, file.close());
}

TEST(Hdf5CreateTest, ReinitializesHdf5OnColdDirectCreateInChild) {
  RunInExecveChild([]() -> bool {
    return Hdf5CreateColdInit("cold_init_create.nc") == NC_NOERR;
  });
}

TEST(Hdf5CreateTest, HandlesPathVolAndUnregisteredPropertyFailuresInChild) {
  RunInExecveChild([]() -> bool {
    if (Hdf5CreateWithUnconvertiblePath("/unconvertible/diskless_path.nc") !=
        EACCES) {
      return false;
    }
    {
      ScopedFailRootVol vol;
      if (!vol.Init()) {
        return false;
      }
      if (Hdf5CreateWithRootGroupOpenFailure("vol_fail_root.nc") !=
          NC_EFILEMETA) {
        return false;
      }
    }
    if (Hdf5CreateFailPlistProperty(kHdf5CreateFailUnregisterFaplCore) !=
        NC_EHDFERR) {
      return false;
    }
    if (Hdf5CreateFailPlistProperty(kHdf5CreateFailUnregisterLinkInfo) !=
        NC_EHDFERR) {
      return false;
    }
    if (Hdf5CreateFailPlistProperty(kHdf5CreateFailUnregisterObjTrackTimes) !=
        NC_EHDFERR) {
      return false;
    }
    if (Hdf5CreateFailPlistProperty(kHdf5CreateFailUnregisterLibverLowBound) !=
        NC_EHDFERR) {
      return false;
    }
    if (Hdf5CreateFailPlistProperty(kHdf5CreateFailUnregisterMetaBlockSize) !=
        NC_EHDFERR) {
      return false;
    }
    if (Hdf5CreateFailPlistProperty(kHdf5CreateFailUnregisterCloseDegree) !=
        NC_EHDFERR) {
      return false;
    }
    return true;
  });
}

TEST(Hdf5CreateTest, HandlesPropertyListCreationFailuresInChild) {
  RunInExecveChild([]() -> bool {
    if (Hdf5CreateFailPlistProperty(kHdf5CreateFailCreateFcpl) != NC_EHDFERR) {
      return false;
    }
    if (Hdf5CreateFailPlistProperty(kHdf5CreateFailCreateFapl) != NC_EHDFERR) {
      return false;
    }
    return true;
  });
}

}  // namespace
}  // namespace netcdf
