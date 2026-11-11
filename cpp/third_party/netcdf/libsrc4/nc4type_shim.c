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

#include "third_party/netcdf/google/libsrc4/nc4type_shim.h"

#include <stddef.h>
#include <string.h>

#include "third_party/netcdf/config.h"
#include "third_party/netcdf/include/nc4dispatch.h"
#include "third_party/netcdf/include/nc4internal.h"

int nc4type_test_inq_type(int ncid, nc_type typeid1, char *name, size_t *size) {
  return NC4_inq_type(ncid, typeid1, name, size);
}

int nc4type_test_inq_type_fixed_size(int ncid, nc_type xtype, int *fixedsizep) {
  return NC4_inq_type_fixed_size(ncid, xtype, fixedsizep);
}

int nc4type_test_get_typeclass(int ncid, nc_type xtype, int *type_class) {
  NC_FILE_INFO_T *h5 = NULL;
  int retval = nc4_find_grp_h5(ncid, NULL, &h5);
  if (retval) {
    return retval;
  }
  return nc4_get_typeclass(h5, xtype, type_class);
}

int nc4type_test_set_varsize(nc_type type_id, nc_type type_class,
                             int *out_varsized) {
  NC_TYPE_INFO_T info;
  int retval;

  memset(&info, 0, sizeof(info));
  info.hdr.sort = NCTYP;
  info.hdr.id = type_id;
  info.nc_type_class = type_class;
  retval = NC4_set_varsize(&info);
  if (out_varsized) {
    *out_varsized = info.varsized;
  }
  return retval;
}

int nc4type_test_var_varsized(nc_type type_id, int initial_varsized) {
  NC_TYPE_INFO_T info;
  NC_VAR_INFO_T var;

  memset(&info, 0, sizeof(info));
  memset(&var, 0, sizeof(var));
  info.hdr.sort = NCTYP;
  info.hdr.id = type_id;
  info.varsized = initial_varsized;
  var.hdr.sort = NCVAR;
  var.type_info = &info;
  return NC4_var_varsized(&var);
}
