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
// Unit tests for HDF5 dataset creation, opening, I/O, scatter/gather/fill/
// iteration, vlen sizing, chunk inspection/conversion, error handling, and
// asynchronous VOL + event-set integration (third_party/hdf5/src/H5D.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5D.c

#include "third_party/hdf5/src/hdf5.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

// ---------------------------------------------------------------------------
// Minimal asynchronous pass-through VOL connector over H5VL_NATIVE.
// Emits synthetic request tokens whenever non-NULL request pointers are
// provided, and supports selective fault/capability injection for testing
// H5D.c async rollback, VOL failure handling, and generic vlen sizing.
// ---------------------------------------------------------------------------

struct MockVolObj {
  hid_t under_vol_id;
  void* under_object;
  bool is_request = false;
  int req_id = 0;
};

struct MockVolState {
  ~MockVolState() {
    for (MockVolObj* req : live_tokens) {
      delete req;
    }
    live_tokens.clear();
  }

  MockVolObj* AllocToken() {
    auto* tok = new MockVolObj{H5VL_NATIVE, nullptr, true, ++next_id};
    live_tokens.insert(tok);
    return tok;
  }

  void FreeToken(MockVolObj* tok) {
    if (tok != nullptr && live_tokens.erase(tok) > 0) {
      delete tok;
    }
  }

  int next_id = 0;
  std::unordered_set<MockVolObj*> live_tokens;
  bool fail_next_dset_create = false;
  bool fail_next_dset_open = false;
  bool fail_next_dset_read = false;
  bool fail_next_dset_write = false;
  bool fail_next_dset_get = false;
  bool fail_next_dset_specific = false;
  bool fail_next_dset_optional = false;
  bool fail_next_dset_close = false;
  bool fail_next_opt_query = false;
  bool force_vlen_generic_path = false;
};

struct MockVolInfo {
  hid_t under_vol_id;
  void* under_vol_info;
};

struct MockVolWrapCtx {
  hid_t under_vol_id;
  void* under_wrap_ctx;
};

MockVolState*& ActiveMockVolState() {
  static MockVolState* state = nullptr;
  return state;
}

MockVolObj* WrapObj(void* under_obj, hid_t under_vol_id) {
  auto* o = new MockVolObj{under_vol_id, under_obj};
  H5Iinc_ref(o->under_vol_id);
  return o;
}

void FreeObj(MockVolObj* o) {
  if (o == nullptr) {
    return;
  }
  hid_t err = H5Eget_current_stack();
  H5Idec_ref(o->under_vol_id);
  H5Eset_current_stack(err);
  delete o;
}

void* MaybeEmitToken(void** req) {
  if (req != nullptr && ActiveMockVolState() != nullptr) {
    *req = ActiveMockVolState()->AllocToken();
  }
  return req ? *req : nullptr;
}

herr_t MockInit(hid_t /*vipl_id*/) { return 0; }
herr_t MockTerm() { return 0; }

void* MockInfoCopy(const void* _info) {
  const auto* info = static_cast<const MockVolInfo*>(_info);
  auto* copy = new MockVolInfo{info->under_vol_id, nullptr};
  H5Iinc_ref(copy->under_vol_id);
  if (info->under_vol_info != nullptr) {
    H5VLcopy_connector_info(copy->under_vol_id, &copy->under_vol_info,
                            info->under_vol_info);
  }
  return copy;
}

herr_t MockInfoCmp(int* cmp_value, const void* _i1, const void* _i2) {
  const auto* i1 = static_cast<const MockVolInfo*>(_i1);
  const auto* i2 = static_cast<const MockVolInfo*>(_i2);
  *cmp_value = 0;
  H5VLcmp_connector_cls(cmp_value, i1->under_vol_id, i2->under_vol_id);
  if (*cmp_value != 0) {
    return 0;
  }
  H5VLcmp_connector_info(cmp_value, i1->under_vol_id, i1->under_vol_info,
                         i2->under_vol_info);
  return 0;
}

herr_t MockInfoFree(void* _info) {
  auto* info = static_cast<MockVolInfo*>(_info);
  hid_t err = H5Eget_current_stack();
  if (info->under_vol_info != nullptr) {
    H5VLfree_connector_info(info->under_vol_id, info->under_vol_info);
  }
  H5Idec_ref(info->under_vol_id);
  H5Eset_current_stack(err);
  delete info;
  return 0;
}

void* MockWrapGetObject(const void* obj) {
  const auto* o = static_cast<const MockVolObj*>(obj);
  if (o->is_request || o->under_object == nullptr) {
    return nullptr;
  }
  return H5VLget_object(o->under_object, o->under_vol_id);
}

herr_t MockWrapGetCtx(const void* obj, void** wrap_ctx) {
  const auto* o = static_cast<const MockVolObj*>(obj);
  auto* ctx = new MockVolWrapCtx{o->under_vol_id, nullptr};
  H5Iinc_ref(ctx->under_vol_id);
  if (!o->is_request && o->under_object != nullptr) {
    H5VLget_wrap_ctx(o->under_object, o->under_vol_id, &ctx->under_wrap_ctx);
  }
  *wrap_ctx = ctx;
  return 0;
}

void* MockWrapObj(void* obj, H5I_type_t obj_type, void* _ctx) {
  auto* ctx = static_cast<MockVolWrapCtx*>(_ctx);
  void* under = H5VLwrap_object(obj, obj_type, ctx->under_vol_id,
                                ctx->under_wrap_ctx);
  return under ? WrapObj(under, ctx->under_vol_id) : nullptr;
}

void* MockUnwrapObj(void* obj) {
  auto* o = static_cast<MockVolObj*>(obj);
  void* under = H5VLunwrap_object(o->under_object, o->under_vol_id);
  if (under != nullptr) {
    FreeObj(o);
  }
  return under;
}

herr_t MockWrapFreeCtx(void* _ctx) {
  auto* ctx = static_cast<MockVolWrapCtx*>(_ctx);
  hid_t err = H5Eget_current_stack();
  if (ctx->under_wrap_ctx != nullptr) {
    H5VLfree_wrap_ctx(ctx->under_wrap_ctx, ctx->under_vol_id);
  }
  H5Idec_ref(ctx->under_vol_id);
  H5Eset_current_stack(err);
  delete ctx;
  return 0;
}

void* MockDatasetCreate(void* obj, const H5VL_loc_params_t* loc_params,
                        const char* name, hid_t lcpl_id, hid_t type_id,
                        hid_t space_id, hid_t dcpl_id, hid_t dapl_id,
                        hid_t dxpl_id, void** req) {
  if (ActiveMockVolState() != nullptr &&
      ActiveMockVolState()->fail_next_dset_create) {
    ActiveMockVolState()->fail_next_dset_create = false;
    return nullptr;
  }
  auto* o = static_cast<MockVolObj*>(obj);
  void* under = H5VLdataset_create(o->under_object, loc_params, o->under_vol_id,
                                   name, lcpl_id, type_id, space_id, dcpl_id,
                                   dapl_id, dxpl_id, nullptr);
  if (under == nullptr) {
    return nullptr;
  }
  MaybeEmitToken(req);
  return WrapObj(under, o->under_vol_id);
}

void* MockDatasetOpen(void* obj, const H5VL_loc_params_t* loc_params,
                      const char* name, hid_t dapl_id, hid_t dxpl_id,
                      void** req) {
  if (ActiveMockVolState() != nullptr &&
      ActiveMockVolState()->fail_next_dset_open) {
    ActiveMockVolState()->fail_next_dset_open = false;
    return nullptr;
  }
  auto* o = static_cast<MockVolObj*>(obj);
  void* under = H5VLdataset_open(o->under_object, loc_params, o->under_vol_id,
                                 name, dapl_id, dxpl_id, nullptr);
  if (under == nullptr) {
    return nullptr;
  }
  MaybeEmitToken(req);
  return WrapObj(under, o->under_vol_id);
}

herr_t MockDatasetRead(size_t count, void* dset[], hid_t mem_type_id[],
                       hid_t mem_space_id[], hid_t file_space_id[],
                       hid_t plist_id, void* buf[], void** req) {
  if (ActiveMockVolState() != nullptr &&
      ActiveMockVolState()->fail_next_dset_read) {
    ActiveMockVolState()->fail_next_dset_read = false;
    return -1;
  }
  // When simulating a non-native VOL connector that lacks
  // H5VL_NATIVE_DATASET_GET_VLEN_BUF_SIZE, H5D__vlen_get_buf_size_gen invokes
  // dataset read with a transient DXPL carrying a custom VL allocator. Fulfill
  // the VL allocation directly so we do not re-enter H5VL__native_dataset_read
  // with a transient DXPL that is freed before H5CX_pop().
  if (ActiveMockVolState() != nullptr &&
      ActiveMockVolState()->force_vlen_generic_path &&
      plist_id != H5P_DEFAULT && plist_id != H5P_DATASET_XFER_DEFAULT) {
    H5MM_allocate_t alloc_func = nullptr;
    void* alloc_info = nullptr;
    H5MM_free_t free_func = nullptr;
    void* free_info = nullptr;
    if (H5Pget_vlen_mem_manager(plist_id, &alloc_func, &alloc_info, &free_func,
                                &free_info) >= 0 &&
        alloc_func != nullptr) {
      alloc_func(2 * sizeof(int), alloc_info);
      MaybeEmitToken(req);
      return 0;
    }
  }
  std::vector<void*> under_objs(count);
  hid_t under_vol_id = static_cast<MockVolObj*>(dset[0])->under_vol_id;
  for (size_t i = 0; i < count; ++i) {
    auto* o = static_cast<MockVolObj*>(dset[i]);
    if (o->under_vol_id != under_vol_id) {
      return -1;
    }
    under_objs[i] = o->under_object;
  }
  herr_t rc = H5VLdataset_read(count, under_objs.data(), under_vol_id,
                               mem_type_id, mem_space_id, file_space_id,
                               plist_id, buf, nullptr);
  if (rc >= 0) {
    MaybeEmitToken(req);
  }
  return rc;
}

