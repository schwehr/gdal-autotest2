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

#ifndef THIRD_PARTY_NETCDF_GOOGLE_LIBDISPATCH_DS3UTIL_SHIM_H_
#define THIRD_PARTY_NETCDF_GOOGLE_LIBDISPATCH_DS3UTIL_SHIM_H_

#ifdef __cplusplus
extern "C" {
#endif

struct NCglobalstate;

void ds3util_shim_clear_aws(struct NCglobalstate* gs);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // THIRD_PARTY_NETCDF_GOOGLE_LIBDISPATCH_DS3UTIL_SHIM_H_
