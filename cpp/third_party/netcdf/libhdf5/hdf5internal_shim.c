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

#include "third_party/netcdf/google/libhdf5/hdf5internal_shim.h"

#include "third_party/netcdf/config.h"
#include <stdlib.h>
#include <string.h>
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/hdf5internal.h"

static void EnsureHdf5Initialized(void) {
  if (!nc4_hdf5_initialized) {
    nc4_hdf5_initialize();
  }
}

int NetcdfTestHdf5IsInitialized(void) {
  return nc4_hdf5_initialized;
}

int NetcdfTestHdf5InitializeDispatch(void) {
  return NC_HDF5_initialize();
}

int NetcdfTestHdf5FinalizeDispatch(void) {
  return NC_HDF5_finalize();
}

int NetcdfTestFindHdfTypeForNcType(int ncid, int nc_typeid, int* found_out,
                                   int* matched_typeid_out) {
  NC_FILE_INFO_T* h5 = NULL;
  NC_TYPE_INFO_T* type = NULL;
  NC_TYPE_INFO_T* matched = NULL;
  NC_HDF5_TYPE_INFO_T* hdf5_type = NULL;
  hid_t target_hid = 0;
  int retval;

  EnsureHdf5Initialized();
  if ((retval = nc4_find_grp_h5(ncid, NULL, &h5)) != NC_NOERR) {
    return retval;
  }
  if ((retval = nc4_find_type(h5, (nc_type)nc_typeid, &type)) != NC_NOERR) {
    return retval;
  }
  if (!type || !type->format_type_info) {
    return NC_EBADTYPE;
  }
  hdf5_type = (NC_HDF5_TYPE_INFO_T*)type->format_type_info;
  target_hid = hdf5_type->native_hdf_typeid ? hdf5_type->native_hdf_typeid
                                            : hdf5_type->hdf_typeid;
  matched = nc4_rec_find_hdf_type(h5, target_hid);
  if (found_out) {
    *found_out = (matched != NULL) ? 1 : 0;
  }
  if (matched_typeid_out) {
    *matched_typeid_out = matched ? matched->hdr.id : NC_NAT;
  }
  return NC_NOERR;
}

int NetcdfTestFindHdfTypeBuiltinInt(int ncid, int* found_out) {
  NC_FILE_INFO_T* h5 = NULL;
  NC_TYPE_INFO_T* matched = NULL;
  int retval;

  EnsureHdf5Initialized();
  if ((retval = nc4_find_grp_h5(ncid, NULL, &h5)) != NC_NOERR) {
    return retval;
  }
  matched = nc4_rec_find_hdf_type(h5, H5T_NATIVE_INT);
  if (found_out) {
    *found_out = (matched != NULL) ? 1 : 0;
  }
  return NC_NOERR;
}

int NetcdfTestFindHdfTypeInvalidHid(int ncid, int* found_out) {
  NC_FILE_INFO_T* h5 = NULL;
  NC_TYPE_INFO_T* matched = NULL;
  int retval;

  EnsureHdf5Initialized();
  if ((retval = nc4_find_grp_h5(ncid, NULL, &h5)) != NC_NOERR) {
    return retval;
  }
  matched = nc4_rec_find_hdf_type(h5, (hid_t)-1);
  if (found_out) {
    *found_out = (matched != NULL) ? 1 : 0;
  }
  return NC_NOERR;
}

int NetcdfTestReformCoordVarWithDimscale(int ncid, int dimid, int varid) {
  NC_GRP_INFO_T* grp = NULL;
  NC_FILE_INFO_T* h5 = NULL;
  NC_DIM_INFO_T* dim = NULL;
  NC_VAR_INFO_T* var = NULL;
  NC_HDF5_DIM_INFO_T* hdf5_dim = NULL;
  NC_HDF5_VAR_INFO_T* hdf5_var = NULL;
  int retval;

  EnsureHdf5Initialized();
  if ((retval = nc4_find_grp_h5(ncid, &grp, &h5)) != NC_NOERR) {
    return retval;
  }
  if ((retval = nc4_find_dim(grp, dimid, &dim, NULL)) != NC_NOERR) {
    return retval;
  }
  if (!(var = (NC_VAR_INFO_T*)ncindexith(grp->vars, (size_t)varid))) {
    return NC_ENOTVAR;
  }
  hdf5_dim = (NC_HDF5_DIM_INFO_T*)dim->format_dim_info;
  hdf5_var = (NC_HDF5_VAR_INFO_T*)var->format_var_info;
  if (!hdf5_dim || hdf5_dim->hdf_dimscaleid <= 0 || !hdf5_var) {
    return NC_EINVAL;
  }
  return nc4_reform_coord_var(grp, var, dim);
}

