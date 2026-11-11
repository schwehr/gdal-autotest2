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

#ifndef THIRD_PARTY_NETCDF_GOOGLE_LIBSRC4_NC4TYPE_SHIM_H_
#define THIRD_PARTY_NETCDF_GOOGLE_LIBSRC4_NC4TYPE_SHIM_H_

#include <stddef.h>

#include "third_party/netcdf/include/netcdf.h"

#ifdef __cplusplus
extern "C" {
#endif

// Calls NC4_inq_type() directly, bypassing the libdispatch atomic-type
// short-circuit in nc_inq_type().
int nc4type_test_inq_type(int ncid, nc_type typeid1, char* name, size_t* size);

// Calls NC4_inq_type_fixed_size() directly, which has no public API wrapper.
int nc4type_test_inq_type_fixed_size(int ncid, nc_type xtype, int* fixedsizep);

// Resolves ncid to NC_FILE_INFO_T* via nc4_find_grp_h5() and invokes
// nc4_get_typeclass().
int nc4type_test_get_typeclass(int ncid, nc_type xtype, int* type_class);

// Invokes NC4_set_varsize() on a zeroed NC_TYPE_INFO_T with configurable
// hdr.id and nc_type_class.
int nc4type_test_set_varsize(nc_type type_id, nc_type type_class,
                             int* out_varsized);

// Invokes NC4_var_varsized() on a zeroed NC_VAR_INFO_T backed by a zeroed
// NC_TYPE_INFO_T with configurable hdr.id and varsized flag.
int nc4type_test_var_varsized(nc_type type_id, int initial_varsized);

#ifdef __cplusplus
}
#endif

#endif  // THIRD_PARTY_NETCDF_GOOGLE_LIBSRC4_NC4TYPE_SHIM_H_
