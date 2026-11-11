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

// Unit tests for //third_party/netcdf/libhdf5/hdf5set_format_compatibility.c.

#include <array>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <ios>

#include "testing/base/public/gunit.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on
#include "third_party/netcdf/google/libhdf5/hdf5set_format_compatibility_shim.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"

namespace netcdf {
namespace {

TEST(Hdf5SetFormatCompatibilityTest, ValidFileAccessPlistSetsV18Bounds) {
  int low_is_v18 = 0;
  int high_is_latest = 0;
  ASSERT_EQ(NC_NOERR, NetcdfTestSetFormatCompatibilityValidFapl(
                          &low_is_v18, &high_is_latest));
  EXPECT_EQ(low_is_v18, 1);
  EXPECT_EQ(high_is_latest, 1);

  // Calling again verifies idempotency and tolerates nullptr outputs.
  ASSERT_EQ(NC_NOERR,
            NetcdfTestSetFormatCompatibilityValidFapl(nullptr, nullptr));
}

TEST(Hdf5SetFormatCompatibilityTest, InvalidPropertyListsReturnHdfErr) {
  EXPECT_EQ(NC_EHDFERR, NetcdfTestSetFormatCompatibilityInvalidHid());
  EXPECT_EQ(NC_EHDFERR, NetcdfTestSetFormatCompatibilityDefaultPlist());
  EXPECT_EQ(NC_EHDFERR, NetcdfTestSetFormatCompatibilityWrongPlistClass());
}

TEST(Hdf5SetFormatCompatibilityTest,
     PosixCreateAndWriteReopenProduceSuperblockVersionTwo) {
  TempFile file("format_compat.nc");
  int dimid = -1;
  int varid = -1;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create(file.c_str(), NC_NETCDF4 | NC_CLOBBER, nc.ptr()));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 3, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "vals", NC_INT, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    constexpr std::array<int, 3> kInitial = {10, 20, 30};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, varid, kInitial.data()));

    int superblock = -1;
    ASSERT_EQ(NC_NOERR,
              nc_get_att_int(nc, NC_GLOBAL, "_SuperblockVersion", &superblock));
    EXPECT_EQ(superblock, 2);
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  // Reopen for writing (triggers hdf5open.c call to
  // hdf5set_format_compatibility) and extend metadata + data.
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_WRITE, nc.ptr()));
    ASSERT_EQ(NC_NOERR, nc_redef(nc));
    int varid2 = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_dimid(nc, "x", &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "extra", NC_DOUBLE, 1, &dimid, &varid2));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    constexpr std::array<double, 3> kExtra = {1.5, 2.5, 3.5};
    ASSERT_EQ(NC_NOERR, nc_put_var_double(nc, varid2, kExtra.data()));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  // Read-only verification preserves v2 superblock and all written data.
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, nc.ptr()));
    int superblock = -1;
    ASSERT_EQ(NC_NOERR,
              nc_get_att_int(nc, NC_GLOBAL, "_SuperblockVersion", &superblock));
    EXPECT_EQ(superblock, 2);

    std::array<double, 3> got_extra{};
    ASSERT_EQ(NC_NOERR, nc_inq_varid(nc, "extra", &varid));
    ASSERT_EQ(NC_NOERR, nc_get_var_double(nc, varid, got_extra.data()));
    EXPECT_EQ(got_extra, (std::array<double, 3>{1.5, 2.5, 3.5}));
  }
}

// Owns a malloc-allocated NC_memio input buffer until nc_open_memio takes
// ownership of it (clearing info.memory) or until scope exit.
struct OwnedMemioInput {
  NC_memio info{};

  OwnedMemioInput() = default;
  OwnedMemioInput(const OwnedMemioInput&) = delete;
  OwnedMemioInput& operator=(const OwnedMemioInput&) = delete;
  ~OwnedMemioInput() { std::free(info.memory); }
};

// Reads the entire contents of |path| into a freshly malloc'd OwnedMemioInput.
bool ReadFileToOwnedMemio(const char* path, OwnedMemioInput* out) {
  if (out == nullptr) {
    return false;
  }
  std::free(out->info.memory);
  out->info = NC_memio{};

  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in.is_open()) {
    return false;
  }
  const std::ifstream::pos_type end_pos = in.tellg();
  if (end_pos == std::ifstream::pos_type(-1)) {
    return false;
  }
  const std::streamoff signed_size = static_cast<std::streamoff>(end_pos);
  if (signed_size <= 0) {
    return false;
  }
  const std::size_t size = static_cast<std::size_t>(signed_size);
  const std::streamsize stream_size = static_cast<std::streamsize>(signed_size);
  if (!in.seekg(0, std::ios::beg)) {
    return false;
  }
  void* mem = std::malloc(size);
  if (mem == nullptr) {
    return false;
  }
  out->info.memory = mem;
  out->info.size = size;
  out->info.flags = 0;
  if (!in.read(static_cast<char*>(out->info.memory), stream_size)) {
    return false;
  }
  return in.gcount() == stream_size;
}

