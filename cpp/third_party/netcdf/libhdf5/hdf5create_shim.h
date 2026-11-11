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

#ifndef THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_HDF5CREATE_SHIM_H_
#define THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_HDF5CREATE_SHIM_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Invokes NC4_create() directly with an illegal cmode before dispatch mode
// checks intercept it. Only safe for cmode flags rejected before ncid lookup.
int Hdf5CreateWithIllegalCmode(const char* path, int cmode);

// Allocates a temporary NC entry and invokes NC4_create() directly, bypassing
// libdispatch check_create_mode() (for example to exercise inmemory|diskless
// mutual exclusion or non-NULL NC_memio create parameters). Cleans up all
// registered NC/HDF5 structures before returning.
int Hdf5CreateBypassDispatch(const char* path, int cmode, size_t initialsz,
                             int pass_empty_memio);

enum Hdf5CreateFailPropKind {
  kHdf5CreateFailUnregisterCloseDegree = 0,
  kHdf5CreateFailUnregisterMetaBlockSize = 1,
  kHdf5CreateFailUnregisterLibverLowBound = 2,
  kHdf5CreateFailUnregisterObjTrackTimes = 3,
  kHdf5CreateFailUnregisterLinkInfo = 4,
  kHdf5CreateFailUnregisterFaplCore = 5,
  kHdf5CreateFailCreateFcpl = 6,
  kHdf5CreateFailCreateFapl = 7,
};

// Calls NC4_create() directly with NC_NETCDF4 | NC_INMEMORY and a non-NULL
// buffer with size == 0 to exercise NC4_create_image_file() failure cleanup.
int Hdf5CreateWithNonNullZeroSizeMemio(void);

// Temporarily sets global chunk cache preemption out of [0, 1] and calls
// nc_create() so H5Pset_cache() fails; restores global state before returning.
int Hdf5CreateWithInvalidChunkCachePreemption(const char* path);

// Resets nc4_hdf5_initialized via nc4_hdf5_finalize() and calls NC4_create()
// directly in an isolated child process.
int Hdf5CreateColdInit(const char* path);

// Switches path conversion platform to NCPD_WIN around direct NC4_create()
// with NC_DISKLESS so NCpathcvt() fails and returns NULL; restores platform.
int Hdf5CreateWithUnconvertiblePath(const char* path);

// Injects HDF5 property-list creation or setter failure via H5Punregister or
// H5Pregister2 depending on |kind| (best called inside an isolated child).
int Hdf5CreateFailPlistProperty(enum Hdf5CreateFailPropKind kind);

// Registers the temporary pass-through root-open failure VOL connector and
// returns its HDF5 hid_t cast to long long (< 0 on error).
long long Hdf5CreateRegisterFailRootVol(void);

// Reparses HDF5_VOL_CONNECTOR after environment updates (returns 0 on success).
int Hdf5CreateReparseDefaultVol(void);

// Arms one-shot failure on H5Gopen2(..., "/", ...) inside NC4_create() using
// NC_NETCDF4 | NC_DISKLESS and cleans up leftover NC_FILE_INFO_T resources.
int Hdf5CreateWithRootGroupOpenFailure(const char* path);

// Reparses HDF5_VOL_CONNECTOR after restoring environment and unregisters
// |vol_id| cleanly (returns 0 on success).
int Hdf5CreateCleanupFailRootVol(long long vol_id);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_HDF5CREATE_SHIM_H_