int NetcdfTestReformCoordVarWasCoordNoAttached(int ncid, int dimid,
                                               int varid) {
  NC_GRP_INFO_T* grp = NULL;
  NC_FILE_INFO_T* h5 = NULL;
  NC_DIM_INFO_T* dim = NULL;
  NC_VAR_INFO_T* var = NULL;
  NC_HDF5_VAR_INFO_T* hdf5_var = NULL;
  int retval;

  EnsureHdf5Initialized();
  if ((retval = nc4_find_grp_h5(ncid, &grp, &h5)) != NC_NOERR) {
    return retval;
  }
  if ((retval = nc4_find_dim(grp, dimid, &dim, NULL)) != NC_NOERR) {
    return retval;
  }
  if (!(var = (NC_VAR_INFO_T*)ncindexith(grp->vars, (size_t)varid))) {
    return NC_ENOTVAR;
  }
  hdf5_var = (NC_HDF5_VAR_INFO_T*)var->format_var_info;
  if (!hdf5_var || !var->was_coord_var) {
    return NC_EINVAL;
  }
  if (hdf5_var->dimscale_attached) {
    free(hdf5_var->dimscale_attached);
    hdf5_var->dimscale_attached = NULL;
  }
  return nc4_reform_coord_var(grp, var, dim);
}

int NetcdfTestReformCoordVarZeroDimDataset(int ncid, int dimid, int varid) {
  NC_GRP_INFO_T* grp = NULL;
  NC_FILE_INFO_T* h5 = NULL;
  NC_DIM_INFO_T* dim = NULL;
  NC_VAR_INFO_T* var = NULL;
  NC_HDF5_DIM_INFO_T* hdf5_dim = NULL;
  NC_HDF5_VAR_INFO_T* hdf5_var = NULL;
  int retval;

  EnsureHdf5Initialized();
  if ((retval = nc4_find_grp_h5(ncid, &grp, &h5)) != NC_NOERR) {
    return retval;
  }
  if ((retval = nc4_find_dim(grp, dimid, &dim, NULL)) != NC_NOERR) {
    return retval;
  }
  if (!(var = (NC_VAR_INFO_T*)ncindexith(grp->vars, (size_t)varid))) {
    return NC_ENOTVAR;
  }
  hdf5_dim = (NC_HDF5_DIM_INFO_T*)dim->format_dim_info;
  hdf5_var = (NC_HDF5_VAR_INFO_T*)var->format_var_info;
  if (!hdf5_dim || hdf5_dim->hdf_dimscaleid <= 0 || !hdf5_var ||
      !hdf5_var->dimscale_attached || var->ndims == 0) {
    return NC_EINVAL;
  }
  if ((retval = delete_dimscale_dataset(grp, dimid, dim)) != NC_NOERR) {
    return retval;
  }
  /* Leave dimscale_attached[0] set while dim_datasetid == 0 to cover the
   * already-detached guard in nc4_reform_coord_var(). */
  hdf5_var->dimscale_attached[0] = NC_TRUE;
  return nc4_reform_coord_var(grp, var, dim);
}

int NetcdfTestFindGrpH5Var(int ncid, int varid, int pass_null_outputs,
                           int* varid_out) {
  NC_FILE_INFO_T* h5 = NULL;
  NC_GRP_INFO_T* grp = NULL;
  NC_VAR_INFO_T* var = NULL;
  int retval;

  EnsureHdf5Initialized();
  if (pass_null_outputs) {
    retval = nc4_hdf5_find_grp_h5_var(ncid, varid, NULL, NULL, NULL);
    if (retval == NC_NOERR && varid_out) {
      *varid_out = varid;
    }
    return retval;
  }
  retval = nc4_hdf5_find_grp_h5_var(ncid, varid, &h5, &grp, &var);
  if (retval == NC_NOERR && varid_out) {
    *varid_out = var ? var->hdr.id : -1;
  }
  return retval;
}