// Defines a 1-D NC_INT variable "v" and writes |values| into |ncid|.
void WriteInitialIntVar(int ncid, const std::array<int, 2>& values) {
  int dimid = -1;
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "n", values.size(), &dimid));
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v", NC_INT, 1, &dimid, &varid));
  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, varid, values.data()));
}

// Overwrites existing variable "v" in |ncid| with |values|.
void OverwriteIntVar(int ncid, const std::array<int, 2>& values) {
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "v", &varid));
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, varid, values.data()));
}

// Asserts that variable "v" in |ncid| equals |expected|.
void ExpectIntVar(int ncid, const std::array<int, 2>& expected) {
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "v", &varid));
  std::array<int, 2> read_back{};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(ncid, varid, read_back.data()));
  EXPECT_EQ(read_back, expected);
}

// Asserts that global attribute "_SuperblockVersion" equals |expected|.
void ExpectSuperblockVersion(int ncid, int expected) {
  int superblock = -1;
  ASSERT_EQ(NC_NOERR,
            nc_get_att_int(ncid, NC_GLOBAL, "_SuperblockVersion", &superblock));
  EXPECT_EQ(superblock, expected);
}

TEST(Hdf5SetFormatCompatibilityTest,
     DisklessAndInMemoryOpenForWritePreserveData) {
  constexpr std::array<int, 2> kInitialData = {7, 11};
  constexpr std::array<int, 2> kUpdatedData = {13, 17};

  {
    ScopedNcFile diskless;
    ASSERT_EQ(NC_NOERR, nc_create("compat_diskless.nc",
                                  NC_NETCDF4 | NC_DISKLESS, diskless.ptr()));
    ASSERT_NO_FATAL_FAILURE(ExpectSuperblockVersion(diskless, 2));
    ASSERT_NO_FATAL_FAILURE(WriteInitialIntVar(diskless, kInitialData));
    ASSERT_NO_FATAL_FAILURE(ExpectIntVar(diskless, kInitialData));
  }

  {
    TempFile persist_file("compat_diskless_persist.nc");
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR, nc_create(persist_file.c_str(),
                                    NC_NETCDF4 | NC_CLOBBER, nc.ptr()));
      ASSERT_NO_FATAL_FAILURE(WriteInitialIntVar(nc, kInitialData));
      ASSERT_EQ(NC_NOERR, nc.close());
    }
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR,
                nc_open(persist_file.c_str(),
                        NC_WRITE | NC_DISKLESS | NC_PERSIST, nc.ptr()));
      ASSERT_NO_FATAL_FAILURE(OverwriteIntVar(nc, kUpdatedData));
      ASSERT_EQ(NC_NOERR, nc.close());
    }
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR, nc_open(persist_file.c_str(),
                                  NC_NOWRITE | NC_DISKLESS, nc.ptr()));
      ASSERT_NO_FATAL_FAILURE(ExpectIntVar(nc, kUpdatedData));
      ASSERT_NO_FATAL_FAILURE(ExpectSuperblockVersion(nc, 2));
    }
  }

  {
    ScopedMemio created_mem;
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR, nc_create_mem("compat_mem_create.nc", NC_NETCDF4,
                                        4096, nc.ptr()));
      ASSERT_NO_FATAL_FAILURE(WriteInitialIntVar(nc, kInitialData));
      ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), created_mem.get()));
    }
    ScopedNcFile ro_nc;
    ASSERT_EQ(NC_NOERR, nc_open_mem("compat_mem_create_ro.nc", NC_NOWRITE,
                                    created_mem.size(),
                                    created_mem.get()->memory, ro_nc.ptr()));
    ASSERT_NO_FATAL_FAILURE(ExpectIntVar(ro_nc, kInitialData));
  }

  // Unlocked NC_WRITE nc_open_memio requires an HDF5 image created with tracked
  // link creation order (such as via nc_create) and takes ownership of the
  // malloc()'d input buffer.
  OwnedMemioInput rw_input;
  {
    TempFile seed_file("compat_rw_seed.nc");
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR, nc_create(seed_file.c_str(), NC_NETCDF4 | NC_CLOBBER,
                                    nc.ptr()));
      ASSERT_NO_FATAL_FAILURE(WriteInitialIntVar(nc, kInitialData));
      ASSERT_EQ(NC_NOERR, nc.close());
    }
    ASSERT_TRUE(ReadFileToOwnedMemio(seed_file.c_str(), &rw_input));
  }

  ScopedMemio updated_mem;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_open_memio("compat_mem_rw.nc", NC_WRITE | NC_NETCDF4,
                                      &rw_input.info, nc.ptr()));
    ASSERT_NO_FATAL_FAILURE(OverwriteIntVar(nc, kUpdatedData));
    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), updated_mem.get()));
  }

  ScopedNcFile ro_nc;
  ASSERT_EQ(NC_NOERR,
            nc_open_mem("compat_mem_ro.nc", NC_NOWRITE, updated_mem.size(),
                        updated_mem.get()->memory, ro_nc.ptr()));
  ASSERT_NO_FATAL_FAILURE(ExpectIntVar(ro_nc, kUpdatedData));
  ASSERT_NO_FATAL_FAILURE(ExpectSuperblockVersion(ro_nc, 2));
}

}  // namespace
}  // namespace netcdf
