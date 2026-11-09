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
//
// Tests public file management APIs in third_party/hdf5/src/H5F.c, including
// file creation, opening, reopening, flushing, closing (sync and async),
// mounting/unmounting, object enumeration, metadata cache configuration and
// logging, SWMR mode, page buffering stats, EOA adjustments, and VOL failure
// propagation.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5F.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define H5F_TEST_HAS_ASAN 1
#else
#define H5F_TEST_HAS_ASAN 0
#endif
#elif defined(__SANITIZE_ADDRESS__)
#define H5F_TEST_HAS_ASAN 1
#else
#define H5F_TEST_HAS_ASAN 0
#endif

// Fault-injection and async-token configuration for the local passthrough
// VOL connector used to exercise VOL error paths and H5ES paths in H5F.c.
struct TestVolConfig {
  bool fail_file_create = false;
  bool fail_file_open = false;
  bool fail_file_get = false;
  bool fail_file_specific = false;
  bool fail_file_is_accessible = false;
  bool fail_file_delete = false;
  bool fail_file_reopen_null = false;
  bool fail_file_optional = false;
  bool fail_file_close = false;
  bool fail_group_open = false;
  bool fail_group_specific = false;
  bool fail_group_close = false;
  bool fail_opt_query = false;
  bool fail_post_open = false;
  bool return_req_on_file_create = false;
  bool return_req_on_file_open = false;
  bool return_req_on_file_reopen = false;
  bool return_req_on_post_open = false;
  bool return_req_on_file_flush = false;
  bool return_req_on_file_close = false;

  void Reset() { *this = TestVolConfig{}; }
};

TestVolConfig g_vol_cfg;

struct TestVolInfo {
  hid_t under_vol_id = H5I_INVALID_HID;
  void* under_vol_info = nullptr;
};

struct TestVolObj {
  hid_t under_vol_id = H5I_INVALID_HID;
  void* under_obj = nullptr;
};

struct TestVolWrapCtx {
  hid_t under_vol_id = H5I_INVALID_HID;
  void* under_wrap_ctx = nullptr;
};

struct TestVolReq {
  hid_t under_vol_id = H5I_INVALID_HID;
  void* under_req = nullptr;
  H5VL_request_status_t status = H5VL_REQUEST_STATUS_SUCCEED;
};

TestVolObj* WrapUnderObj(void* under_obj, hid_t under_vol_id) {
  if (under_obj == nullptr) {
    return nullptr;
  }
  H5Iinc_ref(under_vol_id);
  return new TestVolObj{under_vol_id, under_obj};
}

void FreeVolObj(TestVolObj* obj) {
  if (obj == nullptr) {
    return;
  }
  const hid_t err_id = H5Eget_current_stack();
  if (obj->under_vol_id != H5I_INVALID_HID) {
    H5Idec_ref(obj->under_vol_id);
  }
  H5Eset_current_stack(err_id);
  delete obj;
}

TestVolReq* MakeSyntheticReq(hid_t under_vol_id, void* under_req = nullptr) {
  H5Iinc_ref(under_vol_id);
  return new TestVolReq{under_vol_id, under_req, H5VL_REQUEST_STATUS_SUCCEED};
}

herr_t VolInit(hid_t /*vipl_id*/) { return 0; }
herr_t VolTerm() { return 0; }

void* VolInfoCopy(const void* raw_info) {
  const auto* src = static_cast<const TestVolInfo*>(raw_info);
  if (src == nullptr || H5Iis_valid(src->under_vol_id) <= 0) {
    return nullptr;
  }
  auto* dst = new TestVolInfo{src->under_vol_id, nullptr};
  H5Iinc_ref(dst->under_vol_id);
  if (src->under_vol_info != nullptr) {
    H5VLcopy_connector_info(dst->under_vol_id, &dst->under_vol_info,
                            src->under_vol_info);
  }
  return dst;
}

herr_t VolInfoCmp(int* cmp_value, const void* raw1, const void* raw2) {
  const auto* info1 = static_cast<const TestVolInfo*>(raw1);
  const auto* info2 = static_cast<const TestVolInfo*>(raw2);
  *cmp_value = 0;
  H5VLcmp_connector_cls(cmp_value, info1->under_vol_id, info2->under_vol_id);
  if (*cmp_value != 0) {
    return 0;
  }
  H5VLcmp_connector_info(cmp_value, info1->under_vol_id, info1->under_vol_info,
                         info2->under_vol_info);
  return 0;
}

herr_t VolInfoFree(void* raw_info) {
  auto* info = static_cast<TestVolInfo*>(raw_info);
  if (info == nullptr) {
    return 0;
  }
  const hid_t err_id = H5Eget_current_stack();
  if (info->under_vol_info != nullptr) {
    H5VLfree_connector_info(info->under_vol_id, info->under_vol_info);
  }
  if (info->under_vol_id != H5I_INVALID_HID) {
    H5Idec_ref(info->under_vol_id);
  }
  H5Eset_current_stack(err_id);
  delete info;
  return 0;
}

herr_t VolInfoToStr(const void* /*info*/, char** str) {
  *str = nullptr;
  return 0;
}

herr_t VolInfoFromStr(const char* /*str*/, void** info) {
  *info = nullptr;
  return 0;
}

void* VolWrapGetObject(const void* raw_obj) {
  const auto* o = static_cast<const TestVolObj*>(raw_obj);
  return H5VLget_object(o->under_obj, o->under_vol_id);
}

herr_t VolWrapGetCtx(const void* raw_obj, void** wrap_ctx) {
  const auto* o = static_cast<const TestVolObj*>(raw_obj);
  auto* ctx = new TestVolWrapCtx{o->under_vol_id, nullptr};
  H5Iinc_ref(ctx->under_vol_id);
  H5VLget_wrap_ctx(o->under_obj, o->under_vol_id, &ctx->under_wrap_ctx);
  *wrap_ctx = ctx;
  return 0;
}

void* VolWrapObject(void* obj, H5I_type_t obj_type, void* raw_ctx) {
  auto* ctx = static_cast<TestVolWrapCtx*>(raw_ctx);
  void* under = H5VLwrap_object(obj, obj_type, ctx->under_vol_id,
                                ctx->under_wrap_ctx);
  return WrapUnderObj(under, ctx->under_vol_id);
}

void* VolUnwrapObject(void* raw_obj) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  void* under = H5VLunwrap_object(o->under_obj, o->under_vol_id);
  if (under != nullptr) {
    FreeVolObj(o);
  }
  return under;
}

herr_t VolFreeWrapCtx(void* raw_ctx) {
  auto* ctx = static_cast<TestVolWrapCtx*>(raw_ctx);
  if (ctx == nullptr) {
    return 0;
  }
  const hid_t err_id = H5Eget_current_stack();
  if (ctx->under_wrap_ctx != nullptr) {
    H5VLfree_wrap_ctx(ctx->under_wrap_ctx, ctx->under_vol_id);
  }
  H5Idec_ref(ctx->under_vol_id);
  H5Eset_current_stack(err_id);
  delete ctx;
  return 0;
}

void* VolAttrCreate(void* raw_obj, const H5VL_loc_params_t* loc_params,
                    const char* name, hid_t type_id, hid_t space_id,
                    hid_t acpl_id, hid_t aapl_id, hid_t dxpl_id, void** req) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  void* under = H5VLattr_create(o->under_obj, loc_params, o->under_vol_id, name,
                                type_id, space_id, acpl_id, aapl_id, dxpl_id,
                                req);
  return WrapUnderObj(under, o->under_vol_id);
}

void* VolAttrOpen(void* raw_obj, const H5VL_loc_params_t* loc_params,
                  const char* name, hid_t aapl_id, hid_t dxpl_id, void** req) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  void* under = H5VLattr_open(o->under_obj, loc_params, o->under_vol_id, name,
                              aapl_id, dxpl_id, req);
  return WrapUnderObj(under, o->under_vol_id);
}

herr_t VolAttrRead(void* raw_attr, hid_t mem_type_id, void* buf, hid_t dxpl_id,
                   void** req) {
  auto* o = static_cast<TestVolObj*>(raw_attr);
  return H5VLattr_read(o->under_obj, o->under_vol_id, mem_type_id, buf, dxpl_id,
                       req);
}

herr_t VolAttrWrite(void* raw_attr, hid_t mem_type_id, const void* buf,
                    hid_t dxpl_id, void** req) {
  auto* o = static_cast<TestVolObj*>(raw_attr);
  return H5VLattr_write(o->under_obj, o->under_vol_id, mem_type_id, buf,
                        dxpl_id, req);
}

herr_t VolAttrGet(void* raw_obj, H5VL_attr_get_args_t* args, hid_t dxpl_id,
                  void** req) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  return H5VLattr_get(o->under_obj, o->under_vol_id, args, dxpl_id, req);
}

herr_t VolAttrSpecific(void* raw_obj, const H5VL_loc_params_t* loc_params,
                       H5VL_attr_specific_args_t* args, hid_t dxpl_id,
                       void** req) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  return H5VLattr_specific(o->under_obj, loc_params, o->under_vol_id, args,
                           dxpl_id, req);
}

herr_t VolAttrOptional(void* raw_obj, H5VL_optional_args_t* args, hid_t dxpl_id,
                       void** req) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  return H5VLattr_optional(o->under_obj, o->under_vol_id, args, dxpl_id, req);
}

herr_t VolAttrClose(void* raw_attr, hid_t dxpl_id, void** req) {
  auto* o = static_cast<TestVolObj*>(raw_attr);
  herr_t rc = H5VLattr_close(o->under_obj, o->under_vol_id, dxpl_id, req);
  if (rc >= 0) {
    FreeVolObj(o);
  }
  return rc;
}

void* VolDatasetCreate(void* raw_obj, const H5VL_loc_params_t* loc_params,
                       const char* name, hid_t lcpl_id, hid_t type_id,
                       hid_t space_id, hid_t dcpl_id, hid_t dapl_id,
                       hid_t dxpl_id, void** req) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  void* under = H5VLdataset_create(o->under_obj, loc_params, o->under_vol_id,
                                   name, lcpl_id, type_id, space_id, dcpl_id,
                                   dapl_id, dxpl_id, req);
  return WrapUnderObj(under, o->under_vol_id);
}

void* VolDatasetOpen(void* raw_obj, const H5VL_loc_params_t* loc_params,
                     const char* name, hid_t dapl_id, hid_t dxpl_id,
                     void** req) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  void* under = H5VLdataset_open(o->under_obj, loc_params, o->under_vol_id,
                                 name, dapl_id, dxpl_id, req);
  return WrapUnderObj(under, o->under_vol_id);
}

herr_t VolDatasetRead(size_t count, void* dset[], hid_t mem_type_id[],
                      hid_t mem_space_id[], hid_t file_space_id[],
                      hid_t dxpl_id, void* buf[], void** req) {
  std::vector<void*> under(count);
  hid_t under_vol_id = H5I_INVALID_HID;
  for (size_t i = 0; i < count; ++i) {
    auto* o = static_cast<TestVolObj*>(dset[i]);
    under[i] = o->under_obj;
    under_vol_id = o->under_vol_id;
  }
  return H5VLdataset_read(count, under.data(), under_vol_id, mem_type_id,
                          mem_space_id, file_space_id, dxpl_id, buf, req);
}

herr_t VolDatasetWrite(size_t count, void* dset[], hid_t mem_type_id[],
                       hid_t mem_space_id[], hid_t file_space_id[],
                       hid_t dxpl_id, const void* buf[], void** req) {
  std::vector<void*> under(count);
  hid_t under_vol_id = H5I_INVALID_HID;
  for (size_t i = 0; i < count; ++i) {
    auto* o = static_cast<TestVolObj*>(dset[i]);
    under[i] = o->under_obj;
    under_vol_id = o->under_vol_id;
  }
  return H5VLdataset_write(count, under.data(), under_vol_id, mem_type_id,
                           mem_space_id, file_space_id, dxpl_id, buf, req);
}

herr_t VolDatasetGet(void* raw_dset, H5VL_dataset_get_args_t* args,
                     hid_t dxpl_id, void** req) {
  auto* o = static_cast<TestVolObj*>(raw_dset);
  return H5VLdataset_get(o->under_obj, o->under_vol_id, args, dxpl_id, req);
}

herr_t VolDatasetSpecific(void* raw_dset, H5VL_dataset_specific_args_t* args,
                          hid_t dxpl_id, void** req) {
  auto* o = static_cast<TestVolObj*>(raw_dset);
  return H5VLdataset_specific(o->under_obj, o->under_vol_id, args, dxpl_id,
                              req);
}

herr_t VolDatasetOptional(void* raw_dset, H5VL_optional_args_t* args,
                          hid_t dxpl_id, void** req) {
  auto* o = static_cast<TestVolObj*>(raw_dset);
  return H5VLdataset_optional(o->under_obj, o->under_vol_id, args, dxpl_id,
                              req);
}

