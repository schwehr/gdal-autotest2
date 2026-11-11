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

#ifndef THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_NC4MEM_SHIM_H_
#define THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_NC4MEM_SHIM_H_

#ifdef __cplusplus
extern "C" {
#endif

// Calls NC4_open_image_file() with invalid memio arguments (`memory == NULL`
// and/or `size == 0`) to exercise the NC_EINVAL guard directly.
int Nc4MemShimOpenImageInvalidArgs(int null_memory, int zero_size);

// Calls NC4_create_image_file() with a non-NULL buffer pointer and size == 0
// so NC4_image_init() rejects it cleanly and returns NC_EHDFERR without leaks.
int Nc4MemShimCreateImageInvalidSize(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_NC4MEM_SHIM_H_