int NetcdfTestFindGrpVarAtt(int ncid, int varid, const char* name, int attnum,
                            int use_name, int pass_null_outputs,
                            char* norm_name_out, int* matched_attid_out) {
  NC_FILE_INFO_T* h5 = NULL;
  NC_GRP_INFO_T* grp = NULL;
  NC_VAR_INFO_T* var = NULL;
  NC_ATT_INFO_T* att = NULL;
  char norm_name_buf[NC_MAX_NAME + 1];
  int retval;

  EnsureHdf5Initialized();
  memset(norm_name_buf, 0, sizeof(norm_name_buf));
  if (pass_null_outputs) {
    return nc4_hdf5_find_grp_var_att(ncid, varid, name, attnum, use_name, NULL,
                                     NULL, NULL, NULL, NULL);
  }
  retval = nc4_hdf5_find_grp_var_att(ncid, varid, name, attnum, use_name,
                                     norm_name_buf, &h5, &grp, &var, &att);
  if (retval == NC_NOERR) {
    if (norm_name_out) {
      memcpy(norm_name_out, norm_name_buf, sizeof(norm_name_buf));
    }
    if (matched_attid_out) {
      *matched_attid_out = att ? att->hdr.id : -1;
    }
  }
  return retval;
}

int NetcdfTestHdf5GetChunkCache(int ncid, int pass_null_outputs,
                                size_t* size_out, size_t* nelems_out,
                                float* preemption_out) {
  EnsureHdf5Initialized();
  if (pass_null_outputs) {
    return nc4_hdf5_get_chunk_cache(ncid, NULL, NULL, NULL);
  }
  return nc4_hdf5_get_chunk_cache(ncid, size_out, nelems_out, preemption_out);
}

static int LookupGrpDimVar(int ncid, int dimid, int varid,
                           NC_GRP_INFO_T** grp_out, NC_FILE_INFO_T** h5_out,
                           NC_DIM_INFO_T** dim_out, NC_VAR_INFO_T** var_out) {
  NC_GRP_INFO_T* grp = NULL;
  NC_FILE_INFO_T* h5 = NULL;
  NC_DIM_INFO_T* dim = NULL;
  NC_VAR_INFO_T* var = NULL;
  int retval;

  EnsureHdf5Initialized();
  if ((retval = nc4_find_grp_h5(ncid, &grp, &h5)) != NC_NOERR) {
    return retval;
  }
  if (dim_out) {
    if ((retval = nc4_find_dim(grp, dimid, &dim, NULL)) != NC_NOERR) {
      return retval;
    }
    if (!dim->format_dim_info) {
      return NC_EINVAL;
    }
  }
  if (var_out) {
    if (varid < 0 ||
        !(var = (NC_VAR_INFO_T*)ncindexith(grp->vars, (size_t)varid))) {
      return NC_ENOTVAR;
    }
    if (!var->format_var_info) {
      return NC_EINVAL;
    }
  }
  if (grp_out) {
    *grp_out = grp;
  }
  if (h5_out) {
    *h5_out = h5;
  }
  if (dim_out) {
    *dim_out = dim;
  }
  if (var_out) {
    *var_out = var;
  }
  return NC_NOERR;
}

int NetcdfTestFindDimLenErrorPaths(int root_ncid, int target_ncid, int dimid,
                                   int varid, int mode) {
  char missing_dataset_name[] = "__no_such_h5_dataset__";
  NC_GRP_INFO_T* root_grp = NULL;
  NC_GRP_INFO_T* target_grp = NULL;
  NC_VAR_INFO_T* var = NULL;
  NC_HDF5_VAR_INFO_T* hdf5_var = NULL;
  size_t len = 0;
  size_t* lenp = &len;
  int retval;

  if ((retval = LookupGrpDimVar(root_ncid, -1, -1, &root_grp, NULL, NULL,
                                NULL)) != NC_NOERR) {
    return retval;
  }
  if ((retval = LookupGrpDimVar(target_ncid, -1, varid, &target_grp, NULL,
                                NULL, &var)) != NC_NOERR) {
    return retval;
  }
  hdf5_var = (NC_HDF5_VAR_INFO_T*)var->format_var_info;
  if (!var->created) {
    return NC_EINVAL;
  }
  if (mode == 0) {
    /* Force nc4_open_var_grp2() failure inside find_var_dim_max_length(), and
     * restore the original open hdf_datasetid (H5Dopen2 leaves -1 on error). */
    char* orig_name = var->hdr.name;
    hid_t orig_dsid = hdf5_var->hdf_datasetid;
    var->hdr.name = missing_dataset_name;
    hdf5_var->hdf_datasetid = 0;
    retval = nc4_find_dim_len(root_grp, dimid, &lenp);
    var->hdr.name = orig_name;
    hdf5_var->hdf_datasetid = orig_dsid;
    return retval;
  }
  if (mode == 1) {
    hid_t orig_dsid = hdf5_var->hdf_datasetid;
    hdf5_var->hdf_datasetid = (hid_t)-1;
    retval = nc4_find_dim_len(target_grp, dimid, &lenp);
    hdf5_var->hdf_datasetid = orig_dsid;
    return retval;
  }
  if (mode == 2) {
    size_t orig_ndims = var->ndims;
    if (orig_ndims == 0) {
      return NC_EINVAL;
    }
    var->ndims = orig_ndims + 1;
    retval = nc4_find_dim_len(target_grp, dimid, &lenp);
    var->ndims = orig_ndims;
    return retval;
  }
  return NC_EINVAL;
}

