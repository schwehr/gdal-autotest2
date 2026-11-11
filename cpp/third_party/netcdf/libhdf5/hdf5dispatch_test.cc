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

// Clean-room unit tests for //third_party/netcdf/libhdf5/hdf5dispatch.c.
//
// Exercises the HDF5 dispatch table wiring, format dispatch across
// NC_NETCDF4 and NC_NETCDF4 | NC_CLASSIC_MODEL, and the internal
// NC_HDF5_initialize() / NC_HDF5_finalize() lifecycle in an isolated child.

#include <cstdlib>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"

extern "C" {
#include "third_party/netcdf/include/hdf5dispatch.h"
#include "third_party/netcdf/include/hdf5internal.h"
#include "third_party/netcdf/include/nc4dispatch.h"
int nc_initialize(void);
}  // extern "C"

namespace netcdf {
namespace {

TEST(Hdf5DispatchTest, DispatchTableExposesExpectedMetadataAndEntryPoints) {
  ASSERT_EQ(NC_NOERR, nc_initialize());
  ASSERT_NE(HDF5_dispatch_table, nullptr);
  EXPECT_EQ(HDF5_dispatch_table->model, NC_FORMATX_NC4);
  EXPECT_EQ(HDF5_dispatch_table->dispatch_version, NC_DISPATCH_VERSION);

  EXPECT_EQ(HDF5_dispatch_table->create, &NC4_create);
  EXPECT_EQ(HDF5_dispatch_table->open, &NC4_open);
  EXPECT_EQ(HDF5_dispatch_table->redef, &NC4_redef);
  EXPECT_EQ(HDF5_dispatch_table->_enddef, &NC4__enddef);
  EXPECT_EQ(HDF5_dispatch_table->sync, &NC4_sync);
  EXPECT_EQ(HDF5_dispatch_table->abort, &NC4_abort);
  EXPECT_EQ(HDF5_dispatch_table->close, &NC4_close);
  EXPECT_EQ(HDF5_dispatch_table->set_fill, &NC4_set_fill);
  EXPECT_EQ(HDF5_dispatch_table->inq_format, &NC4_inq_format);
  EXPECT_EQ(HDF5_dispatch_table->inq_format_extended, &NC4_inq_format_extended);
  EXPECT_EQ(HDF5_dispatch_table->inq, &NC4_inq);
  EXPECT_EQ(HDF5_dispatch_table->inq_type, &NC4_inq_type);

  EXPECT_EQ(HDF5_dispatch_table->def_dim, &HDF5_def_dim);
  EXPECT_EQ(HDF5_dispatch_table->inq_dimid, &NC4_inq_dimid);
  EXPECT_EQ(HDF5_dispatch_table->inq_dim, &HDF5_inq_dim);
  EXPECT_EQ(HDF5_dispatch_table->inq_unlimdim, &NC4_inq_unlimdim);
  EXPECT_EQ(HDF5_dispatch_table->rename_dim, &HDF5_rename_dim);

  EXPECT_EQ(HDF5_dispatch_table->inq_att, &NC4_HDF5_inq_att);
  EXPECT_EQ(HDF5_dispatch_table->inq_attid, &NC4_HDF5_inq_attid);
  EXPECT_EQ(HDF5_dispatch_table->inq_attname, &NC4_HDF5_inq_attname);
  EXPECT_EQ(HDF5_dispatch_table->rename_att, &NC4_HDF5_rename_att);
  EXPECT_EQ(HDF5_dispatch_table->del_att, &NC4_HDF5_del_att);
  EXPECT_EQ(HDF5_dispatch_table->get_att, &NC4_HDF5_get_att);
  EXPECT_EQ(HDF5_dispatch_table->put_att, &NC4_HDF5_put_att);

  EXPECT_EQ(HDF5_dispatch_table->def_var, &NC4_def_var);
  EXPECT_EQ(HDF5_dispatch_table->inq_varid, &NC4_inq_varid);
  EXPECT_EQ(HDF5_dispatch_table->rename_var, &NC4_rename_var);
  EXPECT_EQ(HDF5_dispatch_table->inq_var_all, &NC4_HDF5_inq_var_all);

  EXPECT_EQ(HDF5_dispatch_table->def_grp, &NC4_def_grp);
  EXPECT_EQ(HDF5_dispatch_table->rename_grp, &NC4_rename_grp);
  EXPECT_EQ(HDF5_dispatch_table->def_var_filter, &NC4_hdf5_def_var_filter);
  EXPECT_EQ(HDF5_dispatch_table->set_var_chunk_cache,
            &NC4_HDF5_set_var_chunk_cache);
  EXPECT_EQ(HDF5_dispatch_table->inq_var_filter_ids,
            &NC4_hdf5_inq_var_filter_ids);
  EXPECT_EQ(HDF5_dispatch_table->inq_var_filter_info,
            &NC4_hdf5_inq_var_filter_info);
  EXPECT_EQ(HDF5_dispatch_table->inq_filter_avail, &NC4_hdf5_inq_filter_avail);
  EXPECT_EQ(HDF5_dispatch_table->def_var_quantize, &NC4_def_var_quantize);
  EXPECT_EQ(HDF5_dispatch_table->inq_var_quantize, &NC4_inq_var_quantize);
}

TEST(Hdf5DispatchTest, DispatchesNetcdf4AndClassicModelInMemoryOperations) {
  // Enhanced NetCDF-4/HDF5 dataset in memory.
  ScopedMemio memio;
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("dispatch_nc4.nc", NC_NETCDF4, 4096, file.ptr()));

