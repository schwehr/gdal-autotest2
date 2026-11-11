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

#include "third_party/netcdf/google/libdispatch/dvar_shim.h"

#include <stddef.h>

#include "third_party/netcdf/config.h"
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/ncdispatch.h"

static NC_Dispatch g_dvar_udf_dispatch;
static int g_dvar_udf_fault = kDvarFaultNone;

static int (*g_dvar_orig_inq_dim)(int ncid, int dimid, char* name,
                                  size_t* lenp) = NULL;
static int (*g_dvar_orig_inq_unlimdim)(int ncid, int* unlimdimidp) = NULL;
static int (*g_dvar_orig_inq_unlimdims)(int ncid, int* nunlimdimsp,
                                        int* unlimdimidsp) = NULL;
static int (*g_dvar_orig_inq_var_all)(
    int ncid, int varid, char* name, nc_type* xtypep, int* ndimsp,
    int* dimidsp, int* nattsp, int* shufflep, int* deflatep,
    int* deflate_levelp, int* fletcher32p, int* contiguousp,
    size_t* chunksizesp, int* no_fill, void* fill_valuep, int* endiannessp,
    unsigned int* idp, size_t* nparamsp, unsigned int* params) = NULL;

static int dvar_udf_inq_dim(int ncid, int dimid, char* name, size_t* lenp) {
  if (g_dvar_udf_fault == kDvarFaultFailInqDim) {
    return NC_EBADDIM;
  }
  return g_dvar_orig_inq_dim(ncid, dimid, name, lenp);
}

static int dvar_udf_inq_unlimdim(int ncid, int* unlimdimidp) {
  if (g_dvar_udf_fault == kDvarFaultFailUnlimdim) {
    return NC_EBADDIM;
  }
  return g_dvar_orig_inq_unlimdim(ncid, unlimdimidp);
}

static int dvar_udf_inq_unlimdims(int ncid, int* nunlimdimsp,
                                  int* unlimdimidsp) {
  if (g_dvar_udf_fault == kDvarFaultFailUnlimdimsCount &&
      unlimdimidsp == NULL) {
    return NC_EBADDIM;
  }
  if (g_dvar_udf_fault == kDvarFaultZeroUnlimdims) {
    if (nunlimdimsp) *nunlimdimsp = 0;
    return NC_NOERR;
  }
  if (g_dvar_udf_fault == kDvarFaultFailUnlimdimsIds &&
      unlimdimidsp != NULL) {
    return NC_EBADDIM;
  }
  return g_dvar_orig_inq_unlimdims(ncid, nunlimdimsp, unlimdimidsp);
}

static int dvar_udf_inq_var_all(
    int ncid, int varid, char* name, nc_type* xtypep, int* ndimsp,
    int* dimidsp, int* nattsp, int* shufflep, int* deflatep,
    int* deflate_levelp, int* fletcher32p, int* contiguousp,
    size_t* chunksizesp, int* no_fill, void* fill_valuep, int* endiannessp,
    unsigned int* idp, size_t* nparamsp, unsigned int* params) {
  if (g_dvar_udf_fault == kDvarFaultFailVardimid && dimidsp != NULL) {
    return NC_ENOTVAR;
  }
  return g_dvar_orig_inq_var_all(
      ncid, varid, name, xtypep, ndimsp, dimidsp, nattsp, shufflep, deflatep,
      deflate_levelp, fletcher32p, contiguousp, chunksizesp, no_fill,
      fill_valuep, endiannessp, idp, nparamsp, params);
}

int netcdf_dvar_register_udf(void) {
  int stat = NC_NOERR;
  g_dvar_udf_fault = kDvarFaultNone;
  if (!NC_initialized) {
    stat = nc_initialize();
    if (stat != NC_NOERR) return stat;
  }
  if (!NC3_dispatch_table) {
    return NC_ENOTBUILT;
  }
  g_dvar_udf_dispatch = *NC3_dispatch_table;
  g_dvar_udf_dispatch.model = NC_FORMATX_UDF8;

  g_dvar_orig_inq_dim = NC3_dispatch_table->inq_dim;
  g_dvar_orig_inq_unlimdim = NC3_dispatch_table->inq_unlimdim;
  g_dvar_orig_inq_unlimdims = NC3_dispatch_table->inq_unlimdims;
  g_dvar_orig_inq_var_all = NC3_dispatch_table->inq_var_all;

  g_dvar_udf_dispatch.inq_dim = dvar_udf_inq_dim;
  g_dvar_udf_dispatch.inq_unlimdim = dvar_udf_inq_unlimdim;
  g_dvar_udf_dispatch.inq_unlimdims = dvar_udf_inq_unlimdims;
  g_dvar_udf_dispatch.inq_var_all = dvar_udf_inq_var_all;

  return nc_def_user_format(NC_UDF8, &g_dvar_udf_dispatch, NULL);
}

void netcdf_dvar_set_udf_fault(int fault_mode) {
  g_dvar_udf_fault = fault_mode;
}