int NetcdfTestBreakCoordVarDetachError(int ncid, int dimid, int coord_varid,
                                       int obs_varid) {
  NC_GRP_INFO_T* grp = NULL;
  NC_DIM_INFO_T* dim = NULL;
  NC_VAR_INFO_T* coord_var = NULL;
  NC_VAR_INFO_T* obs_var = NULL;
  NC_HDF5_DIM_INFO_T* hdf5_dim = NULL;
  NC_HDF5_VAR_INFO_T* coord_h5var = NULL;
  NC_HDF5_VAR_INFO_T* obs_h5var = NULL;
  hid_t orig_obs_dsid;
  int retval;

  if ((retval = LookupGrpDimVar(ncid, dimid, coord_varid, &grp, NULL, &dim,
                                &coord_var)) != NC_NOERR) {
    return retval;
  }
  if ((retval = LookupGrpDimVar(ncid, -1, obs_varid, NULL, NULL, NULL,
                                &obs_var)) != NC_NOERR) {
    return retval;
  }
  hdf5_dim = (NC_HDF5_DIM_INFO_T*)dim->format_dim_info;
  coord_h5var = (NC_HDF5_VAR_INFO_T*)coord_var->format_var_info;
  obs_h5var = (NC_HDF5_VAR_INFO_T*)obs_var->format_var_info;
  if (dim->coord_var != coord_var || coord_var->ndims == 0 ||
      coord_var->dim[0] != dim || coord_var->dimids[0] != dim->hdr.id ||
      hdf5_dim->hdf_dimscaleid != 0 || coord_h5var->hdf_datasetid <= 0 ||
      !obs_var->created || obs_var->ndims == 0 ||
      obs_var->dimids[0] != dim->hdr.id || obs_h5var->hdf_datasetid <= 0 ||
      !obs_h5var->dimscale_attached || !obs_h5var->dimscale_attached[0]) {
    return NC_EINVAL;
  }
  orig_obs_dsid = obs_h5var->hdf_datasetid;
  obs_h5var->hdf_datasetid = (hid_t)-1;
  retval = nc4_break_coord_var(grp, coord_var, dim);
  obs_h5var->hdf_datasetid = orig_obs_dsid;
  return retval;
}

