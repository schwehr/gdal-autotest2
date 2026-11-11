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

#include "third_party/netcdf/google/libhdf5/hdf5create_shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "third_party/netcdf/config.h"  // GOOGLE MODIFICATION
#include "third_party/netcdf/include/hdf5internal.h"
#include "third_party/netcdf/include/nc.h"
#include "third_party/netcdf/include/nc4dispatch.h"
#include "third_party/netcdf/include/ncdispatch.h"
#include "third_party/netcdf/include/ncglobal.h"
#include "third_party/netcdf/include/ncpathmgr.h"
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"

extern herr_t H5VL__reparse_def_vol_conn_variable_test(void);

static herr_t AlwaysFailPropCreate(const char* name, size_t size, void* value) {
  (void)name;
  (void)size;
  (void)value;
  return -1;
}

typedef struct FailRootVolInfo {
  hid_t under_vol_id;
  void* under_vol_info;
} FailRootVolInfo;

typedef struct FailRootVolObj {
  void* under_object;
  hid_t under_vol_id;
} FailRootVolObj;

typedef struct FailRootVolWrapCtx {
  hid_t under_vol_id;
  void* under_wrap_ctx;
} FailRootVolWrapCtx;

static int g_fail_next_root_open = 0;
static int g_root_open_failed = 0;

static const H5VL_class_t* GetFailRootVolClassPtr(void);

static FailRootVolObj* FailRootVolNewObj(void* under_obj, hid_t under_vol_id) {
  FailRootVolObj* obj = NULL;
  if (under_obj == NULL) {
    return NULL;
  }
  obj = (FailRootVolObj*)calloc(1, sizeof(FailRootVolObj));
  if (obj == NULL) {
    return NULL;
  }
  obj->under_object = under_obj;
  obj->under_vol_id = under_vol_id;
  if (H5Iinc_ref(obj->under_vol_id) < 0) {
    free(obj);
    return NULL;
  }
  return obj;
}

static herr_t FailRootVolFreeObj(FailRootVolObj* obj) {
  hid_t err_id = H5Eget_current_stack();
  if (obj != NULL) {
    H5Idec_ref(obj->under_vol_id);
    free(obj);
  }
  H5Eset_current_stack(err_id);
  return 0;
}

static herr_t FailRootVolInfoFree(void* info_ptr) {
  FailRootVolInfo* info = (FailRootVolInfo*)info_ptr;
  hid_t err_id;
  if (info == NULL) {
    return 0;
  }
  err_id = H5Eget_current_stack();
  if (info->under_vol_info != NULL) {
    H5VLfree_connector_info(info->under_vol_id, info->under_vol_info);
  }
  H5Idec_ref(info->under_vol_id);
  H5Eset_current_stack(err_id);
  free(info);
  return 0;
}

static void* FailRootVolInfoCopy(const void* info_ptr) {
  const FailRootVolInfo* info = (const FailRootVolInfo*)info_ptr;
  FailRootVolInfo* new_info = NULL;
  if (info == NULL || H5Iis_valid(info->under_vol_id) <= 0) {
    return NULL;
  }
  new_info = (FailRootVolInfo*)calloc(1, sizeof(FailRootVolInfo));
  if (new_info == NULL) {
    return NULL;
  }
  new_info->under_vol_id = info->under_vol_id;
  if (H5Iinc_ref(new_info->under_vol_id) < 0) {
    free(new_info);
    return NULL;
  }
  if (info->under_vol_info != NULL) {
    if (H5VLcopy_connector_info(new_info->under_vol_id,
                                &new_info->under_vol_info,
                                info->under_vol_info) < 0) {
      H5Idec_ref(new_info->under_vol_id);
      free(new_info);
      return NULL;
    }
  }
  return new_info;
}

static herr_t FailRootVolInfoCmp(int* cmp_value, const void* info1_ptr,
                                 const void* info2_ptr) {
  const FailRootVolInfo* info1 = (const FailRootVolInfo*)info1_ptr;
  const FailRootVolInfo* info2 = (const FailRootVolInfo*)info2_ptr;
  if (cmp_value == NULL || info1 == NULL || info2 == NULL) {
    return -1;
  }
  *cmp_value = 0;
  if (H5VLcmp_connector_cls(cmp_value, info1->under_vol_id,
                            info2->under_vol_id) < 0) {
    return -1;
  }
  if (*cmp_value != 0) {
    return 0;
  }
  return H5VLcmp_connector_info(cmp_value, info1->under_vol_id,
                                info1->under_vol_info, info2->under_vol_info);
}

