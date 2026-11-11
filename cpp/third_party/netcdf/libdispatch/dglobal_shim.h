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

// Pure-C shim bridging C global struct scoping differences (`struct GlobalAWS`
// nested inside `NCglobalstate` vs file-scope forward declaration in
// ncglobal.h) for C++20 unit tests in dglobal_test.cc.

#ifndef THIRD_PARTY_NETCDF_GOOGLE_LIBDISPATCH_DGLOBAL_SHIM_H_
#define THIRD_PARTY_NETCDF_GOOGLE_LIBDISPATCH_DGLOBAL_SHIM_H_

#ifdef __cplusplus
extern "C" {
#endif

struct NCglobalstate;

// Frees any existing values first and populates all fields inside `gs->aws`.
// NULL string parameters clear the corresponding field.
void dglobal_shim_populate_aws(struct NCglobalstate* gs,
                               const char* default_region,
                               const char* config_file, const char* profile,
                               const char* access_key_id,
                               const char* secret_access_key);

// Invokes `NC_clearawsparams(&gs->aws)`.
void dglobal_shim_clear_aws(struct NCglobalstate* gs);

// Returns 1 if all five fields inside `gs->aws` are NULL, 0 otherwise.
int dglobal_shim_aws_is_cleared(const struct NCglobalstate* gs);

// Read-only accessors for `gs->aws` string fields.
const char* dglobal_shim_aws_default_region(const struct NCglobalstate* gs);
const char* dglobal_shim_aws_config_file(const struct NCglobalstate* gs);
const char* dglobal_shim_aws_profile(const struct NCglobalstate* gs);
const char* dglobal_shim_aws_access_key_id(const struct NCglobalstate* gs);
const char* dglobal_shim_aws_secret_access_key(const struct NCglobalstate* gs);

#ifdef __cplusplus
}
#endif

#endif  // THIRD_PARTY_NETCDF_GOOGLE_LIBDISPATCH_DGLOBAL_SHIM_H_
