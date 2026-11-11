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

#include "third_party/netcdf/google/libhdf5/nc4info_shim.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "third_party/netcdf/config.h"
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/nc4internal.h"
#include "third_party/netcdf/include/hdf5internal.h"
#include "third_party/netcdf/include/nc_provenance.h"

extern void ncprintprovenance(NC4_Provenance* info);

static char* ShimDupString(const char* src) {
  size_t len = 0;
  char* copy = NULL;
  if (src == NULL) {
    return NULL;
  }
  len = strlen(src);
  copy = (char*)malloc(len + 1);
  if (copy != NULL) {
    memcpy(copy, src, len + 1);
  }
  return copy;
}

static void InitFakeNc4File(NC_FILE_INFO_T* h5,
                            NC_HDF5_FILE_INFO_T* hdf5_file,
                            NC_GRP_INFO_T* root_grp,
                            NC_HDF5_GRP_INFO_T* hdf5_grp,
                            hid_t hdfid, hid_t grpid) {
  memset(h5, 0, sizeof(*h5));
  memset(hdf5_file, 0, sizeof(*hdf5_file));
  memset(root_grp, 0, sizeof(*root_grp));
  memset(hdf5_grp, 0, sizeof(*hdf5_grp));

  hdf5_file->hdfid = hdfid;
  hdf5_grp->hdf_grpid = grpid;
  root_grp->hdr.id = 0x10000;
  root_grp->nc4_info = h5;
  root_grp->format_grp_info = hdf5_grp;
  h5->hdr.id = 0x10000;
  h5->root_grp = root_grp;
  h5->format_file_info = hdf5_file;
}

int Nc4InfoShimProvenanceInit(void) {
  return NC4_provenance_init();
}

int Nc4InfoShimProvenanceFinalize(void) {
  return NC4_provenance_finalize();
}

int Nc4InfoShimNewProvenanceBadHdfid(void) {
  NC_FILE_INFO_T h5;
  NC_HDF5_FILE_INFO_T hdf5_file;
  NC_GRP_INFO_T root_grp;
  NC_HDF5_GRP_INFO_T hdf5_grp;
  H5E_auto2_t prev_func = NULL;
  void* prev_data = NULL;
  int stat = NC_NOERR;

  H5Eget_auto2(H5E_DEFAULT, &prev_func, &prev_data);
  H5Eset_auto2(H5E_DEFAULT, NULL, NULL);
  InitFakeNc4File(&h5, &hdf5_file, &root_grp, &hdf5_grp, -1, -1);
  stat = NC4_new_provenance(&h5);
  NC4_clear_provenance(&h5.provenance);
  H5Eset_auto2(H5E_DEFAULT, prev_func, prev_data);
  return stat;
}

