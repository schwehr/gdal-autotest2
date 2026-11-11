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

#include "third_party/netcdf/google/libhdf5/nc4memcb_shim.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "third_party/netcdf/config.h"
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/nc4internal.h"
#include "third_party/netcdf/include/hdf5internal.h"

/* Declared in third_party/hdf5/src/H5VLtest.c and compiled into //third_party/hdf5. */
extern herr_t H5VL__reparse_def_vol_conn_variable_test(void);
/* Defined in libhdf5/nc4memcb.c, declared forward in hdf5file.c. */
extern int NC4_extract_file_image(NC_FILE_INFO_T* h5, int abort);

static hid_t g_capture_vol_id = H5I_INVALID_HID;
static int g_has_captured_cb = 0;
static int g_fail_next_create = 0;
static H5FD_file_image_callbacks_t g_captured_cb;

static H5FD_file_image_op_t OpFromInt(int op) {
  switch (op) {
    case kNc4MemcbOpNoOp:
      return H5FD_FILE_IMAGE_OP_NO_OP;
    case kNc4MemcbOpPropertyListSet:
      return H5FD_FILE_IMAGE_OP_PROPERTY_LIST_SET;
    case kNc4MemcbOpPropertyListCopy:
      return H5FD_FILE_IMAGE_OP_PROPERTY_LIST_COPY;
    case kNc4MemcbOpPropertyListGet:
      return H5FD_FILE_IMAGE_OP_PROPERTY_LIST_GET;
    case kNc4MemcbOpPropertyListClose:
      return H5FD_FILE_IMAGE_OP_PROPERTY_LIST_CLOSE;
    case kNc4MemcbOpFileOpen:
      return H5FD_FILE_IMAGE_OP_FILE_OPEN;
    case kNc4MemcbOpFileResize:
      return H5FD_FILE_IMAGE_OP_FILE_RESIZE;
    case kNc4MemcbOpFileClose:
      return H5FD_FILE_IMAGE_OP_FILE_CLOSE;
    default:
      return H5FD_FILE_IMAGE_OP_NO_OP;
  }
}

static void MaybeCaptureCallbacks(hid_t fapl_id) {
  H5FD_file_image_callbacks_t cb;
  memset(&cb, 0, sizeof(cb));
  if (H5Pget_file_image_callbacks(fapl_id, &cb) >= 0) {
    if (cb.udata != NULL && cb.udata_free != NULL) {
      cb.udata_free(cb.udata);
      cb.udata = NULL;
    }
    if (cb.image_malloc != NULL) {
      g_captured_cb = cb;
      g_has_captured_cb = 1;
    }
  }
}

/* Native-delegating VOL callbacks for capturing NC4_image_init's FAPL. */

static void* CapAttrCreate(void* obj, const H5VL_loc_params_t* loc_params,
                           const char* attr_name, hid_t type_id,
                           hid_t space_id, hid_t acpl_id, hid_t aapl_id,
                           hid_t dxpl_id, void** req) {
  return H5VLattr_create(obj, loc_params, H5VL_NATIVE, attr_name, type_id,
                         space_id, acpl_id, aapl_id, dxpl_id, req);
}

static void* CapAttrOpen(void* obj, const H5VL_loc_params_t* loc_params,
                         const char* attr_name, hid_t aapl_id, hid_t dxpl_id,
                         void** req) {
  return H5VLattr_open(obj, loc_params, H5VL_NATIVE, attr_name, aapl_id,
                       dxpl_id, req);
}

static herr_t CapAttrRead(void* attr, hid_t mem_type_id, void* buf,
                          hid_t dxpl_id, void** req) {
  return H5VLattr_read(attr, H5VL_NATIVE, mem_type_id, buf, dxpl_id, req);
}

static herr_t CapAttrWrite(void* attr, hid_t mem_type_id, const void* buf,
                           hid_t dxpl_id, void** req) {
  return H5VLattr_write(attr, H5VL_NATIVE, mem_type_id, buf, dxpl_id, req);
}

