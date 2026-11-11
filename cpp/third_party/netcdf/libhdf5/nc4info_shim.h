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

#ifndef THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_NC4INFO_SHIM_H_
#define THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_NC4INFO_SHIM_H_

#ifdef __cplusplus
extern "C" {
#endif

// Initializes global provenance metadata via NC4_provenance_init().
int Nc4InfoShimProvenanceInit(void);

// Finalizes global provenance metadata via NC4_provenance_finalize().
// Note: nc4info.c does not reset globalpropinitialized; call only inside an
// isolated child process.
int Nc4InfoShimProvenanceFinalize(void);

// Drives NC4_new_provenance() with an invalid HDF5 file ID (-1).
int Nc4InfoShimNewProvenanceBadHdfid(void);

// Inside an isolated child process, finalizes global provenance and calls
// NC4_new_provenance() + NC4_write_provenance() against a fresh HDF5 file at
// `path` to cover the post-finalize globalprovenance.ncproperties == NULL path.
int Nc4InfoShimNewProvenanceAfterFinalize(const char* path, int* out_has_props,
                                          int* out_version,
                                          int* out_superblock);

// Drives NC4_read_provenance() with an invalid HDF5 file ID (-1).
int Nc4InfoShimReadProvenanceBadHdfid(void);

// Drives NC4_write_provenance() error branches:
// - `no_write != 0`: sets h5.no_write = NC_TRUE (expects NC_EPERM).
// - `bad_grpid != 0`: uses hdf_grpid = -1 with non-empty ncproperties
//   (expects NC_EFILEMETA).
// - `empty_props != 0`: creates a fresh HDF5 file at `path` and sets
//   ncproperties = "" so H5Tset_size(..., 0) fails (expects NC_EFILEMETA).
int Nc4InfoShimWriteProvenanceError(const char* path, int no_write,
                                    int bad_grpid, int empty_props);

// Exercises NC4_clear_provenance() on NULL, zeroed, and heap-populated structs.
int Nc4InfoShimTestClearProvenance(void);

// Invokes ncprintprovenance() with the supplied fields and flushes stderr.
int Nc4InfoShimPrintProvenance(int version, int superblock,
                               const char* ncproperties);

// Creates a raw HDF5 file at `path` without going through netCDF-4:
// - `mode == 0`: no `_NCProperties` attribute; enables link/attribute creation
//   order tracking so netCDF-4 can open the file with `NC_WRITE`.
// - `mode == 1`: non-string scalar int `_NCProperties` attribute.
// - `mode == 2`: fixed ASCII string `_NCProperties` attribute with
// `text_value`.
int Nc4InfoShimCreateRawHdf5File(const char* path, int mode,
                                 const char* text_value);

// Queries HDF5 root attribute existence of `_NCProperties` on `path`.
int Nc4InfoShimHasRawNcPropertiesAttr(const char* path, int* exists_out);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_NC4INFO_SHIM_H_
