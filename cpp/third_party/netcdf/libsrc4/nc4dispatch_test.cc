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

// Unit tests for //third_party/netcdf/libsrc4/nc4dispatch.c.
//
// Exercises compiled-in NC4_initialize() (reserved attribute setup) and
// NC4_finalize(), plus full library nc_initialize()/nc_finalize() lifecycle
// cycles isolated inside an execve child process.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libsrc4/nc4dispatch.c

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/libsrc4/nc4dispatch_shim.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

namespace netcdf {
namespace {

TEST(Nc4DispatchTest, InitializeAndFinalizeAreIdempotentAndInitReservedAttrs) {
  // Repeat calls must succeed cleanly and keep the sorted reserved-attribute
  // table intact without perturbing sibling HDF5/NCList state.
  EXPECT_EQ(NC_NOERR, NetcdfTestNc4Initialize());
  EXPECT_EQ(NC_NOERR, NetcdfTestNc4Initialize());

  // All entries registered in NC_reserved[] (libsrc4/nc4internal.c), including
  // entries out of order in the static initializer that require qsort().
  constexpr const char* kReservedAttrs[] = {
      "CLASS",
      "DIMENSION_LIST",
      "NAME",
      "REFERENCE_LIST",
      "_ARRAY_DIMENSIONS",
      "_Codecs",
      "_Format",
      "_IsNetcdf4",
      "_NCProperties",
      "_Netcdf4Coordinates",
      "_Netcdf4Dimid",
      "_SuperblockVersion",
      "_nc3_strict",
      "_nczarr_array",
      "_nczarr_attr",
      "_nczarr_group",
      "_nczarr_superblock",
  };
  for (const char* attr : kReservedAttrs) {
    EXPECT_EQ(1, NetcdfTestHasReservedAttr(attr)) << attr;
  }

  // NC_FillValue is handled separately by libhdf5 and is not in NC_reserved[].
  EXPECT_EQ(0, NetcdfTestHasReservedAttr(NC_FillValue));
  EXPECT_EQ(0, NetcdfTestHasReservedAttr(""));
  EXPECT_EQ(0, NetcdfTestHasReservedAttr("definitely_not_a_reserved_attr"));

  EXPECT_EQ(NC_NOERR, NetcdfTestNc4Finalize());
  EXPECT_EQ(NC_NOERR, NetcdfTestNc4Finalize());
  EXPECT_EQ(NC_NOERR, NetcdfTestNc4Initialize());
  EXPECT_EQ(1, NetcdfTestHasReservedAttr("_NCProperties"));
  EXPECT_EQ(1, NetcdfTestHasReservedAttr("_nczarr_array"));
  EXPECT_EQ(0, NetcdfTestHasReservedAttr(NC_FillValue));
}

TEST(Nc4DispatchTest, FullLibraryInitializeFinalizeRoundTripsInIsolatedChild) {
  RunInExecveChild([]() -> bool {
    if (nc_initialize() != NC_NOERR) {
      return false;
    }
    if (NetcdfTestHasReservedAttr("_NCProperties") != 1 ||
        NetcdfTestHasReservedAttr(NC_FillValue) != 0) {
      return false;
    }

    ScopedNcFile file;
    if (nc_create_mem("nc4dispatch_child.nc", NC_NETCDF4, 0, file.ptr()) !=
        NC_NOERR) {
      return false;
    }
    ScopedMemio memio;
    if (nc_close_memio(file.get(), memio.get()) != NC_NOERR) {
      return false;
    }
    file.release();
    if (memio.bytes() == nullptr || memio.size() == 0) {
      return false;
    }
    memio.reset();

    if (nc_finalize() != NC_NOERR) {
      return false;
    }
    // Re-initializing after finalize invokes NC4_initialize() again cleanly.
    if (nc_initialize() != NC_NOERR) {
      return false;
    }
    if (NetcdfTestHasReservedAttr("_Netcdf4Dimid") != 1) {
      return false;
    }
    return nc_finalize() == NC_NOERR;
  });
}

}  // namespace
}  // namespace netcdf