static herr_t FailRootVolInfoToStr(const void* info_ptr, char** str) {
  const FailRootVolInfo* info = (const FailRootVolInfo*)info_ptr;
  H5VL_class_value_t under_value = (H5VL_class_value_t)0;
  char* under_vol_string = NULL;
  size_t under_len = 0;
  size_t str_size = 0;
  if (info == NULL || str == NULL) {
    return -1;
  }
  if (H5VLget_value(info->under_vol_id, &under_value) < 0) {
    return -1;
  }
  if (H5VLconnector_info_to_str(info->under_vol_info, info->under_vol_id,
                                &under_vol_string) < 0) {
    return -1;
  }
  if (under_vol_string != NULL) {
    under_len = strlen(under_vol_string);
  }
  str_size = 48 + under_len;
  *str = (char*)H5allocate_memory(str_size, 0);
  if (*str != NULL) {
    snprintf(*str, str_size, "under_vol=%u;under_info={%s}",
             (unsigned)under_value,
             under_vol_string != NULL ? under_vol_string : "");
  }
  if (under_vol_string != NULL) {
    H5free_memory(under_vol_string);
  }
  return *str != NULL ? 0 : -1;
}

static herr_t FailRootVolStrToInfo(const char* str, void** info_out) {
  unsigned long under_vol_val = 0;
  hid_t under_vol_id = H5I_INVALID_HID;
  void* under_vol_info = NULL;
  FailRootVolInfo* info = NULL;
  const char* open_brace = NULL;
  const char* close_brace = NULL;
  if (info_out == NULL) {
    return -1;
  }
  if (str != NULL) {
    const char* key = strstr(str, "under_vol=");
    if (key != NULL) {
      under_vol_val = strtoul(key + 10, NULL, 10);
    }
    open_brace = strchr(str, '{');
    close_brace = strrchr(str, '}');
  }
  under_vol_id = H5VLregister_connector_by_value(
      (H5VL_class_value_t)under_vol_val, H5P_DEFAULT);
  if (under_vol_id < 0) {
    return -1;
  }
  if (open_brace != NULL && close_brace != NULL &&
      close_brace > open_brace + 1) {
    size_t sub_len = (size_t)(close_brace - (open_brace + 1));
    char* sub = (char*)malloc(sub_len + 1);
    if (sub == NULL) {
      H5Idec_ref(under_vol_id);
      return -1;
    }
    memcpy(sub, open_brace + 1, sub_len);
    sub[sub_len] = '\0';
    if (H5VLconnector_str_to_info(sub, under_vol_id, &under_vol_info) < 0) {
      free(sub);
      H5Idec_ref(under_vol_id);
      return -1;
    }
    free(sub);
  }
  info = (FailRootVolInfo*)calloc(1, sizeof(FailRootVolInfo));
  if (info == NULL) {
    if (under_vol_info != NULL) {
      H5VLfree_connector_info(under_vol_id, under_vol_info);
    }
    H5Idec_ref(under_vol_id);
    return -1;
  }
  info->under_vol_id = under_vol_id;
  info->under_vol_info = under_vol_info;
  *info_out = info;
  return 0;
}

static void* FailRootVolGetObject(const void* obj) {
  const FailRootVolObj* o = (const FailRootVolObj*)obj;
  return H5VLget_object(o->under_object, o->under_vol_id);
}

static herr_t FailRootVolGetWrapCtx(const void* obj, void** wrap_ctx) {
  const FailRootVolObj* o = (const FailRootVolObj*)obj;
  FailRootVolWrapCtx* ctx =
      (FailRootVolWrapCtx*)calloc(1, sizeof(FailRootVolWrapCtx));
  if (ctx == NULL) {
    return -1;
  }
  ctx->under_vol_id = o->under_vol_id;
  if (H5Iinc_ref(ctx->under_vol_id) < 0) {
    free(ctx);
    return -1;
  }
  if (H5VLget_wrap_ctx(o->under_object, o->under_vol_id,
                       &ctx->under_wrap_ctx) < 0) {
    H5Idec_ref(ctx->under_vol_id);
    free(ctx);
    return -1;
  }
  *wrap_ctx = ctx;
  return 0;
}

static void* FailRootVolWrapObject(void* obj, H5I_type_t obj_type,
                                   void* wrap_ctx) {
  FailRootVolWrapCtx* ctx = (FailRootVolWrapCtx*)wrap_ctx;
  void* under =
      H5VLwrap_object(obj, obj_type, ctx->under_vol_id, ctx->under_wrap_ctx);
  if (under != NULL) {
    return FailRootVolNewObj(under, ctx->under_vol_id);
  }
  return NULL;
}

