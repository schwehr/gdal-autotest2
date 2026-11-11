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
//
// Pure-C test shim exposing internal `hdf5set_format_compatibility()` checks
// without leaking `<hdf5.h>` into C++20 translation units.

#ifndef THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_HDF5SET_FORMAT_COMPATIBILITY_SHIM_H_
#define THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_HDF5SET_FORMAT_COMPATIBILITY_SHIM_H_

#ifdef __cplusplus
extern "C" {
#endif

// Creates an HDF5 file access property list, invokes
// `hdf5set_format_compatibility(fapl)`, queries the resulting libver bounds,
// and reports whether `low == H5F_LIBVER_V18` and `high == H5F_LIBVER_LATEST`.
int NetcdfTestSetFormatCompatibilityValidFapl(int* low_is_v18,
                                              int* high_is_latest);

// Calls `hdf5set_format_compatibility` with an invalid property list ID (`-1`).
int NetcdfTestSetFormatCompatibilityInvalidHid(void);

// Calls `hdf5set_format_compatibility(H5P_DEFAULT)`.
int NetcdfTestSetFormatCompatibilityDefaultPlist(void);

// Calls `hdf5set_format_compatibility` with a dataset creation property list
// instead of a file access property list.
int NetcdfTestSetFormatCompatibilityWrongPlistClass(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_HDF5SET_FORMAT_COMPATIBILITY_SHIM_H_