herr_t VolDatasetClose(void* raw_dset, hid_t dxpl_id, void** req) {
  auto* o = static_cast<TestVolObj*>(raw_dset);
  herr_t rc = H5VLdataset_close(o->under_obj, o->under_vol_id, dxpl_id, req);
  if (rc >= 0) {
    FreeVolObj(o);
  }
  return rc;
}

void* VolDatatypeCommit(void* raw_obj, const H5VL_loc_params_t* loc_params,
                        const char* name, hid_t type_id, hid_t lcpl_id,
                        hid_t tcpl_id, hid_t tapl_id, hid_t dxpl_id,
                        void** req) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  void* under = H5VLdatatype_commit(o->under_obj, loc_params, o->under_vol_id,
                                    name, type_id, lcpl_id, tcpl_id, tapl_id,
                                    dxpl_id, req);
  return WrapUnderObj(under, o->under_vol_id);
}

void* VolDatatypeOpen(void* raw_obj, const H5VL_loc_params_t* loc_params,
                      const char* name, hid_t tapl_id, hid_t dxpl_id,
                      void** req) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  void* under = H5VLdatatype_open(o->under_obj, loc_params, o->under_vol_id,
                                  name, tapl_id, dxpl_id, req);
  return WrapUnderObj(under, o->under_vol_id);
}

herr_t VolDatatypeGet(void* raw_dt, H5VL_datatype_get_args_t* args,
                      hid_t dxpl_id, void** req) {
  auto* o = static_cast<TestVolObj*>(raw_dt);
  return H5VLdatatype_get(o->under_obj, o->under_vol_id, args, dxpl_id, req);
}

herr_t VolDatatypeSpecific(void* raw_dt, H5VL_datatype_specific_args_t* args,
                           hid_t dxpl_id, void** req) {
  auto* o = static_cast<TestVolObj*>(raw_dt);
  return H5VLdatatype_specific(o->under_obj, o->under_vol_id, args, dxpl_id,
                               req);
}

herr_t VolDatatypeOptional(void* raw_dt, H5VL_optional_args_t* args,
                           hid_t dxpl_id, void** req) {
  auto* o = static_cast<TestVolObj*>(raw_dt);
  return H5VLdatatype_optional(o->under_obj, o->under_vol_id, args, dxpl_id,
                               req);
}

herr_t VolDatatypeClose(void* raw_dt, hid_t dxpl_id, void** req) {
  auto* o = static_cast<TestVolObj*>(raw_dt);
  herr_t rc = H5VLdatatype_close(o->under_obj, o->under_vol_id, dxpl_id, req);
  if (rc >= 0) {
    FreeVolObj(o);
  }
  return rc;
}

void* VolFileCreate(const char* name, unsigned flags, hid_t fcpl_id,
                    hid_t fapl_id, hid_t dxpl_id, void** req) {
  if (g_vol_cfg.fail_file_create) {
    return nullptr;
  }
  TestVolInfo* info = nullptr;
  if (H5Pget_vol_info(fapl_id, reinterpret_cast<void**>(&info)) < 0 ||
      info == nullptr) {
    return nullptr;
  }
  auto info_cleanup = absl::MakeCleanup([&]() { VolInfoFree(info); });

  ScopedPlist under_fapl(H5Pcopy(fapl_id));
  if (!under_fapl.valid()) {
    return nullptr;
  }
  if (H5Pset_vol(under_fapl, info->under_vol_id, info->under_vol_info) < 0) {
    return nullptr;
  }

  void* under = H5VLfile_create(name, flags, fcpl_id, under_fapl, dxpl_id, req);
  if (under == nullptr) {
    return nullptr;
  }
  TestVolObj* wrapped = WrapUnderObj(under, info->under_vol_id);
  if (wrapped != nullptr && req != nullptr &&
      g_vol_cfg.return_req_on_file_create) {
    *req = MakeSyntheticReq(info->under_vol_id);
  }
  return wrapped;
}

void* VolFileOpen(const char* name, unsigned flags, hid_t fapl_id,
                  hid_t dxpl_id, void** req) {
  if (g_vol_cfg.fail_file_open) {
    return nullptr;
  }
  TestVolInfo* info = nullptr;
  if (H5Pget_vol_info(fapl_id, reinterpret_cast<void**>(&info)) < 0 ||
      info == nullptr) {
    return nullptr;
  }
  auto info_cleanup = absl::MakeCleanup([&]() { VolInfoFree(info); });

  ScopedPlist under_fapl(H5Pcopy(fapl_id));
  if (!under_fapl.valid()) {
    return nullptr;
  }
  if (H5Pset_vol(under_fapl, info->under_vol_id, info->under_vol_info) < 0) {
    return nullptr;
  }

  void* under = H5VLfile_open(name, flags, under_fapl, dxpl_id, req);
  if (under == nullptr) {
    return nullptr;
  }
  TestVolObj* wrapped = WrapUnderObj(under, info->under_vol_id);
  if (wrapped != nullptr && req != nullptr &&
      g_vol_cfg.return_req_on_file_open) {
    *req = MakeSyntheticReq(info->under_vol_id);
  }
  return wrapped;
}

herr_t VolFileGet(void* raw_file, H5VL_file_get_args_t* args, hid_t dxpl_id,
                  void** req) {
  if (g_vol_cfg.fail_file_get) {
    return -1;
  }
  auto* o = static_cast<TestVolObj*>(raw_file);
  return H5VLfile_get(o->under_obj, o->under_vol_id, args, dxpl_id, req);
}

herr_t VolFileSpecific(void* raw_file, H5VL_file_specific_args_t* args,
                       hid_t dxpl_id, void** req) {
  if (args->op_type == H5VL_FILE_IS_ACCESSIBLE ||
      args->op_type == H5VL_FILE_DELETE) {
    const hid_t orig_fapl = args->op_type == H5VL_FILE_IS_ACCESSIBLE
                                ? args->args.is_accessible.fapl_id
                                : args->args.del.fapl_id;
    TestVolInfo* info = nullptr;
    if (H5Pget_vol_info(orig_fapl, reinterpret_cast<void**>(&info)) < 0 ||
        info == nullptr) {
      return -1;
    }
    auto info_cleanup = absl::MakeCleanup([&]() { VolInfoFree(info); });

    ScopedPlist under_fapl(H5Pcopy(orig_fapl));
    if (!under_fapl.valid() ||
        H5Pset_vol(under_fapl, info->under_vol_id, info->under_vol_info) < 0) {
      return -1;
    }

    if (args->op_type == H5VL_FILE_IS_ACCESSIBLE &&
        g_vol_cfg.fail_file_is_accessible) {
      return -1;
    }
    if (args->op_type == H5VL_FILE_DELETE && g_vol_cfg.fail_file_delete) {
      return -1;
    }

    H5VL_file_specific_args_t my_args = *args;
    if (args->op_type == H5VL_FILE_IS_ACCESSIBLE) {
      my_args.args.is_accessible.fapl_id = under_fapl;
    } else {
      my_args.args.del.fapl_id = under_fapl;
    }
    return H5VLfile_specific(nullptr, info->under_vol_id, &my_args, dxpl_id,
                             req);
  }

  if (g_vol_cfg.fail_file_specific) {
    return -1;
  }
  auto* o = static_cast<TestVolObj*>(raw_file);
  if (args->op_type == H5VL_FILE_REOPEN && g_vol_cfg.fail_file_reopen_null) {
    *args->args.reopen.file = nullptr;
    return 0;
  }

  herr_t rc =
      H5VLfile_specific(o->under_obj, o->under_vol_id, args, dxpl_id, req);
  if (rc >= 0 && args->op_type == H5VL_FILE_REOPEN &&
      *args->args.reopen.file != nullptr) {
    *args->args.reopen.file =
        WrapUnderObj(*args->args.reopen.file, o->under_vol_id);
    if (req != nullptr && g_vol_cfg.return_req_on_file_reopen) {
      *req = MakeSyntheticReq(o->under_vol_id);
    }
  } else if (rc >= 0 && args->op_type == H5VL_FILE_FLUSH && req != nullptr &&
             g_vol_cfg.return_req_on_file_flush) {
    *req = MakeSyntheticReq(o->under_vol_id);
  }
  return rc;
}

herr_t VolFileOptional(void* raw_file, H5VL_optional_args_t* args,
                       hid_t dxpl_id, void** req) {
  auto* o = static_cast<TestVolObj*>(raw_file);
  if (args->op_type == H5VL_NATIVE_FILE_POST_OPEN) {
    if (g_vol_cfg.fail_post_open) {
      return -1;
    }
    herr_t rc =
        H5VLfile_optional(o->under_obj, o->under_vol_id, args, dxpl_id, req);
    if (rc >= 0 && req != nullptr && g_vol_cfg.return_req_on_post_open) {
      *req = MakeSyntheticReq(o->under_vol_id);
    }
    return rc;
  }
  if (g_vol_cfg.fail_file_optional) {
    return -1;
  }
  return H5VLfile_optional(o->under_obj, o->under_vol_id, args, dxpl_id, req);
}

herr_t VolFileClose(void* raw_file, hid_t dxpl_id, void** req) {
  if (g_vol_cfg.fail_file_close) {
    return -1;
  }
  auto* o = static_cast<TestVolObj*>(raw_file);
  const hid_t under_vol_id = o->under_vol_id;
  herr_t rc = H5VLfile_close(o->under_obj, under_vol_id, dxpl_id, req);
  if (rc >= 0) {
    if (req != nullptr && g_vol_cfg.return_req_on_file_close) {
      *req = MakeSyntheticReq(under_vol_id);
    }
    FreeVolObj(o);
  }
  return rc;
}

void* VolGroupCreate(void* raw_obj, const H5VL_loc_params_t* loc_params,
                     const char* name, hid_t lcpl_id, hid_t gcpl_id,
                     hid_t gapl_id, hid_t dxpl_id, void** req) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  void* under = H5VLgroup_create(o->under_obj, loc_params, o->under_vol_id,
                                 name, lcpl_id, gcpl_id, gapl_id, dxpl_id, req);
  return WrapUnderObj(under, o->under_vol_id);
}

void* VolGroupOpen(void* raw_obj, const H5VL_loc_params_t* loc_params,
                   const char* name, hid_t gapl_id, hid_t dxpl_id, void** req) {
  if (g_vol_cfg.fail_group_open) {
    return nullptr;
  }
  auto* o = static_cast<TestVolObj*>(raw_obj);
  void* under = H5VLgroup_open(o->under_obj, loc_params, o->under_vol_id, name,
                               gapl_id, dxpl_id, req);
  return WrapUnderObj(under, o->under_vol_id);
}

herr_t VolGroupGet(void* raw_obj, H5VL_group_get_args_t* args, hid_t dxpl_id,
                   void** req) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  return H5VLgroup_get(o->under_obj, o->under_vol_id, args, dxpl_id, req);
}

herr_t VolGroupSpecific(void* raw_obj, H5VL_group_specific_args_t* args,
                        hid_t dxpl_id, void** req) {
  if (g_vol_cfg.fail_group_specific) {
    return -1;
  }
  auto* o = static_cast<TestVolObj*>(raw_obj);
  if (args->op_type == H5VL_GROUP_MOUNT) {
    H5VL_group_specific_args_t unwrapped = *args;
    unwrapped.args.mount.child_file =
        static_cast<TestVolObj*>(args->args.mount.child_file)->under_obj;
    return H5VLgroup_specific(o->under_obj, o->under_vol_id, &unwrapped,
                              dxpl_id, req);
  }
  return H5VLgroup_specific(o->under_obj, o->under_vol_id, args, dxpl_id, req);
}

herr_t VolGroupOptional(void* raw_obj, H5VL_optional_args_t* args,
                        hid_t dxpl_id, void** req) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  return H5VLgroup_optional(o->under_obj, o->under_vol_id, args, dxpl_id, req);
}

herr_t VolGroupClose(void* raw_grp, hid_t dxpl_id, void** req) {
  auto* o = static_cast<TestVolObj*>(raw_grp);
  const bool force_fail = g_vol_cfg.fail_group_close;
  // Close and release underlying object even when testing H5Fmount/H5Funmount
  // cleanup error handling so no HDF5 group object leaks.
  herr_t rc = H5VLgroup_close(o->under_obj, o->under_vol_id, dxpl_id, req);
  if (rc >= 0) {
    FreeVolObj(o);
  }
  return force_fail ? -1 : rc;
}

