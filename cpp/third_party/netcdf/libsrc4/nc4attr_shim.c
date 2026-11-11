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

#include "third_party/netcdf/google/libsrc4/nc4attr_shim.h"

#include <stddef.h>

#include "third_party/netcdf/config.h"
#include "third_party/netcdf/include/nc.h"
#include "third_party/netcdf/include/nc4dispatch.h"
#include "third_party/netcdf/include/nc4internal.h"
#include "third_party/netcdf/include/ncdispatch.h"
#include "third_party/netcdf/include/ncindex.h"
#include "third_party/netcdf/include/netcdf.h"

// Internal non-static helper in libsrc4/nc4attr.c without a header prototype.
extern int nc4_get_att(int ncid, int varid, const char* name, nc_type* xtype,
                       nc_type mem_type, size_t* lenp, int* attnum, void* data);

int NetcdfTestNc4InqAtt(int ncid, int varid, const char* name, nc_type* xtypep,
                        size_t* lenp) {
  return NC4_inq_att(ncid, varid, name, xtypep, lenp);
}

int NetcdfTestNc4InqAttid(int ncid, int varid, const char* name, int* attnump) {
  return NC4_inq_attid(ncid, varid, name, attnump);
}

int NetcdfTestNc4InqAttname(int ncid, int varid, int attnum, char* name) {
  return NC4_inq_attname(ncid, varid, attnum, name);
}

int NetcdfTestNc4GetAtt(int ncid, int varid, const char* name, void* value,
                        nc_type memtype) {
  return NC4_get_att(ncid, varid, name, value, memtype);
}

int NetcdfTestNc4GetAttRaw(int ncid, int varid, const char* name,
                           nc_type* xtypep, nc_type mem_type, size_t* lenp,
                           int* attnump, void* data) {
  return nc4_get_att(ncid, varid, name, xtypep, mem_type, lenp, attnump, data);
}

int NetcdfTestNc4GetAttPtrsNullName(int ncid, int varid, nc_type* xtypep,
                                    nc_type mem_type, size_t* lenp,
                                    int* attnump, void* data) {
  NC_FILE_INFO_T* h5 = NULL;
  NC_GRP_INFO_T* grp = NULL;
  NC_VAR_INFO_T* var = NULL;
  const int retval = nc4_find_grp_h5(ncid, &grp, &h5);
  if (retval != NC_NOERR) {
    return retval;
  }
  if (varid != NC_GLOBAL) {
    if (varid < 0) {
      return NC_ENOTVAR;
    }
    var = (NC_VAR_INFO_T*)ncindexith(grp->vars, (size_t)varid);
    if (var == NULL) {
      return NC_ENOTVAR;
    }
  }
  return nc4_get_att_ptrs(h5, grp, var, NULL, xtypep, mem_type, lenp, attnump,
                          data);
}