int NetcdfTestDeleteDimscaleErrorPaths(int ncid, int dimid, int obs_varid,
                                       int mode) {
  char missing_dimscale_name[] = "__no_such_dimscale__";
  NC_GRP_INFO_T* grp = NULL;
  NC_DIM_INFO_T* dim = NULL;
  NC_HDF5_DIM_INFO_T* hdf5_dim = NULL;
  int retval;

  if ((retval = LookupGrpDimVar(ncid, dimid, -1, &grp, NULL, &dim, NULL)) !=
      NC_NOERR) {
    return retval;
  }
  hdf5_dim = (NC_HDF5_DIM_INFO_T*)dim->format_dim_info;
  if (hdf5_dim->hdf_dimscaleid <= 0) {
    return NC_EINVAL;
  }
  if (mode == 0) {
    NC_VAR_INFO_T* obs_var = NULL;
    NC_HDF5_VAR_INFO_T* obs_h5var = NULL;
    hid_t orig_obs_dsid;
    if ((retval = LookupGrpDimVar(ncid, -1, obs_varid, NULL, NULL, NULL,
                                  &obs_var)) != NC_NOERR) {
      return retval;
    }
    obs_h5var = (NC_HDF5_VAR_INFO_T*)obs_var->format_var_info;
    if (!obs_var->created || obs_h5var->hdf_datasetid <= 0 ||
        obs_var->ndims == 0 || !obs_h5var->dimscale_attached ||
        !obs_h5var->dimscale_attached[0]) {
      return NC_EINVAL;
    }
    orig_obs_dsid = obs_h5var->hdf_datasetid;
    obs_h5var->hdf_datasetid = (hid_t)-1;
    retval = delete_dimscale_dataset(grp, dimid, dim);
    obs_h5var->hdf_datasetid = orig_obs_dsid;
    return retval;
  }
  if (mode == 1) {
    /* Pass 0 (>= 0) so rec_detach_scales() assertion holds while
     * H5Dclose(0) fails. */
    hid_t orig_dimscale = hdf5_dim->hdf_dimscaleid;
    hdf5_dim->hdf_dimscaleid = 0;
    retval = delete_dimscale_dataset(grp, dimid, dim);
    hdf5_dim->hdf_dimscaleid = orig_dimscale;
    return retval;
  }
  if (mode == 2) {
    char* orig_name = dim->hdr.name;
    dim->hdr.name = missing_dimscale_name;
    retval = delete_dimscale_dataset(grp, dimid, dim);
    dim->hdr.name = orig_name;
    return retval;
  }
  return NC_EINVAL;
}

int NetcdfTestReformCoordVarErrorPaths(int ncid, int dimid, int varid,
                                        int obs_varid, int mode) {
  char missing_dimscale_name[] = "__no_such_dimscale__";
  NC_GRP_INFO_T* grp = NULL;
  NC_DIM_INFO_T* dim = NULL;
  NC_VAR_INFO_T* var = NULL;
  NC_HDF5_DIM_INFO_T* hdf5_dim = NULL;
  NC_HDF5_VAR_INFO_T* hdf5_var = NULL;
  int retval;

  if ((retval = LookupGrpDimVar(ncid, dimid, varid, &grp, NULL, &dim, &var)) !=
      NC_NOERR) {
    return retval;
  }
  hdf5_dim = (NC_HDF5_DIM_INFO_T*)dim->format_dim_info;
  hdf5_var = (NC_HDF5_VAR_INFO_T*)var->format_var_info;
  if (mode == 0) {
    hid_t orig_dsid = hdf5_var->hdf_datasetid;
    if (var->ndims == 0 || var->dimids[0] != dim->hdr.id ||
        !hdf5_var->dimscale_attached || !hdf5_var->dimscale_attached[0] ||
        hdf5_dim->hdf_dimscaleid <= 0 || orig_dsid <= 0) {
      return NC_EINVAL;
    }
    hdf5_var->hdf_datasetid = (hid_t)-1;
    retval = nc4_reform_coord_var(grp, var, dim);
    hdf5_var->hdf_datasetid = orig_dsid;
    return retval;
  }
  if (mode == 1 || mode == 2) {
    if (hdf5_dim->hdf_dimscaleid <= 0) {
      return NC_EINVAL;
    }
    if (hdf5_var->dimscale_attached) {
      free(hdf5_var->dimscale_attached);
      hdf5_var->dimscale_attached = NULL;
    }
    if (mode == 1) {
      hid_t orig_dimscale = hdf5_dim->hdf_dimscaleid;
      hdf5_dim->hdf_dimscaleid = (hid_t)-1;
      retval = nc4_reform_coord_var(grp, var, dim);
      hdf5_dim->hdf_dimscaleid = orig_dimscale;
      return retval;
    }
    {
      char* orig_name = dim->hdr.name;
      dim->hdr.name = missing_dimscale_name;
      retval = nc4_reform_coord_var(grp, var, dim);
      dim->hdr.name = orig_name;
      return retval;
    }
  }
  if (mode == 3) {
    NC_VAR_INFO_T* obs_var = NULL;
    NC_HDF5_VAR_INFO_T* obs_h5var = NULL;
    nc_bool_t orig_dimscale = hdf5_var->dimscale;
    NC_VAR_INFO_T* orig_coord_var = dim->coord_var;
    hid_t orig_obs_dsid;
    if ((retval = LookupGrpDimVar(ncid, -1, obs_varid, NULL, NULL, NULL,
                                  &obs_var)) != NC_NOERR) {
      return retval;
    }
    obs_h5var = (NC_HDF5_VAR_INFO_T*)obs_var->format_var_info;
    if (!var->was_coord_var || var->ndims == 0 ||
        var->dimids[0] != dim->hdr.id || hdf5_dim->hdf_dimscaleid != 0 ||
        hdf5_var->hdf_datasetid <= 0 || !obs_var->created ||
        obs_h5var->hdf_datasetid <= 0) {
      return NC_EINVAL;
    }
    if (hdf5_var->dimscale_attached) {
      free(hdf5_var->dimscale_attached);
      hdf5_var->dimscale_attached = NULL;
    }
    orig_obs_dsid = obs_h5var->hdf_datasetid;
    obs_h5var->hdf_datasetid = (hid_t)-1;
    retval = nc4_reform_coord_var(grp, var, dim);
    obs_h5var->hdf_datasetid = orig_obs_dsid;
    hdf5_var->dimscale = orig_dimscale;
    dim->coord_var = orig_coord_var;
    return retval;
  }
  return NC_EINVAL;
}

