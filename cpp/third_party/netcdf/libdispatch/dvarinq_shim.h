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

#ifndef THIRD_PARTY_NETCDF_GOOGLE_LIBDISPATCH_DVARINQ_SHIM_H_
#define THIRD_PARTY_NETCDF_GOOGLE_LIBDISPATCH_DVARINQ_SHIM_H_

#ifdef __cplusplus
extern "C" {
#endif

// Registers an NC3-backed NC_UDF9 dispatch table with deterministic
// inq_var_filter_info responses keyed by varid so all dvarinq.c switch and
// parameter-count error branches are exercisable hermetically in RAM.
int netcdf_dvarinq_register_udf(void);

#ifdef __cplusplus
}
#endif

#endif  // THIRD_PARTY_NETCDF_GOOGLE_LIBDISPATCH_DVARINQ_SHIM_H_