    int fmt = 0;
    int ext_fmt = 0;
    int mode = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_format(file, &fmt));
    ASSERT_EQ(NC_NOERR, nc_inq_format_extended(file, &ext_fmt, &mode));
    EXPECT_EQ(fmt, NC_FORMAT_NETCDF4);
    EXPECT_EQ(ext_fmt, NC_FORMATX_NC4);
    EXPECT_NE(mode & NC_NETCDF4, 0);

    int grpid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(file, "telemetry", &grpid));
    int dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(grpid, "sample", 4, &dimid));
    int varid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(grpid, "values", NC_FLOAT, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc_def_var_deflate(grpid, varid, /*shuffle=*/1,
                                           /*deflate=*/1, /*deflate_level=*/4));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_quantize(grpid, varid, NC_QUANTIZE_BITGROOM, 3));
    const float kInput[4] = {1.5F, -2.25F, 3.75F, 0.5F};
    ASSERT_EQ(NC_NOERR, nc_enddef(file));
    ASSERT_EQ(NC_NOERR, nc_put_var_float(grpid, varid, kInput));

    ASSERT_EQ(NC_NOERR, nc_close_memio(file.release(), memio.get()));
  }

  ASSERT_NE(memio.bytes(), nullptr);
  ASSERT_GT(memio.size(), 0u);

  {
    ScopedNcFile reopened;
    ASSERT_EQ(NC_NOERR, nc_open_mem("dispatch_nc4.nc", NC_NOWRITE, memio.size(),
                                    const_cast<unsigned char*>(memio.bytes()),
                                    reopened.ptr()));
    int fmt = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_format(reopened, &fmt));
    EXPECT_EQ(fmt, NC_FORMAT_NETCDF4);

    int grpid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_ncid(reopened, "telemetry", &grpid));
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(grpid, "values", &varid));
    int quantize_mode = -1;
    int nsd = -1;
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_quantize(grpid, varid, &quantize_mode, &nsd));
    EXPECT_EQ(quantize_mode, NC_QUANTIZE_BITGROOM);
    EXPECT_EQ(nsd, 3);
  }

  // NetCDF-4 classic-model dataset in memory.
  ScopedMemio classic_memio;
  {
    ScopedNcFile classic_file;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("dispatch_nc4c.nc", NC_NETCDF4 | NC_CLASSIC_MODEL,
                            4096, classic_file.ptr()));
    int fmt = 0;
    int ext_fmt = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_format(classic_file, &fmt));
    ASSERT_EQ(NC_NOERR,
              nc_inq_format_extended(classic_file, &ext_fmt, nullptr));
    EXPECT_EQ(fmt, NC_FORMAT_NETCDF4_CLASSIC);
    EXPECT_EQ(ext_fmt, NC_FORMATX_NC4);

    int dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(classic_file, "idx", 2, &dimid));
    int varid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(classic_file, "ids", NC_INT, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(classic_file));
    const std::vector<int> kIds = {7, 11};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(classic_file, varid, kIds.data()));
    ASSERT_EQ(NC_NOERR,
              nc_close_memio(classic_file.release(), classic_memio.get()));
  }
}

TEST(Hdf5DispatchTest, InitializeAndFinalizeLifecycleInIsolatedChild) {
  RunInExecveChild([]() -> bool {
    if (nc_initialize() != NC_NOERR) {
      return false;
    }
    if (NC_HDF5_finalize() != NC_NOERR) {
      return false;
    }
    if (nc4_hdf5_initialized != 0) {
      return false;
    }

    // First initialize hits the !nc4_hdf5_initialized true branch.
    if (NC_HDF5_initialize() != NC_NOERR) {
      return false;
    }
    if (nc4_hdf5_initialized != 1 || HDF5_dispatch_table == nullptr) {
      return false;
    }

    // Second initialize hits the !nc4_hdf5_initialized false branch.
    if (NC_HDF5_initialize() != NC_NOERR) {
      return false;
    }
    if (nc4_hdf5_initialized != 1) {
      return false;
    }

    int ncid = -1;
    if (nc_create_mem("lifecycle.nc", NC_NETCDF4, 2048, &ncid) != NC_NOERR) {
      return false;
    }
    int dimid = -1;
    int varid = -1;
    if (nc_def_dim(ncid, "n", 3, &dimid) != NC_NOERR ||
        nc_def_var(ncid, "x", NC_INT, 1, &dimid, &varid) != NC_NOERR) {
      static_cast<void>(nc_abort(ncid));
      return false;
    }
    NC_memio memio{};
    if (nc_close_memio(ncid, &memio) != NC_NOERR) {
      std::free(memio.memory);
      return false;
    }
    std::free(memio.memory);

    if (NC_HDF5_finalize() != NC_NOERR) {
      return false;
    }
    if (nc4_hdf5_initialized != 0) {
      return false;
    }

    // Leave initialized before exit so any runtime cleanup remains valid.
    return NC_HDF5_initialize() == NC_NOERR;
  });
}

}  // namespace
}  // namespace netcdf