int NetcdfTestCloseAttError(int ncid, int varid, int attnum) {
  NC_GRP_INFO_T* grp = NULL;
  NCindex* attlist = NULL;
  NC_ATT_INFO_T* att = NULL;
  NC_HDF5_ATT_INFO_T* hdf5_att = NULL;
  hid_t orig_native;
  int retval;

  if ((retval = LookupGrpDimVar(ncid, -1, -1, &grp, NULL, NULL, NULL)) !=
      NC_NOERR) {
    return retval;
  }
  if (varid == NC_GLOBAL) {
    attlist = grp->att;
  } else {
    NC_VAR_INFO_T* var = NULL;
    if ((retval = LookupGrpDimVar(ncid, -1, varid, NULL, NULL, NULL, &var)) !=
        NC_NOERR) {
      return retval;
    }
    attlist = var->att;
  }
  if (attnum < 0 ||
      !(att = (NC_ATT_INFO_T*)ncindexith(attlist, (size_t)attnum)) ||
      !att->format_att_info) {
    return NC_ENOTATT;
  }
  hdf5_att = (NC_HDF5_ATT_INFO_T*)att->format_att_info;
  orig_native = hdf5_att->native_hdf_typeid;
  hdf5_att->native_hdf_typeid = (hid_t)-1;
  retval = nc4_HDF5_close_att(att);
  hdf5_att->native_hdf_typeid = orig_native;
  return retval;
}

int NetcdfTestCloseTypeError(int ncid, int nc_typeid, int mode) {
  NC_FILE_INFO_T* h5 = NULL;
  NC_TYPE_INFO_T* type = NULL;
  NC_HDF5_TYPE_INFO_T* hdf5_type = NULL;
  hid_t orig_hdf;
  hid_t orig_native;
  int retval;

  if ((retval = LookupGrpDimVar(ncid, -1, -1, NULL, &h5, NULL, NULL)) !=
      NC_NOERR) {
    return retval;
  }
  if ((retval = nc4_find_type(h5, (nc_type)nc_typeid, &type)) != NC_NOERR) {
    return retval;
  }
  if (!type || !type->format_type_info) {
    return NC_EBADTYPE;
  }
  hdf5_type = (NC_HDF5_TYPE_INFO_T*)type->format_type_info;
  orig_hdf = hdf5_type->hdf_typeid;
  orig_native = hdf5_type->native_hdf_typeid;
  if (mode == 0) {
    hdf5_type->hdf_typeid = (hid_t)-1;
    retval = nc4_HDF5_close_type(type);
  } else if (mode == 1) {
    hdf5_type->hdf_typeid = 0;
    hdf5_type->native_hdf_typeid = (hid_t)-1;
    retval = nc4_HDF5_close_type(type);
  } else {
    return NC_EINVAL;
  }
  hdf5_type->hdf_typeid = orig_hdf;
  hdf5_type->native_hdf_typeid = orig_native;
  return retval;
}

