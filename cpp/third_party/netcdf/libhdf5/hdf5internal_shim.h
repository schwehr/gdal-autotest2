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

#ifndef THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_HDF5INTERNAL_SHIM_H_
#define THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_HDF5INTERNAL_SHIM_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Inspects the HDF5 backend initialization flag and exercises direct HDF5
// dispatch initialization / finalization.
int NetcdfTestHdf5IsInitialized(void);
int NetcdfTestHdf5InitializeDispatch(void);
int NetcdfTestHdf5FinalizeDispatch(void);

// Exercises nc4_rec_find_hdf_type() with the HDF5 datatype corresponding to
// `nc_typeid`, a built-in native integer type, or an invalid HDF5 hid_t.
int NetcdfTestFindHdfTypeForNcType(int ncid, int nc_typeid, int* found_out,
                                   int* matched_typeid_out);
int NetcdfTestFindHdfTypeBuiltinInt(int ncid, int* found_out);
int NetcdfTestFindHdfTypeInvalidHid(int ncid, int* found_out);

// Exercises edge-case branches of nc4_reform_coord_var().
int NetcdfTestReformCoordVarWithDimscale(int ncid, int dimid, int varid);
int NetcdfTestReformCoordVarWasCoordNoAttached(int ncid, int dimid, int varid);
int NetcdfTestReformCoordVarZeroDimDataset(int ncid, int dimid, int varid);

// Wraps nc4_hdf5_find_grp_h5_var(), nc4_hdf5_find_grp_var_att(), and
// nc4_hdf5_get_chunk_cache() with optional-NULL output coverage.
int NetcdfTestFindGrpH5Var(int ncid, int varid, int pass_null_outputs,
                           int* varid_out);
int NetcdfTestFindGrpVarAtt(int ncid, int varid, const char* name, int attnum,
                            int use_name, int pass_null_outputs,
                            char* norm_name_out, int* matched_attid_out);
int NetcdfTestHdf5GetChunkCache(int ncid, int pass_null_outputs,
                                size_t* size_out, size_t* nelems_out,
                                float* preemption_out);

// Fault-injection helpers that temporarily inject an invalid handle or name,
// invoke internal hdf5internal.c routines, and restore saved state before
// returning so closing or aborting never leaks resources.
int NetcdfTestFindDimLenErrorPaths(int root_ncid, int target_ncid, int dimid,
                                   int varid, int mode);
int NetcdfTestBreakCoordVarDetachError(int ncid, int dimid, int coord_varid,
                                       int obs_varid);
int NetcdfTestDeleteDimscaleErrorPaths(int ncid, int dimid, int obs_varid,
                                       int mode);
int NetcdfTestReformCoordVarErrorPaths(int ncid, int dimid, int varid,
                                       int obs_varid, int mode);
int NetcdfTestCloseAttError(int ncid, int varid, int attnum);
int NetcdfTestCloseTypeError(int ncid, int nc_typeid, int mode);
int NetcdfTestFindGrpH5VarMetaError(int ncid, int varid);
int NetcdfTestFindGrpVarAttErrorPaths(int ncid, int varid, int mode);
int NetcdfTestHdf5GetChunkCacheInvalidHdfid(int ncid);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_HDF5INTERNAL_SHIM_H_