herr_t VolLinkCreate(H5VL_link_create_args_t* args, void* raw_obj,
                     const H5VL_loc_params_t* loc_params, hid_t lcpl_id,
                     hid_t lapl_id, hid_t dxpl_id, void** req) {
  void* under_obj = nullptr;
  hid_t under_vol_id = H5I_INVALID_HID;
  if (raw_obj != nullptr) {
    auto* o = static_cast<TestVolObj*>(raw_obj);
    under_obj = o->under_obj;
    under_vol_id = o->under_vol_id;
  } else {
    under_vol_id = H5VLget_connector_id_by_name("native");
  }
  return H5VLlink_create(args, under_obj, loc_params, under_vol_id, lcpl_id,
                         lapl_id, dxpl_id, req);
}

herr_t VolLinkCopy(void* src_obj, const H5VL_loc_params_t* loc_params1,
                   void* dst_obj, const H5VL_loc_params_t* loc_params2,
                   hid_t lcpl_id, hid_t lapl_id, hid_t dxpl_id, void** req) {
  auto* s = static_cast<TestVolObj*>(src_obj);
  auto* d = static_cast<TestVolObj*>(dst_obj);
  return H5VLlink_copy(s->under_obj, loc_params1, d->under_obj, loc_params2,
                       s->under_vol_id, lcpl_id, lapl_id, dxpl_id, req);
}

herr_t VolLinkMove(void* src_obj, const H5VL_loc_params_t* loc_params1,
                   void* dst_obj, const H5VL_loc_params_t* loc_params2,
                   hid_t lcpl_id, hid_t lapl_id, hid_t dxpl_id, void** req) {
  auto* s = static_cast<TestVolObj*>(src_obj);
  auto* d = static_cast<TestVolObj*>(dst_obj);
  return H5VLlink_move(s->under_obj, loc_params1, d->under_obj, loc_params2,
                       s->under_vol_id, lcpl_id, lapl_id, dxpl_id, req);
}

herr_t VolLinkGet(void* raw_obj, const H5VL_loc_params_t* loc_params,
                  H5VL_link_get_args_t* args, hid_t dxpl_id, void** req) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  return H5VLlink_get(o->under_obj, loc_params, o->under_vol_id, args, dxpl_id,
                      req);
}

herr_t VolLinkSpecific(void* raw_obj, const H5VL_loc_params_t* loc_params,
                       H5VL_link_specific_args_t* args, hid_t dxpl_id,
                       void** req) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  return H5VLlink_specific(o->under_obj, loc_params, o->under_vol_id, args,
                           dxpl_id, req);
}

herr_t VolLinkOptional(void* raw_obj, const H5VL_loc_params_t* loc_params,
                       H5VL_optional_args_t* args, hid_t dxpl_id, void** req) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  return H5VLlink_optional(o->under_obj, loc_params, o->under_vol_id, args,
                           dxpl_id, req);
}

void* VolObjectOpen(void* raw_obj, const H5VL_loc_params_t* loc_params,
                    H5I_type_t* opened_type, hid_t dxpl_id, void** req) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  void* under = H5VLobject_open(o->under_obj, loc_params, o->under_vol_id,
                                opened_type, dxpl_id, req);
  return WrapUnderObj(under, o->under_vol_id);
}

herr_t VolObjectCopy(void* src_obj, const H5VL_loc_params_t* src_loc_params,
                     const char* src_name, void* dst_obj,
                     const H5VL_loc_params_t* dst_loc_params,
                     const char* dst_name, hid_t ocpypl_id, hid_t lcpl_id,
                     hid_t dxpl_id, void** req) {
  auto* s = static_cast<TestVolObj*>(src_obj);
  auto* d = static_cast<TestVolObj*>(dst_obj);
  return H5VLobject_copy(s->under_obj, src_loc_params, src_name, d->under_obj,
                         dst_loc_params, dst_name, s->under_vol_id, ocpypl_id,
                         lcpl_id, dxpl_id, req);
}

herr_t VolObjectGet(void* raw_obj, const H5VL_loc_params_t* loc_params,
                    H5VL_object_get_args_t* args, hid_t dxpl_id, void** req) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  return H5VLobject_get(o->under_obj, loc_params, o->under_vol_id, args,
                        dxpl_id, req);
}

herr_t VolObjectSpecific(void* raw_obj, const H5VL_loc_params_t* loc_params,
                         H5VL_object_specific_args_t* args, hid_t dxpl_id,
                         void** req) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  return H5VLobject_specific(o->under_obj, loc_params, o->under_vol_id, args,
                             dxpl_id, req);
}

herr_t VolObjectOptional(void* raw_obj, const H5VL_loc_params_t* loc_params,
                         H5VL_optional_args_t* args, hid_t dxpl_id,
                         void** req) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  return H5VLobject_optional(o->under_obj, loc_params, o->under_vol_id, args,
                             dxpl_id, req);
}

extern const H5VL_class_t kTestVolClass;

herr_t VolIntrospectGetConnCls(void* raw_obj, H5VL_get_conn_lvl_t lvl,
                               const H5VL_class_t** conn_cls) {
  if (lvl == H5VL_GET_CONN_LVL_CURR) {
    *conn_cls = &kTestVolClass;
    return 0;
  }
  auto* o = static_cast<TestVolObj*>(raw_obj);
  return H5VLintrospect_get_conn_cls(o->under_obj, o->under_vol_id, lvl,
                                     conn_cls);
}

herr_t VolIntrospectGetCapFlags(const void* raw_info, uint64_t* cap_flags) {
  const auto* info = static_cast<const TestVolInfo*>(raw_info);
  if (info == nullptr || H5Iis_valid(info->under_vol_id) <= 0) {
    return -1;
  }
  return H5VLintrospect_get_cap_flags(info->under_vol_info, info->under_vol_id,
                                      cap_flags);
}

herr_t VolIntrospectOptQuery(void* raw_obj, H5VL_subclass_t cls, int opt_type,
                             uint64_t* flags) {
  if (cls == H5VL_SUBCLS_FILE && opt_type == H5VL_NATIVE_FILE_POST_OPEN) {
    if (g_vol_cfg.fail_opt_query) {
      return -1;
    }
  }
  auto* o = static_cast<TestVolObj*>(raw_obj);
  herr_t rc =
      H5VLintrospect_opt_query(o->under_obj, o->under_vol_id, cls, opt_type,
                               flags);
  if (rc >= 0 && cls == H5VL_SUBCLS_FILE &&
      opt_type == H5VL_NATIVE_FILE_POST_OPEN) {
    *flags |= H5VL_OPT_QUERY_SUPPORTED;
  }
  return rc;
}

herr_t VolRequestWait(void* raw_req, uint64_t /*timeout*/,
                      H5VL_request_status_t* status) {
  auto* r = static_cast<TestVolReq*>(raw_req);
  *status = r->status;
  return 0;
}

herr_t VolRequestNotify(void* raw_req, H5VL_request_notify_t cb, void* ctx) {
  auto* r = static_cast<TestVolReq*>(raw_req);
  if (cb != nullptr) {
    cb(ctx, r->status);
  }
  return 0;
}

herr_t VolRequestCancel(void* /*raw_req*/, H5VL_request_status_t* status) {
  *status = H5VL_REQUEST_STATUS_CANCELED;
  return 0;
}

herr_t VolRequestSpecific(void* /*raw_req*/,
                          H5VL_request_specific_args_t* args) {
  if (args->op_type == H5VL_REQUEST_GET_EXEC_TIME) {
    *args->args.get_exec_time.exec_ts = 0;
    *args->args.get_exec_time.exec_time = 0;
  } else if (args->op_type == H5VL_REQUEST_GET_ERR_STACK) {
    args->args.get_err_stack.err_stack_id = H5I_INVALID_HID;
  }
  return 0;
}

herr_t VolRequestOptional(void* /*raw_req*/, H5VL_optional_args_t* /*args*/) {
  return 0;
}

herr_t VolRequestFree(void* raw_req) {
  auto* r = static_cast<TestVolReq*>(raw_req);
  if (r != nullptr) {
    const hid_t err_id = H5Eget_current_stack();
    if (r->under_vol_id != H5I_INVALID_HID) {
      H5Idec_ref(r->under_vol_id);
    }
    H5Eset_current_stack(err_id);
    delete r;
  }
  return 0;
}

herr_t VolBlobPut(void* raw_obj, const void* buf, size_t size, void* blob_id,
                  void* ctx) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  return H5VLblob_put(o->under_obj, o->under_vol_id, buf, size, blob_id, ctx);
}

herr_t VolBlobGet(void* raw_obj, const void* blob_id, void* buf, size_t size,
                  void* ctx) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  return H5VLblob_get(o->under_obj, o->under_vol_id, blob_id, buf, size, ctx);
}

herr_t VolBlobSpecific(void* raw_obj, void* blob_id,
                       H5VL_blob_specific_args_t* args) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  return H5VLblob_specific(o->under_obj, o->under_vol_id, blob_id, args);
}

herr_t VolBlobOptional(void* raw_obj, void* blob_id,
                       H5VL_optional_args_t* args) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  return H5VLblob_optional(o->under_obj, o->under_vol_id, blob_id, args);
}

herr_t VolTokenCmp(void* raw_obj, const H5O_token_t* token1,
                   const H5O_token_t* token2, int* cmp_value) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  return H5VLtoken_cmp(o->under_obj, o->under_vol_id, token1, token2,
                       cmp_value);
}

herr_t VolTokenToStr(void* raw_obj, H5I_type_t obj_type,
                     const H5O_token_t* token, char** token_str) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  return H5VLtoken_to_str(o->under_obj, obj_type, o->under_vol_id, token,
                          token_str);
}

herr_t VolTokenFromStr(void* raw_obj, H5I_type_t obj_type,
                       const char* token_str, H5O_token_t* token) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  return H5VLtoken_from_str(o->under_obj, obj_type, o->under_vol_id, token_str,
                            token);
}

herr_t VolOptional(void* raw_obj, H5VL_optional_args_t* args, hid_t dxpl_id,
                   void** req) {
  auto* o = static_cast<TestVolObj*>(raw_obj);
  return H5VLoptional(o->under_obj, o->under_vol_id, args, dxpl_id, req);
}

const H5VL_class_t kTestVolClass = {
    H5VL_VERSION,
    static_cast<H5VL_class_value_t>(510),
    "h5f_test_passthrough_vol",
    1,
    H5VL_CAP_FLAG_ASYNC,
    VolInit,
    VolTerm,
    {sizeof(TestVolInfo), VolInfoCopy, VolInfoCmp, VolInfoFree, VolInfoToStr,
     VolInfoFromStr},
    {VolWrapGetObject, VolWrapGetCtx, VolWrapObject, VolUnwrapObject,
     VolFreeWrapCtx},
    {VolAttrCreate, VolAttrOpen, VolAttrRead, VolAttrWrite, VolAttrGet,
     VolAttrSpecific, VolAttrOptional, VolAttrClose},
    {VolDatasetCreate, VolDatasetOpen, VolDatasetRead, VolDatasetWrite,
     VolDatasetGet, VolDatasetSpecific, VolDatasetOptional, VolDatasetClose},
    {VolDatatypeCommit, VolDatatypeOpen, VolDatatypeGet, VolDatatypeSpecific,
     VolDatatypeOptional, VolDatatypeClose},
    {VolFileCreate, VolFileOpen, VolFileGet, VolFileSpecific, VolFileOptional,
     VolFileClose},
    {VolGroupCreate, VolGroupOpen, VolGroupGet, VolGroupSpecific,
     VolGroupOptional, VolGroupClose},
    {VolLinkCreate, VolLinkCopy, VolLinkMove, VolLinkGet, VolLinkSpecific,
     VolLinkOptional},
    {VolObjectOpen, VolObjectCopy, VolObjectGet, VolObjectSpecific,
     VolObjectOptional},
    {VolIntrospectGetConnCls, VolIntrospectGetCapFlags, VolIntrospectOptQuery},
    {VolRequestWait, VolRequestNotify, VolRequestCancel, VolRequestSpecific,
     VolRequestOptional, VolRequestFree},
    {VolBlobPut, VolBlobGet, VolBlobSpecific, VolBlobOptional},
    {VolTokenCmp, VolTokenToStr, VolTokenFromStr},
    VolOptional,
};

// RAII registration of kTestVolClass on top of the native VOL connector.
class ScopedTestVol {
 public:
  ScopedTestVol() {
    g_vol_cfg.Reset();
    native_vol_id_ = H5VLget_connector_id_by_name("native");
    vol_id_ = H5VLregister_connector(&kTestVolClass, H5P_DEFAULT);
  }

  ~ScopedTestVol() {
    g_vol_cfg.Reset();
    if (vol_id_ != H5I_INVALID_HID) {
      H5VLunregister_connector(vol_id_);
    }
    if (native_vol_id_ != H5I_INVALID_HID) {
      H5VLclose(native_vol_id_);
    }
  }