static void* FailRootVolUnwrapObject(void* obj) {
  FailRootVolObj* o = (FailRootVolObj*)obj;
  void* under = H5VLunwrap_object(o->under_object, o->under_vol_id);
  if (under != NULL) {
    FailRootVolFreeObj(o);
  }
  return under;
}

static herr_t FailRootVolFreeWrapCtx(void* wrap_ctx) {
  FailRootVolWrapCtx* ctx = (FailRootVolWrapCtx*)wrap_ctx;
  hid_t err_id = H5Eget_current_stack();
  if (ctx->under_wrap_ctx != NULL) {
    H5VLfree_wrap_ctx(ctx->under_wrap_ctx, ctx->under_vol_id);
  }
  H5Idec_ref(ctx->under_vol_id);
  H5Eset_current_stack(err_id);
  free(ctx);
  return 0;
}

static void* FailRootVolFileCreate(const char* name, unsigned flags,
                                   hid_t fcpl_id, hid_t fapl_id, hid_t dxpl_id,
                                   void** req) {
  FailRootVolInfo* info = NULL;
  FailRootVolObj* file = NULL;
  hid_t under_fapl_id = H5I_INVALID_HID;
  void* under = NULL;
  if (H5Pget_vol_info(fapl_id, (void**)&info) < 0 || info == NULL) {
    return NULL;
  }
  under_fapl_id = H5Pcopy(fapl_id);
  if (under_fapl_id < 0) {
    FailRootVolInfoFree(info);
    return NULL;
  }
  if (H5Pset_vol(under_fapl_id, info->under_vol_id, info->under_vol_info) >=
      0) {
    under = H5VLfile_create(name, flags, fcpl_id, under_fapl_id, dxpl_id, req);
    if (under != NULL) {
      file = FailRootVolNewObj(under, info->under_vol_id);
      if (file == NULL) {
        (void)H5VLfile_close(under, info->under_vol_id, dxpl_id, NULL);
      } else if (req != NULL && *req != NULL) {
        *req = FailRootVolNewObj(*req, info->under_vol_id);
      }
    }
  }
  H5Pclose(under_fapl_id);
  FailRootVolInfoFree(info);
  return file;
}

static void* FailRootVolFileOpen(const char* name, unsigned flags,
                                 hid_t fapl_id, hid_t dxpl_id, void** req) {
  FailRootVolInfo* info = NULL;
  FailRootVolObj* file = NULL;
  hid_t under_fapl_id = H5I_INVALID_HID;
  void* under = NULL;
  if (H5Pget_vol_info(fapl_id, (void**)&info) < 0 || info == NULL) {
    return NULL;
  }
  under_fapl_id = H5Pcopy(fapl_id);
  if (under_fapl_id < 0) {
    FailRootVolInfoFree(info);
    return NULL;
  }
  if (H5Pset_vol(under_fapl_id, info->under_vol_id, info->under_vol_info) >=
      0) {
    under = H5VLfile_open(name, flags, under_fapl_id, dxpl_id, req);
    if (under != NULL) {
      file = FailRootVolNewObj(under, info->under_vol_id);
      if (file == NULL) {
        (void)H5VLfile_close(under, info->under_vol_id, dxpl_id, NULL);
      } else if (req != NULL && *req != NULL) {
        *req = FailRootVolNewObj(*req, info->under_vol_id);
      }
    }
  }
  H5Pclose(under_fapl_id);
  FailRootVolInfoFree(info);
  return file;
}

static herr_t FailRootVolFileGet(void* file, H5VL_file_get_args_t* args,
                                 hid_t dxpl_id, void** req) {
  FailRootVolObj* o = (FailRootVolObj*)file;
  herr_t st = H5VLfile_get(o->under_object, o->under_vol_id, args, dxpl_id, req);
  if (req != NULL && *req != NULL) {
    *req = FailRootVolNewObj(*req, o->under_vol_id);
  }
  return st;
}

