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

#ifndef THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_NC4HDF_SHIM_H_
#define THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_NC4HDF_SHIM_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Ensures an in-place test XOR HDF5 filter is registered idempotently.
int Nc4HdfEnsureCustomFilterRegistered(unsigned int filter_id);

// Clears precomputed chunksizes so var_create_dataset computes defaults itself.
int Nc4HdfZeroVarChunksizes(int ncid, int varid);

// Sets internal quantize_mode/nsd to exercise write_quantize_att error paths.
int Nc4HdfSetVarQuantizeMode(int ncid, int varid, int quantize_mode, int nsd);

// Injects a filter entry directly via NC4_hdf5_addfilter for error testing.
int Nc4HdfInjectBadFilterParams(int ncid, int varid, unsigned int filter_id,
                                size_t nparams, const unsigned int* params);

// Sets var->was_coord_var directly for testing detach on already-attached vars.
int Nc4HdfSetVarWasCoordVar(int ncid, int varid, int was_coord_var);

// Exercises nc4_open_var_grp2 on bad, uncreated, closed, and open datasets.
int Nc4HdfTestOpenVarGrp2(int ncid, int created_varid, int uncreated_varid,
                          int bad_varid);

// Exercises nc4_get_hdf_typeid directly for given xtype and endianness.
int Nc4HdfTestGetHdfTypeid(int ncid, int xtype, int endianness);

// Exercises NC_EDIMSCALE error returns from
// rec_detach_scales/rec_reattach_scales.
int Nc4HdfTestRecScalesError(int ncid, int dimid);

// Exercises nc4_adjust_var_cache when type_info->size is zero (NC_STRING path).
int Nc4HdfTestAdjustVarCacheWithZeroTypeSize(int ncid, int varid,
                                             size_t* out_cache_size);

// Exercises showopenobjects, showopenobjects5, reportopenobjects, reportobject.
int Nc4HdfTestShowOpenObjects(int ncid, int grpid, const char* attr_name);

// Creates a raw HDF5 file without _NCProperties/_nc3_strict to test NC4_walk.
// mode == 0: below threshold (< 2 reserved markers -> NC4_isnetcdf4 == 0).
// mode == 1: at/above threshold (>= 2 markers + phony_dim -> NC4_isnetcdf4 ==
// 1).
int Nc4HdfCreateRawHdf5File(const char* path, int mode);

// Creates a raw HDF5 file with dimscales, cross-group attachment, and
// unattached datasets for testing nc4_rec_match_dimscales.
int Nc4HdfCreateRawHdf5ForDimscaleMatch(const char* path);

// Exercises nc4_rec_match_dimscales on an opened raw HDF5 file.
int Nc4HdfTestRecMatchDimscales(int ncid, int* out_phony_ndims,
                                int* out_sub_dim_matched);

// Exercises NC4_hdf5get_libversion, NC4_hdf5get_superblock, and NC4_isnetcdf4.
int Nc4HdfGetProvenanceDirect(int ncid, unsigned* major, unsigned* minor,
                              unsigned* release, int* superblock, int* is_nc4,
                              int* bad_superblock_err);

// Checks whether an HDF5 attribute exists on a variable dataset.
int Nc4HdfVarHasAttr(int ncid, int varid, const char* attr_name, int* exists);

// Checks whether an HDF5 attribute exists on a dimscale dataset.
int Nc4HdfDimHasAttr(int ncid, const char* dim_name, const char* attr_name,
                     int* exists);

// Resets negative hdf_datasetid (e.g. -1 after failed H5Dcreate2) to 0 so
// nc_abort() does not call H5Dclose(-1) in close_vars().
int Nc4HdfClearInvalidDatasetId(int ncid, int varid);

// Deletes an HDF5 attribute from a created variable dataset if it exists.
int Nc4HdfDeleteVarAttr(int ncid, int varid, const char* attr_name);

// Exercises non-leaking error paths in commit_type(), create_group(),
// write_nc3_strict_att(), and nc4_rec_write_groups_types().
int Nc4HdfTestCommitTypeAndGroupErrors(const char* ro_plain_path);

// Exercises error paths that intentionally abort in a forked child process:
//   0: nc4_adjust_var_cache reopen error
//   1: commit_type NC_COMPOUND H5Tinsert offset error
//   2: commit_type NC_ENUM H5Tenum_create float base error
//   3: commit_type NC_ENUM duplicate member H5Tenum_insert error
//   5: nc4_rec_match_dimscales H5S_NULL dataspace dataset error
//   6: nc4_rec_match_dimscales var->ndims rank mismatch error
//   7: nc4_rec_match_dimscales non-scalar dataset with ndims == 0 error
int Nc4HdfTestForkedErrorSubcase(int subcase);

// Exercises error paths in flag_atts_dirty(), nc4_get_hdf_typeid(),
// put_att_grpa(), write_attlist(), write_netcdf4_dimid(), var_create_dataset(),
// attach_dimscales(), var_exists(), remove_coord_atts(), write_var(),
// nc4_create_dim_wo_var(), write_dim(), and nc4_rec_write_metadata().
int Nc4HdfTestWriteMetadataAndVarErrors(const char* ro_attrs_path);

// Exercises error paths in nc4_rec_match_dimscales(), NC4_isnetcdf4(), and
// NC4_root_att_exists() without leaks.
int Nc4HdfTestMatchDimscalesAndIsNc4Errors(const char* raw_file_path);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // THIRD_PARTY_NETCDF_GOOGLE_LIBHDF5_NC4HDF_SHIM_H_