  ScopedPlist MakeCoreFapl() const {
    ScopedPlist fapl = hdf5::MakeCoreFapl();
    if (fapl.valid()) {
      TestVolInfo info{native_vol_id_, nullptr};
      if (H5Pset_vol(fapl, vol_id_, &info) < 0) {
        fapl.reset();
      }
    }
    return fapl;
  }

  ScopedPlist MakeSec2Fapl() const {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    if (fapl.valid()) {
      TestVolInfo info{native_vol_id_, nullptr};
      if (H5Pset_fapl_sec2(fapl) < 0 || H5Pset_vol(fapl, vol_id_, &info) < 0) {
        fapl.reset();
      }
    }
    return fapl;
  }

  hid_t vol_id() const { return vol_id_; }

 private:
  hid_t native_vol_id_ = H5I_INVALID_HID;
  hid_t vol_id_ = H5I_INVALID_HID;
};

std::vector<hid_t> GetOpenFileIds() {
  const ssize_t count = H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_FILE);
  if (count <= 0) {
    return {};
  }
  std::vector<hid_t> ids(static_cast<size_t>(count));
  const ssize_t got =
      H5Fget_obj_ids(H5F_OBJ_ALL, H5F_OBJ_FILE, ids.size(), ids.data());
  if (got < 0) {
    return {};
  }
  ids.resize(static_cast<size_t>(got));
  return ids;
}

// Closes any H5I_FILE handles created during calls that fail after
// H5VL_register() inside H5Fcreate/open/reopen (upstream H5F.c error paths).
void CloseNewFileIds(const std::vector<hid_t>& before) {
  for (hid_t id : GetOpenFileIds()) {
    bool existed = false;
    for (hid_t b : before) {
      if (b == id) {
        existed = true;
        break;
      }
    }
    if (!existed) {
      H5Fclose(id);
    }
  }
}

ScopedPlist MakeSec2Fapl() {
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  if (fapl.valid() && H5Pset_fapl_sec2(fapl) < 0) {
    fapl.reset();
  }
  return fapl;
}

// Creates a populated group/dataset/datatype/attribute in `file` for tests.
struct SampleObjects {
  ScopedSpace space;
  ScopedType dtype;
  ScopedGroup group;
  ScopedDataset dset;
  ScopedAttr attr;
};

SampleObjects CreateSampleObjects(hid_t file) {
  SampleObjects objs;
  objs.space.reset(H5Screate(H5S_SCALAR));
  objs.dtype.reset(H5Tcopy(H5T_NATIVE_INT32));
  EXPECT_GE(H5Tcommit2(file, "committed_int", objs.dtype, H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  objs.group.reset(
      H5Gcreate2(file, "grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  objs.dset.reset(H5Dcreate2(objs.group, "ds", H5T_NATIVE_INT32, objs.space,
                             H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  const int32_t val = 42;
  EXPECT_GE(H5Dwrite(objs.dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     &val),
            0);
  objs.attr.reset(
      H5Acreate2(objs.dset, "tag", H5T_NATIVE_INT32, objs.space, H5P_DEFAULT,
                 H5P_DEFAULT));
  EXPECT_GE(H5Awrite(objs.attr, H5T_NATIVE_INT32, &val), 0);
  return objs;
}

TEST(H5FTest, GetCreateAndAccessPlists) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_userblock(fcpl, 512), 0);

  ScopedFile file = CreateInMemoryFile("plist_test.h5", H5F_ACC_TRUNC, fcpl);
  ASSERT_TRUE(file.valid());

  ScopedPlist got_fcpl(H5Fget_create_plist(file));
  ASSERT_TRUE(got_fcpl.valid());
  hsize_t userblock = 0;
  ASSERT_GE(H5Pget_userblock(got_fcpl, &userblock), 0);
  EXPECT_EQ(userblock, 512);

  ScopedPlist got_fapl(H5Fget_access_plist(file));
  ASSERT_TRUE(got_fapl.valid());
  EXPECT_EQ(H5Pget_driver(got_fapl), H5FD_CORE);

  ScopedErrorSilence silence;
  EXPECT_EQ(H5Fget_create_plist(H5I_INVALID_HID), H5I_INVALID_HID);
  EXPECT_EQ(H5Fget_create_plist(fcpl), H5I_INVALID_HID);
  EXPECT_EQ(H5Fget_access_plist(H5I_INVALID_HID), H5I_INVALID_HID);
  EXPECT_EQ(H5Fget_access_plist(fcpl), H5I_INVALID_HID);
}

TEST(H5FTest, ObjCountAndObjIdsSingleAndAllFiles) {
  ScopedFile file1 = CreateInMemoryFile("obj1.h5");
  ScopedFile file2 = CreateInMemoryFile("obj2.h5");
  ASSERT_TRUE(file1.valid());
  ASSERT_TRUE(file2.valid());

  SampleObjects objs = CreateSampleObjects(file1);
  ASSERT_TRUE(objs.group.valid());
  ASSERT_TRUE(objs.dset.valid());
  ASSERT_TRUE(objs.attr.valid());
  ASSERT_TRUE(objs.dtype.valid());

  // Per-file object count & IDs.
  EXPECT_EQ(H5Fget_obj_count(file1, H5F_OBJ_FILE), 1);
  EXPECT_EQ(H5Fget_obj_count(file1, H5F_OBJ_GROUP), 1);
  EXPECT_EQ(H5Fget_obj_count(file1, H5F_OBJ_DATASET), 1);
  EXPECT_EQ(H5Fget_obj_count(file1, H5F_OBJ_DATATYPE), 1);
  EXPECT_EQ(H5Fget_obj_count(file1, H5F_OBJ_ATTR), 1);
  const ssize_t f1_total = H5Fget_obj_count(file1, H5F_OBJ_ALL);
  EXPECT_EQ(f1_total, 5);

  std::vector<hid_t> f1_ids(static_cast<size_t>(f1_total));
  EXPECT_EQ(H5Fget_obj_ids(file1, H5F_OBJ_ALL, f1_ids.size(), f1_ids.data()),
            5);

  // Library-wide counts and IDs across all object types and truncation.
  EXPECT_GE(H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_FILE), 2);
  EXPECT_GE(H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_DATASET), 1);
  EXPECT_GE(H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_GROUP), 1);
  EXPECT_GE(H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_DATATYPE), 1);
  EXPECT_GE(H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_ATTR), 1);
  const ssize_t all_total = H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_ALL);
  EXPECT_GE(all_total, 6);

  std::vector<hid_t> all_ids(static_cast<size_t>(all_total));
  EXPECT_EQ(
      H5Fget_obj_ids(H5F_OBJ_ALL, H5F_OBJ_ALL, all_ids.size(), all_ids.data()),
      all_total);

  // Ask for each type individually as well as max_objs truncation -> H5_ITER_STOP.
  std::array<hid_t, 2> one_id = {H5I_INVALID_HID, H5I_INVALID_HID};
  EXPECT_EQ(H5Fget_obj_ids(H5F_OBJ_ALL, H5F_OBJ_FILE, 1, one_id.data()), 1);
  EXPECT_EQ(H5Fget_obj_ids(H5F_OBJ_ALL, H5F_OBJ_DATASET, 1, one_id.data()), 1);
  EXPECT_EQ(H5Fget_obj_ids(H5F_OBJ_ALL, H5F_OBJ_GROUP, 1, one_id.data()), 1);
  EXPECT_EQ(H5Fget_obj_ids(H5F_OBJ_ALL, H5F_OBJ_DATATYPE, 1, one_id.data()), 1);
  EXPECT_EQ(H5Fget_obj_ids(H5F_OBJ_ALL, H5F_OBJ_ATTR, 1, one_id.data()), 1);
  EXPECT_EQ(H5Fget_obj_ids(H5F_OBJ_ALL, H5F_OBJ_ALL, 0, one_id.data()), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Fget_obj_count(file1, 0), 0);
  EXPECT_LT(H5Fget_obj_count(H5I_INVALID_HID, H5F_OBJ_FILE), 0);
  EXPECT_LT(H5Fget_obj_count(objs.group, H5F_OBJ_FILE), 0);
  EXPECT_LT(H5Fget_obj_ids(file1, 0, 1, one_id.data()), 0);
  EXPECT_LT(H5Fget_obj_ids(file1, H5F_OBJ_FILE, 1, nullptr), 0);
  EXPECT_LT(H5Fget_obj_ids(H5I_INVALID_HID, H5F_OBJ_FILE, 1, one_id.data()), 0);
  EXPECT_LT(H5Fget_obj_ids(objs.group, H5F_OBJ_FILE, 1, one_id.data()), 0);
}