static herr_t FailRootVolFileSpecific(void* file,
                                      H5VL_file_specific_args_t* args,
                                      hid_t dxpl_id, void** req) {
  FailRootVolObj* o = (FailRootVolObj*)file;
  H5VL_file_specific_args_t my_args;
  H5VL_file_specific_args_t* new_args = args;
  void* new_o = NULL;
  FailRootVolInfo* info = NULL;
  hid_t under_vol_id = H5I_INVALID_HID;
  herr_t st;
  if (args->op_type == H5VL_FILE_IS_ACCESSIBLE) {
    memcpy(&my_args, args, sizeof(my_args));
    if (H5Pget_vol_info(args->args.is_accessible.fapl_id, (void**)&info) < 0 ||
        info == NULL) {
      return -1;
    }
    under_vol_id = info->under_vol_id;
    my_args.args.is_accessible.fapl_id =
        H5Pcopy(args->args.is_accessible.fapl_id);
    H5Pset_vol(my_args.args.is_accessible.fapl_id, info->under_vol_id,
               info->under_vol_info);
    new_args = &my_args;
  } else if (args->op_type == H5VL_FILE_DELETE) {
    memcpy(&my_args, args, sizeof(my_args));
    if (H5Pget_vol_info(args->args.del.fapl_id, (void**)&info) < 0 ||
        info == NULL) {
      return -1;
    }
    under_vol_id = info->under_vol_id;
    my_args.args.del.fapl_id = H5Pcopy(args->args.del.fapl_id);
    H5Pset_vol(my_args.args.del.fapl_id, info->under_vol_id,
               info->under_vol_info);
    new_args = &my_args;
  } else {
    under_vol_id = o->under_vol_id;
    new_o = o->under_object;
  }
  st = H5VLfile_specific(new_o, under_vol_id, new_args, dxpl_id, req);
  if (req != NULL && *req != NULL) {
    *req = FailRootVolNewObj(*req, under_vol_id);
  }
  if (args->op_type == H5VL_FILE_IS_ACCESSIBLE) {
    H5Pclose(my_args.args.is_accessible.fapl_id);
    FailRootVolInfoFree(info);
  } else if (args->op_type == H5VL_FILE_DELETE) {
    H5Pclose(my_args.args.del.fapl_id);
    FailRootVolInfoFree(info);
  } else if (args->op_type == H5VL_FILE_REOPEN) {
    if (st >= 0 && *args->args.reopen.file != NULL) {
      *args->args.reopen.file =
          FailRootVolNewObj(*args->args.reopen.file, under_vol_id);
    }
  }
  return st;
}

static herr_t FailRootVolFileOptional(void* file, H5VL_optional_args_t* args,
                                      hid_t dxpl_id, void** req) {
  FailRootVolObj* o = (FailRootVolObj*)file;
  herr_t st =
      H5VLfile_optional(o->under_object, o->under_vol_id, args, dxpl_id, req);
  if (req != NULL && *req != NULL) {
    *req = FailRootVolNewObj(*req, o->under_vol_id);
  }
  return st;
}

static herr_t FailRootVolFileClose(void* file, hid_t dxpl_id, void** req) {
  FailRootVolObj* o = (FailRootVolObj*)file;
  herr_t st = H5VLfile_close(o->under_object, o->under_vol_id, dxpl_id, req);
  if (req != NULL && *req != NULL) {
    *req = FailRootVolNewObj(*req, o->under_vol_id);
  }
  if (st >= 0) {
    FailRootVolFreeObj(o);
  }
  return st;
}

static void* FailRootVolGroupCreate(void* obj,
                                    const H5VL_loc_params_t* loc_params,
                                    const char* name, hid_t lcpl_id,
                                    hid_t gcpl_id, hid_t gapl_id, hid_t dxpl_id,
                                    void** req) {
  FailRootVolObj* o = (FailRootVolObj*)obj;
  void* under = H5VLgroup_create(o->under_object, loc_params, o->under_vol_id,
                                 name, lcpl_id, gcpl_id, gapl_id, dxpl_id, req);
  if (under != NULL) {
    if (req != NULL && *req != NULL) {
      *req = FailRootVolNewObj(*req, o->under_vol_id);
    }
    return FailRootVolNewObj(under, o->under_vol_id);
  }
  return NULL;
}

static void* FailRootVolGroupOpen(void* obj,
                                  const H5VL_loc_params_t* loc_params,
                                  const char* name, hid_t gapl_id,
                                  hid_t dxpl_id, void** req) {
  FailRootVolObj* o = (FailRootVolObj*)obj;
  void* under = NULL;
  if (g_fail_next_root_open && name != NULL && strcmp(name, "/") == 0) {
    g_fail_next_root_open = 0;
    g_root_open_failed = 1;
    return NULL;
  }
  under = H5VLgroup_open(o->under_object, loc_params, o->under_vol_id, name,
                         gapl_id, dxpl_id, req);
  if (under != NULL) {
    if (req != NULL && *req != NULL) {
      *req = FailRootVolNewObj(*req, o->under_vol_id);
    }
    return FailRootVolNewObj(under, o->under_vol_id);
  }
  return NULL;
}

