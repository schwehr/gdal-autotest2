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

// Pure-C test shim for //third_party/netcdf/libhdf5/hdf5open.c.
//
// Keeps <hdf5.h>, <hdf5_hl.h>, and unguarded netCDF internal headers inside C
// translation units while exposing plain C entry points to C++20 GoogleTest
// binaries.

#ifndef THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_HDF5OPEN_SHIM_H_
#define THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_HDF5OPEN_SHIM_H_

#ifdef __cplusplus
extern "C" {
#endif

// Variant selectors for H5OpenShimCreateNegativeFile().
enum H5OpenShimNegativeVariant {
  kH5OpenShimNegH5sNullVar = 1,
  kH5OpenShimNegBadCoordsLen = 2,
  kH5OpenShimNeg2dAttr = 3,
  kH5OpenShimNegCommittedBitfield = 4,
  kH5OpenShimNegDanglingLink = 5,
};

// Probe selectors for H5OpenShimProbeDirectOpen().
enum H5OpenShimDirectOpenKind {
  kH5OpenShimDirectBadNcid = 1,
  kH5OpenShimDirectIllegalMmap = 2,
  kH5OpenShimDirectDisklessInmem = 3,
  kH5OpenShimDirectInmemNullParams = 4,
  kH5OpenShimDirectInmemEmptyMem = 5,
  kH5OpenShimDirectColdInit = 6,
};

// Creates a raw HDF5 file containing committed and transient user-defined
// types, dimension scales (named, unnamed, unlimited without variable, parent
// group scale referenced from a child group), phony-dim candidates, virtual,
// compact, contiguous, chunked/deflate/szip/big-endian datasets, fixed/vlen
// string datasets and attributes, and ignored bitfield/reference objects.
// Returns 0 on success or -1 on HDF5 creation failure.
int H5OpenShimCreateRawHdf5File(const char* path, int use_crt_order,
                                int include_classic_attr);

// Creates a chunked HDF5 file with unregistered custom filter IDs (400/401),
// malformed deflate filter parameter counts/levels, and a mandatory fletcher32
// filter applied to a variable-length string dataset.
// Returns 0 on success or -1 on HDF5 creation failure.
int H5OpenShimCreateBadFilterFile(const char* path);

// Creates a negative-test HDF5 file exercising specific error paths in
// hdf5open.c selected by H5OpenShimNegativeVariant.
// Returns 0 on success or -1 on HDF5 creation failure.
int H5OpenShimCreateNegativeFile(const char* path, int variant);

// Invokes NC4_open() directly to exercise internal validation and
// initialization branches filtered out by public nc_open().
// Returns the netCDF status code produced by NC4_open().
int H5OpenShimProbeDirectOpen(int kind, const char* path, int mode,
                              void* memio_void);

// Invokes nc4_get_var_meta() twice on an open variable to exercise its
// already-read early return path. Returns the netCDF status code.
int H5OpenShimProbeGetVarMetaTwice(int ncid, int varid);

// Extra fixture selectors for H5OpenShimCreateExtraFixtureFile().
enum H5OpenShimExtraFixtureKind {
  kH5OpenShimExtraCommittedVarStr = 1,
  kH5OpenShimExtraLdblGlobalAttr = 2,
  kH5OpenShimExtraChunkedVarStrDeflate = 3,
  kH5OpenShimExtraUntrackedSubgroup = 4,
  kH5OpenShimExtraVaxFloatInSubgroup = 5,
  kH5OpenShimExtra2dScaleNoCoords = 6,
  kH5OpenShimExtraBadDimListZeroRef = 7,
  kH5OpenShimExtraBadDimListWithCoords = 8,
  kH5OpenShimExtraBadQuantizeStrAttr = 9,
  kH5OpenShimExtraReadTypeCmpLongField = 10,
  kH5OpenShimExtraReadTypeCmpArrayLdbl = 11,
  kH5OpenShimExtraReadTypeCmpScalarLdbl = 12,
  kH5OpenShimExtraReadTypeVlenLdbl = 13,
  kH5OpenShimExtraReadTypeEnum24Bit = 14,
  kH5OpenShimExtraReadTypeEnumLongMember = 15,
};

// Creates an extra raw HDF5 fixture selected by H5OpenShimExtraFixtureKind.
// Returns 0 on success or -1 on HDF5 creation failure.
int H5OpenShimCreateExtraFixtureFile(const char* path, int kind);

// Temporarily sets global chunk cache preemption to an invalid value (> 1.0),
// opens `path` via nc_open(NC_NOWRITE), restores preemption, and returns the
// netCDF status code produced by nc_open().
int H5OpenShimProbeInvalidPreemption(const char* path);

// Runs leaky or global-filter/path-mutating raw HDF5 negative probes (stub
// SZIP filter counts, bad quantize attribute types, bad DIMENSION_LIST with
// _Netcdf4Coordinates, committed type errors in read_type, and NCPD_WIN
// unconvertible path in nc4_H5Fopen) using temporary files created inside
// `dir`. Intended only for an isolated child process. Returns 0 on success or
// -1 on failure.
int H5OpenShimRunLeakyRawSuite(const char* dir);

// Mutates the HDF5 H5P_FILE_ACCESS property class sequentially to trigger FAPL
// setup failure paths in nc4_open_file() against an existing netCDF-4 file
// `path`. Intended only for an isolated child process. Returns 0 on success or
// -1 on failure.
int H5OpenShimRunH5pClassFailureSuite(const char* path);

// Registers and activates a pass-through fault-injecting HDF5 VOL connector to
// exercise internal HDF5 API error paths in hdf5open.c using fixtures created
// under `dir`. Intended only for an isolated child process. Returns 0 on
// success or -1 on failure.
int H5OpenShimRunOpenFaultVolSuite(const char* dir);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif  // THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_HDF5OPEN_SHIM_H_