TEST(H5FTest, GetVfdHandleCoreAndSec2) {
  ScopedFile mem = CreateInMemoryFile("vfd_core.h5");
  ASSERT_TRUE(mem.valid());
  void* core_handle = nullptr;
  ASSERT_GE(H5Fget_vfd_handle(mem, H5P_DEFAULT, &core_handle), 0);
  EXPECT_NE(core_handle, nullptr);

  TempFile sec2_path("vfd_sec2.h5");
  ScopedPlist fapl = MakeSec2Fapl();
  ASSERT_TRUE(fapl.valid());
  ScopedFile sec2(
      H5Fcreate(sec2_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(sec2.valid());
  void* fd_ptr = nullptr;
  ASSERT_GE(H5Fget_vfd_handle(sec2, H5P_DEFAULT, &fd_ptr), 0);
  ASSERT_NE(fd_ptr, nullptr);
  EXPECT_GE(*static_cast<int*>(fd_ptr), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Fget_vfd_handle(mem, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5Fget_vfd_handle(H5I_INVALID_HID, H5P_DEFAULT, &core_handle), 0);
  EXPECT_LT(H5Fget_vfd_handle(fapl, H5P_DEFAULT, &core_handle), 0);
}

TEST(H5FTest, IsAccessibleAndDelete) {
  TempFile h5_path("accessible_test.h5");
  ScopedPlist fapl = MakeSec2Fapl();
  ASSERT_TRUE(fapl.valid());
  {
    ScopedFile f(
        H5Fcreate(h5_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(f.valid());
  }

  EXPECT_GT(H5Fis_accessible(h5_path.c_str(), H5P_DEFAULT), 0);
  EXPECT_GT(H5Fis_accessible(h5_path.c_str(), fapl), 0);

  TempFile txt_path("not_hdf5.bin");
  {
    std::FILE* fp = std::fopen(txt_path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    const char kJunk[] = "NOT_AN_HDF5_FILE";
    std::fwrite(kJunk, 1, sizeof(kJunk), fp);
    std::fclose(fp);
  }
  EXPECT_EQ(H5Fis_accessible(txt_path.c_str(), fapl), 0);

  // Deleting valid HDF5 file succeeds; deleting non-HDF5 or bad paths fails.
  ASSERT_GE(H5Fdelete(h5_path.c_str(), H5P_DEFAULT), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Fis_accessible(h5_path.c_str(), H5P_DEFAULT), 0);
  EXPECT_LT(H5Fis_accessible(nullptr, H5P_DEFAULT), 0);
  EXPECT_LT(H5Fis_accessible("", H5P_DEFAULT), 0);
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  EXPECT_LT(H5Fis_accessible(txt_path.c_str(), fcpl), 0);

  EXPECT_LT(H5Fdelete(nullptr, H5P_DEFAULT), 0);
  EXPECT_LT(H5Fdelete("", H5P_DEFAULT), 0);
  EXPECT_LT(H5Fdelete(txt_path.c_str(), fcpl), 0);
  EXPECT_LT(H5Fdelete(h5_path.c_str(), fapl), 0);
  EXPECT_LT(H5Fdelete(txt_path.c_str(), fapl), 0);
}

TEST(H5FTest, CreateAndOpenFlagsAndInvalidArgs) {
  TempFile path("create_open_flags.h5");
  ScopedPlist fapl = MakeSec2Fapl();
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fapl.valid());
  ASSERT_TRUE(fcpl.valid());

  // flags == 0 defaults to H5F_ACC_EXCL, so a second create with 0 fails.
  {
    ScopedFile f(H5Fcreate(path.c_str(), 0, H5P_DEFAULT, fapl));
    ASSERT_TRUE(f.valid());
  }
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5Fcreate(path.c_str(), 0, H5P_DEFAULT, fapl), H5I_INVALID_HID);
  }

  // Recreate with latest libver bounds so SWMR open works on Sec2.
  ASSERT_GE(H5Pset_libver_bounds(fapl, H5F_LIBVER_V110, H5F_LIBVER_LATEST), 0);
  {
    ScopedFile f(H5Fcreate(path.c_str(), H5F_ACC_TRUNC, fcpl, fapl));
    ASSERT_TRUE(f.valid());
    ScopedGroup g(H5Gcreate2(f, "swmr_g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    EXPECT_TRUE(g.valid());
  }

  {
    ScopedFile rw(H5Fopen(path.c_str(), H5F_ACC_RDWR | H5F_ACC_SWMR_WRITE, fapl));
    ASSERT_TRUE(rw.valid());
    unsigned intent = 0;
    ASSERT_GE(H5Fget_intent(rw, &intent), 0);
    EXPECT_EQ(intent, H5F_ACC_RDWR | H5F_ACC_SWMR_WRITE);

    ScopedFile ro(
        H5Fopen(path.c_str(), H5F_ACC_RDONLY | H5F_ACC_SWMR_READ, fapl));
    ASSERT_TRUE(ro.valid());
  }

  {
    ScopedFile f_ro(H5Fopen(path.c_str(), H5F_ACC_RDONLY, fapl));
    ASSERT_TRUE(f_ro.valid());
    unsigned intent = 999;
    ASSERT_GE(H5Fget_intent(f_ro, &intent), 0);
    EXPECT_EQ(intent, H5F_ACC_RDONLY);
  }

  ScopedErrorSilence silence;
  EXPECT_EQ(H5Fcreate(nullptr, H5F_ACC_TRUNC, H5P_DEFAULT, fapl),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Fcreate("", H5F_ACC_TRUNC, H5P_DEFAULT, fapl), H5I_INVALID_HID);
  EXPECT_EQ(H5Fcreate(path.c_str(), 0x8000u, H5P_DEFAULT, fapl),
            H5I_INVALID_HID);
  EXPECT_EQ(
      H5Fcreate(path.c_str(), H5F_ACC_TRUNC | H5F_ACC_EXCL, H5P_DEFAULT, fapl),
      H5I_INVALID_HID);
  EXPECT_EQ(H5Fcreate(path.c_str(), H5F_ACC_TRUNC, fapl, fapl),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fcpl),
            H5I_INVALID_HID);

  EXPECT_EQ(H5Fopen(nullptr, H5F_ACC_RDONLY, fapl), H5I_INVALID_HID);
  EXPECT_EQ(H5Fopen("", H5F_ACC_RDONLY, fapl), H5I_INVALID_HID);
  EXPECT_EQ(H5Fopen(path.c_str(), H5F_ACC_TRUNC, fapl), H5I_INVALID_HID);
  EXPECT_EQ(H5Fopen(path.c_str(), H5F_ACC_EXCL, fapl), H5I_INVALID_HID);
  EXPECT_EQ(H5Fopen(path.c_str(), 0x8000u, fapl), H5I_INVALID_HID);
  EXPECT_EQ(H5Fopen(path.c_str(), H5F_ACC_SWMR_WRITE, fapl), H5I_INVALID_HID);
  EXPECT_EQ(H5Fopen(path.c_str(), H5F_ACC_RDWR | H5F_ACC_SWMR_READ, fapl),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Fopen(path.c_str(), H5F_ACC_RDONLY, fcpl), H5I_INVALID_HID);
  TempFile missing("does_not_exist.h5");
  EXPECT_EQ(H5Fopen(missing.c_str(), H5F_ACC_RDONLY, fapl), H5I_INVALID_HID);
}

TEST(H5FTest, ReopenFlushAndCloseAllObjectTypes) {
  ScopedFile file = CreateInMemoryFile("reopen_flush.h5");
  ASSERT_TRUE(file.valid());
  SampleObjects objs = CreateSampleObjects(file);
  ASSERT_TRUE(objs.attr.valid());

  EXPECT_GE(H5Fflush(file, H5F_SCOPE_LOCAL), 0);
  EXPECT_GE(H5Fflush(objs.group, H5F_SCOPE_GLOBAL), 0);
  EXPECT_GE(H5Fflush(objs.dset, H5F_SCOPE_LOCAL), 0);
  EXPECT_GE(H5Fflush(objs.dtype, H5F_SCOPE_GLOBAL), 0);
  EXPECT_GE(H5Fflush(objs.attr, H5F_SCOPE_LOCAL), 0);

  ScopedFile reopened(H5Freopen(file));
  ASSERT_TRUE(reopened.valid());
  unsigned long fno1 = 0;
  unsigned long fno2 = 0;
  ASSERT_GE(H5Fget_fileno(file, &fno1), 0);
  ASSERT_GE(H5Fget_fileno(reopened, &fno2), 0);
  EXPECT_EQ(fno1, fno2);

  // Close one handle explicitly to test H5Fclose success directly.
  EXPECT_GE(H5Fclose(reopened.release()), 0);

  ScopedErrorSilence silence;
  EXPECT_EQ(H5Freopen(H5I_INVALID_HID), H5I_INVALID_HID);
  EXPECT_EQ(H5Freopen(objs.group), H5I_INVALID_HID);
  EXPECT_LT(H5Fflush(H5I_INVALID_HID, H5F_SCOPE_LOCAL), 0);
  EXPECT_LT(H5Fflush(objs.space, H5F_SCOPE_LOCAL), 0);
  EXPECT_LT(H5Fclose(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Fclose(objs.group), 0);
}

TEST(H5FTest, NativeAsyncApisWithNoneAndEmptyEventSet) {
  TempFile path("async_native.h5");
  ScopedPlist fapl = MakeSec2Fapl();
  ASSERT_TRUE(fapl.valid());

  // H5ES_NONE path (synchronous execution via *_async entry points).
  {
    ScopedFile f(
        H5Fcreate_async(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl,
                        H5ES_NONE));
    ASSERT_TRUE(f.valid());
    ScopedFile r(H5Freopen_async(f, H5ES_NONE));
    ASSERT_TRUE(r.valid());
    EXPECT_GE(H5Fflush_async(f, H5F_SCOPE_GLOBAL, H5ES_NONE), 0);
    EXPECT_GE(H5Fclose_async(r.release(), H5ES_NONE), 0);
    EXPECT_GE(H5Fclose_async(f.release(), H5ES_NONE), 0);
  }
  {
    ScopedFile f(H5Fopen_async(path.c_str(), H5F_ACC_RDONLY, fapl, H5ES_NONE));
    ASSERT_TRUE(f.valid());
    EXPECT_GE(H5Fclose_async(f.release(), H5ES_NONE), 0);
  }

  // Real H5ES_t with native VOL (no request tokens produced, exercises token_ptr
  // setup and connector refcount inc/dec paths safely).
  ScopedEventSet es(H5EScreate());
  ASSERT_TRUE(es.valid());
  {
    ScopedFile f(
        H5Fcreate_async(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl, es));
    ASSERT_TRUE(f.valid());
    ScopedFile r(H5Freopen_async(f, es));
    ASSERT_TRUE(r.valid());
    EXPECT_GE(H5Fflush_async(f, H5F_SCOPE_LOCAL, es), 0);
    EXPECT_GE(H5Fclose_async(r.release(), es), 0);
    EXPECT_GE(H5Fclose_async(f.release(), es), 0);
  }
  {
    ScopedFile f(H5Fopen_async(path.c_str(), H5F_ACC_RDWR, fapl, es));
    ASSERT_TRUE(f.valid());
    EXPECT_GE(H5Fclose_async(f.release(), es), 0);
  }

  ScopedErrorSilence silence;
  EXPECT_EQ(
      H5Fcreate_async(nullptr, H5F_ACC_TRUNC, H5P_DEFAULT, fapl, H5ES_NONE),
      H5I_INVALID_HID);
  EXPECT_EQ(H5Fopen_async(nullptr, H5F_ACC_RDONLY, fapl, H5ES_NONE),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Freopen_async(H5I_INVALID_HID, H5ES_NONE), H5I_INVALID_HID);
  EXPECT_LT(H5Fflush_async(H5I_INVALID_HID, H5F_SCOPE_LOCAL, H5ES_NONE), 0);
  EXPECT_LT(H5Fclose_async(H5I_INVALID_HID, H5ES_NONE), 0);
}

TEST(H5FTest, MountAndUnmountByFileAndGroupIds) {
  ScopedFile parent = CreateInMemoryFile("mount_parent.h5");
  ScopedFile child = CreateInMemoryFile("mount_child.h5");
  ASSERT_TRUE(parent.valid());
  ASSERT_TRUE(child.valid());

  ScopedGroup mnt_grp(
      H5Gcreate2(parent, "mnt", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup sub_grp(
      H5Gcreate2(mnt_grp, "sub", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(mnt_grp.valid());
  ASSERT_TRUE(sub_grp.valid());

  ScopedSpace s(H5Screate(H5S_SCALAR));
  ScopedDataset child_ds(H5Dcreate2(child, "child_ds", H5T_NATIVE_INT32, s,
                                    H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(child_ds.valid());
  const int32_t expected = 1234;
  ASSERT_GE(
      H5Dwrite(child_ds, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
               &expected),
      0);

  // Mount and unmount using file ID.
  ASSERT_GE(H5Fmount(parent, "/mnt", child, H5P_DEFAULT), 0);
  {
    ScopedDataset opened(H5Dopen2(parent, "/mnt/child_ds", H5P_DEFAULT));
    ASSERT_TRUE(opened.valid());
    int32_t got = 0;
    ASSERT_GE(H5Dread(opened, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      &got),
              0);
    EXPECT_EQ(got, expected);
  }
  ASSERT_GE(H5Funmount(parent, "/mnt"), 0);

  // Mount and unmount using group ID.
  ASSERT_GE(H5Fmount(mnt_grp, "sub", child, H5P_DEFAULT), 0);
  ASSERT_GE(H5Funmount(mnt_grp, "sub"), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Fmount(child_ds, "mnt", child, H5P_DEFAULT), 0);
  EXPECT_LT(H5Fmount(parent, nullptr, child, H5P_DEFAULT), 0);
  EXPECT_LT(H5Fmount(parent, "", child, H5P_DEFAULT), 0);
  EXPECT_LT(H5Fmount(parent, "/mnt", mnt_grp, H5P_DEFAULT), 0);
  ScopedPlist fapl = MakeCoreFapl();
  EXPECT_LT(H5Fmount(parent, "/mnt", child, fapl), 0);
  EXPECT_LT(H5Fmount(parent, "/nonexistent", child, H5P_DEFAULT), 0);

  EXPECT_LT(H5Funmount(child_ds, "/mnt"), 0);
  EXPECT_LT(H5Funmount(parent, nullptr), 0);
  EXPECT_LT(H5Funmount(parent, ""), 0);
  EXPECT_LT(H5Funmount(parent, "/mnt"), 0);
}

TEST(H5FTest, IntentFilenoNameInfoAndDsetHint) {
  ScopedFile f1 = CreateInMemoryFile("alpha_file.h5");
  ScopedFile f2 = CreateInMemoryFile("beta_file.h5");
  ASSERT_TRUE(f1.valid());
  ASSERT_TRUE(f2.valid());

  SampleObjects objs = CreateSampleObjects(f1);
  ASSERT_TRUE(objs.attr.valid());

  // NULL out-pointers in H5Fget_intent / H5Fget_fileno exit quietly (SUCCEED).
  EXPECT_GE(H5Fget_intent(H5I_INVALID_HID, nullptr), 0);
  EXPECT_GE(H5Fget_fileno(H5I_INVALID_HID, nullptr), 0);

  unsigned intent = 0;
  ASSERT_GE(H5Fget_intent(f1, &intent), 0);
  EXPECT_EQ(intent, H5F_ACC_RDWR);

  unsigned long fno1 = 0;
  unsigned long fno2 = 0;
  ASSERT_GE(H5Fget_fileno(f1, &fno1), 0);
  ASSERT_GE(H5Fget_fileno(f2, &fno2), 0);
  EXPECT_NE(fno1, fno2);

  // H5Fget_name across all object types, size==0, NULL buf, and truncation.
  const ssize_t len = H5Fget_name(f1, nullptr, 0);
  ASSERT_GT(len, 0);
  std::string name_buf(static_cast<size_t>(len) + 1, '\0');
  EXPECT_EQ(H5Fget_name(objs.group, name_buf.data(), name_buf.size()), len);
  EXPECT_EQ(name_buf.c_str(), std::string("alpha_file.h5"));
  EXPECT_EQ(H5Fget_name(objs.dset, name_buf.data(), name_buf.size()), len);
  EXPECT_EQ(H5Fget_name(objs.dtype, name_buf.data(), name_buf.size()), len);
  EXPECT_EQ(H5Fget_name(objs.attr, name_buf.data(), name_buf.size()), len);

  std::array<char, 4> sentinel = {'X', 'Y', 'Z', 'W'};
  EXPECT_EQ(H5Fget_name(f1, sentinel.data(), 0), len);
  EXPECT_EQ(sentinel[0], 'X');

  std::array<char, 6> truncated{};
  EXPECT_EQ(H5Fget_name(f1, truncated.data(), truncated.size()), len);
  EXPECT_STREQ(truncated.data(), "alpha");

  // H5Fget_info2 across all object types.
  H5F_info2_t finfo{};
  EXPECT_GE(H5Fget_info2(f1, &finfo), 0);
  EXPECT_GE(H5Fget_info2(objs.group, &finfo), 0);
  EXPECT_GE(H5Fget_info2(objs.dset, &finfo), 0);
  EXPECT_GE(H5Fget_info2(objs.dtype, &finfo), 0);
  EXPECT_GE(H5Fget_info2(objs.attr, &finfo), 0);

  // H5Fget/set_dset_no_attrs_hint.
  bool minimize = true;
  ASSERT_GE(H5Fget_dset_no_attrs_hint(f1, &minimize), 0);
  EXPECT_FALSE(minimize);
  ASSERT_GE(H5Fset_dset_no_attrs_hint(f1, true), 0);
  ASSERT_GE(H5Fget_dset_no_attrs_hint(f1, &minimize), 0);
  EXPECT_TRUE(minimize);
  ASSERT_GE(H5Fset_dset_no_attrs_hint(f1, false), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Fget_intent(H5I_INVALID_HID, &intent), 0);
  EXPECT_LT(H5Fget_intent(objs.group, &intent), 0);
  EXPECT_LT(H5Fget_fileno(H5I_INVALID_HID, &fno1), 0);
  EXPECT_LT(H5Fget_fileno(objs.group, &fno1), 0);
  EXPECT_LT(H5Fget_name(H5I_INVALID_HID, name_buf.data(), name_buf.size()), 0);
  EXPECT_LT(H5Fget_name(objs.space, name_buf.data(), name_buf.size()), 0);
  EXPECT_LT(H5Fget_info2(f1, nullptr), 0);
  EXPECT_LT(H5Fget_info2(H5I_INVALID_HID, &finfo), 0);
  EXPECT_LT(H5Fget_info2(objs.space, &finfo), 0);
  EXPECT_LT(H5Fget_dset_no_attrs_hint(f1, nullptr), 0);
  EXPECT_LT(H5Fget_dset_no_attrs_hint(H5I_INVALID_HID, &minimize), 0);
  EXPECT_LT(H5Fset_dset_no_attrs_hint(H5I_INVALID_HID, true), 0);
}

TEST(H5FTest, FreespaceSectionsFilesizeFileImageAndEoa) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_file_space_strategy(fcpl, H5F_FSPACE_STRATEGY_FSM_AGGR,
                                       /*persist=*/true, /*threshold=*/1),
            0);

  ScopedFile mem = CreateInMemoryFile("freespace.h5", H5F_ACC_TRUNC, fcpl);
  ASSERT_TRUE(mem.valid());

  // Create temp_ds followed by anchor_ds so deleting temp_ds leaves an
  // interior hole tracked as a free-space section rather than shrinking EOA.
  {
    ScopedSpace space(H5Screate_simple(1, std::array<hsize_t, 1>{1024}.data(),
                                       nullptr));
    ScopedDataset ds(H5Dcreate2(mem, "temp_ds", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ds.valid());
    std::vector<int32_t> data(1024, 7);
    ASSERT_GE(
        H5Dwrite(ds, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                 data.data()),
        0);
    ScopedDataset anchor(H5Dcreate2(mem, "anchor_ds", H5T_NATIVE_INT32, space,
                                    H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(anchor.valid());
    ASSERT_GE(H5Dwrite(anchor, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       data.data()),
              0);
  }
  ASSERT_GE(H5Fflush(mem, H5F_SCOPE_GLOBAL), 0);
  ASSERT_GE(H5Ldelete(mem, "temp_ds", H5P_DEFAULT), 0);
  ASSERT_GE(H5Fflush(mem, H5F_SCOPE_GLOBAL), 0);

  EXPECT_GE(H5Fget_freespace(mem), 0);
  const ssize_t nsects =
      H5Fget_free_sections(mem, H5FD_MEM_DEFAULT, 0, nullptr);
  ASSERT_GT(nsects, 0);
  std::vector<H5F_sect_info_t> sects(static_cast<size_t>(nsects));
  EXPECT_EQ(
      H5Fget_free_sections(mem, H5FD_MEM_DEFAULT, sects.size(), sects.data()),
      nsects);

  hsize_t filesize = 0;
  ASSERT_GE(H5Fget_filesize(mem, &filesize), 0);
  EXPECT_GT(filesize, 0);

  const ssize_t image_len = H5Fget_file_image(mem, nullptr, 0);
  ASSERT_GT(image_len, 8);
  std::vector<char> image(static_cast<size_t>(image_len));
  EXPECT_EQ(H5Fget_file_image(mem, image.data(), image.size()), image_len);
  EXPECT_EQ(std::memcmp(image.data(), "\x89HDF\r\n\x1a\n", 8), 0);

  // H5Fget_eoa(file, nullptr) hits the NULL guard and succeeds on any valid ID;
  // Core VFD rejects non-NULL H5Fget_eoa because it lacks SWMR support.
  haddr_t eoa = HADDR_UNDEF;
  ASSERT_GE(H5Fget_eoa(mem, nullptr), 0);

  TempFile sec2_path("eoa_incr.h5");
  ScopedPlist fapl = MakeSec2Fapl();
  ASSERT_TRUE(fapl.valid());
  ScopedFile sec2(
      H5Fcreate(sec2_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(sec2.valid());
  haddr_t eoa_before = 0;
  haddr_t eoa_after = 0;
  ASSERT_GE(H5Fget_eoa(sec2, &eoa_before), 0);
  EXPECT_NE(eoa_before, HADDR_UNDEF);
  ASSERT_GE(H5Fincrement_filesize(sec2, 4096), 0);
  ASSERT_GE(H5Fget_eoa(sec2, &eoa_after), 0);
  EXPECT_GE(eoa_after, eoa_before + 4096);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Fget_eoa(mem, &eoa), 0);
  EXPECT_LT(H5Fget_freespace(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Fget_filesize(mem, nullptr), 0);
  EXPECT_LT(H5Fget_filesize(H5I_INVALID_HID, &filesize), 0);
  EXPECT_LT(H5Fget_file_image(H5I_INVALID_HID, nullptr, 0), 0);
  std::array<char, 4> tiny{};
  EXPECT_LT(H5Fget_file_image(mem, tiny.data(), tiny.size()), 0);
  EXPECT_LT(H5Fget_eoa(H5I_INVALID_HID, &eoa), 0);
  EXPECT_LT(H5Fincrement_filesize(H5I_INVALID_HID, 1024), 0);
  EXPECT_LT(H5Fincrement_filesize(mem, 1024), 0);
  EXPECT_LT(H5Fget_free_sections(H5I_INVALID_HID, H5FD_MEM_DEFAULT, 0, nullptr),
            0);
  EXPECT_LT(H5Fget_free_sections(mem, H5FD_MEM_DEFAULT, 0, sects.data()), 0);
}

TEST(H5FTest, MetadataCacheConfigStatsLoggingAndImageInfo) {
  ScopedFile mem = CreateInMemoryFile("mdc_test.h5");
  ASSERT_TRUE(mem.valid());

  H5AC_cache_config_t cfg{};
  cfg.version = H5AC__CURR_CACHE_CONFIG_VERSION;
  ASSERT_GE(H5Fget_mdc_config(mem, &cfg), 0);
  cfg.set_initial_size = true;
  cfg.initial_size = 2 * 1024 * 1024;
  ASSERT_GE(H5Fset_mdc_config(mem, &cfg), 0);

  double hit_rate = -1.0;
  ASSERT_GE(H5Fget_mdc_hit_rate(mem, &hit_rate), 0);
  EXPECT_GE(hit_rate, 0.0);

  size_t max_sz = 0;
  size_t min_clean_sz = 0;
  size_t cur_sz = 0;
  int cur_entries = -1;
  ASSERT_GE(H5Fget_mdc_size(mem, &max_sz, &min_clean_sz, &cur_sz, &cur_entries),
            0);
  EXPECT_GT(max_sz, 0);
  EXPECT_GE(cur_entries, 0);
  ASSERT_GE(H5Fget_mdc_size(mem, nullptr, nullptr, nullptr, nullptr), 0);

  ASSERT_GE(H5Freset_mdc_hit_rate_stats(mem), 0);

  // Cache image absent case, and generated cache image present case.
  haddr_t img_addr = 0;
  hsize_t img_len = 999;
  ASSERT_GE(H5Fget_mdc_image_info(mem, &img_addr, &img_len), 0);
  EXPECT_EQ(img_addr, HADDR_UNDEF);
  EXPECT_EQ(img_len, 0);

  TempFile img_file("mdc_image.h5");
  {
    ScopedPlist fapl = MakeSec2Fapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(fapl, H5F_LIBVER_V110, H5F_LIBVER_LATEST),
              0);
    H5AC_cache_image_config_t icfg{};
    icfg.version = H5AC__CURR_CACHE_IMAGE_CONFIG_VERSION;
    icfg.generate_image = true;
    icfg.save_resize_status = false;
    icfg.entry_ageout = H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE;
    ASSERT_GE(H5Pset_mdc_image_config(fapl, &icfg), 0);
    ScopedFile f(
        H5Fcreate(img_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(f.valid());
    ScopedGroup g(H5Gcreate2(f, "g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    EXPECT_TRUE(g.valid());
  }
  {
    ScopedPlist fapl = MakeSec2Fapl();
    ScopedFile f(H5Fopen(img_file.c_str(), H5F_ACC_RDONLY, fapl));
    ASSERT_TRUE(f.valid());
    haddr_t addr2 = HADDR_UNDEF;
    hsize_t len2 = 0;
    ASSERT_GE(H5Fget_mdc_image_info(f, &addr2, &len2), 0);
    EXPECT_NE(addr2, HADDR_UNDEF);
    EXPECT_GT(len2, 0);
  }

  // MDC logging without options enabled, then enabled on Sec2 + TempFile log.
  bool enabled = true;
  bool active = true;
  ASSERT_GE(H5Fget_mdc_logging_status(mem, &enabled, &active), 0);
  EXPECT_FALSE(enabled);
  EXPECT_FALSE(active);

  TempFile data_path("mdc_log_data.h5");
  TempFile log_path("mdc_events.log");
  {
    ScopedPlist fapl = MakeSec2Fapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_mdc_log_options(fapl, /*is_enabled=*/true,
                                     log_path.c_str(), /*start_on_access=*/false),
              0);
    ScopedFile f(
        H5Fcreate(data_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(f.valid());

    ASSERT_GE(H5Fget_mdc_logging_status(f, &enabled, &active), 0);
    EXPECT_TRUE(enabled);
    EXPECT_FALSE(active);

    ASSERT_GE(H5Fstart_mdc_logging(f), 0);
    ASSERT_GE(H5Fget_mdc_logging_status(f, &enabled, &active), 0);
    EXPECT_TRUE(active);
    ScopedGroup g(H5Gcreate2(f, "logged_grp", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
    EXPECT_TRUE(g.valid());
    ASSERT_GE(H5Fstop_mdc_logging(f), 0);
    ASSERT_GE(H5Fget_mdc_logging_status(f, &enabled, &active), 0);
    EXPECT_FALSE(active);
  }

  ScopedErrorSilence silence;
  H5AC_cache_config_t bad_cfg = cfg;
  bad_cfg.version = 0;
  EXPECT_LT(H5Fget_mdc_config(mem, nullptr), 0);
  EXPECT_LT(H5Fget_mdc_config(mem, &bad_cfg), 0);
  EXPECT_LT(H5Fget_mdc_config(H5I_INVALID_HID, &cfg), 0);
  EXPECT_LT(H5Fset_mdc_config(H5I_INVALID_HID, &cfg), 0);
  EXPECT_LT(H5Fset_mdc_config(mem, &bad_cfg), 0);
  EXPECT_LT(H5Fget_mdc_hit_rate(mem, nullptr), 0);
  EXPECT_LT(H5Fget_mdc_hit_rate(H5I_INVALID_HID, &hit_rate), 0);
  EXPECT_LT(H5Fget_mdc_size(H5I_INVALID_HID, &max_sz, nullptr, nullptr, nullptr),
            0);
  EXPECT_LT(H5Freset_mdc_hit_rate_stats(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Fget_mdc_image_info(H5I_INVALID_HID, &img_addr, &img_len), 0);
  EXPECT_LT(H5Fstart_mdc_logging(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Fstart_mdc_logging(mem), 0);
  EXPECT_LT(H5Fstop_mdc_logging(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Fstop_mdc_logging(mem), 0);
  EXPECT_LT(H5Fget_mdc_logging_status(H5I_INVALID_HID, &enabled, &active), 0);
}

TEST(H5FTest, LibverFormatConvertSwmrRetryAndElinkCache) {
  // 1. H5Fset_libver_bounds and H5Fformat_convert.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl, H5F_LIBVER_V110, H5F_LIBVER_LATEST), 0);
  ScopedFile mem(H5Fcreate("libver.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(mem.valid());

  H5F_info2_t info{};
  ASSERT_GE(H5Fget_info2(mem, &info), 0);
  EXPECT_GE(info.super.version, 3u);

  ASSERT_GE(H5Fset_libver_bounds(mem, H5F_LIBVER_V18, H5F_LIBVER_V114), 0);
  ASSERT_GE(H5Fformat_convert(mem), 0);
  ASSERT_GE(H5Fget_info2(mem, &info), 0);
  EXPECT_EQ(info.super.version, 2u);

  // 2. H5Fstart_swmr_write and H5Fget_metadata_read_retry_info on Sec2.
  TempFile swmr_path("swmr_start.h5");
  ScopedPlist swmr_fapl = MakeSec2Fapl();
  ASSERT_TRUE(swmr_fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(swmr_fapl, H5F_LIBVER_V110, H5F_LIBVER_LATEST), 0);
  ASSERT_GE(H5Pset_metadata_read_attempts(swmr_fapl, 3), 0);
  {
    ScopedFile swmr(H5Fcreate(swmr_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                              swmr_fapl));
    ASSERT_TRUE(swmr.valid());
    EXPECT_GE(H5Fstart_swmr_write(swmr), 0);

    H5F_retry_info_t retry_info{};
    ASSERT_GE(H5Fget_metadata_read_retry_info(swmr, &retry_info), 0);
    for (uint32_t* p : retry_info.retries) {
      if (p != nullptr) {
        H5free_memory(p);
      }
    }
  }

  // 3. H5Fclear_elink_file_cache with Sec2 files in TempDir.
  TempDir elink_dir("elink_cache_dir");
  const std::string parent_path = elink_dir.child("parent.h5").native();
  const std::string child_path = elink_dir.child("child.h5").native();
  ScopedPlist sec2_fapl = MakeSec2Fapl();
  ASSERT_GE(H5Pset_elink_file_cache_size(sec2_fapl, 4), 0);
  {
    ScopedFile c(
        H5Fcreate(child_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, sec2_fapl));
    ASSERT_TRUE(c.valid());
    ScopedGroup cg(H5Gcreate2(c, "target", H5P_DEFAULT, H5P_DEFAULT,
                              H5P_DEFAULT));
    EXPECT_TRUE(cg.valid());
  }
  {
    ScopedFile p(
        H5Fcreate(parent_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, sec2_fapl));
    ASSERT_TRUE(p.valid());
    ASSERT_GE(H5Lcreate_external(child_path.c_str(), "/target", p, "ext_target",
                                 H5P_DEFAULT, H5P_DEFAULT),
              0);
    {
      ScopedObject ext(H5Oopen(p, "ext_target", H5P_DEFAULT));
      ASSERT_TRUE(ext.valid());
    }
    EXPECT_GE(H5Fclear_elink_file_cache(p), 0);
  }

  ScopedErrorSilence silence;
  EXPECT_LT(H5Fset_libver_bounds(H5I_INVALID_HID, H5F_LIBVER_EARLIEST,
                                 H5F_LIBVER_LATEST),
            0);
  EXPECT_LT(H5Fset_libver_bounds(fapl, H5F_LIBVER_V18, H5F_LIBVER_V110), 0);
  EXPECT_LT(H5Fformat_convert(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Fstart_swmr_write(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Fstart_swmr_write(mem), 0);  // Core VFD does not support SWMR
  EXPECT_LT(H5Fget_metadata_read_retry_info(mem, nullptr), 0);
  H5F_retry_info_t retry_info{};
  EXPECT_LT(
      H5Fget_metadata_read_retry_info(H5I_INVALID_HID, &retry_info), 0);
  EXPECT_LT(H5Fclear_elink_file_cache(H5I_INVALID_HID), 0);
}

TEST(H5FTest, PageBufferingStats) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fcpl.valid());
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl, H5F_LIBVER_V110, H5F_LIBVER_LATEST), 0);
  ASSERT_GE(H5Pset_file_space_strategy(fcpl, H5F_FSPACE_STRATEGY_PAGE, true, 1),
            0);
  ASSERT_GE(H5Pset_page_buffer_size(fapl, 4096, 50, 0), 0);

  ScopedFile pb_file(
      H5Fcreate("page_buf.h5", H5F_ACC_TRUNC, fcpl, fapl));
  ASSERT_TRUE(pb_file.valid());

  ScopedGroup g(
      H5Gcreate2(pb_file, "pg", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(g.valid());

  unsigned accesses[2]{};
  unsigned hits[2]{};
  unsigned misses[2]{};
  unsigned evictions[2]{};
  unsigned bypasses[2]{};
  ASSERT_GE(H5Fget_page_buffering_stats(pb_file, accesses, hits, misses,
                                        evictions, bypasses),
            0);
  ASSERT_GE(H5Freset_page_buffering_stats(pb_file), 0);
  ASSERT_GE(H5Fget_page_buffering_stats(pb_file, accesses, hits, misses,
                                        evictions, bypasses),
            0);
  EXPECT_EQ(accesses[0], 0u);
  EXPECT_EQ(accesses[1], 0u);

  ScopedFile no_pb = CreateInMemoryFile("no_pb.h5");
  ASSERT_TRUE(no_pb.valid());

  ScopedErrorSilence silence;
  EXPECT_LT(H5Freset_page_buffering_stats(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Freset_page_buffering_stats(no_pb), 0);
  EXPECT_LT(H5Fget_page_buffering_stats(H5I_INVALID_HID, accesses, hits, misses,
                                        evictions, bypasses),
            0);
  EXPECT_LT(H5Fget_page_buffering_stats(pb_file, nullptr, hits, misses,
                                        evictions, bypasses),
            0);
  EXPECT_LT(H5Fget_page_buffering_stats(pb_file, accesses, nullptr, misses,
                                        evictions, bypasses),
            0);
  EXPECT_LT(H5Fget_page_buffering_stats(pb_file, accesses, hits, nullptr,
                                        evictions, bypasses),
            0);
  EXPECT_LT(H5Fget_page_buffering_stats(pb_file, accesses, hits, misses,
                                        nullptr, bypasses),
            0);
  EXPECT_LT(H5Fget_page_buffering_stats(pb_file, accesses, hits, misses,
                                        evictions, nullptr),
            0);
  EXPECT_LT(H5Fget_page_buffering_stats(no_pb, accesses, hits, misses,
                                        evictions, bypasses),
            0);
}

TEST(H5FTest, VolCreateOpenReopenSyncAndPostOpenFailures) {
  ScopedTestVol vol;
  ScopedPlist fapl = vol.MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());

  ScopedErrorSilence silence;
  const std::vector<hid_t> before = GetOpenFileIds();

  // VOL file_create and file_open returning NULL.
  g_vol_cfg.fail_file_create = true;
  EXPECT_EQ(H5Fcreate("v_fail_c.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Fcreate_async("v_fail_ca.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl,
                            H5ES_NONE),
            H5I_INVALID_HID);
  g_vol_cfg.fail_file_create = false;

  g_vol_cfg.fail_file_open = true;
  EXPECT_EQ(H5Fopen("v_fail_o.h5", H5F_ACC_RDONLY, fapl), H5I_INVALID_HID);
  EXPECT_EQ(H5Fopen_async("v_fail_oa.h5", H5F_ACC_RDONLY, fapl, H5ES_NONE),
            H5I_INVALID_HID);
  g_vol_cfg.fail_file_open = false;

  // Create a valid file through TestVol (Sec2 so we can reopen by name too).
  TempFile sec2_path("vol_post_open.h5");
  ScopedPlist sec2_fapl = vol.MakeSec2Fapl();
  ASSERT_TRUE(sec2_fapl.valid());
  {
    ScopedFile f(H5Fcreate(sec2_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                           sec2_fapl));
    ASSERT_TRUE(f.valid());
  }

  // opt_query failure inside H5F__post_open_api_common during create/open/reopen.
  g_vol_cfg.fail_opt_query = true;
  EXPECT_EQ(H5Fcreate("v_oq_c.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl),
            H5I_INVALID_HID);
  CloseNewFileIds(before);
  EXPECT_EQ(H5Fopen(sec2_path.c_str(), H5F_ACC_RDONLY, sec2_fapl),
            H5I_INVALID_HID);
  CloseNewFileIds(before);
  g_vol_cfg.fail_opt_query = false;

  // post_open optional failure on create, open, reopen (sync & async).
  g_vol_cfg.fail_post_open = true;
  EXPECT_EQ(H5Fcreate("v_po_c.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl),
            H5I_INVALID_HID);
  CloseNewFileIds(before);
  EXPECT_EQ(H5Fcreate_async("v_po_ca.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl,
                            H5ES_NONE),
            H5I_INVALID_HID);
  CloseNewFileIds(before);
  EXPECT_EQ(H5Fopen(sec2_path.c_str(), H5F_ACC_RDONLY, sec2_fapl),
            H5I_INVALID_HID);
  CloseNewFileIds(before);
  EXPECT_EQ(
      H5Fopen_async(sec2_path.c_str(), H5F_ACC_RDONLY, sec2_fapl, H5ES_NONE),
      H5I_INVALID_HID);
  CloseNewFileIds(before);
  g_vol_cfg.fail_post_open = false;

  // Reopen with post_open failure (sync & async) and reopen returning NULL.
  ScopedFile open_f(H5Fopen(sec2_path.c_str(), H5F_ACC_RDWR, sec2_fapl));
  ASSERT_TRUE(open_f.valid());
  const std::vector<hid_t> before_reopen = GetOpenFileIds();

  g_vol_cfg.fail_post_open = true;
  EXPECT_EQ(H5Freopen(open_f), H5I_INVALID_HID);
  CloseNewFileIds(before_reopen);
  EXPECT_EQ(H5Freopen_async(open_f, H5ES_NONE), H5I_INVALID_HID);
  CloseNewFileIds(before_reopen);
  g_vol_cfg.fail_post_open = false;

  g_vol_cfg.fail_file_reopen_null = true;
  EXPECT_EQ(H5Freopen(open_f), H5I_INVALID_HID);
  g_vol_cfg.fail_file_reopen_null = false;
}

TEST(H5FTest, VolAsyncTokensAndEventSetInsertFailures) {
  ScopedTestVol vol;
  TempFile path("vol_async_tokens.h5");
  ScopedPlist fapl = vol.MakeSec2Fapl();
  ASSERT_TRUE(fapl.valid());

  // 1. Successful async create/reopen/open/flush producing synthetic tokens.
  {
    ScopedEventSet es(H5EScreate());
    ASSERT_TRUE(es.valid());

    g_vol_cfg.return_req_on_file_create = true;
    g_vol_cfg.return_req_on_file_open = true;
    g_vol_cfg.return_req_on_file_reopen = true;
    g_vol_cfg.return_req_on_post_open = true;
    g_vol_cfg.return_req_on_file_flush = true;

    ScopedFile f(
        H5Fcreate_async(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl, es));
    ASSERT_TRUE(f.valid());
    ScopedFile r(H5Freopen_async(f, es));
    ASSERT_TRUE(r.valid());
    ASSERT_GE(H5Fflush_async(f, H5F_SCOPE_LOCAL, es), 0);

    ScopedFile o(
        H5Fopen_async(path.c_str(), H5F_ACC_RDONLY, fapl, es));
    ASSERT_TRUE(o.valid());

    size_t count = 0;
    ASSERT_GE(H5ESget_count(es, &count), 0);
    EXPECT_EQ(count, 7u);  // create+po, reopen+po, flush, open+po

    size_t in_prog = 99;
    bool err_occurred = true;
    ASSERT_GE(H5ESwait(es, H5ES_WAIT_FOREVER, &in_prog, &err_occurred), 0);
    EXPECT_EQ(in_prog, 0u);
    EXPECT_FALSE(err_occurred);

#if !H5F_TEST_HAS_ASAN
    // H5F.c:1095 reads H5VL_OBJ_CONNECTOR(vol_obj) after H5I_dec_app_ref_async
    // frees vol_obj when closing the final file reference with a non-NULL
    // async request token; keep enabled outside ASan to cover line 1095-1098.
    g_vol_cfg.return_req_on_file_close = true;
    ASSERT_GE(H5Fclose_async(o.release(), es), 0);
    ASSERT_GE(H5Fclose_async(r.release(), es), 0);
    ASSERT_GE(H5Fclose_async(f.release(), es), 0);
    ASSERT_GE(H5ESwait(es, H5ES_WAIT_FOREVER, &in_prog, &err_occurred), 0);
#endif
  }

  g_vol_cfg.Reset();

  // 2. Event-set insert failures on 1st insert (plus H5I_dec_app_ref failure)
  //    and on 2nd insert (post_open token).
  struct InsertLimiter {
    int fail_on_call = 1;
    int calls = 0;
  };
  auto insert_cb = [](const H5ES_op_info_t* /*op_info*/, void* raw_ctx) -> int {
    auto* lim = static_cast<InsertLimiter*>(raw_ctx);
    ++lim->calls;
    return (lim->calls >= lim->fail_on_call) ? -1 : 0;
  };

  ScopedErrorSilence silence;
  const std::vector<hid_t> baseline = GetOpenFileIds();

  // Helper to run 1st-insert fail with and without fail_file_close, and 2nd
  // insert failure on create/open/reopen.
  {
    g_vol_cfg.return_req_on_file_create = true;
    g_vol_cfg.return_req_on_post_open = true;

    // 1st insert failure on H5Fcreate_async.
    {
      ScopedEventSet es(H5EScreate());
      InsertLimiter lim{1, 0};
      ASSERT_GE(H5ESregister_insert_func(es, +insert_cb, &lim), 0);
      EXPECT_EQ(
          H5Fcreate_async(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl, es),
          H5I_INVALID_HID);

      lim = {1, 0};
      g_vol_cfg.fail_file_close = true;
      EXPECT_EQ(
          H5Fcreate_async(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl, es),
          H5I_INVALID_HID);
      g_vol_cfg.fail_file_close = false;
      CloseNewFileIds(baseline);
    }

    // 2nd insert failure (post_open) on H5Fcreate_async.
    {
      ScopedEventSet es(H5EScreate());
      InsertLimiter lim{2, 0};
      ASSERT_GE(H5ESregister_insert_func(es, +insert_cb, &lim), 0);
      EXPECT_EQ(
          H5Fcreate_async(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl, es),
          H5I_INVALID_HID);
      CloseNewFileIds(baseline);
      size_t in_prog = 0;
      bool err = false;
      H5ESwait(es, H5ES_WAIT_FOREVER, &in_prog, &err);
    }
  }

  g_vol_cfg.Reset();

  // Create valid file for open/reopen/flush async error tests.
  {
    ScopedFile f(
        H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(f.valid());
  }

  // Open & reopen insert failures (1st insert w/ and w/o close fail + 2nd).
  g_vol_cfg.return_req_on_file_open = true;
  g_vol_cfg.return_req_on_file_reopen = true;
  g_vol_cfg.return_req_on_post_open = true;
  g_vol_cfg.return_req_on_file_flush = true;
  {
    ScopedEventSet es(H5EScreate());
    InsertLimiter lim{1, 0};
    ASSERT_GE(H5ESregister_insert_func(es, +insert_cb, &lim), 0);
    EXPECT_EQ(H5Fopen_async(path.c_str(), H5F_ACC_RDONLY, fapl, es),
              H5I_INVALID_HID);

    lim = {1, 0};
    g_vol_cfg.fail_file_close = true;
    EXPECT_EQ(H5Fopen_async(path.c_str(), H5F_ACC_RDONLY, fapl, es),
              H5I_INVALID_HID);
    g_vol_cfg.fail_file_close = false;
    CloseNewFileIds(baseline);

    lim = {2, 0};
    EXPECT_EQ(H5Fopen_async(path.c_str(), H5F_ACC_RDONLY, fapl, es),
              H5I_INVALID_HID);
    CloseNewFileIds(baseline);
    size_t in_prog = 0;
    bool err = false;
    H5ESwait(es, H5ES_WAIT_FOREVER, &in_prog, &err);
  }

  {
    ScopedFile base(H5Fopen(path.c_str(), H5F_ACC_RDWR, fapl));
    ASSERT_TRUE(base.valid());
    const std::vector<hid_t> with_base = GetOpenFileIds();

    ScopedEventSet es(H5EScreate());
    InsertLimiter lim{1, 0};
    ASSERT_GE(H5ESregister_insert_func(es, +insert_cb, &lim), 0);

    EXPECT_EQ(H5Freopen_async(base, es), H5I_INVALID_HID);

    lim = {1, 0};
    g_vol_cfg.fail_file_close = true;
    EXPECT_EQ(H5Freopen_async(base, es), H5I_INVALID_HID);
    g_vol_cfg.fail_file_close = false;
    CloseNewFileIds(with_base);

    lim = {2, 0};
    EXPECT_EQ(H5Freopen_async(base, es), H5I_INVALID_HID);
    CloseNewFileIds(with_base);

    lim = {1, 0};
    EXPECT_LT(H5Fflush_async(base, H5F_SCOPE_LOCAL, es), 0);

#if !H5F_TEST_HAS_ASAN
    {
      TempFile close_err_path("vol_close_err.h5");
      ScopedFile to_close(H5Fcreate(close_err_path.c_str(), H5F_ACC_TRUNC,
                                    H5P_DEFAULT, fapl));
      ASSERT_TRUE(to_close.valid());
      lim = {1, 0};
      g_vol_cfg.return_req_on_file_close = true;
      EXPECT_LT(H5Fclose_async(to_close.release(), es), 0);
      g_vol_cfg.return_req_on_file_close = false;
    }
#endif

    size_t in_prog = 0;
    bool err = false;
    H5ESwait(es, H5ES_WAIT_FOREVER, &in_prog, &err);
  }
}

TEST(H5FTest, VolMountGetSpecificOptionalAndCloseFailures) {
  ScopedTestVol vol;
  ScopedPlist fapl = vol.MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());

  ScopedFile f(H5Fcreate("vol_errs.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ScopedFile child(H5Fcreate("vol_child.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ScopedFile native_child = CreateInMemoryFile("native_child.h5");
  ASSERT_TRUE(f.valid());
  ASSERT_TRUE(child.valid());
  ASSERT_TRUE(native_child.valid());

  ScopedGroup mnt(H5Gcreate2(f, "mnt", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(mnt.valid());

  ScopedErrorSilence silence;

  // Cross-VOL mount rejection + group_open / group_specific / group_close fails.
  EXPECT_LT(H5Fmount(f, "/mnt", native_child, H5P_DEFAULT), 0);

  g_vol_cfg.fail_group_open = true;
  EXPECT_LT(H5Fmount(f, "/mnt", child, H5P_DEFAULT), 0);
  EXPECT_LT(H5Funmount(f, "/mnt"), 0);
  g_vol_cfg.fail_group_open = false;

  g_vol_cfg.fail_group_specific = true;
  EXPECT_LT(H5Fmount(f, "/mnt", child, H5P_DEFAULT), 0);
  EXPECT_LT(H5Funmount(f, "/mnt"), 0);
  g_vol_cfg.fail_group_specific = false;

  g_vol_cfg.fail_group_close = true;
  // Mount/unmount succeed internally, then HDONE_ERROR turns the close failure
  // in done: into a negative return value.
  EXPECT_LT(H5Fmount(f, "/mnt", child, H5P_DEFAULT), 0);
  EXPECT_LT(H5Funmount(f, "/mnt"), 0);
  g_vol_cfg.fail_group_close = false;
  EXPECT_LT(H5Funmount(f, "/mnt"), 0);  // Already unmounted above.

  // file_get VOL failure across all H5VL_file_get wrappers in H5F.c.
  g_vol_cfg.fail_file_get = true;
  EXPECT_EQ(H5Fget_create_plist(f), H5I_INVALID_HID);
  EXPECT_EQ(H5Fget_access_plist(f), H5I_INVALID_HID);
  EXPECT_LT(H5Fget_obj_count(f, H5F_OBJ_ALL), 0);
  hid_t dummy_id = H5I_INVALID_HID;
  EXPECT_LT(H5Fget_obj_ids(f, H5F_OBJ_ALL, 1, &dummy_id), 0);
  unsigned intent = 0;
  EXPECT_LT(H5Fget_intent(f, &intent), 0);
  unsigned long fileno = 0;
  EXPECT_LT(H5Fget_fileno(f, &fileno), 0);
  char name_buf[32]{};
  EXPECT_LT(H5Fget_name(f, name_buf, sizeof(name_buf)), 0);
  g_vol_cfg.fail_file_get = false;

  // file_specific VOL failures.
  g_vol_cfg.fail_file_is_accessible = true;
  EXPECT_LT(H5Fis_accessible("vol_errs.h5", fapl), 0);
  EXPECT_LT(H5Fdelete("vol_errs.h5", fapl), 0);
  g_vol_cfg.fail_file_is_accessible = false;

  TempFile sec2_del("vol_del.h5");
  ScopedPlist sec2_fapl = vol.MakeSec2Fapl();
  {
    ScopedFile d(
        H5Fcreate(sec2_del.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, sec2_fapl));
    ASSERT_TRUE(d.valid());
  }
  g_vol_cfg.fail_file_delete = true;
  EXPECT_LT(H5Fdelete(sec2_del.c_str(), sec2_fapl), 0);
  g_vol_cfg.fail_file_delete = false;
  EXPECT_GE(H5Fdelete(sec2_del.c_str(), sec2_fapl), 0);

  g_vol_cfg.fail_file_specific = true;
  EXPECT_LT(H5Fflush(f, H5F_SCOPE_LOCAL), 0);
  EXPECT_EQ(H5Freopen(f), H5I_INVALID_HID);
  g_vol_cfg.fail_file_specific = false;

  // file_optional VOL failure across all H5VL_file_optional wrappers in H5F.c.
  g_vol_cfg.fail_file_optional = true;
  void* handle = nullptr;
  EXPECT_LT(H5Fget_vfd_handle(f, H5P_DEFAULT, &handle), 0);
  EXPECT_LT(H5Fget_freespace(f), 0);
  hsize_t fsz = 0;
  EXPECT_LT(H5Fget_filesize(f, &fsz), 0);
  EXPECT_LT(H5Fget_file_image(f, nullptr, 0), 0);
  H5AC_cache_config_t cfg{};
  cfg.version = H5AC__CURR_CACHE_CONFIG_VERSION;
  EXPECT_LT(H5Fget_mdc_config(f, &cfg), 0);
  EXPECT_LT(H5Fset_mdc_config(f, &cfg), 0);
  double hr = 0.0;
  EXPECT_LT(H5Fget_mdc_hit_rate(f, &hr), 0);
  size_t s1 = 0;
  EXPECT_LT(H5Fget_mdc_size(f, &s1, nullptr, nullptr, nullptr), 0);
  EXPECT_LT(H5Freset_mdc_hit_rate_stats(f), 0);
  H5F_info2_t finfo{};
  EXPECT_LT(H5Fget_info2(f, &finfo), 0);
  H5F_retry_info_t rinfo{};
  EXPECT_LT(H5Fget_metadata_read_retry_info(f, &rinfo), 0);
  EXPECT_LT(H5Fget_free_sections(f, H5FD_MEM_DEFAULT, 0, nullptr), 0);
  EXPECT_LT(H5Fclear_elink_file_cache(f), 0);
  EXPECT_LT(H5Fstart_swmr_write(f), 0);
  EXPECT_LT(H5Fstart_mdc_logging(f), 0);
  EXPECT_LT(H5Fstop_mdc_logging(f), 0);
  bool b1 = false;
  bool b2 = false;
  EXPECT_LT(H5Fget_mdc_logging_status(f, &b1, &b2), 0);
  EXPECT_LT(H5Fset_libver_bounds(f, H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);
  EXPECT_LT(H5Fformat_convert(f), 0);
  EXPECT_LT(H5Freset_page_buffering_stats(f), 0);
  unsigned arr[2]{};
  EXPECT_LT(H5Fget_page_buffering_stats(f, arr, arr, arr, arr, arr), 0);
  haddr_t addr = 0;
  EXPECT_LT(H5Fget_mdc_image_info(f, &addr, &fsz), 0);
  EXPECT_LT(H5Fget_eoa(f, &addr), 0);
  EXPECT_LT(H5Fincrement_filesize(f, 512), 0);
  EXPECT_LT(H5Fget_dset_no_attrs_hint(f, &b1), 0);
  EXPECT_LT(H5Fset_dset_no_attrs_hint(f, true), 0);
  g_vol_cfg.fail_file_optional = false;

  // file_close VOL failure on sync & async close, then recover cleanly.
  mnt.reset();
  ScopedEventSet es(H5EScreate());
  ASSERT_TRUE(es.valid());
  g_vol_cfg.fail_file_close = true;
  EXPECT_LT(H5Fclose(f), 0);
  EXPECT_LT(H5Fclose_async(f, es), 0);
  g_vol_cfg.fail_file_close = false;
  EXPECT_GE(H5Fclose(f.release()), 0);
}

}  // namespace
}  // namespace hdf5