static herr_t FailRootVolGroupGet(void* obj, H5VL_group_get_args_t* args,
                                  hid_t dxpl_id, void** req) {
  FailRootVolObj* o = (FailRootVolObj*)obj;
  herr_t st =
      H5VLgroup_get(o->under_object, o->under_vol_id, args, dxpl_id, req);
  if (req != NULL && *req != NULL) {
    *req = FailRootVolNewObj(*req, o->under_vol_id);
  }
  return st;
}

static herr_t FailRootVolGroupSpecific(void* obj,
                                       H5VL_group_specific_args_t* args,
                                       hid_t dxpl_id, void** req) {
  FailRootVolObj* o = (FailRootVolObj*)obj;
  herr_t st =
      H5VLgroup_specific(o->under_object, o->under_vol_id, args, dxpl_id, req);
  if (req != NULL && *req != NULL) {
    *req = FailRootVolNewObj(*req, o->under_vol_id);
  }
  return st;
}

static herr_t FailRootVolGroupOptional(void* obj, H5VL_optional_args_t* args,
                                       hid_t dxpl_id, void** req) {
  FailRootVolObj* o = (FailRootVolObj*)obj;
  herr_t st =
      H5VLgroup_optional(o->under_object, o->under_vol_id, args, dxpl_id, req);
  if (req != NULL && *req != NULL) {
    *req = FailRootVolNewObj(*req, o->under_vol_id);
  }
  return st;
}

static herr_t FailRootVolGroupClose(void* grp, hid_t dxpl_id, void** req) {
  FailRootVolObj* o = (FailRootVolObj*)grp;
  herr_t st = H5VLgroup_close(o->under_object, o->under_vol_id, dxpl_id, req);
  if (req != NULL && *req != NULL) {
    *req = FailRootVolNewObj(*req, o->under_vol_id);
  }
  if (st >= 0) {
    FailRootVolFreeObj(o);
  }
  return st;
}

static herr_t FailRootVolIntrospectGetConnCls(void* obj,
                                              H5VL_get_conn_lvl_t lvl,
                                              const H5VL_class_t** conn_cls) {
  const FailRootVolObj* o = (const FailRootVolObj*)obj;
  if (lvl == H5VL_GET_CONN_LVL_CURR) {
    *conn_cls = GetFailRootVolClassPtr();
    return 0;
  }
  return H5VLintrospect_get_conn_cls(o->under_object, o->under_vol_id, lvl,
                                     conn_cls);
}

static herr_t FailRootVolIntrospectGetCapFlags(const void* info_ptr,
                                               uint64_t* cap_flags) {
  const FailRootVolInfo* info = (const FailRootVolInfo*)info_ptr;
  herr_t st;
  if (info == NULL || H5Iis_valid(info->under_vol_id) <= 0) {
    return -1;
  }
  st = H5VLintrospect_get_cap_flags(info->under_vol_info, info->under_vol_id,
                                    cap_flags);
  if (st >= 0) {
    *cap_flags |= GetFailRootVolClassPtr()->cap_flags;
  }
  return st;
}

static herr_t FailRootVolIntrospectOptQuery(void* obj, H5VL_subclass_t subcls,
                                           int opt_type, uint64_t* flags) {
  const FailRootVolObj* o = (const FailRootVolObj*)obj;
  return H5VLintrospect_opt_query(o->under_object, o->under_vol_id, subcls,
                                  opt_type, flags);
}

static const H5VL_class_t kFailRootVolClass = {
    .version = H5VL_VERSION,
    .value = (H5VL_class_value_t)51101,
    .name = "hdf5create_fail_root_vol",
    .conn_version = 1,
    .cap_flags = H5VL_CAP_FLAG_NONE,
    .info_cls =
        {
            .size = sizeof(FailRootVolInfo),
            .copy = FailRootVolInfoCopy,
            .cmp = FailRootVolInfoCmp,
            .free = FailRootVolInfoFree,
            .to_str = FailRootVolInfoToStr,
            .from_str = FailRootVolStrToInfo,
        },
    .wrap_cls =
        {
            .get_object = FailRootVolGetObject,
            .get_wrap_ctx = FailRootVolGetWrapCtx,
            .wrap_object = FailRootVolWrapObject,
            .unwrap_object = FailRootVolUnwrapObject,
            .free_wrap_ctx = FailRootVolFreeWrapCtx,
        },
    .file_cls =
        {
            .create = FailRootVolFileCreate,
            .open = FailRootVolFileOpen,
            .get = FailRootVolFileGet,
            .specific = FailRootVolFileSpecific,
            .optional = FailRootVolFileOptional,
            .close = FailRootVolFileClose,
        },
    .group_cls =
        {
            .create = FailRootVolGroupCreate,
            .open = FailRootVolGroupOpen,
            .get = FailRootVolGroupGet,
            .specific = FailRootVolGroupSpecific,
            .optional = FailRootVolGroupOptional,
            .close = FailRootVolGroupClose,
        },
    .introspect_cls =
        {
            .get_conn_cls = FailRootVolIntrospectGetConnCls,
            .get_cap_flags = FailRootVolIntrospectGetCapFlags,
            .opt_query = FailRootVolIntrospectOptQuery,
        },
};