herr_t MockDatasetWrite(size_t count, void* dset[], hid_t mem_type_id[],
                        hid_t mem_space_id[], hid_t file_space_id[],
                        hid_t plist_id, const void* buf[], void** req) {
  if (ActiveMockVolState() != nullptr &&
      ActiveMockVolState()->fail_next_dset_write) {
    ActiveMockVolState()->fail_next_dset_write = false;
    return -1;
  }
  std::vector<void*> under_objs(count);
  hid_t under_vol_id = static_cast<MockVolObj*>(dset[0])->under_vol_id;
  for (size_t i = 0; i < count; ++i) {
    auto* o = static_cast<MockVolObj*>(dset[i]);
    if (o->under_vol_id != under_vol_id) {
      return -1;
    }
    under_objs[i] = o->under_object;
  }
  herr_t rc = H5VLdataset_write(count, under_objs.data(), under_vol_id,
                                mem_type_id, mem_space_id, file_space_id,
                                plist_id, buf, nullptr);
  if (rc >= 0) {
    MaybeEmitToken(req);
  }
  return rc;
}

herr_t MockDatasetGet(void* dset, H5VL_dataset_get_args_t* args, hid_t dxpl_id,
                      void** req) {
  if (ActiveMockVolState() != nullptr &&
      ActiveMockVolState()->fail_next_dset_get) {
    ActiveMockVolState()->fail_next_dset_get = false;
    return -1;
  }
  auto* o = static_cast<MockVolObj*>(dset);
  herr_t rc =
      H5VLdataset_get(o->under_object, o->under_vol_id, args, dxpl_id, nullptr);
  if (rc >= 0) {
    MaybeEmitToken(req);
  }
  return rc;
}

herr_t MockDatasetSpecific(void* obj, H5VL_dataset_specific_args_t* args,
                           hid_t dxpl_id, void** req) {
  if (ActiveMockVolState() != nullptr &&
      ActiveMockVolState()->fail_next_dset_specific) {
    ActiveMockVolState()->fail_next_dset_specific = false;
    return -1;
  }
  auto* o = static_cast<MockVolObj*>(obj);
  herr_t rc = H5VLdataset_specific(o->under_object, o->under_vol_id, args,
                                   dxpl_id, nullptr);
  if (rc >= 0) {
    MaybeEmitToken(req);
  }
  return rc;
}

herr_t MockDatasetOptional(void* obj, H5VL_optional_args_t* args, hid_t dxpl_id,
                           void** req) {
  if (ActiveMockVolState() != nullptr &&
      ActiveMockVolState()->fail_next_dset_optional) {
    ActiveMockVolState()->fail_next_dset_optional = false;
    return -1;
  }
  auto* o = static_cast<MockVolObj*>(obj);
  herr_t rc = H5VLdataset_optional(o->under_object, o->under_vol_id, args,
                                   dxpl_id, nullptr);
  if (rc >= 0) {
    MaybeEmitToken(req);
  }
  return rc;
}

herr_t MockDatasetClose(void* dset, hid_t dxpl_id, void** req) {
  auto* o = static_cast<MockVolObj*>(dset);
  if (ActiveMockVolState() != nullptr &&
      ActiveMockVolState()->fail_next_dset_close) {
    ActiveMockVolState()->fail_next_dset_close = false;
    return -1;
  }
  herr_t rc =
      H5VLdataset_close(o->under_object, o->under_vol_id, dxpl_id, nullptr);
  if (rc >= 0) {
    FreeObj(o);
    MaybeEmitToken(req);
  }
  return rc;
}

void* MockFileCreate(const char* name, unsigned flags, hid_t fcpl_id,
                     hid_t fapl_id, hid_t dxpl_id, void** req) {
  MockVolInfo* info = nullptr;
  if (H5Pget_vol_info(fapl_id, reinterpret_cast<void**>(&info)) < 0 ||
      info == nullptr) {
    return nullptr;
  }
  ScopedPlist under_fapl(H5Pcopy(fapl_id));
  H5Pset_vol(under_fapl.get(), info->under_vol_id, info->under_vol_info);
  void* under = H5VLfile_create(name, flags, fcpl_id, under_fapl.get(), dxpl_id,
                                req);
  void* result = under ? WrapObj(under, info->under_vol_id) : nullptr;
  MockInfoFree(info);
  return result;
}

void* MockFileOpen(const char* name, unsigned flags, hid_t fapl_id,
                   hid_t dxpl_id, void** req) {
  MockVolInfo* info = nullptr;
  if (H5Pget_vol_info(fapl_id, reinterpret_cast<void**>(&info)) < 0 ||
      info == nullptr) {
    return nullptr;
  }
  ScopedPlist under_fapl(H5Pcopy(fapl_id));
  H5Pset_vol(under_fapl.get(), info->under_vol_id, info->under_vol_info);
  void* under = H5VLfile_open(name, flags, under_fapl.get(), dxpl_id, req);
  void* result = under ? WrapObj(under, info->under_vol_id) : nullptr;
  MockInfoFree(info);
  return result;
}

herr_t MockFileGet(void* file, H5VL_file_get_args_t* args, hid_t dxpl_id,
                   void** req) {
  auto* o = static_cast<MockVolObj*>(file);
  return H5VLfile_get(o->under_object, o->under_vol_id, args, dxpl_id, req);
}

herr_t MockFileSpecific(void* file, H5VL_file_specific_args_t* args,
                        hid_t dxpl_id, void** req) {
  auto* o = static_cast<MockVolObj*>(file);
  if (o == nullptr) {
    return -1;
  }
  return H5VLfile_specific(o->under_object, o->under_vol_id, args, dxpl_id,
                           req);
}

herr_t MockFileOptional(void* file, H5VL_optional_args_t* args, hid_t dxpl_id,
                        void** req) {
  auto* o = static_cast<MockVolObj*>(file);
  return H5VLfile_optional(o->under_object, o->under_vol_id, args, dxpl_id,
                           req);
}

herr_t MockFileClose(void* file, hid_t dxpl_id, void** req) {
  auto* o = static_cast<MockVolObj*>(file);
  herr_t rc = H5VLfile_close(o->under_object, o->under_vol_id, dxpl_id, req);
  if (rc >= 0) {
    FreeObj(o);
  }
  return rc;
}

void* MockGroupCreate(void* obj, const H5VL_loc_params_t* loc_params,
                      const char* name, hid_t lcpl_id, hid_t gcpl_id,
                      hid_t gapl_id, hid_t dxpl_id, void** req) {
  auto* o = static_cast<MockVolObj*>(obj);
  void* under = H5VLgroup_create(o->under_object, loc_params, o->under_vol_id,
                                 name, lcpl_id, gcpl_id, gapl_id, dxpl_id, req);
  return under ? WrapObj(under, o->under_vol_id) : nullptr;
}

void* MockGroupOpen(void* obj, const H5VL_loc_params_t* loc_params,
                    const char* name, hid_t gapl_id, hid_t dxpl_id,
                    void** req) {
  auto* o = static_cast<MockVolObj*>(obj);
  void* under = H5VLgroup_open(o->under_object, loc_params, o->under_vol_id,
                               name, gapl_id, dxpl_id, req);
  return under ? WrapObj(under, o->under_vol_id) : nullptr;
}

herr_t MockGroupGet(void* obj, H5VL_group_get_args_t* args, hid_t dxpl_id,
                    void** req) {
  auto* o = static_cast<MockVolObj*>(obj);
  return H5VLgroup_get(o->under_object, o->under_vol_id, args, dxpl_id, req);
}

herr_t MockGroupSpecific(void* obj, H5VL_group_specific_args_t* args,
                         hid_t dxpl_id, void** req) {
  auto* o = static_cast<MockVolObj*>(obj);
  return H5VLgroup_specific(o->under_object, o->under_vol_id, args, dxpl_id,
                            req);
}

herr_t MockGroupOptional(void* obj, H5VL_optional_args_t* args, hid_t dxpl_id,
                         void** req) {
  auto* o = static_cast<MockVolObj*>(obj);
  return H5VLgroup_optional(o->under_object, o->under_vol_id, args, dxpl_id,
                            req);
}

herr_t MockGroupClose(void* grp, hid_t dxpl_id, void** req) {
  auto* o = static_cast<MockVolObj*>(grp);
  herr_t rc = H5VLgroup_close(o->under_object, o->under_vol_id, dxpl_id, req);
  if (rc >= 0) {
    FreeObj(o);
  }
  return rc;
}

herr_t MockObjectCopy(void* src_obj, const H5VL_loc_params_t* src_loc_params,
                      const char* src_name, void* dst_obj,
                      const H5VL_loc_params_t* dst_loc_params,
                      const char* dst_name, hid_t ocpypl_id, hid_t lcpl_id,
                      hid_t dxpl_id, void** req) {
  auto* o_src = static_cast<MockVolObj*>(src_obj);
  auto* o_dst = static_cast<MockVolObj*>(dst_obj);
  return H5VLobject_copy(o_src->under_object, src_loc_params, src_name,
                         o_dst->under_object, dst_loc_params, dst_name,
                         o_src->under_vol_id, ocpypl_id, lcpl_id, dxpl_id, req);
}

herr_t MockIntrospectGetConnCls(void* obj, H5VL_get_conn_lvl_t lvl,
                                const H5VL_class_t** conn_cls) {
  extern const H5VL_class_t kMockVolClass;
  if (lvl == H5VL_GET_CONN_LVL_CURR) {
    *conn_cls = &kMockVolClass;
    return 0;
  }
  auto* o = static_cast<MockVolObj*>(obj);
  return H5VLintrospect_get_conn_cls(o->under_object, o->under_vol_id, lvl,
                                     conn_cls);
}