int Nc4InfoShimNewProvenanceAfterFinalize(const char* path,
                                          int* out_has_props,
                                          int* out_version,
                                          int* out_superblock) {
  NC_FILE_INFO_T h5;
  NC_HDF5_FILE_INFO_T hdf5_file;
  NC_GRP_INFO_T root_grp;
  NC_HDF5_GRP_INFO_T hdf5_grp;
  hid_t fid = H5I_INVALID_HID;
  hid_t gid = H5I_INVALID_HID;
  int stat = NC_NOERR;

  if (path == NULL) {
    return NC_EINVAL;
  }
  stat = NC4_provenance_init();
  if (stat != NC_NOERR) {
    return stat;
  }
  stat = NC4_provenance_finalize();
  if (stat != NC_NOERR) {
    return stat;
  }
  /* Second finalize should be safe and hit NULL ncproperties in clear. */
  stat = NC4_provenance_finalize();
  if (stat != NC_NOERR) {
    return stat;
  }
  /* Init again after finalize triggers the globalpropinitialized guard. */
  stat = NC4_provenance_init();
  if (stat != NC_NOERR) {
    return stat;
  }

  fid = H5Fcreate(path, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  if (fid < 0) {
    return NC_EHDFERR;
  }
  gid = H5Gopen2(fid, "/", H5P_DEFAULT);
  if (gid < 0) {
    H5Fclose(fid);
    return NC_EHDFERR;
  }

  InitFakeNc4File(&h5, &hdf5_file, &root_grp, &hdf5_grp, fid, gid);
  stat = NC4_new_provenance(&h5);
  if (stat == NC_NOERR) {
    if (out_has_props != NULL) {
      *out_has_props = (h5.provenance.ncproperties != NULL) ? 1 : 0;
    }
    if (out_version != NULL) {
      *out_version = h5.provenance.version;
    }
    if (out_superblock != NULL) {
      *out_superblock = h5.provenance.superblockversion;
    }
    stat = NC4_write_provenance(&h5);
  }
  NC4_clear_provenance(&h5.provenance);
  H5Gclose(gid);
  H5Fclose(fid);
  return stat;
}

int Nc4InfoShimReadProvenanceBadHdfid(void) {
  NC_FILE_INFO_T h5;
  NC_HDF5_FILE_INFO_T hdf5_file;
  NC_GRP_INFO_T root_grp;
  NC_HDF5_GRP_INFO_T hdf5_grp;
  H5E_auto2_t prev_func = NULL;
  void* prev_data = NULL;
  int stat = NC_NOERR;

  H5Eget_auto2(H5E_DEFAULT, &prev_func, &prev_data);
  H5Eset_auto2(H5E_DEFAULT, NULL, NULL);
  InitFakeNc4File(&h5, &hdf5_file, &root_grp, &hdf5_grp, -1, -1);
  stat = NC4_read_provenance(&h5);
  NC4_clear_provenance(&h5.provenance);
  H5Eset_auto2(H5E_DEFAULT, prev_func, prev_data);
  return stat;
}

int Nc4InfoShimWriteProvenanceError(const char* path, int no_write,
                                    int bad_grpid, int empty_props) {
  NC_FILE_INFO_T h5;
  NC_HDF5_FILE_INFO_T hdf5_file;
  NC_GRP_INFO_T root_grp;
  NC_HDF5_GRP_INFO_T hdf5_grp;
  hid_t fid = H5I_INVALID_HID;
  hid_t gid = H5I_INVALID_HID;
  H5E_auto2_t prev_func = NULL;
  void* prev_data = NULL;
  int stat = NC_NOERR;

  H5Eget_auto2(H5E_DEFAULT, &prev_func, &prev_data);
  H5Eset_auto2(H5E_DEFAULT, NULL, NULL);
  InitFakeNc4File(&h5, &hdf5_file, &root_grp, &hdf5_grp, -1, -1);

  if (no_write) {
    h5.no_write = NC_TRUE;
    stat = NC4_write_provenance(&h5);
  } else if (bad_grpid) {
    h5.provenance.ncproperties = ShimDupString("version=2");
    stat = NC4_write_provenance(&h5);
  } else if (empty_props) {
    if (path == NULL) {
      stat = NC_EINVAL;
      goto cleanup;
    }
    fid = H5Fcreate(path, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (fid < 0) {
      stat = NC_EHDFERR;
      goto cleanup;
    }
    gid = H5Gopen2(fid, "/", H5P_DEFAULT);
    if (gid < 0) {
      stat = NC_EHDFERR;
      goto cleanup;
    }
    hdf5_file.hdfid = fid;
    hdf5_grp.hdf_grpid = gid;
    h5.provenance.ncproperties = ShimDupString("");
    stat = NC4_write_provenance(&h5);
  }

cleanup:
  NC4_clear_provenance(&h5.provenance);
  if (gid >= 0) {
    H5Gclose(gid);
  }
  if (fid >= 0) {
    H5Fclose(fid);
  }
  H5Eset_auto2(H5E_DEFAULT, prev_func, prev_data);
  return stat;
}

int Nc4InfoShimTestClearProvenance(void) {
  NC4_Provenance prov;
  int stat = NC_NOERR;

  stat = NC4_clear_provenance(NULL);
  if (stat != NC_NOERR) {
    return stat;
  }

  memset(&prov, 0, sizeof(prov));
  stat = NC4_clear_provenance(&prov);
  if (stat != NC_NOERR) {
    return stat;
  }

  prov.version = 2;
  prov.superblockversion = 3;
  prov.ncproperties = ShimDupString("version=2,netcdf=4.10.1");
  if (prov.ncproperties == NULL) {
    return NC_ENOMEM;
  }
  stat = NC4_clear_provenance(&prov);
  if (stat != NC_NOERR) {
    return stat;
  }
  if (prov.ncproperties != NULL || prov.version != 0 ||
      prov.superblockversion != 0) {
    return NC_EINTERNAL;
  }
  return NC_NOERR;
}

int Nc4InfoShimPrintProvenance(int version, int superblock,
                               const char* ncproperties) {
  NC4_Provenance info;
  memset(&info, 0, sizeof(info));
  info.version = version;
  info.superblockversion = superblock;
  if (ncproperties != NULL) {
    info.ncproperties = ShimDupString(ncproperties);
    if (info.ncproperties == NULL) {
      return NC_ENOMEM;
    }
  }
  ncprintprovenance(&info);
  fflush(stderr);
  NC4_clear_provenance(&info);
  return NC_NOERR;
}

int Nc4InfoShimCreateRawHdf5File(const char* path, int mode,
                                 const char* text_value) {
  hid_t fcpl = H5I_INVALID_HID;
  hid_t fid = H5I_INVALID_HID;
  hid_t gid = H5I_INVALID_HID;
  hid_t space = H5I_INVALID_HID;
  hid_t atype = H5I_INVALID_HID;
  hid_t att = H5I_INVALID_HID;
  int stat = NC_NOERR;

  if (path == NULL) {
    return NC_EINVAL;
  }
  if (mode == 0) {
    /* Track link and attribute creation order so netCDF-4 can open with
     * NC_WRITE without returning NC_ECANTWRITE in rec_read_metadata. */
    fcpl = H5Pcreate(H5P_FILE_CREATE);
    if (fcpl < 0 ||
        H5Pset_link_creation_order(
            fcpl, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED) < 0 ||
        H5Pset_attr_creation_order(
            fcpl, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED) < 0) {
      stat = NC_EHDFERR;
      goto cleanup;
    }
  }
  fid = H5Fcreate(path, H5F_ACC_TRUNC,
                  (fcpl >= 0) ? fcpl : H5P_DEFAULT, H5P_DEFAULT);
  if (fid < 0) {
    stat = NC_EHDFERR;
    goto cleanup;
  }
  gid = H5Gopen2(fid, "/", H5P_DEFAULT);
  if (gid < 0) {
    stat = NC_EHDFERR;
    goto cleanup;
  }

  if (mode == 1) {
    int val = 42;
    space = H5Screate(H5S_SCALAR);
    if (space < 0) {
      stat = NC_EHDFERR;
      goto cleanup;
    }
    att = H5Acreate1(gid, NCPROPS, H5T_NATIVE_INT, space, H5P_DEFAULT);
    if (att < 0 || H5Awrite(att, H5T_NATIVE_INT, &val) < 0) {
      stat = NC_EHDFERR;
      goto cleanup;
    }
  } else if (mode == 2) {
    size_t len = (text_value != NULL) ? strlen(text_value) : 0;
    if (len == 0) {
      stat = NC_EINVAL;
      goto cleanup;
    }
    atype = H5Tcopy(H5T_C_S1);
    if (atype < 0 || H5Tset_strpad(atype, H5T_STR_NULLTERM) < 0 ||
        H5Tset_cset(atype, H5T_CSET_ASCII) < 0 || H5Tset_size(atype, len) < 0) {
      stat = NC_EHDFERR;
      goto cleanup;
    }
    space = H5Screate(H5S_SCALAR);
    if (space < 0) {
      stat = NC_EHDFERR;
      goto cleanup;
    }
    att = H5Acreate1(gid, NCPROPS, atype, space, H5P_DEFAULT);
    if (att < 0 || H5Awrite(att, atype, text_value) < 0) {
      stat = NC_EHDFERR;
      goto cleanup;
    }
  }

cleanup:
  if (att >= 0) {
    H5Aclose(att);
  }
  if (atype >= 0) {
    H5Tclose(atype);
  }
  if (space >= 0) {
    H5Sclose(space);
  }
  if (gid >= 0) {
    H5Gclose(gid);
  }
  if (fid >= 0) {
    H5Fclose(fid);
  }
  if (fcpl >= 0) {
    H5Pclose(fcpl);
  }
  return stat;
}

int Nc4InfoShimHasRawNcPropertiesAttr(const char* path, int* exists_out) {
  hid_t fid = H5I_INVALID_HID;
  hid_t gid = H5I_INVALID_HID;
  htri_t exists = -1;

  if (path == NULL || exists_out == NULL) {
    return NC_EINVAL;
  }
  *exists_out = 0;
  fid = H5Fopen(path, H5F_ACC_RDONLY, H5P_DEFAULT);
  if (fid < 0) {
    return NC_EHDFERR;
  }
  gid = H5Gopen2(fid, "/", H5P_DEFAULT);
  if (gid < 0) {
    H5Fclose(fid);
    return NC_EHDFERR;
  }
  exists = H5Aexists(gid, NCPROPS);
  H5Gclose(gid);
  H5Fclose(fid);
  if (exists < 0) {
    return NC_EHDFERR;
  }
  *exists_out = (exists > 0) ? 1 : 0;
  return NC_NOERR;
}