static const H5VL_class_t* GetFailRootVolClassPtr(void) {
  return &kFailRootVolClass;
}

int Hdf5CreateWithIllegalCmode(const char* path, int cmode) {
  int stat = nc_initialize();
  if (stat != NC_NOERR) {
    return stat;
  }
  return NC4_create(path, cmode, 0, 0, NULL, NULL, HDF5_dispatch_table, -1);
}

int Hdf5CreateBypassDispatch(const char* path, int cmode, size_t initialsz,
                             int pass_empty_memio) {
  NC* ncp = NULL;
  NC_memio empty_memio;
  void* params = NULL;
  int stat = nc_initialize();
  if (stat != NC_NOERR) {
    return stat;
  }

  empty_memio.size = 0;
  empty_memio.memory = NULL;
  empty_memio.flags = 0;
  if (pass_empty_memio) {
    params = &empty_memio;
  }

  stat = new_NC(HDF5_dispatch_table, path, cmode, &ncp);
  if (stat != NC_NOERR) {
    return stat;
  }
  add_to_NCList(ncp);

  stat = NC4_create(ncp->path, cmode, initialsz, 0, NULL, params,
                    HDF5_dispatch_table, ncp->ext_ncid);
  if (stat != NC_NOERR) {
    del_from_NCList(ncp);
    free_NC(ncp);
    return stat;
  }

  if ((cmode & NC_INMEMORY) == NC_INMEMORY) {
    NC_memio out_mem;
    out_mem.size = 0;
    out_mem.memory = NULL;
    out_mem.flags = 0;
    stat = nc_close_memio(ncp->ext_ncid, &out_mem);
    if (out_mem.memory != NULL) {
      free(out_mem.memory);
    }
    return stat;
  }
  return nc_close(ncp->ext_ncid);
}

int Hdf5CreateWithNonNullZeroSizeMemio(void) {
  NC* ncp = NULL;
  NC_memio memio;
  void* buf = NULL;
  H5E_auto2_t saved_efunc = NULL;
  void* saved_edata = NULL;
  int stat = nc_initialize();
  if (stat != NC_NOERR) {
    return stat;
  }

  buf = malloc(16);
  if (buf == NULL) {
    return NC_ENOMEM;
  }
  memset(buf, 0, 16);
  memio.size = 0;
  memio.memory = buf;
  memio.flags = 0;

  stat =
      new_NC(HDF5_dispatch_table, "nonnull_zero_memio.nc", NC_NETCDF4 | NC_INMEMORY, &ncp);
  if (stat != NC_NOERR) {
    free(buf);
    return stat;
  }
  add_to_NCList(ncp);

  (void)H5Eget_auto2(H5E_DEFAULT, &saved_efunc, &saved_edata);
  (void)H5Eset_auto2(H5E_DEFAULT, NULL, NULL);

  stat = NC4_create(ncp->path, NC_NETCDF4 | NC_INMEMORY, 0, 0, NULL, &memio,
                    HDF5_dispatch_table, ncp->ext_ncid);

  (void)H5Eset_auto2(H5E_DEFAULT, saved_efunc, saved_edata);

  if (stat == NC_NOERR) {
    NC_memio out_mem;
    out_mem.size = 0;
    out_mem.memory = NULL;
    out_mem.flags = 0;
    stat = nc_close_memio(ncp->ext_ncid, &out_mem);
    if (out_mem.memory != NULL) {
      free(out_mem.memory);
    }
    return stat;
  }

  del_from_NCList(ncp);
  free_NC(ncp);
  return stat;
}

int Hdf5CreateWithInvalidChunkCachePreemption(const char* path) {
  struct NCglobalstate* gs = NULL;
  float saved_preemption = 0.0f;
  H5E_auto2_t saved_efunc = NULL;
  void* saved_edata = NULL;
  int ncid = -1;
  int stat = nc_initialize();
  if (stat != NC_NOERR) {
    return stat;
  }

  gs = NC_getglobalstate();
  saved_preemption = gs->chunkcache.preemption;

  (void)H5Eget_auto2(H5E_DEFAULT, &saved_efunc, &saved_edata);
  (void)H5Eset_auto2(H5E_DEFAULT, NULL, NULL);

  gs->chunkcache.preemption = 2.0f;
  stat = nc_create(path, NC_NETCDF4 | NC_DISKLESS, &ncid);
  gs->chunkcache.preemption = saved_preemption;

  (void)H5Eset_auto2(H5E_DEFAULT, saved_efunc, saved_edata);

  if (stat == NC_NOERR && ncid >= 0) {
    (void)nc_close(ncid);
  }
  return stat;
}

