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

#include "third_party/netcdf/google/libhdf5/hdf5attr_shim.h"

#include "third_party/netcdf/config.h"  // GOOGLE MODIFICATION
#include "third_party/netcdf/include/hdf5internal.h"
#include "third_party/netcdf/include/nc4internal.h"

extern int nc4_get_att_special(NC_FILE_INFO_T* h5, const char* name,
                               nc_type* filetypep, nc_type mem_type,
                               size_t* lenp, int* attnump, void* data);

int Hdf5CreateRawHdf5File(const char* path) {
  hid_t fcpl_id = -1;
  hid_t file_id = -1;
  int status = -1;

  if ((fcpl_id = H5Pcreate(H5P_FILE_CREATE)) < 0) {
    goto done;
  }
  if (H5Pset_link_creation_order(
          fcpl_id, (H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED)) < 0) {
    goto done;
  }
  if (H5Pset_attr_creation_order(
          fcpl_id, (H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED)) < 0) {
    goto done;
  }
  file_id = H5Fcreate(path, H5F_ACC_TRUNC, fcpl_id, H5P_DEFAULT);
  if (file_id < 0) {
    goto done;
  }
  status = 0;

done:
  if (file_id >= 0 && H5Fclose(file_id) < 0) {
    status = -1;
  }
  if (fcpl_id >= 0 && H5Pclose(fcpl_id) < 0) {
    status = -1;
  }
  return status;
}

int Hdf5CallGetAttSpecial(int ncid, const char* name, nc_type mem_type,
                          int query_attnum, int pass_null_outputs,
                          nc_type* filetype_out, size_t* len_out,
                          void* data_out) {
  NC_GRP_INFO_T* grp = NULL;
  NC_FILE_INFO_T* h5 = NULL;
  int attnum = -1;
  int stat = nc_initialize();
  if (stat != NC_NOERR) {
    return stat;
  }
  if ((stat = nc4_find_grp_h5(ncid, &grp, &h5)) != NC_NOERR) {
    return stat;
  }
  return nc4_get_att_special(
      h5, name, pass_null_outputs ? NULL : filetype_out, mem_type,
      pass_null_outputs ? NULL : len_out, query_attnum ? &attnum : NULL,
      data_out);
}

int Hdf5PutAttDirect(int ncid, int varid, const char* name, nc_type file_type,
                     size_t len, const void* data, nc_type mem_type,
                     int force) {
  NC_GRP_INFO_T* grp = NULL;
  int stat = nc_initialize();
  if (stat != NC_NOERR) {
    return stat;
  }
  if ((stat = nc4_find_nc4_grp(ncid, &grp)) != NC_NOERR) {
    return stat;
  }
  return nc4_put_att(grp, varid, name, file_type, len, data, mem_type, force);
}