int NetcdfTestFindGrpH5VarMetaError(int ncid, int varid) {
  NC_VAR_INFO_T* var = NULL;
  NC_HDF5_VAR_INFO_T* hdf5_var = NULL;
  hid_t orig_dsid;
  int retval;

  if ((retval = LookupGrpDimVar(ncid, -1, varid, NULL, NULL, NULL, &var)) !=
      NC_NOERR) {
    return retval;
  }
  if (var->meta_read || !var->created) {
    return NC_EINVAL;
  }
  hdf5_var = (NC_HDF5_VAR_INFO_T*)var->format_var_info;
  orig_dsid = hdf5_var->hdf_datasetid;
  hdf5_var->hdf_datasetid = (hid_t)-1;
  retval = nc4_hdf5_find_grp_h5_var(ncid, varid, NULL, NULL, NULL);
  hdf5_var->hdf_datasetid = orig_dsid;
  return retval;
}

int NetcdfTestFindGrpVarAttErrorPaths(int ncid, int varid, int mode) {
  NC_GRP_INFO_T* grp = NULL;
  NC_HDF5_GRP_INFO_T* hdf5_grp = NULL;
  NC_VAR_INFO_T* var = NULL;
  NC_HDF5_VAR_INFO_T* hdf5_var = NULL;
  int retval;

  if ((retval = LookupGrpDimVar(ncid, -1, -1, &grp, NULL, NULL, NULL)) !=
      NC_NOERR) {
    return retval;
  }
  hdf5_grp = (NC_HDF5_GRP_INFO_T*)grp->format_grp_info;
  if (!hdf5_grp) {
    return NC_EINVAL;
  }
  if (mode == 0) {
    hid_t orig_grpid = hdf5_grp->hdf_grpid;
    if (grp->atts_read) {
      return NC_EINVAL;
    }
    hdf5_grp->hdf_grpid = (hid_t)-1;
    retval = nc4_hdf5_find_grp_var_att(ncid, NC_GLOBAL, NULL, 0, 0, NULL, NULL,
                                       NULL, NULL, NULL);
    hdf5_grp->hdf_grpid = orig_grpid;
    return retval;
  }
  if ((retval = LookupGrpDimVar(ncid, -1, varid, NULL, NULL, NULL, &var)) !=
      NC_NOERR) {
    return retval;
  }
  hdf5_var = (NC_HDF5_VAR_INFO_T*)var->format_var_info;
  if (mode == 1) {
    hid_t orig_dsid = hdf5_var->hdf_datasetid;
    if (var->atts_read) {
      return NC_EINVAL;
    }
    hdf5_var->hdf_datasetid = (hid_t)-1;
    retval = nc4_hdf5_find_grp_var_att(ncid, varid, NULL, 0, 0, NULL, NULL,
                                       NULL, NULL, NULL);
    hdf5_var->hdf_datasetid = orig_dsid;
    return retval;
  }
  if (mode == 2) {
    int orig_atts_read = var->atts_read;
    hid_t orig_dsid = hdf5_var->hdf_datasetid;
    if (var->meta_read || !var->created) {
      return NC_EINVAL;
    }
    var->atts_read = 1;
    hdf5_var->hdf_datasetid = (hid_t)-1;
    retval = nc4_hdf5_find_grp_var_att(ncid, varid, NULL, 0, 0, NULL, NULL,
                                       NULL, NULL, NULL);
    hdf5_var->hdf_datasetid = orig_dsid;
    var->atts_read = orig_atts_read;
    return retval;
  }
  return NC_EINVAL;
}

int NetcdfTestHdf5GetChunkCacheInvalidHdfid(int ncid) {
  NC_FILE_INFO_T* h5 = NULL;
  NC_HDF5_FILE_INFO_T* hdf5_info = NULL;
  hid_t orig_hdfid;
  size_t size = 0;
  size_t nelems = 0;
  float preemption = 0.0f;
  int retval;

  if ((retval = LookupGrpDimVar(ncid, -1, -1, NULL, &h5, NULL, NULL)) !=
      NC_NOERR) {
    return retval;
  }
  hdf5_info = (NC_HDF5_FILE_INFO_T*)h5->format_file_info;
  if (!hdf5_info) {
    return NC_EINVAL;
  }
  orig_hdfid = hdf5_info->hdfid;
  hdf5_info->hdfid = (hid_t)-1;
  retval = nc4_hdf5_get_chunk_cache(ncid, &size, &nelems, &preemption);
  hdf5_info->hdfid = orig_hdfid;
  return retval;
}
