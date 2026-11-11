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

#include "third_party/netcdf/google/libdispatch/dvarinq_shim.h"

#include <stddef.h>

#include "third_party/netcdf/config.h"
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_filter.h"
#include "third_party/netcdf/include/ncdispatch.h"

static NC_Dispatch g_dvarinq_udf_dispatch;
static int (*g_dvarinq_orig_inq_var_filter_info)(
    int ncid, int varid, unsigned int id, size_t* nparamsp,
    unsigned int* params) = NULL;

static int dvarinq_udf_inq_var_filter_info(
    int ncid, int varid, unsigned int id, size_t* nparamsp,
    unsigned int* params) {
  if (varid == 0) {
    return NC_ENOTNC4;
  }
  if (varid == 1 && id == H5Z_FILTER_DEFLATE) {
    if (nparamsp) *nparamsp = 2;
    if (params) {
      params[0] = 1U;
      params[1] = 2U;
    }
    return NC_NOERR;
  }
  if (varid == 2 && id == H5Z_FILTER_SZIP) {
    if (nparamsp) *nparamsp = 1;
    if (params) {
      params[0] = 0U;
    }
    return NC_NOERR;
  }
  if (varid == 3 && id == H5Z_FILTER_SZIP) {
    if (nparamsp) *nparamsp = 3;
    if (params) {
      params[0] = NC_SZIP_EC;
      params[1] = 8U;
      params[2] = 77U;
    }
    return NC_NOERR;
  }
  if (g_dvarinq_orig_inq_var_filter_info) {
    return g_dvarinq_orig_inq_var_filter_info(ncid, varid, id, nparamsp,
                                              params);
  }
  return NC_ENOTNC4;
}

int netcdf_dvarinq_register_udf(void) {
  int stat = NC_NOERR;
  if (!NC_initialized) {
    stat = nc_initialize();
    if (stat != NC_NOERR) return stat;
  }
  if (!NC3_dispatch_table) {
    return NC_ENOTBUILT;
  }
  g_dvarinq_udf_dispatch = *NC3_dispatch_table;
  g_dvarinq_udf_dispatch.model = NC_FORMATX_UDF9;
  g_dvarinq_orig_inq_var_filter_info = NC3_dispatch_table->inq_var_filter_info;
  g_dvarinq_udf_dispatch.inq_var_filter_info = dvarinq_udf_inq_var_filter_info;
  return nc_def_user_format(NC_UDF9, &g_dvarinq_udf_dispatch, NULL);
}
