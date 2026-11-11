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

#ifndef THIRD_PARTY_NETCDF_GOOGLE_LIBDISPATCH_DVAR_SHIM_H_
#define THIRD_PARTY_NETCDF_GOOGLE_LIBDISPATCH_DVAR_SHIM_H_

#ifdef __cplusplus
extern "C" {
#endif

enum NetcdfDvarUdfFault {
  kDvarFaultNone = 0,
  kDvarFaultFailVardimid = 1,
  kDvarFaultFailInqDim = 2,
  kDvarFaultFailUnlimdim = 3,
  kDvarFaultFailUnlimdimsCount = 4,
  kDvarFaultZeroUnlimdims = 5,
  kDvarFaultFailUnlimdimsIds = 6,
};

// Registers an NC3-backed NC_UDF8 dispatch table supporting deterministic
// fault injection for internal dvar.c helpers (NC_is_recvar, NC_inq_recvar,
// NC_getshape, NC_check_nulls).
int netcdf_dvar_register_udf(void);

// Sets the active fault mode for NC_UDF8 calls (use RAII guard to reset to
// kDvarFaultNone).
void netcdf_dvar_set_udf_fault(int fault_mode);

#ifdef __cplusplus
}
#endif

#endif  // THIRD_PARTY_NETCDF_GOOGLE_LIBDISPATCH_DVAR_SHIM_H_
