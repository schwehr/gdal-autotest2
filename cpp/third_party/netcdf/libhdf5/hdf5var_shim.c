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

#include "third_party/netcdf/google/libhdf5/hdf5var_shim.h"

#include <string.h>

// clang-format off
#include "third_party/netcdf/config.h"
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/hdf5internal.h"
// clang-format on

int NetcdfGoogleCreateRawHdf5VarFixture(const char* path) {
  int status = -1;
  hid_t fcpl_id = -1;
  hid_t fapl_id = -1;
  hid_t file_id = -1;
  hid_t fstr_type = -1;
  hid_t fstr_space = -1;
  hid_t fstr_dset = -1;
  hid_t scalar_space = -1;
  hid_t scalar_dset = -1;
  hid_t flt_dcpl = -1;
  hid_t flt_space = -1;
  hid_t flt_dset = -1;

  if (path == NULL) {
    return -1;
  }

  /* NetCDF-4 requires tracked link creation order on root group for NC_WRITE. */
  fcpl_id = H5Pcreate(H5P_FILE_CREATE);
  if (fcpl_id < 0) {
    goto cleanup;
  }
  if (H5Pset_obj_track_times(fcpl_id, 0) < 0 ||
      H5Pset_link_creation_order(
          fcpl_id, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED) < 0 ||
      H5Pset_attr_creation_order(
          fcpl_id, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED) < 0) {
    goto cleanup;
  }

  fapl_id = H5Pcreate(H5P_FILE_ACCESS);
  if (fapl_id < 0) {
    goto cleanup;
  }
  if (H5Pset_libver_bounds(fapl_id, H5F_LIBVER_V18, H5F_LIBVER_LATEST) < 0) {
    goto cleanup;
  }

  file_id = H5Fcreate(path, H5F_ACC_TRUNC, fcpl_id, fapl_id);
  if (file_id < 0) {
    goto cleanup;
  }

  fstr_type = H5Tcopy(H5T_C_S1);
  if (fstr_type < 0) {
    goto cleanup;
  }
  if (H5Tset_size(fstr_type, 8) < 0 ||
      H5Tset_strpad(fstr_type, H5T_STR_NULLPAD) < 0) {
    goto cleanup;
  }

  {
    const hsize_t dims1[1] = {3};
    char buf1[3 * 8];
    memset(buf1, 0, sizeof(buf1));
    memcpy(buf1, "alpha", 5);
    memcpy(buf1 + 8, "bravo", 5);
    memcpy(buf1 + 16, "12345678", 8);

    fstr_space = H5Screate_simple(1, dims1, NULL);
    if (fstr_space < 0) {
      goto cleanup;
    }
    fstr_dset = H5Dcreate2(file_id, "fixed_str_1d", fstr_type, fstr_space,
                           H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (fstr_dset < 0) {
      goto cleanup;
    }
    if (H5Dwrite(fstr_dset, fstr_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf1) <
        0) {
      goto cleanup;
    }
  }

  {
    char scalar_buf[8];
    memset(scalar_buf, 0, sizeof(scalar_buf));
    memcpy(scalar_buf, "scalar8!", 8);

    scalar_space = H5Screate(H5S_SCALAR);
    if (scalar_space < 0) {
      goto cleanup;
    }
    scalar_dset = H5Dcreate2(file_id, "fixed_str_scalar", fstr_type,
                             scalar_space, H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT);
    if (scalar_dset < 0) {
      goto cleanup;
    }
    if (H5Dwrite(scalar_dset, fstr_type, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                 scalar_buf) < 0) {
      goto cleanup;
    }
  }

  {
    const hsize_t chunk_dims[1] = {2};
    const hsize_t dims_flt[1] = {4};
    const int vals[4] = {10, 20, 30, 40};

    flt_dcpl = H5Pcreate(H5P_DATASET_CREATE);
    if (flt_dcpl < 0) {
      goto cleanup;
    }
    if (H5Pset_chunk(flt_dcpl, 1, chunk_dims) < 0 ||
        H5Pset_filter(flt_dcpl, (H5Z_filter_t)31999, H5Z_FLAG_OPTIONAL, 0,
                      NULL) < 0) {
      goto cleanup;
    }
    flt_space = H5Screate_simple(1, dims_flt, NULL);
    if (flt_space < 0) {
      goto cleanup;
    }
    flt_dset = H5Dcreate2(file_id, "missing_filter_var", H5T_NATIVE_INT,
                          flt_space, H5P_DEFAULT, flt_dcpl, H5P_DEFAULT);
    if (flt_dset < 0) {
      goto cleanup;
    }
    if (H5Dwrite(flt_dset, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                 vals) < 0) {
      goto cleanup;
    }
  }

  status = 0;

cleanup:
  if (flt_dset >= 0) H5Dclose(flt_dset);
  if (flt_space >= 0) H5Sclose(flt_space);
  if (flt_dcpl >= 0) H5Pclose(flt_dcpl);
  if (scalar_dset >= 0) H5Dclose(scalar_dset);
  if (scalar_space >= 0) H5Sclose(scalar_space);
  if (fstr_dset >= 0) H5Dclose(fstr_dset);
  if (fstr_space >= 0) H5Sclose(fstr_space);
  if (fstr_type >= 0) H5Tclose(fstr_type);
  if (file_id >= 0 && H5Fclose(file_id) < 0) {
    status = -1;
  }
  if (fapl_id >= 0) H5Pclose(fapl_id);
  if (fcpl_id >= 0) H5Pclose(fcpl_id);
  return status;
}
