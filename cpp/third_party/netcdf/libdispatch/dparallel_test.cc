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

// Unit tests for //third_party/netcdf/libdispatch/dparallel.c.
//
// USE_PARALLEL is disabled in google3 (config.h), so all C and Fortran-bridge
// parallel I/O entry points return NC_ENOPAR cleanly without touching files
// or mutating output ncid pointers.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dparallel.c

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"
#include "third_party/netcdf/include/netcdf_f.h"

extern "C" {
#include "third_party/netcdf/include/ncdispatch.h"

int nc_create_par(const char* path, int cmode, MPI_Comm comm, MPI_Info info,
                  int* ncidp);
int nc_open_par(const char* path, int omode, MPI_Comm comm, MPI_Info info,
                int* ncidp);
int nc_var_par_access(int ncid, int varid, int par_access);
}  // extern "C"

namespace netcdf {
namespace {

TEST(DparallelTest, CreateParReturnsEnoparAcrossModesAndNullArgs) {
  TempFile nc_path("dparallel_create.nc");
  int ncid = -1;

  for (const int cmode :
       {NC_CLOBBER, NC_64BIT_OFFSET, NC_CDF5, NC_NETCDF4,
        NC_NETCDF4 | NC_CLASSIC_MODEL, NC_NETCDF4 | NC_DISKLESS}) {
    SCOPED_TRACE(cmode);
    EXPECT_EQ(NC_ENOPAR, nc_create_par(nc_path.c_str(), cmode, MPI_COMM_WORLD,
                                       MPI_INFO_NULL, &ncid));
    EXPECT_EQ(-1, ncid);
  }

  EXPECT_EQ(NC_ENOPAR, nc_create_par(nullptr, NC_NETCDF4, MPI_COMM_WORLD,
                                     MPI_INFO_NULL, nullptr));
}

TEST(DparallelTest, OpenParReturnsEnoparAcrossModesAndNullArgs) {
  TempFile nc_path("dparallel_open.nc");
  {
    ScopedNcFile writer;
    ASSERT_EQ(NC_NOERR, nc_create(nc_path.c_str(), NC_NETCDF4, writer.ptr()));
    ASSERT_EQ(NC_NOERR, writer.close());
  }

  int ncid = -1;
  EXPECT_EQ(NC_ENOPAR, nc_open_par(nc_path.c_str(), NC_NOWRITE, MPI_COMM_WORLD,
                                   MPI_INFO_NULL, &ncid));
  EXPECT_EQ(-1, ncid);

  EXPECT_EQ(NC_ENOPAR, nc_open_par(nc_path.c_str(), NC_WRITE, MPI_COMM_WORLD,
                                   MPI_INFO_NULL, &ncid));
  EXPECT_EQ(-1, ncid);

  EXPECT_EQ(NC_ENOPAR, nc_open_par(nullptr, NC_NOWRITE, MPI_COMM_WORLD,
                                   MPI_INFO_NULL, nullptr));
}

TEST(DparallelTest, FortranParallelBridgesReturnEnopar) {
  TempFile nc_path("dparallel_fortran.nc");
  int ncid = -1;

  EXPECT_EQ(NC_ENOPAR,
            nc_create_par_fortran(nc_path.c_str(), NC_NETCDF4, 0, 0, &ncid));
  EXPECT_EQ(-1, ncid);
  EXPECT_EQ(NC_ENOPAR,
            nc_create_par_fortran(nullptr, NC_CLOBBER, 0, 0, nullptr));

  EXPECT_EQ(NC_ENOPAR,
            nc_open_par_fortran(nc_path.c_str(), NC_NOWRITE, 0, 0, &ncid));
  EXPECT_EQ(-1, ncid);
  EXPECT_EQ(NC_ENOPAR, nc_open_par_fortran(nullptr, NC_WRITE, 0, 0, nullptr));
}

TEST(DparallelTest, VarParAccessReturnsEnoparForInvalidAndOpenDatasets) {
  EXPECT_EQ(NC_ENOPAR, nc_var_par_access(-1, 0, 0));
  EXPECT_EQ(NC_ENOPAR, nc_var_par_access(-1, NC_GLOBAL, 1));

  for (const int mode : {NC_CLOBBER | NC_DISKLESS, NC_NETCDF4 | NC_DISKLESS}) {
    SCOPED_TRACE(mode);
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create("dparallel_var.nc", mode, nc.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "d", 3, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "v", NC_INT, 1, &dimid, &varid));

    EXPECT_EQ(NC_ENOPAR, nc_var_par_access(nc.get(), varid, 0));
    EXPECT_EQ(NC_ENOPAR, nc_var_par_access(nc.get(), varid, 1));
    EXPECT_EQ(NC_ENOPAR, nc_var_par_access(nc.get(), NC_GLOBAL, 1));
    EXPECT_EQ(NC_NOERR, nc.close());
  }
}

}  // namespace
}  // namespace netcdf
