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

// Pure-C test fixture helper for creating raw HDF5 datasets that exercise
// non-netCDF-4-created paths in //third_party/netcdf/libhdf5/hdf5var.c.

#ifndef THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_HDF5VAR_SHIM_H_
#define THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_HDF5VAR_SHIM_H_

#ifdef __cplusplus
extern "C" {
#endif

// Creates an HDF5 file at `path` containing:
// - "fixed_str_1d": 1D (len 3) fixed-length 8-byte C string dataset.
// - "fixed_str_scalar": scalar (0D) fixed-length 8-byte C string dataset.
// - "missing_filter_var": 1D (len 4, chunk 2) int32 dataset configured with
//   an unregistered optional HDF5 filter ID (31999).
// Returns 0 on success and non-zero on HDF5 error.
int NetcdfGoogleCreateRawHdf5VarFixture(const char* path);

#ifdef __cplusplus
}
#endif

#endif  // THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_HDF5VAR_SHIM_H_
