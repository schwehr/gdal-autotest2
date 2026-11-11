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

// Clean-room unit tests for //third_party/netcdf/libhdf5/hdf5debug.c.
//
// NOTE: `libhdf5/hdf5debug.h` undefines `H5CATCH`, so all definitions in
// `hdf5debug.c` compile out in normal builds (0 executable lines). This smoke
// test exercises basic NetCDF-4 error propagation without crashing or leaking.

#include <array>
#include <cstdint>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

namespace netcdf {
namespace {

TEST(Hdf5DebugTest, SmokeInMemoryRoundTripAndErrorHandling) {
  int raw_ncid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("debug_smoke.nc", NC_NETCDF4, 4096, &raw_ncid));
  ScopedNcFile nc(raw_ncid);

  int dimid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 2, &dimid));
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v", NC_INT, 1, &dimid, &varid));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));

  const std::array<int, 2> values = {7, 11};
  ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, varid, values.data()));

  NC_memio raw_mem{};
  ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), &raw_mem));
  ScopedMemio mem(raw_mem);
  EXPECT_GT(mem.size(), 0U);

  int ro_ncid = -1;
  ASSERT_EQ(NC_NOERR, nc_open_mem("debug_smoke.nc", NC_NOWRITE, mem.size(),
                                  mem.get()->memory, &ro_ncid));
  ScopedNcFile ro_nc(ro_ncid);

  std::array<int, 2> read_back = {0, 0};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(ro_nc, varid, read_back.data()));
  EXPECT_EQ(values, read_back);

  // Trigger clean HDF5/dispatch error paths with invalid IDs and garbage bytes.
  EXPECT_EQ(NC_EBADID, nc_inq_nvars(-1, &varid));
  std::array<uint8_t, 16> junk = {};
  int junk_ncid = -1;
  EXPECT_EQ(NC_ENOTNC, nc_open_mem("junk.nc", NC_NOWRITE, junk.size(),
                                   junk.data(), &junk_ncid));
}

}  // namespace
}  // namespace netcdf
