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

#include "third_party/netcdf/google/libhdf5/nc4hdf_shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "third_party/netcdf/config.h"
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
#include "third_party/netcdf/include/nc4internal.h"
#include "third_party/netcdf/include/hdf5internal.h"
#include "third_party/netcdf/include/nclist.h"

/* External symbols in libhdf5/nc4hdf.c not listed in hdf5internal.h. */
extern int nc4_rec_match_dimscales(NC_GRP_INFO_T *grp);
extern void reportobject(int uselog, hid_t id, unsigned int type);
extern void showopenobjects5(NC_FILE_INFO_T *h5);
extern void showopenobjects(int ncid);

static void EnsureHdf5Init(void) {
  if (!nc4_hdf5_initialized) {
    nc4_hdf5_initialize();
  }
}

static size_t CustomXorFilter(unsigned int flags, size_t cd_nelmts,
                              const unsigned int cd_values[], size_t nbytes,
                              size_t *buf_size, void **buf) {
  unsigned char key =
      (unsigned char)(cd_nelmts > 0 ? (cd_values[0] & 0xffu) : 0x5au);
  unsigned char *bytes = (unsigned char *)(*buf);
  (void)flags;
  (void)buf_size;
  if (!bytes) {
    return 0;
  }
  for (size_t i = 0; i < nbytes; ++i) {
    bytes[i] ^= key;
  }
  return nbytes;
}

int Nc4HdfEnsureCustomFilterRegistered(unsigned int filter_id) {
  htri_t avail;
  H5Z_class2_t cls;
  EnsureHdf5Init();
  avail = H5Zfilter_avail((H5Z_filter_t)filter_id);
  if (avail < 0) {
    return NC_EHDFERR;
  }
  if (avail > 0) {
    return NC_NOERR;
  }
  cls.version = H5Z_CLASS_T_VERS;
  cls.id = (H5Z_filter_t)filter_id;
  cls.encoder_present = 1;
  cls.decoder_present = 1;
  cls.name = "google_test_xor";
  cls.can_apply = NULL;
  cls.set_local = NULL;
  cls.filter = CustomXorFilter;
  if (H5Zregister(&cls) < 0) {
    return NC_EHDFERR;
  }
  return NC_NOERR;
}

int Nc4HdfZeroVarChunksizes(int ncid, int varid) {
  NC_FILE_INFO_T *h5 = NULL;
  NC_GRP_INFO_T *grp = NULL;
  NC_VAR_INFO_T *var = NULL;
  int retval = nc4_find_grp_h5_var(ncid, varid, &h5, &grp, &var);
  (void)h5;
  (void)grp;
  if (retval != NC_NOERR) {
    return retval;
  }
  if (var->chunksizes != NULL) {
    for (size_t d = 0; d < var->ndims; ++d) {
      var->chunksizes[d] = 0;
    }
  }
  return NC_NOERR;
}

int Nc4HdfSetVarQuantizeMode(int ncid, int varid, int quantize_mode, int nsd) {
  NC_FILE_INFO_T *h5 = NULL;
  NC_GRP_INFO_T *grp = NULL;
  NC_VAR_INFO_T *var = NULL;
  int retval = nc4_find_grp_h5_var(ncid, varid, &h5, &grp, &var);
  (void)h5;
  (void)grp;
  if (retval != NC_NOERR) {
    return retval;
  }
  var->quantize_mode = quantize_mode;
  var->nsd = nsd;
  return NC_NOERR;
}

int Nc4HdfInjectBadFilterParams(int ncid, int varid, unsigned int filter_id,
                                size_t nparams, const unsigned int *params) {
  NC_FILE_INFO_T *h5 = NULL;
  NC_GRP_INFO_T *grp = NULL;
  NC_VAR_INFO_T *var = NULL;
  int retval = nc4_find_grp_h5_var(ncid, varid, &h5, &grp, &var);
  (void)h5;
  (void)grp;
  if (retval != NC_NOERR) {
    return retval;
  }
  return NC4_hdf5_addfilter(var, filter_id, nparams, params, 0);
}

int Nc4HdfSetVarWasCoordVar(int ncid, int varid, int was_coord_var) {
  NC_FILE_INFO_T *h5 = NULL;
  NC_GRP_INFO_T *grp = NULL;
  NC_VAR_INFO_T *var = NULL;
  int retval = nc4_find_grp_h5_var(ncid, varid, &h5, &grp, &var);
  (void)h5;
  (void)grp;
  if (retval != NC_NOERR) {
    return retval;
  }
  var->was_coord_var = was_coord_var ? NC_TRUE : NC_FALSE;
  return NC_NOERR;
}

int Nc4HdfTestOpenVarGrp2(int ncid, int created_varid, int uncreated_varid,
                           int bad_varid) {
  NC_GRP_INFO_T *grp = NULL;
  NC_VAR_INFO_T *created_var = NULL;
  NC_VAR_INFO_T *uncreated_var = NULL;
  NC_HDF5_VAR_INFO_T *created_h5 = NULL;
  NC_HDF5_VAR_INFO_T *uncreated_h5 = NULL;
  hid_t ds1 = 0;
  hid_t ds2 = 0;
  int retval;

  retval = nc4_find_grp_h5(ncid, &grp, NULL);
  if (retval != NC_NOERR) {
    return retval;
  }

  /* Invalid varid -> NC_ENOTVAR. */
  if (nc4_open_var_grp2(grp, bad_varid, &ds1) != NC_ENOTVAR) {
    return NC_EINTERNAL;
  }

  /* Uncreated varid -> H5Dopen2 fails -> NC_ENOTVAR. Reset hdf_datasetid back
   * to 0 so nc_close / nc_abort does not attempt H5Dclose(-1). */
  uncreated_var = (NC_VAR_INFO_T *)ncindexith(grp->vars, (size_t)uncreated_varid);
  if (!uncreated_var || !uncreated_var->format_var_info) {
    return NC_ENOTVAR;
  }
  uncreated_h5 = (NC_HDF5_VAR_INFO_T *)uncreated_var->format_var_info;
  if (nc4_open_var_grp2(grp, uncreated_varid, &ds1) != NC_ENOTVAR) {
    uncreated_h5->hdf_datasetid = 0;
    return NC_EINTERNAL;
  }
  uncreated_h5->hdf_datasetid = 0;

  /* Created varid: close dataset handle temporarily, reopen via nc4_open_var_grp2,
   * and verify cached handle reuse on second call. */
  created_var = (NC_VAR_INFO_T *)ncindexith(grp->vars, (size_t)created_varid);
  if (!created_var || !created_var->format_var_info) {
    return NC_ENOTVAR;
  }
  created_h5 = (NC_HDF5_VAR_INFO_T *)created_var->format_var_info;
  if (created_h5->hdf_datasetid > 0) {
    if (H5Dclose(created_h5->hdf_datasetid) < 0) {
      return NC_EHDFERR;
    }
    created_h5->hdf_datasetid = 0;
  }
  retval = nc4_open_var_grp2(grp, created_varid, &ds1);
  if (retval != NC_NOERR || ds1 <= 0) {
    return retval ? retval : NC_EHDFERR;
  }
  retval = nc4_open_var_grp2(grp, created_varid, &ds2);
  if (retval != NC_NOERR || ds2 != ds1) {
    return retval ? retval : NC_EHDFERR;
  }
  return NC_NOERR;
}

int Nc4HdfTestGetHdfTypeid(int ncid, int xtype, int endianness) {
  NC_FILE_INFO_T *h5 = NULL;
  hid_t hdf_typeid = -1;
  int retval = nc4_find_grp_h5(ncid, NULL, &h5);
  if (retval != NC_NOERR) {
    return retval;
  }
  retval = nc4_get_hdf_typeid(h5, (nc_type)xtype, &hdf_typeid, endianness);
  if (retval == NC_NOERR && hdf_typeid >= 0) {
    if (H5Tclose(hdf_typeid) < 0) {
      return NC_EHDFERR;
    }
  }
  return retval;
}

int Nc4HdfTestRecScalesError(int ncid, int dimid) {
  NC_FILE_INFO_T *h5 = NULL;
  int retval = nc4_find_grp_h5(ncid, NULL, &h5);
  if (retval != NC_NOERR) {
    return retval;
  }
  /* dimscaleid = 0 satisfies dimscaleid >= 0 assertions while H5DS returns < 0. */
  if (rec_detach_scales(h5->root_grp, dimid, (hid_t)0) != NC_EDIMSCALE) {
    return NC_EINTERNAL;
  }
  if (rec_reattach_scales(h5->root_grp, dimid, (hid_t)0) != NC_EDIMSCALE) {
    return NC_EINTERNAL;
  }
  return NC_NOERR;
}

int Nc4HdfTestAdjustVarCacheWithZeroTypeSize(int ncid, int varid,
                                             size_t *out_cache_size) {
  NC_FILE_INFO_T *h5 = NULL;
  NC_GRP_INFO_T *grp = NULL;
  NC_VAR_INFO_T *var = NULL;
  size_t saved_size;
  int retval = nc4_find_grp_h5_var(ncid, varid, &h5, &grp, &var);
  (void)h5;
  if (retval != NC_NOERR) {
    return retval;
  }
  saved_size = var->type_info->size;
  var->type_info->size = 0;
  var->chunkcache.size = CHUNK_CACHE_SIZE;
  retval = nc4_adjust_var_cache(grp, var);
  var->type_info->size = saved_size;
  if (out_cache_size != NULL) {
    *out_cache_size = var->chunkcache.size;
  }
  return retval;
}

int Nc4HdfTestShowOpenObjects(int ncid, int grpid, const char *attr_name) {
  NC_FILE_INFO_T *h5 = NULL;
  NC_GRP_INFO_T *grp = NULL;
  NC_HDF5_GRP_INFO_T *hdf5_grp = NULL;
  hid_t attid = -1;
  int retval;

  /* Invalid ncid prints "failed\n" cleanly without error. */
  showopenobjects(-1);

  retval = nc4_find_grp_h5(grpid, &grp, &h5);
  if (retval != NC_NOERR) {
    return retval;
  }
  (void)h5;
  hdf5_grp = (NC_HDF5_GRP_INFO_T *)grp->format_grp_info;

  /* Open an HDF5 attribute temporarily so H5F_OBJ_ATTR branch is hit. */
  if (attr_name != NULL && hdf5_grp->hdf_grpid > 0) {
    attid = H5Aopen(hdf5_grp->hdf_grpid, attr_name, H5P_DEFAULT);
  }

  showopenobjects(ncid);

  /* Exercise unknown object type and invalid object id paths in reportobject. */
  if (hdf5_grp->hdf_grpid > 0) {
    reportobject(0, hdf5_grp->hdf_grpid, 9999u);
  }
  reportobject(0, (hid_t)-1, H5F_OBJ_FILE);

  if (attid >= 0) {
    H5Aclose(attid);
  }
  return NC_NOERR;
}