int Hdf5CreateColdInit(const char* path) {
  NC* ncp = NULL;
  int stat = nc_initialize();
  if (stat != NC_NOERR) {
    return stat;
  }

  nc4_hdf5_finalize();

  stat = new_NC(HDF5_dispatch_table, path, NC_NETCDF4 | NC_DISKLESS, &ncp);
  if (stat != NC_NOERR) {
    return stat;
  }
  add_to_NCList(ncp);

  stat = NC4_create(ncp->path, NC_NETCDF4 | NC_DISKLESS, 0, 0, NULL, NULL,
                    HDF5_dispatch_table, ncp->ext_ncid);
  if (stat != NC_NOERR) {
    del_from_NCList(ncp);
    free_NC(ncp);
    return stat;
  }
  return nc_close(ncp->ext_ncid);
}

int Hdf5CreateWithUnconvertiblePath(const char* path) {
  int stat = nc_initialize();
  if (stat != NC_NOERR) {
    return stat;
  }

  NCpathsetplatform(NCPD_WIN);
  stat = Hdf5CreateBypassDispatch(path, NC_NETCDF4 | NC_DISKLESS, 0, 0);
  NCpathforceplatform();
  return stat;
}

int Hdf5CreateFailPlistProperty(enum Hdf5CreateFailPropKind kind) {
  H5E_auto2_t saved_efunc = NULL;
  void* saved_edata = NULL;
  int def_val = 0;
  int stat = nc_initialize();
  if (stat != NC_NOERR) {
    return stat;
  }

  (void)H5Eget_auto2(H5E_DEFAULT, &saved_efunc, &saved_edata);
  (void)H5Eset_auto2(H5E_DEFAULT, NULL, NULL);

  switch (kind) {
    case kHdf5CreateFailUnregisterCloseDegree:
      if (H5Punregister(H5P_FILE_ACCESS, "close_degree") < 0) {
        stat = NC_EHDFERR;
      } else {
        stat = Hdf5CreateBypassDispatch("fail_close_degree.nc",
                                        NC_NETCDF4 | NC_DISKLESS, 0, 0);
      }
      break;
    case kHdf5CreateFailUnregisterMetaBlockSize: {
      struct NCglobalstate* gs = NC_getglobalstate();
      unsigned char saved_meta_block[sizeof(gs->meta_block_size)];
      memcpy(saved_meta_block, &gs->meta_block_size,
             sizeof(gs->meta_block_size));
      stat = nc_set_meta_block_size(4096);
      if (stat == NC_NOERR) {
        if (H5Punregister(H5P_FILE_ACCESS, "meta_block_size") < 0) {
          stat = NC_EHDFERR;
        } else {
          stat = Hdf5CreateBypassDispatch("fail_meta_block.nc",
                                          NC_NETCDF4 | NC_DISKLESS, 0, 0);
        }
      }
      memcpy(&gs->meta_block_size, saved_meta_block,
             sizeof(gs->meta_block_size));
      break;
    }
    case kHdf5CreateFailUnregisterLibverLowBound:
      if (H5Punregister(H5P_FILE_ACCESS, "libver_low_bound") < 0) {
        stat = NC_EHDFERR;
      } else {
        stat = Hdf5CreateBypassDispatch("fail_libver.nc",
                                        NC_NETCDF4 | NC_DISKLESS, 0, 0);
      }
      break;
    case kHdf5CreateFailUnregisterObjTrackTimes:
      if (H5Punregister(H5P_OBJECT_CREATE, "object header flags") < 0) {
        stat = NC_EHDFERR;
      } else {
        stat = Hdf5CreateBypassDispatch("fail_obj_track.nc",
                                        NC_NETCDF4 | NC_DISKLESS, 0, 0);
      }
      break;
    case kHdf5CreateFailUnregisterLinkInfo:
      if (H5Punregister(H5P_GROUP_CREATE, "link info") < 0) {
        stat = NC_EHDFERR;
      } else {
        stat = Hdf5CreateBypassDispatch("fail_link_info.nc",
                                        NC_NETCDF4 | NC_DISKLESS, 0, 0);
      }
      break;
    case kHdf5CreateFailUnregisterFaplCore:
      if (H5Punregister(H5P_FILE_ACCESS, "vfd_info") < 0) {
        stat = NC_EHDFERR;
      } else {
        stat = Hdf5CreateBypassDispatch("fail_fapl_core.nc",
                                        NC_NETCDF4 | NC_DISKLESS, 0, 0);
      }
      break;
    case kHdf5CreateFailCreateFcpl:
      if (H5Pregister2(H5P_FILE_CREATE, "hdf5create_test_fail_fcpl",
                       sizeof(int), &def_val, AlwaysFailPropCreate, NULL, NULL,
                       NULL, NULL, NULL, NULL) < 0) {
        stat = NC_EHDFERR;
      } else {
        stat = Hdf5CreateBypassDispatch("fail_create_fcpl.nc",
                                        NC_NETCDF4 | NC_DISKLESS, 0, 0);
        (void)H5Punregister(H5P_FILE_CREATE, "hdf5create_test_fail_fcpl");
      }
      break;
    case kHdf5CreateFailCreateFapl:
      if (H5Pregister2(H5P_FILE_ACCESS, "hdf5create_test_fail_fapl",
                       sizeof(int), &def_val, AlwaysFailPropCreate, NULL, NULL,
                       NULL, NULL, NULL, NULL) < 0) {
        stat = NC_EHDFERR;
      } else {
        stat = Hdf5CreateBypassDispatch("fail_create_fapl.nc",
                                        NC_NETCDF4 | NC_DISKLESS, 0, 0);
        (void)H5Punregister(H5P_FILE_ACCESS, "hdf5create_test_fail_fapl");
      }
      break;
    default:
      stat = NC_EINVAL;
      break;
  }

  (void)H5Eset_auto2(H5E_DEFAULT, saved_efunc, saved_edata);
  return stat;
}

