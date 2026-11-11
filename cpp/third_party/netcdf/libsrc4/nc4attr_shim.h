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

#ifndef THIRD_PARTY_NETCDF_GOOGLE_LIBSRC4_NC4ATTR_SHIM_H_
#define THIRD_PARTY_NETCDF_GOOGLE_LIBSRC4_NC4ATTR_SHIM_H_

#include <stddef.h>

#include "third_party/netcdf/include/netcdf.h"

#ifdef __cplusplus
extern "C" {
#endif

// Direct C shims forwarding to libsrc4/nc4attr.c symbols that HDF5_dispatcher
// overrides with NC4_HDF5_* variants.

int NetcdfTestNc4InqAtt(int ncid, int varid, const char* name, nc_type* xtypep,
                        size_t* lenp);

int NetcdfTestNc4InqAttid(int ncid, int varid, const char* name, int* attnump);

int NetcdfTestNc4InqAttname(int ncid, int varid, int attnum, char* name);

int NetcdfTestNc4GetAtt(int ncid, int varid, const char* name, void* value,
                        nc_type memtype);

int NetcdfTestNc4GetAttRaw(int ncid, int varid, const char* name,
                           nc_type* xtypep, nc_type mem_type, size_t* lenp,
                           int* attnump, void* data);

int NetcdfTestNc4GetAttPtrsNullName(int ncid, int varid, nc_type* xtypep,
                                    nc_type mem_type, size_t* lenp,
                                    int* attnump, void* data);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // THIRD_PARTY_NETCDF_GOOGLE_LIBSRC4_NC4ATTR_SHIM_H_
