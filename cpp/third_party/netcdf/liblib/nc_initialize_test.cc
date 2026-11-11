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

// Unit tests for //third_party/netcdf/liblib/nc_initialize.c.
//
// Uses isolated child processes so global library teardown/re-initialization
// cycles never affect sibling tests or Testify repeat iterations.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/liblib/nc_initialize.c

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/isolated_child.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

extern "C" {
int nc_initialize(void);
int nc_finalize(void);
}  // extern "C"

namespace netcdf {
namespace {

TEST(NcInitializeTest, InitializeIsIdempotentAndFinalizeReinitializesCleanly) {
  RunInExecveChild([]() -> bool {
    if (nc_initialize() != NC_NOERR) {
      return false;
    }
    // Second nc_initialize() hits the already-initialized fast path.
    if (nc_initialize() != NC_NOERR) {
      return false;
    }

    int ncid = -1;
    if (nc_create_mem("init_probe.nc", NC_NETCDF4, 1024, &ncid) != NC_NOERR) {
      return false;
    }
    NC_memio memio{};
    if (nc_close_memio(ncid, &memio) != NC_NOERR) {
      return false;
    }
    std::free(memio.memory);

    if (nc_finalize() != NC_NOERR) {
      return false;
    }
    // Second nc_finalize() hits the already-finalized fast path.
    if (nc_finalize() != NC_NOERR) {
      return false;
    }
    // Re-initializing after finalize works cleanly as well.
    if (nc_initialize() != NC_NOERR) {
      return false;
    }
    return nc_finalize() == NC_NOERR;
  });
}

}  // namespace
}  // namespace netcdf