long long Hdf5CreateRegisterFailRootVol(void) {
  if (nc_initialize() != NC_NOERR) {
    return -1;
  }
  return (long long)H5VLregister_connector(&kFailRootVolClass, H5P_DEFAULT);
}

int Hdf5CreateReparseDefaultVol(void) {
  return H5VL__reparse_def_vol_conn_variable_test() < 0 ? NC_EHDFERR : NC_NOERR;
}

int Hdf5CreateWithRootGroupOpenFailure(const char* path) {
  NC* ncp = NULL;
  H5E_auto2_t saved_efunc = NULL;
  void* saved_edata = NULL;
  int stat = nc_initialize();
  if (stat != NC_NOERR) {
    return stat;
  }

  stat = new_NC(HDF5_dispatch_table, path, NC_NETCDF4 | NC_DISKLESS, &ncp);
  if (stat != NC_NOERR) {
    return stat;
  }
  add_to_NCList(ncp);

  (void)H5Eget_auto2(H5E_DEFAULT, &saved_efunc, &saved_edata);
  (void)H5Eset_auto2(H5E_DEFAULT, NULL, NULL);

  g_fail_next_root_open = 1;
  g_root_open_failed = 0;
  stat = NC4_create(ncp->path, NC_NETCDF4 | NC_DISKLESS, 0, 0, NULL, NULL,
                    HDF5_dispatch_table, ncp->ext_ncid);
  g_fail_next_root_open = 0;

  (void)H5Eset_auto2(H5E_DEFAULT, saved_efunc, saved_edata);

  if (stat == NC_NOERR) {
    g_root_open_failed = 0;
    return nc_close(ncp->ext_ncid);
  }

  if (g_root_open_failed && ncp->dispatchdata != NULL) {
    NC_FILE_INFO_T* h5 = (NC_FILE_INFO_T*)ncp->dispatchdata;
    if (h5->root_grp != NULL && h5->root_grp->format_grp_info != NULL) {
      NC_HDF5_GRP_INFO_T* hdf5_grp =
          (NC_HDF5_GRP_INFO_T*)h5->root_grp->format_grp_info;
      if (hdf5_grp->hdf_grpid < 0) {
        hdf5_grp->hdf_grpid = 0;
      }
    }
    (void)nc4_close_hdf5_file(h5, 1, NULL);
    ncp->dispatchdata = NULL;
  }
  g_root_open_failed = 0;

  del_from_NCList(ncp);
  free_NC(ncp);
  return stat;
}

int Hdf5CreateCleanupFailRootVol(long long vol_id) {
  int stat =
      H5VL__reparse_def_vol_conn_variable_test() < 0 ? NC_EHDFERR : NC_NOERR;
  if (vol_id >= 0 && H5VLunregister_connector((hid_t)vol_id) < 0 &&
      stat == NC_NOERR) {
    stat = NC_EHDFERR;
  }
  return stat;
}