herr_t MockIntrospectGetCapFlags(const void* _info, uint64_t* cap_flags) {
  const auto* info = static_cast<const MockVolInfo*>(_info);
  herr_t rc = H5VLintrospect_get_cap_flags(info->under_vol_info,
                                           info->under_vol_id, cap_flags);
  if (rc >= 0) {
    *cap_flags |= H5VL_CAP_FLAG_ASYNC;
  }
  return rc;
}

herr_t MockIntrospectOptQuery(void* obj, H5VL_subclass_t cls, int opt_type,
                              uint64_t* flags) {
  if (ActiveMockVolState() != nullptr &&
      ActiveMockVolState()->fail_next_opt_query) {
    ActiveMockVolState()->fail_next_opt_query = false;
    return -1;
  }
  auto* o = static_cast<MockVolObj*>(obj);
  herr_t rc = H5VLintrospect_opt_query(o->under_object, o->under_vol_id, cls,
                                       opt_type, flags);
  if (rc >= 0 && ActiveMockVolState() != nullptr &&
      ActiveMockVolState()->force_vlen_generic_path &&
      cls == H5VL_SUBCLS_DATASET &&
      opt_type == H5VL_NATIVE_DATASET_GET_VLEN_BUF_SIZE) {
    *flags &= ~static_cast<uint64_t>(H5VL_OPT_QUERY_SUPPORTED);
  }
  return rc;
}

herr_t MockRequestWait(void* /*req*/, uint64_t /*timeout*/,
                       H5VL_request_status_t* status) {
  *status = H5VL_REQUEST_STATUS_SUCCEED;
  return 0;
}

herr_t MockRequestSpecific(void* /*req*/, H5VL_request_specific_args_t* args) {
  if (args->op_type == H5VL_REQUEST_GET_EXEC_TIME) {
    *args->args.get_exec_time.exec_ts = 1;
    *args->args.get_exec_time.exec_time = 1;
  }
  return 0;
}

herr_t MockRequestFree(void* req) {
  if (ActiveMockVolState() != nullptr) {
    ActiveMockVolState()->FreeToken(static_cast<MockVolObj*>(req));
  }
  return 0;
}

herr_t MockBlobPut(void* obj, const void* buf, size_t size, void* blob_id,
                   void* ctx) {
  auto* o = static_cast<MockVolObj*>(obj);
  return H5VLblob_put(o->under_object, o->under_vol_id, buf, size, blob_id,
                      ctx);
}

herr_t MockBlobGet(void* obj, const void* blob_id, void* buf, size_t size,
                   void* ctx) {
  auto* o = static_cast<MockVolObj*>(obj);
  return H5VLblob_get(o->under_object, o->under_vol_id, blob_id, buf, size,
                      ctx);
}

herr_t MockBlobSpecific(void* obj, void* blob_id,
                        H5VL_blob_specific_args_t* args) {
  auto* o = static_cast<MockVolObj*>(obj);
  return H5VLblob_specific(o->under_object, o->under_vol_id, blob_id, args);
}

herr_t MockBlobOptional(void* obj, void* blob_id, H5VL_optional_args_t* args) {
  auto* o = static_cast<MockVolObj*>(obj);
  return H5VLblob_optional(o->under_object, o->under_vol_id, blob_id, args);
}

constexpr H5VL_class_value_t kMockVolValue =
    static_cast<H5VL_class_value_t>(519);