static herr_t WriteScalarIntAttr(hid_t loc_id, const char *name, int value) {
  hid_t space_id = H5Screate(H5S_SCALAR);
  hid_t att_id = -1;
  herr_t err = -1;
  if (space_id < 0) {
    return -1;
  }
  att_id = H5Acreate1(loc_id, name, H5T_NATIVE_INT, space_id, H5P_DEFAULT);
  if (att_id >= 0) {
    err = H5Awrite(att_id, H5T_NATIVE_INT, &value);
    H5Aclose(att_id);
  }
  H5Sclose(space_id);
  return err;
}

int Nc4HdfCreateRawHdf5File(const char *path, int mode) {
  hid_t file_id = -1;
  hid_t grp_id = -1;
  hid_t space_id = -1;
  hid_t type_id = -1;
  hid_t dset1 = -1;
  hid_t dset2 = -1;
  hsize_t dims[1] = {4};
  int retval = NC_NOERR;

  if (path == NULL) {
    return NC_EINVAL;
  }
  EnsureHdf5Init();

  file_id = H5Fcreate(path, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  if (file_id < 0) {
    return NC_EHDFERR;
  }

  /* Committed HDF5 opaque datatype exercises NC4_walk default: break on H5G_TYPE
   * while remaining readable by netCDF-4 open (unlike bare committed int). */
  type_id = H5Tcreate(H5T_OPAQUE, 4);
  if (type_id < 0 || H5Tset_tag(type_id, "test_opaque") < 0 ||
      H5Tcommit2(file_id, "my_opaque_t", type_id, H5P_DEFAULT, H5P_DEFAULT,
                 H5P_DEFAULT) < 0) {
    retval = NC_EHDFERR;
    goto cleanup;
  }

  grp_id = H5Gcreate2(file_id, "grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  space_id = H5Screate_simple(1, dims, NULL);
  if (grp_id < 0 || space_id < 0) {
    retval = NC_EHDFERR;
    goto cleanup;
  }

  if (mode == 0) {
    /* Only 1 reserved attr across the entire file -> count == 1 (< 2). */
    dset1 = H5Dcreate2(file_id, "plain_data", H5T_NATIVE_INT, space_id,
                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    dset2 = H5Dcreate2(grp_id, "one_attr", H5T_NATIVE_INT, space_id,
                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (dset1 < 0 || dset2 < 0 ||
        WriteScalarIntAttr(dset2, NC_DIMID_ATT_NAME, 0) < 0) {
      retval = NC_EHDFERR;
      goto cleanup;
    }
  } else {
    /* Dataset named "phony_dim" (+1) and H5DSset_scale adds CLASS + NAME (+2). */
    dset1 = H5Dcreate2(file_id, "phony_dim", H5T_NATIVE_INT, space_id,
                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    dset2 = H5Dcreate2(grp_id, "marked_data", H5T_NATIVE_INT, space_id,
                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (dset1 < 0 || dset2 < 0 || H5DSset_scale(dset2, "scale") < 0) {
      retval = NC_EHDFERR;
      goto cleanup;
    }
  }

cleanup:
  if (dset2 >= 0) H5Dclose(dset2);
  if (dset1 >= 0) H5Dclose(dset1);
  if (space_id >= 0) H5Sclose(space_id);
  if (grp_id >= 0) H5Gclose(grp_id);
  if (type_id >= 0) H5Tclose(type_id);
  if (file_id >= 0) H5Fclose(file_id);
  return retval;
}

int Nc4HdfCreateRawHdf5ForDimscaleMatch(const char *path) {
  hid_t file_id = -1;
  hid_t sub_matched = -1;
  hid_t phony_grp = -1;
  hid_t s4 = -1, s2 = -1, s5 = -1, s3 = -1, s_scalar = -1, s_unlim = -1;
  hid_t unlim_dcpl = -1;
  hid_t scale0 = -1, local_scale = -1;
  hid_t root_matched = -1, sub_var = -1;
  hid_t scalar_ds = -1, raw1 = -1, raw2 = -1, raw_fixed3 = -1;
  hid_t raw_unlim1 = -1, raw_unlim2 = -1;
  hsize_t d4[1] = {4};
  hsize_t d2[1] = {2};
  hsize_t d5[1] = {5};
  hsize_t d3[1] = {3};
  hsize_t d_unlim[1] = {3};
  hsize_t d_unlim_max[1] = {H5S_UNLIMITED};
  hsize_t chunk2[1] = {2};
  int retval = NC_NOERR;

  if (path == NULL) {
    return NC_EINVAL;
  }
  EnsureHdf5Init();

  file_id = H5Fcreate(path, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  sub_matched =
      H5Gcreate2(file_id, "sub_matched", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  phony_grp =
      H5Gcreate2(file_id, "phony_grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  s4 = H5Screate_simple(1, d4, NULL);
  s2 = H5Screate_simple(1, d2, NULL);
  s5 = H5Screate_simple(1, d5, NULL);
  s3 = H5Screate_simple(1, d3, NULL);
  s_scalar = H5Screate(H5S_SCALAR);
  s_unlim = H5Screate_simple(1, d_unlim, d_unlim_max);
  unlim_dcpl = H5Pcreate(H5P_DATASET_CREATE);
  if (file_id < 0 || sub_matched < 0 || phony_grp < 0 || s4 < 0 || s2 < 0 ||
      s5 < 0 || s3 < 0 || s_scalar < 0 || s_unlim < 0 || unlim_dcpl < 0 ||
      H5Pset_chunk(unlim_dcpl, 1, chunk2) < 0) {
    retval = NC_EHDFERR;
    goto cleanup;
  }

  scale0 = H5Dcreate2(file_id, "scale0", H5T_NATIVE_INT, s4, H5P_DEFAULT,
                      H5P_DEFAULT, H5P_DEFAULT);
  root_matched = H5Dcreate2(file_id, "root_matched", H5T_NATIVE_FLOAT, s4,
                            H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  local_scale = H5Dcreate2(sub_matched, "local_scale", H5T_NATIVE_INT, s2,
                           H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  sub_var = H5Dcreate2(sub_matched, "sub_var", H5T_NATIVE_DOUBLE, s4,
                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  if (scale0 < 0 || root_matched < 0 || local_scale < 0 || sub_var < 0 ||
      H5DSset_scale(scale0, "scale0") < 0 ||
      H5DSset_scale(local_scale, "local_scale") < 0 ||
      H5DSattach_scale(root_matched, scale0, 0) < 0 ||
      H5DSattach_scale(sub_var, scale0, 0) < 0) {
    retval = NC_EHDFERR;
    goto cleanup;
  }

  scalar_ds = H5Dcreate2(phony_grp, "scalar_ds", H5T_NATIVE_INT, s_scalar,
                         H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  raw1 = H5Dcreate2(phony_grp, "raw1", H5T_NATIVE_INT, s5, H5P_DEFAULT,
                    H5P_DEFAULT, H5P_DEFAULT);
  raw2 = H5Dcreate2(phony_grp, "raw2", H5T_NATIVE_INT, s5, H5P_DEFAULT,
                    H5P_DEFAULT, H5P_DEFAULT);
  raw_fixed3 = H5Dcreate2(phony_grp, "raw_fixed3", H5T_NATIVE_INT, s3,
                          H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  raw_unlim1 = H5Dcreate2(phony_grp, "raw_unlim1", H5T_NATIVE_INT, s_unlim,
                          H5P_DEFAULT, unlim_dcpl, H5P_DEFAULT);
  raw_unlim2 = H5Dcreate2(phony_grp, "raw_unlim2", H5T_NATIVE_INT, s_unlim,
                          H5P_DEFAULT, unlim_dcpl, H5P_DEFAULT);
  if (scalar_ds < 0 || raw1 < 0 || raw2 < 0 || raw_fixed3 < 0 ||
      raw_unlim1 < 0 || raw_unlim2 < 0) {
    retval = NC_EHDFERR;
    goto cleanup;
  }

cleanup:
  if (raw_unlim2 >= 0) H5Dclose(raw_unlim2);
  if (raw_unlim1 >= 0) H5Dclose(raw_unlim1);
  if (raw_fixed3 >= 0) H5Dclose(raw_fixed3);
  if (raw2 >= 0) H5Dclose(raw2);
  if (raw1 >= 0) H5Dclose(raw1);
  if (scalar_ds >= 0) H5Dclose(scalar_ds);
  if (sub_var >= 0) H5Dclose(sub_var);
  if (local_scale >= 0) H5Dclose(local_scale);
  if (root_matched >= 0) H5Dclose(root_matched);
  if (scale0 >= 0) H5Dclose(scale0);
  if (unlim_dcpl >= 0) H5Pclose(unlim_dcpl);
  if (s_unlim >= 0) H5Sclose(s_unlim);
  if (s_scalar >= 0) H5Sclose(s_scalar);
  if (s3 >= 0) H5Sclose(s3);
  if (s5 >= 0) H5Sclose(s5);
  if (s2 >= 0) H5Sclose(s2);
  if (s4 >= 0) H5Sclose(s4);
  if (phony_grp >= 0) H5Gclose(phony_grp);
  if (sub_matched >= 0) H5Gclose(sub_matched);
  if (file_id >= 0) H5Fclose(file_id);
  return retval;
}

int Nc4HdfTestRecMatchDimscales(int ncid, int *out_phony_ndims,
                                int *out_sub_dim_matched) {
  NC_FILE_INFO_T *h5 = NULL;
  NC_GRP_INFO_T *root_grp = NULL;
  NC_GRP_INFO_T *sub_grp = NULL;
  NC_GRP_INFO_T *phony_grp = NULL;
  NC_VAR_INFO_T *root_matched = NULL;
  NC_VAR_INFO_T *sub_var = NULL;
  NC_DIM_INFO_T *scale0_dim = NULL;
  int retval = nc4_find_grp_h5(ncid, &root_grp, &h5);
  if (retval != NC_NOERR) {
    return retval;
  }

  sub_grp = (NC_GRP_INFO_T *)ncindexlookup(root_grp->children, "sub_matched");
  phony_grp = (NC_GRP_INFO_T *)ncindexlookup(root_grp->children, "phony_grp");
  if (!sub_grp || !phony_grp) {
    return NC_ENOGRP;
  }
  scale0_dim = (NC_DIM_INFO_T *)ncindexlookup(root_grp->dim, "scale0");
  root_matched = (NC_VAR_INFO_T *)ncindexlookup(root_grp->vars, "root_matched");
  sub_var = (NC_VAR_INFO_T *)ncindexlookup(sub_grp->vars, "sub_var");
  if (!scale0_dim || !root_matched || !sub_var) {
    return NC_ENOTVAR;
  }

  /* Clear root_matched->dim[0] while keeping dimids[0] to exercise line 2135
   * nc4_find_dim lookup, plus token matching. */
  root_matched->dim[0] = NULL;

  /* Clear both dim[0] and dimids[0] on sub_var so nc4_rec_match_dimscales must
   * walk child->parent dims using H5Otoken_cmp. */
  sub_var->dim[0] = NULL;
  sub_var->dimids[0] = -1;

  /* Delete open-time phony dimensions from phony_grp and clear its var dims
   * so nc4_rec_match_dimscales recreates and matches them cleanly. */
  while (ncindexsize(phony_grp->dim) > 0) {
    NC_DIM_INFO_T *d = (NC_DIM_INFO_T *)ncindexith(phony_grp->dim, 0);
    if (d->format_dim_info) {
      NC_HDF5_DIM_INFO_T *hd = (NC_HDF5_DIM_INFO_T *)d->format_dim_info;
      if (hd->hdf_dimscaleid) {
        H5Dclose(hd->hdf_dimscaleid);
      }
      free(hd);
      d->format_dim_info = NULL;
    }
    nclistset(h5->alldims, (size_t)d->hdr.id, NULL);
    nc4_dim_list_del(phony_grp, d);
  }
  if (!ncindexrebuild(phony_grp->dim)) {
    return NC_ENOMEM;
  }
  for (size_t i = 0; i < ncindexsize(phony_grp->vars); ++i) {
    NC_VAR_INFO_T *v = (NC_VAR_INFO_T *)ncindexith(phony_grp->vars, i);
    for (size_t d = 0; d < v->ndims; ++d) {
      v->dim[d] = NULL;
      v->dimids[d] = -1;
    }
  }

  retval = nc4_rec_match_dimscales(root_grp);
  if (retval != NC_NOERR) {
    return retval;
  }

  if (out_phony_ndims != NULL) {
    *out_phony_ndims = (int)ncindexsize(phony_grp->dim);
  }
  if (out_sub_dim_matched != NULL) {
    *out_sub_dim_matched =
        (sub_var->dim[0] == scale0_dim && root_matched->dim[0] == scale0_dim)
            ? 1
            : 0;
  }
  return NC_NOERR;
}

int Nc4HdfGetProvenanceDirect(int ncid, unsigned *major, unsigned *minor,
                              unsigned *release, int *superblock, int *is_nc4,
                              int *bad_superblock_err) {
  NC_FILE_INFO_T *h5 = NULL;
  NC_HDF5_FILE_INFO_T *hdf5_info = NULL;
  hid_t saved_hdfid;
  int dummy_super = -1;
  int retval = nc4_find_grp_h5(ncid, NULL, &h5);
  if (retval != NC_NOERR) {
    return retval;
  }
  retval = NC4_hdf5get_libversion(major, minor, release);
  if (retval != NC_NOERR) {
    return retval;
  }
  retval = NC4_hdf5get_superblock(h5, superblock);
  if (retval != NC_NOERR) {
    return retval;
  }
  if (is_nc4 != NULL) {
    *is_nc4 = NC4_isnetcdf4(h5);
  }
  if (bad_superblock_err != NULL) {
    hdf5_info = (NC_HDF5_FILE_INFO_T *)h5->format_file_info;
    saved_hdfid = hdf5_info->hdfid;
    hdf5_info->hdfid = -1;
    *bad_superblock_err = NC4_hdf5get_superblock(h5, &dummy_super);
    hdf5_info->hdfid = saved_hdfid;
  }
  return NC_NOERR;
}

int Nc4HdfVarHasAttr(int ncid, int varid, const char *attr_name, int *exists) {
  NC_FILE_INFO_T *h5 = NULL;
  NC_GRP_INFO_T *grp = NULL;
  NC_VAR_INFO_T *var = NULL;
  NC_HDF5_VAR_INFO_T *hdf5_var = NULL;
  htri_t res;
  int retval = nc4_find_grp_h5_var(ncid, varid, &h5, &grp, &var);
  (void)h5;
  (void)grp;
  if (retval != NC_NOERR) {
    return retval;
  }
  hdf5_var = (NC_HDF5_VAR_INFO_T *)var->format_var_info;
  if (!hdf5_var || hdf5_var->hdf_datasetid <= 0) {
    return NC_ENOTVAR;
  }
  res = H5Aexists(hdf5_var->hdf_datasetid, attr_name);
  if (res < 0) {
    return NC_EHDFERR;
  }
  *exists = res > 0 ? 1 : 0;
  return NC_NOERR;
}

int Nc4HdfDimHasAttr(int ncid, const char *dim_name, const char *attr_name,
                     int *exists) {
  NC_GRP_INFO_T *grp = NULL;
  NC_DIM_INFO_T *dim = NULL;
  NC_HDF5_DIM_INFO_T *hdf5_dim = NULL;
  hid_t dsid = 0;
  htri_t res;
  int retval = nc4_find_grp_h5(ncid, &grp, NULL);
  if (retval != NC_NOERR) {
    return retval;
  }
  dim = (NC_DIM_INFO_T *)ncindexlookup(grp->dim, dim_name);
  if (!dim) {
    return NC_EBADDIM;
  }
  hdf5_dim = (NC_HDF5_DIM_INFO_T *)dim->format_dim_info;
  if (dim->coord_var && dim->coord_var->format_var_info) {
    dsid = ((NC_HDF5_VAR_INFO_T *)dim->coord_var->format_var_info)->hdf_datasetid;
  } else if (hdf5_dim) {
    dsid = hdf5_dim->hdf_dimscaleid;
  }
  if (dsid <= 0) {
    return NC_EDIMSCALE;
  }
  res = H5Aexists(dsid, attr_name);
  if (res < 0) {
    return NC_EHDFERR;
  }
  *exists = res > 0 ? 1 : 0;
  return NC_NOERR;
}

int Nc4HdfClearInvalidDatasetId(int ncid, int varid) {
  NC_FILE_INFO_T *h5 = NULL;
  NC_GRP_INFO_T *grp = NULL;
  NC_VAR_INFO_T *var = NULL;
  NC_HDF5_VAR_INFO_T *hdf5_var = NULL;
  int retval = nc4_find_grp_h5_var(ncid, varid, &h5, &grp, &var);
  (void)h5;
  (void)grp;
  if (retval != NC_NOERR) {
    return retval;
  }
  hdf5_var = (NC_HDF5_VAR_INFO_T *)var->format_var_info;
  if (hdf5_var && hdf5_var->hdf_datasetid < 0) {
    hdf5_var->hdf_datasetid = 0;
  }
  return NC_NOERR;
}

int Nc4HdfDeleteVarAttr(int ncid, int varid, const char *attr_name) {
  NC_FILE_INFO_T *h5 = NULL;
  NC_GRP_INFO_T *grp = NULL;
  NC_VAR_INFO_T *var = NULL;
  NC_HDF5_VAR_INFO_T *hdf5_var = NULL;
  htri_t exists;
  int retval;
  if (attr_name == NULL) {
    return NC_EINVAL;
  }
  retval = nc4_find_grp_h5_var(ncid, varid, &h5, &grp, &var);
  (void)h5;
  (void)grp;
  if (retval != NC_NOERR) {
    return retval;
  }
  hdf5_var = (NC_HDF5_VAR_INFO_T *)var->format_var_info;
  if (!hdf5_var || hdf5_var->hdf_datasetid <= 0) {
    return NC_ENOTVAR;
  }
  exists = H5Aexists(hdf5_var->hdf_datasetid, attr_name);
  if (exists < 0) {
    return NC_EHDFERR;
  }
  if (exists > 0 && H5Adelete(hdf5_var->hdf_datasetid, attr_name) < 0) {
    return NC_EHDFERR;
  }
  return NC_NOERR;
}

/* Helper macro to fail with status code while preserving clean teardown. */
#define SHIM_CHECK_NC(call) \
  do {                      \
    int rc_ = (call);       \
    if (rc_ != NC_NOERR) {  \
      return rc_;           \
    }                       \
  } while (0)

#define SHIM_EXPECT(cond, err) \
  do {                         \
    if (!(cond)) {             \
      retval = (err);          \
      goto cleanup;            \
    }                          \
  } while (0)

int Nc4HdfTestCommitTypeAndGroupErrors(const char* ro_plain_path) {
  int retval = NC_NOERR;
  int rnc = -1;
  int ncid = -1;
  hid_t ro_fid = -1;
  hid_t ro_grpid = -1;

  if (!ro_plain_path) {
    return NC_EINVAL;
  }

  /* Create a NetCDF-4 file without NC_CLASSIC_MODEL (_nc3_strict absent). */
  if ((retval = nc_create(ro_plain_path, NC_NETCDF4 | NC_CLOBBER, &rnc))) {
    return retval;
  }
  {
    int d = -1, v = -1;
    if ((retval = nc_def_dim(rnc, "d", 2, &d)) ||
        (retval = nc_def_var(rnc, "v", NC_INT, 1, &d, &v)) ||
        (retval = nc_enddef(rnc))) {
      nc_abort(rnc);
      return retval;
    }
    if ((retval = nc_close(rnc))) {
      return retval;
    }
    rnc = -1;
  }

  ro_fid = H5Fopen(ro_plain_path, H5F_ACC_RDONLY, H5P_DEFAULT);
  if (ro_fid < 0) {
    return NC_EHDFERR;
  }
  ro_grpid = H5Gopen2(ro_fid, "/", H5P_DEFAULT);
  if (ro_grpid < 0) {
    H5Fclose(ro_fid);
    return NC_EHDFERR;
  }

  /* Case 1: NC_COMPOUND size == 0 -> H5Tcreate(H5T_COMPOUND, 0) < 0 (line 1203). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_TYPE_INFO_T* type = NULL;
    NC_HDF5_TYPE_INFO_T* hdf5_type = NULL;
    nc_type tid = 0;
    size_t saved_size = 0;
    SHIM_EXPECT(nc_create_mem("ct1.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_compound(ncid, 4, "c0", &tid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(ncid, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_type(h5, tid, &type) == NC_NOERR && type != NULL,
                NC_EBADTYPE);
    hdf5_type = (NC_HDF5_TYPE_INFO_T*)type->format_type_info;
    saved_size = type->size;
    type->size = 0;
    retval = nc4_rec_write_groups_types(h5->root_grp);
    hdf5_type->hdf_typeid = 0;
    type->size = saved_size;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* Case 2: NC_COMPOUND field with nc_typeid = NC_NAT (line 1213). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_TYPE_INFO_T* type = NULL;
    NC_FIELD_INFO_T* field = NULL;
    nc_type tid = 0;
    nc_type saved_ftype = 0;
    SHIM_EXPECT(nc_create_mem("ct2.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_compound(ncid, sizeof(int), "c_nat", &tid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_insert_compound(ncid, tid, "f", 0, NC_INT) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(ncid, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_type(h5, tid, &type) == NC_NOERR && type != NULL,
                NC_EBADTYPE);
    field = (NC_FIELD_INFO_T*)nclistget(type->u.c.field, 0);
    SHIM_EXPECT(field != NULL, NC_EBADTYPE);
    saved_ftype = field->nc_typeid;
    field->nc_typeid = NC_NAT;
    retval = nc4_rec_write_groups_types(h5->root_grp);
    field->nc_typeid = saved_ftype;
    SHIM_EXPECT(retval == NC_EBADTYPE, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* Case 3: NC_COMPOUND array field with dim_size[0] == 0 (lines 1226, 1228). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_TYPE_INFO_T* type = NULL;
    NC_FIELD_INFO_T* field = NULL;
    nc_type tid = 0;
    int dims[1] = {2};
    int saved_dim = 0;
    SHIM_EXPECT(nc_create_mem("ct3.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_compound(ncid, sizeof(int) * 2, "c_arr0", &tid) ==
                    NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_insert_array_compound(ncid, tid, "a", 0, NC_INT, 1, dims) ==
                    NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(ncid, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_type(h5, tid, &type) == NC_NOERR && type != NULL,
                NC_EBADTYPE);
    field = (NC_FIELD_INFO_T*)nclistget(type->u.c.field, 0);
    SHIM_EXPECT(field != NULL && field->dim_size != NULL, NC_EBADTYPE);
    saved_dim = field->dim_size[0];
    field->dim_size[0] = 0;
    retval = nc4_rec_write_groups_types(h5->root_grp);
    field->dim_size[0] = saved_dim;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* Case 4: NC_VLEN base_nc_typeid = NC_NAT (line 1249). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_TYPE_INFO_T* type = NULL;
    nc_type tid = 0;
    nc_type saved_base = 0;
    SHIM_EXPECT(nc_create_mem("ct4.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_vlen(ncid, "v_nat", NC_INT, &tid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(ncid, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_type(h5, tid, &type) == NC_NOERR && type != NULL,
                NC_EBADTYPE);
    saved_base = type->u.v.base_nc_typeid;
    type->u.v.base_nc_typeid = NC_NAT;
    retval = nc4_rec_write_groups_types(h5->root_grp);
    type->u.v.base_nc_typeid = saved_base;
    SHIM_EXPECT(retval == NC_EBADTYPE, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* Case 5: NC_OPAQUE size == 0 (line 1259). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_TYPE_INFO_T* type = NULL;
    NC_HDF5_TYPE_INFO_T* hdf5_type = NULL;
    nc_type tid = 0;
    size_t saved_size = 0;
    SHIM_EXPECT(nc_create_mem("ct5.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_opaque(ncid, 4, "o0", &tid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(ncid, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_type(h5, tid, &type) == NC_NOERR && type != NULL,
                NC_EBADTYPE);
    hdf5_type = (NC_HDF5_TYPE_INFO_T*)type->format_type_info;
    saved_size = type->size;
    type->size = 0;
    retval = nc4_rec_write_groups_types(h5->root_grp);
    hdf5_type->hdf_typeid = 0;
    type->size = saved_size;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* Case 6: NC_ENUM base_nc_typeid = NC_NAT (line 1272). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_TYPE_INFO_T* type = NULL;
    nc_type tid = 0;
    nc_type saved_base = 0;
    int e0 = 1;
    SHIM_EXPECT(nc_create_mem("ct6.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_enum(ncid, NC_INT, "e_nat", &tid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_insert_enum(ncid, tid, "E0", &e0) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(ncid, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_type(h5, tid, &type) == NC_NOERR && type != NULL,
                NC_EBADTYPE);
    saved_base = type->u.e.base_nc_typeid;
    type->u.e.base_nc_typeid = NC_NAT;
    retval = nc4_rec_write_groups_types(h5->root_grp);
    type->u.e.base_nc_typeid = saved_base;
    SHIM_EXPECT(retval == NC_EBADTYPE, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* Case 7: Unknown nc_type_class in child group (lines 1288, 2066). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* sub_grp = NULL;
    NC_TYPE_INFO_T* type = NULL;
    int subid = -1;
    nc_type tid = 0;
    nc_type saved_cls = 0;
    SHIM_EXPECT(nc_create_mem("ct7.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_grp(ncid, "sub", &subid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_opaque(subid, 4, "bad_cls", &tid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(subid, &sub_grp, &h5) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_type(h5, tid, &type) == NC_NOERR && type != NULL,
                NC_EBADTYPE);
    saved_cls = type->nc_type_class;
    type->nc_type_class = 999;
    retval = nc4_rec_write_groups_types(h5->root_grp);
    type->nc_type_class = saved_cls;
    SHIM_EXPECT(retval == NC_EBADTYPE, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* Case 8: H5Tcommit1 failure on NC_OPAQUE with hdf_grpid = -1 (line 1293). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_HDF5_GRP_INFO_T* hdf5_grp = NULL;
    nc_type tid = 0;
    hid_t saved_gid = -1;
    SHIM_EXPECT(nc_create_mem("ct8.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_opaque(ncid, 4, "o_fail", &tid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(ncid, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    hdf5_grp = (NC_HDF5_GRP_INFO_T*)h5->root_grp->format_grp_info;
    saved_gid = hdf5_grp->hdf_grpid;
    hdf5_grp->hdf_grpid = -1;
    retval = nc4_rec_write_groups_types(h5->root_grp);
    hdf5_grp->hdf_grpid = saved_gid;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* Case 9: write_nc3_strict_att with hdf_grpid = -1 (lines 1329, 2052). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_HDF5_GRP_INFO_T* hdf5_grp = NULL;
    hid_t saved_gid = -1;
    SHIM_EXPECT(nc_create_mem("ct9.nc", NC_NETCDF4 | NC_CLASSIC_MODEL, 0,
                              &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(ncid, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    hdf5_grp = (NC_HDF5_GRP_INFO_T*)h5->root_grp->format_grp_info;
    saved_gid = hdf5_grp->hdf_grpid;
    hdf5_grp->hdf_grpid = -1;
    retval = nc4_rec_write_groups_types(h5->root_grp);
    hdf5_grp->hdf_grpid = saved_gid;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* Case 10: write_nc3_strict_att on read-only ro_grpid (line 1339). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_HDF5_GRP_INFO_T* hdf5_grp = NULL;
    hid_t saved_gid = -1;
    SHIM_EXPECT(nc_create_mem("ct10.nc", NC_NETCDF4 | NC_CLASSIC_MODEL, 0,
                              &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(ncid, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    hdf5_grp = (NC_HDF5_GRP_INFO_T*)h5->root_grp->format_grp_info;
    saved_gid = hdf5_grp->hdf_grpid;
    hdf5_grp->hdf_grpid = ro_grpid;
    retval = nc4_rec_write_groups_types(h5->root_grp);
    hdf5_grp->hdf_grpid = saved_gid;
    SHIM_EXPECT(retval == NC_EFILEMETA, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* Case 11: create_group on read-only ro_grpid (lines 1400, 2046, 2066). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* sub_grp = NULL;
    NC_HDF5_GRP_INFO_T* root_hdf5 = NULL;
    NC_HDF5_GRP_INFO_T* sub_hdf5 = NULL;
    int subid = -1;
    hid_t saved_gid = -1;
    SHIM_EXPECT(nc_create_mem("ct11.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_grp(ncid, "sub_ro", &subid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(subid, &sub_grp, &h5) == NC_NOERR, NC_EHDFERR);
    root_hdf5 = (NC_HDF5_GRP_INFO_T*)h5->root_grp->format_grp_info;
    sub_hdf5 = (NC_HDF5_GRP_INFO_T*)sub_grp->format_grp_info;
    saved_gid = root_hdf5->hdf_grpid;
    root_hdf5->hdf_grpid = ro_grpid;
    retval = nc4_rec_write_groups_types(h5->root_grp);
    sub_hdf5->hdf_grpid = 0;
    root_hdf5->hdf_grpid = saved_gid;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

cleanup:
  if (ncid >= 0) nc_abort(ncid);
  if (ro_grpid >= 0) H5Gclose(ro_grpid);
  if (ro_fid >= 0) H5Fclose(ro_fid);
  return retval;
}

int Nc4HdfTestForkedErrorSubcase(int subcase) {
  int ncid = -1;
  int retval = NC_NOERR;

  if (subcase == 0) {
    /* nc4_adjust_var_cache reopen error (line 1155). */
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_VAR_INFO_T* var = NULL;
    NC_HDF5_VAR_INFO_T* hdf5_var = NULL;
    int dimid = -1, varid = -1;
    size_t cs[1] = {20000};
    SHIM_CHECK_NC(nc_create_mem("fork0.nc", NC_NETCDF4, 0, &ncid));
    SHIM_CHECK_NC(nc_def_dim(ncid, "d", 40000, &dimid));
    SHIM_CHECK_NC(nc_def_var(ncid, "v", NC_INT, 1, &dimid, &varid));
    SHIM_CHECK_NC(nc_def_var_chunking(ncid, varid, NC_CHUNKED, cs));
    SHIM_CHECK_NC(nc_enddef(ncid));
    SHIM_CHECK_NC(nc4_find_grp_h5_var(ncid, varid, &h5, &grp, &var));
    hdf5_var = (NC_HDF5_VAR_INFO_T*)var->format_var_info;
    var->chunkcache.size = CHUNK_CACHE_SIZE;
    hdf5_var->hdf_datasetid = -1;
    retval = nc4_adjust_var_cache(grp, var);
    return (retval == NC_EHDFERR) ? NC_NOERR : NC_EINVAL;
  }

  if (subcase == 1) {
    /* commit_type NC_COMPOUND H5Tinsert offset error (line 1239). */
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_TYPE_INFO_T* type = NULL;
    NC_FIELD_INFO_T* field = NULL;
    nc_type tid = 0;
    SHIM_CHECK_NC(nc_create_mem("fork1.nc", NC_NETCDF4, 0, &ncid));
    SHIM_CHECK_NC(nc_def_compound(ncid, sizeof(int), "c_off", &tid));
    SHIM_CHECK_NC(nc_insert_compound(ncid, tid, "x", 0, NC_INT));
    SHIM_CHECK_NC(nc4_find_grp_h5(ncid, &grp, &h5));
    SHIM_CHECK_NC(nc4_find_type(h5, tid, &type));
    field = (NC_FIELD_INFO_T*)nclistget(type->u.c.field, 0);
    if (!field) return NC_EBADTYPE;
    field->offset = 100;
    retval = nc4_rec_write_groups_types(h5->root_grp);
    return (retval == NC_EHDFERR) ? NC_NOERR : NC_EINVAL;
  }

  if (subcase == 2) {
    /* commit_type NC_ENUM H5Tenum_create(NC_FLOAT) error (line 1276). */
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_TYPE_INFO_T* type = NULL;
    nc_type tid = 0;
    int v0 = 1;
    SHIM_CHECK_NC(nc_create_mem("fork2.nc", NC_NETCDF4, 0, &ncid));
    SHIM_CHECK_NC(nc_def_enum(ncid, NC_INT, "e_flt", &tid));
    SHIM_CHECK_NC(nc_insert_enum(ncid, tid, "A", &v0));
    SHIM_CHECK_NC(nc4_find_grp_h5(ncid, &grp, &h5));
    SHIM_CHECK_NC(nc4_find_type(h5, tid, &type));
    type->u.e.base_nc_typeid = NC_FLOAT;
    retval = nc4_rec_write_groups_types(h5->root_grp);
    return (retval == NC_EHDFERR) ? NC_NOERR : NC_EINVAL;
  }

  if (subcase == 3) {
    /* commit_type NC_ENUM duplicate member name error (line 1282). */
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_TYPE_INFO_T* type = NULL;
    NC_ENUM_MEMBER_INFO_T* m1 = NULL;
    nc_type tid = 0;
    int v0 = 0, v1 = 1;
    SHIM_CHECK_NC(nc_create_mem("fork3.nc", NC_NETCDF4, 0, &ncid));
    SHIM_CHECK_NC(nc_def_enum(ncid, NC_INT, "e_dup", &tid));
    SHIM_CHECK_NC(nc_insert_enum(ncid, tid, "A", &v0));
    SHIM_CHECK_NC(nc_insert_enum(ncid, tid, "B", &v1));
    SHIM_CHECK_NC(nc4_find_grp_h5(ncid, &grp, &h5));
    SHIM_CHECK_NC(nc4_find_type(h5, tid, &type));
    m1 = (NC_ENUM_MEMBER_INFO_T*)nclistget(type->u.e.enum_member, 1);
    if (!m1 || !m1->name) return NC_EBADTYPE;
    m1->name[0] = 'A';
    retval = nc4_rec_write_groups_types(h5->root_grp);
    return (retval == NC_EHDFERR) ? NC_NOERR : NC_EINVAL;
  }

  if (subcase == 5) {
    /* nc4_rec_match_dimscales H5S_NULL dataspace dataset (lines 2216-2218). */
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_VAR_INFO_T* var = NULL;
    NC_HDF5_GRP_INFO_T* hdf5_grp = NULL;
    NC_HDF5_VAR_INFO_T* hdf5_var = NULL;
    hid_t null_sp = -1, null_ds = -1;
    int dimid = -1, varid = -1;
    SHIM_CHECK_NC(nc_create_mem("fork5.nc", NC_NETCDF4, 0, &ncid));
    SHIM_CHECK_NC(nc_def_dim(ncid, "d", 2, &dimid));
    SHIM_CHECK_NC(nc_def_var(ncid, "v", NC_INT, 1, &dimid, &varid));
    SHIM_CHECK_NC(nc_enddef(ncid));
    SHIM_CHECK_NC(nc4_find_grp_h5_var(ncid, varid, &h5, &grp, &var));
    hdf5_grp = (NC_HDF5_GRP_INFO_T*)grp->format_grp_info;
    hdf5_var = (NC_HDF5_VAR_INFO_T*)var->format_var_info;
    null_sp = H5Screate(H5S_NULL);
    if (null_sp < 0) return NC_EHDFERR;
    null_ds = H5Dcreate2(hdf5_grp->hdf_grpid, "raw_null", H5T_NATIVE_INT,
                         null_sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    H5Sclose(null_sp);
    if (null_ds < 0) return NC_EHDFERR;
    hdf5_var->hdf_datasetid = null_ds;
    retval = nc4_rec_match_dimscales(h5->root_grp);
    return (retval == NC_EHDFERR) ? NC_NOERR : NC_EINVAL;
  }

  if (subcase == 6) {
    /* nc4_rec_match_dimscales rank mismatch (lines 2221-2223). */
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_VAR_INFO_T* v1_info = NULL;
    NC_VAR_INFO_T* v2_info = NULL;
    NC_HDF5_VAR_INFO_T* v1_hdf5 = NULL;
    NC_HDF5_VAR_INFO_T* v2_hdf5 = NULL;
    int d0 = -1, d1 = -1, v1 = -1, v2 = -1;
    int dims2[2];
    SHIM_CHECK_NC(nc_create_mem("fork6.nc", NC_NETCDF4, 0, &ncid));
    SHIM_CHECK_NC(nc_def_dim(ncid, "d0", 2, &d0));
    SHIM_CHECK_NC(nc_def_dim(ncid, "d1", 3, &d1));
    dims2[0] = d0;
    dims2[1] = d1;
    SHIM_CHECK_NC(nc_def_var(ncid, "v1", NC_INT, 1, &d0, &v1));
    SHIM_CHECK_NC(nc_def_var(ncid, "v2", NC_INT, 2, dims2, &v2));
    SHIM_CHECK_NC(nc_enddef(ncid));
    SHIM_CHECK_NC(nc4_find_grp_h5_var(ncid, v1, &h5, &grp, &v1_info));
    SHIM_CHECK_NC(nc4_find_grp_h5_var(ncid, v2, &h5, &grp, &v2_info));
    v1_hdf5 = (NC_HDF5_VAR_INFO_T*)v1_info->format_var_info;
    v2_hdf5 = (NC_HDF5_VAR_INFO_T*)v2_info->format_var_info;
    v2_hdf5->hdf_datasetid = v1_hdf5->hdf_datasetid;
    retval = nc4_rec_match_dimscales(h5->root_grp);
    return (retval == NC_EHDFERR) ? NC_NOERR : NC_EINVAL;
  }

  if (subcase == 7) {
    /* nc4_rec_match_dimscales non-scalar dataset with ndims == 0 (line 2230). */
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_VAR_INFO_T* v1_info = NULL;
    NC_VAR_INFO_T* vs_info = NULL;
    NC_HDF5_VAR_INFO_T* v1_hdf5 = NULL;
    NC_HDF5_VAR_INFO_T* vs_hdf5 = NULL;
    int d0 = -1, v1 = -1, vs = -1;
    SHIM_CHECK_NC(nc_create_mem("fork7.nc", NC_NETCDF4, 0, &ncid));
    SHIM_CHECK_NC(nc_def_dim(ncid, "d0", 2, &d0));
    SHIM_CHECK_NC(nc_def_var(ncid, "v1", NC_INT, 1, &d0, &v1));
    SHIM_CHECK_NC(nc_def_var(ncid, "vs", NC_INT, 0, NULL, &vs));
    SHIM_CHECK_NC(nc_enddef(ncid));
    SHIM_CHECK_NC(nc4_find_grp_h5_var(ncid, v1, &h5, &grp, &v1_info));
    SHIM_CHECK_NC(nc4_find_grp_h5_var(ncid, vs, &h5, &grp, &vs_info));
    v1_hdf5 = (NC_HDF5_VAR_INFO_T*)v1_info->format_var_info;
    vs_hdf5 = (NC_HDF5_VAR_INFO_T*)vs_info->format_var_info;
    vs_hdf5->hdf_datasetid = v1_hdf5->hdf_datasetid;
    retval = nc4_rec_match_dimscales(h5->root_grp);
    return (retval == NC_EHDFERR) ? NC_NOERR : NC_EINVAL;
  }

  return NC_EINVAL;
}

int Nc4HdfTestWriteMetadataAndVarErrors(const char* ro_attrs_path) {
  int retval = NC_NOERR;
  int rnc = -1;
  int ncid = -1;
  hid_t ro_fid = -1;
  hid_t ro_grpid = -1;
  hid_t ro_ds_all = -1;
  hid_t ro_ds_class = -1;
  hid_t ro_ds_name = -1;
  hid_t ro_ds_plain = -1;
  hid_t sp_scalar = -1;

  if (!ro_attrs_path) {
    return NC_EINVAL;
  }

  /* Create disk file with global att "g_int", three dimscale datasets whose
   * _Netcdf4Dimid/CLASS/NAME attrs are selectively stripped, and a non-coord var. */
  if ((retval = nc_create(ro_attrs_path, NC_NETCDF4 | NC_CLOBBER, &rnc))) {
    return retval;
  }
  {
    int gv = 42;
    int da = -1, db = -1, dc = -1, dp = -1;
    int va = -1, vb = -1, vc = -1, vplain = -1;
    if ((retval = nc_put_att_int(rnc, NC_GLOBAL, "g_int", NC_INT, 1, &gv)) ||
        (retval = nc_def_dim(rnc, "da", 2, &da)) ||
        (retval = nc_def_dim(rnc, "db", 2, &db)) ||
        (retval = nc_def_dim(rnc, "dc", 2, &dc)) ||
        (retval = nc_def_dim(rnc, "dp", 2, &dp)) ||
        (retval = nc_enddef(rnc)) || (retval = nc_redef(rnc)) ||
        (retval = nc_def_var(rnc, "da", NC_INT, 1, &da, &va)) ||
        (retval = nc_def_var(rnc, "db", NC_INT, 1, &db, &vb)) ||
        (retval = nc_def_var(rnc, "dc", NC_INT, 1, &dc, &vc)) ||
        (retval = nc_def_var(rnc, "plain", NC_INT, 1, &dp, &vplain)) ||
        (retval = nc_enddef(rnc))) {
      nc_abort(rnc);
      return retval;
    }
    /* Strip selected coord attrs before closing so read-only deletes reach
     * each branch in remove_coord_atts(). */
    if ((retval = Nc4HdfDeleteVarAttr(rnc, vb, "_Netcdf4Dimid")) ||
        (retval = Nc4HdfDeleteVarAttr(rnc, vc, "_Netcdf4Dimid")) ||
        (retval = Nc4HdfDeleteVarAttr(rnc, vc, "CLASS"))) {
      nc_abort(rnc);
      return retval;
    }
    if ((retval = nc_close(rnc))) {
      return retval;
    }
    rnc = -1;
  }

  ro_fid = H5Fopen(ro_attrs_path, H5F_ACC_RDONLY, H5P_DEFAULT);
  if (ro_fid < 0) return NC_EHDFERR;
  ro_grpid = H5Gopen2(ro_fid, "/", H5P_DEFAULT);
  ro_ds_all = H5Dopen2(ro_fid, "da", H5P_DEFAULT);
  ro_ds_class = H5Dopen2(ro_fid, "db", H5P_DEFAULT);
  ro_ds_name = H5Dopen2(ro_fid, "dc", H5P_DEFAULT);
  ro_ds_plain = H5Dopen2(ro_fid, "plain", H5P_DEFAULT);
  sp_scalar = H5Screate(H5S_SCALAR);
  SHIM_EXPECT(ro_grpid >= 0 && ro_ds_all >= 0 && ro_ds_class >= 0 &&
                  ro_ds_name >= 0 && ro_ds_plain >= 0 && sp_scalar >= 0,
              NC_EHDFERR);

  /* 1. flag_atts_dirty(NULL) via replace_existing_var with NULL var->att (line 48). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_VAR_INFO_T* var = NULL;
    NCindex* saved_att = NULL;
    int sub = -1, vid = -1, fv1 = 7, fv2 = 9;
    SHIM_EXPECT(nc_create_mem("wm1.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_grp(ncid, "sub", &sub) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(sub, "v", NC_INT, 0, NULL, &vid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_var_fill(sub, vid, 0, &fv1) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_enddef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_redef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_put_att_int(sub, vid, NC_FillValue, NC_INT, 1, &fv2) ==
                    NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5_var(sub, vid, &h5, &grp, &var) == NC_NOERR,
                NC_EHDFERR);
    saved_att = var->att;
    var->att = NULL;
    retval = nc_enddef(ncid);
    var->att = saved_att;
    SHIM_EXPECT(retval == NC_NOERR, retval);
    SHIM_EXPECT(nc_close(ncid) == NC_NOERR, NC_EHDFERR);
    ncid = -1;
  }

  /* 2. nc4_get_hdf_typeid H5Tcopy(-1) error (line 401). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_TYPE_INFO_T* type = NULL;
    NC_HDF5_TYPE_INFO_T* hdf5_type = NULL;
    nc_type tid = 0;
    hid_t saved_tid = -1, out_tid = 0;
    SHIM_EXPECT(nc_create_mem("wm2.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_opaque(ncid, 4, "op", &tid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_enddef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(ncid, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_type(h5, tid, &type) == NC_NOERR && type != NULL,
                NC_EBADTYPE);
    hdf5_type = (NC_HDF5_TYPE_INFO_T*)type->format_type_info;
    saved_tid = hdf5_type->hdf_typeid;
    hdf5_type->hdf_typeid = -1;
    retval = nc4_get_hdf_typeid(h5, tid, &out_tid, 0);
    hdf5_type->hdf_typeid = saved_tid;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);
    retval = NC_NOERR;
    SHIM_EXPECT(nc_close(ncid) == NC_NOERR, NC_EHDFERR);
    ncid = -1;
  }

  /* 3. put_att_grpa h5->no_write (lines 449, 617, 1959). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    int av = 1;
    SHIM_EXPECT(nc_create_mem("wm3.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_put_att_int(ncid, NC_GLOBAL, "a", NC_INT, 1, &av) ==
                    NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(ncid, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    h5->no_write = NC_TRUE;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    h5->no_write = NC_FALSE;
    SHIM_EXPECT(retval == NC_EPERM, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* 4. put_att_grpa nc4_open_var_grp2 error in write_var (lines 457, 1754). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_VAR_INFO_T* var = NULL;
    NC_HDF5_GRP_INFO_T* hdf5_grp = NULL;
    NC_HDF5_VAR_INFO_T* hdf5_var = NULL;
    int vid = -1, av = 5;
    hid_t saved_ds = -1, saved_gid = -1;
    SHIM_EXPECT(nc_create_mem("wm4.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(ncid, "v", NC_INT, 0, NULL, &vid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_enddef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_redef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_put_att_int(ncid, vid, "va", NC_INT, 1, &av) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5_var(ncid, vid, &h5, &grp, &var) == NC_NOERR,
                NC_EHDFERR);
    hdf5_grp = (NC_HDF5_GRP_INFO_T*)grp->format_grp_info;
    hdf5_var = (NC_HDF5_VAR_INFO_T*)var->format_var_info;
    saved_ds = hdf5_var->hdf_datasetid;
    saved_gid = hdf5_grp->hdf_grpid;
    hdf5_var->hdf_datasetid = 0;
    hdf5_grp->hdf_grpid = -1;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    hdf5_var->hdf_datasetid = saved_ds;
    hdf5_grp->hdf_grpid = saved_gid;
    SHIM_EXPECT(retval == NC_ENOTVAR, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* 5. put_att_grpa NC_NAT -> H5Tclose(-1) -> var_create_dataset write_attlist
   * (lines 466, 578, 1068). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_VAR_INFO_T* var = NULL;
    NC_ATT_INFO_T* att = NULL;
    int vid = -1, av = 5;
    nc_type saved_at = 0;
    SHIM_EXPECT(nc_create_mem("wm5.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(ncid, "v", NC_INT, 0, NULL, &vid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_put_att_int(ncid, vid, "va", NC_INT, 1, &av) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5_var(ncid, vid, &h5, &grp, &var) == NC_NOERR,
                NC_EHDFERR);
    att = (NC_ATT_INFO_T*)ncindexith(var->att, 0);
    SHIM_EXPECT(att != NULL, NC_ENOTATT);
    saved_at = att->nc_typeid;
    att->nc_typeid = NC_NAT;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    att->nc_typeid = saved_at;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* 6. put_att_grpa H5Screate_simple(H5S_UNLIMITED) error (line 510). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_ATT_INFO_T* att = NULL;
    int av = 5;
    size_t saved_len = 0;
    SHIM_EXPECT(nc_create_mem("wm6.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_put_att_int(ncid, NC_GLOBAL, "huge", NC_INT, 1, &av) ==
                    NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(ncid, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    att = (NC_ATT_INFO_T*)ncindexith(grp->att, 0);
    SHIM_EXPECT(att != NULL, NC_ENOTATT);
    saved_len = att->len;
    att->len = (size_t)H5S_UNLIMITED;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    att->len = saved_len;
    SHIM_EXPECT(retval == NC_EATTMETA, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* 7. put_att_grpa H5Aexists(-1) (line 516). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_HDF5_GRP_INFO_T* hdf5_grp = NULL;
    int av = 5;
    hid_t saved_gid = -1;
    SHIM_EXPECT(nc_create_mem("wm7.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_put_att_int(ncid, NC_GLOBAL, "g", NC_INT, 1, &av) ==
                    NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(ncid, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    hdf5_grp = (NC_HDF5_GRP_INFO_T*)grp->format_grp_info;
    saved_gid = hdf5_grp->hdf_grpid;
    hdf5_grp->hdf_grpid = -1;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    hdf5_grp->hdf_grpid = saved_gid;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* 8a/b/c. put_att_grpa on read-only ro_grpid: H5Adelete mismatch (541),
   * H5Awrite reuse (559), and H5Acreate1 missing (569). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_HDF5_GRP_INFO_T* hdf5_grp = NULL;
    float fv = 1.5f;
    int iv = 42;
    hid_t saved_gid = -1;

    SHIM_EXPECT(nc_create_mem("wm8a.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_put_att_float(ncid, NC_GLOBAL, "g_int", NC_FLOAT, 1, &fv) ==
                    NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(ncid, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    hdf5_grp = (NC_HDF5_GRP_INFO_T*)grp->format_grp_info;
    saved_gid = hdf5_grp->hdf_grpid;
    hdf5_grp->hdf_grpid = ro_grpid;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    hdf5_grp->hdf_grpid = saved_gid;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;

    SHIM_EXPECT(nc_create_mem("wm8b.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_put_att_int(ncid, NC_GLOBAL, "g_int", NC_INT, 1, &iv) ==
                    NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(ncid, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    hdf5_grp = (NC_HDF5_GRP_INFO_T*)grp->format_grp_info;
    saved_gid = hdf5_grp->hdf_grpid;
    hdf5_grp->hdf_grpid = ro_grpid;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    hdf5_grp->hdf_grpid = saved_gid;
    SHIM_EXPECT(retval == NC_EATTMETA, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;

    SHIM_EXPECT(nc_create_mem("wm8c.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_put_att_int(ncid, NC_GLOBAL, "g_absent", NC_INT, 1, &iv) ==
                    NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(ncid, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    hdf5_grp = (NC_HDF5_GRP_INFO_T*)grp->format_grp_info;
    saved_gid = hdf5_grp->hdf_grpid;
    hdf5_grp->hdf_grpid = ro_grpid;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    hdf5_grp->hdf_grpid = saved_gid;
    SHIM_EXPECT(retval == NC_EATTMETA, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* 9. write_netcdf4_dimid errors via write_var(bad_coord_order=1): H5Aexists
   * error (757, 1723), H5Acreate1 ro (766), and H5Awrite ro (772). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_VAR_INFO_T* var = NULL;
    NC_HDF5_VAR_INFO_T* hdf5_var = NULL;
    int did = -1, vid = -1;
    hid_t saved_ds = -1;
    SHIM_EXPECT(nc_create_mem("wm9.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_dim(ncid, "d", 2, &did) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(ncid, "v", NC_INT, 1, &did, &vid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_enddef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5_var(ncid, vid, &h5, &grp, &var) == NC_NOERR,
                NC_EHDFERR);
    hdf5_var = (NC_HDF5_VAR_INFO_T*)var->format_var_info;
    saved_ds = hdf5_var->hdf_datasetid;

    hdf5_var->hdf_datasetid = -1;
    retval = nc4_rec_write_metadata(h5->root_grp, NC_TRUE);
    hdf5_var->hdf_datasetid = saved_ds;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);

    hdf5_var->hdf_datasetid = ro_ds_plain;
    retval = nc4_rec_write_metadata(h5->root_grp, NC_TRUE);
    hdf5_var->hdf_datasetid = saved_ds;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);

    hdf5_var->hdf_datasetid = ro_ds_all;
    retval = nc4_rec_write_metadata(h5->root_grp, NC_TRUE);
    hdf5_var->hdf_datasetid = saved_ds;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);
    retval = NC_NOERR;
    SHIM_EXPECT(nc_close(ncid) == NC_NOERR, NC_EHDFERR);
    ncid = -1;
  }

  /* 10a. var_create_dataset nc4_get_hdf_typeid(NC_NAT) error (line 833). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_VAR_INFO_T* var = NULL;
    int vid = -1;
    nc_type saved_tid = 0;
    SHIM_EXPECT(nc_create_mem("wm10a.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(ncid, "v", NC_INT, 0, NULL, &vid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5_var(ncid, vid, &h5, &grp, &var) == NC_NOERR,
                NC_EHDFERR);
    saved_tid = var->type_info->hdr.id;
    var->type_info->hdr.id = NC_NAT;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    var->type_info->hdr.id = saved_tid;
    SHIM_EXPECT(retval == NC_EBADTYPE, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* 10b. var_create_dataset nc4_get_fill_value error on VLEN (line 845). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_TYPE_INFO_T* vtype = NULL;
    nc_type vt = 0;
    int dmy = -1, vv = -1, fv_el = 9;
    nc_vlen_t fv = {1, &fv_el};
    nc_type saved_base = 0;
    SHIM_EXPECT(nc_create_mem("wm10b.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_vlen(ncid, "vt", NC_INT, &vt) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(ncid, "dmy", NC_INT, 0, NULL, &dmy) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_enddef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_redef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(ncid, "vv", vt, 0, NULL, &vv) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_var_fill(ncid, vv, 0, &fv) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(ncid, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_type(h5, vt, &vtype) == NC_NOERR && vtype != NULL,
                NC_EBADTYPE);
    saved_base = vtype->u.v.base_nc_typeid;
    vtype->u.v.base_nc_typeid = NC_NAT;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    vtype->u.v.base_nc_typeid = saved_base;
    SHIM_EXPECT(retval == NC_EBADTYPE, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* 10c. var_create_dataset H5Screate_simple error (line 981). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_DIM_INFO_T* dim = NULL;
    int did = -1, v0 = -1, v2 = -1;
    size_t saved_len = 0;
    SHIM_EXPECT(nc_create_mem("wm10c.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_dim(ncid, "d", 2, &did) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(ncid, "d0", NC_INT, 1, &did, &v0) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_enddef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_redef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(ncid, "v2", NC_INT, 1, &did, &v2) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_var_chunking(ncid, v2, NC_CONTIGUOUS, NULL) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(ncid, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    dim = (NC_DIM_INFO_T*)ncindexlookup(grp->dim, "d");
    SHIM_EXPECT(dim != NULL, NC_EBADDIM);
    saved_len = dim->len;
    dim->len = (size_t)H5S_UNLIMITED;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    dim->len = saved_len;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* 10d. var_create_dataset H5Pset_chunk error when chunk[1] == 0 (line 1005). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_DIM_INFO_T* dim_f = NULL;
    NC_VAR_INFO_T* v2_info = NULL;
    int du = -1, df = -1, vdmy = -1, v2 = -1;
    int dims[2];
    size_t cs[2] = {1, 1};
    size_t saved_flen = 0, saved_cs1 = 0;
    SHIM_EXPECT(nc_create_mem("wm10d.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_dim(ncid, "u", NC_UNLIMITED, &du) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_dim(ncid, "f", 2, &df) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(ncid, "dmy", NC_INT, 1, &df, &vdmy) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_enddef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_redef(ncid) == NC_NOERR, NC_EHDFERR);
    dims[0] = du;
    dims[1] = df;
    SHIM_EXPECT(nc_def_var(ncid, "v2", NC_INT, 2, dims, &v2) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_var_chunking(ncid, v2, NC_CHUNKED, cs) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5_var(ncid, v2, &h5, &grp, &v2_info) == NC_NOERR,
                NC_EHDFERR);
    dim_f = (NC_DIM_INFO_T*)ncindexlookup(grp->dim, "f");
    SHIM_EXPECT(dim_f != NULL && v2_info->chunksizes != NULL, NC_EBADDIM);
    saved_flen = dim_f->len;
    saved_cs1 = v2_info->chunksizes[1];
    dim_f->len = 0;
    v2_info->chunksizes[1] = 0;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    dim_f->len = saved_flen;
    v2_info->chunksizes[1] = saved_cs1;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* 10e. var_create_dataset H5Pset_chunk_cache error (line 1019). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_VAR_INFO_T* var = NULL;
    int did = -1, vid = -1;
    size_t cs[1] = {2};
    float saved_pre = 0.0f;
    SHIM_EXPECT(nc_create_mem("wm10e.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_dim(ncid, "d", 4, &did) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(ncid, "v", NC_INT, 1, &did, &vid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_var_chunking(ncid, vid, NC_CHUNKED, cs) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5_var(ncid, vid, &h5, &grp, &var) == NC_NOERR,
                NC_EHDFERR);
    saved_pre = var->chunkcache.preemption;
    var->chunkcache.preemption = 2.0f;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    var->chunkcache.preemption = saved_pre;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* 11a. attach_dimscales dsid <= 0 in child group (lines 1464, 2005, 2014). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_VAR_INFO_T* cd_info = NULL;
    NC_HDF5_VAR_INFO_T* cd_hdf5 = NULL;
    int did = -1, cd = -1, sub = -1, w = -1;
    hid_t saved_ds = -1;
    SHIM_EXPECT(nc_create_mem("wm11a.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_dim(ncid, "d", 2, &did) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(ncid, "d", NC_INT, 1, &did, &cd) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_grp(ncid, "sub", &sub) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_enddef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_redef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(sub, "w", NC_INT, 1, &did, &w) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5_var(ncid, cd, &h5, &grp, &cd_info) == NC_NOERR,
                NC_EHDFERR);
    cd_hdf5 = (NC_HDF5_VAR_INFO_T*)cd_info->format_var_info;
    saved_ds = cd_hdf5->hdf_datasetid;
    cd_hdf5->hdf_datasetid = 0;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    cd_hdf5->hdf_datasetid = saved_ds;
    SHIM_EXPECT(retval == NC_EDIMSCALE, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* 11b. attach_dimscales H5DSattach_scale error with dataspace id (line 1468). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_VAR_INFO_T* var = NULL;
    NC_DIM_INFO_T* dim = NULL;
    NC_HDF5_VAR_INFO_T* v_hdf5 = NULL;
    NC_HDF5_DIM_INFO_T* d_hdf5 = NULL;
    int did = -1, vid = -1;
    hid_t saved_dsid = -1;
    SHIM_EXPECT(nc_create_mem("wm11b.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_dim(ncid, "d", 2, &did) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(ncid, "v", NC_INT, 1, &did, &vid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_enddef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5_var(ncid, vid, &h5, &grp, &var) == NC_NOERR,
                NC_EHDFERR);
    dim = (NC_DIM_INFO_T*)ncindexlookup(grp->dim, "d");
    SHIM_EXPECT(dim != NULL, NC_EBADDIM);
    v_hdf5 = (NC_HDF5_VAR_INFO_T*)var->format_var_info;
    d_hdf5 = (NC_HDF5_DIM_INFO_T*)dim->format_dim_info;
    saved_dsid = d_hdf5->hdf_dimscaleid;
    d_hdf5->hdf_dimscaleid = sp_scalar;
    v_hdf5->dimscale_attached[0] = NC_FALSE;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    d_hdf5->hdf_dimscaleid = saved_dsid;
    v_hdf5->dimscale_attached[0] = NC_TRUE;
    SHIM_EXPECT(retval == NC_EDIMSCALE, NC_EINVAL);
    retval = NC_NOERR;
    SHIM_EXPECT(nc_close(ncid) == NC_NOERR, NC_EHDFERR);
    ncid = -1;
  }

  /* 12a. var_exists H5Lexists(-1) during became_coord_var (lines 1498, 1619). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_HDF5_GRP_INFO_T* hdf5_grp = NULL;
    int dx = -1, vy = -1;
    hid_t saved_gid = -1;
    SHIM_EXPECT(nc_create_mem("wm12a.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_dim(ncid, "x", 2, &dx) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(ncid, "y", NC_INT, 1, &dx, &vy) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_enddef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_redef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_rename_var(ncid, vy, "x") == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(ncid, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    hdf5_grp = (NC_HDF5_GRP_INFO_T*)grp->format_grp_info;
    saved_gid = hdf5_grp->hdf_grpid;
    hdf5_grp->hdf_grpid = -1;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    hdf5_grp->hdf_grpid = saved_gid;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* 12b. var_exists dangling soft link on replace_existing_var (1505, 1643). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_HDF5_GRP_INFO_T* hdf5_grp = NULL;
    int sub = -1, dx = -1, vx = -1, fv1 = 1, fv2 = 2;
    SHIM_EXPECT(nc_create_mem("wm12b.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_grp(ncid, "sub", &sub) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_dim(sub, "x", 2, &dx) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(sub, "x", NC_INT, 1, &dx, &vx) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_var_fill(sub, vx, 0, &fv1) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_enddef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_redef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_put_att_int(sub, vx, NC_FillValue, NC_INT, 1, &fv2) ==
                    NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(sub, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    hdf5_grp = (NC_HDF5_GRP_INFO_T*)grp->format_grp_info;
    SHIM_EXPECT(H5Ldelete(hdf5_grp->hdf_grpid, "x", H5P_DEFAULT) >= 0,
                NC_EHDFERR);
    SHIM_EXPECT(H5Lcreate_soft("/no_such_target", hdf5_grp->hdf_grpid, "x",
                               H5P_DEFAULT, H5P_DEFAULT) >= 0,
                NC_EHDFERR);
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    H5Ldelete(hdf5_grp->hdf_grpid, "x", H5P_DEFAULT);
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* 12c. write_var replace_existing_var d1->coord_var == NULL (line 1652) +
   * rec_detach_scales error (line 1660). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    int sub = -1, dx = -1, dy = -1, vx = -1, fv1 = 1, fv2 = 2;
    SHIM_EXPECT(nc_create_mem("wm12c.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_grp(ncid, "sub", &sub) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_dim(sub, "x", 2, &dx) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_dim(sub, "y", 2, &dy) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(sub, "x", NC_INT, 1, &dy, &vx) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_var_fill(sub, vx, 0, &fv1) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_enddef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_redef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_put_att_int(sub, vx, NC_FillValue, NC_INT, 1, &fv2) ==
                    NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(sub, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    SHIM_EXPECT(retval == NC_EDIMSCALE, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* 12d/e. remove_coord_atts errors (1537, 1541, 1551, 1559, 1673) +
   * H5DSdetach_scale on was_coord_var (line 1693). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_VAR_INFO_T* var = NULL;
    NC_HDF5_VAR_INFO_T* v_hdf5 = NULL;
    int dx = -1, vx = -1;
    hid_t saved_ds = -1;
    SHIM_EXPECT(nc_create_mem("wm12d.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_dim(ncid, "x", 2, &dx) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(ncid, "x", NC_INT, 1, &dx, &vx) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_enddef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_redef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_rename_var(ncid, vx, "v_renamed") == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5_var(ncid, vx, &h5, &grp, &var) == NC_NOERR,
                NC_EHDFERR);
    v_hdf5 = (NC_HDF5_VAR_INFO_T*)var->format_var_info;
    saved_ds = v_hdf5->hdf_datasetid;

    v_hdf5->hdf_datasetid = -1;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    v_hdf5->hdf_datasetid = saved_ds;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);

    v_hdf5->hdf_datasetid = ro_ds_all;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    v_hdf5->hdf_datasetid = saved_ds;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);

    v_hdf5->hdf_datasetid = ro_ds_class;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    v_hdf5->hdf_datasetid = saved_ds;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);

    v_hdf5->hdf_datasetid = ro_ds_name;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    v_hdf5->hdf_datasetid = saved_ds;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);

    v_hdf5->hdf_datasetid = ro_ds_plain;
    v_hdf5->dimscale_attached[0] = NC_TRUE;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    v_hdf5->hdf_datasetid = saved_ds;
    v_hdf5->dimscale_attached[0] = NC_FALSE;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* 12f. write_var H5Dclose(-1) on replace_existing_var (line 1704). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_VAR_INFO_T* var = NULL;
    NC_HDF5_VAR_INFO_T* v_hdf5 = NULL;
    int sub = -1, vid = -1, fv1 = 1, fv2 = 2;
    hid_t saved_ds = -1;
    SHIM_EXPECT(nc_create_mem("wm12f.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_grp(ncid, "sub", &sub) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(sub, "v", NC_INT, 0, NULL, &vid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_var_fill(sub, vid, 0, &fv1) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_enddef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_redef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_put_att_int(sub, vid, NC_FillValue, NC_INT, 1, &fv2) ==
                    NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5_var(sub, vid, &h5, &grp, &var) == NC_NOERR,
                NC_EHDFERR);
    v_hdf5 = (NC_HDF5_VAR_INFO_T*)var->format_var_info;
    saved_ds = v_hdf5->hdf_datasetid;
    v_hdf5->hdf_datasetid = -1;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    v_hdf5->hdf_datasetid = saved_ds;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* 12g. write_var H5Gunlink error on replace_existing_var (line 1709). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_HDF5_GRP_INFO_T* hdf5_grp = NULL;
    int sub = -1, vid = -1, fv1 = 1, fv2 = 2;
    SHIM_EXPECT(nc_create_mem("wm12g.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_grp(ncid, "sub", &sub) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(sub, "v", NC_INT, 0, NULL, &vid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_var_fill(sub, vid, 0, &fv1) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_enddef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_redef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_put_att_int(sub, vid, NC_FillValue, NC_INT, 1, &fv2) ==
                    NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(sub, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    hdf5_grp = (NC_HDF5_GRP_INFO_T*)grp->format_grp_info;
    SHIM_EXPECT(H5Ldelete(hdf5_grp->hdf_grpid, "v", H5P_DEFAULT) >= 0,
                NC_EHDFERR);
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    SHIM_EXPECT(retval == NC_EDIMMETA, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* 12h. write_var rec_reattach_scales error on replace_existing_var (1734). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_VAR_INFO_T* u_info = NULL;
    NC_HDF5_VAR_INFO_T* u_hdf5 = NULL;
    int sub = -1, d = -1, cd = -1, vu = -1, fv1 = 1, fv2 = 2;
    hid_t saved_u_ds = -1;
    SHIM_EXPECT(nc_create_mem("wm12h.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_grp(ncid, "sub", &sub) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_dim(sub, "d", 2, &d) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(sub, "d", NC_INT, 1, &d, &cd) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(sub, "u", NC_INT, 1, &d, &vu) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_var_fill(sub, cd, 0, &fv1) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_enddef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_redef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_put_att_int(sub, cd, NC_FillValue, NC_INT, 1, &fv2) ==
                    NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5_var(sub, vu, &h5, &grp, &u_info) == NC_NOERR,
                NC_EHDFERR);
    u_hdf5 = (NC_HDF5_VAR_INFO_T*)u_info->format_var_info;
    saved_u_ds = u_hdf5->hdf_datasetid;
    u_hdf5->dimscale_attached[0] = NC_FALSE;
    u_hdf5->hdf_datasetid = -1;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    u_hdf5->hdf_datasetid = saved_u_ds;
    u_hdf5->dimscale_attached[0] = NC_TRUE;
    SHIM_EXPECT(retval == NC_EDIMSCALE, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* 13a. nc4_create_dim_wo_var H5Screate_simple(H5S_UNLIMITED) error
   * (lines 1814, 1881, 1980). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_DIM_INFO_T* dim = NULL;
    int did = -1;
    size_t saved_len = 0;
    SHIM_EXPECT(nc_create_mem("wm13a.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_dim(ncid, "d", 2, &did) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(ncid, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    dim = (NC_DIM_INFO_T*)ncindexlookup(grp->dim, "d");
    SHIM_EXPECT(dim != NULL, NC_EBADDIM);
    saved_len = dim->len;
    dim->len = (size_t)H5S_UNLIMITED;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    dim->len = saved_len;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* 13b. nc4_create_dim_wo_var H5Dcreate2 on read-only ro_grpid (line 1829). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_DIM_INFO_T* dim = NULL;
    NC_HDF5_GRP_INFO_T* hdf5_grp = NULL;
    NC_HDF5_DIM_INFO_T* d_hdf5 = NULL;
    int did = -1;
    hid_t saved_gid = -1;
    SHIM_EXPECT(nc_create_mem("wm13b.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_dim(ncid, "d", 2, &did) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(ncid, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    dim = (NC_DIM_INFO_T*)ncindexlookup(grp->dim, "d");
    SHIM_EXPECT(dim != NULL, NC_EBADDIM);
    hdf5_grp = (NC_HDF5_GRP_INFO_T*)grp->format_grp_info;
    d_hdf5 = (NC_HDF5_DIM_INFO_T*)dim->format_dim_info;
    saved_gid = hdf5_grp->hdf_grpid;
    hdf5_grp->hdf_grpid = ro_grpid;
    retval = nc4_rec_write_metadata(h5->root_grp, 0);
    d_hdf5->hdf_dimscaleid = 0;
    hdf5_grp->hdf_grpid = saved_gid;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);
    retval = NC_NOERR;
    nc_abort(ncid);
    ncid = -1;
  }

  /* 13c. write_dim write_netcdf4_dimid(-1) error (line 1922). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_DIM_INFO_T* dim = NULL;
    NC_HDF5_DIM_INFO_T* d_hdf5 = NULL;
    int did = -1;
    hid_t saved_dsid = -1;
    SHIM_EXPECT(nc_create_mem("wm13c.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_dim(ncid, "d", 2, &did) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_enddef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(ncid, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    dim = (NC_DIM_INFO_T*)ncindexlookup(grp->dim, "d");
    SHIM_EXPECT(dim != NULL, NC_EBADDIM);
    d_hdf5 = (NC_HDF5_DIM_INFO_T*)dim->format_dim_info;
    saved_dsid = d_hdf5->hdf_dimscaleid;
    d_hdf5->hdf_dimscaleid = -1;
    retval = nc4_rec_write_metadata(h5->root_grp, NC_TRUE);
    d_hdf5->hdf_dimscaleid = saved_dsid;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);
    retval = NC_NOERR;
    SHIM_EXPECT(nc_close(ncid) == NC_NOERR, NC_EHDFERR);
    ncid = -1;
  }

cleanup:
  if (ncid >= 0) nc_abort(ncid);
  if (sp_scalar >= 0) H5Sclose(sp_scalar);
  if (ro_ds_plain >= 0) H5Dclose(ro_ds_plain);
  if (ro_ds_name >= 0) H5Dclose(ro_ds_name);
  if (ro_ds_class >= 0) H5Dclose(ro_ds_class);
  if (ro_ds_all >= 0) H5Dclose(ro_ds_all);
  if (ro_grpid >= 0) H5Gclose(ro_grpid);
  if (ro_fid >= 0) H5Fclose(ro_fid);
  return retval;
}

int Nc4HdfTestMatchDimscalesAndIsNc4Errors(const char* raw_file_path) {
  int retval = NC_NOERR;
  int ncid = -1;

  if (!raw_file_path) {
    return NC_EINVAL;
  }

  /* 1. Child recursion + H5Dget_space error (lines 2102, 2202). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* sub_grp = NULL;
    NC_VAR_INFO_T* var = NULL;
    NC_HDF5_VAR_INFO_T* hdf5_var = NULL;
    int sub = -1, did = -1, vid = -1;
    hid_t saved_ds = -1;
    SHIM_EXPECT(nc_create_mem("md1.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_grp(ncid, "sub", &sub) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_dim(sub, "d", 2, &did) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(sub, "v", NC_INT, 1, &did, &vid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_enddef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5_var(sub, vid, &h5, &sub_grp, &var) == NC_NOERR,
                NC_EHDFERR);
    hdf5_var = (NC_HDF5_VAR_INFO_T*)var->format_var_info;
    saved_ds = hdf5_var->hdf_datasetid;
    hdf5_var->hdf_datasetid = -1;
    retval = nc4_rec_match_dimscales(h5->root_grp);
    hdf5_var->hdf_datasetid = saved_ds;
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);
    retval = NC_NOERR;
    SHIM_EXPECT(nc_close(ncid) == NC_NOERR, NC_EHDFERR);
    ncid = -1;
  }

  /* 2. H5Otoken_cmp error when dimscale_hdf5_objids != NULL (line 2169). */
  {
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_VAR_INFO_T* var = NULL;
    NC_HDF5_VAR_INFO_T* hdf5_var = NULL;
    HDF5_OBJID_T* fake_objids = NULL;
    int did = -1, vid = -1;
    hid_t saved_ds = -1;
    SHIM_EXPECT(nc_create_mem("md2.nc", NC_NETCDF4, 0, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_def_dim(ncid, "d", 2, &did) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc_def_var(ncid, "v", NC_INT, 1, &did, &vid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc_enddef(ncid) == NC_NOERR, NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5_var(ncid, vid, &h5, &grp, &var) == NC_NOERR,
                NC_EHDFERR);
    hdf5_var = (NC_HDF5_VAR_INFO_T*)var->format_var_info;
    fake_objids = (HDF5_OBJID_T*)calloc(1, sizeof(HDF5_OBJID_T));
    SHIM_EXPECT(fake_objids != NULL, NC_ENOMEM);
    saved_ds = hdf5_var->hdf_datasetid;
    hdf5_var->dimscale_hdf5_objids = fake_objids;
    hdf5_var->hdf_datasetid = -1;
    retval = nc4_rec_match_dimscales(h5->root_grp);
    hdf5_var->hdf_datasetid = saved_ds;
    hdf5_var->dimscale_hdf5_objids = NULL;
    free(fake_objids);
    SHIM_EXPECT(retval == NC_EHDFERR, NC_EINVAL);
    retval = NC_NOERR;
    SHIM_EXPECT(nc_close(ncid) == NC_NOERR, NC_EHDFERR);
    ncid = -1;
  }

  /* 3. NC4_root_att_exists H5Aexists < 0 (line 2561) and NC4_walk failure
   * (line 2533) on raw HDF5 file without _NCProperties/_nc3_strict. */
  {
    hid_t fid = -1, sid = -1, dsid = -1;
    hsize_t dims[1] = {2};
    NC_FILE_INFO_T* h5 = NULL;
    NC_GRP_INFO_T* grp = NULL;
    NC_VAR_INFO_T* var = NULL;
    NC_HDF5_GRP_INFO_T* hdf5_grp = NULL;
    NC_HDF5_VAR_INFO_T* hdf5_var = NULL;
    hid_t saved_gid = -1;
    int is_nc4 = -1;

    fid = H5Fcreate(raw_file_path, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    sid = H5Screate_simple(1, dims, NULL);
    if (fid >= 0 && sid >= 0) {
      dsid = H5Dcreate2(fid, "raw_ds", H5T_NATIVE_INT, sid, H5P_DEFAULT,
                        H5P_DEFAULT, H5P_DEFAULT);
    }
    if (dsid >= 0) H5Dclose(dsid);
    if (sid >= 0) H5Sclose(sid);
    if (fid >= 0) H5Fclose(fid);
    SHIM_EXPECT(fid >= 0 && sid >= 0 && dsid >= 0, NC_EHDFERR);

    SHIM_EXPECT(nc_open(raw_file_path, NC_NOWRITE, &ncid) == NC_NOERR,
                NC_EHDFERR);
    SHIM_EXPECT(nc4_find_grp_h5(ncid, &grp, &h5) == NC_NOERR, NC_EHDFERR);
    var = (NC_VAR_INFO_T*)ncindexlookup(grp->vars, "raw_ds");
    SHIM_EXPECT(var != NULL, NC_ENOTVAR);
    hdf5_grp = (NC_HDF5_GRP_INFO_T*)h5->root_grp->format_grp_info;
    hdf5_var = (NC_HDF5_VAR_INFO_T*)var->format_var_info;
    saved_gid = hdf5_grp->hdf_grpid;

    hdf5_grp->hdf_grpid = -1;
    is_nc4 = NC4_isnetcdf4(h5);
    (void)is_nc4;

    hdf5_grp->hdf_grpid = hdf5_var->hdf_datasetid;
    is_nc4 = NC4_isnetcdf4(h5);
    hdf5_grp->hdf_grpid = saved_gid;
    SHIM_EXPECT(is_nc4 == 0, NC_EINVAL);

    SHIM_EXPECT(nc_close(ncid) == NC_NOERR, NC_EHDFERR);
    ncid = -1;
  }

cleanup:
  if (ncid >= 0) nc_abort(ncid);
  return retval;
}

#undef SHIM_EXPECT
#undef SHIM_CHECK_NC