static herr_t CapAttrGet(void* attr, H5VL_attr_get_args_t* args, hid_t dxpl_id,
                         void** req) {
  return H5VLattr_get(attr, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t CapAttrSpecific(void* obj, const H5VL_loc_params_t* loc_params,
                              H5VL_attr_specific_args_t* args, hid_t dxpl_id,
                              void** req) {
  return H5VLattr_specific(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t CapAttrOptional(void* attr, H5VL_optional_args_t* args,
                              hid_t dxpl_id, void** req) {
  return H5VLattr_optional(attr, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t CapAttrClose(void* attr, hid_t dxpl_id, void** req) {
  return H5VLattr_close(attr, H5VL_NATIVE, dxpl_id, req);
}

static void* CapDatasetCreate(void* obj, const H5VL_loc_params_t* loc_params,
                              const char* name, hid_t lcpl_id, hid_t type_id,
                              hid_t space_id, hid_t dcpl_id, hid_t dapl_id,
                              hid_t dxpl_id, void** req) {
  return H5VLdataset_create(obj, loc_params, H5VL_NATIVE, name, lcpl_id,
                            type_id, space_id, dcpl_id, dapl_id, dxpl_id, req);
}

static void* CapDatasetOpen(void* obj, const H5VL_loc_params_t* loc_params,
                            const char* name, hid_t dapl_id, hid_t dxpl_id,
                            void** req) {
  return H5VLdataset_open(obj, loc_params, H5VL_NATIVE, name, dapl_id, dxpl_id,
                          req);
}

static herr_t CapDatasetRead(size_t count, void* dset[], hid_t mem_type_id[],
                             hid_t mem_space_id[], hid_t file_space_id[],
                             hid_t dxpl_id, void* buf[], void** req) {
  return H5VLdataset_read(count, dset, H5VL_NATIVE, mem_type_id, mem_space_id,
                          file_space_id, dxpl_id, buf, req);
}

static herr_t CapDatasetWrite(size_t count, void* dset[], hid_t mem_type_id[],
                              hid_t mem_space_id[], hid_t file_space_id[],
                              hid_t dxpl_id, const void* buf[], void** req) {
  return H5VLdataset_write(count, dset, H5VL_NATIVE, mem_type_id, mem_space_id,
                           file_space_id, dxpl_id, buf, req);
}

static herr_t CapDatasetGet(void* dset, H5VL_dataset_get_args_t* args,
                            hid_t dxpl_id, void** req) {
  return H5VLdataset_get(dset, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t CapDatasetSpecific(void* obj, H5VL_dataset_specific_args_t* args,
                                 hid_t dxpl_id, void** req) {
  return H5VLdataset_specific(obj, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t CapDatasetOptional(void* obj, H5VL_optional_args_t* args,
                                 hid_t dxpl_id, void** req) {
  return H5VLdataset_optional(obj, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t CapDatasetClose(void* dset, hid_t dxpl_id, void** req) {
  return H5VLdataset_close(dset, H5VL_NATIVE, dxpl_id, req);
}

static void* CapDatatypeCommit(void* obj, const H5VL_loc_params_t* loc_params,
                               const char* name, hid_t type_id, hid_t lcpl_id,
                               hid_t tcpl_id, hid_t tapl_id, hid_t dxpl_id,
                               void** req) {
  return H5VLdatatype_commit(obj, loc_params, H5VL_NATIVE, name, type_id,
                             lcpl_id, tcpl_id, tapl_id, dxpl_id, req);
}

static void* CapDatatypeOpen(void* obj, const H5VL_loc_params_t* loc_params,
                             const char* name, hid_t tapl_id, hid_t dxpl_id,
                             void** req) {
  return H5VLdatatype_open(obj, loc_params, H5VL_NATIVE, name, tapl_id, dxpl_id,
                           req);
}

static herr_t CapDatatypeGet(void* dt, H5VL_datatype_get_args_t* args,
                             hid_t dxpl_id, void** req) {
  return H5VLdatatype_get(dt, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t CapDatatypeSpecific(void* obj, H5VL_datatype_specific_args_t* args,
                                  hid_t dxpl_id, void** req) {
  return H5VLdatatype_specific(obj, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t CapDatatypeOptional(void* obj, H5VL_optional_args_t* args,
                                  hid_t dxpl_id, void** req) {
  return H5VLdatatype_optional(obj, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t CapDatatypeClose(void* dt, hid_t dxpl_id, void** req) {
  return H5VLdatatype_close(dt, H5VL_NATIVE, dxpl_id, req);
}

static void* CapFileCreate(const char* name, unsigned flags, hid_t fcpl_id,
                           hid_t fapl_id, hid_t dxpl_id, void** req) {
  hid_t under_fapl = H5I_INVALID_HID;
  void* file = NULL;
  MaybeCaptureCallbacks(fapl_id);
  if (g_fail_next_create) {
    g_fail_next_create = 0;
    return NULL;
  }
  under_fapl = H5Pcopy(fapl_id);
  if (under_fapl < 0) {
    return NULL;
  }
  if (H5Pset_vol(under_fapl, H5VL_NATIVE, NULL) >= 0) {
    file = H5VLfile_create(name, flags, fcpl_id, under_fapl, dxpl_id, req);
  }
  H5Pclose(under_fapl);
  return file;
}

static void* CapFileOpen(const char* name, unsigned flags, hid_t fapl_id,
                         hid_t dxpl_id, void** req) {
  hid_t under_fapl = H5I_INVALID_HID;
  void* file = NULL;
  MaybeCaptureCallbacks(fapl_id);
  under_fapl = H5Pcopy(fapl_id);
  if (under_fapl < 0) {
    return NULL;
  }
  if (H5Pset_vol(under_fapl, H5VL_NATIVE, NULL) >= 0) {
    file = H5VLfile_open(name, flags, under_fapl, dxpl_id, req);
  }
  H5Pclose(under_fapl);
  return file;
}

static herr_t CapFileGet(void* file, H5VL_file_get_args_t* args, hid_t dxpl_id,
                         void** req) {
  return H5VLfile_get(file, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t CapFileSpecific(void* obj, H5VL_file_specific_args_t* args,
                              hid_t dxpl_id, void** req) {
  return H5VLfile_specific(obj, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t CapFileOptional(void* file, H5VL_optional_args_t* args,
                              hid_t dxpl_id, void** req) {
  return H5VLfile_optional(file, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t CapFileClose(void* file, hid_t dxpl_id, void** req) {
  return H5VLfile_close(file, H5VL_NATIVE, dxpl_id, req);
}

static void* CapGroupCreate(void* obj, const H5VL_loc_params_t* loc_params,
                            const char* name, hid_t lcpl_id, hid_t gcpl_id,
                            hid_t gapl_id, hid_t dxpl_id, void** req) {
  return H5VLgroup_create(obj, loc_params, H5VL_NATIVE, name, lcpl_id, gcpl_id,
                          gapl_id, dxpl_id, req);
}

static void* CapGroupOpen(void* obj, const H5VL_loc_params_t* loc_params,
                          const char* name, hid_t gapl_id, hid_t dxpl_id,
                          void** req) {
  return H5VLgroup_open(obj, loc_params, H5VL_NATIVE, name, gapl_id, dxpl_id,
                        req);
}

static herr_t CapGroupGet(void* obj, H5VL_group_get_args_t* args, hid_t dxpl_id,
                          void** req) {
  return H5VLgroup_get(obj, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t CapGroupSpecific(void* obj, H5VL_group_specific_args_t* args,
                               hid_t dxpl_id, void** req) {
  return H5VLgroup_specific(obj, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t CapGroupOptional(void* obj, H5VL_optional_args_t* args,
                               hid_t dxpl_id, void** req) {
  return H5VLgroup_optional(obj, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t CapGroupClose(void* grp, hid_t dxpl_id, void** req) {
  return H5VLgroup_close(grp, H5VL_NATIVE, dxpl_id, req);
}

static herr_t CapLinkCreate(H5VL_link_create_args_t* args, void* obj,
                            const H5VL_loc_params_t* loc_params, hid_t lcpl_id,
                            hid_t lapl_id, hid_t dxpl_id, void** req) {
  return H5VLlink_create(args, obj, loc_params, H5VL_NATIVE, lcpl_id, lapl_id,
                         dxpl_id, req);
}

static herr_t CapLinkCopy(void* src_obj, const H5VL_loc_params_t* loc_params1,
                          void* dst_obj, const H5VL_loc_params_t* loc_params2,
                          hid_t lcpl_id, hid_t lapl_id, hid_t dxpl_id,
                          void** req) {
  return H5VLlink_copy(src_obj, loc_params1, dst_obj, loc_params2, H5VL_NATIVE,
                       lcpl_id, lapl_id, dxpl_id, req);
}

static herr_t CapLinkMove(void* src_obj, const H5VL_loc_params_t* loc_params1,
                          void* dst_obj, const H5VL_loc_params_t* loc_params2,
                          hid_t lcpl_id, hid_t lapl_id, hid_t dxpl_id,
                          void** req) {
  return H5VLlink_move(src_obj, loc_params1, dst_obj, loc_params2, H5VL_NATIVE,
                       lcpl_id, lapl_id, dxpl_id, req);
}

static herr_t CapLinkGet(void* obj, const H5VL_loc_params_t* loc_params,
                         H5VL_link_get_args_t* args, hid_t dxpl_id,
                         void** req) {
  return H5VLlink_get(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t CapLinkSpecific(void* obj, const H5VL_loc_params_t* loc_params,
                              H5VL_link_specific_args_t* args, hid_t dxpl_id,
                              void** req) {
  return H5VLlink_specific(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t CapLinkOptional(void* obj, const H5VL_loc_params_t* loc_params,
                              H5VL_optional_args_t* args, hid_t dxpl_id,
                              void** req) {
  return H5VLlink_optional(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

static void* CapObjectOpen(void* obj, const H5VL_loc_params_t* loc_params,
                           H5I_type_t* opened_type, hid_t dxpl_id, void** req) {
  return H5VLobject_open(obj, loc_params, H5VL_NATIVE, opened_type, dxpl_id,
                         req);
}

static herr_t CapObjectCopy(void* src_obj, const H5VL_loc_params_t* loc_params1,
                            const char* src_name, void* dst_obj,
                            const H5VL_loc_params_t* loc_params2,
                            const char* dst_name, hid_t ocpypl_id,
                            hid_t lcpl_id, hid_t dxpl_id, void** req) {
  return H5VLobject_copy(src_obj, loc_params1, src_name, dst_obj, loc_params2,
                         dst_name, H5VL_NATIVE, ocpypl_id, lcpl_id, dxpl_id,
                         req);
}

static herr_t CapObjectGet(void* obj, const H5VL_loc_params_t* loc_params,
                           H5VL_object_get_args_t* args, hid_t dxpl_id,
                           void** req) {
  return H5VLobject_get(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t CapObjectSpecific(void* obj, const H5VL_loc_params_t* loc_params,
                                H5VL_object_specific_args_t* args,
                                hid_t dxpl_id, void** req) {
  return H5VLobject_specific(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t CapObjectOptional(void* obj, const H5VL_loc_params_t* loc_params,
                                H5VL_optional_args_t* args, hid_t dxpl_id,
                                void** req) {
  return H5VLobject_optional(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t CapIntrospectGetConnCls(void* obj, H5VL_get_conn_lvl_t lvl,
                                      const H5VL_class_t** conn_cls) {
  return H5VLintrospect_get_conn_cls(obj, H5VL_NATIVE, lvl, conn_cls);
}

static herr_t CapIntrospectGetCapFlags(const void* info, uint64_t* cap_flags) {
  (void)info;
  return H5VLintrospect_get_cap_flags(NULL, H5VL_NATIVE, cap_flags);
}

static herr_t CapIntrospectOptQuery(void* obj, H5VL_subclass_t cls, int opt_type,
                                    uint64_t* flags) {
  return H5VLintrospect_opt_query(obj, H5VL_NATIVE, cls, opt_type, flags);
}

static herr_t CapBlobPut(void* obj, const void* buf, size_t size, void* blob_id,
                         void* ctx) {
  return H5VLblob_put(obj, H5VL_NATIVE, buf, size, blob_id, ctx);
}

static herr_t CapBlobGet(void* obj, const void* blob_id, void* buf, size_t size,
                         void* ctx) {
  return H5VLblob_get(obj, H5VL_NATIVE, blob_id, buf, size, ctx);
}

static herr_t CapBlobSpecific(void* obj, void* blob_id,
                              H5VL_blob_specific_args_t* args) {
  return H5VLblob_specific(obj, H5VL_NATIVE, blob_id, args);
}

static herr_t CapTokenCmp(void* obj, const H5O_token_t* token1,
                          const H5O_token_t* token2, int* cmp_value) {
  return H5VLtoken_cmp(obj, H5VL_NATIVE, token1, token2, cmp_value);
}

static herr_t CapTokenToStr(void* obj, H5I_type_t obj_type,
                            const H5O_token_t* token, char** token_str) {
  return H5VLtoken_to_str(obj, obj_type, H5VL_NATIVE, token, token_str);
}

static herr_t CapTokenFromStr(void* obj, H5I_type_t obj_type,
                              const char* token_str, H5O_token_t* token) {
  return H5VLtoken_from_str(obj, obj_type, H5VL_NATIVE, token_str, token);
}

static const H5VL_class_t kCaptureVolClass = {
    .version = H5VL_VERSION,
    .value = (H5VL_class_value_t)51099,
    .name = "nc4memcb_capture_vol",
    .conn_version = 1,
    .cap_flags = 0,
    .initialize = NULL,
    .terminate = NULL,
    .info_cls = {0},
    .wrap_cls = {0},
    .attr_cls =
        {
            .create = CapAttrCreate,
            .open = CapAttrOpen,
            .read = CapAttrRead,
            .write = CapAttrWrite,
            .get = CapAttrGet,
            .specific = CapAttrSpecific,
            .optional = CapAttrOptional,
            .close = CapAttrClose,
        },
    .dataset_cls =
        {
            .create = CapDatasetCreate,
            .open = CapDatasetOpen,
            .read = CapDatasetRead,
            .write = CapDatasetWrite,
            .get = CapDatasetGet,
            .specific = CapDatasetSpecific,
            .optional = CapDatasetOptional,
            .close = CapDatasetClose,
        },
    .datatype_cls =
        {
            .commit = CapDatatypeCommit,
            .open = CapDatatypeOpen,
            .get = CapDatatypeGet,
            .specific = CapDatatypeSpecific,
            .optional = CapDatatypeOptional,
            .close = CapDatatypeClose,
        },
    .file_cls =
        {
            .create = CapFileCreate,
            .open = CapFileOpen,
            .get = CapFileGet,
            .specific = CapFileSpecific,
            .optional = CapFileOptional,
            .close = CapFileClose,
        },
    .group_cls =
        {
            .create = CapGroupCreate,
            .open = CapGroupOpen,
            .get = CapGroupGet,
            .specific = CapGroupSpecific,
            .optional = CapGroupOptional,
            .close = CapGroupClose,
        },
    .link_cls =
        {
            .create = CapLinkCreate,
            .copy = CapLinkCopy,
            .move = CapLinkMove,
            .get = CapLinkGet,
            .specific = CapLinkSpecific,
            .optional = CapLinkOptional,
        },
    .object_cls =
        {
            .open = CapObjectOpen,
            .copy = CapObjectCopy,
            .get = CapObjectGet,
            .specific = CapObjectSpecific,
            .optional = CapObjectOptional,
        },
    .introspect_cls =
        {
            .get_conn_cls = CapIntrospectGetConnCls,
            .get_cap_flags = CapIntrospectGetCapFlags,
            .opt_query = CapIntrospectOptQuery,
        },
    .request_cls = {0},
    .blob_cls =
        {
            .put = CapBlobPut,
            .get = CapBlobGet,
            .specific = CapBlobSpecific,
            .optional = NULL,
        },
    .token_cls =
        {
            .cmp = CapTokenCmp,
            .to_str = CapTokenToStr,
            .from_str = CapTokenFromStr,
        },
    .optional = NULL,
};

void Nc4MemcbShimEnsureHdf5Initialized(void) {
  if (!nc4_hdf5_initialized) {
    nc4_hdf5_initialize();
  }
}

int Nc4MemcbShimRegisterCaptureVol(void) {
  if (g_capture_vol_id >= 0) {
    return 0;
  }
  if (H5open() < 0) {
    return -1;
  }
  g_capture_vol_id = H5VLregister_connector(&kCaptureVolClass, H5P_DEFAULT);
  return (g_capture_vol_id >= 0) ? 0 : -1;
}

int Nc4MemcbShimUnregisterCaptureVol(void) {
  herr_t stat = 0;
  if (g_capture_vol_id >= 0) {
    stat = H5VLunregister_connector(g_capture_vol_id);
    g_capture_vol_id = H5I_INVALID_HID;
  }
  return (stat >= 0) ? 0 : -1;
}

int Nc4MemcbShimReparseDefaultVol(void) {
  return (H5VL__reparse_def_vol_conn_variable_test() >= 0) ? 0 : -1;
}

void Nc4MemcbShimSetFailNextCreate(int fail_next) {
  g_fail_next_create = fail_next;
}

void Nc4MemcbShimResetCapturedCallbacks(void) {
  g_has_captured_cb = 0;
  g_fail_next_create = 0;
  memset(&g_captured_cb, 0, sizeof(g_captured_cb));
}

int Nc4MemcbShimHasCapturedCallbacks(void) {
  return g_has_captured_cb;
}

void* Nc4MemcbShimCallMalloc(size_t size, int op, nc4memcb_udata_mirror_t* ud) {
  if (!g_has_captured_cb || g_captured_cb.image_malloc == NULL) {
    return NULL;
  }
  return g_captured_cb.image_malloc(size, OpFromInt(op), ud);
}

void* Nc4MemcbShimCallMemcpy(void* dest, const void* src, size_t size, int op,
                             nc4memcb_udata_mirror_t* ud) {
  if (!g_has_captured_cb || g_captured_cb.image_memcpy == NULL) {
    return NULL;
  }
  return g_captured_cb.image_memcpy(dest, src, size, OpFromInt(op), ud);
}

void* Nc4MemcbShimCallRealloc(void* ptr, size_t size, int op,
                              nc4memcb_udata_mirror_t* ud) {
  if (!g_has_captured_cb || g_captured_cb.image_realloc == NULL) {
    return NULL;
  }
  return g_captured_cb.image_realloc(ptr, size, OpFromInt(op), ud);
}

int Nc4MemcbShimCallFree(void* ptr, int op, nc4memcb_udata_mirror_t* ud) {
  if (!g_has_captured_cb || g_captured_cb.image_free == NULL) {
    return -1;
  }
  return (int)g_captured_cb.image_free(ptr, OpFromInt(op), ud);
}

void* Nc4MemcbShimCallUdataCopy(nc4memcb_udata_mirror_t* ud) {
  if (!g_has_captured_cb || g_captured_cb.udata_copy == NULL) {
    return NULL;
  }
  return g_captured_cb.udata_copy(ud);
}

int Nc4MemcbShimCallUdataFree(nc4memcb_udata_mirror_t* ud) {
  if (!g_has_captured_cb || g_captured_cb.udata_free == NULL) {
    return -1;
  }
  return (int)g_captured_cb.udata_free(ud);
}

int Nc4MemcbShimImageInitMissingInput(void) {
  NC_FILE_INFO_T h5;
  memset(&h5, 0, sizeof(h5));
  h5.mem.created = 0;
  h5.mem.memio.memory = NULL;
  h5.mem.memio.size = 64;
  return (NC4_image_init(&h5) < 0) ? -1 : 0;
}

int Nc4MemcbShimImageInitZeroSize(void) {
  unsigned char dummy[8] = {0};
  NC_FILE_INFO_T h5;
  memset(&h5, 0, sizeof(h5));
  h5.mem.created = 0;
  h5.mem.memio.memory = dummy;
  h5.mem.memio.size = 0;
  return (NC4_image_init(&h5) < 0) ? -1 : 0;
}

int Nc4MemcbShimImageInitCreateFail(void) {
  unsigned char dummy[64] = {0};
  NC_FILE_INFO_T h5;
  H5E_auto2_t prev_func = NULL;
  void* prev_data = NULL;
  hid_t fid = H5I_INVALID_HID;

  H5Eget_auto2(H5E_DEFAULT, &prev_func, &prev_data);
  H5Eset_auto2(H5E_DEFAULT, NULL, NULL);

  memset(&h5, 0, sizeof(h5));
  h5.mem.created = 1;
  h5.mem.imageflags = H5LT_FILE_IMAGE_OPEN_RW;
  h5.mem.memio.memory = dummy;
  h5.mem.memio.size = sizeof(dummy);

  g_fail_next_create = 1;
  fid = NC4_image_init(&h5);
  g_fail_next_create = 0;
  NC4_image_finalize(h5.mem.udata);

  H5Eset_auto2(H5E_DEFAULT, prev_func, prev_data);
  return (fid < 0) ? -1 : 0;
}

void Nc4MemcbShimFinalizeNull(void) {
  NC4_image_finalize(NULL);
}

int Nc4MemcbShimExtractAbortNullUdata(void) {
  NC_FILE_INFO_T h5;
  memset(&h5, 0, sizeof(h5));
  h5.mem.udata = NULL;
  return NC4_extract_file_image(&h5, /*abort=*/1);
}