const H5VL_class_t kMockVolClass = {
    H5VL_VERSION,
    kMockVolValue,
    "h5d_test_async_passthru",
    0,
    H5VL_CAP_FLAG_ASYNC,
    MockInit,
    MockTerm,
    {/* info_cls */ sizeof(MockVolInfo), MockInfoCopy, MockInfoCmp, MockInfoFree,
     nullptr, nullptr},
    {/* wrap_cls */ MockWrapGetObject, MockWrapGetCtx, MockWrapObj,
     MockUnwrapObj, MockWrapFreeCtx},
    {/* attr_cls */ nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
     nullptr, nullptr},
    {/* dataset_cls */ MockDatasetCreate, MockDatasetOpen, MockDatasetRead,
     MockDatasetWrite, MockDatasetGet, MockDatasetSpecific,
     MockDatasetOptional, MockDatasetClose},
    {/* datatype_cls */ nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
    {/* file_cls */ MockFileCreate, MockFileOpen, MockFileGet, MockFileSpecific,
     MockFileOptional, MockFileClose},
    {/* group_cls */ MockGroupCreate, MockGroupOpen, MockGroupGet,
     MockGroupSpecific, MockGroupOptional, MockGroupClose},
    {/* link_cls */ nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
    {/* object_cls */ nullptr, MockObjectCopy, nullptr, nullptr, nullptr},
    {/* introspect_cls */ MockIntrospectGetConnCls, MockIntrospectGetCapFlags,
     MockIntrospectOptQuery},
    {/* request_cls */ MockRequestWait, nullptr, nullptr, MockRequestSpecific,
     nullptr, MockRequestFree},
    {/* blob_cls */ MockBlobPut, MockBlobGet, MockBlobSpecific,
     MockBlobOptional},
    {/* token_cls */ nullptr, nullptr, nullptr},
    nullptr,
};

H5VL_class_t MakeMockVolClass() { return kMockVolClass; }

// ---------------------------------------------------------------------------
// Synchronous dataset lifecycle, metadata, and I/O tests
// ---------------------------------------------------------------------------

TEST(H5DTest, SyncLifecycleAnonymousLinkAndQueries) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedPlist lcpl(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(lcpl.valid());
  ASSERT_GE(H5Pset_create_intermediate_group(lcpl.get(), 1), 0);

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  int fill_val = -7;
  ASSERT_GE(H5Pset_fill_value(dcpl.get(), H5T_NATIVE_INT, &fill_val), 0);

  ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_TRUE(dapl.valid());

  const hsize_t dims[2] = {3, 4};
  ScopedSpace space(H5Screate_simple(2, dims, nullptr));
  ASSERT_TRUE(space.valid());

  // Create with non-default LCPL, DCPL, and DAPL (intermediate group auto-made).
  ScopedDataset dset(H5Dcreate2(file.get(), "/sub/contig", H5T_NATIVE_INT,
                                space.get(), lcpl.get(), dcpl.get(),
                                dapl.get()));
  ASSERT_TRUE(dset.valid());

  H5D_space_status_t status = H5D_SPACE_STATUS_ERROR;
  ASSERT_GE(H5Dget_space_status(dset.get(), &status), 0);
  EXPECT_EQ(status, H5D_SPACE_STATUS_NOT_ALLOCATED);
  EXPECT_EQ(H5Dget_storage_size(dset.get()), 0);
  EXPECT_EQ(H5Dget_offset(dset.get()), HADDR_UNDEF);

  std::array<int, 12> data{};
  for (int i = 0; i < 12; ++i) {
    data[i] = i * 3 + 1;
  }
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL, dxpl.get(),
                     data.data()),
            0);

  ASSERT_GE(H5Dget_space_status(dset.get(), &status), 0);
  EXPECT_EQ(status, H5D_SPACE_STATUS_ALLOCATED);
  EXPECT_EQ(H5Dget_storage_size(dset.get()), 12 * sizeof(int));
  EXPECT_NE(H5Dget_offset(dset.get()), HADDR_UNDEF);

  ScopedSpace got_space(H5Dget_space(dset.get()));
  ASSERT_TRUE(got_space.valid());
  hsize_t got_dims[2] = {};
  ASSERT_EQ(H5Sget_simple_extent_dims(got_space.get(), got_dims, nullptr), 2);
  EXPECT_EQ(got_dims[0], 3);
  EXPECT_EQ(got_dims[1], 4);

  ScopedType got_type(H5Dget_type(dset.get()));
  ASSERT_TRUE(got_type.valid());
  EXPECT_EQ(H5Tget_class(got_type.get()), H5T_INTEGER);

  ScopedPlist got_dcpl(H5Dget_create_plist(dset.get()));
  ASSERT_TRUE(got_dcpl.valid());
  int got_fill = 0;
  ASSERT_GE(H5Pget_fill_value(got_dcpl.get(), H5T_NATIVE_INT, &got_fill), 0);
  EXPECT_EQ(got_fill, -7);

  ScopedPlist got_dapl(H5Dget_access_plist(dset.get()));
  ASSERT_TRUE(got_dapl.valid());

  std::array<int, 12> out{};
  ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL, dxpl.get(),
                    out.data()),
            0);
  EXPECT_EQ(out, data);

  ASSERT_GE(H5Dflush(dset.get()), 0);
  ASSERT_GE(H5Drefresh(dset.get()), 0);
  dset.reset();

  ScopedDataset reopened(H5Dopen2(file.get(), "/sub/contig", dapl.get()));
  ASSERT_TRUE(reopened.valid());
  std::fill(out.begin(), out.end(), 0);
  ASSERT_GE(H5Dread(reopened.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, out.data()),
            0);
  EXPECT_EQ(out, data);
  reopened.reset();

  // Anonymous creation with explicit DCPL/DAPL + H5Olink, plus unlinked anon.
  ScopedDataset anon_linked(H5Dcreate_anon(
      file.get(), H5T_NATIVE_INT, space.get(), dcpl.get(), dapl.get()));
  ASSERT_TRUE(anon_linked.valid());
  ASSERT_GE(H5Dwrite(anon_linked.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, data.data()),
            0);
  ASSERT_GE(H5Olink(anon_linked.get(), file.get(), "/linked_anon", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  anon_linked.reset();
  EXPECT_GT(H5Lexists(file.get(), "/linked_anon", H5P_DEFAULT), 0);

  ScopedDataset anon_temp(H5Dcreate_anon(
      file.get(), H5T_NATIVE_INT, space.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(anon_temp.valid());
  anon_temp.reset();
}

TEST(H5DTest, MultiDatasetReadWriteSyncAndErrors) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  const hsize_t dims[1] = {4};
  ScopedSpace space(H5Screate_simple(1, dims, nullptr));
  ASSERT_TRUE(space.valid());

  std::array<ScopedDataset, 3> dsets;
  for (int i = 0; i < 3; ++i) {
    std::string name = "/d" + std::to_string(i);
    dsets[i].reset(H5Dcreate2(file.get(), name.c_str(), H5T_NATIVE_INT,
                              space.get(), H5P_DEFAULT, H5P_DEFAULT,
                              H5P_DEFAULT));
    ASSERT_TRUE(dsets[i].valid());
  }

  // count == 0 is explicitly permitted as a no-op in sync multi APIs.
  EXPECT_GE(H5Dwrite_multi(0, nullptr, nullptr, nullptr, nullptr, H5P_DEFAULT,
                           nullptr),
            0);
  EXPECT_GE(
      H5Dread_multi(0, nullptr, nullptr, nullptr, nullptr, H5P_DEFAULT, nullptr),
      0);

  std::array<int, 4> w0 = {1, 2, 3, 4};
  std::array<int, 4> w1 = {10, 20, 30, 40};
  std::array<int, 4> w2 = {100, 200, 300, 400};
  hid_t dids[3] = {dsets[0].get(), dsets[1].get(), dsets[2].get()};
  hid_t mtypes[3] = {H5T_NATIVE_INT, H5T_NATIVE_INT, H5T_NATIVE_INT};
  hid_t mspaces[3] = {H5S_ALL, H5S_ALL, H5S_ALL};
  hid_t fspaces[3] = {H5S_ALL, H5S_ALL, H5S_ALL};
  const void* wbufs[3] = {w0.data(), w1.data(), w2.data()};

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ASSERT_GE(
      H5Dwrite_multi(3, dids, mtypes, mspaces, fspaces, dxpl.get(), wbufs), 0);

  std::array<int, 4> r0{};
  std::array<int, 4> r1{};
  std::array<int, 4> r2{};
  void* rbufs[3] = {r0.data(), r1.data(), r2.data()};
  ASSERT_GE(
      H5Dread_multi(3, dids, mtypes, mspaces, fspaces, H5P_DEFAULT, rbufs), 0);
  EXPECT_EQ(r0, w0);
  EXPECT_EQ(r1, w1);
  EXPECT_EQ(r2, w2);

  // Single-dataset multi calls also use stack buffer without heap allocation.
  ASSERT_GE(
      H5Dwrite_multi(1, dids, mtypes, mspaces, fspaces, H5P_DEFAULT, wbufs), 0);
  ASSERT_GE(
      H5Dread_multi(1, dids, mtypes, mspaces, fspaces, H5P_DEFAULT, rbufs), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(
      H5Dread_multi(1, nullptr, mtypes, mspaces, fspaces, H5P_DEFAULT, rbufs),
      0);
  EXPECT_LT(
      H5Dread_multi(1, dids, nullptr, mspaces, fspaces, H5P_DEFAULT, rbufs), 0);
  EXPECT_LT(
      H5Dread_multi(1, dids, mtypes, nullptr, fspaces, H5P_DEFAULT, rbufs), 0);
  EXPECT_LT(
      H5Dread_multi(1, dids, mtypes, mspaces, nullptr, H5P_DEFAULT, rbufs), 0);
  EXPECT_LT(
      H5Dread_multi(1, dids, mtypes, mspaces, fspaces, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(
      H5Dread_multi(1, dids, mtypes, mspaces, fspaces, space.get(), rbufs), 0);

  EXPECT_LT(
      H5Dwrite_multi(1, nullptr, mtypes, mspaces, fspaces, H5P_DEFAULT, wbufs),
      0);
  EXPECT_LT(
      H5Dwrite_multi(1, dids, nullptr, mspaces, fspaces, H5P_DEFAULT, wbufs),
      0);
  EXPECT_LT(
      H5Dwrite_multi(1, dids, mtypes, nullptr, fspaces, H5P_DEFAULT, wbufs), 0);
  EXPECT_LT(
      H5Dwrite_multi(1, dids, mtypes, mspaces, nullptr, H5P_DEFAULT, wbufs), 0);
  EXPECT_LT(
      H5Dwrite_multi(1, dids, mtypes, mspaces, fspaces, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(
      H5Dwrite_multi(1, dids, mtypes, mspaces, fspaces, space.get(), wbufs), 0);

  hid_t bad_dids[2] = {space.get(), dids[1]};
  EXPECT_LT(
      H5Dread_multi(2, bad_dids, mtypes, mspaces, fspaces, H5P_DEFAULT, rbufs),
      0);
  EXPECT_LT(
      H5Dwrite_multi(2, bad_dids, mtypes, mspaces, fspaces, H5P_DEFAULT, wbufs),
      0);

  hid_t bad_second[2] = {dids[0], space.get()};
  EXPECT_LT(H5Dread_multi(2, bad_second, mtypes, mspaces, fspaces, H5P_DEFAULT,
                          rbufs),
            0);
  EXPECT_LT(H5Dwrite_multi(2, bad_second, mtypes, mspaces, fspaces, H5P_DEFAULT,
                           wbufs),
            0);
}

TEST(H5DTest, ScatterGatherFillAndIterateOperations) {
  // Scatter: deliver 6 ints across 3 callback invocations (2 ints per call).
  struct ScatterCtx {
    std::array<std::vector<int>, 3> chunks = {
        std::vector<int>{10, 20},
        std::vector<int>{30, 40},
        std::vector<int>{50, 60},
    };
    size_t idx = 0;
  };
  auto scatter_op = [](const void** src_buf, size_t* src_buf_nbytes,
                       void* op_data) -> herr_t {
    auto* ctx = static_cast<ScatterCtx*>(op_data);
    if (ctx->idx >= ctx->chunks.size()) {
      return -1;
    }
    *src_buf = ctx->chunks[ctx->idx].data();
    *src_buf_nbytes = ctx->chunks[ctx->idx].size() * sizeof(int);
    ++ctx->idx;
    return 0;
  };

  const hsize_t dims[1] = {10};
  ScopedSpace space(H5Screate_simple(1, dims, nullptr));
  ASSERT_TRUE(space.valid());
  const hsize_t start = 2;
  const hsize_t count = 6;
  ASSERT_GE(
      H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, &start, nullptr, &count,
                          nullptr),
      0);

  std::array<int, 10> buf{};
  ScatterCtx sctx;
  ASSERT_GE(H5Dscatter(scatter_op, &sctx, H5T_NATIVE_INT, space.get(),
                       buf.data()),
            0);
  EXPECT_EQ(sctx.idx, 3);
  EXPECT_THAT(buf, ElementsAre(0, 0, 10, 20, 30, 40, 50, 60, 0, 0));

  // Gather with small destination buffer requiring 3 callback calls.
  std::vector<int> gathered;
  auto gather_op = [](const void* dst_buf, size_t dst_buf_bytes,
                      void* op_data) -> herr_t {
    auto* out = static_cast<std::vector<int>*>(op_data);
    const auto* vals = static_cast<const int*>(dst_buf);
    out->insert(out->end(), vals, vals + dst_buf_bytes / sizeof(int));
    return 0;
  };
  std::array<int, 2> tmp_gather{};
  ASSERT_GE(H5Dgather(space.get(), buf.data(), H5T_NATIVE_INT,
                      tmp_gather.size() * sizeof(int), tmp_gather.data(),
                      gather_op, &gathered),
            0);
  EXPECT_THAT(gathered, ElementsAre(10, 20, 30, 40, 50, 60));

  // Gather with NULL callback when destination buffer holds full selection.
  std::array<int, 6> full_gather{};
  ASSERT_GE(H5Dgather(space.get(), buf.data(), H5T_NATIVE_INT,
                      full_gather.size() * sizeof(int), full_gather.data(),
                      nullptr, nullptr),
            0);
  EXPECT_THAT(full_gather, ElementsAre(10, 20, 30, 40, 50, 60));

  // Fill with explicit value and with NULL (zero) fill value.
  int fill_val = 99;
  ASSERT_GE(H5Dfill(&fill_val, H5T_NATIVE_INT, buf.data(), H5T_NATIVE_INT,
                    space.get()),
            0);
  EXPECT_THAT(buf, ElementsAre(0, 0, 99, 99, 99, 99, 99, 99, 0, 0));

  ASSERT_GE(H5Dfill(nullptr, H5T_NATIVE_INT, buf.data(), H5T_NATIVE_INT,
                    space.get()),
            0);
  EXPECT_THAT(buf, ElementsAre(0, 0, 0, 0, 0, 0, 0, 0, 0, 0));

  // Iterate full completion (0), short-circuit positive (>0), and error (<0).
  int visit_count = 0;
  auto iter_sum = [](void* elem, hid_t /*type_id*/, unsigned ndim,
                     const hsize_t* point, void* op_data) -> herr_t {
    EXPECT_EQ(ndim, 1u);
    auto* elem_ptr = static_cast<int*>(elem);
    *elem_ptr = static_cast<int>(point[0] * 10);
    ++(*static_cast<int*>(op_data));
    return 0;
  };
  ASSERT_EQ(H5Diterate(buf.data(), H5T_NATIVE_INT, space.get(), iter_sum,
                       &visit_count),
            0);
  EXPECT_EQ(visit_count, 6);
  EXPECT_THAT(buf, ElementsAre(0, 0, 20, 30, 40, 50, 60, 70, 0, 0));

  int short_circuit = 0;
  auto iter_stop = [](void* /*elem*/, hid_t /*type_id*/, unsigned /*ndim*/,
                      const hsize_t* /*point*/, void* op_data) -> herr_t {
    if (++(*static_cast<int*>(op_data)) == 2) {
      return 7;
    }
    return 0;
  };
  EXPECT_EQ(H5Diterate(buf.data(), H5T_NATIVE_INT, space.get(), iter_stop,
                       &short_circuit),
            7);
  EXPECT_EQ(short_circuit, 2);

  // Scatter / Gather / Fill / Iterate validation and callback failures.
  ScopedErrorSilence silence;
  EXPECT_LT(
      H5Dscatter(nullptr, nullptr, H5T_NATIVE_INT, space.get(), buf.data()), 0);
  EXPECT_LT(
      H5Dscatter(scatter_op, &sctx, space.get(), space.get(), buf.data()), 0);
  EXPECT_LT(H5Dscatter(scatter_op, &sctx, H5T_NATIVE_INT, H5I_INVALID_HID,
                       buf.data()),
            0);
  EXPECT_LT(
      H5Dscatter(scatter_op, &sctx, H5T_NATIVE_INT, space.get(), nullptr), 0);

  auto bad_scat_err = [](const void** /*b*/, size_t* /*n*/, void*) -> herr_t {
    return -1;
  };
  EXPECT_LT(H5Dscatter(bad_scat_err, nullptr, H5T_NATIVE_INT, space.get(),
                       buf.data()),
            0);

  auto bad_scat_null = [](const void** b, size_t* n, void*) -> herr_t {
    *b = nullptr;
    *n = sizeof(int);
    return 0;
  };
  EXPECT_LT(H5Dscatter(bad_scat_null, nullptr, H5T_NATIVE_INT, space.get(),
                       buf.data()),
            0);

  static const int kDummyInt = 5;
  auto bad_scat_zero = [](const void** b, size_t* n, void*) -> herr_t {
    *b = &kDummyInt;
    *n = 0;
    return 0;
  };
  EXPECT_LT(H5Dscatter(bad_scat_zero, nullptr, H5T_NATIVE_INT, space.get(),
                       buf.data()),
            0);

  auto bad_scat_misaligned = [](const void** b, size_t* n, void*) -> herr_t {
    *b = &kDummyInt;
    *n = sizeof(int) - 1;
    return 0;
  };
  EXPECT_LT(H5Dscatter(bad_scat_misaligned, nullptr, H5T_NATIVE_INT,
                       space.get(), buf.data()),
            0);

  static const int kTooMany[8] = {};
  auto bad_scat_overflow = [](const void** b, size_t* n, void*) -> herr_t {
    *b = kTooMany;
    *n = sizeof(kTooMany);
    return 0;
  };
  EXPECT_LT(H5Dscatter(bad_scat_overflow, nullptr, H5T_NATIVE_INT, space.get(),
                       buf.data()),
            0);

  EXPECT_LT(H5Dgather(H5I_INVALID_HID, buf.data(), H5T_NATIVE_INT,
                      sizeof(full_gather), full_gather.data(), nullptr,
                      nullptr),
            0);
  EXPECT_LT(H5Dgather(space.get(), nullptr, H5T_NATIVE_INT, sizeof(full_gather),
                      full_gather.data(), nullptr, nullptr),
            0);
  EXPECT_LT(H5Dgather(space.get(), buf.data(), space.get(), sizeof(full_gather),
                      full_gather.data(), nullptr, nullptr),
            0);
  EXPECT_LT(H5Dgather(space.get(), buf.data(), H5T_NATIVE_INT, 0,
                      full_gather.data(), nullptr, nullptr),
            0);
  EXPECT_LT(H5Dgather(space.get(), buf.data(), H5T_NATIVE_INT,
                      sizeof(full_gather), nullptr, nullptr, nullptr),
            0);
  EXPECT_LT(H5Dgather(space.get(), buf.data(), H5T_NATIVE_INT, 1,
                      full_gather.data(), gather_op, &gathered),
            0);
  EXPECT_LT(H5Dgather(space.get(), buf.data(), H5T_NATIVE_INT,
                      tmp_gather.size() * sizeof(int), tmp_gather.data(),
                      nullptr, nullptr),
            0);
  auto bad_gather_cb = [](const void*, size_t, void*) -> herr_t { return -1; };
  EXPECT_LT(H5Dgather(space.get(), buf.data(), H5T_NATIVE_INT,
                      tmp_gather.size() * sizeof(int), tmp_gather.data(),
                      bad_gather_cb, nullptr),
            0);

  EXPECT_LT(H5Dfill(&fill_val, H5T_NATIVE_INT, nullptr, H5T_NATIVE_INT,
                    space.get()),
            0);
  // Upstream H5Dfill passes 0 to HGOTO_ERROR for invalid space/type IDs while
  // still pushing an error onto the default stack.
  H5Eclear2(H5E_DEFAULT);
  EXPECT_EQ(H5Dfill(&fill_val, H5T_NATIVE_INT, buf.data(), H5T_NATIVE_INT,
                    H5I_INVALID_HID),
            0);
  EXPECT_GT(H5Eget_num(H5E_DEFAULT), 0);
  H5Eclear2(H5E_DEFAULT);
  EXPECT_EQ(H5Dfill(&fill_val, H5I_INVALID_HID, buf.data(), H5T_NATIVE_INT,
                    space.get()),
            0);
  EXPECT_GT(H5Eget_num(H5E_DEFAULT), 0);
  H5Eclear2(H5E_DEFAULT);
  EXPECT_EQ(H5Dfill(&fill_val, H5T_NATIVE_INT, buf.data(), H5I_INVALID_HID,
                    space.get()),
            0);
  EXPECT_GT(H5Eget_num(H5E_DEFAULT), 0);
  H5Eclear2(H5E_DEFAULT);

  EXPECT_LT(H5Diterate(buf.data(), H5T_NATIVE_INT, space.get(), nullptr,
                       nullptr),
            0);
  EXPECT_LT(
      H5Diterate(nullptr, H5T_NATIVE_INT, space.get(), iter_sum, &visit_count),
      0);
  EXPECT_LT(
      H5Diterate(buf.data(), H5I_INVALID_HID, space.get(), iter_sum, nullptr),
      0);
  EXPECT_LT(H5Diterate(buf.data(), H5T_NATIVE_INT, H5I_INVALID_HID, iter_sum,
                       nullptr),
            0);
  ScopedSpace empty_extent(H5Screate(H5S_SIMPLE));
  ASSERT_TRUE(empty_extent.valid());
  EXPECT_LT(H5Diterate(buf.data(), H5T_NATIVE_INT, empty_extent.get(), iter_sum,
                       nullptr),
            0);
}

TEST(H5DTest, ChunkedOperationsExtendConvertAndVlenNative) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST, H5F_LIBVER_LATEST),
      0);
  ScopedFile file(
      H5Fcreate("chunk_ops.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  const hsize_t init_dims[2] = {4, 4};
  const hsize_t max_dims[2] = {H5S_UNLIMITED, H5S_UNLIMITED};
  const hsize_t chunk_dims[2] = {2, 2};
  ScopedSpace space(H5Screate_simple(2, init_dims, max_dims));
  ASSERT_TRUE(space.valid());
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 2, chunk_dims), 0);

  ScopedDataset dset(H5Dcreate2(file.get(), "/chk", H5T_NATIVE_INT, space.get(),
                                H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  // Direct chunk write & read with non-default DXPL.
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  const std::array<int, 4> c00 = {1, 2, 3, 4};
  const std::array<int, 4> c02 = {5, 6, 7, 8};
  const hsize_t off00[2] = {0, 0};
  const hsize_t off02[2] = {0, 2};
  ASSERT_GE(H5Dwrite_chunk(dset.get(), dxpl.get(), 0, off00,
                           c00.size() * sizeof(int), c00.data()),
            0);
  ASSERT_GE(H5Dwrite_chunk(dset.get(), H5P_DEFAULT, 0, off02,
                           c02.size() * sizeof(int), c02.data()),
            0);
  ASSERT_GE(H5Dflush(dset.get()), 0);

  H5D_chunk_index_t idx_type = H5D_CHUNK_IDX_NONE;
  ASSERT_GE(H5Dget_chunk_index_type(dset.get(), &idx_type), 0);
  EXPECT_EQ(idx_type, H5D_CHUNK_IDX_BT2);

  hsize_t chk_nbytes = 0;
  ASSERT_GE(H5Dget_chunk_storage_size(dset.get(), off00, &chk_nbytes), 0);
  EXPECT_EQ(chk_nbytes, c00.size() * sizeof(int));

  uint32_t read_filters = 123;
  size_t read_size = 0;
  ASSERT_GE(H5Dread_chunk2(dset.get(), dxpl.get(), off00, &read_filters,
                           nullptr, &read_size),
            0);
  EXPECT_EQ(read_size, c00.size() * sizeof(int));
  std::array<int, 4> c_out{};
  ASSERT_GE(H5Dread_chunk2(dset.get(), H5P_DEFAULT, off00, &read_filters,
                           c_out.data(), &read_size),
            0);
  EXPECT_EQ(c_out, c00);

  hsize_t nchunks = 0;
  ASSERT_GE(H5Dget_num_chunks(dset.get(), H5S_ALL, &nchunks), 0);
  EXPECT_EQ(nchunks, 2);

  hsize_t got_off[2] = {};
  unsigned got_mask = 0;
  haddr_t got_addr = HADDR_UNDEF;
  hsize_t got_size = 0;
  ASSERT_GE(H5Dget_chunk_info(dset.get(), H5S_ALL, 1, got_off, &got_mask,
                              &got_addr, &got_size),
            0);
  EXPECT_EQ(got_size, c02.size() * sizeof(int));
  EXPECT_NE(got_addr, HADDR_UNDEF);

  ASSERT_GE(H5Dget_chunk_info_by_coord(dset.get(), off02, &got_mask, &got_addr,
                                       &got_size),
            0);
  EXPECT_EQ(got_size, c02.size() * sizeof(int));

  int iter_seen = 0;
  auto chunk_cb = [](const hsize_t* /*offset*/, unsigned /*filter_mask*/,
                     haddr_t /*addr*/, hsize_t nbytes,
                     void* op_data) -> herr_t {
    EXPECT_EQ(nbytes, 4 * sizeof(int));
    ++(*static_cast<int*>(op_data));
    return H5_ITER_CONT;
  };
  ASSERT_EQ(H5Dchunk_iter(dset.get(), dxpl.get(), chunk_cb, &iter_seen), 0);
  EXPECT_EQ(iter_seen, 2);

  // Convert format to v1 B-tree index; read chunk back and extend dataset.
  ASSERT_GE(H5Dformat_convert(dset.get()), 0);
  std::fill(c_out.begin(), c_out.end(), 0);
  ASSERT_GE(H5Dread_chunk2(dset.get(), H5P_DEFAULT, off00, &read_filters,
                           c_out.data(), &read_size),
            0);
  EXPECT_EQ(c_out, c00);

  const hsize_t new_dims[2] = {6, 6};
  ASSERT_GE(H5Dset_extent(dset.get(), new_dims), 0);

  // Vlen dataset sizing via native VOL fast path.
  ScopedType vlen_type(H5Tvlen_create(H5T_NATIVE_INT));
  ASSERT_TRUE(vlen_type.valid());
  const hsize_t vlen_dims[1] = {2};
  ScopedSpace vlen_space(H5Screate_simple(1, vlen_dims, nullptr));
  ASSERT_TRUE(vlen_space.valid());
  ScopedDataset vlen_dset(H5Dcreate2(file.get(), "/vlen", vlen_type.get(),
                                     vlen_space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                     H5P_DEFAULT));
  ASSERT_TRUE(vlen_dset.valid());
  std::array<int, 2> row0 = {1, 2};
  std::array<int, 3> row1 = {3, 4, 5};
  hvl_t vdata[2] = {{row0.size(), row0.data()}, {row1.size(), row1.data()}};
  ASSERT_GE(H5Dwrite(vlen_dset.get(), vlen_type.get(), H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, vdata),
            0);
  hsize_t vlen_bytes = 0;
  ASSERT_GE(H5Dvlen_get_buf_size(vlen_dset.get(), vlen_type.get(),
                                 vlen_space.get(), &vlen_bytes),
            0);
  EXPECT_EQ(vlen_bytes, 5 * sizeof(int));

  // Negative checks for chunk/vlen/extent/flush/refresh public entry points.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Dread_chunk2(file.get(), H5P_DEFAULT, off00, &read_filters,
                           c_out.data(), &read_size),
            0);
  EXPECT_LT(H5Dread_chunk2(dset.get(), H5P_DEFAULT, nullptr, &read_filters,
                           c_out.data(), &read_size),
            0);
  EXPECT_LT(H5Dread_chunk2(dset.get(), H5P_DEFAULT, off00, nullptr,
                           c_out.data(), &read_size),
            0);
  EXPECT_LT(H5Dread_chunk2(dset.get(), H5P_DEFAULT, off00, &read_filters,
                           c_out.data(), nullptr),
            0);
  EXPECT_LT(H5Dread_chunk2(dset.get(), space.get(), off00, &read_filters,
                           c_out.data(), &read_size),
            0);
  const hsize_t missing_off[2] = {4, 4};
  EXPECT_LT(H5Dread_chunk2(dset.get(), H5P_DEFAULT, missing_off, &read_filters,
                           c_out.data(), &read_size),
            0);

  EXPECT_LT(H5Dwrite_chunk(file.get(), H5P_DEFAULT, 0, off00, 4, c00.data()),
            0);
  EXPECT_LT(H5Dwrite_chunk(dset.get(), H5P_DEFAULT, 0, off00, 4, nullptr), 0);
  EXPECT_LT(H5Dwrite_chunk(dset.get(), H5P_DEFAULT, 0, nullptr, 4, c00.data()),
            0);
  EXPECT_LT(H5Dwrite_chunk(dset.get(), H5P_DEFAULT, 0, off00, 0, c00.data()),
            0);
  EXPECT_LT(H5Dwrite_chunk(dset.get(), space.get(), 0, off00, 4, c00.data()),
            0);

  EXPECT_LT(H5Dformat_convert(file.get()), 0);
  EXPECT_LT(H5Dget_chunk_index_type(file.get(), &idx_type), 0);
  EXPECT_LT(H5Dget_chunk_index_type(dset.get(), nullptr), 0);
  EXPECT_LT(H5Dget_chunk_storage_size(file.get(), off00, &chk_nbytes), 0);
  EXPECT_LT(H5Dget_chunk_storage_size(dset.get(), nullptr, &chk_nbytes), 0);
  EXPECT_LT(H5Dget_chunk_storage_size(dset.get(), off00, nullptr), 0);
  EXPECT_LT(H5Dget_num_chunks(file.get(), H5S_ALL, &nchunks), 0);
  EXPECT_LT(H5Dget_num_chunks(dset.get(), H5S_ALL, nullptr), 0);

  EXPECT_LT(H5Dget_chunk_info(dset.get(), H5S_ALL, 0, nullptr, nullptr, nullptr,
                              nullptr),
            0);
  EXPECT_LT(H5Dget_chunk_info(file.get(), H5S_ALL, 0, got_off, nullptr, nullptr,
                              nullptr),
            0);
  EXPECT_LT(H5Dget_chunk_info(dset.get(), H5S_ALL, 99, got_off, nullptr,
                              nullptr, nullptr),
            0);
  EXPECT_LT(H5Dget_chunk_info(dset.get(), dset.get(), 0, got_off, nullptr,
                              nullptr, nullptr),
            0);

  EXPECT_LT(H5Dget_chunk_info_by_coord(file.get(), off00, &got_mask, &got_addr,
                                       &got_size),
            0);
  EXPECT_LT(H5Dget_chunk_info_by_coord(dset.get(), off00, nullptr, nullptr,
                                       nullptr),
            0);
  EXPECT_LT(H5Dget_chunk_info_by_coord(dset.get(), nullptr, &got_mask,
                                       &got_addr, &got_size),
            0);
  EXPECT_GE(H5Dget_chunk_info_by_coord(dset.get(), missing_off, &got_mask,
                                       &got_addr, &got_size),
            0);
  EXPECT_EQ(got_size, 0);
  EXPECT_EQ(got_addr, HADDR_UNDEF);

  EXPECT_LT(H5Dchunk_iter(file.get(), H5P_DEFAULT, chunk_cb, &iter_seen), 0);
  EXPECT_LT(H5Dchunk_iter(dset.get(), H5P_DEFAULT, nullptr, &iter_seen), 0);
  EXPECT_LT(H5Dchunk_iter(dset.get(), space.get(), chunk_cb, &iter_seen), 0);
  auto err_cb = [](const hsize_t*, unsigned, haddr_t, hsize_t,
                   void*) -> herr_t { return -1; };
  EXPECT_LT(H5Dchunk_iter(dset.get(), H5P_DEFAULT, err_cb, nullptr), 0);

  EXPECT_LT(
      H5Dvlen_get_buf_size(file.get(), vlen_type.get(), vlen_space.get(),
                           &vlen_bytes),
      0);
  EXPECT_LT(H5Dvlen_get_buf_size(vlen_dset.get(), space.get(), vlen_space.get(),
                                 &vlen_bytes),
            0);
  EXPECT_LT(H5Dvlen_get_buf_size(vlen_dset.get(), vlen_type.get(),
                                 vlen_type.get(), &vlen_bytes),
            0);
  EXPECT_LT(H5Dvlen_get_buf_size(vlen_dset.get(), vlen_type.get(),
                                 vlen_space.get(), nullptr),
            0);

  EXPECT_LT(H5Dset_extent(file.get(), new_dims), 0);
  EXPECT_LT(H5Dset_extent(dset.get(), nullptr), 0);
  EXPECT_LT(H5Dflush(file.get()), 0);
  EXPECT_LT(H5Drefresh(file.get()), 0);
}

TEST(H5DTest, SyncValidationFailurePaths) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  const hsize_t dims[1] = {4};
  ScopedSpace space(H5Screate_simple(1, dims, nullptr));
  ASSERT_TRUE(space.valid());

  ScopedDataset existing(H5Dcreate2(file.get(), "/existing", H5T_NATIVE_INT,
                                    space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                    H5P_DEFAULT));
  ASSERT_TRUE(existing.valid());

  ScopedErrorSilence silence;

  EXPECT_LT(H5Dcreate2(file.get(), nullptr, H5T_NATIVE_INT, space.get(),
                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Dcreate2(file.get(), "", H5T_NATIVE_INT, space.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Dcreate2(space.get(), "d", H5T_NATIVE_INT, space.get(),
                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Dcreate2(file.get(), "d", H5T_NATIVE_INT, space.get(),
                       space.get(), H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Dcreate2(file.get(), "d", H5T_NATIVE_INT, space.get(),
                       H5P_DEFAULT, space.get(), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Dcreate2(file.get(), "/existing", H5T_NATIVE_INT, space.get(),
                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
            0);

  EXPECT_LT(H5Dcreate_anon(file.get(), H5T_NATIVE_INT, space.get(), space.get(),
                           H5P_DEFAULT),
            0);
  EXPECT_LT(H5Dcreate_anon(file.get(), H5T_NATIVE_INT, space.get(), H5P_DEFAULT,
                           space.get()),
            0);
  EXPECT_LT(H5Dcreate_anon(H5I_INVALID_HID, H5T_NATIVE_INT, space.get(),
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Dcreate_anon(space.get(), H5T_NATIVE_INT, space.get(),
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Dcreate_anon(file.get(), H5I_INVALID_HID, space.get(),
                           H5P_DEFAULT, H5P_DEFAULT),
            0);

  EXPECT_LT(H5Dopen2(file.get(), nullptr, H5P_DEFAULT), 0);
  EXPECT_LT(H5Dopen2(file.get(), "", H5P_DEFAULT), 0);
  EXPECT_LT(H5Dopen2(space.get(), "/existing", H5P_DEFAULT), 0);
  EXPECT_LT(H5Dopen2(H5I_INVALID_HID, "/existing", H5P_DEFAULT), 0);
  EXPECT_LT(H5Dopen2(file.get(), "/missing", H5P_DEFAULT), 0);

  EXPECT_LT(H5Dclose(file.get()), 0);
  EXPECT_LT(H5Dclose(H5I_INVALID_HID), 0);

  EXPECT_LT(H5Dget_space(file.get()), 0);
  H5D_space_status_t st = H5D_SPACE_STATUS_ERROR;
  EXPECT_LT(H5Dget_space_status(file.get(), &st), 0);
  EXPECT_LT(H5Dget_type(file.get()), 0);
  EXPECT_LT(H5Dget_create_plist(file.get()), 0);
  EXPECT_LT(H5Dget_access_plist(file.get()), 0);
  EXPECT_EQ(H5Dget_storage_size(file.get()), 0);
  EXPECT_EQ(H5Dget_offset(file.get()), HADDR_UNDEF);

  int buf[4] = {};
  EXPECT_LT(H5Dread(file.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    buf),
            0);
  EXPECT_LT(H5Dwrite(file.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     buf),
            0);
}

TEST(H5DTest, AsyncVolEventSetFaultInjectionAndVlenGeneric) {
  MockVolState state;
  ActiveMockVolState() = &state;
  auto clear_state = absl::MakeCleanup([]() { ActiveMockVolState() = nullptr; });

  H5VL_class_t vol_cls = MakeMockVolClass();
  hid_t vol_id = H5VLregister_connector(&vol_cls, H5P_DEFAULT);
  ASSERT_GE(vol_id, 0);
  auto unregister_vol =
      absl::MakeCleanup([&]() { H5VLunregister_connector(vol_id); });

  // Second distinct VOL connector to test mismatched-VOL multi-dataset errors.
  H5VL_class_t vol_cls2 = MakeMockVolClass();
  vol_cls2.value = static_cast<H5VL_class_value_t>(520);
  vol_cls2.name = "h5d_test_async_passthru_alt";
  hid_t vol_id2 = H5VLregister_connector(&vol_cls2, H5P_DEFAULT);
  ASSERT_GE(vol_id2, 0);
  auto unregister_vol2 =
      absl::MakeCleanup([&]() { H5VLunregister_connector(vol_id2); });

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  MockVolInfo info{H5VL_NATIVE, nullptr};
  ASSERT_GE(H5Pset_vol(fapl.get(), vol_id, &info), 0);

  ScopedFile file(
      H5Fcreate("async_dset.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  ScopedPlist fapl2 = MakeCoreFapl();
  ASSERT_TRUE(fapl2.valid());
  ASSERT_GE(H5Pset_vol(fapl2.get(), vol_id2, &info), 0);
  ScopedFile file2(
      H5Fcreate("async_dset2.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl2.get()));
  ASSERT_TRUE(file2.valid());

  const hsize_t init_dims[1] = {4};
  const hsize_t max_dims[1] = {H5S_UNLIMITED};
  const hsize_t chunk_dims[1] = {4};
  ScopedSpace space(H5Screate_simple(1, init_dims, max_dims));
  ASSERT_TRUE(space.valid());
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, chunk_dims), 0);

  ScopedEventSet es(H5EScreate());
  ASSERT_TRUE(es.valid());

  // 1. All 9 H5D*_async APIs with H5ES_NONE (token_ptr == H5_REQUEST_NULL).
  {
    ScopedDataset d0(H5Dcreate_async(file.get(), "/sync0", H5T_NATIVE_INT,
                                     space.get(), H5P_DEFAULT, dcpl.get(),
                                     H5P_DEFAULT, H5ES_NONE));
    ASSERT_TRUE(d0.valid());
    ScopedDataset d1(H5Dcreate_async(file.get(), "/sync1", H5T_NATIVE_INT,
                                     space.get(), H5P_DEFAULT, dcpl.get(),
                                     H5P_DEFAULT, H5ES_NONE));
    ASSERT_TRUE(d1.valid());

    std::array<int, 4> w0 = {1, 2, 3, 4};
    std::array<int, 4> w1 = {5, 6, 7, 8};
    ASSERT_GE(H5Dwrite_async(d0.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                             H5P_DEFAULT, w0.data(), H5ES_NONE),
              0);
    hid_t dids[2] = {d0.get(), d1.get()};
    hid_t mtypes[2] = {H5T_NATIVE_INT, H5T_NATIVE_INT};
    hid_t mspaces[2] = {H5S_ALL, H5S_ALL};
    hid_t fspaces[2] = {H5S_ALL, H5S_ALL};
    const void* wbufs[2] = {w0.data(), w1.data()};
    ASSERT_GE(H5Dwrite_multi_async(2, dids, mtypes, mspaces, fspaces,
                                   H5P_DEFAULT, wbufs, H5ES_NONE),
              0);

    std::array<int, 4> r0{};
    std::array<int, 4> r1{};
    void* rbufs[2] = {r0.data(), r1.data()};
    ASSERT_GE(H5Dread_async(d0.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                            H5P_DEFAULT, r0.data(), H5ES_NONE),
              0);
    EXPECT_EQ(r0, w0);
    ASSERT_GE(H5Dread_multi_async(2, dids, mtypes, mspaces, fspaces,
                                  H5P_DEFAULT, rbufs, H5ES_NONE),
              0);
    EXPECT_EQ(r1, w1);

    ScopedSpace sp(H5Dget_space_async(d0.get(), H5ES_NONE));
    ASSERT_TRUE(sp.valid());

    const hsize_t ext[1] = {8};
    ASSERT_GE(H5Dset_extent_async(d0.get(), ext, H5ES_NONE), 0);

    EXPECT_GE(H5Dclose_async(d0.release(), H5ES_NONE), 0);
    EXPECT_GE(H5Dclose_async(d1.release(), H5ES_NONE), 0);

    ScopedDataset opened(H5Dopen_async(file.get(), "/sync0", H5P_DEFAULT,
                                       H5ES_NONE));
    ASSERT_TRUE(opened.valid());
    EXPECT_GE(H5Dclose_async(opened.release(), H5ES_NONE), 0);
  }

  // 2. All 9 H5D*_async APIs with valid es_id (tokens inserted & awaited).
  {
    ScopedDataset a0(H5Dcreate_async(file.get(), "/async0", H5T_NATIVE_INT,
                                     space.get(), H5P_DEFAULT, dcpl.get(),
                                     H5P_DEFAULT, es.get()));
    ASSERT_TRUE(a0.valid());
    ScopedDataset a1(H5Dcreate_async(file.get(), "/async1", H5T_NATIVE_INT,
                                     space.get(), H5P_DEFAULT, dcpl.get(),
                                     H5P_DEFAULT, es.get()));
    ASSERT_TRUE(a1.valid());

    std::array<int, 4> w0 = {11, 22, 33, 44};
    std::array<int, 4> w1 = {55, 66, 77, 88};
    ASSERT_GE(H5Dwrite_async(a0.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                             H5P_DEFAULT, w0.data(), es.get()),
              0);
    hid_t dids[2] = {a0.get(), a1.get()};
    hid_t mtypes[2] = {H5T_NATIVE_INT, H5T_NATIVE_INT};
    hid_t mspaces[2] = {H5S_ALL, H5S_ALL};
    hid_t fspaces[2] = {H5S_ALL, H5S_ALL};
    const void* wbufs[2] = {w0.data(), w1.data()};
    ASSERT_GE(H5Dwrite_multi_async(2, dids, mtypes, mspaces, fspaces,
                                   H5P_DEFAULT, wbufs, es.get()),
              0);

    std::array<int, 4> r0{};
    std::array<int, 4> r1{};
    void* rbufs[2] = {r0.data(), r1.data()};
    ASSERT_GE(H5Dread_async(a0.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                            H5P_DEFAULT, r0.data(), es.get()),
              0);
    EXPECT_EQ(r0, w0);
    ASSERT_GE(H5Dread_multi_async(2, dids, mtypes, mspaces, fspaces,
                                  H5P_DEFAULT, rbufs, es.get()),
              0);
    EXPECT_EQ(r1, w1);

    ScopedSpace sp(H5Dget_space_async(a0.get(), es.get()));
    ASSERT_TRUE(sp.valid());
    const hsize_t ext[1] = {8};
    ASSERT_GE(H5Dset_extent_async(a0.get(), ext, es.get()), 0);

    ASSERT_GE(H5Dclose_async(a0.release(), es.get()), 0);
    ASSERT_GE(H5Dclose_async(a1.release(), es.get()), 0);

    ScopedDataset opened(H5Dopen_async(file.get(), "/async0", H5P_DEFAULT,
                                       es.get()));
    ASSERT_TRUE(opened.valid());
    ASSERT_GE(H5Dclose_async(opened.release(), es.get()), 0);

    size_t in_progress = 1;
    bool err_occurred = true;
    ASSERT_GE(H5ESwait(es.get(), H5ES_WAIT_FOREVER, &in_progress, &err_occurred),
              0);
    EXPECT_EQ(in_progress, 0);
    EXPECT_FALSE(err_occurred);
    EXPECT_TRUE(state.live_tokens.empty());
  }

  // 3. Exercise H5Dvlen_get_buf_size generic path when VOL lacks OPT_QUERY flag.
  {
    ScopedType vlen_type(H5Tvlen_create(H5T_NATIVE_INT));
    ASSERT_TRUE(vlen_type.valid());
    const hsize_t vdims[1] = {2};
    ScopedSpace vspace(H5Screate_simple(1, vdims, nullptr));
    ASSERT_TRUE(vspace.valid());
    ScopedDataset vdset(H5Dcreate2(file.get(), "/vlen_gen", vlen_type.get(),
                                   vspace.get(), H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT));
    ASSERT_TRUE(vdset.valid());
    std::array<int, 2> row0 = {10, 20};
    std::array<int, 1> row1 = {30};
    hvl_t vdata[2] = {{row0.size(), row0.data()}, {row1.size(), row1.data()}};
    ASSERT_GE(H5Dwrite(vdset.get(), vlen_type.get(), H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, vdata),
              0);

    state.force_vlen_generic_path = true;
    hsize_t gen_bytes = 0;
    ASSERT_GE(H5Dvlen_get_buf_size(vdset.get(), vlen_type.get(), vspace.get(),
                                   &gen_bytes),
              0);
    EXPECT_EQ(gen_bytes, 4 * sizeof(int));
    state.force_vlen_generic_path = false;
  }

  // 4. Error & H5ES_insert rollback paths across async APIs, VOL failures, and
  //    mismatched VOL connectors in multi-dataset calls.
  {
    ScopedErrorSilence silence;
    hid_t bad_es = fapl.get();  // Non-zero, non-H5I_EVENTSET hid_t.

    EXPECT_LT(H5Dcreate_async(file.get(), "/bad_es_c", H5T_NATIVE_INT,
                              space.get(), H5P_DEFAULT, dcpl.get(),
                              H5P_DEFAULT, bad_es),
              0);
    EXPECT_LT(H5Dopen_async(file.get(), "/async0", H5P_DEFAULT, bad_es), 0);

    ScopedDataset tmp(
        H5Dopen2(file.get(), "/async0", H5P_DEFAULT));
    ASSERT_TRUE(tmp.valid());
    std::array<int, 8> buf{};
    hid_t did = tmp.get();
    hid_t mt = H5T_NATIVE_INT;
    hid_t ms = H5S_ALL;
    hid_t fs = H5S_ALL;
    const void* wbuf = buf.data();
    void* rbuf = buf.data();

    EXPECT_LT(H5Dget_space_async(tmp.get(), bad_es), 0);
    EXPECT_LT(H5Dwrite_async(tmp.get(), mt, ms, fs, H5P_DEFAULT, wbuf, bad_es),
              0);
    EXPECT_LT(H5Dread_async(tmp.get(), mt, ms, fs, H5P_DEFAULT, rbuf, bad_es),
              0);
    EXPECT_LT(H5Dwrite_multi_async(1, &did, &mt, &ms, &fs, H5P_DEFAULT, &wbuf,
                                   bad_es),
              0);
    EXPECT_LT(H5Dread_multi_async(1, &did, &mt, &ms, &fs, H5P_DEFAULT, &rbuf,
                                  bad_es),
              0);
    const hsize_t ext[1] = {12};
    EXPECT_LT(H5Dset_extent_async(tmp.get(), ext, bad_es), 0);
    EXPECT_LT(H5Dclose_async(tmp.release(), bad_es), 0);

    // Underlying failures with valid es_id (plus count == 0 in *_multi_async).
    EXPECT_LT(H5Dcreate_async(file.get(), nullptr, H5T_NATIVE_INT, space.get(),
                              H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, es.get()),
              0);
    EXPECT_LT(H5Dopen_async(file.get(), "/missing", H5P_DEFAULT, es.get()), 0);
    EXPECT_LT(H5Dget_space_async(file.get(), es.get()), 0);
    EXPECT_LT(H5Dread_async(file.get(), mt, ms, fs, H5P_DEFAULT, rbuf, es.get()),
              0);
    EXPECT_LT(
        H5Dwrite_async(file.get(), mt, ms, fs, H5P_DEFAULT, wbuf, es.get()), 0);
    EXPECT_LT(H5Dread_multi_async(0, nullptr, nullptr, nullptr, nullptr,
                                  H5P_DEFAULT, nullptr, es.get()),
              0);
    EXPECT_LT(H5Dwrite_multi_async(0, nullptr, nullptr, nullptr, nullptr,
                                   H5P_DEFAULT, nullptr, es.get()),
              0);
    EXPECT_LT(H5Dset_extent_async(file.get(), ext, es.get()), 0);
    EXPECT_LT(H5Dclose_async(file.get(), es.get()), 0);

    // Mismatched VOL connectors in multi-dataset read/write.
    ScopedDataset d_vol1(H5Dcreate2(file.get(), "/v1", H5T_NATIVE_INT,
                                    space.get(), H5P_DEFAULT, dcpl.get(),
                                    H5P_DEFAULT));
    ASSERT_TRUE(d_vol1.valid());
    ScopedDataset d_vol2(H5Dcreate2(file2.get(), "/v2", H5T_NATIVE_INT,
                                    space.get(), H5P_DEFAULT, dcpl.get(),
                                    H5P_DEFAULT));
    ASSERT_TRUE(d_vol2.valid());
    hid_t mix_dids[2] = {d_vol1.get(), d_vol2.get()};
    hid_t mix_types[2] = {H5T_NATIVE_INT, H5T_NATIVE_INT};
    hid_t mix_spaces[2] = {H5S_ALL, H5S_ALL};
    const void* mix_wbufs[2] = {wbuf, wbuf};
    void* mix_rbufs[2] = {rbuf, rbuf};
    EXPECT_LT(H5Dread_multi(2, mix_dids, mix_types, mix_spaces, mix_spaces,
                            H5P_DEFAULT, mix_rbufs),
              0);
    EXPECT_LT(H5Dwrite_multi(2, mix_dids, mix_types, mix_spaces, mix_spaces,
                             H5P_DEFAULT, mix_wbufs),
              0);

    // VOL failure injections for get / specific / optional / introspect.
    state.fail_next_dset_get = true;
    EXPECT_LT(H5Dget_space(d_vol1.get()), 0);
    state.fail_next_dset_get = true;
    H5D_space_status_t st = H5D_SPACE_STATUS_ERROR;
    EXPECT_LT(H5Dget_space_status(d_vol1.get(), &st), 0);
    state.fail_next_dset_get = true;
    EXPECT_LT(H5Dget_type(d_vol1.get()), 0);
    state.fail_next_dset_get = true;
    EXPECT_LT(H5Dget_create_plist(d_vol1.get()), 0);
    state.fail_next_dset_get = true;
    EXPECT_LT(H5Dget_access_plist(d_vol1.get()), 0);
    state.fail_next_dset_get = true;
    EXPECT_EQ(H5Dget_storage_size(d_vol1.get()), 0);

    state.fail_next_dset_optional = true;
    EXPECT_EQ(H5Dget_offset(d_vol1.get()), HADDR_UNDEF);
    state.fail_next_dset_specific = true;
    EXPECT_LT(H5Dflush(d_vol1.get()), 0);
    state.fail_next_dset_specific = true;
    EXPECT_LT(H5Drefresh(d_vol1.get()), 0);
    state.fail_next_dset_optional = true;
    EXPECT_LT(H5Dformat_convert(d_vol1.get()), 0);
    state.fail_next_dset_optional = true;
    H5D_chunk_index_t idx_type = H5D_CHUNK_IDX_NONE;
    EXPECT_LT(H5Dget_chunk_index_type(d_vol1.get(), &idx_type), 0);
    state.fail_next_dset_optional = true;
    hsize_t chk_bytes = 0;
    const hsize_t zero_off[1] = {0};
    EXPECT_LT(H5Dget_chunk_storage_size(d_vol1.get(), zero_off, &chk_bytes), 0);
    state.fail_next_dset_optional = true;
    hsize_t nchk = 0;
    EXPECT_LT(H5Dget_num_chunks(d_vol1.get(), H5S_ALL, &nchk), 0);

    // Ensure at least one chunk exists before failing BY_IDX / CHUNK_ITER.
    ASSERT_GE(H5Dwrite(d_vol1.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, wbuf),
              0);
    state.fail_next_dset_optional = true;
    hsize_t chk_off[1] = {};
    EXPECT_LT(H5Dget_chunk_info(d_vol1.get(), H5S_ALL, 0, chk_off, nullptr,
                                nullptr, nullptr),
              0);

    // Trigger opt_query failure, vlen optional failure, and generic path failure.
    ScopedDataset vd_err(H5Dopen2(file.get(), "/vlen_gen", H5P_DEFAULT));
    ASSERT_TRUE(vd_err.valid());
    ScopedType vt(H5Dget_type(vd_err.get()));
    ScopedSpace vs(H5Dget_space(vd_err.get()));
    hsize_t vsz = 0;
    state.fail_next_opt_query = true;
    EXPECT_LT(H5Dvlen_get_buf_size(vd_err.get(), vt.get(), vs.get(), &vsz), 0);
    state.fail_next_dset_optional = true;
    EXPECT_LT(H5Dvlen_get_buf_size(vd_err.get(), vt.get(), vs.get(), &vsz), 0);
    state.force_vlen_generic_path = true;
    state.fail_next_dset_read = true;
    EXPECT_LT(H5Dvlen_get_buf_size(vd_err.get(), vt.get(), vs.get(), &vsz), 0);
    state.force_vlen_generic_path = false;
  }
}

}  // namespace
}  // namespace hdf5
