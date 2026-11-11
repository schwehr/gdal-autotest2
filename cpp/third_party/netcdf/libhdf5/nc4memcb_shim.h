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

#ifndef THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_NC4MEMCB_SHIM_H_
#define THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_NC4MEMCB_SHIM_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mirror of nc4memcb.c's H5LT_file_image_ud_t layout (pure-C types only so
// C++ callers do not pull <hdf5.h> or internal headers).
typedef struct nc4memcb_udata_mirror {
  void* app_image_ptr;
  size_t app_image_size;
  void* fapl_image_ptr;
  size_t fapl_image_size;
  int fapl_ref_count;
  void* vfd_image_ptr;
  size_t vfd_image_size;
  int vfd_ref_count;
  unsigned flags;
  int ref_count;
  void* h5;
} nc4memcb_udata_mirror_t;

// Mirrors H5LT_FILE_IMAGE_* flags used by nc4memcb.c.
enum {
  kNc4MemcbFlagOpenRw = 0x0001u,
  kNc4MemcbFlagDontCopy = 0x0002u,
  kNc4MemcbFlagDontRelease = 0x0004u,
};

// Mirrors H5FD_file_image_op_t values.
enum {
  kNc4MemcbOpNoOp = 0,
  kNc4MemcbOpPropertyListSet = 1,
  kNc4MemcbOpPropertyListCopy = 2,
  kNc4MemcbOpPropertyListGet = 3,
  kNc4MemcbOpPropertyListClose = 4,
  kNc4MemcbOpFileOpen = 5,
  kNc4MemcbOpFileResize = 6,
  kNc4MemcbOpFileClose = 7,
};

// Ensures the netCDF HDF5 layer (including silent H5E error stack) is ready.
void Nc4MemcbShimEnsureHdf5Initialized(void);

// Registers the pass-through capture VOL connector (`nc4memcb_capture_vol`).
// Returns >= 0 on success.
int Nc4MemcbShimRegisterCaptureVol(void);

// Unregisters the capture VOL connector after all files using it are closed and
// default VOL has been restored.
int Nc4MemcbShimUnregisterCaptureVol(void);

// Reparses HDF5_VOL_CONNECTOR so the active default VOL takes effect.
int Nc4MemcbShimReparseDefaultVol(void);

// Arms/disarms failure on the next VOL file_create call.
void Nc4MemcbShimSetFailNextCreate(int fail_next);

// Resets/queries captured H5FD_file_image_callbacks_t pointers.
void Nc4MemcbShimResetCapturedCallbacks(void);
int Nc4MemcbShimHasCapturedCallbacks(void);

// Invokes the captured file-image callbacks against `ud`.
void* Nc4MemcbShimCallMalloc(size_t size, int op, nc4memcb_udata_mirror_t* ud);
void* Nc4MemcbShimCallMemcpy(void* dest, const void* src, size_t size, int op,
                             nc4memcb_udata_mirror_t* ud);
void* Nc4MemcbShimCallRealloc(void* ptr, size_t size, int op,
                              nc4memcb_udata_mirror_t* ud);
int Nc4MemcbShimCallFree(void* ptr, int op, nc4memcb_udata_mirror_t* ud);
void* Nc4MemcbShimCallUdataCopy(nc4memcb_udata_mirror_t* ud);
int Nc4MemcbShimCallUdataFree(nc4memcb_udata_mirror_t* ud);

// Direct NC4_image_init / NC4_image_finalize / NC4_extract_file_image boundary
// helpers (all leak-free).
int Nc4MemcbShimImageInitMissingInput(void);
int Nc4MemcbShimImageInitZeroSize(void);
int Nc4MemcbShimImageInitCreateFail(void);
void Nc4MemcbShimFinalizeNull(void);
int Nc4MemcbShimExtractAbortNullUdata(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_NC4MEMCB_SHIM_H_
