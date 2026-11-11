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

#ifndef THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_HDF5ATTR_SHIM_H_
#define THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_HDF5ATTR_SHIM_H_

#include <stddef.h>

#include "third_party/netcdf/include/netcdf.h"

#ifdef __cplusplus
extern "C" {
#endif

// Creates a minimal raw HDF5 file without a netCDF `_NCProperties` attribute
// so opening it yields `h5->provenance.ncproperties == NULL`. Returns 0 on
// success, non-zero on failure.
int Hdf5CreateRawHdf5File(const char* path);

// Invokes internal `nc4_get_att_special()` directly with an arbitrary
// `mem_type`, optional `attnump` output query, and optional NULL output
// pointers.
int Hdf5CallGetAttSpecial(int ncid, const char* name, nc_type mem_type,
                          int query_attnum, int pass_null_outputs,
                          nc_type* filetype_out, size_t* len_out,
                          void* data_out);

// Invokes internal `nc4_put_att()` directly with arbitrary `mem_type` and
// `force` flag.
int Hdf5PutAttDirect(int ncid, int varid, const char* name, nc_type file_type,
                     size_t len, const void* data, nc_type mem_type, int force);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_HDF5ATTR_SHIM_H_
